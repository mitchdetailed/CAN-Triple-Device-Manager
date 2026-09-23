// "Up / Down Counters" (Calculations menu) — grid editor for
// Configuration::counterRows, mapping 1:1 onto the firmware counter table.
#pragma once

#include <QDialog>

#include "../model/configuration.h"

class QLabel;
class QPushButton;
class QTreeWidget;

namespace ct {

class CountersDialog : public QDialog
{
    Q_OBJECT
public:
    explicit CountersDialog(Configuration *config, QWidget *parent = nullptr);

private:
    void rebuild();
    void onAdd();
    void onChange();
    void onRemove();
    // ↑ Move Up / ↓ Move Down: swaps the selected row with its neighbour in the
    // working copy and follows it. Row order is evaluation order.
    void onMove(int delta);
    void updateButtons();
    void updatePreserveBudget(); // "N of 20" retained-counter budget

    Configuration *m_config;
    QList<CounterRow> m_rows; // working copy, committed on OK
    QTreeWidget *m_tree;
    QLabel *m_preserveLabel;
    QPushButton *m_addButton;
    QPushButton *m_changeButton;
    QPushButton *m_removeButton;
    QPushButton *m_upButton;
    QPushButton *m_downButton;
};

} // namespace ct
