// File > Secure Configuration Builder — turns a .ct3 into a .ct3s package that
// only installs where it is meant to.
//
// Replaces Save Secure Config, and is a different shape of tool. That command
// saved THE OPEN DOCUMENT in a second format; this one takes a configuration as
// an input, attaches a policy to it, and produces a deployable artefact. The
// open document is offered as the default source because it usually is the
// source, but nothing here depends on it.
//
// ---------------------------------------------------------------------------
// The policy has two halves and they are not alike.
//
// The MATCH half decides where the package may install. Manufacturer, model and
// version are each optional; the Firmware Key is not, and has no checkbox for
// that reason. Every package names a key and every target must prove it, which
// means an unlicensed unit takes no packages at all — issue a licence with the
// Firmware License Manager first.
//
// The UPDATE half changes the device's access passwords as the package
// installs. That is possible at all because the Firmware Key is a master key:
// the device lets a host that proved it overwrite passwords nobody present
// knows. It is worth doing HERE, rather than through Set Access Passwords,
// because an install is the one moment it reliably sticks — access keys live in
// the config store's write-once header, and an install is what erases and
// re-commits that header.
//
// A ticked box with an empty field CLEARS that password. Unticked leaves it
// alone. Those are different instructions and the file records them differently.
#pragma once

#include <QDialog>

#include "../model/configuration.h"
#include "../model/secure_file.h"

class QCheckBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QSpinBox;

namespace ct {

class SecureBuilderDialog : public QDialog
{
    Q_OBJECT

public:
    // `openDocumentPath` seeds the source field; empty is fine and means the
    // user must browse. The open Configuration is NOT taken by reference on
    // purpose — the builder loads its source from disk every time, so a package
    // is built from a file somebody can point at rather than from whatever
    // happens to be in memory and possibly unsaved.
    explicit SecureBuilderDialog(const QString &openDocumentPath, QWidget *parent = nullptr);

private:
    void browseSource();
    void refreshEnabled();
    void build();
    // Collect the form into a policy. Returns false with `why` filled when the
    // form does not describe an installable package.
    bool collectPolicy(SecurePackagePolicy *out, QString *why) const;

    QLineEdit *m_source = nullptr;
    // The revision this package stamps on the unit. Sits with the source rather
    // than with the matches because it is a property of what is being shipped,
    // not a condition on where.
    QSpinBox *m_packageVersion = nullptr;

    QCheckBox *m_matchManufacturerCheck = nullptr;
    QLineEdit *m_matchManufacturer = nullptr;
    QCheckBox *m_matchModelCheck = nullptr;
    QLineEdit *m_matchModel = nullptr;
    QCheckBox *m_matchVersionCheck = nullptr;
    QLineEdit *m_matchVersion = nullptr;
    // No checkbox: the key always applies.
    QLineEdit *m_key = nullptr;

    QCheckBox *m_setSendCheck = nullptr;
    QLineEdit *m_setSend = nullptr;
    QCheckBox *m_setGetCheck = nullptr;
    QLineEdit *m_setGet = nullptr;
    QCheckBox *m_setSlotCheck[4] = {nullptr, nullptr, nullptr, nullptr};
    QLineEdit *m_setSlot[4] = {nullptr, nullptr, nullptr, nullptr};

    QLabel *m_warning = nullptr;
    QDialogButtonBox *m_buttons = nullptr;
};

} // namespace ct
