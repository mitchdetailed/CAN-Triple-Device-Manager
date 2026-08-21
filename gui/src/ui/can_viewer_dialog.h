// "CAN Viewer" — raw frame monitor fed by the device's monitor stream,
// plus an inject-frame panel (CMD_INJECT_CAN_FRAME). Non-modal.
#pragma once

#include <QDialog>

#include <cstdint>
#include <deque>
#include <map>

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
    void onOverwriteToggled();
    void appendFrameRow(const ct::MonitorStreamPayload &frame); // one row, no filtering
    // The "frames dropped" marker row for a frame carrying MONFLAG_GAP. Its own
    // method because callers must place it INDEPENDENTLY of the bus filter: the
    // device drops from the one monitor stream, so the loss is not scoped to
    // the bus of the frame that happened to carry the flag.
    void appendGapRow(const ct::MonitorStreamPayload &frame);
    // DISPLAY filters: per-bus, plus "Tx Msgs" for the device's own frames.
    // Capture and file export are never filtered, so a hidden frame is still
    // recorded and re-showing its category restores the history.
    bool busVisible(quint8 busIdx) const;
    bool frameVisible(const ct::MonitorStreamPayload &frame) const;
    void rebuildTable(); // replay the buffer through the current filters
    void updateCountLabel();

    // --- Overwrite Mode ------------------------------------------------------
    // One row per identifier carrying only its most recent frame, in the shape
    // of PCAN-View's Receive/Transmit tab. The identity of a "message" is the
    // bus it came from, its arbitration ID, whether that ID is extended, and
    // its direction — a standard 0x100 and an extended 0x100 are different
    // frames on the wire, and the device's own transmit of an ID it also
    // receives is not the same traffic.
    struct OverwriteKey {
        quint8 bus;
        quint32 canId;
        bool extended;
        quint8 direction;
        // Orders the table: by bus, then by arbitration ID, then by the
        // remaining fields so the order is total and stable.
        bool operator<(const OverwriteKey &o) const
        {
            if (bus != o.bus) return bus < o.bus;
            if (canId != o.canId) return canId < o.canId;
            if (extended != o.extended) return extended < o.extended;
            return direction < o.direction;
        }
    };
    struct OverwriteEntry {
        ct::MonitorStreamPayload frame{}; // the most recent one
        quint64 count = 0;                // frames seen for this identifier
        int row = -1;                     // its table row, or -1 if filtered out
    };
    static OverwriteKey keyFor(const ct::MonitorStreamPayload &frame);
    void rebuildOverwriteTable();
    void renderOverwriteRow(int row, const OverwriteEntry &entry);

    // The live table is a bounded window for responsiveness; the full capture
    // buffer (for file export) is far larger and independent of it.
    static constexpr int kMaxDisplayRows = 5000;         // rows kept on screen
    static constexpr std::size_t kMaxFrames = 10'000'000; // capture buffer cap
    // Overwrite Mode's row count is bounded by the number of DISTINCT
    // identifiers, not by the frame rate, so it needs its own ceiling: a node
    // spraying random extended IDs is exactly the fault you would open this
    // viewer to diagnose, and it must not be able to grow the table without
    // limit. Far above any real bus — a busy J1939 network carries a few
    // hundred. Past it, existing identifiers keep updating and new ones are
    // ignored, which the frame count says out loud.
    static constexpr std::size_t kMaxIdentifiers = 10'000;

    DeviceLink *m_link;
    QTableWidget *m_table;
    // Full capture buffer for file export — a deque so append and drop-oldest
    // are O(1) even at millions of frames (the table shows only a live window).
    std::deque<MonitorStreamPayload> m_frames;
    // Latest frame per identifier. Maintained in BOTH modes so that ticking
    // Overwrite Mode shows the bus immediately instead of waiting for every
    // message to come round again, and so the counts survive the toggle.
    std::map<OverwriteKey, OverwriteEntry> m_latest;
    bool m_sawGap = false;            // any MONFLAG_GAP since the last Clear
    bool m_identifierLimit = false;   // kMaxIdentifiers reached
    QCheckBox *m_pauseCheck;
    QCheckBox *m_autoScrollCheck;
    QCheckBox *m_busChecks[3]{}; // CAN 1..3 display filters
    QCheckBox *m_txCheck;        // show the device's own transmitted frames
    QCheckBox *m_overwriteCheck;
    QLabel *m_countLabel;

    // Inject panel
    QComboBox *m_injectBus;
    QLineEdit *m_injectId;      // hex
    QCheckBox *m_injectExtended;
    QLineEdit *m_injectData;    // hex bytes, space separated
    QPushButton *m_injectButton;
};

} // namespace ct
