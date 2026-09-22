// "Select Channels" for Monitor Channels — which channels the grid shows, and in
// what order. Every channel the document maps is listed on the left, the chosen
// ones on the right, moved between them with >> and << (or a double click);
// Ctrl and Shift pick several at once, as in any list. The right-hand list is
// dragged into the order wanted, and that order is the grid's, top to bottom.
//
// An empty right-hand list means "every channel", and the count line says so:
// "show nothing" is not a thing anyone asks a monitor for, and a picker that
// could produce an empty grid would have to explain the empty grid afterwards.
#pragma once

#include <QDialog>
#include <QHash>
#include <QList>
#include <QStringList>

#include "../model/channel.h"

class QLabel;
class QLineEdit;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace ct {

class MonitorChannelSelectDialog : public QDialog
{
    Q_OBJECT
public:
    // `channels`: every channel the monitor can show. `selected`: the names
    // currently chosen, in order; a name `channels` does not carry is dropped.
    MonitorChannelSelectDialog(const QList<Channel> &channels, const QStringList &selected,
                               QWidget *parent = nullptr);

    // The chosen names, top to bottom. Empty = show every channel.
    QStringList selection() const;

    // Replaces the selection with `names`, in their order, keeping only the
    // names this configuration has; the rest come back in *notFound, in the
    // list's order. What Load List… does once the file is read — public so
    // the file dialog is not in the way of testing it.
    void applyChannelList(const QStringList &names, QStringList *notFound);

private:
    void moveRight();
    void moveLeft();
    // Save List… / Load List…: the selection as a .ct3l file (model/channel_list.h)
    // in the Channel Lists folder, so a set of channels outlives the document
    // it was picked from. A name the loaded list has and this configuration
    // does not is reported by name — "not found" — and left out.
    void onSaveList();
    void onLoadList();
    void applyFind();
    void updateCount();
    void updateButtons();
    QTreeWidgetItem *makeItem(const Channel &ch) const;
    // Puts an item back on the left in name order, respecting the Find box.
    void addToAvailable(QTreeWidgetItem *item);

    QLineEdit *m_find = nullptr;
    QTreeWidget *m_available = nullptr;
    QTreeWidget *m_selected = nullptr;
    QPushButton *m_toRight = nullptr;
    QPushButton *m_toLeft = nullptr;
    QPushButton *m_saveList = nullptr;
    QPushButton *m_loadList = nullptr;
    QLabel *m_count = nullptr;
};

} // namespace ct
