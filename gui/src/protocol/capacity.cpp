#include "capacity.h"

#include <cstddef> // offsetof, for the extension's fields

#include <QCoreApplication>

#include <QJsonArray>
#include <QLocale>
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

int DeviceCapacity::limitOf(DeviceTable table) const
{
    if (!plansTables())
        return capacityOf(table);
    const int i = int(table);
    return i < plan.ceiling.size() ? plan.ceiling[i] : 0;
}

namespace {

qint64 planEntry(const QVector<int> &capacities, int table)
{
    return table >= 0 && table < capacities.size() ? qMax(0, capacities[table]) : 0;
}

// How many entries a slice of measure `measure` has for a plan (the 2.0
// protocol.h, ARENA_MEASURE_*); false for a measure this build does not know.
bool measureEntries(const PlanBudget &plan, int measure, const QVector<int> &capacities,
                    qint64 *entries)
{
    const qint64 channels = planEntry(capacities, int(DeviceTable::Signals));
    const qint64 messages = planEntry(capacities, int(DeviceTable::Messages));
    switch (measure) {
    case ARENA_MEASURE_CHANNEL: *entries = channels; return true;
    case ARENA_MEASURE_CHANNEL_END: *entries = channels + 1; return true;
    case ARENA_MEASURE_CHANNEL_BIT: *entries = (channels + 7) / 8; return true;
    case ARENA_MEASURE_MESSAGE: *entries = messages; return true;
    case ARENA_MEASURE_MESSAGE_BIT: *entries = (messages + 31) / 32; return true;
    case ARENA_MEASURE_MESSAGE_WORD: *entries = ((messages + 31) / 32 + 31) / 32; return true;
    case ARENA_MEASURE_CONDITION:
        *entries = planEntry(capacities, int(DeviceTable::Conditions));
        return true;
    case ARENA_MEASURE_COUNTER:
        *entries = planEntry(capacities, int(DeviceTable::Counters));
        return true;
    case ARENA_MEASURE_TIMER:
        *entries = planEntry(capacities, int(DeviceTable::Timers));
        return true;
    case ARENA_MEASURE_INTEGRATOR:
        *entries = planEntry(capacities, int(DeviceTable::Integrators));
        return true;
    case ARENA_MEASURE_STAGE_ROW_BIT: {
        qint64 rows = 0;
        for (const int t : plan.stageTables)
            rows = qMax(rows, planEntry(capacities, t));
        *entries = qint64(plan.stageTables.size()) * (rows > 0 ? (rows + 31) / 32 : 1);
        return true;
    }
    default:
        return false;
    }
}

} // namespace

PlanCostResult DeviceCapacity::planCost(const QVector<int> &capacities, int valueNames,
                                        int notes) const
{
    PlanCostResult r;
    if (!plansTables())
        return r;
    // FLASH: the header, then each table at its capacity times its record
    // rounded up to 8.
    r.flashBytes = plan.headerBytes;
    for (int t = 0; t < tables.size(); ++t)
        r.flashBytes += planEntry(capacities, t) * ((tables[t].itemSize + 7) / 8 * 8);
    // NAMES: each kind's run of label_bytes names on a multiple of label_align,
    // messages, channels, relays, then (phase 4, step 3b) a unit for each
    // channel and the value names where the target keeps them; the last kind's
    // run not rounded up.
    if (labelBytes > 0) {
        const qint64 align = qMax(1, plan.labelAlign);
        QVector<qint64> runs{planEntry(capacities, int(DeviceTable::Messages)),
                             planEntry(capacities, int(DeviceTable::Signals)),
                             planEntry(capacities, int(DeviceTable::Relays))};
        if (keepsUnits())
            runs.append(planEntry(capacities, int(DeviceTable::Signals)));
        if (keepsValueNames())
            runs.append(qMax(0, valueNames));
        // Config Notes: after the value names, which then end on a boundary
        // like the rest, notes or none.
        if (keepsNotes())
            runs.append(qMax(0, notes));
        for (int i = 0; i < runs.size(); ++i)
            r.labelBytes += i + 1 < runs.size() ? (runs[i] * labelBytes + align - 1) / align * align
                                                : runs[i] * labelBytes;
    }
    // RAM: the slices in carving order, each on its alignment.
    qint64 at = 0;
    for (const PlanBudget::Slice &s : plan.slices) {
        qint64 entries = 0;
        if (s.measure >= plan.measureCount || !measureEntries(plan, s.measure, capacities, &entries)) {
            r.costed = false;
            break;
        }
        const qint64 align = qMax(1, s.align);
        at = (at + align - 1) / align * align + entries * s.bytes;
    }
    r.arenaBytes = r.costed ? at : 0;
    // Well formed: within every ceiling, and the paired tables paired.
    bool wellFormed = true;
    for (int t = 0; t < capacities.size(); ++t)
        if (capacities[t] < 0 || capacities[t] > (t < plan.ceiling.size() ? plan.ceiling[t] : 0))
            wellFormed = false;
    if (planEntry(capacities, int(DeviceTable::Tables2x16Out))
        != planEntry(capacities, int(DeviceTable::Tables2x16Def)))
        wellFormed = false;
    if (planEntry(capacities, int(DeviceTable::Tables8x8Row))
        != planEntry(capacities, int(DeviceTable::Tables8x8Def)) * TABLE_8X8_SITES)
        wellFormed = false;
    if (!wellFormed) {
        r.verdict = ERR_OUT_OF_BOUNDS;
        return r;
    }
    if (r.flashBytes > plan.flashBytes)
        r.shortOf |= PLAN_SHORT_FLASH;
    // The transmit queues' floors beside the slices, where the queues are in
    // that RAM (phase 4, step 4c).
    if (r.costed && r.arenaBytes + (holdsTxQueues() ? 3 * qint64(plan.txQueueFloor) : 0)
                        > plan.arenaBytes)
        r.shortOf |= PLAN_SHORT_ARENA;
    if (r.labelBytes > plan.labelRegionBytes)
        r.shortOf |= PLAN_SHORT_NAMES;
    r.verdict = r.shortOf != 0 ? ERR_NO_ROOM : 0;
    return r;
}

qint64 DeviceCapacity::arenaWithReferences(qint64 sliceBytes, qint64 references,
                                           qint64 udsBytes) const
{
    const qint64 at = (sliceBytes + 1) / 2 * 2 + qMax<qint64>(0, references) * plan.refBytes;
    if (!holdsTxQueues())
        return at;
    // Then the UDS requests' buffers (on 4 bytes each, as planUdsBytes gives
    // them), then each bus's transmit queue at its floor (phase 4, step 4c).
    return (at + 3) / 4 * 4 + qMax<qint64>(0, udsBytes) + 3 * qint64(plan.txQueueFloor);
}

bool DeviceCapacity::referencesFit(qint64 sliceBytes, qint64 references, qint64 udsBytes) const
{
    return arenaWithReferences(sliceBytes, references, udsBytes) <= plan.arenaBytes
           && references < 0xFFFF;
}

bool DeviceCapacity::sameLayout(const DeviceCapacity &other) const
{
    if (storeVersion != other.storeVersion || tables.size() != other.tables.size())
        return false;
    // Two targets that take a plan: each configuration carries its own, so only
    // the records' sizes say whether one written by one reads on the other.
    const bool sizesOnly = plansTables() && other.plansTables();
    for (int i = 0; i < tables.size(); ++i) {
        if ((!sizesOnly && tables[i].capacity != other.tables[i].capacity)
            || tables[i].itemSize != other.tables[i].itemSize)
            return false;
    }
    return true;
}

DeviceCapacity DeviceCapacity::builtIn()
{
    DeviceCapacity c;
    c.storeVersion = EXPECTED_STORE_VERSION;
    // The 1.x layout: fifteen tables. The sixteenth (the 2.0's UDS requests)
    // only ever comes from a report, so a comparison with one that cannot
    // report stays table for table.
    c.tables.resize(DEVICE_TABLE_COUNT_1X);
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
    if (retainedValues > 0) {
        o[QStringLiteral("retained")] = QJsonObject{
            {QStringLiteral("values"), retainedValues},
            {QStringLiteral("intervalMs"), retainedIntervalMs},
            {QStringLiteral("noWear"), retainedNoWear},
        };
        if (retainedChannels)
            o[QStringLiteral("retained")] = QJsonObject{
                {QStringLiteral("values"), retainedValues},
                {QStringLiteral("intervalMs"), retainedIntervalMs},
                {QStringLiteral("noWear"), retainedNoWear},
                {QStringLiteral("channels"), true},
            };
    }
    if (scriptBudget > 0 || !scriptCosts.isEmpty()) {
        QJsonArray costs;
        for (const char c : scriptCosts)
            costs.append(int(quint8(c)));
        o[QStringLiteral("script")] = QJsonObject{
            {QStringLiteral("budget"), scriptBudget},
            {QStringLiteral("costs"), costs},
        };
    }
    if (crc8Features != 0)
        o[QStringLiteral("crc8")] = QJsonObject{{QStringLiteral("features"), crc8Features}};
    if (canFeatures != 0)
        o[QStringLiteral("can")] = QJsonObject{{QStringLiteral("features"), canFeatures}};
    if (labelBytes != 0) {
        QJsonObject labels{{QStringLiteral("bytes"), labelBytes}};
        if (labelFlags != 0)
            labels[QStringLiteral("flags")] = labelFlags;
        o[QStringLiteral("labels")] = labels;
    }
    if (plan.flags != 0) {
        QJsonArray ceiling, stages, slices;
        for (const int c : plan.ceiling)
            ceiling.append(c);
        for (const int t : plan.stageTables)
            stages.append(t);
        for (const PlanBudget::Slice &s : plan.slices)
            slices.append(QJsonArray{s.measure, s.align, s.bytes});
        o[QStringLiteral("plan")] = QJsonObject{
            {QStringLiteral("flags"), plan.flags},
            {QStringLiteral("header"), plan.headerBytes},
            {QStringLiteral("flash"), double(plan.flashBytes)},
            {QStringLiteral("arena"), double(plan.arenaBytes)},
            {QStringLiteral("names"), double(plan.labelRegionBytes)},
            {QStringLiteral("align"), plan.labelAlign},
            {QStringLiteral("ref"), plan.refBytes},
            {QStringLiteral("ceiling"), ceiling},
            {QStringLiteral("measures"), plan.measureCount},
            {QStringLiteral("stages"), stages},
            {QStringLiteral("slices"), slices},
        };
        if (holdsTxQueues()) {
            QJsonObject p = o[QStringLiteral("plan")].toObject();
            p[QStringLiteral("txQueueFloor")] = plan.txQueueFloor;
            p[QStringLiteral("txQueueEntryOverhead")] = plan.txQueueEntryOverhead;
            o[QStringLiteral("plan")] = p;
        }
    }
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
    const QJsonObject retained = object[QStringLiteral("retained")].toObject();
    out->retainedValues = qMax(0, retained[QStringLiteral("values")].toInt());
    out->retainedIntervalMs = qMax(0, retained[QStringLiteral("intervalMs")].toInt());
    out->retainedNoWear = retained[QStringLiteral("noWear")].toBool();
    out->retainedChannels = retained[QStringLiteral("channels")].toBool();
    const QJsonObject script = object[QStringLiteral("script")].toObject();
    out->scriptBudget = qMax(0, script[QStringLiteral("budget")].toInt());
    for (const QJsonValue &v : script[QStringLiteral("costs")].toArray())
        out->scriptCosts.append(char(quint8(qBound(0, v.toInt(), 255))));
    out->crc8Features = qBound(
        0, object[QStringLiteral("crc8")].toObject()[QStringLiteral("features")].toInt(), 0xFFFF);
    out->canFeatures = qBound(
        0, object[QStringLiteral("can")].toObject()[QStringLiteral("features")].toInt(), 0xFFFF);
    out->labelBytes = qBound(
        0, object[QStringLiteral("labels")].toObject()[QStringLiteral("bytes")].toInt(), 255);
    out->labelFlags = qBound(
        0, object[QStringLiteral("labels")].toObject()[QStringLiteral("flags")].toInt(), 255);
    const QJsonObject plan = object[QStringLiteral("plan")].toObject();
    if (!plan.isEmpty()) {
        PlanBudget &p = out->plan;
        p.flags = qBound(0, plan[QStringLiteral("flags")].toInt(), 0xFFFF);
        p.headerBytes = qMax(0, plan[QStringLiteral("header")].toInt());
        p.flashBytes = qMax<qint64>(0, qint64(plan[QStringLiteral("flash")].toDouble()));
        p.arenaBytes = qMax<qint64>(0, qint64(plan[QStringLiteral("arena")].toDouble()));
        p.labelRegionBytes = qMax<qint64>(0, qint64(plan[QStringLiteral("names")].toDouble()));
        p.labelAlign = qMax(1, plan[QStringLiteral("align")].toInt());
        p.refBytes = qMax(0, plan[QStringLiteral("ref")].toInt());
        for (const QJsonValue &v : plan[QStringLiteral("ceiling")].toArray())
            p.ceiling.append(qBound(0, v.toInt(), 0xFFFF));
        p.measureCount = qBound(0, plan[QStringLiteral("measures")].toInt(), 255);
        for (const QJsonValue &v : plan[QStringLiteral("stages")].toArray())
            p.stageTables.append(qBound(0, v.toInt(), 255));
        for (const QJsonValue &v : plan[QStringLiteral("slices")].toArray()) {
            const QJsonArray s = v.toArray();
            if (s.size() != 3)
                return false;
            p.slices.append(PlanBudget::Slice{qBound(0, s[0].toInt(), 255),
                                              qBound(1, s[1].toInt(), 255),
                                              qBound(0, s[2].toInt(), 0xFFFF)});
        }
        p.txQueueFloor = qBound(0, plan[QStringLiteral("txQueueFloor")].toInt(), 0xFFFF);
        p.txQueueEntryOverhead =
            qBound(0, plan[QStringLiteral("txQueueEntryOverhead")].toInt(), 255);
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
    // Between two that take a plan the capacities are only default plans (see
    // sameLayout): the record sizes are the layout.
    const bool sizesOnly = from.plansTables() && to.plansTables();
    for (int i = 0; i < n; ++i) {
        const DeviceTable table = DeviceTable(i);
        const int a = from.capacityOf(table), b = to.capacityOf(table);
        const int sa = from.itemSizeOf(table), sb = to.itemSizeOf(table);
        if (a != b && !sizesOnly)
            out.append(QStringLiteral("%1: %2 → %3").arg(deviceTableName(table)).arg(a).arg(b));
        else if (sa != sb)
            out.append(QStringLiteral("%1: %2-byte records → %3-byte")
                           .arg(deviceTableName(table))
                           .arg(sa)
                           .arg(sb));
    }
    return out;
}

QStringList countsExceeding(const QVector<int> &counts, const DeviceCapacity &device,
                            qint64 references, int valueNames, int notes, qint64 udsBytes)
{
    QStringList out;
    for (int i = 0; i < counts.size(); ++i) {
        const DeviceTable table = DeviceTable(i);
        const int holds = device.limitOf(table);
        if (counts[i] > holds)
            out.append(device.plansTables()
                           ? QStringLiteral("%1 %2, but this device holds at most %3")
                                 .arg(counts[i])
                                 .arg(deviceTableName(table))
                                 .arg(holds)
                           : QStringLiteral("%1 %2, but this device holds %3")
                                 .arg(counts[i])
                                 .arg(deviceTableName(table))
                                 .arg(holds));
    }
    // The notes: 16 KB at the most (Config Notes).
    if (device.keepsNotes() && notes > LABEL_NOTE_SLOTS_MAX)
        out.append(QStringLiteral("%1 bytes of notes, but this device keeps %2")
                       .arg(notes * LABEL_STORE_BYTES)
                       .arg(LABEL_NOTE_SLOTS_MAX * LABEL_STORE_BYTES));
    // The value names a plan declares in two bytes (phase 4, step 3b).
    if (device.keepsValueNames() && valueNames > 0xFFFF)
        out.append(QStringLiteral("%1 value names, but this device keeps at most %2")
                       .arg(valueNames)
                       .arg(0xFFFF));
    if (!device.plansTables() || !out.isEmpty())
        return out;
    // The plan these counts declare, against the unit's rooms.
    const PlanCostResult cost =
        device.planCost(planCapacities(counts), device.keepsValueNames() ? valueNames : 0,
                        device.keepsNotes() ? notes : 0);
    const auto bytes = [](qint64 n) { return QLocale().toString(qlonglong(n)); };
    if (cost.verdict == ERR_OUT_OF_BOUNDS)
        out.append(QStringLiteral("tables this device cannot pair up (2x16 table outputs or 8x8 "
                                  "table rows)"));
    if (cost.shortOf & PLAN_SHORT_FLASH)
        out.append(QStringLiteral("its tables need %1 bytes of configuration storage, but this "
                                  "device has %2")
                       .arg(bytes(cost.flashBytes), bytes(device.plan.flashBytes)));
    if (cost.shortOf & PLAN_SHORT_ARENA)
        out.append(QStringLiteral("its tables need %1 bytes of working memory, but this device "
                                  "has %2")
                       .arg(bytes(cost.arenaBytes), bytes(device.plan.arenaBytes)));
    if (cost.shortOf & PLAN_SHORT_NAMES)
        out.append(QStringLiteral("its names need %1 bytes, but this device keeps %2")
                       .arg(bytes(cost.labelBytes), bytes(device.plan.labelRegionBytes)));
    if (cost.verdict == 0 && cost.costed && references >= 0
        && !device.referencesFit(cost.arenaBytes, references, udsBytes))
        out.append((device.holdsTxQueues()
                        ? QStringLiteral("its tables, the %1 links between its channels and its "
                                         "calculations and the least of its transmit queues need "
                                         "%2 bytes of working memory, but this device has %3")
                        : QStringLiteral("its tables and the %1 links between its channels and "
                                         "its calculations need %2 bytes of working memory, but "
                                         "this device has %3"))
                       .arg(bytes(references),
                            bytes(device.arenaWithReferences(cost.arenaBytes, references, udsBytes)),
                            bytes(device.plan.arenaBytes)));
    return out;
}

QVector<int> planCapacities(QVector<int> counts)
{
    counts.resize(qMax(int(counts.size()), DEVICE_TABLE_COUNT));
    int &def2 = counts[int(DeviceTable::Tables2x16Def)];
    int &out2 = counts[int(DeviceTable::Tables2x16Out)];
    def2 = out2 = qMax(def2, out2);
    int &def8 = counts[int(DeviceTable::Tables8x8Def)];
    int &rows8 = counts[int(DeviceTable::Tables8x8Row)];
    def8 = qMax(def8, (rows8 + TABLE_8X8_SITES - 1) / TABLE_8X8_SITES);
    rows8 = def8 * TABLE_8X8_SITES;
    return counts;
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
    case DeviceTable::Uds: return QStringLiteral("UDS requests");
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
    if (p[0] != CAPACITY_REPORT_FORMAT && p[0] != CAPACITY_REPORT_FORMAT_EXT)
        return false; // entries laid out in a way this build does not know
    const int count = p[1];
    if (bytes.size() < kHeader + count * kEntry)
        return false;
    // Format 2: the extension after the entries, all of it there, and at least
    // the retained fields every 2.0 build sends. The first builds end it there;
    // later ones add the script cost model, then the CRC8 features, the CAN
    // features, where names live and the stored project, and a longer block
    // still carries fields this build does not know yet.
    if (p[0] == CAPACITY_REPORT_FORMAT_EXT) {
        const int at = kHeader + count * kEntry;
        constexpr int kRetainedEnd = int(offsetof(CapacityExtension, script_budget));
        constexpr int kScriptCosts = int(offsetof(CapacityExtension, script_op_costs));
        if (bytes.size() < at + 2)
            return false;
        const int size = qFromLittleEndian<quint16>(p + at);
        if (size < kRetainedEnd || bytes.size() < at + size)
            return false;
        out->retainedValues = qFromLittleEndian<quint16>(p + at + 2);
        out->retainedIntervalMs = qFromLittleEndian<quint16>(p + at + 4);
        out->retainedNoWear =
            (qFromLittleEndian<quint16>(p + at + 6) & CAPACITY_RETAINED_NO_WEAR) != 0;
        out->retainedChannels =
            (qFromLittleEndian<quint16>(p + at + 6) & CAPACITY_RETAINED_CHANNELS) != 0;
        if (size >= kScriptCosts) {
            out->scriptBudget = qFromLittleEndian<quint16>(p + at + kRetainedEnd);
            const int ops = qMin(int(p[at + kRetainedEnd + 2]), size - kScriptCosts);
            out->scriptCosts = bytes.mid(at + kScriptCosts, ops);
        }
        constexpr int kCrc8At = int(offsetof(CapacityExtension, crc8_features));
        if (size >= kCrc8At + 2)
            out->crc8Features = qFromLittleEndian<quint16>(p + at + kCrc8At);
        constexpr int kCanAt = int(offsetof(CapacityExtension, can_features));
        if (size >= kCanAt + 2)
            out->canFeatures = qFromLittleEndian<quint16>(p + at + kCanAt);
        constexpr int kLabelsAt = int(offsetof(CapacityExtension, label_bytes));
        if (size >= kLabelsAt + 1)
            out->labelBytes = p[at + kLabelsAt];
        constexpr int kLabelFlagsAt = int(offsetof(CapacityExtension, label_flags));
        if (size >= kLabelFlagsAt + 1)
            out->labelFlags = p[at + kLabelFlagsAt];
        constexpr int kBuildAt = int(offsetof(CapacityExtension, build_flags));
        if (size >= kBuildAt + 2)
            out->buildFlags = qFromLittleEndian<quint16>(p + at + kBuildAt);
        constexpr int kProjectAt = int(offsetof(CapacityExtension, project_bytes));
        if (size >= kProjectAt + 4)
            out->projectBytes =
                int(qMin<quint32>(qFromLittleEndian<quint32>(p + at + kProjectAt), 0x7FFFFFFFu));
        // The plan (phase 4), whole or not at all: a block that stops inside it
        // is from no firmware, and a target that takes no plan is the safe
        // reading of one. It ends at the transmit queues' fields (step 4c),
        // which a block from before them does not have.
        if (size >= int(offsetof(CapacityExtension, tx_queue_floor))) {
            const quint8 *q = p + at;
            const auto u16 = [q](size_t off) { return int(qFromLittleEndian<quint16>(q + off)); };
            const auto u32 = [q](size_t off) { return qint64(qFromLittleEndian<quint32>(q + off)); };
            PlanBudget &plan = out->plan;
            plan.flags = u16(offsetof(CapacityExtension, plan_flags));
            plan.headerBytes = u16(offsetof(CapacityExtension, plan_header_bytes));
            plan.flashBytes = u32(offsetof(CapacityExtension, plan_flash_bytes));
            plan.arenaBytes = u32(offsetof(CapacityExtension, arena_bytes));
            plan.labelRegionBytes = u32(offsetof(CapacityExtension, label_region_bytes));
            plan.labelAlign = qMax(1, u16(offsetof(CapacityExtension, label_align)));
            plan.refBytes = u16(offsetof(CapacityExtension, ref_bytes));
            for (int i = 0; i < qMin(count, CAPACITY_MAX_TABLES); ++i)
                plan.ceiling.append(u16(offsetof(CapacityExtension, ceiling) + size_t(i) * 2));
            plan.measureCount = q[offsetof(CapacityExtension, measure_count)];
            const int stages =
                qMin(int(q[offsetof(CapacityExtension, stage_count)]), CAPACITY_MAX_STAGES);
            for (int i = 0; i < stages; ++i)
                plan.stageTables.append(q[offsetof(CapacityExtension, stage_table) + size_t(i)]);
            const int slices =
                qMin(int(q[offsetof(CapacityExtension, slice_count)]), CAPACITY_MAX_SLICES);
            for (int i = 0; i < slices; ++i) {
                const size_t s = offsetof(CapacityExtension, slice) + size_t(i) * sizeof(CapacitySlice);
                plan.slices.append(PlanBudget::Slice{q[s], qMax(1, int(q[s + 1])), u16(s + 2)});
            }
            if (size >= int(sizeof(CapacityExtension)) && (plan.flags & CAPACITY_PLAN_TX_QUEUES)) {
                plan.txQueueFloor = u16(offsetof(CapacityExtension, tx_queue_floor));
                plan.txQueueEntryOverhead = q[offsetof(CapacityExtension, tx_queue_entry_overhead)];
            } else {
                plan.flags &= ~int(CAPACITY_PLAN_TX_QUEUES); // stated without its numbers: as none
            }
            if (!out->plansTables())
                out->plan = PlanBudget{}; // stated, but not taken: as none
        }
    }
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

QString retainedIntervalText(int ms)
{
    if (ms >= 60000 && ms % 60000 == 0) {
        const int minutes = ms / 60000;
        return minutes == 1 ? QCoreApplication::translate("DeviceCapacity", "minute")
                            : QCoreApplication::translate("DeviceCapacity", "%1 minutes").arg(minutes);
    }
    return QCoreApplication::translate("DeviceCapacity", "%1 s")
        .arg(QString::number(ms / 1000.0, 'g', 3));
}

} // namespace ct
