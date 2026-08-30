#include "dbc_import.h"

#include <QHash>
#include <QPair>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>

namespace ct {

bool DbcMessage::hasMultiplexing() const
{
    for (const DbcSignal &s : signalList)
        if (s.isMultiplexor || s.isMultiplexed)
            return true;
    return false;
}

const DbcSignal *DbcMessage::multiplexor() const
{
    for (const DbcSignal &s : signalList)
        if (s.isMultiplexor)
            return &s;
    return nullptr;
}

// ------------------------------------------------------------------ parsing

DbcFile parseDbc(const QString &text, QStringList *warnings)
{
    DbcFile file;

    // BO_ <id> <Name>: <dlc> <Transmitter>
    static const QRegularExpression boRe(
        QStringLiteral(R"(^\s*BO_\s+(\d+)\s+([A-Za-z_]\w*)\s*:\s*(\d+)\s+(\S+))"));
    // SG_ <Name> [M|m<n>] : <start>|<len>@<order><sign> (<factor>,<offset>) [<min>|<max>] "<unit>" ...
    static const QRegularExpression sgRe(QStringLiteral(
        R"RX(^\s*SG_\s+([A-Za-z_]\w*)\s*(M|m\d+)?\s*:\s*(\d+)\|(\d+)@([01])([-+])\s*\(([^,]+),([^)]+)\)\s*\[([^|]*)\|([^\]]*)\]\s*"([^"]*)")RX"));
    static const QRegularExpression versionRe(QStringLiteral(R"RX(^\s*VERSION\s+"([^"]*)")RX"));
    // SIG_VALTYPE_ <msgId> <SigName> : <1|2>;  (1 = IEEE754 float, 2 = double).
    // The SG_ line of a float signal still reads @1+ / @0+, so without this
    // marker a float imports as an integer and decodes to garbage on-device.
    static const QRegularExpression valTypeRe(QStringLiteral(
        R"(^\s*SIG_VALTYPE_\s+(\d+)\s+([A-Za-z_]\w*)\s*:\s*(\d+))"));

    // Track the current message by index, not pointer: appending another
    // message can reallocate the QList and dangle a cached pointer.
    int currentIdx = -1;
    const QStringList lines = text.split(QRegularExpression(QStringLiteral("\r\n|\n|\r")));
    for (const QString &line : lines) {
        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty())
            continue;

        if (file.version.isEmpty()) {
            const auto vm = versionRe.match(line);
            if (vm.hasMatch()) {
                file.version = vm.captured(1);
                continue;
            }
        }

        const auto bo = boRe.match(line);
        if (bo.hasMatch()) {
            DbcMessage msg;
            msg.rawId = bo.captured(1).toUInt();
            msg.name = bo.captured(2);
            msg.dlc = bo.captured(3).toInt();
            msg.transmitter = bo.captured(4);
            // Bit 31 flags an extended id in some tools; otherwise anything
            // above the 11-bit range is extended by definition.
            msg.extended = (msg.rawId & 0x80000000u) || ((msg.rawId & 0x1FFFFFFFu) > 0x7FFu);
            msg.canId = msg.rawId & 0x1FFFFFFFu;
            file.messages.append(msg);
            currentIdx = file.messages.size() - 1;
            continue;
        }

        if (trimmed.startsWith(QStringLiteral("SG_"))) {
            const auto sg = sgRe.match(line);
            if (!sg.hasMatch()) {
                if (warnings)
                    warnings->append(QStringLiteral("Could not parse signal line: %1").arg(trimmed));
                continue;
            }
            if (currentIdx < 0) {
                if (warnings)
                    warnings->append(QStringLiteral("Signal '%1' outside any message — skipped")
                                         .arg(sg.captured(1)));
                continue;
            }
            DbcSignal s;
            s.name = sg.captured(1);
            const QString mux = sg.captured(2);
            if (mux == QLatin1String("M")) {
                s.isMultiplexor = true;
            } else if (mux.startsWith(QLatin1Char('m'))) {
                s.isMultiplexed = true;
                s.muxValue = mux.mid(1).toInt();
            }
            s.startBit = sg.captured(3).toInt();
            s.bitLength = sg.captured(4).toInt();
            s.bigEndian = sg.captured(5) == QLatin1Char('0'); // @0 = Motorola
            s.isSigned = sg.captured(6) == QLatin1Char('-');
            s.factor = sg.captured(7).toDouble();
            s.offset = sg.captured(8).toDouble();
            s.minValue = sg.captured(9).toDouble();
            s.maxValue = sg.captured(10).toDouble();
            s.unit = sg.captured(11);
            file.messages[currentIdx].signalList.append(s);
            continue;
        }

        // SIG_VALTYPE_ lines come after the BO_/SG_ blocks, so every signal
        // they reference is already parsed.
        if (trimmed.startsWith(QStringLiteral("SIG_VALTYPE_"))) {
            const auto vt = valTypeRe.match(line);
            if (!vt.hasMatch())
                continue;
            const quint32 rawId = vt.captured(1).toUInt();
            const QString sigName = vt.captured(2);
            const int type = vt.captured(3).toInt();
            DbcSignal *found = nullptr;
            for (DbcMessage &msg : file.messages) {
                if (msg.rawId != rawId && msg.canId != (rawId & 0x1FFFFFFFu))
                    continue;
                for (DbcSignal &s : msg.signalList)
                    if (s.name == sigName) {
                        found = &s;
                        break;
                    }
                if (found)
                    break;
            }
            if (!found) {
                if (warnings)
                    warnings->append(
                        QStringLiteral("SIG_VALTYPE_ for unknown signal '%1' (message %2) — ignored")
                            .arg(sigName).arg(rawId));
                continue;
            }
            found->valueType = type;
            if (warnings && type == 2)
                warnings->append(
                    QStringLiteral("Signal '%1' is a 64-bit IEEE754 double — the device only "
                                   "decodes 32-bit floats, so it will import as a raw integer "
                                   "and produce wrong values")
                        .arg(sigName));
            else if (warnings && type == 1 && found->bitLength != 32)
                warnings->append(
                    QStringLiteral("Float signal '%1' has bit length %2 — IEEE754 floats must "
                                   "be 32 bits; the length is forced to 32")
                        .arg(sigName).arg(found->bitLength));
            continue;
        }
    }

    // Order the messages by arbitration id, lowest first, which is the order a
    // bus arbitrates in and the order every other CAN tool lists them in. A DBC
    // file has no required message order — they come out in whatever order the
    // authoring tool wrote them, which is often neither id nor name — so
    // importing verbatim scatters related ids through the list and makes the
    // import dialog tedious to check against a spec sheet.
    //
    // Done HERE, at the end of the parse, rather than in the import dialog, for
    // two reasons: every consumer inherits one order, and the message indices
    // that the dialog stores in its tree items are handed out downstream of this
    // point, so they are indices into the sorted list from the outset. Sorting
    // during the loop would break `currentIdx`, which is what attaches each SG_
    // line to the message above it.
    //
    // Stable, and on the masked canId rather than rawId: rawId carries the
    // extended-frame flag in bit 31, so sorting on it would file every extended
    // message after every standard one instead of by the id itself. Two
    // messages sharing an id (a malformed file, or the same id standard and
    // extended) keep their file order.
    std::stable_sort(file.messages.begin(), file.messages.end(),
                     [](const DbcMessage &a, const DbcMessage &b) {
                         return a.canId < b.canId;
                     });
    return file;
}

// -------------------------------------------------------------- conversion

int dbcStartBitToLsb(int dbcStartBit, int bitLength, bool bigEndian)
{
    if (!bigEndian)
        return dbcStartBit; // Intel: the DBC start bit is already the LSB
    // Motorola: the DBC start bit is the MSB. Walk toward the LSB — within a
    // byte the next-less-significant bit is bitIndex-1; crossing below bit 0
    // wraps to bit 7 of the NEXT byte (higher index). After bitLength-1 steps
    // we are at the signal's LSB, which is the app's start bit.
    int byteIndex = dbcStartBit / 8;
    int bitIndex = dbcStartBit % 8;
    for (int i = 1; i < bitLength; ++i) {
        if (bitIndex == 0) {
            bitIndex = 7;
            byteIndex += 1;
        } else {
            bitIndex -= 1;
        }
    }
    return byteIndex * 8 + bitIndex;
}

SectionAlignment alignmentForDbcSignal(const DbcSignal &sig)
{
    return sig.bigEndian ? SectionAlignment::Normal : SectionAlignment::WordSwap;
}

CommsChannelRow rowFromDbcSignal(const DbcSignal &sig, const QString &channelName)
{
    CommsChannelRow row;
    row.channelName = channelName;
    row.startBit = dbcStartBitToLsb(sig.startBit, sig.bitLength, sig.bigEndian);
    row.bitLength = sig.valueType == 1 ? 32 : sig.bitLength;
    row.dbcType = sig.valueType == 1 ? int(DbcType::IEEE754)
                  : sig.isSigned     ? int(DbcType::Signed)
                                     : int(DbcType::Unsigned);
    row.dbcFactor = sig.factor;
    row.dbcOffset = sig.offset;
    row.defaultValue = 0.0;
    return row;
}

// Decimal places implied by a scaling factor (0.1 -> 1, 0.05 -> 2, 1 -> 0).
static int decimalsForFactor(double factor)
{
    double f = std::abs(factor);
    if (f == 0.0)
        return 0;
    int d = 0;
    while (d < 8 && std::abs(f - std::round(f)) > 1e-9 * std::max(1.0, std::abs(f))) {
        f *= 10.0;
        ++d;
    }
    return d;
}

// Storage types a channel can use, smallest first, mirroring the kDataTypes
// table the channel dialogs derive their ranges from (edit_channel_dialog.cpp,
// constants_dialog.cpp, tables_dialog.cpp). An integer channel holds its
// physical value as a scaled integer, so the range it can represent is
// rawRange x 10^-decimalPlaces — which is why the type has to be chosen from
// the PHYSICAL range and the decimals together, never from the raw bit width.
namespace {
struct StorageType {
    const char *name;
    double rawMin;
    double rawMax;
    int maxDecimals; // the dialogs cap Decimal Places per type
};
// The "float" channel's span, matching the kDataTypes tables in the dialogs.
constexpr double kFloatChannelMin = -1e9;
constexpr double kFloatChannelMax = 1e9;
const StorageType kStorageTypes[] = {
    {"u8", 0.0, 255.0, 2},
    {"s8", -128.0, 127.0, 2},
    {"u16", 0.0, 65535.0, 4},
    {"s16", -32768.0, 32767.0, 4},
    {"u32", 0.0, 4294967295.0, 8},
    {"s32", -2147483648.0, 2147483647.0, 8},
};
} // namespace

// Smallest storage type that can represent every value in [lo, hi] at the given
// decimal precision; "float" when no integer type can. Guarantees fitment: a
// 16-bit raw signal scaled by 0.036 spans 0..2359.26, which needs u32 at 3 dp
// (u32 reaches 4294967.295) — u16 would top out at 65.535 and the firmware
// would clamp every real reading to that.
QString storageTypeForRange(double lo, double hi, int decimals)
{
    // Compare the ROUNDED integers — those are exactly what a scaled-integer
    // channel stores, so the test needs no fudge factor. (pow(10, n) is exact
    // for n <= 8, and rounding absorbs the multiply's last-bit error, which a
    // relative tolerance would over-approximate by orders of magnitude.)
    const double scale = std::pow(10.0, decimals);
    const double needLo = std::round(lo * scale);
    const double needHi = std::round(hi * scale);
    for (const StorageType &t : kStorageTypes) {
        if (decimals > t.maxDecimals)
            continue; // the channel editor would clamp the precision away
        if (needLo < t.rawMin || needHi > t.rawMax)
            continue;
        return QLatin1String(t.name);
    }
    return QStringLiteral("float");
}

bool storageTypeHoldsRange(const QString &dataType, double lo, double hi, int decimals)
{
    if (dataType.isEmpty())
        return true; // type not chosen yet — nothing to contradict
    if (dataType == QLatin1String("float"))
        return true; // stored as float32 on the wire — no scaled-integer ceiling
    if (dataType == QLatin1String("boolean"))
        return decimals == 0 && lo >= 0.0 && hi <= 1.0;
    const double scale = std::pow(10.0, decimals);
    const double needLo = std::round(lo * scale);
    const double needHi = std::round(hi * scale);
    for (const StorageType &t : kStorageTypes) {
        if (dataType != QLatin1String(t.name))
            continue;
        return decimals <= t.maxDecimals && needLo >= t.rawMin && needHi <= t.rawMax;
    }
    return true; // unknown type name — don't cry wolf
}

// The physical span an INTEGER-CODED signal can carry: both raw endpoints
// through physical = raw x factor + offset. A negative factor flips the
// ordering, and a negative offset can push an UNSIGNED signal negative (the
// classic temperature-with-offset-40 case), which is what decides signedness —
// not the DBC's raw sign flag. Not meaningful for an IEEE754 signal, whose raw
// bits are the value rather than an integer to scale.
void dbcPhysicalRange(const DbcSignal &sig, double *lo, double *hi)
{
    const int n = std::min(std::max(sig.bitLength, 1), 64);
    double rawLo, rawHi;
    if (sig.isSigned) {
        rawLo = -std::pow(2.0, n - 1);
        rawHi = std::pow(2.0, n - 1) - 1.0;
    } else {
        rawLo = 0.0;
        rawHi = std::pow(2.0, n) - 1.0;
    }
    const double a = rawLo * sig.factor + sig.offset;
    const double b = rawHi * sig.factor + sig.offset;
    *lo = std::min(a, b);
    *hi = std::max(a, b);
}

QString channelNameFromDbcSignal(const QString &signalName)
{
    QString name = signalName;
    name.replace(QLatin1Char('_'), QLatin1Char(' '));
    return name.simplified();
}

Channel channelFromDbcSignal(const DbcSignal &sig, const QString &channelName)
{
    Channel c;
    c.name = channelName;
    // The catalogue's spelling, not the DBC's. Importing "degC" verbatim gave
    // channels a unit no list offers, which then matched no other channel and
    // had to be retyped; dbcUnitFor answers with a unit the app actually has.
    // The import dialog overwrites both from its own columns anyway - this is
    // the answer for any caller that does not.
    const DbcUnit picked = dbcUnitFor(sig.unit);
    c.unit = picked.unit;
    c.quantity = picked.quantity;
    c.decimalPlaces = decimalsForFactor(sig.factor);
    c.baseResolution = std::pow(10.0, -c.decimalPlaces);

    // An IEEE754 signal's raw bits ARE the value, so there is no integer span to
    // push through factor/offset and no scaled-integer storage to size.
    const bool ieee = sig.valueType == 1;

    // The range the channel must hold. A DBC's declared [min|max] is the
    // author's intent and wins when it is a real span; otherwise fall back to
    // everything the field can physically encode (for a float signal, the app's
    // float span). Either way it is a concrete range — the mapper copies it into
    // the signal's min_val/max_val and the firmware CLAMPS every reading to it.
    if (sig.maxValue > sig.minValue) {
        c.minValue = sig.minValue;
        c.maxValue = sig.maxValue;
    } else if (ieee) {
        c.minValue = kFloatChannelMin;
        c.maxValue = kFloatChannelMax;
    } else {
        dbcPhysicalRange(sig, &c.minValue, &c.maxValue);
    }

    // Size the storage to that range at the precision the factor needs, rather
    // than to the raw bit width: the two are unrelated once a factor is
    // involved, and picking by width silently truncates the channel's range.
    if (ieee) {
        c.dataType = QStringLiteral("float");
    } else {
        c.dataType = storageTypeForRange(c.minValue, c.maxValue, c.decimalPlaces);
        // A 1-bit field that really is a flag stays a boolean.
        if (sig.bitLength <= 1 && c.decimalPlaces == 0 && c.minValue >= 0.0 && c.maxValue <= 1.0)
            c.dataType = QStringLiteral("boolean");
    }

    // NOTE: the range is deliberately NOT trimmed to the dialogs' nominal float
    // span (+/-1e9). That number is a UI display convention, not a storage
    // limit — min_val/max_val go to the device as float32 (reach ~3.4e38) and
    // become its clamp. Trimming would silently discard most of a wide signal's
    // range, which is the very bug this sizing work exists to remove: J1939's
    // High Resolution Total Vehicle Distance (32-bit, 5 m/bit) legitimately
    // spans 0..2.1e10, and capping it at 1e9 throws away ~95% of it.
    c.category = QStringLiteral("User Channels");
    c.userDefined = true;
    return c;
}

bool muxSelectorForValue(const DbcSignal &mux, int value, int *byteOffset, quint32 *id,
                         quint32 *idMask, QString *reason, int messageLenBytes)
{
    auto fail = [&](const QString &why) {
        if (reason)
            *reason = why;
        return false;
    };
    const int len = mux.bitLength;
    // v15: the device's selector window is 2 bytes (mux_id/mux_mask are 16-bit),
    // so a multiplexor field must fit within 16 bits of its byte offset.
    if (len < 1 || len > 16)
        return fail(QStringLiteral("multiplexor width %1 is out of range (1–16)").arg(len));
    // The firmware reads the selector window anywhere inside the frame, so the
    // offset limit is the message length (FD frames go up to 64 bytes).
    const int maxOffset = qBound(1, messageLenBytes, 64) - 1;

    // The device selector reads an up-to-4-byte LITTLE-ENDIAN window at
    // byteOffset and matches (window & idMask) == (id & idMask). Express the
    // multiplexor's bit field within that window.
    if (!mux.bigEndian) {
        const int lsb = mux.startBit;         // Intel: start bit is the LSB
        const int base = lsb / 8;
        const int bitInWindow = lsb % 8;
        if (base < 0 || base > maxOffset)
            return fail(QStringLiteral("multiplexor byte offset %1 is out of range (0–%2)")
                            .arg(base).arg(maxOffset));
        if (bitInWindow + len > 16)
            return fail(QStringLiteral("multiplexor field spans past the 16-bit selector "
                                       "window at byte %1").arg(base));
        const quint32 fieldMask = (1u << len) - 1u;
        *byteOffset = base;
        *idMask = fieldMask << bitInWindow;
        *id = (quint32(value) << bitInWindow) & *idMask;
        return true;
    }
    // Motorola: the multiplexor's MSB is at mux.startBit and the field descends.
    // Only a field confined to a single byte maps cleanly to the LE window.
    const int msbByte = mux.startBit / 8;
    const int msbBit = mux.startBit % 8;
    const int loBit = msbBit - (len - 1);
    if (loBit < 0)
        return fail(QStringLiteral("Motorola multiplexor spans multiple bytes (unsupported)"));
    if (msbByte < 0 || msbByte > maxOffset)
        return fail(QStringLiteral("multiplexor byte offset %1 is out of range (0–%2)")
                        .arg(msbByte).arg(maxOffset));
    const quint32 fieldMask = (1u << len) - 1u;
    *byteOffset = msbByte;
    *idMask = fieldMask << loBit;
    *id = (quint32(value) << loBit) & *idMask;
    return true;
}

// -------------------------------------------------------- unit -> quantity

// Every DBC spelling this import knows, and what it is in the catalogue's terms.
//
// The value is (quantity, unit) rather than just the quantity, which is the
// whole change: knowing "degC" is a Temperature was never enough, because the
// channel still ended up carrying the string "degC" that no unit list offers.
// The second half says which of Temperature's units it IS.
//
// Where the DBC's own spelling is already the catalogue's, the pair simply
// repeats it - "psi" and "bar" and "rpm" are the same word in both - and those
// rows are here rather than special-cased so that one table answers the whole
// question.
static const QHash<QString, QPair<QString, QString>> &dbcUnitTable()
{
    static const QHash<QString, QPair<QString, QString>> map = {
        {"c", {"Temperature", "C"}},       {"°c", {"Temperature", "C"}},
        {"degc", {"Temperature", "C"}},    {"celsius", {"Temperature", "C"}},
        {"f", {"Temperature", "F"}},       {"°f", {"Temperature", "F"}},
        {"degf", {"Temperature", "F"}},    {"k", {"Temperature", "K"}},
        {"kelvin", {"Temperature", "K"}},
        {"kpa", {"Pressure and Stress", "kPa"}},  {"pa", {"Pressure and Stress", "Pa"}},
        {"mpa", {"Pressure and Stress", "MPa"}},  {"bar", {"Pressure and Stress", "bar"}},
        {"mbar", {"Pressure and Stress", "mbar"}}, {"psi", {"Pressure and Stress", "psi"}},
        // hPa and atm have no catalogue unit; mbar is hPa exactly, and atm is
        // named as bar because that is the nearest thing offered. Both are
        // marked ADJUSTED below rather than silently accepted, because the
        // NUMBERS do not convert - only the label changes, and a user who meant
        // atmospheres needs to see that and decide.
        {"hpa", {"Pressure and Stress", "mbar"}}, {"atm", {"Pressure and Stress", "bar"}},
        {"inhg", {"Pressure and Stress", "inHg"}},
        {"mmhg", {"Pressure and Stress", "mmHg"}},
        {"km/h", {"Speed", "km/h"}}, {"kph", {"Speed", "km/h"}}, {"kmh", {"Speed", "km/h"}},
        {"mph", {"Speed", "mile/h"}}, {"mi/h", {"Speed", "mile/h"}},
        {"m/s", {"Speed", "m/s"}}, {"knots", {"Speed", "knots"}}, {"kn", {"Speed", "knots"}},
        {"rpm", {"Rotational Speed", "rpm"}}, {"1/min", {"Rotational Speed", "rpm"}},
        {"rev/min", {"Rotational Speed", "rpm"}}, {"min-1", {"Rotational Speed", "rpm"}},
        {"v", {"Voltage", "V"}}, {"mv", {"Voltage", "mV"}}, {"volt", {"Voltage", "V"}},
        {"volts", {"Voltage", "V"}},
        // kV has no catalogue unit; V is the nearest, and it is a 1000x
        // relabel, so it is ADJUSTED rather than accepted.
        {"kv", {"Voltage", "V"}},
        {"a", {"Current", "A"}}, {"ma", {"Current", "mA"}}, {"amp", {"Current", "A"}},
        {"amps", {"Current", "A"}},
        {"deg", {"Angle", "deg"}}, {"°", {"Angle", "deg"}},
        {"degree", {"Angle", "deg"}}, {"degrees", {"Angle", "deg"}},
        {"rad", {"Angle", "rad"}}, {"radians", {"Angle", "rad"}},
        {"nm", {"Torque", "Nm"}}, {"n.m", {"Torque", "Nm"}},
        {"ftlb", {"Torque", "ftlb"}}, {"lbft", {"Torque", "ftlb"}},
        {"w", {"Power", "W"}}, {"kw", {"Power", "kW"}}, {"hp", {"Power", "hp"}},
        {"watt", {"Power", "W"}},
        // PS (metric horsepower) is not hp, but hp is what is offered.
        {"ps", {"Power", "hp"}},
        {"%", {"Ratio", "%"}}, {"percent", {"Ratio", "%"}}, {"ratio", {"Ratio", "Ratio"}},
        {"s", {"Time", "s"}}, {"sec", {"Time", "s"}}, {"secs", {"Time", "s"}},
        {"ms", {"Time", "ms"}}, {"us", {"Time", "us"}}, {"µs", {"Time", "us"}},
        {"min", {"Time", "min"}}, {"h", {"Time", "h"}}, {"hr", {"Time", "h"}},
        {"hour", {"Time", "h"}},
        {"kg", {"Weight & Force", "kg"}}, {"g", {"Weight & Force", "g"}},
        {"mg", {"Weight & Force", "mg"}}, {"t", {"Weight & Force", "t"}},
        {"n", {"Weight & Force", "N"}}, {"lb", {"Weight & Force", "lb"}},
        {"lbs", {"Weight & Force", "lb"}}, {"oz", {"Weight & Force", "oz"}},
        {"newton", {"Weight & Force", "N"}},
        {"l", {"Volume", "l"}}, {"ml", {"Volume", "ml"}}, {"cc", {"Volume", "cc"}},
        {"cm3", {"Volume", "cc"}}, {"gal", {"Volume", "USgal"}},
        {"gallon", {"Volume", "USgal"}},
        {"l/h", {"Volume Flow", "l/h"}}, {"l/min", {"Volume Flow", "l/min"}},
        {"l/s", {"Volume Flow", "l/s"}}, {"cc/min", {"Volume Flow", "cc/min"}},
        {"cc/s", {"Volume Flow", "cc/s"}},
        {"g/s", {"Mass Flow", "g/s"}}, {"kg/h", {"Mass Flow", "kg/h"}},
        {"kg/s", {"Mass Flow", "kg/s"}}, {"g/min", {"Mass Flow", "g/min"}},
        {"lb/h", {"Mass Flow", "lb/h"}},
        {"ohm", {"Resistance", "ohm"}}, {"ohms", {"Resistance", "ohm"}},
        {"kohm", {"Resistance", "kohm"}},
        {"lambda", {"Air Fuel Ratio", "LA"}}, {"afr", {"Air Fuel Ratio", "A/F"}},
        {"a/f", {"Air Fuel Ratio", "A/F"}},
        {"m/s2", {"Acceleration", "m/s/s"}}, {"m/s^2", {"Acceleration", "m/s/s"}},
        {"m/s²", {"Acceleration", "m/s/s"}}, {"g/s/s", {"Acceleration", "m/s/s"}},
    };
    return map;
}

DbcUnit dbcUnitFor(const QString &rawUnit)
{
    DbcUnit out;
    const QString key = rawUnit.trimmed().toLower();
    if (key.isEmpty()) {
        // No unit is not an unrecognised unit. A DBC signal with "" really is
        // unitless, and flagging every one of them would bury the handful that
        // need a decision under the many that do not.
        out.quantity = QStringLiteral("Unitless");
        out.unit = QString();
        return out;
    }
    const auto it = dbcUnitTable().constFind(key);
    if (it != dbcUnitTable().constEnd()) {
        out.quantity = it->first;
        out.unit = it->second;
        return out;
    }
    // Unplaceable. Unitless with the flag up, so the row is marked in the
    // import panel and the user picks - which is the only honest answer, since
    // a wrong guess here becomes a channel whose unit says something untrue
    // about its numbers.
    out.quantity = QStringLiteral("Unitless");
    out.unit = QString();
    out.recognised = false;
    return out;
}

QString quantityForUnit(const QString &unit)
{
    return dbcUnitFor(unit).quantity;
}

} // namespace ct
