// Calculations > Tables — 2x16 (1-axis) and 8x8 (2-axis) lookup tables.
#include "tables_dialog.h"

#include <QButtonGroup>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
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
#include <QRadioButton>
#include <QScreen>
#include <QScrollArea>
#include <QSet>
#include <QSpinBox>
#include <QStyledItemDelegate>
#include <QTableWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>

#include "../model/channel_catalog.h"
#include "../protocol/wire_structs.h"
#include "axis_setup_dialog.h"
#include "channel_field.h"
#include "numeric_grid.h"
#include "select_channel_dialog.h"
#include "trimmed_spin_box.h"

namespace ct {

namespace {

// Storage data types offered for a table's output channel (mirrors Constants).
struct DataTypeInfo {
    const char *name;
    double rawMin;
    double rawMax;
    int maxDecimals;
    bool scalesWithResolution;
};
const DataTypeInfo kDataTypes[] = {
    {"boolean", 0.0, 1.0, 0, false},
    {"u8", 0.0, 255.0, 2, true},
    {"u16", 0.0, 65535.0, 4, true},
    {"u32", 0.0, 4294967295.0, 8, true},
    {"s8", -128.0, 127.0, 2, true},
    {"s16", -32768.0, 32767.0, 4, true},
    {"s32", -2147483648.0, 2147483647.0, 8, true},
    {"float", -1e9, 1e9, 8, false},
};
const DataTypeInfo *dataTypeInfo(const QString &name)
{
    for (const DataTypeInfo &t : kDataTypes)
        if (name == QLatin1String(t.name))
            return &t;
    return nullptr;
}
void outputRange(const QString &dataType, int decimals, double *lo, double *hi, double *res)
{
    const DataTypeInfo *t = dataTypeInfo(dataType);
    const double resolution = std::pow(10.0, -decimals);
    const double scale = (t && t->scalesWithResolution) ? resolution : 1.0;
    if (res)
        *res = resolution;
    *lo = t ? t->rawMin * scale : -1e9;
    *hi = t ? t->rawMax * scale : 1e9;
}

// The catalogue channel a table registers so it can be referenced elsewhere.
Channel channelForOutput(const QString &name, const QString &dataType, int decimals)
{
    Channel c;
    c.name = name;
    c.quantity = QStringLiteral("Unitless");
    c.unit.clear();
    c.dataType = dataType;
    c.decimalPlaces = decimals;
    double lo = 0, hi = 0, res = 1.0;
    outputRange(dataType, decimals, &lo, &hi, &res);
    c.baseResolution = res;
    c.minValue = lo;
    c.maxValue = hi;
    c.category = QStringLiteral("User Channels");
    c.userDefined = true;
    return c;
}

// A table axis input's range/decimals come from its channel definition; an
// unknown channel is permissive so editing isn't blocked.
CellSpec channelSpec(Configuration *config, const QString &name)
{
    const Channel c = config->catalog().findByName(name);
    if (c.isValid())
        return {c.minValue, c.maxValue, qBound(0, c.decimalPlaces, 8), true};
    return {-1e9, 1e9, 3, true};
}

// The output cells' range/decimals come from the table's own data type + dps.
CellSpec outputSpec(const QString &dataType, int decimals)
{
    double lo = 0, hi = 0, res = 1.0;
    outputRange(dataType, decimals, &lo, &hi, &res);
    return {lo, hi, decimals, true};
}

// Shared "output channel" group used by both editors (name / data type / decimals).
struct OutputControls {
    // The group itself comes back so the CALLER decides where it sits. It
    // used to add itself to the layout it was handed, which fixed it at
    // whatever point in construction the call happened to be - and that was
    // the top, before the sites the output is computed from.
    QGroupBox *group;
    QLineEdit *nameEdit;
    QComboBox *typeCombo;
    QSpinBox *decimalsSpin;
};
OutputControls buildOutputGroup(QWidget *parent, const QString &name,
                                const QString &dataType, int decimals)
{
    auto *group = new QGroupBox(QObject::tr("Output Channel"), parent);
    auto *form = new QFormLayout(group);
    OutputControls oc{};
    oc.group = group;
    oc.nameEdit = new QLineEdit(name, group);
    oc.nameEdit->setMaxLength(MAX_CHANNEL_NAME_BYTES); // device label budget
    oc.nameEdit->setToolTip(
        QObject::tr("Up to %1 characters — the device stores a %2-byte label.")
            .arg(MAX_CHANNEL_NAME_BYTES).arg(SIGNAL_LABEL_LEN));
    form->addRow(QObject::tr("Channel Name:"), oc.nameEdit);
    oc.typeCombo = new QComboBox(group);
    for (const DataTypeInfo &t : kDataTypes)
        oc.typeCombo->addItem(QLatin1String(t.name));
    oc.typeCombo->setCurrentIndex(qMax(0, oc.typeCombo->findText(dataType)));
    form->addRow(QObject::tr("Data Type:"), oc.typeCombo);
    oc.decimalsSpin = new QSpinBox(group);
    oc.decimalsSpin->setRange(0, 8);
    oc.decimalsSpin->setValue(decimals);
    form->addRow(QObject::tr("Decimal Places:"), oc.decimalsSpin);
    return oc;
}

// One axis: a channel picker + Interpolated / Discrete-centered radios, and
// (for the 8x8) an "Edit Axis…" button that opens the dedicated Axis Setup
// window. box/baseTitle are kept so the owner can retitle the group with the
// chosen channel — the label that tells the two axes apart.
struct AxisControls {
    QLineEdit *channelEdit = nullptr;
    QRadioButton *interpRadio = nullptr;
    QRadioButton *discreteRadio = nullptr;
    QGroupBox *box = nullptr;
    QString baseTitle;
    QPushButton *editButton = nullptr; // null unless withEditButton
};
// Retitle the group "X Axis — Manifold Pressure kPa" so which axis is which, and
// on what channel, is readable at a glance. Pure display: the identity comes out
// of channelField() and only the LABEL derived from it reaches the title, so the
// title can never be mistaken for the stored name.
void retitleAxisGroup(const AxisControls &ac, Configuration *config)
{
    if (!ac.box)
        return;
    const QString name = ac.channelEdit ? channelField(ac.channelEdit) : QString();
    ac.box->setTitle(name.isEmpty()
                         ? ac.baseTitle
                         : QStringLiteral("%1 — %2")
                               .arg(ac.baseTitle, config->catalog().labelFor(name)));
}
AxisControls buildAxisGroup(QDialog *parent, Configuration *config, QVBoxLayout *layout,
                            const QString &title, const QString &channel, bool interp,
                            const ConfigPatch &livePatch, std::function<void()> onChanged,
                            bool withEditButton = false)
{
    auto *group = new QGroupBox(title, parent);
    auto *v = new QVBoxLayout(group);
    auto *row = new QHBoxLayout;
    AxisControls ac{};
    ac.box = group;
    ac.baseTitle = title;
    // Displays "Coolant Temp °C", remembers "Coolant Temp": read it back with
    // channelField(), never with text().
    ac.channelEdit = new QLineEdit(group);
    ac.channelEdit->setReadOnly(true);
    setChannelField(ac.channelEdit, channel, config->catalog());
    auto *pick = new QPushButton(QObject::tr("Select…"), group);
    // An axis reads a channel and never writes it, so any channel can drive it.
    QObject::connect(pick, &QPushButton::clicked, parent,
                     [parent, config, ac, livePatch, onChanged]() {
        const QString picked = SelectChannelDialog::pickInput(config,
                                                              channelField(ac.channelEdit),
                                                              parent, livePatch);
        if (!picked.isEmpty()) {
            setChannelField(ac.channelEdit, picked, config->catalog());
            retitleAxisGroup(ac, config);
            if (onChanged)
                onChanged(); // axis channel changed -> re-derive its cell constraints
        }
    });
    row->addWidget(new QLabel(QObject::tr("Input:")));
    row->addWidget(ac.channelEdit, 1);
    row->addWidget(pick);
    if (withEditButton) {
        ac.editButton = new QPushButton(QObject::tr("Edit Axis…"), group);
        ac.editButton->setToolTip(
            QObject::tr("Define this axis's breakpoints in a dedicated window."));
        row->addWidget(ac.editButton);
    }
    v->addLayout(row);
    auto *modeRow = new QHBoxLayout;
    ac.interpRadio = new QRadioButton(QObject::tr("Interpolated"), group);
    ac.discreteRadio = new QRadioButton(QObject::tr("Discrete (centered)"), group);
    auto *bg = new QButtonGroup(parent);
    bg->addButton(ac.interpRadio);
    bg->addButton(ac.discreteRadio);
    ac.interpRadio->setChecked(interp);
    ac.discreteRadio->setChecked(!interp);
    modeRow->addWidget(ac.interpRadio);
    modeRow->addWidget(ac.discreteRadio);
    modeRow->addStretch();
    v->addLayout(modeRow);
    layout->addWidget(group);
    retitleAxisGroup(ac, config);
    return ac;
}

bool validateOutputName(QDialog *dlg, const QString &name, const QStringList &reserved,
                        const QStringList &siblingNames, const QString &axisA, const QString &axisB)
{
    if (name.isEmpty()) {
        QMessageBox::warning(dlg, dlg->windowTitle(),
                             QObject::tr("The output channel name must not be empty."));
        return false;
    }
    if (name.toUtf8().size() > MAX_CHANNEL_NAME_BYTES) {
        QMessageBox::warning(dlg, dlg->windowTitle(),
                             QObject::tr("Names are limited to %1 bytes on the device.")
                                 .arg(MAX_CHANNEL_NAME_BYTES));
        return false;
    }
    if (name.compare(axisA, Qt::CaseInsensitive) == 0
        || (!axisB.isEmpty() && name.compare(axisB, Qt::CaseInsensitive) == 0)) {
        QMessageBox::warning(dlg, dlg->windowTitle(),
                             QObject::tr("The output channel must differ from the axis input."));
        return false;
    }
    for (const QString &s : siblingNames)
        if (s.compare(name, Qt::CaseInsensitive) == 0) {
            QMessageBox::warning(dlg, dlg->windowTitle(),
                                 QObject::tr("A table already outputs to \"%1\".").arg(name));
            return false;
        }
    for (const QString &s : reserved)
        if (s.compare(name, Qt::CaseInsensitive) == 0) {
            QMessageBox::warning(dlg, dlg->windowTitle(),
                                 QObject::tr("A channel named \"%1\" already exists. Choose a "
                                             "different name.").arg(name));
            return false;
        }
    return true;
}

// ------------------------------------------------------------------- 2x16
class Table2x16Editor : public QDialog
{
public:
    Table2x16Editor(Configuration *config, const Table2x16Row &row, const QStringList &reserved,
                   const QStringList &siblingNames, const ConfigPatch &livePatch,
                   QWidget *parent)
        : QDialog(parent), m_config(config), m_row(row)
    {
        setWindowTitle(tr("2x16 Table"));
        setModal(true);
        m_reserved = reserved;
        m_siblings = siblingNames;

        auto *layout = new QVBoxLayout(this);
        m_out = buildOutputGroup(this, row.outputChannel, row.dataType, row.decimalPlaces);
        m_axis = buildAxisGroup(this, config, layout, tr("Input Axis"), row.xChannel, row.xInterp,
                                livePatch, [this]() { updateConstraints(); });

        auto *gridGroup = new QGroupBox(tr("Sites and Output Values"), this);
        auto *gv = new QVBoxLayout(gridGroup);
        m_grid = new QTableWidget(2, TABLE_2X16_SITES, gridGroup);
        m_grid->setVerticalHeaderLabels({tr("Axis"), tr("Output")});
        QStringList cols;
        for (int i = 0; i < TABLE_2X16_SITES; ++i)
            cols << QString::number(i + 1);
        m_grid->setHorizontalHeaderLabels(cols);
        // Fixed-width columns that scroll, NOT Stretch: 16 stretched columns in a
        // dialog this size would be ~28 px each — too narrow to read a value.
        m_grid->horizontalHeader()->setSectionResizeMode(QHeaderView::Fixed);
        m_grid->horizontalHeader()->setDefaultSectionSize(64);
        recomputeSpecs();
        for (int i = 0; i < TABLE_2X16_SITES; ++i) {
            m_grid->setItem(0, i, i < row.xSites.size() ? numItem(row.xSites.at(i), m_xSpec)
                                                        : blankItem());
            m_grid->setItem(1, i, i < row.outputs.size() ? numItem(row.outputs.at(i), m_outSpec)
                                                         : blankItem());
        }
        auto *delegate = new GridDelegate([this](int r, int c) { return specForCell(r, c); },
                                          m_grid);
        // Same band as the 8x8, one axis narrower: row 0 holds the sites, row 1
        // their outputs.
        delegate->setHeaderBand([](int r, int) { return r == 0; });
        m_grid->setItemDelegate(delegate);
        m_grid->installEventFilter(this); // clipboard + Delete, via m_clip
        m_clip = std::make_unique<GridClipboard>(
            m_grid, [this](int r, int c) { return specForCell(r, c); },
            [this]() { sortAxis(); updateConstraints(); });
        // Keep the axis ascending: re-sort (carrying each site's output) when an
        // axis cell is edited; a value duplicating an existing site is discarded.
        // Connected after populating so it doesn't self-fire.
        connect(m_grid, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *item) {
            if (item->row() != 0)
                return;
            const int c = item->column();
            if (!cellBlank(m_grid, 0, c) && axisHasDuplicate(c)) {
                const QSignalBlocker block(m_grid);
                item->setText(QString()); // that axis value already exists — clear it
                return;
            }
            sortAxis();
        });
        gv->addWidget(m_grid);
        auto *help = new QLabel(
            tr("Fill only the sites you need (left to right, up to %1) — they auto-sort "
               "ascending. Each site needs an output; Delete clears a cell. Values are "
               "limited to their channel's range/decimals.")
                .arg(TABLE_2X16_SITES));
        help->setWordWrap(true);
        gv->addWidget(help);
        layout->addWidget(gridGroup);

        // THE OUTPUT CHANNEL GOES LAST, matching the User Condition editor:
        // what the table produces reads as the conclusion of the sites above
        // it, not as a question asked before any of them are on screen.
        layout->addWidget(m_out.group);

        // Output type/decimals feed the output cells' constraints.
        connect(m_out.typeCombo, &QComboBox::currentTextChanged, this,
                [this]() { updateConstraints(); });
        connect(m_out.decimalsSpin, &QSpinBox::valueChanged, this,
                [this]() { updateConstraints(); });

        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        connect(buttons, &QDialogButtonBox::accepted, this, &Table2x16Editor::validateAndAccept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        layout->addWidget(buttons);
        // Wide enough for roughly 12 of the 16 columns; the grid scrolls
        // horizontally for the rest rather than forcing an oversized window.
        resize(880, 470);
    }

    Table2x16Row result() const { return m_row; }

private:
    CellSpec specForCell(int row, int /*col*/) const { return row == 0 ? m_xSpec : m_outSpec; }

    void recomputeSpecs()
    {
        // Cap the decimals control to what the output data type allows.
        const DataTypeInfo *ti = dataTypeInfo(m_out.typeCombo->currentText());
        const QSignalBlocker block(m_out.decimalsSpin);
        m_out.decimalsSpin->setRange(0, ti ? ti->maxDecimals : 8);
        m_xSpec = channelSpec(m_config, channelField(m_axis.channelEdit));
        m_outSpec = outputSpec(m_out.typeCombo->currentText(), m_out.decimalsSpin->value());
    }
    void updateConstraints()
    {
        recomputeSpecs();
        for (int c = 0; c < TABLE_2X16_SITES; ++c) {
            if (!cellBlank(m_grid, 0, c))
                m_grid->item(0, c)->setText(trimmedNumber(
                    qBound(m_xSpec.lo, cellValue(m_grid, 0, c), m_xSpec.hi), m_xSpec.decimals));
            if (!cellBlank(m_grid, 1, c))
                m_grid->item(1, c)->setText(trimmedNumber(
                    qBound(m_outSpec.lo, cellValue(m_grid, 1, c), m_outSpec.hi),
                    m_outSpec.decimals));
        }
    }

    bool eventFilter(QObject *obj, QEvent *event) override
    {
        if (obj == m_grid && event->type() == QEvent::KeyPress) {
            if (m_clip->handleKeyPress(static_cast<QKeyEvent *>(event)))
                return true;
        }
        return QDialog::eventFilter(obj, event);
    }

    // True if the (populated) axis cell at column c equals another populated
    // axis cell — the value has already been entered elsewhere on the axis.
    bool axisHasDuplicate(int c) const
    {
        const double v = cellValue(m_grid, 0, c);
        for (int o = 0; o < TABLE_2X16_SITES; ++o)
            if (o != c && !cellBlank(m_grid, 0, o) && cellValue(m_grid, 0, o) == v)
                return true;
        return false;
    }

    // Sort the populated (contiguous) axis sites ascending, moving each site's
    // output with it. Only the leading run is touched (a gap is left for accept
    // to flag). Blocking signals avoids re-entering itemChanged during rewrite.
    void sortAxis()
    {
        int n = 0;
        while (n < TABLE_2X16_SITES && !cellBlank(m_grid, 0, n))
            ++n;
        if (n < 2)
            return;
        struct Site {
            double key;
            QString ax, out;
        };
        QList<Site> sites;
        for (int c = 0; c < n; ++c)
            sites.append({cellValue(m_grid, 0, c), m_grid->item(0, c)->text(),
                          m_grid->item(1, c) ? m_grid->item(1, c)->text() : QString()});
        std::stable_sort(sites.begin(), sites.end(),
                         [](const Site &a, const Site &b) { return a.key < b.key; });
        const QSignalBlocker block(m_grid);
        for (int c = 0; c < n; ++c) {
            m_grid->item(0, c)->setText(sites[c].ax);
            m_grid->item(1, c)->setText(sites[c].out);
        }
    }

    void validateAndAccept()
    {
        const QString name = m_out.nameEdit->text().trimmed();
        if (!validateOutputName(this, name, m_reserved, m_siblings,
                                channelField(m_axis.channelEdit), QString()))
            return;
        // Populated sites are contiguous from the left; count them.
        int n = 0;
        while (n < TABLE_2X16_SITES && !cellBlank(m_grid, 0, n))
            ++n;
        for (int i = n; i < TABLE_2X16_SITES; ++i)
            if (!cellBlank(m_grid, 0, i)) {
                QMessageBox::warning(this, windowTitle(),
                                     tr("Fill axis sites contiguously from the left (site %1 is "
                                        "empty but site %2 is filled).").arg(n + 1).arg(i + 1));
                return;
            }
        if (n >= 1 && channelField(m_axis.channelEdit).isEmpty()) {
            QMessageBox::warning(this, windowTitle(), tr("Select an input axis channel."));
            return;
        }
        for (int i = 0; i < n; ++i)
            if (cellBlank(m_grid, 1, i)) {
                QMessageBox::warning(this, windowTitle(),
                                     tr("Site %1 has no output value.").arg(i + 1));
                return;
            }
        for (int i = 0; i + 1 < n; ++i)
            if (cellValue(m_grid, 0, i + 1) <= cellValue(m_grid, 0, i)) {
                QMessageBox::warning(this, windowTitle(),
                                     tr("Axis sites must strictly ascend (site %1 <= site %2).")
                                         .arg(i + 2).arg(i + 1));
                return;
            }
        m_row.outputChannel = name;
        m_row.dataType = m_out.typeCombo->currentText();
        m_row.decimalPlaces = m_out.decimalsSpin->value();
        m_row.xChannel = channelField(m_axis.channelEdit);
        m_row.xInterp = m_axis.interpRadio->isChecked();
        m_row.xSites.clear();
        m_row.outputs.clear();
        for (int i = 0; i < n; ++i) {
            m_row.xSites.append(cellValue(m_grid, 0, i));
            m_row.outputs.append(cellValue(m_grid, 1, i));
        }
        accept();
    }

    Configuration *m_config;
    Table2x16Row m_row;
    QStringList m_reserved, m_siblings;
    OutputControls m_out{};
    AxisControls m_axis{};
    QTableWidget *m_grid = nullptr;
    std::unique_ptr<GridClipboard> m_clip;
    CellSpec m_xSpec, m_outSpec;
};

// -------------------------------------------------------------------- 8x8
class Table8x8Editor : public QDialog
{
public:
    Table8x8Editor(Configuration *config, const Table8x8Row &row, const QStringList &reserved,
                   const QStringList &siblingNames, const ConfigPatch &livePatch,
                   QWidget *parent)
        : QDialog(parent), m_config(config), m_row(row)
    {
        setWindowTitle(tr("8x8 Table"));
        setModal(true);
        m_reserved = reserved;
        m_siblings = siblingNames;

        // The body scrolls and the button box stays outside it, for the same
        // reason the Condition editor is built that way: an output group, two
        // axis groups and a kGridDim-row grid want roughly 780 px of height,
        // which fits a 1080p desktop at 100% but NOT at 150% scaling, where
        // only ~720 logical px are usable. Letting the window grow instead
        // would push OK off the bottom of the screen with no way to reach it.
        auto *content = new QWidget;
        auto *layout = new QVBoxLayout(content);
        m_out = buildOutputGroup(this, row.outputChannel, row.dataType, row.decimalPlaces);
        m_xAxis = buildAxisGroup(this, config, layout, tr("X Axis"), row.xChannel, row.xInterp,
                                 livePatch, [this]() { updateConstraints(); }, true);
        m_yAxis = buildAxisGroup(this, config, layout, tr("Y Axis"), row.yChannel, row.yInterp,
                                 livePatch, [this]() { updateConstraints(); }, true);
        // "Edit Axis…" opens the dedicated Axis Setup window for that axis; its
        // result is written back into the grid's site band. Captures livePatch
        // (by value) so the picker inside the axis window sees this session's
        // half-edited channels, exactly as the inline picker does.
        connect(m_xAxis.editButton, &QPushButton::clicked, this,
                [this, livePatch]() { openAxisSetup(true, livePatch); });
        connect(m_yAxis.editButton, &QPushButton::clicked, this,
                [this, livePatch]() { openAxisSetup(false, livePatch); });

        auto *gridGroup = new QGroupBox(tr("Sites and Output Grid"), this);
        auto *gv = new QVBoxLayout(gridGroup);
        // kGridDim square (9x9): corner + X sites across the top row, Y sites
        // down the left column, and the inner TABLE_8X8_SITES square holding
        // the outputs. The document model stores those row-major over the
        // FILLED X width — outputs[y*nx + x], not a fixed stride of 8 — which
        // is what validateAndAccept() writes and the loader below reads.
        m_grid = new QTableWidget(kGridDim, kGridDim, gridGroup);
        m_grid->horizontalHeader()->setVisible(false);
        m_grid->verticalHeader()->setVisible(false);
        // Fixed-width columns that scroll, NOT Stretch — the same call the 2x16
        // grid makes, and now for the same reason: nine stretched columns in a
        // dialog this size would be ~60 px each, too narrow for a value carrying
        // the 8 decimal places the output type allows. At 64 px the nine columns
        // come to 576, which the 700 px window below shows without scrolling;
        // narrow the window and the grid scrolls rather than crushing its cells.
        m_grid->horizontalHeader()->setSectionResizeMode(QHeaderView::Fixed);
        m_grid->horizontalHeader()->setDefaultSectionSize(64);
        // QTableWidget's sizeHint is a fixed 256x192 whatever it contains, so
        // the height that shows all nine rows has to be asked for outright.
        // Computed rather than written as a literal: the row height is the
        // platform's, and at 150% scaling a hardcoded figure clips the bottom
        // row. Anything the window cannot fit is taken by the scroll area.
        m_grid->setMinimumHeight(kGridDim * m_grid->verticalHeader()->defaultSectionSize()
                                 + 2 * m_grid->frameWidth() + 2);
        recomputeSpecs();
        // Corner spells out the grid's orientation: X sites run across the top
        // row, Y sites down the left column.
        auto *corner = new QTableWidgetItem(tr("Y ↓ \\ X →"));
        corner->setFlags(Qt::ItemIsEnabled); // non-editable label
        corner->setToolTip(tr("X sites run across the top; Y sites down the left."));
        m_grid->setItem(0, 0, corner);
        const int xs = int(row.xSites.size());
        const int ys = int(row.ySites.size());
        for (int x = 0; x < TABLE_8X8_SITES; ++x)
            m_grid->setItem(0, x + 1, x < xs ? numItem(row.xSites.at(x), m_xSpec) : blankItem());
        for (int y = 0; y < TABLE_8X8_SITES; ++y)
            m_grid->setItem(y + 1, 0, y < ys ? numItem(row.ySites.at(y), m_ySpec) : blankItem());
        for (int y = 0; y < TABLE_8X8_SITES; ++y)
            for (int x = 0; x < TABLE_8X8_SITES; ++x) {
                const bool used = x < xs && y < ys;
                m_grid->setItem(y + 1, x + 1,
                                used ? numItem(row.outputs.value(y * xs + x, 0.0), m_outSpec)
                                     : blankItem());
            }
        auto *delegate = new GridDelegate([this](int r, int c) { return specForCell(r, c); },
                                          m_grid);
        // Tint the site band — top row (X), left column (Y) and the corner — so
        // the axis inputs read apart from the output cells they index.
        delegate->setHeaderBand([](int r, int c) { return r == 0 || c == 0; });
        m_grid->setItemDelegate(delegate);
        m_grid->installEventFilter(this); // clipboard + Delete, via m_clip
        m_clip = std::make_unique<GridClipboard>(
            m_grid, [this](int r, int c) { return specForCell(r, c); }, [this]() {
                sortXAxis();
                sortYAxis();
                updateConstraints();
            });
        // Keep axes ascending: editing an X site re-sorts columns, a Y site
        // re-sorts rows (each carries its output cells). A value duplicating an
        // existing site on that axis is discarded. Connected post-populate.
        connect(m_grid, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *item) {
            if (item->row() == 0 && item->column() >= 1) {
                if (!cellBlank(m_grid, 0, item->column()) && xAxisHasDuplicate(item->column())) {
                    const QSignalBlocker block(m_grid);
                    item->setText(QString());
                    return;
                }
                sortXAxis();
            } else if (item->column() == 0 && item->row() >= 1) {
                if (!cellBlank(m_grid, item->row(), 0) && yAxisHasDuplicate(item->row())) {
                    const QSignalBlocker block(m_grid);
                    item->setText(QString());
                    return;
                }
                sortYAxis();
            }
        });
        gv->addWidget(m_grid);
        auto *help = new QLabel(
            tr("Fill only the X/Y sites you need (up to %1 per axis) — they auto-sort "
               "ascending. Every cell in the filled grid needs an output; Delete clears a "
               "cell. Values are limited to their channel's range/decimals.")
                .arg(TABLE_8X8_SITES));
        help->setWordWrap(true);
        gv->addWidget(help);
        layout->addWidget(gridGroup);

        // THE OUTPUT CHANNEL GOES LAST, matching the User Condition editor:
        // what the table produces reads as the conclusion of the sites above
        // it, not as a question asked before any of them are on screen.
        layout->addWidget(m_out.group);

        connect(m_out.typeCombo, &QComboBox::currentTextChanged, this,
                [this]() { updateConstraints(); });
        connect(m_out.decimalsSpin, &QSpinBox::valueChanged, this,
                [this]() { updateConstraints(); });

        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        connect(buttons, &QDialogButtonBox::accepted, this, &Table8x8Editor::validateAndAccept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

        auto *scroll = new QScrollArea(this);
        scroll->setWidget(content);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        // Off, so the grid owns the horizontal axis: a narrow window scrolls the
        // grid's own columns, which keeps the axis pickers and the help text at
        // full width instead of sliding them sideways with it.
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        auto *outer = new QVBoxLayout(this);
        outer->addWidget(scroll, 1);
        outer->addWidget(buttons);

        // Ask for the height the body wants, capped at what the desktop can
        // show; past that the scroll area takes over. 700 wide covers the
        // 9 x 64 px grid (576) plus the group box and layout margins.
        const QScreen *scr = screen();
        const int available = scr ? scr->availableGeometry().height() : 1040;
        const int wanted = content->sizeHint().height() + buttons->sizeHint().height() + 24;
        resize(700, qMin(wanted, available - 80));
    }

    Table8x8Row result() const { return m_row; }

private:
    // The grid carries one extra row and column: the top row holds the X sites,
    // the left column the Y sites, and (0,0) is a label. TABLE_8X8_SITES + 1 = 9.
    static constexpr int kGridDim = TABLE_8X8_SITES + 1;

    // (0,0) corner is a non-editable label; top row = X sites, left col = Y sites,
    // the inner TABLE_8X8_SITES square = outputs.
    CellSpec specForCell(int row, int col) const
    {
        if (row == 0 && col == 0)
            return {0, 0, 0, false};
        if (row == 0)
            return m_xSpec;
        if (col == 0)
            return m_ySpec;
        return m_outSpec;
    }
    void recomputeSpecs()
    {
        const DataTypeInfo *ti = dataTypeInfo(m_out.typeCombo->currentText());
        const QSignalBlocker block(m_out.decimalsSpin);
        m_out.decimalsSpin->setRange(0, ti ? ti->maxDecimals : 8);
        m_xSpec = channelSpec(m_config, channelField(m_xAxis.channelEdit));
        m_ySpec = channelSpec(m_config, channelField(m_yAxis.channelEdit));
        m_outSpec = outputSpec(m_out.typeCombo->currentText(), m_out.decimalsSpin->value());
    }
    void updateConstraints()
    {
        recomputeSpecs();
        const auto reformat = [&](int r, int c, const CellSpec &s) {
            if (!cellBlank(m_grid, r, c))
                m_grid->item(r, c)->setText(
                    trimmedNumber(qBound(s.lo, cellValue(m_grid, r, c), s.hi), s.decimals));
        };
        for (int x = 0; x < TABLE_8X8_SITES; ++x)
            reformat(0, x + 1, m_xSpec);
        for (int y = 0; y < TABLE_8X8_SITES; ++y)
            reformat(y + 1, 0, m_ySpec);
        for (int y = 0; y < TABLE_8X8_SITES; ++y)
            for (int x = 0; x < TABLE_8X8_SITES; ++x)
                reformat(y + 1, x + 1, m_outSpec);
    }

    bool eventFilter(QObject *obj, QEvent *event) override
    {
        if (obj == m_grid && event->type() == QEvent::KeyPress) {
            if (m_clip->handleKeyPress(static_cast<QKeyEvent *>(event)))
                return true;
        }
        return QDialog::eventFilter(obj, event);
    }

    // Populated (contiguous) X/Y site counts.
    int filledX() const
    {
        int n = 0;
        while (n < TABLE_8X8_SITES && !cellBlank(m_grid, 0, n + 1))
            ++n;
        return n;
    }
    int filledY() const
    {
        int n = 0;
        while (n < TABLE_8X8_SITES && !cellBlank(m_grid, n + 1, 0))
            ++n;
        return n;
    }
    // Duplicate detection per axis: an X site (col c) or Y site (row r) whose
    // value already exists elsewhere on that axis. The sweep runs over the whole
    // axis band (1..TABLE_8X8_SITES), not the filled run — a site typed into a
    // column past the current run still has to be caught.
    bool xAxisHasDuplicate(int c) const
    {
        const double v = cellValue(m_grid, 0, c);
        for (int o = 1; o <= TABLE_8X8_SITES; ++o)
            if (o != c && !cellBlank(m_grid, 0, o) && cellValue(m_grid, 0, o) == v)
                return true;
        return false;
    }
    bool yAxisHasDuplicate(int r) const
    {
        const double v = cellValue(m_grid, r, 0);
        for (int o = 1; o <= TABLE_8X8_SITES; ++o)
            if (o != r && !cellBlank(m_grid, o, 0) && cellValue(m_grid, o, 0) == v)
                return true;
        return false;
    }

    // Sort X sites ascending, permuting the output columns to match.
    //
    // Which columns take part is the filled axis run (nx), but each one carries
    // its FULL TABLE_8X8_SITES-cell output column — not just the currently-filled
    // rows. The grid lets outputs be typed before the sites, so a cell can
    // legitimately sit outside today's filled rectangle; bounding the carry by
    // filledY() would leave those cells behind and silently bind them to the
    // wrong site (with filledY() == 0 the permutation would be skipped entirely,
    // sorting the axis header while every output stayed put). All
    // kGridDim*kGridDim = 81 cells exist from construction, so the full band is
    // always safe to index.
    void sortXAxis()
    {
        const int nx = filledX();
        if (nx < 2)
            return;
        struct Col {
            double key;
            QString site;
            QStringList outs;
        };
        QList<Col> cols;
        for (int x = 0; x < nx; ++x) {
            Col col{cellValue(m_grid, 0, x + 1), m_grid->item(0, x + 1)->text(), {}};
            for (int y = 0; y < TABLE_8X8_SITES; ++y)
                col.outs << m_grid->item(y + 1, x + 1)->text();
            cols.append(col);
        }
        std::stable_sort(cols.begin(), cols.end(),
                         [](const Col &a, const Col &b) { return a.key < b.key; });
        const QSignalBlocker block(m_grid);
        for (int x = 0; x < nx; ++x) {
            m_grid->item(0, x + 1)->setText(cols[x].site);
            for (int y = 0; y < TABLE_8X8_SITES; ++y)
                m_grid->item(y + 1, x + 1)->setText(cols[x].outs.value(y));
        }
    }
    // Sort Y sites ascending, permuting the output rows to match. Carries the
    // full TABLE_8X8_SITES-cell output row for the same reason as sortXAxis.
    void sortYAxis()
    {
        const int ny = filledY();
        if (ny < 2)
            return;
        struct Row {
            double key;
            QString site;
            QStringList outs;
        };
        QList<Row> rows;
        for (int y = 0; y < ny; ++y) {
            Row row{cellValue(m_grid, y + 1, 0), m_grid->item(y + 1, 0)->text(), {}};
            for (int x = 0; x < TABLE_8X8_SITES; ++x)
                row.outs << m_grid->item(y + 1, x + 1)->text();
            rows.append(row);
        }
        std::stable_sort(rows.begin(), rows.end(),
                         [](const Row &a, const Row &b) { return a.key < b.key; });
        const QSignalBlocker block(m_grid);
        for (int y = 0; y < ny; ++y) {
            m_grid->item(y + 1, 0)->setText(rows[y].site);
            for (int x = 0; x < TABLE_8X8_SITES; ++x)
                m_grid->item(y + 1, x + 1)->setText(rows[y].outs.value(x));
        }
    }

    // Open the Axis Setup window for one axis, seeded from the grid's current
    // site band; write the result back on OK.
    void openAxisSetup(bool isX, const ConfigPatch &livePatch)
    {
        const AxisControls &ac = isX ? m_xAxis : m_yAxis;
        AxisSetupDialog::Axis a;
        a.title = isX ? tr("X Axis") : tr("Y Axis");
        a.channel = channelField(ac.channelEdit);
        a.interp = ac.interpRadio->isChecked();
        a.maxSites = TABLE_8X8_SITES;
        for (int i = 0; i < TABLE_8X8_SITES; ++i) {
            const int r = isX ? 0 : i + 1;
            const int c = isX ? i + 1 : 0;
            if (!cellBlank(m_grid, r, c))
                a.sites.append(cellValue(m_grid, r, c));
        }
        AxisSetupDialog dlg(m_config, a, livePatch, this);
        if (dlg.exec() == QDialog::Accepted)
            applyAxis(isX, dlg.result());
    }

    void applyAxis(bool isX, const AxisSetupDialog::Axis &a)
    {
        AxisControls &ac = isX ? m_xAxis : m_yAxis;
        setChannelField(ac.channelEdit, a.channel, m_config->catalog());
        ac.interpRadio->setChecked(a.interp);
        ac.discreteRadio->setChecked(!a.interp);
        retitleAxisGroup(ac, m_config);
        const QSignalBlocker block(m_grid);
        recomputeSpecs();
        const CellSpec s = isX ? m_xSpec : m_ySpec;
        const int n = int(a.sites.size());
        for (int i = 0; i < TABLE_8X8_SITES; ++i) {
            const int r = isX ? 0 : i + 1;
            const int c = isX ? i + 1 : 0;
            m_grid->setItem(r, c, i < n ? numItem(a.sites.at(i), s) : blankItem());
        }
        // Outputs are bound to grid POSITION, so shrinking an axis orphans the
        // trailing rows/cols — clear them, leaving only cells that will be saved.
        if (isX) {
            for (int y = 0; y < TABLE_8X8_SITES; ++y)
                for (int x = n; x < TABLE_8X8_SITES; ++x)
                    m_grid->setItem(y + 1, x + 1, blankItem());
        } else {
            for (int y = n; y < TABLE_8X8_SITES; ++y)
                for (int x = 0; x < TABLE_8X8_SITES; ++x)
                    m_grid->setItem(y + 1, x + 1, blankItem());
        }
        updateConstraints();
    }

    void validateAndAccept()
    {
        const QString name = m_out.nameEdit->text().trimmed();
        if (!validateOutputName(this, name, m_reserved, m_siblings,
                                channelField(m_xAxis.channelEdit),
                                channelField(m_yAxis.channelEdit)))
            return;
        // Contiguous populated X sites (top row) and Y sites (left column).
        const int nx = filledX();
        for (int x = nx; x < TABLE_8X8_SITES; ++x)
            if (!cellBlank(m_grid, 0, x + 1)) {
                QMessageBox::warning(this, windowTitle(),
                                     tr("Fill X sites contiguously from the left."));
                return;
            }
        const int ny = filledY();
        for (int y = ny; y < TABLE_8X8_SITES; ++y)
            if (!cellBlank(m_grid, y + 1, 0)) {
                QMessageBox::warning(this, windowTitle(),
                                     tr("Fill Y sites contiguously from the top."));
                return;
            }
        if ((nx >= 1 || ny >= 1)
            && (channelField(m_xAxis.channelEdit).isEmpty()
                || channelField(m_yAxis.channelEdit).isEmpty())) {
            QMessageBox::warning(this, windowTitle(), tr("Select both X and Y axis channels."));
            return;
        }
        // Every cell in the filled nx*ny grid needs an output.
        for (int y = 0; y < ny; ++y)
            for (int x = 0; x < nx; ++x)
                if (cellBlank(m_grid, y + 1, x + 1)) {
                    QMessageBox::warning(this, windowTitle(),
                                         tr("The cell at X site %1, Y site %2 has no output value.")
                                             .arg(x + 1).arg(y + 1));
                    return;
                }
        for (int i = 0; i + 1 < nx; ++i)
            if (cellValue(m_grid, 0, i + 2) <= cellValue(m_grid, 0, i + 1)) {
                QMessageBox::warning(this, windowTitle(), tr("X axis sites must strictly ascend."));
                return;
            }
        for (int i = 0; i + 1 < ny; ++i)
            if (cellValue(m_grid, i + 2, 0) <= cellValue(m_grid, i + 1, 0)) {
                QMessageBox::warning(this, windowTitle(), tr("Y axis sites must strictly ascend."));
                return;
            }
        m_row.outputChannel = name;
        m_row.dataType = m_out.typeCombo->currentText();
        m_row.decimalPlaces = m_out.decimalsSpin->value();
        m_row.xChannel = channelField(m_xAxis.channelEdit);
        m_row.yChannel = channelField(m_yAxis.channelEdit);
        m_row.xInterp = m_xAxis.interpRadio->isChecked();
        m_row.yInterp = m_yAxis.interpRadio->isChecked();
        m_row.xSites.clear();
        m_row.ySites.clear();
        m_row.outputs.clear();
        for (int x = 0; x < nx; ++x)
            m_row.xSites.append(cellValue(m_grid, 0, x + 1));
        for (int y = 0; y < ny; ++y)
            m_row.ySites.append(cellValue(m_grid, y + 1, 0));
        // Row-major over the X width (nx): outputs[y*nx + x].
        for (int y = 0; y < ny; ++y)
            for (int x = 0; x < nx; ++x)
                m_row.outputs.append(cellValue(m_grid, y + 1, x + 1));
        accept();
    }

    Configuration *m_config;
    Table8x8Row m_row;
    QStringList m_reserved, m_siblings;
    OutputControls m_out{};
    AxisControls m_xAxis{};
    AxisControls m_yAxis{};
    QTableWidget *m_grid = nullptr;
    std::unique_ptr<GridClipboard> m_clip;
    CellSpec m_xSpec, m_ySpec, m_outSpec;
};

} // namespace

// -------------------------------------------------------------------------
TablesDialog::TablesDialog(Configuration *config, QWidget *parent)
    : QDialog(parent), m_config(config), m_rows2x16(config->table2x16Rows),
      m_rows8x8(config->table8x8Rows)
{
    setWindowTitle(tr("Tables"));
    setModal(true);
    resize(660, 400);

    for (const Table2x16Row &t : m_rows2x16)
        m_orig2x16 << t.outputChannel;
    for (const Table8x8Row &t : m_rows8x8)
        m_orig8x8 << t.outputChannel;

    auto *mainLayout = new QVBoxLayout(this);
    auto *topLayout = new QHBoxLayout;

    m_tree = new QTreeWidget(this);
    m_tree->setColumnCount(5);
    m_tree->setHeaderLabels({tr("#"), tr("Type"), tr("Output"), tr("Axes"), tr("Active")});
    m_tree->setRootIsDecorated(false);
    m_tree->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->header()->setStretchLastSection(true);
    m_tree->setColumnWidth(0, 36);
    m_tree->setColumnWidth(1, 56);
    m_tree->setColumnWidth(2, 180);
    m_tree->setColumnWidth(3, 220);
    topLayout->addWidget(m_tree, 1);

    auto *buttonColumn = new QVBoxLayout;
    m_add2x16Button = new QPushButton(tr("Add 2x16…"), this);
    m_add8x8Button = new QPushButton(tr("Add 8x8…"), this);
    m_changeButton = new QPushButton(tr("Change…"), this);
    m_removeButton = new QPushButton(tr("Remove"), this);
    buttonColumn->addWidget(m_add2x16Button);
    buttonColumn->addWidget(m_add8x8Button);
    buttonColumn->addWidget(m_changeButton);
    buttonColumn->addWidget(m_removeButton);
    buttonColumn->addStretch(1);
    topLayout->addLayout(buttonColumn);
    mainLayout->addLayout(topLayout, 1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    mainLayout->addWidget(buttons);

    connect(m_add2x16Button, &QPushButton::clicked, this, &TablesDialog::onAdd2x16);
    connect(m_add8x8Button, &QPushButton::clicked, this, &TablesDialog::onAdd8x8);
    connect(m_changeButton, &QPushButton::clicked, this, &TablesDialog::onChange);
    connect(m_removeButton, &QPushButton::clicked, this, &TablesDialog::onRemove);
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, [this]() { onChange(); });
    connect(m_tree, &QTreeWidget::itemSelectionChanged, this, &TablesDialog::updateButtons);
    connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
        commit();
        QDialog::accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    // A rename can arrive from an axis picker while these working copies are
    // open — see Configuration::channelRenamed. The rename-tracking names
    // follow too, so commit() compares each edited output against the name the
    // DOCUMENT now uses — otherwise it would "rename" from a name that no
    // longer exists and leave the renamed channel behind in the catalog.
    m_renameConnection = connect(
        m_config, &Configuration::channelRenamed, this,
        [this](const QString &oldName, const QString &newName) {
            renameChannelRefs(m_rows2x16, oldName, newName);
            renameChannelRefs(m_rows8x8, oldName, newName);
            for (QString &n : m_orig2x16)
                if (n.compare(oldName, Qt::CaseInsensitive) == 0)
                    n = newName;
            for (QString &n : m_orig8x8)
                if (n.compare(oldName, Qt::CaseInsensitive) == 0)
                    n = newName;
            rebuild();
        });

    rebuild();
    updateButtons();
}

// Tree rows: all 2x16 first, then all 8x8. UserRole holds is8x8 (bool); UserRole+1
// holds the index within that list.
//
// The Output/Axes columns are DISPLAY ONLY — every row that is acted on
// (onChange, onRemove) is located through those two UserRole values, never by
// reading a column back — so the channel names in them carry their unit.
void TablesDialog::rebuild()
{
    const int sel = m_tree->currentItem() ? m_tree->indexOfTopLevelItem(m_tree->currentItem()) : -1;
    m_tree->clear();
    const ChannelCatalog &cat = m_config->catalog();
    int n = 0;
    for (int i = 0; i < m_rows2x16.size(); ++i, ++n) {
        const Table2x16Row &t = m_rows2x16[i];
        auto *item = new QTreeWidgetItem(m_tree);
        item->setText(0, QString::number(n + 1));
        item->setText(1, tr("2x16"));
        item->setText(2, cat.labelFor(t.outputChannel));
        item->setText(3, cat.labelFor(t.xChannel));
        item->setText(4, t.active ? tr("Yes") : tr("No"));
        item->setData(0, Qt::UserRole, false);
        item->setData(0, Qt::UserRole + 1, i);
    }
    for (int i = 0; i < m_rows8x8.size(); ++i, ++n) {
        const Table8x8Row &t = m_rows8x8[i];
        auto *item = new QTreeWidgetItem(m_tree);
        item->setText(0, QString::number(n + 1));
        item->setText(1, tr("8x8"));
        item->setText(2, cat.labelFor(t.outputChannel));
        item->setText(3, QStringLiteral("%1 / %2").arg(cat.labelFor(t.xChannel),
                                                       cat.labelFor(t.yChannel)));
        item->setText(4, t.active ? tr("Yes") : tr("No"));
        item->setData(0, Qt::UserRole, true);
        item->setData(0, Qt::UserRole + 1, i);
    }
    if (sel >= 0 && sel < m_tree->topLevelItemCount())
        m_tree->setCurrentItem(m_tree->topLevelItem(sel));
    else if (m_tree->topLevelItemCount() > 0)
        m_tree->setCurrentItem(m_tree->topLevelItem(m_tree->topLevelItemCount() - 1));
}

// All output names except those owned by tables (a table output must not collide
// with an unrelated channel — it would overwrite that channel's definition).
QStringList TablesDialog::reservedNames() const
{
    QSet<QString> own;
    for (const Table2x16Row &t : m_rows2x16)
        own.insert(t.outputChannel.toLower());
    for (const Table8x8Row &t : m_rows8x8)
        own.insert(t.outputChannel.toLower());
    for (const QString &n : m_orig2x16)
        if (!n.isEmpty())
            own.insert(n.toLower());
    for (const QString &n : m_orig8x8)
        if (!n.isEmpty())
            own.insert(n.toLower());
    QStringList reserved;
    for (const Channel &c : m_config->catalog().userChannels())
        if (!own.contains(c.name.toLower()))
            reserved << c.name;
    return reserved;
}

// All other tables' output names (both types), excluding one being edited.
QStringList TablesDialog::siblingOutputs(bool is8x8, int exceptIdx) const
{
    QStringList names;
    for (int i = 0; i < m_rows2x16.size(); ++i)
        if (!(!is8x8 && i == exceptIdx))
            names << m_rows2x16[i].outputChannel;
    for (int i = 0; i < m_rows8x8.size(); ++i)
        if (!(is8x8 && i == exceptIdx))
            names << m_rows8x8[i].outputChannel;
    return names;
}

ConfigPatch TablesDialog::liveView() const
{
    // By value: the table editor holds the patch for as long as it is open.
    return [rows2x16 = m_rows2x16, rows8x8 = m_rows8x8](Configuration &c) {
        c.table2x16Rows = rows2x16;
        c.table8x8Rows = rows8x8;
    };
}

void TablesDialog::onAdd2x16()
{
    if (m_rows2x16.size() >= MAX_TABLES_2X16) {
        QMessageBox::warning(this, windowTitle(),
                             tr("The device supports at most %1 2x16 tables.").arg(MAX_TABLES_2X16));
        return;
    }
    const QStringList siblings = siblingOutputs(false, -1);
    Table2x16Row fresh;
    fresh.outputChannel = tr("Table %1").arg(m_rows2x16.size() + 1);
    Table2x16Editor editor(m_config, fresh, reservedNames(), siblings, liveView(), this);
    if (editor.exec() == QDialog::Accepted) {
        m_rows2x16.append(editor.result());
        m_orig2x16.append(QString());
        rebuild();
        updateButtons();
    }
}

void TablesDialog::onAdd8x8()
{
    if (m_rows8x8.size() >= MAX_TABLES_8X8) {
        QMessageBox::warning(this, windowTitle(),
                             tr("The device supports at most %1 8x8 tables.").arg(MAX_TABLES_8X8));
        return;
    }
    const QStringList siblings = siblingOutputs(true, -1);
    Table8x8Row fresh;
    fresh.outputChannel = tr("Table %1").arg(m_rows8x8.size() + 1);
    Table8x8Editor editor(m_config, fresh, reservedNames(), siblings, liveView(), this);
    if (editor.exec() == QDialog::Accepted) {
        m_rows8x8.append(editor.result());
        m_orig8x8.append(QString());
        rebuild();
        updateButtons();
    }
}

void TablesDialog::onChange()
{
    QTreeWidgetItem *item = m_tree->currentItem();
    if (!item)
        return;
    const bool is8x8 = item->data(0, Qt::UserRole).toBool();
    const int idx = item->data(0, Qt::UserRole + 1).toInt();
    if (is8x8) {
        if (idx < 0 || idx >= m_rows8x8.size())
            return;
        const QStringList siblings = siblingOutputs(true, idx);
        Table8x8Editor editor(m_config, m_rows8x8[idx], reservedNames(), siblings, liveView(),
                              this);
        if (editor.exec() == QDialog::Accepted) {
            m_rows8x8[idx] = editor.result();
            rebuild();
            updateButtons();
        }
    } else {
        if (idx < 0 || idx >= m_rows2x16.size())
            return;
        const QStringList siblings = siblingOutputs(false, idx);
        Table2x16Editor editor(m_config, m_rows2x16[idx], reservedNames(), siblings, liveView(),
                               this);
        if (editor.exec() == QDialog::Accepted) {
            m_rows2x16[idx] = editor.result();
            rebuild();
            updateButtons();
        }
    }
}

void TablesDialog::onRemove()
{
    QTreeWidgetItem *item = m_tree->currentItem();
    if (!item)
        return;
    const bool is8x8 = item->data(0, Qt::UserRole).toBool();
    const int idx = item->data(0, Qt::UserRole + 1).toInt();
    if (is8x8 && idx >= 0 && idx < m_rows8x8.size()) {
        m_rows8x8.removeAt(idx);
        m_orig8x8.removeAt(idx);
    } else if (!is8x8 && idx >= 0 && idx < m_rows2x16.size()) {
        m_rows2x16.removeAt(idx);
        m_orig2x16.removeAt(idx);
    }
    rebuild();
    updateButtons();
}

void TablesDialog::updateButtons()
{
    const bool sel = m_tree->currentItem() != nullptr && !m_tree->selectedItems().isEmpty();
    m_changeButton->setEnabled(sel);
    m_removeButton->setEnabled(sel);
}

void TablesDialog::commit()
{
    // This commit's own renameChannelReferences calls re-emit channelRenamed.
    // The listener exists for renames arriving from OUTSIDE while the copies
    // were open; letting it run now would rewrite m_orig* mid-loop, and the
    // catalog sync below would then never drop a renamed table's old channel.
    disconnect(m_renameConnection);

    // Assign the working rows into the document FIRST, so renameChannelReferences
    // can rewrite a sibling table's axis input (a table axis may be another
    // table's output) that was edited in this same session.
    m_config->table2x16Rows = m_rows2x16;
    m_config->table8x8Rows = m_rows8x8;

    // Carry references across any output rename so consumers keep pointing at it.
    for (int i = 0; i < m_rows2x16.size(); ++i) {
        const QString orig = m_orig2x16.value(i);
        if (!orig.isEmpty() && orig.compare(m_rows2x16[i].outputChannel, Qt::CaseInsensitive) != 0)
            m_config->renameChannelReferences(orig, m_rows2x16[i].outputChannel);
    }
    for (int i = 0; i < m_rows8x8.size(); ++i) {
        const QString orig = m_orig8x8.value(i);
        if (!orig.isEmpty() && orig.compare(m_rows8x8[i].outputChannel, Qt::CaseInsensitive) != 0)
            m_config->renameChannelReferences(orig, m_rows8x8[i].outputChannel);
    }

    // Sync the catalogue: drop channels for removed tables, add/update the rest.
    QSet<QString> newNames;
    for (const Table2x16Row &t : m_rows2x16)
        newNames.insert(t.outputChannel.toLower());
    for (const Table8x8Row &t : m_rows8x8)
        newNames.insert(t.outputChannel.toLower());
    const auto dropIfGone = [&](const QString &name) {
        if (!name.isEmpty() && !newNames.contains(name.toLower()))
            m_config->catalog().removeUserChannel(name);
    };
    for (const QString &n : m_orig2x16)
        dropIfGone(n);
    for (const QString &n : m_orig8x8)
        dropIfGone(n);
    for (const Table2x16Row &t : m_rows2x16)
        if (!t.outputChannel.isEmpty())
            m_config->catalog().addOrUpdateUserChannel(
                channelForOutput(t.outputChannel, t.dataType, t.decimalPlaces));
    for (const Table8x8Row &t : m_rows8x8)
        if (!t.outputChannel.isEmpty())
            m_config->catalog().addOrUpdateUserChannel(
                channelForOutput(t.outputChannel, t.dataType, t.decimalPlaces));

    m_config->setDirty();
}

} // namespace ct
