#include "usb_mode_dialog.h"

#include <QButtonGroup>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QRadioButton>
#include <QVBoxLayout>

#include "../protocol/device_link.h"
#include "../protocol/wire_structs.h"

namespace ct {

UsbModeDialog::UsbModeDialog(DeviceLink *link, const device_session::UsbModeInfo &info,
                             ProveViewer proveViewer, QWidget *parent)
    : QDialog(parent),
      m_link(link),
      m_info(info),
      m_proveViewer(std::move(proveViewer))
{
    setWindowTitle(tr("USB Mode"));

    m_state = new QLabel(this);
    m_state->setWordWrap(true);
    // The dialog is only ever opened on the Manager's port: as the adapter the
    // unit has no port to open it on.
    QString state;
    if (info.fellBack) {
        state = tr("This unit is set to be a SocketCAN adapter, but no program on this PC "
                   "used the adapter, so it gave this PC the Device Manager's port instead. "
                   "When the USB cable comes out it is the adapter again, for the next PC.");
    } else if (info.stored == USB_MODE_GS_USB) {
        state = tr("This unit is the Device Manager's port now, and starts as a SocketCAN "
                   "adapter at every power-up.");
    } else {
        state = tr("This unit is the Device Manager's port now, and starts as it at every "
                   "power-up.");
    }
    m_state->setText(state);

    auto *box = new QGroupBox(tr("Make it"), this);
    auto *manager = new QRadioButton(tr("The Device Manager's port, now and at every power-up"),
                                     box);
    auto *forNow = new QRadioButton(tr("A SocketCAN adapter until it restarts, then the Device "
                                       "Manager's port again"),
                                    box);
    auto *kept = new QRadioButton(tr("A SocketCAN adapter, now and at every power-up"), box);
    m_group = new QButtonGroup(this);
    m_group->addButton(manager, ManagerPort);
    m_group->addButton(forNow, AdapterForNow);
    m_group->addButton(kept, AdapterKept);
    auto *boxLayout = new QVBoxLayout(box);
    boxLayout->addWidget(manager);
    boxLayout->addWidget(forNow);
    boxLayout->addWidget(kept);

    m_note = new QLabel(this);
    m_note->setWordWrap(true);
    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    m_status->setStyleSheet(QStringLiteral("color: palette(link);"));
    m_status->hide();

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_ok = buttons->button(QDialogButtonBox::Ok);
    connect(buttons, &QDialogButtonBox::accepted, this, &UsbModeDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_state);
    layout->addWidget(box);
    layout->addWidget(m_note);
    layout->addWidget(m_status);
    layout->addWidget(buttons);
    setMinimumWidth(460);

    connect(m_group, &QButtonGroup::idClicked, this, &UsbModeDialog::choiceChanged);
    // OK with what is offered must not surprise. A unit on the Manager's port
    // that starts as it is offered that: OK changes nothing. One that fell back
    // is offered it too, the remedy this PC needs. One that starts as the
    // adapter but is the Manager's port now is offered nothing, since every
    // choice changes something: OK waits for one.
    setChoice(info.stored == USB_MODE_GS_USB && !info.fellBack ? NoChoice : ManagerPort);
}

UsbModeDialog::Choice UsbModeDialog::choice() const
{
    return Choice(m_group->checkedId());
}

void UsbModeDialog::setChoice(Choice choice)
{
    if (QAbstractButton *b = m_group->button(choice)) {
        b->setChecked(true);
    } else if (QAbstractButton *checked = m_group->checkedButton()) {
        // An exclusive group will not uncheck its last button by itself.
        m_group->setExclusive(false);
        checked->setChecked(false);
        m_group->setExclusive(true);
    }
    choiceChanged();
}

QString UsbModeDialog::stateText() const
{
    return m_state->text();
}

QString UsbModeDialog::statusText() const
{
    return m_status->isHidden() ? QString() : m_status->text();
}

void UsbModeDialog::choiceChanged()
{
    m_ok->setEnabled(choice() != NoChoice);
    if (choice() == NoChoice) {
        m_note->setText(tr("Choose what the unit should be."));
        return;
    }
    if (choice() == ManagerPort) {
        m_note->setText(m_info.stored == USB_MODE_GS_USB || m_info.fellBack
                            ? tr("The unit stays connected here, and from now on starts as the "
                                 "Device Manager's port.")
                            : tr("Nothing changes: it is that already."));
        return;
    }
    // Where the unit goes, and the way back from there. Windows has no driver
    // for the adapter; the firmware gives such a PC the Manager's port again,
    // but only once the cable has been out (the PC that chose is left alone).
#ifdef Q_OS_LINUX
    const QString away =
        tr("The unit becomes the adapter now, and this connection ends. With the "
           "cantriple-socketcan package its buses are SocketCAN interfaces (ctriple status "
           "lists them). To come back, choose the unit under Online > Connect, or run "
           "ctriple manager.");
#else
    const QString away =
        tr("The unit becomes the adapter now, and this connection ends. This PC has no driver "
           "for the adapter: unplug the USB cable and plug it in again, and within a few "
           "seconds the unit gives it the Device Manager's port (firmware 2.0.11 or later; "
           "earlier firmware needs the unit's power cycled).");
#endif
    m_note->setText(away + QLatin1Char(' ')
                    + tr("If the unit has a CAN Viewer password, you will be asked for it."));
}

bool UsbModeDialog::send(quint8 mode, bool store)
{
    QString error;
    quint8 code = 0;
    if (device_session::setUsbMode(m_link, mode, store, &error, &code))
        return true;
    // The adapter reads and writes the buses raw, as the CAN Viewer does, so
    // the same password guards it: prove it and ask once more.
    if (code == ERR_LOCKED && m_proveViewer && m_proveViewer(this)) {
        if (device_session::setUsbMode(m_link, mode, store, &error, &code))
            return true;
    }
    QString text;
    switch (code) {
    case ERR_LOCKED:
        text = tr("The unit wants its CAN Viewer password before it becomes the adapter.");
        break;
    case ERR_FLASH_WRITE:
        text = tr("The unit could not keep the choice. Nothing was changed.");
        break;
    case ERR_OUT_OF_BOUNDS:
        text = tr("This firmware cannot keep a USB mode across power-ups. Update the unit's "
                  "firmware.");
        break;
    case ERR_INVALID_CMD:
        text = tr("This firmware has no SocketCAN mode. Update the unit's firmware.");
        break;
    default:
        text = error.isEmpty() ? tr("The unit did not answer.") : error;
        break;
    }
    m_status->setText(text);
    m_status->show();
    return false;
}

void UsbModeDialog::accept()
{
    m_status->hide();
    switch (choice()) {
    case ManagerPort:
        // Sent whenever it is not already what the unit is set to, and always
        // after a fallback: that is this PC choosing the Manager's port, which
        // keeps the unit from turning back into the adapter when the cable
        // comes out (firmware 2.0.11).
        if (m_info.stored == USB_MODE_GS_USB || m_info.fellBack) {
            if (!send(USB_MODE_MANAGER, true))
                return;
        }
        break;
    case AdapterForNow:
        // Until it restarts means it starts as the Manager's port: say so
        // first if it is set to start as the adapter (changing only that).
        if (m_info.stored == USB_MODE_GS_USB && !send(USB_MODE_MANAGER, true))
            return;
        if (!send(USB_MODE_GS_USB, false))
            return;
        m_unitLeft = true;
        break;
    case AdapterKept:
        if (!send(USB_MODE_GS_USB, true))
            return;
        m_unitLeft = true;
        break;
    case NoChoice:
        return;
    }
    QDialog::accept();
}

} // namespace ct
