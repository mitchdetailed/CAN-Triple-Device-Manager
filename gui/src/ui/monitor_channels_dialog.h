// "Monitor Channels" (Online > Monitor Channels, F3) — live grid of channel
// values fed by the device's always-on value stream. Non-modal.
//
// With device firmware 1.0.12 the grid also OVERRIDES: an Override tick and an
// editor per channel pin that channel on the device at the editor's value
// (CMD_SET_OVERRIDE), so transmit messages, conditions and calculations run
// against a made-up reading. The dialog is the override's only owner: it
// refreshes the device's lease every second while anything is pinned, and
// closing it releases everything. The device releases them itself a few
// seconds after the dialog's last word (OVERRIDE_LEASE_MS), which is what
// makes a pulled cable safe. Device channels — written by the hardware — get
// no tick. On older firmware the two columns stay hidden.
#pragma once

#include <QDialog>
#include <QElapsedTimer>
#include <QHash>
#include <QMap>
#include <QSet>

#include "../model/channel.h"
#include "../model/configuration.h"
#include "../protocol/device_link.h"

class QCloseEvent;
class QLabel;
class QPushButton;
class QTableWidget;
class QTableWidgetItem;
class QTimer;

namespace ct {

class MonitorChannelsDialog : public QDialog
{
    Q_OBJECT
public:
    MonitorChannelsDialog(DeviceLink *link, Configuration *config, QWidget *parent = nullptr);

    // Rebuilds the channel rows from the current document mapping (call after
    // the configuration changes / is sent). Releases every override first:
    // the rows it belonged to are about to be replaced.
    void rebuild();

    // How many channels this dialog currently has pinned on the device.
    int overrideCount() const { return m_overriddenRows.size(); }

protected:
    // Closing releases every override on the device, then the lease stops.
    void closeEvent(QCloseEvent *event) override;

private:
    void onSignalValues(const QList<ct::SignalValueEntry> &values);
    void onStaleTick(); // grays out rows not updated within ~2 s

    // Overrides. The probe sends one lease command: older firmware answers
    // "unknown command", and the columns stay hidden.
    void probeOverrideSupport();
    void onOverrideToggled(QTableWidgetItem *item);
    void sendOverride(int row);
    void onLeaseTick();
    void clearAllOverrides(bool tellDevice);
    void updateOverrideSummary();
    QWidget *makeOverrideEditor(const Channel &ch, int row);
    double editorValue(int row) const;

    DeviceLink *m_link;
    Configuration *m_config;
    QTableWidget *m_table;
    QLabel *m_infoLabel;
    QTimer *m_staleTimer;
    QPushButton *m_clearOverridesButton = nullptr;
    QTimer *m_leaseTimer = nullptr;
    QHash<int, int> m_signalToRow;        // device signal idx -> table row
    QHash<int, int> m_rowToSignal;        // table row -> device signal idx
    QHash<int, qint64> m_lastUpdateMs;    // device signal idx -> ms timestamp
    QHash<int, QString> m_signalUnits;    // device signal idx -> unit suffix
    QHash<int, int> m_signalDecimals;     // device signal idx -> decimal places
    // Only signals whose channel is enumerated (Channel::enumLabels non-empty)
    // have an entry, so the per-update lookup misses cheaply for the rest.
    QHash<int, QMap<int, QString>> m_signalEnumLabels;
    QSet<int> m_overriddenRows;           // rows pinned on the device right now
    bool m_overridesSupported = false;
    bool m_overrideProbeDone = false;
    bool m_updatingChecks = false;        // guards itemChanged while the code ticks boxes
    QElapsedTimer m_clock;
};

} // namespace ct
