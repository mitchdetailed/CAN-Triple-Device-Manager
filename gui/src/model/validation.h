// Configuration validation ("File > Check Channels"). The firmware validates
// almost nothing, so the GUI is the safety layer (see FIRMWARE-NOTES.md).
#pragma once

#include <QList>
#include <QString>

namespace ct {

class Configuration;

struct ValidationIssue {
    enum Severity { Error, Warning, Info };
    Severity severity = Error;
    QString location; // e.g. "CAN 1 · Receive 0x640 · Engine RPM"
    QString message;
};

QList<ValidationIssue> validateConfiguration(const Configuration &config);

} // namespace ct
