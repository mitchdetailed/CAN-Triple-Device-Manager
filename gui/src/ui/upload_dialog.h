// "Upload Configuration" — Online > Upload Configuration…
//
// The uploader. Send Configuration is the engineer's command: it asks few
// questions and assumes you know what is on the bench. This is the one you hand
// to a dealer, an installer, or a customer — it opens a .ct3s, reads the unit in
// front of it, and refuses to install anything the package was not built for.
//
// Every rule is shown pass or fail BEFORE anything is written, because the
// failure this exists to prevent is silent: a configuration that installs
// cleanly on the wrong product and misbehaves a week later on a track.
//
//   Vendor ID       exact match against the package
//   Model ID        exact match against the package
//   Serial Number   must be in the package's allow-list, when it pins one
//   Fleet Key       the device must PROVE it, not merely claim the identity
//   Config Version  warns (never refuses) if the unit already runs a newer one
//
// None of these read a byte of the device's configuration — that is the point.
// A locked-down unit can be checked completely without being opened, which is
// what lets a customer take updates for a protected config they cannot read.
//
// What it is not: the fleet key is shared across a fleet, so one dumped unit
// compromises attestation for every unit that shares it. Serial pinning narrows
// WHICH unit an update reaches; a serial is public and is not a second secret. This
// stops the wrong file reaching the wrong product and a look-alike collecting
// someone else's update. It is not DRM and it is not a signature.
#pragma once

#include <QDialog>
#include <QList>
#include <QString>

#include "../model/access_keys.h"
#include "../model/configuration.h"
#include "../protocol/device_link.h"
#include "../protocol/device_session.h"

class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTreeWidget;

namespace ct {

// One rule's verdict. Kept as data rather than rendered on the spot so the Send
// path can apply the same rules without drawing this dialog.
struct UploadRule
{
    enum Status {
        Pass,
        // Worth saying, not worth stopping for. Only the version rule warns:
        // installing an older revision is a real thing people do on purpose.
        Warn,
        // Refuses the upload outright. There is no override — a rule that can be
        // clicked past is a warning wearing a costume, and an installer handed
        // this dialog has no way to judge which is which.
        Fail,
        NotChecked // nothing to compare — an unprovisioned unit or package
    };
    QString name;     // "Model ID"
    QString expected; // what the package wants; never quotes config content
    QString actual;   // what the device says
    Status status = NotChecked;
    // One finished sentence naming what is wrong, set by whichever branch
    // decided the rule failed or warned. Phrased at the point the decision is
    // made rather than assembled later from name/expected/actual, because the
    // branches differ in kind: "the device reports a different vendor" and "the
    // device has no vendor at all" are both a vendor failure and want different
    // words. A generic "%1: expected %2, got %3" can only say the first.
    QString message;
};

// The whole verdict.
struct UploadVerdict
{
    QList<UploadRule> rules;
    bool deviceReadable = false; // false when offline or the firmware is too old
    QString problem;             // set when deviceReadable is false

    // Nothing failed, nothing warned, and at least one rule actually ran — the
    // quiet case. A verdict of nothing but NotChecked is NOT this: it
    // established nothing at all, which is a third answer and reads as neither
    // a pass nor a refusal. See the definition.
    bool allPassed() const;
    // Any Fail. This and only this refuses the upload.
    bool hasBlockingFailure() const;
    QStringList failureSummaries() const;
    QStringList warningSummaries() const;
};

class UploadDialog : public QDialog
{
    Q_OBJECT
public:
    UploadDialog(DeviceLink *link, Configuration *config, QWidget *parent = nullptr);

    // Apply the package's rules to the connected device. Reads the device's
    // fleet identity and challenges its fleet key; never touches the stored
    // configuration. Shared with Send Configuration so the two can never
    // disagree about what counts as a match.
    static UploadVerdict evaluate(DeviceLink *link, const Configuration &config);

private:
    void refresh();
    void onUpload();
    void onOpenPackage();

    DeviceLink *m_link;
    Configuration *m_config;
    UploadVerdict m_verdict;
    // Set only by a package opened through this dialog's own button, and it is
    // what arms Upload. The application's open document is deliberately not
    // enough: an unsaved, empty, or half-edited document checks out against a
    // device as five NotChecked rules, which used to render as a green summary
    // and an enabled Upload — one click away from wiping a customer's unit with
    // nothing. This dialog cannot tell a package it was handed from a scratch
    // file somebody left open, so it does not guess; opening the file here is
    // the act that makes it unambiguous.
    bool m_packageOpened;

    QLineEdit *m_packageEdit;    // the .ct3s currently loaded, read-only
    QPushButton *m_openButton;
    QTreeWidget *m_ruleTree;     // Rule | Package wants | Device says | verdict
    QLabel *m_summaryLabel;
    QPushButton *m_uploadButton;
    QDialogButtonBox *m_buttons;
};

} // namespace ct
