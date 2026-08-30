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
#include "name_limits.h"

namespace ct {

namespace {

enum {
    RoleMsgIdx = Qt::UserRole + 1,
    RoleSigIdx = Qt::UserRole + 2, // -1 on message rows
    // The unit string the FILE said, kept beside the one being imported. The
    // two differ whenever the DBC spelled a unit the catalogue does not offer,
    // and the original is what the warning has to quote - "degC" means nothing
    // to the user if the dialog has already replaced it with "C".
    RoleRawUnit = Qt::UserRole + 3,
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

// A signal whose DBC unit this import could not place. Marked rather than
// guessed at, and marked on the UNIT cell because that is the one to fix.
void markUnknownUnit(QTreeWidgetItem *item, const QString &rawUnit)
{
    const QColor warn = item->treeWidget()
                            && item->treeWidget()->palette().color(QPalette::Base).lightness() < 128
                        ? QColor(0xFF, 0xA1, 0x78)
                        : QColor(0xC0, 0x30, 0x00);
    item->setForeground(3, warn);
    item->setText(3, QObject::tr("(pick one)"));
    const QString tip =
        QObject::tr("The file says the unit is \"%1\", which is not one this application "
                    "offers. Set the Channel Type, then pick a unit here - or leave it "
                    "unitless. Nothing converts the NUMBERS, so choose the unit the raw "
                    "values are already in.")
            .arg(rawUnit);
    item->setToolTip(3, tip);
    item->setToolTip(1, tip);
}

// Column 3 editor: the UNIT, as a combo of what the catalogue offers for the
// Channel Type in column 1 - so an imported channel ends up with a unit the app
// actually has, and is indistinguishable from a hand-made one.
//
// It was a ReadOnlyDelegate, which is what made an unmapped unit a dead end: a
// DBC saying "Nm/deg" imported a channel whose unit no list contained, the
// column showed it, and there was no way to correct it without leaving the
// import and editing the channel afterwards.
//
// The list is rebuilt per edit rather than cached because column 1 is editable
// too: change the Channel Type and the units that make sense change with it.
class UnitDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QWidget *createEditor(QWidget *parent, const QStyleOptionViewItem &,
                          const QModelIndex &index) const override
    {
        const QModelIndex c0 = index.model()->index(index.row(), 0, index.parent());
        if (c0.data(RoleSigIdx).toInt() < 0)
            return nullptr; // message row
        const QModelIndex c1 = index.model()->index(index.row(), 1, index.parent());
        auto *combo = new QComboBox(parent);
        combo->addItems(ChannelCatalog::unitsForQuantity(c1.data(Qt::EditRole).toString()));
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
        // BYTES, not characters - setMaxLength counts QChars and the label
        // budget is UTF-8 bytes. See name_limits.h.
        if (auto *line = qobject_cast<QLineEdit *>(editor))
            ct::limitToUtf8Bytes(line, MAX_CHANNEL_NAME_BYTES);
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
        s += QObject::tr("  [multiplexor — becomes the compound identifier]");
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
    m_tree->setItemDelegateForColumn(3, new UnitDelegate(m_tree));
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
            // Underscores become spaces here, at the one point where a DBC
            // signal turns into a channel name the user can see and edit
            // before it is created. Not in the parser: the file refers to its
            // own signals by the underscored name (SIG_VALTYPE_), and not in
            // accept() either, which would import a name different from the
            // one shown in this column.
            sItem->setText(0, channelNameFromDbcSignal(sig.name));
            // THE CATALOGUE'S SPELLING, not the file's. A .dbc writes its unit
            // as free text - "degC", "Deg C", "Celsius" are one unit and none
            // of them is how this application spells it - so importing the
            // string verbatim produced channels whose unit no list offered.
            const DbcUnit picked = dbcUnitFor(sig.unit);
            sItem->setText(1, picked.quantity);
            sItem->setText(2, signalDetails(sig));
            sItem->setText(3, picked.unit);
            sItem->setData(0, RoleRawUnit, sig.unit);
            if (!picked.recognised) {
                // Nothing matched, so the import does not guess. The row is
                // marked, both columns are editable, and the note at OK counts
                // whatever is still unresolved - the alternative is a channel
                // carrying a unit that says something untrue about its numbers.
                markUnknownUnit(sItem, sig.unit);
            }
            // THE MULTIPLEXOR IS NOT A CHANNEL, and offering it as one is how a
            // multiplexed message imported into a section that could not be
            // saved. Its bits ARE the compound identifier's selector: the
            // import turns each multiplexor VALUE into an identifier, and the
            // device writes that selector into the frame after the channels.
            // Imported as a row as well, it sat exactly on top of its own
            // selector - a blocking clash that the section editor reports the
            // moment the message is opened, about a row the user never chose to
            // put there.
            //
            // Shown, though, and not hidden: the row is how a reader sees which
            // signal decides the variant, and the Details column now says what
            // becomes of it. Only the checkbox goes.
            const bool isSelector = sig.isMultiplexor && msg.hasMultiplexing();
            if (isSelector) {
                sItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
                sItem->setToolTip(0,
                    tr("The multiplexor is not imported as a channel: this message becomes "
                       "a compound section, and each of its multiplexor values becomes an "
                       "identifier. The identifier IS this signal - the device writes it "
                       "into the frame itself, so a channel on the same bits would be "
                       "overwritten by it."));
            } else {
                sItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable
                                | Qt::ItemIsEditable);
                sItem->setCheckState(0, Qt::Unchecked);
            }
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
    if (m_updating)
        return;
    // CHANNEL TYPE MOVED: the unit beside it may no longer be one of that
    // type's, and a Pressure channel measured in "rpm" is not a thing the
    // catalogue can express. Snap it to the new type's default rather than
    // leaving a pair that cannot both be true.
    if (column == 1 && item->data(0, RoleSigIdx).toInt() >= 0) {
        const QStringList allowed = ChannelCatalog::unitsForQuantity(item->text(1));
        if (!allowed.contains(item->text(3))) {
            m_updating = true;
            item->setText(3, ChannelCatalog::defaultUnitForQuantity(item->text(1)));
            // Whatever the file said is answered for now, so the mark comes off
            // and the row stops counting as unresolved.
            item->setForeground(3, QBrush());
            item->setToolTip(3, QString());
            item->setToolTip(1, QString());
            m_updating = false;
        }
        return;
    }
    if (column == 3 && item->data(0, RoleSigIdx).toInt() >= 0) {
        // A unit chosen by hand clears the mark for the same reason.
        item->setForeground(3, QBrush());
        item->setToolTip(3, QString());
        item->setToolTip(1, QString());
        return;
    }
    if (column != 0)
        return;
    m_updating = true;
    if (item->data(0, RoleSigIdx).toInt() < 0) {
        // Message row toggled: cascade to its signals.
        const Qt::CheckState st = item->checkState(0);
        if (st != Qt::PartiallyChecked)
            for (int si = 0; si < item->childCount(); ++si)
                if (item->child(si)->flags() & Qt::ItemIsUserCheckable)
                    item->child(si)->setCheckState(0, st);
    } else if (QTreeWidgetItem *p = item->parent()) {
        // Signal row toggled: recompute the parent's tristate.
        //
        // Over the CHECKABLE children only. A multiplexed message has one child
        // with no checkbox — the multiplexor, which becomes the identifier
        // rather than a channel — and counting it in the total would leave the
        // message stuck at PartiallyChecked with every signal it actually
        // offers already ticked.
        int checked = 0;
        int checkable = 0;
        for (int si = 0; si < p->childCount(); ++si) {
            if (!(p->child(si)->flags() & Qt::ItemIsUserCheckable))
                continue;
            ++checkable;
            if (p->child(si)->checkState(0) == Qt::Checked)
                ++checked;
        }
        p->setCheckState(0, checked == 0 ? Qt::Unchecked
                                         : checked == checkable ? Qt::Checked
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
            if (mItem->child(si)->flags() & Qt::ItemIsUserCheckable)
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
            // AND THE UNIT, which this column used to show and then discard:
            // the channel took the DBC's raw string from channelFromDbcSignal
            // whatever the user picked here. "(pick one)" means they were asked
            // and did not, so the channel is unitless rather than carrying a
            // placeholder as its unit.
            const QString chosenUnit = sItem->text(3);
            ch.unit = chosenUnit == tr("(pick one)") ? QString() : chosenUnit;
            if (chosenUnit == tr("(pick one)")) {
                warns.append(tr("%1 . %2: the file's unit \"%3\" is not one this application "
                                "offers and none was picked, so the channel is unitless")
                                 .arg(msg.name, sig.name,
                                      sItem->data(0, RoleRawUnit).toString()));
            }
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
            // HELD BACK rather than made into a row. The tree does not offer the
            // multiplexor a checkbox, so this normally holds nothing; it is
            // separated anyway because the fallbacks below turn the message into
            // a PLAIN section, and there the multiplexor is an ordinary field
            // with no selector to collide with - so which branch runs decides
            // whether it is a channel, and that decision cannot be made until
            // the selected signals are known.
            for (QTreeWidgetItem *sItem : checkedSigs) {
                const DbcSignal &sig = msg.signalList[sItem->data(0, RoleSigIdx).toInt()];
                if (sig.isMultiplexed) {
                    byValue[sig.muxValue].append(sItem);
                } else if (sig.isMultiplexor) {
                    // NOT A ROW. Its bits become the identifier selector below,
                    // and the device writes that selector into the frame after
                    // the channels - so a channel here is not sharing those bits
                    // but being overwritten by them, which the section editor
                    // refuses to save. buildTree gives it no checkbox for the
                    // same reason; this is the second lock on the same door,
                    // because the first one is a UI flag and this is the code
                    // that builds the section.
                    continue;
                } else {
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
