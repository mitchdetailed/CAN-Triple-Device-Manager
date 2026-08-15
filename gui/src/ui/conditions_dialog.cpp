// Implementation of the "Conditions" grid editor (Calculations menu).
// A condition is a boolean logic channel: it evaluates "A op B" and drives its
// output channel true (1) while the comparison holds, false (0) otherwise.
#include "conditions_dialog.h"

#include <QButtonGroup>
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
#include <QRadioButton>
#include <QScreen>
#include <QScrollArea>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include "../protocol/wire_structs.h"
#include "channel_field.h"
#include "select_channel_dialog.h"
#include "trimmed_spin_box.h"

namespace ct {

namespace {

QString formatNumber(double v)
{
    return QString::number(v, 'g', 9);
}

// One "A op B" comparison's text, e.g. "Engine RPM < 1500".
QString conditionTermText(const ConditionTermRow &t)
{
    const QStringList ops = ConditionsDialog::opNames();
    const QString op = (t.op >= 0 && t.op < ops.size()) ? ops.at(t.op) : QStringLiteral("?");
    const QString b = t.bIsChannel ? t.bChannel : formatNumber(t.bConst);
    return QStringLiteral("%1 %2 %3").arg(t.aChannel, op, b);
}

// The whole expression, bracketed left to right exactly as the device folds it.
QString conditionText(const ConditionRow &row)
{
    QStringList parts;
    for (const ConditionTermRow &t : row.terms)
        parts << conditionTermText(t);
    return joinConditionTerms(parts, row.joiners, QStringLiteral("AND"), QStringLiteral("OR"));
}

// This dialog's working rows as a patch over the document. The grid writes back
// only on OK, so mid-session the document both lacks rows just added and still
// carries rows just deleted or re-pointed; the channel picker judges against
// this instead. Captured by value — the row editor outlives the call.
ConfigPatch livePatch(const QList<ConditionRow> &rows)
{
    return [rows](Configuration &c) { c.conditionRows = rows; };
}

// ---------------------------------------------------------------------------
// File-local row editor dialog (no Q_OBJECT needed — lambda connections only).
// ---------------------------------------------------------------------------
class ConditionRowEditor : public QDialog
{
public:
    ConditionRowEditor(Configuration *config, const ConditionRow &row,
                       const ConfigPatch &livePatch, QWidget *parent)
        : QDialog(parent), m_config(config), m_row(row), m_livePatch(livePatch)
    {
        setWindowTitle(tr("Condition"));
        setModal(true);

        // Three comparison groups make a tall body — around 680 px, which fits
        // a 1080p screen at 100% but NOT at 150% scaling, where the usable
        // height is only ~720 logical px. So the body scrolls and the button
        // box stays outside it, always reachable.
        m_content = new QWidget;
        auto *mainLayout = new QVBoxLayout(m_content);
        auto *intro = new QLabel(
            tr("The output channel is true (1) while the condition holds, else false (0). "
               "Comparisons are combined left to right — with three, the first two are "
               "evaluated together first."),
            m_content);
        intro->setWordWrap(true);
        mainLayout->addWidget(intro);

        // --- How many comparisons ------------------------------------------
        auto *countRow = new QHBoxLayout;
        countRow->addWidget(new QLabel(tr("Number of comparisons:"), this));
        m_countCombo = new QComboBox(this);
        for (int n = 1; n <= COND_MAX_TERMS; ++n)
            m_countCombo->addItem(QString::number(n));
        countRow->addWidget(m_countCombo);
        countRow->addStretch(1);
        mainLayout->addLayout(countRow);

        // --- The comparisons, with an AND/OR selector between each pair -----
        for (int i = 0; i < COND_MAX_TERMS; ++i) {
            if (i > 0) {
                auto *joinRow = new QHBoxLayout;
                auto *joinCombo = new QComboBox(this);
                joinCombo->addItem(tr("AND"), int(COND_JOIN_AND));
                joinCombo->addItem(tr("OR"), int(COND_JOIN_OR));
                joinRow->addStretch(1);
                joinRow->addWidget(joinCombo);
                joinRow->addStretch(1);
                auto *joinHolder = new QWidget(this);
                joinHolder->setLayout(joinRow);
                mainLayout->addWidget(joinHolder);
                m_joinCombos.append(joinCombo);
                m_joinHolders.append(joinHolder);
                connect(joinCombo, &QComboBox::currentIndexChanged, this,
                        [this](int) { updatePreview(); });
            }
            m_terms.append(buildTerm(i, mainLayout));
        }

        // --- Live preview of the expression as the device will evaluate it --
        m_previewLabel = new QLabel(this);
        m_previewLabel->setWordWrap(true);
        m_previewLabel->setTextFormat(Qt::PlainText);
        mainLayout->addWidget(m_previewLabel);

        // --- Output channel ------------------------------------------------
        auto *outForm = new QFormLayout;
        auto *outRow = new QHBoxLayout;
        m_outputEdit = new QLineEdit(this);
        m_outputEdit->setReadOnly(true);
        m_outputSelect = new QPushButton(tr("Select…"), this);
        outRow->addWidget(m_outputEdit, 1);
        outRow->addWidget(m_outputSelect);
        outForm->addRow(tr("Output channel:"), outRow);
        mainLayout->addLayout(outForm);

        // --- Active + buttons ----------------------------------------------
        m_activeCheck = new QCheckBox(tr("Active"), this);
        mainLayout->addWidget(m_activeCheck);
        mainLayout->addStretch(1);

        m_buttons = new QDialogButtonBox(
            QDialogButtonBox::Ok | QDialogButtonBox::Cancel, Qt::Horizontal, this);
        auto *buttons = m_buttons;

        auto *scroll = new QScrollArea(this);
        scroll->setWidget(m_content);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        auto *outer = new QVBoxLayout(this);
        outer->addWidget(scroll, 1);
        outer->addWidget(m_buttons);

        connect(m_outputSelect, &QPushButton::clicked, this, [this] {
            // The condition writes this channel — output guard rails.
            const QString picked = SelectChannelDialog::pickOutput(
                m_config, ct::channelField(m_outputEdit), this, m_livePatch);
            if (!picked.isEmpty())
                ct::setChannelField(m_outputEdit, picked, m_config->catalog());
        });
        connect(m_countCombo, &QComboBox::currentIndexChanged, this,
                [this](int) { updateTermVisibility(); });
        connect(buttons, &QDialogButtonBox::accepted, this, [this] { onOk(); });
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

        // --- Load the row --------------------------------------------------
        const int count = qBound(1, int(m_row.terms.size()), COND_MAX_TERMS);
        m_countCombo->setCurrentIndex(count - 1);
        for (int i = 0; i < COND_MAX_TERMS; ++i)
            loadTerm(m_terms[i], m_row.terms.value(i, ConditionTermRow{}),
                     m_config->catalog());
        for (int i = 0; i < m_joinCombos.size(); ++i) {
            const int join = m_row.joiners.value(i, int(COND_JOIN_AND));
            m_joinCombos[i]->setCurrentIndex(join == int(COND_JOIN_OR) ? 1 : 0);
        }
        ct::setChannelField(m_outputEdit, m_row.outputChannel, m_config->catalog());
        m_activeCheck->setChecked(m_row.active);
        updateTermVisibility();

        resizeToContent(620);
    }

    ConditionRow row() const { return m_row; }

private:
    // The widgets of one comparison, kept together so the three are identical.
    struct TermWidgets {
        QGroupBox *box = nullptr;
        QLineEdit *aEdit = nullptr;
        QPushButton *aSelect = nullptr;
        QComboBox *opCombo = nullptr;
        QRadioButton *bChannelRadio = nullptr;
        QRadioButton *bConstRadio = nullptr;
        QLineEdit *bChannelEdit = nullptr;
        QPushButton *bSelect = nullptr;
        QDoubleSpinBox *bConstSpin = nullptr;
    };

    TermWidgets buildTerm(int index, QVBoxLayout *parentLayout)
    {
        TermWidgets w;
        w.box = new QGroupBox(tr("Comparison %1").arg(index + 1), this);
        auto *grid = new QGridLayout(w.box);

        w.aEdit = new QLineEdit(w.box);
        w.aEdit->setReadOnly(true);
        w.aSelect = new QPushButton(tr("Select…"), w.box);
        w.opCombo = new QComboBox(w.box);
        w.opCombo->addItems(ConditionsDialog::opNames());
        grid->addWidget(new QLabel(tr("Input A:"), w.box), 0, 0);
        grid->addWidget(w.aEdit, 0, 1);
        grid->addWidget(w.aSelect, 0, 2);
        grid->addWidget(w.opCombo, 0, 3);

        w.bChannelRadio = new QRadioButton(tr("Channel:"), w.box);
        w.bConstRadio = new QRadioButton(tr("Constant:"), w.box);
        // One button group PER comparison, or the three would be mutually
        // exclusive with each other instead of within themselves.
        auto *radios = new QButtonGroup(w.box);
        radios->addButton(w.bChannelRadio);
        radios->addButton(w.bConstRadio);
        w.bChannelEdit = new QLineEdit(w.box);
        w.bChannelEdit->setReadOnly(true);
        w.bSelect = new QPushButton(tr("Select…"), w.box);
        w.bConstSpin = new TrimmedDoubleSpinBox(w.box);
        w.bConstSpin->setRange(-1e9, 1e9);
        w.bConstSpin->setDecimals(6);
        grid->addWidget(w.bChannelRadio, 1, 0);
        grid->addWidget(w.bChannelEdit, 1, 1);
        grid->addWidget(w.bSelect, 1, 2);
        grid->addWidget(w.bConstRadio, 2, 0);
        grid->addWidget(w.bConstSpin, 2, 1);
        grid->setColumnStretch(1, 1);
        parentLayout->addWidget(w.box);

        // Both sides of a comparison are read, so both are input pickers.
        connect(w.aSelect, &QPushButton::clicked, this, [this, index] {
            TermWidgets &t = m_terms[index];
            const QString picked = SelectChannelDialog::pickInput(
                m_config, ct::channelField(t.aEdit), this, m_livePatch);
            if (!picked.isEmpty()) {
                ct::setChannelField(t.aEdit, picked, m_config->catalog());
                updatePreview();
            }
        });
        connect(w.bSelect, &QPushButton::clicked, this, [this, index] {
            TermWidgets &t = m_terms[index];
            const QString picked = SelectChannelDialog::pickInput(
                m_config, ct::channelField(t.bChannelEdit), this, m_livePatch);
            if (!picked.isEmpty()) {
                ct::setChannelField(t.bChannelEdit, picked, m_config->catalog());
                t.bChannelRadio->setChecked(true);
                updatePreview();
            }
        });
        connect(w.bChannelRadio, &QRadioButton::toggled, this, [this, index](bool) {
            updateTermEnables(m_terms[index]);
            updatePreview();
        });
        connect(w.opCombo, &QComboBox::currentIndexChanged, this,
                [this](int) { updatePreview(); });
        connect(w.bConstSpin, &QDoubleSpinBox::valueChanged, this,
                [this](double) { updatePreview(); });
        return w;
    }

    static void updateTermEnables(const TermWidgets &w)
    {
        const bool ch = w.bChannelRadio->isChecked();
        w.bChannelEdit->setEnabled(ch);
        w.bSelect->setEnabled(ch);
        w.bConstSpin->setEnabled(!ch);
    }

    static void loadTerm(const TermWidgets &w, const ConditionTermRow &t,
                         const ChannelCatalog &catalog)
    {
        ct::setChannelField(w.aEdit, t.aChannel, catalog);
        w.opCombo->setCurrentIndex(qBound(0, t.op, w.opCombo->count() - 1));
        if (t.bIsChannel) {
            w.bChannelRadio->setChecked(true);
            ct::setChannelField(w.bChannelEdit, t.bChannel, catalog);
        } else {
            w.bConstRadio->setChecked(true);
        }
        w.bConstSpin->setValue(t.bConst);
        updateTermEnables(w);
    }

    // The channel edits DISPLAY "Coolant Temp °C"; only channelField() gives back
    // the bare name that belongs in the row.
    static ConditionTermRow readTerm(const TermWidgets &w)
    {
        ConditionTermRow t;
        t.aChannel = ct::channelField(w.aEdit);
        t.op = w.opCombo->currentIndex();
        t.bIsChannel = w.bChannelRadio->isChecked();
        t.bChannel = ct::channelField(w.bChannelEdit);
        t.bConst = w.bConstSpin->value();
        return t;
    }

    int termCount() const { return m_countCombo->currentIndex() + 1; }

    void updateTermVisibility()
    {
        const int n = termCount();
        for (int i = 0; i < m_terms.size(); ++i)
            m_terms[i].box->setVisible(i < n);
        for (int i = 0; i < m_joinHolders.size(); ++i)
            m_joinHolders[i]->setVisible(i + 1 < n);
        updatePreview();
        // Shrink back down when comparisons are hidden — the layout's minimum
        // grows with the widgets, so without this the window keeps the tallest
        // size it ever had. Width is left alone so a manual resize survives.
        resizeToContent(width());
    }

    // Height the body wants, but never more than the screen can show. Anything
    // beyond that scrolls rather than pushing the buttons off the bottom.
    void resizeToContent(int targetWidth)
    {
        const QScreen *scr = screen();
        const int available = scr ? scr->availableGeometry().height() : 1040;
        const int maxHeight = available - 80; // title bar + a margin
        const int wanted = m_content->sizeHint().height()
                           + m_buttons->sizeHint().height() + 24;
        resize(targetWidth > 0 ? targetWidth : 620, qMin(wanted, maxHeight));
    }

    void updatePreview()
    {
        m_previewLabel->setText(tr("Evaluates as:  %1").arg(currentRow().terms.isEmpty()
                                                                ? QString()
                                                                : conditionText(currentRow())));
    }

    // The row as the dialog currently stands (used for the preview and on OK).
    ConditionRow currentRow() const
    {
        ConditionRow r = m_row;
        const int n = termCount();
        r.terms.clear();
        r.joiners.clear();
        for (int i = 0; i < n; ++i) {
            r.terms.append(readTerm(m_terms[i]));
            if (i + 1 < n)
                r.joiners.append(m_joinCombos[i]->currentData().toInt());
        }
        r.outputChannel = ct::channelField(m_outputEdit);
        r.active = m_activeCheck->isChecked();
        return r;
    }

    void onOk()
    {
        const int n = termCount();
        for (int i = 0; i < n; ++i) {
            const TermWidgets &w = m_terms[i];
            const QString which = n > 1 ? tr(" for comparison %1").arg(i + 1) : QString();
            if (ct::channelField(w.aEdit).isEmpty()) {
                QMessageBox::warning(this, tr("Condition"),
                                     tr("Select a channel for Input A%1.").arg(which));
                return;
            }
            if (w.bChannelRadio->isChecked() && ct::channelField(w.bChannelEdit).isEmpty()) {
                QMessageBox::warning(this, tr("Condition"),
                                     tr("Select a channel for Input B%1.").arg(which));
                return;
            }
        }
        if (ct::channelField(m_outputEdit).isEmpty()) {
            QMessageBox::warning(this, tr("Condition"), tr("Select an output channel."));
            return;
        }
        m_row = currentRow();
        accept();
    }

    Configuration *m_config;
    ConditionRow m_row;
    ConfigPatch m_livePatch;

    QWidget *m_content = nullptr; // scrolling body (everything but the buttons)
    QDialogButtonBox *m_buttons = nullptr;
    QComboBox *m_countCombo = nullptr;
    QList<TermWidgets> m_terms;
    QList<QComboBox *> m_joinCombos;
    QList<QWidget *> m_joinHolders;
    QLabel *m_previewLabel = nullptr;
    QLineEdit *m_outputEdit = nullptr;
    QPushButton *m_outputSelect = nullptr;
    QCheckBox *m_activeCheck = nullptr;
};

} // namespace

// ---------------------------------------------------------------------------
// ConditionsDialog
// ---------------------------------------------------------------------------

ConditionsDialog::ConditionsDialog(Configuration *config, QWidget *parent)
    : QDialog(parent), m_config(config), m_rows(config->conditionRows)
{
    setWindowTitle(tr("Conditions"));
    setModal(true);
    resize(620, 400);

    m_tree = new QTreeWidget(this);
    m_tree->setColumnCount(4);
    m_tree->setHeaderLabels({ tr("#"), tr("Active"), tr("Condition"), tr("Output channel") });
    m_tree->setRootIsDecorated(false);
    m_tree->setAllColumnsShowFocus(true);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->header()->setStretchLastSection(true);
    m_tree->setColumnWidth(0, 36);
    m_tree->setColumnWidth(1, 50);
    m_tree->setColumnWidth(2, 260);

    m_addButton = new QPushButton(tr("Add…"), this);
    m_changeButton = new QPushButton(tr("Change…"), this);
    m_removeButton = new QPushButton(tr("Remove"), this);

    auto *buttonBox = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, Qt::Horizontal, this);

    auto *sideLayout = new QVBoxLayout;
    sideLayout->addWidget(m_addButton);
    sideLayout->addWidget(m_changeButton);
    sideLayout->addWidget(m_removeButton);
    sideLayout->addStretch(1);

    auto *topLayout = new QHBoxLayout;
    topLayout->addWidget(m_tree, 1);
    topLayout->addLayout(sideLayout);

    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->addLayout(topLayout, 1);
    mainLayout->addWidget(buttonBox);

    connect(m_addButton, &QPushButton::clicked, this, &ConditionsDialog::onAdd);
    connect(m_changeButton, &QPushButton::clicked, this, &ConditionsDialog::onChange);
    connect(m_removeButton, &QPushButton::clicked, this, &ConditionsDialog::onRemove);
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem *, int) { onChange(); });
    connect(m_tree, &QTreeWidget::itemSelectionChanged, this,
            &ConditionsDialog::updateButtons);
    connect(buttonBox, &QDialogButtonBox::accepted, this, [this] {
        // Commit the working copy back into the document.
        m_config->conditionRows = m_rows;
        m_config->setDirty();
        accept();
    });
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
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

QStringList ConditionsDialog::opNames()
{
    return { QStringLiteral("="),
             QString(QChar(0x2260)),   // ≠
             QStringLiteral("<"),
             QString(QChar(0x2264)),   // ≤
             QStringLiteral(">"),
             QString(QChar(0x2265)) }; // ≥
}

void ConditionsDialog::rebuild()
{
    const int selected = m_tree->indexOfTopLevelItem(m_tree->currentItem());
    m_tree->clear();
    for (int i = 0; i < m_rows.size(); ++i) {
        const ConditionRow &row = m_rows.at(i);
        auto *item = new QTreeWidgetItem(m_tree);
        item->setText(0, QString::number(i + 1));
        item->setText(1, row.active ? tr("Yes") : tr("No"));
        item->setText(2, conditionText(row));
        // Pure display: the row a tree item stands for is its position, so the
        // output column can carry the unit. Column 2 is the expression and stays
        // bare — a unit between the channel and the operator would read as part
        // of the comparison ("Engine RPM RPM < 1500").
        item->setText(3, m_config->catalog().labelFor(row.outputChannel));
    }
    if (selected >= 0 && selected < m_tree->topLevelItemCount())
        m_tree->setCurrentItem(m_tree->topLevelItem(selected));
}

void ConditionsDialog::onAdd()
{
    if (m_rows.size() >= MAX_CONDITIONS) {
        QMessageBox::warning(this, tr("Conditions"),
                             tr("The device supports at most %1 conditions.")
                                 .arg(MAX_CONDITIONS));
        return;
    }
    ConditionRow row;
    ConditionRowEditor editor(m_config, row, livePatch(m_rows), this);
    if (editor.exec() == QDialog::Accepted) {
        m_rows.append(editor.row());
        rebuild();
        m_tree->setCurrentItem(m_tree->topLevelItem(m_tree->topLevelItemCount() - 1));
        updateButtons();
    }
}

void ConditionsDialog::onChange()
{
    const int idx = m_tree->indexOfTopLevelItem(m_tree->currentItem());
    if (idx < 0 || idx >= m_rows.size())
        return;
    ConditionRowEditor editor(m_config, m_rows.at(idx), livePatch(m_rows), this);
    if (editor.exec() == QDialog::Accepted) {
        m_rows[idx] = editor.row();
        rebuild();
    }
}

void ConditionsDialog::onRemove()
{
    const int idx = m_tree->indexOfTopLevelItem(m_tree->currentItem());
    if (idx < 0 || idx >= m_rows.size())
        return;
    m_rows.removeAt(idx);
    rebuild();
    updateButtons();
}

void ConditionsDialog::updateButtons()
{
    const bool hasSelection =
        m_tree->indexOfTopLevelItem(m_tree->currentItem()) >= 0;
    m_changeButton->setEnabled(hasSelection);
    m_removeButton->setEnabled(hasSelection);
}

} // namespace ct
