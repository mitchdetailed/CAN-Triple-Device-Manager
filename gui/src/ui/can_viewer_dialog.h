// "CAN Viewer" — raw frame monitor fed by the device's monitor stream,
// plus an inject-frame panel (CMD_INJECT_CAN_FRAME). Non-modal.
#pragma once

#include <QDialog>

#include <deque>

#include "../protocol/device_link.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;

namespace ct {

class CanViewerDialog : public QDialog
{
    Q_OBJECT
public:
    explicit CanViewerDialog(DeviceLink *link, QWidget *parent = nullptr);

private:
    void onMonitorFrame(const ct::MonitorStreamPayload &frame);
    void onInjectClicked();
    void onClearClicked();
    void onSaveClicked(); // write the buffered frames as a Vector ASCII (.asc) log
    void appendFrameRow(const ct::MonitorStreamPayload &frame); // one row, no filtering
    // The "frames dropped" marker row for a frame carrying MONFLAG_GAP. Its own
    // method because callers must place it INDEPENDENTLY of the bus filter: the
    // device drops from the one monitor stream, so the loss is not scoped to
    // the bus of the frame that happened to carry the flag.
    void appendGapRow(const ct::MonitorStreamPayload &frame);
    // Per-bus DISPLAY filter. Capture and file export are never filtered, so a
    // hidden bus is still recorded and re-showing it restores its history.
    bool busVisible(quint8 busIdx) const;
    void rebuildTable(); // replay the buffer through the current filters

    // The live table is a bounded window for responsiveness; the full capture
    // buffer (for file export) is far larger and independent of it.
    static constexpr int kMaxDisplayRows = 5000;         // rows kept on screen
    static constexpr std::size_t kMaxFrames = 10'000'000; // capture buffer cap

    DeviceLink *m_link;
    QTableWidget *m_table;
    // Full capture buffer for file export — a deque so append and drop-oldest
    // are O(1) even at millions of frames (the table shows only a live window).
    std::deque<MonitorStreamPayload> m_frames;
    QCheckBox *m_pauseCheck;
    QCheckBox *m_autoScrollCheck;
    QCheckBox *m_busChecks[3]{}; // CAN 1..3 display filters
    QLabel *m_countLabel;

    // Inject panel
    QComboBox *m_injectBus;
    QLineEdit *m_injectId;      // hex
    QCheckBox *m_injectExtended;
    QLineEdit *m_injectData;    // hex bytes, space separated
    QPushButton *m_injectButton;
};

} // namespace ct
