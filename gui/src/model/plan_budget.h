// What a configuration's records cost a CAN Triple 2.0 beyond their tables'
// capacities: the DEPENDENCY REFERENCES its engine keeps, from each channel to
// the rows that read or write it (the 2.0 protocol.h, "The plan's cost"). They
// take the engine's RAM after the plan's slices, and a unit refuses a Send
// whose references overrun what its plan left at its SAVE (ERR_NO_ROOM), so a
// Send counts them first (countsExceeding, capacity.h), the way the unit will.
//
// Free of the mapper and of the document: DeviceTables is a plain holder of
// records, so test_plan_v2 can hold this count against the 2.0 engine's own
// without linking either.
#pragma once

#include <QtGlobal>

namespace ct {

struct DeviceTables;
struct DeviceCapacity;

// One for each channel index below the configuration's channels (its signal
// records; an unused 0xFFFF is none) that a record names, by its kind's rule:
//
//   constant     its output
//   2x16 table   its x input, its output
//   8x8 table    its x and y inputs, its output
//   maths row    each of inputs a, b and c that is a channel, its output
//   counter      each of up, down, reset and enable that is a channel input
//                (COUNTER_SRC_SIGNAL), its follow input, its output
//   condition    each of its six term slots (three a side, used or not) that
//                is not a message term: its a input, and its b input when
//                that is a channel; its output
//
// Every record counts, active or not: the unit's dependency index is built
// over all of them.
qint64 planReferences(const DeviceTables &tables);
// The UDS requests' buffers (wire_structs.h, UDS REQUESTS): for each bus the
// largest request or kept response of its records, on 4 bytes, summed; the
// unit's own arithmetic (engine_core.c, udsBufferNeed). 0 without any.
qint64 planUdsBytes(const DeviceTables &tables);

// The values these records have a unit keep across power cycles (its FRAM,
// phase 4 step 3a), counted as the CAN Triple 2.0 walks them
// (engine_retained_next): every channel flagged SIG_BITS_RETAIN, then the
// output of each active counter and integrator set to Preserve whose channel is
// one of the configuration's and not flagged already. On tables mapped for a
// unit that keeps no channels, every active Preserve counter and integrator,
// as that unit keeps them by row.
int retainedValuesOf(const DeviceTables &tables);

// The value names a configuration's names take on a unit (phase 4, step 3b):
// those the tables carry (DeviceTables::valueNameLabels, mapped for every
// target) where `device` keeps value names, none elsewhere. valueNamesOf asks
// the tables' own target.
int valueNamesFor(const DeviceTables &tables, const DeviceCapacity &device);
int valueNamesOf(const DeviceTables &tables);

// The notes' slots (Config Notes): those the tables carry
// (DeviceTables::noteLabels, mapped for every target) where `device` keeps
// notes, none elsewhere; notesSlotsOf asks the tables' own target.
int notesSlotsFor(const DeviceTables &tables, const DeviceCapacity &device);
int notesSlotsOf(const DeviceTables &tables);

// What a configuration takes of its target's memory, beside what the target
// has: the main window's status bar. Worked out as the unit works it out, on
// the tables' own target (DeviceTables::capacity, the document's):
//
//   flash  the configuration storage its header and tables take
//          (DeviceCapacity::planCost), the script's chunks with them when the
//          tables carry them (mapWithScript's do);
//   RAM    the working memory its SAVE needs: the tables' slices, the
//          references (planReferences) and each bus's transmit queue at its
//          floor (DeviceCapacity::arenaWithReferences). What the unit has past
//          that goes to the transmit queues as well;
//   names  its names, and the units and value names a unit keeps with them;
//   kept   the values it keeps across power cycles (retainedValuesOf).
//
// `sized` is false on a target that takes no plan (a CAN Triple, a 2.0 before
// phase 4, this program's built-in numbers): fixed tables, a set number of
// each item, with nothing here to share out. `costed` is false when the target
// states its RAM in a measure this build cannot work out: ramUsed is then 0
// and only the unit can say.
struct MemoryUse {
    bool sized = false;
    bool costed = false;
    qint64 flashUsed = 0;
    qint64 flashTotal = 0;
    qint64 ramUsed = 0;
    qint64 ramTotal = 0;
    qint64 namesUsed = 0;
    qint64 namesTotal = 0;
    int keptUsed = 0;
    int keptTotal = 0;
};
MemoryUse memoryUseOf(const DeviceTables &tables);

} // namespace ct
