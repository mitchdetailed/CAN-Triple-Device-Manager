#include "lua_sandbox.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#include <cstring>

namespace ct {

// The hook and the allocator get the sandbox through lua_getallocf's ud /
// an extra-space pointer. Lua 5.4's LUA_EXTRASPACE gives every state a
// pointer-sized scratch area reachable from any lua_State* — including
// coroutine threads, which lua_getallocf also covers. Simplest is to use the
// allocator ud for both.
struct SandboxAccess {
    static LuaSandbox *from(lua_State *L)
    {
        void *ud = nullptr;
        lua_getallocf(L, &ud);
        return static_cast<LuaSandbox *>(ud);
    }
    static void hook(lua_State *L)
    {
        LuaSandbox *s = from(L);
        if (s->m_cancelled.load()) {
            luaL_error(L, "script stopped");
        }
        if (s->m_clock.isValid() && s->m_clock.elapsed() > s->m_timeLimitMs) {
            luaL_error(L, "script exceeded the %d second time limit",
                       int(s->m_timeLimitMs / 1000));
        }
    }
};

namespace {

// Check the deadline every 50k instructions: at very roughly 10-100 M Lua
// instructions/second that is a check every few milliseconds — frequent
// enough that a runaway loop dies promptly, rare enough to cost nothing.
constexpr int kHookInterval = 50000;

void countHook(lua_State *L, lua_Debug *)
{
    SandboxAccess::hook(L);
}

// print() replacement: assemble the line exactly the way stock print does
// (luaL_tolstring honours __tostring, tab separators) and hand it to the
// host instead of stdout, which a windowed application does not have.
int sandboxPrint(lua_State *L)
{
    auto *out = static_cast<LuaSandbox::OutputFn *>(
        lua_touserdata(L, lua_upvalueindex(1)));
    const int n = lua_gettop(L);
    QString line;
    for (int i = 1; i <= n; ++i) {
        size_t len = 0;
        const char *s = luaL_tolstring(L, i, &len);
        if (i > 1) {
            line += QLatin1Char('\t');
        }
        line += QString::fromUtf8(s, int(len));
        lua_pop(L, 1); // luaL_tolstring's copy
    }
    if (*out) {
        (*out)(line);
    }
    return 0;
}

// Runs inside a protected call so an out-of-memory during setup surfaces as
// a Lua error rather than a panic (which would abort the process).
int installEnvironment(lua_State *L)
{
    // The libraries a configuration script legitimately needs. Each is set as
    // a global, same as luaL_openlibs would; what is NOT opened matters more:
    // io, os (replaced below), package/require, debug, coroutine (nothing here
    // is async, and coroutine.wrap is a favourite hook-evasion tool).
    luaL_requiref(L, LUA_GNAME, luaopen_base, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_UTF8LIBNAME, luaopen_utf8, 1);
    lua_pop(L, 1);

    // Strip the base-library loaders. dofile/loadfile reach the filesystem;
    // load() accepts precompiled bytecode, and unvalidated bytecode is the
    // canonical sandbox escape. Scripts have no legitimate use for any of
    // them — their code arrives through the console.
    static const char *const kRemoved[] = { "dofile", "loadfile", "load" };
    for (const char *name : kRemoved) {
        lua_pushnil(L);
        lua_setglobal(L, name);
    }

    // A reduced os: the clock and calendar, nothing that touches the system.
    // Built by copying the whitelisted entries out of the real library, so
    // the implementations are stock; the full table is then discarded.
    luaL_requiref(L, LUA_OSLIBNAME, luaopen_os, 0); // 0: not set as global
    lua_createtable(L, 0, 4);
    static const char *const kOsKeep[] = { "time", "clock", "date", "difftime" };
    for (const char *name : kOsKeep) {
        lua_getfield(L, -2, name);
        lua_setfield(L, -2, name);
    }
    lua_setglobal(L, "os");
    lua_pop(L, 1); // the real os table

    return 0;
}

// Message handler for pcall: append a traceback so "attempt to index a nil
// value" says WHERE. Registered per call, standard shape.
int traceback(lua_State *L)
{
    const char *msg = lua_tostring(L, 1);
    luaL_traceback(L, L, msg ? msg : "(non-string error)", 1);
    return 1;
}

} // namespace

void *LuaSandbox::boundedAlloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
    auto *self = static_cast<LuaSandbox *>(ud);
    // Per the Lua contract: when ptr is NULL, osize describes the object KIND
    // being created, not a size.
    const size_t oldSize = ptr ? osize : 0;

    if (nsize == 0) {
        free(ptr);
        self->m_memUsed -= oldSize;
        return nullptr;
    }
    if (nsize > oldSize && self->m_memUsed + (nsize - oldSize) > self->m_memLimit) {
        return nullptr; // Lua raises "not enough memory"
    }
    void *grown = realloc(ptr, nsize);
    if (grown) {
        self->m_memUsed += nsize;
        self->m_memUsed -= oldSize;
    }
    return grown;
}

LuaSandbox::LuaSandbox() = default;

LuaSandbox::~LuaSandbox()
{
    if (m_state) {
        lua_close(m_state);
    }
}

bool LuaSandbox::open(QString *error)
{
    m_state = lua_newstate(boundedAlloc, this);
    if (!m_state) {
        if (error) {
            *error = QStringLiteral("could not create the Lua state (out of memory)");
        }
        return false;
    }

    lua_pushcfunction(m_state, installEnvironment);
    if (lua_pcall(m_state, 0, 0, 0) != LUA_OK) {
        if (error) {
            *error = QString::fromUtf8(lua_tostring(m_state, -1));
        }
        lua_close(m_state);
        m_state = nullptr;
        return false;
    }

    // print goes to the host. The OutputFn lives in this object; the closure
    // holds a pointer to it, so setOutput after open still takes effect.
    lua_pushlightuserdata(m_state, &m_output);
    lua_pushcclosure(m_state, sandboxPrint, 1);
    lua_setglobal(m_state, "print");

    lua_sethook(m_state, countHook, LUA_MASKCOUNT, kHookInterval);
    return true;
}

bool LuaSandbox::run(const QString &source, const QString &chunkName, QString *error)
{
    const QByteArray src = source.toUtf8();
    const QByteArray name = QStringLiteral("=%1").arg(chunkName).toUtf8();

    // Mode "t": SOURCE text only. See the header — precompiled chunks are the
    // canonical sandbox escape and are refused before they reach the VM.
    if (luaL_loadbufferx(m_state, src.constData(), size_t(src.size()),
                         name.constData(), "t") != LUA_OK) {
        if (error) {
            *error = QString::fromUtf8(lua_tostring(m_state, -1));
        }
        lua_pop(m_state, 1);
        return false;
    }

    lua_pushcfunction(m_state, traceback);
    lua_insert(m_state, -2); // handler below the chunk

    m_cancelled.store(false);
    m_clock.start();
    const int rc = lua_pcall(m_state, 0, 0, -2);
    lua_remove(m_state, rc == LUA_OK ? -1 : -2); // the handler

    if (rc != LUA_OK) {
        if (error) {
            *error = QString::fromUtf8(lua_tostring(m_state, -1));
        }
        lua_pop(m_state, 1);
        return false;
    }
    return true;
}

} // namespace ct
