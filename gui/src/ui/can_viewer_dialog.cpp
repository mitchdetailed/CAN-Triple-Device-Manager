// Implementation of the CAN Viewer dialog: live raw-frame monitor fed by
// the device's CMD_MONITOR_STREAM, plus an inject-frame panel.
#include "can_viewer_dialog.h"

#include <QApplication>
#include <QPointer>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QGridLayout>
#include <QStringList>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTextStream>
#include <QTimer>
#include <QVBoxLayout>

#include <cstring>

#include "../protocol/asc_log.h"

namespace ct {

namespace {

enum Column {
    ColTime = 0,
    ColBus,
    ColDir,
    ColId,
    ColLen,
    ColFrameCount, // Overwrite Mode only — hidden in the scrolling view
    ColData,
    ColCount       // sentinel: number of columns
};

QTableWidgetItem *makeItem(const QString &text)
{
    auto *item = new QTableWidgetItem(text);
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    return item;
}

// Set a cell's text, reusing the item that is already there. Overwrite Mode
// rewrites the same rows for the life of the dialog, so allocating a fresh
// QTableWidgetItem per frame would churn the heap at bus rate for no reason.
void setCellText(QTableWidget *table, int row, int col, const QString &text,
                 const QFont *font = nullptr)
{
    if (QTableWidgetItem *item = table->item(row, col)) {
        item->setText(text);
        return;
    }
    auto *item = makeItem(text);
    if (font)
        item->setFont(*font);
    table->setItem(row, col, item);
}

// The ID as the viewer writes it: standard as 0x%03X, extended as 0x%08X, so
// the width itself marks the format (matches the section editor's Base Address).
QString formatCanId(quint32 canId, bool extended)
{
    return QStringLiteral("0x")
           + QStringLiteral("%1").arg(canId, extended ? 8 : 3, 16, QLatin1Char('0')).toUpper();
}

QString formatData(const ct::MonitorStreamPayload &frame)
{
    const int len = qMin<int>(frame.data_len, 64);
    QString text;
    text.reserve(len * 3);
    for (int i = 0; i < len; ++i) {
        if (i)
            text += QLatin1Char(' ');
        text += QStringLiteral("%1").arg(frame.data[i], 2, 16, QLatin1Char('0')).toUpper();
    }
    return text;
}

} // namespace

CanViewerDialog::CanViewerDialog(DeviceLink *link, QWidget *parent)
    : QDialog(parent)
    , m_link(link)
{
    setWindowTitle(tr("CAN Viewer"));
    setModal(false);
    resize(800, 600);

    auto *mainLayout = new QVBoxLayout(this);

    // --- Top control row ---------------------------------------------------
    auto *topRow = new QHBoxLayout;
    m_pauseCheck = new QCheckBox(tr("Pause"), this);
    m_autoScrollCheck = new QCheckBox(tr("Auto scroll"), this);
    m_autoScrollCheck->setChecked(true);
    m_countLabel = new QLabel(tr("%1 frames buffered").arg(0), this);
    m_countLabel->setObjectName(QStringLiteral("countLabel"));
    auto *saveButton = new QPushButton(tr("Save to File…"), this);
    auto *clearButton = new QPushButton(tr("Clear"), this);

    m_pauseCheck->setObjectName(QStringLiteral("pauseCheck"));
    m_autoScrollCheck->setObjectName(QStringLiteral("autoScrollCheck"));
    topRow->addWidget(m_pauseCheck);
    topRow->addWidget(m_autoScrollCheck);

    // Display filters. These hide rows; they do NOT stop capture. Every frame
    // the device streams still goes into the buffer and into Save to File, so
    // unchecking a bus to read a quiet one cannot silently cost you the trace
    // you were recording — and re-checking brings its history back.
    topRow->addSpacing(12);
    topRow->addWidget(new QLabel(tr("Show:"), this));
    for (int b = 0; b < 3; ++b) {
        m_busChecks[b] = new QCheckBox(tr("CAN %1").arg(b + 1), this);
        m_busChecks[b]->setObjectName(QStringLiteral("busCheck%1").arg(b + 1));
        m_busChecks[b]->setChecked(true);
        m_busChecks[b]->setToolTip(tr("Show frames from CAN %1. Hiding a bus only affects "
                                      "this list — every bus is still captured and saved.")
                                       .arg(b + 1));
        connect(m_busChecks[b], &QCheckBox::toggled, this, &CanViewerDialog::rebuildTable);
        topRow->addWidget(m_busChecks[b]);
    }

    // The device's own transmissions — engine transmit messages, relayed
    // frames, and the echo of an injected one. Ticked by default, because
    // hiding what this device put on the wire is a deliberate act and never a
    // surprise: on a gateway the Tx side is usually the half you are checking.
    m_txCheck = new QCheckBox(tr("Tx Msgs"), this);
    m_txCheck->setObjectName(QStringLiteral("txCheck"));
    m_txCheck->setChecked(true);
    m_txCheck->setToolTip(tr("Show frames the device transmitted (the Tx rows). Untick to "
                             "leave only what the buses carried in. Like the bus filters "
                             "this affects this list alone — Tx frames are still captured "
                             "and still written by Save to File."));
    connect(m_txCheck, &QCheckBox::toggled, this, &CanViewerDialog::rebuildTable);
    topRow->addWidget(m_txCheck);

    topRow->addStretch(1);
    topRow->addWidget(m_countLabel);
    topRow->addWidget(saveButton);
    topRow->addWidget(clearButton);
    mainLayout->addLayout(topRow);

    // --- Second control row: view mode ---------------------------------------
    auto *modeRow = new QHBoxLayout;
    m_overwriteCheck = new QCheckBox(tr("Overwrite Mode"), this);
    m_overwriteCheck->setObjectName(QStringLiteral("overwriteCheck"));
    m_overwriteCheck->setToolTip(tr("Show one row per message instead of a scrolling trace: "
                                    "each arbitration ID keeps a single row carrying its "
                                    "most recent data, ordered by bus and then by ID. Use it "
                                    "to read current values; untick it to read history. "
                                    "Capture is unaffected — Save to File still writes the "
                                    "whole trace, frame by frame."));
    connect(m_overwriteCheck, &QCheckBox::toggled, this, &CanViewerDialog::onOverwriteToggled);
    modeRow->addWidget(m_overwriteCheck);
    modeRow->addStretch(1);
    mainLayout->addLayout(modeRow);

    // --- Frame table --------------------------------------------------------
    m_table = new QTableWidget(0, ColCount, this);
    m_table->setObjectName(QStringLiteral("frameTable"));
    m_table->setHorizontalHeaderLabels(QStringList()
                                       << tr("Time (s)") << tr("Bus") << tr("Dir")
                                       << tr("ID") << tr("Len") << tr("Count") << tr("Data"));
    // Count belongs to Overwrite Mode alone: in the scrolling trace every row
    // is one frame, so a per-row count would read 1 all the way down.
    m_table->setColumnHidden(ColFrameCount, true);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->verticalHeader()->setVisible(false);
    m_table->verticalHeader()->setDefaultSectionSize(
        m_table->fontMetrics().height() + 4);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->setColumnWidth(ColTime, 80);
    m_table->setColumnWidth(ColBus, 60);
    m_table->setColumnWidth(ColDir, 45);
    m_table->setColumnWidth(ColId, 100);
    m_table->setColumnWidth(ColLen, 45);
    m_table->setColumnWidth(ColFrameCount, 80);
    mainLayout->addWidget(m_table, 1);

    // --- Inject panel --------------------------------------------------------
    auto *injectBox = new QGroupBox(tr("Inject Frames"), this);
    injectBox->setToolTip(tr("A payload of 9 or more bytes is sent as a CAN FD frame; 8 or "
                             "fewer as a classic frame. The frame is also received by the "
                             "device — parsed, and forwarded by any matching relay."));
    // A GRID, not eight copies of the old row. Eight rows of repeated "Bus:",
    // "ID:", "Data:" labels is most of the panel's width spent saying the same
    // four words eight times; one header row says them once and the slots line
    // up under it.
    auto *injectGrid = new QGridLayout(injectBox);
    injectGrid->setHorizontalSpacing(8);
    injectGrid->setVerticalSpacing(3);
    injectGrid->addWidget(new QLabel(tr("Bus"), injectBox), 0, 0);
    injectGrid->addWidget(new QLabel(tr("ID"), injectBox), 0, 1);
    injectGrid->addWidget(new QLabel(tr("Data"), injectBox), 0, 3);
    injectGrid->addWidget(new QLabel(tr("Hz"), injectBox), 0, 4);
    injectGrid->setColumnStretch(3, 1); // the data field takes the slack

    const QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);

    for (int i = 0; i < kInjectSlots; ++i) {
        InjectSlot &s = m_inject[i];
        const int row = i + 1; // row 0 is the header

        s.bus = new QComboBox(injectBox);
        s.bus->addItem(tr("CAN 1"));
        s.bus->addItem(tr("CAN 2"));
        s.bus->addItem(tr("CAN 3"));
        injectGrid->addWidget(s.bus, row, 0);

        s.id = new QLineEdit(injectBox);
        s.id->setPlaceholderText(tr("0x7E0"));
        s.id->setMaximumWidth(100);
        s.id->setFont(mono);
        injectGrid->addWidget(s.id, row, 1);

        s.extended = new QCheckBox(tr("Extended"), injectBox);
        injectGrid->addWidget(s.extended, row, 2);

        s.data = new QLineEdit(injectBox);
        s.data->setPlaceholderText(tr("00 11 22 33 44 55 66 77"));
        s.data->setFont(mono);
        injectGrid->addWidget(s.data, row, 3);

        // ONCE FIRST, AND THE DEFAULT. It is what every release before this one
        // did, so a slot nobody has configured behaves the way the single row
        // it replaced behaved: press the button, one frame goes.
        s.rate = new QComboBox(injectBox);
        s.rate->addItem(tr("Once"), 0);
        for (const int hz : {1, 2, 5, 10, 20, 50, 100})
            s.rate->addItem(QString::number(hz), hz);
        injectGrid->addWidget(s.rate, row, 4);

        s.button = new QPushButton(injectBox);
        injectGrid->addWidget(s.button, row, 5);

        s.timer = new QTimer(this);

        connect(s.button, &QPushButton::clicked, this, [this, i] { onInjectClicked(i); });
        connect(s.id, &QLineEdit::returnPressed, this, [this, i] { onInjectClicked(i); });
        connect(s.data, &QLineEdit::returnPressed, this, [this, i] { onInjectClicked(i); });
        connect(s.timer, &QTimer::timeout, this, [this, i] { onInjectTick(i); });
        connect(s.rate, &QComboBox::currentIndexChanged, this, [this, i](int) {
            // A rate changed mid-run re-times the running slot rather than
            // stopping it: the user asked for a different rate, not for the
            // injection to end, and making them press Stop and Start to get
            // there is a step that exists only because the code found it
            // easier. Switching to Once does stop it — there is no rate left
            // to run at.
            InjectSlot &slot = m_inject[i];
            if (slot.timer->isActive()) {
                const int hz = slot.rate->currentData().toInt();
                if (hz <= 0)
                    stopInject(i);
                else
                    slot.timer->start(qMax(1, 1000 / hz));
            }
            updateInjectSlot(i);
        });

        updateInjectSlot(i);
    }

    mainLayout->addWidget(injectBox);

    connect(saveButton, &QPushButton::clicked, this, &CanViewerDialog::onSaveClicked);
    connect(clearButton, &QPushButton::clicked, this, &CanViewerDialog::onClearClicked);
    connect(m_link, &DeviceLink::monitorFrame, this, &CanViewerDialog::onMonitorFrame);
}

bool CanViewerDialog::busVisible(quint8 busIdx) const
{
    // Anything outside 1..3 is shown rather than hidden: a frame the device
    // labelled with a bus this dialog does not know about is exactly the thing
    // a viewer must not swallow.
    if (busIdx < 1 || busIdx > 3)
        return true;
    return m_busChecks[busIdx - 1]->isChecked();
}

// Every display filter in one place, so the live path, the rebuild and
// Overwrite Mode cannot drift apart on what "shown" means.
bool CanViewerDialog::frameVisible(const ct::MonitorStreamPayload &frame) const
{
    if (frame.direction && !m_txCheck->isChecked())
        return false;
    return busVisible(frame.bus_idx);
}

CanViewerDialog::OverwriteKey CanViewerDialog::keyFor(const ct::MonitorStreamPayload &frame)
{
    OverwriteKey key;
    key.bus = frame.bus_idx;
    key.canId = frame.can_id;
    key.extended = (frame.flags & ct::MONFLAG_EXTENDED) != 0;
    key.direction = frame.direction;
    return key;
}

// "N frames buffered", plus anything that makes the view less than a complete
// account of the traffic. Both notes are sticky until Clear: a drop that
// scrolled off the top still happened, and a reader deciding a message was
// never sent needs to know the viewer might simply not have been told.
void CanViewerDialog::updateCountLabel()
{
    const QString base =
        tr("%1 frames buffered").arg(QLocale().toString(qulonglong(m_frames.size())));
    QStringList notes;
    if (m_sawGap)
        notes << tr("frames were dropped");
    if (m_identifierLimit)
        notes << tr("identifier limit reached");
    m_countLabel->setText(notes.isEmpty()
                              ? base
                              : base + QStringLiteral(" — ") + notes.join(QStringLiteral(", ")));
}

void CanViewerDialog::onOverwriteToggled()
{
    const bool overwrite = m_overwriteCheck->isChecked();
    m_table->setColumnHidden(ColFrameCount, !overwrite);
    // Nothing scrolls in a table whose rows hold still, and the ordering is by
    // identifier rather than by arrival, so there is no "newest" row to chase.
    m_autoScrollCheck->setEnabled(!overwrite);
    rebuildTable();
}

// Rebuild the visible rows from the capture buffer. Called when a filter or the
// view mode changes, so hiding and re-showing a category restores its history
// instead of leaving a hole from the moment it was unticked.
void CanViewerDialog::rebuildTable()
{
    // Gap rows span several columns; a stale span left over one of the rows
    // this rebuild is about to write would swallow the cells underneath it.
    m_table->clearSpans();

    if (m_overwriteCheck->isChecked()) {
        rebuildOverwriteTable();
        return;
    }

    m_table->setUpdatesEnabled(false);
    m_table->setRowCount(0);
    // Walk backwards to find where the last screenful of MATCHING frames
    // starts, then replay forwards from there — filling the window without
    // building rows that would immediately be dropped off the top.
    int matching = 0;
    int firstIdx = 0;
    for (int i = int(m_frames.size()) - 1; i >= 0; --i) {
        if (frameVisible(m_frames[i]) && ++matching >= kMaxDisplayRows) {
            firstIdx = i;
            break;
        }
    }
    for (int i = firstIdx; i < int(m_frames.size()); ++i) {
        // Gap markers replay regardless of the filter — same reasoning as the
        // live path (see appendGapRow).
        if (m_frames[i].flags & ct::MONFLAG_GAP)
            appendGapRow(m_frames[i]);
        if (frameVisible(m_frames[i]))
            appendFrameRow(m_frames[i]);
    }
    m_table->setUpdatesEnabled(true);
    if (m_autoScrollCheck->isChecked())
        m_table->scrollToBottom();
}

// Overwrite Mode's table IS m_latest, in m_latest's own order — the map is
// keyed to sort by bus and then by arbitration ID, so walking it in order
// produces the rows in order. Each entry remembers the row it landed on, which
// is what lets the live path rewrite one row in place instead of searching.
void CanViewerDialog::rebuildOverwriteTable()
{
    m_table->setUpdatesEnabled(false);
    m_table->setRowCount(0);
    int row = 0;
    for (auto &pair : m_latest) {
        OverwriteEntry &entry = pair.second;
        if (!frameVisible(entry.frame)) {
            entry.row = -1;
            continue;
        }
        m_table->insertRow(row);
        entry.row = row;
        renderOverwriteRow(row, entry);
        ++row;
    }
    m_table->setUpdatesEnabled(true);
}

void CanViewerDialog::renderOverwriteRow(int row, const OverwriteEntry &entry)
{
    static const QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    const ct::MonitorStreamPayload &frame = entry.frame;

    setCellText(m_table, row, ColTime, QString::number(frame.timestamp_ms / 1000.0, 'f', 3));
    setCellText(m_table, row, ColBus, tr("CAN %1").arg(frame.bus_idx));
    setCellText(m_table, row, ColDir, frame.direction ? tr("Tx") : tr("Rx"));
    setCellText(m_table, row, ColId,
                formatCanId(frame.can_id, (frame.flags & ct::MONFLAG_EXTENDED) != 0), &mono);
    setCellText(m_table, row, ColLen, QString::number(qMin<int>(frame.data_len, 64)));
    setCellText(m_table, row, ColFrameCount, QLocale().toString(qulonglong(entry.count)));
    setCellText(m_table, row, ColData, formatData(frame), &mono);
}

void CanViewerDialog::onMonitorFrame(const ct::MonitorStreamPayload &frame)
{
    if (m_pauseCheck->isChecked())
        return;

    // Full capture buffer (independent of the on-screen window): O(1) append,
    // drop the single oldest frame once the cap is reached. Deliberately
    // unfiltered — the bus checkboxes hide rows, they do not stop capture.
    m_frames.push_back(frame);
    if (m_frames.size() > kMaxFrames)
        m_frames.pop_front();

    const bool gap = (frame.flags & ct::MONFLAG_GAP) != 0;
    if (gap)
        m_sawGap = true;

    // Latest-per-identifier, maintained in BOTH modes. Ticking Overwrite Mode
    // then shows the bus as it stands rather than filling in over the next few
    // seconds as each message comes round again, and the counts survive a
    // toggle instead of restarting. The cost is one map lookup per frame.
    auto it = m_latest.find(keyFor(frame));
    bool newIdentifier = false;
    if (it != m_latest.end()) {
        it->second.frame = frame;
        ++it->second.count;
    } else if (m_latest.size() < kMaxIdentifiers) {
        OverwriteEntry entry;
        entry.frame = frame;
        entry.count = 1;
        it = m_latest.emplace(keyFor(frame), entry).first;
        newIdentifier = true;
    } else {
        // Past the ceiling. Known identifiers keep updating; this one is not
        // tracked, and the frame count says so rather than quietly dropping it.
        it = m_latest.end();
        m_identifierLimit = true;
    }

    updateCountLabel();

    if (m_overwriteCheck->isChecked()) {
        if (it == m_latest.end())
            return;
        // A new identifier lands somewhere in the middle of the sort order and
        // shifts every row below it, so the table is rebuilt. That happens once
        // per identifier — the steady state is the branch below, which rewrites
        // the cells of one row that is already in the right place.
        if (newIdentifier) {
            if (frameVisible(frame))
                rebuildTable();
        } else if (it->second.row >= 0) {
            renderOverwriteRow(it->second.row, it->second);
        }
        return;
    }

    // A gap marker outlives the filter (see appendGapRow); only the frame row
    // itself is subject to the display checkboxes.
    if (!frameVisible(frame) && !gap)
        return;

    // Live table: a bounded window. Drop the oldest tenth in one batch when the
    // cap is reached — removing one row per frame would make every frame
    // O(rowCount) at steady state.
    if (m_table->rowCount() >= kMaxDisplayRows) {
        m_table->setUpdatesEnabled(false);
        const int drop = kMaxDisplayRows / 10;
        for (int i = 0; i < drop; ++i)
            m_table->removeRow(0);
        m_table->setUpdatesEnabled(true);
    }

    if (gap)
        appendGapRow(frame);
    if (frameVisible(frame))
        appendFrameRow(frame);

    if (m_autoScrollCheck->isChecked())
        m_table->scrollToBottom();
}

// A gap BEFORE a frame gets its own row rather than a mark on that frame. The
// device sets MONFLAG_GAP on the first frame that got through after it had to
// drop some, so the loss belongs between the previous row and this one — and a
// reader scanning for a missing message needs to see the hole where it
// happened, not a subtle attribute on the frame that survived it. Callers add
// this row whether or not the carrying frame's bus is displayed: the drop was
// from the whole monitor stream (one sticky flag in the firmware, not one per
// bus), and filtering the marker away with its bus made a filtered trace look
// complete exactly when it was not.
void CanViewerDialog::appendGapRow(const ct::MonitorStreamPayload &frame)
{
    const int gapRow = m_table->rowCount();
    m_table->insertRow(gapRow);
    auto *item = makeItem(tr("frames dropped — the device could not send them "
                             "as fast as the bus produced them"));
    QFont f = item->font();
    f.setItalic(true);
    item->setFont(f);
    item->setForeground(QBrush(QColor(0xB0, 0x50, 0x00)));
    m_table->setItem(gapRow, ColTime,
                     makeItem(QString::number(frame.timestamp_ms / 1000.0, 'f', 3)));
    m_table->setItem(gapRow, ColBus, item);
    m_table->setSpan(gapRow, ColBus, 1, ColData - ColBus + 1);
}

void CanViewerDialog::appendFrameRow(const ct::MonitorStreamPayload &frame)
{
    const int row = m_table->rowCount();
    m_table->insertRow(row);

    // Resolved once — this runs on the hot path at high frame rates.
    static const QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);

    m_table->setItem(row, ColTime,
                     makeItem(QString::number(frame.timestamp_ms / 1000.0, 'f', 3)));
    m_table->setItem(row, ColBus, makeItem(tr("CAN %1").arg(frame.bus_idx)));
    m_table->setItem(row, ColDir, makeItem(frame.direction ? tr("Tx") : tr("Rx")));

    auto *idItem = makeItem(formatCanId(frame.can_id, (frame.flags & ct::MONFLAG_EXTENDED) != 0));
    idItem->setFont(mono);
    m_table->setItem(row, ColId, idItem);

    m_table->setItem(row, ColLen, makeItem(QString::number(qMin<int>(frame.data_len, 64))));

    auto *dataItem = makeItem(formatData(frame));
    dataItem->setFont(mono);
    m_table->setItem(row, ColData, dataItem);
}

void CanViewerDialog::updateInjectSlot(int slot)
{
    InjectSlot &s = m_inject[slot];
    const bool running = s.timer->isActive();
    const bool once = s.rate->currentData().toInt() <= 0;
    s.button->setText(running ? tr("Stop") : (once ? tr("Send") : tr("Start")));
    // The frame is held still while it is being sent. Editing the ID or the
    // payload mid-run would change what is going out with nothing on screen to
    // say from which frame onwards.
    s.bus->setEnabled(!running);
    s.id->setEnabled(!running);
    s.extended->setEnabled(!running);
    s.data->setEnabled(!running);
}

void CanViewerDialog::stopInject(int slot)
{
    m_inject[slot].timer->stop();
    updateInjectSlot(slot);
}

void CanViewerDialog::onInjectClicked(int slot)
{
    InjectSlot &s = m_inject[slot];
    if (s.timer->isActive()) {
        stopInject(slot);
        return;
    }
    // Drop a re-press that lands while the initial send is still inside its
    // nested event loop. onInjectTick guards the same way; onInjectClicked did
    // not, so pressing Start again (or Enter in the Data field) during the
    // ~1.5 s a stalled requestSync holds would stack a second sync transaction
    // on the one DeviceLink. Placed AFTER the stop-check so a run can always be
    // stopped.
    if (s.inFlight)
        return;

    const int hz = s.rate->currentData().toInt();
    if (hz <= 0) {
        injectOnce(slot, /*announce=*/true);
        return;
    }

    // THE FIRST FRAME GOES BEFORE THE TIMER STARTS, and its verdict decides
    // whether the timer starts at all. A bad ID or a device that is not
    // answering is reported once, at the press that caused it, instead of the
    // run starting and failing on its first tick where the message has to be
    // suppressed to avoid a box per tick.
    QPointer<CanViewerDialog> self(this);
    if (!injectOnce(slot, /*announce=*/true))
        return;
    if (!self)
        return;

    s.timer->start(qMax(1, 1000 / hz));
    updateInjectSlot(slot);
}

void CanViewerDialog::onInjectTick(int slot)
{
    InjectSlot &s = m_inject[slot];
    // Skip, do not queue. The previous send is still inside its nested event
    // loop; letting this tick through would nest another one inside it.
    if (s.inFlight)
        return;

    QPointer<CanViewerDialog> self(this);
    if (injectOnce(slot, /*announce=*/false))
        return;
    if (!self)
        return;

    // A refused send ends the run rather than retrying forever. Stopped BEFORE
    // the box, so the timer cannot fire again into a modal loop and stack a
    // second warning behind the first.
    stopInject(slot);
    QMessageBox::warning(this, tr("CAN Viewer"),
                         tr("Injection on slot %1 stopped: the device did not accept the "
                            "frame.")
                             .arg(slot + 1));
}

bool CanViewerDialog::injectOnce(int slot, bool announce)
{
    InjectSlot &s = m_inject[slot];
    const auto refuse = [&](const QString &why) {
        if (announce)
            QMessageBox::warning(this, tr("CAN Viewer"), why);
        return false;
    };

    // --- Parse the CAN ID ----------------------------------------------------
    QString idText = s.id->text().trimmed();
    if (idText.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive))
        idText = idText.mid(2);
    if (idText.isEmpty())
        return refuse(tr("Enter a CAN ID (hex)."));
    bool ok = false;
    const quint32 canId = idText.toUInt(&ok, 16);
    if (!ok) {
        return refuse(
            tr("'%1' is not a valid hexadecimal CAN ID.").arg(s.id->text()));
    }
    const bool extended = s.extended->isChecked();
    const quint32 maxId = extended ? 0x1FFFFFFFu : 0x7FFu;
    if (canId > maxId) {
        return refuse(tr("CAN ID 0x%1 is out of range (maximum 0x%2 for %3 IDs).")
                          .arg(QString::number(canId, 16).toUpper(),
                               QString::number(maxId, 16).toUpper(),
                               extended ? tr("extended") : tr("standard")));
    }

    // --- Parse the data bytes ------------------------------------------------
    QString dataText = s.data->text();
    dataText.replace(QLatin1Char(','), QLatin1Char(' '));
    const QStringList tokens = dataText.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (tokens.size() > 64)
        return refuse(tr("Too many data bytes (%1); maximum is 64.").arg(tokens.size()));
    uint8_t bytes[64];
    std::memset(bytes, 0, sizeof(bytes));
    for (int i = 0; i < tokens.size(); ++i) {
        bool byteOk = false;
        const uint value = tokens.at(i).toUInt(&byteOk, 16);
        if (!byteOk || value > 0xFF)
            return refuse(tr("'%1' is not a valid hexadecimal byte.").arg(tokens.at(i)));
        bytes[i] = static_cast<uint8_t>(value);
    }

    // --- Build and send the payload -------------------------------------------
    InjectCanPayload payload;
    std::memset(&payload, 0, sizeof(payload));
    payload.bus_idx = static_cast<uint8_t>(s.bus->currentIndex() + 1); // 1..3
    payload.can_id = canId;
    payload.flags = extended ? 0x01 : 0x00;
    payload.data_len = static_cast<uint8_t>(tokens.size());
    std::memcpy(payload.data, bytes, sizeof(payload.data));

    QByteArray raw(reinterpret_cast<const char *>(&payload),
                   static_cast<int>(sizeof(payload)));
    QString error;
    // requestSync spins a nested event loop for up to ~1.5 s. This dialog is
    // modeless and WA_DeleteOnClose, so the user can close it while that loop
    // runs — and the deleteLater is delivered by the very same loop, destroying
    // `this` under our feet. The guard survives that: if the dialog is gone
    // when the call returns, nothing here may be touched, `s` included. Without
    // it the QMessageBox below dereferences a freed parent, and it does so
    // precisely when the device is not answering — the moment the user reaches
    // for the X.
    QPointer<CanViewerDialog> self(this);
    s.inFlight = true;
    const bool sent = m_link->requestSync(CMD_INJECT_CAN_FRAME, raw, nullptr, &error);
    if (!self)
        return false;
    s.inFlight = false;
    if (!sent && announce) {
        QMessageBox::warning(this, tr("CAN Viewer"),
                             tr("Failed to inject frame: %1").arg(error));
    }
    return sent;
}

void CanViewerDialog::onClearClicked()
{
    m_table->clearSpans(); // gap rows span columns; the spans must go with them
    m_table->setRowCount(0);
    m_frames.clear();
    m_frames.shrink_to_fit(); // release the capture buffer's memory
    // Clear empties Overwrite Mode too: its rows and counts are a summary of
    // the same capture, so leaving them would leave the viewer asserting
    // traffic it no longer holds any record of.
    m_latest.clear();
    m_sawGap = false;
    m_identifierLimit = false;
    updateCountLabel();
}

void CanViewerDialog::onSaveClicked()
{
    if (m_frames.empty()) {
        QMessageBox::information(this, tr("Save to File"), tr("There are no frames to save."));
        return;
    }

    const QString path = QFileDialog::getSaveFileName(
        this, tr("Save CAN Log"), QStringLiteral("canviewer.asc"),
        tr("Vector ASCII log (*.asc);;All files (*)"));
    if (path.isEmpty())
        return;

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, tr("Save to File"),
                             tr("Could not open '%1' for writing:\n%2")
                                 .arg(path, file.errorString()));
        return;
    }

    // Writing a large buffer (up to millions of lines) can take a while.
    QApplication::setOverrideCursor(Qt::WaitCursor);
    QTextStream out(&file);
    out << ascHeader(QDateTime::currentDateTime());
    // Timestamps are relative to the first buffered frame so the log starts
    // near zero (the device reports absolute uptime milliseconds).
    const quint32 t0 = m_frames.front().timestamp_ms;
    for (const MonitorStreamPayload &f : m_frames)
        out << ascFrameLine(f, t0) << '\n';
    out.flush();
    QApplication::restoreOverrideCursor();

    if (file.error() != QFileDevice::NoError) {
        QMessageBox::warning(this, tr("Save to File"),
                             tr("Error writing '%1':\n%2").arg(path, file.errorString()));
        return;
    }
    QMessageBox::information(this, tr("Save to File"),
                             tr("Saved %1 frames to %2.")
                                 .arg(QLocale().toString(qulonglong(m_frames.size())))
                                 .arg(QFileInfo(path).fileName()));
}

} // namespace ct
