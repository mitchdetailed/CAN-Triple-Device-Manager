// "Math Channels" (Calculations menu) — grid editor for Configuration::mathRows,
// mapping 1:1 onto the firmware math table.
#pragma once

#include <QDialog>

#include "../model/configuration.h"

class QPushButton;
class QTreeWidget;

namespace ct {

class MathDialog : public QDialog
{
    Q_OBJECT
public:
    explicit MathDialog(Configuration *config, QWidget *parent = nullptr);

    // Operator display names indexed by ct::MathOp.
    static QStringList opNames();

private:
    void rebuild();
    void onAdd();
    void onChange();
    void onRemove();
    void updateButtons();

    Configuration *m_config;
    QList<MathRow> m_rows; // working copy, committed on OK
    QTreeWidget *m_tree;
    QPushButton *m_addButton;
    QPushButton *m_changeButton;
    QPushButton *m_removeButton;
};

} // namespace ct
