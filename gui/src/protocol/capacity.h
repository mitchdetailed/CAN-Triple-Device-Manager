// What a firmware can hold: the capacity report, parsed.
//
// One parser for two sources. A running unit answers CMD_GET_CAPACITY with a
// CapacityReportHeader and table_count entries; a .ctf carries the identical
// bytes at FW_CAPS_OFFSET behind FW_CAPS_MAGIC. Both reach parseCapacityReport
// as the bytes from the header onward, so what the Manager believes about a
// file it is about to install and about the unit it is talking to comes
// through one function — the two cannot be read differently by accident.
//
// Deliberately free of DeviceLink and of the model: protocol/ code reads it
// off a link (device_session) or a file (FirmwareImage), and the model will
// consume it to size its checks, so it must depend on nothing heavier than
// wire_structs.h.
#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include "wire_structs.h"

namespace ct {

// What a unit that does not state its retained values keeps: the CAN Triple
// 1.x's flash ring (PRESERVE_MAX in its preserve_store.h, held against it in
// test_firmware_link), flushed once a minute.
constexpr int kRetainedValuesBuiltIn = 20;
constexpr int kRetainedIntervalBuiltInMs = 60000;

struct TableCapacity {
    int capacity = 0; // records the firmware holds; 0 = it has no such table
    int itemSize = 0; // bytes per record on the wire
};

// The PLAN (the CAN Triple 2.0, phase 4): how many records each table of a
// configuration holds is the configuration's own choice, declared by its Send
// (CMD_STAGE_PLAN), and the unit's capacity report says what it may declare
// and what it costs (the plan block of CapacityExtension; the arithmetic is
// the 2.0 protocol.h's "The plan's cost"). Empty — flags 0 — on every unit and
// image that takes no plan.
struct PlanBudget {
    struct Slice {
        int measure = 0; // ARENA_MEASURE_*
        int align = 1;   // the slice starts on a multiple of this
        int bytes = 0;   // an entry
        bool operator==(const Slice &o) const
        {
            return measure == o.measure && align == o.align && bytes == o.bytes;
        }
    };
    int flags = 0;               // CAPACITY_PLAN_*
    int headerBytes = 0;         // a configuration's header, ahead of its tables
    qint64 flashBytes = 0;       // the most its header and tables may take
    qint64 arenaBytes = 0;       // the engine's RAM, for slices and references, and
                                 // (CAPACITY_PLAN_TX_QUEUES) the transmit queues
    qint64 labelRegionBytes = 0; // the room for names
    int labelAlign = 1;          // a kind's run of names starts on a multiple of this
    int refBytes = 0;            // a dependency reference
    QVector<int> ceiling;        // the most a plan may declare of each table, in order
    int measureCount = 0;        // the measures the slices may name
    QVector<int> stageTables;    // the dependency stages' tables
    QVector<Slice> slices;       // the engine's RAM, in carving order
    // With CAPACITY_PLAN_TX_QUEUES (phase 4, step 4c): what each bus's transmit
    // queue takes at least, and what a queued frame takes beside its data.
    int txQueueFloor = 0;
    int txQueueEntryOverhead = 0;

    bool operator==(const PlanBudget &o) const
    {
        return flags == o.flags && headerBytes == o.headerBytes && flashBytes == o.flashBytes
               && arenaBytes == o.arenaBytes && labelRegionBytes == o.labelRegionBytes
               && labelAlign == o.labelAlign && refBytes == o.refBytes && ceiling == o.ceiling
               && measureCount == o.measureCount && stageTables == o.stageTables
               && slices == o.slices && txQueueFloor == o.txQueueFloor
               && txQueueEntryOverhead == o.txQueueEntryOverhead;
    }
    bool operator!=(const PlanBudget &o) const { return !(*this == o); }
};

// What a plan costs a unit, worked out as the unit works it out (its answer to
// the plan's ask, PlanCost): the configuration flash its header and tables
// take, the engine RAM its slices take, the room its names take, and whether a
// Send declaring it would be taken. The dependency references come on top of
// the slices at the SAVE (DeviceCapacity::referencesFit).
struct PlanCostResult {
    quint8 verdict = 0;    // 0 (taken), ERR_OUT_OF_BOUNDS (not well formed), ERR_NO_ROOM
    int shortOf = 0;       // PLAN_SHORT_*: the rooms it overruns
    qint64 flashBytes = 0; // its header and tables
    qint64 arenaBytes = 0; // its slices
    qint64 labelBytes = 0; // its names
    // False when the report names a measure this build does not know: the RAM
    // is then not worked out (arenaBytes 0, never short), and only the unit can
    // say. A newer firmware's report can do that; the unit still refuses what
    // does not fit, before anything of the running configuration changes.
    bool costed = true;
    bool fits() const { return verdict == 0; }
};

struct DeviceCapacity {
    quint16 storeVersion = 0;
    // Indexed by DeviceTable. May run PAST DeviceTable::Crc8 on firmware that
    // has appended a table this build does not know, and may in principle
    // stop short of it — capacityOf() answers 0 for either, so a consumer
    // asking about a table the firmware lacks is told it holds none.
    QVector<TableCapacity> tables;
    // True when the numbers came from a chosen source — a unit's report, or a
    // firmware image — rather than being this build's assumption. False for
    // builtIn(). A document whose capacity is `reported` records it in the
    // file (Configuration); one that is not carries nothing and loads as
    // builtIn() again, which is what it always was.
    bool reported = false;
    // Where the numbers came from, for people: "device on COM19",
    // "can-triple-1.0.10.ctf (firmware 1.0.10)". Empty for builtIn(). Not
    // part of sameLayout(): two sources describing one layout ARE one layout.
    QString label;

    // Retained values: the counters and integrators whose value the unit keeps
    // across power cycles (Preserve). Stated by the 2.0 line's report (format
    // 2); a format-1 report, and builtIn(), state nothing, and such a unit keeps
    // at most kRetainedValuesBuiltIn in a flash store written once a minute.
    // Not part of sameLayout(): no table moves for them.
    int retainedValues = 0;     // 0: not stated
    int retainedIntervalMs = 0; // 0: not stated
    bool retainedNoWear = false;
    // Any channel can be kept, not only counters and integrators
    // (CAPACITY_RETAINED_CHANNELS: a CAN Triple 2.0 from phase 4).
    bool retainedChannels = false;
    bool retainsChannels() const { return retainedChannels; }

    int retainedLimit() const
    {
        return retainedValues > 0 ? retainedValues : kRetainedValuesBuiltIn;
    }
    int retainedEveryMs() const
    {
        return retainedIntervalMs > 0 ? retainedIntervalMs : kRetainedIntervalBuiltInMs;
    }
    // Writing them wears the unit's store (flash), so a value that changes all
    // the time costs erases: the CAN Triple 1.x.
    bool retainedWearLimited() const { return !retainedNoWear; }
    bool sameRetained(const DeviceCapacity &other) const
    {
        return retainedValues == other.retainedValues
               && retainedIntervalMs == other.retainedIntervalMs
               && retainedNoWear == other.retainedNoWear
               && retainedChannels == other.retainedChannels;
    }

    // The script cost model: the tick budget in cost units, and what each
    // opcode costs from 0 up (one byte each), as a CAN Triple 2.0 states them.
    // Not stated (0, empty) by a format-1 report, builtIn() or the first 2.0
    // builds, whose units charge as the CAN Triple 1.x does: the table and the
    // budget compiled into the VM (script_vm.c, SCRIPT_TICK_BUDGET).
    // ScriptCostScope (script_simulator.h) puts a capacity's model in force
    // for the compiler, the disassembler and the simulator. Not part of
    // sameLayout().
    int scriptBudget = 0;
    QByteArray scriptCosts;
    bool sameScriptModel(const DeviceCapacity &other) const
    {
        return scriptBudget == other.scriptBudget && scriptCosts == other.scriptCosts;
    }

    // Transmit CRC8 beyond the single-byte elements every firmware takes:
    // CAPACITY_CRC8_* bits, as a CAN Triple 2.0 states them. None (0) from a
    // format-1 report, builtIn() or the 2.0 builds before them. A unit that
    // states any of them also takes CRC8 byte positions past 7 — its CAN FD
    // frames' — which the Manager offers the CAN Triple 1.x none of. Not part
    // of sameLayout(): no table moves for them.
    int crc8Features = 0;
    bool crc8WholeId() const { return (crc8Features & CAPACITY_CRC8_WHOLE_ID) != 0; }
    bool crc8DataRuns() const { return (crc8Features & CAPACITY_CRC8_DATA_RUNS) != 0; }
    // Long rules (a CAN Triple 2.0 from its store v22): a record holds
    // CRC8_V2_RECORD_ELEMENTS elements and a rule up to CRC8_MAX_RULE_ELEMENTS,
    // in continuation records after its own. Elsewhere a rule is one record of
    // up to CRC8_MAX_ELEMENTS.
    bool crc8LongRules() const { return (crc8Features & CAPACITY_CRC8_LONG_RULES) != 0; }
    int crc8RecordElements() const
    {
        return crc8LongRules() ? CRC8_V2_RECORD_ELEMENTS : CRC8_MAX_ELEMENTS;
    }
    int crc8MaxElements() const { return crc8LongRules() ? CRC8_MAX_RULE_ELEMENTS : CRC8_MAX_ELEMENTS; }
    // The highest frame byte a CRC8 rule may name: its location, a Data element
    // or either end of a run.
    int crc8MaxByte() const { return crc8Features != 0 ? 63 : 7; }
    bool sameCrc8(const DeviceCapacity &other) const
    {
        return crc8Features == other.crc8Features;
    }

    // The unit's CAN buses beyond the CAN Triple 1.x's: CAPACITY_CAN_* bits,
    // as a CAN Triple 2.0 states them. None (0) from a format-1 report,
    // builtIn() or the 2.0 builds before them. Not part of sameLayout().
    int canFeatures = 0;
    // CAN FD data phases of 4, 5 and 8 Mbit/s; without it, 1 and 2 only.
    bool fastData() const { return (canFeatures & CAPACITY_CAN_FAST_DATA) != 0; }
    // A CAN FD transmit message may go without the bit rate switch (its BRS box);
    // without it, every FD frame switches.
    bool brsPerMessage() const { return (canFeatures & CAPACITY_CAN_BRS_PER_MESSAGE) != 0; }
    // The unit makes UDS requests (DeviceTable::Uds, a CAN Triple 2.0 from
    // firmware 2.0.4): stated, and with a table to hold them.
    bool udsRequests() const
    {
        return (canFeatures & CAPACITY_CAN_UDS_REQUESTS) != 0 && limitOf(DeviceTable::Uds) > 0;
    }
    bool sameCan(const DeviceCapacity &other) const { return canFeatures == other.canFeatures; }

    // Where the unit keeps names. LABEL_STORE_BYTES on a CAN Triple 2.0 whose
    // names are in its label store: its message, signal and relay records are
    // then the ones without labels (V2_* in wire_structs.h), and every name is
    // written and read by CMD_WRITE_LABELS / CMD_READ_LABELS. 0 from a format-1
    // report, builtIn() and the 2.0 builds before it, whose records carry them.
    int labelBytes = 0;
    bool labelsApart() const { return labelBytes > 0; }
    // The longest name, in UTF-8 bytes, the unit keeps whole.
    int channelNameBytes() const { return labelsApart() ? labelBytes : MAX_CHANNEL_NAME_BYTES; }
    int messageNameBytes() const { return labelsApart() ? labelBytes : MAX_MESSAGE_NAME_BYTES; }
    // What the label store keeps beside the names (phase 4, step 3b):
    // LABEL_FLAG_* from a CAN Triple 2.0's report, 0 everywhere else. A unit
    // that keeps units has a unit for each channel (LABEL_KIND_UNIT), one that
    // keeps value names as many as a Send declares (LABEL_KIND_VALUE_NAME), and
    // both come out of its room for names (planCost).
    int labelFlags = 0;
    bool keepsUnits() const { return labelsApart() && (labelFlags & LABEL_FLAG_UNITS) != 0; }
    bool keepsValueNames() const
    {
        return keepsUnits() && (labelFlags & LABEL_FLAG_VALUE_NAMES) != 0;
    }
    // Config Notes: the configuration's notes in its label store, as many slots
    // as a Send declares (LABEL_KIND_NOTE), from its room for names too.
    bool keepsNotes() const { return keepsValueNames() && (labelFlags & LABEL_FLAG_NOTES) != 0; }
    bool sameLabels(const DeviceCapacity &other) const
    {
        return labelBytes == other.labelBytes && labelFlags == other.labelFlags;
    }

    // The most a stored project may be, header included (CMD_WRITE_PROJECT):
    // PROJECT_BYTES on a CAN Triple 2.0 that keeps the document a
    // configuration was sent from beside it, 0 everywhere else. A fact about
    // the unit, not about what a configuration can do, so it is no part of
    // sameTarget() and a document's target never records it: a Send asks the
    // unit it is about to write.
    int projectBytes = 0;
    bool keepsProject() const { return projectBytes > 0; }

    // What kind of build stated this: CAPACITY_BUILD_* (a CAN Triple 2.0's, from
    // the unit or from a .ctf's block). A fact about the image, not about what
    // a configuration can do, so no part of sameTarget(). productionBuild(): it
    // sets the unit's readout protection the first time it starts.
    int buildFlags = 0;
    bool productionBuild() const { return (buildFlags & CAPACITY_BUILD_PRODUCTION) != 0; }

    // The plan (PlanBudget): stated by a CAN Triple 2.0 from phase 4 on, empty
    // everywhere else. A target that takes one sizes a configuration by its
    // ceilings and its budget rather than by the entries above, which are then
    // only the DEFAULT plan, what a Send that declares none is laid out with.
    // Part of sameTarget(): the budget is what a configuration is judged by.
    PlanBudget plan;
    bool plansTables() const { return (plan.flags & CAPACITY_PLAN_STAGE) != 0; }
    // The unit reads a channel's message index in twelve bits (wire_structs.h,
    // SIG_MSG_HI_*): it came with the plan, in the same store version.
    bool wideMessageIndex() const { return plansTables(); }
    bool samePlan(const DeviceCapacity &other) const { return plan == other.plan; }
    // The most of a table a configuration may have on this target: its
    // ceiling on one that takes a plan (where the budget decides the rest:
    // planCost), its capacity otherwise.
    int limitOf(DeviceTable table) const;
    // What a plan of `capacities` (records per table, DeviceTable order) costs
    // this target, and whether a Send declaring it would be taken. For a target
    // that takes no plan, the cost of nothing: verdict 0. `valueNames`: the
    // value names its names take as well, on a target that keeps them (step 3b);
    // `notes`: the notes' slots, on one that keeps them (Config Notes).
    PlanCostResult planCost(const QVector<int> &capacities, int valueNames = 0,
                            int notes = 0) const;
    // Whether a configuration whose slices take `sliceBytes` of the engine's
    // RAM, and which makes `references` dependency references, fits that RAM
    // at its SAVE: the slices rounded up to 2, then the references (fewer than
    // 65,535 of them), then on a unit whose transmit queues are in that RAM
    // (holdsTxQueues), from there rounded up to 4, each bus's queue at its
    // floor. What is left of the RAM goes to the queues too, but a SAVE needs
    // no more of it.
    // `udsBytes`: the UDS requests' buffers (planUdsBytes), after the references
    // and before the queues' floors, like them counted at the SAVE.
    qint64 arenaWithReferences(qint64 sliceBytes, qint64 references, qint64 udsBytes = 0) const;
    bool referencesFit(qint64 sliceBytes, qint64 references, qint64 udsBytes = 0) const;
    // The transmit queues are in the engine's RAM (phase 4, step 4c; the 2.0
    // protocol.h, "The transmit queues"): a plan is declared, and a SAVE
    // checked, with plan.txQueueFloor for each of the three buses (planCost,
    // arenaWithReferences).
    bool holdsTxQueues() const { return (plan.flags & CAPACITY_PLAN_TX_QUEUES) != 0; }

    // The same firmware as far as a configuration can tell: the same layout,
    // and every feature stated the same. `label` and `reported` say where the
    // numbers came from, not what they are, so they are no part of it.
    bool sameTarget(const DeviceCapacity &other) const
    {
        return sameLayout(other) && sameRetained(other) && sameScriptModel(other)
               && sameCrc8(other) && sameCan(other) && sameLabels(other) && samePlan(other);
    }

    int capacityOf(DeviceTable table) const;
    int itemSizeOf(DeviceTable table) const;

    // Same store version, same table count, same capacity and record size in
    // every slot: a configuration mapped against one fits the other exactly.
    // Between two targets that take a plan, the capacities are only their
    // default plans and a stored configuration carries its own (its layout
    // identity covers the record sizes alone), so the sizes are what is
    // compared there.
    bool sameLayout(const DeviceCapacity &other) const;

    // The shape a .ct3 body holds: {"label", "storeVersion", "tables":
    // [[capacity, itemSize], ...]} in DeviceTable order, then "retained",
    // "script" (budget, costs), "crc8" (features) and "plan" (the PlanBudget)
    // where the source stated them. fromJson yields a
    // `reported` capacity — a file only ever records a chosen one — and
    // refuses an object with no tables, which is not a target at all.
    QJsonObject toJson() const;
    static bool fromJson(const QJsonObject &object, DeviceCapacity *out);

    // The numbers this build's wire_structs.h carries — what every firmware
    // before the report held, and the fallback when a unit or an image cannot
    // say. The STANDARD firmware answers exactly this (test_firmware_link
    // holds its reply to it); a variant (firmware 1.0.11, include/variant.h)
    // answers its own numbers, which is the whole reason the report exists.
    static DeviceCapacity builtIn();
};

// How `to` lays its tables out differently from `from`, one line per table
// that differs ("CRC8 rules: 20 → 40", "script chunks: 384 → 320"), plus one
// for a store-version change; empty when the two are the same layout. For the
// sentence that tells a user a firmware update will not keep the stored
// configuration: a unit refuses an image written under another layout, so
// "which tables changed" is the whole explanation of why it comes back empty.
QStringList layoutDifferences(const DeviceCapacity &from, const DeviceCapacity &to);

// The table's name as a user reads it — "messages", "channels", "CRC8 rules"
// — for the sentences that say a configuration does not fit a unit.
QString deviceTableName(DeviceTable table);

// How often retained values are written, as a user reads it: "0.1 s", "2 s",
// "minute", "5 minutes" — for "written every %1".
QString retainedIntervalText(int ms);

// Which of `counts` — records per table, indexed by DeviceTable — a unit of
// capacity `device` cannot hold, one sentence each ("40 CRC8 rules, but this
// device holds 20"); empty when everything fits. The one comparison behind
// both a Send's fit check (device_mapper's tablesExceeding, over mapped
// tables) and a sealed package's install verdict (over the counts its policy
// recorded), so the two cannot judge the same numbers differently.
//
// On a unit that takes a plan the counts ARE the plan a Send declares: each
// is held against its table's ceiling, and then the whole against the unit's
// configuration flash, engine RAM and room for names (planCost). With
// `references` (the configuration's dependency references, plan_budget.h)
// the RAM is judged as the unit's SAVE judges it; -1 leaves that to the unit.
// `valueNames`: the value names the configuration's names take as well, on a
// unit that keeps them (phase 4, step 3b; plan_budget.h valueNamesFor), and
// `notes` the notes' slots on one that keeps them (notesSlotsFor).
QStringList countsExceeding(const QVector<int> &counts, const DeviceCapacity &device,
                            qint64 references = -1, int valueNames = 0, int notes = 0,
                            qint64 udsBytes = 0);

// The plan a Send of `counts` (records per table, DeviceTable order) declares:
// the counts, with the tables that must agree made to — the 2x16 Out table as
// long as its Def table, and eight 8x8 rows for each 8x8 table, whichever of a
// pair is longer deciding. The mapper writes them agreeing already; tables a
// Get read from a unit are taken as they come. Shared by the fit check
// (countsExceeding) and the Send's CMD_STAGE_PLAN, so the plan judged is the
// plan declared.
QVector<int> planCapacities(QVector<int> counts);

// Parse a report from its first byte (the CapacityReportHeader). Tolerates
// trailing bytes — an image has code after the block — and refuses a format
// this build does not read, a count the bytes cannot hold, or nothing at all.
// On success `reported` is set.
bool parseCapacityReport(const QByteArray &bytes, DeviceCapacity *out);

} // namespace ct
