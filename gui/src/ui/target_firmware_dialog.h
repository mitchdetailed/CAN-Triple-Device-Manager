// "Target Firmware" — which firmware's capacities the open document is sized
// against, and where to take them from: the connected unit's report, a
// firmware image file, or this build's built-in numbers.
//
// It exists because the numbers stopped being constants. Every dialog's "at
// most n", the mapper's "table is full", and which Calculations items are
// offered at all now come from Configuration::capacity(), and a document
// built for a variant that holds 40 CRC8 rules has to be able to say so
// before that variant is on the cable. The dialog applies a choice
// immediately (it is an edit, saved with the file) and shows what the
// document uses against what the target holds, so a target that no longer
// fits is visible here and named in Check Channels.
#pragma once

#include <QDialog>

#include "../protocol/capacity.h"

class QLabel;
class QPushButton;
class QTableWidget;

namespace ct {

class Configuration;
class DeviceLink;

class TargetFirmwareDialog : public QDialog
{
    Q_OBJECT
public:
    explicit TargetFirmwareDialog(Configuration *config, DeviceLink *link,
                                  QWidget *parent = nullptr);

private:
    void useConnectedDevice();
    void useFirmwareImage();
    void useBuiltIn();
    // Applies `capacity` to the document, refreshes the table, and — when the
    // document no longer maps against it — says how many rows Check Channels
    // will name. Never refuses: shrinking a target is how a document is
    // brought down to a smaller variant, and the rows to cut are the user's
    // choice.
    void apply(const DeviceCapacity &capacity);
    void refresh();

    Configuration *m_config;
    DeviceLink *m_link;
    QLabel *m_targetLabel;
    QTableWidget *m_table;
    QPushButton *m_deviceButton;
};

} // namespace ct
