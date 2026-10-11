#include "plan_budget.h"

#include "device_mapper.h" // DeviceTables

namespace ct {

qint64 planReferences(const DeviceTables &tables)
{
    const int channels = int(tables.signalConfigs.size());
    const auto ref = [channels](quint16 idx) -> qint64 { return int(idx) < channels ? 1 : 0; };
    qint64 n = 0;
    for (const ConstantConfig &k : tables.constants)
        n += ref(k.dest_signal_idx);
    for (const Table2x16Def &t : tables.tables2x16Def)
        n += ref(t.x_signal_idx) + ref(t.dest_signal_idx);
    for (const Table8x8Def &t : tables.tables8x8Def)
        n += ref(t.x_signal_idx) + ref(t.y_signal_idx) + ref(t.dest_signal_idx);
    for (const MathConfig &m : tables.math) {
        if (m.input_a_type == 1)
            n += ref(m.input_a_idx);
        if (m.input_b_type == 1)
            n += ref(m.input_b_idx);
        // Input c's channel is the first two bytes of its value (wire_structs.h).
        if (m.input_c_type == 1)
            n += ref(quint16(m.input_c_val[0] | (m.input_c_val[1] << 8)));
        n += ref(m.dest_signal_idx);
    }
    for (const CounterConfig &c : tables.counters) {
        // Up, down, reset and enable, each with its kind in two bits of
        // input_kinds, in that order.
        const quint16 inputs[4] = {c.up_signal_idx, c.down_signal_idx, c.reset_signal_idx,
                                   c.enable_signal_idx};
        for (int i = 0; i < 4; ++i)
            if (((c.input_kinds >> (2 * i)) & 3) == COUNTER_SRC_SIGNAL)
                n += ref(inputs[i]);
        n += ref(c.follow_signal_idx) + ref(c.dest_signal_idx);
    }
    for (const ConditionConfig &d : tables.conditions) {
        for (int t = 0; t < 2 * COND_MAX_TERMS; ++t) {
            const ConditionTerm term = t < COND_MAX_TERMS ? d.set_terms[t]
                                                          : d.reset_terms[t - COND_MAX_TERMS];
            if (condOpIsMessage(term.op))
                continue;
            n += ref(term.input_a_signal_idx);
            if (term.input_b_type == 1)
                n += ref(term.b.input_b_idx);
        }
        n += ref(d.dest_signal_idx);
    }
    return n;
}

int retainedValuesOf(const DeviceTables &tables)
{
    int n = 0;
    const int channels = int(tables.signalConfigs.size()); // not `signals`: a Qt keyword
    // The record's bit says "keep this channel" only to a unit that keeps
    // channels; on a CAN Triple it is a digit of a received identifier's mask.
    const bool keepsChannels = tables.capacity.retainsChannels();
    const auto flagged = [&](quint16 ch) {
        return keepsChannels && int(ch) < channels && sigRetain(tables.signalConfigs[ch]);
    };
    for (const CanSignalConfig &s : tables.signalConfigs)
        if (keepsChannels && sigRetain(s))
            ++n;
    // A unit that keys its set by channel (it keeps channels) passes over an
    // output past the channels, as there is no channel to keep; one that keys
    // it by counter and integrator (a CAN Triple, a 2.0 before phase 4) keeps
    // every flagged row.
    const bool byChannel = tables.capacity.retainsChannels();
    const auto kept = [&](quint16 dest) {
        return (!byChannel || int(dest) < channels) && !flagged(dest);
    };
    for (const CounterConfig &c : tables.counters)
        if ((c.flags & COUNTERFLAG_ACTIVE) && (c.flags & COUNTERFLAG_PRESERVE) && kept(c.dest_signal_idx))
            ++n;
    for (const IntegratorConfig &g : tables.integrators)
        if ((g.flags & INTEGFLAG_ACTIVE) && (g.flags & INTEGFLAG_PRESERVE) && kept(g.dest_signal_idx))
            ++n;
    return n;
}

qint64 planUdsBytes(const DeviceTables &tables)
{
    qint64 need[3] = {0, 0, 0};
    for (const UdsConfig &c : tables.uds) {
        if (!(c.flags & UDSFLAG_ACTIVE) || c.request_msg >= tables.messages.size())
            continue;
        const int bus = tables.messages[c.request_msg].src_bus;
        if (bus < 1 || bus > 3)
            continue;
        qint64 want = qMin(int(c.request_len), UDS_REQUEST_MAX);
        if (c.response_msg != SIG_MSG_NONE && !(c.flags & UDSFLAG_NO_RESPONSE)
            && c.response_max > want)
            want = c.response_max;
        want = (qMin<qint64>(want, UDS_RESPONSE_MAX) + 3) / 4 * 4;
        need[bus - 1] = qMax(need[bus - 1], want);
    }
    return need[0] + need[1] + need[2];
}

int valueNamesFor(const DeviceTables &tables, const DeviceCapacity &device)
{
    return device.keepsValueNames() ? int(tables.valueNameLabels.size()) : 0;
}

int valueNamesOf(const DeviceTables &tables)
{
    return valueNamesFor(tables, tables.capacity);
}

int notesSlotsFor(const DeviceTables &tables, const DeviceCapacity &device)
{
    return device.keepsNotes() ? int(tables.noteLabels.size()) : 0;
}

int notesSlotsOf(const DeviceTables &tables)
{
    return notesSlotsFor(tables, tables.capacity);
}

QVector<int> tableCountsOf(const DeviceTables &tables)
{
    QVector<int> counts(DEVICE_TABLE_COUNT, 0);
    // Slot by slot rather than a positional list: the index IS the table.
    counts[int(DeviceTable::Messages)] = int(tables.messages.size());
    counts[int(DeviceTable::Signals)] = int(tables.signalConfigs.size());
    counts[int(DeviceTable::Math)] = int(tables.math.size());
    counts[int(DeviceTable::Conditions)] = int(tables.conditions.size());
    counts[int(DeviceTable::Counters)] = int(tables.counters.size());
    counts[int(DeviceTable::Timers)] = int(tables.timers.size());
    counts[int(DeviceTable::Constants)] = int(tables.constants.size());
    counts[int(DeviceTable::Relays)] = int(tables.relays.size());
    counts[int(DeviceTable::Tables2x16Def)] = int(tables.tables2x16Def.size());
    counts[int(DeviceTable::Tables2x16Out)] = int(tables.tables2x16Out.size());
    counts[int(DeviceTable::Tables8x8Def)] = int(tables.tables8x8Def.size());
    counts[int(DeviceTable::Tables8x8Row)] = int(tables.tables8x8Row.size());
    counts[int(DeviceTable::Integrators)] = int(tables.integrators.size());
    counts[int(DeviceTable::Script)] = int(tables.scriptChunks.size());
    counts[int(DeviceTable::Crc8)] = int(tables.crc8.size());
    counts[int(DeviceTable::Uds)] = int(tables.uds.size());
    return counts;
}

MemoryUse memoryUseOf(const DeviceTables &tables)
{
    MemoryUse use;
    const DeviceCapacity &target = tables.capacity;
    if (!target.plansTables())
        return use;
    use.sized = true;
    // The plan a Send declares (config_transfer.cpp), costed by the report.
    const PlanCostResult cost =
        target.planCost(planCapacities(tableCountsOf(tables)), valueNamesOf(tables),
                        notesSlotsOf(tables));
    use.costed = cost.costed;
    use.flashUsed = cost.flashBytes;
    use.flashTotal = target.plan.flashBytes;
    use.ramUsed =
        cost.costed ? target.arenaWithReferences(cost.arenaBytes, planReferences(tables),
                                                 planUdsBytes(tables))
                    : 0;
    use.ramTotal = target.plan.arenaBytes;
    use.namesUsed = cost.labelBytes;
    use.namesTotal = target.plan.labelRegionBytes;
    use.keptUsed = retainedValuesOf(tables);
    use.keptTotal = target.retainedLimit();
    return use;
}

} // namespace ct
