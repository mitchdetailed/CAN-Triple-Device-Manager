// The Manager's plan arithmetic against the CAN Triple 2.0 unit's own.
//
// A Send to a unit that takes a plan is judged twice: first by the Manager (the
// fit check: DeviceCapacity::planCost over the unit's capacity report, and
// planReferences over the records), then by the unit (CMD_STAGE_PLAN, and its
// SAVE's count of the references). The two must agree, or the Manager refuses
// what the unit would take, or lets through what the unit then refuses with
// the Send under way. So the unit's own answers are asked here, from its real
// store and engine compiled for the host (v2_plan_oracle.c):
//   - its capacity report, read by the Manager's parser: the plan block read
//     right, a target that takes a plan and reads twelve-bit indices;
//   - 20,000 random plans: the verdict, configuration flash, RAM and names the
//     Manager works out from the report, against what the unit answers;
//   - 60 random configurations of every record that names a channel, and of
//     messages: the working memory the Manager works out for the SAVE (the
//     slices, the references it counts and, step 4c, the transmit queues),
//     against what the unit's SAVE counts;
//   - the sentences the fit check gives a plan over each room;
//   - the status bar's figures (memoryUseOf) for those 60 configurations: the
//     RAM the unit's SAVE needs and the flash its ask answers for their plan.
#include <QCoreApplication>
#include <QJsonObject>

#include <cstdio>
#include <cstring>

#include "../src/model/device_mapper.h"
#include "../src/model/plan_budget.h"
#include "../src/protocol/capacity.h"

extern "C" {
const unsigned char *v2o_report(unsigned *size);
unsigned v2o_table_count(void);
unsigned v2o_plan_verdict(const unsigned short *cap, unsigned *flash, unsigned *arena);
unsigned v2o_plan_names(const unsigned short *cap, unsigned valueNames, unsigned notes);
long v2o_save_need(unsigned short channels, const void *constants, unsigned nk, const void *t2x16,
                   unsigned n2, const void *t8x8, unsigned n8, const void *math, unsigned nm,
                   const void *counters, unsigned nc, const void *conditions, unsigned nd,
                   const void *messages, unsigned nmsg, unsigned long *slices);
long v2o_retained(unsigned short channels, const unsigned char *flagged, const void *counters,
                  unsigned nc, const void *integrators, unsigned ni);
}

using namespace ct;

namespace {

int fails = 0;

#define CHECK(cond, what)                                                        \
    do {                                                                         \
        if (cond) {                                                              \
            std::printf("  PASS  %s\n", what);                                   \
        } else {                                                                 \
            std::printf("  FAIL  %s  (%s:%d)\n", what, __FILE__, __LINE__);      \
            ++fails;                                                             \
        }                                                                        \
    } while (0)

quint32 g_prng = 0x2C2026u;
quint32 prand()
{
    quint32 x = g_prng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return g_prng = x;
}
quint32 below(quint32 n) { return n ? prand() % n : 0u; }

DeviceCapacity unitReport()
{
    unsigned size = 0;
    const unsigned char *bytes = v2o_report(&size);
    DeviceCapacity cap;
    parseCapacityReport(QByteArray(reinterpret_cast<const char *>(bytes), int(size)), &cap);
    return cap;
}

void testReport()
{
    std::printf("the unit's capacity report, read by the Manager\n");
    const DeviceCapacity cap = unitReport();
    CHECK(cap.reported && cap.plansTables(), "it takes a plan");
    CHECK(cap.keepsUnits() && cap.keepsValueNames(), "and keeps units and value names (step 3b)");
    CHECK(cap.wideMessageIndex(), "and reads a channel's message in twelve bits");
    CHECK(int(cap.tables.size()) == int(v2o_table_count())
              && int(cap.plan.ceiling.size()) == int(v2o_table_count()),
          "an entry and a ceiling for each of its tables");
    CHECK(cap.plan.headerBytes == 512 && cap.plan.arenaBytes == 84992 && cap.plan.refBytes == 2,
          "a 512-byte header, an 83 KB arena, 2-byte references");
    CHECK(cap.plan.flashBytes == 229296 && cap.plan.labelRegionBytes == 786432
              && cap.plan.labelAlign == 4096 && cap.labelBytes == 32,
          "229,296 bytes of configuration flash, 768 KB of names on 4,096");
    CHECK(cap.plan.measureCount == ARENA_MEASURE_COUNT && !cap.plan.slices.isEmpty()
              && cap.plan.stageTables.size() == 6,
          "the measures this build knows, its slices and six dependency stages");
    CHECK(cap.limitOf(DeviceTable::Messages) == SIG_MSG_MAX
              && cap.limitOf(DeviceTable::Signals) == 65534
              && cap.limitOf(DeviceTable::Conditions) == 4095
              && cap.limitOf(DeviceTable::Tables8x8Row) == 4095 * TABLE_8X8_SITES
              && cap.limitOf(DeviceTable::Crc8) == 65534,
          "its limits are the ceilings: 4,094 messages, 65,534 channels, 4,095 rows, 65,534 "
          "CRC8 records");
    // The default plan: what every Send that declares none is laid out with.
    QVector<int> def;
    for (const TableCapacity &t : cap.tables)
        def.append(t.capacity);
    const PlanCostResult d = cap.planCost(def);
    CHECK(d.fits() && d.costed && d.flashBytes == 102848 && d.arenaBytes == 32863,
          "its default plan: 102,848 bytes of flash and 32,863 of RAM, as the unit says");
    // Kept in a document's target.
    DeviceCapacity back;
    CHECK(DeviceCapacity::fromJson(cap.toJson(), &back) && back.plan == cap.plan
              && back.sameTarget(cap),
          "a document's target keeps the plan, and is the same target read back");
    DeviceCapacity other = cap;
    other.plan.arenaBytes -= 4;
    CHECK(!other.sameTarget(cap), "a target whose budget differs is another target");
    DeviceCapacity bigger = cap;
    bigger.tables[int(DeviceTable::Messages)].capacity += 100;
    CHECK(bigger.sameLayout(cap) && layoutDifferences(cap, bigger).isEmpty(),
          "between two that take a plan, a different default plan is the same layout");
    bigger.tables[int(DeviceTable::Messages)].itemSize += 2;
    CHECK(!bigger.sameLayout(cap) && layoutDifferences(cap, bigger).size() == 1,
          "a different record is not");
    DeviceCapacity narrow = DeviceCapacity::builtIn();
    CHECK(!narrow.plansTables() && !narrow.wideMessageIndex()
              && narrow.limitOf(DeviceTable::Messages) == MAX_MESSAGES,
          "a 1.x target takes no plan, reads nine bits and holds its capacities");
    CHECK(narrow.planCost(def).fits(), "and a plan costs it nothing");
    // This unit carves its slices widest alignment first, so none is ever
    // padded; a report that orders them otherwise is costed by the documented
    // rule all the same (protocol.h: each slice starts at the total rounded up
    // to its alignment).
    DeviceCapacity mixed = cap;
    mixed.plan.slices = {PlanBudget::Slice{ARENA_MEASURE_CHANNEL, 1, 1},
                         PlanBudget::Slice{ARENA_MEASURE_CHANNEL, 4, 4},
                         PlanBudget::Slice{ARENA_MEASURE_MESSAGE, 2, 1},
                         PlanBudget::Slice{ARENA_MEASURE_MESSAGE, 2, 2}};
    QVector<int> few(int(cap.tables.size()), 0);
    few[int(DeviceTable::Signals)] = 3;
    few[int(DeviceTable::Messages)] = 5;
    // 3 x 1 = 3; up to 4, then 3 x 4 = 16; 5 x 1 = 21; up to 22, then 5 x 2 = 32.
    CHECK(mixed.planCost(few).arenaBytes == 32,
          "a slice starts at the total rounded up to its alignment");
    // 33 up to 34, ten references to 54; the transmit queues from 56 (step 4c).
    CHECK(mixed.arenaWithReferences(33, 10) == 56 + 3 * qint64(mixed.plan.txQueueFloor),
          "and the references start at the slices rounded up to 2, the queues' floors after "
          "them on 4");
}

void testPlans()
{
    std::printf("20,000 random plans, costed by the Manager and by the unit\n");
    const DeviceCapacity cap = unitReport();
    const int n = int(cap.tables.size());
    int valid = 0, fit = 0, wrong = 0, names = 0;
    for (int k = 0; k < 20000; ++k) {
        QVector<int> plan(n);
        QVector<unsigned short> wire(n);
        for (int t = 0; t < n; ++t) {
            const quint32 def = quint32(cap.tables[t].capacity);
            const quint32 ceil = quint32(cap.plan.ceiling[t]);
            const quint32 pick = below(4);
            plan[t] = int(pick == 0   ? 0u
                          : pick == 1 ? below(def + 1)
                          : pick == 2 ? below(def * 4 + 1)
                                      : below(ceil + 2));
        }
        if (k & 1) { // half pair the split tables, as a Send's would
            plan[int(DeviceTable::Tables2x16Out)] = plan[int(DeviceTable::Tables2x16Def)];
            plan[int(DeviceTable::Tables8x8Def)] %= 512;
            plan[int(DeviceTable::Tables8x8Row)] =
                plan[int(DeviceTable::Tables8x8Def)] * TABLE_8X8_SITES;
        }
        for (int t = 0; t < n; ++t)
            wire[t] = static_cast<unsigned short>(plan[t]);
        // Step 3b: value names beside the tables, none, a few or many.
        const quint32 vpick = below(3);
        const int valueNames = int(vpick == 0 ? 0u : vpick == 1 ? below(200) : below(30000));
        // Config Notes: none, a few slots, or all 16 KB.
        const quint32 npick = below(3);
        const int notes = int(npick == 0 ? 0u : npick == 1 ? below(40) : quint32(LABEL_NOTE_SLOTS_MAX));
        unsigned flash = 0, arena = 0;
        const unsigned verdict = v2o_plan_verdict(wire.constData(), &flash, &arena);
        const PlanCostResult cost = cap.planCost(plan, valueNames, notes);
        // The unit's verdict leaves the names to its glue: the same verdict
        // without them.
        const bool shortNotNames = (cost.shortOf & (PLAN_SHORT_FLASH | PLAN_SHORT_ARENA)) != 0;
        const unsigned manager = cost.verdict == ERR_OUT_OF_BOUNDS ? ERR_OUT_OF_BOUNDS
                                 : shortNotNames                   ? ERR_NO_ROOM
                                                                   : 0u;
        if (manager != verdict || !cost.costed)
            ++wrong;
        if (qint64(v2o_plan_names(wire.constData(), unsigned(valueNames), unsigned(notes)))
            != cost.labelBytes)
            ++names;
        if (cost.verdict != ERR_OUT_OF_BOUNDS) {
            ++valid;
            if (qint64(flash) != cost.flashBytes || qint64(arena) != cost.arenaBytes)
                ++wrong;
            if (verdict == 0)
                ++fit;
        }
    }
    std::printf("  %d well formed, %d fitting; %d verdicts or sizes differ, %d names differ\n",
                valid, fit, wrong, names);
    CHECK(wrong == 0, "every verdict, flash and RAM figure the unit's");
    CHECK(names == 0, "every plan's names the unit's");
    CHECK(valid > 1000 && fit > 500, "over well-formed and fitting plans alike");
}

quint16 channel(int channels)
{
    // Below the plan's channels, past them, and unused.
    return below(10) == 0 ? quint16(0xFFFF) : quint16(below(quint32(channels) + 40));
}

void testReferences()
{
    std::printf("the working memory a SAVE needs (slices, references and transmit queues), "
                "worked out by the Manager and by the unit\n");
    unsigned size = 0;
    const unsigned char *report = v2o_report(&size);
    DeviceCapacity cap;
    CHECK(parseCapacityReport(QByteArray(reinterpret_cast<const char *>(report), int(size)), &cap)
              && cap.holdsTxQueues(),
          "the unit's report, with its transmit queues in its working memory");
    int wrong = 0, total = 0, queued = 0, statusWrong = 0;
    for (int round = 0; round < 60; ++round) {
        DeviceTables t;
        const int channels = 20 + int(below(300));
        // Messages of every kind: transmit and receive, active or not, on a
        // real bus or not, every length up to past 64.
        t.messages.resize(4 + int(below(400)));
        for (int i = 0; i < t.messages.size(); ++i) {
            CanMessageConfig &m = t.messages[i];
            m = CanMessageConfig{};
            m.can_id = 0x100u + quint32(i);
            m.flags = quint8((below(10) ? MSGFLAG_ACTIVE : 0) | (below(2) ? MSGFLAG_TRANSMIT : 0)
                             | (below(4) == 0 ? MSGFLAG_FD : 0));
            m.src_bus = quint8(below(5));
            m.dlc = quint8(below(72));
            m.period_ms = 100;
            m.tx_trigger_cond = 0xFFFF;
        }
        t.signalConfigs.resize(channels);
        t.constants.resize(int(below(61)));
        t.tables2x16Def.resize(int(below(21)));
        t.tables8x8Def.resize(int(below(21)));
        t.math.resize(int(below(81)));
        t.counters.resize(int(below(61)));
        t.conditions.resize(int(below(81)));
        for (ConstantConfig &k : t.constants) {
            k = ConstantConfig{};
            k.dest_signal_idx = channel(channels);
            k.is_active = quint8(below(2));
        }
        for (Table2x16Def &d : t.tables2x16Def) {
            d = Table2x16Def{};
            d.x_signal_idx = channel(channels);
            d.dest_signal_idx = channel(channels);
        }
        for (Table8x8Def &d : t.tables8x8Def) {
            d = Table8x8Def{};
            d.x_signal_idx = channel(channels);
            d.y_signal_idx = channel(channels);
            d.dest_signal_idx = channel(channels);
        }
        for (MathConfig &m : t.math) {
            m = MathConfig{};
            m.op = quint8(below(31));
            m.input_a_type = quint8(below(2));
            m.input_a_idx = channel(channels);
            m.input_b_type = quint8(below(2));
            m.input_b_idx = channel(channels);
            m.input_c_type = quint8(below(2));
            const quint16 c = channel(channels);
            m.input_c_val[0] = quint8(c & 0xFF);
            m.input_c_val[1] = quint8(c >> 8);
            m.dest_signal_idx = channel(channels);
        }
        for (CounterConfig &c : t.counters) {
            c = CounterConfig{};
            c.up_signal_idx = channel(channels);
            c.down_signal_idx = channel(channels);
            c.reset_signal_idx = channel(channels);
            c.enable_signal_idx = channel(channels);
            c.follow_signal_idx = channel(channels);
            c.dest_signal_idx = channel(channels);
            c.input_kinds =
                quint8(below(3) | (below(3) << 2) | (below(3) << 4) | (below(3) << 6));
        }
        for (ConditionConfig &d : t.conditions) {
            d = ConditionConfig{};
            for (int s = 0; s < COND_MAX_TERMS; ++s) {
                for (ConditionTerm *term : {&d.set_terms[s], &d.reset_terms[s]}) {
                    term->op = quint8(below(8));
                    term->input_a_signal_idx =
                        condOpIsMessage(term->op) ? quint16(below(4)) : channel(channels);
                    term->input_b_type = quint8(below(2));
                    if (term->input_b_type)
                        term->b.input_b_idx = channel(channels);
                }
            }
            d.set_count = quint8(1 + below(COND_MAX_TERMS));
            d.dest_signal_idx = channel(channels);
        }
        unsigned long slices = 0;
        const long unit = v2o_save_need(
            quint16(channels), t.constants.constData(), unsigned(t.constants.size()),
            t.tables2x16Def.constData(), unsigned(t.tables2x16Def.size()),
            t.tables8x8Def.constData(), unsigned(t.tables8x8Def.size()), t.math.constData(),
            unsigned(t.math.size()), t.counters.constData(), unsigned(t.counters.size()),
            t.conditions.constData(), unsigned(t.conditions.size()), t.messages.constData(),
            unsigned(t.messages.size()), &slices);
        const qint64 references = planReferences(t);
        // The transmit messages ask nothing of a SAVE: the queues' floors are
        // all it needs of them.
        const qint64 manager = cap.arenaWithReferences(qint64(slices), references);
        total += int(references);
        for (const CanMessageConfig &m : t.messages)
            if ((m.flags & MSGFLAG_ACTIVE) && (m.flags & MSGFLAG_TRANSMIT) && m.src_bus >= 1
                && m.src_bus <= 3)
                ++queued;
        if (unit < 0 || qint64(unit) != manager) {
            ++wrong;
            std::printf("  round %d: the unit's SAVE needs %ld bytes, the Manager says %lld\n",
                        round, unit, static_cast<long long>(manager));
        }
        // The status bar's figures for the same configuration: its RAM is what
        // the SAVE needs, its flash what the unit answers for the plan a Send
        // declares (planCapacities over the counts).
        t.capacity = cap;
        const MemoryUse use = memoryUseOf(t);
        const QVector<int> declared = planCapacities(tableCountsOf(t));
        QVector<unsigned short> plan(int(v2o_table_count()), 0);
        for (int i = 0; i < plan.size() && i < declared.size(); ++i)
            plan[i] = static_cast<unsigned short>(declared[i]);
        unsigned flash = 0, arena = 0;
        (void)v2o_plan_verdict(plan.constData(), &flash, &arena);
        if (!use.sized || !use.costed || unit < 0 || use.ramUsed != qint64(unit)
            || use.flashUsed != qint64(flash) || use.ramTotal != cap.plan.arenaBytes
            || use.flashTotal != cap.plan.flashBytes) {
            ++statusWrong;
            std::printf("  round %d: the status bar says flash %lld, RAM %lld; the unit %u, %ld\n",
                        round, static_cast<long long>(use.flashUsed),
                        static_cast<long long>(use.ramUsed), flash, unit);
        }
    }
    std::printf("  60 configurations, %d references and %d transmit messages in all\n", total,
                queued);
    CHECK(wrong == 0, "the Manager's working memory is the unit's SAVE's, byte for byte");
    CHECK(statusWrong == 0,
          "the status bar's flash and RAM are the unit's, byte for byte (memoryUseOf)");
    // Fixed tables: nothing to share out, so no figures.
    DeviceTables fixed;
    fixed.capacity = DeviceCapacity::builtIn();
    CHECK(!memoryUseOf(fixed).sized, "a target with fixed tables has no memory figures");
}

// Any channel kept across power cycles (step 3a): the values the Manager counts
// for the fit check against the ones the unit's own walk finds.
void testRetainedCount()
{
    std::printf("the values kept across power cycles, counted by the Manager and by the unit\n");
    int wrong = 0, total = 0;
    for (int round = 0; round < 40; ++round) {
        DeviceTables t;
        t.capacity.retainedChannels = true; // the unit keys its set by channel
        const int channels = 20 + int(below(300));
        t.signalConfigs.resize(channels);
        QVector<unsigned char> flagged(channels, 0);
        for (int s = 0; s < channels; ++s) {
            flagged[s] = below(8) == 0 ? 1 : 0;
            CanSignalConfig sig{};
            sigSetRetain(sig, flagged[s] != 0);
            t.signalConfigs[s] = sig;
        }
        t.counters.resize(int(below(61)));
        for (CounterConfig &c : t.counters) {
            c = CounterConfig{};
            c.up_signal_idx = c.down_signal_idx = c.follow_signal_idx = 0xFFFF;
            c.reset_signal_idx = c.enable_signal_idx = 0xFFFF;
            c.dest_signal_idx = below(10) == 0 ? quint16(channels + below(40)) : quint16(below(channels));
            c.step = 1.0f;
            c.rate_hz = 1;
            c.flags = quint8((below(4) ? COUNTERFLAG_ACTIVE : 0) | (below(2) ? COUNTERFLAG_PRESERVE : 0));
        }
        t.integrators.resize(int(below(9)));
        for (IntegratorConfig &g : t.integrators) {
            g = IntegratorConfig{};
            g.input_signal_idx = g.reset_signal_idx = g.enable_signal_idx = 0xFFFF;
            g.dest_signal_idx = quint16(below(channels));
            g.rate_hz = 1;
            g.flags = quint8((below(4) ? INTEGFLAG_ACTIVE : 0) | INTEGFLAG_CONST_INPUT
                             | (below(2) ? INTEGFLAG_PRESERVE : 0));
        }
        const long unit = v2o_retained(quint16(channels), flagged.constData(), t.counters.constData(),
                                       unsigned(t.counters.size()), t.integrators.constData(),
                                       unsigned(t.integrators.size()));
        const int manager = retainedValuesOf(t);
        total += manager;
        if (unit < 0 || unit != manager) {
            ++wrong;
            std::printf("  round %d: the unit keeps %ld, the Manager counts %d\n", round, unit, manager);
        }
    }
    std::printf("  40 configurations, %d values kept in all\n", total);
    CHECK(wrong == 0, "the Manager counts every value the unit keeps, and no other");
}

void testSentences()
{
    std::printf("what the fit check says of a plan over each room\n");
    const DeviceCapacity cap = unitReport();
    QVector<int> counts(DEVICE_TABLE_COUNT, 0);
    counts[int(DeviceTable::Messages)] = 1500;
    counts[int(DeviceTable::Signals)] = 400;
    CHECK(countsExceeding(counts, cap, 1000).isEmpty(),
          "1,500 messages, 400 channels and 1,000 references fit");
    counts[int(DeviceTable::Messages)] = 4095;
    const QStringList over = countsExceeding(counts, cap);
    CHECK(over.size() == 1 && over[0].contains(QStringLiteral("at most 4094")),
          "4,095 messages are past the ceiling, and say so");
    counts[int(DeviceTable::Messages)] = 1500;
    counts[int(DeviceTable::Conditions)] = 2000;
    const QStringList ram = countsExceeding(counts, cap);
    CHECK(ram.size() == 1 && ram[0].contains(QStringLiteral("working memory")),
          "2,000 conditions beside them overrun the working memory");
    counts[int(DeviceTable::Conditions)] = 0;
    counts[int(DeviceTable::Signals)] = 30000;
    const QStringList names = countsExceeding(counts, cap);
    bool namesSaid = false;
    for (const QString &s : names)
        namesSaid = namesSaid || s.contains(QStringLiteral("names need"));
    CHECK(namesSaid, "30,000 channels' names overrun the room for names");
    counts[int(DeviceTable::Signals)] = 400;
    const QStringList refs = countsExceeding(counts, cap, 40000);
    CHECK(refs.size() == 1 && refs[0].contains(QStringLiteral("links between")),
          "40,000 references overrun what the plan left, as the SAVE would say");
    QVector<int> unpaired(DEVICE_TABLE_COUNT, 0);
    unpaired[int(DeviceTable::Tables2x16Def)] = 3;
    unpaired[int(DeviceTable::Tables8x8Row)] = 17;
    const QVector<int> paired = planCapacities(unpaired);
    CHECK(paired[int(DeviceTable::Tables2x16Out)] == 3
              && paired[int(DeviceTable::Tables8x8Def)] == 3
              && paired[int(DeviceTable::Tables8x8Row)] == 24
              && cap.planCost(paired).fits(),
          "a Send's plan pairs its split tables, the longer of each pair deciding");
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    testReport();
    testPlans();
    testReferences();
    testRetainedCount();
    testSentences();
    if (fails == 0)
        std::printf("test_plan_v2: all checks passed\n");
    else
        std::printf("test_plan_v2: %d check(s) FAILED\n", fails);
    return fails == 0 ? 0 : 1;
}
