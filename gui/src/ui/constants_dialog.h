// "Constants" (Calculations menu) — grid editor for Configuration::constantRows,
// mapping 1:1 onto the firmware constants table. A constant is essentially a
// custom channel (Name + Data Type + Decimals, range derived from the type)
// carrying a fixed Value; it has no Channel Type or Display Units. On OK the
// dialog also syncs a catalogue channel for each constant so it can be
// referenced elsewhere.
#pragma once

#include <QDialog>

#include "../model/configuration.h"

class QPushButton;
class QTreeWidget;

namespace ct {

class ConstantsDialog : public QDialog
{
    Q_OBJECT
public:
    explicit ConstantsDialog(Configuration *config, QWidget *parent = nullptr);

private:
    void rebuild();
    void onAdd();
    void onChange();
    void onRemove();
    void updateButtons();
    void commit();

    Configuration *m_config;
    QList<ConstantRow> m_rows;          // working copy, committed on OK
    QList<QString> m_originalNames;     // per-row committed name ("" = added this session)
    QTreeWidget *m_tree;
    QPushButton *m_addButton;
    QPushButton *m_changeButton;
    QPushButton *m_removeButton;
};

} // namespace ct
