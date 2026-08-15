// "Up / Down Counters" (Calculations menu) — grid editor for
// Configuration::counterRows, mapping 1:1 onto the firmware counter table.
// Mirrors MoTeC's Up/Down Counters dialog.
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
    void updateButtons();
    void updatePreserveBudget(); // "N of 20" retained-counter budget

    Configuration *m_config;
    QList<CounterRow> m_rows; // working copy, committed on OK
    QTreeWidget *m_tree;
    QLabel *m_preserveLabel;
    QPushButton *m_addButton;
    QPushButton *m_changeButton;
    QPushButton *m_removeButton;
};

} // namespace ct
