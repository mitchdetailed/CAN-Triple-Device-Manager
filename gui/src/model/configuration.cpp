#include "configuration.h"

#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "../protocol/wire_structs.h" // TABLE_2X16_SITES, MSGPROT_*
#include "comms_template.h"          // kCommsTemplateFileType - the .ct3t marker
#include "config_file.h"             // the format-2 .ct3 container

// The application version, stamped into every saved file as `writtenBy`. Only
// the application target defines it; the test binaries link this translation
// unit without it, and a file a test writes has no build to name. Guarded rather
// than made a link-time symbol because the fallback is genuinely correct — the
// question "which build wrote this" has no answer there.
#ifndef CT_APP_VERSION
#define CT_APP_VERSION "unknown"
#endif

namespace ct {

// ---- CommsProtection: the two conversions the tier has ----

// The tier's numeric value IS the wire level, and these pin that claim to the
// header rather than restating it. If anyone ever renumbers the enum or the
// MSGPROT_* block independently, the build stops here instead of a Send quietly
// downgrading every Hidden message on the device to Read Only.
static_assert(int(CommsProtection::None) << 6 == MSGPROT_NONE, "MSGPROT_NONE");
static_assert(int(CommsProtection::ReadOnly) << 6 == MSGPROT_READONLY, "MSGPROT_READONLY");
static_assert(int(CommsProtection::Hidden) << 6 == MSGPROT_HIDDEN, "MSGPROT_HIDDEN");
static_assert(int(CommsProtection::Protected) << 6 == MSGPROT_PROTECTED, "MSGPROT_PROTECTED");
static_assert(MSGPROT_MASK == 0xC0, "MSGPROT_MASK");

quint8 commsProtectionToWire(CommsProtection protection)
{
    return quint8(quint8(protection) << 6) & MSGPROT_MASK;
}

CommsProtection commsProtectionFromWire(quint8 flags)
{
    // Every one of the four bit patterns is a defined tier, so there is no
    // unknown case to fail closed on — unlike the JSON token, which a text
    // editor can invent. 0x80 (2.2.x "Read-only", which CONCEALED) lands on
    // Hidden and 0xC0 on Protected, which is the whole reason the encoding is
    // this way round; see the table in wire_structs.h.
    return CommsProtection((flags & MSGPROT_MASK) >> 6);
}

// Stable JSON spellings. Chosen as strings rather than the raw level so that a
// hand-edited garbage value is UNRECOGNISED and can clamp — an integer has no
// invalid values to notice.
QString commsProtectionToken(CommsProtection protection)
{
    switch (protection) {
    case CommsProtection::ReadOnly:  return QStringLiteral("readOnly");
    case CommsProtection::Hidden:    return QStringLiteral("hidden");
    case CommsProtection::Protected: return QStringLiteral("protected");
    case CommsProtection::None:      break;
    }
    // None has no spelling: it is written by omitting the key entirely, so an
    // ordinary message carries no trace of the feature. Callers must not emit
    // this empty string.
    return QString();
}

CommsProtection commsProtectionFromToken(const QString &token)
{
    if (token == QLatin1String("readOnly"))
        return CommsProtection::ReadOnly;
    if (token == QLatin1String("hidden"))
        return CommsProtection::Hidden;
    if (token == QLatin1String("protected"))
        return CommsProtection::Protected;
    // Anything else — a typo, a truncated write, a value invented by hand —
    // clamps to the STRONGEST tier. Nothing in a plain .ct3 is signed, so an
    // unrecognised token is a routine possibility rather than a corruption
    // theory, and Protected is the only direction the guess can be wrong in
    // without handing out a protocol. Over-restricting annoys; the other way
    // leaks. Note that an ABSENT key never reaches here: missing means None,
    // which is what every file written before schema 14 says by omission.
    return CommsProtection::Protected;
}

// ---- CommsChannelRow ----

QJsonObject CommsChannelRow::toJson() const
{
    QJsonObject o;
    o["channel"] = channelName;
    o["defaultValue"] = defaultValue;
    o["startBit"] = startBit;
    o["bitLength"] = bitLength;
    o["dbcType"] = dbcType;
    o["dbcFactor"] = dbcFactor;
    o["dbcOffset"] = dbcOffset;
    // Written unconditionally rather than only when false: "clamp" is the
    // default, so a reader that meets the key absent has to assume it, and a
    // file that STATES it can be told apart from one that predates it.
    o["clampToRange"] = clampToRange;
    return o;
}

CommsChannelRow CommsChannelRow::fromJson(const QJsonObject &o)
{
    CommsChannelRow r;
    r.channelName = o["channel"].toString();
    r.defaultValue = o["defaultValue"].toDouble(0);
    r.startBit = o["startBit"].toInt(0);
    r.bitLength = o["bitLength"].toInt(16);
    r.dbcType = o["dbcType"].toInt(int(DbcType::Unsigned));
    r.dbcFactor = o["dbcFactor"].toDouble(1.0);
    r.dbcOffset = o["dbcOffset"].toDouble(0.0);
    // Absent means a pre-schema-19 file, which could only ever clamp.
    r.clampToRange = o["clampToRange"].toBool(true);
    return r;
}

// ---- CompoundIdentifier ----

QJsonObject CompoundIdentifier::toJson() const
{
    QJsonObject o;
    o["offset"] = byteOffset;
    o["id"] = QString::number(id, 16).toUpper();
    o["idMask"] = QString::number(idMask, 16).toUpper();
    o["configured"] = configured;
    QJsonArray arr;
    for (const CommsChannelRow &r : rows)
        arr.append(r.toJson());
    o["channels"] = arr;
    return o;
}

CompoundIdentifier CompoundIdentifier::fromJson(const QJsonObject &o)
{
    CompoundIdentifier c;
    c.byteOffset = o["offset"].toInt(0);
    c.id = o["id"].toString(QStringLiteral("0")).toUInt(nullptr, 16);
    c.idMask = o["idMask"].toString(QStringLiteral("FF")).toUInt(nullptr, 16);
    for (const auto &v : o["channels"].toArray())
        c.rows.append(CommsChannelRow::fromJson(v.toObject()));
    // Files saved before the flag existed marked a slot "in use" by non-default
    // values — infer that so their identifiers still show up.
    c.configured = o["configured"].toBool(c.id != 0 || c.byteOffset != 0 || !c.rows.isEmpty());
    return c;
}

// ---- CommsSection ----

void CommsSection::normalizeCompound()
{
    if (!compound || rows.isEmpty())
        return;
    // Distribute legacy always-present rows into every *used* identifier
    // (configured or already carrying channels); leave them alone if there is no
    // real identifier to hold them (validation then flags them as orphaned).
    bool anyUsed = false;
    for (const CompoundIdentifier &ident : identifiers)
        if (ident.configured || !ident.rows.isEmpty()) {
            anyUsed = true;
            break;
        }
    if (!anyUsed)
        return;
    for (CompoundIdentifier &ident : identifiers)
        if (ident.configured || !ident.rows.isEmpty())
            ident.rows = rows + ident.rows; // common channels first
    rows.clear();
}

QList<CommsChannelRow> CommsSection::allRows() const
{
    if (!compound)
        return rows;
    // Compound sections carry channels only inside identifiers (every channel
    // belongs to a multiplexor value — there is no shared always-present set).
    QList<CommsChannelRow> all;
    for (const CompoundIdentifier &ident : identifiers)
        all += ident.rows;
    return all;
}

QStringList CommsSection::channelNames() const
{
    QStringList names;
    for (const CommsChannelRow &r : allRows())
        names.append(r.channelName);
    return names;
}

bool CommsSection::mentionsChannel(const QString &channelName) const
{
    // The CRC8 publish channel counts as a mention: it is written by this
    // section on the device, and a "mentions" that missed it would let
    // Remove Unused Channels collect a channel the checksum is publishing to.
    if (crcChannel.compare(channelName, Qt::CaseInsensitive) == 0 && isCrc8())
        return true;
    const auto anyOf = [&channelName](const QList<CommsChannelRow> &list) {
        for (const CommsChannelRow &r : list)
            if (r.channelName.compare(channelName, Qt::CaseInsensitive) == 0)
                return true;
        return false;
    };
    // Mirrors allRows() exactly, including the deliberate omission of a compound
    // section's legacy always-present `rows` — normalizeCompound() has folded
    // those into every identifier, and counting them twice here would make this
    // disagree with every listing built from allRows().
    if (!compound)
        return anyOf(rows);
    for (const CompoundIdentifier &ident : identifiers)
        if (anyOf(ident.rows))
            return true;
    return false;
}

QString CommsSection::displayDetail(bool revealed) const
{
    if (device == SectionDevice::Off)
        return QStringLiteral("Off");
    // A concealed section still says WHAT it is — a customer needs to know a
    // message exists, and which way it flows, to make sense of the channels it
    // produces. Everything that describes the protocol is withheld: the CAN ID,
    // the frame length and FD-ness, the byte order, the timing and the routing.
    //
    // Hidden and Protected are worded DIFFERENTLY rather than both saying
    // "(protected)". They are not the same promise: Hidden is a document rule
    // that anyone holding this section's password can lift, while Protected
    // additionally needs a live device to agree. A viewer deciding whether it is
    // worth asking for a password should be told which one they are looking at.
    //
    // ReadOnly deliberately falls through to the full detail below. It conceals
    // nothing — isConcealed() is false for it — and printing "(read-only)" in
    // place of the ID would be the v21 behaviour this release exists to undo.
    if (isConcealed(revealed)) {
        const QString what = device == SectionDevice::TransmitMessage ? QStringLiteral("transmit")
                             : device == SectionDevice::TransmitCrc8  ? QStringLiteral("transmit CRC8")
                             : device == SectionDevice::MessageRelay  ? QStringLiteral("relay")
                                                                      : QStringLiteral("receive");
        return what
               + (protection == CommsProtection::Protected ? QStringLiteral(" (protected)")
                                                           : QStringLiteral(" (hidden)"));
    }
    QStringList parts;
    parts << (device == SectionDevice::TransmitMessage ? QStringLiteral("transmit")
              : device == SectionDevice::TransmitCrc8  ? QStringLiteral("transmit CRC8")
              : device == SectionDevice::MessageRelay  ? QStringLiteral("relay")
                                                       : QStringLiteral("receive"));
    QString id = QStringLiteral("ID 0x") + QString::number(baseAddress, 16).toUpper();
    if (extended)
        id += QStringLiteral(" (extended)");
    parts << id;
    if (!isRelay())
        parts << QStringLiteral("%1%2 bytes")
                     .arg(fd ? QStringLiteral("CAN FD ") : QString())
                     .arg(messageLengthBytes);
    return parts.join(QStringLiteral(", "));
}

QJsonObject CommsSection::toJson() const
{
    QJsonObject o;
    o["name"] = name;
    o["device"] = device == SectionDevice::ReceiveMessage    ? "receive"
                  : device == SectionDevice::TransmitMessage ? "transmit"
                  : device == SectionDevice::MessageRelay    ? "relay"
                  : device == SectionDevice::TransmitCrc8    ? "transmitCrc8"
                                                             : "off";
    o["alignment"] = alignment == SectionAlignment::Normal ? "normal" : "wordSwap";
    o["receiveTimeoutMs"] = receiveTimeoutMs;
    o["defaultValueOnTimeout"] = defaultValueOnTimeout;
    o["diagnosticChannel"] = diagnosticChannel;
    o["extended"] = extended;
    o["fd"] = fd;
    o["baseAddress"] = QString::number(baseAddress, 16).toUpper();
    o["routeEnable"] = routeEnable;
    o["routeBusMask"] = routeBusMask;
    o["cyclic"] = cyclic;
    o["transmitRateHz"] = transmitRateHz;
    o["transmitPeriodMs"] = transmitPeriodMs;
    // Written only when there is one, so a Cyclic message carries no trace of
    // the feature — the same idiom the protection tier uses for None.
    if (!transmitCondition.isEmpty())
        o["transmitCondition"] = transmitCondition;
    o["messageLength"] = messageLengthBytes;
    o["compound"] = compound;
    o["compoundTxMode"] = compoundTxMode == CompoundTxMode::Sequential ? "sequential" : "batch";
    o["relayBitmask"] = QString::number(relayBitmask, 16).toUpper();
    o["relayInvert"] = relayInvert;
    // Schema 16: the Transmit CRC8 recipe, written only when the section IS
    // one — an ordinary message carries no trace of the feature, the same
    // idiom "protection" below uses. The hex fields are written as "0x"
    // strings because that is how a human hand-editing a .ct3 writes a
    // polynomial, and how every CRC reference table prints one.
    if (device == SectionDevice::TransmitCrc8) {
        o["crcChannel"] = crcChannel;
        o["crcByteLocation"] = crcByteLocation;
        o["crcPolynomial"] = QStringLiteral("0x%1").arg(crcPolynomial, 2, 16, QLatin1Char('0')).toUpper().replace(QStringLiteral("0X"), QStringLiteral("0x"));
        o["crcInitValue"] = QStringLiteral("0x%1").arg(crcInitValue, 2, 16, QLatin1Char('0')).toUpper().replace(QStringLiteral("0X"), QStringLiteral("0x"));
        o["crcFinalXor"] = QStringLiteral("0x%1").arg(crcFinalXor, 2, 16, QLatin1Char('0')).toUpper().replace(QStringLiteral("0X"), QStringLiteral("0x"));
        o["crcRefIn"] = crcRefIn;
        o["crcRefOut"] = crcRefOut;
        QJsonArray elems;
        for (const CrcElement &e : crcElements) {
            QJsonObject eo;
            eo["type"] = e.type == CrcElement::Id    ? "id"
                         : e.type == CrcElement::Raw ? "raw"
                                                     : "data";
            eo["value"] = e.value;
            elems.append(eo);
        }
        o["crcElements"] = elems;
    }
    // Schema 14. ONE key, written only when there is a tier to record, so an
    // ordinary message carries no trace of the feature — the idiom the v20
    // readOnlyComms key used, now applied to the whole thing. "protectedComms",
    // "readOnlyComms" and the v7 "hidden" are legacy READ-ONLY keys from here
    // on: still parsed by fromJson for older files, never written again. Writing
    // them alongside "protection" would give a hand-editor two disagreeing
    // sources of truth and a shipped v13 build a reason to think it understood
    // the file.
    if (protection != CommsProtection::None)
        o["protection"] = commsProtectionToken(protection);
    // The section's own password, still written when set: it is what unlocks
    // Read Only and Hidden, and it did not retire with the DEVICE's per-message
    // key. The PBKDF2-derived value, never the password.
    if (messageKey != kNoAccessKey)
        o["messageKey"] = QString::number(messageKey);
    QJsonArray rowArr;
    for (const CommsChannelRow &r : rows)
        rowArr.append(r.toJson());
    o["channels"] = rowArr;
    QJsonArray identArr;
    for (const CompoundIdentifier &c : identifiers)
        identArr.append(c.toJson());
    o["identifiers"] = identArr;
    return o;
}

CommsSection CommsSection::fromJson(const QJsonObject &o, int fileVersion)
{
    CommsSection s;
    s.name = o["name"].toString();
    const QString dev = o["device"].toString(QStringLiteral("receive"));
    s.device = dev == "transmit"     ? SectionDevice::TransmitMessage
               : dev == "relay"      ? SectionDevice::MessageRelay
               : dev == "transmitCrc8" ? SectionDevice::TransmitCrc8
               : dev == "off"        ? SectionDevice::Off
                                     : SectionDevice::ReceiveMessage;
    s.alignment = o["alignment"].toString(QStringLiteral("normal")) == "wordSwap"
                      ? SectionAlignment::WordSwap
                      : SectionAlignment::Normal;
    s.receiveTimeoutMs = o["receiveTimeoutMs"].toInt(2200);
    s.defaultValueOnTimeout = o["defaultValueOnTimeout"].toBool(true);
    s.diagnosticChannel = o["diagnosticChannel"].toString();
    s.extended = o["extended"].toBool(false);
    s.fd = o["fd"].toBool(false);
    s.baseAddress = o["baseAddress"].toString(QStringLiteral("0")).toUInt(nullptr, 16);
    s.routeEnable = o["routeEnable"].toBool(false);
    s.routeBusMask = o["routeBusMask"].toInt(0);
    s.cyclic = o["cyclic"].toBool(true);
    s.transmitRateHz = o["transmitRateHz"].toInt(50);
    s.transmitPeriodMs = o["transmitPeriodMs"].toInt(0);
    s.transmitCondition = o["transmitCondition"].toString();
    s.messageLengthBytes = o["messageLength"].toInt(8);
    s.compound = o["compound"].toBool(false);
    s.compoundTxMode = o["compoundTxMode"].toString(QStringLiteral("batch")) == "sequential"
                           ? CompoundTxMode::Sequential
                           : CompoundTxMode::Batch;
    s.relayBitmask = o["relayBitmask"].toString(QStringLiteral("0")).toUInt(nullptr, 16);
    s.relayInvert = o["relayInvert"].toBool(false);
    // ---- Transmit CRC8 recipe, schema 16 ----
    // Absent keys land on the struct defaults, which is also what an older
    // file (that cannot hold a transmitCrc8 section anyway) reads as. The hex
    // fields accept "0x1D" and bare "1D" alike — toUInt(base 16) takes both —
    // and clamp to a byte, because a hand-edited out-of-range polynomial
    // must not silently become a different one on the wire.
    if (s.device == SectionDevice::TransmitCrc8) {
        const auto hexByte = [&o](const char *key) {
            QString t = o[QLatin1String(key)].toString(QStringLiteral("0"));
            if (t.startsWith(QLatin1String("0x"), Qt::CaseInsensitive))
                t = t.mid(2);
            return int(qBound(0u, t.toUInt(nullptr, 16), 255u));
        };
        s.crcChannel = o["crcChannel"].toString();
        s.crcByteLocation = qBound(0, o["crcByteLocation"].toInt(0), 7);
        s.crcPolynomial = hexByte("crcPolynomial");
        s.crcInitValue = hexByte("crcInitValue");
        s.crcFinalXor = hexByte("crcFinalXor");
        s.crcRefIn = o["crcRefIn"].toBool(false);
        s.crcRefOut = o["crcRefOut"].toBool(false);
        for (const QJsonValue &v : o["crcElements"].toArray()) {
            const QJsonObject eo = v.toObject();
            CrcElement e;
            const QString t = eo["type"].toString(QStringLiteral("data"));
            e.type = t == "id" ? CrcElement::Id : t == "raw" ? CrcElement::Raw
                                                             : CrcElement::Data;
            const int cap = e.type == CrcElement::Id    ? 3
                            : e.type == CrcElement::Data ? 7
                                                         : 255;
            e.value = qBound(0, eo["value"].toInt(0), cap);
            s.crcElements.append(e);
            if (s.crcElements.size() >= 15)
                break; /* CRC8_MAX_ELEMENTS — extra hand-added rows are dropped */
        }
    }
    // ---- protection tier, schema 14, read in strict order ----
    //
    // Schema 14 and later: one key, and an absent one honestly means None. A
    // present but unrecognised one clamps to Protected (commsProtectionFromToken
    // says why). Nothing older is consulted — the legacy keys are not written
    // any more, and re-running the migration over a v14 file is exactly how a
    // ReadOnly section would ratchet into Hidden on every single load.
    if (fileVersion >= 14) {
        s.protection = o.contains(QStringLiteral("protection"))
                           ? commsProtectionFromToken(o[QStringLiteral("protection")].toString())
                           : CommsProtection::None;
    } else if (o.contains(QStringLiteral("protectedComms"))
                   ? o[QStringLiteral("protectedComms")].toBool(false)
                   : o[QStringLiteral("hidden")].toBool(false)) {
        // v19's "protectedComms", and the v7 "hidden" it was renamed from, both
        // become Protected. The v7 key is the sharpest trap in this migration:
        // it is literally named "hidden" and it is the direct ancestor of
        // protectedComms, NOT of the new Hidden tier. Re-pointing it at Hidden
        // because the names match looks like a bug fix in review and silently
        // downgrades every schema-7 file. Do not.
        s.protection = CommsProtection::Protected;
    } else if (o[QStringLiteral("readOnlyComms")].toBool(false)) {
        // v20's "Read-only" becomes HIDDEN, not the new Read Only. v21 made both
        // old flags CONCEAL — isConcealed() ORed them — so a message marked this
        // way in an existing file is one the author has never seen displayed.
        // The new Read Only permits viewing, so mapping it there would print the
        // CAN ID, frame layout, timing and every channel's bit position of every
        // read-only message in every existing file, on first open, with nothing
        // on screen to say it had happened. Over-restricting annoys;
        // under-restricting leaks. Same principle the v8 fallback above states.
        s.protection = CommsProtection::Hidden;
    }
    // Absent from every file written before v20, which reads as "no password on
    // this section" — the honest default, and the one that loses nothing rather
    // than inventing a protection the author never set. Still live: it is the
    // password that unlocks Read Only and Hidden.
    s.messageKey = AccessKey(o[QStringLiteral("messageKey")].toString().toULongLong());
    for (const auto &v : o["channels"].toArray())
        s.rows.append(CommsChannelRow::fromJson(v.toObject()));
    for (const auto &v : o["identifiers"].toArray())
        s.identifiers.append(CompoundIdentifier::fromJson(v.toObject()));
    s.normalizeCompound(); // migrate any legacy always-present rows into identifiers
    return s;
}

// ---- BusConfig ----

QJsonObject BusConfig::toJson() const
{
    QJsonObject o;
    o["enabled"] = enabled;
    o["rateKbps"] = rateKbps;
    o["dataRateKbps"] = dataRateKbps;
    o["termination"] = termination;
    QJsonArray arr;
    for (const CommsSection &s : sections)
        arr.append(s.toJson());
    o["sections"] = arr;
    return o;
}

BusConfig BusConfig::fromJson(const QJsonObject &o, int fileVersion)
{
    BusConfig b;
    b.enabled = o["enabled"].toBool(false);
    b.rateKbps = o["rateKbps"].toInt(1000);
    b.dataRateKbps = o["dataRateKbps"].toInt(0);
    b.termination = o["termination"].toBool(false);
    for (const auto &v : o["sections"].toArray())
        b.sections.append(CommsSection::fromJson(v.toObject(), fileVersion));
    return b;
}

// ---- MathRow / ConditionRow ----

QJsonObject MathRow::toJson() const
{
    QJsonObject o;
    o["op"] = op;
    o["aIsChannel"] = aIsChannel;
    o["aChannel"] = aChannel;
    o["aConst"] = aConst;
    o["bIsChannel"] = bIsChannel;
    o["bChannel"] = bChannel;
    o["bConst"] = bConst;
    o["cIsChannel"] = cIsChannel;
    o["cChannel"] = cChannel;
    o["cConst"] = cConst;
    o["dest"] = destChannel;
    o["active"] = active;
    return o;
}

MathRow MathRow::fromJson(const QJsonObject &o)
{
    MathRow m;
    m.op = o["op"].toInt(0);
    m.aIsChannel = o["aIsChannel"].toBool(true);
    m.aChannel = o["aChannel"].toString();
    m.aConst = o["aConst"].toDouble(0);
    m.bIsChannel = o["bIsChannel"].toBool(false);
    m.bChannel = o["bChannel"].toString();
    m.bConst = o["bConst"].toDouble(0);
    // c* absent in schema <= 9 files: default to an unused const-0 operand.
    m.cIsChannel = o["cIsChannel"].toBool(false);
    m.cChannel = o["cChannel"].toString();
    m.cConst = o["cConst"].toDouble(0);
    m.destChannel = o["dest"].toString();
    m.active = o["active"].toBool(true);
    return m;
}

bool CounterRow::readsUpDown() const
{
    return mode == int(ct::COUNTER_MODE_UPDOWN);
}

bool CounterRow::readsFollow() const
{
    return mode == int(ct::COUNTER_MODE_FOLLOW);
}

bool ConditionTermRow::isMessageOp() const
{
    return condOpIsMessage(quint8(op));
}

QJsonObject ConditionTermRow::toJson() const
{
    QJsonObject o;
    o["aChannel"] = aChannel;
    o["op"] = op;
    o["bIsChannel"] = bIsChannel;
    o["bChannel"] = bChannel;
    o["bConst"] = bConst;
    // Written only for a message operator, so a comparison carries no trace of
    // the feature — the idiom the protection tier and the transmit condition
    // both use.
    if (isMessageOp()) {
        o["aMessageBus"] = aMessageBus;
        o["aMessage"] = aMessage;
    }
    return o;
}

ConditionTermRow ConditionTermRow::fromJson(const QJsonObject &o)
{
    ConditionTermRow t;
    t.aChannel = o["aChannel"].toString();
    t.op = o["op"].toInt(0);
    t.bIsChannel = o["bIsChannel"].toBool(false);
    t.bChannel = o["bChannel"].toString();
    t.bConst = o["bConst"].toDouble(0);
    t.aMessageBus = o["aMessageBus"].toInt(0);
    t.aMessage = o["aMessage"].toString();
    return t;
}

bool invertConditionExpr(const QList<ConditionTermRow> &terms, const QList<int> &joiners,
                         QList<ConditionTermRow> *outTerms, QList<int> *outJoiners)
{
    // De Morgan, applied at every gap of a strictly left-to-right fold.
    //
    //   NOT((X J t))  ==  (NOT X) J' (NOT t)
    //
    // where J' is the flipped joiner. Applying that from the outside in leaves
    // every term negated and every joiner flipped, and nothing else changes —
    // the bracketing is the same shape, so the editor still displays it the way
    // the device evaluates it.
    for (const ConditionTermRow &t : terms)
        if (t.isMessageOp())
            return false; // "a frame did not arrive" is not a thing to latch on
    QList<ConditionTermRow> inv;
    inv.reserve(terms.size());
    for (ConditionTermRow t : terms) {
        t.op = int(condOpNegate(quint8(t.op)));
        inv.append(t);
    }
    QList<int> flipped;
    flipped.reserve(joiners.size());
    for (int j : joiners)
        flipped.append(j == int(COND_JOIN_OR) ? int(COND_JOIN_AND) : int(COND_JOIN_OR));
    *outTerms = inv;
    *outJoiners = flipped;
    return true;
}

QString joinConditionTerms(const QStringList &termTexts, const QList<int> &joiners,
                           const QString &andWord, const QString &orWord)
{
    if (termTexts.isEmpty())
        return QString();
    QString expr = termTexts.first();
    for (int i = 1; i < termTexts.size(); ++i) {
        const bool isOr = joiners.value(i - 1, int(COND_JOIN_AND)) == int(COND_JOIN_OR);
        // Bracket the accumulated left side from the second join onward: with
        // one join "A and B" is unambiguous, but with two the grouping is the
        // whole point, so it is spelled out.
        if (i > 1)
            expr = QLatin1Char('(') + expr + QLatin1Char(')');
        expr += QLatin1Char(' ') + (isOr ? orWord : andWord) + QLatin1Char(' ');
        expr += termTexts.at(i);
    }
    return expr;
}

QStringList ConditionRow::inputChannels() const
{
    QStringList names;
    // BOTH expressions. A Reset comparison reads a channel exactly as a Set one
    // does, and a rename walk that missed it would leave the Reset pointing at
    // a name that no longer exists — the latch would then never clear.
    for (const QList<ConditionTermRow> *side : {&setTerms, &resetTerms}) {
        for (const ConditionTermRow &t : *side) {
            if (t.isMessageOp())
                continue; // its operand is a message, not a channel
            if (!t.aChannel.isEmpty())
                names << t.aChannel;
            if (t.bIsChannel && !t.bChannel.isEmpty())
                names << t.bChannel;
        }
    }
    return names;
}

QList<QPair<int, QString>> ConditionRow::messageRefs() const
{
    QList<QPair<int, QString>> refs;
    for (const QList<ConditionTermRow> *side : {&setTerms, &resetTerms})
        for (const ConditionTermRow &t : *side)
            if (t.isMessageOp() && !t.aMessage.isEmpty())
                refs.append({t.aMessageBus, t.aMessage});
    return refs;
}

namespace {
// One expression in and out of JSON, plus the invariant every consumer relies
// on: at most COND_MAX_TERMS terms, at least one, and exactly one joiner per gap.
QJsonObject exprToJson(const QList<ConditionTermRow> &terms, const QList<int> &joiners)
{
    QJsonObject o;
    QJsonArray termArray;
    for (const ConditionTermRow &t : terms)
        termArray.append(t.toJson());
    o["terms"] = termArray;
    QJsonArray joinArray;
    for (int j : joiners)
        joinArray.append(j);
    o["joiners"] = joinArray;
    return o;
}

void normaliseExpr(QList<ConditionTermRow> *terms, QList<int> *joiners)
{
    // A file written by a future build may carry more terms than this one
    // supports; keep the leading ones rather than failing the load.
    if (terms->size() > COND_MAX_TERMS)
        *terms = terms->mid(0, COND_MAX_TERMS);
    if (terms->isEmpty())
        terms->append(ConditionTermRow{});
    while (joiners->size() < terms->size() - 1)
        joiners->append(int(COND_JOIN_AND));
    *joiners = joiners->mid(0, terms->size() - 1);
}

void exprFromJson(const QJsonObject &o, QList<ConditionTermRow> *terms, QList<int> *joiners)
{
    terms->clear();
    joiners->clear();
    for (const auto &v : o["terms"].toArray())
        terms->append(ConditionTermRow::fromJson(v.toObject()));
    for (const auto &v : o["joiners"].toArray())
        joiners->append(v.toInt(int(COND_JOIN_AND)));
    normaliseExpr(terms, joiners);
}
} // namespace

QJsonObject ConditionRow::toJson() const
{
    QJsonObject o;
    o["mode"] = mode == ConditionMode::Momentary ? "momentary" : "setreset";
    o["set"] = exprToJson(setTerms, setJoiners);
    o["reset"] = exprToJson(resetTerms, resetJoiners);
    o["latchHz"] = latchHz;
    // Written only when in use, so a condition that does not qualify carries no
    // trace of the feature — the idiom the protection tier and the transmit
    // condition both follow.
    if (qualifySetMs > 0) {
        o["qualifySetMs"] = qualifySetMs;
        o["qualifySetTerms"] = qualifySetTerms;
    }
    if (qualifyResetMs > 0) {
        o["qualifyResetMs"] = qualifyResetMs;
        o["qualifyResetTerms"] = qualifyResetTerms;
    }
    o["outputChannel"] = outputChannel;
    o["active"] = active;
    return o;
}

ConditionRow ConditionRow::fromJson(const QJsonObject &o)
{
    ConditionRow c;
    if (o.contains(QStringLiteral("mode"))) {
        c.mode = o["mode"].toString() == QLatin1String("momentary") ? ConditionMode::Momentary
                                                                   : ConditionMode::SetReset;
        exprFromJson(o["set"].toObject(), &c.setTerms, &c.setJoiners);
        exprFromJson(o["reset"].toObject(), &c.resetTerms, &c.resetJoiners);
        c.latchHz = qBound(1, o["latchHz"].toInt(10), int(COND_LATCH_MAX_HZ));
        // Absent in every file written before the qualifier existed, and 0 is
        // exactly what those files meant.
        // "qualifyMs" is the one-sided spelling this briefly had before Reset
        // got its own; it was never released, and reading it as the Set side is
        // what any file written in that window meant.
        c.qualifySetMs = qMax(0, o["qualifySetMs"].toInt(o["qualifyMs"].toInt(0)));
        c.qualifySetTerms = qBound(
            0, o["qualifySetTerms"].toInt(o["qualifyTerms"].toInt(0)), (1 << COND_MAX_TERMS) - 1);
        c.qualifyResetMs = qMax(0, o["qualifyResetMs"].toInt(0));
        c.qualifyResetTerms =
            qBound(0, o["qualifyResetTerms"].toInt(0), (1 << COND_MAX_TERMS) - 1);
    } else {
        // THE MIGRATION. Before the modes a condition was a plain level: its
        // output followed one expression, with no memory. That is exactly a
        // Set/Reset whose Reset is the logical inverse of its Set — the latch
        // sets whenever the expression holds and resets whenever it does not,
        // which is a level with extra steps — so the file's single expression
        // becomes the Set and its inverse becomes the Reset.
        //
        // Keyed on the ABSENCE of "mode" rather than on the file version,
        // because that is self-describing and idempotent: a file that has been
        // through this once has a mode and never sees it again.
        c.mode = ConditionMode::SetReset;
        c.setTerms.clear();
        c.setJoiners.clear();
        if (o.contains(QStringLiteral("terms"))) {
            for (const auto &v : o["terms"].toArray())
                c.setTerms.append(ConditionTermRow::fromJson(v.toObject()));
            for (const auto &v : o["joiners"].toArray())
                c.setJoiners.append(v.toInt(int(COND_JOIN_AND)));
        } else {
            // Pre-v14 files hold ONE comparison inline, on the condition object
            // itself; it becomes this condition's first (and only) term.
            c.setTerms.append(ConditionTermRow::fromJson(o));
        }
        normaliseExpr(&c.setTerms, &c.setJoiners);
        if (!invertConditionExpr(c.setTerms, c.setJoiners, &c.resetTerms, &c.resetJoiners)) {
            // Unreachable for a real pre-modes file — the message operators did
            // not exist when those were written, and they are the only thing
            // that refuses. Falling back to the Set expression unchanged would
            // build a latch that resets whenever it sets, so leave the Reset
            // empty instead: "never resets" is at least honest, and validation
            // reports it.
            c.resetTerms.clear();
            c.resetJoiners.clear();
        }
    }
    normaliseExpr(&c.setTerms, &c.setJoiners);
    normaliseExpr(&c.resetTerms, &c.resetJoiners);
    // Pre-boolean files stored the output under "targetChannel" (SET_SIGNAL_VAL
    // action); fall back to it so older conditions keep their output channel.
    c.outputChannel = o["outputChannel"].toString(o["targetChannel"].toString());
    c.active = o["active"].toBool(true);
    return c;
}

QJsonObject CounterSource::toJson() const
{
    QJsonObject o;
    o["kind"] = kind;
    o["bus"] = messageBus;
    o["message"] = message;
    return o;
}

CounterSource CounterSource::fromJson(const QJsonObject &o)
{
    CounterSource s;
    s.kind = qBound(0, o["kind"].toInt(0), int(COUNTER_SRC_MSG_TX));
    s.messageBus = qBound(0, o["bus"].toInt(0), 3);
    s.message = o["message"].toString();
    return s;
}

QJsonObject CounterRow::toJson() const
{
    QJsonObject o;
    o["output"] = outputChannel;
    o["mode"] = mode;
    o["up"] = upChannel;
    o["down"] = downChannel;
    o["follow"] = followChannel;
    o["reset"] = resetChannel;
    o["enable"] = enableChannel;
    // Written only for an input that is NOT a plain channel, so a counter using
    // channels throughout carries no trace of the feature — the idiom every
    // optional key here follows.
    if (upSource.isMessage())
        o["upSource"] = upSource.toJson();
    if (downSource.isMessage())
        o["downSource"] = downSource.toJson();
    if (resetSource.isMessage())
        o["resetSource"] = resetSource.toJson();
    if (enableSource.isMessage())
        o["enableSource"] = enableSource.toJson();
    o["min"] = minValue;
    o["max"] = maxValue;
    o["resetValue"] = resetValue;
    o["step"] = step;
    o["rollAtLimits"] = rollAtLimits;
    o["preserveValue"] = preserveValue;
    o["rateHz"] = rateHz;
    o["rateCountDown"] = rateCountDown;
    o["active"] = active;
    return o;
}

CounterRow CounterRow::fromJson(const QJsonObject &o)
{
    CounterRow c;
    c.outputChannel = o["output"].toString();
    c.mode = o["mode"].toInt(0);
    c.upChannel = o["up"].toString();
    c.downChannel = o["down"].toString();
    c.followChannel = o["follow"].toString();
    c.resetChannel = o["reset"].toString();
    c.enableChannel = o["enable"].toString();
    c.upSource = CounterSource::fromJson(o["upSource"].toObject());
    c.downSource = CounterSource::fromJson(o["downSource"].toObject());
    c.resetSource = CounterSource::fromJson(o["resetSource"].toObject());
    c.enableSource = CounterSource::fromJson(o["enableSource"].toObject());
    c.minValue = o["min"].toDouble(0);
    c.maxValue = o["max"].toDouble(1000000);
    c.resetValue = o["resetValue"].toDouble(0);
    c.step = o["step"].toDouble(1);
    c.rollAtLimits = o["rollAtLimits"].toBool(false);
    c.preserveValue = o["preserveValue"].toBool(false);
    // Absent in every pre-rate-mode file, and the defaults are right for one:
    // it can only be an edge or follow counter, which ignores both fields.
    c.rateHz = o["rateHz"].toInt(1);
    c.rateCountDown = o["rateCountDown"].toBool(false);
    c.active = o["active"].toBool(true);
    return c;
}

QJsonObject IntegratorRow::toJson() const
{
    QJsonObject o;
    o["output"] = outputChannel;
    o["inputIsChannel"] = inputIsChannel;
    o["inputChannel"] = inputChannel;
    o["inputValue"] = inputValue;
    o["rateHz"] = rateHz;
    o["countDown"] = countDown;
    o["startValue"] = startValue;
    o["enable"] = enableChannel;
    o["reset"] = resetChannel;
    o["resetValue"] = resetValue;
    o["min"] = minValue;
    o["max"] = maxValue;
    o["preserveValue"] = preserveValue;
    o["active"] = active;
    return o;
}

IntegratorRow IntegratorRow::fromJson(const QJsonObject &o)
{
    IntegratorRow g;
    g.outputChannel = o["output"].toString();
    g.inputIsChannel = o["inputIsChannel"].toBool(true);
    g.inputChannel = o["inputChannel"].toString();
    g.inputValue = o["inputValue"].toDouble(0);
    g.rateHz = o["rateHz"].toInt(10);
    g.countDown = o["countDown"].toBool(false);
    g.startValue = o["startValue"].toDouble(0);
    g.enableChannel = o["enable"].toString();
    g.resetChannel = o["reset"].toString();
    g.resetValue = o["resetValue"].toDouble(0);
    g.minValue = o["min"].toDouble(0);
    g.maxValue = o["max"].toDouble(1000000);
    g.preserveValue = o["preserveValue"].toBool(false);
    g.active = o["active"].toBool(true);
    return g;
}

QStringList TimerRow::inputChannels() const
{
    QStringList out;
    for (const ConditionTermRow *t : {&startTerm, &stopTerm}) {
        if (t->isMessageOp())
            continue;
        if (!t->aChannel.isEmpty())
            out << t->aChannel;
        if (t->bIsChannel && !t->bChannel.isEmpty())
            out << t->bChannel;
    }
    return out;
}

QList<QPair<int, QString>> TimerRow::messageRefs() const
{
    QList<QPair<int, QString>> out;
    for (const ConditionTermRow *t : {&startTerm, &stopTerm})
        if (t->isMessageOp() && t->aMessageBus >= 1 && !t->aMessage.isEmpty())
            out.append({t->aMessageBus, t->aMessage});
    return out;
}

QJsonObject TimerRow::toJson() const
{
    QJsonObject o;
    o["output"] = outputChannel;
    // Schema 20. The old "start"/"stop" strings are NOT written alongside these
    // — two spellings of one trigger is how they come to disagree, and a term
    // says everything the string did and more.
    o["startTerm"] = startTerm.toJson();
    o["stopTerm"] = stopTerm.toJson();
    o["countDown"] = countDown;
    o["rollover"] = rollover;
    o["limit"] = limitValue;
    o["setOnStart"] = setOnStart;
    o["startValue"] = startValue;
    o["setOnStop"] = setOnStop;
    o["stopValue"] = stopValue;
    o["active"] = active;
    return o;
}

TimerRow TimerRow::fromJson(const QJsonObject &o)
{
    TimerRow t;
    t.outputChannel = o["output"].toString();
    // Schema 20 terms when they are there; otherwise the pre-20 channel names,
    // migrated into the comparison the device was already making. "Start when
    // this channel is non-zero" IS (channel NEQ 0), so the migration is exact
    // rather than approximate, and an empty name stays an unused half.
    if (o.contains(QStringLiteral("startTerm"))) {
        t.startTerm = ConditionTermRow::fromJson(o["startTerm"].toObject());
    } else if (!o["start"].toString().isEmpty()) {
        t.startTerm.aChannel = o["start"].toString();
        t.startTerm.op = COND_OP_NEQ;
        t.startTerm.bIsChannel = false;
        t.startTerm.bConst = 0;
    }
    if (o.contains(QStringLiteral("stopTerm"))) {
        t.stopTerm = ConditionTermRow::fromJson(o["stopTerm"].toObject());
    } else if (!o["stop"].toString().isEmpty()) {
        t.stopTerm.aChannel = o["stop"].toString();
        t.stopTerm.op = COND_OP_NEQ;
        t.stopTerm.bIsChannel = false;
        t.stopTerm.bConst = 0;
    }
    t.countDown = o["countDown"].toBool(false);
    t.rollover = o["rollover"].toBool(false);
    t.limitValue = o["limit"].toDouble(0);
    t.setOnStart = o["setOnStart"].toBool(false);
    t.startValue = o["startValue"].toDouble(0);
    t.setOnStop = o["setOnStop"].toBool(false);
    t.stopValue = o["stopValue"].toDouble(0);
    t.active = o["active"].toBool(true);
    return t;
}

QJsonObject ConstantRow::toJson() const
{
    QJsonObject o;
    o["name"] = name;
    o["dataType"] = dataType;
    o["decimals"] = decimalPlaces;
    o["value"] = value;
    o["active"] = active;
    return o;
}

ConstantRow ConstantRow::fromJson(const QJsonObject &o)
{
    ConstantRow c;
    c.name = o["name"].toString();
    c.dataType = o["dataType"].toString();
    c.decimalPlaces = o["decimals"].toInt(0);
    c.value = o["value"].toDouble(0);
    c.active = o["active"].toBool(true);
    return c;
}

// ---- Table2x16Row / Table8x8Row ----

static QJsonArray doublesToJson(const QList<double> &v)
{
    QJsonArray a;
    for (double x : v)
        a.append(x);
    return a;
}

// Read a variable-length double array (only the populated sites of a table).
static QList<double> doublesFromJson(const QJsonArray &a)
{
    QList<double> v;
    v.reserve(a.size());
    for (const auto &e : a)
        v.append(e.toDouble(0.0));
    return v;
}

QJsonObject Table2x16Row::toJson() const
{
    QJsonObject o;
    o["output"] = outputChannel;
    o["dataType"] = dataType;
    o["decimals"] = decimalPlaces;
    o["xChannel"] = xChannel;
    o["xInterp"] = xInterp;
    o["xSites"] = doublesToJson(xSites);
    o["outputs"] = doublesToJson(outputs);
    o["active"] = active;
    return o;
}

Table2x16Row Table2x16Row::fromJson(const QJsonObject &o)
{
    Table2x16Row t;
    t.outputChannel = o["output"].toString();
    t.dataType = o["dataType"].toString(QStringLiteral("float"));
    t.decimalPlaces = o["decimals"].toInt(0);
    t.xChannel = o["xChannel"].toString();
    t.xInterp = o["xInterp"].toBool(true);
    t.xSites = doublesFromJson(o["xSites"].toArray());
    t.outputs = doublesFromJson(o["outputs"].toArray());
    // Pre-v13 files hold at most 8 sites; they load unchanged into the wider
    // row. Anything longer than the current width is a file from a future
    // build — keep the leading sites rather than failing the load.
    if (t.xSites.size() > TABLE_2X16_SITES)
        t.xSites = t.xSites.mid(0, TABLE_2X16_SITES);
    if (t.outputs.size() > TABLE_2X16_SITES)
        t.outputs = t.outputs.mid(0, TABLE_2X16_SITES);
    t.active = o["active"].toBool(true);
    return t;
}

QJsonObject Table8x8Row::toJson() const
{
    QJsonObject o;
    o["output"] = outputChannel;
    o["dataType"] = dataType;
    o["decimals"] = decimalPlaces;
    o["xChannel"] = xChannel;
    o["yChannel"] = yChannel;
    o["xInterp"] = xInterp;
    o["yInterp"] = yInterp;
    o["xSites"] = doublesToJson(xSites);
    o["ySites"] = doublesToJson(ySites);
    o["outputs"] = doublesToJson(outputs);
    o["active"] = active;
    return o;
}

Table8x8Row Table8x8Row::fromJson(const QJsonObject &o)
{
    Table8x8Row t;
    t.outputChannel = o["output"].toString();
    t.dataType = o["dataType"].toString(QStringLiteral("float"));
    t.decimalPlaces = o["decimals"].toInt(0);
    t.xChannel = o["xChannel"].toString();
    t.yChannel = o["yChannel"].toString();
    t.xInterp = o["xInterp"].toBool(true);
    t.yInterp = o["yInterp"].toBool(true);
    t.xSites = doublesFromJson(o["xSites"].toArray());
    t.ySites = doublesFromJson(o["ySites"].toArray());
    t.outputs = doublesFromJson(o["outputs"].toArray());
    t.active = o["active"].toBool(true);
    // Sites are deliberately NOT clipped to TABLE_8X8_SITES the way a 2x16 row's
    // are. `outputs` is strided by xSites.size(), so dropping a trailing X site
    // without re-striding the grid would silently shift every cell after the
    // first row — a corruption where the 1-D case merely loses a breakpoint. An
    // over-wide row (only a hand-edited or future-version file can produce one)
    // is instead clamped where the width is actually applied, in the mapper,
    // which reads the top-left 8x8 of it and leaves the file untouched.
    return t;
}

// The schema-11 "tables4x4" form. Field for field the same object: the 4x4 row
// carried variable-length xSites/ySites and a row-major `outputs` strided by
// xSites.size(), never a fixed 4, so a saved 4x4 IS an 8x8 whose sites happen to
// stop at 4. Counts carry over untouched, sites and outputs copy verbatim, and
// the unused sites simply do not exist in the model — the mapper materialises
// the top-left placement when it fills the fixed-width wire records.
//
// It exists as its own function rather than as a call to fromJson at the old key
// because the equivalence above is a fact about the two formats that is worth
// stating once, in the place a reader goes looking for the migration.
Table8x8Row Table8x8Row::fromTable4x4Json(const QJsonObject &o)
{
    return fromJson(o);
}

// ---- Configuration ----

// Fresh-document bus settings: every bus starts Off at 1M classic until the
// user deliberately turns it on in Connections > Communications.
static void applyDefaultBusSettings(BusConfig bus[3])
{
    for (int i = 0; i < 3; ++i) {
        bus[i].enabled = false;
        bus[i].rateKbps = 1000;
        bus[i].dataRateKbps = 0;
        bus[i].termination = false;
    }
}

Configuration::Configuration(QObject *parent)
    : QObject(parent)
{
    applyDefaultBusSettings(bus);
}

void Configuration::setDirty(bool dirty)
{
    if (m_dirty == dirty)
        return;
    m_dirty = dirty;
    emit dirtyChanged(dirty);
}

QString Configuration::displayName() const
{
    if (m_filePath.isEmpty())
        return QStringLiteral("Untitled");
    return QFileInfo(m_filePath).completeBaseName();
}

void Configuration::setConfigTitle(const QString &title)
{
    const QString trimmed = title.trimmed();
    if (m_configTitle == trimmed)
        return;
    m_configTitle = trimmed;
    setDirty();
}

QString Configuration::effectiveTitle() const
{
    if (!m_configTitle.isEmpty())
        return m_configTitle;
    return m_filePath.isEmpty() ? QString() : QFileInfo(m_filePath).completeBaseName();
}

// The single writer for the script pair. Everything that can change either half
// — the Script Editor, a file load, a Get, copyContentTo, clearContent — arrives
// here, which is the whole point: the precedence rule is applied once, in one
// expression, instead of being restated (and eventually mis-stated) at each
// call site.
//
// PRECEDENCE, and why it is enforced by CONSTRUCTION rather than by checking:
//
//   1. a source present  -> that is the script; a retained image is stale and
//                           is DROPPED here, not carried alongside;
//   2. no source, image   -> the image is the script, sent back verbatim;
//   3. neither            -> no script, which still means "remove the script
//                           from the device". Unchanged behaviour.
//
// Rule 1 is the dangerous one. A document holding both would have to pick a
// winner every time anything asked it — the Send path, Verify's comparison, the
// editor, the report — and one site picking differently from another sends a
// script nobody has looked at to a unit in a vehicle. Dropping the image the
// instant a source appears means the question can never be asked, because the
// state that raises it cannot be built.
//
// hasScriptSource()'s definition of "present" is used deliberately: blank or
// whitespace-only counts as ABSENT, exactly as ScriptCompiler::attachTo reads
// it. Any other predicate here would let "   " drop a retained image and then
// compile to nothing, which is the silent script-stripping this feature exists
// to remove.
void Configuration::setScript(const QString &source, const QByteArray &retainedBytecode)
{
    m_scriptSource = source;
    m_scriptBytecode = hasScriptSource() ? QByteArray() : retainedBytecode;
}

void Configuration::clearContent()
{
    for (auto &b : bus)
        b = BusConfig{};
    applyDefaultBusSettings(bus);
    mathRows.clear();
    conditionRows.clear();
    counterRows.clear();
    timerRows.clear();
    integratorRows.clear();
    constantRows.clear();
    table2x16Rows.clear();
    table8x8Rows.clear();
    comments.clear();
    // BOTH halves of the script, through the one door. A Get calls this before
    // it maps, so the image it is about to retain starts from an empty pair
    // rather than landing beside the outgoing document's source — which the
    // invariant would then have to resolve, silently, in favour of a source the
    // device knows nothing about.
    setScript(QString(), QByteArray());
    m_configTitle.clear();
    m_catalog.setUserChannels({});
    m_filePath.clear();
    setDirty(false);
    emit documentReset();
}

void Configuration::clear()
{
    // A new document is a new document: no access passwords, no fleet identity,
    // an upload policy back at its strict default, and no key material or
    // session grant left over from the one that was open before. m_secureFile
    // goes with them — an untitled document has no format yet, and inheriting
    // "this is a .ct3s" from the previous file would make the first Save write a
    // container nobody asked for.
    //
    // This half is what clearContent() deliberately does NOT do, and the reason
    // the two are separate: a Get rebuilds the CONTENT from the device and must
    // leave everything below alone. Before the split, mapFromDevice called this
    // function and a Get therefore wiped the open document's access verifiers —
    // after which hasCommsPassword() was false, commsRevealed() was true, and
    // every protection box in the document was untickable with no challenge at
    // all. A round trip through the hardware was the shortest way past the whole
    // feature.
    //
    // Wiped BEFORE the content, so the single documentReset() clearContent()
    // emits is seen by observers with the document already fully empty rather
    // than half-cleared.
    m_accessVerifiers.clear();
    m_fleetIdentity = FleetIdentity{};
    m_uploadPolicy = UploadPolicy{};
    m_commsKey = kNoAccessKey;
    m_lockedDeviceUid.clear();
    m_deviceLockKey = kNoAccessKey;
    m_commsRevealed = false;
    m_sectionGrants.clear();
    m_secureFile = false;
    m_secureOptions = SecureSaveOptions{};
    clearContent();
}

// ---- Configuration: access passwords and fleet identity ----

bool Configuration::hasCommsPassword() const
{
    return m_accessVerifiers.isSet(AccessFunction::EditProtectedComms);
}

bool Configuration::revealProtectedComms(const QString &password)
{
    // An unset verifier verifies nothing, so this is also the honest answer for
    // a document with no comms password: there was nothing to reveal, and
    // commsRevealed() was already true without anyone typing anything.
    if (!m_accessVerifiers.verifier(AccessFunction::EditProtectedComms).verify(password))
        return false;
    m_commsRevealed = true;
    // Derive the device key while we still can. The password is held for the
    // length of this call and never stored, so this is the only moment the
    // 4-byte key the hardware compares is reachable — the file's verifier
    // deliberately cannot produce it (see access_keys.h). Miss it here and the
    // session can see the protected messages but cannot open a device that
    // carries the same password.
    m_commsKey = deriveAccessKey(password);
    return true;
}

void Configuration::concealProtectedComms()
{
    m_commsRevealed = false;
    m_commsKey = kNoAccessKey;
    // Every per-section grant goes with it. Conceal means "forget what this
    // session was told", and a section unlocked by its own password is exactly
    // that — leaving those standing would make Conceal a half-measure that
    // re-hides some messages and not others, with nothing on screen to say
    // which.
    m_sectionGrants.clear();
}

// A grant names a SECTION — this bus, this name — and records the messageKey it
// was proved against. Not a bare name: the name is a value the person being kept
// out gets to choose, and a name-only grant set let a section created on another
// bus, with a password of the attacker's own, unlock the real one. Not a bus and
// name either: recording the key is what makes the grant die when the secret it
// stood for is replaced. See the contract in the header.
//
// The name is lower-cased on the way in and on the way out, so this answers
// case-insensitively like renameChannelReferences() and applyBusSections() do.
// An unnamed section is not grantable: the name is the handle, and a section
// with no name has nothing for a grant to be about.
//
// One grant per (bus, name), replaced rather than appended, so re-proving a
// section whose password has changed leaves ONE record — the current one. Two
// records for one section would mean the stale key kept answering.
void Configuration::grantSectionAccess(int busIndex, const CommsSection &section)
{
    if (busIndex < 0 || busIndex > 2 || section.name.isEmpty())
        return;
    // A KEYLESS section records nothing. kNoAccessKey is not a secret — every
    // keyless section in the document carries it — so a grant taken against it
    // would leave the lower-cased NAME deciding on its own, which is the hole
    // the key was added to close, reopened for exactly the sections a Get
    // produces. Unlocking a keyless "Engine Data" on CAN 2 revealed a keyless
    // "Engine Data" on CAN 1 and made it lowerable, and neither had a password
    // anyone had proved.
    //
    // The existing grant goes with it rather than being left standing: this call
    // means "here is what was proved about this section NOW", and what was
    // proved is nothing, so a record saying otherwise would be answering with a
    // secret the section no longer holds.
    if (section.messageKey == kNoAccessKey) {
        revokeSectionAccess(busIndex, section.name);
        return;
    }
    const QString lower = section.name.toLower();
    for (SectionGrant &g : m_sectionGrants) {
        if (g.busIndex == busIndex && g.lowerName == lower) {
            g.provedKey = section.messageKey;
            return;
        }
    }
    m_sectionGrants.append(SectionGrant{busIndex, lower, section.messageKey});
}

// The exact, bus-checked question, expressed as the general one so the two
// cannot drift: a caller that knows its bus is asking for the same match plus
// one more field, not for a different rule.
bool Configuration::sectionAccessGranted(int busIndex, const CommsSection &section) const
{
    // A negative bus is not "any bus" here. This name promises a bus-checked
    // answer, so a caller that does not know which bus it means is asking
    // sectionGrantProves()'s question and must be told no rather than yes.
    if (busIndex < 0)
        return false;
    return sectionGrantProves(section, busIndex);
}

// The name-and-key match. `busIndex` is checked whenever the caller supplied one
// and skipped at -1, which is the display sites that hold a section value and no
// bus; the declaration says which callers are which and why.
//
// A KEYLESS SECTION MATCHES NOTHING. That is the whole of defect D, and the
// reason it took so long to see is that the loop below reads as if the key were
// doing the work: kNoAccessKey is what EVERY keyless section carries, so where
// it is the key the comparison is satisfied by every keyless grant in the
// session and the name — a value the person being kept out chooses — is left
// deciding on its own. The probe: two keyless Protected sections of the same
// name on CAN 1 and CAN 2, a document password set and unproved, unlock the CAN
// 2 one, and the CAN 1 one turned revealed and lowerable.
//
// grantSectionAccess() already refuses to record one, so this is the belt: a
// grant that reached the list another way — a stale record from before the key
// was cleared, a future writer — still answers for nothing.
bool Configuration::sectionGrantProves(const CommsSection &section, int busIndex) const
{
    if (section.name.isEmpty() || section.messageKey == kNoAccessKey)
        return false;
    const QString lower = section.name.toLower();
    for (const SectionGrant &g : m_sectionGrants)
        if ((busIndex < 0 || g.busIndex == busIndex) && g.lowerName == lower
            && g.provedKey == section.messageKey)
            return true;
    return false;
}

void Configuration::revokeSectionAccess(int busIndex, const QString &sectionName)
{
    const QString lower = sectionName.toLower();
    // By bus and name only — NOT by key. Rule 3 revokes a name the section may
    // no longer carry the key for; matching the key too would leave the grant
    // this call exists to drop.
    m_sectionGrants.removeIf([&](const SectionGrant &g) {
        return g.busIndex == busIndex && g.lowerName == lower;
    });
}

// The proof requirements per tier, stated once. Everything that has to RUN a
// challenge — the section editor's tier ladder, Communications Setup's unlock —
// reads this rather than switching on the tier itself, so a fourth tier or a
// changed requirement lands in one place instead of three that already disagree
// about Read Only.
//
// Protected demands BOTH halves as of 2.3.1. It used to demand the device round
// trip alone, which left it the only marked tier with no per-section secret: a
// document's Protected Comms password is one password for every Protected
// message in it, so proving it once opened all of them. The user's rule 1 gives
// every marked tier a password of its own and rule 2 asks for "a new password for
// the new attribute" — Protected is not excepted from either.
Configuration::SectionProofs Configuration::proofsRequiredFor(CommsProtection tier)
{
    switch (tier) {
    case CommsProtection::None:
        return {false, false};
    case CommsProtection::ReadOnly:
    case CommsProtection::Hidden:
        // No device is named anywhere in these two tiers, deliberately. They are
        // the document's own business and must stay usable with nothing plugged
        // in — an offline tool, a laptop on a bench, a .ct3 mailed to a customer.
        return {true, false};
    case CommsProtection::Protected:
        return {true, true};
    }
    return {true, true}; // an unrecognised tier is guarded, never waved through
}

// Which password opens a section is decided by its TIER, and this is the one
// place that decides it. Every suppression site in the app — the sections list,
// the section editor, the Channel Summary report, Check Channels, Monitor
// Channels, the Lua bindings — asks this and passes the answer to
// CommsSection::isConcealed().
//
// It used to be `commsRevealed() || sectionAccessGranted(name)`, and that made
// the Hidden tier inert on exactly the documents it was built for.
// commsRevealed() is true whenever a document carries no Protected Comms
// verifier, and a user who chose PER-SECTION passwords has no reason to set one
// — so a Hidden section with its own messageKey printed its CAN ID, its frame
// layout and every channel's bit position everywhere, and anySectionConcealed()
// answered false so it saved to a plain .ct3 without objection.
//
// It then treated a marked section with NO key as open, and that was the user's
// bug: the wire has no room for a key, so a Get returns every section keyless,
// and a message the user had marked Hidden came back padlocked in the list with
// its channels listed beside it and its editor opening on a double-click. It
// FAILS CLOSED now. See the header for what that costs and why the cost is the
// one the product was always asking for.
bool Configuration::isSectionRevealed(const CommsSection &section, int busIndex) const
{
    switch (section.protection) {
    case CommsProtection::None:
        return true;
    case CommsProtection::ReadOnly:
        // Never conceals. That IS the tier — visible and not editable — and it
        // is the entire difference between Read Only and Hidden.
        return true;
    case CommsProtection::Hidden:
        // This section's own password, and no other. The document-wide Edit
        // Protected Comms password is deliberately NOT accepted here: the user's
        // decision is that Hidden is the section's business, and a master key
        // over it was never authorised. A section with no password of its own has
        // no grant either (grantSectionAccess refuses to record one), so it is
        // concealed from everybody — there is no password in existence that could
        // open it, and showing the protocol to someone who cannot produce one is
        // the exact failure this tier exists to prevent.
        return sectionGrantProves(section, busIndex);
    case CommsProtection::Protected:
        return protectedSectionProved(section, busIndex);
    }
    return false;
}

// Protected's two halves, in the one place both predicates read.
//
// A grant means BOTH proofs were met — the section's own password and a live
// device confirming Protected Comms — because that is the contract on
// grantSectionAccess and proofsRequiredFor is what the two dialogs run before
// they call it. commsRevealed() on its own is deliberately NOT accepted for a
// keyed section: it is a verifier sitting in the same file as the message it
// guards, it is one password for every Protected message in the document, and it
// says nothing whatever about this section's own.
//
// THERE IS NO KEYLESS ARM ANY MORE, and its removal is the third face of the
// user's bug. It used to read `|| (messageKey == kNoAccessKey && commsRevealed())`
// and was justified as an upgrade path for Protected messages written before
// 2.3.1, which never carried a section key. What it actually did was open every
// Protected section a Get returns — the wire carries `reserved[4]` and no key,
// so all of them are keyless — to any session whose document has no Edit
// Protected Comms password, and commsRevealed() is TRUE for such a document.
// Retrieve a configuration into a fresh window and its Protected messages were
// legible. The same shape as the Hidden hole above, one tier up.
//
// The pre-2.3.1 documents it was written for are not left stranded: their author
// holds the original file, the section can still be removed, and giving it a
// password is free (applyBusSections treats a FIRST key as an addition). What
// they cannot do is read a marked message with no password behind it, which was
// never a thing this application could honestly offer.
bool Configuration::protectedSectionProved(const CommsSection &section, int busIndex) const
{
    return sectionGrantProves(section, busIndex);
}

// The untick rule, per section. Same tiering as isSectionRevealed above with one
// difference that matters: Read Only conceals nothing, but LOWERING it still
// takes its password, because that is what the tier is for.
//
// Keyless fails closed here too, at ALL THREE marked tiers including Read Only.
// An untick is authorised by a password and a keyless section has none, so there
// is nothing that could authorise it. Read Only is in scope even though it hides
// nothing, because what an untick gives away is the MARKING rather than the
// protocol: the tier whose whole promise is "this needs my password to change"
// must not answer "there is no password, so help yourself".
bool Configuration::maySectionLower(const CommsSection &section, int busIndex) const
{
    switch (section.protection) {
    case CommsProtection::None:
        return true;
    case CommsProtection::ReadOnly:
    case CommsProtection::Hidden:
        return sectionGrantProves(section, busIndex);
    case CommsProtection::Protected:
        return protectedSectionProved(section, busIndex);
    }
    return false;
}

bool Configuration::setCommsPassword(const QString &password)
{
    // Changing the password of a document whose protected messages you cannot
    // see would be the way past not knowing the old one.
    if (hasCommsPassword() && !commsRevealed())
        return false;

    // m_secureOptions has to move with the password, because two of its fields
    // are derived from it. It is the recipe a plain Save reuses to rewrite a
    // document that came from a .ct3s: `password` is what writeSecureFile wraps
    // the file key under, and `embeddedCommsKey` is the 4-byte key sealed into
    // the file so a customer's copy can satisfy a device's protected-comms gate
    // without ever being told the password. Rewrite the verifier and leave those
    // behind and the next Save seals the file under the PREVIOUS password while
    // the body inside it carries the new verifier — the old password opens the
    // container and then reveals nothing, and the new one does not open it at
    // all. The file becomes unopenable with either, which is not a state anyone
    // can recover from.
    //
    // requirePassword is deliberately NOT touched: it says which of the two
    // container modes the file is in, and that is the Save Secure Config
    // dialog's decision, not a side effect of changing a password.
    if (password.isEmpty()) {
        // Clearing is not the same as forgetting: the verifier goes, and with it
        // the key, because there is no longer a password for the key to be the
        // derivation of. Nothing about any SECTION moves when it does, at any
        // tier, and that is worth saying because it used to. While Protected
        // carried a keyless arm reading `commsRevealed()`, clearing the document
        // password turned every keyless Protected section visible and freely
        // untickable in the same instant — a "clear a password" action that
        // published messages. The arm is gone (see protectedSectionProved), so
        // Read Only, Hidden and Protected all answer to their own passwords here
        // and this was never a master key over any of them.
        m_accessVerifiers.setVerifier(AccessFunction::EditProtectedComms, AccessVerifier{});
        m_commsKey = kNoAccessKey;
        m_commsRevealed = false;
        // Nothing left to wrap under and no key worth embedding. If the document
        // came from a password-protected .ct3s, the next Save now fails in
        // writeSecureFile — asked to require a password it has not been given —
        // and that is the right way round: silently re-saving it in the
        // key-travels-in-the-file mode would hand back a weaker file under the
        // same name, with nothing on screen to say the requirement had gone.
        m_secureOptions.password.clear();
        m_secureOptions.embeddedCommsKey = kNoAccessKey;
        setDirty();
        return true;
    }
    m_accessVerifiers.setVerifier(AccessFunction::EditProtectedComms,
                                  AccessVerifier::make(password));
    m_commsKey = deriveAccessKey(password);
    // Only kept when the file's key is actually wrapped under it, matching what
    // loadFromFile stores and for the same reason: holding cleartext for a mode
    // that never reads it is a liability rather than a convenience.
    m_secureOptions.password = m_secureOptions.requirePassword ? password : QString();
    m_secureOptions.embeddedCommsKey = m_commsKey;
    // Whoever just set the password holds it; making them type it back to see
    // their own messages would be theatre.
    m_commsRevealed = true;
    setDirty();
    return true;
}

void Configuration::setFleetIdentity(const FleetIdentity &id)
{
    m_fleetIdentity = id;
    setDirty();
}

void Configuration::setUploadPolicy(const UploadPolicy &policy)
{
    m_uploadPolicy = policy;
    setDirty();
}

void Configuration::setDeviceLock(const QString &uid, AccessKey key)
{
    // Normalised on the way in — upper case, no separators — so a UID pasted
    // from Device Status, typed by hand, or copied out of an email all compare
    // equal. mayBeSentTo() then needs no cleverness.
    QString clean;
    for (const QChar &c : uid)
        if (c.isLetterOrNumber())
            clean += c.toUpper();
    m_lockedDeviceUid = clean;
    m_deviceLockKey = clean.isEmpty() ? kNoAccessKey : key;
    setDirty();
}

bool Configuration::mayBeSentTo(const QString &uid) const
{
    if (m_lockedDeviceUid.isEmpty())
        return true; // not locked: goes anywhere
    QString clean;
    for (const QChar &c : uid)
        if (c.isLetterOrNumber())
            clean += c.toUpper();
    // An empty uid means the device could not tell us who it is — firmware too
    // old for the identity command. A locked configuration must NOT be sent to
    // a unit that cannot be identified: "I do not know" is not "it matches".
    if (clean.isEmpty())
        return false;
    return clean == m_lockedDeviceUid;
}

// Document schema version. Bumped to 2 for the DBC-style comms rows; to 3 when
// the 1-axis lookup table widened to 16 sites and its key became "tables2x16";
// to 4 for the v16 "integrators" key; to 5 when v17 gave those rows a
// direction, a start value and a preserve flag.
// The bump matters: a v13-written file still parses cleanly in a pre-v13 build,
// which would find no "tables2x8" key and silently load ZERO 1-axis tables. The
// version guard turns that into an explicit "saved by a newer version" refusal.
// A brand-new key is the same hazard in the same direction — a pre-v16 build
// would drop every integrator without a word — so it earns a bump too. New
// keys INSIDE an existing object are no safer: a pre-v17 build would read a
// v17 decrementor, miss "countDown", and load it as an integrator counting the
// wrong way.
// v6 added the "lock" record and, with it, the read-protected form in which
// every content key moves inside an encrypted "body" blob. A pre-v6 build
// opening a v6 file would find no buses, no channels and no calculations and
// present that as an empty configuration, so the guard has to refuse it.
// v7 added CommsSection::hidden. A pre-v7 build would not find the key, load
// the section as ordinary, and display every field the flag exists to withhold
// — the failure mode the guard exists for, so it earns a bump like any other.
// v8 renamed that flag to "protectedComms", replaced the single "lock" record
// with the per-function "accessVerifiers" object, and added "updateIdentity".
// The rename alone would force the bump for the same reason v7 did — a pre-v8
// build finds no "hidden" key and shows every protected message in full — and
// the other two only cost an old build information it never had anyway. Going
// the other way is a migration rather than a version check: see openLegacyBody.
// v9 replaced "updateIdentity" with "fleetIdentity" — the same block grown two
// string identifiers and a serial number — and added "uploadPolicy" beside it.
// Both halves earn the bump on their own: a pre-v9 build finds no
// "updateIdentity", loads a blank identity, and shows a configuration as
// belonging to no fleet when it belongs to a very specific one.
// v10 added the math rows' third operand (cIsChannel/cChannel/cConst — the
// advanced-math ops). New keys inside an existing object again: a pre-v10
// build would load a MULADD missing its C and quietly compute something the
// user never wrote, so it earns the bump like v17's "countDown" did. Older
// files keep loading — missing c* keys default to an unused const-0 operand.
//
// v11 added the counters' rate mode (rateHz/rateCountDown, and mode value 2).
// This one is not merely additive: a pre-v11 build reading mode == 2 clamps it
// back to a mode it does understand, so it would show — and re-send — an
// up/down counter with no inputs where the file says "step every 100 ms". That
// is a silent change of meaning rather than a missing field, which is exactly
// what the guard is for. Older files keep loading; their counters have no rate
// keys and default to 1 Hz counting up, which the edge modes never read.
//
// v12 replaced the 4x4 lookup table with an 8x8: the "tables4x4" key became
// "tables8x8" and its rows may now carry up to 8 sites per axis. Both halves
// earn the bump, in the two directions this comment keeps describing. Forwards:
// a pre-v12 build opening a v12 file finds no "tables4x4" and loads ZERO
// two-axis tables without a word — the same silent loss v13's "tables2x8"
// rename would have caused, and the guard turns it into a refusal. Backwards is
// not a version check but a MIGRATION, and it is unconditional: loadBody still
// reads "tables4x4" and lands each one in the top-left of an 8x8, so every
// schema-11 file keeps opening with its tables intact. See Table8x8Row::
// fromTable4x4Json for why that migration is a parse rather than a reshaping.
//
// This number is deliberately NOT part of the reset that took PROTOCOL_VERSION
// and FLASH_STORE_VERSION back to 1 for the v1 product. Those two describe a
// conversation with hardware that has never shipped, so nothing in the world
// remembers their old values and renumbering costs nothing. A .ct3 is the
// opposite: files written at fileVersion 2 through 8 are sitting on people's
// disks right now, and renumbering this constant to 1 would make readWrapper's
// "saved by a newer version" guard refuse every single one of them. The file
// schema keeps counting, and the mismatch with the wire version is the point
// rather than an oversight.
// v13 added "deviceLock": the UID of the single unit a configuration may be
// sent to, plus the key that authorises moving that lock. Additive, and
// omitted entirely when unset — but it earns the bump for the same reason v11
// did. A pre-v13 build finds no "deviceLock", loads the file as unlocked, and
// will happily send it to any unit on the bench. That is not a missing field;
// it is the file's central instruction being silently discarded, and the
// symptom is a calibration written into the wrong vehicle.
//
// v14 replaced "protectedComms" + "readOnlyComms" + the v7 "hidden" with a
// single ordered "protection" token. This is the least optional bump in the
// list, and it is the v7 case exactly: a shipped v13 build opening a v14 file
// finds no key it recognises, loads a Hidden message as an ordinary one, and
// then prints its CAN ID, frame layout, timing and every channel's bit position
// in the sections list, the Channels pane, the Config Summary and Check
// Channels. Because the guard refuses the file outright instead, that build
// says "saved by a newer version" and shows nothing — no silent misread, no
// leak. Going the other way is a migration and lives in CommsSection::fromJson,
// which is why that function had to start taking the file version: the migration
// must run on v13 and older and MUST NOT run on v14, or a Read Only section
// ratchets into Hidden on every load.
//
// v15 added "scriptBytecode": the compiled script image a Get retained from a
// device, base64, present only in a document read back off a unit that was
// running a script. Additive like v11 and v13 — and mandatory for the same
// reason, in its sharpest form yet. There is NO SOURCE anywhere for that image;
// the file is the only copy. A shipped 2.3.2 build finds no key it recognises,
// loads the document as scriptless, and the next Send STRIPS THE SCRIPT OFF THE
// DEVICE — the exact footgun this feature exists to remove, re-armed by an older
// build silently discarding one key. The guard refuses the file to that build
// instead, which turns "your script is gone" into "this file needs a newer
// version".
// 16: the Transmit CRC8 section device and its recipe keys (crcChannel,
// crcByteLocation, crcPolynomial/Init/FinalXor, crcRefIn/Out, crcElements).
// A bump for the same reason the script's was: an older build would read a
// transmitCrc8 section as a RECEIVE message (the device-token fallback) and
// the next Send would install a listener where the author configured a
// stamped transmit. Refusing the file names the real remedy instead.
// 17: Triggered transmit — "transmitCondition" and "resetConditionOnTransmit"
// on a transmit section. Additive keys, and the bump is for the now-familiar
// reason: "cyclic": false has been a legal, inert value in every file since the
// beginning, so an older build reads one of these sections as a perfectly
// ordinary message and finds nothing missing. It would then send a message that
// transmits CONTINUOUSLY where the author configured it to transmit only on a
// condition — traffic on a customer's bus that nobody asked for, from a file
// that looked like it loaded cleanly. Refusing names the remedy instead.
//
// The same release forces every User Condition's output channel to boolean.
// That migration needs no version to key off: it is idempotent, it runs
// unconditionally on load, and a file already holding boolean outputs is
// unchanged by it.
// 18: User Condition modes. A condition is now Momentary or Set/Reset and
// carries TWO expressions under "set" and "reset", plus "mode" and "latchHz";
// the old single "terms"/"joiners" pair is gone, and a comparison may name a
// MESSAGE ("aMessageBus"/"aMessage") instead of comparing channels.
//
// The bump is mandatory for the usual reason in its sharpest form yet: an older
// build reads a modes file, finds no "terms" key, and loads every condition as
// a single empty comparison — silently, because an absent key has always been a
// legal way to say "default". It would then Send a configuration whose logic is
// simply missing. Refusing the file names the remedy instead.
//
// Going the OTHER way is a migration and lives in ConditionRow::fromJson: a
// file with no "mode" is pre-modes, and its one expression becomes the Set with
// its logical inverse as the Reset. That is keyed on the absent key rather than
// on this number, and deliberately — it is self-describing and idempotent, so
// it cannot run twice. The version is threaded to fromJson only where absence
// is AMBIGUOUS, which for conditions it is not; see the note on loadBody.
// 18 -> 19 adds CommsChannelRow::clampToRange. The key is additive and its
// absence is unambiguous (a file without it predates the option and clamped),
// so nothing needs migrating — the bump is for the guard ABOVE, in the other
// direction. A build that predates the flag would drop it on load and Send a
// configuration that clamps where the author asked it to roll over: a silent
// behaviour change on a real bus. Refusing to open the file says so instead.
static constexpr int kConfigSchemaVersion = 20;

// The one accessor, so nothing outside this file has to hold a second copy of
// the number. Communications templates stamp it into the file they write and
// hand it back to CommsSection::fromJson on the way in.
int configSchemaVersion()
{
    return kConfigSchemaVersion;
}

namespace {

// Reads and validates the outer wrapper. Shared by peekFile and loadFromFile so
// the two can never disagree about what counts as a configuration file.
bool readWrapper(const QString &path, QJsonObject *rootOut, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error)
            *error = f.errorString();
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &parseError);
    if (doc.isNull() || !doc.isObject()) {
        if (error)
            *error =
                QStringLiteral("Not a valid configuration file: %1").arg(parseError.errorString());
        return false;
    }
    const QJsonObject root = doc.object();
    if (root["fileType"].toString() != QLatin1String("CANTripleConfig")) {
        if (error)
            *error = QStringLiteral("Not a CAN Triple configuration file");
        return false;
    }
    if (root["fileVersion"].toInt(1) > kConfigSchemaVersion) {
        if (error)
            *error = QStringLiteral("This file was saved by a newer version of "
                                    "CAN Triple Device Manager and can't be opened.");
        return false;
    }
    if (rootOut)
        *rootOut = root;
    return true;
}

// True when a JSON root carries the retired single-password record in its
// read-protecting form — the one shape that will not open without a password.
bool isLegacyReadProtected(const QJsonObject &root)
{
    return root[QStringLiteral("lock")].toObject()[QStringLiteral("protectRead")].toBool(false);
}

// ------------------------------------------------------ pre-v8 lock migration
//
// Files written before v8 may carry a "lock" record: the old single
// Configuration Password. protectRead moved every content key into a sealed
// base64 "body"; protectWrite left the body legible and added a "bodyMac" over
// it. That system is gone — the dialog, the document API and the firmware
// commands with it — but the files are not, and refusing them would strand
// every configuration anybody ever protected with it.
//
// So it is read here and nowhere else. A read-protected file is decrypted with
// the old scheme (the primitives in config_lock.h survive for exactly this and
// for the .ct3s container) and its recovered body handed on. A write-protected
// one simply loads: write protection was only ever a deterrent painted over
// legible JSON, and there is nothing left in the app to enforce it. The
// "bodyMac" is not checked either — verifying it needs a key derived from a
// password the caller may never have been asked for, and a tamper warning that
// can only sometimes appear is worse than one that never does.
//
// The migration is ONE WAY, and deliberately: whatever comes out of it has no
// lock at all. The user re-protects with Set Access Passwords and Save Secure
// Config, which is a differently shaped and considerably stronger protection.
// There is no route back — kConfigSchemaVersion's guard means the v8 file this
// document will next be saved as could not be opened by the build that wrote
// the original anyway.
bool openLegacyBody(const QJsonObject &root, const QString &password, QJsonObject *bodyOut,
                    QString *error)
{
    if (!isLegacyReadProtected(root)) {
        *bodyOut = root; // no lock, or write-protect only: the content keys are right here
        return true;
    }
    const QJsonObject lock = root[QStringLiteral("lock")].toObject();
    const QByteArray salt =
        QByteArray::fromBase64(lock[QStringLiteral("salt")].toString().toLatin1());
    const int iterations = lock[QStringLiteral("iterations")].toInt(kLockDefaultIterations);
    // The upper bound is a denial-of-service guard: deriveKeys runs PBKDF2 for
    // `iterations` rounds, so a hand-edited "iterations": 2000000000 would grind
    // for the better part of an hour before this even gets to check the
    // password. A real file uses kLockDefaultIterations; past kMaxKdfIterations
    // is treated as a damaged record. See kMaxKdfIterations in access_keys.h.
    if (salt.size() != kLockSaltBytes || iterations <= 0 || iterations > kMaxKdfIterations) {
        if (error)
            *error = QStringLiteral("This file's password record is damaged, so it cannot "
                                    "be unlocked.");
        return false;
    }

    ConfigKeys keys = deriveKeys(password, salt, iterations);
    const QByteArray sealed =
        QByteArray::fromBase64(root[QStringLiteral("body")].toString().toLatin1());
    QByteArray plain;
    // The tag is checked before anything is decrypted, so a wrong password and a
    // damaged file fail identically here — which is why the message names both
    // rather than guessing.
    const bool opened = openPayload(sealed, keys, &plain, nullptr);
    keys.clear();
    if (!opened) {
        if (error)
            *error = password.isEmpty()
                         ? QStringLiteral("This configuration is password protected.")
                         : QStringLiteral("Wrong password, or the file is damaged.");
        return false;
    }

    const QJsonDocument bodyDoc = QJsonDocument::fromJson(plain);
    plain.fill('\0'); // the plaintext is the secret; do not leave it in freed heap
    if (!bodyDoc.isObject()) {
        if (error)
            *error = QStringLiteral("The encrypted section did not contain a configuration.");
        return false;
    }
    *bodyOut = bodyDoc.object();
    return true;
}

} // namespace

bool Configuration::peekFile(const QString &path, FilePeek *out, QString *error)
{
    FilePeek peek;
    // ct:: qualified because Configuration has a member of the same name that
    // asks a different question — is THIS DOCUMENT secure, rather than is that
    // file — and unqualified lookup would find the member first.
    if (ct::isSecureFile(path)) {
        SecureFileInfo info;
        if (!peekSecureFile(path, &info, error))
            return false;
        peek.secure = true;
        peek.requiresPassword = info.requiresPassword;
        // Whether a .ct3s carries a comms verifier is not knowable from its
        // header — the verifiers live in the sealed body, which is where they
        // belong. False is the honest answer and it costs nothing: a standard
        // .ct3s opens without a password and stays concealed either way, and a
        // password-protected one is already covered by requiresPassword.
        peek.commsProtected = false;
    } else if (isBinaryConfigFile(path)) {
        // FORMAT 2. Neither secure nor password-bearing: a .ct3 has never had a
        // password and gaining a container did not give it one, so both flags
        // stay false and Open goes straight through without a prompt.
        //
        // The verifier set costs a decrypt to reach, because it lives in the
        // sealed body where it belongs. Paid rather than skipped: the JSON
        // wrapper answered this question, and a peek that quietly stopped
        // answering it would be a difference between the two formats that
        // nothing announced and nothing tested.
        QByteArray plain;
        if (!readBinaryConfigFile(path, &plain, nullptr, error))
            return false;
        const QJsonDocument bodyDoc = QJsonDocument::fromJson(plain);
        plain.fill('\0');
        peek.commsProtected = AccessVerifierSet::fromJson(
                                  bodyDoc.object()
                                      .value(QStringLiteral("accessVerifiers"))
                                      .toObject())
                                  .isSet(AccessFunction::EditProtectedComms);
    } else {
        QJsonObject root;
        if (!readWrapper(path, &root, error))
            return false;
        peek.commsProtected =
            AccessVerifierSet::fromJson(root.value(QStringLiteral("accessVerifiers")).toObject())
                .isSet(AccessFunction::EditProtectedComms);
        // A pre-v8 read-protected .ct3 will not open without its old password
        // either, and this flag is the caller's only cue to ask for one. Saying
        // so here is what makes the migration reachable — otherwise the load
        // fails demanding a password the user was never given a box to type.
        // It is not a .ct3s, so `secure` stays false.
        if (isLegacyReadProtected(root))
            peek.requiresPassword = true;
    }
    if (out)
        *out = peek;
    return true;
}

bool Configuration::loadFromFile(const QString &path, QString *error, const QString &password)
{
    QJsonObject body;
    bool secure = false;
    SecureSaveOptions secureOptions;
    AccessKey embeddedKey = kNoAccessKey;
    // The schema the file was WRITTEN at, which decides whether the pre-14
    // protection keys are migrated. It has to be captured here because the two
    // formats keep it in different places, and because for a pre-v8
    // read-protected .ct3 it lives in the legible wrapper while the content
    // lives in the sealed body — a body that carries no version of its own and
    // would otherwise default to 1. Both routes land below 14 for every file
    // that predates this release, which is the answer that matters.
    int fileVersion = 1;

    // The magic decides, not the extension: a .ct3s that lost its suffix in a
    // mail client still opens as what it is, and a .ct3 renamed to .ct3s is not
    // fed to the binary reader.
    if (ct::isSecureFile(path)) {
        QByteArray plain;
        SecureFileInfo info;
        if (!readSecureFile(path, password, &plain, &info, error))
            return false;
        const QJsonDocument bodyDoc = QJsonDocument::fromJson(plain);
        plain.fill('\0'); // the recovered body is the secret the container exists for
        if (!bodyDoc.isObject()) {
            if (error)
                *error = QStringLiteral("The secure file did not contain a configuration.");
            return false;
        }
        body = bodyDoc.object();
        // A COMMUNICATIONS TEMPLATE is the same container with different
        // contents, and this is where the two are told apart. Without this
        // check a .ct3t would decrypt perfectly, parse as a configuration with
        // every key absent, and REPLACE the open document with an empty one —
        // a wrong file must produce a sentence, not a blank document. Asked of
        // the template's own marker rather than by requiring "fileType" to say
        // CANTripleConfig, because a .ct3s body has never carried a fileType
        // and demanding one now would refuse every secure file already written.
        if (body.value(QStringLiteral("fileType")).toString()
            == QLatin1String(kCommsTemplateFileType)) {
            if (error)
                *error = QStringLiteral("This is a communications template, not a "
                                        "configuration. Load it from Connections > "
                                        "Communications with the Load… button.");
            return false;
        }
        // A .ct3s has no legible wrapper to hold the schema version, so
        // saveSecureToFile writes it inside the body and it is checked here —
        // the container's own formatVersion covers the layout of the bytes, not
        // the shape of the configuration in them.
        fileVersion = body.value(QStringLiteral("fileVersion")).toInt(1);
        if (fileVersion > kConfigSchemaVersion) {
            if (error)
                *error = QStringLiteral("This file was saved by a newer version of "
                                        "CAN Triple Device Manager and can't be opened.");
            return false;
        }
        secure = true;
        secureOptions.requirePassword = info.requiresPassword;
        // Kept so a plain Save can rewrite the file the way it was found. Only
        // for a file that required one — otherwise a speculative password the
        // user typed for a standard .ct3s would silently start requiring itself.
        secureOptions.password = info.requiresPassword ? password : QString();
        secureOptions.embeddedCommsKey = info.embeddedCommsKey;
        embeddedKey = info.embeddedCommsKey;
    } else if (isBinaryConfigFile(path)) {
        // FORMAT 2 — the sealed .ct3. What comes out of the container is the
        // same object the JSON wrapper used to be, fileType and fileVersion
        // included, so everything below this point reads it without caring
        // which of the three routes produced it.
        QByteArray plain;
        if (!readBinaryConfigFile(path, &plain, nullptr, error))
            return false;
        const QJsonDocument bodyDoc = QJsonDocument::fromJson(plain);
        plain.fill('\0');
        if (!bodyDoc.isObject()) {
            if (error)
                *error =
                    QStringLiteral("This configuration file is damaged and cannot be opened.");
            return false;
        }
        body = bodyDoc.object();
        // The marker is REQUIRED here, unlike on the .ct3s path where demanding
        // one would refuse every secure file already written. A format-2 body
        // has carried fileType since the first one existed, so anything sealed
        // behind this preamble without it is not a configuration.
        if (body.value(QStringLiteral("fileType")).toString()
            != QLatin1String("CANTripleConfig")) {
            if (error)
                *error = QStringLiteral("This file is not a CAN Triple configuration.");
            return false;
        }
        // The preamble says a schema too, and it is deliberately not consulted:
        // it is cleartext and editable, this one is covered by the payload's
        // tag, and two sources for one answer is how they come to disagree.
        fileVersion = body.value(QStringLiteral("fileVersion")).toInt(1);
        if (fileVersion > kConfigSchemaVersion) {
            if (error)
                *error = QStringLiteral("This file was saved by a newer version of "
                                        "CAN Triple Device Manager and can't be opened.");
            return false;
        }
    } else {
        QJsonObject root;
        if (!readWrapper(path, &root, error))
            return false;
        // From the WRAPPER, not from `body`: for a pre-v8 read-protected file
        // the two are different objects and only the wrapper is legible.
        fileVersion = root.value(QStringLiteral("fileVersion")).toInt(1);
        if (!openLegacyBody(root, password, &body, error))
            return false;
    }

    loadBody(body, fileVersion);

    m_secureFile = secure;
    m_secureOptions = secureOptions;

    // The key and the grant are two different things, and this is the one place
    // in the app where it is easy to conflate them.
    //
    // A .ct3s can carry the 4-byte Protected Comms key so that a customer
    // who has never seen the password can still satisfy a DEVICE's
    // protected-comms gate: send this configuration, get it back, update a
    // fleet. It does not follow that they may LOOK at the protected messages.
    // The key is for talking to hardware; the password is for reading the
    // protocol. So loading installs the key and leaves the document CONCEALED —
    // opening a .ct3s never reveals anything, however much key material came
    // with it.
    m_commsKey = embeddedKey;
    m_commsRevealed = false;
    // The per-section grants go with it. A grant records "somebody typed THIS
    // section's password", and it is matched by NAME — so carrying one across an
    // Open hands the incoming document's identically-named section to a viewer
    // who proved nothing about it. Two unrelated files sharing a section name is
    // not exotic; a great many of them are called "Engine Data". File > New
    // clears these through clear(), but Open loads over the LIVE document
    // without one, which is precisely how this survived its first review.
    m_sectionGrants.clear();
    // The one apparent exception is not one: whoever typed the right password
    // has proved they are entitled to the protocol, whether they typed it to
    // open a password-protected .ct3s or offered it speculatively for a file
    // that turned out to want it. The verifier decides, never the container.
    if (!password.isEmpty()
        && m_accessVerifiers.verifier(AccessFunction::EditProtectedComms).verify(password)) {
        m_commsRevealed = true;
        // A file that already handed us a key agrees with this derivation —
        // same password, same fixed salt — so skip a second 210k-round stretch
        // for a value we already have.
        if (m_commsKey == kNoAccessKey)
            m_commsKey = deriveAccessKey(password);
    }

    m_filePath = path;
    setDirty(false);
    emit documentReset();
    return true;
}

// The content keys. `root` is the file's root object for a plain .ct3, and the
// RECOVERED payload for a .ct3s or a migrated pre-v8 read-protected file — all
// of them carry exactly the same keys, which is what lets one reader serve
// every format. Schema 2 replaced the offset/length/mask + mult/div/adder comms rows
// with DBC-style start bit / bit length / type / factor / offset; older
// (schema 1) files still load, but their comms rows predate the DBC keys and
// come in with the default layout. The "saved by a newer version" guard lives
// in readWrapper, before anything here runs.
//
// `fileVersion` is the schema the file was written at. Only the comms sections
// read it, and only to decide whether the pre-14 protection keys need
// migrating; every other migration in here is unconditional because it keys off
// a renamed or absent JSON key, which is self-describing. The protection tier is
// not: schema 14 writes "protection" and stops writing the old keys, so
// "no legacy key present" is true of a v14 file AND of a v13 file with no
// protection, and only the version separates them.
void Configuration::loadBody(const QJsonObject &root, int fileVersion)
{
    for (auto &b : bus)
        b = BusConfig{};
    const QJsonArray buses = root["buses"].toArray();
    for (int i = 0; i < 3 && i < buses.size(); ++i)
        bus[i] = BusConfig::fromJson(buses[i].toObject(), fileVersion);

    QList<Channel> userChannels;
    for (const auto &v : root["userChannels"].toArray())
        userChannels.append(Channel::fromJson(v.toObject()));
    m_catalog.setUserChannels(userChannels);

    mathRows.clear();
    for (const auto &v : root["math"].toArray())
        mathRows.append(MathRow::fromJson(v.toObject()));
    conditionRows.clear();
    for (const auto &v : root["conditions"].toArray())
        conditionRows.append(ConditionRow::fromJson(v.toObject()));
    counterRows.clear();
    for (const auto &v : root["counters"].toArray())
        counterRows.append(CounterRow::fromJson(v.toObject()));
    timerRows.clear();
    for (const auto &v : root["timers"].toArray())
        timerRows.append(TimerRow::fromJson(v.toObject()));
    // v16. A pre-v16 file has no "integrators" key and loads with none, which is
    // exactly right — the feature did not exist, so there is nothing to recover.
    integratorRows.clear();
    for (const auto &v : root["integrators"].toArray())
        integratorRows.append(IntegratorRow::fromJson(v.toObject()));
    constantRows.clear();
    for (const auto &v : root["constants"].toArray())
        constantRows.append(ConstantRow::fromJson(v.toObject()));
    // v13 renamed the key when the table widened 8 -> 16 sites; pre-v13 files
    // still carry "tables2x8" and load unchanged (their 8 sites simply leave
    // the wider row half empty).
    table2x16Rows.clear();
    const QJsonArray tables2x16Json = root.contains(QStringLiteral("tables2x16"))
                                          ? root["tables2x16"].toArray()
                                          : root["tables2x8"].toArray();
    for (const auto &v : tables2x16Json)
        table2x16Rows.append(Table2x16Row::fromJson(v.toObject()));
    // v12 widened the two-axis table 4x4 -> 8x8 and renamed its key. A
    // schema-11 file's "tables4x4" entries are migrated into the top-left of an
    // 8x8 (counts carry over, sites and outputs copy, the remaining sites are
    // simply unused) so those files keep opening with their tables intact.
    //
    // BOTH keys are read and the results appended, rather than picking one as
    // the "tables2x8" rename above does. That rename was a rename — the same
    // rows under a new name, so choosing was safe. Here the two keys hold
    // different generations of the table, and choosing has a failure mode:
    // a file carrying "tables8x8": [] beside a populated "tables4x4" (a
    // hand-merge, or anything written by a build that emitted both) would load
    // the empty array and drop every table silently. Appending cannot lose one.
    // Nothing this app writes contains both keys.
    table8x8Rows.clear();
    for (const auto &v : root["tables8x8"].toArray())
        table8x8Rows.append(Table8x8Row::fromJson(v.toObject()));
    for (const auto &v : root["tables4x4"].toArray())
        table8x8Rows.append(Table8x8Row::fromTable4x4Json(v.toObject()));
    comments = root["comments"].toString();
    // v15. Both halves of the script in one call, so a file that somehow carries
    // both keys — hand-edited, or merged — resolves through the SAME precedence
    // rule as everything else instead of loading a pair this document could not
    // otherwise hold. Absent in every pre-v15 file, where the empty QByteArray is
    // the honest answer: those documents have a source or they have no script.
    //
    // Deliberately NOT verified here. The bytes are checked by the DEVICE's own
    // script_verify() at the two moments it matters — when a Get produces them
    // and again before a Send emits them (ScriptCompiler::attachTo) — and a
    // refusal at load would have nowhere to say so: loadBody has no notes
    // channel, so it could only drop the image silently, which is precisely the
    // failure mode this feature removes.
    setScript(root["scriptSource"].toString(),
              QByteArray::fromBase64(root["scriptBytecode"].toString().toLatin1()));
    m_configTitle = root["configTitle"].toString();
    // v8. Absent in every older file, and absent is right: a pre-v8 document had
    // no access passwords, so the default set says exactly what was true of it.
    // A migrated read-protected file lands here too, which is where its old lock
    // stops existing.
    m_accessVerifiers = AccessVerifierSet::fromJson(root["accessVerifiers"].toObject());
    {
        const QJsonArray slotKeys = root["messagePasswords"].toArray();
        for (int s = 1; s <= kCommsPasswordSlots; ++s) {
            const int i = s - 1;
            m_commsPasswords[i] = i < slotKeys.size()
                                      ? AccessKey(quint32(slotKeys.at(i).toDouble()))
                                      : kNoAccessKey;
        }
    }

    // v9. "fleetIdentity" is the current block. "updateIdentity" is what v8
    // called it, and it is read here as a DELIBERATELY PARTIAL migration.
    //
    // The two blocks disagree about what a vendor and a model ARE. v8 stored
    // both as opaque 32-bit numbers; a fleet identity names them as text,
    // because text is what the firmware now compiles in and what the wire now
    // carries as two 16-byte NUL-padded fields. There is no function from
    // 0x4D4F5445 to a name — the number was whatever the fleet builder typed
    // into a spin box — so the two old ids are dropped on the floor and
    // vendorId/modelId come through empty for the user to fill in.
    //
    // Inventing something legible out of the digits would be worse than an empty
    // field, and that is the whole argument. An empty field is visibly
    // unfinished: the fleet identity dialog shows it blank and the uploader
    // refuses to match anything against it, so the user is stopped at the one
    // moment they can still say what the vendor is called. A plausible-looking
    // "0x4D4F5445" or "VENDOR-1296651333" would sail through both and then fail
    // to match a real device, which presents as hardware being wrong rather than
    // as a migration that could not finish.
    //
    // configVersion and flags carry across unchanged; they meant the same
    // thing in both blocks. The old seriesId is dropped with the block that
    // defined it — there is no field left for it to land in. The old fleetKey goes with the ids rather than
    // surviving alone — a configuration that can no longer name its fleet cannot
    // target one either, so holding on to the secret would keep the only part of
    // the block that is dangerous to keep and useless without the rest.
    if (root.contains(QStringLiteral("fleetIdentity"))) {
        m_fleetIdentity = FleetIdentity::fromJson(root["fleetIdentity"].toObject());
    } else {
        const QJsonObject legacy = root[QStringLiteral("updateIdentity")].toObject();
        m_fleetIdentity = FleetIdentity{};
        // toInteger() rather than toInt(): these are opaque 32-bit numbers and
        // one above 2^31 comes back negative through the int overload.
        m_fleetIdentity.configVersion = quint16(legacy["configVersion"].toInteger());
        m_fleetIdentity.flags = quint16(legacy["flags"].toInteger());
    }
    // Absent in every pre-v9 file, and UploadPolicy's own defaults are the strict
    // pair — prove the fleet key, refuse a downgrade. That is the right
    // direction for a silent default to fail in: an old configuration arrives
    // demanding MORE of a device than it ever did, which an operator can see in
    // the uploader and deliberately relax, rather than less, which nobody would
    // notice until it had installed somewhere it should not have.
    m_uploadPolicy = UploadPolicy::fromJson(root[QStringLiteral("uploadPolicy")].toObject());

    // Absent for every configuration written before this existed, and for every
    // unlocked one since — which loads as "not locked", the honest default. Read
    // through setDeviceLock's normaliser so a hand-edited file with spaces or
    // lower case in the UID still matches the device.
    m_lockedDeviceUid.clear();
    m_deviceLockKey = kNoAccessKey;
    if (root.contains(QStringLiteral("deviceLock"))) {
        const QJsonObject lock = root[QStringLiteral("deviceLock")].toObject();
        setDeviceLock(lock[QStringLiteral("uid")].toString(),
                      AccessKey(lock[QStringLiteral("key")].toString().toULongLong()));
    }

    // Every User Condition output is boolean, including in files written before
    // that was true. LAST in this function on purpose: it reads conditionRows
    // and writes the catalogue, and both are set above — the channels at the top
    // of loadBody, the conditions after them.
    //
    // Unconditional rather than gated on fileVersion, because it is idempotent
    // and describes what the engine has always done rather than a format change.
    // A file where every condition output is already boolean loads bit-identical.
    forceConditionOutputsBoolean();
}

namespace {

// The .ct3s body: everything buildBody() writes, plus the one field buildBody()
// is structurally incapable of writing.
//
// FleetIdentity::fleetKey is the fleet secret — the thing a device proves it
// holds before it will accept an update. buildBody() passes `false` to
// FleetIdentity::toJson as a literal, so no caller, present or future, can talk
// it into emitting the key. Only this function can, and the only place it is
// called is saveSecureToFile.
//
// THE ORIGINAL REASON WAS "a plain .ct3 is a text file people mail to each
// other", AND THAT REASON IS GONE: as of format 2 a .ct3 is the same sealed
// container a .ct3s is. The behaviour is kept anyway, on the two reasons that
// survive, and they are weaker than the one they replace — worth saying so
// rather than pretending the argument is as strong as it was.
//
//   - A .ct3 is scrambled, not secret. Its key travels inside it, exactly as a
//     standard .ct3s's does, so neither is a vault; but a .ct3s is produced by
//     a deliberate act through a dialog that says what it carries, and a .ct3
//     is what gets attached to an email without much thought. The fleet's only
//     secret belongs in the file somebody had to mean to create.
//   - Nothing needs it there. A .ct3 is a working file; the fleet key matters
//     when a configuration is handed to somebody who must deploy it, which is
//     what a .ct3s is for.
//
// If that ever stops being convincing, the change is to call this from
// saveToFile as well — not to loosen buildBody(). A bool parameter on
// buildBody() was rejected on purpose: it would put the fleet secret one
// mistyped argument away from every file the program writes, and that is a
// mistake worth making impossible rather than merely unlikely.
QJsonObject withFleetKey(QJsonObject body, const FleetIdentity &identity)
{
    body[QStringLiteral("fleetIdentity")] = identity.toJson(true);
    return body;
}

} // namespace

QJsonObject Configuration::buildBody() const
{
    QJsonObject root;
    QJsonArray buses;
    for (const auto &b : bus)
        buses.append(b.toJson());
    root["buses"] = buses;
    QJsonArray userChannels;
    for (const Channel &c : m_catalog.userChannels())
        userChannels.append(c.toJson());
    root["userChannels"] = userChannels;
    QJsonArray math;
    for (const MathRow &m : mathRows)
        math.append(m.toJson());
    root["math"] = math;
    QJsonArray conds;
    for (const ConditionRow &c : conditionRows)
        conds.append(c.toJson());
    root["conditions"] = conds;
    QJsonArray counters;
    for (const CounterRow &c : counterRows)
        counters.append(c.toJson());
    root["counters"] = counters;
    QJsonArray timers;
    for (const TimerRow &t : timerRows)
        timers.append(t.toJson());
    root["timers"] = timers;
    QJsonArray integrators;
    for (const IntegratorRow &g : integratorRows)
        integrators.append(g.toJson());
    root["integrators"] = integrators;
    QJsonArray constants;
    for (const ConstantRow &c : constantRows)
        constants.append(c.toJson());
    root["constants"] = constants;
    QJsonArray tables2x16;
    for (const Table2x16Row &t : table2x16Rows)
        tables2x16.append(t.toJson());
    root["tables2x16"] = tables2x16;
    // The new key only. "tables4x4" is read on load and never written again —
    // writing both would give a file two generations of the same table and no
    // rule about which one wins when they disagree.
    QJsonArray tables8x8;
    for (const Table8x8Row &t : table8x8Rows)
        tables8x8.append(t.toJson());
    root["tables8x8"] = tables8x8;
    root["configTitle"] = m_configTitle;
    root["comments"] = comments;
    // The script, in whichever of its two forms this document holds. Both keys
    // are written only when non-empty, so every existing .ct3 stays
    // byte-identical and a document with no script carries no trace of the
    // feature.
    //
    // The invariant makes these mutually exclusive: at most one of the two is
    // ever written, so a file — like the document it came from — cannot record a
    // source beside a stale image for a later load to choose between.
    if (!m_scriptSource.isEmpty())
        root["scriptSource"] = m_scriptSource;
    // v15. The compiled image a Get retained, base64 because JSON has no bytes.
    // This is the ONLY copy of that script in existence — there is no source to
    // recompile it from — which is what earned the schema bump: a shipped 2.3.2
    // build finds no key it knows, loads the document as scriptless, and the
    // next Send REMOVES the script from the unit. The fileVersion guard refuses
    // the file to that build instead, so the loss cannot happen quietly.
    if (!m_scriptBytecode.isEmpty())
        root["scriptBytecode"] = QString::fromLatin1(m_scriptBytecode.toBase64());
    // v8. A verifier is salted and one-way: it cannot be turned back into the
    // password, and — the part that matters — it cannot be turned into the
    // 4-byte key the hardware compares either. That is precisely why
    // access_keys.h derives two unrelated things from one password, and it is
    // what makes this safe to write into a .ct3 anyone can read.
    root["accessVerifiers"] = m_accessVerifiers.toJson();
    // The four message passwords, as derived keys. They are written like every
    // other secret this document holds — the key, never the password — and a
    // slot with nothing in it is written as 0 rather than omitted, so the array
    // is always four long and a slot number always means the same slot.
    {
        QJsonArray slotKeys;
        for (int s = 1; s <= kCommsPasswordSlots; ++s)
            slotKeys.append(double(commsPassword(s)));
        root["messagePasswords"] = slotKeys;
    }
    // false, and not negotiable at this call site — see withFleetKey.
    root["fleetIdentity"] = m_fleetIdentity.toJson(false);
    // Always written, even when it is the default pair, because the two flags
    // that matter are the ones somebody turned OFF. Omitting a default-looking
    // policy would read back as the default on load, which is the same thing —
    // but only until the defaults change, and then every quietly-omitted policy
    // would silently adopt the new ones.
    root["uploadPolicy"] = m_uploadPolicy.toJson();
    // Written only when set, so an unlocked configuration carries no trace of
    // the feature and a diff between two unlocked files stays clean. The key is
    // written alongside because it is what lets the person who set the lock move
    // it later; it is a PBKDF2-derived key, not the password, exactly like the
    // access keys above — see accessVerifiers.
    if (!m_lockedDeviceUid.isEmpty()) {
        QJsonObject lock;
        lock[QStringLiteral("uid")] = m_lockedDeviceUid;
        lock[QStringLiteral("key")] = QString::number(m_deviceLockKey);
        root[QStringLiteral("deviceLock")] = lock;
    }
    return root;
}

bool Configuration::saveToFile(const QString &path, QString *error)
{
    // A CONCEALED KEYED SECTION NO LONGER REFUSES THE SAVE, and the reason the
    // refusal existed is worth keeping visible because it was a good one right
    // up until the format changed underneath it.
    //
    // It read: this writer emits the body as indented JSON, so a concealed
    // message would go to disk with its CAN ID, DLC, byte order, timing,
    // routing and every signal's start bit, length, factor and offset legible
    // in Notepad — a complete bypass of concealment, no password asked for, the
    // protocol handed to somebody not entitled to read it. Every word of that
    // was true of format 1 and none of it is true of format 2. The body is
    // sealed now (config_file.h), and what comes out of the file is what went
    // in: the tier and the messageKey travel with the section, so reopening
    // this file produces the same padlocked row, needing the same password.
    //
    // What a save does, then, is move a locked message from one sealed file to
    // another. The person doing it cannot read the message before or after. The
    // guard was protecting against emitting protocol in the clear, and nothing
    // here emits anything in the clear any more.
    //
    // It also cost more than it protected. A configuration read back off a unit
    // arrives with every marked section keyless — the wire has no room for a
    // key — and that case already had a carve-out; the case left refusing was
    // the ordinary one of holding a supplier's configuration and wanting to
    // save your own edits to the rest of it.
    //
    // WHAT STILL ENFORCES THE MARKING, unchanged: opening a concealed section
    // for editing, which takes that message's own Message Password and, at
    // Protect Communication, the Protected Comms password proved against a
    // connected device. Saving and loading move the box; those two open it.
    // anyKeyedSectionConcealed() remains as the question, and no longer as a
    // veto.

    QJsonObject root;
    root["fileType"] = QStringLiteral("CANTripleConfig");
    root["fileVersion"] = kConfigSchemaVersion;
    // Which BUILD wrote this, for people rather than for the parser. fileVersion
    // says how to read the file and is the only thing load ever compares;
    // writtenBy says who produced it, which is the question actually asked when
    // a configuration turns up behaving oddly and nobody remembers which machine
    // it came off.
    //
    // Deliberately never read back. A parser that branched on an application
    // version would be deciding structure from a number that does not describe
    // structure — that is fileVersion's job, and two numbers answering one
    // question is how they drift apart.
    root["writtenBy"] = QStringLiteral(CT_APP_VERSION);
    const QJsonObject body = buildBody();
    for (auto it = body.constBegin(); it != body.constEnd(); ++it)
        root.insert(it.key(), it.value());

    // COMPACT, not indented. These bytes are the plaintext of a sealed
    // container now, so indentation would pad the ciphertext without making
    // anything legible — the same call saveSecureToFile and the template writer
    // make, for the same reason.
    QByteArray plain = QJsonDocument(root).toJson(QJsonDocument::Compact);
    const bool written = writeBinaryConfigFile(path, plain, kConfigSchemaVersion,
                                               QStringLiteral(CT_APP_VERSION), error);
    // The body passed through this buffer on its way into the container; do not
    // leave a legible copy of it in freed heap when the whole point of the
    // format is that it is not lying around legible.
    plain.fill('\0');
    if (!written)
        return false;

    // The write is still PROVEN to have reached disk before this returns, and
    // it is still an in-place QFile write rather than an atomic replace, for
    // the reason config_file.cpp sets out at length: a rename cannot overwrite
    // a file OneDrive happens to have open, and that is a far commoner event
    // than a full disk.
    m_filePath = path;
    // What is on disk is now legible JSON, so that is what this document is.
    // Choosing between the two formats — and in particular not downgrading a
    // .ct3s to a .ct3 by accident — is the caller's job; isSecureFile() is the
    // question it asks to do it.
    m_secureFile = false;
    setDirty(false);
    return true;
}

bool Configuration::saveSecureToFile(const QString &path, const SecureSaveOptions &options,
                                     QString *error)
{
    // The same relaxation saveToFile makes, for the same reason and with the
    // same limit. A .ct3s was ALWAYS sealed, so this writer never emitted
    // anything in the clear and its refusal rested purely on the principle that
    // a session which cannot read a message should not be the one that rewrites
    // the file carrying it. The principle survives; what changed is that it is
    // now enforced where it bites — at the editor — rather than by refusing a
    // save that discloses nothing. A concealed section is written with its tier
    // and its messageKey intact and comes back concealed.

    QJsonObject body = withFleetKey(buildBody(), m_fleetIdentity);
    // The version guard readWrapper applies to a .ct3, carried where a .ct3s can
    // keep it. loadFromFile reads it back out of the recovered body.
    body[QStringLiteral("fileVersion")] = kConfigSchemaVersion;
    // Compact: these bytes are the plaintext of an encrypted container, and
    // indentation would only pad the ciphertext without making anything legible.
    QByteArray plain = QJsonDocument(body).toJson(QJsonDocument::Compact);
    const bool ok = writeSecureFile(path, plain, options, error);
    plain.fill('\0'); // the fleet secret passed through here; do not leave it behind
    if (!ok)
        return false;

    m_filePath = path;
    m_secureFile = true;
    m_secureOptions = options;
    setDirty(false);
    return true;
}

// One reference. Every renameChannelRefs() walk below is sums of this, so the
// case rule (insensitive, like all channel matching) is decided exactly once.
static int renameRef(QString &ref, const QString &oldName, const QString &newName)
{
    if (ref.compare(oldName, Qt::CaseInsensitive) != 0)
        return 0;
    ref = newName;
    return 1;
}

// The per-family walks renameChannelReferences() sums over the document. See
// their declarations in comms_types.h for why they are free functions: the
// grid dialogs run the same walks over their private working copies when
// channelRenamed fires, and sharing the code is what keeps the two sides
// agreeing on which fields carry a channel name.
int renameChannelRefs(CommsSection &section, const QString &oldName, const QString &newName)
{
    int updated = renameRef(section.diagnosticChannel, oldName, newName);
    // The CRC8 publish channel is a reference like any row's — a rename that
    // missed it would leave the checksum publishing into a name that no
    // longer exists, exactly the dangling reference this walk exists to
    // prevent.
    updated += renameRef(section.crcChannel, oldName, newName);
    // A Triggered message names its User Condition by the condition's output
    // channel, so this walk is what keeps that binding alive across a rename —
    // and it is the reason the binding is a channel name rather than a row
    // index. Miss it and the message quietly stops transmitting: the condition
    // still exists, but nothing answers to the name the section remembers.
    updated += renameRef(section.transmitCondition, oldName, newName);
    for (CommsChannelRow &row : section.rows)
        updated += renameRef(row.channelName, oldName, newName);
    for (CompoundIdentifier &ident : section.identifiers)
        for (CommsChannelRow &row : ident.rows)
            updated += renameRef(row.channelName, oldName, newName);
    return updated;
}

int renameChannelRefs(QList<CommsSection> &sections, const QString &oldName,
                      const QString &newName)
{
    int updated = 0;
    for (CommsSection &s : sections)
        updated += renameChannelRefs(s, oldName, newName);
    return updated;
}

int renameChannelRefs(QList<MathRow> &rows, const QString &oldName, const QString &newName)
{
    int updated = 0;
    for (MathRow &m : rows) {
        updated += renameRef(m.aChannel, oldName, newName);
        updated += renameRef(m.bChannel, oldName, newName);
        updated += renameRef(m.cChannel, oldName, newName);
        updated += renameRef(m.destChannel, oldName, newName);
    }
    return updated;
}

int renameChannelRefs(QList<ConditionRow> &rows, const QString &oldName, const QString &newName)
{
    int updated = 0;
    for (ConditionRow &c : rows) {
        // BOTH expressions, including a Momentary's unused Reset half. That half
        // is kept in the document so switching modes does not destroy what was
        // typed, so it has to be repointed too — otherwise a rename while in
        // Momentary would quietly break the Reset the moment the user switched
        // back to Set/Reset.
        for (QList<ConditionTermRow> *side : {&c.setTerms, &c.resetTerms}) {
            for (ConditionTermRow &t : *side) {
                if (t.isMessageOp())
                    continue; // names a message, not a channel
                updated += renameRef(t.aChannel, oldName, newName);
                updated += renameRef(t.bChannel, oldName, newName);
            }
        }
        updated += renameRef(c.outputChannel, oldName, newName);
    }
    return updated;
}

int renameChannelRefs(QList<CounterRow> &rows, const QString &oldName, const QString &newName)
{
    int updated = 0;
    for (CounterRow &c : rows) {
        updated += renameRef(c.outputChannel, oldName, newName);
        updated += renameRef(c.upChannel, oldName, newName);
        updated += renameRef(c.downChannel, oldName, newName);
        updated += renameRef(c.followChannel, oldName, newName);
        updated += renameRef(c.resetChannel, oldName, newName);
        updated += renameRef(c.enableChannel, oldName, newName);
    }
    return updated;
}

int renameChannelRefs(QList<TimerRow> &rows, const QString &oldName, const QString &newName)
{
    int updated = 0;
    for (TimerRow &t : rows) {
        updated += renameRef(t.outputChannel, oldName, newName);
        // Both operands of both terms. A message operand is not a channel and
        // is deliberately not touched here — renaming a MESSAGE is a different
        // walk, the one the conditions already go through.
        for (ConditionTermRow *tr : {&t.startTerm, &t.stopTerm}) {
            if (tr->isMessageOp())
                continue;
            updated += renameRef(tr->aChannel, oldName, newName);
            if (tr->bIsChannel)
                updated += renameRef(tr->bChannel, oldName, newName);
        }
    }
    return updated;
}

int renameChannelRefs(QList<IntegratorRow> &rows, const QString &oldName, const QString &newName)
{
    int updated = 0;
    for (IntegratorRow &g : rows) {
        updated += renameRef(g.outputChannel, oldName, newName);
        updated += renameRef(g.inputChannel, oldName, newName);
        updated += renameRef(g.enableChannel, oldName, newName);
        updated += renameRef(g.resetChannel, oldName, newName);
    }
    return updated;
}

int renameChannelRefs(QList<ConstantRow> &rows, const QString &oldName, const QString &newName)
{
    int updated = 0;
    for (ConstantRow &c : rows)
        updated += renameRef(c.name, oldName, newName);
    return updated;
}

int renameChannelRefs(QList<Table2x16Row> &rows, const QString &oldName, const QString &newName)
{
    int updated = 0;
    for (Table2x16Row &t : rows) {
        updated += renameRef(t.outputChannel, oldName, newName);
        updated += renameRef(t.xChannel, oldName, newName);
    }
    return updated;
}

int renameChannelRefs(QList<Table8x8Row> &rows, const QString &oldName, const QString &newName)
{
    int updated = 0;
    for (Table8x8Row &t : rows) {
        updated += renameRef(t.outputChannel, oldName, newName);
        updated += renameRef(t.xChannel, oldName, newName);
        updated += renameRef(t.yChannel, oldName, newName);
    }
    return updated;
}

bool Configuration::hasProtectedComms() const
{
    for (const auto &b : bus)
        for (const CommsSection &s : b.sections)
            if (s.device != SectionDevice::Off && s.protection == CommsProtection::Protected)
                return true;
    return false;
}

int Configuration::renameChannelReferences(const QString &oldName, const QString &newName)
{
    int updated = 0;
    for (auto &b : bus)
        updated += renameChannelRefs(b.sections, oldName, newName);
    updated += renameChannelRefs(mathRows, oldName, newName);
    updated += renameChannelRefs(conditionRows, oldName, newName);
    updated += renameChannelRefs(counterRows, oldName, newName);
    updated += renameChannelRefs(timerRows, oldName, newName);
    updated += renameChannelRefs(integratorRows, oldName, newName);
    updated += renameChannelRefs(constantRows, oldName, newName);
    updated += renameChannelRefs(table2x16Rows, oldName, newName);
    updated += renameChannelRefs(table8x8Rows, oldName, newName);
    if (updated > 0)
        setDirty();
    // Deliberately NOT behind `updated > 0`: the listeners are open grid
    // dialogs, and a working copy can hold the document's only reference to
    // the old name (a row added since the dialog opened). See the signal.
    emit channelRenamed(oldName, newName);
    return updated;
}

// One message reference, repointed if it is the one being renamed. Compared
// case-insensitively and against the trimmed name, because that is how the
// mapper resolves these (messageRefKey trims and lower-cases) and how the
// validator matches them — a rename that only changed case would otherwise
// leave references the device can still resolve but this walk would not.
static int renameMessageRef(int refBus, QString &refName, int bus, const QString &oldName,
                            const QString &newName)
{
    if (refBus != bus || refName.trimmed().compare(oldName.trimmed(), Qt::CaseInsensitive) != 0)
        return 0;
    refName = newName;
    return 1;
}

int Configuration::renameMessageReferences(int bus, const QString &oldName, const QString &newName)
{
    // An empty NEW name is refused rather than written: the section editor
    // auto-names an empty field, so this cannot arrive from the UI, and blanking
    // a reference would turn "names a message that was renamed" into "names no
    // message at all" — which validation reports as an unfinished input rather
    // than the rename it actually was.
    if (oldName.trimmed().isEmpty() || newName.trimmed().isEmpty()
        || oldName.trimmed().compare(newName.trimmed(), Qt::CaseSensitive) == 0)
        return 0;

    int updated = 0;
    for (ConditionRow &c : conditionRows)
        for (QList<ConditionTermRow> *side : {&c.setTerms, &c.resetTerms})
            for (ConditionTermRow &t : *side)
                if (t.isMessageOp())
                    updated += renameMessageRef(t.aMessageBus, t.aMessage, bus, oldName, newName);
    for (TimerRow &t : timerRows)
        for (ConditionTermRow *term : {&t.startTerm, &t.stopTerm})
            if (term->isMessageOp())
                updated +=
                    renameMessageRef(term->aMessageBus, term->aMessage, bus, oldName, newName);
    // Counters: four of the five inputs can be a message. Follow cannot — it
    // reads a VALUE and a frame arriving has none. The rows the current type
    // does not read are walked too, deliberately: they are kept so switching
    // type back restores them, and one restored to a name that no longer exists
    // would be a broken input the user never touched.
    for (CounterRow &c : counterRows)
        for (CounterSource *src : {&c.upSource, &c.downSource, &c.resetSource, &c.enableSource})
            if (src->isMessage())
                updated += renameMessageRef(src->messageBus, src->message, bus, oldName, newName);
    if (updated > 0)
        setDirty();
    return updated;
}

int Configuration::forceConditionOutputsBoolean()
{
    int changed = 0;
    for (const ConditionRow &c : conditionRows) {
        if (!c.active || c.outputChannel.isEmpty())
            continue;
        // A device channel is the firmware's, not the document's, and cannot be
        // retyped from here. A condition writing into one is already a mistake
        // the mapper refuses; silently rewriting a built-in definition would be
        // a worse answer than leaving validation to say so.
        if (ChannelCatalog::isDeviceChannel(c.outputChannel))
            continue;
        Channel ch = m_catalog.findByName(c.outputChannel);
        if (!ch.isValid())
            continue; // nothing to retype yet; the picker creates it typed
        if (ch.dataType == QLatin1String("boolean") && ch.minValue == 0.0
            && ch.maxValue == 1.0 && ch.decimalPlaces == 0)
            continue;
        ch.dataType = QStringLiteral("boolean");
        ch.minValue = 0.0;
        ch.maxValue = 1.0;
        ch.decimalPlaces = 0;
        m_catalog.addOrUpdateUserChannel(ch);
        ++changed;
    }
    if (changed > 0)
        setDirty();
    return changed;
}

void Configuration::copyContentTo(Configuration &target) const
{
    for (int i = 0; i < 3; ++i)
        target.bus[i] = bus[i];
    target.mathRows = mathRows;
    target.conditionRows = conditionRows;
    target.counterRows = counterRows;
    target.timerRows = timerRows;
    target.integratorRows = integratorRows;
    target.constantRows = constantRows;
    target.table2x16Rows = table2x16Rows;
    target.table8x8Rows = table8x8Rows;
    target.comments = comments;
    // BOTH halves, in one call. Copying only the source is the mistake this
    // shape of change has already made once — see the m_accessVerifiers note
    // below — and here it would be silent and expensive: the copy would map as
    // a document with NO script, so anything comparing against a device (Verify,
    // the live view's mapping) would report the unit's script as an unexplained
    // difference, and anything that saved the copy would lose the only image of
    // it in existence.
    target.setScript(m_scriptSource, m_scriptBytecode);
    target.m_catalog = m_catalog;
    // m_accessVerifiers and m_commsRevealed ARE copied, and that reversed a
    // deliberate-looking omission. A live view is a Configuration, so anything
    // asked of a Configuration can be asked of it — including
    // maySectionLower(). Omitting the verifier set left the copy with
    // hasCommsPassword() false, which makes commsRevealed() true, which makes
    // every Protected tier in the copy freely lowerable and every Protected
    // section legible; whatever the scratch copy then produced could be written
    // back over the real document. Copying BOTH makes the copy answer exactly what
    // its source answers — no more, since m_commsRevealed travels with the
    // verifiers rather than defaulting open. This is a prerequisite for
    // applyBusSections, not a tidy-up.
    target.m_accessVerifiers = m_accessVerifiers;
    target.m_commsRevealed = m_commsRevealed;
    // The per-section grants travel for exactly the same reason, and the
    // asymmetry would be worse than the omission above: without them a scratch
    // copy would re-CONCEAL a section this session has already proved it may
    // read, so a live-view lookup (which channel does this row generate?) would
    // come back short precisely for the messages the user is looking at.
    target.m_sectionGrants = m_sectionGrants;
    // Still deliberately absent: m_commsKey is live key material a scratch copy
    // has no use for, and m_fleetIdentity / m_uploadPolicy answer which HARDWARE
    // a file may reach, which is no part of "which channels exist and what
    // generates them". In particular the copy never carries the fleet secret
    // inside the identity.
}

void Configuration::buildLiveView(Configuration &target, const ConfigPatch &patch) const
{
    copyContentTo(target);
    if (patch)
        patch(target);
}

bool Configuration::anySectionConcealed() const
{
    // No commsRevealed() short-circuit. It used to return false here for any
    // document without an Protected Comms password, which meant a Hidden
    // section guarded by its own password was written out to a plain .ct3 in
    // full by a session that could not read it.
    for (int b = 0; b < 3; ++b)
        for (const CommsSection &s : bus[b].sections)
            if (s.isConcealed(isSectionRevealed(s, b)))
                return true;
    return false;
}

// The file writers' question, and the narrower one. Why they no longer ask
// anySectionConcealed() is on the declaration; the short of it is that
// isSectionRevealed() fails closed now, so a Get fills the document with
// concealed sections and the broad test would have made backing up a unit
// impossible while protecting nothing that exists.
bool Configuration::anyKeyedSectionConcealed() const
{
    for (int b = 0; b < 3; ++b)
        for (const CommsSection &s : bus[b].sections)
            if (s.messageKey != kNoAccessKey && s.isConcealed(isSectionRevealed(s, b)))
                return true;
    return false;
}

// Protection is a property of the MESSAGE; it reaches a channel through the
// rows that carry it. So a channel inherits a tier when any section on any bus
// mentions it — including inside a compound identifier, which channelNames()
// already folds in.
//
// The old single isChannelProtected() answered TWO questions at once, and they
// have come apart. Concealment and edit-locking are no longer the same set of
// sections: a ReadOnly message shows every field it has and still must not be
// edited. So there are two predicates, each asking the CommsSection question
// that matches it, and a caller must pick the one its control actually needs —
// isChannelConcealed to withhold a VALUE, isChannelEditLocked to grey out a
// CONTROL. Using the concealment one to drive a control is the bug this split
// exists to make impossible: it would leave a Read Only message's channels
// editable, and changing a channel's base resolution silently changes what that
// message decodes to, with nothing on screen to explain the new numbers.
//
// Names are compared case-insensitively, matching renameChannelReferences() and
// the allocated/generated listings. A document where "Engine RPM" and
// "engine rpm" were meant to be different channels would have come apart long
// before it reached here.
bool Configuration::isChannelConcealed(const QString &channelName) const
{
    if (channelName.isEmpty())
        return false;
    // There is deliberately no commsRevealed() fast path, however tempting one is
    // for a predicate table models call per row on every keystroke. The
    // document-wide flag is true for any document with no Protected Comms
    // password, so the shortcut un-concealed every channel of every Hidden
    // section guarded by its own password. Correctness first; isSectionRevealed()
    // is a switch and a hash lookup, and the walk is over three buses.
    for (int b = 0; b < 3; ++b)
        for (const CommsSection &s : bus[b].sections)
            if (s.isConcealed(isSectionRevealed(s, b)) && s.mentionsChannel(channelName))
                return true;
    return false;
}

// No document-wide early-out, deliberately, and that is a cost as well as a
// rule. The old isChannelProtected() returned the moment commsRevealed() was
// true, which made it free on most documents; the tiers took that away, because
// commsRevealed() is true for any document with no Protected Comms
// password and says nothing at all about a Read Only or Hidden section holding
// its own. There is no cheaper stand-in either: an edit lock is NOT lifted by
// any password — revealing buys viewing and the right to untick, and the untick
// is what unlocks editing — so even a session holding every password in the
// document must get `true` here, and the walk has to happen.
//
// What was reclaimed instead is the per-call allocation: mentionsChannel()
// answers without building a QStringList, so the walk is comparisons over
// already-resident strings. Callers that ask this once per row of a list still
// want the second half of the fix — hoist editLockedChannelNames() into a set
// outside their loop (see ChannelEditorDialog::rebuild()) rather than paying
// O(rows x locked sections) here.
bool Configuration::isChannelEditLocked(const QString &channelName) const
{
    if (channelName.isEmpty())
        return false;
    for (const auto &b : bus)
        for (const CommsSection &s : b.sections)
            if (s.isEditLocked() && s.mentionsChannel(channelName))
                return true;
    return false;
}

QStringList Configuration::concealedChannelNames() const
{
    QStringList names;
    // Same reason isChannelConcealed() has no fast path: commsRevealed() says
    // nothing about a Hidden section holding its own password.
    for (int b = 0; b < 3; ++b)
        for (const CommsSection &s : bus[b].sections) {
            if (!s.isConcealed(isSectionRevealed(s, b)))
                continue;
            for (const QString &n : s.channelNames())
                if (!n.isEmpty() && !names.contains(n, Qt::CaseInsensitive))
                    names.append(n);
        }
    return names;
}

QStringList Configuration::editLockedChannelNames() const
{
    QStringList names;
    for (const auto &b : bus)
        for (const CommsSection &s : b.sections) {
            if (!s.isEditLocked())
                continue;
            for (const QString &n : s.channelNames())
                if (!n.isEmpty() && !names.contains(n, Qt::CaseInsensitive))
                    names.append(n);
        }
    return names;
}

// What applyBusSections calls "the same message": the three fields a user would
// point at to say two rows in the sections list are one message seen twice.
//
// The NAME IS LAST in the joined string, deliberately. Put it first and a name
// containing the separator forges an identity — "Engine|100" at address 0x200
// would key identically to "Engine" at 0x100 followed by a device kind of 200 —
// and a forged identity here pairs a proposed section with a prior record that
// is not its own, which is the whole failure this key exists to stop. With the
// two numbers in front, everything after the second separator is the name and no
// name can be misread as anything else.
static QString sectionPairingKey(const CommsSection &s)
{
    return QStringLiteral("%1|%2|%3")
        .arg(s.baseAddress)
        .arg(int(s.device))
        .arg(s.name.toLower());
}

// The same message WITH THE NAME TAKEN OUT: the CAN ID, the direction, and the
// channels it carries. This is what pairs a section whose NAME has changed, and
// it exists because the name-only fallback below cannot: rename a marked section
// and untick it in the same commit and it paired with nothing, so the chokepoint
// read the change as a removal plus an addition and waved it through. Confirmed
// at Read Only, Hidden and Protect Communication, keyed and keyless.
//
// The CAN ID is IN the key, which is what keeps this from swallowing legitimate
// work. Removal is permitted at every tier, so "remove this message and add a
// different one" must stay possible inside one commit; a genuinely different
// message is at a different address, carries different channels, or both, and
// pairs with nothing here exactly as it should. What it will not let you say is
// "this message, at this address, carrying these channels, is a NEW one because
// I typed a new name over it".
//
// Channel names are written LENGTH-FIRST for the reason sectionPairingKey puts
// the numbers first: they are user text, they may contain any separator this
// function could choose, and a forged boundary here pairs a proposed section with
// a prior record that is not its own — which is the failure the key exists to
// stop, arrived at from the other direction.
static QString sectionBodyKey(const CommsSection &s)
{
    QString key = QStringLiteral("%1|%2").arg(s.baseAddress).arg(int(s.device));
    for (const QString &n : s.channelNames()) {
        const QString lower = n.toLower();
        key += QStringLiteral("|%1:%2").arg(lower.size()).arg(lower);
    }
    return key;
}

// ---- the untick rule, enforced in exactly one place ----
//
// Every writer that can replace a bus's sections goes through here: the section
// editor's write-back, the communications dialog's per-section overwrite and
// its whole-bus commit on OK, New, Remove, Remove All, DBC import, and the Lua
// bindings. The invariant is that a section that is STILL PRESENT may not be
// HANDED TO SOMEBODY ELSE unless the challenge THAT SECTION's tier demands has
// been met for it, and there are two ways to hand it over:
//
//   its tier LOWERED, which is the untick this has always refused; and
//   its messageKey REPLACED, which is the same act by the other route and used
//   to be free. The tier never moves, so the untick guard never fired — but the
//   thing the guard protects was gone: a Read Only section conceals nothing, so
//   its editor opens with no challenge at all, and typing anything into Message
//   Password made the message yours. The next trip untucks Read Only with the
//   NEW password. Changing the lock is exactly as privileged as removing it,
//   because both end with somebody else holding the message.
//
// Raising is always free, and removing is always allowed — all three tiers
// permit removal, stated three times in the spec. SETTING a first password on a
// section that had none is free too: there was no prior owner to displace.
//
// The check is PER SECTION and runs on every document. It used to be gated by a
// document-wide `if (!mayLowerProtection())`, which was commsRevealed(), which
// is true for any document with no Protected Comms password — so on the
// documents that use per-section passwords the chokepoint did nothing whatever
// and the untick rule lived entirely in SectionEditorDialog's checkbox handler.
//
// It is here, on the model, and NOT on QCheckBox::toggled, which is where it
// looks like it belongs and where it would enforce nothing: the bulk bus commit,
// the whole-section overwrite, DBC import, a Get and the Lua bindings all reach
// the document without any checkbox being touched. A guard on the widget is a
// guard on one of thirteen paths.
//
// Matched by NAME FIRST, and then by the message's BODY. The name is the one
// field a concealed section still shows, so it is the handle a person would use:
// a name that vanished was REMOVED, which is permitted; a name that is new was
// ADDED, which is permitted; a name that persisted with a lower tier is the
// untick, and it is refused.
//
// The name ALONE was not enough, and the reasoning that said it was is worth
// writing down because it was nearly right. It ran: a concealed section cannot be
// renamed, because its editor will not open for it, so a marked section and a
// renamed section are disjoint. That is true of Hidden and Protect Communication
// and FALSE OF READ ONLY, which is a marked tier that conceals nothing and whose
// editor opens for anybody — and the premise was never needed for Read Only in
// the first place, since a caller reaching this function is not obliged to have
// come through an editor at all. Rename and untick in one proposed list and the
// section paired with nothing, so this read a removal plus an addition and
// allowed it. No path in the shipped UI does that today; the contract of this
// function is that anything which can replace sections routes through it, which
// is a promise about callers that do not exist yet. See sectionBodyKey.
//
// Remove-and-recreate under the same name AND the same address AND the same
// channels is now the only shape that gets through, and that is not a hole this
// can close — removal is permitted by spec, so the sequence is always available
// across two commits whatever this function does. For Hidden and Protected it
// DESTROYS rather than reveals, because the operator cannot see what to retype.
// For Read Only it is a genuine three-click bypass, which is why Read Only must
// be described as accident prevention and never as security.
//
// Two sections may legitimately share a name, so the `before` side is a LIST per
// name — the same shape device_mapper.cpp uses to pair its per-identity
// snapshots, and for the same reason. A single-entry map let one of a duplicate
// pair stand in for the other: with two "Engine Data" sections, whichever the
// QHash happened to keep answered for both, so lowering the guarded one was
// authorised by the unguarded one's tier.
//
// Which entry of that list a proposed section pairs with is decided by IDENTITY
// first — name, base address and device kind — then by the name alone, then by
// the body alone, and only then by document order.
// Order alone was a shipped regression: it is a property of POSITION, so a pure
// REORDER of two same-named sections handed each of them the other's prior
// record, and the model saw two messages trading passwords where the user had
// changed nothing. "Move Up on the second of two 'Secret' sections, then OK"
// came back refused with a message about changing a Message Password.
//
// Document order is still the fallback and has to be: two sections identical on
// all three are indistinguishable to anyone reading the sections list, so their
// position is the only pairing information that exists. It is also what keeps
// the duplicate-name guard above working, since a renamed or renumbered section
// has no identity match to find.
//
// Anything added later that can replace sections — an undo stack, a Duplicate
// Section command, a scripting API — must route through here too.
bool Configuration::applyBusSections(int busIndex, const QList<CommsSection> &next,
                                     QString *refusal)
{
    if (busIndex < 0 || busIndex > 2) {
        if (refusal)
            *refusal = QStringLiteral("Internal error: no such bus.");
        return false;
    }
    // Case-insensitive to match every other name comparison in this file. The
    // WHOLE prior section is kept, not just its tier: maySectionLower() has to
    // ask about the section AS IT STANDS IN THE DOCUMENT — its messageKey and
    // its tier — because `next` is the proposed replacement and its tier is
    // already the lowered one. Asking the incoming copy would let a caller that
    // also cleared messageKey authorise its own untick.
    struct Prior {
        const CommsSection *section;
        bool consumed;
    };
    QList<Prior> priors;
    QHash<QString, QList<int>> byIdentity; // name + address + kind -> priors, doc order
    QHash<QString, QList<int>> byName;     // lower-cased name only  -> priors, doc order
    QHash<QString, QList<int>> byBody;     // address + kind + channels, no name
    for (const CommsSection &s : bus[busIndex].sections) {
        priors.append(Prior{&s, false});
        // An UNNAMED prior is still indexed — by body, the one handle that does
        // not need a name. It used to be skipped outright on the reasoning that
        // there was "nothing for a name match to be about", and that left an
        // untick of an unnamed marked section pairing with nothing and reading as
        // an ADDITION: tier cleared, key gone, nothing proved, at all three tiers.
        // It is the rename-laundering shape the body index exists to stop,
        // reached by deleting the name instead of changing it.
        //
        // Narrow to reach — the editor auto-names an empty field and a Get names
        // every section it rebuilds, so only a hand-edited file arrives here. That
        // is exactly how latent the rename case was, and the argument is the same
        // one: this chokepoint refuses on its own, for callers that never came
        // through an editor. Skipping a record over how it is NAMED is the
        // chokepoint trusting its callers again.
        if (!s.name.isEmpty()) {
            byIdentity[sectionPairingKey(s)].append(priors.size() - 1);
            byName[s.name.toLower()].append(priors.size() - 1);
        }
        byBody[sectionBodyKey(s)].append(priors.size() - 1);
    }
    // The first prior in `index` under `key` that nothing has paired with yet,
    // consumed on the way out — so one prior record can answer for exactly one
    // proposed section however many times its name or its identity repeats.
    // Entries are popped from the list they were found in and merely SKIPPED in
    // the other, which is what lets the two indexes share one pool of records.
    const auto takeFrom = [&priors](QHash<QString, QList<int>> &index, const QString &key) {
        const auto it = index.find(key);
        if (it == index.end())
            return -1;
        while (!it->isEmpty()) {
            const int idx = it->takeFirst();
            if (!priors[idx].consumed) {
                priors[idx].consumed = true;
                return idx;
            }
        }
        return -1;
    };
    // THREE passes, and they cannot be folded into one. A single pass in `next`
    // order lets a section that has merely been RENUMBERED — no identity match,
    // so it falls to the name — consume the very record a later section would
    // have matched exactly, which puts the reorder bug back in a smaller shape.
    // Every identity match is settled first; the name then divides up what is
    // left, and the body divides up what the name could not.
    //
    // The BODY pass is last because it is the only handle the user cannot see. A
    // section that still answers to its own name has said what it is; one that
    // matches only on its address and its channels is being recognised in spite of
    // what it now calls itself, which is the right answer for a rename and the
    // wrong one to reach for while a plain name match is still available.
    QList<int> pairing;
    pairing.reserve(next.size());
    for (const CommsSection &s : next)
        pairing.append(s.name.isEmpty() ? -1 : takeFrom(byIdentity, sectionPairingKey(s)));
    for (int i = 0; i < next.size(); ++i)
        if (pairing[i] < 0 && !next.at(i).name.isEmpty())
            pairing[i] = takeFrom(byName, next.at(i).name.toLower());
    // The body pass, deliberately WITHOUT the name guard the two above carry.
    // Requiring a name here is what let an unnamed section launder an untick: the
    // body key is precisely the handle that does not need one, so gating it on a
    // name discards the only match an unnamed record could ever make.
    for (int i = 0; i < next.size(); ++i)
        if (pairing[i] < 0)
            pairing[i] = takeFrom(byBody, sectionBodyKey(next.at(i)));

    for (int i = 0; i < next.size(); ++i) {
        const CommsSection &s = next.at(i);
        if (pairing[i] < 0)
            continue; // unnamed, a new name, or one more than the document had
        const CommsSection &prior = *priors.at(pairing[i]).section;
        const bool tierLowered = s.protection < prior.protection;
        // A password REPLACED, which hands the section over just as completely.
        // Only when there was one to replace: a section that arrived keyless has
        // no prior owner, and every section a Get produces arrives keyless, so
        // demanding a proof here would make "set a password after a Get" — the
        // one action that FIXES a keyless section — the thing this refuses.
        const bool keyReplaced =
            prior.messageKey != kNoAccessKey && s.messageKey != prior.messageKey;
        if (!tierLowered && !keyReplaced)
            continue; // unchanged, or raised, or newly given a password — always free
        // Whichever challenges THIS section's tier demands — see
        // proofsRequiredFor(): its own password for Read Only and Hidden, and for
        // Protected that password AND an Protected Comms proof against a
        // live device. The section editor and the comms dialog run them and
        // record the result via grantSectionAccess. A section with no password of
        // its own answers NO here — nothing exists that could authorise giving it
        // away — and the way out is to give it a first password, which is free
        // above, or to remove it, which is free at every tier.
        //
        // The BUS goes in. This caller knows which one it is replacing, so the
        // grant that answers must be a grant taken on that bus; leaving it out
        // would let a grant for a same-named section on another bus authorise
        // this untick whenever the two genuinely share a password.
        //
        // maySectionLower() is asked for BOTH kinds of hand-over, and asked about
        // the section AS IT STANDS IN THE DOCUMENT: `next` is the proposal, and
        // its tier is already the lowered one and its key already the new one, so
        // asking the incoming copy would let a caller authorise its own change.
        //
        // The grant is checked against the PRIOR section's name, and now that a
        // rename can pair (see sectionBodyKey) that is the whole of why `prior` is
        // the right thing to hand over rather than a detail of it. The challenge
        // was answered for the name and the key the document was holding at the
        // time; a rename in the same commit does not change what was proved, and
        // asking about the proposal would let the new name decide whether the old
        // one had been unlocked.
        if (maySectionLower(prior, busIndex))
            continue;
        if (refusal) {
            // A KEYLESS prior gets its own wording, and it has to. Every message
            // below tells the user to open the section and give a password they
            // do not have and that nobody has: this section arrived without one
            // — from a Get, or from a file written before markings carried
            // passwords — so "type the current password" is advice that cannot be
            // followed.
            //
            // AND IT IS SAID PER TIER, because the way out is not the same at all
            // three and the single message that used to stand here sent two thirds
            // of its readers looking for a door the application refuses to open.
            // It said "give it a Message Password first and untick the marking
            // afterwards". At Read Only that is exactly right and it works end to
            // end: the tier conceals nothing, its editor opens for anybody, a
            // first password is free, and the untick goes through on the next
            // visit. At Hidden and Protect Communication the editor will NOT open
            // — CommunicationsDialog::unlockConcealedSection refuses a concealed
            // section with no password, since nothing exists that could open it —
            // so there is no way to give it one, and the only thing that works is
            // removal.
            //
            // THIS IS A BACKSTOP. Every route to it is refused earlier: the
            // section editor will not close over a marked tier with no password
            // (rule 1), and Communications Setup will not open a concealed keyless
            // section at all. It is worded as if a person will read it because the
            // contract of this function is that future writers arrive here too,
            // and a backstop that lies is worse than no backstop.
            if (prior.messageKey == kNoAccessKey)
                *refusal =
                    prior.protection == CommsProtection::ReadOnly
                        ? QStringLiteral(
                              "\"%1\" arrived without a Message Password — from a configuration "
                              "read back off a device, or from a file written before markings "
                              "carried passwords — so there is no password that could authorise "
                              "unticking Read Only.\n\nOpen it in Communications Setup, give it a "
                              "Message Password and save; unticking Read Only then asks for that "
                              "password on the next visit. Or remove the message — removing is "
                              "allowed at every level.")
                              .arg(s.name)
                        : QStringLiteral(
                              "\"%1\" arrived without a Message Password — from a configuration "
                              "read back off a device, or from a file written before markings "
                              "carried passwords — so there is no password that could authorise "
                              "unmarking it, and none can be given to it either: the message "
                              "conceals itself, and its editor will not open when there is "
                              "nothing to open it with.\n\nThe message can be REMOVED, which is "
                              "allowed at every level, and it can be reordered and sent as it "
                              "stands. The configuration file it was built in still holds its "
                              "password and still opens it.")
                              .arg(s.name);
            else if (tierLowered)
                *refusal = prior.protection == CommsProtection::Protected
                               ? QStringLiteral(
                                     "\"%1\" is marked Protect Communication. Lowering it needs "
                                     "this section's own Message Password AND the Protected Comms "
                                     "password confirmed by a connected CAN Triple — open "
                                     "it in Communications Setup and untick the box there. The "
                                     "section can still be removed without either.")
                                     .arg(s.name)
                               : QStringLiteral(
                                     "\"%1\" is protected. Lowering its protection needs this "
                                     "section's own Message Password — open it in Communications "
                                     "Setup and untick the box there. The section can still be "
                                     "removed without it.")
                                     .arg(s.name);
            else
                *refusal =
                    prior.protection == CommsProtection::Protected
                        ? QStringLiteral(
                              "\"%1\" is marked Protect Communication. Changing its Message "
                              "Password needs the CURRENT one AND the Protected Comms "
                              "password confirmed by a connected CAN Triple — open it in "
                              "Communications Setup and change it there. The section can still "
                              "be removed without either.")
                              .arg(s.name)
                        : QStringLiteral(
                              "\"%1\" is protected. Changing its Message Password needs the "
                              "CURRENT one — open it in Communications Setup and change it "
                              "there. The section can still be removed without it.")
                              .arg(s.name);
        }
        return false;
    }
    // A RENAME REPOINTS WHAT NAMED THE OLD NAME. `pairing` already says which
    // incoming section is which prior one — it is computed above for the
    // protection rules, and identity/name/body is a far better answer than
    // position — so a name that changed between a pair is a rename, and
    // conditions, timer triggers and counter inputs that keyed off it follow.
    //
    // Message references are held by name and bus because there is no stable id
    // to hold instead, so without this they simply stopped resolving: the row
    // still said "when EngineData is received" for a message that no longer
    // existed, Check Channels said nothing for timers, and the first report was
    // the Send refusing the whole configuration.
    //
    // DONE HERE, past every refusal above, so a rename that is about to be
    // rejected does not repoint anything. And done BEFORE the assignment on the
    // next line, because `priors` holds pointers INTO the list that assignment
    // replaces — reading a prior name afterwards would read freed memory.
    for (int i = 0; i < next.size(); ++i) {
        if (pairing[i] < 0)
            continue;
        const QString &was = priors.at(pairing[i]).section->name;
        if (!was.isEmpty() && was != next.at(i).name)
            renameMessageReferences(busIndex + 1, was, next.at(i).name);
    }
    bus[busIndex].sections = next;
    setDirty();
    return true;
}

QStringList Configuration::allocatedChannelNames() const
{
    QStringList names;
    for (const auto &b : bus)
        for (const CommsSection &s : b.sections)
            for (const QString &n : s.channelNames())
                if (!names.contains(n, Qt::CaseInsensitive))
                    names.append(n);
    return names;
}

QStringList Configuration::generatedChannelNames() const
{
    QStringList names;
    // Device channels are always generated — by the firmware, not by anything
    // in this document. Listing them here is what stops every reference to
    // Device OnTime being reported as "nothing writes this, it reads its
    // default value", and what makes a document row that ALSO writes the name
    // show up as the duplicate writer it is.
    for (const Channel &c : ChannelCatalog::deviceChannels())
        names.append(c.name);
    for (const auto &b : bus)
        for (const CommsSection &s : b.sections) {
            if (s.isReceive())
                for (const QString &n : s.channelNames())
                    if (!names.contains(n, Qt::CaseInsensitive))
                        names.append(n);
            // A Transmit CRC8 section PUBLISHES its computed checksum to a
            // channel — the device writes that slot every time the message
            // composes, so the channel is generated exactly like a receive
            // channel is, and omitting it here would report every reference to
            // it as "nothing writes this".
            if (s.isCrc8() && !s.crcChannel.isEmpty()
                && !names.contains(s.crcChannel, Qt::CaseInsensitive))
                names.append(s.crcChannel);
        }
    for (const MathRow &m : mathRows)
        if (m.active && !m.destChannel.isEmpty() && !names.contains(m.destChannel, Qt::CaseInsensitive))
            names.append(m.destChannel);
    for (const ConditionRow &c : conditionRows)
        if (c.active && !c.outputChannel.isEmpty()
            && !names.contains(c.outputChannel, Qt::CaseInsensitive))
            names.append(c.outputChannel);
    for (const CounterRow &c : counterRows)
        if (c.active && !c.outputChannel.isEmpty()
            && !names.contains(c.outputChannel, Qt::CaseInsensitive))
            names.append(c.outputChannel);
    for (const TimerRow &t : timerRows)
        if (t.active && !t.outputChannel.isEmpty()
            && !names.contains(t.outputChannel, Qt::CaseInsensitive))
            names.append(t.outputChannel);
    for (const IntegratorRow &g : integratorRows)
        if (g.active && !g.outputChannel.isEmpty()
            && !names.contains(g.outputChannel, Qt::CaseInsensitive))
            names.append(g.outputChannel);
    for (const ConstantRow &c : constantRows)
        if (c.active && !c.name.isEmpty() && !names.contains(c.name, Qt::CaseInsensitive))
            names.append(c.name);
    // A table only generates its output when it has sites — the mapper skips an
    // empty table, so an empty table's output is NOT generated on the device.
    for (const Table2x16Row &t : table2x16Rows)
        if (t.active && !t.xSites.isEmpty() && !t.outputChannel.isEmpty()
            && !names.contains(t.outputChannel, Qt::CaseInsensitive))
            names.append(t.outputChannel);
    for (const Table8x8Row &t : table8x8Rows)
        if (t.active && !t.xSites.isEmpty() && !t.ySites.isEmpty() && !t.outputChannel.isEmpty()
            && !names.contains(t.outputChannel, Qt::CaseInsensitive))
            names.append(t.outputChannel);
    return names;
}

} // namespace ct
