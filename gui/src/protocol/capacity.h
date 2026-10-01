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
               && retainedNoWear == other.retainedNoWear;
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
    bool sameLabels(const DeviceCapacity &other) const { return labelBytes == other.labelBytes; }

    // The same firmware as far as a configuration can tell: the same layout,
    // and every feature stated the same. `label` and `reported` say where the
    // numbers came from, not what they are, so they are no part of it.
    bool sameTarget(const DeviceCapacity &other) const
    {
        return sameLayout(other) && sameRetained(other) && sameScriptModel(other)
               && sameCrc8(other) && sameCan(other) && sameLabels(other);
    }

    int capacityOf(DeviceTable table) const;
    int itemSizeOf(DeviceTable table) const;

    // Same store version, same table count, same capacity and record size in
    // every slot: a configuration mapped against one fits the other exactly.
    bool sameLayout(const DeviceCapacity &other) const;

    // The shape a .ct3 body holds: {"label", "storeVersion", "tables":
    // [[capacity, itemSize], ...]} in DeviceTable order, then "retained",
    // "script" (budget, costs) and "crc8" (features) where the source stated
    // them. fromJson yields a
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
QStringList countsExceeding(const QVector<int> &counts, const DeviceCapacity &device);

// Parse a report from its first byte (the CapacityReportHeader). Tolerates
// trailing bytes — an image has code after the block — and refuses a format
// this build does not read, a count the bytes cannot hold, or nothing at all.
// On success `reported` is set.
bool parseCapacityReport(const QByteArray &bytes, DeviceCapacity *out);

} // namespace ct
