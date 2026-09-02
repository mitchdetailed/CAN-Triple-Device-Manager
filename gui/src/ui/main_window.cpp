#include "main_window.h"

#include <QApplication>
#include <QCoreApplication> // applicationVersion(), for the title bar
#include <QClipboard>
#include <QCheckBox>
#include <QCloseEvent>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QDir>
#include <QFileInfo>
#include <QGroupBox>
#include <QInputDialog>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressDialog>
#include <QPushButton>
#include <QSettings>
#include <QStatusBar>
#include <QVBoxLayout>

#include <cstring>

#include "../model/access_keys.h"
#include "../model/device_mapper.h"
#include "../model/secure_file.h"
#include "../model/user_paths.h"
#include "../model/validation.h"
#include "../protocol/config_transfer.h"
#include "../protocol/device_session.h"
#include "../protocol/firmware_update.h" // FwUpdateStatus — the running store version
#include "access_passwords_dialog.h"
#include "can_viewer_dialog.h"
#include "channel_editor_dialog.h"
#include "check_channels_dialog.h"
#include "config_summary_dialog.h"
#include "communications_dialog.h"
#include "conditions_dialog.h"
#include "constants_dialog.h"
#include "tables_dialog.h"
#include "counters_dialog.h"
#include "timers_dialog.h"
#include "integrators_dialog.h"
#include "connection_settings_dialog.h"
#include "firmware_update_dialog.h"
#include "firmware_license_dialog.h"
#include "secure_builder_dialog.h"
#include "help_window.h"
#include "lua_console_dialog.h"
#include "math_dialog.h"
#include "monitor_channels_dialog.h"
#include "script_editor_dialog.h"
#include "../scripting/script_compiler.h"   // mapWithScript

namespace ct {

namespace {
constexpr int kMaxRecentFiles = 8;
const char *kRecentFilesKey = "recentFiles";
// Open and Save As offer both formats, because the magic decides which reader
// runs and a user who typed ".ct3s" into Save As should still find their file
// in the Open dialog afterwards.
const char *kFileFilter =
    "CAN Triple Configurations (*.ct3 *.ct3s *.json);;All Files (*)";

// SAVE AS offers the three formats separately, because they are three different
// answers to "who can read this file" and a combined filter would hide that.
// Order is the default first: Qt preselects filter one, so the sealed .ct3 is
// what you get by pressing Save without touching the dropdown.
//
// JSON is last and says what it is in the entry itself. Somebody choosing it
// from a list should not have to already know that it is the legible one — that
// IS the choice being made.
const char *kSaveAsFilter =
    "CAN Triple Configuration (*.ct3);;"
    "CAN Triple Secure Configuration (*.ct3s);;"
    "JSON — readable, not encrypted (*.json)";
// Save Secure Config only ever writes the binary container, so it lists only
// that — offering *.ct3 there would suggest the format followed the extension,
// which it does not.
const char *kSecureFileFilter = "CAN Triple Secure Configurations (*.ct3s);;All Files (*)";

// WHERE A FILE DIALOG STARTS. Every one of these used to pass {} and let Qt
// decide, which in practice means "wherever the last file dialog in this
// process happened to be" — so opening a DBC, or saving an .asc log, moved
// where Save Configuration would next offer to write. That is the kind of thing
// that puts a configuration in a folder nobody meant.
//
// So: the document's OWN folder when it has one, because a Save As on an open
// file belongs beside it, and the program's Configurations folder otherwise.
//
// Only ever a STARTING POINT. It is not enforced and it is not a refusal: the
// user may browse anywhere from there, and a configuration saved somewhere else
// entirely is a perfectly ordinary thing to do. So this deliberately does not
// probe writability the way saving a template does — an unwritable default
// should quietly leave the user in the dialog, free to pick another folder,
// rather than stop them with a message before they have chosen anything.
QString startIn(const QString &currentFile)
{
    if (!currentFile.isEmpty())
        return QFileInfo(currentFile).absolutePath();
    const QString dir = ct::configurationsDirectory();
    QDir().mkpath(dir); // best effort; the dialog copes with an absent folder
    return dir;
}

// Blocks interaction while a synchronous device command runs its nested
// event loop, so the user can't re-enter menus mid-command.
class BusyScope
{
public:
    explicit BusyScope(QWidget *widget)
        : m_widget(widget)
    {
        m_widget->setEnabled(false);
        QApplication::setOverrideCursor(Qt::WaitCursor);
    }
    ~BusyScope()
    {
        QApplication::restoreOverrideCursor();
        m_widget->setEnabled(true);
    }

private:
    QWidget *m_widget;
};
} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    buildMenus();
    buildCentral();

    statusBar();
    m_documentLabel = new QLabel;
    statusBar()->addWidget(m_documentLabel);
    // Says whether this session can see the protected messages, so "why does
    // this message show no detail?" is answerable without hunting through menus.
    // Hidden entirely when the document has no comms password, because then
    // there is nothing being withheld from anyone.
    m_lockLabel = new QLabel;
    m_lockLabel->setVisible(false);
    statusBar()->addPermanentWidget(m_lockLabel);
    m_connectionLabel = new QLabel;
    statusBar()->addPermanentWidget(m_connectionLabel);

    connect(&m_config, &Configuration::dirtyChanged, this, [this]() { updateWindowTitle(); });
    connect(&m_config, &Configuration::documentReset, this, [this]() {
        updateWindowTitle();
        updateProtectionState(); // New/Open/Get changes which document is protected
    });
    connect(&m_link, &DeviceLink::connected, this, &MainWindow::updateConnectionStatus);
    connect(&m_link, &DeviceLink::disconnected, this, &MainWindow::updateConnectionStatus);
    connect(&m_link, &DeviceLink::logMessage, this, [this](const QString &text) {
        statusBar()->showMessage(tr("Device: %1").arg(text.trimmed()), 4000);
    });

    updateWindowTitle();
    updateConnectionStatus();
}

void MainWindow::buildMenus()
{
    // File
    QMenu *fileMenu = menuBar()->addMenu(tr("&File"));
    fileMenu->addAction(tr("&New"), QKeySequence::New, this, &MainWindow::onNew);
    fileMenu->addAction(tr("&Open…"), QKeySequence::Open, this, &MainWindow::onOpen);
    fileMenu->addAction(tr("&Save"), QKeySequence::Save, this, &MainWindow::onSave);
    fileMenu->addAction(tr("Save &As…"), QKeySequence::SaveAs, this, &MainWindow::onSaveAs);
    // Replaces Save Secure Config. That command saved the open document in a
    // second format; this one takes a configuration as an input, attaches an
    // install policy to it, and produces a deployable package — which is what
    // a .ct3s was always for and what the old name never said.
    fileMenu->addAction(tr("Secure Configuration &Builder…"), this,
                        &MainWindow::onSecureBuilder);
    fileMenu->addSeparator();
    fileMenu->addAction(tr("Check &Channels"), this, &MainWindow::onCheckChannels);
    fileMenu->addAction(tr("Config S&ummary…"), this, &MainWindow::onConfigSummary);
    fileMenu->addSeparator();
    m_revealAction = fileMenu->addAction(tr("&Reveal Protected Comms…"), this,
                                         &MainWindow::onRevealProtectedComms);
    // Conceal has no state to gather and nothing to fail, so it is a lambda
    // rather than another slot on the window.
    m_concealAction = fileMenu->addAction(tr("Conceal Protected Co&mms"), this, [this]() {
        m_config.concealProtectedComms();
        updateProtectionState();
    });
    fileMenu->addSeparator();
    m_recentMenu = fileMenu->addMenu(tr("Recent Files"));
    fileMenu->addSeparator();
    fileMenu->addAction(tr("E&xit"), QKeySequence(Qt::CTRL | Qt::Key_Q), this, &QWidget::close);
    updateRecentMenu();

    // Connections
    QMenu *connectionsMenu = menuBar()->addMenu(tr("&Connections"));
    connectionsMenu->addAction(tr("&Communications…"), this, &MainWindow::onCommunications);

    // Calculations
    QMenu *calcMenu = menuBar()->addMenu(tr("C&alculations"));
    calcMenu->addAction(tr("&Math Channels…"), this, &MainWindow::onMathChannels);
    // Mnemonic moved o -> C with the rename: "Up / Down Counters" already owns U
    // and "Constants" owns n, so C is the letter left in "User Conditions".
    calcMenu->addAction(tr("User &Conditions…"), this, &MainWindow::onConditions);
    calcMenu->addAction(tr("&Timers…"), this, &MainWindow::onTimers);
    calcMenu->addAction(tr("&Up / Down Counters…"), this, &MainWindow::onCounters);
    calcMenu->addAction(tr("&Integrators…"), this, &MainWindow::onIntegrators);
    calcMenu->addAction(tr("Co&nstants…"), this, &MainWindow::onConstants);
    calcMenu->addAction(tr("Ta&bles…"), this, &MainWindow::onTables);
    calcMenu->addSeparator();
    // Below the separator because it is a different kind of thing from the rows
    // above: those are grids the device evaluates, this is code it runs. It is
    // in Calculations rather than Tools because it computes channel values like
    // everything else here — the Lua Console next door edits the DOCUMENT and
    // never reaches a device, which is the opposite direction entirely.
    calcMenu->addAction(tr("&Device Script…"), this, &MainWindow::onDeviceScript);

    // Online
    QMenu *onlineMenu = menuBar()->addMenu(tr("&Online"));
    // The link's own verbs head the menu: everything below them talks to the
    // device, so the first question — are we talking at all? — is answered
    // first. Connect retries this session's last successful port before it
    // resorts to the settings dialog (reconnecting after a deliberate
    // Disconnect or a re-seated cable is the common case, and it should not
    // cost a dialog); the FIRST connect of a session always goes through
    // Connection Settings, because no port choice survives a restart by
    // design. The pair's enabled states track the link in
    // updateConnectionStatus().
    m_connectAction = onlineMenu->addAction(tr("&Connect"), this, &MainWindow::onConnect);
    m_disconnectAction = onlineMenu->addAction(tr("&Disconnect"), this, &MainWindow::onDisconnect);
    m_connectAction->setEnabled(!m_link.isOpen());
    m_disconnectAction->setEnabled(m_link.isOpen());
    onlineMenu->addSeparator();
    onlineMenu->addAction(tr("&Send Configuration"), QKeySequence(Qt::Key_F5), this,
                          &MainWindow::onSendConfiguration);
    // Directly under Send, because it is the same verb with a different subject:
    // that one sends what is open, this one sends a sealed file without opening
    // it. Keeping them adjacent is what makes the distinction findable.
    onlineMenu->addAction(tr("Send Sec&ure Configuration…"), this,
                          &MainWindow::onSendSecureConfiguration);
    onlineMenu->addAction(tr("&Get Configuration"), this, &MainWindow::onGetConfiguration);
    onlineMenu->addSeparator();
    onlineMenu->addAction(tr("&Monitor Channels…"), QKeySequence(Qt::Key_F3), this,
                          &MainWindow::onMonitorChannels);
    // Mnemonic on the "w" rather than "V", which is free again now that Verify
    // Configuration is gone — kept as it is because a shortcut people have
    // learned is not worth churning for a tidier letter.
    onlineMenu->addAction(tr("CAN Vie&wer…"), QKeySequence(Qt::Key_F4), this,
                          &MainWindow::onCanViewer);
    onlineMenu->addSeparator();
    // Send Configuration also saves to flash; a separate save step isn't
    // needed. Nor a separate LOAD: the config runs FROM flash (single-copy
    // model), so the running and stored configuration cannot differ, and the
    // "reload the stored image" command whose reason to exist was undoing a
    // half-finished upload against a backup copy was retired with the wire
    // opcode (0x0B) when the backup copy went away.
    //
    // Nor a CLEAR. Wiping a unit is File > New then Send: every Send begins
    // with the same flash erase the dedicated command performed, so an empty
    // document empties the device — while KEEPING the stored access
    // passwords. The removed menu item erased their flash copy along with
    // the configuration, and a unit power-cycled after it came back with no
    // passwords at all; making the wipe a Send closes that hole and runs it
    // through the Send password like any other reconfiguration. The wire
    // command (CMD_CLEAR_CONFIG) stays: it is the first step of every Send.
    onlineMenu->addAction(tr("Reset Device"), this, &MainWindow::onResetDevice);
    onlineMenu->addSeparator();
    onlineMenu->addAction(tr("Device Status…"), this, &MainWindow::onDeviceStatus);
    // Beside Device Status because both read the unit rather than the document,
    // and apart from it because they answer opposite questions — see the note on
    // onGetDeviceInfo for why the permanent facts do not belong in a report
    // whose every other line is a snapshot.
    onlineMenu->addAction(tr("Get Device &Info…"), this, &MainWindow::onGetDeviceInfo);
    // Beside Device Status rather than with the configuration items above: this
    // replaces the firmware, not the configuration, and grouping it with Send /
    // Get would invite exactly the confusion between the two that the warning
    // about the stored-configuration format exists to prevent.
    onlineMenu->addAction(tr("Update Fi&rmware…"), this, &MainWindow::onUpdateFirmware);
    onlineMenu->addSeparator();
    // Set Access Passwords writes into the DEVICE, which is why it is here
    // rather than under File. The Firmware License Manager beside it is the
    // same kind of thing — it edits the unit, not the document — and opens
    // without a connection only because composing a licence is desk work; Apply
    // is the step that needs hardware.
    //
    // The dealer-facing installer is Send Secure Configuration, at the top of
    // this menu with the other send commands: it is the one item here that will
    // refuse to program a device, on the policy sealed into the package.
    onlineMenu->addAction(tr("Set Access &Passwords…"), this, &MainWindow::onSetAccessPasswords);
    onlineMenu->addAction(tr("&Firmware License Manager…"), this,
                          &MainWindow::onFirmwareLicense);

    // Tools
    QMenu *toolsMenu = menuBar()->addMenu(tr("&Tools"));
    toolsMenu->addAction(tr("Channel &Editor…"), this, &MainWindow::onChannelEditor);
    // Beside the Channel Editor because they answer the same need at different
    // scales: the editor changes channels one at a time, the console changes
    // four hundred of them in a loop.
    toolsMenu->addAction(tr("&Lua Console…"), this, &MainWindow::onLuaConsole);
    toolsMenu->addSeparator();
    // Serial port settings belong to the app, not the document.
    toolsMenu->addAction(tr("Connection &Settings…"), this, &MainWindow::onConnectionSettings);

    // Help
    QMenu *helpMenu = menuBar()->addMenu(tr("&Help"));
    helpMenu->addAction(tr("&Contents…"), QKeySequence::HelpContents, this,
                        &MainWindow::onHelpContents);
    helpMenu->addSeparator();
    helpMenu->addAction(tr("&About…"), this, &MainWindow::onAbout);

    updateProtectionState();
}

// What replaced the old document-wide lock, and why it does so much less.
//
// Until v19 one password protected the whole configuration, and this function's
// ancestor disabled every menu item that could change anything: Communications,
// all of Calculations, the Channel Editor, Save, Get. That was the right shape
// for "this document is read-only" and the wrong shape for what people actually
// ship. The thing worth protecting is a handful of messages whose CAN protocol
// is proprietary — not the customer's own math channels, not their transmit
// messages, not the names they give things.
//
// So protection is now per MESSAGE, and since 2.3.0 it is one ordered TIER per
// message rather than a flag (see CommsSection::protection). A marked section
// refuses to be edited at every tier, and from Hidden upward it also withholds
// its detail; the channels it carries follow it in both respects. Everything
// else stays fully editable, because a customer must be able to build their own
// work around a supplier's message without holding anyone's password. Nothing
// here disables a menu wholesale, and that is deliberate rather than unfinished:
// the suppression lives at each site that would otherwise show or change
// protected detail.
//
// All that is left for this function is the pair of File items that toggle the
// session's view, and the status line that says which way it currently sits.
//
// Reveal / Conceal govern HIDDEN AND PROTECTED ONLY, and the wording has to say
// so. Read Only conceals nothing from anybody — that is the whole difference
// between it and Hidden — so a status line reading "concealed" over a document
// whose only marked message is Read Only would be describing something that is
// on screen in full.
void MainWindow::updateProtectionState()
{
    const bool hasPassword = m_config.hasCommsPassword();
    const bool revealed = m_config.commsRevealed();
    // What is actually being withheld right now, asked per section — which is
    // not the same question as "does this document have a password". A document
    // whose sections are all None or Read Only withholds nothing whatever the
    // password state, and a section unlocked with its own password this session
    // is out of the count too.
    const bool anyConcealed = m_config.anySectionConcealed();

    if (m_revealAction) {
        m_revealAction->setEnabled(hasPassword && !revealed);
        m_revealAction->setToolTip(
            // This tooltip has overclaimed twice, each time by one step, and the
            // second overclaim is the one 2.3.2 removed. It used to promise that
            // entering this password SHOWS the Protect Communication messages.
            // It does not, at any tier: every marked message is opened by its own
            // Message Password, per message, in Communications Setup — and for
            // Protect Communication by that password AND a connected device
            // confirming this one. A password that opens no message must not
            // offer to, or the user enters it, sees nothing change, and concludes
            // the feature is broken.
            hasPassword ? tr("Enter the Protected Comms password to prove it for this "
                             "session — it is one half of what a Protect Communication message "
                             "asks for, and it is what lets the password itself be changed. It "
                             "does not open any message on its own: every marked message is "
                             "unlocked individually in Communications Setup, using the Message "
                             "Password that message was given. Read Only messages are shown in "
                             "full either way.")
                        : tr("This configuration has no Protected Comms password, so "
                             "there is nothing to enter here. Marked messages are unlocked "
                             "individually in Communications Setup, using the Message Password "
                             "each one was given."));
    }
    if (m_concealAction) {
        m_concealAction->setEnabled(hasPassword && revealed);
        // This one is the half that does reach every section, and it is why the
        // pair is still worth having: concealing forgets every per-section
        // password given this session, so it re-locks the whole document at once.
        m_concealAction->setToolTip(tr("Withhold the Hidden and Protected messages again, and "
                                       "forget every Message Password given this session."));
    }

    if (m_lockLabel) {
        m_lockLabel->setText(anyConcealed ? tr("Hidden comms — concealed")
                                          : tr("Hidden comms — revealed"));
        // Shown when there is either something being withheld or a password that
        // could withhold it. A document with a password and nothing above Read
        // Only still reads "revealed", which is true and is the only honest
        // thing to put there.
        m_lockLabel->setVisible(hasPassword || anyConcealed);
    }
}

void MainWindow::buildCentral()
{
    auto *central = new QWidget;
    auto *layout = new QVBoxLayout(central);
    layout->addStretch();
    auto *title = new QLabel(QStringLiteral("<div align='center'>"
                                            "<h1>CAN Triple</h1>"
                                            "<h2>DEVICE MANAGER</h2></div>"));
    title->setAlignment(Qt::AlignCenter);
    layout->addWidget(title);
    auto *hint = new QLabel(tr("Connections → Communications to define messages and channels.\n"
                               "Online → Send Configuration (F5) to program the device.\n"
                               "Online → Monitor Channels (F3) for live values."));
    hint->setAlignment(Qt::AlignCenter);
    hint->setStyleSheet(QStringLiteral("color: gray;"));
    layout->addWidget(hint);
    layout->addStretch();
    setCentralWidget(central);
}

void MainWindow::updateWindowTitle()
{
    // The version rides in the title bar, because it is the one place a user
    // can read which build they are running without opening a dialog — and
    // "which version is that on?" is the first question about any behaviour
    // worth reporting. Help > About still carries the full detail.
    //
    // From QCoreApplication::applicationVersion() rather than CT_APP_VERSION
    // directly: main() sets it from that macro, which CMake sets from
    // project(), so the number keeps ONE origin and this file needs no compile
    // definition of its own. Empty only where main() never ran (a harness
    // constructing the window directly), and the name then stands alone
    // instead of trailing a stray space.
    const QString version = QCoreApplication::applicationVersion();
    const QString product = version.isEmpty()
                                ? tr("CAN Triple Device Manager")
                                : tr("CAN Triple Device Manager %1").arg(version);
    setWindowTitle(tr("%1 - %2%3%4")
                       .arg(product, m_config.displayName(),
                            m_config.isDirty() ? QStringLiteral(" *") : QString(),
                            m_config.hasCommsPassword() ? tr(" [protected]") : QString()));
    if (m_documentLabel)
        m_documentLabel->setText(m_config.filePath().isEmpty() ? tr("Unsaved configuration")
                                                               : m_config.filePath());
}

void MainWindow::updateConnectionStatus()
{
    if (m_link.isOpen()) {
        m_connectionLabel->setText(tr("Connected: %1 @ %2").arg(m_link.portName())
                                       .arg(m_link.baudRate()));
        // Remembered for Online > Connect, for THIS session only — the port
        // is chosen fresh every run on purpose, but within a run "the port I
        // was just using" is almost always the answer.
        m_lastPort = m_link.portName();
        m_lastBaud = m_link.baudRate();
    } else {
        m_connectionLabel->setText(tr("Not connected"));
    }
    // Null-guarded: the first call happens while the window is still being
    // built, before the Online menu exists.
    if (m_connectAction)
        m_connectAction->setEnabled(!m_link.isOpen());
    if (m_disconnectAction)
        m_disconnectAction->setEnabled(m_link.isOpen());
}

bool MainWindow::ensureDeviceAccess(AccessFunction fn)
{
    QString error;
    device_session::AccessState state;
    // Both device calls in this function are synchronous, which means each one
    // spins a nested event loop while it waits. Without the window disabled
    // around them the menus and their shortcuts keep firing inside that loop:
    // F5 re-enters Send Configuration while this read is still outstanding, and
    // the two commands then interleave their frames on one link, each parsing
    // the other's replies. Every other device-touching path here already takes
    // this precaution — this one was the gap.
    //
    // The scope closes before anything is reported, for the same reason it does
    // in AccessPasswordsDialog: a message box raised over a still-disabled
    // window under a still-spinning wait cursor reads as a hung application.
    bool read = false;
    {
        BusyScope busy(this);
        read = device_session::readAccessState(&m_link, &state, &error);
    }
    if (!read) {
        QMessageBox::warning(this, tr("Device"),
                             tr("Could not read which access passwords this device has set."
                                "\n\n%1").arg(error));
        return false;
    }
    // Nothing set for this function, or firmware too old to have the notion:
    // there is nothing to prove. The three are independent, so a device that
    // guards Get says nothing about whether it guards Send.
    if (!state.supported || !state.isSet(fn))
        return true;

    // Protected Comms is the one the operator may legitimately never have
    // been told. A customer holding a .ct3s has the 4-byte key inside the file
    // and no idea what password produced it, so try what the session already
    // holds before asking for something they cannot supply. Only when there is
    // no key, or the device belongs to a different fleet and rejects it, does
    // this fall through to the prompt.
    if (fn == AccessFunction::EditProtectedComms && m_config.commsKey() != kNoAccessKey) {
        bool wrongKey = false;
        bool proved = false;
        {
            BusyScope busy(this);
            proved = device_session::proveAccess(&m_link, fn, m_config.commsKey(), &error,
                                                 &wrongKey);
        }
        if (proved)
            return true;
        if (!wrongKey) {
            QMessageBox::warning(this, tr("Device Password"), error);
            return false;
        }
    }

    AccessKey proved = kNoAccessKey;
    if (!AccessPasswordsDialog::promptAndProve(&m_link, fn, this, &proved))
        return false;
    // Remember a freshly proved comms key so the rest of the session does not
    // ask again — but never over a key the document already carries, which is
    // the one a Save Secure Config would embed.
    if (fn == AccessFunction::EditProtectedComms && m_config.commsKey() == kNoAccessKey)
        m_config.setCommsKey(proved);
    return true;
}

bool MainWindow::ensureConnected()
{
    if (m_link.isOpen())
        return true;
    ConnectionSettingsDialog dialog(&m_link, this);
    dialog.exec();
    // The dialog may well have just connected; without this the status bar
    // (and the Connect/Disconnect pair) said "Not connected" until some other
    // path happened to refresh them.
    updateConnectionStatus();
    return m_link.isOpen();
}

void MainWindow::onConnect()
{
    if (m_link.isOpen())
        return;
    // This session's last successful settings first — see the menu comment.
    // A failure here (port gone, taken by another program) is not reported,
    // it just falls through to the dialog, which shows the live port list
    // and is the better error message.
    if (!m_lastPort.isEmpty()) {
        QString error;
        if (m_link.open(m_lastPort, m_lastBaud, &error)) {
            updateConnectionStatus();
            return;
        }
    }
    ConnectionSettingsDialog dialog(&m_link, this);
    dialog.exec();
    updateConnectionStatus();
}

void MainWindow::onDisconnect()
{
    if (!m_link.isOpen())
        return;
    m_link.close();
    updateConnectionStatus();
}

// ---- File ----

bool MainWindow::maybeSave()
{
    if (!m_config.isDirty())
        return true;
    const auto answer = QMessageBox::warning(
        this, tr("CAN Triple Device Manager"),
        tr("The configuration has unsaved changes.\nDo you want to save them?"),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (answer == QMessageBox::Save)
        return onSave();
    return answer == QMessageBox::Discard;
}

void MainWindow::onNew()
{
    if (!maybeSave())
        return;
    m_config.clear();
    updateWindowTitle();
}

void MainWindow::onOpen()
{
    if (!maybeSave())
        return;
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Open Configuration"),
        startIn(m_config.filePath()), tr(kFileFilter));
    if (path.isEmpty())
        return;
    openPath(path);
}

// Opens a file, asking for the password when it needs one. peekFile reads only
// the header, so the prompt happens BEFORE the current document is disturbed —
// cancelling leaves what was open exactly as it was.
bool MainWindow::openPath(const QString &path)
{
    QString error;
    Configuration::FilePeek peek;
    if (!Configuration::peekFile(path, &peek, &error)) {
        QMessageBox::warning(this, tr("Open Configuration"), error);
        return false;
    }

    // requiresPassword on its own, deliberately not "secure && requiresPassword".
    // peekFile raises that flag for two quite different files: a .ct3s whose file
    // key is wrapped under the Protected Comms password, and a pre-v8 plain
    // .ct3 whose body is sealed under the retired Configuration Password. The
    // second is not a secure container and peekFile leaves `secure` false for it,
    // so requiring both conditions meant that file never reached this prompt —
    // and, having no other way to be handed a password, could not be opened at
    // all. The flag is the only cue either file gives, so it is the whole gate.
    if (peek.requiresPassword) {
        // Which password is wanted differs between the two, and naming the wrong
        // one is worse than naming none: someone hunting for a "Protected
        // Comms" password on a file written before that idea existed will search
        // for something nobody ever set.
        const QString prompt =
            peek.secure
                ? tr("\"%1\" is a secure configuration and cannot be opened without its "
                     "Protected Comms password.\n\nEnter the password:")
                      .arg(QFileInfo(path).fileName())
                : tr("\"%1\" was saved with the old Configuration Password and cannot be "
                     "opened without it.\n\nEnter the password:")
                      .arg(QFileInfo(path).fileName());
        // Ask, and keep asking: the only other way out of this loop is Cancel,
        // which is right even for a damaged file — loadFromFile's message says
        // which of the two happened, and a user who sees "this file is damaged"
        // three times will stop.
        for (;;) {
            bool ok = false;
            const QString password = QInputDialog::getText(
                this, tr("Open Configuration"), prompt, QLineEdit::Password, QString(), &ok);
            if (!ok)
                return false;
            if (m_config.loadFromFile(path, &error, password))
                break;
            QMessageBox::warning(this, tr("Open Configuration"), error);
        }
    } else if (!m_config.loadFromFile(path, &error)) {
        // A plain .ct3, or a .ct3s whose key travels inside it: either opens
        // with no password at all, so a failure here is a real failure.
        QMessageBox::warning(this, tr("Open Configuration"), error);
        return false;
    }

    addRecentFile(path);
    updateWindowTitle();
    updateProtectionState();
    return true;
}

void MainWindow::onRevealProtectedComms()
{
    if (!m_config.hasCommsPassword())
        return;
    for (;;) {
        bool ok = false;
        const QString password = QInputDialog::getText(
            this, tr("Reveal Protected Comms"),
            tr("Enter the Protected Comms password to see and change the messages "
               "marked \"Protect Communication\":"),
            QLineEdit::Password, QString(), &ok);
        if (!ok)
            return;
        if (m_config.revealProtectedComms(password))
            break;
        // Say only that it is wrong. Anything more — how close it was, how long
        // the real one is — is a hint, and there is no rate limit on a local
        // dialog to make hints affordable.
        QMessageBox::warning(this, tr("Reveal Protected Comms"),
                             tr("That password is not correct."));
    }
    updateProtectionState();
    if (m_monitorDialog)
        m_monitorDialog->rebuild();
}

bool MainWindow::onSave()
{
    if (m_config.filePath().isEmpty())
        return onSaveAs();

    QString error;
    bool ok = false;
    // SAVE FOLLOWS THE FORMAT THE FILE ALREADY HAS, all three of them. Changing
    // it here would be the worst possible default in either direction: the file
    // keeps its name and its place on disk, so nothing on screen would say that
    // a sealed configuration had just been rewritten legible, or that a legible
    // one the user was diffing in version control had just turned opaque.
    switch (m_config.fileFormat()) {
    case Configuration::FileFormat::Secure:
        ok = m_config.saveSecureToFile(m_config.filePath(), m_config.secureOptions(), &error);
        break;
    case Configuration::FileFormat::Json:
        ok = m_config.saveJsonToFile(m_config.filePath(), &error);
        break;
    case Configuration::FileFormat::Sealed:
        ok = m_config.saveToFile(m_config.filePath(), &error);
        break;
    }
    if (!ok) {
        QMessageBox::warning(this, tr("Save Configuration"), error);
        return false;
    }
    updateWindowTitle();
    return true;
}

bool MainWindow::onSaveAs()
{
    // The filter the user actually picked comes back here, and it — not the
    // typed extension — is what decides the format. A name typed without a
    // suffix has to become something, and the dropdown is the only place the
    // user said which.
    QString selected;
    QString path = QFileDialog::getSaveFileName(this, tr("Save Configuration"),
                                                startIn(m_config.filePath()),
                                                tr(kSaveAsFilter), &selected);
    if (path.isEmpty())
        return false;

    // An extension typed by hand OUTRANKS the dropdown, because typing
    // "setup.json" is a clearer statement of intent than leaving a combo box
    // where it happened to be. Only these three are honoured; any other suffix
    // (or none) takes the dropdown's format and keeps the name.
    const QString typed = QFileInfo(path).suffix().toLower();
    Configuration::FileFormat format = Configuration::FileFormat::Sealed;
    if (typed == QLatin1String("json")
        || (typed.isEmpty() && selected.contains(QLatin1String(".json")))) {
        format = Configuration::FileFormat::Json;
    } else if (typed == QLatin1String("ct3s")
               || (typed.isEmpty() && selected.contains(QLatin1String(".ct3s")))) {
        format = Configuration::FileFormat::Secure;
    }
    if (typed.isEmpty()) {
        path += format == Configuration::FileFormat::Json    ? QStringLiteral(".json")
                : format == Configuration::FileFormat::Secure ? QStringLiteral(".ct3s")
                                                              : QStringLiteral(".ct3");
    }

    QString error;
    bool ok = false;
    switch (format) {
    case Configuration::FileFormat::Json:
        // Chosen, never inherited. The one thing that would be wrong is for a
        // configuration to become legible without anybody asking.
        ok = m_config.saveJsonToFile(path, &error);
        break;
    case Configuration::FileFormat::Secure:
        // Save As reaches the secure writer with the options the document
        // already has, which for a document that was never secure is the
        // default: sealed, no password. Save Secure Config… is the route that
        // ASKS about a password, and it stays the way to add one.
        ok = m_config.saveSecureToFile(path, m_config.secureOptions(), &error);
        break;
    case Configuration::FileFormat::Sealed:
        ok = m_config.saveToFile(path, &error);
        break;
    }
    if (!ok) {
        QMessageBox::warning(this, tr("Save Configuration"), error);
        return false;
    }
    addRecentFile(path);
    updateWindowTitle();
    return true;
}

void MainWindow::onSecureBuilder()
{
    // Seeded with the open document's path, and only its path: the builder loads
    // its source from disk. Packaging what happens to be in the editor would let
    // an unsaved edit reach a customer's device without ever existing in a file
    // anybody could go back to.
    SecureBuilderDialog dialog(m_config.filePath(), this);
    dialog.exec();
}

void MainWindow::onCheckChannels()
{
    CheckChannelsDialog::run(&m_config, this);
}

void MainWindow::onConfigSummary()
{
    ConfigSummaryDialog dialog(&m_config, this);
    dialog.exec();
}

void MainWindow::addRecentFile(const QString &path)
{
    QSettings settings;
    QStringList recent = settings.value(QLatin1String(kRecentFilesKey)).toStringList();
    recent.removeAll(path);
    recent.prepend(path);
    while (recent.size() > kMaxRecentFiles)
        recent.removeLast();
    settings.setValue(QLatin1String(kRecentFilesKey), recent);
    updateRecentMenu();
}

void MainWindow::updateRecentMenu()
{
    m_recentMenu->clear();
    const QStringList recent = QSettings().value(QLatin1String(kRecentFilesKey)).toStringList();
    m_recentMenu->setEnabled(!recent.isEmpty());
    for (const QString &path : recent) {
        QAction *action = m_recentMenu->addAction(QFileInfo(path).fileName());
        action->setData(path);
        action->setToolTip(path);
        connect(action, &QAction::triggered, this, &MainWindow::openRecent);
    }
}

void MainWindow::openRecent()
{
    auto *action = qobject_cast<QAction *>(sender());
    if (!action)
        return;
    const QString path = action->data().toString();
    if (!maybeSave())
        return;
    openPath(path); // same password handling as Open…
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (!maybeSave()) {
        event->ignore();
        return;
    }
    // The help window is deliberately PARENTLESS (see onHelpContents), so it
    // does not go when this does. Qt quits on the last window closing, and a
    // manual left open would keep the process alive with no way back to the
    // program it documents.
    if (m_helpWindow)
        m_helpWindow->close();
    event->accept();
}

// ---- Connections / Calculations ----

// May this session lower a Protect Communication marking? By spec that means a
// CONNECTED DEVICE confirming the Protected Comms password, and this is the
// only place in the program that knows how to ask. Handed to Communications
// Setup and on to the section editor as a ProtectedCommsProver, so neither of
// them has to learn the protocol — or drag the serial layer into headers that
// GUI-only test targets compile.
//
// Reports its own failures, because a bool coming back from three quite
// different refusals would leave the caller inventing a message for a situation
// it cannot see.
bool MainWindow::proveProtectedCommsForEdit()
{
    if (!m_link.isOpen()) {
        QMessageBox::warning(
            this, tr("Protect Communication"),
            tr("Unticking Protect Communication needs the Protected Comms password "
               "checked by a connected CAN Triple. Holding this configuration is deliberately "
               "not enough — that device round trip is the only thing that makes this stronger "
               "than Hidden.\n\n"
               "Connect a device (Tools > Connection Settings…) and try again. The section can "
               "still be removed, reordered and sent without one."));
        return false;
    }
    QString error;
    device_session::AccessState state;
    bool read = false;
    {
        BusyScope busy(this); // sync round trip; block re-entry during it
        read = device_session::readAccessState(&m_link, &state, &error);
    }
    if (!read) {
        QMessageBox::warning(this, tr("Protect Communication"),
                             tr("Could not read which access passwords this device has set, so "
                                "it cannot authorise this.\n\n%1").arg(error));
        return false;
    }
    // Nothing for the device to check. Deliberately NOT accepted in silence:
    // this is exactly the state in which the tier is inert, and the operator
    // should learn it here rather than discover afterwards that a message marked
    // Protected was never guarded by anything. ensureDeviceAccess would return
    // true without a word, which is right for a Send and wrong for this.
    if (!state.supported || !state.isSet(AccessFunction::EditProtectedComms)) {
        return QMessageBox::question(
                   this, tr("Protect Communication"),
                   tr("This device has no Protected Comms password set, so there is "
                      "nothing for it to check — a Protect Communication marking is not being "
                      "enforced by anything here.\n\n"
                      "Set one under Online > Set Access Passwords… if this unit is meant to "
                      "guard these messages.\n\nContinue anyway?"),
                   QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
           == QMessageBox::Yes;
    }
    // Through the ordinary access path, so it tries the key this document
    // already carries before asking for a password — someone deploying a .ct3s
    // has it inside the file and may never have been told the password.
    return ensureDeviceAccess(AccessFunction::EditProtectedComms);
}

void MainWindow::onCommunications()
{
    // The prover travels for exactly one reason: unticking Protect Communication
    // requires the Protected Comms password to be proved against a
    // CONNECTED device, and that round trip is the only thing that makes the
    // tier stronger than Hidden. Passed whether or not a device is connected —
    // the prover asks, and "nothing connected" is an honest no rather than an
    // error the dialog has to guess at.
    CommunicationsDialog dialog(&m_config, this,
                                [this]() { return proveProtectedCommsForEdit(); });
    dialog.exec();
    // The session may have unlocked a section, so anything showing protected
    // detail has to be repainted, and the status line reads differently.
    updateProtectionState();
    if (m_monitorDialog)
        m_monitorDialog->rebuild();
}

void MainWindow::onMathChannels()
{
    MathDialog dialog(&m_config, this);
    dialog.exec();
    if (m_monitorDialog)
        m_monitorDialog->rebuild();
}

void MainWindow::onConditions()
{
    ConditionsDialog dialog(&m_config, this);
    dialog.exec();
    // Every User Condition output is boolean. Applied here rather than inside
    // the dialog's accept because the picker can create or re-type a channel
    // from inside it — the Edit… button reaches the full Channel Editor — so
    // the state that has to be corrected is whatever the document holds when
    // the dialog closes, cancelled or not.
    m_config.forceConditionOutputsBoolean();
    if (m_monitorDialog)
        m_monitorDialog->rebuild();
}

void MainWindow::onCounters()
{
    CountersDialog dialog(&m_config, this);
    dialog.exec();
    if (m_monitorDialog)
        m_monitorDialog->rebuild();
}

void MainWindow::onTimers()
{
    TimersDialog dialog(&m_config, this);
    dialog.exec();
    if (m_monitorDialog)
        m_monitorDialog->rebuild();
}

void MainWindow::onIntegrators()
{
    IntegratorsDialog dialog(&m_config, this);
    dialog.exec();
    if (m_monitorDialog)
        m_monitorDialog->rebuild();
}

void MainWindow::onConstants()
{
    ConstantsDialog dialog(&m_config, this);
    dialog.exec();
    if (m_monitorDialog)
        m_monitorDialog->rebuild();
}

void MainWindow::onTables()
{
    TablesDialog dialog(&m_config, this);
    dialog.exec();
    if (m_monitorDialog)
        m_monitorDialog->rebuild();
}

void MainWindow::onDeviceScript()
{
    // Modal, unlike the Lua Console. The console is a tool you leave open beside
    // the document; this edits one field OF the document and commits it on OK,
    // so leaving it open beside a document being edited elsewhere would mean two
    // versions of the same script's channel names drifting apart.
    ScriptEditorDialog dialog(m_config, this);
    connect(&dialog, &ScriptEditorDialog::helpRequested, this, &MainWindow::showHelpPage);
    dialog.exec();
    updateWindowTitle(); // the script is document content; OK can dirty it
}

// ---- Online ----

// Online > Send Secure Configuration… — install a .ct3s without opening it.
//
// Send Configuration sends the document you are looking at, and File > Open
// followed by Send is how a package gets installed by hand. Both leave the
// contents in the application, which is exactly right when the contents are
// yours and wrong when they are not: hand either to a dealer and "install this
// update" quietly becomes "and here is the configuration to browse".
//
// This path never creates a document. The package is decoded into a local
// Configuration that lives inside this function, is used only to produce the
// device tables, and is destroyed on the way out. m_config is not read, not
// written, and not consulted anywhere below — the operator's own work is still
// on screen, untouched, when the upload finishes.
//
// Nothing about the package is displayed either. Not a message count, not a
// channel name, not a bus rate, and not the per-stage progress text (which would
// otherwise count the channels out loud). A mapping failure reports how many
// errors there were and not one word of what they were. That is the difference
// between a file the app declines to show and a file the app shows in pieces.
//
// The honest boundary, so nobody mistakes this for more than it is: the bytes
// are decrypted in this process to be sent at all. This withholds them from the
// UI; it does not withhold them from someone instrumenting the application. What
// makes a package genuinely unreadable is saving it with "Require access
// password for use", and then it cannot be installed here without that password
// either — see the note where the password is asked for.
void MainWindow::onSendSecureConfiguration()
{
    const QString title = tr("Send Secure Configuration");

    if (!ensureConnected())
        return;

    // No document of its own to sit beside: this command installs a sealed
    // package without opening it, so it starts where the packages are kept.
    const QString path =
        QFileDialog::getOpenFileName(this, title, startIn({}), tr(kSecureFileFilter));
    if (path.isEmpty())
        return;

    // Peek before committing to anything. A .ct3 opened here would be a mistake
    // worth naming rather than quietly sending: this command exists for sealed
    // packages, and a plain configuration has no business pretending to be one.
    Configuration::FilePeek peek;
    QString error;
    if (!Configuration::peekFile(path, &peek, &error)) {
        QMessageBox::warning(this, title, error);
        return;
    }
    if (!peek.secure) {
        QMessageBox::warning(this, title,
                             tr("\"%1\" is a plain configuration, not a secure one.\n\n"
                                "Open it and use Online → Send Configuration, or save it with "
                                "File → Save Secure Config… first.")
                                 .arg(QFileInfo(path).fileName()));
        return;
    }

    // No password prompt here any more: v2 removed the container's
    // password-protected mode, so a .ct3s always opens. What stops a package
    // installing on the wrong unit is the licence match, checked against the
    // device rather than typed by whoever is holding the laptop.

    // The package. Scoped to this function on purpose — see the header comment.
    Configuration package;
    if (!package.loadFromFile(path, &error)) {
        QMessageBox::warning(this, title, error);
        return;
    }

    // ------------------------------------------------ the install policy
    //
    // Decided BEFORE the device is touched: the verdict is pure, the key proof
    // that follows it is read-only, and nothing that changes device state runs
    // until the person has said yes further down. The old order wrote the
    // package's passwords into device RAM and only then asked whether to
    // install — a No left them changed for the session.
    //
    // The decision itself lives in packageInstallVerdict(), where a test can
    // reach it. This function keeps the wording.
    const SecurePackagePolicy policy = package.secureOptions().policy;

    device_session::LicenseState deviceLicense;
    QString licenseError;
    {
        BusyScope busy(this);
        device_session::readLicense(&m_link, &deviceLicense, &licenseError);
    }

    const InstallVerdict verdict = packageInstallVerdict(
        policy, deviceLicense.supported, deviceLicense.manufacturer, deviceLicense.model,
        deviceLicense.firmwareVersion);
    if (verdict.noPolicy) {
        QMessageBox::critical(
            this, title,
            tr("\"%1\" carries no install policy, so there is no way to tell which devices "
               "it was built for.\n\nIt has NOT been sent. Rebuild it with "
               "File → Secure Configuration Builder.")
                .arg(QFileInfo(path).fileName()));
        return;
    }
    if (verdict.deviceUnlicensed) {
        QMessageBox::critical(
            this, title,
            tr("This unit's firmware cannot report a licence, so it cannot be matched against "
               "this package.\n\nIt has NOT been sent. Update the unit's firmware."));
        return;
    }
    if (!verdict.ok()) {
        // Each failure names the field and both values: "this package was not
        // built for this device" is true and useless to the person holding the
        // laptop, while "Model: package wants X, device says Y" tells them
        // whether they picked up the wrong file or the wrong unit.
        const auto fieldLabel = [this](const QString &field) {
            if (field == QLatin1String("manufacturer"))
                return tr("Manufacturer");
            if (field == QLatin1String("model"))
                return tr("Model");
            return tr("Version");
        };
        QStringList lines;
        for (const InstallMismatch &m : verdict.mismatches)
            lines << tr("%1: package wants \"%2\", device says \"%3\"")
                         .arg(fieldLabel(m.field), m.wanted, m.actual);
        QMessageBox::critical(this, title,
                              tr("This package was not built for the connected device, so it "
                                 "has NOT been sent.\n\n%1")
                                  .arg(lines.join(QStringLiteral("\n"))));
        return;
    }

    // THE KEY, which is not optional and is not a string compare. The host picks
    // a nonce and the device answers under the key it holds — so this proves the
    // unit really carries the licence rather than merely reporting one, and a
    // look-alike that echoed the right manufacturer and model still fails here.
    // Read-only: the device's state is unchanged by answering.
    {
        bool mismatch = false;
        QString proveError;
        bool proved = false;
        {
            BusyScope busy(this);
            proved = device_session::proveLicenseKey(&m_link, policy.key, &proveError, &mismatch);
        }
        if (!proved) {
            QMessageBox::critical(
                this, title,
                tr("This unit could not prove the Firmware Key this package requires, so the "
                   "package has NOT been sent.\n\n%1")
                    .arg(proveError.isEmpty() ? tr("The device did not answer.") : proveError));
            return;
        }
    }

    // mapWithScript: a package carrying a script — written as Lua, or retained
    // as bytecode from a Get — would otherwise be programmed with the script
    // silently stripped, because plain mapToDevice emits no chunks and no chunks
    // means "remove it". A package built from a Get is exactly the document that
    // now carries a retained image, so this is on the common path, not a corner.
    // Pure, and ahead of the confirmation so a broken package is refused before
    // anyone is asked to approve installing it.
    const MappingResult mapped = mapWithScript(package);
    if (!mapped.ok()) {
        // The count, and deliberately not the list. Every mapper error names a
        // row — a channel, a start bit, a message — and printing them here would
        // undo the whole point of this command over a broken package.
        QMessageBox::warning(this, title,
                             tr("This package cannot be programmed onto a device: the mapping "
                                "reports %1 error(s).\n\nIt has to be corrected by whoever "
                                "built it — open the configuration it was built from to see "
                                "them.")
                                 .arg(mapped.errors.size()));
        return;
    }

    // THE CONFIRMATION — and everything above it left the device exactly as it
    // was. Everything below it changes something.
    QString confirm = tr("Install \"%1\" on the connected device?\n\n"
                         "This replaces the device's entire configuration and saves it to "
                         "flash, so it reloads at every power-up.")
                          .arg(QFileInfo(path).fileName());
    if (policy.changesPasswords())
        confirm += tr("\n\nThis package also sets the device's access passwords.");
    confirm += tr("\n\nThe package is not opened and its contents are not shown. The "
                  "configuration you have open stays as it is.");
    if (QMessageBox::question(this, title, confirm, QMessageBox::Yes | QMessageBox::No,
                              QMessageBox::No)
        != QMessageBox::Yes)
        return;

    // Password updates need the device to accept writes it would otherwise
    // refuse, which means proving the key TO the device — the other direction
    // from the check above, and the grant that makes it a master key.
    //
    // Done BEFORE the transfer, and the order is load-bearing: access keys live
    // in the config store's write-once header, so they have to be in the
    // device's RAM before the Save-to-Flash that ends the transfer commits that
    // header. Written afterwards they would sit in RAM uncommitted and vanish at
    // the next power cycle — the documented trap this whole arrangement exists
    // to avoid. A CLEAR_CONFIG does not disturb them (engine resetRuntime leaves
    // the key record alone), so writing them first is safe.
    if (policy.changesPasswords()) {
        QString proveError;
        bool granted = false;
        {
            BusyScope busy(this);
            granted = device_session::proveLicenseSecret(&m_link, policy.key, &proveError);
        }
        if (!granted) {
            QMessageBox::critical(
                this, title,
                tr("This package sets device passwords, and the unit would not accept the "
                   "Firmware Key as authority to do so. Nothing has been sent.\n\n%1")
                    .arg(proveError.isEmpty() ? tr("The device did not answer.") : proveError));
            return;
        }

        QStringList failed;
        const auto applyOne = [&](bool wanted, AccessKey key, AccessFunction fn, int slot,
                                  const QString &label) {
            if (!wanted)
                return;
            QString err;
            // The package carries DERIVED keys, never phrases, so there is
            // nothing to derive here. kNoAccessKey is the clear sentinel — the
            // instruction a ticked box with an empty field records.
            const bool ok = key == kNoAccessKey
                                ? device_session::clearAccessKey(&m_link, fn, &err, slot)
                                : device_session::writeAccessKey(&m_link, fn, key, &err, slot);
            if (!ok)
                failed << tr("%1 (%2)").arg(label, err.isEmpty() ? tr("refused") : err);
        };
        {
            BusyScope busy(this);
            applyOne(policy.setSend, policy.sendKey, AccessFunction::SendConfiguration, 1,
                     tr("Send Configuration"));
            applyOne(policy.setGet, policy.getKey, AccessFunction::GetConfiguration, 1,
                     tr("Get Configuration"));
            for (int i = 0; i < 4; ++i)
                applyOne(policy.setCommsSlot[i], policy.commsSlotKey[i],
                         AccessFunction::EditProtectedComms, i + 1,
                         tr("Protected Comms slot %1").arg(i + 1));
        }
        if (!failed.isEmpty()) {
            // Refused BEFORE the configuration goes out, so the unit is left as
            // it was rather than carrying a new configuration under passwords
            // that are half old and half new.
            QMessageBox::critical(this, title,
                                  tr("Some of this package's passwords could not be set, so "
                                     "nothing has been sent.\n\n%1")
                                      .arg(failed.join(QStringLiteral("\n"))));
            return;
        }
    }

    if (!ensureDeviceAccess(AccessFunction::SendConfiguration))
        return;

    QVector<ControlCanPayload> busSetups;
    for (int i = 0; i < 3; ++i) {
        ControlCanPayload setup{};
        setup.bus_idx = quint8(i + 1);
        setup.mode = package.bus[i].enabled ? 1 : 0;
        setup.baud_rate = busRateHz(package.bus[i].rateKbps);
        setup.data_baud_rate = busRateHz(package.bus[i].dataRateKbps);
        setup.termination = package.bus[i].termination ? 1 : 0;
        busSetups.append(setup);
    }

    auto *progress = new QProgressDialog(tr("Installing…"), tr("Cancel"), 0, 100, this);
    progress->setWindowTitle(title);
    progress->setWindowModality(Qt::WindowModal);
    progress->setMinimumDuration(0);
    // The version the package stamps on the unit comes from its policy — the
    // Builder's "Package version" — not from the document inside it.
    auto *transfer = ConfigTransfer::send(&m_link, mapped.tables, /*verify=*/true, busSetups,
                                          /*saveToFlash=*/true, policy.configVersion,
                                          package.effectiveTitle(), /*resetAfter=*/false, this);
    // The bar moves; the stage text does not. ConfigTransfer's stages read
    // "Sending channels (64/82)", which is a running count of what is in the
    // package — harmless-looking and still more than this command promises.
    connect(transfer, &ConfigTransfer::progress, progress,
            [progress](int done, int total, const QString &) {
                progress->setMaximum(total);
                progress->setValue(done);
            });
    connect(progress, &QProgressDialog::canceled, transfer, &ConfigTransfer::cancel);
    connect(transfer, &ConfigTransfer::finished, this,
            [this, progress, transfer, title](bool ok, const QString &transferError) {
                progress->close();
                progress->deleteLater();
                if (!ok) {
                    QMessageBox::warning(this, title, transferError);
                    return;
                }
                QMessageBox::information(
                    this, title,
                    transfer->flashSaveWasSkipped()
                        ? tr("Installed and verified.\n\nThis firmware is too old to save to "
                             "flash, so the configuration is lost at power-off.")
                        : tr("Installed, verified, and saved to flash.\n\nIt reloads "
                             "automatically at every power-up."));
            });
}

void MainWindow::onSendConfiguration()
{
    if (!ensureConnected())
        return;
    // Prove the DEVICE's Send password before anything else — there is no point
    // mapping and confirming a send the device will refuse at the first write.
    if (!ensureDeviceAccess(AccessFunction::SendConfiguration))
        return;

    // PROTECTED COMMS GOES ONLY TO A UNIT THAT PROVES THE SAME KEY.
    //
    // Every other tier is a convention of this application — the device has
    // enforced nothing about message protection since 2.3.0. Protected is the
    // one tier that was always meant to be more than that, and what makes it
    // more is a connected unit agreeing about a secret. Until now that
    // agreement was asked for when somebody EDITED a protected message and
    // never when they sent one, so a configuration full of Protected messages
    // went happily to a device that had never heard of its password.
    //
    // Refused in three separate ways rather than one, because "it would not
    // send" is not a diagnosis:
    //
    //   * the unit has no password set  — no password is not the same password
    //   * the document holds no key     — nothing to match with
    //   * the unit disagrees about it   — the case this exists for
    //
    // There is NO prompt-and-retry here, unlike ensureDeviceAccess(). Asking
    // the operator to type a password would let anyone who knows the unit's
    // secret send a document that was never sealed with it, which is exactly
    // the substitution this tier exists to prevent. The key has to be IN the
    // document, and it gets there by the document having been secured.
    if (m_config.hasProtectedComms()) {
        const QString title = tr("Send Configuration");
        const QString tail = tr("\n\nNothing has been sent.");

        device_session::AccessState state;
        QString accessError;
        bool read = false;
        {
            BusyScope busy(this);
            read = device_session::readAccessState(&m_link, &state, &accessError);
        }
        // The decision is device_session::protectedSendVerdict's, so the order
        // of these refusals is pinned by a test rather than by this function
        // being read carefully. The prove round trip runs only once the cheaper
        // refusals have been ruled out, which is why the verdict is asked twice:
        // once with proved=false to reach that point, once with the result.
        using V = device_session::ProtectedSendVerdict;
        V verdict = device_session::protectedSendVerdict(read, state, m_config.commsKey(),
                                                         /*proved=*/false, /*wrongKey=*/false);
        bool wrongKey = false;
        if (verdict == V::ProofFailed) { // i.e. nothing cheaper refused it
            bool proved = false;
            {
                BusyScope busy(this);
                proved = device_session::proveAccess(&m_link,
                                                     AccessFunction::EditProtectedComms,
                                                     m_config.commsKey(), &accessError,
                                                     &wrongKey);
            }
            verdict = device_session::protectedSendVerdict(read, state, m_config.commsKey(),
                                                           proved, wrongKey);
        }

        switch (verdict) {
        case V::Allowed:
            break;
        case V::NoDeviceAnswer:
            QMessageBox::warning(this, title,
                                 tr("This configuration contains messages marked Protect "
                                    "Communication, and this unit could not say which access "
                                    "passwords it has set.%1\n\n%2")
                                     .arg(tail, accessError));
            return;
        case V::DeviceHasNoPassword:
            QMessageBox::warning(
                this, title,
                tr("This configuration contains messages marked Protect Communication, and "
                   "this unit has no Protected Comms password set — so it cannot be the "
                   "same password.%1\n\nSet the unit's Protected Comms password to match "
                   "this configuration's under Online > Set Access Passwords…, then "
                   "send again.")
                    .arg(tail));
            return;
        case V::DocumentHasNoKey:
            QMessageBox::warning(
                this, title,
                tr("This configuration contains messages marked Protect Communication but "
                   "carries no Protected Comms password of its own, so there is nothing to "
                   "match against this unit's.%1\n\nGive the configuration its Protected "
                   "Comms password with File → Save Secure Config…, or remove the "
                   "Protect Communication markings.")
                    .arg(tail));
            return;
        case V::Mismatch:
            QMessageBox::warning(
                this, title,
                tr("This configuration's Protected Comms password does not match this "
                   "unit's.%1\n\nA configuration containing messages marked Protect "
                   "Communication goes only to a unit that holds the same password.")
                    .arg(tail));
            return;
        case V::ProofFailed:
            QMessageBox::warning(
                this, title,
                tr("This unit could not confirm the Protected Comms password.%1\n\n%2")
                    .arg(tail, accessError));
            return;
        }
    }

    // Does this device's firmware speak the same configuration format this build
    // writes? Asked BEFORE anything is sent, because the alternative is what a
    // user actually met: the device NACKs the first message chunk with
    // ERR_INVALID_LEN and the send fails with "invalid length", which is a true
    // statement about a frame and a useless one about the problem.
    //
    // The record sizes are already version-checked ON THE WIRE — the device
    // tests length == 4 + count*item_size, so a mismatched write is refused
    // rather than misread, and that is the part that protects the device. This
    // is the other half: saying which of the two is out of date, and what to do.
    {
        FwUpdateStatus fw{};
        QString statusError;
        bool gotStatus = false;
        {
            BusyScope busy(this);
            FirmwareUpdater updater(&m_link);
            gotStatus = updater.readStatus(&fw, &statusError);
        }
        // Only when the device actually reported one. A unit whose firmware
        // predates the field, or has no bootloader, answers 0 — and "I could not
        // tell" must not become "it is wrong", or this would block sends to
        // hardware this build talks to perfectly well.
        if (gotStatus && fw.running_store_version != 0
            && fw.running_store_version != EXPECTED_STORE_VERSION) {
            const bool deviceOlder = fw.running_store_version < EXPECTED_STORE_VERSION;
            QMessageBox::warning(
                this, tr("Send Configuration"),
                deviceOlder
                    ? tr("This device's firmware stores configurations in an older format than "
                         "this version of the Device Manager writes (device v%1, this app v%2), "
                         "so the two cannot exchange one.\n\n"
                         "Nothing has been sent. Update the unit with Online → Update "
                         "Firmware…, then send again.\n\n"
                         "A firmware update across this change clears the unit's stored "
                         "configuration, so it will need this one sending afterwards either "
                         "way.")
                          .arg(fw.running_store_version)
                          .arg(EXPECTED_STORE_VERSION)
                    : tr("This device's firmware stores configurations in a NEWER format than "
                         "this version of the Device Manager writes (device v%1, this app v%2)."
                         "\n\nNothing has been sent. Use the Device Manager that came with "
                         "that firmware.")
                          .arg(fw.running_store_version)
                          .arg(EXPECTED_STORE_VERSION));
            return;
        }
    }

    // NO IDENTITY CHECK HERE ANY MORE. Send used to ask the uploader's rules
    // whether this configuration belonged on this unit, and warn if not. Those
    // rules compared a document's fleet block against an identity compiled into
    // the firmware, and both halves are gone: the compiled identity was replaced
    // by the writable firmware licence, and the policy is being repositioned onto
    // the device's own hardware record (Online → Get Device Info) rather than
    // onto a fleet block in the file.
    //
    // Deliberately left as nothing rather than half-rewired. A check that
    // compares against a field nobody sets any more is worse than no check: it
    // passes everything and reads as though something was verified.
    //
    // The device lock above is untouched and still refuses outright — that one
    // names a chip, not a fleet, and never depended on any of this.

    const auto issues = validateConfiguration(m_config);
    const bool hasErrors = std::any_of(issues.begin(), issues.end(), [](const ValidationIssue &i) {
        return i.severity == ValidationIssue::Error;
    });
    if (hasErrors) {
        QMessageBox::warning(this, tr("Send Configuration"),
                             tr("The configuration has errors. Fix them first — "
                                "see File → Check Channels."));
        CheckChannelsDialog::run(&m_config, this);
        return;
    }

    const MappingResult mapped = mapWithScript(m_config);
    if (!mapped.ok()) {
        // The error list is unbounded (one line per bad row) — keep the box
        // small and put the full list in the scrollable details pane.
        QMessageBox box(QMessageBox::Warning, tr("Send Configuration"),
                        tr("Cannot map the configuration to the device (%1 error(s)).\n\n"
                           "See Show Details for the full list.")
                            .arg(mapped.errors.size()),
                        QMessageBox::Ok, this);
        box.setDetailedText(mapped.errors.join(QStringLiteral("\n")));
        box.exec();
        return;
    }

    // Per-bus CONTROL_CAN setups from the Communications rate/mode settings
    // (v2 firmware applies them; v1 NACKs and the step is skipped).
    QVector<ControlCanPayload> busSetups;
    QStringList busSummary;
    for (int i = 0; i < 3; ++i) {
        ControlCanPayload setup{};
        setup.bus_idx = quint8(i + 1);
        setup.mode = m_config.bus[i].enabled ? 1 : 0;
        setup.baud_rate = busRateHz(m_config.bus[i].rateKbps);
        setup.data_baud_rate = busRateHz(m_config.bus[i].dataRateKbps);
        setup.termination = m_config.bus[i].termination ? 1 : 0;
        busSetups.append(setup);
        if (!m_config.bus[i].enabled)
            busSummary.append(tr("  CAN %1: off").arg(i + 1));
        else if (m_config.bus[i].isFd())
            busSummary.append(tr("  CAN %1: %2k + FD %3k").arg(i + 1)
                                  .arg(busRateLabel(m_config.bus[i].rateKbps),
                                       busRateLabel(m_config.bus[i].dataRateKbps)));
        else
            busSummary.append(tr("  CAN %1: %2k classic").arg(i + 1)
                                  .arg(busRateLabel(m_config.bus[i].rateKbps)));
    }

    QString detail = tr("This will replace the device configuration with:\n"
                        "  %1 messages, %2 channels, %3 math, %4 conditions, "
                        "%5 counters, %6 timers.")
                         .arg(mapped.tables.messages.size())
                         .arg(mapped.tables.signalConfigs.size())
                         .arg(mapped.tables.math.size())
                         .arg(mapped.tables.conditions.size())
                         .arg(mapped.tables.counters.size())
                         .arg(mapped.tables.timers.size());
    detail += tr("\n\nBus settings applied:\n%1").arg(busSummary.join(QStringLiteral("\n")));
    detail += tr("\n\nThe configuration is saved to flash so it reloads at every power-up.");
    // Said here rather than in a prompt of its own. It is worth knowing before
    // clicking OK, and it is not worth a question — the operator is looking at
    // the confirmation for this very send, which is exactly where a remark about
    // that send belongs.

    // A Protected section is unticked by proving Protected Comms AGAINST A
    // DEVICE, so on a unit with no such password set the tier is inert: the
    // section editor's own challenge finds nothing to check and lets it through.
    // Say so before the configuration goes, not after — the operator is sending
    // work marked Protected to a unit that will not be able to guard it, and the
    // fix (Online > Set Access Passwords…, then send again) has to happen on
    // this device, in this session, or it does not happen at all.
    //
    // Asked LAST among the pre-send checks and answered with a plain Yes/No,
    // because it is not a reason to refuse: a bench unit, a development board or
    // a fleet whose passwords are set at the end of the line are all ordinary,
    // and a check that cannot be overridden becomes a reason to stop marking
    // messages Protected in the first place.
    bool anyProtected = false;
    for (const auto &b : m_config.bus)
        for (const CommsSection &s : b.sections)
            if (s.protection == CommsProtection::Protected)
                anyProtected = true;
    if (anyProtected) {
        QString accessError;
        device_session::AccessState access;
        bool readAccess = false;
        {
            BusyScope busy(this); // sync round trip — see the readIdentity note above
            readAccess = device_session::readAccessState(&m_link, &access, &accessError);
        }
        // Only when the device actually answered. "I could not tell" must not
        // become "it is wrong" — that is the same rule the store-version check
        // above follows, and for the same reason.
        if (readAccess && access.supported
            && !access.isSet(AccessFunction::EditProtectedComms)) {
            if (QMessageBox::warning(
                    this, tr("Send Configuration"),
                    tr("This configuration marks messages Protect Communication, but this "
                       "device has no Protected Comms password set.\n\n"
                       "Opening a message marked that way needs a device confirming that "
                       "password, so on this unit there is nothing to confirm and the marking "
                       "will not be enforced by anything.\n\n"
                       "Set it under Online > Set Access Passwords… and send again, or send "
                       "now and set it afterwards.\n\nSend anyway?"),
                    QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
                != QMessageBox::Yes)
                return;
        }
    }

    // Send dialog: a required configuration title (stored on the device, ≤32
    // bytes) plus an optional post-send reset.
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Send Configuration"));
    auto *dlgLayout = new QVBoxLayout(&dlg);
    auto *detailLabel = new QLabel(detail, &dlg);
    detailLabel->setWordWrap(true);
    dlgLayout->addWidget(detailLabel);

    // The warning list is unbounded (one line per mapper note), so it lives in
    // its own scrolling view instead of growing the dialog past the screen.
    if (!mapped.warnings.isEmpty()) {
        dlgLayout->addWidget(new QLabel(tr("Warnings (%1):").arg(mapped.warnings.size()), &dlg));
        auto *warnView = new QPlainTextEdit(mapped.warnings.join(QStringLiteral("\n")), &dlg);
        warnView->setReadOnly(true);
        warnView->setLineWrapMode(QPlainTextEdit::NoWrap);
        warnView->setMaximumHeight(180);
        dlgLayout->addWidget(warnView);
    }

    auto *form = new QFormLayout;
    auto *titleEdit = new QLineEdit(m_config.effectiveTitle(), &dlg);
    titleEdit->setMaxLength(CONFIG_NAME_LEN);
    titleEdit->setPlaceholderText(tr("required"));
    form->addRow(tr("Configuration Title :"), titleEdit);
    dlgLayout->addLayout(form);

    // Device binding. Only offered when the device can actually report an
    // identity — on older firmware there is nothing to bind to, and a tick box
    // that silently did nothing would be worse than none.
    QString identityError;
    device_session::Identity identity;
    // BusyScope: this is a synchronous device round trip on the main thread, and
    // its nested event loop would otherwise let F5 launch a SECOND Send whose
    // CLEAR_CONFIG interleaves, step by step, with the first — two transfers
    // racing over one device. Disabling the window for the duration closes that.
    {
        BusyScope busy(this);
        device_session::readIdentity(&m_link, &identity, &identityError);
    }
    auto *bindCheck = new QCheckBox(tr("Lock this configuration to this device"), &dlg);
    bindCheck->setEnabled(identity.supported && !identity.uid.isEmpty());
    bindCheck->setToolTip(
        identity.supported
            ? tr("Stores this device's unique chip ID (%1) with the configuration. The device "
                 "then refuses to run it if it is copied to a different CAN Triple — by a "
                 "flash programmer, or by any tool that is not this one.\n\n"
                 "Sending to another device re-binds it, so replacing hardware still works.")
                  .arg(identity.uidText())
            : tr("This firmware is too old to report a device ID."));
    dlgLayout->addWidget(bindCheck);

    auto *resetCheck = new QCheckBox(tr("Reset device after sending"), &dlg);
    dlgLayout->addWidget(resetCheck);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    dlgLayout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    auto *okButton = buttons->button(QDialogButtonBox::Ok);
    auto refreshOk = [okButton, titleEdit]() {
        okButton->setEnabled(!titleEdit->text().trimmed().isEmpty());
    };
    connect(titleEdit, &QLineEdit::textChanged, &dlg, refreshOk);
    refreshOk();
    titleEdit->setFocus();
    titleEdit->selectAll();

    if (dlg.exec() != QDialog::Accepted)
        return;

    // 2.3.0 removed a step here: unlockDeviceMessagesForSend(), which read the
    // device's message table, found the records marked protected, and proved a
    // per-message password for each one before the send's opening
    // CMD_CLEAR_CONFIG could be refused. There is nothing left for it to do —
    // the firmware's per-message key, its CMD_MSG_ACCESS_RESPONSE handler and
    // its clear gate are all gone, so a clear is now refused for exactly one
    // reason (the Send password) and that is proved at the top of this function.
    // Do not reinstate it: opcode 0x40 NACKs ERR_INVALID_CMD by design, and a
    // host that reads that as "wrong password" is the unescapable prompt loop
    // the retirement comment in wire_structs.h describes.
    const QString configTitle = titleEdit->text().trimmed();
    const bool resetAfter = resetCheck->isChecked();
    m_config.setConfigTitle(configTitle);
    updateWindowTitle();

    // Binding goes BEFORE the tables. It is an ordinary write, so it is accepted
    // now that Send has been proved, and doing it first means a send that dies
    // half way leaves the device owning what it has rather than orphaned. Not
    // fatal on old firmware — the message says what was not applied and the
    // configuration still goes.
    //
    // Access passwords are deliberately NOT written here. They belong to the
    // device rather than to the document, and quietly installing one as a side
    // effect of a Send is how a fleet ends up locked with nobody sure by whom.
    // Online → Set Access Passwords is the only place they change.
    QStringList notApplied;
    QString stepError;
    bool bindOk = false;
    {
        BusyScope busy(this); // sync round trip — see the readIdentity note above
        bindOk = device_session::writeBinding(
            &m_link, bindCheck->isChecked() ? identity.uid : QByteArray(), &stepError);
    }
    if (!bindOk)
        notApplied << stepError;

    auto *progress = new QProgressDialog(tr("Sending configuration…"), tr("Cancel"), 0, 100, this);
    progress->setWindowModality(Qt::WindowModal);
    progress->setMinimumDuration(0);
    // No configuration version, deliberately. This is the engineer's Send, not
    // a release: nullopt sends an empty Save-to-Flash payload, which the
    // firmware reads as "leave the stored version alone". Only a package —
    // through Send Secure Configuration — stamps a version on a unit.
    auto *transfer = ConfigTransfer::send(&m_link, mapped.tables, /*verify=*/true, busSetups,
                                          /*saveToFlash=*/true, std::nullopt, configTitle,
                                          resetAfter, this);
    connect(transfer, &ConfigTransfer::progress, progress,
            [progress](int done, int total, const QString &stage) {
                progress->setMaximum(total);
                progress->setValue(done);
                progress->setLabelText(stage);
            });
    connect(progress, &QProgressDialog::canceled, transfer, &ConfigTransfer::cancel);
    const bool bound = bindCheck->isChecked() && !identity.uid.isEmpty();
    const QString boundTo = identity.uidText();
    connect(transfer, &ConfigTransfer::finished, this,
            [this, progress, transfer, notApplied, bound, boundTo](bool ok, const QString &error) {
        progress->close();
        progress->deleteLater();
        if (ok) {
            QString text = transfer->flashSaveWasSkipped()
                ? tr("Configuration sent and verified.\n\n"
                     "This firmware is too old to save to flash, so the configuration "
                     "lives in device RAM and is lost at power-off.")
                : tr("Configuration sent, verified, and saved to flash.\n\n"
                     "It reloads automatically at every power-up.");
            if (bound)
                text += tr("\n\nLocked to this device (%1). It will not run on any other "
                           "CAN Triple.").arg(boundTo);
            if (!notApplied.isEmpty())
                text += tr("\n\nNot applied:\n%1").arg(notApplied.join(QStringLiteral("\n")));
            if (!transfer->skippedStages().isEmpty())
                text += tr("\n\nSkipped (not accepted by this firmware):\n%1")
                            .arg(transfer->skippedStages().join(QStringLiteral("\n")));
            QMessageBox::information(this, tr("Send Configuration"), text);
        } else {
            QMessageBox::warning(this, tr("Send Configuration"), error);
        }
        if (m_monitorDialog)
            m_monitorDialog->rebuild();
    });
}

void MainWindow::onGetConfiguration()
{
    if (!ensureConnected())
        return;
    // Reading is the function being asked for, so a device that guards only
    // Send still answers a Get without a password.
    if (!ensureDeviceAccess(AccessFunction::GetConfiguration))
        return;

    // A device holding a configuration bound to a different chip reads back as
    // empty, because the engine refused to load it. Say so — otherwise the user
    // gets a blank document and no idea why.
    {
        QString error;
        device_session::Identity identity;
        bool boundElsewhere = false;
        {
            BusyScope busy(this); // sync round trip; block re-entry during it
            boundElsewhere = device_session::readIdentity(&m_link, &identity, &error)
                             && identity.boundElsewhere();
        }
        if (boundElsewhere) {
            QMessageBox::warning(
                this, tr("Get Configuration"),
                tr("This device (%1) holds a configuration that was locked to a different "
                   "CAN Triple, so it is not running and there is nothing to read back.\n\n"
                   "Send a configuration to this device to replace it.")
                    .arg(identity.uidText()));
            return;
        }
    }

    if (m_config.isDirty()
        && QMessageBox::question(this, tr("Get Configuration"),
                                 tr("Reading the device configuration replaces the current "
                                    "unsaved document. Continue?"),
                                 QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
               != QMessageBox::Yes)
        return;

    runGetTransfer(/*allowUnlockRetry=*/true);
}

// A read was refused with ERR_LOCKED. Name the password that actually unlocks
// it and offer to enter it; true means the caller may try the read again.
//
// It named the WRONG password until 2.3.0, and had done since v21. The text
// said Protected Comms unlocks a refused table read, on the strength of a
// per-message read gate that v21 deleted. Since then the only thing that refuses
// a table read is ACCESS_FN_GET, and 2.3.0 removed the last device-side notion
// of message protection altogether — the device hands back every record it holds
// to anyone who can satisfy Get, which is a documented weak point rather than an
// oversight (see serial_proto.c's read-path rationale). So the offer is
// GET CONFIGURATION now.
//
// Both are still tried, in that order, because the two failures are
// indistinguishable from here: the device answers ERR_LOCKED and says nothing
// about which function it meant. Get is the one that can actually be refused by
// a 2.3.0 unit, so it is offered first and named in the prompt. Protected
// Comms follows because a unit still running 2.2.x enforces the retired
// per-message read gate and would refuse the same chunk again. Neither costs the
// user a prompt they did not need: ensureDeviceAccess returns immediately for a
// function the device reports as unset, so the only passwords asked for are the
// ones this particular unit actually has.
bool MainWindow::offerProtectedCommsUnlock(const QString &title)
{
    const auto answer = QMessageBox::question(
        this, title,
        tr("This device refused to hand back its stored configuration, which means it has a "
           "Get Configuration password set.\n\n"
           "Enter it now and read the configuration?"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
    if (answer != QMessageBox::Yes)
        return false;
    // Through the ordinary access path, so each one tries the key this document
    // already carries before asking for a password — someone holding a .ct3s has
    // the key in the file and may never have been told the password. Both report
    // their own failures, so there is nothing to add here.
    if (!ensureDeviceAccess(AccessFunction::GetConfiguration))
        return false;
    // Best effort, and deliberately not fatal: a unit whose firmware predates
    // 2.3.0 can still refuse a chunk carrying a marked record. A device with no
    // Protected Comms password set returns true here without asking
    // anything, which is the ordinary case and must stay silent.
    ensureDeviceAccess(AccessFunction::EditProtectedComms);
    return true;
}

// The read itself, separated from the checks above so it can be run a SECOND
// time after a password is proved — see the locked branch at the bottom.
void MainWindow::runGetTransfer(bool allowUnlockRetry)
{
    auto *progress = new QProgressDialog(tr("Reading configuration…"), tr("Cancel"), 0, 100, this);
    progress->setWindowModality(Qt::WindowModal);
    progress->setMinimumDuration(0);
    auto *transfer = ConfigTransfer::get(&m_link, this);
    connect(transfer, &ConfigTransfer::progress, progress,
            [progress](int done, int total, const QString &stage) {
                progress->setMaximum(total);
                progress->setValue(done);
                progress->setLabelText(stage);
            });
    connect(progress, &QProgressDialog::canceled, transfer, &ConfigTransfer::cancel);
    connect(transfer, &ConfigTransfer::tablesReady, this, [this, transfer](const DeviceTables &tables) {
        // A device with nothing stored answers every read with an empty table.
        // Mapping that over the open document replaces it with a blank one, and
        // since there is then nothing to report, the old code said nothing at
        // all — which is indistinguishable from Get being broken. Check first,
        // say so, and leave the document alone.
        const bool deviceEmpty =
            tables.messages.isEmpty() && tables.signalConfigs.isEmpty() && tables.math.isEmpty()
            && tables.conditions.isEmpty() && tables.counters.isEmpty() && tables.timers.isEmpty()
            && tables.constants.isEmpty() && tables.relays.isEmpty()
            && tables.tables2x16Def.isEmpty() && tables.tables2x16Out.isEmpty()
            && tables.tables8x8Def.isEmpty() && tables.tables8x8Row.isEmpty()
            && tables.integrators.isEmpty() && tables.scriptChunks.isEmpty();
        if (deviceEmpty) {
            QMessageBox::information(
                this, tr("Get Configuration"),
                tr("This device has no configuration stored, so there is nothing to read "
                   "back. The open document has been left as it is.\n\n"
                   "A device reads as empty when it has never been sent a configuration, "
                   "or after a firmware update that changed the stored-image format — in "
                   "that case the configuration is not recoverable from the device and "
                   "must be sent again."));
            return;
        }
        QStringList notes;
        mapFromDevice(tables, m_config, &notes, transfer->deviceBusSetup());
        updateWindowTitle();
        updateProtectionState();
        if (m_monitorDialog)
            m_monitorDialog->rebuild();
        QStringList lines;
        const QString devName = transfer->deviceConfigName();
        if (!devName.isEmpty()) {
            m_config.setConfigTitle(devName);
            lines << tr("Configuration title: %1").arg(devName);
        }
        // Always say what came back, even when there is nothing else to report.
        // A Get that quietly swaps the document for a different one is the same
        // failure as the empty case, just harder to notice.
        int sectionCount = 0;
        for (const auto &b : m_config.bus)
            sectionCount += b.sections.size();
        lines << tr("Read %1 message section(s) and %2 channel(s) from the device.")
                     .arg(sectionCount)
                     .arg(m_config.catalog().userChannels().size());
        lines += notes.mid(0, 15);
        // Say what was NOT read, and say it prominently. A Get maps a lost or
        // refused reply to an empty table, so a stage the transfer skipped is a
        // table that reads as empty here for a reason that is not "the device
        // has none" — and a Send only surfaced this until now, leaving a Get
        // silently short. A lost reply (flaky link) is the dangerous one and is
        // called out as such, because the document now on screen is missing
        // whatever it covered.
        if (!transfer->skippedStages().isEmpty()) {
            lines << QString();
            lines << (transfer->anyReplyLost()
                          ? tr("WARNING — some replies were lost, so this configuration may "
                               "be INCOMPLETE. The tables below could not be read and read "
                               "as empty; check the connection and Get again:")
                          : tr("Not read back from this firmware (these tables read as "
                               "empty here):"));
            lines += transfer->skippedStages();
        }
        QMessageBox::information(this, tr("Get Configuration"),
                                 tr("Configuration read.\n\n%1").arg(lines.join(QStringLiteral("\n"))));
    });
    connect(transfer, &ConfigTransfer::finished, this,
            [this, progress, transfer, allowUnlockRetry](bool ok, const QString &error) {
        progress->close();
        progress->deleteLater();
        if (ok)
            return;

        // A Get can clear its own password gate and still be refused, and the
        // bare device error ("the device configuration is password protected")
        // sends the user looking for the wrong password. The device withholds a
        // table-read chunk that CONTAINS PROTECTED MESSAGE RECORDS unless Edit
        // Protected Comms has been proved — deliberately, because the Get
        // password alone must not hand back the protocol that protection exists
        // to keep. So name the right password and offer to enter it.
        //
        // Only once (allowUnlockRetry): if the read is still refused after a
        // proof, something else is guarding it and asking again in a loop would
        // teach the user nothing.
        if (allowUnlockRetry && transfer->failedLocked()
            && offerProtectedCommsUnlock(tr("Get Configuration"))) {
            runGetTransfer(/*allowUnlockRetry=*/false);
            return;
        }
        QMessageBox::warning(this, tr("Get Configuration"), error);
    });
}

void MainWindow::onMonitorChannels()
{
    if (!m_monitorDialog) {
        m_monitorDialog = new MonitorChannelsDialog(&m_link, &m_config, this);
        m_monitorDialog->setAttribute(Qt::WA_DeleteOnClose);
    }
    m_monitorDialog->show();
    m_monitorDialog->raise();
    m_monitorDialog->activateWindow();
}

void MainWindow::onCanViewer()
{
    if (!m_viewerDialog) {
        m_viewerDialog = new CanViewerDialog(&m_link, this);
        m_viewerDialog->setAttribute(Qt::WA_DeleteOnClose);
    }
    m_viewerDialog->show();
    m_viewerDialog->raise();
    m_viewerDialog->activateWindow();
}

void MainWindow::onResetDevice()
{
    if (!ensureConnected())
        return;
    if (!ensureDeviceAccess(AccessFunction::SendConfiguration))
        return;
    if (QMessageBox::question(this, tr("Reset Device"),
                              tr("Reboot the device now? It re-initializes and reloads its saved "
                                 "configuration from flash; live streams pause briefly."),
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
        != QMessageBox::Yes)
        return;
    QString error;
    bool ok = false;
    {
        BusyScope busy(this);
        ok = m_link.requestSync(CMD_RESET_DEVICE, {}, nullptr, &error);
    }
    if (ok)
        QMessageBox::information(this, tr("Reset Device"),
                                 tr("Reset command sent — the device is rebooting."));
    else
        QMessageBox::warning(this, tr("Reset Device"), error);
}

void MainWindow::onUpdateFirmware()
{
    if (!ensureConnected())
        return;
    // Replacing the firmware is at least as consequential as replacing the
    // configuration, and the device gates the update commands on the same key,
    // so ask for it here rather than letting the first NACK be the prompt.
    if (!ensureDeviceAccess(AccessFunction::SendConfiguration))
        return;

    // The dialog resets the device part-way through, which clears the access
    // proofs held in its RAM. Give it a way to establish them again before the
    // post-update configuration restore, or that restore NACKs ERR_LOCKED on
    // every password-protected unit — the ones whose configuration is most
    // worth restoring.
    FirmwareUpdateDialog dialog(&m_link, this, [this]() {
        return ensureDeviceAccess(AccessFunction::SendConfiguration);
    });
    connect(&dialog, &FirmwareUpdateDialog::helpRequested, this, &MainWindow::showHelpPage);
    dialog.exec();

    // The device may be running different firmware now, and the open document
    // has not changed — but anything cached about the unit has. Re-read it
    // rather than leaving stale protection state on screen.
    updateProtectionState();
}

void MainWindow::onDeviceStatus()
{
    if (!ensureConnected())
        return;
    QByteArray payload;
    QString error;
    bool ok = false;
    {
        BusyScope busy(this);
        ok = m_link.requestSync(CMD_GET_STATUS, {}, &payload, &error);
    }
    if (!ok || payload.size() < int(sizeof(DeviceStatus))) {
        QMessageBox::warning(this, tr("Device Status"),
                             error.isEmpty() ? tr("Unexpected response") : error);
        return;
    }
    DeviceStatus status;
    std::memcpy(&status, payload.constData(), sizeof(status));
    QString text = tr("Uptime: %1 s\n\n").arg(status.uptime_ms / 1000.0, 0, 'f', 1);
    for (int i = 0; i < 3; ++i)
        text += tr("CAN %1:  rx %2   tx %3\n")
                    .arg(i + 1)
                    .arg(status.rx_count[i])
                    .arg(status.tx_count[i]);
    text += tr("\nActive: %1 messages, %2 channels, %3 math, %4 conditions")
                .arg(status.active_msg_count)
                .arg(status.active_sig_count)
                .arg(status.active_math_count)
                .arg(status.active_cond_count);
    if (payload.size() > int(sizeof(DeviceStatus)))
        text += tr("\n\nFirmware protocol: v%1").arg(quint8(payload[sizeof(DeviceStatus)]));
    else
        text += tr("\n\nFirmware protocol: v1 (original — flash the firmware/ project "
                   "for live streams, transmit messages, and bus control)");

    // Identity, access and — the reason this is here — why a device that looks
    // inert is inert. "0 active messages" alone never explains a configuration
    // that belongs to a different unit.
    device_session::Identity identity;
    QString sessionError;
    QString deviceId; // kept for the Copy button below
    if (device_session::readIdentity(&m_link, &identity, &sessionError) && identity.supported) {
        deviceId = identity.uidText();
        text += tr("\n\nDevice ID: %1").arg(deviceId);
        switch (identity.configStatus) {
        case CONFIG_STATUS_OK:
            break;
        case CONFIG_STATUS_WRONG_DEVICE:
            text += tr("\n⚠ The stored configuration is locked to a DIFFERENT CAN Triple, so "
                       "it is not running. Send a configuration to this device to replace it.");
            break;
        default:
            text += tr("\nNo stored configuration — the device is running bus defaults.");
            break;
        }
    }

    // WHICH passwords are set, never what they are — the device does not hand
    // those out. Listing them here is what tells an operator in advance that a
    // Send is going to ask for something, rather than finding out mid-transfer.
    device_session::AccessState access;
    if (device_session::readAccessState(&m_link, &access, &sessionError) && access.supported) {
        if (!access.any()) {
            text += tr("\n\nAccess passwords: none set.");
        } else {
            QStringList names;
            const AccessFunction *functions = allAccessFunctions();
            for (int i = 0; i < kAccessFunctionCount; ++i) {
                if (access.isSet(functions[i]))
                    names << accessFunctionLabel(functions[i]);
            }
            text += tr("\n\nAccess passwords set for: %1").arg(names.join(QStringLiteral(", ")));
        }
    }

    // Who the unit is LICENSED to. This was the fleet identity, which was
    // compiled into the firmware and was therefore the one line in this dialog
    // nobody with a serial cable could have changed. A licence is writable by
    // design, so it does not carry that guarantee and must not be read as if it
    // did — what cannot be rewritten is the hardware record behind
    // Online \u2192 Get Device Info, burned once into OTP.
    device_session::LicenseState statusLicense;
    if (device_session::readLicense(&m_link, &statusLicense, &sessionError)
        && statusLicense.supported) {
        const QString dash = QStringLiteral("\u2014");
        if (statusLicense.blank()) {
            text += tr("\n\nFirmware licence: none issued.");
        } else {
            text += tr("\n\nFirmware licence: %1 / %2 / %3")
                        .arg(statusLicense.manufacturer.isEmpty() ? dash
                                                                  : statusLicense.manufacturer,
                             statusLicense.model.isEmpty() ? dash : statusLicense.model,
                             statusLicense.firmwareVersion.isEmpty()
                                 ? dash
                                 : statusLicense.firmwareVersion);
        }
        // The two secrets are reported separately because they mean different
        // things and only one of them protects anything. A unit can hold a
        // Firmware Key and still be freely rewritable.
        text += statusLicense.keySet
                    ? tr("\nFirmware Key: held, so this unit can prove which licence it "
                         "holds.")
                    : tr("\nFirmware Key: none, so this unit can report a licence but cannot "
                         "prove it.");
        text += statusLicense.updaterSet
                    ? tr("\nFW Updater Password: set, so these details cannot be changed "
                         "without it.")
                    : tr("\nFW Updater Password: none, so anyone who connects can rewrite "
                         "these details.");
    }

    // The configuration version: the revision of the configuration THIS unit is
    // running, stamped by the package that installed it. A plain Send leaves it
    // alone, so it stays the last release's number rather than becoming zero.
    device_session::ConfigVersionState configVersion;
    if (device_session::readConfigVersion(&m_link, &configVersion, &sessionError)
        && configVersion.supported)
        text += tr("\n\nConfiguration version on this unit: %1").arg(configVersion.version);
    // Built rather than QMessageBox::information, for the Device ID. It is a
    // 24-character hex string that has to be typed into Lock to Device somewhere
    // else — reading it off the screen and retyping it is exactly the kind of
    // transcription nobody gets right first time.
    //
    // Two ways out, because they suit different moments: the whole report is
    // selectable, so right-click gives the standard Copy on any part of it
    // (QLabel's selectable text brings its own context menu), and a Copy Device
    // ID button takes just the UID with no selecting at all.
    QMessageBox box(QMessageBox::Information, tr("Device Status"), text, QMessageBox::Ok, this);
    box.setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    QPushButton *copyId = nullptr;
    if (!deviceId.isEmpty()) {
        // ActionRole so it does not close the box: copying is something you do
        // ALONGSIDE reading the rest, not instead of it.
        copyId = box.addButton(tr("Copy Device ID"), QMessageBox::ActionRole);
        connect(copyId, &QPushButton::clicked, this, [this, deviceId, copyId]() {
            QApplication::clipboard()->setText(deviceId);
            // Say so on the button itself; a status bar message would be behind
            // this modal box, and a message box on top of it would be absurd.
            copyId->setText(tr("Device ID copied"));
        });
    }
    box.exec();
}

// Online > Get Device Info — the manufacturing record burned into the OTP.
//
// Its own dialog rather than four more lines in Device Status, though both read
// the same unit over the same cable. They answer opposite questions. Device
// Status says what this device is DOING, and every line of it can differ between
// two readings a second apart. This says what this device IS, and no line of it
// can ever change: OTP has no erase, so the record is burned once at manufacture
// and reads the same on the day the unit is scrapped. Folding the permanent
// facts into the volatile ones would have made the whole report look like a
// snapshot — and this is the half somebody quotes in a warranty claim.
//
// Not gated by a password, like the Device ID beside it and for the same reason:
// nothing here describes a configuration. An RMA cannot be conditioned on
// holding the password of the configuration being returned as faulty.
void MainWindow::onGetDeviceInfo()
{
    if (!ensureConnected())
        return;
    const QString title = tr("Device Info");

    device_session::DeviceInfo info;
    QString error;
    bool ok = false;
    {
        BusyScope busy(this); // sync round trip on the main thread; block re-entry
        ok = device_session::readDeviceInfo(&m_link, &info, &error);
    }
    if (!ok) {
        QMessageBox::warning(this, title, error.isEmpty() ? tr("Unexpected response") : error);
        return;
    }
    if (!info.supported) {
        // The record is in the silicon either way — this firmware just has no
        // command to hand it over. Worth saying, so nobody concludes the board
        // was never provisioned and burns a second one over the top of it.
        QMessageBox::information(
            this, title,
            tr("This firmware cannot report a manufacturing record — the command is newer "
               "than the firmware on this unit.\n\nThe record is in the chip either way. "
               "Update the unit's firmware to read it."));
        return;
    }

    // "Unknown", never a blank. An empty field means that double-word was never
    // burned, which is a fact about the board rather than an empty string, and
    // showing nothing would read as a value of nothing.
    const QString unknown = tr("Unknown");
    const auto shown = [&unknown](const QString &value) {
        return value.isEmpty() ? unknown : value;
    };

    // Plain text in the dialog's own font, like every other report in this
    // application. An earlier version set the five rows in a monospaced <pre> so
    // the values lined up in a column; it did line up, and it read as a
    // different application. Labels are padded no further than one space for the
    // same reason — space padding only aligns in a fixed-width font, and in a
    // proportional one it is a ragged gap pretending to be a column.
    QString text;
    text += tr("Manufacturer: %1\n").arg(shown(info.manufacturer));
    text += tr("Product: %1\n").arg(shown(info.product));
    text += tr("HW Version: %1\n").arg(shown(info.hardwareVersion));
    text += info.serialKnown ? tr("HW Serial: %1\n").arg(info.serialNumber)
                             : tr("HW Serial: %1\n").arg(unknown);
    if (info.dateText.isEmpty())
        text += tr("Manufactured: %1").arg(unknown);
    else if (info.date.isValid())
        text += tr("Manufactured: %1").arg(info.date.toString(QStringLiteral("d MMMM yyyy")));
    else
        // Burned, but not as DDMMYYYY. Shown exactly as it stands rather than
        // coerced into the nearest plausible day: a wrong date presented as a
        // right one is worse than one that visibly does not parse.
        text += tr("Manufactured: %1  (not a readable DDMMYYYY date)").arg(info.dateText);

    // The firmware licence, printed whenever the firmware can answer at all —
    // including when nothing has been issued, which prints "none issued" rather
    // than nothing. Hiding the section on a blank unit made "no licence" and
    // "this build does not show licences" look identical, and only one of those
    // is worth doing anything about.
    //
    // NEITHER SECRET IS HERE and neither can be. The device discloses no key and
    // no password; the most that could be said is whether they exist, and the
    // Firmware License Manager is where that belongs, beside the fields that
    // change them.
    device_session::LicenseState license;
    QString licenseError;
    if (device_session::readLicense(&m_link, &license, &licenseError) && license.supported) {
        if (license.blank()) {
            text += tr("\n\nFirmware licence: none issued.");
        } else {
            text += tr("\n\nFirmware Manufacturer: %1").arg(shown(license.manufacturer));
            text += tr("\nFirmware Model: %1").arg(shown(license.model));
            text += tr("\nFirmware Version: %1").arg(shown(license.firmwareVersion));
        }
    }

    // A note only when there is something to explain. A burned record speaks for
    // itself and had a line under it saying the values are permanent, which is
    // true of every reading and therefore worth saying nowhere — the manual
    // carries it. An EMPTY record does need a sentence, because five Unknowns
    // otherwise look like a failed read rather than an unburned part.
    if (info.blank()) {
        text += tr("\n\nThis unit's OTP has never been burned, so it cannot say who made it. "
                   "Nothing is wrong with the device — the record is programmed during "
                   "manufacture and this part did not receive one.");
    }

    // Selectable text plus a Copy button, the same pair Device Status offers and
    // for the same reason: the whole point of this dialog is that its contents
    // get quoted somewhere else, and retyping a serial number off a screen is
    // exactly the transcription nobody gets right first time.
    QMessageBox box(QMessageBox::Information, title, text, QMessageBox::Ok, this);
    box.setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    QPushButton *copyAll = box.addButton(tr("Copy"), QMessageBox::ActionRole);
    connect(copyAll, &QPushButton::clicked, this, [text, copyAll]() {
        QApplication::clipboard()->setText(text);
        copyAll->setText(tr("Copied"));
    });
    box.exec();
}

void MainWindow::onSetAccessPasswords()
{
    if (!ensureConnected())
        return;
    // The document goes in as well as the link: setting Protected Comms
    // writes a verifier into the open configuration so the file and the hardware
    // agree about which password reveals the protected messages.
    AccessPasswordsDialog dialog(&m_link, &m_config, this);
    dialog.exec();
    updateWindowTitle();
    updateProtectionState();
}

// Deliberately does NOT insist on a connection, which is a change from the
// screen this replaced. That one could WRITE an identity into a device, so a
// device was the whole point of opening it. This one cannot: a unit's identity
// is compiled into its firmware, the device panel is read-only, and all a
// connected unit adds is the ability to copy its vendor and model into the
// document instead of retyping them. Building the package a customer's car will
// be given six months from now is desk work, and demanding hardware for it would
// be demanding hardware to type two strings.
// Online > Firmware License Manager. Needs a connection, unlike the Fleet
// Identity dialog it replaces: that one edited the DOCUMENT and a device was
// optional, this one edits the DEVICE and there is nothing to show without one.
//
// No updateWindowTitle() afterwards either, and its absence is the point. The
// licence is not part of the document, so writing one does not dirty anything
// and there is nothing to save.
void MainWindow::onFirmwareLicense()
{
    // No ensureConnected() here, deliberately. Composing a licence is desk work
    // and the dialog opens without hardware; Apply is the step that needs a
    // unit, so the connect routine is handed to the dialog to call at that
    // point rather than run as a toll on opening it.
    FirmwareLicenseDialog dialog(&m_link, [this]() { return ensureConnected(); }, this);
    dialog.exec();
}

// (The paragraphs that used to follow described the uploader, which is gone —
// see the note above. Set Access Passwords needs a link for the ordinary
// reason: the passwords live in the device.)
//
// No access password is proved here, and that is not an omission. The point of
// the check half is that it works on a unit which will not open itself: reading
// the licence and challenging the Firmware Key are unconditional on the
// device, so a customer can be told "this update is not for your car" without
// anyone holding that device's Send or Get password. The dialog's own Upload
// button asks for what the WRITE needs, at the moment it needs it.
// Upload Configuration is GONE, and this note is where it was.
//
// It was the dealer-facing installer: open a package, compare it against the
// unit in front of you, refuse if it does not belong. Every one of those
// comparisons came from the fleet identity compiled into the firmware, and that
// identity was replaced by the writable firmware licence. With the rules gone
// the dialog was a table with nothing to put in it.
//
// What took over the job is Send Secure Configuration, which enforces a policy
// sealed into the package itself: manufacturer, model and version matches, and a
// Firmware Key the device must PROVE rather than merely report. That is a
// stronger check than the one removed \u2014 the old vendor and model comparison was
// two strings a look-alike could echo back.
//
// The one capability that went with it: installing a plain .ct3 from a file
// without loading it into the editor. File > Open followed by Send does the same
// thing, and a plain .ct3 conceals nothing anyway, so what was lost is a step
// rather than a capability.

// ---- Tools / Help ----

void MainWindow::onChannelEditor()
{
    ChannelEditorDialog::run(&m_config, this);
    // The Channel Editor can retype any channel, including one a User Condition
    // writes — where the type is not the user's to choose. Put it back rather
    // than refusing the edit: refusing would mean greying out the type of a
    // channel whose OTHER fields (name, units, description) are perfectly
    // editable, and explaining why in a dialog nobody reads.
    m_config.forceConditionOutputsBoolean();
    updateWindowTitle(); // editing a channel can mark the document dirty
    if (m_monitorDialog)
        m_monitorDialog->rebuild();
}

void MainWindow::onConnectionSettings()
{
    ConnectionSettingsDialog dialog(&m_link, this);
    dialog.exec();
    updateConnectionStatus();
}

// The manual is a window rather than a dialog, and a singleton like Monitor
// Channels: F1 while it is already open should bring it forward, not stack a
// second copy of the same book on the desk.
void MainWindow::onHelpContents()
{
    if (!m_helpWindow) {
        // NO PARENT, deliberately. F1 is offered from inside modal dialogs —
        // the Device Script editor and the firmware updater — and a modal
        // dialog blocks input to its parent window and to every sibling of its
        // parent. A help window parented to the main window is one of those
        // siblings, so it would open behind the dialog and, worse, be inert:
        // no scrolling, no links, no index. Reported from the Device Script
        // dialog, where the manual is most likely to be wanted.
        //
        // Parentless puts it outside that hierarchy entirely, which is what
        // keeps it usable. It costs the closeEvent above, where it has to be
        // closed by hand.
        m_helpWindow = new HelpWindow(nullptr);
        m_helpWindow->setAttribute(Qt::WA_DeleteOnClose);
    }
    m_helpWindow->show();
    m_helpWindow->raise();
    m_helpWindow->activateWindow();
}

// Context help: open the manual at one page. Goes through onHelpContents so the
// singleton rule and the raise/activate behaviour live in exactly one place.
void MainWindow::showHelpPage(const QString &pageFileName)
{
    onHelpContents();
    if (m_helpWindow)
        m_helpWindow->showPage(pageFileName);
}

void MainWindow::onLuaConsole()
{
    if (!m_luaConsole) {
        m_luaConsole = new LuaConsoleDialog(m_config, this);
        m_luaConsole->setAttribute(Qt::WA_DeleteOnClose);
        connect(m_luaConsole, &LuaConsoleDialog::helpRequested, this,
                &MainWindow::showHelpPage);
        // A script that mutated the document is the same situation as a Get
        // that mapped new tables in: everything showing document state needs
        // to hear about it. Same refresh set as the Get handler's.
        connect(m_luaConsole, &LuaConsoleDialog::configurationChanged, this, [this]() {
            updateWindowTitle();
            updateProtectionState();
            if (m_monitorDialog)
                m_monitorDialog->rebuild();
        });
    }
    m_luaConsole->show();
    m_luaConsole->raise();
    m_luaConsole->activateWindow();
}

// The about box has a job beyond vanity: name the licence, disclaim warranty,
// and say where the source is. It used to point at DESIGN.md and
// FIRMWARE-NOTES.md "in the project folder" — files an installed user has
// never seen — so the repository link replaces them and doubles as the
// source pointer.
//
// The links are live: QMessageBox::about puts its text in a QLabel whose
// interaction flags come from SH_MessageBox_TextInteractionFlags
// (Qt::LinksAccessibleByMouse) with openExternalLinks set, so an <a href> opens
// in the browser. The anchor text is the bare URL anyway, so the address stays
// readable and copyable even if a style ever turns clicking off.
void MainWindow::onAbout()
{
    QMessageBox::about(this, tr("About CAN Triple Device Manager"),
                       tr("<h3>CAN Triple Device Manager %1</h3>"
                          "<p>Configuration tool for the CAN Triple gateway "
                          "(STM32G473, 3× CAN).</p>"
                          "<p>Copyright © 2026 Minton Performance.</p>"
                          "<p>This program is open-source software under the MIT "
                          "License. It comes with ABSOLUTELY NO WARRANTY, to the "
                          "extent permitted by law. "
                          "Source code: <a href=\"%2\">%2</a></p>"
                          "<p>Includes Qt 6 — Copyright © The Qt Company Ltd. and "
                          "other contributors — used unmodified under the GNU Lesser "
                          "General Public License, version 3. See "
                          "THIRD-PARTY-NOTICES.txt beside the program for the full "
                          "list of bundled components.</p>"
                          "<p>The manual is under <b>Help &gt; Contents</b>.</p>")
                           .arg(QCoreApplication::applicationVersion(),
                                QStringLiteral("https://github.com/mitchdetailed/CAN-Triple-Device-Manager")));
}

} // namespace ct
