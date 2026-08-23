// Calculations > Math Channels — grid editor for Configuration::mathRows.
#include "math_dialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include "../protocol/wire_structs.h"
#include "channel_field.h"
#include "select_channel_dialog.h"
#include "trimmed_spin_box.h"

namespace ct {

namespace {

// ---------------------------------------------------------------------------
// Row editor dialog (file-local, no Q_OBJECT).
// ---------------------------------------------------------------------------
class MathRowEditor : public QDialog
{
public:
    MathRowEditor(Configuration *config, const MathRow &row, const ConfigPatch &livePatch,
                  QWidget *parent)
        : QDialog(parent), m_config(config), m_row(row), m_livePatch(livePatch)
    {
        setWindowTitle(QObject::tr("Math Channel"));
        setModal(true);

        auto *mainLayout = new QVBoxLayout(this);

        // Operation
        auto *opLayout = new QHBoxLayout;
        opLayout->addWidget(new QLabel(QObject::tr("Operation:"), this));
        m_opCombo = new QComboBox(this);
        m_opCombo->addItems(MathDialog::opNames());
        if (m_row.op >= 0 && m_row.op < m_opCombo->count())
            m_opCombo->setCurrentIndex(m_row.op);
        opLayout->addWidget(m_opCombo, 1);
        mainLayout->addLayout(opLayout);

        // Input A group
        auto *groupA = new QGroupBox(QObject::tr("Input A"), this);
        buildInputGroup(groupA, m_aChannelRadio, m_aChannelEdit, m_aSelectButton,
                        m_aConstRadio, m_aConstSpin);
        setChannelField(m_aChannelEdit, m_row.aChannel, m_config->catalog());
        m_aConstSpin->setValue(m_row.aConst);
        if (m_row.aIsChannel)
            m_aChannelRadio->setChecked(true);
        else
            m_aConstRadio->setChecked(true);
        mainLayout->addWidget(groupA);

        // Input B group
        m_groupB = new QGroupBox(QObject::tr("Input B"), this);
        buildInputGroup(m_groupB, m_bChannelRadio, m_bChannelEdit, m_bSelectButton,
                        m_bConstRadio, m_bConstSpin);
        setChannelField(m_bChannelEdit, m_row.bChannel, m_config->catalog());
        m_bConstSpin->setValue(m_row.bConst);
        if (m_row.bIsChannel)
            m_bChannelRadio->setChecked(true);
        else
            m_bConstRadio->setChecked(true);
        mainLayout->addWidget(m_groupB);

        // Input C group (arity-3 ops only; hidden otherwise)
        m_groupC = new QGroupBox(QObject::tr("Input C"), this);
        buildInputGroup(m_groupC, m_cChannelRadio, m_cChannelEdit, m_cSelectButton,
                        m_cConstRadio, m_cConstSpin);
        setChannelField(m_cChannelEdit, m_row.cChannel, m_config->catalog());
        m_cConstSpin->setValue(m_row.cConst);
        if (m_row.cIsChannel)
            m_cChannelRadio->setChecked(true);
        else
            m_cConstRadio->setChecked(true);
        mainLayout->addWidget(m_groupC);

        // Output channel
        auto *destLayout = new QHBoxLayout;
        destLayout->addWidget(new QLabel(QObject::tr("Output Channel:"), this));
        m_destEdit = new QLineEdit(this);
        m_destEdit->setReadOnly(true);
        setChannelField(m_destEdit, m_row.destChannel, m_config->catalog());
        destLayout->addWidget(m_destEdit, 1);
        auto *destSelect = new QPushButton(QObject::tr("Select…"), this);
        destLayout->addWidget(destSelect);
        mainLayout->addLayout(destLayout);

        // Active
        m_activeCheck = new QCheckBox(QObject::tr("Active"), this);
        m_activeCheck->setChecked(m_row.active);
        mainLayout->addWidget(m_activeCheck);

        mainLayout->addStretch(1);

        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        mainLayout->addWidget(buttons);

        // Wiring
        // A, B and C are read; the destination is written. Different guard
        // rails — see ChannelRole.
        QObject::connect(m_aSelectButton, &QPushButton::clicked, this, [this]() {
            const QString picked = SelectChannelDialog::pickInput(m_config,
                                                                  channelField(m_aChannelEdit),
                                                                  this, m_livePatch);
            if (!picked.isEmpty()) {
                setChannelField(m_aChannelEdit, picked, m_config->catalog());
                m_aChannelRadio->setChecked(true);
            }
        });
        QObject::connect(m_bSelectButton, &QPushButton::clicked, this, [this]() {
            const QString picked = SelectChannelDialog::pickInput(m_config,
                                                                  channelField(m_bChannelEdit),
                                                                  this, m_livePatch);
            if (!picked.isEmpty()) {
                setChannelField(m_bChannelEdit, picked, m_config->catalog());
                m_bChannelRadio->setChecked(true);
            }
        });
        QObject::connect(m_cSelectButton, &QPushButton::clicked, this, [this]() {
            const QString picked = SelectChannelDialog::pickInput(m_config,
                                                                  channelField(m_cChannelEdit),
                                                                  this, m_livePatch);
            if (!picked.isEmpty()) {
                setChannelField(m_cChannelEdit, picked, m_config->catalog());
                m_cChannelRadio->setChecked(true);
            }
        });
        QObject::connect(destSelect, &QPushButton::clicked, this, [this]() {
            const QString picked = SelectChannelDialog::pickOutput(m_config,
                                                                   channelField(m_destEdit),
                                                                   this, m_livePatch);
            if (!picked.isEmpty())
                setChannelField(m_destEdit, picked, m_config->catalog());
        });

        auto updateEnables = [this]() {
            const bool aChan = m_aChannelRadio->isChecked();
            m_aChannelEdit->setEnabled(aChan);
            m_aSelectButton->setEnabled(aChan);
            m_aConstSpin->setEnabled(!aChan);
            const bool bChan = m_bChannelRadio->isChecked();
            m_bChannelEdit->setEnabled(bChan);
            m_bSelectButton->setEnabled(bChan);
            m_bConstSpin->setEnabled(!bChan);
            const bool cChan = m_cChannelRadio->isChecked();
            m_cChannelEdit->setEnabled(cChan);
            m_cSelectButton->setEnabled(cChan);
            m_cConstSpin->setEnabled(!cChan);
        };
        QObject::connect(m_aChannelRadio, &QRadioButton::toggled, this, updateEnables);
        QObject::connect(m_bChannelRadio, &QRadioButton::toggled, this, updateEnables);
        QObject::connect(m_cChannelRadio, &QRadioButton::toggled, this, updateEnables);
        updateEnables();

        // Only the operands the op reads are shown — the group boxes appear
        // and disappear as the operation changes arity.
        auto updateArity = [this]() {
            const int arity = mathOpArity(m_opCombo->currentIndex());
            m_groupB->setVisible(arity >= 2);
            m_groupC->setVisible(arity >= 3);
        };
        QObject::connect(m_opCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                         this, updateArity);
        updateArity();

        QObject::connect(buttons, &QDialogButtonBox::accepted, this, [this]() { validateAndAccept(); });
        QObject::connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

        resize(420, sizeHint().height());
    }

    MathRow result() const { return m_row; }

private:
    void buildInputGroup(QGroupBox *group,
                         QRadioButton *&channelRadio, QLineEdit *&channelEdit,
                         QPushButton *&selectButton,
                         QRadioButton *&constRadio, QDoubleSpinBox *&constSpin)
    {
        auto *grid = new QGridLayout(group);
        channelRadio = new QRadioButton(QObject::tr("Channel"), group);
        channelEdit = new QLineEdit(group);
        channelEdit->setReadOnly(true);
        selectButton = new QPushButton(QObject::tr("Select…"), group);
        constRadio = new QRadioButton(QObject::tr("Constant"), group);
        constSpin = new TrimmedDoubleSpinBox(group);
        constSpin->setRange(-1e9, 1e9);
        constSpin->setDecimals(4);
        grid->addWidget(channelRadio, 0, 0);
        grid->addWidget(channelEdit, 0, 1);
        grid->addWidget(selectButton, 0, 2);
        grid->addWidget(constRadio, 1, 0);
        grid->addWidget(constSpin, 1, 1);
        grid->setColumnStretch(1, 1);
    }

    void validateAndAccept()
    {
        // Only the operands the op reads are checked or kept; the unused ones
        // are stored as defaults so the row matches what a Get reads back.
        const int arity = mathOpArity(m_opCombo->currentIndex());
        if (m_aChannelRadio->isChecked() && channelField(m_aChannelEdit).isEmpty()) {
            QMessageBox::warning(this, QObject::tr("Math Channel"),
                                 QObject::tr("Please select a channel for Input A."));
            return;
        }
        if (arity >= 2 && m_bChannelRadio->isChecked() && channelField(m_bChannelEdit).isEmpty()) {
            QMessageBox::warning(this, QObject::tr("Math Channel"),
                                 QObject::tr("Please select a channel for Input B."));
            return;
        }
        if (arity >= 3 && m_cChannelRadio->isChecked() && channelField(m_cChannelEdit).isEmpty()) {
            QMessageBox::warning(this, QObject::tr("Math Channel"),
                                 QObject::tr("Please select a channel for Input C."));
            return;
        }
        if (channelField(m_destEdit).isEmpty()) {
            QMessageBox::warning(this, QObject::tr("Math Channel"),
                                 QObject::tr("Please select an output channel."));
            return;
        }
        m_row.op = m_opCombo->currentIndex();
        m_row.aIsChannel = m_aChannelRadio->isChecked();
        m_row.aChannel = channelField(m_aChannelEdit);
        m_row.aConst = m_aConstSpin->value();
        m_row.bIsChannel = arity >= 2 && m_bChannelRadio->isChecked();
        m_row.bChannel = arity >= 2 ? channelField(m_bChannelEdit) : QString();
        m_row.bConst = arity >= 2 ? m_bConstSpin->value() : 0;
        m_row.cIsChannel = arity >= 3 && m_cChannelRadio->isChecked();
        m_row.cChannel = arity >= 3 ? channelField(m_cChannelEdit) : QString();
        m_row.cConst = arity >= 3 ? m_cConstSpin->value() : 0;
        m_row.destChannel = channelField(m_destEdit);
        m_row.active = m_activeCheck->isChecked();
        accept();
    }

    Configuration *m_config;
    MathRow m_row;
    ConfigPatch m_livePatch;
    QComboBox *m_opCombo = nullptr;
    QRadioButton *m_aChannelRadio = nullptr;
    QLineEdit *m_aChannelEdit = nullptr;
    QPushButton *m_aSelectButton = nullptr;
    QRadioButton *m_aConstRadio = nullptr;
    QDoubleSpinBox *m_aConstSpin = nullptr;
    QGroupBox *m_groupB = nullptr;
    QRadioButton *m_bChannelRadio = nullptr;
    QLineEdit *m_bChannelEdit = nullptr;
    QPushButton *m_bSelectButton = nullptr;
    QRadioButton *m_bConstRadio = nullptr;
    QDoubleSpinBox *m_bConstSpin = nullptr;
    QGroupBox *m_groupC = nullptr;
    QRadioButton *m_cChannelRadio = nullptr;
    QLineEdit *m_cChannelEdit = nullptr;
    QPushButton *m_cSelectButton = nullptr;
    QRadioButton *m_cConstRadio = nullptr;
    QDoubleSpinBox *m_cConstSpin = nullptr;
    QLineEdit *m_destEdit = nullptr;
    QCheckBox *m_activeCheck = nullptr;
};

// One operand cell of the grid. A channel operand is DISPLAY only here — the
// row it belongs to is found by its index in the tree, never by parsing this
// text back — so it carries its unit; a constant is just the number.
QString inputText(bool isChannel, const QString &channel, double constant,
                  const ChannelCatalog &catalog)
{
    return isChannel ? catalog.labelFor(channel) : QString::number(constant);
}

// This dialog's working rows as a patch over the document. The grid writes back
// only on OK, so mid-session the document both lacks rows just added and still
// carries rows just deleted or re-pointed; the channel picker judges against
// this instead. Captured by value — the row editor outlives the call.
ConfigPatch livePatch(const QList<MathRow> &rows)
{
    return [rows](Configuration &c) { c.mathRows = rows; };
}

} // namespace

// ---------------------------------------------------------------------------
// MathDialog
// ---------------------------------------------------------------------------
MathDialog::MathDialog(Configuration *config, QWidget *parent)
    : QDialog(parent), m_config(config), m_rows(config->mathRows)
{
    setWindowTitle(tr("Math Channels"));
    setModal(true);
    resize(700, 400);

    auto *mainLayout = new QVBoxLayout(this);
    auto *topLayout = new QHBoxLayout;

    m_tree = new QTreeWidget(this);
    m_tree->setColumnCount(7);
    m_tree->setHeaderLabels({tr("#"), tr("Active"), tr("Operation"),
                             tr("Input A"), tr("Input B"), tr("Input C"),
                             tr("Output Channel")});
    m_tree->setRootIsDecorated(false);
    m_tree->setAllColumnsShowFocus(true);
    m_tree->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->header()->setStretchLastSection(true);
    m_tree->setColumnWidth(0, 36);
    m_tree->setColumnWidth(1, 56);
    m_tree->setColumnWidth(2, 110);
    m_tree->setColumnWidth(3, 130);
    m_tree->setColumnWidth(4, 130);
    m_tree->setColumnWidth(5, 130);
    topLayout->addWidget(m_tree, 1);

    // MoTeC-style vertical button stack on the right
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

    connect(m_addButton, &QPushButton::clicked, this, &MathDialog::onAdd);
    connect(m_changeButton, &QPushButton::clicked, this, &MathDialog::onChange);
    connect(m_removeButton, &QPushButton::clicked, this, &MathDialog::onRemove);
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, [this]() { onChange(); });
    connect(m_tree, &QTreeWidget::itemSelectionChanged, this, &MathDialog::updateButtons);

    connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
        m_config->mathRows = m_rows;
        m_config->setDirty();
        QDialog::accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    // A rename can arrive from a row editor's picker while this working copy
    // is open — see Configuration::channelRenamed.
    connect(m_config, &Configuration::channelRenamed, this,
            [this](const QString &oldName, const QString &newName) {
                renameChannelRefs(m_rows, oldName, newName);
                rebuild();
            });

    rebuild();
    updateButtons();
}

QStringList MathDialog::opNames()
{
    // Indexed by ct::MathOp — the combo index IS the wire value, so this list
    // must stay in enum order with no gaps.
    return {tr("A + B"), tr("A − B"), tr("A × B"), tr("A ÷ B"),
            tr("Scale (A × B)"), tr("Min(A, B)"), tr("Max(A, B)"),
            tr("A AND B"), tr("A OR B"),
            tr("Absolute (|A|)"), tr("Negate (−A)"), tr("Square root (√A)"),
            tr("Floor (A)"), tr("Ceiling (A)"), tr("Round (A)"),
            tr("Modulo (A mod B)"), tr("XOR (bitwise)"),
            tr("Logical AND (A and B)"), tr("Logical OR (A or B)"),
            tr("Logical NOT (A)"),
            tr("A > B"), tr("A ≥ B"), tr("A < B"), tr("A ≤ B"),
            tr("A = B"), tr("A ≠ B"),
            tr("Multiply-add (A × B + C)"), tr("Clamp (A between B and C)"),
            tr("Interpolate (A to B by C)"), tr("Select (A ? B : C)"),
            tr("Wrap (A into B..C)")};
}

void MathDialog::rebuild()
{
    const int selected = m_tree->currentItem()
        ? m_tree->indexOfTopLevelItem(m_tree->currentItem()) : -1;

    m_tree->clear();
    const QStringList ops = opNames();
    const ChannelCatalog &catalog = m_config->catalog();
    for (int i = 0; i < m_rows.size(); ++i) {
        const MathRow &row = m_rows.at(i);
        auto *item = new QTreeWidgetItem(m_tree);
        // Operands the op does not read stay blank rather than showing a
        // meaningless 0 — the row reads like the expression it computes.
        const int arity = mathOpArity(row.op);
        item->setText(0, QString::number(i + 1));
        item->setText(1, row.active ? tr("Yes") : tr("No"));
        item->setText(2, (row.op >= 0 && row.op < ops.size()) ? ops.at(row.op) : QString());
        item->setText(3, inputText(row.aIsChannel, row.aChannel, row.aConst, catalog));
        item->setText(4, arity >= 2 ? inputText(row.bIsChannel, row.bChannel, row.bConst, catalog)
                                    : QString());
        item->setText(5, arity >= 3 ? inputText(row.cIsChannel, row.cChannel, row.cConst, catalog)
                                    : QString());
        item->setText(6, catalog.labelFor(row.destChannel));
    }

    if (selected >= 0 && selected < m_tree->topLevelItemCount())
        m_tree->setCurrentItem(m_tree->topLevelItem(selected));
    else if (m_tree->topLevelItemCount() > 0 && selected >= m_tree->topLevelItemCount())
        m_tree->setCurrentItem(m_tree->topLevelItem(m_tree->topLevelItemCount() - 1));
}

void MathDialog::onAdd()
{
    if (m_rows.size() >= MAX_MATH_COMPUTATIONS) {
        QMessageBox::warning(this, windowTitle(),
                             tr("The device supports at most %1 math channels.")
                                 .arg(MAX_MATH_COMPUTATIONS));
        return;
    }
    MathRowEditor editor(m_config, MathRow(), livePatch(m_rows), this);
    if (editor.exec() == QDialog::Accepted) {
        m_rows.append(editor.result());
        rebuild();
        m_tree->setCurrentItem(m_tree->topLevelItem(m_tree->topLevelItemCount() - 1));
        updateButtons();
    }
}

void MathDialog::onChange()
{
    QTreeWidgetItem *item = m_tree->currentItem();
    if (!item)
        return;
    const int index = m_tree->indexOfTopLevelItem(item);
    if (index < 0 || index >= m_rows.size())
        return;
    MathRowEditor editor(m_config, m_rows.at(index), livePatch(m_rows), this);
    if (editor.exec() == QDialog::Accepted) {
        m_rows[index] = editor.result();
        rebuild();
        updateButtons();
    }
}

void MathDialog::onRemove()
{
    QTreeWidgetItem *item = m_tree->currentItem();
    if (!item)
        return;
    const int index = m_tree->indexOfTopLevelItem(item);
    if (index < 0 || index >= m_rows.size())
        return;
    m_rows.removeAt(index);
    rebuild();
    updateButtons();
}

void MathDialog::updateButtons()
{
    const bool hasSelection = m_tree->currentItem() != nullptr
        && !m_tree->selectedItems().isEmpty();
    m_changeButton->setEnabled(hasSelection);
    m_removeButton->setEnabled(hasSelection);
}

} // namespace ct
