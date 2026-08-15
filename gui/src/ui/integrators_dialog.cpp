// Calculations > Integrators — grid editor for Configuration::integratorRows.
// An integrator is a rate accumulator: every step it adds its input to its
// output channel, `rateHz` times a second. Raw accumulation, not input * dt —
// see IntegratorRow and the firmware's IntegratorConfig for why.
#include "integrators_dialog.h"

#include <QButtonGroup>
#include <QCheckBox>
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
#include <QRadioButton>
#include <QSpinBox>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include "../protocol/wire_structs.h"
#include "channel_field.h"
#include "select_channel_dialog.h"
#include "trimmed_spin_box.h"

namespace ct {

namespace {

// This dialog's working rows as a patch over the document — the grid writes
// back only on OK, so the channel picker judges against this rather than the
// stale document. Captured by value; the row editor outlives the call.
ConfigPatch livePatch(const QList<IntegratorRow> &rows)
{
    return [rows](Configuration &c) { c.integratorRows = rows; };
}

// A read-only channel field with its Select… button, as used for every channel
// slot in this dialog. Returns the field; `select` receives the button.
//
// The field is built empty and always filled through ct::setChannelField(), so
// it shows "Coolant Temp °C" while remembering "Coolant Temp"; every read of it
// goes through ct::channelField(). Named makeChannelField() so it cannot be
// confused with that reader.
QLineEdit *makeChannelField(QWidget *parent, QHBoxLayout **rowOut, QPushButton **select)
{
    auto *edit = new QLineEdit(parent);
    edit->setReadOnly(true);
    *select = new QPushButton(QObject::tr("Select…"), parent);
    auto *row = new QHBoxLayout;
    row->addWidget(edit, 1);
    row->addWidget(*select);
    *rowOut = row;
    return edit;
}

// ---------------------------------------------------------------------------
// File-local row editor dialog (no Q_OBJECT needed — lambda connections only).
// ---------------------------------------------------------------------------
class IntegratorRowEditor : public QDialog
{
public:
    IntegratorRowEditor(Configuration *config, const IntegratorRow &row,
                        const ConfigPatch &livePatch, QWidget *parent)
        : QDialog(parent), m_config(config), m_row(row), m_livePatch(livePatch)
    {
        setWindowTitle(QObject::tr("Integrator"));
        setModal(true);

        auto *mainLayout = new QVBoxLayout(this);

        // --- Output ---------------------------------------------------------
        {
            auto *group = new QGroupBox(QObject::tr("Output"), this);
            auto *form = new QFormLayout(group);
            QHBoxLayout *row = nullptr;
            QPushButton *select = nullptr;
            m_outputEdit = makeChannelField(group, &row, &select);
            form->addRow(QObject::tr("Output Channel :"), row);
            QObject::connect(select, &QPushButton::clicked, this, [this] {
                const QString picked = SelectChannelDialog::pickOutput(
                    m_config, ct::channelField(m_outputEdit), this, m_livePatch);
                if (!picked.isEmpty()) {
                    ct::setChannelField(m_outputEdit, picked, m_config->catalog());
                    updateSummary();
                }
            });
            mainLayout->addWidget(group);
        }

        // --- Input + rate ---------------------------------------------------
        {
            auto *group = new QGroupBox(QObject::tr("Accumulate"), this);
            auto *form = new QFormLayout(group);

            m_inputChannelRadio = new QRadioButton(QObject::tr("Channel :"), group);
            m_inputValueRadio = new QRadioButton(QObject::tr("Fixed value :"), group);
            auto *inputButtons = new QButtonGroup(this);
            inputButtons->addButton(m_inputChannelRadio);
            inputButtons->addButton(m_inputValueRadio);

            QHBoxLayout *row = nullptr;
            m_inputEdit = makeChannelField(group, &row, &m_inputSelect);
            form->addRow(m_inputChannelRadio, row);

            m_inputValueSpin = new TrimmedDoubleSpinBox(group);
            m_inputValueSpin->setRange(-1e9, 1e9);
            m_inputValueSpin->setDecimals(6);
            form->addRow(m_inputValueRadio, m_inputValueSpin);

            m_rateSpin = new QSpinBox(group);
            m_rateSpin->setRange(1, INTEGRATOR_MAX_HZ);
            m_rateSpin->setSuffix(QObject::tr(" Hz"));
            m_rateSpin->setToolTip(
                QObject::tr("How many times a second the input is applied.\n"
                            "The engine evaluates at %1 Hz, which is the ceiling.")
                    .arg(INTEGRATOR_MAX_HZ));
            form->addRow(QObject::tr("Rate :"), m_rateSpin);

            // Direction. "Count down" is what makes this row a decrementor:
            // same input, same rate, subtracted from a starting value instead.
            auto *dirRow = new QHBoxLayout;
            m_countUpRadio = new QRadioButton(QObject::tr("Count up"), group);
            m_countDownRadio = new QRadioButton(QObject::tr("Count down"), group);
            auto *dirButtons = new QButtonGroup(this);
            dirButtons->addButton(m_countUpRadio);
            dirButtons->addButton(m_countDownRadio);
            dirRow->addWidget(m_countUpRadio);
            dirRow->addWidget(m_countDownRadio);
            dirRow->addStretch(1);
            form->addRow(QObject::tr("Direction :"), dirRow);

            QObject::connect(m_countDownRadio, &QRadioButton::toggled, this,
                             [this](bool) { updateSummary(); });

            m_summary = new QLabel(group);
            m_summary->setWordWrap(true);
            form->addRow(QString(), m_summary);

            QObject::connect(m_inputSelect, &QPushButton::clicked, this, [this] {
                const QString picked = SelectChannelDialog::pickInput(
                    m_config, ct::channelField(m_inputEdit), this, m_livePatch);
                if (!picked.isEmpty()) {
                    ct::setChannelField(m_inputEdit, picked, m_config->catalog());
                    updateSummary();
                }
            });
            QObject::connect(m_inputChannelRadio, &QRadioButton::toggled, this,
                             [this](bool) { updateInputMode(); });
            QObject::connect(m_inputValueSpin, &QDoubleSpinBox::valueChanged, this,
                             [this](double) { updateSummary(); });
            QObject::connect(m_rateSpin, &QSpinBox::valueChanged, this,
                             [this](int) { updateSummary(); });

            mainLayout->addWidget(group);
        }

        // --- Enable / reset --------------------------------------------------
        {
            auto *group = new QGroupBox(QObject::tr("Control"), this);
            auto *form = new QFormLayout(group);

            m_startValueSpin = new TrimmedDoubleSpinBox(group);
            m_startValueSpin->setRange(-1e9, 1e9);
            m_startValueSpin->setDecimals(6);
            m_startValueSpin->setToolTip(
                QObject::tr("The value the device loads at power-up. For a count-down\n"
                            "integrator this is the peak it counts down from."));
            form->addRow(QObject::tr("Starting value :"), m_startValueSpin);
            QObject::connect(m_startValueSpin, &QDoubleSpinBox::valueChanged, this,
                             [this](double) { updateSummary(); });

            QHBoxLayout *row = nullptr;
            QPushButton *enableSelect = nullptr;
            m_enableEdit = makeChannelField(group, &row, &enableSelect);
            m_enableEdit->setPlaceholderText(QObject::tr("(always accumulating)"));
            form->addRow(QObject::tr("Enable while channel :"), row);

            QPushButton *resetSelect = nullptr;
            m_resetEdit = makeChannelField(group, &row, &resetSelect);
            m_resetEdit->setPlaceholderText(QObject::tr("(never reset)"));
            form->addRow(QObject::tr("Reset on channel :"), row);

            m_resetValueSpin = new TrimmedDoubleSpinBox(group);
            m_resetValueSpin->setRange(-1e9, 1e9);
            m_resetValueSpin->setDecimals(6);
            form->addRow(QObject::tr("When reset, set value to :"), m_resetValueSpin);

            m_preserveCheck =
                new QCheckBox(QObject::tr("Preserve value across power cycles"), group);
            m_preserveCheck->setToolTip(
                QObject::tr("Retains the running total in flash, restoring it at the next\n"
                            "power-up instead of the starting value.\n\n"
                            "Shares a 20-entry store with preserved counters.\n\n"
                            "A running integrator changes constantly, so unlike a counter\n"
                            "it writes to flash on almost every 60 s save — which makes it\n"
                            "the main consumer of both the store's space and its erase\n"
                            "budget."));
            form->addRow(QString(), m_preserveCheck);

            auto *note = new QLabel(
                QObject::tr("Enable gates accumulation (true = value > 0) and pauses it "
                            "rather than losing steps. Reset triggers on a rising edge and "
                            "applies even while disabled. Without Preserve, the value "
                            "returns to the starting value at every power-up."),
                group);
            note->setWordWrap(true);
            form->addRow(QString(), note);

            // Both are trigger inputs: the picker's Input side, which reads a
            // channel and so offers the whole catalogue.
            QObject::connect(enableSelect, &QPushButton::clicked, this, [this] {
                const QString picked = SelectChannelDialog::pickInput(
                    m_config, ct::channelField(m_enableEdit), this, m_livePatch);
                if (!picked.isEmpty())
                    ct::setChannelField(m_enableEdit, picked, m_config->catalog());
            });
            QObject::connect(resetSelect, &QPushButton::clicked, this, [this] {
                const QString picked = SelectChannelDialog::pickInput(
                    m_config, ct::channelField(m_resetEdit), this, m_livePatch);
                if (!picked.isEmpty())
                    ct::setChannelField(m_resetEdit, picked, m_config->catalog());
            });

            mainLayout->addWidget(group);
        }

        // --- Limits ----------------------------------------------------------
        {
            auto *group = new QGroupBox(QObject::tr("Limits"), this);
            auto *form = new QFormLayout(group);
            m_minSpin = new TrimmedDoubleSpinBox(group);
            m_minSpin->setRange(-1e9, 1e9);
            m_minSpin->setDecimals(6);
            form->addRow(QObject::tr("Minimum :"), m_minSpin);
            m_maxSpin = new TrimmedDoubleSpinBox(group);
            m_maxSpin->setRange(-1e9, 1e9);
            m_maxSpin->setDecimals(6);
            form->addRow(QObject::tr("Maximum :"), m_maxSpin);
            auto *note = new QLabel(
                QObject::tr("The value holds at these limits — for a count-down "
                            "integrator the minimum is the floor it stops at. A maximum "
                            "that does not exceed the minimum turns clamping off entirely."),
                group);
            note->setWordWrap(true);
            form->addRow(QString(), note);
            mainLayout->addWidget(group);
        }

        // --- Active + buttons -------------------------------------------------
        m_activeCheck = new QCheckBox(QObject::tr("Active"), this);
        mainLayout->addWidget(m_activeCheck);

        auto *buttons = new QDialogButtonBox(
            QDialogButtonBox::Ok | QDialogButtonBox::Cancel, Qt::Horizontal, this);
        mainLayout->addWidget(buttons);

        QObject::connect(buttons, &QDialogButtonBox::accepted, this, [this] { onOk(); });
        QObject::connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

        // --- Load the row -----------------------------------------------------
        ct::setChannelField(m_outputEdit, m_row.outputChannel, m_config->catalog());
        ct::setChannelField(m_inputEdit, m_row.inputChannel, m_config->catalog());
        m_inputValueSpin->setValue(m_row.inputValue);
        if (m_row.inputIsChannel)
            m_inputChannelRadio->setChecked(true);
        else
            m_inputValueRadio->setChecked(true);
        m_rateSpin->setValue(qBound(1, m_row.rateHz, INTEGRATOR_MAX_HZ));
        if (m_row.countDown)
            m_countDownRadio->setChecked(true);
        else
            m_countUpRadio->setChecked(true);
        m_startValueSpin->setValue(m_row.startValue);
        ct::setChannelField(m_enableEdit, m_row.enableChannel, m_config->catalog());
        ct::setChannelField(m_resetEdit, m_row.resetChannel, m_config->catalog());
        m_resetValueSpin->setValue(m_row.resetValue);
        m_minSpin->setValue(m_row.minValue);
        m_maxSpin->setValue(m_row.maxValue);
        m_preserveCheck->setChecked(m_row.preserveValue);
        m_activeCheck->setChecked(m_row.active);
        updateInputMode();

        resize(500, sizeHint().height());
    }

    IntegratorRow result() const { return m_row; }

private:
    void updateInputMode()
    {
        const bool channel = m_inputChannelRadio->isChecked();
        m_inputEdit->setEnabled(channel);
        m_inputSelect->setEnabled(channel);
        m_inputValueSpin->setEnabled(!channel);
        updateSummary();
    }

    // Spell out what the configured row actually does. The rate scales the
    // result, so the per-second figure is the number that surprises people;
    // for a count-down row the starting value matters just as much.
    //
    // Bare names here, deliberately: this sentence is an assignment statement
    // ("Trip Distance += Speed"), and a unit wedged between the operands would
    // read as part of the algebra rather than as a label.
    void updateSummary()
    {
        const QString outName = ct::channelField(m_outputEdit);
        const QString out = outName.isEmpty() ? QObject::tr("the output") : outName;
        const int rate = m_rateSpin->value();
        const bool down = m_countDownRadio->isChecked();
        const QString op = down ? QStringLiteral("-=") : QStringLiteral("+=");
        QString text;
        if (m_inputChannelRadio->isChecked()) {
            const QString inName = ct::channelField(m_inputEdit);
            const QString in = inName.isEmpty() ? QObject::tr("input") : inName;
            text = QObject::tr("%1 %2 %3, %4 times a second — so a steady %3 of 1 moves it "
                               "%4 per second.")
                       .arg(out, op, in).arg(rate);
        } else {
            const double step = m_inputValueSpin->value();
            text = QObject::tr("%1 %2 %3, %4 times a second — %5 per second.")
                       .arg(out, op)
                       .arg(step)
                       .arg(rate)
                       .arg(step * rate);
        }
        text += down ? QObject::tr(" Starts at %1 and counts down.")
                           .arg(m_startValueSpin->value())
                     : QObject::tr(" Starts at %1.").arg(m_startValueSpin->value());
        m_summary->setText(text);
    }

    void onOk()
    {
        // Bare names throughout — these are the strings that reach the document,
        // and the comparison below has to be name against name.
        const QString outputName = ct::channelField(m_outputEdit);
        const QString inputName = ct::channelField(m_inputEdit);

        if (outputName.isEmpty()) {
            QMessageBox::warning(this, QObject::tr("Integrator"),
                                 QObject::tr("Please select an output channel."));
            return;
        }
        if (m_inputChannelRadio->isChecked() && inputName.isEmpty()) {
            QMessageBox::warning(this, QObject::tr("Integrator"),
                                 QObject::tr("Please select an input channel, or switch to "
                                             "a fixed value."));
            return;
        }
        // Caught here as well as in validation: an integrator fed by its own
        // output doubles every step instead of accumulating, and the mistake is
        // far cheaper to explain at the point of making it.
        if (m_inputChannelRadio->isChecked()
            && inputName.compare(outputName, Qt::CaseInsensitive) == 0) {
            QMessageBox::warning(this, QObject::tr("Integrator"),
                                 QObject::tr("The input and output cannot be the same "
                                             "channel — the value would double every step "
                                             "instead of accumulating."));
            return;
        }
        m_row.outputChannel = outputName;
        m_row.inputIsChannel = m_inputChannelRadio->isChecked();
        m_row.inputChannel = inputName;
        m_row.inputValue = m_inputValueSpin->value();
        m_row.rateHz = m_rateSpin->value();
        m_row.countDown = m_countDownRadio->isChecked();
        m_row.startValue = m_startValueSpin->value();
        m_row.enableChannel = ct::channelField(m_enableEdit);
        m_row.resetChannel = ct::channelField(m_resetEdit);
        m_row.resetValue = m_resetValueSpin->value();
        m_row.minValue = m_minSpin->value();
        m_row.maxValue = m_maxSpin->value();
        m_row.preserveValue = m_preserveCheck->isChecked();
        m_row.active = m_activeCheck->isChecked();
        accept();
    }

    Configuration *m_config;
    IntegratorRow m_row;
    ConfigPatch m_livePatch;

    QLineEdit *m_outputEdit = nullptr;
    QRadioButton *m_inputChannelRadio = nullptr;
    QRadioButton *m_inputValueRadio = nullptr;
    QLineEdit *m_inputEdit = nullptr;
    QPushButton *m_inputSelect = nullptr;
    QDoubleSpinBox *m_inputValueSpin = nullptr;
    QSpinBox *m_rateSpin = nullptr;
    QRadioButton *m_countUpRadio = nullptr;
    QRadioButton *m_countDownRadio = nullptr;
    QLabel *m_summary = nullptr;
    QDoubleSpinBox *m_startValueSpin = nullptr;
    QLineEdit *m_enableEdit = nullptr;
    QLineEdit *m_resetEdit = nullptr;
    QDoubleSpinBox *m_resetValueSpin = nullptr;
    QDoubleSpinBox *m_minSpin = nullptr;
    QDoubleSpinBox *m_maxSpin = nullptr;
    QCheckBox *m_preserveCheck = nullptr;
    QCheckBox *m_activeCheck = nullptr;
};

} // namespace

// ---------------------------------------------------------------------------
// IntegratorsDialog
// ---------------------------------------------------------------------------
IntegratorsDialog::IntegratorsDialog(Configuration *config, QWidget *parent)
    : QDialog(parent), m_config(config), m_rows(config->integratorRows)
{
    setWindowTitle(tr("Integrators"));
    setModal(true);
    resize(760, 360);

    m_tree = new QTreeWidget(this);
    m_tree->setColumnCount(8);
    m_tree->setHeaderLabels({ tr("#"), tr("Active"), tr("Output"), tr("Applies"),
                              tr("Rate"), tr("Direction"), tr("Starts at"),
                              tr("Reset") });
    m_tree->setRootIsDecorated(false);
    m_tree->setAllColumnsShowFocus(true);
    m_tree->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->header()->setStretchLastSection(true);
    m_tree->setColumnWidth(0, 36);
    m_tree->setColumnWidth(1, 56);
    m_tree->setColumnWidth(2, 140);
    m_tree->setColumnWidth(3, 140);
    m_tree->setColumnWidth(4, 60);
    m_tree->setColumnWidth(5, 90);
    m_tree->setColumnWidth(6, 80);

    m_addButton = new QPushButton(tr("Add…"), this);
    m_changeButton = new QPushButton(tr("Change…"), this);
    m_removeButton = new QPushButton(tr("Remove"), this);

    auto *sideLayout = new QVBoxLayout;
    sideLayout->addWidget(m_addButton);
    sideLayout->addWidget(m_changeButton);
    sideLayout->addWidget(m_removeButton);
    sideLayout->addStretch(1);

    auto *topLayout = new QHBoxLayout;
    topLayout->addWidget(m_tree, 1);
    topLayout->addLayout(sideLayout);

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, Qt::Horizontal, this);

    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->addLayout(topLayout, 1);
    mainLayout->addWidget(buttons);

    connect(m_addButton, &QPushButton::clicked, this, &IntegratorsDialog::onAdd);
    connect(m_changeButton, &QPushButton::clicked, this, &IntegratorsDialog::onChange);
    connect(m_removeButton, &QPushButton::clicked, this, &IntegratorsDialog::onRemove);
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem *, int) { onChange(); });
    connect(m_tree, &QTreeWidget::itemSelectionChanged, this,
            &IntegratorsDialog::updateButtons);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        m_config->integratorRows = m_rows;
        m_config->setDirty();
        accept();
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

void IntegratorsDialog::rebuild()
{
    const int selected = m_tree->indexOfTopLevelItem(m_tree->currentItem());
    // The grid is pure display: a row is identified by its position (every
    // handler goes through indexOfTopLevelItem() into m_rows), so the channel
    // columns can carry the unit without any of it being read back as a name.
    const ChannelCatalog &catalog = m_config->catalog();
    m_tree->clear();
    for (int i = 0; i < m_rows.size(); ++i) {
        const IntegratorRow &row = m_rows.at(i);
        auto *item = new QTreeWidgetItem(m_tree);
        item->setText(0, QString::number(i + 1));
        item->setText(1, row.active ? tr("Yes") : tr("No"));
        item->setText(2, catalog.labelFor(row.outputChannel));
        item->setText(3, row.inputIsChannel ? catalog.labelFor(row.inputChannel)
                                            : QString::number(row.inputValue));
        item->setText(4, tr("%1 Hz").arg(row.rateHz));
        item->setText(5, row.countDown ? tr("Down") : tr("Up"));
        // Preserve changes what happens at the next power-up, so it belongs
        // next to the starting value it overrides rather than in a column of
        // its own.
        item->setText(6, row.preserveValue
                             ? tr("%1 (preserved)").arg(row.startValue)
                             : QString::number(row.startValue));
        item->setText(7, row.resetChannel.isEmpty()
                             ? tr("(none)")
                             : catalog.labelFor(row.resetChannel));
    }
    if (selected >= 0 && selected < m_tree->topLevelItemCount())
        m_tree->setCurrentItem(m_tree->topLevelItem(selected));
}

void IntegratorsDialog::onAdd()
{
    if (m_rows.size() >= MAX_INTEGRATORS) {
        QMessageBox::warning(this, windowTitle(),
                             tr("The device supports at most %1 integrators.")
                                 .arg(MAX_INTEGRATORS));
        return;
    }
    IntegratorRowEditor editor(m_config, IntegratorRow(), livePatch(m_rows), this);
    if (editor.exec() == QDialog::Accepted) {
        m_rows.append(editor.result());
        rebuild();
        m_tree->setCurrentItem(m_tree->topLevelItem(m_tree->topLevelItemCount() - 1));
        updateButtons();
    }
}

void IntegratorsDialog::onChange()
{
    const int index = m_tree->indexOfTopLevelItem(m_tree->currentItem());
    if (index < 0 || index >= m_rows.size())
        return;
    IntegratorRowEditor editor(m_config, m_rows.at(index), livePatch(m_rows), this);
    if (editor.exec() == QDialog::Accepted) {
        m_rows[index] = editor.result();
        rebuild();
        updateButtons();
    }
}

void IntegratorsDialog::onRemove()
{
    const int index = m_tree->indexOfTopLevelItem(m_tree->currentItem());
    if (index < 0 || index >= m_rows.size())
        return;
    m_rows.removeAt(index);
    rebuild();
    updateButtons();
}

void IntegratorsDialog::updateButtons()
{
    // QTreeWidget keeps a non-null currentItem() after the selection clears, so
    // gate on an actual highlighted row.
    const bool hasSelection = !m_tree->selectedItems().isEmpty();
    m_changeButton->setEnabled(hasSelection);
    m_removeButton->setEnabled(hasSelection);
    m_addButton->setEnabled(m_rows.size() < MAX_INTEGRATORS);
}

} // namespace ct
