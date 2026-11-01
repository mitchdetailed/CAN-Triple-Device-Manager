// Implementation of the "Connection Settings" dialog: COM port + baud
// selection, connect/disconnect, and a GET_STATUS test button.
#include "connection_settings_dialog.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QElapsedTimer>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSerialPortInfo>
#include <QThread>
#include <QVBoxLayout>
#include <QFormLayout>

#include <cstring>

#include "../protocol/usb_port.h"
#include "../protocol/wire_structs.h"

namespace ct {

namespace {

const qint32 kBaudRates[] = { 7372800, 3686400, 1843200, 921600, 460800, 230400, 115200 };

// A port's item carries whether it is a CAN Triple 2.0 (usb_port.h).
constexpr int kUsbUnitRole = Qt::UserRole + 1;
// A unit that is a SocketCAN adapter now: its index in m_adapters.
constexpr int kAdapterRole = Qt::UserRole + 2;
// How long a unit switched back from the adapter has to show its port: it
// leaves the bus for a quarter of a second, and the PC then sets up a port.
constexpr int kPortBackMs = 8000;

} // namespace

ConnectionSettingsDialog::ConnectionSettingsDialog(DeviceLink *link, QWidget *parent,
                                                   bool closeOnConnect)
    : QDialog(parent),
      m_link(link)
{
    m_closeOnConnect = closeOnConnect;
    setWindowTitle(tr("Connection Settings"));

    m_portCombo = new QComboBox(this);
    m_portCombo->setMinimumWidth(320);

    m_baudCombo = new QComboBox(this);
    m_baudCombo->setEditable(true);
    for (qint32 baud : kBaudRates) {
        QString label = (baud == 7372800)
            ? tr("7372800 (CAN Triple default — ST-Link V3)")
            : QString::number(baud);
        m_baudCombo->addItem(label, baud);
    }
    m_baudCombo->setCurrentIndex(0);

    auto *form = new QFormLayout;
    form->addRow(tr("Port:"), m_portCombo);
    form->addRow(tr("Baud rate:"), m_baudCombo);

    m_refreshButton = new QPushButton(tr("Refresh"), this);
    m_testButton = new QPushButton(tr("Test"), this);
    m_connectButton = new QPushButton(tr("Connect"), this);
    auto *closeButton = new QPushButton(tr("Close"), this);

    auto *buttonRow = new QHBoxLayout;
    buttonRow->addWidget(m_refreshButton);
    buttonRow->addStretch(1);
    buttonRow->addWidget(m_testButton);
    buttonRow->addWidget(m_connectButton);
    buttonRow->addWidget(closeButton);

    m_statusLabel = new QLabel(tr("Not connected"), this);

    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->addLayout(form);
    mainLayout->addLayout(buttonRow);
    mainLayout->addWidget(m_statusLabel);

    connect(m_refreshButton, &QPushButton::clicked, this,
            &ConnectionSettingsDialog::refreshPorts);
    // The baud rate means nothing to a USB unit, so it follows the choice.
    connect(m_portCombo, &QComboBox::currentIndexChanged, this,
            &ConnectionSettingsDialog::updateUi);
    connect(m_connectButton, &QPushButton::clicked, this,
            &ConnectionSettingsDialog::onConnectClicked);
    connect(m_testButton, &QPushButton::clicked, this,
            &ConnectionSettingsDialog::onTestClicked);
    connect(closeButton, &QPushButton::clicked, this, &QDialog::reject);

    connect(m_link, &DeviceLink::connected, this, &ConnectionSettingsDialog::updateUi);
    connect(m_link, &DeviceLink::disconnected, this, &ConnectionSettingsDialog::updateUi);

    refreshPorts();
    updateUi();
}

void ConnectionSettingsDialog::refreshPorts()
{
    const QString previous = m_portCombo->currentData().toString();
    m_portCombo->clear();

    int preselect = -1;
    // THE HIGHEST-NUMBERED COM PORT is the default. Windows numbers a device it
    // has not met before after every port it already knows, so the unit that
    // was just plugged in is nearly always the highest. Among ST-LINK ports
    // first, though: a Bluetooth serial port is often the highest number on a
    // laptop, and it is never a CAN Triple. No ST-LINK at all, and the highest
    // port stands. The port already chosen still wins on Refresh.
    //
    // A CAN Triple 2.0 comes before all of them, and is named: it is its own
    // USB device, so its port IS the unit, where an ST-LINK's port is only a
    // way to one. Windows' own driver calls it "USB Serial Device", which says
    // nothing, so the name comes from its USB IDs (usb_port.h).
    int highestIndex = -1;
    int highestNumber = -1;
    int highestStlinkIndex = -1;
    int highestStlinkNumber = -1;
    int highestUsbIndex = -1;
    int highestUsbNumber = -1;
    const auto ports = QSerialPortInfo::availablePorts();
    for (const QSerialPortInfo &info : ports) {
        const bool usbUnit = isCanTriple2Port(info);
        QString label = info.portName();
        if (usbUnit)
            label += tr(" — CAN Triple 2.0 (USB)");
        else if (!info.description().isEmpty())
            label += QStringLiteral(" — ") + info.description();
        m_portCombo->addItem(label, info.portName());
        const int idx = m_portCombo->count() - 1;
        m_portCombo->setItemData(idx, usbUnit, kUsbUnitRole);
        if (info.portName() == previous && preselect < 0)
            preselect = idx;
        const QString name = info.portName();
        if (name.startsWith(QStringLiteral("COM"), Qt::CaseInsensitive)) {
            bool ok = false;
            const int number = name.mid(3).toInt(&ok);
            if (ok && number > highestNumber) {
                highestNumber = number;
                highestIndex = idx;
            }
            const bool stlink =
                info.description().contains(QStringLiteral("STLink"), Qt::CaseInsensitive)
                || info.description().contains(QStringLiteral("ST-Link"), Qt::CaseInsensitive)
                || info.manufacturer().contains(QStringLiteral("STMicroelectronics"),
                                                Qt::CaseInsensitive);
            if (ok && stlink && number > highestStlinkNumber) {
                highestStlinkNumber = number;
                highestStlinkIndex = idx;
            }
            if (ok && usbUnit && number > highestUsbNumber) {
                highestUsbNumber = number;
                highestUsbIndex = idx;
            }
        } else if (usbUnit && highestUsbIndex < 0) {
            // Not a COM port (Linux names it ttyACMn): the first one is the unit.
            highestUsbIndex = idx;
        }
    }

    // Units that are SocketCAN adapters now, after the ports: no port of their
    // own, but they are CAN Triples all the same, and Connect brings one back.
    m_adapters = usb_adapter::adapterUnits();
    int firstAdapterIndex = -1;
    for (int i = 0; i < m_adapters.size(); ++i) {
        m_portCombo->addItem(tr("CAN Triple 2.0 %1 — SocketCAN adapter now (Connect switches "
                                "it back)")
                                 .arg(m_adapters[i].shortId().toUpper()),
                             QString());
        const int idx = m_portCombo->count() - 1;
        m_portCombo->setItemData(idx, true, kUsbUnitRole);
        m_portCombo->setItemData(idx, i, kAdapterRole);
        if (firstAdapterIndex < 0)
            firstAdapterIndex = idx;
    }

    if (preselect < 0)
        preselect = highestUsbIndex;
    if (preselect < 0)
        preselect = firstAdapterIndex;
    if (preselect < 0)
        preselect = highestStlinkIndex;
    if (preselect < 0)
        preselect = highestIndex;
    if (preselect < 0 && m_portCombo->count() > 0)
        preselect = 0;
    if (preselect >= 0)
        m_portCombo->setCurrentIndex(preselect);
}

void ConnectionSettingsDialog::onConnectClicked()
{
    if (m_link->isOpen()) {
        m_link->close();
        updateUi();
        return;
    }

    if (m_portCombo->currentIndex() < 0) {
        QMessageBox::warning(this, tr("Connection Settings"),
                             tr("No serial port selected."));
        return;
    }

    const QVariant adapter = m_portCombo->currentData(kAdapterRole);
    if (adapter.isValid()) {
        connectAdapter(m_adapters.value(adapter.toInt()));
        return;
    }

    const QString port = m_portCombo->currentData().toString();

    bool baudOk = false;
    qint32 baud = 0;
    const QVariant baudData = m_baudCombo->currentData();
    if (baudData.isValid() &&
        m_baudCombo->currentText() == m_baudCombo->itemText(m_baudCombo->currentIndex())) {
        baud = baudData.toInt(&baudOk);
    } else {
        baud = m_baudCombo->currentText().trimmed().toInt(&baudOk);
    }
    if (!baudOk || baud <= 0) {
        QMessageBox::warning(this, tr("Connection Settings"),
                             tr("Invalid baud rate."));
        return;
    }

    // Opening a port can block this thread for several seconds — a Bluetooth
    // SPP COM port that is paired but not in range takes 5-20 s to fail — and
    // during it the window stops repainting and Windows paints "Not Responding"
    // over it. A busy cursor plus a forced repaint of the label tells the user
    // the app is working, not hung, for the cost of two lines. (A truly async
    // open needs the port on a worker thread, which is a larger change than the
    // symptom warrants.)
    QString err;
    bool opened = false;
    {
        QApplication::setOverrideCursor(Qt::BusyCursor);
        m_statusLabel->setText(tr("Opening %1…").arg(port));
        m_statusLabel->repaint();
        opened = m_link->open(port, baud, &err);
        QApplication::restoreOverrideCursor();
    }
    if (!opened) {
        QMessageBox::warning(this, tr("Connection Settings"),
                             tr("Failed to open %1:\n%2").arg(port, err));
    }
    updateUi();
    // Opened to get a link, and there is one: done. The status bar says what
    // answered, which is where the eye goes next.
    if (opened && m_closeOnConnect)
        accept();
}

void ConnectionSettingsDialog::connectAdapter(const usb_adapter::AdapterUnit &unit)
{
    const QString title = tr("Connection Settings");
    QMessageBox ask(QMessageBox::Question, title,
                    tr("CAN Triple 2.0 %1 is a SocketCAN adapter now, with no port for the "
                       "Device Manager.\n\nSwitch it back to the Device Manager's port? Its "
                       "SocketCAN interfaces (ct%2-can1 to -can3) go away, and its buses go "
                       "back to its own configuration.")
                        .arg(unit.shortId().toUpper(), unit.shortId()),
                    QMessageBox::Cancel, this);
    QPushButton *switchBack = ask.addButton(tr("Switch Back"), QMessageBox::AcceptRole);
    // Back to normal is what this is usually for, so it is offered ticked; a
    // unit kept as the adapter on purpose can be left so by unticking it.
    auto *keep = new QCheckBox(tr("And start as the Device Manager's port at every power-up"),
                               &ask);
    keep->setChecked(true);
    ask.setCheckBox(keep);
    ask.setDefaultButton(switchBack);
    ask.exec();
    if (ask.clickedButton() != switchBack)
        return;

    QString err;
    if (!usb_adapter::switchToManager(unit, keep->isChecked(), &err)) {
        QMessageBox::warning(this, title, err);
        refreshPorts();
        return;
    }

    // Its port, by its serial number, once the PC has set it up.
    QString port;
    {
        QApplication::setOverrideCursor(Qt::BusyCursor);
        m_statusLabel->setText(tr("Waiting for the unit's port…"));
        m_statusLabel->repaint();
        QElapsedTimer waited;
        waited.start();
        while (port.isEmpty() && waited.elapsed() < kPortBackMs) {
            QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
            QThread::msleep(100);
            for (const QSerialPortInfo &info : QSerialPortInfo::availablePorts()) {
                if (isCanTriple2Port(info)
                    && info.serialNumber().compare(unit.serial, Qt::CaseInsensitive) == 0)
                    port = info.portName();
            }
        }
        QApplication::restoreOverrideCursor();
    }
    refreshPorts();
    if (port.isEmpty()) {
        updateUi();
        QMessageBox::warning(this, title,
                             tr("The unit took the request, but its port has not appeared "
                                "yet. Press Refresh in a moment."));
        return;
    }
    const int idx = m_portCombo->findData(port);
    if (idx >= 0)
        m_portCombo->setCurrentIndex(idx);
    // A port just set up can refuse an open for a moment (udev is still
    // giving it its permissions): a few tries.
    bool opened = false;
    for (int attempt = 0; attempt < 10 && !opened; ++attempt) {
        if (attempt > 0)
            QThread::msleep(200);
        opened = m_link->open(port, kBaudRates[0], &err);
    }
    if (!opened)
        QMessageBox::warning(this, title, tr("Failed to open %1:\n%2").arg(port, err));
    updateUi();
    if (opened && m_closeOnConnect)
        accept();
}

void ConnectionSettingsDialog::onTestClicked()
{
    if (!m_link->isOpen()) {
        QMessageBox::warning(this, tr("Test"), tr("Not connected."));
        return;
    }

    QByteArray payload;
    QString err;
    if (!m_link->requestSync(CMD_GET_STATUS, QByteArray(), &payload, &err)) {
        QMessageBox::warning(this, tr("Test"),
                             tr("GET_STATUS failed:\n%1").arg(err));
        return;
    }

    if (payload.size() < static_cast<int>(sizeof(DeviceStatus))) {
        QMessageBox::warning(this, tr("Test"),
                             tr("GET_STATUS returned %1 bytes; expected %2.")
                                 .arg(payload.size())
                                 .arg(static_cast<int>(sizeof(DeviceStatus))));
        return;
    }

    DeviceStatus st;
    std::memcpy(&st, payload.constData(), sizeof(st));

    QString text = tr("Device responded.\n\n"
                      "Uptime: %1 s\n"
                      "CAN1: rx %2, tx %3\n"
                      "CAN2: rx %4, tx %5\n"
                      "CAN3: rx %6, tx %7\n\n"
                      "Active messages: %8\n"
                      "Active signals: %9\n"
                      "Active math: %10\n"
                      "Active conditions: %11")
                       .arg(st.uptime_ms / 1000)
                       .arg(st.rx_count[0]).arg(st.tx_count[0])
                       .arg(st.rx_count[1]).arg(st.tx_count[1])
                       .arg(st.rx_count[2]).arg(st.tx_count[2])
                       .arg(st.active_msg_count)
                       .arg(st.active_sig_count)
                       .arg(st.active_math_count)
                       .arg(st.active_cond_count);
    QMessageBox::information(this, tr("Test"), text);
}

void ConnectionSettingsDialog::updateUi()
{
    const bool open = m_link->isOpen();

    m_connectButton->setText(open ? tr("Disconnect") : tr("Connect"));
    m_testButton->setEnabled(open);
    m_portCombo->setEnabled(!open);
    const bool usbUnit = m_portCombo->currentData(kUsbUnitRole).toBool();
    m_baudCombo->setEnabled(!open && !usbUnit);
    m_baudCombo->setToolTip(usbUnit ? tr("The CAN Triple 2.0 is a USB device: no baud rate "
                                         "applies to it.")
                                    : QString());
    m_refreshButton->setEnabled(!open);

    if (open) {
        m_statusLabel->setText(isCanTriple2Port(m_link->portName())
                                   ? tr("Connected to %1 (USB)").arg(m_link->portName())
                                   : tr("Connected to %1 @ %2")
                                         .arg(m_link->portName())
                                         .arg(m_link->baudRate()));
    } else {
        m_statusLabel->setText(tr("Not connected"));
    }
}

} // namespace ct
