// "Integrators" (Calculations menu) — grid editor for
// Configuration::integratorRows, mapping 1:1 onto the firmware integrator
// table. An integrator adds its input to its output channel at a fixed rate.
#pragma once

#include <QDialog>

#include "../model/configuration.h"

class QPushButton;
class QTreeWidget;

namespace ct {

class IntegratorsDialog : public QDialog
{
    Q_OBJECT
public:
    explicit IntegratorsDialog(Configuration *config, QWidget *parent = nullptr);

private:
    void rebuild();
    void onAdd();
    void onChange();
    void onRemove();
    void updateButtons();

    Configuration *m_config;
    QList<IntegratorRow> m_rows; // working copy, committed on OK
    QTreeWidget *m_tree;
    QPushButton *m_addButton;
    QPushButton *m_changeButton;
    QPushButton *m_removeButton;
};

} // namespace ct
