// "Conditions" (Calculations menu) — grid editor for
// Configuration::conditionRows, mapping 1:1 onto the firmware condition table.
#pragma once

#include <QDialog>

#include "../model/configuration.h"

class QPushButton;
class QTreeWidget;

namespace ct {

class ConditionsDialog : public QDialog
{
    Q_OBJECT
public:
    explicit ConditionsDialog(Configuration *config, QWidget *parent = nullptr);

    static QStringList opNames();     // indexed by ct::ConditionOp (==, !=, <, <=, >, >=)

private:
    void rebuild();
    void onAdd();
    void onChange();
    void onRemove();
    void updateButtons();

    Configuration *m_config;
    QList<ConditionRow> m_rows; // working copy, committed on OK
    QTreeWidget *m_tree;
    QPushButton *m_addButton;
    QPushButton *m_changeButton;
    QPushButton *m_removeButton;
};

} // namespace ct
