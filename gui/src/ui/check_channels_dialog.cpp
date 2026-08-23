// File > Check Channels — validation report dialog.
#include "check_channels_dialog.h"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QStyle>
#include <QTime>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include <utility>

#include "../model/config_report.h"
#include "../model/configuration.h"
#include "../model/validation.h"

namespace ct {

namespace {

int severityRank(ValidationIssue::Severity sev)
{
    switch (sev) {
    case ValidationIssue::Error:   return 0;
    case ValidationIssue::Warning: return 1;
    case ValidationIssue::Info:    return 2;
    }
    return 3;
}

} // namespace

CheckChannelsDialog::CheckChannelsDialog(Configuration *config, QWidget *parent)
    : QDialog(parent)
    , m_config(config)
    , m_tree(nullptr)
    , m_summaryLabel(nullptr)
{
    setWindowTitle(tr("Check Channels"));
    setModal(true);
    resize(700, 450);

    auto *mainLayout = new QVBoxLayout(this);

    // States exactly which document state was checked — the check always runs
    // on the CURRENT in-memory configuration, saved or not.
    m_stateLabel = new QLabel(this);
    m_stateLabel->setWordWrap(true);
    mainLayout->addWidget(m_stateLabel);

    m_tree = new QTreeWidget(this);
    m_tree->setColumnCount(3);
    m_tree->setHeaderLabels({tr("Severity"), tr("Location"), tr("Message")});
    m_tree->setRootIsDecorated(false);
    m_tree->setAllColumnsShowFocus(true);
    m_tree->setUniformRowHeights(true);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(2, QHeaderView::Stretch);
    mainLayout->addWidget(m_tree, 1);

    m_summaryLabel = new QLabel(this);
    mainLayout->addWidget(m_summaryLabel);

    auto *buttonBox = new QDialogButtonBox(QDialogButtonBox::Close, this);
    auto *checkAgainButton = new QPushButton(tr("Check Again"), this);
    buttonBox->addButton(checkAgainButton, QDialogButtonBox::ActionRole);
    connect(checkAgainButton, &QPushButton::clicked, this, [this]() { recheck(); });
    m_removeUnusedButton = new QPushButton(tr("Remove Unused Channels…"), this);
    buttonBox->addButton(m_removeUnusedButton, QDialogButtonBox::ActionRole);
    connect(m_removeUnusedButton, &QPushButton::clicked, this,
            [this]() { onRemoveUnused(); });
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    mainLayout->addWidget(buttonBox);

    recheck();
}

void CheckChannelsDialog::recheck()
{
    m_tree->clear();
    m_hasErrors = false;
    m_unused.clear();

    QList<ValidationIssue> issues;
    if (m_config) {
        issues = validateConfiguration(*m_config);
        m_unused = analyzeChannelUsage(*m_config).unused;

        QString state = tr("Configuration: %1 — %2")
                            .arg(m_config->displayName(),
                                 m_config->filePath().isEmpty() ? tr("not saved to a file")
                                                                : m_config->filePath());
        if (m_config->isDirty())
            state += tr(" — unsaved changes");
        state += tr("\nChecked at %1 on the current in-memory state (saving is not required).")
                     .arg(QTime::currentTime().toString(QStringLiteral("HH:mm:ss")));
        m_stateLabel->setText(state);
    }
    m_removeUnusedButton->setEnabled(!m_unused.isEmpty());
    m_removeUnusedButton->setToolTip(
        m_unused.isEmpty()
            ? tr("Every catalogue channel is generated, used, or referenced somewhere")
            : tr("Remove the %1 channel(s) nothing references").arg(m_unused.size()));

    // Stable sort: errors, then warnings, then info; keep original order
    // within each severity group.
    QList<ValidationIssue> sorted;
    sorted.reserve(issues.size());
    for (int rank = 0; rank <= 2; ++rank) {
        for (const ValidationIssue &issue : issues) {
            if (severityRank(issue.severity) == rank)
                sorted.append(issue);
        }
    }

    int errorCount = 0;
    int warningCount = 0;

    QStyle *st = style();
    const QIcon errorIcon = st->standardIcon(QStyle::SP_MessageBoxCritical);
    const QIcon warningIcon = st->standardIcon(QStyle::SP_MessageBoxWarning);
    const QIcon infoIcon = st->standardIcon(QStyle::SP_MessageBoxInformation);

    for (const ValidationIssue &issue : sorted) {
        auto *item = new QTreeWidgetItem(m_tree);
        switch (issue.severity) {
        case ValidationIssue::Error:
            item->setIcon(0, errorIcon);
            item->setText(0, tr("Error"));
            ++errorCount;
            break;
        case ValidationIssue::Warning:
            item->setIcon(0, warningIcon);
            item->setText(0, tr("Warning"));
            ++warningCount;
            break;
        case ValidationIssue::Info:
            item->setIcon(0, infoIcon);
            item->setText(0, tr("Info"));
            break;
        }
        item->setText(1, issue.location);
        item->setText(2, issue.message);
        // Word-wrap tooltip on the (possibly elided) message column.
        item->setToolTip(2, QStringLiteral("<qt>%1</qt>")
                                .arg(issue.message.toHtmlEscaped()));
        if (!issue.location.isEmpty())
            item->setToolTip(1, issue.location);
    }

    m_hasErrors = errorCount > 0;

    if (errorCount == 0 && warningCount == 0) {
        m_summaryLabel->setText(tr("No problems found."));
        m_summaryLabel->setStyleSheet(QStringLiteral("color: green;"));
    } else {
        m_summaryLabel->setText(tr("%1 errors, %2 warnings.")
                                    .arg(errorCount)
                                    .arg(warningCount));
        m_summaryLabel->setStyleSheet(errorCount > 0
                                          ? QStringLiteral("color: red;")
                                          : QString());
    }
}

void CheckChannelsDialog::onRemoveUnused()
{
    if (!m_config || m_unused.isEmpty())
        return;
    // The channel list is unbounded (hundreds after clearing out a big DBC
    // import), so it goes in the scrollable details pane, not the question.
    QMessageBox box(QMessageBox::Question, windowTitle(),
                    tr("Remove %1 unused channel(s) from the configuration?\n\n"
                       "Nothing references them (no comms row, calculation, or Off section), "
                       "so no other part of the configuration changes. "
                       "See Show Details for the full list.")
                        .arg(m_unused.size()),
                    QMessageBox::Yes | QMessageBox::No, this);
    box.setDefaultButton(QMessageBox::No);
    box.setDetailedText(m_unused.join(QStringLiteral("\n")));
    if (box.exec() != QMessageBox::Yes)
        return;
    for (const QString &name : std::as_const(m_unused))
        m_config->catalog().removeUserChannel(name);
    m_config->setDirty();
    recheck();
}

bool CheckChannelsDialog::run(Configuration *config, QWidget *parent)
{
    CheckChannelsDialog dlg(config, parent);
    dlg.exec();
    return !dlg.hasErrors();
}

} // namespace ct
