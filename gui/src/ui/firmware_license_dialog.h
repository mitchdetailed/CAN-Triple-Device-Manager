// Online > Firmware License Manager — who this firmware is licensed to.
//
// Replaces the Fleet Identity dialog, and is a different kind of thing despite
// looking similar. Fleet Identity edited a DOCUMENT: four values saved into a
// .ct3 that an uploader later compared against a device whose own identity was
// compiled into its binary. This edits the DEVICE. There is no document half,
// no "apply to configuration" and nothing to save afterwards — Apply writes the
// record into the unit's flash and the unit is where it lives.
//
// ---------------------------------------------------------------------------
// TWO SECRETS, AND THEY ARE NOT INTERCHANGEABLE
//
//   FW Updater Password — the GATE. Blank means anyone with a cable may write
//     these details. Set means the device demands it first, and this dialog
//     prompts for it before Apply will go through. This is the one that
//     protects the record.
//
//   Firmware Key — the CLAIM. It authorises nothing. It is the value a unit
//     proves in order to show which licence it holds, for an upload policy to
//     check against. A unit can carry a key and still be freely rewritable,
//     which is the right state for a board that has been given an identity but
//     not yet locked down.
//
// BOTH ARE WRITE-ONLY, which shapes the whole dialog. The device will not
// disclose either, so neither field can ever be populated with what is already
// there. They show blank with a placeholder saying so; blank means keep. That
// also means the dialog cannot verify a passphrase locally — proving one is a
// round trip, which is exactly what the device demands before it will write.
//
// ---------------------------------------------------------------------------
// The dialog opens WITHOUT a connection. Composing a licence is desk work, and
// requiring a unit on the cable before you can even see the fields would make
// it impossible to prepare one. Apply is what needs hardware: it connects,
// prompts for the password if the unit wants one, and only then writes.
#pragma once

#include <QByteArray>
#include <QDialog>

#include <functional>

#include "../protocol/device_session.h"

class QCheckBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;

namespace ct {

class DeviceLink;

class FirmwareLicenseDialog : public QDialog
{
    Q_OBJECT

public:
    // `ensureConnected` is the owner's connect-or-prompt routine, passed in
    // rather than reached for: this dialog has no business knowing about
    // ConnectionSettingsDialog, and a test can hand it a stub.
    FirmwareLicenseDialog(DeviceLink *link, std::function<bool()> ensureConnected,
                          QWidget *parent = nullptr);

private:
    void reload();          // read the device's record into the fields
    void refreshEnabled();  // Apply enabled, and what the state lines say
    void apply();
    // Prove the FW Updater Password if the unit holds one, prompting until it is
    // right or the user gives up. True when the session may write.
    bool ensureProved();

    DeviceLink *m_link = nullptr;
    std::function<bool()> m_ensureConnected;
    device_session::LicenseState m_current;
    bool m_proved = false;

    QLineEdit *m_manufacturer = nullptr;
    QLineEdit *m_model = nullptr;
    QLineEdit *m_version = nullptr;
    QLineEdit *m_key = nullptr;
    QLineEdit *m_updater = nullptr;
    // Removing a password cannot be expressed by the field itself: blank there
    // already means KEEP, which is what makes editing a model name bearable. So
    // the removal is a separate, deliberate act — and one that is only offered
    // while the field is empty, since "set it to this" and "take it away" are
    // contradictory instructions.
    QCheckBox *m_clearUpdater = nullptr;
    QLabel *m_state = nullptr;
    QLabel *m_warning = nullptr;
    QDialogButtonBox *m_buttons = nullptr;
};

} // namespace ct
