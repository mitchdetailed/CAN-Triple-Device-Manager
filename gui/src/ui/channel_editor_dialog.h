// "Channel Editor" (Tools menu) — one table of every channel in the document
// with its storage type, precision, range and receive-timeout default, plus a
// search box and a way into the existing Edit Custom Channel dialog.
#pragma once

#include <QDialog>

class QLabel;
class QLineEdit;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace ct {

class Configuration;

class ChannelEditorDialog : public QDialog
{
    Q_OBJECT
public:
    explicit ChannelEditorDialog(Configuration *config, QWidget *parent = nullptr);

    static void run(Configuration *config, QWidget *parent);

private:
    void rebuild();
    void onEdit();
    void onNew();
    void updateButtons();
    QString currentChannelName() const;

    Configuration *m_config;
    QLineEdit *m_searchEdit;
    QTreeWidget *m_tree;
    QLabel *m_summaryLabel;
    QPushButton *m_editButton;
    QPushButton *m_newButton;
};

} // namespace ct
