// Online > Fleet Identity… — see fleet_identity_dialog.h for why the two panels
// are not symmetrical.
//
// Three implementation notes, because each one is a decision somebody will
// otherwise "fix":
//
//   * The device is read ONCE, when the dialog opens, and never polled. A unit's
//     identity is fixed at build time, so there is nothing to poll for; the only
//     field that can move is Config Version, and it moves when a configuration
//     is saved to flash rather than while this dialog is on screen.
//
//   * Nothing here writes to the device. There is no CMD to write an identity
//     and there is deliberately no device_session::writeFleetIdentity() to call.
//     Every button on this dialog acts on the open document.
//
//   * The fleet-key passphrase is folded with deriveAccessKey(), which is 210,000
//     rounds of PBKDF2 and takes a visible fraction of a second. It runs on a
//     button press and never on a keystroke, which is why the live status line
//     works from publicFields() — the four public fields, no key — rather than
//     from identityFromFields().
#include "fleet_identity_dialog.h"

#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QSet>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStringList>
#include <QVBoxLayout>

namespace ct {

namespace {

// Blocks the widget while a device round trip runs its nested event loop. The
// same shape as MainWindow's and AccessPasswordsDialog's; duplicated for the
// third time rather than shared because one small RAII class is still cheaper
// than a header for it, and `widget` may be null here — copyFromDevice() is a
// static that a caller can hand no parent.
class BusyScope
{
public:
    explicit BusyScope(QWidget *widget)
        : m_widget(widget)
    {
        if (m_widget)
            m_widget->setEnabled(false);
        QApplication::setOverrideCursor(Qt::WaitCursor);
    }
    ~BusyScope()
    {
        QApplication::restoreOverrideCursor();
        if (m_widget)
            m_widget->setEnabled(true);
    }

private:
    QWidget *m_widget;
};

// Fixed eight digits. These numbers are compared by eye far more often than they
// are typed — against a device readout, against a product sheet, against another
// engineer's screen — and a ragged column defeats that.
QString hexFieldText(quint32 value)
{
    return QStringLiteral("0x")
           + QStringLiteral("%1").arg(value, 8, 16, QLatin1Char('0')).toUpper();
}

// Both bases at once, for anywhere a number is only ever read. Serial numbers in
// particular arrive in whichever base the people handling them happened to use —
// a build flag writes 0x00000123, a despatch note writes 291 — and showing one
// of the two invites someone to conclude they are looking at different units.
QString hexAndDecimal(quint32 value)
{
    return QObject::tr("%1  (= %2 decimal)").arg(hexFieldText(value)).arg(value);
}

// Quoted, so a trailing space is visible. Vendor and model are compared byte for
// byte, which makes "ACME " and "ACME" two different fleets, and that difference
// is invisible in an unquoted readout.
QString quotedOrUnset(const QString &text)
{
    return text.isEmpty() ? QObject::tr("(not set)") : QStringLiteral("\"%1\"").arg(text);
}

// The allow-list as typed. Lines that are not numbers are COUNTED rather than
// dropped in silence: an unreadable line makes the list shorter, a shorter list
// is a more permissive policy, and a policy that quietly widened itself is
// exactly the failure this dialog exists to prevent.
//
// A leading "0x" means hex and anything else means decimal. There is no way to
// guess for a bare "100", so the rule is stated beside the field rather than
// inferred here — inferring it would make 0x100 and 100 the same device on some
// lines and different devices on others.
struct SerialList
{
    QList<quint32> serials; // de-duplicated, in the order first seen
    int badLines = 0;
};

SerialList parseSerialList(const QString &text)
{
    SerialList out;
    QSet<quint32> seen;
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty())
            continue; // blank lines and a trailing newline are not mistakes
        bool ok = false;
        const quint32 serial =
            trimmed.startsWith(QLatin1String("0x"), Qt::CaseInsensitive)
                ? trimmed.mid(2).toUInt(&ok, 16)
                : trimmed.toUInt(&ok, 10);
        if (!ok) {
            ++out.badLines;
            continue;
        }
        // A serial named twice is still one device. Deduplicating keeps the
        // count honest without rewriting what the user typed.
        if (!seen.contains(serial)) {
            seen.insert(serial);
            out.serials.append(serial);
        }
    }
    return out;
}

// Canonical form for a list this app wrote itself: hex, so it round-trips
// through parseSerialList() meaning the same thing.
QString serialListText(const QList<quint32> &serials)
{
    QStringList lines;
    lines.reserve(serials.size());
    for (quint32 serial : serials)
        lines << hexFieldText(serial);
    return lines.join(QLatin1Char('\n'));
}

// The button box owns its buttons and the header keeps no pointer to any of
// them, so they are found by role when their enabled state has to change. One
// button per role here, which is what makes this unambiguous.
QAbstractButton *buttonWithRole(QDialogButtonBox *box, QDialogButtonBox::ButtonRole role)
{
    const QList<QAbstractButton *> buttons = box->buttons();
    for (QAbstractButton *button : buttons) {
        if (box->buttonRole(button) == role)
            return button;
    }
    return nullptr;
}

// Asked after a copy, never rolled into it. Copying the vendor and model
// off the unit in front of you is a convenience and cannot narrow anything;
// adding the serial narrows the package to that one unit and locks out the rest
// of the fleet. That is a decision, so it gets a question, and the question
// defaults to No — the safe answer is the one that installs on more devices,
// because the wrong pinning shows up as a customer who cannot take an update.
//
// Shared by the dialog's button and by the static, so an operator sees the same
// words whichever route they came in by.
bool askToPinSerial(QWidget *parent, const QString &title, quint32 serial)
{
    return QMessageBox::question(
               parent, title,
               QObject::tr("Restrict this configuration to serial number %1 as well?\n\n"
                           "The vendor and model have been copied. Adding the serial "
                           "pins the package to this one unit: no other device in the fleet "
                           "will accept it. That is right for a replacement cut for a single "
                           "vehicle and wrong for a release, so it is asked rather than "
                           "assumed.")
                   .arg(hexAndDecimal(serial)),
               QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
           == QMessageBox::Yes;
}

// The four public fields straight off the widgets. Split out from
// identityFromFields() because that one folds the passphrase into the fleet
// key, and kAccessKeyIterations rounds of PBKDF2 is far too slow to run on every
// keystroke — which is exactly what the live status line would do.
FleetIdentity publicFields(QLineEdit *vendor, QLineEdit *model, QSpinBox *version)
{
    // Empty strings are exactly how an identity says "no
    // fleet", and it is a state the user is entitled to type their way into, so
    // nothing here is an error. The two strings are clamped again even though
    // the editors clamp live: it costs nothing and it means this function is
    // correct on its own rather than correct because of a signal connection
    // somewhere else.
    FleetIdentity id;
    id.vendorId = FleetIdentity::clampToWire(vendor->text(), kFleetVendorIdBytes);
    id.modelId = FleetIdentity::clampToWire(model->text(), kFleetModelIdBytes);
    id.configVersion = quint16(version->value());
    return id;
}

} // namespace

FleetIdentityDialog::FleetIdentityDialog(DeviceLink *link, Configuration *config, QWidget *parent)
    : QDialog(parent)
    , m_link(link)
    , m_config(config)
    , m_vendorEdit(nullptr)
    , m_modelEdit(nullptr)
    , m_versionSpin(nullptr)
    , m_fleetKeyEdit(nullptr)
    , m_serialsEdit(nullptr)
    , m_requireKeyCheck(nullptr)
    , m_requireNewerCheck(nullptr)
    , m_deviceLabel(nullptr)
    , m_statusLabel(nullptr)
    , m_buttons(nullptr)
{
    setWindowTitle(tr("Fleet Identity"));
    setModal(true);

    auto *layout = new QVBoxLayout(this);

    // -------------------------------------------------------------- this device
    // First, and read-only. What the unit in front of you actually is, which is
    // the fact everything in the panel below is measured against.
    auto *deviceGroup = new QGroupBox(tr("This Device"));
    auto *deviceLayout = new QVBoxLayout(deviceGroup);
    m_deviceLabel = new QLabel;
    m_deviceLabel->setWordWrap(true);
    m_deviceLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    deviceLayout->addWidget(m_deviceLabel);

    // Always shown, offline included. A panel of greyed-out facts with no
    // explanation reads as a feature somebody forgot to finish, and the first
    // thing a user does about a missing feature is go looking for the button
    // that writes an identity to a device. There is no such button and there is
    // no such command; saying so here is cheaper than the support call.
    auto *compiledNote = new QLabel(
        tr("A device's identity is compiled into its firmware, so nothing here — and no command "
           "on the wire — can change it. Re-badging a unit means setting its values in "
           "firmware/identity.local.ini and flashing it. That is what makes the identity worth "
           "believing: a flash erase cannot lose it and a packet cannot forge it. The one "
           "exception is Config Version, which the "
           "device records when a configuration is saved to its flash."));
    compiledNote->setWordWrap(true);
    deviceLayout->addWidget(compiledNote);
    layout->addWidget(deviceGroup);

    // ------------------------------------------------------ this configuration
    auto *fileGroup = new QGroupBox(tr("This Configuration"));
    auto *fileGrid = new QGridLayout(fileGroup);
    fileGrid->setColumnStretch(1, 1);
    int row = 0;

    const FleetIdentity current = m_config ? m_config->fleetIdentity() : FleetIdentity{};

    // Vendor and model cross the wire as fixed 16-BYTE NUL-padded fields, so the
    // limit is bytes and not characters. The counter beside each field is there
    // because the difference is invisible until it bites: "Motörsport Elek" is
    // 15 characters and 16 bytes, and a user who types a sixteenth character
    // would otherwise watch nothing happen with no idea why.
    auto addTextRow = [&](const QString &caption, int maxBytes, const QString &initial,
                          const QString &tip) -> QLineEdit * {
        fileGrid->addWidget(new QLabel(caption), row, 0);
        auto *edit = new QLineEdit;
        edit->setToolTip(tip);
        fileGrid->addWidget(edit, row, 1);
        auto *counter = new QLabel;
        counter->setToolTip(tr("These fields are 16 bytes on the wire, not 16 characters. A "
                               "character outside plain ASCII takes two bytes or more."));
        fileGrid->addWidget(counter, row, 2);

        auto sync = [edit, counter, maxBytes]() {
            // Clamp on the way in rather than validating on the way out.
            // QLineEdit::setMaxLength() counts QChars and would let a 16-byte
            // field take 16 accented characters, so the truncation has to happen
            // here, on a UTF-8 boundary, via clampToWire().
            const QString clamped = FleetIdentity::clampToWire(edit->text(), maxBytes);
            if (clamped != edit->text()) {
                // Blocked so this does not re-enter through textChanged. The
                // cursor is put back where it was: only the tail is ever
                // removed, so the position the user was typing at is still
                // valid unless it was itself past the new end.
                const int pos = edit->cursorPosition();
                const QSignalBlocker blocker(edit);
                edit->setText(clamped);
                edit->setCursorPosition(qMin(pos, int(clamped.size())));
            }
            counter->setText(tr("%1/%2 bytes").arg(clamped.toUtf8().size()).arg(maxBytes));
        };
        connect(edit, &QLineEdit::textChanged, this, sync);
        edit->setText(initial);
        sync(); // setText() on an already-empty field emits nothing, so fill the
                // counter by hand for the untitled-document case
        ++row;
        return edit;
    };

    m_vendorEdit = addTextRow(tr("Vendor ID :"), kFleetVendorIdBytes, current.vendorId,
                              tr("Who built the device this configuration is for. Compared "
                                 "exactly against what the device reports — case, spacing and "
                                 "punctuation all count."));
    m_modelEdit = addTextRow(tr("Model ID :"), kFleetModelIdBytes, current.modelId,
                             tr("Which product line. Compared exactly, like the vendor."));


    fileGrid->addWidget(new QLabel(tr("Config Version :")), row, 0);
    m_versionSpin = new QSpinBox;
    m_versionSpin->setRange(0, 65535);
    m_versionSpin->setValue(current.configVersion);
    m_versionSpin->setToolTip(tr("Which revision of this configuration it is. The device "
                                 "refuses an update that is not numbered higher than the one it "
                                 "already runs, so this has to go up every time a configuration "
                                 "is released. It is the one part of a device's identity that is "
                                 "not compiled in: the device records it when a configuration is "
                                 "saved to its flash."));
    fileGrid->addWidget(m_versionSpin, row, 1);
    ++row;

    fileGrid->addWidget(new QLabel(tr("Fleet Key :")), row, 0);
    m_fleetKeyEdit = new QLineEdit;
    m_fleetKeyEdit->setEchoMode(QLineEdit::Password);
    // A passphrase, never four raw bytes. The four bytes are what the hardware
    // compares, but nobody should be asked to carry them around: the same
    // passphrase folds to the same key on every machine, which is what lets a
    // fleet be built and packaged from more than one bench.
    m_fleetKeyEdit->setPlaceholderText(current.fleetKey != kNoAccessKey
                                            ? tr("(leave blank to keep the current key)")
                                            : tr("(no fleet key)"));
    m_fleetKeyEdit->setToolTip(tr("A passphrase. It is folded into the four-byte key the "
                                   "firmware was built with; the passphrase used here must be "
                                   "the one CT_FLEET_KEY came from, or the device's answer to "
                                   "the challenge will not match."));
    fileGrid->addWidget(m_fleetKeyEdit, row, 1, 1, 2);
    ++row;

    // The identity.local.ini line, spelled out and ready to paste.
    //
    // Without this the two halves of the feature could not be connected at all.
    // The passphrase is folded into four bytes here; the firmware's identity
    // file needs those same four bytes as a hex literal; and nothing anywhere
    // showed them. The honest answer to "what do I type for CT_FLEET_KEY?" was
    // "there is no way to find out", which made the whole attestation
    // unbuildable in practice.
    //
    // Read-only and selectable rather than editable: this is an output, and a
    // field you could type into would invite someone to change the key here and
    // wonder why the passphrase above no longer matched.
    fileGrid->addWidget(new QLabel(tr("Identity line :")), row, 0);
    m_fleetKeyFlagEdit = new QLineEdit;
    m_fleetKeyFlagEdit->setReadOnly(true);
    m_fleetKeyFlagEdit->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_fleetKeyFlagEdit->setToolTip(tr("Paste this line into firmware/identity.local.ini — the "
                                      "gitignored identity file you create by copying "
                                      "identity.local.ini.example — and reflash the unit. The "
                                      "passphrase above and this line are the two halves of the "
                                      "same key: a device built with a different value cannot "
                                      "answer this configuration's challenge."));
    fileGrid->addWidget(m_fleetKeyFlagEdit, row, 1, 1, 2);
    ++row;
    // Derived on focus-out, never per keystroke: deriveAccessKey() is 210,000
    // rounds of PBKDF2, which is the point of it and also far too slow to run on
    // every character typed.
    connect(m_fleetKeyEdit, &QLineEdit::editingFinished, this,
            &FleetIdentityDialog::refreshFleetKeyFlag);
    refreshFleetKeyFlag();

    // Said in the dialog rather than only in the manual, because the failure it
    // prevents is silent: save a plain .ct3, mail it out, and the attestation
    // that was supposed to protect the fleet simply never runs.
    auto *keyNote = new QLabel(
        tr("Leave the key blank to keep the one this configuration already holds — a passphrase "
           "can never be shown back, so the field cannot be filled in for you. Clearing the "
           "vendor and model is what removes a key.\n\n"
           "The fleet key is the fleet's secret and is stored only in a secure configuration "
           "(File > Save Secure Config…). A plain .ct3 keeps the vendor, model and "
           "version and drops the key. A mistyped passphrase locks nothing: it produces a "
           "different key, which shows up as a device that cannot prove it belongs to the "
           "fleet."));
    keyNote->setWordWrap(true);
    fileGrid->addWidget(keyNote, row, 0, 1, 3);

    layout->addWidget(fileGroup);

    // ------------------------------------------------------------ upload policy
    auto *policyGroup = new QGroupBox(tr("Upload Policy"));
    auto *policyLayout = new QVBoxLayout(policyGroup);
    policyLayout->addWidget(new QLabel(tr("Allowed serial numbers :")));

    m_serialsEdit = new QPlainTextEdit;
    m_serialsEdit->setPlaceholderText(tr("(any serial number in the fleet)"));
    m_serialsEdit->setTabChangesFocus(true); // a Tab here means "next field", not a tab character
    m_serialsEdit->setPlainText(
        serialListText(m_config ? m_config->uploadPolicy().allowedSerials : QList<quint32>{}));
    // Four lines: enough to see a short list without stealing the height the
    // notes below need, and it scrolls for the fleet-wide recall that is fifty
    // lines long.
    m_serialsEdit->setFixedHeight(m_serialsEdit->fontMetrics().lineSpacing() * 4 + 12);
    policyLayout->addWidget(m_serialsEdit);

    auto *serialsNote = new QLabel(
        tr("One serial number per line. A number starting 0x is read as hexadecimal and anything "
           "else as decimal, so 0x100 and 100 are different devices. Leave the list empty to let "
           "any device in the fleet install this configuration."));
    serialsNote->setWordWrap(true);
    policyLayout->addWidget(serialsNote);

    auto *serialsCount = new QLabel;
    serialsCount->setWordWrap(true);
    policyLayout->addWidget(serialsCount);
    auto syncSerialsCount = [this, serialsCount]() {
        const SerialList list = parseSerialList(m_serialsEdit->toPlainText());
        QString text;
        if (list.serials.isEmpty()) {
            text = tr("No serial numbers listed — any device in the fleet may install this.");
        } else {
            text = tr("%n serial number(s) listed.", nullptr, int(list.serials.size()));
        }
        if (list.badLines > 0) {
            // Loud, because an unreadable line does not fail — it disappears,
            // and a list that lost a line is a policy that lets a unit in.
            text += QLatin1Char(' ')
                    + tr("%n line(s) could not be read as a number and will be ignored.", nullptr,
                         list.badLines);
        }
        serialsCount->setText(text);
    };
    connect(m_serialsEdit, &QPlainTextEdit::textChanged, this, syncSerialsCount);
    syncSerialsCount(); // setPlainText() above ran before the connection existed

    m_requireKeyCheck = new QCheckBox(tr("Require the device to prove the fleet key"));
    m_requireKeyCheck->setChecked(m_config ? m_config->uploadPolicy().requireFleetKey : true);
    m_requireKeyCheck->setToolTip(tr("Without proof, the fleet block is four values a look-alike "
                                     "can echo back. Turn this off only for a fleet whose "
                                     "firmware was built without a fleet key."));
    policyLayout->addWidget(m_requireKeyCheck);

    m_requireNewerCheck = new QCheckBox(tr("Warn if the device already runs a newer version"));
    m_requireNewerCheck->setChecked(m_config ? m_config->uploadPolicy().warnOnOlderVersion : true);
    // Says "warn" because it warns. It refused, once, and a checkbox that reads
    // "require" while merely remarking is the kind of small lie that costs
    // somebody an afternoon.
    m_requireNewerCheck->setToolTip(
        tr("Installing an older configuration is allowed either way — this only decides whether "
           "the uploader remarks on it. Reinstalling the SAME version is never remarked on."));
    policyLayout->addWidget(m_requireNewerCheck);
    layout->addWidget(policyGroup);

    m_statusLabel = new QLabel;
    m_statusLabel->setWordWrap(true);
    layout->addWidget(m_statusLabel);

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    m_buttons->addButton(tr("Copy from Device"), QDialogButtonBox::ActionRole);
    m_buttons->addButton(tr("Apply to Configuration"), QDialogButtonBox::ApplyRole);
    // Neither action closes the dialog: copying from the unit and applying to
    // the document are usually done one after the other, and a dialog that
    // vanished after the first would make the second a fresh trip through the
    // menu.
    connect(m_buttons, &QDialogButtonBox::clicked, this, [this](QAbstractButton *button) {
        switch (m_buttons->buttonRole(button)) {
        case QDialogButtonBox::ActionRole:
            onCopyFromDevice();
            break;
        case QDialogButtonBox::ApplyRole:
            onApplyToDocument();
            break;
        default:
            reject();
            break;
        }
    });
    // No button answers the Return key. Return belongs to the field being edited
    // — it is how a hex value is committed and reformatted — and a dialog where
    // a stray Return can overwrite the document's fleet identity is a dialog
    // that will eventually overwrite one.
    const QList<QAbstractButton *> boxButtons = m_buttons->buttons();
    for (QAbstractButton *button : boxButtons) {
        if (auto *push = qobject_cast<QPushButton *>(button)) {
            push->setAutoDefault(false);
            push->setDefault(false);
        }
    }
    layout->addWidget(m_buttons);

    for (QLineEdit *edit : {m_vendorEdit, m_modelEdit, m_fleetKeyEdit})
        connect(edit, &QLineEdit::textChanged, this, &FleetIdentityDialog::updateStatus);
    connect(m_versionSpin, &QSpinBox::valueChanged, this, &FleetIdentityDialog::updateStatus);
    connect(m_serialsEdit, &QPlainTextEdit::textChanged, this, &FleetIdentityDialog::updateStatus);
    connect(m_requireKeyCheck, &QCheckBox::toggled, this, &FleetIdentityDialog::updateStatus);
    connect(m_requireNewerCheck, &QCheckBox::toggled, this, &FleetIdentityDialog::updateStatus);

    loadFromDevice();
    updateStatus();
    resize(620, sizeHint().height());
}

// Reads the device's block once, when the dialog opens. Every branch below says
// WHICH of the several "nothing to show" states this is, because they call for
// completely different actions — plug something in, update the firmware, or
// write an identity file and reflash — and a single "unavailable" would leave
// the user guessing which.
void FleetIdentityDialog::loadFromDevice()
{
    m_deviceState = device_session::FleetIdentityState{};

    if (!m_link || !m_link->isOpen()) {
        m_deviceLabel->setText(tr("Not connected. Connect to a CAN Triple to read the identity "
                                  "it was built with."));
        return;
    }

    QString error;
    bool read = false;
    {
        BusyScope busy(this);
        read = device_session::readFleetIdentity(m_link, &m_deviceState, &error);
    }
    if (!read) {
        m_deviceLabel->setText(tr("The device's identity could not be read.\n\n%1").arg(error));
        return;
    }
    if (!m_deviceState.supported) {
        m_deviceLabel->setText(tr("This device's firmware predates fleet identities, so it does "
                                  "not report one. Building and flashing current firmware is "
                                  "what gives it one."));
        return;
    }
    if (!m_deviceState.identity.isSet()) {
        // Emphatically not an error. A unit built without CT_VENDOR_ID and the
        // rest is the normal state of a bench device, and it stays fully usable
        // — it simply cannot be checked against, so the uploader has nothing to
        // refuse on.
        m_deviceLabel->setText(tr("This device was built without a fleet identity. It reports no "
                                  "vendor or model, so an upload cannot be matched "
                                  "against it. Set CT_VENDOR_ID, CT_MODEL_ID, CT_SERIAL_NUMBER "
                                  "and CT_FLEET_KEY in firmware/identity.local.ini (copy "
                                  "identity.local.ini.example to create it) and reflash to give "
                                  "it one."));
        return;
    }

    // Built a line at a time rather than as one format string with six markers.
    // Vendor and model are arbitrary text chosen by whoever set the build flags,
    // so one of them could contain a "%2"; substituting them one at a time into
    // separate literals means such a string is printed rather than treated as a
    // place marker for the field below it.
    const FleetIdentity &id = m_deviceState.identity;
    QStringList lines;
    lines << tr("Vendor ID : %1").arg(quotedOrUnset(id.vendorId));
    lines << tr("Model ID : %1").arg(quotedOrUnset(id.modelId));
    lines << tr("Serial Number : %1").arg(hexAndDecimal(id.serialNumber));
    lines << tr("Config Version : %1").arg(id.configVersion);
    lines << tr("Fleet Key : %1")
                 .arg(m_deviceState.keyPresent
                          ? tr("programmed (never read back — the device proves it by answering "
                               "a challenge)")
                          : tr("none"));
    m_deviceLabel->setText(lines.join(QLatin1Char('\n')));
}

FleetIdentity FleetIdentityDialog::identityFromFields() const
{
    FleetIdentity id = publicFields(m_vendorEdit, m_modelEdit, m_versionSpin);

    if (m_config) {
        const FleetIdentity &current = m_config->fleetIdentity();
        // Neither of these has a control in the panel above, and both are
        // carried through rather than zeroed. flags has no editable bit yet, so
        // zeroing it would let this version of the app quietly discard a field a
        // later one understood. serialNumber is a statement about a piece of
        // hardware and a configuration is not one — the configuration side pins
        // serials through the upload policy instead — so writing a zero over
        // whatever is there would be this dialog asserting something it was
        // never asked about.
        id.flags = current.flags;
        id.serialNumber = current.serialNumber;

        // A blank passphrase means "leave the key as it is", not "there is no
        // key". It has to: a passphrase can never be shown back to the user, so
        // the field cannot be pre-filled, and an empty field that meant "clear"
        // would wipe the fleet secret every time someone opened this dialog to
        // bump a version. Clearing a key is done by clearing the identity — see
        // onApplyToDocument().
        id.fleetKey = current.fleetKey;
    }

    const QString passphrase = m_fleetKeyEdit->text();
    if (!passphrase.isEmpty())
        id.fleetKey = deriveAccessKey(passphrase); // deliberately slow; once per click
    return id;
}

UploadPolicy FleetIdentityDialog::policyFromFields() const
{
    UploadPolicy policy;
    policy.allowedSerials = parseSerialList(m_serialsEdit->toPlainText()).serials;
    policy.requireFleetKey = m_requireKeyCheck->isChecked();
    policy.warnOnOlderVersion = m_requireNewerCheck->isChecked();
    return policy;
}

void FleetIdentityDialog::onCopyFromDevice()
{
    if (!m_link || !m_link->isOpen()) {
        QMessageBox::warning(this, windowTitle(),
                             tr("Connect to a device before copying its identity."));
        return;
    }
    if (!m_deviceState.supported) {
        QMessageBox::warning(this, windowTitle(),
                             tr("This device's firmware does not report a fleet identity."));
        return;
    }
    if (!m_deviceState.identity.isSet()) {
        QMessageBox::information(this, windowTitle(),
                                 tr("This device was built without a fleet identity, so there is "
                                    "nothing to copy."));
        return;
    }

    const FleetIdentity &id = m_deviceState.identity;
    m_vendorEdit->setText(id.vendorId);
    m_modelEdit->setText(id.modelId);
    // Config Version is deliberately not copied. The device's version is what it
    // is ALREADY RUNNING, and a package numbered the same is by definition not
    // newer — copying it would produce a configuration the uploader refuses, and
    // the user would be left wondering which of the two fields lied to them.

    if (id.serialNumber != 0 && askToPinSerial(this, windowTitle(), id.serialNumber)) {
        // Appended to the text the user has, rather than rewritten from the
        // parsed list: rewriting would silently delete any line that failed to
        // parse, which is the one thing this dialog has just promised not to do.
        const SerialList list = parseSerialList(m_serialsEdit->toPlainText());
        if (!list.serials.contains(id.serialNumber)) {
            QString text = m_serialsEdit->toPlainText();
            if (!text.isEmpty() && !text.endsWith(QLatin1Char('\n')))
                text += QLatin1Char('\n');
            text += hexFieldText(id.serialNumber);
            m_serialsEdit->setPlainText(text);
        }
    }

    updateStatus();
}

void FleetIdentityDialog::onApplyToDocument()
{
    if (!m_config)
        return;

    // Asked before anything is written, not reported after. A dropped line makes
    // the allow-list shorter and a shorter list admits more devices, so this is
    // the one mistake in the dialog that fails in the permissive direction.
    const SerialList serials = parseSerialList(m_serialsEdit->toPlainText());
    if (serials.badLines > 0
        && QMessageBox::question(
               this, windowTitle(),
               tr("%n line(s) of the allowed serial numbers could not be read as a number and "
                  "will be left out, which makes the list more permissive rather than less. "
                  "Apply anyway?",
                  nullptr, serials.badLines),
               QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
               != QMessageBox::Yes)
        return;

    // Clearing the vendor and model is how a configuration is unprovisioned,
    // and it takes the fleet key with it: an identity that names
    // no fleet has no fleet to keep a secret for, and a key left behind would be
    // one nobody could see, use or remove.
    //
    // The question is answered from the public fields alone, before any
    // passphrase is folded. identityFromFields() runs 210,000 rounds of PBKDF2,
    // and doing that to produce a key we are about to throw away would be a
    // second of dead dialog in exchange for nothing.
    const bool cleared =
        !publicFields(m_vendorEdit, m_modelEdit, m_versionSpin).isSet();
    FleetIdentity id;
    {
        // That derivation is felt when it does run, so the dialog is blocked for
        // it rather than left looking as though it ignored the click.
        BusyScope busy(this);
        if (!cleared)
            id = identityFromFields();
    }
    m_config->setFleetIdentity(id);
    // The policy is applied either way. It is the user's typing and throwing it
    // away because the identity above happens to be blank would lose work for no
    // reason — it simply has nothing to check until a fleet is named again.
    const UploadPolicy policy = policyFromFields();
    m_config->setUploadPolicy(policy);

    QStringList lines;
    if (cleared) {
        lines << tr("This configuration no longer names a fleet, and any fleet key it held has "
                    "gone with it. The upload policy is kept, but there is nothing for it to "
                    "check until a vendor or model is set again.");
    } else {
        lines << tr("This configuration is now for the fleet named above.");
        if (policy.pinsSerial()) {
            lines << tr("It will install only on the %n serial number(s) listed.", nullptr,
                        int(policy.allowedSerials.size()));
        }
        if (id.fleetKey != kNoAccessKey) {
            // Named explicitly. "Save" on a document that came from a plain .ct3
            // writes a plain .ct3, which drops the key without complaint, and
            // the loss only shows up much later as an upload that cannot
            // challenge the device.
            lines << tr("It carries a fleet key, so save it with File > Save Secure Config… — "
                        "that is the only save that keeps the key. A plain .ct3 will drop it.");
        }
    }
    lines << tr("Save the configuration for this to take effect on the file.");
    QMessageBox::information(this, windowTitle(), lines.join(QStringLiteral("\n\n")));

    updateStatus();
}

void FleetIdentityDialog::updateStatus()
{
    const FleetIdentity fields =
        publicFields(m_vendorEdit, m_modelEdit, m_versionSpin);
    const UploadPolicy policy = policyFromFields();
    const bool online = m_link && m_link->isOpen() && m_deviceState.supported;

    if (QAbstractButton *apply = buttonWithRole(m_buttons, QDialogButtonBox::ApplyRole))
        apply->setEnabled(m_config != nullptr);
    if (QAbstractButton *copy = buttonWithRole(m_buttons, QDialogButtonBox::ActionRole))
        copy->setEnabled(online && m_deviceState.identity.isSet());

    // Whether a key will be in play, answered without folding the passphrase:
    // this runs on every keystroke and PBKDF2 does not belong here.
    const bool haveKey = !m_fleetKeyEdit->text().isEmpty()
                         || (m_config && m_config->fleetIdentity().fleetKey != kNoAccessKey);
    const FleetIdentity &device = m_deviceState.identity;

    // The same order the uploader applies its rules in, so the line the user
    // reads here is the line they will read there. Reporting the version problem
    // on a device that is the wrong model would only send them to fix the
    // harmless half of it.
    QString text;
    if (!online) {
        text = tr("Connect to a CAN Triple that reports a fleet identity to check this "
                  "configuration against it.");
    } else if (!device.isSet()) {
        text = tr("This device was built without a fleet identity, so there is nothing to check "
                  "against. Only a firmware build can give it one.");
    } else if (!fields.isSet()) {
        text = tr("This configuration names no fleet, so the uploader has nothing to match "
                  "against the device.");
    } else if (!fields.sameFleetAs(device)) {
        text = tr("This is a different vendor or model from the one the device reports, "
                  "so the uploader would refuse to install it here.");
    } else if (!policy.allowsSerial(device.serialNumber)) {
        text = tr("This device's serial number %1 is not in the allowed list, so the uploader "
                  "would refuse to install this here.")
                   .arg(hexAndDecimal(device.serialNumber));
    } else if (policy.warnOnOlderVersion && fields.configVersion != 0
               && fields.configVersion < device.configVersion) {
        // Strictly older, and only then. Equal is the ordinary case — the same
        // revision going back onto a unit — and saying anything about it would
        // make the common path look like a problem.
        text = tr("The device already runs version %1, which is newer than this package. It "
                  "will still install; the uploader will just say so.")
                   .arg(device.configVersion);
    } else if (policy.requireFleetKey && !m_deviceState.keyPresent) {
        text = tr("Everything else matches, but this configuration requires the device to prove "
                  "the fleet key and this device holds none — its firmware was built without "
                  "one. Reflash it with a fleet key, or take the requirement off.");
    } else if (policy.requireFleetKey && !haveKey) {
        text = tr("Everything else matches, but this configuration requires proof of the fleet "
                  "key and holds no key to challenge the device with. Enter the fleet "
                  "passphrase, or open the secure configuration that carries it.");
    } else {
        text = tr("This device is part of the fleet this configuration is for, and the uploader "
                  "would install it.");
    }
    m_statusLabel->setText(text);
}

// The same copy the dialog's button performs, for a caller that has no dialog —
// the menu action that wants to stamp the unit on the bench onto the open
// document in one click.
//
// It writes to the Configuration directly, and it writes only vendor, model and
// fleet: the version stays where the user put it (the device's is the one it is
// already running, so copying it would make the package not newer), and the
// serial is offered as a separate question that defaults to No, because pinning
// a package to one unit locks out every other unit in the fleet.
bool FleetIdentityDialog::copyFromDevice(DeviceLink *link, Configuration *config, QWidget *parent)
{
    const QString title = tr("Fleet Identity");
    if (!config)
        return false;

    if (!link || !link->isOpen()) {
        QMessageBox::warning(parent, title,
                             tr("Connect to a device before copying its identity."));
        return false;
    }

    device_session::FleetIdentityState state;
    QString error;
    bool read = false;
    {
        // The scope closes before anything is reported: a message box put up
        // while the parent is still disabled and the wait cursor is still on
        // reads as a frozen application.
        BusyScope busy(parent);
        read = device_session::readFleetIdentity(link, &state, &error);
    }
    if (!read) {
        QMessageBox::warning(parent, title,
                             tr("The device's identity could not be read.\n\n%1").arg(error));
        return false;
    }
    if (!state.supported) {
        QMessageBox::warning(parent, title,
                             tr("This device's firmware predates fleet identities, so it does "
                                "not report one."));
        return false;
    }
    if (!state.identity.isSet()) {
        QMessageBox::information(parent, title,
                                 tr("This device was built without a fleet identity, so there is "
                                    "nothing to copy. Set CT_VENDOR_ID, CT_MODEL_ID, "
                                    "CT_SERIAL_NUMBER and CT_FLEET_KEY in "
                                    "firmware/identity.local.ini (copy identity.local.ini.example "
                                    "to create it) and reflash to give it one."));
        return false;
    }

    // Read-modify-write rather than assignment: the document's own version,
    // flags and fleet key have nothing to do with what the device reports, and
    // an assignment would take all three out along with the fields being copied.
    FleetIdentity id = config->fleetIdentity();
    id.vendorId = state.identity.vendorId;
    id.modelId = state.identity.modelId;
    config->setFleetIdentity(id);

    if (state.identity.serialNumber != 0
        && askToPinSerial(parent, title, state.identity.serialNumber)) {
        UploadPolicy policy = config->uploadPolicy();
        if (!policy.allowedSerials.contains(state.identity.serialNumber)) {
            policy.allowedSerials.append(state.identity.serialNumber);
            config->setUploadPolicy(policy);
        }
    }
    return true;
}


// The CT_FLEET_KEY line for firmware/identity.local.ini, derived from the
// passphrase in the field above and from nothing else. It is a plain KEY=VALUE
// line and not a -D flag: the identity values left platformio.ini, and
// scripts/build_flags.py is what turns that file into the defines now.
//
// It used to fall back to the key the open configuration already carried, so
// that a blank field still showed a line. That was a hole rather than a
// convenience. A .ct3s carries the fleet key by design — that is exactly what
// lets a customer install an update without being told the passphrase — so a
// customer who was sent one had only to open it and pick Online > Fleet
// Identity to read the shared secret of the entire fleet, in hex, with no
// password, no reveal and no device anywhere near them. The line is a
// convenience for somebody who already knows the passphrase; the moment it can
// be had without one it stops being that and becomes the way to learn it.
//
// So there are two states now and only two: a passphrase typed into this dialog
// in this session, and nothing. The blank case shows the placeholder asking for
// a passphrase, which is also the honest answer to "what is this fleet's key?"
// from somebody who cannot supply one.
void FleetIdentityDialog::refreshFleetKeyFlag()
{
    if (!m_fleetKeyFlagEdit)
        return;
    const QString passphrase = m_fleetKeyEdit ? m_fleetKeyEdit->text() : QString();
    AccessKey key = kNoAccessKey;
    if (!passphrase.isEmpty()) {
        BusyScope busy(this); // one PBKDF2 pass; visible, but only on focus-out
        key = deriveAccessKey(passphrase);
    }

    if (key == kNoAccessKey) {
        m_fleetKeyFlagEdit->setText(QString());
        m_fleetKeyFlagEdit->setPlaceholderText(
            tr("(type a passphrase above to get the identity line)"));
        return;
    }
    // hexFieldText() is the same 0x + eight upper-case digits every other id in
    // this dialog is shown with, so the line and the readouts above it line up,
    // and build_flags.py reads the value with int(value, 0) either way. Emitted
    // exactly as identity.local.ini is parsed: KEY=VALUE, no -D, no quotes, no
    // spaces around the '='.
    m_fleetKeyFlagEdit->setText(
        QStringLiteral("CT_FLEET_KEY=%1").arg(hexFieldText(quint32(key))));
}

} // namespace ct
