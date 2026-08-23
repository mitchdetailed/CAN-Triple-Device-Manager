// Calculations > Constants — grid editor for Configuration::constantRows.
#include "constants_dialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSet>
#include <QSpinBox>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <cmath>

#include "../model/channel_catalog.h"
#include "../protocol/wire_structs.h"
#include "trimmed_spin_box.h"

namespace ct {

namespace {

// Raw range + decimal cap per storage type (mirrors Edit Custom Channel). The
// physical range is the raw range scaled by the base resolution for integer
// types; boolean and float take the raw range directly.
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

// Physical range implied by a constant's data type + decimals.
void constantRange(const QString &dataType, int decimals, double *lo, double *hi, double *res)
{
    const DataTypeInfo *t = dataTypeInfo(dataType);
    const double resolution = std::pow(10.0, -decimals);
    const double scale = (t && t->scalesWithResolution) ? resolution : 1.0;
    if (res)
        *res = resolution;
    *lo = t ? t->rawMin * scale : -1e9;
    *hi = t ? t->rawMax * scale : 1e9;
}

// The catalogue channel a constant registers so it can be referenced elsewhere.
Channel channelForConstant(const ConstantRow &k)
{
    Channel c;
    c.name = k.name;
    c.quantity = QStringLiteral("Unitless");
    c.unit.clear();
    c.dataType = k.dataType;
    c.decimalPlaces = k.decimalPlaces;
    double lo = 0, hi = 0, res = 1.0;
    constantRange(k.dataType, k.decimalPlaces, &lo, &hi, &res);
    c.baseResolution = res;
    c.minValue = lo;
    c.maxValue = hi;
    c.category = QStringLiteral("User Channels");
    c.userDefined = true;
    return c;
}

// -------------------------------------------------------------------------
// Row editor (file-local, no Q_OBJECT).
// -------------------------------------------------------------------------
class ConstantRowEditor : public QDialog
{
public:
    ConstantRowEditor(const QList<ConstantRow> &siblings, int editingIndex,
                      const QStringList &reservedNames, const ConstantRow &row, QWidget *parent)
        : QDialog(parent), m_siblings(siblings), m_editingIndex(editingIndex),
          m_reserved(reservedNames), m_row(row)
    {
        setWindowTitle(QObject::tr("Constant"));
        setModal(true);

        auto *nameGroup = new QGroupBox(QObject::tr("Channel Name"), this);
        auto *nameForm = new QFormLayout(nameGroup);
        m_nameEdit = new QLineEdit(nameGroup);
        m_nameEdit->setMaxLength(MAX_CHANNEL_NAME_BYTES); // device label budget
        m_nameEdit->setToolTip(
            QObject::tr("Up to %1 characters — the device stores a %2-byte label.")
                .arg(MAX_CHANNEL_NAME_BYTES).arg(SIGNAL_LABEL_LEN));
        nameForm->addRow(QObject::tr("Channel Name:"), m_nameEdit);

        auto *detailsGroup = new QGroupBox(QObject::tr("Constant Details"), this);
        auto *form = new QFormLayout(detailsGroup);

        m_dataTypeCombo = new QComboBox(detailsGroup);
        m_dataTypeCombo->addItem(QString()); // blank forces a deliberate choice
        for (const DataTypeInfo &t : kDataTypes)
            m_dataTypeCombo->addItem(QLatin1String(t.name));

        m_decimalsSpin = new QSpinBox(detailsGroup);
        m_decimalsSpin->setRange(0, 8);

        m_resolutionSpin = new TrimmedDoubleSpinBox(detailsGroup);
        m_minSpin = new TrimmedDoubleSpinBox(detailsGroup);
        m_maxSpin = new TrimmedDoubleSpinBox(detailsGroup);
        for (QDoubleSpinBox *derived : {m_resolutionSpin, m_minSpin, m_maxSpin}) {
            derived->setRange(-5e9, 5e9);
            derived->setReadOnly(true);
            derived->setButtonSymbols(QAbstractSpinBox::NoButtons);
            derived->setFocusPolicy(Qt::NoFocus);
        }

        m_valueSpin = new TrimmedDoubleSpinBox(detailsGroup);
        m_valueSpin->setRange(-5e9, 5e9);
        m_valueSpin->setButtonSymbols(QAbstractSpinBox::NoButtons);

        form->addRow(QObject::tr("Data Type:"), m_dataTypeCombo);
        form->addRow(QObject::tr("Decimal Places:"), m_decimalsSpin);
        form->addRow(QObject::tr("Base Resolution:"), m_resolutionSpin);
        form->addRow(QObject::tr("Range Minimum:"), m_minSpin);
        form->addRow(QObject::tr("Range Maximum:"), m_maxSpin);
        form->addRow(QObject::tr("Value:"), m_valueSpin);

        m_activeCheck = new QCheckBox(QObject::tr("Active"), this);

        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                             Qt::Horizontal, this);

        auto *mainLayout = new QVBoxLayout(this);
        mainLayout->addWidget(nameGroup);
        mainLayout->addWidget(detailsGroup);
        mainLayout->addWidget(m_activeCheck);
        mainLayout->addWidget(buttons);

        QObject::connect(m_dataTypeCombo, &QComboBox::currentIndexChanged, this,
                         [this](int) { onDataTypeChanged(); });
        QObject::connect(m_decimalsSpin, &QSpinBox::valueChanged, this,
                         [this](int) { updateDerived(); });
        QObject::connect(buttons, &QDialogButtonBox::accepted, this,
                         [this]() { validateAndAccept(); });
        QObject::connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

        // Populate.
        m_nameEdit->setText(m_row.name.isEmpty() ? QObject::tr("New Constant") : m_row.name);
        m_dataTypeCombo->setCurrentIndex(qMax(0, m_dataTypeCombo->findText(m_row.dataType)));
        m_decimalsSpin->setValue(m_row.decimalPlaces);
        onDataTypeChanged(); // caps decimals + derives range, then set the value
        m_valueSpin->setValue(m_row.value);
        m_activeCheck->setChecked(m_row.active);
        m_nameEdit->setFocus();
        m_nameEdit->selectAll();
    }

    ConstantRow result() const { return m_row; }

private:
    void onDataTypeChanged()
    {
        const DataTypeInfo *type = dataTypeInfo(m_dataTypeCombo->currentText());
        m_decimalsSpin->setEnabled(type && type->maxDecimals > 0);
        m_decimalsSpin->setRange(0, type ? type->maxDecimals : 8);
        updateDerived();
    }

    void updateDerived()
    {
        const int decimals = m_decimalsSpin->value();
        double lo = 0, hi = 0, res = 1.0;
        constantRange(m_dataTypeCombo->currentText(), decimals, &lo, &hi, &res);
        for (QDoubleSpinBox *derived : {m_resolutionSpin, m_minSpin, m_maxSpin, m_valueSpin})
            derived->setDecimals(decimals);
        m_resolutionSpin->setValue(res);
        m_minSpin->setValue(lo);
        m_maxSpin->setValue(hi);
        // Keep the value inside the type's range, but only clamp the spin's
        // bounds (don't move a valid value the user already entered).
        m_valueSpin->setRange(lo, hi);
    }

    void validateAndAccept()
    {
        const QString name = m_nameEdit->text().trimmed();
        if (name.isEmpty()) {
            QMessageBox::warning(this, windowTitle(),
                                 QObject::tr("The constant name must not be empty."));
            return;
        }
        if (name.toUtf8().size() > MAX_CHANNEL_NAME_BYTES) {
            QMessageBox::warning(this, windowTitle(),
                                 QObject::tr("Names are limited to %1 bytes on the device.")
                                     .arg(MAX_CHANNEL_NAME_BYTES));
            return;
        }
        if (m_dataTypeCombo->currentText().isEmpty()) {
            QMessageBox::warning(this, windowTitle(),
                                 QObject::tr("Choose a data type for the constant."));
            return;
        }
        for (int i = 0; i < m_siblings.size(); ++i) {
            if (i == m_editingIndex)
                continue;
            if (m_siblings[i].name.compare(name, Qt::CaseInsensitive) == 0) {
                QMessageBox::warning(this, windowTitle(),
                                     QObject::tr("A constant named \"%1\" already exists.").arg(name));
                return;
            }
        }
        // Don't let a constant clobber an unrelated existing channel — the
        // constant would overwrite that channel's definition in the catalogue.
        for (const QString &reserved : m_reserved) {
            if (reserved.compare(name, Qt::CaseInsensitive) == 0) {
                QMessageBox::warning(this, windowTitle(),
                                     QObject::tr("A channel named \"%1\" already exists. "
                                                 "Choose a different name.").arg(name));
                return;
            }
        }
        m_row.name = name;
        m_row.dataType = m_dataTypeCombo->currentText();
        m_row.decimalPlaces = m_decimalsSpin->value();
        m_row.value = m_valueSpin->value();
        m_row.active = m_activeCheck->isChecked();
        accept();
    }

    QList<ConstantRow> m_siblings;
    int m_editingIndex;
    QStringList m_reserved;
    ConstantRow m_row;
    QLineEdit *m_nameEdit = nullptr;
    QComboBox *m_dataTypeCombo = nullptr;
    QSpinBox *m_decimalsSpin = nullptr;
    QDoubleSpinBox *m_resolutionSpin = nullptr;
    QDoubleSpinBox *m_minSpin = nullptr;
    QDoubleSpinBox *m_maxSpin = nullptr;
    QDoubleSpinBox *m_valueSpin = nullptr;
    QCheckBox *m_activeCheck = nullptr;
};

} // namespace

// -------------------------------------------------------------------------
// ConstantsDialog
// -------------------------------------------------------------------------
ConstantsDialog::ConstantsDialog(Configuration *config, QWidget *parent)
    : QDialog(parent), m_config(config), m_rows(config->constantRows)
{
    setWindowTitle(tr("Constants"));
    setModal(true);
    resize(640, 380);

    // Remember each row's committed name so a rename can carry references along.
    for (const ConstantRow &k : m_rows)
        m_originalNames << k.name;

    auto *mainLayout = new QVBoxLayout(this);
    auto *topLayout = new QHBoxLayout;

    m_tree = new QTreeWidget(this);
    m_tree->setColumnCount(5);
    m_tree->setHeaderLabels({tr("#"), tr("Active"), tr("Name"), tr("Data Type"), tr("Value")});
    m_tree->setRootIsDecorated(false);
    m_tree->setAllColumnsShowFocus(true);
    m_tree->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->header()->setStretchLastSection(true);
    m_tree->setColumnWidth(0, 36);
    m_tree->setColumnWidth(1, 56);
    m_tree->setColumnWidth(2, 200);
    m_tree->setColumnWidth(3, 90);
    topLayout->addWidget(m_tree, 1);

    auto *buttonColumn = new QVBoxLayout;
    m_addButton = new QPushButton(tr("Add…"), this);
    m_changeButton = new QPushButton(tr("Change…"), this);
    m_removeButton = new QPushButton(tr("Remove"), this);
    buttonColumn->addWidget(m_addButton);
    buttonColumn->addWidget(m_changeButton);
    buttonColumn->addWidget(m_removeButton);
    buttonColumn->addStretch(1);
    topLayout->addLayout(buttonColumn);

    mainLayout->addLayout(topLayout, 1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    mainLayout->addWidget(buttons);

    connect(m_addButton, &QPushButton::clicked, this, &ConstantsDialog::onAdd);
    connect(m_changeButton, &QPushButton::clicked, this, &ConstantsDialog::onChange);
    connect(m_removeButton, &QPushButton::clicked, this, &ConstantsDialog::onRemove);
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, [this]() { onChange(); });
    connect(m_tree, &QTreeWidget::itemSelectionChanged, this, &ConstantsDialog::updateButtons);

    connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
        commit();
        QDialog::accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    rebuild();
    updateButtons();
}

void ConstantsDialog::rebuild()
{
    const int selected = m_tree->currentItem()
        ? m_tree->indexOfTopLevelItem(m_tree->currentItem()) : -1;

    m_tree->clear();
    for (int i = 0; i < m_rows.size(); ++i) {
        const ConstantRow &row = m_rows.at(i);
        auto *item = new QTreeWidgetItem(m_tree);
        item->setText(0, QString::number(i + 1));
        item->setText(1, row.active ? tr("Yes") : tr("No"));
        item->setText(2, row.name);
        item->setText(3, row.dataType);
        item->setText(4, QString::number(row.value, 'g', 10));
    }

    if (selected >= 0 && selected < m_tree->topLevelItemCount())
        m_tree->setCurrentItem(m_tree->topLevelItem(selected));
    else if (m_tree->topLevelItemCount() > 0 && selected >= m_tree->topLevelItemCount())
        m_tree->setCurrentItem(m_tree->topLevelItem(m_tree->topLevelItemCount() - 1));
}

// Catalogue channel names that don't belong to a constant — a new/renamed
// constant must not collide with one (it would overwrite it). "Belongs to a
// constant" covers both the current working set and names that were constants
// at dialog open (their catalogue entries are still constant-owned until commit
// removes/renames them), so freeing a name mid-session and reusing it is allowed.
static QStringList reservedNames(Configuration *config, const QList<ConstantRow> &rows,
                                 const QList<QString> &originalNames)
{
    QSet<QString> ownNames;
    for (const ConstantRow &k : rows)
        ownNames.insert(k.name.toLower());
    for (const QString &n : originalNames)
        if (!n.isEmpty())
            ownNames.insert(n.toLower());
    QStringList reserved;
    for (const Channel &c : config->catalog().userChannels())
        if (!ownNames.contains(c.name.toLower()))
            reserved << c.name;
    return reserved;
}

void ConstantsDialog::onAdd()
{
    if (m_rows.size() >= MAX_CONSTANTS) {
        QMessageBox::warning(this, windowTitle(),
                             tr("The device supports at most %1 constants.").arg(MAX_CONSTANTS));
        return;
    }
    ConstantRowEditor editor(m_rows, -1, reservedNames(m_config, m_rows, m_originalNames),
                             ConstantRow(), this);
    if (editor.exec() == QDialog::Accepted) {
        m_rows.append(editor.result());
        m_originalNames.append(QString()); // new this session — no prior name
        rebuild();
        m_tree->setCurrentItem(m_tree->topLevelItem(m_tree->topLevelItemCount() - 1));
        updateButtons();
    }
}

void ConstantsDialog::onChange()
{
    QTreeWidgetItem *item = m_tree->currentItem();
    if (!item)
        return;
    const int index = m_tree->indexOfTopLevelItem(item);
    if (index < 0 || index >= m_rows.size())
        return;
    ConstantRowEditor editor(m_rows, index, reservedNames(m_config, m_rows, m_originalNames),
                             m_rows.at(index), this);
    if (editor.exec() == QDialog::Accepted) {
        m_rows[index] = editor.result();
        rebuild();
        updateButtons();
    }
}

void ConstantsDialog::onRemove()
{
    QTreeWidgetItem *item = m_tree->currentItem();
    if (!item)
        return;
    const int index = m_tree->indexOfTopLevelItem(item);
    if (index < 0 || index >= m_rows.size())
        return;
    m_rows.removeAt(index);
    m_originalNames.removeAt(index);
    rebuild();
    updateButtons();
}

void ConstantsDialog::updateButtons()
{
    const bool hasSelection = m_tree->currentItem() != nullptr
        && !m_tree->selectedItems().isEmpty();
    m_changeButton->setEnabled(hasSelection);
    m_removeButton->setEnabled(hasSelection);
}

void ConstantsDialog::commit()
{
    // Snapshot the previously committed constants before anything mutates the
    // document (renameChannelReferences rewrites constantRows in place).
    const QList<ConstantRow> previous = m_config->constantRows;

    // Carry references (math inputs, transmit rows, …) across any rename so a
    // renamed constant's consumers keep pointing at it. Mirrors EditChannelDialog.
    for (int i = 0; i < m_rows.size(); ++i) {
        const QString orig = m_originalNames.value(i);
        if (!orig.isEmpty() && orig.compare(m_rows[i].name, Qt::CaseInsensitive) != 0)
            m_config->renameChannelReferences(orig, m_rows[i].name);
    }

    // Sync the catalogue: drop channels for constants that no longer exist,
    // add/update a channel for each current constant so it stays referenceable.
    QStringList newNames;
    for (const ConstantRow &k : m_rows)
        newNames << k.name.toLower();
    for (const ConstantRow &old : previous)
        if (!old.name.isEmpty() && !newNames.contains(old.name.toLower()))
            m_config->catalog().removeUserChannel(old.name);
    for (const ConstantRow &k : m_rows)
        if (!k.name.isEmpty())
            m_config->catalog().addOrUpdateUserChannel(channelForConstant(k));

    m_config->constantRows = m_rows;
    m_config->setDirty();
}

} // namespace ct
