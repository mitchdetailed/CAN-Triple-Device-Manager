// Communications Setup, driven through the real dialogs. Multiple selection
// first, and then the ORDER of the channel lists, which is the same subject
// seen from the other end: both are about a list whose index means something.
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
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDialogButtonBox>
#include <QListWidget>
#include <QPushButton>
#include <QStringList>
#include <QTabWidget>
#include <QSpinBox>
#include <QTimer>
#include <QTreeWidget>

#include <cstdio>

#include "../src/model/comms_types.h"
#include "../src/model/configuration.h"
#include "../src/ui/communications_dialog.h"
#include "../src/ui/section_editor_dialog.h"

static int fails = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);              \
            ++fails;                                                                 \
        }                                                                            \
    } while (0)

#define REQUIRE(cond)                                                                \
    do {                                                                             \
        if (!(cond)) {                                                               \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);              \
            ++fails;                                                                 \
            return;                                                                  \
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

// THE CHANNELS PANE IS ORDERED BY START BIT, not by row order.
//
// Row order is the order channels were added — after a DBC import, the order
// the .dbc listed them. Reading the frame off a list in that order meant
// holding the start bits in your head and sorting them there.
void testChannelPaneSortsByStartBit()
{
    Configuration config;
    config.clear();
    config.bus[0].enabled = true;

    for (const char *n : {"Alpha", "Bravo", "Charlie"}) {
        Channel c;
        c.name = QString::fromLatin1(n);
        c.dataType = QStringLiteral("u16");
        c.userDefined = true;
        config.catalog().addOrUpdateUserChannel(c);
    }

    // Added deliberately OUT of bit order, which is the case that separates a
    // sorted pane from one that merely looks sorted on tidy input.
    CommsSection s;
    s.name = QStringLiteral("Jumbled");
    s.device = SectionDevice::ReceiveMessage;
    s.baseAddress = 0x200;
    s.messageLengthBytes = 8;
    const auto row = [](const char *name, int startBit) {
        CommsChannelRow r;
        r.channelName = QString::fromLatin1(name);
        r.startBit = startBit;
        r.bitLength = 8;
        return r;
    };
    s.rows << row("Charlie", 32) << row("Alpha", 8) << row("Bravo", 16);
    config.bus[0].sections.append(s);

    CommunicationsDialog d(&config);
    QTreeWidget *sections = tree(d, 0);
    CHECK(sections != nullptr);
    if (!sections)
        return;
    sections->setCurrentItem(sections->topLevelItem(0));

    QWidget *page = busPage(d, 0);
    QListWidget *pane = page ? page->findChild<QListWidget *>() : nullptr;
    CHECK(pane != nullptr);
    if (!pane)
        return;

    QStringList shown;
    for (int i = 0; i < pane->count(); ++i)
        shown << pane->item(i)->text().trimmed();
    const QStringList wanted{QStringLiteral("Alpha"), QStringLiteral("Bravo"),
                             QStringLiteral("Charlie")};
    CHECK(shown == wanted);
    if (shown != wanted)
        std::printf("       got: %s\n", qPrintable(shown.join(QStringLiteral(" | "))));

    // THE ROWS THEMSELVES ARE UNTOUCHED. The pane is a view; reordering the
    // section would change the .ct3 and the editor's own list, which is not
    // what was asked for and would be a much larger promise.
    CHECK(config.bus[0].sections.first().rows.at(0).channelName == QStringLiteral("Charlie"));
    CHECK(config.bus[0].sections.first().rows.at(2).channelName == QStringLiteral("Bravo"));
}

// A NEW message opens with its Message Type set to OFF.
//
// Worth a test for a one-line default because it is the kind of thing that
// drifts back silently: nothing fails if it reverts, the editor just quietly
// starts pre-answering a question the user is supposed to answer. New… opens a
// MODAL editor, so the check runs from a timer inside that modal loop.
void testNewOpensAsOff()
{
    Configuration config;
    buildConfig(config);
    CommunicationsDialog d(&config);
    QPushButton *newButton = button(d, QStringLiteral("New…"));
    CHECK(newButton != nullptr);
    if (!newButton)
        return;

    int seen = -2;   // -2 = the editor was never found
    bool wedged = false;
    QTimer timer;
    int ticks = 0;
    QObject::connect(&timer, &QTimer::timeout, [&]() {
        QWidget *modal = QApplication::activeModalWidget();
        if (!modal) {
            if (++ticks > 400)
                timer.stop();
            return;
        }
        // The Message Type combo is the one offering "Off" — the Channels tab
        // has a "Message Type" group of its own (Single / Compound), so the
        // label is not enough to pick it out and the ITEMS are.
        for (QComboBox *combo : modal->findChildren<QComboBox *>()) {
            bool offersOff = false;
            for (int i = 0; i < combo->count(); ++i)
                offersOff = offersOff || combo->itemText(i) == QStringLiteral("Off");
            if (!offersOff)
                continue;
            seen = combo->currentData().toInt();
            break;
        }
        if (seen == -2) {
            wedged = true; // an editor with no such combo: the test cannot speak
            seen = -3;
        }
        modal->close(); // Cancel: nothing is added, the document is untouched
        timer.stop();
    });
    timer.start(5);
    newButton->click(); // blocks in the editor's modal loop until the timer closes it

    CHECK(!wedged);
    CHECK(seen == int(ct::SectionDevice::Off));
    if (seen != int(ct::SectionDevice::Off) && seen >= 0)
        std::printf("       Message Type came up as %d, wanted Off (%d)\n", seen,
                    int(ct::SectionDevice::Off));
    // Cancelled, so nothing was added — the six A..F messages and no more.
    CHECK(config.bus[0].sections.size() == 6);
}

// A button anywhere in a dialog, by its text. The bus-scoped button() above
// cannot serve: these dialogs have no bus tabs.
QPushButton *buttonIn(QWidget &w, const QString &text)
{
    for (QPushButton *b : w.findChildren<QPushButton *>())
        if (b->text() == text)
            return b;
    return nullptr;
}

QListWidget *channelList(SectionEditorDialog &d)
{
    return d.findChild<QListWidget *>();
}

// Add... / Change... / Remove BESIDE THE CHANNEL LIST. The identifier table has
// a Change... of its own and it is created first, so matching on the text alone
// finds that one and opens the wrong editor. These three are laid out in the
// same column as the list and so share its parent; the identifier buttons sit
// on the compound page inside the Single/Compound stack. (Ancestry alone will
// not separate them: a QTabWidget is a QStackedWidget underneath, so everything
// on this tab has one above it.)
QPushButton *rowButton(SectionEditorDialog &d, const QString &text)
{
    QListWidget *list = channelList(d);
    if (!list)
        return nullptr;
    for (QPushButton *b : d.findChildren<QPushButton *>())
        if (b->text() == text && b->parentWidget() == list->parentWidget())
            return b;
    return nullptr;
}

QStringList listedNames(QListWidget *list)
{
    QStringList names;
    for (int i = 0; i < list->count(); ++i)
        names << list->item(i)->text().section(QLatin1Char(' '), 0, 0);
    return names;
}

void buildOrderConfig(Configuration &config)
{
    config.clear();
    config.bus[0].enabled = true;
    config.bus[0].rateKbps = 500;
    for (const char *n : {"Alpha", "Bravo", "Charlie", "delta", "Echo", "Zulu"}) {
        Channel c;
        c.name = QString::fromLatin1(n);
        c.dataType = QStringLiteral("u16");
        c.baseResolution = 1.0;
        c.minValue = 0;
        c.maxValue = 1000;
        c.userDefined = true;
        config.catalog().addOrUpdateUserChannel(c);
    }
}

CommsChannelRow orderRow(const char *name, int startBit, int bitLength)
{
    CommsChannelRow r;
    r.channelName = QString::fromLatin1(name);
    r.startBit = startBit;
    r.bitLength = bitLength;
    return r;
}

CommsSection orderSection()
{
    CommsSection s;
    s.name = QStringLiteral("Jumbled");
    s.device = SectionDevice::ReceiveMessage;
    s.baseAddress = 0x640;
    s.messageLengthBytes = 8;
    return s;
}

void testTheEditorListIsInFrameOrder()
{
    // Deliberately out of order on the way in, which is the case that separates
    // a sorted list from one that merely looks sorted on tidy input.
    Configuration config;
    buildOrderConfig(config);
    CommsSection s = orderSection();
    s.rows << orderRow("Charlie", 32, 8) << orderRow("Alpha", 0, 8)
           << orderRow("Bravo", 16, 8);

    SectionEditorDialog dialog(&config, s, 0, {}, -1);
    QListWidget *list = channelList(dialog);
    REQUIRE(list != nullptr);
    const QStringList shown = listedNames(list);
    std::printf("  editor list          : %s\n", qPrintable(shown.join(QStringLiteral(" "))));
    CHECK(shown == QStringList({QStringLiteral("Alpha"), QStringLiteral("Bravo"),
                                QStringLiteral("Charlie")}));
}

void testTheTieBreaksDecideTheRest()
{
    // Start bit alone does not order this list, and each tie-break has to be
    // the one that decides its pair. Three rows share bit 0, chosen so that
    // every wrong rule gives a different answer:
    //
    //   Alpha 16 bits, Echo 8, delta 8 — LENGTH must beat name, or the early
    //     name Alpha would come before the narrower Echo.
    //   Echo and delta, same bits and same width — the name must be compared
    //     CASE-INSENSITIVELY, or 'E' (69) sorts before 'd' (100) and Echo
    //     jumps the queue.
    //   Bravo at bit 32 — the primary key still has to run.
    //
    // Fed in an order that no single-key rule reproduces.
    Configuration config;
    buildOrderConfig(config);
    CommsSection s = orderSection();
    s.rows << orderRow("Alpha", 0, 16) << orderRow("Echo", 0, 8)
           << orderRow("delta", 0, 8) << orderRow("Bravo", 32, 8);

    SectionEditorDialog dialog(&config, s, 0, {}, -1);
    QListWidget *list = channelList(dialog);
    REQUIRE(list != nullptr);
    const QStringList shown = listedNames(list);
    std::printf("  tie-breaks           : %s\n", qPrintable(shown.join(QStringLiteral(" "))));
    CHECK(shown == QStringList({QStringLiteral("delta"), QStringLiteral("Echo"),
                                QStringLiteral("Alpha"), QStringLiteral("Bravo")}));
}

void testTheOrderIsWrittenBackNotJustShown()
{
    // The list indexes STRAIGHT into the section's rows — the selected row is row
    // N of the section, and N is also the row's colour in the frame map — so a
    // list sorted differently from the rows underneath would need every one of
    // those sites to translate. The rows themselves are sorted instead, and this
    // is what says so.
    Configuration config;
    buildOrderConfig(config);
    CommsSection s = orderSection();
    s.rows << orderRow("Charlie", 32, 8) << orderRow("Alpha", 0, 8)
           << orderRow("Bravo", 16, 8);

    SectionEditorDialog dialog(&config, s, 0, {}, -1);
    auto *box = dialog.findChild<QDialogButtonBox *>();
    REQUIRE(box != nullptr && box->button(QDialogButtonBox::Ok) != nullptr);
    box->button(QDialogButtonBox::Ok)->click();
    REQUIRE(dialog.result() == QDialog::Accepted);

    const QList<CommsChannelRow> &out = dialog.section().rows;
    REQUIRE(out.size() == 3); // sorted, not deduplicated and not dropped
    CHECK(out.at(0).channelName == QStringLiteral("Alpha"));
    CHECK(out.at(1).channelName == QStringLiteral("Bravo"));
    CHECK(out.at(2).channelName == QStringLiteral("Charlie"));
}

void testEveryIdentifierIsSortedNotOnlyTheVisibleOne()
{
    // A compound section shows one variant at a time, so only identifier 1 is
    // ever drawn here. Sorting just the visible list would save a message whose
    // variants were ordered or not according to which ones the user happened to
    // click on. And no row may cross between them: the identifiers are
    // alternative frames, not one list with headers.
    Configuration config;
    buildOrderConfig(config);
    CommsSection s = orderSection();
    s.compound = true;
    for (int i = 0; i < 16; ++i)
        s.identifiers.append(CompoundIdentifier{});
    s.identifiers[0].configured = true;
    s.identifiers[0].byteOffset = 0;
    s.identifiers[0].id = 1;
    s.identifiers[0].rows << orderRow("Charlie", 32, 8) << orderRow("Bravo", 16, 8);
    s.identifiers[1].configured = true;
    s.identifiers[1].byteOffset = 0;
    s.identifiers[1].id = 2;
    s.identifiers[1].rows << orderRow("Echo", 40, 8) << orderRow("delta", 24, 8);

    SectionEditorDialog dialog(&config, s, 0, {}, -1);
    auto *box = dialog.findChild<QDialogButtonBox *>();
    REQUIRE(box != nullptr && box->button(QDialogButtonBox::Ok) != nullptr);
    box->button(QDialogButtonBox::Ok)->click();
    REQUIRE(dialog.result() == QDialog::Accepted);

    const CommsSection out = dialog.section();
    REQUIRE(out.identifiers.size() >= 2);
    REQUIRE(out.identifiers.at(0).rows.size() == 2);
    REQUIRE(out.identifiers.at(1).rows.size() == 2);
    CHECK(out.identifiers.at(0).rows.at(0).channelName == QStringLiteral("Bravo"));
    CHECK(out.identifiers.at(0).rows.at(1).channelName == QStringLiteral("Charlie"));
    CHECK(out.identifiers.at(1).rows.at(0).channelName == QStringLiteral("delta"));
    CHECK(out.identifiers.at(1).rows.at(1).channelName == QStringLiteral("Echo"));
}

void testChangingAStartBitCarriesTheSelectionWithIt()
{
    // THE REASON THE SELECTION CANNOT SIMPLY BE RE-INDEXED. Editing a row's
    // Start Bit moves it in a sorted list, so putting the highlight back on the
    // old index would leave it on whichever channel slid into that place — and
    // the frame map follows the highlight, so the shading would then point at
    // the wrong signal.
    Configuration config;
    buildOrderConfig(config);
    CommsSection s = orderSection();
    s.rows << orderRow("Alpha", 0, 8) << orderRow("Bravo", 16, 8)
           << orderRow("Charlie", 32, 8);

    SectionEditorDialog dialog(&config, s, 0, {}, -1);
    QListWidget *list = channelList(dialog);
    QPushButton *change = rowButton(dialog, QStringLiteral("Change…"));
    REQUIRE(list != nullptr);
    REQUIRE(change != nullptr);
    list->setCurrentRow(2); // Charlie, last at bit 32

    // Inside the row editor: move Charlie to bit 8, between Alpha and Bravo.
    // The Start Bit box is found by the value it holds rather than by position
    // among the spin boxes, which findChildren() does not promise to order.
    bool moved = false;
    QTimer timer;
    int ticks = 0;
    QObject::connect(&timer, &QTimer::timeout, [&]() {
        QWidget *modal = QApplication::activeModalWidget();
        if (!modal) {
            if (++ticks > 400)
                timer.stop();
            return;
        }
        for (QSpinBox *spin : modal->findChildren<QSpinBox *>()) {
            if (spin->value() == 32) {
                spin->setValue(8);
                moved = true;
                break;
            }
        }
        auto *box = modal->findChild<QDialogButtonBox *>();
        if (moved && box && box->button(QDialogButtonBox::Ok))
            box->button(QDialogButtonBox::Ok)->click();
        else
            modal->close();
        timer.stop();
    });
    timer.start(5);
    change->click(); // blocks in the row editor's modal loop

    REQUIRE(moved);
    const QStringList shown = listedNames(list);
    std::printf("  after moving Charlie : %s (row %d)\n",
                qPrintable(shown.join(QStringLiteral(" "))), list->currentRow());
    CHECK(shown == QStringList({QStringLiteral("Alpha"), QStringLiteral("Charlie"),
                                QStringLiteral("Bravo")}));
    CHECK(list->currentRow() == 1); // the highlight followed the row, not the index
}

void testARenameDuringChangeDoesNotMoveTheRowUnderTheEdit()
{
    // WHY THE SORT IS NOT INSIDE rebuildChannelList().
    //
    // onChangeRow holds a row INDEX across the modal row editor, and that
    // editor's channel picker can commit a rename to the document while it is
    // open — which is the whole point of Configuration::channelRenamed: every
    // dialog with a private working copy rewrites its own rows and redraws.
    // A name is the third sort key, so a redraw that also re-sorted would move
    // rows out from under that index, and the edit would be written over
    // whichever channel had slid into the slot — one the user never opened.
    //
    // Two rows share bit 0 and a width here, so the name alone decides their
    // order and a rename really does reorder them.
    Configuration config;
    buildOrderConfig(config);
    CommsSection s = orderSection();
    s.rows << orderRow("delta", 0, 8) << orderRow("Echo", 0, 8);

    SectionEditorDialog dialog(&config, s, 0, {}, -1);
    QListWidget *list = channelList(dialog);
    QPushButton *change = rowButton(dialog, QStringLiteral("Change…"));
    REQUIRE(list != nullptr);
    REQUIRE(change != nullptr);
    REQUIRE(listedNames(list)
            == QStringList({QStringLiteral("delta"), QStringLiteral("Echo")}));
    list->setCurrentRow(1); // Echo, the second of the pair

    // Inside the row editor, rename the OTHER row's channel to something that
    // sorts after Echo. This is what the picker's Edit... does: it commits to
    // the document immediately, so the new name is selectable.
    bool renamed = false;
    QTimer timer;
    int ticks = 0;
    QObject::connect(&timer, &QTimer::timeout, [&]() {
        QWidget *modal = QApplication::activeModalWidget();
        if (!modal) {
            if (++ticks > 400)
                timer.stop();
            return;
        }
        config.renameChannelReferences(QStringLiteral("delta"), QStringLiteral("Zulu"));
        renamed = true;
        auto *box = modal->findChild<QDialogButtonBox *>();
        if (box && box->button(QDialogButtonBox::Ok))
            box->button(QDialogButtonBox::Ok)->click(); // accept Echo unchanged
        else
            modal->close();
        timer.stop();
    });
    timer.start(5);
    change->click();

    REQUIRE(renamed);
    const QStringList after = listedNames(list);
    std::printf("  rename mid-Change    : %s\n", qPrintable(after.join(QStringLiteral(" "))));
    // Both rows survive, and each keeps its own channel. The failure this
    // guards against writes Echo's row over the renamed one and leaves two
    // Echoes — a channel silently deleted by an edit to a different row.
    CHECK(after == QStringList({QStringLiteral("Echo"), QStringLiteral("Zulu")}));
}

void testThePaneAndTheEditorAgree()
{
    // The two views of one message, on the same comparator. They would have
    // diverged exactly on the tie-breaks: the pane used to sort by start bit
    // alone, which leaves rows sharing one in file order.
    Configuration config;
    buildOrderConfig(config);
    CommsSection s = orderSection();
    s.rows << orderRow("Alpha", 0, 16) << orderRow("Echo", 0, 8)
           << orderRow("delta", 0, 8) << orderRow("Bravo", 32, 8);
    config.bus[0].sections.append(s);

    SectionEditorDialog editor(&config, s, 0, {}, -1);
    QListWidget *editorList = channelList(editor);
    REQUIRE(editorList != nullptr);

    CommunicationsDialog comms(&config);
    QTreeWidget *sections = tree(comms, 0);
    REQUIRE(sections != nullptr);
    sections->setCurrentItem(sections->topLevelItem(0));
    QWidget *page = busPage(comms, 0);
    QListWidget *pane = page ? page->findChild<QListWidget *>() : nullptr;
    REQUIRE(pane != nullptr);

    QStringList paneNames;
    for (int i = 0; i < pane->count(); ++i)
        paneNames << pane->item(i)->text().section(QLatin1Char(' '), 0, 0);
    std::printf("  pane                 : %s\n", qPrintable(paneNames.join(QStringLiteral(" "))));
    CHECK(paneNames == listedNames(editorList));
}

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
    testNewOpensAsOff();
    testChannelPaneSortsByStartBit();
    testTheEditorListIsInFrameOrder();
    testTheTieBreaksDecideTheRest();
    testTheOrderIsWrittenBackNotJustShown();
    testEveryIdentifierIsSortedNotOnlyTheVisibleOne();
    testChangingAStartBitCarriesTheSelectionWithIt();
    testARenameDuringChangeDoesNotMoveTheRowUnderTheEdit();
    testThePaneAndTheEditorAgree();

    if (fails == 0) {
        std::printf("test_comms_multiselect: all checks passed\n");
        return 0;
    }
    std::printf("test_comms_multiselect: %d check(s) failed\n", fails);
    return 1;
}
