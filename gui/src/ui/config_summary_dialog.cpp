#include "config_summary_dialog.h"

#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QPageSize>
#include <QPrintDialog>
#include <QPrinter>
#include <QPushButton>
#include <QTextBrowser>
#include <QTextStream>
#include <QVBoxLayout>

#include "../model/config_report.h"
#include "../model/configuration.h"

namespace ct {

ConfigSummaryDialog::ConfigSummaryDialog(Configuration *config, QWidget *parent)
    : QDialog(parent)
    , m_config(config)
{
    setWindowTitle(tr("Config Summary — %1").arg(config->displayName()));
    resize(900, 650);

    auto *layout = new QVBoxLayout(this);

    m_view = new QTextBrowser;
    m_view->setOpenLinks(false);
    m_view->setLineWrapMode(QTextEdit::NoWrap); // the report is column-aligned
    m_view->setHtml(configSummaryHtml(*config));
    layout->addWidget(m_view, 1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    auto *printButton = new QPushButton(tr("Print…"));
    buttons->addButton(printButton, QDialogButtonBox::ActionRole);
    connect(printButton, &QPushButton::clicked, this, &ConfigSummaryDialog::onPrint);
    auto *pdfButton = new QPushButton(tr("Save PDF…"));
    buttons->addButton(pdfButton, QDialogButtonBox::ActionRole);
    connect(pdfButton, &QPushButton::clicked, this, &ConfigSummaryDialog::onSavePdf);
    auto *textButton = new QPushButton(tr("Save Text…"));
    buttons->addButton(textButton, QDialogButtonBox::ActionRole);
    connect(textButton, &QPushButton::clicked, this, &ConfigSummaryDialog::onSaveText);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

QString ConfigSummaryDialog::suggestedBaseName() const
{
    // "<config name> Channel Summary", next to the saved file when there is one.
    QString dir;
    if (!m_config->filePath().isEmpty())
        dir = QFileInfo(m_config->filePath()).absolutePath() + QLatin1Char('/');
    return dir + tr("%1 Channel Summary").arg(m_config->displayName());
}

void ConfigSummaryDialog::onPrint()
{
    QPrinter printer(QPrinter::HighResolution);
    printer.setPageSize(QPageSize(QPageSize::Letter));
    QPrintDialog dialog(&printer, this);
    dialog.setWindowTitle(tr("Print Config Summary"));
    if (dialog.exec() != QDialog::Accepted)
        return;
    m_view->document()->print(&printer);
}

void ConfigSummaryDialog::onSavePdf()
{
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Save Config Summary as PDF"), suggestedBaseName() + QStringLiteral(".pdf"),
        tr("PDF files (*.pdf)"));
    if (path.isEmpty())
        return;
    QPrinter printer(QPrinter::HighResolution);
    printer.setOutputFormat(QPrinter::PdfFormat);
    printer.setOutputFileName(path);
    printer.setPageSize(QPageSize(QPageSize::Letter));
    m_view->document()->print(&printer);
}

void ConfigSummaryDialog::onSaveText()
{
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Save Config Summary as Text"), suggestedBaseName() + QStringLiteral(".txt"),
        tr("Text files (*.txt)"));
    if (path.isEmpty())
        return;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::warning(this, windowTitle(),
                             tr("Could not write %1:\n%2").arg(path, file.errorString()));
        return;
    }
    QTextStream out(&file);
    out << configSummaryText(*m_config);
}

} // namespace ct
