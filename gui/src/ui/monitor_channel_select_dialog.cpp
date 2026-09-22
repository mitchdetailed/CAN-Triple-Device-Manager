#include "monitor_channel_select_dialog.h"

#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSet>
#include <QShortcut>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>

#include "../model/channel_list.h"
#include "../model/user_paths.h"

namespace ct {

namespace {

enum Column { ColName = 0, ColQuantity, ColUnits, ColType, ColCount };

// Name order, case-insensitive — the order the grid lists channels in when
// nothing is selected, so the left-hand list reads like the grid it feeds.
bool namesBefore(const QString &a, const QString &b)
{
    return QString::compare(a, b, Qt::CaseInsensitive) < 0;
}

QTreeWidget *makeList(const QString &title, const QString &objectName, QWidget *parent)
{
    auto *tree = new QTreeWidget(parent);
    tree->setObjectName(objectName);
    tree->setColumnCount(ColCount);
    tree->setHeaderLabels(QStringList()
                          << title << QObject::tr("Quantity") << QObject::tr("Units")
                          << QObject::tr("Type"));
    tree->setRootIsDecorated(false);
    tree->setUniformRowHeights(true);
    tree->setAllColumnsShowFocus(true);
    // Ctrl adds one, Shift takes a block, a plain click starts over — the
    // selection every list on the desktop has, which is what makes it usable
    // without reading anything.
    tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    tree->setSelectionBehavior(QAbstractItemView::SelectRows);
    tree->header()->setStretchLastSection(false);
    tree->header()->setSectionResizeMode(ColName, QHeaderView::Stretch);
    tree->setColumnWidth(ColQuantity, 140);
    tree->setColumnWidth(ColUnits, 70);
    tree->setColumnWidth(ColType, 64);
    return tree;
}

// The list's selected rows in LIST order, not click order, so a block picked
// with Shift keeps its sequence when it lands on the other side. Rows the Find
// box has hidden are left out: a selection can outlive the row it is on going
// out of view, and moving what cannot be seen is the surprise this avoids.
QList<QTreeWidgetItem *> selectedInOrder(QTreeWidget *tree)
{
    QList<QTreeWidgetItem *> items;
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *item = tree->topLevelItem(i);
        if (item->isSelected() && !item->isHidden())
            items << item;
    }
    return items;
}

} // namespace

MonitorChannelSelectDialog::MonitorChannelSelectDialog(const QList<Channel> &channels,
                                                       const QStringList &selected,
                                                       QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Select Channels"));
    resize(1100, 580);

    m_available = makeList(tr("Available Channels"), QStringLiteral("availableChannels"), this);
    m_selected = makeList(tr("Selected Channels"), QStringLiteral("selectedChannels"), this);
    // The right-hand list is put in order by dragging rows. InternalMove moves
    // the row rather than copying it, and the items refuse drops ONTO
    // themselves (no ItemIsDropEnabled — see makeItem), so a row can only land
    // between rows and never become a child of one.
    m_selected->setDragEnabled(true);
    m_selected->setAcceptDrops(true);
    m_selected->setDropIndicatorShown(true);
    m_selected->setDefaultDropAction(Qt::MoveAction);
    m_selected->setDragDropMode(QAbstractItemView::InternalMove);

    m_find = new QLineEdit(this);
    m_find->setObjectName(QStringLiteral("findChannels"));
    m_find->setPlaceholderText(tr("Find in available channels"));
    m_find->setClearButtonEnabled(true);
    connect(m_find, &QLineEdit::textChanged, this, &MonitorChannelSelectDialog::applyFind);

    m_toRight = new QPushButton(tr(">>"), this);
    m_toRight->setObjectName(QStringLiteral("moveRight"));
    m_toRight->setToolTip(tr("Add the highlighted channels to the selection (or double-click "
                             "one)"));
    m_toLeft = new QPushButton(tr("<<"), this);
    m_toLeft->setObjectName(QStringLiteral("moveLeft"));
    m_toLeft->setToolTip(tr("Remove the highlighted channels from the selection (or "
                            "double-click one, or press Delete)"));
    connect(m_toRight, &QPushButton::clicked, this, &MonitorChannelSelectDialog::moveRight);
    connect(m_toLeft, &QPushButton::clicked, this, &MonitorChannelSelectDialog::moveLeft);
    connect(m_available, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem *, int) { moveRight(); });
    connect(m_selected, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem *, int) { moveLeft(); });
    auto *remove = new QShortcut(QKeySequence::Delete, m_selected);
    remove->setContext(Qt::WidgetShortcut);
    connect(remove, &QShortcut::activated, this, &MonitorChannelSelectDialog::moveLeft);
    connect(m_available->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            [this]() { updateButtons(); });
    connect(m_selected->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            [this]() { updateButtons(); });

    m_count = new QLabel(this);
    m_count->setObjectName(QStringLiteral("selectionCount"));

    m_saveList = new QPushButton(tr("Save List…"), this);
    m_saveList->setObjectName(QStringLiteral("saveList"));
    m_saveList->setToolTip(tr("Save the selected channels, in this order, as a channel list "
                              "file to load into another configuration later."));
    m_loadList = new QPushButton(tr("Load List…"), this);
    m_loadList->setObjectName(QStringLiteral("loadList"));
    m_loadList->setToolTip(tr("Replace the selection with a saved channel list. Channels the "
                              "list names that this configuration does not have are reported "
                              "and left out."));
    connect(m_saveList, &QPushButton::clicked, this, &MonitorChannelSelectDialog::onSaveList);
    connect(m_loadList, &QPushButton::clicked, this, &MonitorChannelSelectDialog::onLoadList);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    // Layout: Find over the left list; the two lists with the arrow buttons
    // between them; the count and OK/Cancel along the bottom.
    auto *left = new QVBoxLayout;
    left->addWidget(m_find);
    left->addWidget(m_available, 1);
    auto *arrows = new QVBoxLayout;
    arrows->addStretch(1);
    arrows->addWidget(m_toRight);
    arrows->addWidget(m_toLeft);
    arrows->addStretch(1);
    auto *right = new QVBoxLayout;
    // Same height as the Find box, so the two headers line up.
    auto *rightSpacer = new QLabel(tr("Drag rows to set the order the monitor shows them in."),
                                   this);
    rightSpacer->setStyleSheet(QStringLiteral("color: gray;"));
    right->addWidget(rightSpacer);
    right->addWidget(m_selected, 1);
    auto *lists = new QHBoxLayout;
    lists->addLayout(left, 1);
    lists->addLayout(arrows);
    lists->addLayout(right, 1);
    auto *bottom = new QHBoxLayout;
    bottom->addWidget(m_loadList);
    bottom->addWidget(m_saveList);
    bottom->addSpacing(12);
    bottom->addWidget(m_count);
    bottom->addStretch(1);
    bottom->addWidget(buttons);
    auto *layout = new QVBoxLayout(this);
    layout->addLayout(lists, 1);
    layout->addLayout(bottom);

    // Populate: the chosen names on the right in their order, the rest on the
    // left in name order. A chosen name the document does not map is dropped
    // here rather than shown as a row that can never light up.
    QHash<QString, int> index;
    for (int i = 0; i < channels.size(); ++i)
        index.insert(channels.at(i).name, i);
    QSet<QString> chosen;
    for (const QString &name : selected) {
        const auto it = index.constFind(name);
        if (it == index.constEnd() || chosen.contains(name))
            continue;
        chosen.insert(name);
        m_selected->addTopLevelItem(makeItem(channels.at(it.value())));
    }
    QList<Channel> rest;
    for (const Channel &ch : channels)
        if (!chosen.contains(ch.name))
            rest << ch;
    std::sort(rest.begin(), rest.end(),
              [](const Channel &a, const Channel &b) { return namesBefore(a.name, b.name); });
    for (const Channel &ch : rest)
        m_available->addTopLevelItem(makeItem(ch));

    updateCount();
    updateButtons();
    m_find->setFocus();
}

QTreeWidgetItem *MonitorChannelSelectDialog::makeItem(const Channel &ch) const
{
    auto *item = new QTreeWidgetItem(QStringList() << ch.name << ch.quantity << ch.unit
                                                   << ch.dataType);
    item->setFlags((item->flags() | Qt::ItemIsDragEnabled) & ~Qt::ItemIsDropEnabled);
    return item;
}

QStringList MonitorChannelSelectDialog::selection() const
{
    QStringList names;
    for (int i = 0; i < m_selected->topLevelItemCount(); ++i)
        names << m_selected->topLevelItem(i)->text(ColName);
    return names;
}

void MonitorChannelSelectDialog::moveRight()
{
    const QList<QTreeWidgetItem *> items = selectedInOrder(m_available);
    if (items.isEmpty())
        return;
    m_selected->clearSelection();
    for (QTreeWidgetItem *item : items) {
        m_available->takeTopLevelItem(m_available->indexOfTopLevelItem(item));
        m_selected->addTopLevelItem(item);
        // Left highlighted on the right, so the eye follows them across.
        item->setSelected(true);
    }
    m_selected->scrollToItem(items.last());
    updateCount();
    updateButtons();
}

void MonitorChannelSelectDialog::moveLeft()
{
    const QList<QTreeWidgetItem *> items = selectedInOrder(m_selected);
    if (items.isEmpty())
        return;
    m_available->clearSelection();
    for (QTreeWidgetItem *item : items) {
        m_selected->takeTopLevelItem(m_selected->indexOfTopLevelItem(item));
        addToAvailable(item);
        item->setSelected(true);
    }
    updateCount();
    updateButtons();
}

void MonitorChannelSelectDialog::addToAvailable(QTreeWidgetItem *item)
{
    const QString name = item->text(ColName);
    int at = 0;
    while (at < m_available->topLevelItemCount()
           && namesBefore(m_available->topLevelItem(at)->text(ColName), name))
        ++at;
    m_available->insertTopLevelItem(at, item);
    const QString needle = m_find->text().trimmed();
    item->setHidden(!needle.isEmpty() && !name.contains(needle, Qt::CaseInsensitive));
}

void MonitorChannelSelectDialog::applyFind()
{
    const QString needle = m_find->text().trimmed();
    for (int i = 0; i < m_available->topLevelItemCount(); ++i) {
        QTreeWidgetItem *item = m_available->topLevelItem(i);
        item->setHidden(!needle.isEmpty()
                        && !item->text(ColName).contains(needle, Qt::CaseInsensitive));
    }
    updateButtons();
}

void MonitorChannelSelectDialog::updateCount()
{
    const int n = m_selected->topLevelItemCount();
    if (n == 0)
        m_count->setText(tr("No channels selected — the monitor will show every channel"));
    else if (n == 1)
        m_count->setText(tr("1 channel selected"));
    else
        m_count->setText(tr("%1 channels selected").arg(n));
    // An empty list is not worth a file.
    if (m_saveList)
        m_saveList->setEnabled(n > 0);
}

void MonitorChannelSelectDialog::applyChannelList(const QStringList &names, QStringList *notFound)
{
    // Everything back to the left first: a loaded list IS the selection, not
    // an addition to whatever was there.
    while (m_selected->topLevelItemCount() > 0)
        addToAvailable(m_selected->takeTopLevelItem(0));
    m_available->clearSelection();
    m_selected->clearSelection();

    QSet<QString> seen;
    for (const QString &name : names) {
        if (seen.contains(name))
            continue;
        seen.insert(name);
        QTreeWidgetItem *item = nullptr;
        for (int i = 0; i < m_available->topLevelItemCount() && !item; ++i)
            if (m_available->topLevelItem(i)->text(ColName) == name)
                item = m_available->topLevelItem(i);
        if (!item) {
            if (notFound)
                *notFound << name;
            continue;
        }
        m_available->takeTopLevelItem(m_available->indexOfTopLevelItem(item));
        item->setHidden(false); // the Find box may have hidden it on the left
        m_selected->addTopLevelItem(item);
    }
    updateCount();
    updateButtons();
}

void MonitorChannelSelectDialog::onSaveList()
{
    const QString title = tr("Save Channel List");
    // The Channel Lists folder is the offer, not a rule: if this copy cannot
    // write there (ensureWritableDirectory says why), the dialog opens on
    // Qt's default and the user picks anywhere.
    QString dir = channelListsDirectory();
    QString why;
    if (!ensureWritableDirectory(dir, &why)) {
        QMessageBox::warning(this, title, why);
        dir.clear();
    }
    QString path = QFileDialog::getSaveFileName(
        this, title,
        dir.isEmpty() ? QString()
                      : dir + QStringLiteral("/Channel List.") + channelListExtension(),
        channelListFilter());
    if (path.isEmpty())
        return;
    if (QFileInfo(path).suffix().isEmpty())
        path += QLatin1Char('.') + channelListExtension();
    QString error;
    if (!writeChannelList(path, selection(), &error))
        QMessageBox::warning(this, title, error);
}

void MonitorChannelSelectDialog::onLoadList()
{
    const QString title = tr("Load Channel List");
    const QString dir = channelListsDirectory();
    QDir().mkpath(dir); // best effort; the dialog copes with an absent folder
    const QString path = QFileDialog::getOpenFileName(this, title, dir, channelListFilter());
    if (path.isEmpty())
        return;
    QStringList names;
    QString error;
    if (!readChannelList(path, &names, &error)) {
        QMessageBox::warning(this, title, error);
        return;
    }
    QStringList notFound;
    applyChannelList(names, &notFound);
    if (notFound.isEmpty())
        return;
    // Named, every one of them (within reason): "3 channels were not found"
    // sends the user hunting; the names say which list to fix or which
    // configuration is short.
    constexpr int kMaxNamed = 25;
    QStringList shown = notFound.mid(0, kMaxNamed);
    if (notFound.size() > kMaxNamed)
        shown << tr("… and %1 more").arg(notFound.size() - kMaxNamed);
    const QString file = QFileInfo(path).fileName();
    QMessageBox::information(
        this, title,
        (notFound.size() == 1
             ? tr("1 channel in \"%1\" is not in this configuration and was left out — "
                  "not found:")
                   .arg(file)
             : tr("%1 channels in \"%2\" are not in this configuration and were left out — "
                  "not found:")
                   .arg(notFound.size())
                   .arg(file))
            + QStringLiteral("\n\n") + shown.join(QLatin1Char('\n')));
}

void MonitorChannelSelectDialog::updateButtons()
{
    m_toRight->setEnabled(!selectedInOrder(m_available).isEmpty());
    m_toLeft->setEnabled(!selectedInOrder(m_selected).isEmpty());
}

} // namespace ct
