// File > Config Summary — the MoTeC-style Channel Summary Report viewer,
// with Print / Save PDF / Save Text export.
#pragma once

#include <QDialog>

class QTextBrowser;

namespace ct {

class Configuration;

class ConfigSummaryDialog : public QDialog
{
    Q_OBJECT
public:
    explicit ConfigSummaryDialog(Configuration *config, QWidget *parent = nullptr);

private:
    void onPrint();
    void onSavePdf();
    void onSaveText();
    QString suggestedBaseName() const;

    Configuration *m_config;
    QTextBrowser *m_view;
};

} // namespace ct
