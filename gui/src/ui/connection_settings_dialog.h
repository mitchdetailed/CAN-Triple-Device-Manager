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
    // closeOnConnect: the dialog was opened to get a link (Online > Connect, or
    // a command that connects first), so a successful Connect is the answer
    // and the dialog closes on it. Off, as from Tools > Connection Settings…,
    // it stays for the Test button and a Disconnect.
    explicit ConnectionSettingsDialog(DeviceLink *link, QWidget *parent = nullptr,
                                      bool closeOnConnect = false);

private:
    void refreshPorts();
    void onConnectClicked();
    bool m_closeOnConnect = false;
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
