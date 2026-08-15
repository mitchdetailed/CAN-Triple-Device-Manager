// Calculations > Timers — grid editor for Configuration::timerRows.
// Mirrors MoTeC's Timers dialog (Start/Stop tab + Settings tab).
#include "timers_dialog.h"

#include <QButtonGroup>
#include <QCheckBox>
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
#include <QRadioButton>
#include <QTabWidget>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>
#include <QWidget>

#include "../protocol/wire_structs.h"
#include "channel_field.h"
#include "select_channel_dialog.h"
#include "trimmed_spin_box.h"

namespace ct {

namespace {

// This dialog's working rows as a patch over the document. The grid writes back
// only on OK, so mid-session the document both lacks rows just added and still
// carries rows just deleted or re-pointed; the channel picker judges against
// this instead. Captured by value — the row editor outlives the call.
ConfigPatch livePatch(const QList<TimerRow> &rows)
{
    return [rows](Configuration &c) { c.timerRows = rows; };
}

// ---------------------------------------------------------------------------
// File-local row editor dialog (no Q_OBJECT needed — lambda connections only).
// ---------------------------------------------------------------------------
class TimerRowEditor : public QDialog
{
public:
    TimerRowEditor(Configuration *config, const TimerRow &row, const ConfigPatch &livePatch,
                   QWidget *parent)
        : QDialog(parent), m_config(config), m_row(row), m_livePatch(livePatch)
    {
        setWindowTitle(QObject::tr("Timer"));
        setModal(true);

        auto *mainLayout = new QVBoxLayout(this);

        auto *tabs = new QTabWidget(this);

        // --- Start / Stop tab ----------------------------------------------
        {
            auto *page = new QWidget(tabs);
            auto *form = new QFormLayout(page);

            auto *outRow = new QHBoxLayout;
            m_outputEdit = new QLineEdit(page);
            m_outputEdit->setReadOnly(true);
            auto *outSelect = new QPushButton(QObject::tr("Select…"), page);
            outRow->addWidget(m_outputEdit, 1);
            outRow->addWidget(outSelect);
            form->addRow(QObject::tr("Output Channel :"), outRow);

            auto *startRow = new QHBoxLayout;
            m_startEdit = new QLineEdit(page);
            m_startEdit->setReadOnly(true);
            auto *startSelect = new QPushButton(QObject::tr("Select…"), page);
            startRow->addWidget(m_startEdit, 1);
            startRow->addWidget(startSelect);
            form->addRow(QObject::tr("Start timer on channel :"), startRow);

            auto *stopRow = new QHBoxLayout;
            m_stopEdit = new QLineEdit(page);
            m_stopEdit->setReadOnly(true);
            auto *stopSelect = new QPushButton(QObject::tr("Select…"), page);
            stopRow->addWidget(m_stopEdit, 1);
            stopRow->addWidget(stopSelect);
            form->addRow(QObject::tr("Stop timer on channel :"), stopRow);

            auto *note = new QLabel(
                QObject::tr("Timer runs while started; each input triggers on its "
                            "rising edge (value > 0)."),
                page);
            note->setWordWrap(true);
            form->addRow(QString(), note);

            // The timer writes its output channel and reads its two triggers.
            QObject::connect(outSelect, &QPushButton::clicked, this, [this] {
                const QString picked = SelectChannelDialog::pickOutput(
                    m_config, ct::channelField(m_outputEdit), this, m_livePatch);
                if (!picked.isEmpty())
                    ct::setChannelField(m_outputEdit, picked, m_config->catalog());
            });
            QObject::connect(startSelect, &QPushButton::clicked, this, [this] {
                const QString picked = SelectChannelDialog::pickInput(
                    m_config, ct::channelField(m_startEdit), this, m_livePatch);
                if (!picked.isEmpty())
                    ct::setChannelField(m_startEdit, picked, m_config->catalog());
            });
            QObject::connect(stopSelect, &QPushButton::clicked, this, [this] {
                const QString picked = SelectChannelDialog::pickInput(
                    m_config, ct::channelField(m_stopEdit), this, m_livePatch);
                if (!picked.isEmpty())
                    ct::setChannelField(m_stopEdit, picked, m_config->catalog());
            });

            tabs->addTab(page, QObject::tr("Start / Stop"));
        }

        // --- Settings tab ---------------------------------------------------
        {
            auto *page = new QWidget(tabs);
            auto *layout = new QVBoxLayout(page);

            // Sequence
            auto *seqGroup = new QGroupBox(QObject::tr("Sequence"), page);
            auto *seqLayout = new QVBoxLayout(seqGroup);
            m_countUpRadio = new QRadioButton(QObject::tr("Count up"), seqGroup);
            m_countDownRadio = new QRadioButton(QObject::tr("Count down"), seqGroup);
            auto *seqButtons = new QButtonGroup(this);
            seqButtons->addButton(m_countUpRadio);
            seqButtons->addButton(m_countDownRadio);
            seqLayout->addWidget(m_countUpRadio);
            seqLayout->addWidget(m_countDownRadio);
            layout->addWidget(seqGroup);

            // Limit
            auto *limitGroup = new QGroupBox(QObject::tr("Limit"), page);
            auto *limitForm = new QFormLayout(limitGroup);
            m_limitSpin = new TrimmedDoubleSpinBox(limitGroup);
            m_limitSpin->setRange(0, 1e9);
            m_limitSpin->setDecimals(6);
            limitForm->addRow(QObject::tr("Value :"), m_limitSpin);
            m_rolloverCheck =
                new QCheckBox(QObject::tr("Roll over when limit exceeded"), limitGroup);
            limitForm->addRow(QString(), m_rolloverCheck);
            layout->addWidget(limitGroup);

            // Start setting
            auto *startGroup = new QGroupBox(QObject::tr("Start Setting"), page);
            auto *startForm = new QFormLayout(startGroup);
            m_setOnStartCheck =
                new QCheckBox(QObject::tr("Enable start setting"), startGroup);
            startForm->addRow(QString(), m_setOnStartCheck);
            m_startValueSpin = new TrimmedDoubleSpinBox(startGroup);
            m_startValueSpin->setRange(-1e9, 1e9);
            m_startValueSpin->setDecimals(6);
            startForm->addRow(QObject::tr("When started, set value to :"), m_startValueSpin);
            layout->addWidget(startGroup);

            // Stop setting
            auto *stopGroup = new QGroupBox(QObject::tr("Stop Setting"), page);
            auto *stopForm = new QFormLayout(stopGroup);
            m_setOnStopCheck =
                new QCheckBox(QObject::tr("Enable stop setting"), stopGroup);
            stopForm->addRow(QString(), m_setOnStopCheck);
            m_stopValueSpin = new TrimmedDoubleSpinBox(stopGroup);
            m_stopValueSpin->setRange(-1e9, 1e9);
            m_stopValueSpin->setDecimals(6);
            stopForm->addRow(QObject::tr("When stopped, set value to :"), m_stopValueSpin);
            layout->addWidget(stopGroup);

            layout->addStretch(1);

            QObject::connect(m_setOnStartCheck, &QCheckBox::toggled,
                             m_startValueSpin, &QWidget::setEnabled);
            QObject::connect(m_setOnStopCheck, &QCheckBox::toggled,
                             m_stopValueSpin, &QWidget::setEnabled);

            tabs->addTab(page, QObject::tr("Settings"));
        }

        mainLayout->addWidget(tabs);

        // --- Active + buttons ----------------------------------------------
        m_activeCheck = new QCheckBox(QObject::tr("Active"), this);
        mainLayout->addWidget(m_activeCheck);

        auto *buttons = new QDialogButtonBox(
            QDialogButtonBox::Ok | QDialogButtonBox::Cancel, Qt::Horizontal, this);
        mainLayout->addWidget(buttons);

        QObject::connect(buttons, &QDialogButtonBox::accepted, this, [this] { onOk(); });
        QObject::connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

        // --- Load the row ---------------------------------------------------
        ct::setChannelField(m_outputEdit, m_row.outputChannel, m_config->catalog());
        ct::setChannelField(m_startEdit, m_row.startChannel, m_config->catalog());
        ct::setChannelField(m_stopEdit, m_row.stopChannel, m_config->catalog());
        if (m_row.countDown)
            m_countDownRadio->setChecked(true);
        else
            m_countUpRadio->setChecked(true);
        m_limitSpin->setValue(m_row.limitValue);
        m_rolloverCheck->setChecked(m_row.rollover);
        m_setOnStartCheck->setChecked(m_row.setOnStart);
        m_startValueSpin->setValue(m_row.startValue);
        m_startValueSpin->setEnabled(m_row.setOnStart);
        m_setOnStopCheck->setChecked(m_row.setOnStop);
        m_stopValueSpin->setValue(m_row.stopValue);
        m_stopValueSpin->setEnabled(m_row.setOnStop);
        m_activeCheck->setChecked(m_row.active);

        resize(460, sizeHint().height());
    }

    TimerRow result() const { return m_row; }

private:
    void onOk()
    {
        if (ct::channelField(m_outputEdit).isEmpty()) {
            QMessageBox::warning(this, QObject::tr("Timer"),
                                 QObject::tr("Please select an output channel."));
            return;
        }
        m_row.outputChannel = ct::channelField(m_outputEdit);
        m_row.startChannel = ct::channelField(m_startEdit);
        m_row.stopChannel = ct::channelField(m_stopEdit);
        m_row.countDown = m_countDownRadio->isChecked();
        m_row.limitValue = m_limitSpin->value();
        m_row.rollover = m_rolloverCheck->isChecked();
        m_row.setOnStart = m_setOnStartCheck->isChecked();
        m_row.startValue = m_startValueSpin->value();
        m_row.setOnStop = m_setOnStopCheck->isChecked();
        m_row.stopValue = m_stopValueSpin->value();
        m_row.active = m_activeCheck->isChecked();
        accept();
    }

    Configuration *m_config;
    TimerRow m_row;
    ConfigPatch m_livePatch;

    QLineEdit *m_outputEdit = nullptr;
    QLineEdit *m_startEdit = nullptr;
    QLineEdit *m_stopEdit = nullptr;
    QRadioButton *m_countUpRadio = nullptr;
    QRadioButton *m_countDownRadio = nullptr;
    QDoubleSpinBox *m_limitSpin = nullptr;
    QCheckBox *m_rolloverCheck = nullptr;
    QCheckBox *m_setOnStartCheck = nullptr;
    QDoubleSpinBox *m_startValueSpin = nullptr;
    QCheckBox *m_setOnStopCheck = nullptr;
    QDoubleSpinBox *m_stopValueSpin = nullptr;
    QCheckBox *m_activeCheck = nullptr;
};

} // namespace

// ---------------------------------------------------------------------------
// TimersDialog
// ---------------------------------------------------------------------------
TimersDialog::TimersDialog(Configuration *config, QWidget *parent)
    : QDialog(parent), m_config(config), m_rows(config->timerRows)
{
    setWindowTitle(tr("Timers"));
    setModal(true);
    resize(700, 400);

    m_tree = new QTreeWidget(this);
    m_tree->setColumnCount(6);
    m_tree->setHeaderLabels({ tr("#"), tr("Active"), tr("Output"),
                              tr("Start"), tr("Stop"), tr("Mode") });
    m_tree->setRootIsDecorated(false);
    m_tree->setAllColumnsShowFocus(true);
    m_tree->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->header()->setStretchLastSection(true);
    m_tree->setColumnWidth(0, 36);
    m_tree->setColumnWidth(1, 56);
    m_tree->setColumnWidth(2, 150);
    m_tree->setColumnWidth(3, 150);
    m_tree->setColumnWidth(4, 150);

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

    connect(m_addButton, &QPushButton::clicked, this, &TimersDialog::onAdd);
    connect(m_changeButton, &QPushButton::clicked, this, &TimersDialog::onChange);
    connect(m_removeButton, &QPushButton::clicked, this, &TimersDialog::onRemove);
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem *, int) { onChange(); });
    connect(m_tree, &QTreeWidget::itemSelectionChanged, this,
            &TimersDialog::updateButtons);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        m_config->timerRows = m_rows;
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

void TimersDialog::rebuild()
{
    const int selected = m_tree->indexOfTopLevelItem(m_tree->currentItem());
    const ChannelCatalog &catalog = m_config->catalog();
    m_tree->clear();
    for (int i = 0; i < m_rows.size(); ++i) {
        const TimerRow &row = m_rows.at(i);
        auto *item = new QTreeWidgetItem(m_tree);
        item->setText(0, QString::number(i + 1));
        item->setText(1, row.active ? tr("Yes") : tr("No"));
        // Display only — the row a tree item stands for is its position, which
        // onChange()/onRemove() look up with indexOfTopLevelItem(), so nothing
        // reads these cells back as a channel name.
        item->setText(2, catalog.labelFor(row.outputChannel));
        item->setText(3, catalog.labelFor(row.startChannel));
        item->setText(4, catalog.labelFor(row.stopChannel));
        item->setText(5, row.countDown ? tr("Count down") : tr("Count up"));
    }
    if (selected >= 0 && selected < m_tree->topLevelItemCount())
        m_tree->setCurrentItem(m_tree->topLevelItem(selected));
}

void TimersDialog::onAdd()
{
    if (m_rows.size() >= MAX_TIMERS) {
        QMessageBox::warning(this, windowTitle(),
                             tr("The device supports at most %1 timers.")
                                 .arg(MAX_TIMERS));
        return;
    }
    TimerRowEditor editor(m_config, TimerRow(), livePatch(m_rows), this);
    if (editor.exec() == QDialog::Accepted) {
        m_rows.append(editor.result());
        rebuild();
        m_tree->setCurrentItem(m_tree->topLevelItem(m_tree->topLevelItemCount() - 1));
        updateButtons();
    }
}

void TimersDialog::onChange()
{
    const int index = m_tree->indexOfTopLevelItem(m_tree->currentItem());
    if (index < 0 || index >= m_rows.size())
        return;
    TimerRowEditor editor(m_config, m_rows.at(index), livePatch(m_rows), this);
    if (editor.exec() == QDialog::Accepted) {
        m_rows[index] = editor.result();
        rebuild();
        updateButtons();
    }
}

void TimersDialog::onRemove()
{
    const int index = m_tree->indexOfTopLevelItem(m_tree->currentItem());
    if (index < 0 || index >= m_rows.size())
        return;
    m_rows.removeAt(index);
    rebuild();
    updateButtons();
}

void TimersDialog::updateButtons()
{
    // QTreeWidget keeps a non-null currentItem() after the selection clears, so
    // gate on an actual highlighted row.
    const bool hasSelection = !m_tree->selectedItems().isEmpty();
    m_changeButton->setEnabled(hasSelection);
    m_removeButton->setEnabled(hasSelection);
}

} // namespace ct
