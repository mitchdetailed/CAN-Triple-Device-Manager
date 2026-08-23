#include "axis_setup_dialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSet>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>

#include "../model/channel.h"
#include "../model/channel_catalog.h"
#include "channel_field.h"
#include "numeric_grid.h"
#include "select_channel_dialog.h"
#include "trimmed_spin_box.h"

namespace ct {

AxisSetupDialog::AxisSetupDialog(Configuration *config, const Axis &axis,
                                 const ConfigPatch &livePatch, QWidget *parent)
    : QDialog(parent), m_config(config), m_axis(axis), m_livePatch(livePatch)
{
    setWindowTitle(tr("%1 — Axis Setup").arg(axis.title.isEmpty() ? tr("Axis") : axis.title));
    setModal(true);

    auto *layout = new QVBoxLayout(this);

    // Input channel: read-only field + Select…, like the axis pickers in the
    // table editors (an axis reads a channel and never writes it). The field
    // SHOWS "Coolant Temp °C" and REMEMBERS "Coolant Temp"; every read of it
    // goes through channelField(), never text().
    auto *channelRow = new QHBoxLayout;
    channelRow->addWidget(new QLabel(tr("Input:"), this));
    m_channelEdit = new QLineEdit(this);
    m_channelEdit->setReadOnly(true);
    setChannelField(m_channelEdit, axis.channel, m_config->catalog());
    auto *pick = new QPushButton(tr("Select…"), this);
    connect(pick, &QPushButton::clicked, this, &AxisSetupDialog::onSelectChannel);
    channelRow->addWidget(m_channelEdit, 1);
    channelRow->addWidget(pick);
    layout->addLayout(channelRow);

    // Axis behaviour: how the lookup treats inputs between sites. "Continuous"
    // interpolates; "Discrete" holds each site's value and switches at the
    // midpoint. These map to the table row's interp flag.
    auto *behaviourRow = new QHBoxLayout;
    behaviourRow->addWidget(new QLabel(tr("Axis Behaviour:"), this));
    m_behaviour = new QComboBox(this);
    m_behaviour->addItem(tr("Continuous (interpolated)"));
    m_behaviour->addItem(tr("Discrete (centered)"));
    m_behaviour->setCurrentIndex(axis.interp ? 0 : 1);
    behaviourRow->addWidget(m_behaviour, 1);
    layout->addLayout(behaviourRow);

    auto *valuesGroup = new QGroupBox(tr("Axis Values"), this);
    auto *gv = new QVBoxLayout(valuesGroup);
    m_valuesLabel = new QLabel(valuesGroup);
    gv->addWidget(m_valuesLabel);

    m_grid = new QTableWidget(1, axis.maxSites, valuesGroup);
    m_grid->verticalHeader()->setVisible(false);
    QStringList cols;
    for (int i = 0; i < axis.maxSites; ++i)
        cols << QString::number(i + 1);
    m_grid->setHorizontalHeaderLabels(cols);
    m_grid->horizontalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    m_grid->horizontalHeader()->setDefaultSectionSize(64);
    // One row of cells; ask for a height that shows it without a vertical
    // scrollbar (QTableWidget's sizeHint ignores content).
    m_grid->setFixedHeight(m_grid->horizontalHeader()->height()
                           + m_grid->verticalHeader()->defaultSectionSize()
                           + 2 * m_grid->frameWidth() + 2);
    m_grid->setItemDelegate(new GridDelegate([this](int, int) { return cellSpec(); }, m_grid));
    m_grid->installEventFilter(this);
    m_clip = new GridClipboard(
        m_grid, [this](int, int) { return cellSpec(); }, [this]() { render(collect()); });
    // Any manual edit keeps the row sorted and compact (duplicates collapse).
    connect(m_grid, &QTableWidget::itemChanged, this, [this]() { render(collect()); });
    gv->addWidget(m_grid);

    auto *toolRow = new QHBoxLayout;
    auto *insertButton = new QPushButton(tr("Insert"), valuesGroup);
    m_deleteButton = new QPushButton(tr("Delete"), valuesGroup);
    m_lineariseButton = new QPushButton(tr("Linearise"), valuesGroup);
    auto *generateButton = new QPushButton(tr("Generate…"), valuesGroup);
    insertButton->setToolTip(tr("Add a breakpoint after the last one."));
    m_deleteButton->setToolTip(tr("Remove the selected breakpoints."));
    m_lineariseButton->setToolTip(tr("Space the breakpoints evenly between the first and last."));
    generateButton->setToolTip(tr("Fill the axis with evenly spaced values from a range."));
    connect(insertButton, &QPushButton::clicked, this, &AxisSetupDialog::onInsert);
    connect(m_deleteButton, &QPushButton::clicked, this, &AxisSetupDialog::onDelete);
    connect(m_lineariseButton, &QPushButton::clicked, this, &AxisSetupDialog::onLinearise);
    connect(generateButton, &QPushButton::clicked, this, &AxisSetupDialog::onGenerate);
    toolRow->addWidget(insertButton);
    toolRow->addWidget(m_deleteButton);
    toolRow->addWidget(m_lineariseButton);
    toolRow->addWidget(generateButton);
    toolRow->addStretch(1);
    gv->addLayout(toolRow);
    layout->addWidget(valuesGroup);

    auto *help = new QLabel(
        tr("Breakpoints ascend left to right — they auto-sort, and duplicates are dropped. "
           "Copy/paste (Ctrl+C / Ctrl+V) moves values to and from a spreadsheet; Delete clears "
           "a cell."),
        this);
    help->setWordWrap(true);
    layout->addWidget(help);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &AxisSetupDialog::validateAndAccept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    render(axis.sites);
    refreshChannelUi();
    resize(qMax(560, axis.maxSites * 64 + 60), sizeHint().height());
}

AxisSetupDialog::~AxisSetupDialog()
{
    delete m_clip;
}

CellSpec AxisSetupDialog::cellSpec() const
{
    const Channel c = m_config->catalog().findByName(channelField(m_channelEdit));
    if (c.isValid())
        return {c.minValue, c.maxValue, qBound(0, c.decimalPlaces, 8), true};
    return {-1e9, 1e9, 3, true};
}

// Non-blank cells, ascending and de-duplicated — the canonical value list, and
// what column c holds after any render() (column c == value index c).
QList<double> AxisSetupDialog::collect() const
{
    QList<double> v;
    for (int c = 0; c < m_axis.maxSites; ++c)
        if (!cellBlank(m_grid, 0, c))
            v.append(cellValue(m_grid, 0, c));
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    return v;
}

void AxisSetupDialog::render(const QList<double> &values)
{
    const QSignalBlocker block(m_grid);
    const CellSpec s = cellSpec();
    for (int c = 0; c < m_axis.maxSites; ++c)
        m_grid->setItem(0, c, c < values.size() ? numItem(values.at(c), s) : blankItem());
}

void AxisSetupDialog::reformat()
{
    render(collect());
}

void AxisSetupDialog::refreshChannelUi()
{
    const Channel c = m_config->catalog().findByName(channelField(m_channelEdit));
    const QString unit = c.isValid() ? c.unit : QString();
    m_valuesLabel->setText(unit.isEmpty()
                               ? tr("Values (maximum %1)").arg(m_axis.maxSites)
                               : tr("Values: %1  (maximum %2)").arg(unit).arg(m_axis.maxSites));
    reformat(); // decimals/range may have changed with the channel
}

void AxisSetupDialog::onSelectChannel()
{
    const QString picked =
        SelectChannelDialog::pickInput(m_config, channelField(m_channelEdit), this, m_livePatch);
    if (!picked.isEmpty()) {
        setChannelField(m_channelEdit, picked, m_config->catalog());
        refreshChannelUi();
    }
}

void AxisSetupDialog::onInsert()
{
    QList<double> v = collect();
    if (v.size() >= m_axis.maxSites) {
        QMessageBox::information(this, windowTitle(),
                                 tr("This axis already has the maximum of %1 breakpoints.")
                                     .arg(m_axis.maxSites));
        return;
    }
    double step = v.size() >= 2 ? v.at(v.size() - 1) - v.at(v.size() - 2) : 1.0;
    if (step <= 0)
        step = 1.0;
    const CellSpec s = cellSpec();
    const double nv = qBound(s.lo, v.isEmpty() ? 0.0 : v.last() + step, s.hi);
    v.append(nv);
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    render(v);
}

void AxisSetupDialog::onDelete()
{
    QSet<int> drop;
    for (QTableWidgetItem *it : m_grid->selectedItems())
        drop.insert(it->column());
    if (drop.isEmpty())
        return;
    const QList<double> v = collect();
    QList<double> keep;
    for (int c = 0; c < v.size(); ++c)
        if (!drop.contains(c))
            keep.append(v.at(c));
    render(keep);
}

void AxisSetupDialog::onLinearise()
{
    const QList<double> v = collect();
    if (v.size() < 2) {
        QMessageBox::information(this, windowTitle(),
                                 tr("Enter at least two breakpoints, then Linearise spaces the "
                                    "ones between them evenly."));
        return;
    }
    const double first = v.first();
    const double last = v.last();
    const int n = v.size();
    const CellSpec s = cellSpec();
    QList<double> out;
    for (int i = 0; i < n; ++i)
        out.append(qBound(s.lo, first + (last - first) * i / (n - 1), s.hi));
    render(out);
}

void AxisSetupDialog::onGenerate()
{
    const CellSpec s = cellSpec();
    const QList<double> cur = collect();

    QDialog dlg(this);
    dlg.setWindowTitle(tr("Generate Axis Values"));
    auto *form = new QFormLayout(&dlg);
    auto *fromSpin = new TrimmedDoubleSpinBox(&dlg);
    auto *toSpin = new TrimmedDoubleSpinBox(&dlg);
    for (TrimmedDoubleSpinBox *sp : {fromSpin, toSpin}) {
        sp->setDecimals(s.decimals);
        sp->setRange(s.lo, s.hi);
        sp->setButtonSymbols(QAbstractSpinBox::NoButtons);
    }
    fromSpin->setValue(cur.isEmpty() ? qBound(s.lo, 0.0, s.hi) : cur.first());
    toSpin->setValue(cur.isEmpty() ? qBound(s.lo, 100.0, s.hi) : cur.last());
    auto *countSpin = new QSpinBox(&dlg);
    countSpin->setRange(2, m_axis.maxSites);
    countSpin->setValue(cur.size() >= 2 ? cur.size() : m_axis.maxSites);
    form->addRow(tr("From:"), fromSpin);
    form->addRow(tr("To:"), toSpin);
    form->addRow(tr("Count:"), countSpin);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    form->addRow(buttons);

    if (dlg.exec() != QDialog::Accepted)
        return;
    const double from = fromSpin->value();
    const double to = toSpin->value();
    if (to <= from) {
        QMessageBox::warning(this, windowTitle(), tr("\"To\" must be greater than \"From\"."));
        return;
    }
    const int n = countSpin->value();
    QList<double> out;
    for (int i = 0; i < n; ++i)
        out.append(qBound(s.lo, from + (to - from) * i / (n - 1), s.hi));
    render(out);
}

void AxisSetupDialog::validateAndAccept()
{
    const QList<double> v = collect();
    if (!v.isEmpty() && channelField(m_channelEdit).isEmpty()) {
        QMessageBox::warning(this, windowTitle(),
                             tr("Select an input channel for this axis, or clear its values."));
        return;
    }
    m_axis.sites = v;
    m_axis.channel = channelField(m_channelEdit);
    m_axis.interp = m_behaviour->currentIndex() == 0;
    accept();
}

bool AxisSetupDialog::eventFilter(QObject *obj, QEvent *event)
{
    if (obj == m_grid && event->type() == QEvent::KeyPress) {
        if (m_clip->handleKeyPress(static_cast<QKeyEvent *>(event)))
            return true;
    }
    return QDialog::eventFilter(obj, event);
}

} // namespace ct
