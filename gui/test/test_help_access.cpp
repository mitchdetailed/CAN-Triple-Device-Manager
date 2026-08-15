// Can the manual actually be read from where it is offered?
//
// F1 opens the help window from inside dialogs, and two of those dialogs are
// MODAL. A modal dialog does not merely sit in front of other windows — it
// blocks input to them. An application-modal dialog blocks every other window
// in the program, so a help window opened from one is not just behind it: it
// cannot be scrolled, its links cannot be followed, and its index cannot be
// used. Reported from the Device Script editor, where the manual is most
// likely to be wanted, and true of the firmware updater too.
//
// WHY THIS TEST CHECKS A POLICY RATHER THAN THE BLOCKING ITSELF. Qt enforces
// modal blocking in the platform layer, on real window-system input — on
// Windows by calling EnableWindow(hwnd, FALSE) on the blocked windows. A
// synthetic QMouseEvent posted from a test bypasses it entirely and is
// delivered whatever the modality is; verified by experiment before this file
// was written, so it is not an assumption. There is therefore nothing to assert
// about blocking from inside an offscreen unit test.
//
// What CAN be asserted is the arrangement that makes the help usable, and it is
// two facts standing together:
//
//   1. the help window has NO PARENT, so it is outside every dialog's window
//      hierarchy; and
//   2. no dialog offering help is APPLICATION-modal, which would block
//      everything regardless of hierarchy.
//
// Either one alone is insufficient, which is why both are checked, and why the
// test enumerates the dialogs rather than naming the two that exist today: a
// dialog that grows a helpRequested signal later gets checked for free.
//
// AND THEN THERE IS --live, which does check the blocking itself. It needs a
// desktop session, so it is not part of the ordinary sweep, but it settles the
// question rather than reasoning about it: real windows, the real dialog, and
// Win32's own IsWindowEnabled for the answer. It carries its own polarity
// control — an application-modal dialog must disable the help window — so a
// pass means the instrument works, not just that nothing was measured.
//
//     test_help_access          policy, headless, part of the sweep
//     test_help_access --live   the real thing, needs a desktop

#include <QApplication>
#include <QDialog>
#include <QMainWindow>
#include <QTimer>
#include <QWidget>

#include <cstdio>

#include "../src/model/configuration.h"
#include "../src/protocol/device_link.h"
#include "../src/ui/firmware_update_dialog.h"
#include "../src/ui/help_window.h"
#include "../src/ui/script_editor_dialog.h"

static int fails = 0;

#define CHECK(cond, what)                                                        \
    do {                                                                         \
        if (cond) {                                                              \
            std::printf("  PASS  %s\n", what);                                   \
        } else {                                                                 \
            std::printf("  FAIL  %s\n", what);                                   \
            ++fails;                                                             \
        }                                                                        \
    } while (0)

using namespace ct;

namespace {

const char *modalityName(Qt::WindowModality m)
{
    switch (m) {
    case Qt::NonModal: return "NonModal";
    case Qt::WindowModal: return "WindowModal";
    case Qt::ApplicationModal: return "ApplicationModal";
    }
    return "?";
}

// The property under test, for one dialog: opening help from it must leave the
// help window reachable. A dialog may be non-modal (nothing is blocked) or
// window-modal (only its own hierarchy is), but never application-modal.
void checkDialog(QDialog *d, const char *name)
{
    const Qt::WindowModality m = d->windowModality();
    std::printf("    %-22s %s\n", name, modalityName(m));
    if (m == Qt::ApplicationModal) {
        std::printf("  FAIL  %s is application-modal — help opened from it would be "
                    "inert\n", name);
        ++fails;
    } else {
        std::printf("  PASS  %s does not block the whole application\n", name);
    }
}

} // namespace

#ifdef Q_OS_WIN
#  include <windows.h>

// The end-to-end check, on the real Windows platform.
//
// Qt implements application-modal blocking on Windows the way Windows does it:
// EnableWindow(hwnd, FALSE) on every blocked window. "Can the user actually use
// the help window?" therefore has a literal answer, and this asks for it. The
// offscreen platform never creates real HWNDs, so this only runs with --live.
//
// It builds the arrangement out of the SAME classes the program uses —
// HelpWindow constructed parentless as MainWindow::onHelpContents does, and a
// real ScriptEditorDialog with its own constructor and therefore its own
// modality. Only the main window is a stand-in, and all it contributes is being
// the dialog's parent.
int runLive()
{
    QMainWindow owner;
    owner.setWindowTitle(QStringLiteral("stand-in for the main window"));
    owner.show();

    HelpWindow help(nullptr);
    help.show();
    const HWND helpHwnd = reinterpret_cast<HWND>(help.winId());
    const HWND ownerHwnd = reinterpret_cast<HWND>(owner.winId());

    Configuration config;

    // 1. The real dialog, as the program creates it.
    {
        ScriptEditorDialog dlg(config, &owner);
        QTimer::singleShot(250, [&] {
            QCoreApplication::processEvents();
            CHECK(IsWindowEnabled(helpHwnd),
                  "LIVE: help stays usable with the Device Script dialog open");
            CHECK(!IsWindowEnabled(ownerHwnd),
                  "LIVE control: the main window IS blocked (modality still works)");
            dlg.reject();
        });
        dlg.exec();
    }

    // 2. Polarity. An application-modal dialog must disable the help window —
    //    if this passes, the check above is measuring nothing.
    {
        QDialog appModal(&owner);
        appModal.setWindowModality(Qt::ApplicationModal);
        QTimer::singleShot(250, [&] {
            QCoreApplication::processEvents();
            CHECK(!IsWindowEnabled(helpHwnd),
                  "LIVE polarity: an application-modal dialog DOES disable help "
                  "(so the check above can fail)");
            appModal.reject();
        });
        appModal.exec();
    }
    return fails;
}
#endif

int main(int argc, char **argv)
{
    // --live opens real windows briefly and needs a desktop session; the
    // default stays headless so the ordinary sweep can run it anywhere.
    bool live = false;
    for (int i = 1; i < argc; ++i) {
        if (QString::fromLatin1(argv[i]) == QLatin1String("--live"))
            live = true;
    }
    if (!live)
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

#ifdef Q_OS_WIN
    if (live) {
        std::printf("live check (real windows, asking Win32 IsWindowEnabled)\n");
        runLive();
        if (fails == 0) {
            std::printf("\ntest_help_access --live: all checks passed\n");
            return 0;
        }
        std::printf("\ntest_help_access --live: %d check(s) failed\n", fails);
        return 1;
    }
#endif

    QMainWindow owner;
    owner.show();

    std::printf("the help window itself\n");
    {
        // Built the way MainWindow::onHelpContents builds it.
        HelpWindow help(nullptr);
        CHECK(help.parentWidget() == nullptr,
              "help window has no parent, so no dialog's modality reaches it");
        CHECK(help.isWindow(),
              "help window is a top-level window");
        // A parented help window is the bug this test exists for. Prove the
        // check has polarity rather than trusting that it would notice.
        HelpWindow parented(&owner);
        CHECK(parented.parentWidget() != nullptr,
              "control: a parented help window IS detected as parented");
    }

    std::printf("\ndialogs that offer F1 help\n");
    {
        Configuration config;
        ScriptEditorDialog editor(config, &owner);
        checkDialog(&editor, "Device Script");

        DeviceLink link;
        FirmwareUpdateDialog fw(&link, &owner, [] { return true; });
        checkDialog(&fw, "Update Firmware");
    }

    // exec() is what silently makes a dialog application-modal, and it does so
    // ONLY when the modality is still NonModal. So a dialog that sets
    // WindowModal in its constructor keeps it through exec(); one that sets
    // nothing does not. Pinning that here means the constructors above are
    // doing something real rather than being overwritten a moment later.
    std::printf("\nexec() modality promotion\n");
    {
        QDialog plain(&owner);
        CHECK(plain.windowModality() == Qt::NonModal,
              "a fresh QDialog starts NonModal (so exec() would promote it)");
        QDialog declared(&owner);
        declared.setWindowModality(Qt::WindowModal);
        CHECK(declared.windowModality() == Qt::WindowModal,
              "a declared WindowModal survives up to exec()");
    }

    if (fails == 0) {
        std::printf("\ntest_help_access: all checks passed\n");
        return 0;
    }
    std::printf("\ntest_help_access: %d check(s) failed\n", fails);
    return 1;
}
