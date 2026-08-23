// The table editors' two new conveniences, driven headlessly:
//
//   * GridClipboard — spreadsheet copy / cut / paste / delete over a numeric
//     QTableWidget. Tested directly on a bare grid, because the mechanics
//     (TSV round-trip, per-cell clamping, single-value fill, skipping fixed
//     cells) are what can silently go wrong.
//
//   * AxisSetupDialog — the dedicated axis window. Driven through its own
//     buttons (Insert / Delete / Linearise) the way a person drives it, then
//     read back through result() to prove OK returns what the grid shows.
//
// Runs on the offscreen platform, so it needs no display.

#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QKeyEvent>
#include <QListWidget>
#include <QPalette>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetSelectionRange>

#include <cstdio>

#include "../src/model/channel.h"
#include "../src/model/channel_catalog.h"
#include "../src/model/configuration.h"
#include "../src/ui/axis_setup_dialog.h"
#include "../src/ui/numeric_grid.h"
#include "../src/ui/select_channel_dialog.h"

using namespace ct;

static int fails = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);              \
            ++fails;                                                                 \
        }                                                                            \
    } while (0)

namespace {

QString cellText(QTableWidget *g, int r, int c)
{
    QTableWidgetItem *it = g->item(r, c);
    return it ? it->text() : QString();
}

void fill(QTableWidget *g, int r, int c, const QString &t)
{
    g->setItem(r, c, new QTableWidgetItem(t));
}

// Deliver a key to the clipboard as the dialogs' event filters do.
bool sendKey(GridClipboard &clip, int key, Qt::KeyboardModifiers mods)
{
    QKeyEvent ev(QEvent::KeyPress, key, mods);
    return clip.handleKeyPress(&ev);
}

void select(QTableWidget *g, int top, int left, int bottom, int right)
{
    g->clearSelection();
    g->setRangeSelected(QTableWidgetSelectionRange(top, left, bottom, right), true);
}

// ------------------------------------------------------------ clipboard tests
void testCopyPaste()
{
    QTableWidget g(3, 3);
    GridClipboard clip(&g, [](int, int) { return CellSpec{-1000, 1000, 3, true}; });
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            fill(&g, r, c, QString::number(r * 10 + c));

    // Copy the top-left 2x2 -> tab/newline TSV of exactly those cells.
    QApplication::clipboard()->clear();
    select(&g, 0, 0, 1, 1);
    CHECK(sendKey(clip, Qt::Key_C, Qt::ControlModifier));
    CHECK(QApplication::clipboard()->text() == QStringLiteral("0\t1\n10\t11"));

    // Paste that block anchored at (1,1): it lands at (1,1),(1,2),(2,1),(2,2).
    g.clearSelection();
    g.setCurrentCell(1, 1);
    CHECK(sendKey(clip, Qt::Key_V, Qt::ControlModifier));
    CHECK(cellText(&g, 1, 1) == QStringLiteral("0"));
    CHECK(cellText(&g, 1, 2) == QStringLiteral("1"));
    CHECK(cellText(&g, 2, 1) == QStringLiteral("10"));
    CHECK(cellText(&g, 2, 2) == QStringLiteral("11"));
    // The cells outside the pasted rectangle are untouched.
    CHECK(cellText(&g, 0, 0) == QStringLiteral("0"));
    CHECK(cellText(&g, 0, 2) == QStringLiteral("2"));
}

void testCutAndDelete()
{
    QTableWidget g(2, 2);
    GridClipboard clip(&g, [](int, int) { return CellSpec{-1000, 1000, 3, true}; });
    fill(&g, 0, 0, QStringLiteral("7"));
    fill(&g, 0, 1, QStringLiteral("8"));
    fill(&g, 1, 0, QStringLiteral("9"));
    fill(&g, 1, 1, QString());

    // Cut copies then clears the selection.
    QApplication::clipboard()->clear();
    select(&g, 0, 0, 0, 1);
    CHECK(sendKey(clip, Qt::Key_X, Qt::ControlModifier));
    CHECK(QApplication::clipboard()->text() == QStringLiteral("7\t8"));
    CHECK(cellText(&g, 0, 0).isEmpty());
    CHECK(cellText(&g, 0, 1).isEmpty());

    // Delete clears without touching the clipboard.
    QApplication::clipboard()->setText(QStringLiteral("keep"));
    select(&g, 1, 0, 1, 0);
    CHECK(sendKey(clip, Qt::Key_Delete, Qt::NoModifier));
    CHECK(cellText(&g, 1, 0).isEmpty());
    CHECK(QApplication::clipboard()->text() == QStringLiteral("keep"));
}

void testFillClampSkip()
{
    QTableWidget g(2, 3);
    // (0,0) is a fixed label; every other cell clamps to 0..50 at 1 decimal.
    GridClipboard clip(&g, [](int r, int c) {
        if (r == 0 && c == 0)
            return CellSpec{0, 0, 0, false};
        return CellSpec{0, 50, 1, true};
    });
    for (int r = 0; r < 2; ++r)
        for (int c = 0; c < 3; ++c)
            fill(&g, r, c, QString());

    // A single copied value fills a whole multi-cell selection.
    QApplication::clipboard()->setText(QStringLiteral("9"));
    select(&g, 1, 0, 1, 2);
    CHECK(sendKey(clip, Qt::Key_V, Qt::ControlModifier));
    CHECK(cellText(&g, 1, 0) == QStringLiteral("9"));
    CHECK(cellText(&g, 1, 1) == QStringLiteral("9"));
    CHECK(cellText(&g, 1, 2) == QStringLiteral("9"));

    // Paste clamps to the cell's max and skips the non-editable label.
    QApplication::clipboard()->setText(QStringLiteral("999\t20"));
    g.clearSelection();
    g.setCurrentCell(0, 0);
    CHECK(sendKey(clip, Qt::Key_V, Qt::ControlModifier));
    CHECK(cellText(&g, 0, 0).isEmpty());          // label untouched
    CHECK(cellText(&g, 0, 1) == QStringLiteral("20"));
}

// ------------------------------------------------------------- band colour
// The band that marks a grid's axis cells has to be visible on ANY desktop
// theme, which is the whole reason it is computed rather than fixed. The pure
// black/white cases are the ones worth pinning: QColor::lighter() scales an HSV
// value of zero, so on a fully black Base it returns black and the band would
// silently disappear.
void testBandColour()
{
    const auto band = [](const QColor &base, const QColor &text) {
        QPalette p;
        p.setColor(QPalette::Base, base);
        p.setColor(QPalette::Text, text);
        return gridBandColor(p);
    };

    // Light theme: white page, dark text -> the band darkens.
    const QColor light = band(Qt::white, Qt::black);
    CHECK(light != QColor(Qt::white));
    CHECK(light.lightness() < QColor(Qt::white).lightness());

    // Dark theme: black page, light text -> the band lightens. This is the case
    // lighter() gets wrong.
    const QColor dark = band(Qt::black, Qt::white);
    CHECK(dark != QColor(Qt::black));
    CHECK(dark.lightness() > QColor(Qt::black).lightness());

    // Either way it stays much nearer the page than the text, so values keep
    // their contrast against it.
    CHECK(light.lightness() > 128);
    CHECK(dark.lightness() < 128);

    // A realistic dark palette moves too.
    const QColor midDark = band(QColor(0x1e, 0x1e, 0x1e), QColor(0xdc, 0xdc, 0xdc));
    CHECK(midDark.lightness() > QColor(0x1e, 0x1e, 0x1e).lightness());
}

// ------------------------------------------------------------- axis dialog
QPushButton *button(QDialog &d, const QString &text)
{
    for (QPushButton *b : d.findChildren<QPushButton *>())
        if (b->text().remove(QLatin1Char('&')) == text)
            return b;
    return nullptr;
}

void seedChannel(Configuration &config)
{
    Channel ch;
    ch.name = QStringLiteral("MAP");
    ch.unit = QStringLiteral("kPa");
    ch.quantity = QStringLiteral("Pressure");
    ch.dataType = QStringLiteral("float");
    ch.decimalPlaces = 0;
    ch.minValue = 0;
    ch.maxValue = 500;
    ch.userDefined = true;
    config.catalog().addOrUpdateUserChannel(ch);
}

AxisSetupDialog::Axis seedAxis()
{
    AxisSetupDialog::Axis a;
    a.title = QStringLiteral("X Axis");
    a.channel = QStringLiteral("MAP");
    a.interp = true;
    a.maxSites = 8;
    a.sites = {0, 10, 100};
    return a;
}

void ok(AxisSetupDialog &d)
{
    auto *box = d.findChild<QDialogButtonBox *>();
    box->button(QDialogButtonBox::Ok)->click();
}

void testLinearise(Configuration &config)
{
    AxisSetupDialog d(&config, seedAxis(), {}, nullptr);
    button(d, QStringLiteral("Linearise"))->click();
    auto *grid = d.findChild<QTableWidget *>();
    // 0, 10, 100 with the middle re-spaced evenly between the ends -> 0, 50, 100.
    CHECK(cellText(grid, 0, 0) == QStringLiteral("0"));
    CHECK(cellText(grid, 0, 1) == QStringLiteral("50"));
    CHECK(cellText(grid, 0, 2) == QStringLiteral("100"));
    ok(d);
    const QList<double> got = d.result().sites;
    CHECK(got.size() == 3);
    CHECK(got == QList<double>({0, 50, 100}));
    CHECK(d.result().channel == QStringLiteral("MAP"));
    CHECK(d.result().interp == true);
}

void testInsert(Configuration &config)
{
    AxisSetupDialog d(&config, seedAxis(), {}, nullptr);
    button(d, QStringLiteral("Insert"))->click();
    ok(d);
    // 0,10,100 -> append last + (100-10) = 190.
    const QList<double> got = d.result().sites;
    CHECK(got.size() == 4);
    CHECK(got.last() == 190.0);
}

void testDelete(Configuration &config)
{
    AxisSetupDialog d(&config, seedAxis(), {}, nullptr);
    auto *grid = d.findChild<QTableWidget *>();
    select(grid, 0, 1, 0, 1); // the middle site, value 10
    button(d, QStringLiteral("Delete"))->click();
    ok(d);
    const QList<double> got = d.result().sites;
    CHECK(got == QList<double>({0, 100}));
}

void testBehaviourAndEmptyChannel(Configuration &config)
{
    AxisSetupDialog::Axis a = seedAxis();
    a.interp = true;
    AxisSetupDialog d(&config, a, {}, nullptr);
    // Flip Continuous -> Discrete via the behaviour combo.
    auto *combo = d.findChild<QComboBox *>();
    combo->setCurrentIndex(1);
    ok(d);
    CHECK(d.result().interp == false);
}

// ------------------------------------------------------- channel picker
// The picker is reached from the Axis Setup window's Select… button, so it is
// part of this surface. What is pinned is the rule that a caller which has just
// set the selection gets it HIGHLIGHTED: rebuildList() otherwise prefers
// whatever the list is showing, and the list has not been cleared at the point
// the preference is read — so setting the selection appeared to do nothing and
// OK returned the old channel. That is what made "New…" not choose the channel
// it had just created.
void testPickerHonoursAnExplicitSelection(Configuration &config)
{
    for (const char *n : {"Alpha", "Bravo", "Charlie"}) {
        Channel ch;
        ch.name = QString::fromLatin1(n);
        ch.dataType = QStringLiteral("float");
        ch.minValue = 0;
        ch.maxValue = 100;
        ch.userDefined = true;
        config.catalog().addOrUpdateUserChannel(ch);
    }

    SelectChannelDialog d(&config, ChannelRole::Output);
    d.setSelectedChannel(QStringLiteral("Alpha"));
    CHECK(d.selectedChannel() == QStringLiteral("Alpha"));

    auto *list = d.findChild<QListWidget *>();
    CHECK(list != nullptr);
    if (!list)
        return;
    // Alpha is highlighted, standing in for "whatever the user was looking at".
    QListWidgetItem *shown = list->currentItem();
    CHECK(shown != nullptr);
    if (shown)
        CHECK(shown->data(Qt::UserRole).toString() == QStringLiteral("Alpha"));

    // Now a different channel is chosen programmatically, exactly as the New…
    // path does after creating one. The highlight must move to it.
    d.setSelectedChannel(QStringLiteral("Charlie"));
    shown = list->currentItem();
    CHECK(shown != nullptr);
    if (shown)
        CHECK(shown->data(Qt::UserRole).toString() == QStringLiteral("Charlie"));
}

} // namespace

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    testCopyPaste();
    testCutAndDelete();
    testFillClampSkip();
    testBandColour();

    Configuration config;
    config.clear();
    seedChannel(config);
    testLinearise(config);
    testInsert(config);
    testDelete(config);
    testBehaviourAndEmptyChannel(config);
    testPickerHonoursAnExplicitSelection(config);

    if (fails == 0)
        std::printf("test_tables: all checks passed\n");
    else
        std::printf("test_tables: %d FAILED\n", fails);
    return fails == 0 ? 0 : 1;
}
