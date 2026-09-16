#include "capacity.h"

#include <QJsonArray>
#include <QtEndian>

namespace ct {

namespace {

const TableCapacity *entry(const DeviceCapacity &c, DeviceTable table)
{
    const int i = int(table);
    return i < c.tables.size() ? &c.tables[i] : nullptr;
}

} // namespace

int DeviceCapacity::capacityOf(DeviceTable table) const
{
    const TableCapacity *e = entry(*this, table);
    return e ? e->capacity : 0;
}

int DeviceCapacity::itemSizeOf(DeviceTable table) const
{
    const TableCapacity *e = entry(*this, table);
    return e ? e->itemSize : 0;
}

bool DeviceCapacity::sameLayout(const DeviceCapacity &other) const
{
    if (storeVersion != other.storeVersion || tables.size() != other.tables.size())
        return false;
    for (int i = 0; i < tables.size(); ++i) {
        if (tables[i].capacity != other.tables[i].capacity
            || tables[i].itemSize != other.tables[i].itemSize)
            return false;
    }
    return true;
}

DeviceCapacity DeviceCapacity::builtIn()
{
    DeviceCapacity c;
    c.storeVersion = EXPECTED_STORE_VERSION;
    c.tables.resize(DEVICE_TABLE_COUNT);
    // Written out slot by slot rather than as a positional list, because the
    // ORDER is the whole content: a list that put timers before counters would
    // compile and would size every check off the wrong table.
    const auto set = [&c](DeviceTable t, int capacity, size_t itemSize) {
        c.tables[int(t)] = TableCapacity{capacity, int(itemSize)};
    };
    set(DeviceTable::Messages, MAX_MESSAGES, sizeof(CanMessageConfig));
    set(DeviceTable::Signals, MAX_SIGNALS, sizeof(CanSignalConfig));
    set(DeviceTable::Math, MAX_MATH_COMPUTATIONS, sizeof(MathConfig));
    set(DeviceTable::Conditions, MAX_CONDITIONS, sizeof(ConditionConfig));
    set(DeviceTable::Counters, MAX_COUNTERS, sizeof(CounterConfig));
    set(DeviceTable::Timers, MAX_TIMERS, sizeof(TimerConfig));
    set(DeviceTable::Constants, MAX_CONSTANTS, sizeof(ConstantConfig));
    set(DeviceTable::Relays, MAX_RELAYS, sizeof(RelayConfig));
    set(DeviceTable::Tables2x16Def, MAX_TABLES_2X16, sizeof(Table2x16Def));
    set(DeviceTable::Tables2x16Out, MAX_TABLES_2X16, sizeof(Table2x16Out));
    set(DeviceTable::Tables8x8Def, MAX_TABLES_8X8, sizeof(Table8x8Def));
    set(DeviceTable::Tables8x8Row, MAX_TABLE_8X8_ROWS, sizeof(Table8x8GridRow));
    set(DeviceTable::Integrators, MAX_INTEGRATORS, sizeof(IntegratorConfig));
    set(DeviceTable::Script, MAX_SCRIPT_CHUNKS, sizeof(ScriptChunk));
    set(DeviceTable::Crc8, MAX_CRC8_MESSAGES, sizeof(Crc8Config));
    c.reported = false;
    return c;
}

QJsonObject DeviceCapacity::toJson() const
{
    QJsonObject o;
    if (!label.isEmpty())
        o[QStringLiteral("label")] = label;
    o[QStringLiteral("storeVersion")] = int(storeVersion);
    QJsonArray entries;
    for (const TableCapacity &t : tables)
        entries.append(QJsonArray{t.capacity, t.itemSize});
    o[QStringLiteral("tables")] = entries;
    return o;
}

bool DeviceCapacity::fromJson(const QJsonObject &object, DeviceCapacity *out)
{
    if (!out)
        return false;
    *out = DeviceCapacity{};
    if (!object.contains(QStringLiteral("tables")))
        return false;
    out->label = object[QStringLiteral("label")].toString();
    out->storeVersion = quint16(object[QStringLiteral("storeVersion")].toInt());
    for (const QJsonValue &v : object[QStringLiteral("tables")].toArray()) {
        const QJsonArray pair = v.toArray();
        if (pair.size() != 2)
            return false;
        out->tables.append(TableCapacity{pair[0].toInt(), pair[1].toInt()});
    }
    out->reported = true;
    return true;
}

QStringList layoutDifferences(const DeviceCapacity &from, const DeviceCapacity &to)
{
    QStringList out;
    if (from.storeVersion != to.storeVersion)
        out.append(QStringLiteral("configuration store: v%1 → v%2")
                       .arg(from.storeVersion)
                       .arg(to.storeVersion));
    const int n = qMax(from.tables.size(), to.tables.size());
    for (int i = 0; i < n; ++i) {
        const DeviceTable table = DeviceTable(i);
        const int a = from.capacityOf(table), b = to.capacityOf(table);
        const int sa = from.itemSizeOf(table), sb = to.itemSizeOf(table);
        if (a != b)
            out.append(QStringLiteral("%1: %2 → %3").arg(deviceTableName(table)).arg(a).arg(b));
        else if (sa != sb)
            out.append(QStringLiteral("%1: %2-byte records → %3-byte")
                           .arg(deviceTableName(table))
                           .arg(sa)
                           .arg(sb));
    }
    return out;
}

QStringList countsExceeding(const QVector<int> &counts, const DeviceCapacity &device)
{
    QStringList out;
    for (int i = 0; i < counts.size(); ++i) {
        const DeviceTable table = DeviceTable(i);
        const int holds = device.capacityOf(table);
        if (counts[i] > holds)
            out.append(QStringLiteral("%1 %2, but this device holds %3")
                           .arg(counts[i])
                           .arg(deviceTableName(table))
                           .arg(holds));
    }
    return out;
}

QString deviceTableName(DeviceTable table)
{
    switch (table) {
    case DeviceTable::Messages: return QStringLiteral("messages");
    case DeviceTable::Signals: return QStringLiteral("channels");
    case DeviceTable::Math: return QStringLiteral("math channels");
    case DeviceTable::Conditions: return QStringLiteral("User Conditions");
    case DeviceTable::Counters: return QStringLiteral("counters");
    case DeviceTable::Timers: return QStringLiteral("timers");
    case DeviceTable::Constants: return QStringLiteral("constants");
    case DeviceTable::Relays: return QStringLiteral("message relays");
    case DeviceTable::Tables2x16Def: return QStringLiteral("2x16 tables");
    case DeviceTable::Tables2x16Out: return QStringLiteral("2x16 table outputs");
    case DeviceTable::Tables8x8Def: return QStringLiteral("8x8 tables");
    case DeviceTable::Tables8x8Row: return QStringLiteral("8x8 table rows");
    case DeviceTable::Integrators: return QStringLiteral("integrators");
    case DeviceTable::Script: return QStringLiteral("script chunks");
    case DeviceTable::Crc8: return QStringLiteral("CRC8 rules");
    }
    return QStringLiteral("table %1").arg(int(table));
}

bool parseCapacityReport(const QByteArray &bytes, DeviceCapacity *out)
{
    if (!out)
        return false;
    *out = DeviceCapacity{};
    constexpr int kHeader = int(sizeof(CapacityReportHeader));
    constexpr int kEntry = int(sizeof(CapacityEntry));
    if (bytes.size() < kHeader)
        return false;
    const auto *p = reinterpret_cast<const quint8 *>(bytes.constData());
    if (p[0] != CAPACITY_REPORT_FORMAT)
        return false; // entries laid out in a way this build does not know
    const int count = p[1];
    if (bytes.size() < kHeader + count * kEntry)
        return false;
    out->storeVersion = qFromLittleEndian<quint16>(p + 2);
    out->tables.reserve(count);
    for (int i = 0; i < count; ++i) {
        const quint8 *e = p + kHeader + i * kEntry;
        out->tables.append(TableCapacity{qFromLittleEndian<quint16>(e),
                                         qFromLittleEndian<quint16>(e + 2)});
    }
    out->reported = true;
    return true;
}

} // namespace ct
