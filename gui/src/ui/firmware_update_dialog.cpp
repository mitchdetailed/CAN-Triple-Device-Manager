#include "firmware_update_dialog.h"

#include <QCheckBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QEventLoop>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QCloseEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QStandardPaths>
#include <QTimer>
#include <QVBoxLayout>

#include "../model/configuration.h"
#include "../model/device_mapper.h"
// For mapWithScript(): the restore below must carry the script, and plain
// mapToDevice() from device_mapper.h silently emits none.
#include "../scripting/script_compiler.h"
#include "../protocol/config_transfer.h"
#include "../protocol/device_session.h"
#include "../model/user_paths.h"

namespace ct {

namespace {

// Sleep without freezing the UI. Used while the device is away installing.
void pump(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

QString versionText(quint16 major, quint16 minor, quint16 patch)
{
    return QStringLiteral("%1.%2.%3").arg(major).arg(minor).arg(patch);
}

// Where backups go. Documents rather than AppData — this is a file the user
// may need to find and send back by hand months later, and AppData is somewhere
// people do not look. The path itself is user_paths.h's, which is the one place
// that knows the layout.
QString backupDirectory()
{
    return firmwareBackupsDirectory();
}

} // namespace

FirmwareUpdateDialog::FirmwareUpdateDialog(DeviceLink *link, QWidget *parent,
                                           ReproveFn reproveSend)
    : QDialog(parent), m_link(link), m_updater(link, this),
      m_reproveSend(std::move(reproveSend))
{
    setWindowTitle(tr("Update Firmware"));
    setMinimumWidth(560);
    // Window-modal so F1 reaches a usable help window; see the same note in
    // ScriptEditorDialog. This dialog wants it more than most — the manual page
    // it opens is the one describing what happens if an update is interrupted,
    // which is exactly what someone reads while an update is in progress.
    setWindowModality(Qt::WindowModal);
    buildUi();

    connect(&m_updater, &FirmwareUpdater::progress, this, [this](qint64 sent, qint64 total) {
        m_progress->setMaximum(static_cast<int>(total));
        m_progress->setValue(static_cast<int>(sent));
    });
    connect(&m_updater, &FirmwareUpdater::statusMessage, this, &FirmwareUpdateDialog::say);

    // The first device read is deferred to showEvent so the dialog paints before
    // it blocks on the serial round trip. Doing it here froze the still-invisible
    // window for the length of a GET/STATUS on a slow or absent device.
    updateReadiness();
}

void FirmwareUpdateDialog::buildUi()
{
    auto *root = new QVBoxLayout(this);

    auto *deviceBox = new QGroupBox(tr("Device"), this);
    auto *deviceLayout = new QVBoxLayout(deviceBox);
    m_deviceInfo = new QLabel(tr("Reading…"), deviceBox);
    m_deviceInfo->setTextFormat(Qt::RichText);
    m_deviceInfo->setWordWrap(true);
    deviceLayout->addWidget(m_deviceInfo);
    root->addWidget(deviceBox);

    auto *fileBox = new QGroupBox(tr("Firmware file"), this);
    auto *fileLayout = new QVBoxLayout(fileBox);
    auto *pathRow = new QHBoxLayout();
    m_pathEdit = new QLineEdit(fileBox);
    m_pathEdit->setPlaceholderText(tr("Select a .ctf firmware image…"));
    m_pathEdit->setReadOnly(true);
    m_browseButton = new QPushButton(tr("Browse…"), fileBox);
    connect(m_browseButton, &QPushButton::clicked, this, &FirmwareUpdateDialog::onBrowse);
    pathRow->addWidget(m_pathEdit, 1);
    pathRow->addWidget(m_browseButton);
    fileLayout->addLayout(pathRow);

    m_imageInfo = new QLabel(fileBox);
    m_imageInfo->setTextFormat(Qt::RichText);
    m_imageInfo->setWordWrap(true);
    fileLayout->addWidget(m_imageInfo);
    root->addWidget(fileBox);

    m_warnings = new QLabel(this);
    m_warnings->setTextFormat(Qt::RichText);
    m_warnings->setWordWrap(true);
    m_warnings->setVisible(false);
    root->addWidget(m_warnings);

    m_backupCheck = new QCheckBox(
        tr("Save a copy of the device's configuration before updating"), this);
    m_backupCheck->setChecked(true);
    m_backupCheck->setToolTip(
        tr("Reads the configuration out of the device and writes it to a dated .ct3 file.\n"
           "This is the only opportunity to save it: if the update changes the stored-image "
           "format, the configuration cannot be recovered from the device afterwards."));
    root->addWidget(m_backupCheck);

    m_progress = new QProgressBar(this);
    m_progress->setRange(0, 100);
    m_progress->setValue(0);
    m_progress->setVisible(false);
    root->addWidget(m_progress);

    m_statusLine = new QLabel(this);
    m_statusLine->setWordWrap(true);
    root->addWidget(m_statusLine);

    auto *buttons = new QDialogButtonBox(this);
    m_updateButton = buttons->addButton(tr("Update Firmware"), QDialogButtonBox::AcceptRole);
    m_closeButton = buttons->addButton(QDialogButtonBox::Close);
    QPushButton *helpButton = buttons->addButton(QDialogButtonBox::Help);
    connect(m_updateButton, &QPushButton::clicked, this, &FirmwareUpdateDialog::onUpdate);
    connect(m_closeButton, &QPushButton::clicked, this, &QDialog::reject);
    connect(helpButton, &QPushButton::clicked, this,
            [this]() { emit helpRequested(QStringLiteral("firmware-update.html")); });
    root->addWidget(buttons);
}

void FirmwareUpdateDialog::keyPressEvent(QKeyEvent *event)
{
    // F1 opens the manual at this dialog's own page. This is the one dialog in
    // the program whose consequences a user is most likely to want to read
    // about BEFORE pressing the button, so it should not cost a trip through
    // the table of contents.
    if (event->key() == Qt::Key_F1) {
        emit helpRequested(QStringLiteral("firmware-update.html"));
        event->accept();
        return;
    }
    QDialog::keyPressEvent(event);
}

void FirmwareUpdateDialog::reject()
{
    // Esc and the Close button both land here. An update in flight runs on
    // nested event loops that keep going whether this window is up or not, so
    // dismissing mid-update would leave it running headless. Refuse until it
    // finishes — the Close button is already disabled by setBusy, this closes
    // the Esc/programmatic path.
    if (m_busy)
        return;
    QDialog::reject();
}

void FirmwareUpdateDialog::closeEvent(QCloseEvent *event)
{
    // The title-bar X, same reasoning as reject().
    if (m_busy) {
        event->ignore();
        return;
    }
    QDialog::closeEvent(event);
}

void FirmwareUpdateDialog::showEvent(QShowEvent *event)
{
    QDialog::showEvent(event);
    // The initial device read, run once and only after the window is up, so the
    // blocking serial round trip does not freeze an unpainted dialog.
    if (m_firstShow) {
        m_firstShow = false;
        refreshDeviceStatus();
        updateReadiness();
    }
}

void FirmwareUpdateDialog::say(const QString &text)
{
    m_statusLine->setText(text);
    // The transfer runs inside nested event loops, so the label would otherwise
    // not repaint until the whole operation finished.
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
}

void FirmwareUpdateDialog::refreshDeviceStatus()
{
    QString error;
    m_statusValid = m_updater.readStatus(&m_status, &error);

    if (!m_statusValid) {
        m_deviceInfo->setText(
            QStringLiteral("<span style='color:#c0392b'>%1</span>").arg(error.toHtmlEscaped()));
        return;
    }

    QStringList lines;
    lines << tr("Running firmware: <b>%1</b>")
                 .arg(versionText(m_status.running_major, m_status.running_minor,
                                  m_status.running_patch));

    if (m_status.bootloader_version == 0) {
        lines << tr("Bootloader: <span style='color:#c0392b'><b>none</b></span> — this device "
                    "cannot be updated over the wire.");
    } else {
        lines << tr("Bootloader: version %1").arg(m_status.bootloader_version);
    }
    lines << tr("Configuration format: v%1").arg(m_status.running_store_version);
    lines << tr("Largest image accepted: %1 KB").arg(m_status.staging_capacity / 1024);

    // What the bootloader did last time. This is the only place a failed
    // install can explain itself — the failure happened while nothing was
    // listening, so it was recorded rather than reported.
    if (m_status.last_result != FW_RESULT_NONE && m_status.last_result != FW_RESULT_OK) {
        lines << tr("<span style='color:#c0392b'>Last install attempt failed: %1"
                    "</span> (%2 attempt(s))")
                     .arg(FirmwareImage::resultText(m_status.last_result).toHtmlEscaped())
                     .arg(m_status.attempts);
    }
    if (m_status.state == FW_STATE_PENDING) {
        lines << tr("<b>An update is already staged and will install at the next restart.</b>");
    }

    m_deviceInfo->setText(lines.join(QStringLiteral("<br>")));
}

void FirmwareUpdateDialog::onBrowse()
{
    // Open where the image actually is. The installer stages the paired
    // can-triple-<version>.ctf into {app}\Firmware precisely so a bench machine
    // that never saw the repositories can restore a unit from it, and this
    // dialog used to pass no start directory at all — so it opened wherever the
    // last file dialog in the process happened to be, and the user navigated to
    // the install directory by hand.
    //
    // A build no installer ever ran has no such folder, and aiming a file
    // dialog at a path that does not exist is the one answer worse than aiming
    // it nowhere. An empty string is QFileDialog's "no preference": the old
    // behaviour, kept for exactly the case it was right for.
    QString startDirectory = firmwareImagesDirectory();
    if (!QDir(startDirectory).exists()) {
        startDirectory.clear();
    }

    const QString path = QFileDialog::getOpenFileName(
        this, tr("Select Firmware Image"), startDirectory,
        tr("CAN Triple firmware (*.ctf);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }

    QString error;
    auto image = FirmwareImage::load(path, &error);
    if (!image) {
        m_image.reset();
        m_pathEdit->clear();
        m_imageInfo->clear();
        // The file was rejected by the DEVICE'S OWN validator, so this verdict
        // is the same one the bootloader would reach — reported here without
        // spending a transfer to discover it.
        QMessageBox::warning(this, tr("Update Firmware"), error);
        updateReadiness();
        return;
    }

    m_image = std::move(image);
    m_pathEdit->setText(QDir::toNativeSeparators(path));

    QStringList lines;
    lines << tr("Version: <b>%1</b>").arg(m_image->versionString());
    lines << tr("Size: %1 bytes").arg(m_image->size());
    lines << tr("Configuration format: v%1").arg(m_image->flashStoreVersion());
    const QString desc = m_image->buildDescription();
    if (!desc.isEmpty()) {
        lines << tr("Build: %1").arg(desc.toHtmlEscaped());
    }
    m_imageInfo->setText(lines.join(QStringLiteral("<br>")));

    updateReadiness();
}

void FirmwareUpdateDialog::updateReadiness()
{
    QStringList warnings;
    bool blocked = false;

    if (!m_statusValid) {
        blocked = true;
    } else if (m_status.bootloader_version == 0) {
        warnings << tr("This device has no bootloader, so firmware cannot be installed over "
                       "the serial link. It needs a one-time bootloader installation using an "
                       "ST-Link debugger.");
        blocked = true;
    }

    if (m_image && m_statusValid && m_status.bootloader_version != 0) {
        if (m_image->minBootloaderVersion() > m_status.bootloader_version) {
            warnings << tr("This image needs bootloader version %1 but the device has version "
                           "%2. It cannot be installed.")
                            .arg(m_image->minBootloaderVersion())
                            .arg(m_status.bootloader_version);
            blocked = true;
        }
        if (m_image->size() > m_status.staging_capacity) {
            warnings << tr("This image is %1 bytes; the device accepts at most %2.")
                            .arg(m_image->size())
                            .arg(m_status.staging_capacity);
            blocked = true;
        }

        // The one that actually costs the user something.
        if (m_image->flashStoreVersion() != m_status.running_store_version) {
            warnings << tr("<b>This update changes the stored-configuration format "
                           "(v%1 → v%2).</b> The device's saved configuration will not survive "
                           "it and the unit will start up with no configuration. Keep the "
                           "backup option ticked — the configuration cannot be recovered from "
                           "the device afterwards.")
                            .arg(m_status.running_store_version)
                            .arg(m_image->flashStoreVersion());
        }

        const int cmp = m_image->compareVersion(m_status.running_major, m_status.running_minor,
                                                m_status.running_patch);
        if (cmp < 0) {
            warnings << tr("This image (%1) is OLDER than the firmware the device is running "
                           "(%2).")
                            .arg(m_image->versionString(),
                                 versionText(m_status.running_major, m_status.running_minor,
                                             m_status.running_patch));
        } else if (cmp == 0) {
            warnings << tr("The device is already running version %1. Installing it again is "
                           "harmless but changes nothing.")
                            .arg(m_image->versionString());
        }
    }

    if (warnings.isEmpty()) {
        m_warnings->setVisible(false);
    } else {
        m_warnings->setText(QStringLiteral("<div style='color:#b9770e'>⚠ %1</div>")
                                .arg(warnings.join(QStringLiteral("<br><br>⚠ "))));
        m_warnings->setVisible(true);
    }

    m_updateButton->setEnabled(m_image.has_value() && !blocked);
}

void FirmwareUpdateDialog::setBusy(bool busy)
{
    m_busy = busy; // reject()/closeEvent() consult this to block dismissal
    m_updateButton->setEnabled(!busy && m_image.has_value());
    m_browseButton->setEnabled(!busy);
    m_backupCheck->setEnabled(!busy);
    m_closeButton->setEnabled(!busy);
    m_progress->setVisible(busy);
}

bool FirmwareUpdateDialog::backupConfiguration(QString *savedPath, QString *error)
{
    say(tr("Reading the device's configuration…"));

    // ConfigTransfer is asynchronous; run it to completion inside a nested loop
    // so this whole procedure stays readable top to bottom.
    QEventLoop loop;
    bool ok = false;
    QString transferError;
    DeviceTables tables;
    QVector<ControlCanPayload> busSetup;
    QString deviceName;

    bool replyLost = false;
    auto *transfer = ConfigTransfer::get(m_link, this);
    connect(transfer, &ConfigTransfer::tablesReady, this,
            [&](const DeviceTables &t) { tables = t; });
    connect(transfer, &ConfigTransfer::finished, this,
            [&](bool success, const QString &err) {
                ok = success;
                transferError = err;
                if (success) {
                    busSetup = transfer->deviceBusSetup();
                    deviceName = transfer->deviceConfigName();
                    replyLost = transfer->anyReplyLost();
                }
                loop.quit();
            });
    loop.exec();

    if (!ok) {
        *error = transferError;
        return false;
    }

    // A Get maps a lost reply to an empty table, so a backup taken over a flaky
    // link can be silently short a table — and this backup is the safety net for
    // a firmware update that is about to erase the device (a format-changing
    // update leaves nothing to fall back on but this file). A truthful partial
    // read from old firmware is fine; a read we are not sure of is not. Refuse,
    // rather than write a backup that looks complete and would restore a
    // configuration missing whatever the lost reply covered.
    if (replyLost) {
        *error = tr("The device's configuration could not be read reliably — one or "
                    "more replies were lost. A backup taken now could be missing part "
                    "of the configuration, and this backup is the only copy once the "
                    "update erases the device. Check the connection and try again.");
        return false;
    }

    const bool deviceEmpty =
        tables.messages.isEmpty() && tables.signalConfigs.isEmpty() && tables.math.isEmpty()
        && tables.conditions.isEmpty() && tables.counters.isEmpty() && tables.timers.isEmpty()
        && tables.constants.isEmpty() && tables.relays.isEmpty()
        && tables.tables2x16Def.isEmpty() && tables.tables2x16Out.isEmpty()
        && tables.tables8x8Def.isEmpty() && tables.tables8x8Row.isEmpty()
        && tables.integrators.isEmpty() && tables.scriptChunks.isEmpty();
    if (deviceEmpty) {
        // Not a failure. Writing a file full of nothing would be worse than
        // writing none: it looks like a backup, and restoring it later would
        // clear a configuration rather than recover one.
        savedPath->clear();
        return true;
    }

    Configuration backup;
    mapFromDevice(tables, backup, nullptr, busSetup);
    if (!deviceName.isEmpty()) {
        backup.setConfigTitle(deviceName);
    }

    // Prove the folder before trusting it with the file. mkpath's answer was
    // thrown away here, which did not lose the failure — saveToFile below
    // refuses a path whose folder is missing, and onUpdate() stops the update
    // on that — but it lost the DIAGNOSIS, at the one moment the user is
    // relying on this file existing: what came back was a QFile sentence about
    // a timestamped .ct3 nobody chose the name of, when the thing that is
    // actually wrong is a folder.
    //
    // And it hid the second failure entirely. mkpath returns true for a folder
    // that merely EXISTS, so one that exists and refuses the write passed for
    // success here and surfaced as "Access is denied" from the writer instead.
    // The probe inside ensureWritableDirectory is what separates the two, and
    // both now come back as a sentence naming the folder, before anything is
    // written and well before the device is touched.
    if (!ensureWritableDirectory(backupDirectory(), error)) {
        return false;
    }

    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    const QString path = QStringLiteral("%1/device-config-%2.ct3").arg(backupDirectory(), stamp);

    QString saveError;
    if (!backup.saveToFile(path, &saveError)) {
        *error = tr("Could not write the backup to %1: %2")
                     .arg(QDir::toNativeSeparators(path), saveError);
        return false;
    }

    *savedPath = path;
    return true;
}

bool FirmwareUpdateDialog::waitForDeviceToReturn(QString *error)
{
    const QString port = m_link->portName();
    const qint32 baud = m_link->baudRate();

    m_link->close();

    // The bootloader erases and copies the application slot before it hands
    // over — roughly a second for a typical image, and the serial port may also
    // disappear and re-enumerate. Poll rather than guessing a fixed delay.
    constexpr int kTotalWaitMs = 25000;
    constexpr int kPollMs = 400;

    for (int waited = 0; waited < kTotalWaitMs; waited += kPollMs) {
        pump(kPollMs);
        say(tr("Waiting for the device to restart… (%1s)").arg(waited / 1000));

        if (!m_link->open(port, baud, nullptr)) {
            continue;   // port not back yet
        }
        // Open is not enough — a port can open while the device behind it is
        // still installing. Require an actual answer.
        QByteArray reply;
        if (m_link->requestSync(CMD_GET_STATUS, QByteArray(), &reply, nullptr,
                                /*timeoutMs=*/500, /*retries=*/1)) {
            return true;
        }
        m_link->close();
    }

    *error = tr("The device did not come back within %1 seconds.\n\n"
                "It may still be installing. Wait a moment, then reconnect from the "
                "Online menu and open this dialog again to see what happened.")
                 .arg(kTotalWaitMs / 1000);
    return false;
}

void FirmwareUpdateDialog::offerConfigurationRestore(const QString &backupPath)
{
    const QString question =
        tr("The device restarted with no configuration, as expected for this update.\n\n"
           "Send the backup taken a moment ago back to the device?\n\n%1")
            .arg(QDir::toNativeSeparators(backupPath));
    if (QMessageBox::question(this, tr("Restore Configuration"), question,
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes)
        != QMessageBox::Yes) {
        return;
    }

    // Re-prove the Send password before writing anything.
    //
    // Access proofs live in the DEVICE's RAM and belong to the serial session.
    // The reset that installed this firmware wiped them, so as far as the unit
    // is concerned this host has never proved anything — and on a
    // password-protected device every step of the Send below would come back
    // ERR_LOCKED. The failure would look like the restore being broken rather
    // than like a password needing re-entry, which is the sort of confusion
    // that gets a good backup abandoned.
    if (m_reproveSend && !m_reproveSend()) {
        QMessageBox::information(
            this, tr("Restore Configuration"),
            tr("The configuration was not restored because the device password "
               "was not provided.\n\nThe backup is still on disk:\n%1\n\n"
               "Open it with File > Open and send it when you are ready.")
                .arg(QDir::toNativeSeparators(backupPath)));
        return;
    }

    Configuration restored;
    QString loadError;
    if (!restored.loadFromFile(backupPath, &loadError)) {
        QMessageBox::warning(this, tr("Restore Configuration"),
                             tr("Could not read the backup: %1").arg(loadError));
        return;
    }

    // mapWithScript, never plain mapToDevice: mapToDevice leaves scriptChunks
    // empty, and empty chunks are precisely how a script is REMOVED from a
    // device. This is the worst line in the product to get that wrong. The
    // backup above is taken through mapFromDevice, so it now carries the unit's
    // compiled script; the update then erases the stored configuration, which
    // makes that backup the only copy of the script in existence; and this is
    // the line that is supposed to put it back.
    const MappingResult mapped = mapWithScript(restored);
    if (!mapped.ok()) {
        QMessageBox::warning(
            this, tr("Restore Configuration"),
            tr("The backup could not be prepared for this firmware:\n\n%1\n\n"
               "The file is still on disk; open it with File > Open and send it manually.")
                .arg(mapped.errors.join(QStringLiteral("\n"))));
        return;
    }

    QVector<ControlCanPayload> busSetups;
    for (int i = 0; i < 3; ++i) {
        ControlCanPayload setup {};
        setup.bus_idx = quint8(i + 1);
        setup.mode = restored.bus[i].enabled ? 1 : 0;
        setup.baud_rate = busRateHz(restored.bus[i].rateKbps);
        setup.data_baud_rate = busRateHz(restored.bus[i].dataRateKbps);
        setup.termination = restored.bus[i].termination ? 1 : 0;
        busSetups.append(setup);
    }

    say(tr("Restoring configuration…"));
    QEventLoop loop;
    bool ok = false;
    QString sendError;
    auto *transfer = ConfigTransfer::send(
        m_link, mapped.tables, /*verify=*/true, busSetups, /*saveToFlash=*/true,
        restored.fleetIdentity().configVersion, restored.effectiveTitle(),
        /*resetAfter=*/false, this);
    connect(transfer, &ConfigTransfer::progress, this,
            [this](int done, int total, const QString &stage) {
                m_progress->setMaximum(total);
                m_progress->setValue(done);
                say(stage);
            });
    connect(transfer, &ConfigTransfer::finished, this,
            [&](bool success, const QString &err) {
                ok = success;
                sendError = err;
                loop.quit();
            });
    loop.exec();

    if (ok) {
        QMessageBox::information(this, tr("Restore Configuration"),
                                 tr("The configuration was restored and saved to the device."));
    } else {
        QMessageBox::warning(
            this, tr("Restore Configuration"),
            tr("The configuration could not be restored: %1\n\n"
               "The backup is still on disk; open it with File > Open and send it manually.")
                .arg(sendError));
    }
}

void FirmwareUpdateDialog::onUpdate()
{
    if (!m_image) {
        return;
    }

    const bool formatChanges =
        m_image->flashStoreVersion() != m_status.running_store_version;

    QString confirm = tr("Install firmware %1 on this device?\n\n"
                         "The image is sent to a spare area of flash first, so the device "
                         "keeps running its current firmware until it restarts. If anything "
                         "goes wrong during the transfer, nothing is lost.")
                          .arg(m_image->versionString());
    if (formatChanges) {
        confirm += tr("\n\nThe device's stored configuration WILL be cleared by this update.");
    }
    if (!m_backupCheck->isChecked()) {
        confirm += tr("\n\nNo backup will be taken.");
    }
    if (QMessageBox::question(this, tr("Update Firmware"), confirm,
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
        != QMessageBox::Yes) {
        return;
    }

    setBusy(true);
    m_progress->setValue(0);

    QString backupPath;
    if (m_backupCheck->isChecked()) {
        QString error;
        if (!backupConfiguration(&backupPath, &error)) {
            setBusy(false);
            say(QString());
            // Stop rather than continue. The user asked for a backup; going
            // ahead without one after failing to take it would destroy exactly
            // the data they were trying to protect.
            QMessageBox::warning(
                this, tr("Update Firmware"),
                tr("The configuration could not be backed up, so the update was not "
                   "started:\n\n%1\n\nUntick the backup option to update anyway.")
                    .arg(error));
            return;
        }
        if (backupPath.isEmpty()) {
            say(tr("The device had no configuration to back up."));
        } else {
            say(tr("Configuration saved to %1").arg(QDir::toNativeSeparators(backupPath)));
        }
    }

    QString error;
    if (!m_updater.upload(*m_image, &error)) {
        setBusy(false);
        m_progress->setVisible(false);
        say(QString());
        QMessageBox::warning(this, tr("Update Firmware"),
                             tr("The update was not installed:\n\n%1\n\n"
                                "The device is still running its existing firmware.")
                                 .arg(error));
        refreshDeviceStatus();
        return;
    }

    say(tr("Restarting the device to install…"));
    if (!m_updater.requestReset(&error)) {
        setBusy(false);
        QMessageBox::warning(
            this, tr("Update Firmware"),
            tr("The firmware was sent and verified, but the device did not accept the "
               "restart command:\n\n%1\n\nPower-cycle the device to install it.")
                .arg(error));
        return;
    }

    if (!waitForDeviceToReturn(&error)) {
        setBusy(false);
        m_progress->setVisible(false);
        QMessageBox::warning(this, tr("Update Firmware"), error);
        return;
    }

    // What actually happened, from the device rather than from hope.
    refreshDeviceStatus();
    setBusy(false);
    m_progress->setVisible(false);

    if (!m_statusValid) {
        say(QString());
        QMessageBox::warning(this, tr("Update Firmware"),
                             tr("The device restarted but did not answer a status request. "
                                "Reconnect from the Online menu and check its firmware version."));
        return;
    }

    const bool installed =
        m_image->compareVersion(m_status.running_major, m_status.running_minor,
                                m_status.running_patch) == 0;
    if (!installed) {
        say(QString());
        QMessageBox::warning(
            this, tr("Update Firmware"),
            tr("The device restarted but is running version %1, not %2.\n\nThe bootloader "
               "reported: %3\n\nThe previous firmware is intact and running.")
                .arg(versionText(m_status.running_major, m_status.running_minor,
                                 m_status.running_patch),
                     m_image->versionString(),
                     FirmwareImage::resultText(m_status.last_result)));
        return;
    }

    say(tr("Firmware %1 installed.").arg(m_image->versionString()));

    // Only offer the restore when the configuration really is gone. Asking
    // after an update that preserved it would invite the user to overwrite a
    // perfectly good configuration with an older copy of itself.
    device_session::Identity identity;
    QString idError;
    const bool haveIdentity = device_session::readIdentity(m_link, &identity, &idError);
    const bool configGone =
        haveIdentity && identity.supported && identity.configStatus == CONFIG_STATUS_NONE;

    if (configGone && !backupPath.isEmpty()) {
        offerConfigurationRestore(backupPath);
    } else if (configGone) {
        QMessageBox::information(
            this, tr("Update Firmware"),
            tr("Firmware %1 is installed.\n\nThe device has no stored configuration — this "
               "update changed the configuration format. Send a configuration to the device "
               "when you are ready.")
                .arg(m_image->versionString()));
    } else {
        QMessageBox::information(this, tr("Update Firmware"),
                                 tr("Firmware %1 is installed and the device's configuration "
                                    "is intact.")
                                     .arg(m_image->versionString()));
    }
}

} // namespace ct
