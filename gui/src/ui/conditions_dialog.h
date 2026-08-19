// "Conditions" (Calculations menu) — grid editor for
// Configuration::conditionRows, mapping 1:1 onto the firmware condition table.
// The row editor is file-local; it edits both of a condition's expressions
// (Set and Reset) and its mode. See ConditionRow.
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

    // The six COMPARISON operators, indexed by ct::ConditionOp (==, !=, <, <=,
    // >, >=). The two message operators (COND_OP_MSG_RX / COND_OP_MSG_TX) are
    // deliberately absent: they are phrases rather than symbols and take no
    // right-hand operand, so the row editor adds them itself and carries their
    // value in the combo's userData instead of by position.
    static QStringList opNames();

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
