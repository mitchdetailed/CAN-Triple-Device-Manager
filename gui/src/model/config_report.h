// Channel usage analysis and the MoTeC-style "Channel Summary Report"
// (File > Config Summary). Pure model code — the dialog only renders what
// this produces, so the report content is unit-tested.
#pragma once

#include <QHash>
#include <QString>
#include <QStringList>

namespace ct {

class Configuration;

// Who generates / consumes each channel. Keys are lower-cased channel names;
// displayName maps back to the first-seen spelling.
struct ChannelUsage {
    QHash<QString, QStringList> generators; // "CAN 1 · Receive 0x640", "Math 1", ...
    QHash<QString, QStringList> users;      // "CAN 2 · Transmit 0x700", "Condition 1 input A", ...
    QHash<QString, QString> displayName;

    QStringList used;       // generators ∪ users, in order of appearance
    QStringList incomplete; // consumed somewhere but never generated
    QStringList unused;     // in the catalogue but neither generated, used,
                            // nor referenced by an Off section / inactive calc
};

// Semantics mirror validation / the device mapper:
// - generators: active receive-section rows (incl. compound identifier rows)
//   and active math / condition / counter / timer / constant outputs.
// - users: transmit-section rows, active calculation inputs, and section
//   diagnostic channels.
// - rows of Off sections and inactive calculations are "dormant": they keep a
//   channel out of `unused` (so cleanup never breaks them) but do not count
//   as generated or used.
ChannelUsage analyzeChannelUsage(const Configuration &config);

// The full Channel Summary Report (Summary Information, Comments, Bus Setup,
// Used Channels, Channels By Function, Incomplete Channels, Unused Channels).
QString configSummaryText(const Configuration &config);
QString configSummaryHtml(const Configuration &config); // same content, styled

} // namespace ct
