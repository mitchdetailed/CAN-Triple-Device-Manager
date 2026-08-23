// A Lua 5.4 state locked down for running configuration scripts.
//
// The threat model is not malice so much as sharing: scripts will be passed
// between users the way DBC files are, pasted from forums, and run on the
// machine that holds fleet keys and customer configurations. So the sandbox
// removes every capability a configuration script has no business having —
// files, processes, environment, native modules, precompiled chunks — and
// bounds the two resources an accident consumes: wall-clock time and memory.
//
// What a script gets: the base library (minus the loaders), string, table,
// math, utf8, and a reduced `os` holding only the clock/date functions. What
// it does not get: io (entirely), os.execute/remove/rename/getenv/exit,
// require/package (never opened), dofile/loadfile/load (removed — chunks come
// in through the console, and accepting bytecode strings would bypass the
// source-only rule below), and the debug library (its introspection is how
// sandboxes get unpicked).
//
// Enforcement details that matter:
//   - Chunks load with mode "t": SOURCE only. Precompiled Lua bytecode is not
//     validated by the VM and a crafted chunk can escape any sandbox, so it is
//     refused at the door.
//   - A count hook checks the deadline every 50,000 instructions. A script
//     that overruns dies with a Lua error, not a frozen application. The hook
//     also honours a cancel flag for a future Stop button.
//   - The allocator refuses growth past a byte budget, so a table-append loop
//     dies as "not enough memory" long before the machine notices.
#pragma once

#include <QElapsedTimer>
#include <QString>

#include <atomic>
#include <cstddef>
#include <functional>

struct lua_State;

namespace ct {

class LuaSandbox
{
public:
    // Every print()'s assembled line, without a trailing newline.
    using OutputFn = std::function<void(const QString &)>;

    LuaSandbox();
    ~LuaSandbox();
    LuaSandbox(const LuaSandbox &) = delete;
    LuaSandbox &operator=(const LuaSandbox &) = delete;

    void setOutput(OutputFn fn) { m_output = std::move(fn); }
    void setTimeLimitMs(qint64 ms) { m_timeLimitMs = ms; }
    void setMemoryLimitBytes(size_t bytes) { m_memLimit = bytes; }

    // Create the state and install the restricted environment. Returns false
    // (with *error) only on out-of-memory during setup.
    bool open(QString *error);

    // Load `source` as a text-only chunk and run it. The deadline starts here.
    // Returns false with *error carrying the Lua error plus a traceback.
    bool run(const QString &source, const QString &chunkName, QString *error);

    // For a future Stop button: the hook raises an error at the next check.
    void cancel() { m_cancelled.store(true); }

    lua_State *state() { return m_state; }

private:
    static void *boundedAlloc(void *ud, void *ptr, size_t osize, size_t nsize);
    static void deadlineHook(lua_State *L, void *debugInfo);

    lua_State *m_state = nullptr;
    OutputFn m_output;

    qint64 m_timeLimitMs = 10000;
    size_t m_memLimit = 64u * 1024u * 1024u;
    size_t m_memUsed = 0;
    QElapsedTimer m_clock;
    std::atomic_bool m_cancelled { false };

    friend struct SandboxAccess;
};

} // namespace ct
