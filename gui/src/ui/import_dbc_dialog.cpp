#include "import_dbc_dialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSet>
#include <QStyledItemDelegate>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>

#include "../model/channel_catalog.h"
#include "../model/configuration.h"
#include "../protocol/wire_structs.h"

namespace ct {

namespace {

enum {
    RoleMsgIdx = Qt::UserRole + 1,
    RoleSigIdx = Qt::UserRole + 2, // -1 on message rows
};

// Column 1 editor: a combo of Channel Type (quantity) values, but only on
// signal rows (message rows have no channel type).
class QuantityDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QWidget *createEditor(QWidget *parent, const QStyleOptionViewItem &,
                          const QModelIndex &index) const override
    {
        const QModelIndex c0 = index.model()->index(index.row(), 0, index.parent());
        if (c0.data(RoleSigIdx).toInt() < 0)
            return nullptr; // message row
        auto *combo = new QComboBox(parent);
        combo->addItems(ChannelCatalog::quantities());
        return combo;
    }
    void setEditorData(QWidget *editor, const QModelIndex &index) const override
    {
        if (auto *combo = qobject_cast<QComboBox *>(editor)) {
            const int i = combo->findText(index.data(Qt::EditRole).toString());
            combo->setCurrentIndex(i >= 0 ? i : 0);
        }
    }
    void setModelData(QWidget *editor, QAbstractItemModel *model,
                      const QModelIndex &index) const override
    {
        if (auto *combo = qobject_cast<QComboBox *>(editor))
            model->setData(index, combo->currentText(), Qt::EditRole);
    }
};

// Column 0 editor: the imported channel name, capped to what fits the device
// label. Only signal rows are editable, so this never sees a message row (whose
// name becomes a host-side section name and has no device-side limit).
class ChannelNameDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QWidget *createEditor(QWidget *parent, const QStyleOptionViewItem &option,
                          const QModelIndex &index) const override
    {
        QWidget *editor = QStyledItemDelegate::createEditor(parent, option, index);
        if (auto *line = qobject_cast<QLineEdit *>(editor))
            line->setMaxLength(MAX_CHANNEL_NAME_BYTES);
        return editor;
    }
};

// Blocks the inline editor on informational columns.
class ReadOnlyDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QWidget *createEditor(QWidget *, const QStyleOptionViewItem &,
                          const QModelIndex &) const override
    {
        return nullptr;
    }
};

QString signalDetails(const DbcSignal &sig)
{
    QString s = QObject::tr("start %1 · %2-bit %3 · %4 · ×%5 %6%7")
                    .arg(sig.startBit)
                    .arg(sig.bitLength)
                    .arg(sig.isSigned ? QObject::tr("signed") : QObject::tr("unsigned"))
                    .arg(sig.bigEndian ? QObject::tr("Motorola") : QObject::tr("Intel"))
                    .arg(sig.factor)
                    .arg(sig.offset >= 0 ? QStringLiteral("+") : QString())
                    .arg(sig.offset);
    if (sig.isMultiplexor)
        s += QObject::tr("  [multiplexor]");
    else if (sig.isMultiplexed)
        s += QObject::tr("  [mux=%1]").arg(sig.muxValue);
    return s;
}

QString messageDetails(const DbcMessage &msg)
{
    QString s = QStringLiteral("0x%1").arg(QString::number(msg.canId, 16).toUpper());
    if (msg.extended)
        s += QObject::tr(" ext");
    s += QObject::tr(" · %1 byte%2").arg(msg.dlc).arg(msg.dlc == 1 ? QString() : QStringLiteral("s"));
    if (msg.hasMultiplexing())
        s += QObject::tr(" · multiplexed");
    return s;
}

} // namespace

ImportDbcDialog::ImportDbcDialog(Configuration *config, const DbcFile &dbc,
                                 const QString &sourceName, int defaultBusIndex,
                                 const QStringList &parseWarnings, QWidget *parent)
    : QDialog(parent)
    , m_config(config)
    , m_dbc(dbc)
{
    setWindowTitle(tr("Import DBC — %1").arg(sourceName));
    resize(820, 620);

    auto *layout = new QVBoxLayout(this);

    // Top row: target bus + filter.
    auto *topRow = new QHBoxLayout;
    topRow->addWidget(new QLabel(tr("Import into :")));
    m_busCombo = new QComboBox;
    for (int i = 0; i < 3; ++i)
        m_busCombo->addItem(tr("CAN %1").arg(i + 1), i);
    m_busCombo->setCurrentIndex(qBound(0, defaultBusIndex, 2));
    topRow->addWidget(m_busCombo);
    topRow->addSpacing(16);
    topRow->addWidget(new QLabel(tr("Filter :")));
    m_filter = new QLineEdit;
    m_filter->setPlaceholderText(tr("Type to filter messages and signals"));
    connect(m_filter, &QLineEdit::textChanged, this, [this]() { applyFilter(); });
    topRow->addWidget(m_filter, 1);
    layout->addLayout(topRow);

    // Tree.
    m_tree = new QTreeWidget;
    m_tree->setColumnCount(4);
    m_tree->setHeaderLabels({tr("Message / Signal"), tr("Channel Type"), tr("Details"), tr("Unit")});
    m_tree->setItemDelegateForColumn(0, new ChannelNameDelegate(m_tree));
    m_tree->setItemDelegateForColumn(1, new QuantityDelegate(m_tree));
    m_tree->setItemDelegateForColumn(2, new ReadOnlyDelegate(m_tree));
    m_tree->setItemDelegateForColumn(3, new ReadOnlyDelegate(m_tree));
    m_tree->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    m_tree->setColumnWidth(0, 240);
    m_tree->setColumnWidth(1, 150);
    m_tree->setColumnWidth(2, 260);
    connect(m_tree, &QTreeWidget::itemChanged, this,
            [this](QTreeWidgetItem *item, int column) { onItemChanged(item, column); });
    layout->addWidget(m_tree, 1);

    // Selection buttons + count.
    auto *selRow = new QHBoxLayout;
    auto *selectAll = new QPushButton(tr("Select All"));
    connect(selectAll, &QPushButton::clicked, this, [this]() { setAllChecked(true); });
    auto *selectNone = new QPushButton(tr("Select None"));
    connect(selectNone, &QPushButton::clicked, this, [this]() { setAllChecked(false); });
    selRow->addWidget(selectAll);
    selRow->addWidget(selectNone);
    selRow->addStretch();
    m_countLabel = new QLabel;
    selRow->addWidget(m_countLabel);
    layout->addLayout(selRow);

    // Parse warnings (if any).
    if (!parseWarnings.isEmpty()) {
        layout->addWidget(new QLabel(tr("Parser notes :")));
        m_warnings = new QPlainTextEdit;
        m_warnings->setReadOnly(true);
        m_warnings->setMaximumHeight(90);
        m_warnings->setPlainText(parseWarnings.join(QLatin1Char('\n')));
        layout->addWidget(m_warnings);
    }

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    m_buttons->button(QDialogButtonBox::Ok)->setText(tr("Import"));
    connect(m_buttons, &QDialogButtonBox::accepted, this, &ImportDbcDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(m_buttons);

    buildTree();
    updateOkState();
}

void ImportDbcDialog::buildTree()
{
    m_updating = true;
    m_tree->clear();
    for (int mi = 0; mi < m_dbc.messages.size(); ++mi) {
        const DbcMessage &msg = m_dbc.messages[mi];
        auto *mItem = new QTreeWidgetItem(m_tree);
        mItem->setText(0, msg.name);
        mItem->setText(2, messageDetails(msg));
        mItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
        mItem->setCheckState(0, Qt::Unchecked);
        mItem->setData(0, RoleMsgIdx, mi);
        mItem->setData(0, RoleSigIdx, -1);
        for (int si = 0; si < msg.signalList.size(); ++si) {
            const DbcSignal &sig = msg.signalList[si];
            auto *sItem = new QTreeWidgetItem(mItem);
            // Column 0 is the name the channel will be CREATED with, and it is
            // editable — accept() reads it straight back out (see makeRow) and
            // hands it to uniqueName(). So it is the identity itself, not a
            // display of one, and it stays bare: appending a unit here would
            // import a channel literally called "EngineSpeed rpm". The unit has
            // its own column (3) beside it, which is where it belongs anyway —
            // these signals are not in the catalogue yet, so their unit comes
            // from the DBC rather than from labelFor().
            sItem->setText(0, sig.name);
            sItem->setText(1, quantityForUnit(sig.unit));
            sItem->setText(2, signalDetails(sig));
            sItem->setText(3, sig.unit);
            sItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable
                            | Qt::ItemIsEditable);
            sItem->setCheckState(0, Qt::Unchecked);
            sItem->setData(0, RoleMsgIdx, mi);
            sItem->setData(0, RoleSigIdx, si);
        }
    }
    m_tree->expandAll();
    m_updating = false;
}

void ImportDbcDialog::applyFilter()
{
    const QString text = m_filter->text().trimmed();
    for (int mi = 0; mi < m_tree->topLevelItemCount(); ++mi) {
        QTreeWidgetItem *mItem = m_tree->topLevelItem(mi);
        const bool msgMatch = text.isEmpty() || mItem->text(0).contains(text, Qt::CaseInsensitive);
        bool anyChild = false;
        for (int si = 0; si < mItem->childCount(); ++si) {
            QTreeWidgetItem *sItem = mItem->child(si);
            const bool sMatch = msgMatch || text.isEmpty()
                                || sItem->text(0).contains(text, Qt::CaseInsensitive);
            sItem->setHidden(!sMatch);
            anyChild = anyChild || sMatch;
        }
        mItem->setHidden(!(msgMatch || anyChild));
    }
}

void ImportDbcDialog::onItemChanged(QTreeWidgetItem *item, int column)
{
    if (m_updating || column != 0)
        return;
    m_updating = true;
    if (item->data(0, RoleSigIdx).toInt() < 0) {
        // Message row toggled: cascade to its signals.
        const Qt::CheckState st = item->checkState(0);
        if (st != Qt::PartiallyChecked)
            for (int si = 0; si < item->childCount(); ++si)
                item->child(si)->setCheckState(0, st);
    } else if (QTreeWidgetItem *p = item->parent()) {
        // Signal row toggled: recompute the parent's tristate.
        int checked = 0;
        for (int si = 0; si < p->childCount(); ++si)
            if (p->child(si)->checkState(0) == Qt::Checked)
                ++checked;
        p->setCheckState(0, checked == 0 ? Qt::Unchecked
                                         : checked == p->childCount() ? Qt::Checked
                                                                      : Qt::PartiallyChecked);
    }
    m_updating = false;
    updateOkState();
}

void ImportDbcDialog::setAllChecked(bool checked)
{
    m_updating = true;
    const Qt::CheckState st = checked ? Qt::Checked : Qt::Unchecked;
    for (int mi = 0; mi < m_tree->topLevelItemCount(); ++mi) {
        QTreeWidgetItem *mItem = m_tree->topLevelItem(mi);
        mItem->setCheckState(0, st);
        for (int si = 0; si < mItem->childCount(); ++si)
            mItem->child(si)->setCheckState(0, st);
    }
    m_updating = false;
    updateOkState();
}

void ImportDbcDialog::updateOkState()
{
    int channels = 0, messages = 0;
    for (int mi = 0; mi < m_tree->topLevelItemCount(); ++mi) {
        QTreeWidgetItem *mItem = m_tree->topLevelItem(mi);
        int here = 0;
        for (int si = 0; si < mItem->childCount(); ++si)
            if (mItem->child(si)->checkState(0) == Qt::Checked)
                ++here;
        channels += here;
        if (here > 0)
            ++messages;
    }
    m_countLabel->setText(tr("%1 channel(s) in %2 message(s) selected").arg(channels).arg(messages));
    m_buttons->button(QDialogButtonBox::Ok)->setEnabled(channels > 0);
}

int ImportDbcDialog::targetBusIndex() const
{
    return m_busCombo->currentData().toInt();
}

void ImportDbcDialog::accept()
{
    QStringList warns;

    // Names must be unique against the existing catalogue and each other so an
    // import never silently clobbers an existing channel.
    QSet<QString> usedNames;
    for (const Channel &c : m_config->catalog().userChannels())
        usedNames.insert(c.name.toLower());
    // DBC signal names routinely run past what the device label holds, so clip
    // to the byte budget here rather than letting the mapper truncate silently.
    // Never cut a multi-byte UTF-8 codepoint in half.
    const auto clip = [](const QString &s, int budget) {
        QByteArray utf8 = s.toUtf8();
        if (utf8.size() <= budget)
            return s;
        // Walk back ONLY when the cut splits a codepoint (first dropped byte is
        // a continuation byte), and then drop that codepoint's lead byte too.
        // Chopping unconditionally would eat a character that fits; leaving the
        // lead byte would make fromUtf8() below substitute U+FFFD, which is
        // THREE bytes and would push the "clipped" name back over the budget.
        const bool splitsCodepoint = (quint8(utf8[budget]) & 0xC0) == 0x80;
        utf8.truncate(budget);
        if (splitsCodepoint) {
            while (!utf8.isEmpty() && (quint8(utf8.back()) & 0xC0) == 0x80)
                utf8.chop(1);
            if (!utf8.isEmpty())
                utf8.chop(1); // the lead byte of the split codepoint
        }
        return QString::fromUtf8(utf8).trimmed();
    };
    const auto uniqueName = [&](const QString &base) {
        const QString root = base.isEmpty() ? tr("Signal") : base;
        const QString clipped = clip(root, MAX_CHANNEL_NAME_BYTES);
        QString candidate = clipped;
        // A " 2" disambiguator has to fit the budget too, so it eats into the
        // root rather than pushing the name back over the limit.
        for (int n = 2; usedNames.contains(candidate.toLower()); ++n) {
            const QString suffix = QStringLiteral(" %1").arg(n);
            candidate = clip(root, MAX_CHANNEL_NAME_BYTES - int(suffix.toUtf8().size())) + suffix;
        }
        usedNames.insert(candidate.toLower());
        if (candidate != base && !base.isEmpty())
            warns.append(candidate == clipped
                             ? tr("Channel '%1' shortened to '%2' (names are limited to %3 bytes "
                                  "on the device)")
                                   .arg(base, candidate).arg(MAX_CHANNEL_NAME_BYTES)
                             : tr("Channel '%1' renamed to '%2' (name already in use)")
                                   .arg(base, candidate));
        return candidate;
    };

    QList<CommsSection> sections;
    QList<Channel> newChannels;

    for (int mi = 0; mi < m_tree->topLevelItemCount(); ++mi) {
        QTreeWidgetItem *mItem = m_tree->topLevelItem(mi);
        QList<QTreeWidgetItem *> checkedSigs;
        for (int si = 0; si < mItem->childCount(); ++si)
            if (mItem->child(si)->checkState(0) == Qt::Checked)
                checkedSigs.append(mItem->child(si));
        if (checkedSigs.isEmpty())
            continue;

        const DbcMessage &msg = m_dbc.messages[mItem->data(0, RoleMsgIdx).toInt()];

        CommsSection section;
        section.device = SectionDevice::ReceiveMessage;
        section.baseAddress = msg.canId;
        section.extended = msg.extended;
        section.fd = msg.dlc > 8;
        section.messageLengthBytes = msg.dlc;
        section.name = msg.name.isEmpty()
                           ? QStringLiteral("Receive 0x%1").arg(QString::number(msg.canId, 16).toUpper())
                           : msg.name;
        // One section carries one byte order; take it from the first selected
        // signal and skip any signal that disagrees.
        const DbcSignal &firstSig = msg.signalList[checkedSigs.first()->data(0, RoleSigIdx).toInt()];
        section.alignment = alignmentForDbcSignal(firstSig);

        // Turn one checked signal item into a row (+ a staged catalogue channel).
        const auto makeRow = [&](QTreeWidgetItem *sItem, CommsChannelRow *rowOut) -> bool {
            const DbcSignal &sig = msg.signalList[sItem->data(0, RoleSigIdx).toInt()];
            if (alignmentForDbcSignal(sig) != section.alignment) {
                warns.append(tr("%1 · %2: byte order differs from the rest of the message — "
                                "skipped").arg(msg.name, sig.name));
                return false;
            }
            const QString importName = sItem->text(0).trimmed();
            const QString unique = uniqueName(importName);
            Channel ch = channelFromDbcSignal(sig, unique);
            ch.quantity = sItem->text(1); // the (possibly edited) Channel Type
            newChannels.append(ch);
            *rowOut = rowFromDbcSignal(sig, unique);
            return true;
        };

        if (msg.hasMultiplexing()) {
            const DbcSignal *muxSig = msg.multiplexor();
            // Non-muxed signals (and the multiplexor) apply to every instance.
            // A compound section has no shared "always-present" set, so they are
            // replicated into each identifier. makeRow is called once per signal
            // (one channel), and the row copied into each identifier.
            QList<CommsChannelRow> commonRows;
            QHash<int, QList<QTreeWidgetItem *>> byValue;
            for (QTreeWidgetItem *sItem : checkedSigs) {
                const DbcSignal &sig = msg.signalList[sItem->data(0, RoleSigIdx).toInt()];
                if (sig.isMultiplexed)
                    byValue[sig.muxValue].append(sItem);
                else {
                    CommsChannelRow row;
                    if (makeRow(sItem, &row))
                        commonRows.append(row);
                }
            }
            if (byValue.isEmpty()) {
                // No multiplexed signals selected — a plain (non-compound) message.
                section.rows = commonRows;
            } else if (!muxSig) {
                warns.append(tr("%1: has multiplexed signals but no multiplexor — imported "
                                "the non-multiplexed channels as a plain message").arg(msg.name));
                section.rows = commonRows;
            } else {
                // Emit identifiers in ascending multiplexor-value order so the
                // result is deterministic (QHash iteration order is randomized).
                QList<int> muxValues = byValue.keys();
                std::sort(muxValues.begin(), muxValues.end());
                for (int value : muxValues) {
                    int offset = 0;
                    quint32 id = 0, mask = 0;
                    QString reason;
                    if (!muxSelectorForValue(*muxSig, value, &offset, &id, &mask, &reason,
                                             msg.dlc)) {
                        warns.append(tr("%1: multiplexor value %2 — %3; its channels were skipped")
                                         .arg(msg.name).arg(value).arg(reason));
                        continue;
                    }
                    CompoundIdentifier ident;
                    ident.byteOffset = offset;
                    ident.id = id;
                    ident.idMask = mask;
                    ident.configured = true;
                    ident.rows = commonRows; // replicate the common channels here
                    for (QTreeWidgetItem *sItem : byValue.value(value)) {
                        CommsChannelRow row;
                        if (makeRow(sItem, &row))
                            ident.rows.append(row);
                    }
                    if (!ident.rows.isEmpty())
                        section.identifiers.append(ident);
                }
                section.compound = !section.identifiers.isEmpty();
                if (!section.compound)
                    section.rows = commonRows; // every identifier failed -> plain message
            }
        } else {
            for (QTreeWidgetItem *sItem : checkedSigs) {
                CommsChannelRow row;
                if (makeRow(sItem, &row))
                    section.rows.append(row);
            }
        }

        if (!section.rows.isEmpty() || !section.identifiers.isEmpty())
            sections.append(section);
    }

    if (sections.isEmpty()) {
        QMessageBox::warning(this, windowTitle(),
                             tr("Nothing was imported (every selected signal was skipped)."));
        return;
    }

    for (const Channel &c : newChannels)
        m_config->catalog().addOrUpdateUserChannel(c);
    if (!newChannels.isEmpty())
        m_config->setDirty();
    m_sections = sections;

    if (!warns.isEmpty()) {
        // One note per rename/skip — a big DBC produces dozens, so the list
        // goes in the scrollable details pane instead of growing the box.
        QMessageBox box(QMessageBox::Information, windowTitle(),
                        tr("Imported %1 message(s) with %2 note(s).\n\n"
                           "See Show Details for the full list.")
                            .arg(sections.size())
                            .arg(warns.size()),
                        QMessageBox::Ok, this);
        box.setDetailedText(warns.join(QLatin1Char('\n')));
        box.exec();
    }

    QDialog::accept();
}

} // namespace ct
