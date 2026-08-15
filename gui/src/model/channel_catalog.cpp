#include "channel_catalog.h"

#include <QMap>
#include <QRegularExpression>

#include "../protocol/wire_structs.h" // DEVCH_* — the device channel ids

namespace ct {

QJsonObject Channel::toJson() const
{
    QJsonObject o;
    o["name"] = name;
    o["quantity"] = quantity;
    o["unit"] = unit;
    o["dataType"] = dataType;
    o["baseResolution"] = baseResolution;
    o["decimalPlaces"] = decimalPlaces;
    o["minValue"] = minValue;
    o["maxValue"] = maxValue;
    o["category"] = category;
    return o;
}

Channel Channel::fromJson(const QJsonObject &o)
{
    Channel c;
    c.name = o["name"].toString();
    c.quantity = o["quantity"].toString(QStringLiteral("Unitless"));
    c.unit = o["unit"].toString();
    c.dataType = o["dataType"].toString(); // empty for pre-data-type files
    c.baseResolution = o["baseResolution"].toDouble(1.0);
    c.decimalPlaces = o["decimalPlaces"].toInt(0);
    c.minValue = o["minValue"].toDouble(-1e9);
    c.maxValue = o["maxValue"].toDouble(1e9);
    c.category = o["category"].toString(QStringLiteral("User Channels"));
    c.userDefined = true;
    return c;
}

// User channels live in the configuration document and travel with the .ct3
// file. Device channels do not: they describe what the hardware always
// provides, so they are compiled in here and appear in every document.
ChannelCatalog::ChannelCatalog() = default;

QString ChannelCatalog::deviceOnTimeName()
{
    return QStringLiteral("Device OnTime");
}

const QList<Channel> &ChannelCatalog::deviceChannels()
{
    // u32 at 2 decimal places is 0.01 s resolution over a 0…42,949,672.95 span
    // — 497 days, far past the 49.7-day wrap of the firmware's millisecond
    // clock, so the type is never the limit. What IS a limit: value slots are
    // float32 holding SECONDS, and past 2^17 = 131,072 s the ulp exceeds 0.01,
    // so the hundredths stop resolving after about 36 hours of uptime and the
    // reading coarsens from there. (An earlier version of this comment said 46
    // hours — that is 2^24 HUNDREDTHS, the bound for an integer hundredths
    // counter, which is not what the slot stores.) Documented in the
    // manual rather than hidden, because it is inherent to every channel the
    // engine carries and not something a different type here would fix.
    //
    // The CAN diagnostics below do NOT inherit that caveat, and it is worth
    // being explicit about why rather than leaving the OnTime warning looking
    // like it covers the whole category: REC, TEC and the three flags are small
    // integers a float32 represents exactly and forever, and Bus Load never
    // leaves 0…100. The two frame counters are the exception — they are
    // unbounded, so they coarsen past 2^24 frames the same way OnTime does.
    // The MCU health block is safe too: degrees and volts never grow, and the
    // reset reason is a small integer.
    static const QList<Channel> channels = [] {
        QList<Channel> list;

        Channel onTime;
        onTime.name = deviceOnTimeName();
        onTime.quantity = QStringLiteral("Time");
        onTime.unit = QStringLiteral("s");
        onTime.dataType = QStringLiteral("u32");
        onTime.decimalPlaces = 2;
        onTime.baseResolution = 0.01;
        onTime.minValue = 0.0;
        onTime.maxValue = 42949672.95;
        onTime.category = QStringLiteral("Device Channels");
        onTime.userDefined = false;
        onTime.deviceChannelId = DEVCH_ONTIME;
        list.append(onTime);

        // The per-bus block, built from one description each so the three buses
        // cannot drift apart. Named "Device CAN1 …" to match the one naming
        // precedent this category has: the "Device " prefix is what keeps these
        // from colliding with a channel a customer already has in a .ct3, which
        // matters more here than it did with one channel because these names are
        // otherwise the obvious ones somebody would pick.
        struct Spec {
            int field;
            const char *suffix;
            const char *quantity;
            const char *unit;
            const char *dataType;
            int decimals;
            double min;
            double max;
        };
        static const Spec specs[] = {
            // Error counters. The CAN standard caps REC at 127 (past that the
            // node is error-passive and stops counting up) and TEC at 255 (past
            // that it is bus-off), so these ranges are the protocol's, not a
            // choice made here.
            {DEVCH_BUS_RX_ERRORS, "Rx Errors", "Unitless", "", "u8", 0, 0.0, 127.0},
            {DEVCH_BUS_TX_ERRORS, "Tx Errors", "Unitless", "", "u8", 0, 0.0, 255.0},
            // The three states, as separate booleans rather than one enum: each
            // reads directly in a condition without anyone having to remember
            // which number meant which state, and they are not exclusive anyway
            // — a bus-off node is also error-passive and also warning.
            {DEVCH_BUS_WARNING, "Warning", "Boolean", "Ok/Error", "boolean", 0, 0.0, 1.0},
            {DEVCH_BUS_ERROR_PASSIVE, "Error Passive", "Boolean", "Ok/Error", "boolean", 0, 0.0,
             1.0},
            {DEVCH_BUS_BUS_OFF, "Bus Off", "Boolean", "Ok/Error", "boolean", 0, 0.0, 1.0},
            // Totals since power-up, not since the configuration was sent —
            // re-sending a config does not reset them (see engine_core.h).
            {DEVCH_BUS_ERROR_FRAMES, "Error Frames", "Unitless", "", "u32", 0, 0.0, 4294967295.0},
            {DEVCH_BUS_RX_COUNT, "Rx Count", "Unitless", "", "u32", 0, 0.0, 4294967295.0},
            {DEVCH_BUS_TX_COUNT, "Tx Count", "Unitless", "", "u32", 0, 0.0, 4294967295.0},
            // An ESTIMATE — the FDCAN peripheral counts frames, not bits, so the
            // firmware models each frame's length and adds an average allowance
            // for stuff bits. Good to a few percent, not to the digit; the help
            // page says so, and the three flags above are what actually tell you
            // a bus is in trouble.
            // u16, not u8: a scaled-integer channel stores range * 10^decimals,
            // so one decimal place over 0…100 needs 0…1000 raw counts and u8
            // would silently clip everything above 25.5 %.
            {DEVCH_BUS_LOAD, "Bus Load", "Ratio", "%", "u16", 1, 0.0, 100.0},
            // Counts RESTARTS the firmware performed, not reconnections that
            // lasted. On a bus that is still faulty a restart is undone within
            // about five milliseconds — well inside one 100 Hz sample — so a
            // count of lasting successes would read 0 on precisely the bus
            // being diagnosed. Paired with "Bus Off" it separates a bus that is
            // down and being retried from one that is flapping, which the state
            // flag alone cannot show because it reads 0 between events.
            {DEVCH_BUS_OFF_RECOVERIES, "Bus Off Recoveries", "Unitless", "", "u32", 0, 0.0,
             4294967295.0},
        };

        for (int bus0 = 0; bus0 < DEVCH_BUS_COUNT; ++bus0) {
            for (const Spec &s : specs) {
                Channel c;
                c.name = QStringLiteral("Device CAN%1 %2")
                             .arg(bus0 + 1)
                             .arg(QLatin1String(s.suffix));
                c.quantity = QLatin1String(s.quantity);
                c.unit = QLatin1String(s.unit);
                c.dataType = QLatin1String(s.dataType);
                c.decimalPlaces = s.decimals;
                c.baseResolution = s.decimals > 0 ? 0.1 : 1.0;
                c.minValue = s.min;
                c.maxValue = s.max;
                c.category = QStringLiteral("Device Channels");
                c.userDefined = false;
                c.deviceChannelId = devChBus(bus0, s.field);
                list.append(c);
            }
        }

        // The MCU health block (store v9): what the silicon reports about
        // itself, keeping the "Device " prefix rule the block above documents.
        // Two caveats belong to the DEFINITION, not just the help page. The
        // Minimum/Maximum pair are excursions since BOOT, not since the
        // configuration loaded — they survive a config clear the way the CAN
        // error totals do, because "what has this unit been through" is
        // exactly the question a mid-diagnosis reconfigure must not erase.
        // And the temperature's tenth of a degree is RESOLUTION, not accuracy:
        // the die sensor is a ±2 °C class device even with its factory
        // calibration applied, so the decimal is for watching change, never
        // for trusting the absolute reading.
        const auto mcuChannel = [&list](const char *name, const char *quantity,
                                        const char *unit, const char *dataType,
                                        int decimals, double resolution, double min,
                                        double max, int id) -> Channel & {
            Channel c;
            c.name = QLatin1String(name);
            c.quantity = QLatin1String(quantity);
            c.unit = QLatin1String(unit);
            c.dataType = QLatin1String(dataType);
            c.decimalPlaces = decimals;
            c.baseResolution = resolution;
            c.minValue = min;
            c.maxValue = max;
            c.category = QStringLiteral("Device Channels");
            c.userDefined = false;
            c.deviceChannelId = id;
            list.append(c);
            return list.last();
        };
        // The temperatures span the whole s16 reach at 0.1 — the derived range
        // an s16/1 dp user channel would get. The VDDA pair do not: 0–5 V is
        // already past anything a live board can read (the part's absolute
        // maximum supply is 3.6 V), and s32 is the wire contract's type for
        // the slot, not a claim about span.
        mcuChannel("Device MCU Temperature", "Temperature", "C", "s16", 1, 0.1,
                   -3276.8, 3276.7, DEVCH_MCU_TEMP);
        mcuChannel("Device MCU VDDA", "Voltage", "V", "s32", 3, 0.001, 0.0, 5.0,
                   DEVCH_MCU_VDDA);
        mcuChannel("Device MCU VDDA Minimum", "Voltage", "V", "s32", 3, 0.001, 0.0,
                   5.0, DEVCH_MCU_VDDA_MIN);
        mcuChannel("Device MCU Temperature Maximum", "Temperature", "C", "s16", 1,
                   0.1, -3276.8, 3276.7, DEVCH_MCU_TEMP_MAX);
        // Why the last reset happened, latched once at boot. ENUMERATED: the
        // labels mirror the firmware's RESET_REASON_* numbering, which is wire
        // contract — a live display shows "Power On (1)", and a value outside
        // the map (a newer firmware's new reason) shows as the bare number
        // rather than borrowing the nearest wrong label.
        Channel &reset = mcuChannel("Device Last Reset Reason", "Unitless", "",
                                    "u8", 0, 1.0, 0.0, 7.0, DEVCH_RESET_REASON);
        reset.enumLabels = {
            {RESET_REASON_UNKNOWN, QStringLiteral("Unknown")},
            {RESET_REASON_POWER_ON, QStringLiteral("Power On")},
            {RESET_REASON_BROWNOUT, QStringLiteral("Brownout")},
            {RESET_REASON_NRST, QStringLiteral("External NRST")},
            {RESET_REASON_SOFTWARE, QStringLiteral("Software Reset")},
            {RESET_REASON_IWDG, QStringLiteral("Independent Watchdog")},
            {RESET_REASON_WWDG, QStringLiteral("Window Watchdog")},
            {RESET_REASON_LOW_POWER, QStringLiteral("Low Power Reset")},
        };
        return list;
    }();
    return channels;
}

Channel ChannelCatalog::deviceChannelById(int id)
{
    for (const Channel &c : deviceChannels())
        if (c.deviceChannelId == id)
            return c;
    return {};
}

bool ChannelCatalog::isDeviceChannel(const QString &name)
{
    for (const Channel &c : deviceChannels())
        if (c.name.compare(name, Qt::CaseInsensitive) == 0)
            return true;
    return false;
}

QList<Channel> ChannelCatalog::allChannels() const
{
    QList<Channel> all = m_userChannels;
    all.append(deviceChannels());
    std::sort(all.begin(), all.end(),
              [](const Channel &a, const Channel &b) { return a.name.localeAwareCompare(b.name) < 0; });
    return all;
}

QStringList ChannelCatalog::categories() const
{
    QStringList cats;
    if (!m_userChannels.isEmpty())
        cats << QStringLiteral("User Channels");
    cats << QStringLiteral("Device Channels");
    return cats;
}

QList<Channel> ChannelCatalog::channelsInCategory(const QString &category) const
{
    Q_UNUSED(category);
    return allChannels();
}

Channel ChannelCatalog::findByName(const QString &name) const
{
    for (const Channel &c : m_userChannels)
        if (c.name.compare(name, Qt::CaseInsensitive) == 0)
            return c;
    // User channels win a name collision on purpose: a document that already
    // had a channel of this name predates the device channel, and silently
    // re-typing it out from under the configuration would be worse than the
    // shadowing. Validation reports the clash.
    for (const Channel &c : deviceChannels())
        if (c.name.compare(name, Qt::CaseInsensitive) == 0)
            return c;
    return {};
}

QString ChannelCatalog::labelFor(const QString &name) const
{
    if (name.isEmpty())
        return name;
    const Channel c = findByName(name);
    // Unknown name: hand back what was asked for. A reference to a deleted
    // channel is a real state the document can be in, and the place that shows
    // it is usually the place the user is about to fix it — showing nothing
    // would hide which reference is broken.
    return c.isValid() ? channelLabel(c) : name;
}

bool ChannelCatalog::matchesSearch(const QString &channelName, const QString &searchText)
{
    const QString trimmed = searchText.trimmed();
    if (trimmed.isEmpty())
        return true;

    // A search that carries regular-expression metacharacters is used as a
    // regex over the whole name — "^Cruise", "Speed$", "set|limit". An invalid
    // pattern falls through to the plain-text path below so that a half-typed
    // "(" doesn't blank the list mid-keystroke.
    static const QRegularExpression metaRe(QStringLiteral(R"([\\^$.\[\]|()*+?{}])"));
    if (trimmed.contains(metaRe)) {
        const QRegularExpression re(trimmed, QRegularExpression::CaseInsensitiveOption);
        if (re.isValid())
            return re.match(channelName).hasMatch();
    }

    // Otherwise every space-separated term must appear ANYWHERE in the name,
    // not just at the start of a word: "set" finds "CruiseSetSpeed", which a
    // word-prefix match could never do for a run-together name. Substring
    // matching subsumes the old behaviour — a term that prefixes a word is
    // still a substring — so existing search habits keep working.
    const QStringList terms = trimmed.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (const QString &t : terms)
        if (!channelName.contains(t, Qt::CaseInsensitive))
            return false;
    return true;
}

QList<Channel> ChannelCatalog::search(const QString &text) const
{
    QList<Channel> result;
    for (const Channel &c : allChannels())
        if (matchesSearch(c.name, text))
            result.append(c);
    return result;
}

void ChannelCatalog::setUserChannels(const QList<Channel> &channels)
{
    m_userChannels = channels;
    for (Channel &c : m_userChannels)
        c.userDefined = true;
}

void ChannelCatalog::addOrUpdateUserChannel(const Channel &channel)
{
    Channel c = channel;
    c.userDefined = true;
    if (c.category.isEmpty())
        c.category = QStringLiteral("User Channels");
    for (Channel &existing : m_userChannels)
        if (existing.name.compare(c.name, Qt::CaseInsensitive) == 0) {
            existing = c;
            return;
        }
    m_userChannels.append(c);
}

void ChannelCatalog::removeUserChannel(const QString &name)
{
    for (int i = 0; i < m_userChannels.size(); ++i)
        if (m_userChannels[i].name.compare(name, Qt::CaseInsensitive) == 0) {
            m_userChannels.removeAt(i);
            return;
        }
}

QStringList ChannelCatalog::quantities()
{
    return {"Unitless",
            "Temperature",
            "Pressure and Stress",
            "Time",
            "Length & Distance",
            "Speed",
            "Acceleration",
            "Weight & Force",
            "Angle",
            "Rotational Speed",
            "Rotational Acceleration",
            "Torque",
            "Power",
            "Volume",
            "Volume Flow",
            "Mass Flow",
            "Voltage",
            "Current",
            "Resistance",
            "Boolean",
            "Air Fuel Ratio",
            "Ratio",
            "Mass Consumption",
            "Fuel Economy",
            "Sound"};
}

static const QMap<QString, QStringList> &unitMap()
{
    static const QMap<QString, QStringList> units = {
        {"Unitless", {""}},
        {"Temperature", {"C", "F", "K"}},
        {"Pressure and Stress",
         {"Pa", "kPa", "kPa a", "kPa g", "MPa", "mbar", "bar", "psi", "psi a", "psi g", "mmHg",
          "inHg", "inH2O"}},
        {"Time", {"us", "ms", "s", "min", "h"}},
        {"Length & Distance", {"mm", "cm", "m", "km", "mil", "in", "ft", "y", "mile", "NM"}},
        {"Speed", {"mm/s", "m/s", "km/h", "ft/s", "ft/min", "mile/h", "knots"}},
        {"Acceleration", {"G", "m/s/s", "ft/s/s"}},
        {"Weight & Force", {"N", "g", "kg", "t", "oz", "lb", "mg"}},
        {"Angle", {"deg", "dBTDC", "dATDC", "rad", "rev"}},
        {"Rotational Speed", {"rpm", "Hz", "rps", "deg/s", "rad/s", "rpmx100", "rpmx1000"}},
        {"Rotational Acceleration", {"deg/s/s", "rad/s/s", "rev/s/s"}},
        {"Torque", {"Nm", "Ncm", "kgm", "inlb", "ftlb"}},
        {"Power", {"W", "kW", "hp"}},
        {"Volume", {"cc", "ml", "l", "cin", "cft", "USgal", "UKgal", "USfloz"}},
        {"Volume Flow", {"cc/s", "cc/min", "ml/s", "ml/min", "l/s", "l/min", "l/h", "cin/s"}},
        {"Mass Flow", {"g/s", "g/min", "kg/s", "kg/min", "kg/h", "lb/s", "lb/min", "lb/h"}},
        {"Voltage", {"V", "mV"}},
        {"Current", {"A", "mA"}},
        {"Resistance", {"ohm", "kohm"}},
        {"Boolean",
         {"Off/On", "Inactive/Active", "No/Yes", "Ok/Error", "Good/Bad", "Cold/Hot", "Rich/Lean"}},
        {"Air Fuel Ratio", {"LA", "A/F"}},
        {"Ratio", {"%", "Ratio", "%Trim"}},
        {"Mass Consumption", {"g/cyl", "g/rev"}},
        {"Fuel Economy", {"l/mi", "km/l", "mpg"}},
        {"Sound", {"db"}},
    };
    return units;
}

QStringList ChannelCatalog::unitsForQuantity(const QString &quantity)
{
    return unitMap().value(quantity, {""});
}

QString ChannelCatalog::defaultUnitForQuantity(const QString &quantity)
{
    // Explicit defaults where they differ from the first listed unit.
    static const QMap<QString, QString> defaults = {
        {"Temperature", "C"},        {"Pressure and Stress", "kPa"},
        {"Time", "s"},               {"Length & Distance", "m"},
        {"Speed", "km/h"},           {"Acceleration", "G"},
        {"Weight & Force", "kg"},    {"Angle", "deg"},
        {"Rotational Speed", "rpm"}, {"Rotational Acceleration", "deg/s/s"},
        {"Torque", "Nm"},            {"Power", "W"},
        {"Volume", "l"},             {"Volume Flow", "cc/s"},
        {"Mass Flow", "g/s"},        {"Voltage", "V"},
        {"Current", "A"},            {"Resistance", "ohm"},
        {"Boolean", "Off/On"},       {"Air Fuel Ratio", "LA"},
        {"Ratio", "%"},              {"Mass Consumption", "g/cyl"},
    };
    const auto it = defaults.constFind(quantity);
    if (it != defaults.constEnd())
        return *it;
    // Otherwise the first listed unit (Fuel Economy -> l/mi, Sound -> db,
    // Unitless -> "").
    const QStringList u = unitsForQuantity(quantity);
    return u.isEmpty() ? QString() : u.first();
}

} // namespace ct
