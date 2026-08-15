// Tools > Channel Editor — every channel in the document in one table.
#include "channel_editor_dialog.h"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QLineEdit>
#include <QPushButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include <QBrush>
#include <QSet> // the hoisted edit-locked channel set in rebuild()

#include "../model/configuration.h"
#include "../model/dbc_import.h" // storageTypeHoldsRange / storageTypeForRange
#include "edit_channel_dialog.h"
#include "trimmed_spin_box.h"    // trimmedNumber

namespace ct {

namespace {

enum Column {
    ColName = 0,
    ColType,
    ColDecimals,
    ColResolution,
    ColMin,
    ColMax,
    ColUnit,
    ColDefault,
    ColSource,
    ColCount
};

// The receive-timeout default that actually applies to a channel, if any. A
// signal only reverts to its default when its message declares a timeout AND
// has the option enabled — the same condition the mapper uses when it turns
// the section's timeout into the message's period_ms — so anything else is
// reported as "not applicable" rather than as a value that never fires.
bool timeoutDefaultFor(const Configuration &config, const QString &channelName, double *valueOut,
                       QString *whereOut)
{
    for (const BusConfig &bus : config.bus) {
        for (const CommsSection &s : bus.sections) {
            if (!s.isReceive() || !s.defaultValueOnTimeout || s.receiveTimeoutMs <= 0)
                continue;
            for (const CommsChannelRow &row : s.allRows()) {
                if (row.channelName.compare(channelName, Qt::CaseInsensitive) != 0)
                    continue;
                if (valueOut)
                    *valueOut = row.defaultValue;
                if (whereOut)
                    *whereOut = s.name;
                return true;
            }
        }
    }
    return false;
}

// Where a channel comes from, for the Source column: the comms section that
// receives or transmits it, or the calculation that generates it.
QString sourceFor(const Configuration &config, const QString &name)
{
    const auto sameName = [&name](const QString &other) {
        return other.compare(name, Qt::CaseInsensitive) == 0;
    };
    for (int b = 0; b < 3; ++b) {
        for (const CommsSection &s : config.bus[b].sections) {
            if (s.device == SectionDevice::Off)
                continue;
            for (const QString &n : s.channelNames())
                if (sameName(n))
                    return QStringLiteral("CAN%1 · %2").arg(b + 1).arg(s.name);
            // The CRC8 publish channel is written by the section without being
            // one of its rows, so it needs naming here or its Source reads
            // blank in the Channel Editor.
            if (s.isCrc8() && sameName(s.crcChannel))
                return QStringLiteral("CAN%1 · %2 CRC8").arg(b + 1).arg(s.name);
        }
    }
    for (const MathRow &m : config.mathRows)
        if (sameName(m.destChannel))
            return QObject::tr("Math");
    for (const ConditionRow &c : config.conditionRows)
        if (sameName(c.outputChannel))
            return QObject::tr("Condition");
    for (const CounterRow &c : config.counterRows)
        if (sameName(c.outputChannel))
            return QObject::tr("Counter");
    for (const TimerRow &t : config.timerRows)
        if (sameName(t.outputChannel))
            return QObject::tr("Timer");
    for (const IntegratorRow &g : config.integratorRows)
        if (sameName(g.outputChannel))
            return QObject::tr("Integrator");
    for (const ConstantRow &k : config.constantRows)
        if (sameName(k.name))
            return QObject::tr("Constant");
    for (const Table2x16Row &t : config.table2x16Rows)
        if (sameName(t.outputChannel))
            return QObject::tr("Table 2x16");
    for (const Table8x8Row &t : config.table8x8Rows)
        if (sameName(t.outputChannel))
            return QObject::tr("Table 8x8");
    return QObject::tr("unused");
}

} // namespace

ChannelEditorDialog::ChannelEditorDialog(Configuration *config, QWidget *parent)
    : QDialog(parent), m_config(config)
{
    setWindowTitle(tr("Channel Editor"));
    setModal(true);
    resize(1000, 620);

    auto *layout = new QVBoxLayout(this);

    auto *searchRow = new QHBoxLayout;
    searchRow->addWidget(new QLabel(tr("Search :"), this));
    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setPlaceholderText(
        tr("Any part of the name, or a regular expression (^Cruise, Speed$, set|limit)"));
    connect(m_searchEdit, &QLineEdit::textChanged, this, &ChannelEditorDialog::rebuild);
    searchRow->addWidget(m_searchEdit, 1);
    layout->addLayout(searchRow);

    m_tree = new QTreeWidget(this);
    m_tree->setColumnCount(ColCount);
    m_tree->setHeaderLabels({tr("Channel"), tr("Data Type"), tr("Dec"), tr("Resolution"),
                             tr("Minimum"), tr("Maximum"), tr("Unit"), tr("Default on Timeout"),
                             tr("Source")});
    m_tree->setRootIsDecorated(false);
    m_tree->setAllColumnsShowFocus(true);
    m_tree->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->setSortingEnabled(true);
    m_tree->sortByColumn(ColName, Qt::AscendingOrder);
    m_tree->header()->setStretchLastSection(true);
    connect(m_tree, &QTreeWidget::itemSelectionChanged, this,
            &ChannelEditorDialog::updateButtons);
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem *, int) { onEdit(); });
    layout->addWidget(m_tree, 1);

    m_summaryLabel = new QLabel(this);
    layout->addWidget(m_summaryLabel);

    auto *buttonRow = new QHBoxLayout;
    m_newButton = new QPushButton(tr("New…"), this);
    connect(m_newButton, &QPushButton::clicked, this, &ChannelEditorDialog::onNew);
    buttonRow->addWidget(m_newButton);
    m_editButton = new QPushButton(tr("Edit…"), this);
    connect(m_editButton, &QPushButton::clicked, this, &ChannelEditorDialog::onEdit);
    buttonRow->addWidget(m_editButton);
    buttonRow->addStretch(1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, Qt::Horizontal, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::accept);
    buttonRow->addWidget(buttons);
    layout->addLayout(buttonRow);

    rebuild();
    m_searchEdit->setFocus();
}

void ChannelEditorDialog::rebuild()
{
    const QString previous = currentChannelName();
    // Sorting has to be off while filling or every insert re-sorts the view.
    m_tree->setSortingEnabled(false);
    m_tree->clear();

    const QList<Channel> all = m_config->catalog().allChannels();
    const QList<Channel> shown = m_config->catalog().search(m_searchEdit->text());
    // Greyed like any other non-editable text rather than in a colour of its
    // own: a protected channel is not a problem to be fixed, it is simply not
    // this session's to change, and the warning colour below has to stay
    // meaning "this one is broken".
    const QBrush lockedText(palette().color(QPalette::Disabled, QPalette::Text));
    // ONE bus walk for the whole table, not one per row. isChannelEditLocked()
    // has no document-wide early-out any more and cannot get one (see its
    // definition — no password lifts an edit lock, so there is nothing true to
    // short-circuit on), which made the old per-row call O(channels x locked
    // sections) on exactly the configurations Read Only exists for: many marked
    // messages, many channels, rebuilt on every keystroke in the search box.
    //
    // Built here as a local rather than cached on Configuration on purpose: bus
    // sections are public members that a dozen writers assign directly, so a
    // cached set has no reliable invalidation point and would go stale silently
    // — leaving a locked channel editable, which is the whole bug the split
    // predicates exist to prevent. A local rebuilt with the table it feeds
    // cannot be stale by construction.
    QSet<QString> editLocked;
    for (const QString &n : m_config->editLockedChannelNames())
        editLocked.insert(n.toCaseFolded());
    int miscount = 0;  // channels whose type cannot hold their own range
    int lockcount = 0; // channels owned by a protected message
    for (const Channel &c : shown) {
        auto *item = new QTreeWidgetItem(m_tree);
        // The name stays BARE here, unlike the lists in Select Channel and the
        // Communications channel pane, and deliberately so. This table already
        // has a Unit column of its own (ColUnit, three lines down, from the
        // same c.unit), so channelLabel() would print the unit twice on one
        // row. And this is the one dialog where a channel's name is the thing
        // being DEFINED rather than referred to: a column headed "Channel"
        // reading "Coolant Temp °C" is exactly the misreading that gets typed
        // back into the Channel Name box in Edit Custom Channel.
        //
        // Identity travels in Qt::UserRole either way — currentChannelName()
        // reads that and never the text — which is what lets the 🔒 marker
        // below be appended to the visible name without corrupting anything.
        item->setText(ColName, c.name);
        item->setData(ColName, Qt::UserRole, c.name);
        item->setText(ColType, c.dataType.isEmpty() ? tr("(not set)") : c.dataType);
        item->setText(ColDecimals, QString::number(c.decimalPlaces));
        item->setText(ColResolution, trimmedNumber(c.baseResolution, 8));
        item->setText(ColMin, trimmedNumber(c.minValue, c.decimalPlaces));
        item->setText(ColMax, trimmedNumber(c.maxValue, c.decimalPlaces));
        item->setText(ColUnit, c.unit);

        double dflt = 0.0;
        QString where;
        if (timeoutDefaultFor(*m_config, c.name, &dflt, &where)) {
            item->setText(ColDefault, trimmedNumber(dflt, c.decimalPlaces));
            item->setToolTip(ColDefault, tr("Applied when \"%1\" times out").arg(where));
        }
        item->setText(ColSource, sourceFor(*m_config, c.name));

        // An integer channel holds a scaled integer, so its reach is
        // rawRange x 10^-decimals. A type too small for the channel's own range
        // is silently lossy — the device clamps to that range — so flag it
        // rather than leaving the user to discover it as a stuck reading.
        const bool clamped =
            !storageTypeHoldsRange(c.dataType, c.minValue, c.maxValue, c.decimalPlaces);
        if (clamped) {
            ++miscount;
            const QString suggestion =
                storageTypeForRange(c.minValue, c.maxValue, c.decimalPlaces);
            const QString why =
                tr("\"%1\" at %2 dp cannot represent %3 … %4 — readings are clamped. "
                   "Use %5 instead.")
                    .arg(c.dataType)
                    .arg(c.decimalPlaces)
                    .arg(trimmedNumber(c.minValue, c.decimalPlaces),
                         trimmedNumber(c.maxValue, c.decimalPlaces), suggestion);
            const QBrush warn(QColor(0xC0, 0x30, 0x00));
            for (int col = 0; col < ColCount; ++col) {
                item->setForeground(col, warn);
                item->setToolTip(col, why);
            }
            item->setText(ColType, tr("%1  ⚠ → %2").arg(c.dataType, suggestion));
        }

        // A channel carried by a protected message is not this session's to
        // redefine, at ANY of the three tiers — Read Only included, and that is
        // the point of asking isChannelEditLocked here rather than
        // isChannelConcealed. A Read Only message is fully visible and still
        // must not be edited, so its channels are exactly the ones the old
        // single predicate would now have left open. Their data type, base
        // resolution and decimal places are the message's decode, so changing
        // them here would leave it reading different numbers with nothing on
        // screen to explain it.
        //
        // The row is marked and greyed rather than removed or blanked: the
        // channel's meaning is public (that is the whole point of protecting the
        // protocol instead of the outputs), and a padlock the user can see is
        // what makes "Edit… did not let me change anything" understandable
        // before they click it rather than after.
        // Case-folded membership in the hoisted set, which is the same question
        // isChannelEditLocked() answers (it compares CaseInsensitive) with the
        // bus walk lifted out of the loop.
        if (editLocked.contains(c.name.toCaseFolded())) {
            ++lockcount;
            item->setText(ColName, tr("%1  🔒").arg(c.name));
            // Nothing in this tree is edited in place today, so this is about
            // the row's future rather than its present: an editable column
            // added later must not silently become a way around the lock.
            item->setFlags(item->flags() & ~Qt::ItemIsEditable);
            const QString locked =
                tr("\"%1\" belongs to a protected message, so its data type, resolution, "
                   "decimal places, range and units are read-only. View… opens it for "
                   "reading. To change it, untick that message's protection in "
                   "Connections > Communications — the password alone does not unlock "
                   "editing, it unlocks the untick.")
                    .arg(c.name);
            // Appended rather than assigned: when the channel is ALSO clamped,
            // both facts are true and the clamp is the one that needs acting on.
            const QString existing = item->toolTip(ColName);
            item->setToolTip(ColName, existing.isEmpty()
                                          ? locked
                                          : existing + QStringLiteral("\n\n") + locked);
            if (!clamped)
                for (int col = 0; col < ColCount; ++col)
                    item->setForeground(col, lockedText);
        }

        // Right-align the numeric columns so magnitudes line up.
        for (int col : {ColDecimals, ColResolution, ColMin, ColMax, ColDefault})
            item->setTextAlignment(col, Qt::AlignRight | Qt::AlignVCenter);

        if (!previous.isEmpty() && c.name.compare(previous, Qt::CaseInsensitive) == 0)
            m_tree->setCurrentItem(item);
    }

    m_tree->setSortingEnabled(true);
    for (int col = 0; col < ColCount - 1; ++col)
        m_tree->resizeColumnToContents(col);

    QString summary = shown.size() == all.size()
                          ? tr("%1 channel(s).").arg(all.size())
                          : tr("%1 of %2 channel(s) shown.").arg(shown.size()).arg(all.size());
    if (lockcount > 0) {
        summary += QStringLiteral("  ");
        summary += tr("🔒 %1 channel(s) belong to a protected message and are read-only.")
                       .arg(lockcount);
    }
    if (miscount > 0) {
        summary += QStringLiteral("  ");
        summary += tr("⚠ %1 channel(s) have a data type too small for their range — "
                      "their readings are clamped on the device.").arg(miscount);
        m_summaryLabel->setStyleSheet(QStringLiteral("color: #C03000;"));
    } else {
        m_summaryLabel->setStyleSheet(QString());
    }
    m_summaryLabel->setText(summary);
    m_summaryLabel->setWordWrap(true);
    updateButtons();
}

QString ChannelEditorDialog::currentChannelName() const
{
    QTreeWidgetItem *item = m_tree->currentItem();
    return (item && item->isSelected()) ? item->data(ColName, Qt::UserRole).toString() : QString();
}

void ChannelEditorDialog::onEdit()
{
    const QString name = currentChannelName();
    if (name.isEmpty())
        return;
    const Channel channel = m_config->catalog().findByName(name);
    if (!channel.isValid())
        return;
    if (ChannelCatalog::isDeviceChannel(name)) {
        // Nothing to open: the definition lives in the firmware. Say so once
        // rather than showing a form whose every field is dead.
        QMessageBox::information(
            this, tr("Device Channel"),
            tr("\"%1\" is provided by the device itself.\n\nIts type, resolution and range are "
               "fixed by the firmware, so there is nothing to edit here. Use it like any other "
               "channel — in maths, conditions, or a transmit message.")
                .arg(name));
        return;
    }
    // Protected channels are not refused here. EditChannelDialog opens them
    // read-only — every value visible, nothing writable, and an explanation of
    // why at the top — which tells the user far more than a dead button would,
    // and it returns an empty name so there is nothing to rebuild.
    if (!EditChannelDialog::createOrEdit(m_config, channel, false, this).isEmpty())
        rebuild();
}

void ChannelEditorDialog::onNew()
{
    if (!EditChannelDialog::createOrEdit(m_config, Channel{}, true, this).isEmpty())
        rebuild();
}

void ChannelEditorDialog::updateButtons()
{
    const QString name = currentChannelName();
    m_editButton->setEnabled(!name.isEmpty());
    // Say which dialog is about to open. A protected channel's Edit… shows
    // everything and changes nothing, and a button that still reads "Edit…"
    // makes that look like a failure instead of the rule.
    // A device channel is defined by the firmware, not by this document, so
    // there is nothing here to edit — the same read-only treatment a protected
    // channel gets, for a different reason, and said in the tooltip so the
    // "View…" label is not a mystery.
    const bool deviceChannel = !name.isEmpty() && ChannelCatalog::isDeviceChannel(name);
    const bool locked =
        !name.isEmpty() && (deviceChannel || m_config->isChannelEditLocked(name));
    m_editButton->setText(locked ? tr("View…") : tr("Edit…"));
    m_editButton->setToolTip(
        deviceChannel
            ? tr("\"%1\" is provided by the device itself. Its definition is fixed by the "
                 "firmware and cannot be changed.")
                  .arg(name)
        : locked ? tr("\"%1\" belongs to a protected message — its definition can be seen "
                      "but not changed until that message's protection is unticked.")
                       .arg(name)
                 : QString());
}

void ChannelEditorDialog::run(Configuration *config, QWidget *parent)
{
    ChannelEditorDialog dialog(config, parent);
    dialog.exec();
}

} // namespace ct
