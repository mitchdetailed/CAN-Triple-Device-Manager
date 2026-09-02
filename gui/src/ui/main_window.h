// Main window — menu shell: File, Connections, Calculations, Online, Tools,
// Help. Offline-first document workflow with explicit Send/Get Configuration.
#pragma once

#include <QMainWindow>
#include <QPointer>

#include "../model/configuration.h"
#include "../protocol/device_link.h"

class QLabel;

namespace ct {

class CanViewerDialog;
class HelpWindow;
class LuaConsoleDialog;
class MonitorChannelsDialog;

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    // File
    void onNew();
    void onOpen();
    bool onSave();
    bool onSaveAs();
    void onSecureBuilder();
    void onCheckChannels();
    void onConfigSummary();
    void onRevealProtectedComms();
    // Opens `path`, asking for a password when the file needs one. Shared by
    // Open… and the recent-files list so both handle a locked file identically.
    bool openPath(const QString &path);
    void openRecent();
    void updateRecentMenu();
    void addRecentFile(const QString &path);
    bool maybeSave(); // returns false when the user cancels

    // Connections / Calculations
    void onCommunications();
    void onMathChannels();
    void onConditions();
    void onCounters();
    void onTimers();
    void onIntegrators();
    void onConstants();
    void onTables();
    void onDeviceScript();

    // Online
    void onSendConfiguration();
    // Push a .ct3s straight to the device without ever opening it as a document.
    // The package is read into a local Configuration that lives for the duration
    // of the call and is destroyed with it: nothing about its contents reaches
    // the open document, the window title, the recent-files list or the screen.
    // This is the path you hand a dealer — the one where "install it" does not
    // imply "and now you can read it".
    void onSendSecureConfiguration();
    void onGetConfiguration();
    // The read half of onGetConfiguration, split out so it can be repeated once
    // after a password is proved. allowUnlockRetry is false on that second run,
    // so a device that is still refusing cannot put the prompt in a loop.
    void runGetTransfer(bool allowUnlockRetry);
    // Offered when a read comes back ERR_LOCKED: names Get Configuration, which
    // is the only function a 2.3.0 unit refuses a table read for, and proves it
    // (then Protected Comms too, silently, for a unit still running the
    // retired 2.2.x read gate). True means the read is worth trying again.
    bool offerProtectedCommsUnlock(const QString &title);
    // There is deliberately no unlockDeviceMessagesForSend(). It read the
    // device's message table before every Send and proved a per-message password
    // for each marked record, because the firmware refused to erase one. 2.3.0
    // deleted that gate, the per-message key and the CMD_MSG_ACCESS_RESPONSE
    // handler it used, so a Send is refused for exactly one reason — the Send
    // password — which onSendConfiguration proves at the top. Reinstating it
    // would send opcode 0x40 at a device that now NACKs ERR_INVALID_CMD.
    void onMonitorChannels();
    void onCanViewer();
    void onResetDevice();
    void onDeviceStatus();
    void onGetDeviceInfo();
    void onUpdateFirmware();
    // Open the manual at one page — context help for dialogs that emit
    // helpRequested(). See HelpWindow::showPage.
    void showHelpPage(const QString &pageFileName);
    void onSetAccessPasswords();
    void onFirmwareLicense();

    // Tools / Help
    void onChannelEditor();
    void onLuaConsole();
    void onConnectionSettings();
    void onConnect();
    void onDisconnect();
    void onHelpContents();
    void onAbout();

    // Makes sure the connected device will accept what we are about to do:
    // reads which access passwords it has set and, if the one for `fn` is in
    // force, asks for it and proves it. Returns false when the user gives up or
    // the link fails, in which case the caller must not proceed.
    //
    // For EditProtectedComms it first tries the key this session already holds
    // — typed earlier, or carried inside a .ct3s — so a customer deploying a
    // locked configuration is never asked for a password they were never given.
    bool ensureDeviceAccess(AccessFunction fn);

    // "May this session lower a Protect Communication marking?" — which by spec
    // means a CONNECTED DEVICE confirming the Protected Comms password.
    // Handed to Communications Setup as a ProtectedCommsProver so the dialogs
    // never touch the link themselves. Reports its own failures; false means
    // leave the marking alone.
    bool proveProtectedCommsForEdit();

    void buildMenus();
    void buildCentral();
    // Reflects the document's protected-comms state: which of Reveal / Conceal
    // is useful, and what the status bar says. Unlike the old write-protection
    // gate this disables NOTHING wholesale — protecting a message locks that
    // message and its channels, not the rest of the document, so a customer can
    // still build their own math, transmit messages and calculations around it.
    void updateProtectionState();
    void updateWindowTitle();
    void updateConnectionStatus();
    bool ensureConnected(); // opens Connection Settings when offline

    Configuration m_config;
    DeviceLink m_link;
    QPointer<MonitorChannelsDialog> m_monitorDialog;
    QPointer<CanViewerDialog> m_viewerDialog;
    QPointer<HelpWindow> m_helpWindow;
    QPointer<LuaConsoleDialog> m_luaConsole;
    QMenu *m_recentMenu = nullptr;
    QLabel *m_connectionLabel = nullptr;
    QLabel *m_documentLabel = nullptr;
    QLabel *m_lockLabel = nullptr;

    QAction *m_revealAction = nullptr;
    QAction *m_concealAction = nullptr;
    QAction *m_connectAction = nullptr;
    QAction *m_disconnectAction = nullptr;
    // Online > Connect's memory, this session only: the port is chosen fresh
    // every run by design, but within a run "the one I was just using" is
    // almost always the answer.
    QString m_lastPort;
    qint32 m_lastBaud = 0;
};

} // namespace ct
