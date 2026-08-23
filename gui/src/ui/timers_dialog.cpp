// Calculations > Timers — grid editor for Configuration::timerRows.
// Mirrors MoTeC's Timers dialog (Start/Stop tab + Settings tab).
#include "timers_dialog.h"

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

// One trigger, as a single line for the list. The list is where somebody checks
// they configured the right thing without opening eight rows, so it spells the
// comparison rather than just naming the channel it reads.
QString termSummary(const ChannelCatalog &catalog, const ConditionTermRow &t)
{
    if (t.isMessageOp()) {
        if (t.aMessage.isEmpty())
            return QString();
        return QStringLiteral("CAN%1 · %2 %3")
            .arg(t.aMessageBus)
            .arg(t.aMessage, t.op == COND_OP_MSG_RX ? QObject::tr("received")
                                                    : QObject::tr("transmitted"));
    }
    if (t.aChannel.isEmpty())
        return QString();
    static const char *const kOps[] = {"=", "≠", "<", "≤", ">", "≥"};
    const QString opText = (t.op >= 0 && t.op < 6) ? QString::fromUtf8(kOps[t.op])
                                                   : QStringLiteral("?");
    const QString rhs =
        t.bIsChannel ? catalog.labelFor(t.bChannel) : ct::trimmedNumber(t.bConst, 6);
    return QStringLiteral("%1 %2 %3").arg(catalog.labelFor(t.aChannel), opText, rhs);
}

} // namespace


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

            buildTerm(page, form, QObject::tr("Start Timer"), &m_start);
            buildTerm(page, form, QObject::tr("Stop Timer"), &m_stop);

            auto *note = new QLabel(
                QObject::tr("Each trigger is a comparison, and it fires on the RISING "
                            "EDGE of that comparison becoming true — not while it stays "
                            "true. Leave a trigger's channel empty and that half never "
                            "fires: a timer with no stop never stops itself."),
                page);
            note->setWordWrap(true);
            form->addRow(QString(), note);

            wireTerm(&m_start);
            wireTerm(&m_stop);

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

        // THE OUTPUT CHANNEL GOES LAST, matching the User Condition editor:
        // what the timer produces reads as the conclusion of what it is built
        // from, not as a question asked before any of it is on screen.
        //
        // OUTSIDE THE TABS, not merely at the foot of the Start / Stop one. The
        // output channel belongs to the timer rather than to either tab, and a
        // required field that disappears when you look at Settings is a field
        // somebody can leave empty without ever seeing it. Below the tab widget
        // it is on screen whichever tab is open, which is also what "the bottom
        // of the window" means for a dialog that has tabs at all.
        auto *outForm = new QFormLayout;
        auto *outRow = new QHBoxLayout;
        m_outputEdit = new QLineEdit(this);
        m_outputEdit->setReadOnly(true);
        auto *outSelect = new QPushButton(QObject::tr("Select…"), this);
        outRow->addWidget(m_outputEdit, 1);
        outRow->addWidget(outSelect);
        outForm->addRow(QObject::tr("Output Channel :"), outRow);
        mainLayout->addLayout(outForm);

        QObject::connect(outSelect, &QPushButton::clicked, this, [this] {
            const QString picked = SelectChannelDialog::pickOutput(
                m_config, ct::channelField(m_outputEdit), this, m_livePatch);
            if (!picked.isEmpty())
                ct::setChannelField(m_outputEdit, picked, m_config->catalog());
        });

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
        loadTerm(&m_start, m_row.startTerm);
        loadTerm(&m_stop, m_row.stopTerm);
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
        m_row.startTerm = readTerm(m_start);
        m_row.stopTerm = readTerm(m_stop);
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


    struct TermFields
    {
        // A GROUP BOX PER TRIGGER, laid out like a User Condition's comparison:
        // one control per line, top to bottom in the order the trigger is read.
        // It used to be a single QFormLayout row with everything on it — kind,
        // channel, Select, operator, Value/Channel, spin box, channel, Select —
        // eight controls running off the right-hand edge, where the operator
        // that decides what the line MEANS was the fifth thing along.
        QGroupBox *box = nullptr;
        QLabel *kindLabel = nullptr;
        QLabel *aLabel = nullptr;
        QWidget *row = nullptr;
        // WHAT KIND OF CONDITION, chosen before anything else on the row,
        // because it decides what the rest of the row even means. The two
        // message kinds used to live at the end of the operator list, which put
        // "was received" in a list of <, >, = and made the reader work out that
        // picking it silently changed the left operand from a channel to a
        // message. Leading with the kind says it up front and matches the
        // counters editor, where the same three choices sit in the same place.
        QComboBox *kind = nullptr;
        QLineEdit *aEdit = nullptr;      // channel operand
        QPushButton *aSelect = nullptr;
        QComboBox *aMessage = nullptr;   // message operand, for the message ops
        QComboBox *opCombo = nullptr;
        // RADIO BUTTONS, not a Value/Channel combo. Two mutually exclusive ways
        // to name the right-hand operand, each on its own line beside the field
        // it selects — the same shape a User Condition uses, so the two editors
        // no longer spell the identical choice two different ways.
        QRadioButton *bChannelRadio = nullptr;
        QRadioButton *bConstRadio = nullptr;
        TrimmedDoubleSpinBox *bValue = nullptr;
        QLineEdit *bEdit = nullptr;
        QPushButton *bSelect = nullptr;
    };

    // ---- the trigger editor ------------------------------------------------
    //
    // Start and Stop are the same comparison, so they are the same widgets built
    // by the same function. The operator combo carries the two MESSAGE operators
    // beside the six comparisons, and picking one swaps the left operand from a
    // channel to a message and hides the right operand entirely: "did this frame
    // happen" has nothing to compare against.
    // The kind combo's values. Deliberately NOT the ConditionOp numbers: the
    // kind is a question about the row's shape, and the operator is a question
    // about a comparison. Mapping between them happens in exactly two places,
    // loadTerm() and readTerm(), and nowhere else.
    static constexpr int kKindChannel = 0;
    static constexpr int kKindMsgRx = 1;
    static constexpr int kKindMsgTx = 2;

    static int kindForOp(int op)
    {
        if (op == COND_OP_MSG_RX)
            return kKindMsgRx;
        if (op == COND_OP_MSG_TX)
            return kKindMsgTx;
        return kKindChannel;
    }

    // Messages are keyed by (bus, name) in the combo's data, joined by a
    // character no section name can contain. An INDEX would be permuted by
    // reordering and rewritten by a Get — the reason ConditionTermRow stores the
    // pair rather than a number.
    static QString messageKey(int bus, const QString &name)
    {
        return QStringLiteral("%1\u0001%2").arg(bus).arg(name);
    }

    // Built to match a User Condition's comparison box, control for control, so
    // that the two editors read the same way:
    //
    //     Compare:      [what kind]
    //     Input A :     [channel] [Select...]
    //                   [operator]
    //     (o) Channel:  [channel] [Select...]
    //     (o) Constant: [value]
    //
    // What a condition has here and a timer does not is the "for the time set
    // below" tick and the duration under it. A timer fires on the RISING EDGE
    // of its comparison, and an edge is an instant — "held true for two
    // seconds" is a property of a level, and there is no level here to hold.
    // Ask for a delayed start and the answer is a User Condition carrying the
    // duration, pointed at from here.
    void buildTerm(QWidget *page, QFormLayout *form, const QString &title, TermFields *f)
    {
        f->box = new QGroupBox(title, page);
        auto *grid = new QGridLayout(f->box);

        f->kindLabel = new QLabel(QObject::tr("Compare:"), f->box);
        f->kind = new QComboBox(f->box);
        f->kind->addItem(QObject::tr("Channel value"), kKindChannel);
        f->kind->addItem(QObject::tr("Message received"), kKindMsgRx);
        f->kind->addItem(QObject::tr("Message transmitted"), kKindMsgTx);

        f->aLabel = new QLabel(QObject::tr("Input A:"), f->box);
        f->aEdit = new QLineEdit(f->box);
        f->aEdit->setReadOnly(true);
        f->aSelect = new QPushButton(QObject::tr("Select…"), f->box);
        // The message selector occupies the SAME cells as the channel picker:
        // one operand seen through a different operator, and exactly one of the
        // two is ever visible.
        f->aMessage = new QComboBox(f->box);

        f->opCombo = new QComboBox(f->box);
        f->opCombo->addItem(QStringLiteral("="), int(COND_OP_EQ));
        f->opCombo->addItem(QStringLiteral("≠"), int(COND_OP_NEQ));
        f->opCombo->addItem(QStringLiteral("<"), int(COND_OP_LT));
        f->opCombo->addItem(QStringLiteral("≤"), int(COND_OP_LTE));
        f->opCombo->addItem(QStringLiteral(">"), int(COND_OP_GT));
        f->opCombo->addItem(QStringLiteral("≥"), int(COND_OP_GTE));
        // The two message operators are NOT here - they are kinds, not
        // comparisons, and the kind combo above is where they belong.

        f->bChannelRadio = new QRadioButton(QObject::tr("Channel:"), f->box);
        f->bConstRadio = new QRadioButton(QObject::tr("Constant:"), f->box);
        // One group PER trigger, or Start's pair and Stop's would be mutually
        // exclusive with each other instead of within themselves.
        auto *radios = new QButtonGroup(f->box);
        radios->addButton(f->bChannelRadio);
        radios->addButton(f->bConstRadio);
        f->bEdit = new QLineEdit(f->box);
        f->bEdit->setReadOnly(true);
        f->bSelect = new QPushButton(QObject::tr("Select…"), f->box);
        f->bValue = new TrimmedDoubleSpinBox(f->box);
        f->bValue->setRange(-1e9, 1e9);
        f->bValue->setDecimals(6);

        grid->addWidget(f->kindLabel, 0, 0);
        grid->addWidget(f->kind, 0, 1, 1, 3);
        grid->addWidget(f->aLabel, 1, 0);
        grid->addWidget(f->aEdit, 1, 1);
        grid->addWidget(f->aSelect, 1, 2);
        grid->addWidget(f->aMessage, 1, 1, 1, 2);
        grid->addWidget(f->opCombo, 2, 1);
        grid->addWidget(f->bChannelRadio, 3, 0);
        grid->addWidget(f->bEdit, 3, 1);
        grid->addWidget(f->bSelect, 3, 2);
        grid->addWidget(f->bConstRadio, 4, 0);
        grid->addWidget(f->bValue, 4, 1);
        grid->setColumnStretch(1, 1);

        for (int b = 0; b < 3; ++b)
            for (const CommsSection &s : m_config->bus[b].sections)
                if (!s.name.isEmpty())
                    f->aMessage->addItem(QStringLiteral("CAN%1 · %2").arg(b + 1).arg(s.name),
                                         messageKey(b + 1, s.name));

        // Spanning the form with no label-column entry: the box carries its own
        // title, and that title is what "Start Timer" / "Stop Timer" now are.
        form->addRow(f->box);
    }

    void wireTerm(TermFields *f)
    {
        QObject::connect(f->aSelect, &QPushButton::clicked, this, [this, f] {
            const QString picked = SelectChannelDialog::pickInput(
                m_config, ct::channelField(f->aEdit), this, m_livePatch);
            if (!picked.isEmpty())
                ct::setChannelField(f->aEdit, picked, m_config->catalog());
        });
        QObject::connect(f->bSelect, &QPushButton::clicked, this, [this, f] {
            const QString picked = SelectChannelDialog::pickInput(
                m_config, ct::channelField(f->bEdit), this, m_livePatch);
            if (!picked.isEmpty())
                ct::setChannelField(f->bEdit, picked, m_config->catalog());
        });
        QObject::connect(f->kind, &QComboBox::currentIndexChanged, this,
                         [this, f](int) { syncTerm(f); });
        QObject::connect(f->opCombo, &QComboBox::currentIndexChanged, this,
                         [this, f](int) { syncTerm(f); });
        QObject::connect(f->bChannelRadio, &QRadioButton::toggled, this,
                         [this, f](bool) { syncTerm(f); });
        QObject::connect(f->bConstRadio, &QRadioButton::toggled, this,
                         [this, f](bool) { syncTerm(f); });
        syncTerm(f);
    }

    void syncTerm(TermFields *f)
    {
        const bool message = f->kind->currentData().toInt() != kKindChannel;
        f->aEdit->setVisible(!message);
        f->aSelect->setVisible(!message);
        f->aMessage->setVisible(message);
        const bool channelB = f->bChannelRadio->isChecked();
        // A MESSAGE TRIGGER HAS NO RIGHT-HAND OPERAND at all - "did this
        // frame happen" has nothing to compare against - so the operator and
        // both operand rows go, and the box shrinks to the two lines that
        // still mean something.
        f->aLabel->setText(message ? QObject::tr("Message:") : QObject::tr("Input A:"));
        f->opCombo->setVisible(!message);
        f->bChannelRadio->setVisible(!message);
        f->bConstRadio->setVisible(!message);
        f->bEdit->setVisible(!message && channelB);
        f->bSelect->setVisible(!message && channelB);
        f->bValue->setVisible(!message && !channelB);
    }

    void loadTerm(TermFields *f, const ConditionTermRow &t)
    {
        const int kindAt = f->kind->findData(kindForOp(t.op));
        f->kind->setCurrentIndex(kindAt >= 0 ? kindAt : 0);
        // Only a comparison has an operator to restore. A message term's op IS
        // its kind, and the combo no longer offers those, so findData would
        // return -1 and quietly reset the row to "=".
        if (!t.isMessageOp()) {
            const int opAt = f->opCombo->findData(t.op);
            f->opCombo->setCurrentIndex(opAt >= 0 ? opAt : 0);
        }
        if (t.isMessageOp()) {
            const int at = f->aMessage->findData(messageKey(t.aMessageBus, t.aMessage));
            if (at >= 0)
                f->aMessage->setCurrentIndex(at);
        } else {
            ct::setChannelField(f->aEdit, t.aChannel, m_config->catalog());
        }
        f->bChannelRadio->setChecked(t.bIsChannel);
        f->bConstRadio->setChecked(!t.bIsChannel);
        f->bValue->setValue(t.bConst);
        ct::setChannelField(f->bEdit, t.bChannel, m_config->catalog());
        syncTerm(f);
    }

    ConditionTermRow readTerm(const TermFields &f) const
    {
        ConditionTermRow t;
        const int kind = f.kind->currentData().toInt();
        if (kind != kKindChannel) {
            t.op = (kind == kKindMsgRx) ? int(COND_OP_MSG_RX) : int(COND_OP_MSG_TX);
            const QStringList parts =
                f.aMessage->currentData().toString().split(QChar(char16_t(0x0001)));
            if (parts.size() == 2) {
                t.aMessageBus = parts.at(0).toInt();
                t.aMessage = parts.at(1);
            }
            return t;
        }
        t.op = f.opCombo->currentData().toInt();
        t.aChannel = ct::channelField(f.aEdit);
        t.bIsChannel = f.bChannelRadio->isChecked();
        t.bChannel = ct::channelField(f.bEdit);
        t.bConst = f.bValue->value();
        return t;
    }

    Configuration *m_config;
    TimerRow m_row;
    ConfigPatch m_livePatch;

    QLineEdit *m_outputEdit = nullptr;

    // ONE TRIGGER EDITOR, TWICE. Start and Stop are the same comparison and get
    // the same widgets; building them from one function is what keeps the two
    // from drifting apart the way two hand-written copies would.
    //
    // The operator combo carries the message operators alongside the six
    // comparisons, exactly as the User Condition editor's does, and choosing one
    // swaps the left-hand operand from a channel to a message — a message
    // operator asks "did this frame happen", so there is nothing to compare it
    // against and the right-hand side goes away with it.
    TermFields m_start;
    TermFields m_stop;
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
        item->setText(3, termSummary(catalog, row.startTerm));
        item->setText(4, termSummary(catalog, row.stopTerm));
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
