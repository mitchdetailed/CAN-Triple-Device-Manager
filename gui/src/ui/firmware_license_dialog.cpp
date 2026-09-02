#include "firmware_license_dialog.h"

#include <QApplication>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

#include "../model/access_keys.h"
#include "../protocol/device_link.h"
#include "../protocol/wire_structs.h"
#include "name_limits.h"

namespace ct {

FirmwareLicenseDialog::FirmwareLicenseDialog(DeviceLink *link,
                                             std::function<bool()> ensureConnected,
                                             QWidget *parent)
    : QDialog(parent), m_link(link), m_ensureConnected(std::move(ensureConnected))
{
    setWindowTitle(tr("Firmware License Manager"));

    auto *layout = new QVBoxLayout(this);
    auto *intro = new QLabel(
        tr("Who this unit's firmware is licensed to. These values live in the device, not "
           "in the configuration file, and they survive a Send, a Clear and a firmware "
           "update."),
        this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto *form = new QFormLayout;
    m_manufacturer = new QLineEdit(this);
    m_model = new QLineEdit(this);
    m_version = new QLineEdit(this);
    m_key = new QLineEdit(this);
    m_updater = new QLineEdit(this);
    m_key->setEchoMode(QLineEdit::Password);
    m_updater->setEchoMode(QLineEdit::Password);

    // Byte budgets, not character counts. The device's fields are fixed-width
    // byte arrays, and one non-ASCII character costs two to four of them — so a
    // 32-"character" cap would let a name through that the device then stored
    // cut in half. Same helper every other name field in this application uses.
    limitToUtf8Bytes(m_manufacturer, LICENSE_MANUFACTURER_LEN);
    limitToUtf8Bytes(m_model, LICENSE_MODEL_LEN);
    limitToUtf8Bytes(m_version, LICENSE_VERSION_LEN);
    // The two SECRETS are capped in characters, not bytes, and that is not an
    // inconsistency: they are passphrases, which never reach the device. What
    // travels is a fixed 16-byte PBKDF2 derivation, so the limit here is about
    // what a person can be asked to retype, not about a field width.
    m_key->setMaxLength(LICENSE_PASSPHRASE_MAX);
    m_updater->setMaxLength(LICENSE_PASSPHRASE_MAX);

    form->addRow(tr("Firmware Manufacturer:"), m_manufacturer);
    form->addRow(tr("Firmware Model:"), m_model);
    form->addRow(tr("Firmware Version:"), m_version);
    form->addRow(tr("Firmware Key:"), m_key);
    form->addRow(tr("FW Updater Password:"), m_updater);
    // Indented under its field by being added as the value half of an empty
    // row, so it reads as belonging to the password above rather than as a
    // separate setting.
    m_clearUpdater = new QCheckBox(tr("Remove the FW Updater Password"), this);
    form->addRow(QString(), m_clearUpdater);
    layout->addLayout(form);

    m_state = new QLabel(this);
    m_state->setWordWrap(true);
    layout->addWidget(m_state);

    m_warning = new QLabel(this);
    m_warning->setWordWrap(true);
    layout->addWidget(m_warning);

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Close, this);
    layout->addWidget(m_buttons);
    connect(m_buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, this,
            &FirmwareLicenseDialog::apply);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    for (QLineEdit *edit : {m_manufacturer, m_model, m_version, m_key, m_updater})
        connect(edit, &QLineEdit::textChanged, this, &FirmwareLicenseDialog::refreshEnabled);
    connect(m_clearUpdater, &QCheckBox::toggled, this, &FirmwareLicenseDialog::refreshEnabled);

    // Only if a unit already happens to be on the cable. Opening this dialog
    // must not drag up a connection prompt — see the header: composing a licence
    // is desk work, and Apply is the step that needs hardware.
    if (m_link && m_link->isOpen())
        reload();
    else
        refreshEnabled();
}

void FirmwareLicenseDialog::reload()
{
    QString error;
    if (!device_session::readLicense(m_link, &m_current, &error)) {
        QMessageBox::warning(this, windowTitle(),
                             error.isEmpty() ? tr("The device did not answer.") : error);
        return;
    }
    m_manufacturer->setText(m_current.manufacturer);
    m_model->setText(m_current.model);
    m_version->setText(m_current.firmwareVersion);
    // Never populated, because the device will not say. The placeholders carry
    // the rule, so nobody reads an empty box as "there is none".
    m_key->clear();
    m_updater->clear();
    m_key->setPlaceholderText(m_current.keySet ? tr("(unchanged — leave blank to keep)")
                                               : tr("(none set)"));
    m_updater->setPlaceholderText(m_current.updaterSet ? tr("(unchanged — leave blank to keep)")
                                                       : tr("(none set — anyone may write)"));
    m_clearUpdater->setChecked(false); // a fresh read is not a pending removal
    refreshEnabled();
}

void FirmwareLicenseDialog::refreshEnabled()
{
    QString state;
    if (!m_link || !m_link->isOpen()) {
        state = tr("Not connected. Apply will connect to a device before writing.");
    } else if (!m_current.supported) {
        state = tr("This unit's firmware is older than the Firmware License Manager and "
                   "cannot hold a licence. Update the firmware, then issue one.");
    } else {
        state = m_current.updaterSet
                    ? tr("This unit has an FW Updater Password. Applying will ask for it.")
                    : tr("This unit has no FW Updater Password, so anyone who connects can "
                         "rewrite these details. Setting one is what stops that.");
    }
    m_state->setText(state);

    // Offered only while the password field is empty: typing a new password and
    // asking to remove it at the same time are contradictory, and disabling one
    // of them is clearer than picking a winner. Unchecked as it is disabled, so
    // a tick made earlier cannot fire once it has stopped being visible.
    const bool mayClear = m_updater->text().isEmpty();
    if (!mayClear && m_clearUpdater->isChecked())
        m_clearUpdater->setChecked(false);
    m_clearUpdater->setEnabled(mayClear);
    m_clearUpdater->setToolTip(
        mayClear ? tr("Leaves this unit with no password, so anyone who connects can rewrite "
                      "these details.")
                 : tr("Clear the FW Updater Password field to remove the password instead of "
                      "changing it."));

    // The only local checks worth making. passwordProblem is the same policy the
    // access passwords use, so a phrase acceptable in one place is acceptable in
    // the other; the device cannot enforce it, since all it ever sees is a
    // derivation that looks the same either way.
    QStringList warnings;
    if (!m_key->text().isEmpty()) {
        const QString why = passwordProblem(m_key->text());
        if (!why.isEmpty())
            warnings << tr("Firmware Key: %1").arg(why);
    }
    if (!m_updater->text().isEmpty()) {
        const QString why = passwordProblem(m_updater->text());
        if (!why.isEmpty())
            warnings << tr("FW Updater Password: %1").arg(why);
    }
    m_warning->setText(warnings.join(QStringLiteral("\n")));
    m_warning->setVisible(!warnings.isEmpty());

    // Apply stays available while disconnected — connecting is its first step.
    // Only a malformed passphrase disables it, because that is the one thing a
    // round trip could not sort out.
    m_buttons->button(QDialogButtonBox::Apply)->setEnabled(warnings.isEmpty());
}

bool FirmwareLicenseDialog::ensureProved()
{
    if (!m_current.updaterSet || m_proved)
        return true;

    // Asked once per dialog session, not once per Apply: proving is a round trip
    // and the device remembers for the connection. Re-asking on every Apply
    // would train people to retype a secret, which is how secrets get simpler.
    for (;;) {
        bool ok = false;
        const QString phrase = QInputDialog::getText(
            this, windowTitle(),
            tr("This unit is protected.\n\nEnter its FW Updater Password:"),
            QLineEdit::Password, QString(), &ok);
        if (!ok)
            return false;

        QString error;
        bool wrongPassword = false;
        if (device_session::proveLicenseSecret(m_link, deriveLicenseKey(phrase), &error,
                                               &wrongPassword)) {
            m_proved = true;
            return true;
        }
        if (!wrongPassword) {
            QMessageBox::warning(this, windowTitle(),
                                 error.isEmpty() ? tr("The device did not answer.") : error);
            return false;
        }
        QMessageBox::warning(this, windowTitle(),
                             tr("That is not this unit's FW Updater Password."));
    }
}

void FirmwareLicenseDialog::apply()
{
    // Step one is hardware. The dialog is usable offline so a licence can be
    // composed at a desk; Apply is where a unit has to be present.
    if (!m_link || !m_link->isOpen()) {
        if (!m_ensureConnected || !m_ensureConnected())
            return;
        // What is typed in the fields is the operator's intent and must survive
        // the connection, so the record is read for its FLAGS only — which
        // secrets the unit holds, and whether it can hold a licence at all.
        // Overwriting the strings here would silently discard the edit that
        // prompted the connection in the first place.
        device_session::LicenseState onDevice;
        QString readError;
        if (!device_session::readLicense(m_link, &onDevice, &readError)) {
            QMessageBox::warning(this, windowTitle(),
                                 readError.isEmpty() ? tr("The device did not answer.")
                                                     : readError);
            return;
        }
        m_current = onDevice;
        m_proved = false;
        m_key->setPlaceholderText(m_current.keySet ? tr("(unchanged — leave blank to keep)")
                                                   : tr("(none set)"));
        m_updater->setPlaceholderText(m_current.updaterSet
                                          ? tr("(unchanged — leave blank to keep)")
                                          : tr("(none set — anyone may write)"));
        refreshEnabled();
    }

    if (!m_current.supported) {
        // Not an unlicensed unit — a unit that cannot be asked. Telling the two
        // apart is the difference between "issue a licence" and "update the
        // firmware first", and only one of those is actionable here.
        QMessageBox::information(
            this, windowTitle(),
            tr("This unit's firmware is older than the Firmware License Manager and cannot "
               "hold a licence.\n\nUpdate the firmware, then issue one."));
        return;
    }

    if (!ensureProved())
        return;

    const QString updaterPhrase = m_updater->text();
    const bool settingUpdater = !updaterPhrase.isEmpty();
    // Only a removal if there is something to remove. Ticking the box on a unit
    // that has no password is not an error worth a dialog — it is a no-op, and
    // saying so would be pedantry about an instruction already satisfied.
    const bool clearingUpdater = m_clearUpdater->isChecked() && m_current.updaterSet;

    if (clearingUpdater) {
        const auto answer = QMessageBox::question(
            this, windowTitle(),
            tr("Remove this unit's FW Updater Password?\n\nAnyone who connects will then be "
               "able to rewrite its manufacturer, model, version and Firmware Key without "
               "being asked for anything."),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes)
            return;
    }

    // Issuing the first password is the one step here that can lock somebody
    // out, so it is the one that asks. Everything else can be edited back; a
    // password nobody wrote down cannot be, and the unit then refuses every
    // later change.
    if (settingUpdater && !m_current.updaterSet) {
        const auto answer = QMessageBox::question(
            this, windowTitle(),
            tr("Set an FW Updater Password on this unit?\n\nFrom then on, changing any of "
               "these values requires it. The device never discloses it and there is no way "
               "to recover it — if it is lost, this unit's licence can no longer be changed "
               "by anyone."),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes)
            return;
    }

    device_session::LicenseWrite write;
    write.manufacturer = m_manufacturer->text();
    write.model = m_model->text();
    write.firmwareVersion = m_version->text();
    // Blank means keep, for both. That is what makes fixing a typo in a model
    // name bearable — neither secret has to be retyped to do it.
    if (!m_key->text().isEmpty())
        write.key = deriveLicenseKey(m_key->text());
    if (settingUpdater)
        write.updaterKey = deriveLicenseKey(updaterPhrase);
    write.clearUpdater = clearingUpdater;

    QString error;
    // The write is a flash erase in the bank the device executes from, so it
    // stalls the unit for tens of milliseconds. Worth the wait cursor.
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const bool ok = device_session::writeLicense(m_link, write, &error);
    QApplication::restoreOverrideCursor();

    if (!ok) {
        QMessageBox::warning(this, windowTitle(),
                             error.isEmpty() ? tr("The device refused the write.") : error);
        return;
    }

    // A changed password ends this session's right to write again — the device
    // drops its proof whenever the password moves, so the local flag has to
    // follow or the next Apply would be refused with no explanation. Removing
    // one counts as moving it, though the point is nearly moot: with no password
    // there is nothing left to prove.
    if (settingUpdater || clearingUpdater)
        m_proved = false;

    QMessageBox::information(this, windowTitle(), tr("The licence has been written to the device."));
    reload();
}

} // namespace ct
