// "Monitor Channels" (Online > Monitor Channels, F3) — live grid of channel
// values fed by the device's always-on value stream. Non-modal.
#pragma once

#include <QDialog>
#include <QElapsedTimer>
#include <QHash>
#include <QMap>

#include "../model/configuration.h"
#include "../protocol/device_link.h"

class QLabel;
class QTableWidget;
class QTimer;

namespace ct {

class MonitorChannelsDialog : public QDialog
{
    Q_OBJECT
public:
    MonitorChannelsDialog(DeviceLink *link, Configuration *config, QWidget *parent = nullptr);

    // Rebuilds the channel rows from the current document mapping (call after
    // the configuration changes / is sent).
    void rebuild();

private:
    void onSignalValues(const QList<ct::SignalValueEntry> &values);
    void onStaleTick(); // grays out rows not updated within ~2 s

    DeviceLink *m_link;
    Configuration *m_config;
    QTableWidget *m_table;
    QLabel *m_infoLabel;
    QTimer *m_staleTimer;
    QHash<int, int> m_signalToRow;        // device signal idx -> table row
    QHash<int, qint64> m_lastUpdateMs;    // device signal idx -> ms timestamp
    QHash<int, QString> m_signalUnits;    // device signal idx -> unit suffix
    QHash<int, int> m_signalDecimals;     // device signal idx -> decimal places
    // Only signals whose channel is enumerated (Channel::enumLabels non-empty)
    // have an entry, so the per-update lookup misses cheaply for the rest.
    QHash<int, QMap<int, QString>> m_signalEnumLabels;
    QElapsedTimer m_clock;
};

} // namespace ct
