#include "secure_builder_dialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include "../model/access_keys.h"
#include "../protocol/wire_structs.h"
#include "name_limits.h"

namespace ct {

namespace {

// One "[x] Label: [field]" row. Returned pair is the box and the field, both
// owned by the form. The field starts disabled and follows the box, which is the
// whole convention of this dialog: an unticked row is an instruction the package
// does not carry, and a greyed field is what says so.
struct Row {
    QCheckBox *check;
    QLineEdit *edit;
};

Row addRow(QFormLayout *form, const QString &label, QWidget *parent, bool secret)
{
    auto *check = new QCheckBox(label, parent);
    auto *edit = new QLineEdit(parent);
    if (secret)
        edit->setEchoMode(QLineEdit::Password);
    edit->setEnabled(false);
    QObject::connect(check, &QCheckBox::toggled, edit, &QLineEdit::setEnabled);
    form->addRow(check, edit);
    return {check, edit};
}

} // namespace

SecureBuilderDialog::SecureBuilderDialog(const QString &openDocumentPath, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Secure Configuration Builder"));

    auto *layout = new QVBoxLayout(this);
    auto *intro = new QLabel(
        tr("Builds a secure package (.ct3s) from a configuration, with a policy that decides "
           "which devices it may install on and what it changes there."),
        this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    // ---- source
    auto *sourceGroup = new QGroupBox(tr("Configuration"), this);
    auto *sourceRow = new QHBoxLayout(sourceGroup);
    m_source = new QLineEdit(openDocumentPath, sourceGroup);
    m_source->setPlaceholderText(tr("Choose a .ct3 to package"));
    auto *browse = new QPushButton(tr("Browse…"), sourceGroup);
    sourceRow->addWidget(m_source, 1);
    sourceRow->addWidget(browse);
    layout->addWidget(sourceGroup);

    // The package's revision. Stamped on the unit when it installs and shown by
    // Device Status afterwards, so a fleet can be asked which release it runs.
    // 0 is "unversioned", which makes no claim; number releases upward.
    auto *versionForm = new QFormLayout;
    m_packageVersion = new QSpinBox(this);
    m_packageVersion->setRange(0, 65535);
    m_packageVersion->setToolTip(
        tr("Recorded on the device when this package installs. 0 means unversioned."));
    versionForm->addRow(tr("Package version:"), m_packageVersion);
    layout->addLayout(versionForm);
    connect(browse, &QPushButton::clicked, this, &SecureBuilderDialog::browseSource);

    // ---- match
    auto *matchGroup = new QGroupBox(tr("Install only on devices matching"), this);
    auto *matchForm = new QFormLayout(matchGroup);
    Row r = addRow(matchForm, tr("Match FW Manufacturer:"), matchGroup, false);
    m_matchManufacturerCheck = r.check;
    m_matchManufacturer = r.edit;
    r = addRow(matchForm, tr("Match FW Model:"), matchGroup, false);
    m_matchModelCheck = r.check;
    m_matchModel = r.edit;
    r = addRow(matchForm, tr("Match FW Version:"), matchGroup, false);
    m_matchVersionCheck = r.check;
    m_matchVersion = r.edit;

    // The key has no checkbox and is always enabled. Every package names one and
    // every target proves it — see the header for why that is not negotiable.
    m_key = new QLineEdit(matchGroup);
    m_key->setEchoMode(QLineEdit::Password);
    m_key->setMaxLength(LICENSE_PASSPHRASE_MAX);
    matchForm->addRow(tr("Match FW Key (required):"), m_key);
    layout->addWidget(matchGroup);

    // The three string matches are compared against the device's licence, whose
    // fields are fixed-width byte arrays — so a match longer than the field
    // could never succeed. Capping here means the form cannot express one.
    limitToUtf8Bytes(m_matchManufacturer, LICENSE_MANUFACTURER_LEN);
    limitToUtf8Bytes(m_matchModel, LICENSE_MODEL_LEN);
    limitToUtf8Bytes(m_matchVersion, LICENSE_VERSION_LEN);

    // ---- passwords
    auto *pwGroup = new QGroupBox(tr("Set device passwords on install"), this);
    auto *pwForm = new QFormLayout(pwGroup);
    r = addRow(pwForm, tr("Update Send Config Password:"), pwGroup, true);
    m_setSendCheck = r.check;
    m_setSend = r.edit;
    r = addRow(pwForm, tr("Update Get Config Password:"), pwGroup, true);
    m_setGetCheck = r.check;
    m_setGet = r.edit;
    for (int i = 0; i < 4; ++i) {
        r = addRow(pwForm, tr("Update Protected Comms Slot %1 Password:").arg(i + 1), pwGroup,
                   true);
        m_setSlotCheck[i] = r.check;
        m_setSlot[i] = r.edit;
    }
    auto *pwNote = new QLabel(
        tr("A ticked box with an empty field REMOVES that password. Unticked leaves it "
           "unchanged. The device accepts these because the package proves the Firmware Key."),
        pwGroup);
    pwNote->setWordWrap(true);
    pwForm->addRow(pwNote);
    layout->addWidget(pwGroup);

    m_warning = new QLabel(this);
    m_warning->setWordWrap(true);
    layout->addWidget(m_warning);

    m_buttons = new QDialogButtonBox(this);
    m_buttons->addButton(tr("Build Package…"), QDialogButtonBox::AcceptRole);
    m_buttons->addButton(QDialogButtonBox::Close);
    layout->addWidget(m_buttons);
    connect(m_buttons, &QDialogButtonBox::accepted, this, &SecureBuilderDialog::build);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    for (QLineEdit *e : {m_source, m_matchManufacturer, m_matchModel, m_matchVersion, m_key,
                         m_setSend, m_setGet, m_setSlot[0], m_setSlot[1], m_setSlot[2],
                         m_setSlot[3]})
        connect(e, &QLineEdit::textChanged, this, &SecureBuilderDialog::refreshEnabled);
    for (QCheckBox *c : {m_matchManufacturerCheck, m_matchModelCheck, m_matchVersionCheck,
                         m_setSendCheck, m_setGetCheck, m_setSlotCheck[0], m_setSlotCheck[1],
                         m_setSlotCheck[2], m_setSlotCheck[3]})
        connect(c, &QCheckBox::toggled, this, &SecureBuilderDialog::refreshEnabled);

    refreshEnabled();
}

void SecureBuilderDialog::browseSource()
{
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Choose a configuration"), QFileInfo(m_source->text()).absolutePath(),
        tr("CAN Triple Configurations (*.ct3);;All Files (*)"));
    if (!path.isEmpty())
        m_source->setText(path);
}

bool SecureBuilderDialog::collectPolicy(SecurePackagePolicy *out, QString *why) const
{
    SecurePackagePolicy p;
    if (m_matchManufacturerCheck->isChecked())
        p.matchManufacturer = m_matchManufacturer->text();
    if (m_matchModelCheck->isChecked())
        p.matchModel = m_matchModel->text();
    if (m_matchVersionCheck->isChecked())
        p.matchVersion = m_matchVersion->text();

    if (m_key->text().isEmpty()) {
        if (why)
            *why = tr("A Firmware Key is required: every package names one and every device "
                      "must prove it.");
        return false;
    }
    p.key = deriveLicenseKey(m_key->text());
    p.configVersion = quint16(m_packageVersion->value());
    if (p.key.size() != kLicenseKeyBytes) {
        if (why)
            *why = tr("The Firmware Key could not be derived.");
        return false;
    }

    // A ticked box always records an instruction, empty field included — that is
    // what makes "remove this password" expressible at all. The phrase is derived
    // HERE and goes no further: the package carries the 4-byte key, never the
    // text. An empty phrase derives to kNoAccessKey, which is the clear sentinel.
    p.setSend = m_setSendCheck->isChecked();
    p.sendKey = deriveAccessKey(m_setSend->text());
    p.setGet = m_setGetCheck->isChecked();
    p.getKey = deriveAccessKey(m_setGet->text());
    for (int i = 0; i < 4; ++i) {
        p.setCommsSlot[i] = m_setSlotCheck[i]->isChecked();
        p.commsSlotKey[i] = deriveAccessKey(m_setSlot[i]->text());
    }

    if (out)
        *out = p;
    return true;
}

void SecureBuilderDialog::refreshEnabled()
{
    QStringList problems;
    if (m_source->text().trimmed().isEmpty())
        problems << tr("Choose a configuration to package.");
    if (m_key->text().isEmpty())
        problems << tr("A Firmware Key is required.");

    // A ticked match with nothing typed would compare against an empty string,
    // which no licence field can equal — a package nothing could ever install.
    // Caught here rather than at install, where it would look like a device
    // fault.
    const auto emptyTicked = [&](QCheckBox *c, QLineEdit *e, const QString &name) {
        if (c->isChecked() && e->text().isEmpty())
            problems << tr("%1 is ticked but empty, so no device could match it.").arg(name);
    };
    emptyTicked(m_matchManufacturerCheck, m_matchManufacturer, tr("Match FW Manufacturer"));
    emptyTicked(m_matchModelCheck, m_matchModel, tr("Match FW Model"));
    emptyTicked(m_matchVersionCheck, m_matchVersion, tr("Match FW Version"));

    // The same password policy the other dialogs apply, and worth more here: a
    // package sets the same password on every unit it installs on, so a weak
    // one is weak fleet-wide. Empty is exempt — that is a clear, not a password.
    const auto weak = [&](QCheckBox *c, QLineEdit *e, const QString &name) {
        if (!c->isChecked() || e->text().isEmpty())
            return;
        const QString why = passwordProblem(e->text());
        if (!why.isEmpty())
            problems << tr("%1: %2").arg(name, why);
    };
    weak(m_setSendCheck, m_setSend, tr("Send Config Password"));
    weak(m_setGetCheck, m_setGet, tr("Get Config Password"));
    for (int i = 0; i < 4; ++i)
        weak(m_setSlotCheck[i], m_setSlot[i], tr("Protected Comms Slot %1 Password").arg(i + 1));

    m_warning->setText(problems.join(QStringLiteral("\n")));
    m_warning->setVisible(!problems.isEmpty());
    for (QAbstractButton *b : m_buttons->buttons()) {
        if (m_buttons->buttonRole(b) == QDialogButtonBox::AcceptRole)
            b->setEnabled(problems.isEmpty());
    }
}

void SecureBuilderDialog::build()
{
    SecurePackagePolicy policy;
    QString why;
    if (!collectPolicy(&policy, &why)) {
        QMessageBox::warning(this, windowTitle(), why);
        return;
    }

    // Loaded from disk every time. The alternative — packaging whatever is in
    // the editor — would let an unsaved edit reach a customer's device without
    // ever existing in a file anybody could go back to.
    Configuration source;
    QString error;
    if (!source.loadFromFile(m_source->text(), &error)) {
        QMessageBox::warning(this, windowTitle(),
                             error.isEmpty() ? tr("That configuration could not be opened.")
                                             : error);
        return;
    }

    QString path = QFileDialog::getSaveFileName(
        this, tr("Save Secure Package"), QFileInfo(m_source->text()).absolutePath(),
        tr("CAN Triple Secure Configurations (*.ct3s);;All Files (*)"));
    if (path.isEmpty())
        return;
    if (QFileInfo(path).suffix().isEmpty())
        path += QStringLiteral(".ct3s");

    SecureSaveOptions options = source.secureOptions();
    options.policy = policy;
    // The key the file carries so a customer's copy can satisfy a device's
    // protected-comms gate without them ever typing the password.
    options.embeddedCommsKey = source.commsKey();

    if (!source.saveSecureToFile(path, options, &error)) {
        QMessageBox::warning(this, windowTitle(),
                             error.isEmpty() ? tr("The package could not be written.") : error);
        return;
    }

    QStringList summary;
    summary << tr("Package written to %1.").arg(QFileInfo(path).fileName());
    QStringList matched;
    if (!policy.matchManufacturer.isEmpty())
        matched << tr("manufacturer");
    if (!policy.matchModel.isEmpty())
        matched << tr("model");
    if (!policy.matchVersion.isEmpty())
        matched << tr("version");
    matched << tr("Firmware Key");
    summary << tr("It installs only on devices matching: %1.").arg(matched.join(QStringLiteral(", ")));
    if (policy.changesPasswords())
        summary << tr("It also sets device passwords as it installs.");
    if (policy.configVersion != 0)
        summary << tr("It stamps configuration version %1 on the unit.").arg(policy.configVersion);
    QMessageBox::information(this, windowTitle(), summary.join(QStringLiteral("\n\n")));
}

} // namespace ct
