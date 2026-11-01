// Online → USB Mode…: which USB device a CAN Triple 2.0 is — the Device
// Manager's serial port, or a SocketCAN adapter that Linux's own gs_usb driver
// makes three CAN interfaces of (the cantriple-socketcan package) — and which
// one it starts as.
//
// Three choices, because the protocol has three: the Device Manager's port now
// and at every power-up; the adapter until the unit restarts; the adapter now
// and at every power-up. There is no "start as the adapter, but not yet": a
// unit told to keep the adapter becomes it at once. Choosing the adapter ends
// this connection, so the dialog says so before OK, and how to come back.
//
// The Manager's port is the way back from a unit that was set to start as the
// adapter: on a PC that cannot use the adapter (Windows, or Linux without the
// package) the unit gives the Manager's port by itself, and this is where it
// is told to stay that way. A refusal is said inside the dialog, which stays
// open.
#pragma once

#include <QDialog>
#include <QString>

#include <functional>

#include "../protocol/device_session.h"

class QButtonGroup;
class QLabel;
class QPushButton;

namespace ct {

class DeviceLink;

class UsbModeDialog : public QDialog
{
    Q_OBJECT

public:
    enum Choice {
        NoChoice = -1,    // nothing chosen yet: OK waits for one
        ManagerPort = 0,  // the Device Manager's port, now and at every power-up
        AdapterForNow = 1, // the adapter until the unit restarts
        AdapterKept = 2,   // the adapter, now and at every power-up
    };

    // Asks for the unit's CAN Viewer password and proves it, over `parent`;
    // false when it was not proved. The main window's is the Access
    // Passwords prompt, a test's is whatever it needs.
    using ProveViewer = std::function<bool(QWidget *parent)>;

    // `info`: the unit's USB mode, read before opening (supported).
    UsbModeDialog(DeviceLink *link, const device_session::UsbModeInfo &info,
                  ProveViewer proveViewer, QWidget *parent = nullptr);

    Choice choice() const;
    void setChoice(Choice choice);
    // What the unit is and starts as, as the dialog says it.
    QString stateText() const;
    // The last refusal, as shown; empty when nothing was refused.
    QString statusText() const;
    // The unit took a choice that made it the adapter: it is leaving the bus,
    // and the link with it.
    bool unitLeft() const { return m_unitLeft; }

public slots:
    void accept() override;

private:
    void choiceChanged();
    bool send(quint8 mode, bool store);

    DeviceLink *m_link;
    device_session::UsbModeInfo m_info;
    ProveViewer m_proveViewer;
    bool m_unitLeft = false;
    QButtonGroup *m_group = nullptr;
    QPushButton *m_ok = nullptr;
    QLabel *m_state = nullptr;
    QLabel *m_note = nullptr;   // what the selected choice will do
    QLabel *m_status = nullptr; // a refusal
};

} // namespace ct
