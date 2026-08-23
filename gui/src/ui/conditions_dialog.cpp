// Implementation of the "Conditions" grid editor (Calculations menu).
// A User Condition drives a boolean output channel from one or two expressions:
// Momentary pulses the output on the rising edge of Set and drops it again by
// itself, Set/Reset latches it between Set and Reset. See ConditionRow.
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
#include <QSignalBlocker>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVariant>
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

// The Momentary hold, offered as the frequency the device stores. Deliberately
// its own list rather than a reuse of ct::kCounterRateChoices, which happens to
// hold the same numbers for an unrelated reason (steps per second) and is free
// to change without this dialog noticing.
// The Compare kind that means an ordinary channel comparison. Not a
// ConditionOp value — the six operators are all valid ops, so the sentinel has
// to sit outside their range.
constexpr int kKindChannel = -1;

constexpr int kLatchHzChoices[] = {1, 2, 5, 10, 20, 50, 100};

constexpr bool latchChoicesFit()
{
    for (int hz : kLatchHzChoices)
        if (hz < 1 || hz > int(COND_LATCH_MAX_HZ))
            return false;
    return true;
}
static_assert(latchChoicesFit(), "a latch frequency the device cannot store");

// The hold a frequency buys, which is what the user is actually choosing —
// 10 Hz is 100 ms — because 1/f is not obvious and getting it wrong is a
// condition that fires for a tenth of the time its author expected.
QString latchItemText(int hz)
{
    return QObject::tr("%1 Hz (%2 ms)").arg(hz).arg(1000 / qMax(1, hz));
}

// The (bus, name) pair a message operand is stored as — 1..3 and the section's
// NAME, never an index. See ConditionTermRow::aMessage for why.
struct MessageRef {
    int bus = 0; // 1..3; 0 = unset
    QString name;
};

QVariant messageData(const MessageRef &ref)
{
    return QVariant(QVariantList{ref.bus, ref.name});
}

MessageRef messageFromData(const QVariant &data)
{
    const QVariantList packed = data.toList();
    return {packed.value(0).toInt(), packed.value(1).toString()};
}

// "CAN 1 · Section 3 · Rx · Engine Data" — where the message sits and what it
// is called, in the vocabulary the Communications list already uses.
//
// The section NUMBER earns its place: a section name is not unique, not even
// within one bus, and it is the only handle this reference has. Two messages
// called "Engine Data" on CAN 1 are indistinguishable without it, and the
// number is where the user goes to tell them apart. It is display only — the
// stored reference is the bus and the name — so reordering the list relabels
// the entry rather than re-pointing it.
//
// NO CAN ID, EVER. This label is built for every message in the document,
// including ones this viewer may not read, and a concealed message's identifier
// is exactly what CommsSection::isConcealed exists to withhold. Keeping the
// label to what the sections list already shows means the combo cannot leak it
// whatever the tier; anything more a message is allowed to say about itself
// comes from displayDetail(), which decides that question in one place for the
// whole application.
QString messageLabel(const MessageRef &ref, int sectionIndex, bool isTransmit)
{
    if (ref.bus < 1 || ref.name.isEmpty())
        return QString();
    if (sectionIndex < 0) // no longer in the document; see the (missing) entry
        return QStringLiteral("CAN %1 · %2").arg(ref.bus).arg(ref.name);
    return QStringLiteral("CAN %1 · Section %2 · %3 · %4")
        .arg(ref.bus)
        .arg(sectionIndex + 1)
        .arg(isTransmit ? QStringLiteral("Tx") : QStringLiteral("Rx"), ref.name);
}

// The same label for a reference held by a condition, looked up in the document.
// Falls back to bus and name when the section has gone, which is what a dangling
// reference has left to say for itself.
QString messageLabel(const Configuration *config, const MessageRef &ref)
{
    if (config && ref.bus >= 1 && ref.bus <= 3) {
        const QList<CommsSection> &sections = config->bus[ref.bus - 1].sections;
        for (int i = 0; i < sections.size(); ++i)
            if (sections[i].name.compare(ref.name, Qt::CaseInsensitive) == 0)
                return messageLabel(ref, i, sections[i].isTransmit());
    }
    return messageLabel(ref, -1, false);
}

// The message operators read as a phrase, not a symbol: "CAN 1 · Engine Data
// was received" is the whole term, and there is no right-hand operand to put
// after it.
QString messageOpText(int op)
{
    return op == int(COND_OP_MSG_TX) ? QObject::tr("was transmitted")
                                     : QObject::tr("was received");
}

// One comparison's text, e.g. "Engine RPM < 1500" or "CAN 1 · Engine Data was
// received".
QString conditionTermText(const Configuration *config, const ConditionTermRow &t)
{
    if (t.isMessageOp())
        return QStringLiteral("%1 %2").arg(messageLabel(config, {t.aMessageBus, t.aMessage}),
                                           messageOpText(t.op));
    const QStringList ops = ConditionsDialog::opNames();
    const QString op = (t.op >= 0 && t.op < ops.size()) ? ops.at(t.op) : QStringLiteral("?");
    const QString b = t.bIsChannel ? t.bChannel : formatNumber(t.bConst);
    return QStringLiteral("%1 %2 %3").arg(t.aChannel, op, b);
}

// One expression, bracketed left to right exactly as the device folds it.
// joinConditionTerms() is shared with the report and with the Reset preview, so
// no two renderings of an expression can disagree about the grouping.
QString conditionExprText(const Configuration *config, const QList<ConditionTermRow> &terms,
                          const QList<int> &joiners)
{
    QStringList parts;
    for (const ConditionTermRow &t : terms)
        parts << conditionTermText(config, t);
    return joinConditionTerms(parts, joiners, QStringLiteral("AND"), QStringLiteral("OR"));
}

// The tree's Condition column. The mode leads, because the same Set expression
// means two different things under the two modes and the column is the only
// place the difference shows without opening the row.
QString conditionSummary(const Configuration *config, const ConditionRow &row)
{
    const QString setText = conditionExprText(config, row.setTerms, row.setJoiners);
    if (row.mode == ConditionMode::Momentary)
        return QObject::tr("Momentary: %1").arg(setText);
    return QObject::tr("Set: %1  /  Reset: %2")
        .arg(setText, conditionExprText(config, row.resetTerms, row.resetJoiners));
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
        setWindowTitle(tr("User Condition"));
        setModal(true);

        // TWO expression groups of three comparisons each — a much taller body
        // than the single expression that already did not fit a 1080p screen at
        // 150% scaling, where the usable height is only ~720 logical px. So the
        // body scrolls and the button box stays outside it, always reachable.
        m_content = new QWidget;
        auto *mainLayout = new QVBoxLayout(m_content);
        auto *intro = new QLabel(
            tr("The output channel is true (1) while the condition holds, else false (0), "
               "and is always a Boolean channel — its data type, range and decimals are "
               "set for you. Comparisons are combined left to right — with three, the "
               "first two are evaluated together first. A comparison can also ask whether "
               "a CAN message was received or transmitted, which is true only on the pass "
               "the frame actually happened."),
            m_content);
        intro->setWordWrap(true);
        mainLayout->addWidget(intro);

        // --- Mode ----------------------------------------------------------
        auto *modeRow = new QHBoxLayout;
        modeRow->addWidget(new QLabel(tr("Mode:"), this));
        m_modeCombo = new QComboBox(this);
        // Index IS ct::ConditionMode, so this order is not cosmetic.
        m_modeCombo->addItem(tr("Momentary"));
        m_modeCombo->addItem(tr("Set / Reset"));
        modeRow->addWidget(m_modeCombo);
        modeRow->addStretch(1);
        mainLayout->addLayout(modeRow);

        // What the chosen mode actually does. The two behave completely
        // differently for the same expression, and "Reset wins" is the part
        // nobody guesses.
        m_modeHint = new QLabel(this);
        m_modeHint->setWordWrap(true);
        mainLayout->addWidget(m_modeHint);

        // --- Latch frequency (Momentary only) ------------------------------
        m_latchHolder = new QWidget(this);
        auto *latchRow = new QHBoxLayout(m_latchHolder);
        latchRow->setContentsMargins(0, 0, 0, 0);
        latchRow->addWidget(new QLabel(tr("Latch Frequency:"), m_latchHolder));
        m_latchCombo = new QComboBox(m_latchHolder);
        for (int hz : kLatchHzChoices)
            m_latchCombo->addItem(latchItemText(hz), hz);
        latchRow->addWidget(m_latchCombo);
        latchRow->addStretch(1);
        mainLayout->addWidget(m_latchHolder);

        // --- The two expressions -------------------------------------------
        buildExpression(m_set, tr("Set"), mainLayout);
        buildExpression(m_reset, tr("Reset"), mainLayout);

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
        connect(m_modeCombo, &QComboBox::currentIndexChanged, this,
                [this](int) { updateMode(); });
        connect(buttons, &QDialogButtonBox::accepted, this, [this] { onOk(); });
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

        // --- Load the row --------------------------------------------------
        m_modeCombo->setCurrentIndex(m_row.mode == ConditionMode::Momentary ? 0 : 1);
        // A latch frequency this build does not offer — a hand-edited file, or
        // one a later build wrote — is kept as its own entry rather than snapped
        // to the nearest offered one. Opening a condition and pressing OK must
        // not silently retime it.
        if (m_latchCombo->findData(m_row.latchHz) < 0)
            m_latchCombo->addItem(latchItemText(m_row.latchHz), m_row.latchHz);
        m_latchCombo->setCurrentIndex(qMax(0, m_latchCombo->findData(m_row.latchHz)));
        // BOTH halves are loaded whatever the mode is: the mode only decides
        // which one is shown, never which one survives.
        loadExpression(m_set, m_row.setTerms, m_row.setJoiners);
        loadExpression(m_reset, m_row.resetTerms, m_row.resetJoiners);

        loadQualify(m_set, m_row.qualifySetMs, m_row.qualifySetTerms);
        loadQualify(m_reset, m_row.qualifyResetMs, m_row.qualifyResetTerms);
        ct::setChannelField(m_outputEdit, m_row.outputChannel, m_config->catalog());
        m_activeCheck->setChecked(m_row.active);
        updateMode();

        resizeToContent(620);
    }

    ConditionRow row() const { return m_row; }

private:
    // The widgets of one comparison, kept together so all six are identical.
    struct TermWidgets {
        QGroupBox *box = nullptr;
        QLabel *kindLabel = nullptr;
        QComboBox *kindCombo = nullptr; // Channel / Message received / transmitted
        QLabel *aLabel = nullptr;
        QLineEdit *aEdit = nullptr;
        QPushButton *aSelect = nullptr;
        QComboBox *aMessageCombo = nullptr;
        QComboBox *opCombo = nullptr;
        QRadioButton *bChannelRadio = nullptr;
        QRadioButton *bConstRadio = nullptr;
        QLineEdit *bChannelEdit = nullptr;
        QPushButton *bSelect = nullptr;
        QDoubleSpinBox *bConstSpin = nullptr;
        // "for" — does the duration below apply to THIS comparison? Only the Set
        // expression's terms carry one; Reset is never qualified.
        QCheckBox *qualifyCheck = nullptr;
    };

    // One expression's widgets. Set and Reset are the same editor twice, built,
    // loaded, read and validated by the same code so the two cannot drift into
    // disagreeing about what a comparison is.
    struct Expr {
        QString title; // "Set" / "Reset", for the validation messages
        QGroupBox *box = nullptr;
        QComboBox *countCombo = nullptr;
        QList<TermWidgets> terms;
        QList<QComboBox *> joinCombos;
        QList<QWidget *> joinHolders;
        QLabel *preview = nullptr;
        // The "for" qualifier, one per expression. Set and Reset each own one
        // and each own clock; the two never share, which is what lets a
        // condition set at once and clear only after the fault has been gone a
        // while.
        QCheckBox *qualifyCheck = nullptr;
        QDoubleSpinBox *qualifySpin = nullptr;
        QLabel *qualifyHint = nullptr;
    };

    void buildExpression(Expr &e, const QString &title, QVBoxLayout *parentLayout)
    {
        e.title = title;
        e.box = new QGroupBox(title, this);
        auto *layout = new QVBoxLayout(e.box);

        // --- How many comparisons ------------------------------------------
        auto *countRow = new QHBoxLayout;
        countRow->addWidget(new QLabel(tr("Number of comparisons:"), e.box));
        e.countCombo = new QComboBox(e.box);
        for (int n = 1; n <= COND_MAX_TERMS; ++n)
            e.countCombo->addItem(QString::number(n));
        countRow->addWidget(e.countCombo);
        countRow->addStretch(1);
        layout->addLayout(countRow);

        // --- The comparisons, with an AND/OR selector between each pair -----
        for (int i = 0; i < COND_MAX_TERMS; ++i) {
            if (i > 0) {
                auto *joinRow = new QHBoxLayout;
                auto *joinCombo = new QComboBox(e.box);
                joinCombo->addItem(tr("AND"), int(COND_JOIN_AND));
                joinCombo->addItem(tr("OR"), int(COND_JOIN_OR));
                joinRow->addStretch(1);
                joinRow->addWidget(joinCombo);
                joinRow->addStretch(1);
                auto *joinHolder = new QWidget(e.box);
                joinHolder->setLayout(joinRow);
                layout->addWidget(joinHolder);
                e.joinCombos.append(joinCombo);
                e.joinHolders.append(joinHolder);
            }
            e.terms.append(buildTerm(e, i, layout));
        }

        // --- Live preview of the expression as the device will evaluate it --
        // --- The "for" qualifier -------------------------------------------
        //
        // Inside the group box, because it belongs to THIS expression: a
        // duration floating above both would read as applying to the condition
        // and there would be no way to say "set at once, clear after five
        // seconds", which is the shape this exists for.
        auto *qualifyRow = new QHBoxLayout;
        e.qualifyCheck = new QCheckBox(tr("Only when true for :"), e.box);
        qualifyRow->addWidget(e.qualifyCheck);
        e.qualifySpin = new QDoubleSpinBox(e.box);
        // 655.35 s is the device's ceiling, not a taste: the duration travels as
        // centiseconds in a u16. The 0.01 s step matches the calculation tick,
        // so what is typed here is exactly what the device counts.
        e.qualifySpin->setRange(0.01, 655.35);
        e.qualifySpin->setDecimals(2);
        e.qualifySpin->setSingleStep(0.01);
        e.qualifySpin->setValue(1.0);
        // NO STEPPER ARROWS. Stepping 0.01 at a time is not how anyone reaches
        // thirty seconds, so the buttons cost width and offer nothing; the box
        // is typed into. Keyboard stepping still works for whoever wants it —
        // NoButtons hides the arrows, it does not disable the behaviour.
        e.qualifySpin->setButtonSymbols(QAbstractSpinBox::NoButtons);
        // Twice the width it asks for, measured AFTER the range, the decimals
        // and the button symbols are set, because all three feed sizeHint().
        // Doubling a hint computed before them would double the wrong number.
        e.qualifySpin->setMinimumWidth(e.qualifySpin->sizeHint().width() * 2);
        qualifyRow->addWidget(e.qualifySpin);
        // "seconds" as a word beside the box rather than a suffix inside it:
        // a suffix shares the editing area, so it moves as digits are typed and
        // sits behind the cursor. Outside, the unit holds still.
        qualifyRow->addWidget(new QLabel(tr("seconds"), e.box));
        e.qualifyHint = new QLabel(e.box);
        e.qualifyHint->setWordWrap(true);
        qualifyRow->addWidget(e.qualifyHint, 1);
        layout->addLayout(qualifyRow);

        e.preview = new QLabel(e.box);
        e.preview->setWordWrap(true);
        e.preview->setTextFormat(Qt::PlainText);
        layout->addWidget(e.preview);

        parentLayout->addWidget(e.box);

        {
            Expr *qe = &e;
            connect(e.qualifyCheck, &QCheckBox::toggled, this,
                    [this, qe](bool) { syncQualify(*qe); });
            for (const TermWidgets &w : e.terms)
                if (w.qualifyCheck)
                    connect(w.qualifyCheck, &QCheckBox::toggled, this,
                            [this, qe](bool) { syncQualify(*qe); });
        }

        // Wired only now that the term list is complete: appending to it
        // reallocates, so a connection made mid-build would capture a pointer
        // into storage that has since moved.
        Expr *ep = &e;
        connect(e.countCombo, &QComboBox::currentIndexChanged, this,
                [this, ep](int) { updateTermVisibility(*ep); });
        for (QComboBox *joinCombo : e.joinCombos)
            connect(joinCombo, &QComboBox::currentIndexChanged, this,
                    [this](int) { updatePreviews(); });
        for (int i = 0; i < e.terms.size(); ++i)
            wireTerm(e, i);
    }

    TermWidgets buildTerm(const Expr &e, int index, QVBoxLayout *parentLayout)
    {
        TermWidgets w;
        w.box = new QGroupBox(tr("Comparison %1").arg(index + 1), e.box);
        auto *grid = new QGridLayout(w.box);

        // WHAT this comparison is, before anything about its operands. First
        // control of the row because it is the first decision, and because the
        // alternative — leaving it in the operator list — put "was received"
        // among =, < and > where nobody setting up a message trigger thought to
        // look for it.
        w.kindLabel = new QLabel(tr("Compare:"), w.box);
        w.kindCombo = new QComboBox(w.box);
        w.kindCombo->addItem(tr("Channel value"), kKindChannel);
        w.kindCombo->addItem(tr("Message received"), int(COND_OP_MSG_RX));
        w.kindCombo->setItemData(w.kindCombo->count() - 1,
                                 tr("True only on the evaluation pass in which a frame for the "
                                    "chosen message arrives — an event, not a level. Every "
                                    "received frame gets its own pass, so none is missed."),
                                 Qt::ToolTipRole);
        w.kindCombo->addItem(tr("Message transmitted"), int(COND_OP_MSG_TX));
        w.kindCombo->setItemData(w.kindCombo->count() - 1,
                                 tr("True only on the evaluation pass in which a frame for the "
                                    "chosen message goes out. A transmission is noticed by the "
                                    "next pass, up to 10 ms later, and two of the same message "
                                    "inside one 10 ms window count as a single event."),
                                 Qt::ToolTipRole);

        w.aLabel = new QLabel(tr("Input A:"), w.box);
        w.aEdit = new QLineEdit(w.box);
        w.aEdit->setReadOnly(true);
        w.aSelect = new QPushButton(tr("Select…"), w.box);
        // The message selector occupies the SAME cells as the channel picker
        // because it is the same operand seen through a different operator;
        // exactly one of the two is ever visible.
        w.aMessageCombo = new QComboBox(w.box);
        w.opCombo = new QComboBox(w.box);
        const QStringList ops = ConditionsDialog::opNames();
        for (int op = 0; op < ops.size(); ++op)
            w.opCombo->addItem(ops.at(op), op);
        // ONE THING PER ROW, top to bottom in the order the comparison is read:
        //
        //     Compare:   [what kind]
        //     Input A :  [channel] [Select...]
        //                [operator]
        //     (o) Channel:  [channel] [Select...]
        //     (o) Constant: [value]
        //     [ ] for the time set below
        //
        // The operator used to sit at the end of the Input A row and the "for"
        // tick after it, which read as a sentence running off the right-hand
        // edge and put the duration — a property of the whole comparison —
        // between the two operands it does not belong to. Down the page each
        // line answers one question, and the tick lands after the thing it
        // qualifies rather than in the middle of it.
        grid->addWidget(w.kindLabel, 0, 0);
        grid->addWidget(w.kindCombo, 0, 1, 1, 3);
        grid->addWidget(w.aLabel, 1, 0);
        grid->addWidget(w.aEdit, 1, 1);
        grid->addWidget(w.aSelect, 1, 2);
        grid->addWidget(w.aMessageCombo, 1, 1, 1, 2);
        grid->addWidget(w.opCombo, 2, 1);
        // "below", not "above": the duration control sits under the comparisons
        // in this group box, and a tick that pointed the wrong way would send
        // somebody hunting for a field that is not there.
        w.qualifyCheck = new QCheckBox(tr("for the time set below"), w.box);
        w.qualifyCheck->setToolTip(tr("Require this comparison alone to hold for the "
                                      "duration set below. Leave every comparison "
                                      "unticked and the duration applies to the whole "
                                      "expression instead."));

        w.bChannelRadio = new QRadioButton(tr("Channel:"), w.box);
        w.bConstRadio = new QRadioButton(tr("Constant:"), w.box);
        // One button group PER comparison, or the six would be mutually
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
        grid->addWidget(w.bChannelRadio, 3, 0);
        grid->addWidget(w.bChannelEdit, 3, 1);
        grid->addWidget(w.bSelect, 3, 2);
        grid->addWidget(w.bConstRadio, 4, 0);
        grid->addWidget(w.bConstSpin, 4, 1);
        // LAST, because it qualifies the comparison above it rather than any one
        // operand — and because it is the only control here that can be absent.
        grid->addWidget(w.qualifyCheck, 5, 0, 1, 4);
        grid->setColumnStretch(1, 1);
        parentLayout->addWidget(w.box);
        return w;
    }

    void wireTerm(Expr &e, int index)
    {
        Expr *ep = &e;
        const TermWidgets &w = e.terms.at(index);

        // Both sides of a comparison are read, so both are input pickers.
        connect(w.aSelect, &QPushButton::clicked, this, [this, ep, index] {
            TermWidgets &t = ep->terms[index];
            const QString picked = SelectChannelDialog::pickInput(
                m_config, ct::channelField(t.aEdit), this, m_livePatch);
            if (!picked.isEmpty()) {
                ct::setChannelField(t.aEdit, picked, m_config->catalog());
                updatePreviews();
            }
        });
        connect(w.bSelect, &QPushButton::clicked, this, [this, ep, index] {
            TermWidgets &t = ep->terms[index];
            const QString picked = SelectChannelDialog::pickInput(
                m_config, ct::channelField(t.bChannelEdit), this, m_livePatch);
            if (!picked.isEmpty()) {
                ct::setChannelField(t.bChannelEdit, picked, m_config->catalog());
                t.bChannelRadio->setChecked(true);
                updatePreviews();
            }
        });
        connect(w.bChannelRadio, &QRadioButton::toggled, this, [this, ep, index](bool) {
            updateTermEnables(ep->terms[index]);
            updatePreviews();
        });
        connect(w.kindCombo, &QComboBox::currentIndexChanged, this, [this, ep, index](int) {
            // The kind decides what input A even is, and which direction of
            // message may be offered, so the selector is rebuilt here rather
            // than once at load.
            updateTermOperand(ep->terms[index]);
            // Changing the kind changes whether this comparison's tick counts,
            // and hiding a checkbox emits no toggled(), so nothing else would
            // recompute the caption. Without this, switching to Message
            // received leaves "the ticked comparison(s) only" on screen with no
            // tick visible anywhere to refer to — and switching back re-arms a
            // still-checked tick under a caption saying the opposite.
            syncQualify(*ep);
            updatePreviews();
            resizeToContent(width()); // the comparison's height changes with it
        });
        connect(w.opCombo, &QComboBox::currentIndexChanged, this,
                [this](int) { updatePreviews(); });
        connect(w.aMessageCombo, &QComboBox::currentIndexChanged, this,
                [this](int) { updatePreviews(); });
        connect(w.bConstSpin, &QDoubleSpinBox::valueChanged, this,
                [this](double) { updatePreviews(); });
    }

    // The op a term actually carries: the message operator when the kind says
    // so, otherwise whichever comparison is selected. One place, so no caller
    // has to remember that two combos share the question.
    static int currentOp(const TermWidgets &w)
    {
        const int kind = w.kindCombo->currentData().toInt();
        return kind == kKindChannel ? w.opCombo->currentData().toInt() : kind;
    }

    static MessageRef currentMessage(const TermWidgets &w)
    {
        return messageFromData(w.aMessageCombo->currentData());
    }

    // Names are matched case-insensitively, the way every other (bus, name)
    // reference into the document is; findData() would compare exactly and
    // would drop a message whose name differs only in case.
    static int indexOfMessage(const QComboBox *combo, const MessageRef &ref)
    {
        if (ref.bus < 1 || ref.name.isEmpty())
            return -1;
        for (int i = 0; i < combo->count(); ++i) {
            const MessageRef item = messageFromData(combo->itemData(i));
            if (item.bus == ref.bus && item.name.compare(ref.name, Qt::CaseInsensitive) == 0)
                return i;
        }
        return -1;
    }

    // The messages this term may name: receive sections for "was received",
    // transmit sections for "was transmitted", across all three buses. Rebuilt
    // whenever the operator changes, because the two operators offer different
    // halves of the document.
    void populateMessages(TermWidgets &w)
    {
        const MessageRef current = currentMessage(w);
        const bool wantTransmit = currentOp(w) == int(COND_OP_MSG_TX);
        const QSignalBlocker block(w.aMessageCombo); // rebuilding is not a user edit
        w.aMessageCombo->clear();
        for (int b = 0; b < 3; ++b) {
            const QList<CommsSection> &sections = m_config->bus[b].sections;
            for (int i = 0; i < sections.size(); ++i) {
                const CommsSection &s = sections[i];
                if (wantTransmit ? !s.isTransmit() : !s.isReceive())
                    continue;
                const MessageRef ref{b + 1, s.name};
                w.aMessageCombo->addItem(messageLabel(ref, i, s.isTransmit()),
                                         messageData(ref));
                // The item text is the name alone (see messageLabel), so nothing
                // withheld can escape through it. The tooltip asks displayDetail
                // rather than composing its own line, because that function is
                // the one place allowed to decide what a marked message may say
                // about itself — here it prints the ID for an ordinary message
                // and "receive (hidden)" for a concealed one.
                w.aMessageCombo->setItemData(w.aMessageCombo->count() - 1,
                                             s.displayDetail(m_config->isSectionRevealed(s, b)),
                                             Qt::ToolTipRole);
            }
        }
        const int found = indexOfMessage(w.aMessageCombo, current);
        if (found >= 0) {
            w.aMessageCombo->setCurrentIndex(found);
        } else if (current.bus >= 1 && !current.name.isEmpty()) {
            // A message this document no longer offers — removed since, renamed,
            // turned around to the other direction, or read back from a device
            // this build could not reconcile. Kept as a selectable entry rather
            // than silently dropped: losing it here would quietly re-point the
            // condition at a different message the next time anyone opened it,
            // and validation is where a dangling reference should be reported.
            w.aMessageCombo->addItem(tr("%1 (missing)").arg(messageLabel(m_config, current)),
                                     messageData(current));
            w.aMessageCombo->setCurrentIndex(w.aMessageCombo->count() - 1);
        }
        if (w.aMessageCombo->count() == 0)
            w.aMessageCombo->addItem(wantTransmit ? tr("(no transmit messages defined)")
                                                  : tr("(no receive messages defined)"));
    }

    // Which operand widgets a comparison shows. A message operator replaces
    // input A with a message selector and drops input B outright — it takes no
    // right operand, and a disabled-but-visible B would invite the user to fill
    // in something the device never reads.
    void updateTermOperand(TermWidgets &w)
    {
        const bool message = condOpIsMessage(quint8(currentOp(w)));
        w.aLabel->setText(message ? tr("Message:") : tr("Input A:"));
        w.opCombo->setVisible(!message);
        w.aEdit->setVisible(!message);
        w.aSelect->setVisible(!message);
        w.aMessageCombo->setVisible(message);
        w.bChannelRadio->setVisible(!message);
        w.bConstRadio->setVisible(!message);
        w.bChannelEdit->setVisible(!message);
        w.bSelect->setVisible(!message);
        w.bConstSpin->setVisible(!message);
        // NO "for" ON A MESSAGE COMPARISON. A message operator is an EVENT: true
        // for exactly the pass in which the frame happened and false on the
        // next. "Held continuously for two seconds" is not a thing it can ever
        // be, so a tick offering it would be offering a condition that can never
        // come true — worse than a missing control, because it looks like it
        // works. The whole-expression duration still applies and still makes
        // sense: it is the FOLDED result that has to hold.
        if (w.qualifyCheck)
            w.qualifyCheck->setVisible(!message);
        // The message list is left alone while a comparison operator is
        // selected, for the reason the hidden expression is left alone when the
        // mode changes: switching operator to look at the other kind of term and
        // switching back must not cost the user what they had picked.
        if (message)
            populateMessages(w);
        updateTermEnables(w);
    }

    static void updateTermEnables(const TermWidgets &w)
    {
        const bool ch = w.bChannelRadio->isChecked();
        w.bChannelEdit->setEnabled(ch);
        w.bSelect->setEnabled(ch);
        w.bConstSpin->setEnabled(!ch);
    }

    void loadTerm(TermWidgets &w, const ConditionTermRow &t)
    {
        const ChannelCatalog &catalog = m_config->catalog();
        ct::setChannelField(w.aEdit, t.aChannel, catalog);
        const bool message = condOpIsMessage(quint8(t.op));
        w.kindCombo->setCurrentIndex(
            qMax(0, w.kindCombo->findData(message ? t.op : kKindChannel)));
        // A message term leaves the comparison combo where it was rather than
        // forcing it to an operator the term does not use, so switching a
        // comparison to a message and back keeps what was chosen.
        if (!message)
            w.opCombo->setCurrentIndex(qMax(0, w.opCombo->findData(t.op)));
        // Seeded before the list is built, because populateMessages() keeps
        // whatever the combo currently names — including one that has gone
        // missing from the document.
        w.aMessageCombo->clear();
        if (t.aMessageBus >= 1 && !t.aMessage.isEmpty()) {
            const MessageRef ref{t.aMessageBus, t.aMessage};
            w.aMessageCombo->addItem(messageLabel(m_config, ref), messageData(ref));
        }
        if (t.bIsChannel) {
            w.bChannelRadio->setChecked(true);
            ct::setChannelField(w.bChannelEdit, t.bChannel, catalog);
        } else {
            w.bConstRadio->setChecked(true);
        }
        w.bConstSpin->setValue(t.bConst);
        updateTermOperand(w);
    }

    // The channel edits DISPLAY "Coolant Temp °C"; only channelField() gives back
    // the bare name that belongs in the row.
    static ConditionTermRow readTerm(const TermWidgets &w)
    {
        ConditionTermRow t;
        t.aChannel = ct::channelField(w.aEdit);
        t.op = currentOp(w);
        t.bIsChannel = w.bChannelRadio->isChecked();
        t.bChannel = ct::channelField(w.bChannelEdit);
        t.bConst = w.bConstSpin->value();
        // The message operand is read whatever the operator is, and so is the
        // channel one: a term that was a message test a moment ago still holds
        // the channel its author picked before that, and neither is thrown away
        // by looking at the other. ConditionTermRow::toJson writes only the half
        // the operator actually uses.
        const MessageRef msg = currentMessage(w);
        t.aMessageBus = msg.bus;
        t.aMessage = msg.name;
        return t;
    }

    void loadExpression(Expr &e, const QList<ConditionTermRow> &terms, const QList<int> &joiners)
    {
        const int count = qBound(1, int(terms.size()), COND_MAX_TERMS);
        e.countCombo->setCurrentIndex(count - 1);
        for (int i = 0; i < e.terms.size(); ++i)
            loadTerm(e.terms[i], terms.value(i, ConditionTermRow{}));
        for (int i = 0; i < e.joinCombos.size(); ++i) {
            const int join = joiners.value(i, int(COND_JOIN_AND));
            e.joinCombos[i]->setCurrentIndex(join == int(COND_JOIN_OR) ? 1 : 0);
        }
    }

    void readExpression(const Expr &e, QList<ConditionTermRow> *terms, QList<int> *joiners) const
    {
        terms->clear();
        joiners->clear();
        const int n = termCount(e);
        for (int i = 0; i < n; ++i) {
            terms->append(readTerm(e.terms.at(i)));
            if (i + 1 < n)
                joiners->append(e.joinCombos.at(i)->currentData().toInt());
        }
    }

    static int termCount(const Expr &e) { return e.countCombo->currentIndex() + 1; }

    ConditionMode currentMode() const
    {
        return m_modeCombo->currentIndex() == int(ConditionMode::Momentary)
                   ? ConditionMode::Momentary
                   : ConditionMode::SetReset;
    }

    void updateTermVisibility(Expr &e)
    {
        const int n = termCount(e);
        for (int i = 0; i < e.terms.size(); ++i)
            e.terms[i].box->setVisible(i < n);
        for (int i = 0; i < e.joinHolders.size(); ++i)
            e.joinHolders[i]->setVisible(i + 1 < n);
        syncQualify(e); // a hidden comparison's tick stops counting, so say so
        updatePreviews();
        // Shrink back down when comparisons are hidden — the layout's minimum
        // grows with the widgets, so without this the window keeps the tallest
        // size it ever had. Width is left alone so a manual resize survives.
        resizeToContent(width());
    }

    void updateMode()
    {
        const bool momentary = currentMode() == ConditionMode::Momentary;
        m_modeHint->setText(
            momentary
                ? tr("The output goes to 1 the moment the Set expression becomes true and "
                     "returns to 0 by itself one latch period later. It retriggers: becoming "
                     "true again while the output is held restarts the hold.")
                : tr("Set drives the output to 1, Reset drives it to 0, and it holds its last "
                     "value in between. Reset wins — while both are true the output is 0."));
        m_latchHolder->setVisible(momentary);
        m_reset.box->setVisible(!momentary);
        // Only the visibility changes. Both expressions stay loaded and are both
        // read back on OK, so switching modes never costs the user the half they
        // are not looking at.
        updateTermVisibility(m_set);
        updateTermVisibility(m_reset);
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

    void updatePreviews()
    {
        const ConditionRow r = currentRow();
        m_set.preview->setText(
            tr("Evaluates as:  %1")
                .arg(conditionExprText(m_config, r.setTerms, r.setJoiners)));
        m_reset.preview->setText(
            tr("Evaluates as:  %1")
                .arg(conditionExprText(m_config, r.resetTerms, r.resetJoiners)));
    }

    // The row as the dialog currently stands (used for the previews and on OK).
    ConditionRow currentRow() const
    {
        ConditionRow r = m_row;
        r.mode = currentMode();
        // BOTH halves, whatever the mode. The one the mode does not use is still
        // the user's work, and ConditionRow says so: the unused expression is
        // kept rather than cleared, and mapToDevice is what decides which one
        // reaches the device.
        readExpression(m_set, &r.setTerms, &r.setJoiners);
        readExpression(m_reset, &r.resetTerms, &r.resetJoiners);
        r.latchHz = m_latchCombo->currentData().toInt();
        readQualify(m_set, &r.qualifySetMs, &r.qualifySetTerms);
        readQualify(m_reset, &r.qualifyResetMs, &r.qualifyResetTerms);
        r.outputChannel = ct::channelField(m_outputEdit);
        r.active = m_activeCheck->isChecked();
        return r;
    }

    // Every comparison the mode actually uses has to be finished. An unfinished
    // one is not a reference to something ungenerated — it is a comparison
    // against nothing, which the device evaluates against whatever signal index
    // zero happens to be. Validation says the same about a saved document; this
    // is what stops it being written in the first place.
    bool checkExpression(const Expr &e)
    {
        const int n = termCount(e);
        for (int i = 0; i < n; ++i) {
            const TermWidgets &w = e.terms.at(i);
            const QString where = n > 1 ? tr("%1 comparison %2").arg(e.title).arg(i + 1)
                                        : tr("the %1 expression").arg(e.title);
            if (condOpIsMessage(quint8(currentOp(w)))) {
                if (currentMessage(w).name.isEmpty()) {
                    QMessageBox::warning(this, tr("User Condition"),
                                         tr("Select a message for %1.").arg(where));
                    return false;
                }
                continue; // a message operator has no right-hand operand to check
            }
            if (ct::channelField(w.aEdit).isEmpty()) {
                QMessageBox::warning(this, tr("User Condition"),
                                     tr("Select a channel for Input A of %1.").arg(where));
                return false;
            }
            if (w.bChannelRadio->isChecked() && ct::channelField(w.bChannelEdit).isEmpty()) {
                QMessageBox::warning(this, tr("User Condition"),
                                     tr("Select a channel for Input B of %1.").arg(where));
                return false;
            }
        }
        return true;
    }

    void onOk()
    {
        if (!checkExpression(m_set))
            return;
        // The Reset expression is checked only where the mode uses it. In
        // Momentary it is carried untouched, and refusing to save a row over a
        // half-written expression the device will never see would trap the user
        // in a dialog over something that has no effect.
        if (currentMode() == ConditionMode::SetReset && !checkExpression(m_reset))
            return;
        if (ct::channelField(m_outputEdit).isEmpty()) {
            QMessageBox::warning(this, tr("User Condition"), tr("Select an output channel."));
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
    QComboBox *m_modeCombo = nullptr;
    QLabel *m_modeHint = nullptr;
    QWidget *m_latchHolder = nullptr;
    // The duration's spin follows its tick, and the per-comparison ticks follow
    // the duration: a "for" on a comparison means nothing without one.
    //
    // The hint is what stops the empty-mask case being a trap. With no
    // comparison ticked the duration applies to the WHOLE expression, and that
    // is a different thing from applying it to each comparison — under AND the
    // conjunction has to persist, where per-comparison lets them mature
    // separately. Saying which is in force beats leaving it to be inferred.
    void loadQualify(Expr &e, int ms, int mask)
    {
        if (!e.qualifyCheck)
            return;
        e.qualifyCheck->setChecked(ms > 0);
        if (ms > 0)
            e.qualifySpin->setValue(ms / 1000.0);
        for (int t = 0; t < e.terms.size(); ++t)
            if (e.terms[t].qualifyCheck)
                e.terms[t].qualifyCheck->setChecked((mask >> t) & 1);
        syncQualify(e);
    }

    void readQualify(const Expr &e, int *ms, int *mask) const
    {
        *ms = 0;
        *mask = 0;
        if (!e.qualifyCheck || !e.qualifyCheck->isChecked())
            return;
        // Rounded, not truncated: 1.5 s from a 0.01 s box is 1500 ms, and a
        // floor would make it 1499 on a machine whose double lands low.
        *ms = int(qRound(e.qualifySpin->value() * 1000.0));
        // Bounded by the comparison COUNT, not by how many boxes were built.
        // Tick comparison 3, then set the count back to 1, and its widget is
        // hidden but still checked — the same stale-tick trap the message kinds
        // have, reached by the other control that hides a comparison.
        const int n = qMin(termCount(e), int(COND_MAX_TERMS));
        for (int t = 0; t < e.terms.size() && t < n; ++t) {
            const TermWidgets &w = e.terms[t];
            if (!w.qualifyCheck || !w.qualifyCheck->isChecked())
                continue;
            // A MESSAGE COMPARISON NEVER CONTRIBUTES A BIT, however the tick was
            // left. Tick "for" on a channel comparison, then change it to
            // Message received, and the tick is hidden but still checked — so
            // reading the widget alone would send the device a term required to
            // hold continuously for two seconds when it is true for one pass by
            // construction. The condition would simply never fire, and nothing
            // on screen would say why.
            if (condOpIsMessage(quint8(currentOp(w))))
                continue;
            *mask |= (1 << t);
        }
    }

    void syncQualify(Expr &e)
    {
        if (!e.qualifyCheck)
            return;
        const bool on = e.qualifyCheck->isChecked();
        e.qualifySpin->setEnabled(on);
        int ticked = 0;
        // COUNTED EXACTLY AS readQualify() COUNTS, because the whole point of
        // the hint is to say what will be sent. It used to ask box->isVisible(),
        // which is false for every child of a dialog that has not been shown
        // yet — and loadQualify() runs inside the constructor, before exec().
        // So a reopened condition computed `ticked == 0` no matter what mask it
        // had stored and captioned itself "the whole side must hold that long"
        // while a per-comparison tick sat drawn and checked beside it. Asking
        // the count instead is both correct before the dialog is shown and the
        // same question updateTermVisibility() answers when it hides a box.
        const int n = qMin(termCount(e), int(COND_MAX_TERMS));
        for (int t = 0; t < e.terms.size(); ++t) {
            const TermWidgets &w = e.terms[t];
            if (!w.qualifyCheck)
                continue;
            w.qualifyCheck->setEnabled(on);
            if (on && t < n && w.qualifyCheck->isChecked()
                && !condOpIsMessage(quint8(currentOp(w))))
                ++ticked;
        }
        if (!on)
            e.qualifyHint->clear();
        else if (ticked == 0)
            e.qualifyHint->setText(tr("— the whole %1 side must hold that long.").arg(e.title));
        else
            e.qualifyHint->setText(tr("— the ticked comparison(s) only; the rest act "
                                      "immediately."));
    }

    QComboBox *m_latchCombo = nullptr;
    Expr m_set;
    Expr m_reset;
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
    setWindowTitle(tr("User Conditions"));
    setModal(true);
    // Wider than it was: a Set/Reset row prints two expressions in the one
    // column, and truncating the Reset would hide the half that says when the
    // latch lets go.
    resize(760, 400);

    m_tree = new QTreeWidget(this);
    m_tree->setColumnCount(4);
    m_tree->setHeaderLabels({ tr("#"), tr("Active"), tr("Condition"), tr("Output channel") });
    m_tree->setRootIsDecorated(false);
    m_tree->setAllColumnsShowFocus(true);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->header()->setStretchLastSection(true);
    m_tree->setColumnWidth(0, 36);
    m_tree->setColumnWidth(1, 50);
    m_tree->setColumnWidth(2, 400);

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
        item->setText(2, conditionSummary(m_config, row));
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
        QMessageBox::warning(this, tr("User Conditions"),
                             tr("The device supports at most %1 User Conditions.")
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
