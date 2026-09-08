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
#include <QRegularExpressionValidator>
#include <QSpinBox>
#include <QVBoxLayout>

#include "../model/access_keys.h"
#include "../protocol/wire_structs.h"
#include "name_limits.h"
#include "hex_input.h"

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

SecureBuilderDialog::SecureBuilderDialog(const QString &openDocumentPath, BuildFn builder,
                                         QWidget *parent)
    : QDialog(parent)
    , m_builder(std::move(builder))
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

    // The two hardware matches: which UNIT, where the three above say which
    // fleet. Both are copied off the device — the MCU ID from Device Status's
    // Copy button, the serial from Get Device Info — so the validators keep
    // out only what could never match: the ID is hex digits or nothing, the
    // serial a number (decimal, or hex with 0x). refreshEnabled() checks the
    // shape a validator cannot while the text is still being typed.
    r = addRow(matchForm, tr("Match MCU ID:"), matchGroup, false);
    m_matchMcuIdCheck = r.check;
    m_matchMcuId = r.edit;
    m_matchMcuId->setValidator(new QRegularExpressionValidator(
        QRegularExpression(QStringLiteral("[0-9A-Fa-f]{0,24}")), this));
    m_matchMcuId->setPlaceholderText(tr("24 hex digits, as Device Status shows it"));
    r = addRow(matchForm, tr("Match HW Serial:"), matchGroup, false);
    m_matchSerialCheck = r.check;
    m_matchSerial = r.edit;
    m_matchSerial->setValidator(new QRegularExpressionValidator(
        QRegularExpression(QStringLiteral("[0-9]{0,20}|0[xX][0-9A-Fa-f]{0,16}")), this));
    m_matchSerial->setPlaceholderText(tr("as Get Device Info shows it, or 0x hex"));

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

    // ---- what the customer's copy can do
    // Install-only is the default: the .ct3 this is built from is the editable
    // master, and a copy inside the package is one more place the
    // configuration exists. Either way the install stream is sealed under the
    // Firmware Key and decrypted by the device — the password guards only the
    // editable copy.
    auto *contentsGroup = new QGroupBox(tr("Package contents"), this);
    auto *contentsForm = new QFormLayout(contentsGroup);
    m_includeEditable = new QCheckBox(
        tr("Include an editable copy, opened only with this package password"), contentsGroup);
    contentsForm->addRow(m_includeEditable);
    m_openPassword = new QLineEdit(contentsGroup);
    m_openPassword->setEchoMode(QLineEdit::Password);
    m_openPassword->setEnabled(false);
    contentsForm->addRow(tr("Package password:"), m_openPassword);
    m_openPasswordConfirm = new QLineEdit(contentsGroup);
    m_openPasswordConfirm->setEchoMode(QLineEdit::Password);
    m_openPasswordConfirm->setEnabled(false);
    contentsForm->addRow(tr("Confirm password:"), m_openPasswordConfirm);
    connect(m_includeEditable, &QCheckBox::toggled, m_openPassword, &QLineEdit::setEnabled);
    connect(m_includeEditable, &QCheckBox::toggled, m_openPasswordConfirm,
            &QLineEdit::setEnabled);
    auto *contentsNote = new QLabel(
        tr("The configuration itself is sealed under the Firmware Key and is decrypted by the "
           "device, never by the Manager. Unticked, the package cannot be opened in the Manager "
           "at all; ticked, it opens with this password and nothing else."),
        contentsGroup);
    contentsNote->setWordWrap(true);
    contentsForm->addRow(contentsNote);
    layout->addWidget(contentsGroup);

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

    for (QLineEdit *e : {m_source, m_matchManufacturer, m_matchModel, m_matchVersion,
                         m_matchMcuId, m_matchSerial, m_key, m_openPassword,
                         m_openPasswordConfirm, m_setSend, m_setGet, m_setSlot[0], m_setSlot[1],
                         m_setSlot[2], m_setSlot[3]})
        connect(e, &QLineEdit::textChanged, this, &SecureBuilderDialog::refreshEnabled);
    for (QCheckBox *c : {m_matchManufacturerCheck, m_matchModelCheck, m_matchVersionCheck,
                         m_matchMcuIdCheck, m_matchSerialCheck, m_includeEditable,
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
    if (m_matchMcuIdCheck->isChecked())
        p.matchMcuId = m_matchMcuId->text().trimmed().toUpper();
    if (m_matchSerialCheck->isChecked()) {
        bool ok = false;
        p.matchSerial = parseUnsignedText(m_matchSerial->text(), &ok);
        p.matchSerialSet = ok;
        if (!ok) {
            if (why)
                *why = tr("Match HW Serial is not a number.");
            return false;
        }
    }

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
    emptyTicked(m_matchMcuIdCheck, m_matchMcuId, tr("Match MCU ID"));
    emptyTicked(m_matchSerialCheck, m_matchSerial, tr("Match HW Serial"));
    // Shape checks the validators cannot make while the text is still being
    // typed: a short ID and an unparsable serial are both packages nothing
    // could ever install.
    if (m_matchMcuIdCheck->isChecked() && !m_matchMcuId->text().isEmpty()
        && m_matchMcuId->text().trimmed().size() != 24)
        problems << tr("Match MCU ID must be the 24 hex digits Device Status shows.");
    if (m_matchSerialCheck->isChecked() && !m_matchSerial->text().isEmpty()) {
        bool ok = false;
        parseUnsignedText(m_matchSerial->text(), &ok);
        if (!ok)
            problems << tr("Match HW Serial must be a number (decimal, or hex with 0x).");
    }

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

    // An editable copy is only as protected as its password, so it takes the
    // same policy as a device password, plus a confirmation: a typo here is a
    // package nobody can ever open.
    if (m_includeEditable->isChecked()) {
        if (m_openPassword->text().isEmpty()) {
            problems << tr("An editable copy needs a package password.");
        } else {
            const QString why = passwordProblem(m_openPassword->text());
            if (!why.isEmpty())
                problems << tr("Package password: %1").arg(why);
            else if (m_openPassword->text() != m_openPasswordConfirm->text())
                problems << tr("The package password and its confirmation differ.");
        }
    }

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
    if (!m_builder) {
        QMessageBox::warning(this, windowTitle(), tr("The package builder is not available."));
        return;
    }

    QString path = QFileDialog::getSaveFileName(
        this, tr("Save Secure Package"), QFileInfo(m_source->text()).absolutePath(),
        tr("CAN Triple Secure Configurations (*.ct3s);;All Files (*)"));
    if (path.isEmpty())
        return;
    if (QFileInfo(path).suffix().isEmpty())
        path += QStringLiteral(".ct3s");

    // The passphrase goes to the builder and no further: it derives the fleet
    // key there, seals the stream with it, and neither is written anywhere.
    PackageBuildRequest request;
    request.sourcePath = m_source->text();
    request.outputPath = path;
    request.fleetPassphrase = m_key->text();
    request.policy = policy;
    request.includeEditable = m_includeEditable->isChecked();
    request.openPassword = request.includeEditable ? m_openPassword->text() : QString();
    const PackageBuildResult result = m_builder(request);
    if (!result.ok) {
        QMessageBox::warning(this, windowTitle(),
                             result.error.isEmpty() ? tr("The package could not be written.")
                                                    : result.error);
        return;
    }
    QMessageBox::information(this, windowTitle(), result.summary.join(QStringLiteral("\n\n")));
}

} // namespace ct
