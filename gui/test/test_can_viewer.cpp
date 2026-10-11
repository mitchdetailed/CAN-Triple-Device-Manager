// The CAN Viewer's display filters and Overwrite Mode, driven through the real
// dialog: frames go in as the device's monitor stream delivers them, and every
// assertion reads the table a user would be looking at.
//
// Two properties carry most of the weight here.
//
// The first is what each filter keeps. The Show boxes decide CAPTURE as well as
// the list (Mitch, 2026-10-11: "if we don't show them, don't capture them"): a
// frame of an unticked bus and direction is in neither the list nor the buffer
// that Save to File writes, while what was captured before a box was unticked
// stays, and comes back when it is ticked. The ID filter is a VIEW, never a
// loss: it is typed live, and a half-typed entry must not throw a recorded
// trace away, so its tests untick, check the view shrank, re-tick, and check
// the history came back rather than resuming from that moment.
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

// The Show box of one bus and direction, "show1Rx" .. "show3Tx".
QCheckBox *showBox(QWidget *w, int bus, bool tx)
{
    return box(w, QStringLiteral("show%1%2").arg(bus).arg(tx ? QStringLiteral("Tx")
                                                               : QStringLiteral("Rx")));
}

// The Show boxes are one per bus and direction (Mitch, 2026-10-11): CAN 1 Rx,
// CAN 1 Tx, CAN 2 Rx, CAN 2 Tx, CAN 3 Rx, CAN 3 Tx, left to right on a row of
// their own, every one ticked by default. Tx rows are shown, as every earlier
// release showed them: on a gateway they are usually the half you are
// checking. The window still opens at its default width.
void testShowBoxesAllTickedByDefault()
{
    ct::DeviceLink link;
    ct::CanViewerDialog d(&link);

    QStringList labels;
    QList<QCheckBox *> boxes;
    for (int b = 1; b <= 3; ++b)
        for (bool tx : {false, true}) {
            QCheckBox *c = showBox(&d, b, tx);
            CHECK(c->isChecked());
            labels << c->text();
            boxes << c;
        }
    CHECK(labels
          == QStringList({QStringLiteral("CAN 1 Rx"), QStringLiteral("CAN 1 Tx"),
                          QStringLiteral("CAN 2 Rx"), QStringLiteral("CAN 2 Tx"),
                          QStringLiteral("CAN 3 Rx"), QStringLiteral("CAN 3 Tx")}));
    // The old boxes are gone, not hidden.
    CHECK(d.findChild<QCheckBox *>(QStringLiteral("txCheck")) == nullptr
          && d.findChild<QCheckBox *>(QStringLiteral("busCheck1")) == nullptr);
    for (QCheckBox *c : d.findChildren<QCheckBox *>())
        CHECK(c->text() != QStringLiteral("Tx Msgs"));

    // Laid out without being shown: left to right on one row, a row of their
    // own (not the one Pause is on).
    d.resize(d.sizeHint());
    d.layout()->activate();
    const auto at = [&d](QWidget *w) { return w->mapTo(&d, QPoint(0, 0)); };
    bool inOrder = true;
    for (int i = 1; i < boxes.size(); ++i)
        inOrder = inOrder && at(boxes[i]).x() > at(boxes[i - 1]).x()
                  && at(boxes[i]).y() == at(boxes[0]).y();
    CHECK(inOrder);
    CHECK(at(box(&d, QStringLiteral("pauseCheck"))).y() < at(boxes[0]).y());

    emit link.monitorFrame(frame(1, 0, 0x100, {0x11}));
    emit link.monitorFrame(frame(1, 1, 0x200, {0x22}));

    QTableWidget *t = table(&d);
    CHECK(t->rowCount() == 2);
    CHECK(cell(t, 0, ColDir) == QStringLiteral("Rx"));
    CHECK(cell(t, 1, ColDir) == QStringLiteral("Tx"));
}

// Unticking CAN 1 Tx hides the device's own frames on CAN 1 and stops their
// capture, and nothing else: what arrives while it is unticked is not in the
// buffer (the count Save to File writes), and re-ticking lists the frames
// captured before it was unticked, not the ones that came while it was.
void testUntickedFramesAreNotCaptured()
{
    ct::DeviceLink link;
    ct::CanViewerDialog d(&link);
    QTableWidget *t = table(&d);
    auto *count = d.findChild<QLabel *>(QStringLiteral("countLabel"));
    CHECK(count != nullptr);
    if (!count)
        return;

    emit link.monitorFrame(frame(1, 0, 0x100, {0x11}));
    emit link.monitorFrame(frame(1, 1, 0x200, {0x22}));
    CHECK(t->rowCount() == 2 && count->text().startsWith(QStringLiteral("2 frames buffered")));

    showBox(&d, 1, true)->setChecked(false);
    CHECK(t->rowCount() == 1);
    CHECK(cell(t, 0, ColId) == QStringLiteral("0x100"));
    // What was captured before stays in the buffer.
    CHECK(count->text().startsWith(QStringLiteral("2 frames buffered")));

    // A frame of the unticked box is not captured; the other is.
    emit link.monitorFrame(frame(1, 1, 0x300, {0x33}));
    emit link.monitorFrame(frame(1, 0, 0x400, {0x44}));
    CHECK(t->rowCount() == 2); // only the Rx one appeared
    CHECK(count->text().startsWith(QStringLiteral("3 frames buffered")));

    // Re-ticking lists what was captured: 0x200 from before, never 0x300.
    showBox(&d, 1, true)->setChecked(true);
    CHECK(view(t)
          == QStringLiteral("CAN 1 Rx 0x100 [11] | CAN 1 Tx 0x200 [22] | CAN 1 Rx 0x400 [44]"));
    // And from now on it is captured again.
    emit link.monitorFrame(frame(1, 1, 0x500, {0x55}));
    CHECK(t->rowCount() == 4 && count->text().startsWith(QStringLiteral("4 frames buffered")));

    // Every box unticked captures nothing at all.
    for (int b = 1; b <= 3; ++b)
        for (bool tx : {false, true})
            showBox(&d, b, tx)->setChecked(false);
    emit link.monitorFrame(frame(2, 0, 0x600, {0x66}));
    emit link.monitorFrame(frame(3, 1, 0x700, {0x77}));
    CHECK(count->text().startsWith(QStringLiteral("4 frames buffered")));
}

// The boxes compose: hiding both of a bus's boxes hides the bus, and a bus's
// Tx alone shows what the device sent there and nothing it received.
void testRxAndTxBoxesCompose()
{
    ct::DeviceLink link;
    ct::CanViewerDialog d(&link);
    QTableWidget *t = table(&d);

    emit link.monitorFrame(frame(1, 0, 0x100, {0x11}));
    emit link.monitorFrame(frame(1, 1, 0x101, {0x11}));
    emit link.monitorFrame(frame(2, 0, 0x200, {0x22}));
    emit link.monitorFrame(frame(2, 1, 0x201, {0x22}));
    CHECK(t->rowCount() == 4);

    showBox(&d, 2, false)->setChecked(false);
    showBox(&d, 2, true)->setChecked(false);
    CHECK(t->rowCount() == 2); // CAN 1 only, both directions

    showBox(&d, 1, true)->setChecked(false);
    CHECK(t->rowCount() == 1);
    CHECK(cell(t, 0, ColId) == QStringLiteral("0x100"));

    showBox(&d, 1, true)->setChecked(true);
    CHECK(t->rowCount() == 2); // CAN 2 is still hidden
    CHECK(view(t) == QStringLiteral("CAN 1 Rx 0x100 [11] | CAN 1 Tx 0x101 [11]"));

    showBox(&d, 2, true)->setChecked(true);
    CHECK(view(t)
          == QStringLiteral("CAN 1 Rx 0x100 [11] | CAN 1 Tx 0x101 [11] | CAN 2 Tx 0x201 [22]"));
}

// Each box hides exactly its own bus and direction: a frame for each of the
// six; unticking any one box hides that frame alone, and re-ticking brings it
// back. A frame labelled with a bus this window does not know is never
// swallowed, whatever is ticked.
void testEachShowBoxHidesItsOwnFrames()
{
    ct::DeviceLink link;
    ct::CanViewerDialog d(&link);
    QTableWidget *t = table(&d);

    for (int b = 1; b <= 3; ++b)
        for (int dir = 0; dir < 2; ++dir)
            emit link.monitorFrame(frame(quint8(b), quint8(dir), quint32(0x100 * b + dir),
                                         {quint8(b)}));
    CHECK(t->rowCount() == 6);
    for (int b = 1; b <= 3; ++b)
        for (int dir = 0; dir < 2; ++dir) {
            const QString id = QStringLiteral("0x%1").arg(0x100 * b + dir, 3, 16, QLatin1Char('0'));
            showBox(&d, b, dir == 1)->setChecked(false);
            CHECK(t->rowCount() == 5 && !view(t).contains(id));
            showBox(&d, b, dir == 1)->setChecked(true);
            CHECK(t->rowCount() == 6 && view(t).contains(id));
        }

    for (int b = 1; b <= 3; ++b)
        for (bool tx : {false, true})
            showBox(&d, b, tx)->setChecked(false);
    CHECK(t->rowCount() == 0);
    emit link.monitorFrame(frame(4, 1, 0x7FF, {0x44}));
    CHECK(t->rowCount() == 1 && cell(t, 0, ColId) == QStringLiteral("0x7FF"));
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

// The Show boxes apply in Overwrite Mode too: re-ticking restores a row as it
// was captured, and an identifier of an unticked box is not tracked while it
// is, so its value and count are those of the frames that were captured.
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

    showBox(&d, 1, true)->setChecked(false);
    CHECK(view(t) == QStringLiteral("CAN 1 Rx 0x100 [11] | CAN 2 Rx 0x300 [33]"));

    showBox(&d, 1, false)->setChecked(false);
    CHECK(view(t) == QStringLiteral("CAN 2 Rx 0x300 [33]"));

    // Frames of the unticked boxes are not captured: 0x200 comes back with the
    // value and the count it had, and an identifier first seen while its box was
    // unticked has no row at all.
    emit link.monitorFrame(frame(1, 1, 0x200, {0x2F}));
    emit link.monitorFrame(frame(1, 1, 0x200, {0x2E}));
    emit link.monitorFrame(frame(1, 0, 0x150, {0x15}));
    showBox(&d, 1, true)->setChecked(true);
    showBox(&d, 1, false)->setChecked(true);
    CHECK(t->rowCount() == 3);
    CHECK(view(t)
          == QStringLiteral("CAN 1 Rx 0x100 [11] | CAN 1 Tx 0x200 [22] | CAN 2 Rx 0x300 [33]"));
    CHECK(cell(t, 1, ColFrameCount) == QStringLiteral("1"));

    // Ticked again, it is tracked again, counting on from what was captured.
    emit link.monitorFrame(frame(1, 1, 0x200, {0x2D}));
    CHECK(cell(t, 1, ColData) == QStringLiteral("2D") && cell(t, 1, ColFrameCount) == QStringLiteral("2"));
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

// A drop reported on a frame whose Show box is unticked is the whole stream's,
// not that bus's: the frame is not captured, the drop is noted at once, and its
// marker row stands before the next frame that IS captured.
void testDropOnAnUncapturedFrameIsKept()
{
    ct::DeviceLink link;
    ct::CanViewerDialog d(&link);
    QTableWidget *t = table(&d);
    auto *label = d.findChild<QLabel *>(QStringLiteral("countLabel"));
    CHECK(label != nullptr);
    if (!label)
        return;

    emit link.monitorFrame(frame(1, 0, 0x100, {0x11}));
    showBox(&d, 2, false)->setChecked(false);
    emit link.monitorFrame(frame(2, 0, 0x200, {0x22}, ct::MONFLAG_GAP)); // not captured
    CHECK(label->text().startsWith(QStringLiteral("1 frames buffered"))
          && label->text().contains(QStringLiteral("dropped")));
    CHECK(t->rowCount() == 1);

    emit link.monitorFrame(frame(1, 0, 0x101, {0x33}));
    CHECK(t->rowCount() == 3);
    CHECK(cell(t, 1, ColBus).contains(QStringLiteral("frames dropped"))
          && cell(t, 2, ColId) == QStringLiteral("0x101"));
    // The marker is kept in the capture: a rebuild puts it back in its place.
    showBox(&d, 2, false)->setChecked(true);
    CHECK(t->rowCount() == 3 && cell(t, 1, ColBus).contains(QStringLiteral("frames dropped")));
    // Carried once: the next frame has no marker of its own.
    emit link.monitorFrame(frame(1, 0, 0x102, {0x44}));
    CHECK(t->rowCount() == 4);

    // Clear forgets a drop not yet placed: a log begun after it starts clean.
    showBox(&d, 2, false)->setChecked(false);
    emit link.monitorFrame(frame(2, 0, 0x201, {0x55}, ct::MONFLAG_GAP)); // not captured
    QPushButton *clear = nullptr;
    for (QPushButton *b : d.findChildren<QPushButton *>())
        if (b->text() == QStringLiteral("Clear"))
            clear = b;
    CHECK(clear != nullptr);
    if (!clear)
        return;
    clear->click();
    emit link.monitorFrame(frame(1, 0, 0x103, {0x66}));
    CHECK(t->rowCount() == 1 && cell(t, 0, ColId) == QStringLiteral("0x103")
          && !label->text().contains(QStringLiteral("dropped")));
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

// ---------------------------------------------------------------------------
// The ID filter.

QLineEdit *idFilter(QWidget *w)
{
    auto *e = w->findChild<QLineEdit *>(QStringLiteral("idFilter"));
    Q_ASSERT(e);
    return e;
}

// The syntax, on the parser alone: hex with or without 0x, ranges, "!" to
// exclude, and the ways a token can be wrong. A bad token is named and does not
// take the good ones down with it.
void testIdFilterSyntax()
{
    using Dialog = ct::CanViewerDialog;
    Dialog::IdFilter f;
    QString error;

    CHECK(Dialog::parseIdFilter(QString(), &f, &error));
    CHECK(f.isEmpty());
    CHECK(f.matches(0x123));

    CHECK(Dialog::parseIdFilter(QStringLiteral("7E8"), &f, &error));
    CHECK(f.include.size() == 1 && f.exclude.isEmpty());
    CHECK(f.matches(0x7E8));
    CHECK(!f.matches(0x7E9));

    CHECK(Dialog::parseIdFilter(QStringLiteral("0x7e8, 100-0x1FF; 300"), &f, &error));
    CHECK(f.include.size() == 3);
    CHECK(f.matches(0x7E8) && f.matches(0x100) && f.matches(0x1FF) && f.matches(0x300));
    CHECK(!f.matches(0x200) && !f.matches(0x0FF));

    CHECK(Dialog::parseIdFilter(QStringLiteral("!3C0"), &f, &error));
    CHECK(f.include.isEmpty() && f.exclude.size() == 1);
    CHECK(f.matches(0x100) && !f.matches(0x3C0));

    // Exclude wins where both cover an ID: the block, minus the one.
    CHECK(Dialog::parseIdFilter(QStringLiteral("100-1FF !150"), &f, &error));
    CHECK(f.matches(0x100) && f.matches(0x1FF) && !f.matches(0x150) && !f.matches(0x200));

    // A bad token is refused by name; the good ones still parse.
    CHECK(!Dialog::parseIdFilter(QStringLiteral("200, ZZ"), &f, &error));
    CHECK(error.contains(QStringLiteral("ZZ")));
    CHECK(f.include.size() == 1 && f.matches(0x200) && !f.matches(0x100));

    // Past the 29-bit ceiling, a reversed range, a dangling dash, a bare 0x.
    CHECK(!Dialog::parseIdFilter(QStringLiteral("20000000"), &f, &error));
    CHECK(!Dialog::parseIdFilter(QStringLiteral("1FF-100"), &f, &error));
    CHECK(!Dialog::parseIdFilter(QStringLiteral("100-"), &f, &error));
    CHECK(!Dialog::parseIdFilter(QStringLiteral("0x"), &f, &error));
    CHECK(f.isEmpty());
}

// Typing an ID hides every other row and loses nothing: frames that arrived
// while the filter was on come back when it is cleared, because the list is
// rebuilt from the capture buffer like the other filters.
void testIdFilterHidesAndRestores()
{
    ct::DeviceLink link;
    ct::CanViewerDialog d(&link);
    QTableWidget *t = table(&d);

    emit link.monitorFrame(frame(1, 0, 0x100, {0x11}));
    emit link.monitorFrame(frame(1, 0, 0x200, {0x22}));
    emit link.monitorFrame(frame(2, 0, 0x300, {0x33}));
    CHECK(t->rowCount() == 3);

    idFilter(&d)->setText(QStringLiteral("200"));
    CHECK(t->rowCount() == 1);
    CHECK(cell(t, 0, ColId) == QStringLiteral("0x200"));

    emit link.monitorFrame(frame(1, 0, 0x100, {0x12})); // hidden, still captured
    emit link.monitorFrame(frame(1, 0, 0x200, {0x23}));
    CHECK(t->rowCount() == 2);

    idFilter(&d)->clear();
    CHECK(t->rowCount() == 5);
    CHECK(view(t).contains(QStringLiteral("0x100 [12]")));
}

// The filter composes with the Show boxes — a frame must pass both — and a bad
// token is named beside the field while the rest of the entry keeps working.
void testIdFilterComposesAndNamesBadTokens()
{
    ct::DeviceLink link;
    ct::CanViewerDialog d(&link);
    QTableWidget *t = table(&d);
    auto *note = d.findChild<QLabel *>(QStringLiteral("idFilterNote"));
    CHECK(note != nullptr);
    if (!note)
        return;

    emit link.monitorFrame(frame(1, 0, 0x100, {0x11}));
    emit link.monitorFrame(frame(2, 0, 0x100, {0x22}));
    emit link.monitorFrame(frame(1, 1, 0x100, {0x33}));
    emit link.monitorFrame(frame(1, 0, 0x200, {0x44}));

    idFilter(&d)->setText(QStringLiteral("100"));
    CHECK(t->rowCount() == 3);
    showBox(&d, 2, false)->setChecked(false);
    CHECK(t->rowCount() == 2);
    showBox(&d, 1, true)->setChecked(false);
    CHECK(t->rowCount() == 1);
    CHECK(view(t) == QStringLiteral("CAN 1 Rx 0x100 [11]"));

    idFilter(&d)->setText(QStringLiteral("100, ZZ"));
    CHECK(note->text().contains(QStringLiteral("ZZ")));
    CHECK(t->rowCount() == 1); // the good token still applies
    idFilter(&d)->setText(QStringLiteral("100"));
    CHECK(note->text().isEmpty());
}

// Overwrite Mode: a filtered-out identifier keeps being tracked — its latest
// frame and its count — so clearing the filter shows the value it has NOW, and a
// new identifier the filter hides adds no row.
void testIdFilterInOverwriteMode()
{
    ct::DeviceLink link;
    ct::CanViewerDialog d(&link);
    QTableWidget *t = table(&d);
    box(&d, QStringLiteral("overwriteCheck"))->setChecked(true);

    emit link.monitorFrame(frame(1, 0, 0x100, {0x01}));
    emit link.monitorFrame(frame(1, 0, 0x200, {0x02}));
    CHECK(t->rowCount() == 2);

    idFilter(&d)->setText(QStringLiteral("!100"));
    CHECK(t->rowCount() == 1);
    CHECK(cell(t, 0, ColId) == QStringLiteral("0x200"));

    emit link.monitorFrame(frame(1, 0, 0x100, {0x0A})); // hidden, still tracked
    emit link.monitorFrame(frame(1, 0, 0x100, {0x0B}));
    emit link.monitorFrame(frame(1, 0, 0x300, {0x03})); // new and shown
    CHECK(t->rowCount() == 2);
    CHECK(view(t) == QStringLiteral("CAN 1 Rx 0x200 [02] | CAN 1 Rx 0x300 [03]"));

    idFilter(&d)->clear();
    CHECK(t->rowCount() == 3);
    CHECK(cell(t, 0, ColId) == QStringLiteral("0x100"));
    CHECK(cell(t, 0, ColData) == QStringLiteral("0B"));
    CHECK(cell(t, 0, ColFrameCount) == QStringLiteral("3"));
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    testShowBoxesAllTickedByDefault();
    testUntickedFramesAreNotCaptured();
    testRxAndTxBoxesCompose();
    testEachShowBoxHidesItsOwnFrames();
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
    testDropOnAnUncapturedFrameIsKept();

    testIdFilterSyntax();
    testIdFilterHidesAndRestores();
    testIdFilterComposesAndNamesBadTokens();
    testIdFilterInOverwriteMode();

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
