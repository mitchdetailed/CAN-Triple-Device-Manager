// Online > Update Firmware…
//
// Walks a firmware update end to end: read what the device is running, parse
// and vet the .ctf, back up the stored configuration, send the image, reboot,
// wait for the device to return, and report what the bootloader made of it.
//
// The backup is not decoration. A firmware update that changes
// FLASH_STORE_VERSION — or, since firmware 1.0.11, that lays the tables out
// differently at the same version (a different VARIANT) — makes the stored
// configuration unreadable, and the device comes back blank with no way to
// recover it from the device — the bytes are still in flash but nothing can
// interpret them. Reading the configuration out BEFORE the update is the only
// moment that data can be saved, so this dialog does it by default and offers
// to send it back afterwards.
#pragma once

#include <QDialog>
#include <QStringList>

#include <functional>
#include <optional>

#include "../protocol/capacity.h"
#include "../protocol/device_link.h"
#include "../protocol/firmware_image.h"
#include "../protocol/firmware_update.h"

class QCheckBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;

namespace ct {

class FirmwareUpdateDialog : public QDialog
{
    Q_OBJECT
public:
    // reproveSend re-establishes the Send-Configuration password on the device,
    // prompting if it has to, and returns whether the session may now write.
    //
    // It is a callback rather than something this dialog does itself because
    // the prompting, the remembering and the "does this device even have a
    // password set" check all live in MainWindow, and duplicating them here
    // would be a second implementation of the one thing that must not disagree.
    //
    // It exists because access proofs are PER SESSION and live in the device's
    // RAM. The reset in the middle of an update wipes them, so the restore Send
    // afterwards is talking to a device that has never heard of this host — and
    // without re-proving, every restore on a password-protected unit fails with
    // ERR_LOCKED. Passing nothing is allowed and means "cannot re-prove": the
    // restore is then still offered, and simply reports the lock if it hits one.
    using ReproveFn = std::function<bool()>;

    // reproveGet proves the Get password again after the device's reset has
    // cleared it — the status read that confirms an install is gated on Get.
    explicit FirmwareUpdateDialog(DeviceLink *link, QWidget *parent = nullptr,
                                  ReproveFn reproveSend = {}, ReproveFn reproveGet = {});

signals:
    // F1, or the Help button. A signal rather than another constructor callback
    // because the manual is owned by MainWindow and every dialog that grows
    // context help will want the same wiring.
    void helpRequested(const QString &pageFileName);

public:
    // Overridden to refuse dismissal while an update runs. The update continues
    // on nested event loops whether the window is visible or not, so an Esc or
    // the Close button would leave it running headless — modality released, the
    // device mid-flash, a second Update dialog reachable. The device's own CRC
    // stops a corrupt install, so blocking the dismissal until the update
    // finishes is enough; there is nothing to roll back. Public to match
    // QDialog::reject(), a public slot.
    void reject() override;

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void closeEvent(QCloseEvent *event) override; // title-bar X, same guard as reject()
    // The first device read is deferred to here so the window paints before it
    // blocks on the serial round trip, rather than freezing during construction.
    void showEvent(QShowEvent *event) override;

private:
    void buildUi();
    void refreshDeviceStatus();
    void onBrowse();
    void onUpdate();
    void updateReadiness();
    void setBusy(bool busy);
    void say(const QString &text);

    // Read the device's configuration and write it to a timestamped .ct3.
    // Returns false with *error set; the caller decides whether to continue.
    bool backupConfiguration(QString *savedPath, QString *error);

    // Close the port, wait for the device to finish installing and re-enumerate,
    // and reopen. The install itself erases and copies the application slot, so
    // the device is genuinely gone for a second or two.
    bool waitForDeviceToReturn(QString *error);

    // After a successful update whose store version changed, offer to send the
    // backup back. Only called when the device really did come back empty.
    void offerConfigurationRestore(const QString &backupPath);

    // Why the stored configuration will NOT survive installing m_image on this
    // unit, one line per difference — a store-version change, or a table the
    // image lays out differently (a variant: "CRC8 rules: 20 → 40"). Empty
    // when it survives, or when neither side can say more than its version
    // and the versions agree. The one predicate behind the warning, the
    // confirmation and the post-update wording, so they cannot disagree.
    QStringList layoutChanges() const;

    DeviceLink *m_link;
    FirmwareUpdater m_updater;
    ReproveFn m_reproveSend;
    ReproveFn m_reproveGet;

    std::optional<FirmwareImage> m_image;
    FwUpdateStatus m_status {};
    // The unit's capacity report, read with the status. nullopt when the
    // firmware predates the report (then its version is all it can say).
    std::optional<DeviceCapacity> m_deviceCapacity;
    bool m_statusValid = false;
    bool m_busy = false;            // an update is running; dismissal is blocked
    bool m_firstShow = true;        // defer the initial device read to showEvent

    QLabel *m_deviceInfo = nullptr;
    QLineEdit *m_pathEdit = nullptr;
    QPushButton *m_browseButton = nullptr;
    QLabel *m_imageInfo = nullptr;
    QLabel *m_warnings = nullptr;
    QCheckBox *m_backupCheck = nullptr;
    QProgressBar *m_progress = nullptr;
    QLabel *m_statusLine = nullptr;
    QPushButton *m_updateButton = nullptr;
    QPushButton *m_closeButton = nullptr;
};

} // namespace ct
