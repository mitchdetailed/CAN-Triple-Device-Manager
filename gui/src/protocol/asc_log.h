// Vector ASCII (.asc) CAN-log formatting, shared by the CAN Viewer's
// "Save to File" export and its host tests. Matches the MoTeC CAN Inspector
// layout so the files open in the same tools.
#pragma once

#include <QString>

#include "wire_structs.h"

class QDateTime;

namespace ct {

// The six-line .asc header block (each line terminated by '\n'), time-stamped
// `when`. Day/month names are forced to the C locale so the output is stable
// and matches MoTeC regardless of the host's regional settings.
QString ascHeader(const QDateTime &when);

// One .asc data line (no trailing newline) for a monitor-stream frame. The
// timestamp is taken relative to t0 (the device-uptime millisecond count of
// the first frame) so a log starts near zero.
QString ascFrameLine(const MonitorStreamPayload &frame, quint32 t0);

} // namespace ct
