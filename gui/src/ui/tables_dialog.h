// Calculations > Tables — manages Configuration::table2x16Rows / table8x8Rows.
// Each table's output is a generated channel (registered in the catalogue like a
// Constant) so it can be referenced by transmit rows, math, conditions, etc.
#pragma once

#include <QDialog>
#include <QList>

#include "../model/configuration.h"

class QPushButton;
class QTreeWidget;

namespace ct {

class TablesDialog : public QDialog
{
    Q_OBJECT
public:
    explicit TablesDialog(Configuration *config, QWidget *parent = nullptr);

private:
    void rebuild();
    void updateButtons();
    void onAdd2x16();
    void onAdd8x8();
    void onChange();
    void onRemove();
    void commit();

    // Names other calcs/channels already own — a table output must not collide.
    QStringList reservedNames() const;
    // Every other table's output name (both types), excluding the one being
    // edited, so two tables can't share an output channel.
    QStringList siblingOutputs(bool is8x8, int exceptIdx) const;
    // This dialog's working tables as a patch over the document — they are not
    // written back until OK, so an axis picker needs this to tell a live
    // channel from a dangling one. See ConfigPatch.
    ConfigPatch liveView() const;

    Configuration *m_config;
    QList<Table2x16Row> m_rows2x16;
    QList<Table8x8Row> m_rows8x8;
    QList<QString> m_orig2x16; // output names at open, for rename tracking
    QList<QString> m_orig8x8;
    // The channelRenamed listener, kept so commit() can drop it first: the
    // commit's own renameChannelReferences calls re-emit the signal, and the
    // handler rewriting m_orig* mid-commit would stop the catalog sync from
    // removing the old-name channels. See commit().
    QMetaObject::Connection m_renameConnection;

    QTreeWidget *m_tree = nullptr;
    QPushButton *m_add2x16Button = nullptr;
    QPushButton *m_add8x8Button = nullptr;
    QPushButton *m_changeButton = nullptr;
    QPushButton *m_removeButton = nullptr;
};

} // namespace ct
