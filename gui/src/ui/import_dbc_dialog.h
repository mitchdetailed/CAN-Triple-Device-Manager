// "Import DBC" — a checkable tree of a parsed .dbc file's messages and their
// signals. The user picks which messages/signals to import as receive channels,
// edits the import name and Channel Type inline, and chooses the target bus.
// Multiplexed messages import as compound sections (per-value sub-messages).
#pragma once

#include <QDialog>
#include <QList>

#include "../model/comms_types.h"
#include "../model/dbc_import.h"

class QComboBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QTreeWidget;
class QTreeWidgetItem;

namespace ct {

class Configuration;

class ImportDbcDialog : public QDialog
{
    Q_OBJECT
public:
    ImportDbcDialog(Configuration *config, const DbcFile &dbc, const QString &sourceName,
                    int defaultBusIndex, const QStringList &parseWarnings,
                    QWidget *parent = nullptr);

    // Valid after accept(): the bus the user chose, and the sections to add.
    int targetBusIndex() const;
    QList<CommsSection> importedSections() const { return m_sections; }

protected:
    void accept() override;

private:
    void buildTree();
    void applyFilter();
    void onItemChanged(QTreeWidgetItem *item, int column);
    void setAllChecked(bool checked);
    void updateOkState();

    Configuration *m_config;
    DbcFile m_dbc;

    QLineEdit *m_filter = nullptr;
    QComboBox *m_busCombo = nullptr;
    QTreeWidget *m_tree = nullptr;
    QPlainTextEdit *m_warnings = nullptr;
    QLabel *m_countLabel = nullptr;
    QDialogButtonBox *m_buttons = nullptr;

    QList<CommsSection> m_sections;
    bool m_updating = false; // guards manual parent/child check propagation
};

} // namespace ct
