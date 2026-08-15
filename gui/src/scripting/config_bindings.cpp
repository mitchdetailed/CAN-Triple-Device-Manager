#include "config_bindings.h"

#include "../model/channel_catalog.h"
#include "../model/configuration.h"
#include "../model/validation.h"
#include "../protocol/wire_structs.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

namespace ct {
namespace {

// ---------------------------------------------------------------- plumbing

BindingContext *ctx(lua_State *L)
{
    return static_cast<BindingContext *>(lua_touserdata(L, lua_upvalueindex(1)));
}

Configuration *cfg(lua_State *L)
{
    return ctx(L)->config;
}

void touch(lua_State *L)
{
    ctx(L)->mutated = true;
}

void pushQString(lua_State *L, const QString &s)
{
    const QByteArray utf8 = s.toUtf8();
    lua_pushlstring(L, utf8.constData(), size_t(utf8.size()));
}

QString checkQString(lua_State *L, int idx)
{
    size_t len = 0;
    const char *s = luaL_checklstring(L, idx, &len);
    return QString::fromUtf8(s, int(len));
}

// Field readers for {key = value} argument tables. Each pops nothing and
// leaves the stack as found; `required` turns absence into an error naming
// the field, which is the difference between a usable script error and
// "attempt to compare nil".
bool getField(lua_State *L, int idx, const char *key)
{
    lua_getfield(L, idx, key);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return false;
    }
    return true; // caller pops
}

QString fieldString(lua_State *L, int idx, const char *key, const QString &fallback,
                    bool required = false)
{
    if (!getField(L, idx, key)) {
        if (required) {
            luaL_error(L, "missing required field '%s'", key);
        }
        return fallback;
    }
    if (!lua_isstring(L, -1)) {
        luaL_error(L, "field '%s' must be a string", key);
    }
    size_t len = 0;
    const char *s = lua_tolstring(L, -1, &len);
    QString out = QString::fromUtf8(s, int(len));
    lua_pop(L, 1);
    return out;
}

double fieldNumber(lua_State *L, int idx, const char *key, double fallback,
                   bool required = false)
{
    if (!getField(L, idx, key)) {
        if (required) {
            luaL_error(L, "missing required field '%s'", key);
        }
        return fallback;
    }
    if (!lua_isnumber(L, -1)) {
        luaL_error(L, "field '%s' must be a number", key);
    }
    const double v = lua_tonumber(L, -1);
    lua_pop(L, 1);
    return v;
}

int fieldInt(lua_State *L, int idx, const char *key, int fallback, bool required = false)
{
    return int(fieldNumber(L, idx, key, fallback, required));
}

bool fieldBool(lua_State *L, int idx, const char *key, bool fallback)
{
    if (!getField(L, idx, key)) {
        return fallback;
    }
    const bool v = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    return v;
}

void setStr(lua_State *L, const char *key, const QString &value)
{
    pushQString(L, value);
    lua_setfield(L, -2, key);
}

void setNum(lua_State *L, const char *key, double value)
{
    lua_pushnumber(L, value);
    lua_setfield(L, -2, key);
}

void setInt(lua_State *L, const char *key, lua_Integer value)
{
    lua_pushinteger(L, value);
    lua_setfield(L, -2, key);
}

void setBool(lua_State *L, const char *key, bool value)
{
    lua_pushboolean(L, value);
    lua_setfield(L, -2, key);
}

// ---------------------------------------------------------------- guards

// The single place the protected-comms rules are enforced for scripts,
// mirroring the UI's suppression sites. Raises; never returns on violation.
//
// isChannelEditLocked, NOT isChannelConcealed. Every caller of this is about to
// CHANGE a channel, and the two questions came apart in 2.3.0: a Read Only
// message is fully visible and still must not be edited, so its channels are
// precisely the ones the concealment predicate would now wave through. The
// commsRevealed() short-circuit went with it — the edit lock is deliberately not
// lifted by the password, because the password buys the right to untick the
// tier and unticking is what unlocks editing.
void guardChannel(lua_State *L, const QString &name)
{
    if (cfg(L)->isChannelEditLocked(name)) {
        luaL_error(L,
                   "channel '%s' belongs to a protected message - its definition is that "
                   "message's, and it stays read-only until the message's protection is "
                   "unticked in Connections > Communications",
                   name.toUtf8().constData());
    }
}

CommsSection *findSection(lua_State *L, int busIdx, const QString &name)
{
    BusConfig &bus = cfg(L)->bus[busIdx];
    for (CommsSection &s : bus.sections) {
        if (s.name == name) {
            return &s;
        }
    }
    return nullptr;
}

int checkBusIndex(lua_State *L, int arg)
{
    const lua_Integer n = luaL_checkinteger(L, arg);
    if (n < 1 || n > 3) {
        luaL_error(L, "bus must be 1..3 (got %d)", int(n));
    }
    return int(n) - 1;
}

// Two guards, because the single old one answered two questions that have come
// apart. Pick by what the caller is about to do:
//
//   guardSectionReadable   about to HAND BACK the section's protocol detail.
//                          Refused only while it is concealed — Read Only is
//                          fully readable, by definition.
//   guardSectionWritable   about to CHANGE it. Refused at every tier, Read Only
//                          included, and NOT lifted by the password: revealing
//                          buys viewing and the right to untick, and unticking
//                          is what unlocks editing. A script cannot untick — see
//                          applySectionFields — so for a script the refusal is
//                          simply final.
//
// Fusing them is the bug this split exists to prevent: it would leave a Read
// Only message freely rewritable from a batch script, which is the one path a
// user would never watch.
void guardSectionReadable(lua_State *L, const CommsSection &s, int busIndex)
{
    // The bus is passed because every caller knows it: a grant taken for a
    // same-named section on another bus must not hand this one's protocol to a
    // script.
    if (s.isConcealed(cfg(L)->isSectionRevealed(s, busIndex))) {
        luaL_error(L,
                   "section '%s' is withheld - open it in Connections > Communications with "
                   "its password first",
                   s.name.toUtf8().constData());
    }
}

void guardSectionWritable(lua_State *L, const CommsSection &s)
{
    if (s.isEditLocked()) {
        luaL_error(L,
                   "section '%s' is protected - untick its protection in Connections > "
                   "Communications before a script may change it",
                   s.name.toUtf8().constData());
    }
}

QString checkChannelNameLength(lua_State *L, const QString &name)
{
    if (name.isEmpty()) {
        luaL_error(L, "a channel name cannot be empty");
    }
    if (name.toUtf8().size() > MAX_CHANNEL_NAME_BYTES) {
        luaL_error(L, "channel name '%s' is longer than %d bytes",
                   name.toUtf8().constData(), MAX_CHANNEL_NAME_BYTES);
    }
    return name;
}

// ---------------------------------------------------------------- channels

void pushChannel(lua_State *L, const Channel &c)
{
    lua_createtable(L, 0, 9);
    setStr(L, "name", c.name);
    setStr(L, "quantity", c.quantity);
    setStr(L, "unit", c.unit);
    setStr(L, "dataType", c.dataType);
    setNum(L, "baseResolution", c.baseResolution);
    setInt(L, "decimalPlaces", c.decimalPlaces);
    setNum(L, "minValue", c.minValue);
    setNum(L, "maxValue", c.maxValue);
    setStr(L, "category", c.category);
}

// Read channel fields from the table at `idx` over `base` (which carries the
// defaults — an existing channel for updates, a fresh one for adds).
Channel readChannel(lua_State *L, int idx, Channel base)
{
    base.name = checkChannelNameLength(L, fieldString(L, idx, "name", base.name, true));
    base.quantity = fieldString(L, idx, "quantity", base.quantity);
    base.unit = fieldString(L, idx, "unit", base.unit);
    base.dataType = fieldString(L, idx, "dataType", base.dataType);
    base.baseResolution = fieldNumber(L, idx, "baseResolution", base.baseResolution);
    base.decimalPlaces = fieldInt(L, idx, "decimalPlaces", base.decimalPlaces);
    base.minValue = fieldNumber(L, idx, "minValue", base.minValue);
    base.maxValue = fieldNumber(L, idx, "maxValue", base.maxValue);
    base.category = fieldString(L, idx, "category", base.category);
    base.userDefined = true;
    return base;
}

int l_channels(lua_State *L)
{
    const QList<Channel> all = cfg(L)->catalog().userChannels();
    lua_createtable(L, all.size(), 0);
    for (int i = 0; i < all.size(); ++i) {
        pushChannel(L, all[i]);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

int l_channelCount(lua_State *L)
{
    lua_pushinteger(L, cfg(L)->catalog().userChannels().size());
    return 1;
}

int l_channel(lua_State *L)
{
    const Channel c = cfg(L)->catalog().findByName(checkQString(L, 1));
    if (!c.isValid()) {
        lua_pushnil(L);
        return 1;
    }
    pushChannel(L, c);
    return 1;
}

int l_addChannel(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    const Channel c = readChannel(L, 1, Channel{});
    if (ChannelCatalog::isDeviceChannel(c.name)) {
        luaL_error(L, "'%s' is a built-in device channel and cannot be created",
                   c.name.toUtf8().constData());
    }
    if (cfg(L)->catalog().findByName(c.name).isValid()) {
        luaL_error(L, "channel '%s' already exists - use ct.setChannel to update it",
                   c.name.toUtf8().constData());
    }
    cfg(L)->catalog().addOrUpdateUserChannel(c);
    touch(L);
    return 0;
}

int l_setChannel(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    const QString name = fieldString(L, 1, "name", {}, true);
    guardChannel(L, name);
    if (ChannelCatalog::isDeviceChannel(name)) {
        luaL_error(L, "'%s' is a built-in device channel and cannot be edited",
                   name.toUtf8().constData());
    }
    const Channel existing = cfg(L)->catalog().findByName(name);
    const Channel updated =
        readChannel(L, 1, existing.isValid() ? existing : Channel{});
    cfg(L)->catalog().addOrUpdateUserChannel(updated);
    touch(L);
    return 0;
}

int l_removeChannel(lua_State *L)
{
    const QString name = checkQString(L, 1);
    // NO guardChannel here either, for the same reason l_removeSection has none:
    // removal is permitted at every tier. Removing a channel definition from the
    // catalogue does not alter the protected message that carries it — the
    // message keeps its row and its bit layout — so this cannot be a route to
    // changing what a protected message decodes to, which is the thing
    // guardChannel exists to stop.
    if (!cfg(L)->catalog().findByName(name).isValid()) {
        luaL_error(L, "no channel named '%s'", name.toUtf8().constData());
    }
    cfg(L)->catalog().removeUserChannel(name);
    touch(L);
    return 0;
}

int l_renameChannel(lua_State *L)
{
    const QString oldName = checkQString(L, 1);
    const QString newName = checkChannelNameLength(L, checkQString(L, 2));
    guardChannel(L, oldName);
    Configuration *c = cfg(L);

    const Channel existing = c->catalog().findByName(oldName);
    if (!existing.isValid()) {
        luaL_error(L, "no channel named '%s'", oldName.toUtf8().constData());
    }
    if (c->catalog().findByName(newName).isValid()) {
        luaL_error(L, "channel '%s' already exists", newName.toUtf8().constData());
    }
    if (ChannelCatalog::isDeviceChannel(newName)) {
        luaL_error(L, "'%s' is a built-in device channel name",
                   newName.toUtf8().constData());
    }

    Channel renamed = existing;
    renamed.name = newName;
    c->catalog().removeUserChannel(oldName);
    c->catalog().addOrUpdateUserChannel(renamed);
    const int refs = c->renameChannelReferences(oldName, newName);
    touch(L);
    lua_pushinteger(L, refs);
    return 1;
}

int l_allocatedChannels(lua_State *L)
{
    const QStringList names = cfg(L)->allocatedChannelNames();
    lua_createtable(L, names.size(), 0);
    for (int i = 0; i < names.size(); ++i) {
        pushQString(L, names[i]);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

int l_generatedChannels(lua_State *L)
{
    const QStringList names = cfg(L)->generatedChannelNames();
    lua_createtable(L, names.size(), 0);
    for (int i = 0; i < names.size(); ++i) {
        pushQString(L, names[i]);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

// Kept under its old name — it is a published scripting API and renaming it
// would break every script that calls it — but it now answers the EDIT LOCK.
// That is the question a script actually asks it: "which of these may I not
// write?" A script that wanted the concealed set instead can read `concealed`
// off ct.sections(), which is where that distinction is visible.
int l_protectedChannels(lua_State *L)
{
    const QStringList names = cfg(L)->editLockedChannelNames();
    lua_createtable(L, names.size(), 0);
    for (int i = 0; i < names.size(); ++i) {
        pushQString(L, names[i]);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

// ---------------------------------------------------------------- document

int l_title(lua_State *L)
{
    pushQString(L, cfg(L)->configTitle());
    return 1;
}

int l_setTitle(lua_State *L)
{
    const QString title = checkQString(L, 1);
    if (title.toUtf8().size() > 32) {
        luaL_error(L, "the configuration title is limited to 32 bytes");
    }
    cfg(L)->setConfigTitle(title);
    touch(L);
    return 0;
}

int l_comments(lua_State *L)
{
    pushQString(L, cfg(L)->comments);
    return 1;
}

int l_setComments(lua_State *L)
{
    cfg(L)->comments = checkQString(L, 1);
    touch(L);
    return 0;
}

// ---------------------------------------------------------------- buses

int l_bus(lua_State *L)
{
    const int idx = checkBusIndex(L, 1);
    const BusConfig &bus = cfg(L)->bus[idx];
    lua_createtable(L, 0, 5);
    setBool(L, "enabled", bus.enabled);
    setInt(L, "rateKbps", bus.rateKbps);
    setInt(L, "dataRateKbps", bus.dataRateKbps);
    setBool(L, "termination", bus.termination);
    setInt(L, "sectionCount", bus.sections.size());
    return 1;
}

int l_setBus(lua_State *L)
{
    const int idx = checkBusIndex(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    BusConfig &bus = cfg(L)->bus[idx];
    bus.enabled = fieldBool(L, 2, "enabled", bus.enabled);
    bus.rateKbps = fieldInt(L, 2, "rateKbps", bus.rateKbps);
    bus.dataRateKbps = fieldInt(L, 2, "dataRateKbps", bus.dataRateKbps);
    bus.termination = fieldBool(L, 2, "termination", bus.termination);
    touch(L);
    return 0;
}

// ---------------------------------------------------------------- sections

QString deviceToString(SectionDevice d)
{
    switch (d) {
    case SectionDevice::Off: return QStringLiteral("off");
    case SectionDevice::ReceiveMessage: return QStringLiteral("receive");
    case SectionDevice::TransmitMessage: return QStringLiteral("transmit");
    // The same token the .ct3 file uses, so a script and a saved file agree
    // about what a section is called.
    case SectionDevice::TransmitCrc8: return QStringLiteral("transmitCrc8");
    case SectionDevice::MessageRelay: return QStringLiteral("relay");
    }
    return QStringLiteral("off");
}

SectionDevice deviceFromString(lua_State *L, const QString &s)
{
    if (s == QLatin1String("off")) return SectionDevice::Off;
    if (s == QLatin1String("receive")) return SectionDevice::ReceiveMessage;
    if (s == QLatin1String("transmit")) return SectionDevice::TransmitMessage;
    if (s == QLatin1String("transmitCrc8")) return SectionDevice::TransmitCrc8;
    if (s == QLatin1String("relay")) return SectionDevice::MessageRelay;
    luaL_error(L,
               "device must be 'off', 'receive', 'transmit', 'transmitCrc8' or 'relay' (got '%s')",
               s.toUtf8().constData());
    return SectionDevice::Off; // unreachable
}

int dbcTypeFrom(lua_State *L, int idx, const char *key, int fallback)
{
    if (!getField(L, idx, key)) {
        return fallback;
    }
    int result = fallback;
    if (lua_isnumber(L, -1)) {
        result = int(lua_tointeger(L, -1));
        if (result < 0 || result > 2) {
            luaL_error(L, "dbcType must be 0..2 or 'unsigned'/'signed'/'float'");
        }
    } else {
        const QString s = QString::fromUtf8(lua_tostring(L, -1));
        if (s == QLatin1String("unsigned")) result = int(DbcType::Unsigned);
        else if (s == QLatin1String("signed")) result = int(DbcType::Signed);
        else if (s == QLatin1String("float")) result = int(DbcType::IEEE754);
        else luaL_error(L, "dbcType must be 'unsigned', 'signed' or 'float' (got '%s')",
                        s.toUtf8().constData());
    }
    lua_pop(L, 1);
    return result;
}

void pushRow(lua_State *L, const CommsChannelRow &r)
{
    lua_createtable(L, 0, 7);
    setStr(L, "channel", r.channelName);
    setInt(L, "startBit", r.startBit);
    setInt(L, "bitLength", r.bitLength);
    switch (DbcType(r.dbcType)) {
    case DbcType::Unsigned: setStr(L, "dbcType", QStringLiteral("unsigned")); break;
    case DbcType::Signed: setStr(L, "dbcType", QStringLiteral("signed")); break;
    case DbcType::IEEE754: setStr(L, "dbcType", QStringLiteral("float")); break;
    }
    setNum(L, "factor", r.dbcFactor);
    setNum(L, "offset", r.dbcOffset);
    setNum(L, "default", r.defaultValue);
}

CommsChannelRow readRow(lua_State *L, int idx)
{
    luaL_checktype(L, idx, LUA_TTABLE);
    CommsChannelRow r;
    r.channelName = fieldString(L, idx, "channel", {}, true);
    r.startBit = fieldInt(L, idx, "startBit", r.startBit);
    r.bitLength = fieldInt(L, idx, "bitLength", r.bitLength);
    r.dbcType = dbcTypeFrom(L, idx, "dbcType", r.dbcType);
    r.dbcFactor = fieldNumber(L, idx, "factor", r.dbcFactor);
    r.dbcOffset = fieldNumber(L, idx, "offset", r.dbcOffset);
    r.defaultValue = fieldNumber(L, idx, "default", r.defaultValue);
    if (r.bitLength < 1 || r.bitLength > 64) {
        luaL_error(L, "bitLength must be 1..64 (row for '%s')",
                   r.channelName.toUtf8().constData());
    }
    if (r.startBit < 0 || r.startBit > 511) {
        luaL_error(L, "startBit must be 0..511 (row for '%s')",
                   r.channelName.toUtf8().constData());
    }
    return r;
}

// rows = { {channel=..., ...}, ... } read from the field `key` at `idx`.
QList<CommsChannelRow> readRows(lua_State *L, int idx, const char *key, bool *present)
{
    QList<CommsChannelRow> rows;
    if (present) {
        *present = false;
    }
    if (!getField(L, idx, key)) {
        return rows;
    }
    if (!lua_istable(L, -1)) {
        luaL_error(L, "'%s' must be an array of row tables", key);
    }
    if (present) {
        *present = true;
    }
    const int n = int(luaL_len(L, -1));
    rows.reserve(n);
    for (int i = 1; i <= n; ++i) {
        lua_rawgeti(L, -1, i);
        rows.append(readRow(L, lua_gettop(L)));
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    return rows;
}

// The scalar fields of a section, shared by list and detail views. Never
// called for a concealed section — the callers guard first.
void pushSectionScalars(lua_State *L, const CommsSection &s)
{
    setStr(L, "name", s.name);
    setStr(L, "device", deviceToString(s.device));
    setInt(L, "id", s.baseAddress);
    setBool(L, "extended", s.extended);
    setBool(L, "fd", s.fd);
    setInt(L, "lengthBytes", s.messageLengthBytes);
    setStr(L, "alignment",
           s.alignment == SectionAlignment::WordSwap ? QStringLiteral("wordswap")
                                                     : QStringLiteral("normal"));
    setBool(L, "compound", s.compound);
    // One ordered tier as a stable token, replacing the v19 `protected = <bool>`
    // — the same four values the .ct3 writes, so a script and a file describe a
    // section the same way. Emitted for None too (as "none"), unlike the JSON,
    // because a script reading s.protection must never get nil for an ordinary
    // message and have to know that nil means None.
    setStr(L, "protection",
           s.protection == CommsProtection::None ? QStringLiteral("none")
                                                 : commsProtectionToken(s.protection));
    // Both halves of the predicate that split, so a script can ask the question
    // it actually has rather than re-deriving it from the tier: `concealed` is
    // "may I read the detail", `editLocked` is "may I change it". They are not
    // the same set — Read Only is visible and locked.
    setBool(L, "editLocked", s.isEditLocked());
    if (s.isTransmit()) {
        setBool(L, "cyclic", s.cyclic);
        setInt(L, "rateHz", s.transmitRateHz);
    }
    if (s.isReceive()) {
        setInt(L, "timeoutMs", s.receiveTimeoutMs);
    }
}

int l_sections(lua_State *L)
{
    const int busIndex = checkBusIndex(L, 1);
    const QList<CommsSection> &sections = cfg(L)->bus[busIndex].sections;

    lua_createtable(L, sections.size(), 0);
    for (int i = 0; i < sections.size(); ++i) {
        const CommsSection &s = sections[i];
        // The bus is known here, so the reveal question is asked per bus: a
        // grant taken for a same-named section on ANOTHER bus must not open this
        // one to a script.
        const bool concealed = s.isConcealed(cfg(L)->isSectionRevealed(s, busIndex));
        if (concealed) {
            // The UI shows a withheld section's NAME and nothing else; the
            // script sees exactly the same. Even the device type is detail.
            // Notably `id` is ABSENT rather than zero — a script must not be
            // able to tell "no ID" from "ID withheld", and nil is the only
            // answer that does not invite a guess.
            lua_createtable(L, 0, 4);
            setStr(L, "name", s.name);
            setStr(L, "protection", commsProtectionToken(s.protection));
            setBool(L, "concealed", true);
            setBool(L, "editLocked", true);
        } else {
            // A Read Only section returns its FULL scalar table, id and all. It
            // conceals nothing; only `editLocked` marks it out, and that is the
            // whole point of the tier.
            lua_createtable(L, 0, 14);
            pushSectionScalars(L, s);
            setBool(L, "concealed", false);
        }
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

int l_section(lua_State *L)
{
    const int busIdx = checkBusIndex(L, 1);
    const QString name = checkQString(L, 2);
    CommsSection *s = findSection(L, busIdx, name);
    if (!s) {
        lua_pushnil(L);
        return 1;
    }
    // READABLE, not writable. ct.section() hands back detail; it changes
    // nothing, and since 2.3.0 the two are different privileges — a Read Only
    // section is fully readable here and still refuses ct.setSection().
    guardSectionReadable(L, *s, busIdx);

    lua_createtable(L, 0, 16);
    pushSectionScalars(L, *s);
    setBool(L, "concealed", false); // it got past the guard, so it is not
    if (s->compound) {
        lua_createtable(L, s->identifiers.size(), 0);
        for (int i = 0; i < s->identifiers.size(); ++i) {
            const CompoundIdentifier &ident = s->identifiers[i];
            lua_createtable(L, 0, 4);
            setInt(L, "byteOffset", ident.byteOffset);
            setInt(L, "id", ident.id);
            setInt(L, "idMask", ident.idMask);
            lua_createtable(L, ident.rows.size(), 0);
            for (int r = 0; r < ident.rows.size(); ++r) {
                pushRow(L, ident.rows[r]);
                lua_rawseti(L, -2, r + 1);
            }
            lua_setfield(L, -2, "rows");
            lua_rawseti(L, -2, i + 1);
        }
        lua_setfield(L, -2, "identifiers");
    } else {
        lua_createtable(L, s->rows.size(), 0);
        for (int r = 0; r < s->rows.size(); ++r) {
            pushRow(L, s->rows[r]);
            lua_rawseti(L, -2, r + 1);
        }
        lua_setfield(L, -2, "rows");
    }
    return 1;
}

// Apply the writable scalar fields from the table at `idx` onto `s`.
void applySectionFields(lua_State *L, int idx, CommsSection &s)
{
    s.device = deviceFromString(L, fieldString(L, idx, "device", deviceToString(s.device)));
    s.baseAddress = quint32(fieldNumber(L, idx, "id", s.baseAddress));
    s.extended = fieldBool(L, idx, "extended", s.extended);
    s.fd = fieldBool(L, idx, "fd", s.fd);
    s.messageLengthBytes = fieldInt(L, idx, "lengthBytes", s.messageLengthBytes);
    const QString align = fieldString(
        L, idx, "alignment",
        s.alignment == SectionAlignment::WordSwap ? QStringLiteral("wordswap")
                                                  : QStringLiteral("normal"));
    if (align == QLatin1String("normal")) {
        s.alignment = SectionAlignment::Normal;
    } else if (align == QLatin1String("wordswap")) {
        s.alignment = SectionAlignment::WordSwap;
    } else {
        luaL_error(L, "alignment must be 'normal' or 'wordswap' (got '%s')",
                   align.toUtf8().constData());
    }
    s.cyclic = fieldBool(L, idx, "cyclic", s.cyclic);
    s.transmitRateHz = fieldInt(L, idx, "rateHz", s.transmitRateHz);
    s.receiveTimeoutMs = fieldInt(L, idx, "timeoutMs", s.receiveTimeoutMs);
    // `protection` is deliberately NOT settable from a script, IN EITHER
    // DIRECTION, and it is refused loudly rather than ignored — a script that
    // set it and was silently obeyed only in the raising direction would be
    // worse than one that failed.
    //
    // Marking a message is a statement about who may see it and who may change
    // it, and a batch tool silently flipping that is how a protection scheme
    // stops meaning anything. The direction that matters most is DOWN: the
    // untick is guarded by a password challenge that only a dialog can run — the
    // section's own password for Read Only and Hidden, a live device confirming
    // Edit Protected Comms for Protected — and there is nowhere in a script for
    // either of those to be asked. Scripting would otherwise be the obvious way
    // past the whole rule. The section editor is the one place a tier changes.
    //
    // A field that MATCHES what the section already has is allowed through, so
    // the read-modify-write a script naturally writes —
    // `ct.setSection(b, n, ct.section(b, n))` after changing one row — is not
    // broken by the very key ct.section() hands back. Only an actual change is
    // refused.
    const QString current = s.protection == CommsProtection::None
                                ? QStringLiteral("none")
                                : commsProtectionToken(s.protection);
    if (fieldString(L, idx, "protection", current) != current) {
        luaL_error(L, "a section's protection cannot be changed from a script - change it in "
                      "Connections > Communications, where the password can be asked for");
    }

    bool rowsPresent = false;
    const QList<CommsChannelRow> rows = readRows(L, idx, "rows", &rowsPresent);
    if (rowsPresent) {
        if (s.compound) {
            luaL_error(L, "section '%s' is compound - phase 1 scripting edits "
                          "simple sections' rows only",
                       s.name.toUtf8().constData());
        }
        s.rows = rows;
    }
}

int l_addSection(lua_State *L)
{
    const int busIdx = checkBusIndex(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);

    CommsSection s;
    s.name = fieldString(L, 2, "name", {}, true);
    if (s.name.isEmpty()) {
        luaL_error(L, "a section needs a name");
    }
    if (findSection(L, busIdx, s.name)) {
        luaL_error(L, "bus %d already has a section named '%s'", busIdx + 1,
                   s.name.toUtf8().constData());
    }
    applySectionFields(L, 2, s);
    cfg(L)->bus[busIdx].sections.append(s);
    touch(L);
    return 0;
}

int l_setSection(lua_State *L)
{
    const int busIdx = checkBusIndex(L, 1);
    const QString name = checkQString(L, 2);
    luaL_checktype(L, 3, LUA_TTABLE);
    CommsSection *s = findSection(L, busIdx, name);
    if (!s) {
        luaL_error(L, "bus %d has no section named '%s'", busIdx + 1,
                   name.toUtf8().constData());
    }
    guardSectionWritable(L, *s);
    // Renames go through the same call: name = "New Name" in the table.
    const QString newName = fieldString(L, 3, "name", s->name);
    if (newName != s->name && findSection(L, busIdx, newName)) {
        luaL_error(L, "bus %d already has a section named '%s'", busIdx + 1,
                   newName.toUtf8().constData());
    }
    s->name = newName;
    applySectionFields(L, 3, *s);
    touch(L);
    return 0;
}

int l_removeSection(lua_State *L)
{
    const int busIdx = checkBusIndex(L, 1);
    const QString name = checkQString(L, 2);
    BusConfig &bus = cfg(L)->bus[busIdx];
    for (int i = 0; i < bus.sections.size(); ++i) {
        if (bus.sections[i].name == name) {
            // NO GUARD, deliberately. Removal is permitted at every tier — the
            // spec says so three times, the Communications Setup dialog has
            // always allowed it, and the firmware's CMD_CLEAR_CONFIG gate was
            // deleted in 2.3.0. This was the ONE place in the host that still
            // refused, which made scripting the only path where the rule did not
            // hold. Protecting a message protects its protocol, not its place in
            // the customer's configuration.
            //
            // It is not a hole. For Hidden and Protected, remove-and-recreate
            // DESTROYS rather than reveals: the operator cannot see what to
            // retype. For Read Only it is a genuine bypass, which is exactly why
            // Read Only is described as accident prevention and never as
            // security.
            bus.sections.removeAt(i);
            touch(L);
            return 0;
        }
    }
    luaL_error(L, "bus %d has no section named '%s'", busIdx + 1,
               name.toUtf8().constData());
    return 0;
}

// ---------------------------------------------------------------- constants

int l_constants(lua_State *L)
{
    const QList<ConstantRow> &rows = cfg(L)->constantRows;
    lua_createtable(L, rows.size(), 0);
    for (int i = 0; i < rows.size(); ++i) {
        lua_createtable(L, 0, 5);
        setStr(L, "name", rows[i].name);
        setStr(L, "dataType", rows[i].dataType);
        setInt(L, "decimalPlaces", rows[i].decimalPlaces);
        setNum(L, "value", rows[i].value);
        setBool(L, "active", rows[i].active);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

int l_addConstant(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    ConstantRow row;
    row.name = checkChannelNameLength(L, fieldString(L, 1, "name", {}, true));
    for (const ConstantRow &existing : cfg(L)->constantRows) {
        if (existing.name == row.name) {
            luaL_error(L, "a constant named '%s' already exists",
                       row.name.toUtf8().constData());
        }
    }
    row.dataType = fieldString(L, 1, "dataType", QStringLiteral("float"));
    row.decimalPlaces = fieldInt(L, 1, "decimalPlaces", 0);
    row.value = fieldNumber(L, 1, "value", 0.0, true);
    row.active = fieldBool(L, 1, "active", true);
    cfg(L)->constantRows.append(row);
    touch(L);
    return 0;
}

int l_removeConstant(lua_State *L)
{
    const QString name = checkQString(L, 1);
    QList<ConstantRow> &rows = cfg(L)->constantRows;
    for (int i = 0; i < rows.size(); ++i) {
        if (rows[i].name == name) {
            rows.removeAt(i);
            touch(L);
            return 0;
        }
    }
    luaL_error(L, "no constant named '%s'", name.toUtf8().constData());
    return 0;
}

// ---------------------------------------------------------------- maths

// The op-name table used to live here. It moved to wire_structs.h as kMathOps
// when the script disassembler needed the same value -> name direction: a
// script opcode in 0x00..0x1E IS a MathOp, so a listing that names an
// instruction is answering the question this table answers, and two copies
// would drift the first time an op was added. The count assert moved with it.

int opFromArg(lua_State *L, int idx, const char *key)
{
    if (!getField(L, idx, key)) {
        luaL_error(L, "missing required field '%s'", key);
    }
    int op = -1;
    if (lua_isnumber(L, -1)) {
        op = int(lua_tointeger(L, -1));
    } else if (lua_isstring(L, -1)) {
        const char *name = lua_tostring(L, -1);
        for (const MathOpEntry &e : kMathOps) {
            if (std::strcmp(e.name, name) == 0) {
                op = e.value;
                break;
            }
        }
        if (op < 0) {
            luaL_error(L, "unknown math op '%s' (see ct.ops)", name);
        }
    } else {
        luaL_error(L, "'%s' must be a number or an op name string", key);
    }
    lua_pop(L, 1);
    if (op < 0 || op > MATH_OP_WRAP) {
        luaL_error(L, "math op %d is out of range 0..%d", op, MATH_OP_WRAP);
    }
    return op;
}

// An operand is a channel (string) or a constant (number).
void readOperand(lua_State *L, int idx, const char *key, bool *isChannel,
                 QString *channel, double *constant)
{
    if (!getField(L, idx, key)) {
        return; // defaults stand (const 0)
    }
    if (lua_type(L, -1) == LUA_TSTRING) {
        *isChannel = true;
        size_t len = 0;
        const char *s = lua_tolstring(L, -1, &len);
        *channel = QString::fromUtf8(s, int(len));
    } else if (lua_isnumber(L, -1)) {
        *isChannel = false;
        *constant = lua_tonumber(L, -1);
    } else {
        luaL_error(L, "operand '%s' must be a channel name (string) or a constant "
                      "(number)", key);
    }
    lua_pop(L, 1);
}

void pushOperand(lua_State *L, const char *key, bool isChannel, const QString &channel,
                 double constant)
{
    if (isChannel) {
        pushQString(L, channel);
    } else {
        lua_pushnumber(L, constant);
    }
    lua_setfield(L, -2, key);
}

int l_mathRows(lua_State *L)
{
    const QList<MathRow> &rows = cfg(L)->mathRows;
    lua_createtable(L, rows.size(), 0);
    for (int i = 0; i < rows.size(); ++i) {
        const MathRow &m = rows[i];
        lua_createtable(L, 0, 7);
        setInt(L, "op", m.op);
        const char *opName = mathOpName(m.op);
        if (*opName != '\0') {
            setStr(L, "opName", QString::fromLatin1(opName));
        }
        pushOperand(L, "a", m.aIsChannel, m.aChannel, m.aConst);
        pushOperand(L, "b", m.bIsChannel, m.bChannel, m.bConst);
        pushOperand(L, "c", m.cIsChannel, m.cChannel, m.cConst);
        setStr(L, "dest", m.destChannel);
        setBool(L, "active", m.active);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

int l_addMath(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    MathRow m;
    m.op = opFromArg(L, 1, "op");
    m.destChannel = fieldString(L, 1, "dest", {}, true);
    readOperand(L, 1, "a", &m.aIsChannel, &m.aChannel, &m.aConst);
    readOperand(L, 1, "b", &m.bIsChannel, &m.bChannel, &m.bConst);
    readOperand(L, 1, "c", &m.cIsChannel, &m.cChannel, &m.cConst);
    m.active = fieldBool(L, 1, "active", true);
    cfg(L)->mathRows.append(m);
    touch(L);
    return 0;
}

int l_removeMath(lua_State *L)
{
    const lua_Integer idx = luaL_checkinteger(L, 1);
    QList<MathRow> &rows = cfg(L)->mathRows;
    if (idx < 1 || idx > rows.size()) {
        luaL_error(L, "math row %d does not exist (1..%d)", int(idx), rows.size());
    }
    rows.removeAt(int(idx) - 1);
    touch(L);
    return 0;
}

// ------------------------------------------------- read-only calculation rows

int l_conditions(lua_State *L)
{
    const QList<ConditionRow> &rows = cfg(L)->conditionRows;
    lua_createtable(L, rows.size(), 0);
    for (int i = 0; i < rows.size(); ++i) {
        lua_createtable(L, 0, 3);
        setStr(L, "output", rows[i].outputChannel);
        setBool(L, "active", rows[i].active);
        const QStringList inputs = rows[i].inputChannels();
        lua_createtable(L, inputs.size(), 0);
        for (int j = 0; j < inputs.size(); ++j) {
            pushQString(L, inputs[j]);
            lua_rawseti(L, -2, j + 1);
        }
        lua_setfield(L, -2, "inputs");
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

int l_counters(lua_State *L)
{
    const QList<CounterRow> &rows = cfg(L)->counterRows;
    lua_createtable(L, rows.size(), 0);
    for (int i = 0; i < rows.size(); ++i) {
        lua_createtable(L, 0, 3);
        setStr(L, "output", rows[i].outputChannel);
        setInt(L, "mode", rows[i].mode);
        setBool(L, "active", rows[i].active);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

int l_timers(lua_State *L)
{
    const QList<TimerRow> &rows = cfg(L)->timerRows;
    lua_createtable(L, rows.size(), 0);
    for (int i = 0; i < rows.size(); ++i) {
        lua_createtable(L, 0, 2);
        setStr(L, "output", rows[i].outputChannel);
        setBool(L, "active", rows[i].active);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

int l_integrators(lua_State *L)
{
    const QList<IntegratorRow> &rows = cfg(L)->integratorRows;
    lua_createtable(L, rows.size(), 0);
    for (int i = 0; i < rows.size(); ++i) {
        lua_createtable(L, 0, 3);
        setStr(L, "output", rows[i].outputChannel);
        setInt(L, "rateHz", rows[i].rateHz);
        setBool(L, "active", rows[i].active);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

int l_tables2x16(lua_State *L)
{
    const QList<Table2x16Row> &rows = cfg(L)->table2x16Rows;
    lua_createtable(L, rows.size(), 0);
    for (int i = 0; i < rows.size(); ++i) {
        lua_createtable(L, 0, 4);
        setStr(L, "output", rows[i].outputChannel);
        setStr(L, "xChannel", rows[i].xChannel);
        setInt(L, "sites", rows[i].xSites.size());
        setBool(L, "active", rows[i].active);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

int l_tables8x8(lua_State *L)
{
    const QList<Table8x8Row> &rows = cfg(L)->table8x8Rows;
    lua_createtable(L, rows.size(), 0);
    for (int i = 0; i < rows.size(); ++i) {
        lua_createtable(L, 0, 5);
        setStr(L, "output", rows[i].outputChannel);
        setStr(L, "xChannel", rows[i].xChannel);
        setStr(L, "yChannel", rows[i].yChannel);
        setInt(L, "cells", rows[i].outputs.size());
        setBool(L, "active", rows[i].active);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

// ---------------------------------------------------------------- validation

int l_validate(lua_State *L)
{
    const QList<ValidationIssue> issues = validateConfiguration(*cfg(L));
    lua_createtable(L, issues.size(), 0);
    for (int i = 0; i < issues.size(); ++i) {
        lua_createtable(L, 0, 3);
        switch (issues[i].severity) {
        case ValidationIssue::Error: setStr(L, "severity", QStringLiteral("error")); break;
        case ValidationIssue::Warning: setStr(L, "severity", QStringLiteral("warning")); break;
        case ValidationIssue::Info: setStr(L, "severity", QStringLiteral("info")); break;
        }
        setStr(L, "location", issues[i].location);
        setStr(L, "message", issues[i].message);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

} // namespace

void installConfigBindings(lua_State *L, BindingContext *context)
{
    static const struct {
        const char *name;
        int (*fn)(lua_State *);
    } kFunctions[] = {
        { "title", l_title },
        { "setTitle", l_setTitle },
        { "comments", l_comments },
        { "setComments", l_setComments },
        { "channels", l_channels },
        { "channel", l_channel },
        { "channelCount", l_channelCount },
        { "addChannel", l_addChannel },
        { "setChannel", l_setChannel },
        { "removeChannel", l_removeChannel },
        { "renameChannel", l_renameChannel },
        { "allocatedChannels", l_allocatedChannels },
        { "generatedChannels", l_generatedChannels },
        { "protectedChannels", l_protectedChannels },
        { "bus", l_bus },
        { "setBus", l_setBus },
        { "sections", l_sections },
        { "section", l_section },
        { "addSection", l_addSection },
        { "setSection", l_setSection },
        { "removeSection", l_removeSection },
        { "constants", l_constants },
        { "addConstant", l_addConstant },
        { "removeConstant", l_removeConstant },
        { "mathRows", l_mathRows },
        { "addMath", l_addMath },
        { "removeMath", l_removeMath },
        { "conditions", l_conditions },
        { "counters", l_counters },
        { "timers", l_timers },
        { "integrators", l_integrators },
        { "tables2x16", l_tables2x16 },
        { "tables8x8", l_tables8x8 },
        { "validate", l_validate },
    };

    lua_createtable(L, 0, int(sizeof(kFunctions) / sizeof(kFunctions[0])) + 1);
    for (const auto &f : kFunctions) {
        lua_pushlightuserdata(L, context);
        lua_pushcclosure(L, f.fn, 1);
        lua_setfield(L, -2, f.name);
    }

    // ct.ops: MULADD -> 26 etc., so scripts write ct.ops.MULADD rather than a
    // number that means nothing in six months.
    lua_createtable(L, 0, int(sizeof(kMathOps) / sizeof(kMathOps[0])));
    for (const MathOpEntry &e : kMathOps) {
        lua_pushinteger(L, e.value);
        lua_setfield(L, -2, e.name);
    }
    lua_setfield(L, -2, "ops");

    lua_setglobal(L, "ct");
}

} // namespace ct
