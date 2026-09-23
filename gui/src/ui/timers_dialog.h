// "Timers" (Calculations menu) — grid editor for Configuration::timerRows,
// mapping 1:1 onto the firmware timer table.
#pragma once

#include <QDialog>

#include "../model/configuration.h"

class QPushButton;
class QTreeWidget;

namespace ct {

class TimersDialog : public QDialog
{
    Q_OBJECT
public:
    explicit TimersDialog(Configuration *config, QWidget *parent = nullptr);

private:
    void rebuild();
    void onAdd();
    void onChange();
    void onRemove();
    // ↑ Move Up / ↓ Move Down: swaps the selected row with its neighbour in the
    // working copy and follows it. Row order is evaluation order.
    void onMove(int delta);
    void updateButtons();

    Configuration *m_config;
    QList<TimerRow> m_rows; // working copy, committed on OK
    QTreeWidget *m_tree;
    QPushButton *m_addButton;
    QPushButton *m_changeButton;
    QPushButton *m_removeButton;
    QPushButton *m_upButton;
    QPushButton *m_downButton;
};

} // namespace ct
