// Move Up / Move Down in the seven Calculations dialogs: the selected row
// swaps with its neighbour in the working copy, the button follows the row so
// it can be pressed again, the edge rows refuse, and OK commits the new order
// — which is the only order that reaches the device, since every table runs
// top to bottom in one engine pass.
//
// Constants and Tables carry rename-tracking lists parallel to their rows
// (m_originalNames, m_orig2x16 / m_orig8x8). A move that left those behind
// would make commit() see "row 1 used to be called K1 and is now K2" and
// rewrite every reference to K1 — so each of those two is committed with a
// consumer referencing a moved row, and the consumer must still name it.
//
// Driven through the real dialogs offscreen, by the buttons' object names.

#include <QApplication>
#include <QDialogButtonBox>
#include <QFile>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeWidget>

#include <cstdio>

#include "../src/model/configuration.h"
#include "../src/ui/conditions_dialog.h"
#include "../src/ui/constants_dialog.h"
#include "../src/ui/counters_dialog.h"
#include "../src/ui/integrators_dialog.h"
#include "../src/ui/math_dialog.h"
#include "../src/ui/tables_dialog.h"
#include "../src/ui/timers_dialog.h"
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

using namespace ct;

QPushButton *button(QDialog &d, const char *name)
{
    auto *b = d.findChild<QPushButton *>(QLatin1String(name));
    Q_ASSERT(b);
    return b;
}

QTreeWidget *tree(QDialog &d)
{
    auto *t = d.findChild<QTreeWidget *>();
    Q_ASSERT(t);
    return t;
}

void select(QDialog &d, int row)
{
    QTreeWidget *t = tree(d);
    t->setCurrentItem(t->topLevelItem(row));
}

void pressOk(QDialog &d)
{
    auto *box = d.findChild<QDialogButtonBox *>();
    Q_ASSERT(box);
    box->button(QDialogButtonBox::Ok)->click();
}

// The common script: three rows A, B, C; select A; Up is refused at the top;
// two Downs carry A to the bottom, following it; Down is then refused; OK.
// Returns true when the buttons behaved; the caller checks the document.
bool moveFirstRowToBottom(QDialog &d)
{
    bool ok = true;
    select(d, 0);
    ok = ok && !button(d, "moveUp")->isEnabled();
    ok = ok && button(d, "moveDown")->isEnabled();
    button(d, "moveDown")->click();
    ok = ok && tree(d)->indexOfTopLevelItem(tree(d)->currentItem()) == 1; // followed
    button(d, "moveDown")->click();
    ok = ok && tree(d)->indexOfTopLevelItem(tree(d)->currentItem()) == 2;
    ok = ok && !button(d, "moveDown")->isEnabled();
    ok = ok && button(d, "moveUp")->isEnabled();
    pressOk(d);
    return ok;
}

void testMath()
{
    Configuration config;
    for (const char *name : {"A", "B", "C"}) {
        MathRow r;
        r.destChannel = QLatin1String(name);
        config.mathRows << r;
    }
    config.setDirty(false);
    MathDialog d(&config);
    CHECK(moveFirstRowToBottom(d));
    CHECK(config.mathRows.size() == 3);
    CHECK(config.mathRows[0].destChannel == QStringLiteral("B"));
    CHECK(config.mathRows[1].destChannel == QStringLiteral("C"));
    CHECK(config.mathRows[2].destChannel == QStringLiteral("A"));
    CHECK(config.isDirty());
}

void testConditions()
{
    Configuration config;
    for (const char *name : {"A", "B", "C"}) {
        ConditionRow r;
        r.outputChannel = QLatin1String(name);
        config.conditionRows << r;
    }
    ConditionsDialog d(&config);
    CHECK(moveFirstRowToBottom(d));
    CHECK(config.conditionRows.size() == 3);
    CHECK(config.conditionRows[2].outputChannel == QStringLiteral("A"));
    CHECK(config.conditionRows[0].outputChannel == QStringLiteral("B"));
}

void testTimers()
{
    Configuration config;
    for (const char *name : {"A", "B", "C"}) {
        TimerRow r;
        r.outputChannel = QLatin1String(name);
        config.timerRows << r;
    }
    TimersDialog d(&config);
    CHECK(moveFirstRowToBottom(d));
    CHECK(config.timerRows.size() == 3);
    CHECK(config.timerRows[2].outputChannel == QStringLiteral("A"));
    CHECK(config.timerRows[0].outputChannel == QStringLiteral("B"));
}

void testCounters()
{
    Configuration config;
    for (const char *name : {"A", "B", "C"}) {
        CounterRow r;
        r.outputChannel = QLatin1String(name);
        config.counterRows << r;
    }
    CountersDialog d(&config);
    CHECK(moveFirstRowToBottom(d));
    CHECK(config.counterRows.size() == 3);
    CHECK(config.counterRows[2].outputChannel == QStringLiteral("A"));
    CHECK(config.counterRows[0].outputChannel == QStringLiteral("B"));
}

void testIntegrators()
{
    Configuration config;
    for (const char *name : {"A", "B", "C"}) {
        IntegratorRow r;
        r.outputChannel = QLatin1String(name);
        config.integratorRows << r;
    }
    IntegratorsDialog d(&config);
    CHECK(moveFirstRowToBottom(d));
    CHECK(config.integratorRows.size() == 3);
    CHECK(config.integratorRows[2].outputChannel == QStringLiteral("A"));
    CHECK(config.integratorRows[0].outputChannel == QStringLiteral("B"));
}

// Constants: the rename tracking must move with the row. A math row reads K1;
// after K1 is moved to the bottom and OK pressed, it must still read K1.
void testConstants()
{
    Configuration config;
    for (const char *name : {"K1", "K2", "K3"}) {
        ConstantRow r;
        r.name = QLatin1String(name);
        r.dataType = QStringLiteral("float");
        config.constantRows << r;
    }
    MathRow consumer;
    consumer.aChannel = QStringLiteral("K1");
    consumer.aIsChannel = true;
    consumer.destChannel = QStringLiteral("Out");
    config.mathRows << consumer;

    ConstantsDialog d(&config);
    CHECK(moveFirstRowToBottom(d));
    CHECK(config.constantRows.size() == 3);
    CHECK(config.constantRows[0].name == QStringLiteral("K2"));
    CHECK(config.constantRows[1].name == QStringLiteral("K3"));
    CHECK(config.constantRows[2].name == QStringLiteral("K1"));
    CHECK(config.mathRows.size() == 1);
    CHECK(config.mathRows[0].aChannel == QStringLiteral("K1")); // not "renamed"
}

// Tables: two 2x16 tables then one 8x8 in the tree; a move stays within its
// kind, and the 8x8 — alone in its kind — cannot move at all. The math row
// reading T1 must still read T1 afterwards (m_orig2x16 moved with the row).
void testTables()
{
    Configuration config;
    for (const char *name : {"T1", "T2"}) {
        Table2x16Row r;
        r.outputChannel = QLatin1String(name);
        r.xChannel = QStringLiteral("X");
        config.table2x16Rows << r;
    }
    Table8x8Row u;
    u.outputChannel = QStringLiteral("U1");
    u.xChannel = QStringLiteral("X");
    u.yChannel = QStringLiteral("Y");
    config.table8x8Rows << u;
    MathRow consumer;
    consumer.aChannel = QStringLiteral("T1");
    consumer.aIsChannel = true;
    consumer.destChannel = QStringLiteral("Out");
    config.mathRows << consumer;

    TablesDialog d(&config);
    QTreeWidget *t = tree(d);
    CHECK(t->topLevelItemCount() == 3);

    select(d, 0); // T1
    CHECK(!button(d, "moveUp")->isEnabled());
    CHECK(button(d, "moveDown")->isEnabled());
    button(d, "moveDown")->click();
    CHECK(t->indexOfTopLevelItem(t->currentItem()) == 1); // T1 followed to the second 2x16 slot
    CHECK(!button(d, "moveDown")->isEnabled());           // last of its kind: no crossing into 8x8
    CHECK(button(d, "moveUp")->isEnabled());

    select(d, 2); // U1, alone among the 8x8 tables
    CHECK(!button(d, "moveUp")->isEnabled());
    CHECK(!button(d, "moveDown")->isEnabled());
    button(d, "moveDown")->click(); // refused, nothing changes
    CHECK(t->topLevelItemCount() == 3);

    pressOk(d);
    CHECK(config.table2x16Rows.size() == 2);
    CHECK(config.table2x16Rows[0].outputChannel == QStringLiteral("T2"));
    CHECK(config.table2x16Rows[1].outputChannel == QStringLiteral("T1"));
    CHECK(config.table8x8Rows.size() == 1 && config.table8x8Rows[0].outputChannel == QStringLiteral("U1"));
    CHECK(config.mathRows.size() == 1);
    CHECK(config.mathRows[0].aChannel == QStringLiteral("T1")); // not "renamed"
}

// A real Calculations dialog through the real path — exec(), resized inside
// its event loop as a user would, then OK — comes back at that size next time,
// from the file-backed store: within the process, and, more to the point,
// from the file's TEXT as a fresh process would read it (QSettings answers
// from its cache inside one process, so only a hand-written file proves the
// parse). The main window's own save at exit must not lose the dialog's key.
void testDialogSizeSurvivesExec()
{
    QTemporaryDir dir;
    const QString ini = dir.path() + QStringLiteral("/windows.ini");
    ct::setWindowMemoryEnabled(true);
    ct::setWindowMemoryFile(ini);
    Configuration config;
    QWidget parent;
    parent.resize(660, 800);
    parent.move(1266, 88);
    parent.show();
    QApplication::processEvents();
    {
        ConditionsDialog d(&config, &parent);
        CHECK(d.size() == QSize(760, 400)); // the default, nothing saved yet
        // Inside the offscreen platform's 800x600 screen: restore bounds a
        // saved size to the screen, which is not what this test is about.
        QTimer::singleShot(0, &d, [&d]() { d.resize(700, 520); });
        QTimer::singleShot(60, &d, [&d]() { d.accept(); });
        d.exec();
    }
    {
        ConditionsDialog d2(&config, &parent);
        std::printf("reopened size before exec: %dx%d\n", d2.size().width(), d2.size().height());
        CHECK(d2.size() == QSize(700, 520));
        QTimer::singleShot(0, &d2, [&d2]() {
            std::printf("reopened size once shown:  %dx%d\n", d2.size().width(), d2.size().height());
        });
        QTimer::singleShot(60, &d2, [&d2]() { d2.accept(); });
        d2.exec();
    }
    // The main window saves its geometry at exit, into the same file.
    {
        QWidget mainWindow;
        mainWindow.resize(640, 400);
        ct::rememberWindowGeometry(&mainWindow, QStringLiteral("main"));
        mainWindow.show();
        mainWindow.close();
    }
    QFile file(ini);
    CHECK(file.open(QIODevice::ReadOnly));
    const QByteArray text = file.readAll();
    file.close();
    CHECK(text.contains("@Size(700 520)"));
    CHECK(text.contains("main"));
    if (!text.contains("@Size(700 520)"))
        std::printf("ini after main-window save: %s\n", text.constData());

    // A fresh process: the value comes from the file's text, not a cache.
    // Written by hand in the form QSettings writes, under a new key.
    {
        QFile out(dir.path() + QStringLiteral("/fresh.ini"));
        CHECK(out.open(QIODevice::WriteOnly));
        out.write("[windows]\nconditions\\size=@Size(720 500)\n");
        out.close();
    }
    ct::setWindowMemoryFile(dir.path() + QStringLiteral("/fresh.ini"));
    {
        ConditionsDialog d3(&config, &parent);
        if (d3.size() != QSize(720, 500))
            std::printf("fresh-file restore gave %dx%d\n", d3.size().width(), d3.size().height());
        CHECK(d3.size() == QSize(720, 500));
    }
    ct::setWindowMemoryFile(QString());
    ct::setWindowMemoryEnabled(false);
}

} // namespace

int main(int argc, char **argv)
{
    // Offscreen unless the caller chose a platform: the same test run with
    // QT_QPA_PLATFORM=windows exercises real screens and DPI, which is how
    // the window-size case below was chased on the bench.
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    testMath();
    testConditions();
    testTimers();
    testCounters();
    testIntegrators();
    testConstants();
    testTables();
    testDialogSizeSurvivesExec();

    if (fails == 0) {
        std::printf("test_calc_order: all checks passed\n");
        return 0;
    }
    std::printf("test_calc_order: %d check(s) failed\n", fails);
    return 1;
}
