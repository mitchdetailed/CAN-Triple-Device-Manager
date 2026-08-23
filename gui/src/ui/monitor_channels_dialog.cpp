// Implementation of the "Monitor Channels" dialog (Online > Monitor
// Channels, F3). Live grid of channel values fed by the device's always-on
// value stream.
#include "monitor_channels_dialog.h"

#include <QBrush>
#include <QHeaderView>
#include <QLabel>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

#include "../model/channel.h"
#include "../model/device_mapper.h"

namespace ct {

namespace {

constexpr int kColChannel = 0;
constexpr int kColValue = 1;
constexpr int kColUnits = 2;

constexpr int kStaleTickMs = 500;
constexpr qint64 kStaleAfterMs = 2000;

// Role on the Value item that remembers whether it is currently grayed, so
// the stale tick only touches items whose state actually changes.
constexpr int kGrayedRole = Qt::UserRole + 1;

// A mapper error, made safe to print on this dialog.
//
// device_mapper writes every error with the message it came from at the front —
// "CAN 1 · Engine Data · RPM: start bit 60 + 16 bits runs past the 8-byte
// frame" — and everything after that prefix is start bit, bit length, byte
// order and frame length. For a CONCEALED message that tail IS the protocol the
// protection exists to withhold, and this dialog is one keystroke away (F3) from
// anybody holding the file, password or no password. So when the message is
// concealed from this viewer the detail goes and the name stays: a customer
// still has to be able to say WHICH message is complaining when they ring
// whoever supplied the configuration, and the name is the one thing a concealed
// message is allowed to say about itself.
//
// Concealed, NOT edit-locked. A Read Only message's errors are printed in full,
// because Read Only withholds nothing — showing a customer "(protected)" in
// place of a fault they are entitled to see and able to fix would be the v21
// behaviour this release exists to undo.
QString safeMappingError(const Configuration &config, const QString &error)
{
    // No commsRevealed() short-circuit. That flag is the DOCUMENT's Edit
    // Protected Comms state and is true for any document that has no such
    // password — which is the normal shape when the sections carry their own —
    // so it returned every Hidden message's start bits and frame lengths in
    // full. The per-section walk below is the only answer.
    for (int busIdx = 0; busIdx < 3; ++busIdx) {
        for (const CommsSection &section : config.bus[busIdx].sections) {
            // Per section AND per bus: one unlocked with its own password this
            // session reports its faults in full while its neighbours stay
            // collapsed, and a grant for a same-named section on another bus
            // opens neither.
            if (!section.isConcealed(config.isSectionRevealed(section, busIdx)))
                continue;
            const QString loc =
                QStringLiteral("CAN %1 · %2").arg(busIdx + 1).arg(section.name);
            if (!error.startsWith(loc))
                continue;
            // The whole location has to match, not just its start. The mapper
            // writes ": " or " · " after the prefix, so anchoring on one of
            // those stops a protected message called "Fuel" from swallowing an
            // error that belongs to an unprotected "Fuel Pressure" — hiding
            // more than necessary is not free here, it would blame the wrong
            // message for a fault the user could otherwise have fixed.
            const QString rest = error.mid(loc.size());
            if (!rest.isEmpty() && !rest.startsWith(QLatin1Char(':'))
                && !rest.startsWith(QStringLiteral(" ·")))
                continue;
            return QObject::tr("%1 — this message is withheld, so what is wrong with it "
                               "cannot be shown here. Open it in Connections > "
                               "Communications with its password to see the detail, or ask "
                               "whoever supplied this configuration")
                .arg(loc);
        }
    }
    return error;
}

} // namespace

MonitorChannelsDialog::MonitorChannelsDialog(DeviceLink *link, Configuration *config,
                                             QWidget *parent)
    : QDialog(parent)
    , m_link(link)
    , m_config(config)
    , m_table(nullptr)
    , m_infoLabel(nullptr)
    , m_staleTimer(nullptr)
{
    setWindowTitle(tr("Monitor Channels"));
    resize(500, 600);

    m_clock.start();

    m_infoLabel = new QLabel(this);
    m_infoLabel->setWordWrap(true);

    m_table = new QTableWidget(0, 3, this);
    m_table->setHorizontalHeaderLabels(
        QStringList() << tr("Channel") << tr("Value") << tr("Units"));
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setAlternatingRowColors(true);
    m_table->verticalHeader()->setVisible(false);
    m_table->verticalHeader()->setDefaultSectionSize(
        m_table->fontMetrics().height() + 6);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->setColumnWidth(kColChannel, 220);
    m_table->setColumnWidth(kColValue, 110);
    m_table->setSortingEnabled(false);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);
    layout->addWidget(m_infoLabel);
    layout->addWidget(m_table, 1);

    m_staleTimer = new QTimer(this);
    m_staleTimer->setInterval(kStaleTickMs);
    connect(m_staleTimer, &QTimer::timeout, this, &MonitorChannelsDialog::onStaleTick);
    m_staleTimer->start();

    if (m_link) {
        connect(m_link, &DeviceLink::signalValues, this,
                &MonitorChannelsDialog::onSignalValues);
    }
    if (m_config) {
        connect(m_config, &Configuration::documentReset, this,
                &MonitorChannelsDialog::rebuild);
    }

    rebuild();
}

void MonitorChannelsDialog::rebuild()
{
    m_signalToRow.clear();
    m_lastUpdateMs.clear();
    m_signalUnits.clear();
    m_signalDecimals.clear();
    m_signalEnumLabels.clear();
    m_table->setRowCount(0);

    if (!m_config) {
        m_infoLabel->setText(tr("No configuration."));
        return;
    }

    const MappingResult mapping = mapToDevice(*m_config);

    // Sort rows by channel name (case-insensitive) for a stable listing.
    QList<QPair<QString, int>> entries; // channel name, signal idx
    entries.reserve(mapping.signalToChannel.size());
    for (auto it = mapping.signalToChannel.constBegin();
         it != mapping.signalToChannel.constEnd(); ++it) {
        entries.append(qMakePair(it.value(), it.key()));
    }
    std::sort(entries.begin(), entries.end(),
              [](const QPair<QString, int> &a, const QPair<QString, int> &b) {
                  const int cmp = QString::compare(a.first, b.first, Qt::CaseInsensitive);
                  if (cmp != 0)
                      return cmp < 0;
                  return a.second < b.second;
              });

    m_table->setRowCount(entries.size());
    const ChannelCatalog &catalog = m_config->catalog();

    for (int row = 0; row < entries.size(); ++row) {
        const QString &name = entries.at(row).first;
        const int signalIdx = entries.at(row).second;

        const Channel ch = catalog.findByName(name);
        const QString unit = ch.isValid() ? ch.unit : QString();
        const int decimals = ch.isValid() ? ch.decimalPlaces : 0;

        m_signalToRow.insert(signalIdx, row);
        m_signalUnits.insert(signalIdx, unit);
        m_signalDecimals.insert(signalIdx, decimals);
        if (ch.isValid() && !ch.enumLabels.isEmpty())
            m_signalEnumLabels.insert(signalIdx, ch.enumLabels);

        // Bare name, not channelLabel(). The unit is not missing from this row:
        // it is the Units column two cells along, filled from this same
        // `unit`, and it reads with the number it belongs to. Decorating the
        // name would put "°C" twice on every row of a three-column grid.
        //
        // Nothing reads this item's text back in any case — a value update
        // finds its row through m_signalToRow, keyed by the device's signal
        // index — so the text here is display only whichever way it is written.
        auto *nameItem = new QTableWidgetItem(name);
        auto *valueItem = new QTableWidgetItem(QStringLiteral("—"));
        valueItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        valueItem->setData(kGrayedRole, false);
        auto *unitItem = new QTableWidgetItem(unit);

        m_table->setItem(row, kColChannel, nameItem);
        m_table->setItem(row, kColValue, valueItem);
        m_table->setItem(row, kColUnits, unitItem);
    }

    if (!mapping.errors.isEmpty())
        m_infoLabel->setText(tr("%n channels — the document has %1 mapping error(s); the device "
                                "mapping may be incomplete. First: %2",
                                nullptr, entries.size())
                                 .arg(mapping.errors.size())
                                 .arg(safeMappingError(*m_config, mapping.errors.first())));
    else
        m_infoLabel->setText(tr("%n channels (mapping of the current document — send the "
                                "configuration to match the device)",
                                nullptr, entries.size()));
}

void MonitorChannelsDialog::onSignalValues(const QList<ct::SignalValueEntry> &values)
{
    const qint64 now = m_clock.elapsed();

    for (const SignalValueEntry &entry : values) {
        const int signalIdx = entry.signal_idx;
        const auto rowIt = m_signalToRow.constFind(signalIdx);
        if (rowIt == m_signalToRow.constEnd())
            continue;

        QTableWidgetItem *valueItem = m_table->item(rowIt.value(), kColValue);
        if (!valueItem)
            continue;

        const int decimals = m_signalDecimals.value(signalIdx, 0);
        QString text = QString::number(double(entry.physical_value), 'f', decimals);
        // An enumerated channel (Device Last Reset Reason) shows its label
        // with the number — "Power On (1)" — because the bare number is a
        // manual lookup at exactly the moment someone is asking why a unit
        // rebooted. A value the map does not name stays the bare number: a
        // newer firmware's new reason must read as unknown, not borrow the
        // nearest wrong label.
        const auto enumIt = m_signalEnumLabels.constFind(signalIdx);
        if (enumIt != m_signalEnumLabels.constEnd()) {
            const auto labelIt =
                enumIt->constFind(int(std::lround(double(entry.physical_value))));
            if (labelIt != enumIt->constEnd())
                text = QStringLiteral("%1 (%2)").arg(labelIt.value()).arg(labelIt.key());
        }
        valueItem->setText(text);
        if (valueItem->data(kGrayedRole).toBool()) {
            valueItem->setData(Qt::ForegroundRole, QVariant());
            valueItem->setData(kGrayedRole, false);
        }
        m_lastUpdateMs.insert(signalIdx, now);
    }
}

void MonitorChannelsDialog::onStaleTick()
{
    if (m_signalToRow.isEmpty())
        return;

    const qint64 now = m_clock.elapsed();

    for (auto it = m_signalToRow.constBegin(); it != m_signalToRow.constEnd(); ++it) {
        const int signalIdx = it.key();
        QTableWidgetItem *valueItem = m_table->item(it.value(), kColValue);
        if (!valueItem)
            continue;

        const auto updIt = m_lastUpdateMs.constFind(signalIdx);
        if (updIt == m_lastUpdateMs.constEnd())
            continue; // never updated: leave the "—" placeholder in default color

        const bool stale = (now - updIt.value()) > kStaleAfterMs;
        const bool grayed = valueItem->data(kGrayedRole).toBool();
        if (stale && !grayed) {
            valueItem->setForeground(QBrush(Qt::gray));
            valueItem->setData(kGrayedRole, true);
        } else if (!stale && grayed) {
            valueItem->setData(Qt::ForegroundRole, QVariant());
            valueItem->setData(kGrayedRole, false);
        }
    }
}

} // namespace ct
