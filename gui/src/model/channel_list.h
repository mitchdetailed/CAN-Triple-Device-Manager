// Channel lists — the .ct3l file behind Save List… / Load List… in the Monitor
// Channels picker. A list is the names of the channels somebody watches, in
// the order they watch them, lifted out of one configuration so the same set
// can be brought up on the next: a workshop's "cooling" list, a dyno's "boost"
// list. Names and nothing else — no values, no mapping, no passwords — so the
// file is plain JSON a person can read and fix in Notepad:
//
//   { "fileType": "CANTripleChannelList", "formatVersion": 1,
//     "channels": ["RPM", "Boost", "Coolant Temp"] }
//
// A name the receiving configuration does not map is the loader's caller's
// problem to report ("not found") and leave out; the file is not the place to
// resolve it, because the same list is meant to be loaded against many
// configurations and the missing channel may be present in the next one.
#pragma once

#include <QString>
#include <QStringList>

namespace ct {

inline constexpr char kChannelListFileType[] = "CANTripleChannelList";
inline constexpr int kChannelListFormatVersion = 1;

QString channelListExtension(); // "ct3l"
QString channelListFilter();    // the file dialogs' filter string

// Writes `names` in order. False with *error on any failure; the file is
// written whole or not at all.
bool writeChannelList(const QString &path, const QStringList &names, QString *error = nullptr);
// Reads a list: blank names dropped, a repeated name kept once (first place).
// False with *error for a file that is not a channel list, or one written by
// a newer format.
bool readChannelList(const QString &path, QStringList *names, QString *error = nullptr);

} // namespace ct
