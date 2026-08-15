// Calculations > Up / Down Counters — grid editor for Configuration::counterRows.
#include "counters_dialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include "../protocol/wire_structs.h"
#include "channel_field.h"
#include "select_channel_dialog.h"
#include "trimmed_spin_box.h"

namespace ct {

namespace {

// Retained-counter budget: the device's preserve store holds this many values
// (PRESERVE_MAX in the firmware's preserve_store.h). Mirrored in validation.cpp.
constexpr int kMaxPreservedCounters = 20;

// This dialog's working rows as a patch over the document. The grid writes back
// only on OK, so mid-session the document both lacks rows just added and still
// carries rows just deleted or re-pointed; the channel picker judges against
// this instead. Captured by value — the row editor outlives the call.
ConfigPatch livePatch(const QList<CounterRow> &rows)
{
    return [rows](Configuration &c) { c.counterRows = rows; };
}

// ---------------------------------------------------------------------------
// Row editor dialog (file-local, no Q_OBJECT).
// ---------------------------------------------------------------------------
class CounterRowEditor : public QDialog
{
public:
    CounterRowEditor(Configuration *config, const CounterRow &row,
                     const ConfigPatch &livePatch, QWidget *parent)
        : QDialog(parent), m_config(config), m_row(row), m_livePatch(livePatch)
    {
        setWindowTitle(QObject::tr("Up / Down Counter Settings"));
        setModal(true);

        auto *mainLayout = new QVBoxLayout(this);

        // --- Output group --------------------------------------------------
        auto *outputGroup = new QGroupBox(QObject::tr("Output"), this);
        auto *outputForm = new QFormLayout(outputGroup);
        auto *outRow = new QHBoxLayout;
        m_outputEdit = new QLineEdit(outputGroup);
        m_outputEdit->setReadOnly(true);
        auto *outSelect = new QPushButton(QObject::tr("Select…"), outputGroup);
        outRow->addWidget(m_outputEdit, 1);
        outRow->addWidget(outSelect);
        outputForm->addRow(QObject::tr("Channel :"), outRow);
        mainLayout->addWidget(outputGroup);

        // --- Input group ---------------------------------------------------
        auto *inputGroup = new QGroupBox(QObject::tr("Input"), this);
        auto *inputForm = new QFormLayout(inputGroup);

        // Index IS the wire value (ct::COUNTER_MODE_*), so this order is not
        // cosmetic — see the mapper, which relies on it.
        m_typeCombo = new QComboBox(inputGroup);
        m_typeCombo->addItem(QObject::tr("Up / Down"));
        m_typeCombo->addItem(QObject::tr("Follow Changes"));
        m_typeCombo->addItem(QObject::tr("Every x Hz"));
        inputForm->addRow(QObject::tr("Type :"), m_typeCombo);

        buildChannelRow(inputForm, QObject::tr("Up :"), m_upEdit, m_upSelect);
        buildChannelRow(inputForm, QObject::tr("Down :"), m_downEdit, m_downSelect);
        buildChannelRow(inputForm, QObject::tr("Follow :"), m_followEdit, m_followSelect);

        // Rate-mode row: how often, and which way. Laid out side by side
        // because they are one sentence — "50 times a second, counting down".
        auto *rateRow = new QWidget(inputGroup);
        auto *rateLayout = new QHBoxLayout(rateRow);
        rateLayout->setContentsMargins(0, 0, 0, 0);
        m_rateCombo = new QComboBox(rateRow);
        for (int hz : ct::kCounterRateChoices)
            m_rateCombo->addItem(QObject::tr("%1 Hz").arg(hz), hz);
        m_rateDirCombo = new QComboBox(rateRow);
        m_rateDirCombo->addItem(QObject::tr("Increment"));
        m_rateDirCombo->addItem(QObject::tr("Decrement"));
        rateLayout->addWidget(m_rateCombo);
        rateLayout->addWidget(m_rateDirCombo);
        rateLayout->addStretch();
        m_rateLabel = new QLabel(QObject::tr("Rate :"), inputGroup);
        inputForm->addRow(m_rateLabel, rateRow);
        m_rateRow = rateRow;
        buildChannelRow(inputForm, QObject::tr("Reset :"), m_resetEdit, m_resetSelect);
        buildChannelRow(inputForm, QObject::tr("Enable :"), m_enableEdit, m_enableSelect);
        mainLayout->addWidget(inputGroup);

        // --- Options group -------------------------------------------------
        auto *optionsGroup = new QGroupBox(QObject::tr("Options"), this);
        auto *optionsForm = new QFormLayout(optionsGroup);

        m_minSpin = makeSpin(optionsGroup);
        m_maxSpin = makeSpin(optionsGroup);
        m_resetValueSpin = makeSpin(optionsGroup);
        m_stepSpin = makeSpin(optionsGroup);
        optionsForm->addRow(QObject::tr("Minimum value :"), m_minSpin);
        optionsForm->addRow(QObject::tr("Maximum value :"), m_maxSpin);
        optionsForm->addRow(QObject::tr("Reset value :"), m_resetValueSpin);
        optionsForm->addRow(QObject::tr("Step :"), m_stepSpin);

        m_rollCheck = new QCheckBox(QObject::tr("Roll at limits"), optionsGroup);
        m_preserveCheck = new QCheckBox(QObject::tr("Preserve value"), optionsGroup);
        m_preserveCheck->setToolTip(QObject::tr(
            "Keep this counter's value across power cycles.\n\n"
            "The value is written to a small flash store about once a minute, so up "
            "to a minute of counting can be lost on a sudden power cut, and it is "
            "reset whenever you send a changed configuration.\n\n"
            "At most %1 counters can be preserved. Requires a device whose flash "
            "has room for the retained-value store — where it does not, counters "
            "still run but start from their reset value at every power-up.")
                                        .arg(kMaxPreservedCounters));
        optionsForm->addRow(QString(), m_rollCheck);
        optionsForm->addRow(QString(), m_preserveCheck);
        mainLayout->addWidget(optionsGroup);

        // --- Active + buttons ----------------------------------------------
        m_activeCheck = new QCheckBox(QObject::tr("Active"), this);
        mainLayout->addWidget(m_activeCheck);
        mainLayout->addStretch(1);

        auto *buttons = new QDialogButtonBox(
            QDialogButtonBox::Ok | QDialogButtonBox::Cancel, Qt::Horizontal, this);
        mainLayout->addWidget(buttons);

        // --- Wiring --------------------------------------------------------
        // The counter writes its output channel and reads every trigger, so the
        // first picker is an output and the rest are inputs.
        wireSelect(outSelect, m_outputEdit, ChannelRole::Output);
        wireSelect(m_upSelect, m_upEdit, ChannelRole::Input);
        wireSelect(m_downSelect, m_downEdit, ChannelRole::Input);
        wireSelect(m_followSelect, m_followEdit, ChannelRole::Input);
        wireSelect(m_resetSelect, m_resetEdit, ChannelRole::Input);
        wireSelect(m_enableSelect, m_enableEdit, ChannelRole::Input);

        // Re-derive the value spinboxes' decimals whenever the output channel
        // changes — both the Select… flow and the initial load below set the
        // edit's text, so a textChanged hook covers both.
        QObject::connect(m_outputEdit, &QLineEdit::textChanged, this,
                         [this](const QString &) { applyOutputPrecision(); });
        QObject::connect(m_typeCombo, qOverload<int>(&QComboBox::currentIndexChanged),
                         this, [this](int) { updateTypeEnables(); });
        QObject::connect(buttons, &QDialogButtonBox::accepted, this, [this] { onOk(); });
        QObject::connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

        // --- Load the row --------------------------------------------------
        // Every one of these boxes shows "Name Unit" and remembers the bare name
        // (see channel_field.h) — they are read back with ct::channelField().
        ct::setChannelField(m_outputEdit, m_row.outputChannel, m_config->catalog());
        m_typeCombo->setCurrentIndex(qBound(0, m_row.mode, m_typeCombo->count() - 1));
        {
            const int rateIdx = m_rateCombo->findData(m_row.rateHz);
            m_rateCombo->setCurrentIndex(rateIdx >= 0 ? rateIdx : 0);
        }
        m_rateDirCombo->setCurrentIndex(m_row.rateCountDown ? 1 : 0);
        ct::setChannelField(m_upEdit, m_row.upChannel, m_config->catalog());
        ct::setChannelField(m_downEdit, m_row.downChannel, m_config->catalog());
        ct::setChannelField(m_followEdit, m_row.followChannel, m_config->catalog());
        ct::setChannelField(m_resetEdit, m_row.resetChannel, m_config->catalog());
        ct::setChannelField(m_enableEdit, m_row.enableChannel, m_config->catalog());
        m_minSpin->setValue(m_row.minValue);
        m_maxSpin->setValue(m_row.maxValue);
        m_resetValueSpin->setValue(m_row.resetValue);
        m_stepSpin->setValue(m_row.step);
        m_rollCheck->setChecked(m_row.rollAtLimits);
        m_preserveCheck->setChecked(m_row.preserveValue);
        m_activeCheck->setChecked(m_row.active);
        updateTypeEnables();

        resize(440, sizeHint().height());
    }

    CounterRow result() const { return m_row; }

private:
    void buildChannelRow(QFormLayout *form, const QString &label,
                         QLineEdit *&edit, QPushButton *&select)
    {
        auto *rowLayout = new QHBoxLayout;
        edit = new QLineEdit(form->parentWidget());
        edit->setReadOnly(true);
        select = new QPushButton(QObject::tr("Select…"), form->parentWidget());
        rowLayout->addWidget(edit, 1);
        rowLayout->addWidget(select);
        form->addRow(label, rowLayout);
    }

    QDoubleSpinBox *makeSpin(QWidget *parent)
    {
        auto *spin = new TrimmedDoubleSpinBox(parent);
        spin->setRange(-1e9, 1e9);
        // Decimals track the output channel's precision (see applyOutputPrecision);
        // 6 is a permissive fallback until a channel is chosen.
        spin->setDecimals(6);
        return spin;
    }

    // The counter's Minimum / Maximum / Reset / Step are all values of the output
    // channel, so they should only offer the decimal precision that channel
    // defines. Falls back to the permissive default when no channel is selected
    // yet (or the selected one is no longer in the catalog), so a stored value's
    // precision is never silently clamped away.
    void applyOutputPrecision()
    {
        const Channel ch =
            m_config->catalog().findByName(ct::channelField(m_outputEdit));
        const int decimals = ch.isValid() ? ch.decimalPlaces : 6;
        for (QDoubleSpinBox *spin : {m_minSpin, m_maxSpin, m_resetValueSpin, m_stepSpin})
            spin->setDecimals(decimals);
    }

    void wireSelect(QPushButton *button, QLineEdit *edit, ChannelRole role)
    {
        QObject::connect(button, &QPushButton::clicked, this, [this, edit, role] {
            const QString current = ct::channelField(edit);
            const QString picked =
                role == ChannelRole::Input
                    ? SelectChannelDialog::pickInput(m_config, current, this, m_livePatch)
                    : SelectChannelDialog::pickOutput(m_config, current, this, m_livePatch);
            if (!picked.isEmpty())
                ct::setChannelField(edit, picked, m_config->catalog());
        });
    }

    void updateTypeEnables()
    {
        const int mode = m_typeCombo->currentIndex();
        const bool follow = mode == ct::COUNTER_MODE_FOLLOW;
        const bool rate = mode == ct::COUNTER_MODE_RATE;
        // A rate counter is driven by the clock, so none of the three counting
        // inputs apply. They are disabled rather than hidden so the dialog does
        // not resize as the type changes; the rate row is hidden instead,
        // because it has nothing to say in the other two modes.
        m_upEdit->setEnabled(!follow && !rate);
        m_upSelect->setEnabled(!follow && !rate);
        m_downEdit->setEnabled(!follow && !rate);
        m_downSelect->setEnabled(!follow && !rate);
        m_followEdit->setEnabled(follow);
        m_followSelect->setEnabled(follow);
        m_rateLabel->setVisible(rate);
        m_rateRow->setVisible(rate);
        // Reset and Enable are always enabled — a rate counter can still be
        // gated and zeroed, which is most of what makes one useful.
    }

    void onOk()
    {
        if (ct::channelField(m_outputEdit).isEmpty()) {
            QMessageBox::warning(this, QObject::tr("Up / Down Counter Settings"),
                                 QObject::tr("Please select an output channel."));
            return;
        }
        m_row.outputChannel = ct::channelField(m_outputEdit);
        m_row.mode = m_typeCombo->currentIndex();
        m_row.rateHz = m_rateCombo->currentData().toInt();
        m_row.rateCountDown = m_rateDirCombo->currentIndex() == 1;
        m_row.upChannel = ct::channelField(m_upEdit);
        m_row.downChannel = ct::channelField(m_downEdit);
        m_row.followChannel = ct::channelField(m_followEdit);
        m_row.resetChannel = ct::channelField(m_resetEdit);
        m_row.enableChannel = ct::channelField(m_enableEdit);
        m_row.minValue = m_minSpin->value();
        m_row.maxValue = m_maxSpin->value();
        m_row.resetValue = m_resetValueSpin->value();
        m_row.step = m_stepSpin->value();
        m_row.rollAtLimits = m_rollCheck->isChecked();
        m_row.preserveValue = m_preserveCheck->isChecked();
        m_row.active = m_activeCheck->isChecked();
        accept();
    }

    Configuration *m_config;
    CounterRow m_row;
    ConfigPatch m_livePatch;

    QLineEdit *m_outputEdit = nullptr;
    QComboBox *m_typeCombo = nullptr;
    QLineEdit *m_upEdit = nullptr;
    QPushButton *m_upSelect = nullptr;
    QLineEdit *m_downEdit = nullptr;
    QPushButton *m_downSelect = nullptr;
    QLineEdit *m_followEdit = nullptr;
    QPushButton *m_followSelect = nullptr;
    QLineEdit *m_resetEdit = nullptr;
    QPushButton *m_resetSelect = nullptr;
    QLineEdit *m_enableEdit = nullptr;
    QPushButton *m_enableSelect = nullptr;
    QDoubleSpinBox *m_minSpin = nullptr;
    QDoubleSpinBox *m_maxSpin = nullptr;
    QDoubleSpinBox *m_resetValueSpin = nullptr;
    QDoubleSpinBox *m_stepSpin = nullptr;
    QCheckBox *m_rollCheck = nullptr;
    QCheckBox *m_preserveCheck = nullptr;
    QCheckBox *m_activeCheck = nullptr;
    QComboBox *m_rateCombo = nullptr;
    QComboBox *m_rateDirCombo = nullptr;
    QLabel *m_rateLabel = nullptr;
    QWidget *m_rateRow = nullptr;
};

// Display only — the row itself is found by index, never by parsing this back,
// so the channel names here carry their units.
QString inputsSummary(const CounterRow &row, const ChannelCatalog &catalog)
{
    QStringList parts;
    if (row.mode == ct::COUNTER_MODE_RATE) {
        parts << QStringLiteral("%1 Hz %2")
                     .arg(row.rateHz)
                     .arg(row.rateCountDown ? QStringLiteral("down") : QStringLiteral("up"));
    } else if (row.mode == ct::COUNTER_MODE_FOLLOW) {
        if (!row.followChannel.isEmpty())
            parts << QStringLiteral("follow=%1").arg(catalog.labelFor(row.followChannel));
    } else {
        if (!row.upChannel.isEmpty())
            parts << QStringLiteral("up=%1").arg(catalog.labelFor(row.upChannel));
        if (!row.downChannel.isEmpty())
            parts << QStringLiteral("down=%1").arg(catalog.labelFor(row.downChannel));
    }
    if (!row.resetChannel.isEmpty())
        parts << QStringLiteral("reset=%1").arg(catalog.labelFor(row.resetChannel));
    if (!row.enableChannel.isEmpty())
        parts << QStringLiteral("en=%1").arg(catalog.labelFor(row.enableChannel));
    return parts.join(QLatin1Char(' '));
}

} // namespace

// ---------------------------------------------------------------------------
// CountersDialog
// ---------------------------------------------------------------------------
CountersDialog::CountersDialog(Configuration *config, QWidget *parent)
    : QDialog(parent), m_config(config), m_rows(config->counterRows)
{
    setWindowTitle(tr("Up / Down Counters"));
    setModal(true);
    resize(700, 400);

    m_tree = new QTreeWidget(this);
    m_tree->setColumnCount(5);
    m_tree->setHeaderLabels({ tr("#"), tr("Active"), tr("Output"),
                              tr("Type"), tr("Inputs") });
    m_tree->setRootIsDecorated(false);
    m_tree->setAllColumnsShowFocus(true);
    m_tree->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->header()->setStretchLastSection(true);
    m_tree->setColumnWidth(0, 36);
    m_tree->setColumnWidth(1, 56);
    m_tree->setColumnWidth(2, 150);
    m_tree->setColumnWidth(3, 90);

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

    // Running budget for the retained-value store, so the limit is visible
    // while you work rather than only as a validation error after the fact.
    m_preserveLabel = new QLabel(this);
    m_preserveLabel->setWordWrap(true);

    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->addLayout(topLayout, 1);
    mainLayout->addWidget(m_preserveLabel);
    mainLayout->addWidget(buttons);

    connect(m_addButton, &QPushButton::clicked, this, &CountersDialog::onAdd);
    connect(m_changeButton, &QPushButton::clicked, this, &CountersDialog::onChange);
    connect(m_removeButton, &QPushButton::clicked, this, &CountersDialog::onRemove);
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem *, int) { onChange(); });
    connect(m_tree, &QTreeWidget::itemSelectionChanged, this,
            &CountersDialog::updateButtons);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        m_config->counterRows = m_rows;
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

void CountersDialog::updatePreserveBudget()
{
    int used = 0;
    for (const CounterRow &row : m_rows)
        if (row.active && row.preserveValue)
            ++used;
    if (used == 0) {
        m_preserveLabel->setText(
            tr("No counters are preserved across power cycles (up to %1 can be).")
                .arg(kMaxPreservedCounters));
        m_preserveLabel->setStyleSheet(QString());
        return;
    }
    m_preserveLabel->setText(tr("Preserved across power cycles: %1 of %2.%3")
                                 .arg(used)
                                 .arg(kMaxPreservedCounters)
                                 .arg(used > kMaxPreservedCounters
                                          ? tr("  ⚠ Over the limit — the device keeps only %1, "
                                               "so turn Preserve off on %2 of them.")
                                                .arg(kMaxPreservedCounters)
                                                .arg(used - kMaxPreservedCounters)
                                          : QString()));
    m_preserveLabel->setStyleSheet(used > kMaxPreservedCounters
                                       ? QStringLiteral("color: #C03000;")
                                       : QString());
}

void CountersDialog::rebuild()
{
    const int selected = m_tree->indexOfTopLevelItem(m_tree->currentItem());
    m_tree->clear();
    for (int i = 0; i < m_rows.size(); ++i) {
        const CounterRow &row = m_rows.at(i);
        auto *item = new QTreeWidgetItem(m_tree);
        item->setText(0, QString::number(i + 1));
        item->setText(1, row.active ? tr("Yes") : tr("No"));
        // Column 2/4 are display: the edited row is found by index (see onChange),
        // so no one reads a channel name back out of the tree.
        item->setText(2, m_config->catalog().labelFor(row.outputChannel));
        item->setText(3, row.mode == ct::COUNTER_MODE_RATE     ? tr("Rate")
                         : row.mode == ct::COUNTER_MODE_FOLLOW ? tr("Follow")
                                                               : tr("Up/Down"));
        item->setText(4, inputsSummary(row, m_config->catalog()));
    }
    if (selected >= 0 && selected < m_tree->topLevelItemCount())
        m_tree->setCurrentItem(m_tree->topLevelItem(selected));
    updatePreserveBudget();
}

void CountersDialog::onAdd()
{
    if (m_rows.size() >= MAX_COUNTERS) {
        QMessageBox::warning(this, windowTitle(),
                             tr("The device supports at most %1 counters.")
                                 .arg(MAX_COUNTERS));
        return;
    }
    CounterRowEditor editor(m_config, CounterRow(), livePatch(m_rows), this);
    if (editor.exec() == QDialog::Accepted) {
        m_rows.append(editor.result());
        rebuild();
        m_tree->setCurrentItem(m_tree->topLevelItem(m_tree->topLevelItemCount() - 1));
        updateButtons();
    }
}

void CountersDialog::onChange()
{
    const int idx = m_tree->indexOfTopLevelItem(m_tree->currentItem());
    if (idx < 0 || idx >= m_rows.size())
        return;
    CounterRowEditor editor(m_config, m_rows.at(idx), livePatch(m_rows), this);
    if (editor.exec() == QDialog::Accepted) {
        m_rows[idx] = editor.result();
        rebuild();
        updateButtons();
    }
}

void CountersDialog::onRemove()
{
    const int idx = m_tree->indexOfTopLevelItem(m_tree->currentItem());
    if (idx < 0 || idx >= m_rows.size())
        return;
    m_rows.removeAt(idx);
    rebuild();
    updateButtons();
}

void CountersDialog::updateButtons()
{
    // QTreeWidget keeps a non-null currentItem() after the selection clears, so
    // gate on an actual highlighted row.
    const bool hasSelection = !m_tree->selectedItems().isEmpty();
    m_changeButton->setEnabled(hasSelection);
    m_removeButton->setEnabled(hasSelection);
}

} // namespace ct
