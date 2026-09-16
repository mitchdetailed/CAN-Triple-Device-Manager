// The CAN Viewer's "Save to File…" formats. Three ways to write the same
// buffered frames:
//
//   Vector ASCII (.asc)     — the original export (asc_log.h), what CANalyzer,
//                             CANoe and most viewers open.
//   SocketCAN log (.log)    — the candump -l / canplayer line format, which
//                             python-can, SavvyCAN, cantools and every Linux
//                             CAN tool read, and the format the OEM captures
//                             this project has been compared against are in.
//   PEAK trace 3.0 (.trc)   — PCAN-View / PCAN-Explorer's own trace file,
//                             per PEAK's "CAN TRC File Format" document.
//
// One record per frame in every format, and the TIMESTAMPS ARE RELATIVE to
// the first buffered frame in every format, as the .asc export always was: the
// device reports uptime milliseconds, not wall-clock time, so a log starts
// near zero and a header carries the host's clock where the format has a
// place for one. Direction (Rx/Tx) is carried by .asc and .trc; the SocketCAN
// line format has no column for it, which is a fact about that format.
#pragma once

#include <QDateTime>
#include <QList>
#include <QString>

#include "asc_log.h"
#include "wire_structs.h"

namespace ct {

enum class CanLogFormat { VectorAsc, SocketCanLog, PeakTrc };

struct CanLogFormatInfo {
    CanLogFormat format;
    QString name;   // "Vector ASCII log"
    QString suffix; // "asc"
    QString filter() const; // "Vector ASCII log (*.asc)" — the QFileDialog entry
};

// In the order the save dialog offers them: Vector ASCII first, as before.
const QList<CanLogFormatInfo> &canLogFormats();
const CanLogFormatInfo &canLogFormatInfo(CanLogFormat format);
// The ";;"-joined filter list for QFileDialog::getSaveFileName.
QString canLogFilterString();

// The file name the save dialog suggests: "Logged Msgs 260915 - 1127.asc" —
// the date as yyMMdd and the time as HHmm, so a day's captures sort in the
// order they were taken, and the extension of the format being offered.
QString canLogSuggestedName(CanLogFormat format, const QDateTime &when);

// Which format a save dialog's outcome asks for. The EXTENSION TYPED wins when
// it names a format — a user who writes "trace.log" with the .asc type still
// selected has said what they want — then the selected filter, then Vector
// ASCII. A path with no extension gets the format's suffix appended by the
// caller (the dialog does that; this only decides).
CanLogFormat canLogFormatFor(const QString &selectedFilter, const QString &path);

// One SocketCAN candump log line, no newline:
//
//     (0000000012.345000) can2 0F3#7E0D0000F0C400F0
//
// Seconds zero-padded to ten digits, microseconds to six (candump writes bare
// digits; the padded form is what the converters this project's captures came
// from write, and every reader parses both), the bus as can1..can3 — the
// device's own numbering, so "can2" is the port labelled CAN 2 — and the
// frame as candump spells it: a 3-digit id for 11-bit, 8 for 29-bit, '#' and
// the bytes with no separators, upper-case throughout. A CAN FD frame is
// "id##F" + bytes, F being one hex digit of flags (bit 0 BRS, bit 1 ESI). A
// frame with no data ends at the '#'.
QString socketCanLogLine(const MonitorStreamPayload &frame, quint32 t0);

// The PEAK TRC 3.0 header block, each line ending in '\n' (the caller maps
// that to the file's line ending; the format wants CR/LF). $STARTTIME is the
// Delphi serial date PEAK's tools expect — days since 1899-12-30 with the time
// of day as the fraction — and $COLUMNS declares the record layout below.
QString trcHeader(const QDateTime &when);

// One TRC 3.0 record, no newline, in the columns the header declares
// (N,O,T,B,I,d,R,L,D):
//
//            1         54.000 DT 2        0F3 Rx - 8    7E 0D 00 00 F0 C4 00 F0
//
// index, time offset in milliseconds to three decimals, type (DT classic;
// FD/FB/FE/BI for CAN FD plain / BRS / ESI / both), bus, id (3 or 8 hex
// digits), Rx/Tx, the reserved '-', the DLC (0..8, or the FD code up to 15)
// and the bytes. `index` is the 1-based message number.
QString trcFrameLine(const MonitorStreamPayload &frame, quint32 t0, quint64 index);

// Per-format line ending: CR/LF for .asc and .trc (what their tools write and
// what the .asc export always produced on Windows), LF for a candump log,
// whose readers take a line up to the newline and choke on a stray CR.
QString canLogLineEnding(CanLogFormat format);

// The header block for a format ('\n'-separated lines; empty for SocketCAN,
// which has none) and one record. The two dispatch to the writers above so the
// dialog has one loop whatever the format.
QString canLogHeader(CanLogFormat format, const QDateTime &when);
QString canLogFrameLine(CanLogFormat format, const MonitorStreamPayload &frame, quint32 t0,
                        quint64 index);

} // namespace ct
