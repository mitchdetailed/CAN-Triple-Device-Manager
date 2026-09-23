// Implementation of the "Monitor Channels" dialog (Online > Monitor
// Channels, F3). Live grid of channel values fed by the device's always-on
// value stream.
#include "monitor_channels_dialog.h"
#include "window_memory.h"

#include <QBrush>
#include <QCloseEvent>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QPushButton>
#include <QHeaderView>
#include <QLabel>
#include <QSet>
#include <QSettings>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <climits>
#include <cmath>
#include <utility>

#include "../model/channel.h"
#include "../model/device_mapper.h"
#include "../protocol/device_session.h"
#include "monitor_channel_select_dialog.h"

namespace ct {

namespace {

constexpr int kColChannel = 0;
constexpr int kColValue = 1;
constexpr int kColUnits = 2;
constexpr int kColOverride = 3;
constexpr int kColOverrideValue = 4;
// How often the device's override lease is refreshed while anything is pinned.
// The device releases everything at OVERRIDE_LEASE_MS (3 s) of silence, so a
// second gives two missed refreshes of slack before a live override drops.
constexpr int kLeaseTickMs = 1000;

constexpr int kStaleTickMs = 500;
constexpr qint64 kStaleAfterMs = 2000;

// QSettings key for the channel selection, so the set being watched outlives
// any one opening of this window. Names, not indices: a name still means the
// same channel after a rebuild, an index does not.
const char *kSelectionKey = "monitorChannelSelection";

// Role on the Value item that remembers whether it is currently grayed, so
// the stale tick only touches items whose state actually changes.
constexpr int kGrayedRole = Qt::UserRole + 1;

// The override editor's limits for a channel: what its data type can hold at
// its decimal places (an integer channel stores a scaled integer, so u8 at one
// decimal place reaches 25.5), narrowed to the channel's own range. The same
// arithmetic the constants editor applies to a constant's type, spelled for
// the catalogue's type names; a float channel takes the dialogs' nominal span.
struct OverrideSpan {
    double lo = -1e9;
    double hi = 1e9;
    double step = 1.0;
    int decimals = 0;
};

OverrideSpan overrideSpanFor(const Channel &ch)
{
    OverrideSpan s;
    s.decimals = qBound(0, ch.decimalPlaces, 6);
    const double res = std::pow(10.0, -s.decimals);
    s.step = ch.baseResolution > 0.0 ? ch.baseResolution : res;
    struct Reach {
        const char *type;
        double lo;
        double hi;
    };
    static const Reach kReach[] = {
        {"u8", 0.0, 255.0},           {"u16", 0.0, 65535.0},
        {"u32", 0.0, 4294967295.0},   {"s8", -128.0, 127.0},
        {"s16", -32768.0, 32767.0},   {"s32", -2147483648.0, 2147483647.0},
    };
    for (const Reach &r : kReach) {
        if (ch.dataType == QLatin1String(r.type)) {
            s.lo = r.lo * res;
            s.hi = r.hi * res;
            break;
        }
    }
    s.lo = std::max(s.lo, ch.minValue);
    s.hi = std::min(s.hi, ch.maxValue);
    if (s.hi < s.lo)
        s.hi = s.lo;
    return s;
}

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
    resize(760, 600);
    rememberWindowSize(this, QStringLiteral("monitorChannels")); // the default, until the user changes it

    m_clock.start();

    m_infoLabel = new QLabel(this);
    m_infoLabel->setWordWrap(true);

    m_table = new QTableWidget(0, 5, this);
    m_table->setHorizontalHeaderLabels(QStringList() << tr("Channel") << tr("Value") << tr("Units")
                                                     << tr("Override") << tr("Override Value"));
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
    m_table->setColumnWidth(kColUnits, 70);
    m_table->setColumnWidth(kColOverride, 76);
    m_table->setSortingEnabled(false);
    // Hidden until the device says it can (probeOverrideSupport).
    m_table->setColumnHidden(kColOverride, true);
    m_table->setColumnHidden(kColOverrideValue, true);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);
    layout->addWidget(m_infoLabel);

    // THE SELECTION. A document maps a few hundred channels and a person
    // watches a dozen; Select Channels… opens a picker
    // (MonitorChannelSelectDialog) whose right-hand list is what the grid
    // shows, top to bottom — so the channels being compared can sit side by
    // side whatever their names. A name filter was the first answer and was
    // replaced by this at Mitch's request: picking from a list, with Ctrl and
    // Shift, and dragging the picks into order.
    //
    // Applying a selection rebuilds the rows in that order (rebuildRows) and
    // hides the rest: every channel stays mapped and updated, and the pinned
    // channels are carried across, so Show All shows current values with the
    // pins still standing. A pinned row is exempt from hiding — a selection
    // must not be able to hide a value that is driving the device. Remembered
    // across openings of this window (QSettings), because the set being
    // watched outlives any one look at it.
    auto *selectRow = new QHBoxLayout;
    m_selectButton = new QPushButton(tr("Select Channels…"), this);
    m_selectButton->setObjectName(QStringLiteral("selectChannels"));
    m_selectButton->setToolTip(tr("Choose which channels the grid shows, and in what order."));
    connect(m_selectButton, &QPushButton::clicked, this,
            &MonitorChannelsDialog::onSelectChannels);
    selectRow->addWidget(m_selectButton);
    m_showAllButton = new QPushButton(tr("Show All"), this);
    m_showAllButton->setObjectName(QStringLiteral("showAllChannels"));
    connect(m_showAllButton, &QPushButton::clicked, this, [this]() {
        m_selection.clear();
        QSettings().setValue(QLatin1String(kSelectionKey), m_selection);
        rebuildRows(/*keepOverrides=*/true); // back to name order
    });
    selectRow->addWidget(m_showAllButton);
    m_selectionLabel = new QLabel(this);
    m_selectionLabel->setObjectName(QStringLiteral("selectionLabel"));
    selectRow->addWidget(m_selectionLabel);
    selectRow->addStretch(1);
    layout->addLayout(selectRow);
    m_selection = QSettings().value(QLatin1String(kSelectionKey)).toStringList();

    layout->addWidget(m_table, 1);

    auto *buttons = new QHBoxLayout;
    m_clearOverridesButton = new QPushButton(tr("Clear All Overrides"), this);
    m_clearOverridesButton->setEnabled(false);
    m_clearOverridesButton->setVisible(false);
    connect(m_clearOverridesButton, &QPushButton::clicked, this,
            [this]() { clearAllOverrides(true); });
    buttons->addWidget(m_clearOverridesButton);
    buttons->addStretch();
    layout->addLayout(buttons);

    m_leaseTimer = new QTimer(this);
    m_leaseTimer->setInterval(kLeaseTickMs);
    connect(m_leaseTimer, &QTimer::timeout, this, &MonitorChannelsDialog::onLeaseTick);
    connect(m_table, &QTableWidget::itemChanged, this, &MonitorChannelsDialog::onOverrideToggled);

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
    rebuildRows(/*keepOverrides=*/false);
}

// The rows, from the document's mapping: the chosen channels first in their
// chosen order, then the rest in name order, which applyChannelSelection()
// then hides. Building the rows in display order is what makes the order plain
// QTableWidget behaviour. An earlier version kept name order and moved the
// vertical header's sections instead; with hundreds of hidden sections and a
// cell widget on every row the view painted the moved rows blank on a real
// unit, while a small offscreen grid painted them fine — a mechanism that
// cannot disagree with itself was the better answer than a diagnosis.
//
// keepOverrides: the pinned channels are carried across by SIGNAL INDEX, the
// one identity that survives the rows being rebuilt while the mapping stands
// still, and the device is told nothing — it holds the pins and the lease
// keeps them. A document change passes false and releases them, as it always
// has: the mapping behind those indices may have changed.
void MonitorChannelsDialog::rebuildRows(bool keepOverrides)
{
    QHash<int, double> pinned; // signal idx -> editor value
    if (keepOverrides) {
        for (const int row : std::as_const(m_overriddenRows)) {
            const int idx = m_rowToSignal.value(row, -1);
            if (idx >= 0)
                pinned.insert(idx, editorValue(row));
        }
        m_overriddenRows.clear();
    } else {
        clearAllOverrides(true); // the rows they belonged to are about to go
    }
    m_updatingChecks = true;
    m_signalToRow.clear();
    m_rowToSignal.clear();
    m_lastUpdateMs.clear();
    m_signalUnits.clear();
    m_signalDecimals.clear();
    m_signalEnumLabels.clear();
    m_channels.clear();
    m_table->setRowCount(0);

    if (!m_config) {
        m_infoLabel->setText(tr("No configuration."));
        m_updatingChecks = false;
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
    // The chosen channels to the front, in the order chosen; stable, so the
    // rest keep their name order behind them.
    QHash<QString, int> rank;
    for (int i = 0; i < m_selection.size(); ++i)
        if (!rank.contains(m_selection.at(i)))
            rank.insert(m_selection.at(i), i);
    std::stable_sort(entries.begin(), entries.end(),
                     [&rank](const QPair<QString, int> &a, const QPair<QString, int> &b) {
                         return rank.value(a.first, INT_MAX) < rank.value(b.first, INT_MAX);
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
        // For the picker: the catalogue's channel, or a bare name for one the
        // mapping knows and the catalogue no longer does.
        if (ch.isValid()) {
            m_channels.append(ch);
        } else {
            Channel bare;
            bare.name = name;
            m_channels.append(bare);
        }

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

        // The override cells. A device channel — written by the hardware every
        // tick — gets no tick box and says why; the firmware refuses it too.
        m_rowToSignal.insert(row, signalIdx);
        auto *overrideItem = new QTableWidgetItem;
        overrideItem->setTextAlignment(Qt::AlignCenter);
        const bool overridable = ch.isValid() && ch.deviceChannelId < 0;
        if (overridable) {
            overrideItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
            overrideItem->setCheckState(Qt::Unchecked);
            overrideItem->setToolTip(tr("Tick to pin this channel on the device at the value "
                                        "beside it; untick to release it."));
            m_table->setCellWidget(row, kColOverrideValue, makeOverrideEditor(ch, row));
        } else {
            overrideItem->setFlags(Qt::NoItemFlags);
            overrideItem->setToolTip(
                tr("A device channel is written by the hardware and cannot be overridden."));
        }
        m_table->setItem(row, kColOverride, overrideItem);
    }
    // Re-pin what was pinned (keepOverrides). The editor is set BEFORE the box
    // is ticked, so its valueChanged finds the box clear and sends nothing; the
    // tick itself lands under m_updatingChecks, so onOverrideToggled sends
    // nothing either. The device already holds these.
    for (auto it = pinned.constBegin(); it != pinned.constEnd(); ++it) {
        const int row = m_signalToRow.value(it.key(), -1);
        if (row < 0)
            continue;
        setEditorValue(row, it.value());
        if (QTableWidgetItem *tick = m_table->item(row, kColOverride))
            if (tick->flags() & Qt::ItemIsUserCheckable)
                tick->setCheckState(Qt::Checked);
        m_overriddenRows.insert(row);
    }
    m_updatingChecks = false;
    updateOverrideSummary();
    applyChannelSelection(); // the rows are new

    probeOverrideSupport();
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
    // The first word from the device is the moment to ask whether it does
    // overrides: the link is certainly open by then.
    if (!m_overrideProbeDone)
        probeOverrideSupport();

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

void MonitorChannelsDialog::onSelectChannels()
{
    MonitorChannelSelectDialog dialog(m_channels, m_selection, this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    m_selection = dialog.selection();
    QSettings().setValue(QLatin1String(kSelectionKey), m_selection);
    rebuildRows(/*keepOverrides=*/true); // the order is decided as the rows are built
}

// Hides the rows outside the selection; the order was settled when the rows
// were built (rebuildRows). "Chosen" is judged by name against the rows that
// exist, so a selection made of names the document does not map shows every
// channel rather than none — the names are kept in the selection regardless,
// because it is the user's and the channels may well be back after the next
// Get.
void MonitorChannelsDialog::applyChannelSelection()
{
    if (!m_table)
        return;
    const int rowCount = m_table->rowCount();
    QSet<QString> chosen;
    for (const QString &name : std::as_const(m_selection))
        chosen.insert(name);
    const auto nameAt = [this](int row) {
        const QTableWidgetItem *item = m_table->item(row, kColChannel);
        return item ? item->text() : QString();
    };
    bool anyChosenRow = false;
    for (int row = 0; row < rowCount && !anyChosenRow; ++row)
        anyChosenRow = chosen.contains(nameAt(row));
    const bool all = !anyChosenRow;

    int shown = 0;
    for (int row = 0; row < rowCount; ++row) {
        const bool visible = all || chosen.contains(nameAt(row)) || m_overriddenRows.contains(row);
        m_table->setRowHidden(row, !visible);
        if (visible)
            ++shown;
    }

    if (m_selectionLabel) {
        m_selectionLabel->setText(all ? tr("Showing all %1 channels").arg(rowCount)
                                      : tr("Showing %1 of %2 channels").arg(shown).arg(rowCount));
    }
    if (m_showAllButton)
        m_showAllButton->setEnabled(!m_selection.isEmpty());
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

// ---------------------------------------------------------------- overrides

void MonitorChannelsDialog::probeOverrideSupport()
{
    if (m_overrideProbeDone || !m_link || !m_link->isOpen())
        return;
    m_overrideProbeDone = true;
    bool supported = false;
    QString error;
    // The lease command is the probe: harmless with nothing pinned, and older
    // firmware answers it "unknown command". No answer at all counts as
    // absent for this opening of the dialog; the next opening asks again.
    if (!device_session::overrideLease(m_link, &supported, &error))
        supported = false;
    m_overridesSupported = supported;
    m_table->setColumnHidden(kColOverride, !supported);
    m_table->setColumnHidden(kColOverrideValue, !supported);
    m_clearOverridesButton->setVisible(supported);
    if (!supported)
        m_infoLabel->setText(m_infoLabel->text()
                             + (error.isEmpty()
                                    ? tr(" Overrides need device firmware 1.0.12.")
                                    : tr(" Overrides unavailable: %1").arg(error)));
}

QWidget *MonitorChannelsDialog::makeOverrideEditor(const Channel &ch, int row)
{
    // An edit while the row is pinned re-sends; while it is not, it just waits
    // for the tick.
    const auto resend = [this, row]() {
        QTableWidgetItem *tick = m_table->item(row, kColOverride);
        if (tick && tick->checkState() == Qt::Checked)
            sendOverride(row);
    };
    if (ch.dataType == QStringLiteral("boolean") || !ch.enumLabels.isEmpty()) {
        auto *combo = new QComboBox(m_table);
        if (ch.enumLabels.isEmpty()) {
            combo->addItem(tr("Off (0)"), 0.0);
            combo->addItem(tr("On (1)"), 1.0);
        } else {
            for (auto it = ch.enumLabels.constBegin(); it != ch.enumLabels.constEnd(); ++it)
                combo->addItem(QStringLiteral("%1 (%2)").arg(it.value()).arg(it.key()),
                               double(it.key()));
        }
        connect(combo, &QComboBox::currentIndexChanged, this, resend);
        return combo;
    }
    const OverrideSpan span = overrideSpanFor(ch);
    auto *spin = new QDoubleSpinBox(m_table);
    spin->setDecimals(span.decimals);
    spin->setRange(span.lo, span.hi);
    spin->setSingleStep(span.step);
    // A typed value, not a nudged one: the step still quantises what is sent
    // (editorValue), but the arrows are noise in a grid of short rows.
    spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
    spin->setKeyboardTracking(false); // one send per committed edit, not per keystroke
    if (!ch.unit.isEmpty())
        spin->setSuffix(QStringLiteral(" ") + ch.unit);
    spin->setValue(qBound(span.lo, 0.0, span.hi));
    connect(spin, &QDoubleSpinBox::valueChanged, this, resend);
    return spin;
}

void MonitorChannelsDialog::setEditorValue(int row, double value)
{
    QWidget *w = m_table->cellWidget(row, kColOverrideValue);
    if (auto *spin = qobject_cast<QDoubleSpinBox *>(w)) {
        spin->setValue(value);
    } else if (auto *combo = qobject_cast<QComboBox *>(w)) {
        const int i = combo->findData(value);
        if (i >= 0)
            combo->setCurrentIndex(i);
    }
}

double MonitorChannelsDialog::editorValue(int row) const
{
    QWidget *w = m_table->cellWidget(row, kColOverrideValue);
    if (auto *spin = qobject_cast<QDoubleSpinBox *>(w)) {
        // Quantised to the channel's resolution before it goes, so a channel
        // stepping by 0.25 never asks the device for 12.3.
        const double step = spin->singleStep();
        return step > 0.0 ? std::round(spin->value() / step) * step : spin->value();
    }
    if (auto *combo = qobject_cast<QComboBox *>(w))
        return combo->currentData().toDouble();
    return 0.0;
}

void MonitorChannelsDialog::onOverrideToggled(QTableWidgetItem *item)
{
    if (m_updatingChecks || !item || item->column() != kColOverride)
        return;
    const int row = item->row();
    if (item->checkState() == Qt::Checked) {
        sendOverride(row);
        return;
    }
    const int signalIdx = m_rowToSignal.value(row, -1);
    QString error;
    if (signalIdx >= 0 && m_link && m_link->isOpen())
        (void)device_session::setChannelOverride(m_link, quint16(signalIdx), false, 0.0f, &error);
    m_overriddenRows.remove(row);
    updateOverrideSummary();
}

void MonitorChannelsDialog::sendOverride(int row)
{
    const int signalIdx = m_rowToSignal.value(row, -1);
    if (signalIdx < 0 || !m_link)
        return;
    const double value = editorValue(row);
    QString error;
    const bool ok = m_link->isOpen()
                    && device_session::setChannelOverride(m_link, quint16(signalIdx), true, float(value), &error);
    if (!ok) {
        // The tick comes off again: a box that stays ticked would claim a pin
        // the device never took.
        m_updatingChecks = true;
        if (QTableWidgetItem *tick = m_table->item(row, kColOverride))
            tick->setCheckState(Qt::Unchecked);
        m_updatingChecks = false;
        m_overriddenRows.remove(row);
        m_infoLabel->setText(tr("Override refused: %1")
                                 .arg(error.isEmpty() ? tr("the device is not connected") : error));
        updateOverrideSummary();
        return;
    }
    m_overriddenRows.insert(row);
    updateOverrideSummary();
}

void MonitorChannelsDialog::onLeaseTick()
{
    if (m_overriddenRows.isEmpty()) {
        m_leaseTimer->stop();
        return;
    }
    bool supported = true;
    QString error;
    if (!m_link || !m_link->isOpen() || !device_session::overrideLease(m_link, &supported, &error) || !supported) {
        // No word reached the device, so it will drop the overrides on its own
        // within the lease. Show the same truth here rather than boxes that
        // stay ticked over values the device stopped honouring.
        clearAllOverrides(false);
        m_infoLabel->setText(tr("Overrides released: the device could not be reached."));
    }
}

void MonitorChannelsDialog::clearAllOverrides(bool tellDevice)
{
    if (tellDevice && !m_overriddenRows.isEmpty() && m_link && m_link->isOpen()) {
        QString error;
        (void)device_session::clearChannelOverrides(m_link, &error);
    }
    m_overriddenRows.clear();
    if (m_table) {
        m_updatingChecks = true;
        for (int row = 0; row < m_table->rowCount(); ++row)
            if (QTableWidgetItem *tick = m_table->item(row, kColOverride))
                if (tick->flags() & Qt::ItemIsUserCheckable)
                    tick->setCheckState(Qt::Unchecked);
        m_updatingChecks = false;
    }
    updateOverrideSummary();
}

void MonitorChannelsDialog::updateOverrideSummary()
{
    const int n = m_overriddenRows.size();
    setWindowTitle(n > 0 ? tr("Monitor Channels — %n overridden", nullptr, n)
                         : tr("Monitor Channels"));
    if (m_clearOverridesButton)
        m_clearOverridesButton->setEnabled(n > 0);
    if (m_leaseTimer) {
        if (n > 0 && !m_leaseTimer->isActive())
            m_leaseTimer->start();
        else if (n == 0 && m_leaseTimer->isActive())
            m_leaseTimer->stop();
    }
    applyChannelSelection(); // a pinned row is exempt, so the set may have changed
}

void MonitorChannelsDialog::closeEvent(QCloseEvent *event)
{
    clearAllOverrides(true);
    QDialog::closeEvent(event);
}

} // namespace ct
