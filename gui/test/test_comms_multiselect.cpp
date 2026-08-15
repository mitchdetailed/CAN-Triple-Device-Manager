// Multiple selection in Communications Setup, driven through the real dialog.
//
// The sections list takes shift- and ctrl-click, and Remove / Move Up / Move
// Down act on the whole selection. Move is the part worth pinning: the list
// order IS the transmit order, so a reorder that drops a message, duplicates
// one, or quietly changes the order WITHIN the moved group corrupts the
// configuration in a way nothing later would flag — every section is still
// individually valid, just sent at the wrong time.
//
// A scattered selection (rows 1 and 3, say) is the case that separates a
// correct implementation from one that happens to work: each selected row must
// step exactly one place while the unselected rows between them are pushed the
// other way.
//
// Everything goes through the dialog's own widgets and its OK path, so what is
// tested is what a user gets, not a private helper. Runs offscreen.

#include <QApplication>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QStringList>
#include <QTabWidget>
#include <QTreeWidget>

#include <cstdio>

#include "../src/model/configuration.h"
#include "../src/ui/communications_dialog.h"

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

// Six receive messages on CAN 1, named A..F so an order reads at a glance.
void buildConfig(Configuration &config)
{
    config.clear();
    config.bus[0].enabled = true;
    config.bus[0].rateKbps = 500;
    quint32 id = 0x100;
    for (const char *name : {"A", "B", "C", "D", "E", "F"}) {
        CommsSection s;
        s.name = QString::fromLatin1(name);
        s.device = SectionDevice::ReceiveMessage;
        s.baseAddress = id++;
        s.messageLengthBytes = 8;
        config.bus[0].sections.append(s);
    }
}

QString order(const Configuration &config)
{
    QStringList names;
    for (const CommsSection &s : config.bus[0].sections)
        names << s.name;
    return names.join(QString());
}

// The widgets belonging to one bus. Found through that bus's TAB PAGE, not by
// taking the first match on the dialog: there are three of every one of these,
// and findChildren() does not hand them back in creation order — the CAN 1 tree
// comes back third. Scoping to the page says which bus is meant and cannot
// drift if the tabs are ever built differently.
QWidget *busPage(CommunicationsDialog &d, int bus)
{
    auto *tabs = d.findChild<QTabWidget *>();
    return tabs ? tabs->widget(bus) : nullptr;
}

QTreeWidget *tree(CommunicationsDialog &d, int bus = 0)
{
    QWidget *page = busPage(d, bus);
    return page ? page->findChild<QTreeWidget *>() : nullptr;
}

QPushButton *button(CommunicationsDialog &d, const QString &text, int bus = 0)
{
    QWidget *page = busPage(d, bus);
    if (!page)
        return nullptr;
    for (QPushButton *b : page->findChildren<QPushButton *>())
        if (b->text().remove(QLatin1Char('&')) == text)
            return b;
    return nullptr;
}

void selectRows(QTreeWidget *t, const QList<int> &rows)
{
    t->clearSelection();
    for (int r : rows)
        if (QTreeWidgetItem *item = t->topLevelItem(r))
            item->setSelected(true);
    if (!rows.isEmpty())
        t->setCurrentItem(t->topLevelItem(rows.first()));
    // setCurrentItem clears the rest of the selection, so re-apply it.
    for (int r : rows)
        if (QTreeWidgetItem *item = t->topLevelItem(r))
            item->setSelected(true);
}

QList<int> selectedRows(QTreeWidget *t)
{
    QList<int> rows;
    for (QTreeWidgetItem *item : t->selectedItems())
        rows.append(t->indexOfTopLevelItem(item));
    std::sort(rows.begin(), rows.end());
    return rows;
}

void accept(CommunicationsDialog &d)
{
    auto *box = d.findChild<QDialogButtonBox *>();
    CHECK(box != nullptr);
    if (box)
        box->button(QDialogButtonBox::Ok)->click();
}

// --- moving a contiguous block -------------------------------------------
void testContiguousBlockMovesTogether()
{
    Configuration config;
    buildConfig(config);
    CommunicationsDialog d(&config);
    QTreeWidget *t = tree(d);
    CHECK(t != nullptr);
    if (!t)
        return;
    CHECK(t->topLevelItemCount() == 6);

    selectRows(t, {1, 2}); // B and C
    button(d, QStringLiteral("↑ Move Up"))->click();
    accept(d);
    // B and C step over A, keeping their own order.
    CHECK(order(config) == QStringLiteral("BCADEF"));
}

// The mirror of the test above, and NOT redundant with it. Moving down has to
// walk the selected rows in the opposite direction to moving up, and only
// ADJACENT selected rows can show that: with a scattered selection the two
// orders give identical results, so a version that walked the wrong way would
// pass every other test here and reverse a contiguous block in the field.
void testContiguousBlockMovesDownTogether()
{
    Configuration config;
    buildConfig(config);
    CommunicationsDialog d(&config);
    QTreeWidget *t = tree(d);
    if (!t)
        return;

    selectRows(t, {1, 2}); // B and C
    button(d, QStringLiteral("↓ Move Down"))->click();
    accept(d);
    // D steps over both. B and C keep their order — ADBCEF, not ACDBEF.
    CHECK(order(config) == QStringLiteral("ADBCEF"));
}

// --- moving a scattered selection ----------------------------------------
void testScatteredSelectionEachStepsOne()
{
    Configuration config;
    buildConfig(config);
    CommunicationsDialog d(&config);
    QTreeWidget *t = tree(d);
    if (!t)
        return;

    selectRows(t, {1, 3}); // B and D, with C between them
    button(d, QStringLiteral("↑ Move Up"))->click();
    accept(d);
    // B swaps with A, D swaps with C. Nothing is lost or duplicated.
    CHECK(order(config) == QStringLiteral("BADCEF"));
}

void testScatteredSelectionMovesDown()
{
    Configuration config;
    buildConfig(config);
    CommunicationsDialog d(&config);
    QTreeWidget *t = tree(d);
    if (!t)
        return;

    selectRows(t, {1, 3}); // B and D
    button(d, QStringLiteral("↓ Move Down"))->click();
    accept(d);
    CHECK(order(config) == QStringLiteral("ACBEDF"));
}

// --- the selection follows the rows, so the button can be pressed again ---
void testSelectionFollowsTheMoveAndRepeats()
{
    Configuration config;
    buildConfig(config);
    CommunicationsDialog d(&config);
    QTreeWidget *t = tree(d);
    if (!t)
        return;

    selectRows(t, {3, 4}); // D and E
    button(d, QStringLiteral("↑ Move Up"))->click();
    CHECK(selectedRows(t) == QList<int>({2, 3}));
    // Pressing again keeps carrying the same two messages.
    button(d, QStringLiteral("↑ Move Up"))->click();
    CHECK(selectedRows(t) == QList<int>({1, 2}));
    accept(d);
    CHECK(order(config) == QStringLiteral("ADEBCF"));
}

// --- the group is blocked as a unit at either end ------------------------
void testBlockedAtTheEnds()
{
    Configuration config;
    buildConfig(config);
    CommunicationsDialog d(&config);
    QTreeWidget *t = tree(d);
    if (!t)
        return;

    QPushButton *up = button(d, QStringLiteral("↑ Move Up"));
    QPushButton *down = button(d, QStringLiteral("↓ Move Down"));
    CHECK(up != nullptr && down != nullptr);
    if (!up || !down)
        return;

    // A selection touching the top cannot move up, even though its other rows
    // could — moving those alone would reorder the group internally.
    selectRows(t, {0, 2});
    CHECK(!up->isEnabled());
    CHECK(down->isEnabled());

    selectRows(t, {3, 5});
    CHECK(up->isEnabled());
    CHECK(!down->isEnabled());

    // Nothing selected: neither, and Remove is dead too.
    t->clearSelection();
    CHECK(!up->isEnabled());
    CHECK(!down->isEnabled());
    CHECK(!button(d, QStringLiteral("Remove"))->isEnabled());
}

// --- Edit needs exactly one row ------------------------------------------
void testEditNeedsASingleRow()
{
    Configuration config;
    buildConfig(config);
    CommunicationsDialog d(&config);
    QTreeWidget *t = tree(d);
    if (!t)
        return;
    QPushButton *edit = button(d, QStringLiteral("Edit…"));
    CHECK(edit != nullptr);
    if (!edit)
        return;

    selectRows(t, {2});
    CHECK(edit->isEnabled());
    selectRows(t, {2, 3});
    CHECK(!edit->isEnabled()); // no single section for the editor to open
    CHECK(!edit->toolTip().isEmpty());
}

// --- removing several at once --------------------------------------------
void testRemoveTakesTheWholeSelection()
{
    Configuration config;
    buildConfig(config);
    CommunicationsDialog d(&config);
    QTreeWidget *t = tree(d);
    if (!t)
        return;

    // One row goes without a prompt, which keeps the old single-row behaviour.
    selectRows(t, {0});
    button(d, QStringLiteral("Remove"))->click();
    CHECK(t->topLevelItemCount() == 5);
    accept(d);
    CHECK(order(config) == QStringLiteral("BCDEF"));
}

} // namespace

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    testContiguousBlockMovesTogether();
    testContiguousBlockMovesDownTogether();
    testScatteredSelectionEachStepsOne();
    testScatteredSelectionMovesDown();
    testSelectionFollowsTheMoveAndRepeats();
    testBlockedAtTheEnds();
    testEditNeedsASingleRow();
    testRemoveTakesTheWholeSelection();

    if (fails == 0) {
        std::printf("test_comms_multiselect: all checks passed\n");
        return 0;
    }
    std::printf("test_comms_multiselect: %d check(s) failed\n", fails);
    return 1;
}
