// "Fleet Identity" — Online > Fleet Identity…
//
// Two panels that are NOT symmetrical, and the asymmetry is the whole design:
//
//   This Device        read-only. A unit's identity is compiled into its
//                      firmware (firmware/include/fleet_identity.h), so nothing
//                      here can change it and nothing on the wire can either.
//                      Re-badging a unit means editing its build flags and
//                      flashing it. That is what makes the identity worth
//                      believing — an attacker cannot send a packet to become
//                      somebody else, and a flash erase cannot lose it.
//
//   This Configuration editable. Says which fleet this file is FOR, and how
//                      strictly a device must match before the uploader will
//                      install it.
//
// The problem it solves: you ship a customer a device running a configuration
// whose CAN protocol is yours. Six months later you send them a new one. They
// must be able to install it, they must not be able to read it, and it must not
// install on anything it was not built for.
#pragma once

#include <QDialog>

#include "../model/access_keys.h"
#include "../model/configuration.h"
#include "../protocol/device_link.h"
#include "../protocol/device_session.h"

class QCheckBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QSpinBox;

namespace ct {

class FleetIdentityDialog : public QDialog
{
    Q_OBJECT
public:
    FleetIdentityDialog(DeviceLink *link, Configuration *config, QWidget *parent = nullptr);

    // Fill the configuration's identity from the connected device, so building
    // an update for a unit in front of you does not mean retyping its vendor and
    // model and getting one character wrong. Returns false when there is no
    // device, no support, or nothing programmed.
    static bool copyFromDevice(DeviceLink *link, Configuration *config, QWidget *parent);

private:
    void loadFromDevice();
    void onApplyToDocument();
    void onCopyFromDevice();
    void updateStatus();
    // Re-derives the key from the passphrase and shows the build-flag line.
    // Called on focus-out, not on every keystroke — see the call site. Only
    // ever from a passphrase typed here: it must never show the key the open
    // configuration already carries, because a .ct3s carries one and printing
    // it would hand the fleet's secret to whoever was merely sent a package.
    // See the definition.
    void refreshFleetKeyFlag();
    FleetIdentity identityFromFields() const;
    UploadPolicy policyFromFields() const;

    DeviceLink *m_link;
    Configuration *m_config;
    device_session::FleetIdentityState m_deviceState;

    QLineEdit *m_vendorEdit;      // <= 16 UTF-8 bytes, enforced live
    QLineEdit *m_modelEdit;       // <= 16 UTF-8 bytes, enforced live
    QSpinBox *m_versionSpin;
    QLineEdit *m_fleetKeyEdit;   // a passphrase, folded to the 4-byte key
    QLineEdit *m_fleetKeyFlagEdit; // read-only echo of the -DCT_FLEET_KEY line
    QPlainTextEdit *m_serialsEdit; // the allow-list, one serial per line
    QCheckBox *m_requireKeyCheck;
    QCheckBox *m_requireNewerCheck;
    QLabel *m_deviceLabel;
    QLabel *m_statusLabel;
    QDialogButtonBox *m_buttons;
};

} // namespace ct
