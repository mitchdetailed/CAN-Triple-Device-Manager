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
#include <QStringList>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTextStream>
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
    ColData,
    ColCount
};

QTableWidgetItem *makeItem(const QString &text)
{
    auto *item = new QTableWidgetItem(text);
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    return item;
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
    auto *saveButton = new QPushButton(tr("Save to File…"), this);
    auto *clearButton = new QPushButton(tr("Clear"), this);

    topRow->addWidget(m_pauseCheck);
    topRow->addWidget(m_autoScrollCheck);

    // Per-bus display filters. These hide rows; they do NOT stop capture. Every
    // frame the device streams still goes into the buffer and into Save to
    // File, so unchecking a bus to read a quiet one cannot silently cost you
    // the trace you were recording — and re-checking brings its history back.
    topRow->addSpacing(12);
    topRow->addWidget(new QLabel(tr("Show:"), this));
    for (int b = 0; b < 3; ++b) {
        m_busChecks[b] = new QCheckBox(tr("CAN %1").arg(b + 1), this);
        m_busChecks[b]->setChecked(true);
        m_busChecks[b]->setToolTip(tr("Show frames from CAN %1. Hiding a bus only affects "
                                      "this list — every bus is still captured and saved.")
                                       .arg(b + 1));
        connect(m_busChecks[b], &QCheckBox::toggled, this, &CanViewerDialog::rebuildTable);
        topRow->addWidget(m_busChecks[b]);
    }

    topRow->addStretch(1);
    topRow->addWidget(m_countLabel);
    topRow->addWidget(saveButton);
    topRow->addWidget(clearButton);
    mainLayout->addLayout(topRow);

    // --- Frame table --------------------------------------------------------
    m_table = new QTableWidget(0, ColCount, this);
    m_table->setHorizontalHeaderLabels(QStringList()
                                       << tr("Time (s)") << tr("Bus") << tr("Dir")
                                       << tr("ID") << tr("Len") << tr("Data"));
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
    mainLayout->addWidget(m_table, 1);

    // --- Inject panel --------------------------------------------------------
    auto *injectBox = new QGroupBox(tr("Inject Frame"), this);
    injectBox->setToolTip(tr("A payload of 9 or more bytes is sent as a CAN FD frame; 8 or "
                             "fewer as a classic frame. The frame is also received by the "
                             "device — parsed, and forwarded by any matching relay."));
    auto *injectRow = new QHBoxLayout(injectBox);

    injectRow->addWidget(new QLabel(tr("Bus:"), injectBox));
    m_injectBus = new QComboBox(injectBox);
    m_injectBus->addItem(tr("CAN 1"));
    m_injectBus->addItem(tr("CAN 2"));
    m_injectBus->addItem(tr("CAN 3"));
    injectRow->addWidget(m_injectBus);

    injectRow->addWidget(new QLabel(tr("ID:"), injectBox));
    m_injectId = new QLineEdit(injectBox);
    m_injectId->setPlaceholderText(tr("0x7E0"));
    m_injectId->setMaximumWidth(100);
    injectRow->addWidget(m_injectId);

    m_injectExtended = new QCheckBox(tr("Extended"), injectBox);
    injectRow->addWidget(m_injectExtended);

    injectRow->addWidget(new QLabel(tr("Data:"), injectBox));
    m_injectData = new QLineEdit(injectBox);
    m_injectData->setPlaceholderText(tr("00 11 22 33 44 55 66 77"));
    injectRow->addWidget(m_injectData, 1);

    m_injectButton = new QPushButton(tr("Send"), injectBox);
    injectRow->addWidget(m_injectButton);

    const QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    m_injectId->setFont(mono);
    m_injectData->setFont(mono);

    mainLayout->addWidget(injectBox);

    connect(saveButton, &QPushButton::clicked, this, &CanViewerDialog::onSaveClicked);
    connect(clearButton, &QPushButton::clicked, this, &CanViewerDialog::onClearClicked);
    connect(m_injectButton, &QPushButton::clicked, this, &CanViewerDialog::onInjectClicked);
    connect(m_injectId, &QLineEdit::returnPressed, this, &CanViewerDialog::onInjectClicked);
    connect(m_injectData, &QLineEdit::returnPressed, this, &CanViewerDialog::onInjectClicked);
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

// Rebuild the visible rows from the capture buffer. Called when a bus filter
// changes, so hiding and re-showing a bus restores its history instead of
// leaving a hole from the moment it was unticked.
void CanViewerDialog::rebuildTable()
{
    m_table->setUpdatesEnabled(false);
    m_table->setRowCount(0);
    // Walk backwards to find where the last screenful of MATCHING frames
    // starts, then replay forwards from there — filling the window without
    // building rows that would immediately be dropped off the top.
    int matching = 0;
    int firstIdx = 0;
    for (int i = int(m_frames.size()) - 1; i >= 0; --i) {
        if (busVisible(m_frames[i].bus_idx) && ++matching >= kMaxDisplayRows) {
            firstIdx = i;
            break;
        }
    }
    for (int i = firstIdx; i < int(m_frames.size()); ++i) {
        // Gap markers replay regardless of the filter — same reasoning as the
        // live path (see appendGapRow).
        if (m_frames[i].flags & ct::MONFLAG_GAP)
            appendGapRow(m_frames[i]);
        if (busVisible(m_frames[i].bus_idx))
            appendFrameRow(m_frames[i]);
    }
    m_table->setUpdatesEnabled(true);
    if (m_autoScrollCheck->isChecked())
        m_table->scrollToBottom();
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

    m_countLabel->setText(tr("%1 frames buffered")
                              .arg(QLocale().toString(qulonglong(m_frames.size()))));

    // A gap marker outlives the filter (see appendGapRow); only the frame row
    // itself is subject to the bus checkboxes.
    const bool gap = (frame.flags & ct::MONFLAG_GAP) != 0;
    if (!busVisible(frame.bus_idx) && !gap)
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
    if (busVisible(frame.bus_idx))
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

    // Standard IDs shown as 0x%03X, extended as 0x%08X (the width marks the
    // format; matches the section editor's Base Address presentation).
    const bool extended = frame.flags & 0x01;
    const QString idText =
        QStringLiteral("0x")
        + QStringLiteral("%1").arg(frame.can_id, extended ? 8 : 3, 16, QLatin1Char('0')).toUpper();
    auto *idItem = makeItem(idText);
    idItem->setFont(mono);
    m_table->setItem(row, ColId, idItem);

    const int len = qMin<int>(frame.data_len, 64);
    m_table->setItem(row, ColLen, makeItem(QString::number(len)));

    QString dataText;
    dataText.reserve(len * 3);
    for (int i = 0; i < len; ++i) {
        if (i)
            dataText += QLatin1Char(' ');
        dataText += QStringLiteral("%1").arg(frame.data[i], 2, 16, QLatin1Char('0')).toUpper();
    }
    auto *dataItem = makeItem(dataText);
    dataItem->setFont(mono);
    m_table->setItem(row, ColData, dataItem);
}

void CanViewerDialog::onInjectClicked()
{
    // --- Parse the CAN ID ----------------------------------------------------
    QString idText = m_injectId->text().trimmed();
    if (idText.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive))
        idText = idText.mid(2);
    if (idText.isEmpty()) {
        QMessageBox::warning(this, tr("CAN Viewer"), tr("Enter a CAN ID (hex)."));
        return;
    }
    bool ok = false;
    const quint32 canId = idText.toUInt(&ok, 16);
    if (!ok) {
        QMessageBox::warning(this, tr("CAN Viewer"),
                             tr("'%1' is not a valid hexadecimal CAN ID.").arg(m_injectId->text()));
        return;
    }
    const bool extended = m_injectExtended->isChecked();
    const quint32 maxId = extended ? 0x1FFFFFFFu : 0x7FFu;
    if (canId > maxId) {
        QMessageBox::warning(this, tr("CAN Viewer"),
                             tr("CAN ID 0x%1 is out of range (maximum 0x%2 for %3 IDs).")
                                 .arg(QString::number(canId, 16).toUpper(),
                                      QString::number(maxId, 16).toUpper(),
                                      extended ? tr("extended") : tr("standard")));
        return;
    }

    // --- Parse the data bytes ------------------------------------------------
    QString dataText = m_injectData->text();
    dataText.replace(QLatin1Char(','), QLatin1Char(' '));
    const QStringList tokens =
        dataText.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (tokens.size() > 64) {
        QMessageBox::warning(this, tr("CAN Viewer"),
                             tr("Too many data bytes (%1); maximum is 64.").arg(tokens.size()));
        return;
    }
    uint8_t bytes[64];
    std::memset(bytes, 0, sizeof(bytes));
    for (int i = 0; i < tokens.size(); ++i) {
        bool byteOk = false;
        const uint value = tokens.at(i).toUInt(&byteOk, 16);
        if (!byteOk || value > 0xFF) {
            QMessageBox::warning(this, tr("CAN Viewer"),
                                 tr("'%1' is not a valid hexadecimal byte.").arg(tokens.at(i)));
            return;
        }
        bytes[i] = static_cast<uint8_t>(value);
    }

    // --- Build and send the payload -------------------------------------------
    InjectCanPayload payload;
    std::memset(&payload, 0, sizeof(payload));
    payload.bus_idx = static_cast<uint8_t>(m_injectBus->currentIndex() + 1); // 1..3
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
    // when the call returns, there is nothing left to report to. Without it the
    // QMessageBox below dereferences a freed parent, and it does so precisely
    // when the device is not answering — the moment the user reaches for the X.
    QPointer<CanViewerDialog> self(this);
    const bool sent = m_link->requestSync(CMD_INJECT_CAN_FRAME, raw, nullptr, &error);
    if (!self)
        return;
    if (!sent) {
        QMessageBox::warning(this, tr("CAN Viewer"),
                             tr("Failed to inject frame: %1").arg(error));
    }
}

void CanViewerDialog::onClearClicked()
{
    m_table->setRowCount(0);
    m_frames.clear();
    m_frames.shrink_to_fit(); // release the capture buffer's memory
    m_countLabel->setText(tr("%1 frames buffered").arg(0));
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
