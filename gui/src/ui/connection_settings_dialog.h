// "Connection Settings" — COM port selection (ST-Link VCP highlighted), baud
// rate (default 7,372,800 — ST-Link V3 required), connect/disconnect, and a
// Test button that runs GET_STATUS.
#pragma once

#include <QDialog>

#include "../protocol/device_link.h"

class QComboBox;
class QLabel;
class QPushButton;

namespace ct {

class ConnectionSettingsDialog : public QDialog
{
    Q_OBJECT
public:
    explicit ConnectionSettingsDialog(DeviceLink *link, QWidget *parent = nullptr);

private:
    void refreshPorts();
    void onConnectClicked();
    void onTestClicked();
    void updateUi();

    DeviceLink *m_link;
    QComboBox *m_portCombo;
    QComboBox *m_baudCombo;
    QPushButton *m_refreshButton;
    QPushButton *m_connectButton;
    QPushButton *m_testButton;
    QLabel *m_statusLabel;
};

} // namespace ct
