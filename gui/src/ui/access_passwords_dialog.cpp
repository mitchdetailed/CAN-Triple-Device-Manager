// Online > Set Access Passwords… — see access_passwords_dialog.h for what the
// three passwords are and why they live in the device rather than in the file.
//
// Everything here is synchronous, because everything underneath it is: a device
// round trip is a nested event loop and a key derivation is a deliberately slow
// 210,000-round PBKDF2. Both are wrapped in a BusyScope so a second click
// cannot re-enter a flow that is already half way through one.
//
// The prompts are plain QInputDialogs rather than a custom form on purpose.
// Dash Manager asks one question at a time — old password, new password,
// confirm — and a single form with three fields would let a user fill in the
// new password before discovering the device wants the old one first.
#include "access_passwords_dialog.h"

#include <QApplication>
#include <QColor>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

namespace ct {

namespace {

// Blocks the widget while a device round trip or a key derivation runs. The
// same shape as MainWindow's BusyScope; duplicated rather than shared because
// one small RAII class is cheaper than a header for it, and `widget` may be
// null here — promptAndProve() is a static that a caller can hand no parent.
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

// Both accents are picked the way the old Configuration Password dialog picked
// its warning colour: by asking the palette whether we are on a dark theme. A
// hard-coded green reads as sickly on one theme or invisible on the other, and
// the palette's own text colours carry no notion of "good" or "wrong".
bool isDarkTheme(const QPalette &pal)
{
    return pal.color(QPalette::Window).lightness() < 128;
}

QColor tickColour(const QPalette &pal)
{
    return isDarkTheme(pal) ? QColor(0x7e, 0xe7, 0x87) : QColor(0x1a, 0x7f, 0x37);
}

QColor problemColour(const QPalette &pal)
{
    return isDarkTheme(pal) ? QColor(0xff, 0xa1, 0x78) : QColor(0xc0, 0x30, 0x00);
}

QString colourRule(const QColor &colour)
{
    return QStringLiteral("color: %1;").arg(colour.name());
}

// What clearing THIS password actually permits. accessFunctionDescription()
// says what withholding a password prevents, which is the same fact from the
// other side; a confirmation has to name the consequence of the button being
// pressed, not the state being left behind.
QString clearingMeans(AccessFunction fn)
{
    switch (fn) {
    case AccessFunction::SendConfiguration:
        return QObject::tr("Anyone who can connect to this device will be able to replace its "
                           "configuration.");
    case AccessFunction::GetConfiguration:
        return QObject::tr("Anyone who can connect to this device will be able to read its "
                           "configuration back out of it.");
    case AccessFunction::EditProtectedComms:
        // Protect Communication ONLY, and only HALF of it since 2.3.1. This
        // password is not a master key over the other two tiers and must not be
        // described as one: Read Only and Hidden are unlocked by the section's own
        // Message Password and by nothing else, so clearing this changes nothing
        // for them. Saying otherwise here was advertising a substitution the code
        // does not perform — and a user told this is the master password would set
        // it INSTEAD of the section passwords that actually guard those tiers.
        return QObject::tr("Protect Communication markings lose their device half — there will "
                           "be nothing left for a unit to check. A message that also carries "
                           "its own Message Password keeps that half and stays guarded; one "
                           "written before every marking needed one has nothing left at all, "
                           "and anyone will be able to untick it. Read Only and Hidden sections "
                           "are unaffected either way: they are guarded by each section's own "
                           "Message Password, which this password never stands in for.");
    }
    return QString();
}

// Ask for one password and prove it against the device, re-asking until it is
// right or the user gives up. Shared by the Set… flow and by promptAndProve();
// they differ only in the wording, which is why the title and intro come in as
// arguments rather than being decided here.
bool proveLoop(DeviceLink *link, AccessFunction fn, QWidget *parent, const QString &title,
               const QString &intro, AccessKey *keyOut)
{
    QString message = intro;
    for (;;) {
        bool ok = false;
        const QString entered =
            QInputDialog::getText(parent, title, message + QObject::tr("\n\nPassword :"),
                                  QLineEdit::Password, QString(), &ok);
        if (!ok)
            return false;
        // The device says this function IS protected, so an empty answer cannot
        // be the right one. Rejecting it here saves a pointless derivation and,
        // more importantly, keeps kNoAccessKey out of proveAccess() — where it
        // would fail as a malformed request rather than as a wrong password.
        if (entered.isEmpty()) {
            message = QObject::tr("That password is not correct.");
            continue;
        }

        QString error;
        bool wrongPassword = false;
        bool proved = false;
        {
            BusyScope busy(parent);
            const AccessKey key = deriveAccessKey(entered);
            proved = device_session::proveAccess(link, fn, key, &error, &wrongPassword);
            if (proved && keyOut)
                *keyOut = key;
        }
        if (proved)
            return true;
        if (!wrongPassword) {
            // A link failure is not a wrong password and must not be reported as
            // one — re-prompting would have the user typing a correct password
            // over and over at a device that is no longer listening.
            QMessageBox::warning(parent, title,
                                 QObject::tr("The device could not check that password.\n\n%1")
                                     .arg(error));
            return false;
        }
        // Says nothing about how wrong it was — no "close", no length hint,
        // nothing that narrows a guess.
        message = QObject::tr("That password is not correct.");
    }
}

} // namespace

AccessPasswordsDialog::AccessPasswordsDialog(DeviceLink *link, Configuration *config,
                                             QWidget *parent)
    : QDialog(parent)
    , m_link(link)
    , m_config(config)
    , m_functionList(nullptr)
    , m_setButton(nullptr)
    , m_legendLabel(nullptr)
    , m_statusLabel(nullptr)
{
    setWindowTitle(tr("Set Access Passwords"));
    setModal(true);

    auto *mainLayout = new QVBoxLayout(this);

    auto *group = new QGroupBox(tr("Function Passwords"), this);
    auto *groupLayout = new QVBoxLayout(group);
    groupLayout->addWidget(new QLabel(tr("Select the function to set the password for :"), group));

    auto *listRow = new QHBoxLayout;
    m_functionList = new QTreeWidget(group);
    m_functionList->setColumnCount(1);
    m_functionList->setHeaderLabels({tr("Function")});
    m_functionList->setRootIsDecorated(false);
    m_functionList->setUniformRowHeights(true);
    m_functionList->setAllColumnsShowFocus(true);
    m_functionList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_functionList->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    listRow->addWidget(m_functionList, 1);

    // Top-aligned beside the list, as in Dash Manager: the button acts on the
    // selected row, so it belongs level with the first one rather than centred
    // against a list whose length never changes.
    auto *buttonColumn = new QVBoxLayout;
    m_setButton = new QPushButton(tr("Set…"), group);
    buttonColumn->addWidget(m_setButton);
    buttonColumn->addStretch(1);
    listRow->addLayout(buttonColumn);
    groupLayout->addLayout(listRow);

    m_legendLabel = new QLabel(tr("✓ = Password set"), group);
    m_legendLabel->setStyleSheet(colourRule(tickColour(palette())));
    groupLayout->addWidget(m_legendLabel);
    mainLayout->addWidget(group, 1);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setWordWrap(true);
    mainLayout->addWidget(m_statusLabel);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    mainLayout->addWidget(buttons);

    connect(m_setButton, &QPushButton::clicked, this, &AccessPasswordsDialog::onSet);
    connect(m_functionList, &QTreeWidget::itemSelectionChanged, this,
            &AccessPasswordsDialog::onSelectionChanged);
    // Double-click is the same as Set…; onSet() re-checks the preconditions the
    // button's enabled state stands for, so a double-click on a dead list does
    // nothing rather than starting a flow that cannot finish.
    connect(m_functionList, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem *, int) { onSet(); });

    refreshList();
    resize(520, 320);
}

void AccessPasswordsDialog::refreshList()
{
    // Which row was selected, so a rebuild does not move the user's place. -1
    // means nothing was selected, which is only true the first time through.
    const QTreeWidgetItem *current = m_functionList->currentItem();
    const int previous = current ? current->data(0, Qt::UserRole).toInt() : -1;
    const int previousSlot = current ? current->data(0, Qt::UserRole + 1).toInt() : 1;

    // Always re-read. The dialog's whole claim is that it shows what is on the
    // unit, and after a Set… the device is the only authority on what took.
    m_state = device_session::AccessState{};
    QString problem;
    if (!m_link || !m_link->isOpen()) {
        problem = tr("Not connected. Access passwords are held in the device, so setting one "
                     "needs a connected CAN Triple running v19 firmware or newer.");
    } else {
        QString error;
        BusyScope busy(this);
        if (!device_session::readAccessState(m_link, &m_state, &error)) {
            problem = tr("The device's access passwords could not be read.\n\n%1").arg(error);
        } else if (!m_state.supported) {
            problem = tr("This device's firmware is older than v19 and has no access passwords. "
                         "The functions below are listed so you can see what a firmware update "
                         "would let you protect.");
        }
    }
    m_statusLabel->setText(problem);
    m_statusLabel->setStyleSheet(problem.isEmpty() ? QString()
                                                   : colourRule(problemColour(palette())));
    m_statusLabel->setVisible(!problem.isEmpty());

    m_functionList->clear();
    const QColor tick = tickColour(palette());
    const AccessFunction *functions = allAccessFunctions();
    for (int i = 0; i < kAccessFunctionCount; ++i) {
        const AccessFunction fn = functions[i];
        // v17: Protected Comms is FOUR rows, one per slot, so a unit can accept
        // configurations sealed under any of four passwords. Send and Get stay
        // single. Each row's tick comes from the device's per-slot mask; on
        // firmware that predates the mask, protSlots is 0 and slot 1 falls back
        // to the function bit, which is what that firmware means by it.
        // ("slotCount", because "slots" is a Qt keyword macro.)
        const int slotCount = fn == AccessFunction::EditProtectedComms ? 4 : 1;
        for (int slot = 1; slot <= slotCount; ++slot) {
            bool isSet = m_state.supported && m_state.isSet(fn);
            if (slotCount > 1)
                isSet = m_state.protSlots != 0 ? (m_state.protSlots & (1u << (slot - 1))) != 0
                                               : (slot == 1 && isSet);
            // The tick is part of the item's TEXT, so the colour applies to the
            // whole row rather than to the glyph alone.
            auto *item = new QTreeWidgetItem(m_functionList);
            const QString label =
                slotCount > 1 ? tr("%1 — Slot %2").arg(accessFunctionLabel(fn)).arg(slot)
                          : accessFunctionLabel(fn);
            item->setText(0,
                          (isSet ? QStringLiteral("✓ ") : QStringLiteral("   ")) + label);
            item->setToolTip(0, accessFunctionDescription(fn));
            item->setData(0, Qt::UserRole, int(fn));
            item->setData(0, Qt::UserRole + 1, slot);
            if (isSet)
                item->setForeground(0, tick);
            if (int(fn) == previous && slot == qMax(1, previousSlot))
                m_functionList->setCurrentItem(item);
        }
    }

    onSelectionChanged();
}

void AccessPasswordsDialog::onSelectionChanged()
{
    // Nothing to set without a row, and nothing to set on a device that cannot
    // hold a password — but the list stays readable in both cases.
    m_setButton->setEnabled(m_state.supported && m_functionList->currentItem() != nullptr);
}

AccessFunction AccessPasswordsDialog::selectedFunction() const
{
    const QTreeWidgetItem *item = m_functionList->currentItem();
    // Only ever asked with a row selected, because Set… is disabled otherwise.
    // The fallback exists so that stops being an assumption.
    if (!item)
        return AccessFunction::SendConfiguration;
    return AccessFunction(item->data(0, Qt::UserRole).toInt());
}

int AccessPasswordsDialog::selectedSlot() const
{
    const QTreeWidgetItem *item = m_functionList->currentItem();
    if (!item)
        return 1;
    const int slot = item->data(0, Qt::UserRole + 1).toInt();
    return slot >= 1 && slot <= 4 ? slot : 1;
}

void AccessPasswordsDialog::onSet()
{
    if (!m_state.supported || !m_functionList->currentItem())
        return;
    runSetFlow(selectedFunction(), selectedSlot());
    // Unconditionally, including after a refusal or a cancel: a flow that got
    // as far as the device may have changed it, and the cheapest way to be
    // right about that is to ask again.
    refreshList();
}

bool AccessPasswordsDialog::runSetFlow(AccessFunction fn, int slot)
{
    const QString title = tr("Set Password");
    // Setting Protected Comms writes a verifier into the open document as
    // well as a key into the device, so that the app and the unit agree about
    // which password opens a protected message.
    // v17: ONLY SLOT 1 TOUCHES THE DOCUMENT. The document seals its protected
    // messages under one Protected Comms password of its own; slots 2..4 are
    // extra keys the DEVICE accepts \u2014 other vendors' \u2014 and have no business
    // rewriting this configuration's.
    const bool touchesDocument =
        fn == AccessFunction::EditProtectedComms && m_config && slot == 1;

    // Checked BEFORE the device is touched. Configuration::setCommsPassword()
    // refuses while the document's protected messages are still concealed, and
    // discovering that after the key is already programmed would leave the two
    // holding different passwords — exactly the mismatch this dialog exists to
    // prevent.
    if (touchesDocument && !m_config->commsRevealed()) {
        QMessageBox::warning(
            this, title,
            tr("This configuration's protected messages are still hidden, so its Protected "
               "Comms password cannot be changed.\n\nReveal them first with File > Reveal "
               "Protected Comms, then set the password again. Changing it only on the device "
               "would leave the device and this configuration expecting different passwords."));
        return false;
    }

    // Step 1: the old password, when there is one. Changing a password you
    // cannot produce would be the way past not knowing it.
    if (m_state.isSet(fn)
        && !proveLoop(m_link, fn, this, title, tr("Enter Old Password"), nullptr))
        return false;

    // Steps 2 and 3: the new password, twice. A mismatch or a declined clear
    // starts the pair again rather than throwing away the whole flow.
    QString newPassword;
    for (;;) {
        bool ok = false;
        const QString first =
            QInputDialog::getText(this, title, tr("Enter New Password\n\nPassword :"),
                                  QLineEdit::Password, QString(), &ok);
        if (!ok)
            return false;

        if (first.isEmpty()) {
            // Blank means "no password from now on". That is a real decision
            // with a real consequence, so it is confirmed in the terms of the
            // function being cleared rather than as a generic "are you sure".
            // v17: the multi-slot function confirms by ROW, and clearing one
            // slot of several is not clearing the protection - the device keeps
            // accepting the other slots' passwords, so the consequence text
            // would overstate.
            QString rowLabel = accessFunctionLabel(fn);
            QString means = clearingMeans(fn);
            if (fn == AccessFunction::EditProtectedComms) {
                rowLabel = tr("%1 — Slot %2").arg(rowLabel).arg(slot);
                const quint8 others = quint8(m_state.protSlots & ~(1u << (slot - 1)));
                if (others != 0)
                    means = tr("The other Protected Comms slots stay set, so the device "
                               "still accepts those passwords.");
            }
            if (QMessageBox::question(this, title,
                                      tr("Leave \"%1\" with no password?\n\n%2")
                                          .arg(rowLabel, means),
                                      QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
                != QMessageBox::Yes)
                continue;
            newPassword.clear();
            break;
        }

        // Checked before the confirmation prompt, so a too-short password is
        // rejected once rather than after being typed twice.
        if (const QString why = passwordProblem(first); !why.isEmpty()) {
            QMessageBox::warning(this, title, why);
            continue;
        }

        const QString second =
            QInputDialog::getText(this, title, tr("Confirm New Password\n\nPassword :"),
                                  QLineEdit::Password, QString(), &ok);
        if (!ok)
            return false;
        if (first != second) {
            QMessageBox::warning(this, title,
                                 tr("The two passwords do not match. Enter the new password "
                                    "again."));
            continue;
        }
        newPassword = first;
        break;
    }

    // Step 4: the device. The key is derived here and goes no further — nothing
    // in this dialog ever displays it, and it is not kept once the write is
    // done.
    QString error;
    bool written = false;
    {
        BusyScope busy(this);
        if (newPassword.isEmpty()) {
            written = device_session::clearAccessKey(m_link, fn, &error, slot);
        } else {
            const AccessKey key = deriveAccessKey(newPassword);
            written = device_session::writeAccessKey(m_link, fn, key, &error, slot);
        }
    }
    if (!written) {
        QMessageBox::warning(this, title,
                             tr("The device would not accept the change.\n\n%1").arg(error));
        return false;
    }

    // The durability limit, and what 2.3.0 does about it.
    //
    // The device holds access keys in its flash HEADER, and STM32 flash programs
    // each doubleword once per erase — so CMD_WRITE_ACCESS_KEYS takes effect for
    // this session but does not reach flash until the next configuration commit.
    // A user who set a password and unplugged the unit used to get it back
    // unprotected, and until 2.3.0 that cost nothing because nothing gated on
    // the key.
    //
    // It costs everything now for Protected Comms. It is the password a
    // device confirms before this application will let a Protect Communication
    // marking be unticked, so "set the password, power-cycle without sending"
    // was a one-power-cycle bypass of the whole tier — and a silent one, because
    // the dialog would still have shown the tick beforehand.
    //
    // So a commit is ATTEMPTED here. An empty payload is the protocol's "leave
    // the stored configuration version alone", which is exactly right: it commits
    // the header the key lives in and asserts nothing about the tables beside it.
    //
    // Be clear about how far that gets, because it was measured on hardware and
    // it is less far than it looks. Only engine_clear_config() erases, and STM32
    // flash programs each doubleword once per erase — so this save SUCCEEDS only
    // on a unit whose header has not been programmed since its last erase, i.e.
    // one that has just been cleared. On any unit already carrying a
    // configuration it comes back NACK and the key stays in RAM. That is the
    // ordinary case, so the warning below is the NORMAL path and not an
    // exception, and the password genuinely does not survive a power cycle until
    // a Send erases and rewrites the region.
    //
    // The real fix is the one serial_proto.c names: a small append-log page for
    // device state, like preserve_store.c already runs for retained counters.
    // Until that exists, this is a durability limit to state plainly rather than
    // a bug to hide behind a forced round trip that mostly fails.
    bool committed = false;
    quint8 commitCode = 0;
    QString commitError;
    {
        BusyScope busy(this);
        committed = m_link->requestSync(CMD_SAVE_TO_FLASH, QByteArray(), nullptr, &commitError,
                                        DeviceLink::kFlashTimeoutMs,
                                        DeviceLink::kDefaultRetries, &commitCode);
    }
    // Committing the header is a write by any other name, so the device gates it
    // on the SEND password. Someone setting a password on a unit they have not
    // yet proved Send against is the ordinary case, not an error — prove it and
    // commit once more. Only once: a second refusal is something else, and a
    // loop here would be a password prompt with no way out.
    //
    // Outside the BusyScope above, deliberately. promptAndProve raises a modal
    // input dialog, and a prompt over a still-disabled window under a still-
    // spinning wait cursor reads as a hung application — the same reason
    // runSetFlow's other round trips close their scope before reporting.
    if (!committed && commitCode == ERR_LOCKED
        && promptAndProve(m_link, AccessFunction::SendConfiguration, this)) {
        BusyScope busy(this);
        committed = m_link->requestSync(CMD_SAVE_TO_FLASH, QByteArray(), nullptr, &commitError,
                                        DeviceLink::kFlashTimeoutMs);
    }
    // NOT A SCREEN OF ITS OWN ANY MORE. On a unit that already holds a
    // configuration this commit fails almost every time - the flash header can
    // only be written once per erase - so a full-page warning here fired on
    // essentially every password set, in the middle of the flow it was
    // interrupting. The fact still matters (power the unit off before the next
    // Send and the password is gone), so it is carried into the ONE
    // confirmation dialog the flow already shows, as a sentence rather than a
    // ceremony.
    Q_UNUSED(commitError);
    const QString durability =
        committed ? QString()
                  : tr("\n\nThe device holds it in memory until the next Online → Send "
                       "Configuration; power the unit off before then and it is gone.");

    // Step 5: the document, for Protected Comms only. The other two are
    // device gates — they say what a unit will do for you, not what a file will
    // show you — and a document has no business storing them.
    if (touchesDocument) {
        if (!m_config->setCommsPassword(newPassword)) {
            // The pre-check above should have made this unreachable, so if it
            // happens the user is told plainly that the two now disagree rather
            // than being left to find out at the next Reveal.
            QMessageBox::warning(
                this, title,
                tr("The device accepted the change, but this configuration could not be updated "
                   "to match. The device and this configuration now expect different Edit "
                   "Protected Comms passwords — set it again once the configuration's protected "
                   "messages are revealed."));
            return false;
        }
        QMessageBox::information(
            this, title,
            (newPassword.isEmpty()
                 ? tr("The Protected Comms password is cleared, on the device and in this "
                      "configuration.\n\nSave the configuration for this to take effect on "
                      "the file.")
                 : tr("The Protected Comms password is set, on the device and in this "
                      "configuration.\n\nSave the configuration for this to take effect on "
                      "the file."))
                + durability);
    } else if (!durability.isEmpty()) {
        // The Send and Get passwords have no confirmation dialog of their own,
        // so the durability sentence gets a small one rather than vanishing.
        QMessageBox::information(this, title,
                                 tr("The password is set on the device.") + durability);
    }
    return true;
}

bool AccessPasswordsDialog::promptAndProve(DeviceLink *link, AccessFunction fn, QWidget *parent,
                                           AccessKey *keyOut)
{
    if (keyOut)
        *keyOut = kNoAccessKey;

    const QString title = tr("Access Password");
    if (!link || !link->isOpen()) {
        QMessageBox::warning(parent, title, tr("There is no connection to a device."));
        return false;
    }

    device_session::AccessState state;
    QString error;
    bool read = false;
    {
        // The scope closes before anything is reported: a message box put up
        // while the parent is still disabled and the wait cursor is still on
        // reads as a frozen application.
        BusyScope busy(parent);
        read = device_session::readAccessState(link, &state, &error);
    }
    if (!read) {
        QMessageBox::warning(parent, title,
                             tr("The device's access passwords could not be read.\n\n%1")
                                 .arg(error));
        return false;
    }
    // Pre-v19 firmware has no access passwords at all, and a function with none
    // set is not something to prove. Both mean "go ahead" — turning either into
    // a prompt would ask for a password that does not exist.
    if (!state.supported || !state.isSet(fn))
        return true;

    // Names the function, because the three passwords are independent and a
    // bare "Password :" leaves the user guessing which of theirs is wanted.
    return proveLoop(link, fn, parent, title,
                     tr("This device is protected. Enter the password for \"%1\".")
                         .arg(accessFunctionLabel(fn)),
                     keyOut);
}

} // namespace ct
