// Windows remember their size between runs — and only when main() has said
// so. Driven on plain widgets with QSettings pointed at a scratch directory.

#include <QApplication>
#include <QDialog>
#include <QFile>
#include <QSettings>
#include <QTemporaryDir>
#include <QWidget>

#include <cstdio>

#include "../src/ui/window_memory.h"

static int fails = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);              \
            ++fails;                                                                 \
        }                                                                            \
    } while (0)

namespace {

// Off by default: a dialog closed at one size opens at its default next time.
void testDisabledRemembersNothing()
{
    ct::setWindowMemoryEnabled(false);
    {
        QDialog d;
        d.resize(300, 200);
        ct::rememberWindowSize(&d, QStringLiteral("t1"));
        d.resize(640, 480);
        d.show();
        d.hide();
    }
    QDialog d2;
    d2.resize(300, 200);
    ct::rememberWindowSize(&d2, QStringLiteral("t1"));
    CHECK(d2.size() == QSize(300, 200));
    CHECK(!QSettings().contains(QStringLiteral("windows/t1/size")));
}

// On: the size at hide comes back, per key; OK/Cancel (hide) is enough.
void testSizeComesBack()
{
    ct::setWindowMemoryEnabled(true);
    {
        QDialog d;
        d.resize(300, 200);
        ct::rememberWindowSize(&d, QStringLiteral("t2"));
        d.resize(640, 480);
        d.show();
        d.accept(); // hides, as OK does
    }
    QDialog d2;
    d2.resize(300, 200);
    ct::rememberWindowSize(&d2, QStringLiteral("t2"));
    CHECK(d2.size() == QSize(640, 480));
    // Another key is another window.
    QDialog other;
    other.resize(300, 200);
    ct::rememberWindowSize(&other, QStringLiteral("t3"));
    CHECK(other.size() == QSize(300, 200));
}

// A size too small to be a window is ignored rather than applied.
void testTinySavedSizeIsIgnored()
{
    ct::setWindowMemoryEnabled(true);
    QSettings().setValue(QStringLiteral("windows/t4/size"), QSize(10, 10));
    QDialog d;
    d.resize(300, 200);
    ct::rememberWindowSize(&d, QStringLiteral("t4"));
    CHECK(d.size() == QSize(300, 200));
}

// The main window keeps its whole geometry; the first run reports nothing to
// restore so the caller can place it, every later run reports restored.
void testGeometryComesBack()
{
    ct::setWindowMemoryEnabled(true);
    {
        QWidget w;
        w.resize(300, 200);
        CHECK(!ct::rememberWindowGeometry(&w, QStringLiteral("t5")));
        // Inside the offscreen platform's 800x600 screen: restoreGeometry
        // shrinks a window to fit its screen, which is the safeguard this
        // feature relies on and not what this test is about.
        w.resize(640, 400);
        w.move(20, 30);
        w.show();
        w.close();
    }
    QWidget w2;
    w2.resize(300, 200);
    CHECK(ct::rememberWindowGeometry(&w2, QStringLiteral("t5")));
    CHECK(w2.size() == QSize(640, 400));
}

// Named a file, the sizes go there — legibly — and come back from it.
void testFileBacked()
{
    ct::setWindowMemoryEnabled(true);
    QTemporaryDir dir;
    const QString ini = dir.path() + QStringLiteral("/windows.ini");
    ct::setWindowMemoryFile(ini);
    {
        QDialog d;
        d.resize(300, 200);
        ct::rememberWindowSize(&d, QStringLiteral("t6"));
        d.resize(500, 350);
        d.show();
        d.hide();
    }
    QFile file(ini);
    CHECK(file.exists());
    CHECK(file.open(QIODevice::ReadOnly) && file.readAll().contains("t6"));
    QDialog d2;
    d2.resize(300, 200);
    ct::rememberWindowSize(&d2, QStringLiteral("t6"));
    CHECK(d2.size() == QSize(500, 350));
    // And nothing of it reached the default store.
    CHECK(!QSettings().contains(QStringLiteral("windows/t6/size")));
    ct::setWindowMemoryFile(QString());
}

// Across PROCESSES, which is the case the program actually has: with
// CT_WM_INI naming a file, the first run (no file yet) saves a size into it
// and the second run reads it back. Run twice by hand; skipped otherwise.
void testAcrossProcesses()
{
    const QString ini = qEnvironmentVariable("CT_WM_INI");
    if (ini.isEmpty())
        return;
    ct::setWindowMemoryEnabled(true);
    ct::setWindowMemoryFile(ini);
    if (!QFile::exists(ini)) {
        QDialog d;
        d.resize(300, 200);
        ct::rememberWindowSize(&d, QStringLiteral("cross"));
        d.resize(555, 444);
        d.show();
        d.accept();
        std::printf("cross-process: first run, wrote %s (exists=%d)\n", qPrintable(ini),
                    int(QFile::exists(ini)));
    } else {
        QDialog d;
        d.resize(300, 200);
        ct::rememberWindowSize(&d, QStringLiteral("cross"));
        std::printf("cross-process: second run, read back %dx%d\n", d.size().width(),
                    d.size().height());
        CHECK(d.size() == QSize(555, 444));
    }
    ct::setWindowMemoryFile(QString());
    ct::setWindowMemoryEnabled(false);
}

} // namespace

int main(int argc, char **argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QTemporaryDir scratch;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, scratch.path());
    QCoreApplication::setOrganizationName(QStringLiteral("ct-test"));
    QCoreApplication::setApplicationName(QStringLiteral("test_window_memory"));

    testDisabledRemembersNothing();
    testSizeComesBack();
    testTinySavedSizeIsIgnored();
    testGeometryComesBack();
    testFileBacked();
    testAcrossProcesses();

    if (fails == 0) {
        std::printf("test_window_memory: all checks passed\n");
        return 0;
    }
    std::printf("test_window_memory: %d check(s) failed\n", fails);
    return 1;
}
