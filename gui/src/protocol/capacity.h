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

    int capacityOf(DeviceTable table) const;
    int itemSizeOf(DeviceTable table) const;

    // Same store version, same table count, same capacity and record size in
    // every slot: a configuration mapped against one fits the other exactly.
    bool sameLayout(const DeviceCapacity &other) const;

    // The shape a .ct3 body holds: {"label", "storeVersion", "tables":
    // [[capacity, itemSize], ...]} in DeviceTable order. fromJson yields a
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
