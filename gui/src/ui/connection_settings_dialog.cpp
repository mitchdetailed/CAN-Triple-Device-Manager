// Implementation of the "Connection Settings" dialog: COM port + baud
// selection, connect/disconnect, and a GET_STATUS test button.
#include "connection_settings_dialog.h"

#include <QApplication>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSerialPortInfo>
#include <QVBoxLayout>
#include <QFormLayout>

#include <cstring>

#include "../protocol/wire_structs.h"

namespace ct {

namespace {

const qint32 kBaudRates[] = { 7372800, 3686400, 1843200, 921600, 460800, 230400, 115200 };

} // namespace

ConnectionSettingsDialog::ConnectionSettingsDialog(DeviceLink *link, QWidget *parent)
    : QDialog(parent),
      m_link(link)
{
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
    int stlinkIndex = -1;
    const auto ports = QSerialPortInfo::availablePorts();
    for (const QSerialPortInfo &info : ports) {
        QString label = info.portName();
        if (!info.description().isEmpty())
            label += QStringLiteral(" — ") + info.description();
        m_portCombo->addItem(label, info.portName());
        const int idx = m_portCombo->count() - 1;
        if (info.portName() == previous && preselect < 0)
            preselect = idx;
        if (stlinkIndex < 0 && info.description().contains(QStringLiteral("STLink"), Qt::CaseInsensitive))
            stlinkIndex = idx;
    }

    if (preselect < 0)
        preselect = stlinkIndex;
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
    m_baudCombo->setEnabled(!open);
    m_refreshButton->setEnabled(!open);

    if (open) {
        m_statusLabel->setText(tr("Connected to %1 @ %2")
                                   .arg(m_link->portName())
                                   .arg(m_link->baudRate()));
    } else {
        m_statusLabel->setText(tr("Not connected"));
    }
}

} // namespace ct
