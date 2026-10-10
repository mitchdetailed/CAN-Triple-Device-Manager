// A channel: a named quantity with fixed base resolution, units, and range.
// Scaling on comms rows converts raw CAN values into base-resolution counts;
// the channel defines what those counts mean.
#pragma once

#include <QJsonObject>
#include <QMap>
#include <QString>

namespace ct {

struct Channel {
    QString name;
    QString quantity = QStringLiteral("Unitless"); // measurement type (Temperature, Pressure, ...)
    QString unit;                                  // display unit, e.g. "°C"
    QString dataType;       // boolean/u8/u16/u32/s8/s16/s32/float; empty = not chosen yet
    double baseResolution = 1.0;                   // physical units per count
    int decimalPlaces = 0;
    double minValue = -1e9; // physical clamp range sent to the device
    double maxValue = 1e9;
    QString category = QStringLiteral("User Channels");
    bool userDefined = false;
    // Which firmware-published value this is (wire_structs.h DEVCH_*), or -1
    // for every user channel. This is the device channel's IDENTITY as far as
    // the mapper is concerned: the wire struct is an array indexed by it, so a
    // new device channel is an enum entry and a catalogue row rather than a new
    // named field and a new special case. Never serialised — device channels do
    // not live in the .ct3, and a document only ever stores the NAME.
    int deviceChannelId = -1;
    // VALUE NAMES: display labels for an enumerated channel's values — "Power
    // On" for 1 on Device Last Reset Reason, "Park" for 0 on a gear selector.
    // The places that show a live value render "label (value)" when the value
    // has a label, the bare number when it does not. A device channel's are
    // compiled in; a USER channel's (phase 4, step 3b) are edited in Edit
    // Custom Channel, imported from a DBC's VAL_ lines, saved in the .ct3 as
    // "valueNames" (only when there are any) and kept on a CAN Triple 2.0 that
    // keeps value names (LABEL_KIND_VALUE_NAME), so a Get brings them back.
    QMap<int, QString> enumLabels;

    bool isValid() const { return !name.isEmpty(); }

    QJsonObject toJson() const;
    static Channel fromJson(const QJsonObject &o);
};

// "Coolant Temp °C" — a channel's name with its unit, for DISPLAY.
//
// A name on its own says what a channel is called but not what it means: 90
// could be °C, kPa or percent, and the unit is exactly what a user has to hold
// in their head while wiring a value from one place to another. So the lists,
// trees and labels that show a channel show this instead of the bare name.
//
// NEVER STORE THE RESULT, and never look a channel up by it. The identity is
// `name` alone — that is what the catalogue is keyed by, what a comms row or a
// math input holds, and what goes into the .ct3 and the device image. A widget
// showing this must carry the bare name separately (Qt::UserRole, or a member)
// for whatever reads the selection back.
//
// A unitless channel gives back the name unchanged, with no trailing space.
inline QString channelLabel(const QString &name, const QString &unit)
{
    return unit.isEmpty() ? name : name + QLatin1Char(' ') + unit;
}
inline QString channelLabel(const Channel &c)
{
    return channelLabel(c.name, c.unit);
}

} // namespace ct
