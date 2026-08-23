// "Check Channels" (File menu) — validation report list with severity icons.
#pragma once

#include <QDialog>

#include "../model/validation.h"

class QLabel;
class QPushButton;
class QTreeWidget;

namespace ct {

class Configuration;

class CheckChannelsDialog : public QDialog
{
    Q_OBJECT
public:
    explicit CheckChannelsDialog(Configuration *config, QWidget *parent = nullptr);

    // Runs validation and shows the dialog. Returns true when there are no
    // errors (warnings/info allowed).
    static bool run(Configuration *config, QWidget *parent);

    bool hasErrors() const { return m_hasErrors; }

private:
    void recheck();
    void onRemoveUnused();

    Configuration *m_config;
    QTreeWidget *m_tree;
    QLabel *m_stateLabel;   // which config state was checked, and when
    QLabel *m_summaryLabel;
    QPushButton *m_removeUnusedButton;
    QStringList m_unused;   // unused catalogue channels found by the last check
    bool m_hasErrors = false;
};

} // namespace ct
