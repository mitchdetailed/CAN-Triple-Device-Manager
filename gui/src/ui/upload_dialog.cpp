// Online > Upload Configuration… — see upload_dialog.h for what the six rules
// are, and for the honest account of what they are worth.
//
// Two things about this file are worth knowing before changing it.
//
// First, evaluate() is a static that takes a link and a document and returns
// data. It draws nothing. Send Configuration applies the same rules from the
// same function, so the two paths cannot drift into disagreeing about what
// counts as a match — which they would within a release if each carried its own
// copy of the comparisons.
//
// Second, every rule is reported whether it passed or not. A dialog that listed
// only the problems would answer "why was this refused?" and nothing else,
// while the question an installer actually has in front of a silent unit is
// "which of these did you check, and what did the device say?".
#include "upload_dialog.h"

#include <QApplication>
#include <QColor>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QStringList>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include "../model/device_mapper.h"
#include "../protocol/config_transfer.h"
#include "../scripting/script_compiler.h"   // mapWithScript
#include "access_passwords_dialog.h"

namespace ct {

namespace {

// Blocks the dialog while the device round trips run. evaluate() makes up to
// two synchronous calls — the identity read and the key challenge — each a
// nested event loop, and without this a second click on Upload would re-enter a
// check that is still half way through the first.
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

// The same palette-derived accents the access passwords dialog uses, and
// duplicated here for the same reason it duplicated its BusyScope: two small
// helpers are cheaper than a shared header, and a hard-coded green reads as
// sickly on one theme and invisible on the other.
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

// Amber, and deliberately not the failure colour. A warning that looks like a
// refusal gets treated as one — the reader stops instead of reading — and a
// warning that looks like a pass gets skipped. It has to be visibly a third
// thing.
QColor warningColour(const QPalette &pal)
{
    return isDarkTheme(pal) ? QColor(0xe3, 0xb3, 0x41) : QColor(0x8a, 0x62, 0x00);
}

QString colourRule(const QColor &colour)
{
    return QStringLiteral("color: %1;").arg(colour.name());
}

// An empty column reads as "the dialog failed to fill this in". "(not set)"
// reads as the fact it is, which is what an unprovisioned side of the
// comparison actually looks like.
QString orUnset(const QString &text)
{
    return text.isEmpty() ? QObject::tr("(not set)") : text;
}

} // namespace

// ---------------------------------------------------------------- the verdict

// NotChecked is deliberately not a failure. A package that pins no serial, or
// that asks for no key, is a package whose author chose not to narrow those
// things — refusing on that basis would turn every plain .ct3 into an error.
// Only an explicit Fail stops an upload.
//
// It is not a pass either, and keeping those two apart is what this function is
// for. A verdict made ENTIRELY of NotChecked rules established nothing
// whatsoever — it is the answer an empty document gives, where there is no
// vendor to compare, no model, no serial pin, no key and no version — and
// reporting that as "everything passed" put a green all-clear and a live Upload
// button in front of an installer with nothing to install. "Nothing objected"
// and "we could not check" are different answers and the caller has to be able
// to tell them apart, so at least one rule must have run and passed before this
// says yes.
bool UploadVerdict::allPassed() const
{
    if (!deviceReadable)
        return false;
    bool anyPassed = false;
    for (const UploadRule &rule : rules) {
        if (rule.status == UploadRule::Fail || rule.status == UploadRule::Warn)
            return false;
        if (rule.status == UploadRule::Pass)
            anyPassed = true;
    }
    return anyPassed;
}

// The only question that decides whether Upload is allowed. A Warn is not a
// failure and never gates anything — it is said once and the upload proceeds.
bool UploadVerdict::hasBlockingFailure() const
{
    for (const UploadRule &rule : rules) {
        if (rule.status == UploadRule::Fail)
            return true;
    }
    return false;
}

QStringList UploadVerdict::failureSummaries() const
{
    // An unreadable device has no rules to fail, but it very much has something
    // wrong with it, and a caller asking "what is wrong" deserves that answer
    // rather than an empty list that reads as "nothing".
    if (!deviceReadable)
        return problem.isEmpty() ? QStringList() : QStringList{problem};

    QStringList out;
    for (const UploadRule &rule : rules) {
        if (rule.status != UploadRule::Fail)
            continue;
        out.append(rule.message);
    }
    return out;
}

// Kept separate from failureSummaries() rather than folded in with a severity
// tag, because the two are acted on differently everywhere: failures refuse,
// warnings are printed. A caller that mixed them would have to re-split them.
QStringList UploadVerdict::warningSummaries() const
{
    QStringList out;
    for (const UploadRule &rule : rules) {
        if (rule.status != UploadRule::Warn)
            continue;
        out.append(rule.message);
    }
    return out;
}

// --------------------------------------------------------------- the checking

// The rules, in the order the dialog lists them.
//
// Every string that ends up in `expected` or `actual` is written to be safe to
// show to whoever is standing in front of the device, which means none of them
// quotes a byte of configuration content — not a message name, not a CAN ID,
// not a channel. That is not fussiness. This check exists so that a unit whose
// configuration must stay closed can still answer "is this update for me?", and
// a diagnostic that leaked the protocol on the way to answering would hand over
// exactly what the arrangement is protecting. The identity fields ARE quoted,
// because they are the one part of a unit that is public by design: the
// firmware answers CMD_READ_FLEET_ID to anybody, precisely so that this
// question can be asked without opening anything.
UploadVerdict UploadDialog::evaluate(DeviceLink *link, const Configuration &config)
{
    UploadVerdict verdict;

    device_session::FleetIdentityState state;
    QString error;
    if (!link || !link->isOpen()) {
        verdict.problem = tr("Not connected. Connect to the CAN Triple you want to update, so "
                             "this package can be checked against it.");
        return verdict;
    }
    if (!device_session::readFleetIdentity(link, &state, &error)) {
        verdict.problem = tr("The device would not say what it is, so nothing about this package "
                             "could be checked against it.\n\n%1").arg(error);
        return verdict;
    }
    if (!state.supported) {
        verdict.problem = tr("This device's firmware predates fleet identities, so it cannot say "
                             "what it is and this package cannot be checked against it.");
        return verdict;
    }
    verdict.deviceReadable = true;

    const FleetIdentity &pkg = config.fleetIdentity();
    const FleetIdentity &dev = state.identity;
    const UploadPolicy &policy = config.uploadPolicy();

    // Vendor and model are the same comparison twice over: exact, case and
    // spacing included, because they are identifiers rather than display names.
    //
    // The asymmetry between the two sides is deliberate and was wrong once. Only
    // the PACKAGE gets to say "nothing to check here" — it is the side making
    // demands, and a package that names no vendor is not asking about vendors.
    // A package that DOES name one and meets a device with none is a failure,
    // not a shrug: the unit cannot be the one this was built for, and treating a
    // blank device as "unknown, carry on" meant a package for a customer's fleet
    // would install on any unbadged board that happened to be plugged in.
    auto addStringRule = [&verdict](const QString &name, const QString &wanted,
                                    const QString &reported) {
        UploadRule rule;
        rule.name = name;
        rule.expected = orUnset(wanted);
        rule.actual = orUnset(reported);
        if (wanted.isEmpty()) {
            rule.status = UploadRule::NotChecked;
        } else if (reported.isEmpty()) {
            rule.status = UploadRule::Fail;
            rule.message = QObject::tr("%1 incorrect — this package is for \"%2\", and the "
                                       "device has no %1 programmed into its firmware.")
                               .arg(name, wanted);
        } else if (wanted == reported) {
            rule.status = UploadRule::Pass;
        } else {
            rule.status = UploadRule::Fail;
            rule.message = QObject::tr("%1 incorrect — this package is for \"%2\", the device "
                                       "reports \"%3\".")
                               .arg(name, wanted, reported);
        }
        verdict.rules.append(rule);
    };
    addStringRule(tr("Vendor ID"), pkg.vendorId, dev.vendorId);
    addStringRule(tr("Model ID"), pkg.modelId, dev.modelId);

    // There was a "Series" rule here, comparing an opaque 32-bit id meant to say
    // which configuration line a unit belonged to WITHIN a model. It was removed
    // as redundant rather than renamed: Model ID is a sixteen-character string
    // set by the same build flag at the same moment, so a config line is better
    // told apart by calling it "CAN Triple TD" than by a hex number nobody can
    // read off a screen. It also sat directly above "Serial Number" in this very
    // table, one letter away from it, which is the last thing a rule list wants.
    //
    // The cost, so it is a decision and not an accident: two configuration lines
    // sharing an identical Model ID can no longer be distinguished. Give them
    // distinct model names.

    {
        // A serial pin is the most explicit statement a package can make about
        // where it belongs — "this one car, not the other nine" — so it is not
        // overridable. An operator who pinned the wrong serial fixes the
        // package; they do not click past it on the customer's bench.
        //
        // The allow-list is summarised rather than printed when it holds more
        // than one entry. Naming every serial would show one customer's fleet
        // to whoever is holding a single unit out of it, and the count is what
        // makes the verdict understandable anyway.
        UploadRule rule;
        rule.name = tr("Serial Number");
        rule.actual = QString::number(dev.serialNumber);
        if (!policy.pinsSerial()) {
            rule.expected = tr("any unit in the fleet");
            rule.status = UploadRule::NotChecked;
        } else {
            rule.expected = policy.allowedSerials.size() == 1
                                ? QString::number(policy.allowedSerials.first())
                                : tr("one of %n listed serial number(s)", nullptr,
                                     int(policy.allowedSerials.size()));
            if (policy.allowsSerial(dev.serialNumber)) {
                rule.status = UploadRule::Pass;
            } else {
                rule.status = UploadRule::Fail;
                // The count, never the list — see the note above about showing
                // one customer's fleet to whoever holds one unit of it.
                rule.message =
                    policy.allowedSerials.size() == 1
                        ? tr("Serial number incorrect — this package is only for unit %1, and "
                             "this device is unit %2.")
                              .arg(policy.allowedSerials.first())
                              .arg(dev.serialNumber)
                        : tr("Serial number incorrect — unit %1 is not one of the %n unit(s) "
                             "this package is for.",
                             nullptr, int(policy.allowedSerials.size()))
                              .arg(dev.serialNumber);
            }
        }
        verdict.rules.append(rule);
    }

    {
        // The attestation, and the only rule here worth anything against
        // somebody deliberately trying it on: the four strings above are all
        // readable off any unit in the fleet, so a look-alike can echo them
        // back. Answering a challenge needs the key.
        //
        // It is skipped when the package has no key, and that is a real limit
        // rather than an oversight: a plain .ct3 never carries one, so
        // requireFleetKey buys nothing at all unless the package was
        // distributed as a .ct3s. The dialog says "Not checked" rather than
        // implying an attestation happened.
        UploadRule rule;
        rule.name = tr("Fleet Key");
        const QString deviceKeyState = state.keyPresent ? tr("holds a key") : tr("no key");
        if (!policy.requireFleetKey) {
            rule.expected = tr("proof not required");
            rule.actual = deviceKeyState;
            rule.status = UploadRule::NotChecked;
        } else if (pkg.fleetKey == kNoAccessKey) {
            rule.expected = tr("this package carries no key");
            rule.actual = deviceKeyState;
            rule.status = UploadRule::NotChecked;
        } else {
            rule.expected = tr("proof of the fleet key");
            if (!state.keyPresent) {
                // The device is not merely unable to answer, it has nothing to
                // answer with — so it cannot be the unit this package is for.
                rule.actual = tr("no key programmed");
                rule.status = UploadRule::Fail;
                rule.message = tr("Fleet key incorrect — the device holds no fleet key, so it "
                                  "cannot prove it belongs to this package's fleet.");
            } else {
                bool mismatch = false;
                QString proveError;
                if (device_session::proveFleetIdentity(link, pkg.fleetKey, &proveError,
                                                       &mismatch)) {
                    rule.actual = tr("proved");
                    rule.status = UploadRule::Pass;
                } else {
                    // A wrong answer and a lost link are different facts, and
                    // reporting one as the other would have an installer
                    // returning good hardware.
                    rule.actual = mismatch ? tr("answered incorrectly")
                                           : tr("did not answer the challenge");
                    rule.status = UploadRule::Fail;
                    rule.message =
                        mismatch
                            ? tr("Fleet key incorrect — the device holds a fleet key, but not "
                                 "this package's. It belongs to a different fleet.")
                            : tr("Fleet key could not be checked — the device did not answer "
                                 "the challenge. %1")
                                  .arg(proveError);
                }
            }
        }
        verdict.rules.append(rule);
    }

    {
        // The only rule that WARNS rather than refuses, and the only one that
        // can, because version order is the only thing here that a knowing
        // operator legitimately goes against. Reinstalling the same revision is
        // routine — a replaced unit, a reload after a clear — and rolling back
        // to an older one is what you do when the newer one turned out worse.
        // Neither is an error. Only going backwards is worth remarking on, and
        // remarking is all it does: the upload proceeds either way.
        UploadRule rule;
        rule.name = tr("Config Version");
        rule.actual = QString::number(dev.configVersion);
        if (!policy.warnOnOlderVersion) {
            rule.expected = tr("any version");
            rule.status = UploadRule::NotChecked;
        } else if (pkg.configVersion == 0) {
            // An unversioned package makes no claim about ordering, so there is
            // nothing to compare and the rule sits out — exactly as it does for
            // an unset vendor or model.
            //
            // Treating 0 as a real version instead meant every Send of a
            // configuration nobody had numbered asked "expects 0 (must be
            // newer) and the device reports 0 — send anyway?", which is a
            // question with no useful answer. A prompt that always appears and
            // is always waved through is worse than no prompt: it teaches people
            // to click past the one that eventually matters.
            rule.expected = tr("not versioned");
            rule.status = UploadRule::NotChecked;
        } else {
            rule.expected = tr("version %1").arg(pkg.configVersion);
            // Equal counts as fine. Reinstalling the revision a unit already
            // runs is not going backwards, and it is the single most common
            // thing anyone does with an update package — flagging it would put
            // a warning on the ordinary case and teach people to ignore the row.
            if (pkg.configVersion >= dev.configVersion) {
                rule.status = UploadRule::Pass;
            } else {
                rule.status = UploadRule::Warn;
                rule.message = tr("Config version — this package is version %1 and the device "
                                  "already runs %2. Installing it moves the device backwards.")
                                   .arg(pkg.configVersion)
                                   .arg(dev.configVersion);
            }
        }
        verdict.rules.append(rule);
    }

    return verdict;
}

// ---------------------------------------------------------------- the dialog

UploadDialog::UploadDialog(DeviceLink *link, Configuration *config, QWidget *parent)
    : QDialog(parent)
    , m_link(link)
    , m_config(config)
    , m_packageOpened(false)
    , m_packageEdit(nullptr)
    , m_openButton(nullptr)
    , m_ruleTree(nullptr)
    , m_summaryLabel(nullptr)
    , m_uploadButton(nullptr)
    , m_buttons(nullptr)
{
    setWindowTitle(tr("Upload Configuration"));
    setModal(true);

    auto *layout = new QVBoxLayout(this);

    // Which file is about to be installed, spelled out in full. An installer
    // sent three packages for three products needs to see which one is loaded
    // before they read a word of the verdict below it.
    auto *packageRow = new QHBoxLayout;
    packageRow->addWidget(new QLabel(tr("Package :"), this));
    m_packageEdit = new QLineEdit(this);
    m_packageEdit->setReadOnly(true);
    packageRow->addWidget(m_packageEdit, 1);
    m_openButton = new QPushButton(tr("Open Package…"), this);
    packageRow->addWidget(m_openButton);
    layout->addLayout(packageRow);

    m_ruleTree = new QTreeWidget(this);
    m_ruleTree->setColumnCount(4);
    m_ruleTree->setHeaderLabels(
        {tr("Rule"), tr("Package wants"), tr("Device says"), tr("Result")});
    m_ruleTree->setRootIsDecorated(false);
    m_ruleTree->setUniformRowHeights(true);
    m_ruleTree->setTextElideMode(Qt::ElideRight);
    // Nothing in the table is actionable, so it takes neither selection nor
    // focus: a highlighted row would suggest a row can be operated on, and
    // there is nothing to operate.
    m_ruleTree->setSelectionMode(QAbstractItemView::NoSelection);
    m_ruleTree->setFocusPolicy(Qt::NoFocus);
    m_ruleTree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_ruleTree->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_ruleTree->header()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_ruleTree->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    layout->addWidget(m_ruleTree, 1);

    m_summaryLabel = new QLabel(this);
    m_summaryLabel->setWordWrap(true);
    m_summaryLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_summaryLabel);

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    m_uploadButton = new QPushButton(tr("Upload"), this);
    // ActionRole, not AcceptRole: the dialog stays open after an upload,
    // because installing a second unit is the normal next thing to do and a
    // dialog that vanished would make it a fresh trip through the menu.
    m_buttons->addButton(m_uploadButton, QDialogButtonBox::ActionRole);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_uploadButton, &QPushButton::clicked, this, &UploadDialog::onUpload);
    connect(m_openButton, &QPushButton::clicked, this, &UploadDialog::onOpenPackage);

    // No button answers the Return key. A dialog where a stray Return can write
    // a customer's device is a dialog that will eventually write one.
    const QList<QAbstractButton *> boxButtons = m_buttons->buttons();
    for (QAbstractButton *button : boxButtons) {
        if (auto *push = qobject_cast<QPushButton *>(button)) {
            push->setAutoDefault(false);
            push->setDefault(false);
        }
    }
    m_openButton->setAutoDefault(false);
    m_openButton->setDefault(false);
    layout->addWidget(m_buttons);

    refresh();
    resize(760, 380);
}

void UploadDialog::refresh()
{
    // Only a package opened through the button beside this field is named here,
    // even when the application already has a file open. See m_packageOpened.
    m_packageEdit->setText(m_packageOpened && m_config
                               ? QDir::toNativeSeparators(m_config->filePath())
                               : tr("(none — click Open Package…)"));

    m_verdict = UploadVerdict{};
    if (!m_config) {
        m_verdict.problem = tr("There is no configuration loaded to upload.");
    } else if (m_packageOpened) {
        // Always re-read. The dialog's whole claim is that it describes the unit
        // on the end of the cable, and the cheapest way to be right about that
        // is to ask it again.
        //
        // With no package open the device is not read at all: every rule would
        // compare against a document nobody chose, and a table of answers to
        // questions that were never asked is worse than an empty one.
        BusyScope busy(this);
        m_verdict = evaluate(m_link, *m_config);
    }

    const QColor tick = tickColour(palette());
    const QColor bad = problemColour(palette());
    const QColor warn = warningColour(palette());
    const QColor greyed = palette().color(QPalette::Disabled, QPalette::Text);

    m_ruleTree->clear();
    const QList<UploadRule> &rules = m_verdict.rules;
    for (const UploadRule &rule : rules) {
        auto *item = new QTreeWidgetItem(m_ruleTree);
        item->setText(0, rule.name);
        item->setText(1, rule.expected);
        item->setText(2, rule.actual);
        switch (rule.status) {
        case UploadRule::Pass:
            item->setText(3, QStringLiteral("✓ ") + tr("Pass"));
            item->setForeground(3, tick);
            break;
        case UploadRule::Warn:
            // Its own glyph and its own colour, because a reader scanning the
            // verdict column has to be able to tell "this stops the install"
            // from "this is worth knowing" without reading a word of it.
            item->setText(3, QStringLiteral("⚠ ") + tr("Warning"));
            item->setForeground(3, warn);
            break;
        case UploadRule::Fail:
            item->setText(3, QStringLiteral("✗ ") + tr("Fail"));
            item->setForeground(3, bad);
            break;
        case UploadRule::NotChecked:
            item->setText(3, QStringLiteral("– ") + tr("Not checked"));
            // The whole row greys out, not just the verdict. A rule with
            // nothing to compare has nothing to say in any of its columns, and
            // leaving them at full contrast invites them to be read as facts
            // that were tested.
            for (int column = 0; column < 4; ++column)
                item->setForeground(column, greyed);
            break;
        }
    }

    const QStringList failures = m_verdict.failureSummaries();
    const QStringList warnings = m_verdict.warningSummaries();
    QString text;
    QColor colour = bad;
    bool canUpload = false;

    if (!m_config) {
        // Before the package question, because with no document at all there is
        // nowhere to open a package INTO, and telling the user to click a button
        // that cannot work is worse than saying what is actually wrong.
        text = m_verdict.problem;
    } else if (!m_packageOpened) {
        // The state this dialog opens in, and the one that used to read as an
        // all-clear: no file chosen, five rules that never ran, a green summary
        // and a live Upload button. Nothing has been checked because there is
        // nothing to check, and saying exactly that is the whole of what this
        // screen can honestly report until a package is chosen.
        text = tr("No package is open, so nothing has been checked and nothing can be "
                  "uploaded. Click Open Package… and choose the configuration file you were "
                  "sent — whatever happens to be open in the application is not a package "
                  "and will not be installed from here.");
    } else if (!m_verdict.deviceReadable) {
        text = m_verdict.problem;
    } else if (m_verdict.hasBlockingFailure()) {
        // One sentence per failed rule rather than "3 checks failed": collapsing
        // them would send the reader back to the table to learn what the
        // summary was for.
        text = tr("%1\n\nThis package was not built for this device, so it cannot be uploaded "
                  "to it.")
                   .arg(failures.join(QLatin1Char('\n')));
    } else if (m_verdict.allPassed()) {
        int notChecked = 0;
        for (const UploadRule &rule : rules) {
            if (rule.status == UploadRule::NotChecked)
                ++notChecked;
        }
        // Says how much was actually established, not how much was asked. "All
        // clear" over four rules that never ran would be the single most
        // misleading sentence this dialog could print.
        text = notChecked == 0
                   ? tr("This package is for this device: every rule passed.")
                   : tr("Nothing refuses this package on this device. %n rule(s) had nothing "
                        "to compare, because the package does not pin them.",
                        nullptr, notChecked);
        colour = tick;
        canUpload = true;
    } else if (!warnings.isEmpty()) {
        // Warnings only. Upload is enabled and asks nothing: the operator was
        // told, and being told is the entire intent. A confirmation here would
        // make going back to a known-good older configuration — which is a
        // deliberate, ordinary act — feel like an error being forced through.
        text = tr("%1\n\nThis package can still be uploaded.")
                   .arg(warnings.join(QLatin1Char('\n')));
        colour = warn;
        canUpload = true;
    } else {
        // Nothing failed, nothing warned, and nothing passed either: a real
        // package that pins none of the five things. It installs — a plain
        // .ct3 has always been allowed to — but it gets the warning colour and
        // not the tick, because the operator is the only check left and ought
        // to know it. Silence from a rule that never ran is not agreement.
        text = tr("Nothing objects to this package, but nothing was checked either: it names "
                  "no vendor, model, serial number, fleet key or version, so every rule above "
                  "sat out. It can be uploaded — this dialog simply cannot tell you it belongs "
                  "on this device.");
        colour = warn;
        canUpload = true;
    }

    m_summaryLabel->setText(text);
    m_summaryLabel->setStyleSheet(colourRule(colour));
    m_uploadButton->setEnabled(canUpload);
}

// Opening a package from inside the uploader, so the person doing an install
// never has to know that File > Open exists.
//
// This repeats MainWindow::openPath — peek, prompt when the file needs a
// password, load — because this dialog does not own the main window and cannot
// call its private helper. The duplication is small and deliberate; if it grows
// (recent files, format migration, anything with state behind it) the right fix
// is to lift the flow into Configuration rather than to widen the copy.
void UploadDialog::onOpenPackage()
{
    if (!m_config)
        return;

    const QString title = tr("Open Package");

    // The document this dialog checks is the application's own, so opening a
    // package here replaces whatever is loaded — including work in progress.
    // maybeSave() belongs to the main window and cannot be reached from here,
    // so the least this can do is say plainly what is about to be lost.
    if (m_config->isDirty()
        && QMessageBox::question(this, title,
                                 tr("The open configuration has unsaved changes, and opening a "
                                    "package replaces it.\n\nDiscard those changes?"),
                                 QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
               != QMessageBox::Yes) {
        return;
    }

    const QString path = QFileDialog::getOpenFileName(
        this, title, QString(),
        tr("CAN Triple Configurations (*.ct3 *.ct3s);;All Files (*)"));
    if (path.isEmpty())
        return;

    // peekFile reads only the header, so the password prompt happens while the
    // current document is still untouched and cancelling leaves it as it was.
    QString error;
    Configuration::FilePeek peek;
    if (!Configuration::peekFile(path, &peek, &error)) {
        QMessageBox::warning(this, title, error);
        return;
    }

    if (peek.secure && peek.requiresPassword) {
        // The file key is wrapped under the Edit Protected Comms password, so
        // there is no reading this file at all without one. Ask, and keep
        // asking: the only other way out is Cancel, which is the right way out
        // even for a damaged file, because loadFromFile's message says which of
        // the two happened.
        for (;;) {
            bool ok = false;
            const QString password = QInputDialog::getText(
                this, title,
                tr("\"%1\" is a secure configuration and cannot be opened without its "
                   "Edit Protected Comms password.\n\nEnter the password:")
                    .arg(QFileInfo(path).fileName()),
                QLineEdit::Password, QString(), &ok);
            if (!ok)
                return;
            if (m_config->loadFromFile(path, &error, password))
                break;
            QMessageBox::warning(this, title, error);
        }
    } else if (!m_config->loadFromFile(path, &error)) {
        // A plain .ct3, or a .ct3s whose key travels inside it: either opens
        // with no password at all, so a failure here is a real failure.
        QMessageBox::warning(this, title, error);
        return;
    }

    // Only here is there a package, and only here does Upload become possible.
    // Every path above returns instead — a cancelled file dialog, a cancelled
    // password prompt, a file that would not load — and each of those has to
    // leave the dialog exactly as it was rather than arm the button with
    // whatever document happened to already be in memory.
    m_packageOpened = true;
    refresh();
}

void UploadDialog::onUpload()
{
    if (!m_config)
        return;
    // The button is disabled until a package has been opened, and this repeats
    // that check for the same reason the blocking-failure check below repeats
    // one: whether a customer's device gets overwritten with an empty document
    // is not a thing to leave resting on a button's enabled state staying
    // correct through every future edit of refresh().
    if (!m_packageOpened)
        return;
    const QString title = windowTitle();

    // Re-run the rules before writing anything. The verdict on screen may be
    // minutes old and the unit it describes may have been unplugged and
    // replaced with the next one off the bench — which is exactly the mistake
    // this dialog exists to catch, and it would be a poor showing to make it
    // here.
    refresh();
    if (!m_verdict.deviceReadable) {
        QMessageBox::warning(this, title, m_verdict.problem);
        return;
    }
    // Warnings do not stop here and do not ask anything — they were shown in the
    // table and in the summary, and that is the whole of what a warning owes
    // anyone. Only a Fail refuses, and the button is already disabled in that
    // state; the check below exists so that stops being an assumption the safety
    // of a customer's device rests on.
    if (m_verdict.hasBlockingFailure()) {
        QMessageBox::warning(this, title,
                             tr("This package was not built for this device.\n\n%1")
                                 .arg(m_verdict.failureSummaries().join(QLatin1Char('\n'))));
        return;
    }

    // The DEVICE's Send password, proved before a byte is written. Nothing here
    // asks for the other two: Get is a read gate this path never uses, and Edit
    // Protected Comms decides what an app will show a user rather than what a
    // device will accept from one.
    if (!AccessPasswordsDialog::promptAndProve(m_link, AccessFunction::SendConfiguration, this))
        return;

    const MappingResult mapped = mapWithScript(*m_config);
    if (!mapped.ok()) {
        // Send Configuration puts the mapper's error list in a details pane,
        // because the engineer reading it is the one who can fix the rows it
        // names. Here the list is withheld: those errors quote message and
        // channel names out of a package that may be locked, the person in
        // front of this dialog cannot change any of them, and printing them
        // would leak the contents of a configuration to somebody who was given
        // only the right to install it.
        QMessageBox::warning(this, title,
                             tr("This package cannot be installed on a CAN Triple (%1 problem(s) "
                                "in it). Nothing was written to the device. The package is "
                                "faulty rather than wrong for this unit, so it has to go back "
                                "to whoever built it.")
                                 .arg(mapped.errors.size()));
        return;
    }

    // Per-bus CONTROL_CAN setups, the same ones Send builds from the
    // Communications rate/mode settings. v2 firmware applies them; v1 NACKs and
    // the step is skipped.
    QVector<ControlCanPayload> busSetups;
    for (int i = 0; i < 3; ++i) {
        ControlCanPayload setup{};
        setup.bus_idx = quint8(i + 1);
        setup.mode = m_config->bus[i].enabled ? 1 : 0;
        setup.baud_rate = busRateHz(m_config->bus[i].rateKbps);
        setup.data_baud_rate = busRateHz(m_config->bus[i].dataRateKbps);
        setup.termination = m_config->bus[i].termination ? 1 : 0;
        busSetups.append(setup);
    }

    // The same transfer Send Configuration runs, with the two decisions Send
    // leaves to the operator settled here instead: it always saves to flash,
    // because an update that vanishes at the next power-off is not an update,
    // and it never binds the configuration to the chip, because locking a
    // customer's file to a customer's board is the package author's decision
    // and not the installer's.
    auto *progress = new QProgressDialog(tr("Uploading configuration…"), tr("Cancel"), 0, 100,
                                         this);
    progress->setWindowModality(Qt::WindowModal);
    progress->setMinimumDuration(0);
    auto *transfer = ConfigTransfer::send(m_link, mapped.tables, /*verify=*/true, busSetups,
                                          /*saveToFlash=*/true,
                                          m_config->fleetIdentity().configVersion,
                                          m_config->effectiveTitle(),
                                          /*resetAfter=*/false, this);
    connect(transfer, &ConfigTransfer::progress, progress,
            [progress](int done, int total, const QString &stage) {
                progress->setMaximum(total);
                progress->setValue(done);
                progress->setLabelText(stage);
            });
    connect(progress, &QProgressDialog::canceled, transfer, &ConfigTransfer::cancel);
    connect(transfer, &ConfigTransfer::finished, this,
            [this, progress, transfer, title](bool ok, const QString &error) {
                progress->close();
                progress->deleteLater();
                if (!ok) {
                    QMessageBox::warning(this, title, error);
                    // Still re-check: a transfer that died half way leaves the
                    // device in a state the table should describe rather than
                    // one it described before the attempt.
                    refresh();
                    return;
                }

                QString text = transfer->flashSaveWasSkipped()
                                   ? tr("Uploaded and verified.\n\nThis firmware is too old to "
                                        "save to flash, so the configuration lives in device RAM "
                                        "and is lost at power-off.")
                                   : tr("Uploaded, verified, and saved to flash.\n\nIt reloads "
                                        "automatically at every power-up.");
                if (!transfer->skippedStages().isEmpty())
                    text += tr("\n\nSkipped (not accepted by this firmware):\n%1")
                                .arg(transfer->skippedStages().join(QStringLiteral("\n")));
                QMessageBox::information(this, title, text);

                // Re-read the unit so the table shows what it now holds. The
                // version rule will normally fail after a successful upload,
                // which is the correct and useful thing for it to say: this
                // package is already on this device.
                refresh();
            });
}

} // namespace ct
