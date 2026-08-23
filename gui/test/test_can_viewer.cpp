// The CAN Viewer's display filters and Overwrite Mode, driven through the real
// dialog: frames go in as the device's monitor stream delivers them, and every
// assertion reads the table a user would be looking at.
//
// Two properties carry most of the weight here.
//
// The first is that filtering is a VIEW, never a loss. The capture buffer is
// what Save to File writes, so a filter that quietly dropped frames on their
// way into it would cost a trace that was already recorded — and the damage
// would only surface later, in a file, with nothing left to compare against.
// Every filter test therefore unticks, checks the view shrank, re-ticks, and
// checks the history came back rather than resuming from that moment.
//
// The second is that Overwrite Mode's row for an identifier holds its LATEST
// frame. A stale row in a view whose entire purpose is "what is the value right
// now" is worse than no row: it reads as live data. The tests send the same ID
// repeatedly with changing payloads and pin the row to the last one — including
// across a rebuild, which is the path a new identifier takes.
//
// Identity is (bus, ID, extended, direction). Standard 0x100 and extended 0x100
// are different frames on the wire, and a gateway's own transmit of an ID it
// also receives is not the same traffic, so each gets its own row. Collapsing
// either pair would show one value where there are two.
//
// Runs offscreen; no device is opened. DeviceLink is constructed but never
// connected — the dialog only listens to its monitorFrame signal, which the
// test emits directly.

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStringList>
#include <QTableWidget>

#include <cstdio>
#include <cstring>

#include "../src/protocol/device_link.h"
#include "../src/ui/can_viewer_dialog.h"

static int fails = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);              \
            ++fails;                                                                 \
        }                                                                            \
    } while (0)

namespace {

enum Column { ColTime = 0, ColBus, ColDir, ColId, ColLen, ColFrameCount, ColData };

// A monitor-stream frame. `data` is given as bytes; data_len follows it.
ct::MonitorStreamPayload frame(quint8 bus, quint8 direction, quint32 canId,
                               const QList<quint8> &data, quint8 flags = 0,
                               quint32 timestampMs = 1000)
{
    ct::MonitorStreamPayload f;
    std::memset(&f, 0, sizeof(f));
    f.timestamp_ms = timestampMs;
    f.bus_idx = bus;
    f.direction = direction;
    f.can_id = canId;
    f.flags = flags;
    f.data_len = static_cast<uint8_t>(data.size());
    for (int i = 0; i < data.size() && i < 64; ++i)
        f.data[i] = data.at(i);
    return f;
}

QCheckBox *box(QWidget *w, const QString &objectName)
{
    auto *c = w->findChild<QCheckBox *>(objectName);
    Q_ASSERT(c);
    return c;
}

QTableWidget *table(QWidget *w)
{
    auto *t = w->findChild<QTableWidget *>(QStringLiteral("frameTable"));
    Q_ASSERT(t);
    return t;
}

QString cell(QTableWidget *t, int row, int col)
{
    QTableWidgetItem *item = t->item(row, col);
    return item ? item->text() : QString();
}

// The whole table as "ID/Dir/Data" per row, joined — one string to compare a
// view against, so a failure prints what is actually on screen.
QString view(QTableWidget *t)
{
    QStringList rows;
    for (int r = 0; r < t->rowCount(); ++r) {
        rows << QStringLiteral("%1 %2 %3 [%4]")
                    .arg(cell(t, r, ColBus), cell(t, r, ColDir), cell(t, r, ColId),
                         cell(t, r, ColData));
    }
    return rows.join(QStringLiteral(" | "));
}

// ---------------------------------------------------------------------------

// Tx rows are shown by default — the behaviour every earlier release had, and
// the half of a gateway's traffic you are usually checking.
void testTxMessagesAreShownByDefault()
{
    ct::DeviceLink link;
    ct::CanViewerDialog d(&link);

    CHECK(box(&d, QStringLiteral("txCheck"))->isChecked());

    emit link.monitorFrame(frame(1, 0, 0x100, {0x11}));
    emit link.monitorFrame(frame(1, 1, 0x200, {0x22}));

    QTableWidget *t = table(&d);
    CHECK(t->rowCount() == 2);
    CHECK(cell(t, 0, ColDir) == QStringLiteral("Rx"));
    CHECK(cell(t, 1, ColDir) == QStringLiteral("Tx"));
}

// Unticking Tx Msgs hides the device's own frames and nothing else; re-ticking
// brings back the ones that arrived while it was off, because the list is
// rebuilt from the capture buffer rather than resumed.
void testTxFilterHidesAndRestores()
{
    ct::DeviceLink link;
    ct::CanViewerDialog d(&link);
    QTableWidget *t = table(&d);

    emit link.monitorFrame(frame(1, 0, 0x100, {0x11}));
    emit link.monitorFrame(frame(1, 1, 0x200, {0x22}));
    CHECK(t->rowCount() == 2);

    box(&d, QStringLiteral("txCheck"))->setChecked(false);
    CHECK(t->rowCount() == 1);
    CHECK(cell(t, 0, ColId) == QStringLiteral("0x100"));

    // Frames that arrive while Tx is hidden are still captured...
    emit link.monitorFrame(frame(1, 1, 0x300, {0x33}));
    emit link.monitorFrame(frame(1, 0, 0x400, {0x44}));
    CHECK(t->rowCount() == 2); // only the Rx one appeared

    // ...and re-ticking shows them, in their original order.
    box(&d, QStringLiteral("txCheck"))->setChecked(true);
    CHECK(t->rowCount() == 4);
    CHECK(cell(t, 0, ColId) == QStringLiteral("0x100"));
    CHECK(cell(t, 1, ColId) == QStringLiteral("0x200"));
    CHECK(cell(t, 2, ColId) == QStringLiteral("0x300"));
    CHECK(cell(t, 3, ColId) == QStringLiteral("0x400"));
}

// The bus filter and the Tx filter both apply; neither one substitutes for the
// other. A Tx frame on a hidden bus stays hidden when Tx is re-shown.
void testBusAndTxFiltersCompose()
{
    ct::DeviceLink link;
    ct::CanViewerDialog d(&link);
    QTableWidget *t = table(&d);

    emit link.monitorFrame(frame(1, 0, 0x100, {0x11}));
    emit link.monitorFrame(frame(1, 1, 0x101, {0x11}));
    emit link.monitorFrame(frame(2, 0, 0x200, {0x22}));
    emit link.monitorFrame(frame(2, 1, 0x201, {0x22}));
    CHECK(t->rowCount() == 4);

    box(&d, QStringLiteral("busCheck2"))->setChecked(false);
    CHECK(t->rowCount() == 2); // CAN 1 only, both directions

    box(&d, QStringLiteral("txCheck"))->setChecked(false);
    CHECK(t->rowCount() == 1);
    CHECK(cell(t, 0, ColId) == QStringLiteral("0x100"));

    box(&d, QStringLiteral("txCheck"))->setChecked(true);
    CHECK(t->rowCount() == 2); // CAN 2 is still hidden
    CHECK(view(t) == QStringLiteral("CAN 1 Rx 0x100 [11] | CAN 1 Tx 0x101 [11]"));
}

// Overwrite Mode collapses a trace to one row per identifier carrying the most
// recent payload. This is the core of the feature: six frames, three rows.
void testOverwriteModeKeepsOnlyTheLatestFrame()
{
    ct::DeviceLink link;
    ct::CanViewerDialog d(&link);
    QTableWidget *t = table(&d);

    emit link.monitorFrame(frame(1, 0, 0x100, {0x01}));
    emit link.monitorFrame(frame(1, 0, 0x200, {0x0A}));
    emit link.monitorFrame(frame(1, 0, 0x100, {0x02}));
    emit link.monitorFrame(frame(1, 0, 0x300, {0xFF}));
    emit link.monitorFrame(frame(1, 0, 0x100, {0x03}));
    emit link.monitorFrame(frame(1, 0, 0x200, {0x0B}));
    CHECK(t->rowCount() == 6); // scrolling view: one row per frame

    box(&d, QStringLiteral("overwriteCheck"))->setChecked(true);
    CHECK(t->rowCount() == 3);
    CHECK(view(t) == QStringLiteral("CAN 1 Rx 0x100 [03] | CAN 1 Rx 0x200 [0B] | "
                                    "CAN 1 Rx 0x300 [FF]"));

    // Still live: a further frame rewrites its row in place and adds no other.
    emit link.monitorFrame(frame(1, 0, 0x100, {0x04}));
    CHECK(t->rowCount() == 3);
    CHECK(cell(t, 0, ColData) == QStringLiteral("04"));

    // And untickng gives the whole trace back — the capture was never touched.
    box(&d, QStringLiteral("overwriteCheck"))->setChecked(false);
    CHECK(t->rowCount() == 7);
}

// Ordering is by bus, then by arbitration ID — regardless of arrival order.
void testOverwriteModeSortsByBusThenId()
{
    ct::DeviceLink link;
    ct::CanViewerDialog d(&link);
    QTableWidget *t = table(&d);
    box(&d, QStringLiteral("overwriteCheck"))->setChecked(true);

    // Deliberately scrambled: highest bus first, descending IDs within it.
    emit link.monitorFrame(frame(3, 0, 0x700, {0x30}));
    emit link.monitorFrame(frame(1, 0, 0x400, {0x14}));
    emit link.monitorFrame(frame(2, 0, 0x050, {0x20}));
    emit link.monitorFrame(frame(1, 0, 0x0A0, {0x1A}));
    emit link.monitorFrame(frame(3, 0, 0x001, {0x31}));
    emit link.monitorFrame(frame(1, 0, 0x200, {0x12}));

    CHECK(t->rowCount() == 6);
    QStringList ids;
    for (int r = 0; r < t->rowCount(); ++r)
        ids << cell(t, r, ColBus) + QLatin1Char(' ') + cell(t, r, ColId);
    CHECK(ids.join(QStringLiteral(",")) ==
          QStringLiteral("CAN 1 0x0A0,CAN 1 0x200,CAN 1 0x400,"
                         "CAN 2 0x050,CAN 3 0x001,CAN 3 0x700"));
}

// An identifier that sorts BEFORE rows already on screen must push them down,
// not overwrite them. This is the rebuild path, and getting it wrong would put
// one message's data on another message's row — the failure this whole view is
// least able to survive, because nothing on screen would look wrong.
void testNewIdentifierInsertsInOrderWithoutDisturbingOthers()
{
    ct::DeviceLink link;
    ct::CanViewerDialog d(&link);
    QTableWidget *t = table(&d);
    box(&d, QStringLiteral("overwriteCheck"))->setChecked(true);

    emit link.monitorFrame(frame(1, 0, 0x500, {0x55}));
    emit link.monitorFrame(frame(1, 0, 0x600, {0x66}));
    CHECK(view(t) == QStringLiteral("CAN 1 Rx 0x500 [55] | CAN 1 Rx 0x600 [66]"));

    emit link.monitorFrame(frame(1, 0, 0x100, {0x11})); // sorts to the top
    CHECK(view(t) == QStringLiteral("CAN 1 Rx 0x100 [11] | CAN 1 Rx 0x500 [55] | "
                                    "CAN 1 Rx 0x600 [66]"));

    // The rows that moved must still update to their OWN latest frame: the
    // insert rewrote every row index, and a stale index would send this frame
    // to whichever row now sits where 0x500 used to be — row 0, holding 0x100.
    emit link.monitorFrame(frame(1, 0, 0x500, {0x5F}));
    CHECK(view(t) == QStringLiteral("CAN 1 Rx 0x100 [11] | CAN 1 Rx 0x500 [5F] | "
                                    "CAN 1 Rx 0x600 [66]"));
}

// Rx 0x100 and Tx 0x100 are different traffic and get different rows; so do a
// standard and an extended ID that happen to share a number. Collapsing either
// pair would show one value where there are two.
void testIdentitySeparatesDirectionAndExtendedFlag()
{
    ct::DeviceLink link;
    ct::CanViewerDialog d(&link);
    QTableWidget *t = table(&d);
    box(&d, QStringLiteral("overwriteCheck"))->setChecked(true);

    emit link.monitorFrame(frame(1, 0, 0x100, {0xAA}));
    emit link.monitorFrame(frame(1, 1, 0x100, {0xBB}));
    emit link.monitorFrame(frame(1, 0, 0x100, {0xCC}, ct::MONFLAG_EXTENDED));
    CHECK(t->rowCount() == 3);

    // Extended sorts after standard at the same numeric ID, and the width of
    // the printed ID is what tells them apart.
    CHECK(view(t) == QStringLiteral("CAN 1 Rx 0x100 [AA] | CAN 1 Tx 0x100 [BB] | "
                                    "CAN 1 Rx 0x00000100 [CC]"));

    // Each keeps its own latest value.
    emit link.monitorFrame(frame(1, 1, 0x100, {0xB1}));
    CHECK(cell(t, 0, ColData) == QStringLiteral("AA"));
    CHECK(cell(t, 1, ColData) == QStringLiteral("B1"));
    CHECK(cell(t, 2, ColData) == QStringLiteral("CC"));
}

// The Count column counts frames per identifier, is visible only in Overwrite
// Mode, and survives toggling the mode — the counts describe the same capture,
// so restarting them at every toggle would misreport a quiet message as new.
void testCountColumn()
{
    ct::DeviceLink link;
    ct::CanViewerDialog d(&link);
    QTableWidget *t = table(&d);

    CHECK(t->isColumnHidden(ColFrameCount)); // scrolling view: every row is one frame

    for (int i = 0; i < 5; ++i)
        emit link.monitorFrame(frame(1, 0, 0x100, {quint8(i)}));
    emit link.monitorFrame(frame(1, 0, 0x200, {0x01}));

    box(&d, QStringLiteral("overwriteCheck"))->setChecked(true);
    CHECK(!t->isColumnHidden(ColFrameCount));
    CHECK(cell(t, 0, ColFrameCount) == QStringLiteral("5"));
    CHECK(cell(t, 1, ColFrameCount) == QStringLiteral("1"));

    // Counting continues in the mode, and through a trip out and back.
    emit link.monitorFrame(frame(1, 0, 0x100, {0x09}));
    CHECK(cell(t, 0, ColFrameCount) == QStringLiteral("6"));
    box(&d, QStringLiteral("overwriteCheck"))->setChecked(false);
    CHECK(t->isColumnHidden(ColFrameCount));
    box(&d, QStringLiteral("overwriteCheck"))->setChecked(true);
    CHECK(cell(t, 0, ColFrameCount) == QStringLiteral("6"));
}

// Frames that arrive while Overwrite Mode is OFF are already summarised when it
// is switched on: the map is maintained in both modes, so the view opens on the
// bus as it stands instead of filling in over the next few seconds.
void testOverwriteModeOpensOnTheCurrentState()
{
    ct::DeviceLink link;
    ct::CanViewerDialog d(&link);
    QTableWidget *t = table(&d);

    emit link.monitorFrame(frame(2, 0, 0x310, {0x01, 0x02}));
    emit link.monitorFrame(frame(2, 0, 0x310, {0x03, 0x04}));

    box(&d, QStringLiteral("overwriteCheck"))->setChecked(true);
    CHECK(t->rowCount() == 1);
    CHECK(cell(t, 0, ColData) == QStringLiteral("03 04"));
    CHECK(cell(t, 0, ColFrameCount) == QStringLiteral("2"));
}

// The display filters apply in Overwrite Mode too, and re-ticking restores the
// row with its full history rather than an empty one waiting to be refilled.
void testFiltersApplyInOverwriteMode()
{
    ct::DeviceLink link;
    ct::CanViewerDialog d(&link);
    QTableWidget *t = table(&d);
    box(&d, QStringLiteral("overwriteCheck"))->setChecked(true);

    emit link.monitorFrame(frame(1, 0, 0x100, {0x11}));
    emit link.monitorFrame(frame(1, 1, 0x200, {0x22}));
    emit link.monitorFrame(frame(2, 0, 0x300, {0x33}));
    CHECK(t->rowCount() == 3);

    box(&d, QStringLiteral("txCheck"))->setChecked(false);
    CHECK(view(t) == QStringLiteral("CAN 1 Rx 0x100 [11] | CAN 2 Rx 0x300 [33]"));

    box(&d, QStringLiteral("busCheck1"))->setChecked(false);
    CHECK(view(t) == QStringLiteral("CAN 2 Rx 0x300 [33]"));

    // A hidden identifier keeps counting while it is off screen, so the count
    // it comes back with is the number of frames, not the number seen.
    emit link.monitorFrame(frame(1, 1, 0x200, {0x2F}));
    emit link.monitorFrame(frame(1, 1, 0x200, {0x2E}));
    box(&d, QStringLiteral("txCheck"))->setChecked(true);
    box(&d, QStringLiteral("busCheck1"))->setChecked(true);
    CHECK(t->rowCount() == 3);
    CHECK(cell(t, 1, ColData) == QStringLiteral("2E"));
    CHECK(cell(t, 1, ColFrameCount) == QStringLiteral("3"));
}

// Pause stops capture in both modes: a paused viewer that kept updating its
// rows would be the one thing Pause exists to prevent.
void testPauseStopsOverwriteUpdates()
{
    ct::DeviceLink link;
    ct::CanViewerDialog d(&link);
    QTableWidget *t = table(&d);
    box(&d, QStringLiteral("overwriteCheck"))->setChecked(true);

    emit link.monitorFrame(frame(1, 0, 0x100, {0x11}));
    box(&d, QStringLiteral("pauseCheck"))->setChecked(true);
    emit link.monitorFrame(frame(1, 0, 0x100, {0x99}));
    emit link.monitorFrame(frame(1, 0, 0x777, {0x99}));

    CHECK(t->rowCount() == 1);
    CHECK(cell(t, 0, ColData) == QStringLiteral("11"));
    CHECK(cell(t, 0, ColFrameCount) == QStringLiteral("1"));
}

// Clear empties Overwrite Mode's rows and counts along with the capture buffer.
// Leaving them would have the viewer assert traffic it holds no record of.
void testClearResetsOverwriteRows()
{
    ct::DeviceLink link;
    ct::CanViewerDialog d(&link);
    QTableWidget *t = table(&d);
    box(&d, QStringLiteral("overwriteCheck"))->setChecked(true);

    emit link.monitorFrame(frame(1, 0, 0x100, {0x11}));
    emit link.monitorFrame(frame(1, 0, 0x100, {0x12}));
    CHECK(t->rowCount() == 1);

    for (QPushButton *b : d.findChildren<QPushButton *>()) {
        if (b->text() == QStringLiteral("Clear"))
            b->click();
    }
    CHECK(t->rowCount() == 0);

    emit link.monitorFrame(frame(1, 0, 0x100, {0x13}));
    CHECK(t->rowCount() == 1);
    CHECK(cell(t, 0, ColFrameCount) == QStringLiteral("1")); // counting restarted
}

// Auto scroll has nothing to chase when rows hold still, so Overwrite Mode
// disables it — and hands it back on the way out rather than leaving a control
// permanently greyed.
void testAutoScrollIsDisabledInOverwriteMode()
{
    ct::DeviceLink link;
    ct::CanViewerDialog d(&link);

    CHECK(box(&d, QStringLiteral("autoScrollCheck"))->isEnabled());
    box(&d, QStringLiteral("overwriteCheck"))->setChecked(true);
    CHECK(!box(&d, QStringLiteral("autoScrollCheck"))->isEnabled());
    box(&d, QStringLiteral("overwriteCheck"))->setChecked(false);
    CHECK(box(&d, QStringLiteral("autoScrollCheck"))->isEnabled());
}

// A frame carrying MONFLAG_GAP means the device could not report everything the
// bus carried. Overwrite Mode has no chronological place to put a marker row,
// so the notice moves to the frame count — where it stays until Clear. Silence
// here would let an under-counted message read as a complete account.
void testDroppedFramesAreReportedInBothModes()
{
    ct::DeviceLink link;
    ct::CanViewerDialog d(&link);
    auto *label = d.findChild<QLabel *>(QStringLiteral("countLabel"));
    CHECK(label != nullptr);
    if (!label)
        return;

    emit link.monitorFrame(frame(1, 0, 0x100, {0x11}));
    CHECK(!label->text().contains(QStringLiteral("dropped")));

    emit link.monitorFrame(frame(1, 0, 0x100, {0x12}, ct::MONFLAG_GAP));
    CHECK(label->text().contains(QStringLiteral("dropped")));

    box(&d, QStringLiteral("overwriteCheck"))->setChecked(true);
    emit link.monitorFrame(frame(1, 0, 0x100, {0x13}));
    CHECK(label->text().contains(QStringLiteral("dropped"))); // sticky, not per-frame
}

} // namespace


// --------------------------------------------------------------- inject panel
//
// EIGHT SLOTS, each with its own rate. The panel was a single row that sent one
// frame per press; a bus worth exercising needs several frames going at once,
// at different rates, left running while you watch the list above them.
//
// What is NOT covered here, said plainly rather than left as a gap somebody
// finds later: starting a run needs a send the device accepts, and this suite
// opens no device, so the running states — the button reading Stop, the fields
// locking, the timer's period, skip-if-busy — are exercised by hand and not
// here. What follows is the shape of the panel and the labelling that decides
// what a press will do, which is testable without hardware and is where a
// silent change would do its damage.
void testInjectPanelHasEightSlotsWithARateDropdown()
{
    ct::DeviceLink link;
    ct::CanViewerDialog d(&link);

    // The rate combos are the ones offering "Once"; the bus combos do not.
    QList<QComboBox *> rates;
    for (QComboBox *c : d.findChildren<QComboBox *>())
        if (c->count() > 0 && c->itemText(0) == QStringLiteral("Once"))
            rates.append(c);
    CHECK(rates.size() == 8);

    // Once, then the seven rates, in the order asked for. Checked by the data
    // rather than the label so a translated build still passes.
    const QList<int> expected = {0, 1, 2, 5, 10, 20, 50, 100};
    for (QComboBox *c : rates) {
        CHECK(c->count() == expected.size());
        for (int i = 0; i < c->count() && i < expected.size(); ++i)
            CHECK(c->itemData(i).toInt() == expected.at(i));
        // Once is the default: an untouched slot does what every release before
        // this one did — one press, one frame.
        CHECK(c->currentIndex() == 0);
    }
}

// "To the right of the Data panel" was the placement asked for, so it is the
// placement pinned. A grid makes this cheap to check and cheap to break.
void testTheRateDropdownSitsRightOfData()
{
    ct::DeviceLink link;
    ct::CanViewerDialog d(&link);

    QGridLayout *grid = nullptr;
    for (QGridLayout *g : d.findChildren<QGridLayout *>())
        if (g->columnCount() >= 6)
            grid = g;
    CHECK(grid != nullptr);
    if (!grid)
        return;

    QComboBox *rate = nullptr;
    for (QComboBox *c : d.findChildren<QComboBox *>())
        if (c->count() > 0 && c->itemText(0) == QStringLiteral("Once")) {
            rate = c;
            break;
        }
    QLineEdit *data = nullptr;
    for (QLineEdit *e : d.findChildren<QLineEdit *>())
        if (e->placeholderText().startsWith(QStringLiteral("00 11"))) {
            data = e;
            break;
        }
    CHECK(rate != nullptr);
    CHECK(data != nullptr);
    if (!rate || !data)
        return;

    int rateRow = -1, rateCol = -1, dataRow = -1, dataCol = -1, span = 0;
    grid->getItemPosition(grid->indexOf(rate), &rateRow, &rateCol, &span, &span);
    grid->getItemPosition(grid->indexOf(data), &dataRow, &dataCol, &span, &span);
    CHECK(rateRow == dataRow);
    CHECK(rateCol > dataCol);
}

// The button says what pressing it will do, and that is the whole of the
// difference between a slot that fires once and a slot that starts running.
void testTheInjectButtonFollowsTheRate()
{
    ct::DeviceLink link;
    ct::CanViewerDialog d(&link);

    const auto count = [&](const QString &text) {
        int n = 0;
        for (QPushButton *b : d.findChildren<QPushButton *>())
            if (b->text() == text)
                ++n;
        return n;
    };

    CHECK(count(QStringLiteral("Send")) == 8);
    CHECK(count(QStringLiteral("Start")) == 0);

    QComboBox *rate = nullptr;
    for (QComboBox *c : d.findChildren<QComboBox *>())
        if (c->count() > 0 && c->itemText(0) == QStringLiteral("Once")) {
            rate = c;
            break;
        }
    CHECK(rate != nullptr);
    if (!rate)
        return;

    rate->setCurrentIndex(3); // 5 Hz
    CHECK(count(QStringLiteral("Send")) == 7);
    CHECK(count(QStringLiteral("Start")) == 1);

    rate->setCurrentIndex(0); // back to Once
    CHECK(count(QStringLiteral("Send")) == 8);
    CHECK(count(QStringLiteral("Start")) == 0);
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    testTxMessagesAreShownByDefault();
    testTxFilterHidesAndRestores();
    testBusAndTxFiltersCompose();
    testOverwriteModeKeepsOnlyTheLatestFrame();
    testOverwriteModeSortsByBusThenId();
    testNewIdentifierInsertsInOrderWithoutDisturbingOthers();
    testIdentitySeparatesDirectionAndExtendedFlag();
    testCountColumn();
    testOverwriteModeOpensOnTheCurrentState();
    testFiltersApplyInOverwriteMode();
    testPauseStopsOverwriteUpdates();
    testClearResetsOverwriteRows();
    testAutoScrollIsDisabledInOverwriteMode();
    testDroppedFramesAreReportedInBothModes();

    testInjectPanelHasEightSlotsWithARateDropdown();
    testTheRateDropdownSitsRightOfData();
    testTheInjectButtonFollowsTheRate();

    if (fails == 0) {
        std::printf("test_can_viewer: all checks passed\n");
        return 0;
    }
    std::printf("test_can_viewer: %d check(s) failed\n", fails);
    return 1;
}
