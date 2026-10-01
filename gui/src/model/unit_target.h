// A Send to a unit that is not the document's target.
//
// A document is judged by its TARGET, the firmware it was made for (File →
// Target Firmware…): Check Channels, the name fields and the mapper all size
// and check it by that. A Send goes to whichever unit is on the cable. When
// that unit is not the target's firmware, what matters is what THIS
// configuration becomes there, so it is judged again with the unit as its
// target, and the difference between the two judgements is the answer:
//
//   problems  errors only the unit has: it cannot run something the document
//             uses (a CAN Triple 2.0's CRC8 elements or fast data rates on a
//             CAN Triple, say), so nothing may be sent;
//   changes   warnings only the unit has: what comes out differently there,
//             above all the names it keeps shorter, for the operator to see
//             before anything is sent.
//
// Both judgements are made on COPIES, and neither copy is ever sent: a copy
// does not carry everything a Send does (copyContentTo leaves the message
// passwords behind, on purpose). Judging both sides on copies means whatever a
// copy lacks it lacks twice, and the difference is the unit's alone. The Send
// is the document's own mapping, in the unit's record form (tablesForUnit).
#pragma once

#include <QString>
#include <QStringList>

#include <functional>

#include "../protocol/capacity.h"
#include "device_mapper.h"

namespace ct {

class Configuration;

struct UnitTargetCheck {
    bool differs = false; // the unit is not the firmware the document targets
    QStringList problems; // errors on the unit alone: refuse the Send
    QStringList changes;  // warnings on the unit alone: show them first
};

// `map` is the Send's own mapping: mapWithScript in the application (it needs
// Lua, which the model does not link), mapToDevice in a test. The document is
// left exactly as it was.
UnitTargetCheck checkUnitTarget(const Configuration &config, const DeviceCapacity &unit,
                                const std::function<MappingResult(const Configuration &)> &map);

// A target as the Target Firmware window names it: its label ("device on
// COM43", "can-triple-2.0.0.ctf (firmware 2.0.0)"), or this program's built-in
// numbers when no firmware was chosen.
QString targetName(const DeviceCapacity &target);

} // namespace ct
