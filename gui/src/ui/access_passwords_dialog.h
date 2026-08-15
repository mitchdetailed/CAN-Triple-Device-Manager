// "Set Access Passwords" — Online > Set Access Passwords…, laid out like the
// same screen in MoTeC Dash Manager: a list of protected functions, a Set…
// button, and a tick against the ones that have a password.
//
//     ┌ Function Passwords ────────────────────────────────┐
//     │ Select the function to set the password for :      │
//     │ ┌───────────────────────────────┐    ┌────────┐    │
//     │ │ Function                      │    │ Set…   │    │
//     │ │ ✓ Send a Configuration        │    └────────┘    │
//     │ │   Get a Configuration         │                  │
//     │ │   Edit Protected Comms        │                  │
//     │ └───────────────────────────────┘                  │
//     │ ✓ = Password set                                   │
//     └────────────────────────────────────────────────────┘
//
// The passwords live in the DEVICE, so this needs a connection — which is why
// it sits under Online rather than File. What the dialog shows is read back
// from the unit each time it opens; it never caches "what we last set", because
// the interesting case is a device someone else configured.
//
// Setting follows Dash Manager exactly: choose a function, click Set…, and if a
// password is already in force give the old one before the new. Entering a
// BLANK new password clears it. The new password is asked for twice, so a
// typo cannot lock a device against its owner.
//
// Edit Protected Comms is the one with a foot in both worlds: it gates the
// device, and it also decides whether this app will reveal messages marked
// "Protect Communication". Setting it here therefore also writes a verifier
// into the open document, so the two stay in step — the same requirement Dash
// Manager states as "the password must match the one used in the template".
#pragma once

#include <QDialog>

#include "../model/access_keys.h"
#include "../model/configuration.h"
#include "../protocol/device_link.h"
#include "../protocol/device_session.h"

class QLabel;
class QPushButton;
class QTreeWidget;

namespace ct {

class AccessPasswordsDialog : public QDialog
{
    Q_OBJECT
public:
    // `config` may be null for a device-only session; when it is not, setting or
    // clearing Edit Protected Comms updates the document to match.
    AccessPasswordsDialog(DeviceLink *link, Configuration *config, QWidget *parent = nullptr);

    // Ask for one function's password and prove it against `link`, re-asking
    // until it is right or the user gives up. Shared with the Send/Get paths, so
    // the prompt a user sees before a Send is the same one they see here.
    // Returns false on cancel or link failure. `keyOut` receives the derived
    // 4-byte key on success, so a caller can reuse it without re-deriving.
    static bool promptAndProve(DeviceLink *link, AccessFunction fn, QWidget *parent,
                               AccessKey *keyOut = nullptr);

private:
    void refreshList();
    void onSet();
    void onSelectionChanged();
    // The Set… flow for one function: old password if there is one, then the new
    // one twice. Returns false if the user backed out or the device refused.
    bool runSetFlow(AccessFunction fn);
    AccessFunction selectedFunction() const;

    DeviceLink *m_link;
    Configuration *m_config;
    device_session::AccessState m_state;
    QTreeWidget *m_functionList;
    QPushButton *m_setButton;
    QLabel *m_legendLabel;
    QLabel *m_statusLabel;
};

} // namespace ct
