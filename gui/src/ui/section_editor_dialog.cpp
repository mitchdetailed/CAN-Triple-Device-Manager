#include "section_editor_dialog.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QVariant>

#include "../model/channel_catalog.h"
#include "../model/frame_layout.h"
#include "../protocol/wire_structs.h" // CRC8_MAX_ELEMENTS, asserted == kCrcElementSlots
#include "add_channel_dialog.h"
#include "channel_field.h"
#include "color_item_delegate.h"
#include "select_channel_dialog.h"

namespace ct {

namespace {

// The frame map draws channel names in its cell tooltips and its caption, but it
// is handed rows — which carry a name and nothing else — and has no route to the
// document's catalogue. So the labels travel to it as a dynamic property,
// name -> "Name Unit"; the key must match kChannelLabelsProperty in
// bit_layout_table.cpp. A map that is never set leaves the widget drawing bare
// names, exactly as it did before.
const char *const kChannelLabelsProperty = "ct_channel_labels";

// Short label for the byte order, so the frame map's title says which way a
// multi-byte field walks — the very thing the map is drawing.
QString alignmentWord(SectionAlignment alignment)
{
    return alignment == SectionAlignment::WordSwap ? QObject::tr("Word Swap (Intel)")
                                                   : QObject::tr("Normal (Motorola)");
}

// The tier's name as the three tick boxes spell it, for prompts that have to say
// which marking is being given up. Not commsProtectionToken() — that is the
// frozen JSON spelling and must never become a display string, or renaming a
// label would rewrite every .ct3 on disk.
QString protectionTierWord(CommsProtection tier)
{
    switch (tier) {
    case CommsProtection::None: return QObject::tr("not protected");
    case CommsProtection::ReadOnly: return QObject::tr("Read Only");
    case CommsProtection::Hidden: return QObject::tr("Hidden");
    case CommsProtection::Protected: return QObject::tr("Protect Communication");
    }
    return QObject::tr("protected");
}

// One byte of hex, 0x-prefixed or bare, the same tolerance the address fields
// give — a user who types "1D" and one who pastes "0x1D" both mean 0x1D, and
// refusing either spelling is a paper cut with no payoff. `ok` reports whether
// the TEXT parsed; the returned value is meaningful only when it did.
int parseHexByte(const QString &text, bool *ok)
{
    QString t = text;
    if (t.startsWith(QLatin1String("0x"), Qt::CaseInsensitive))
        t = t.mid(2);
    bool parsed = false;
    const uint value = t.toUInt(&parsed, 16);
    // The validators cap the field at two digits, but this helper is also fed
    // raw text from accept(), where trusting the widget to have filtered is how
    // an out-of-range byte reaches the model.
    parsed = parsed && value <= 0xFFu;
    if (ok)
        *ok = parsed;
    return parsed ? int(value) : 0;
}

// The 0xNN presentation, mirroring reformatAddress: parse what was typed, fall
// back to `fallback` when it does not parse (initial fill / field cleared
// mid-edit), and rewrite the text in the canonical spelling.
void reformatHexByteEdit(QLineEdit *edit, int fallback)
{
    bool ok = false;
    int value = parseHexByte(edit->text(), &ok);
    if (!ok)
        value = fallback & 0xFF;
    const QSignalBlocker blocker(edit);
    edit->setText(QStringLiteral("0x")
                  + QStringLiteral("%1").arg(value, 2, 16, QLatin1Char('0')).toUpper());
}

// Small editor for one compound identifier (Offset / Identifier / Identifier Mask).
class CompoundIdentifierDialog : public QDialog
{
public:
    CompoundIdentifierDialog(const CompoundIdentifier &ident, int messageLenBytes,
                             QWidget *parent)
        : QDialog(parent)
    {
        setWindowTitle(QObject::tr("Compound Message Identifier"));
        auto *layout = new QGridLayout(this);
        layout->addWidget(new QLabel(QObject::tr("Offset :")), 0, 0);
        m_offset = new QSpinBox;
        // The selector window can start at any byte inside the frame.
        m_offset->setRange(0, qBound(1, messageLenBytes, 64) - 1);
        m_offset->setValue(ident.byteOffset);
        layout->addWidget(m_offset, 0, 1);
        layout->addWidget(new QLabel(QObject::tr("Identifier :")), 1, 0);
        m_id = new QLineEdit(QString::number(ident.id, 16).toUpper());
        m_id->setValidator(new QRegularExpressionValidator(
            QRegularExpression(QStringLiteral("[0-9A-Fa-f]{1,8}")), this));
        layout->addWidget(m_id, 1, 1);
        layout->addWidget(new QLabel(QObject::tr("Identifier Mask :")), 2, 0);
        m_mask = new QLineEdit(QString::number(ident.idMask, 16).toUpper());
        m_mask->setValidator(new QRegularExpressionValidator(
            QRegularExpression(QStringLiteral("[0-9A-Fa-f]{1,8}")), this));
        layout->addWidget(m_mask, 2, 1);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        QObject::connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        QObject::connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        layout->addWidget(buttons, 3, 0, 1, 2);
    }

    void apply(CompoundIdentifier *ident) const
    {
        ident->byteOffset = m_offset->value();
        ident->id = m_id->text().toUInt(nullptr, 16);
        ident->idMask = m_mask->text().toUInt(nullptr, 16);
        // Accepting the dialog claims the slot even at all-default values
        // (offset 0 / ID 0), so the table row populates either way.
        ident->configured = true;
    }

private:
    QSpinBox *m_offset;
    QLineEdit *m_id;
    QLineEdit *m_mask;
};

// "Coolant Temp °C   (bit 0, 16-bit unsigned, ×0.1 +0)" — the channel named with
// its unit, because a raw count means nothing without one. The list this feeds
// is display only: a row is identified by its POSITION (currentRow() indexes
// activeRowList()), never by an item's text, so the decorated string has nowhere
// to leak into. row.channelName remains the identity.
QString rowSummary(const CommsChannelRow &row, const ChannelCatalog &catalog, bool receive)
{
    static const char *const kType[] = {"unsigned", "signed", "ieee754"};
    const int t = qBound(0, row.dbcType, 2);
    const int len = row.dbcType == int(DbcType::IEEE754) ? 32 : row.bitLength;
    // The roll-over note rides inside the bracket only where it does something.
    // `receive` is passed rather than inferred because a hand-edited .ct3 CAN
    // put the flag on a receive row, and the device ignores it there — the
    // Config Summary withholds it on the same test, and two views of one row
    // disagreeing is worse than either answer alone. Validation is what tells
    // the user about the contradiction.
    const QString wrap =
        (receive || row.clampToRange) ? QString() : QObject::tr(", rolls over");
    return QObject::tr("%1   (bit %2, %3-bit %4, ×%5 %6%7%8)")
        .arg(catalog.labelFor(row.channelName))
        .arg(row.startBit)
        .arg(len)
        .arg(QLatin1String(kType[t]))
        .arg(row.dbcFactor)
        .arg(row.dbcOffset >= 0 ? QStringLiteral("+") : QString())
        .arg(row.dbcOffset)
        .arg(wrap);
}

} // namespace

SectionEditorDialog::SectionEditorDialog(Configuration *config, const CommsSection &section,
                                         int busIndex, const ConfigPatch &livePatch,
                                         int sectionIndex, QWidget *parent,
                                         ProtectedCommsProver prover, int busDataRateKbps)
    : QDialog(parent)
    , m_config(config)
    , m_section(section)
    , m_busIndex(busIndex)
    , m_busDataRateKbps(busDataRateKbps)
    , m_livePatch(livePatch)
    , m_sectionIndex(sectionIndex)
    , m_prover(std::move(prover))
    , m_tier(section.protection)
    , m_openedTier(section.protection)
{
    // Before any of it is drawn: the frame map colours signals by their index
    // in the row list, so the map and the list have to be reading the same
    // order from the first paint.
    sortSectionRows();
    setWindowTitle(tr("CAN Communications Setup — CAN %1").arg(busIndex + 1));
    // Taller than the old 560x520: the Channels tab now carries the frame
    // layout map under both panes, and a squashed map is a useless one.
    resize(720, 700);

    auto *layout = new QVBoxLayout(this);
    m_tabs = new QTabWidget;
    auto *parametersPage = new QWidget;
    buildParametersTab(parametersPage);
    m_tabs->addTab(parametersPage, tr("Parameters"));
    auto *channelsPage = new QWidget;
    buildChannelsTab(channelsPage);
    m_tabs->addTab(channelsPage, m_section.isTransmit() ? tr("Transmitted Channels")
                                                        : tr("Received Channels"));
    // Always ADDED, only sometimes VISIBLE (applyDeviceKindEnablement).
    // setTabVisible hides a tab WITHOUT renumbering — the index stays claimed —
    // which is what lets the tab vanish with the Message Type combo while
    // everything that addresses tabs by index keeps working; removeTab would
    // not have that property.
    auto *crcPage = new QWidget;
    buildCrcTab(crcPage);
    m_tabs->addTab(crcPage, tr("CRC8"));
    layout->addWidget(m_tabs);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &SectionEditorDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    // A rename can arrive from a channel row's picker while this working copy
    // is open — see Configuration::channelRenamed. rebuildChannelList()
    // refreshes the frame layout map with it. The identifier table is left
    // alone: it shows no channel names, and rebuilding it would yank the
    // user's identifier selection back to the first row.
    connect(m_config, &Configuration::channelRenamed, this,
            [this](const QString &oldName, const QString &newName) {
                if (renameChannelRefs(m_section, oldName, newName) > 0)
                    rebuildChannelList();
            });

    onDeviceChanged();
    onMessageTypeToggled();
    rebuildChannelList();
    rebuildIdentifierTable();
    // Last word, after every other pass has had its say about the same widgets.
    updateProtectionUi();
}

void SectionEditorDialog::buildParametersTab(QWidget *page)
{
    auto *layout = new QVBoxLayout(page);

    auto *paramGroup = new QGroupBox(tr("Parameters"));
    auto *paramGrid = new QGridLayout(paramGroup);
    int r = 0;

    paramGrid->addWidget(new QLabel(tr("Name :")), r, 0);
    m_nameEdit = new QLineEdit(m_section.name);
    m_nameEdit->setPlaceholderText(tr("(automatic)"));
    paramGrid->addWidget(m_nameEdit, r, 1, 1, 3);
    ++r;

    paramGrid->addWidget(new QLabel(tr("Message Type :")), r, 0);
    m_deviceCombo = new QComboBox;
    m_deviceCombo->addItem(tr("Off"), int(SectionDevice::Off));
    m_deviceCombo->addItem(tr("Receive Message"), int(SectionDevice::ReceiveMessage));
    m_deviceCombo->addItem(tr("Transmit Message"), int(SectionDevice::TransmitMessage));
    // Next to Transmit Message, not appended after Message Relay: it IS a
    // transmit message (isTransmit() answers true) with a checksum stamped on
    // top, and the combo's order is the user's taxonomy.
    m_deviceCombo->addItem(tr("Transmit CRC8"), int(SectionDevice::TransmitCrc8));
    m_deviceCombo->addItem(tr("Message Relay"), int(SectionDevice::MessageRelay));
    m_deviceCombo->setCurrentIndex(m_deviceCombo->findData(int(m_section.device)));
    connect(m_deviceCombo, &QComboBox::currentIndexChanged, this,
            &SectionEditorDialog::onDeviceChanged);
    paramGrid->addWidget(m_deviceCombo, r, 1, 1, 3);
    ++r;

    paramGrid->addWidget(new QLabel(tr("Alignment :")), r, 0);
    m_alignmentCombo = new QComboBox;
    m_alignmentCombo->addItem(tr("Normal (big-endian)"), int(SectionAlignment::Normal));
    m_alignmentCombo->addItem(tr("Word Swap (little-endian)"), int(SectionAlignment::WordSwap));
    m_alignmentCombo->setCurrentIndex(m_alignmentCombo->findData(int(m_section.alignment)));
    // Byte order decides which way a multi-byte field walks through the frame,
    // so the layout map has to be redrawn whenever it changes.
    connect(m_alignmentCombo, &QComboBox::currentIndexChanged, this,
            &SectionEditorDialog::refreshBitTable);
    paramGrid->addWidget(m_alignmentCombo, r, 1, 1, 3);
    ++r;

    // ---- the protection tier ----
    //
    // Three checkboxes, ONE ordered level. They are not three independent flags:
    // each tier's description contains the previous one's, so the boxes are a
    // ladder and are driven as one — see onTierBoxToggled. The v20 shape (two
    // bools plus `readOnlyComms = readOnly || protect` maintained by hand in
    // this one file) is what an ordered level replaces.
    //
    // Every tooltip says the honest strength. All three are conventions of THIS
    // APPLICATION: as of 2.3.0 the device enforces nothing whatever about
    // message protection, and a plain .ct3 is unauthenticated JSON that a text
    // editor defeats. Only a sealed .ct3s makes the bytes unreadable. The help
    // has claimed otherwise twice and must not again.
    auto *tierRow = new QHBoxLayout;
    m_readOnlyCheck = new QCheckBox(tr("Read Only"));
    m_readOnlyCheck->setToolTip(
        tr("VISIBLE to everyone, not editable. Every field of this message and of the "
           "channels it carries is shown; none of them can be changed until the box is "
           "unticked, which needs this section's own password.\n\n"
           "This is ACCIDENT PREVENTION, not security. The viewer can read the whole "
           "message and is still allowed to REMOVE it, so removing it and retyping it "
           "reproduces it without the password. Use it to stop a calibration being "
           "changed by mistake in the field — not to keep a protocol secret. For that, "
           "tick Hidden."));
    tierRow->addWidget(m_readOnlyCheck);
    m_hiddenCheck = new QCheckBox(tr("Hidden"));
    m_hiddenCheck->setToolTip(
        tr("NOT VISIBLE and not editable. Withheld: the CAN ID, frame length, byte order, "
           "timing, routing and every channel's bit layout and scaling — in the sections "
           "list, this editor, Check Channels, Monitor Channels and the Config Summary "
           "report. Channel NAMES stay visible, so the channels this message produces can "
           "still be used everywhere else.\n\n"
           "Substantively stronger than Read Only for the one reason that matters: a "
           "viewer who cannot SEE the message cannot retype it, so removing it destroys "
           "rather than reveals.\n\n"
           "Unticking needs this section's own password. Not encryption — a saved .ct3 "
           "still contains every one of those fields in clear text, and any other serial "
           "tool reads them straight off the device. Only File > Save Secure Config… "
           "(.ct3s) makes the bytes themselves unreadable."));
    tierRow->addWidget(m_hiddenCheck);
    m_protectCheck = new QCheckBox(tr("Protect Communication"));
    m_protectCheck->setToolTip(
        tr("Hidden, plus one thing more: a configuration containing a message marked this "
           "way can only be SENT to a device that holds the matching Protected Comms "
           "password. The check runs at Send, against the connected unit — ticking the box "
           "itself needs only a Message Password, so the tier can be set up offline.\n\n"
           "Opening an existing Protected message still needs its Message Password AND the "
           "Protected Comms password confirmed by a connected device — both, not "
           "either.\n\n"
           "That send gate is the difference between this and Hidden. What is withheld on "
           "screen, and from whom, is identical."));
    tierRow->addWidget(m_protectCheck);
    tierRow->addStretch();
    paramGrid->addLayout(tierRow, r, 0, 1, 4);
    ++r;

    // One handler, three boxes, and the level is recomputed from scratch each
    // time. Connecting three lambdas that each tried to keep the other two
    // consistent is the shape that produced the invariant this replaces.
    connect(m_readOnlyCheck, &QCheckBox::toggled, this,
            [this](bool on) { onTierBoxToggled(CommsProtection::ReadOnly, on); });
    connect(m_hiddenCheck, &QCheckBox::toggled, this,
            [this](bool on) { onTierBoxToggled(CommsProtection::Hidden, on); });
    connect(m_protectCheck, &QCheckBox::toggled, this,
            [this](bool on) { onTierBoxToggled(CommsProtection::Protected, on); });

    // NO MESSAGE PASSWORD FIELD. The password is asked for at the moment it is
    // needed and at no other time: when a marking is taken, and again when one
    // is released. A permanent field invited a different mental model - that the
    // password is a property of the message you edit alongside its byte order -
    // when it is really the price of a specific act, and it left "(unchanged -
    // type to replace)" sitting on screen as the answer to a question nobody had
    // asked. What the dialog carries instead is m_pendingKey: the key chosen
    // through a prompt during this visit, applied by syncParametersFromUi.

    // What the CURRENT tier actually means, in one line under the boxes, so the
    // answer to "why is everything greyed out?" is on screen rather than in a
    // tooltip nobody hovers. updateProtectionUi writes it.
    m_protectionNote = new QLabel;
    m_protectionNote->setWordWrap(true);
    m_protectionNote->setStyleSheet(QStringLiteral("color: gray;"));
    paramGrid->addWidget(m_protectionNote, r, 0, 1, 4);
    ++r;

    paramGrid->addWidget(new QLabel(tr("Receive Timeout :")), r, 0);
    m_timeoutSpin = new QSpinBox;
    m_timeoutSpin->setRange(0, 60000);
    m_timeoutSpin->setSuffix(tr(" ms"));
    m_timeoutSpin->setValue(m_section.receiveTimeoutMs);
    paramGrid->addWidget(m_timeoutSpin, r, 1);
    m_defaultTimeoutCheck = new QCheckBox(tr("Default value on timeout"));
    m_defaultTimeoutCheck->setChecked(m_section.defaultValueOnTimeout);
    paramGrid->addWidget(m_defaultTimeoutCheck, r, 2, 1, 2);
    ++r;

    layout->addWidget(paramGroup);

    auto *canGroup = new QGroupBox(tr("CAN Settings"));
    auto *canGrid = new QGridLayout(canGroup);
    r = 0;

    canGrid->addWidget(new QLabel(tr("Address Format :")), r, 0);
    m_standardRadio = new QRadioButton(tr("Standard"));
    m_extendedRadio = new QRadioButton(tr("Extended"));
    // Both radio pairs share the same parent widget; without explicit button
    // groups Qt would make all four mutually exclusive.
    auto *addressGroup = new QButtonGroup(this);
    addressGroup->addButton(m_standardRadio);
    addressGroup->addButton(m_extendedRadio);
    m_standardRadio->setChecked(!m_section.extended);
    m_extendedRadio->setChecked(m_section.extended);
    auto *radioRow = new QHBoxLayout;
    radioRow->addWidget(m_standardRadio);
    radioRow->addWidget(m_extendedRadio);
    radioRow->addSpacing(16);
    m_fdCheck = new QCheckBox(tr("CAN FD frame"));
    // New FD framing is offered only when the bus itself runs CAN FD (FD Data
    // rate set in Connections > Communications). But a section that is ALREADY
    // FD stays editable — otherwise an FD message reconstructed on a
    // classic-assumed bus (or after the bus's FD rate is cleared) could not be
    // opened at all: its 12–64 byte length would fail the classic-length check
    // with the FD box locked off. In that case the box is enabled and checked
    // so the user can keep FD or deliberately convert the section to classic.
    // From the caller's working copy when supplied (see the constructor
    // comment): the Communications dialog commits its bus tabs on OK, so the
    // config's own value is one whole dialog-visit stale.
    const bool busHasFd = m_busDataRateKbps >= 0
                              ? m_busDataRateKbps > 0
                              : (m_busIndex >= 0 && m_busIndex < 3
                                 && m_config->bus[m_busIndex].dataRateKbps > 0);
    m_fdAllowed = busHasFd || m_section.fd;
    m_fdCheck->setChecked(m_section.fd);
    m_fdCheck->setEnabled(m_fdAllowed);
    const bool allowFd = m_fdAllowed;
    m_fdCheck->setToolTip(allowFd
        ? tr("Allows message lengths 12, 16, 20, 24, 32, 48 and 64 bytes")
        : tr("Set an FD Data rate for this bus (Connections > Communications) to "
             "enable CAN FD frames"));
    // FD decides how long a received frame can be, and so how many bytes the
    // layout map draws for a receive message.
    connect(m_fdCheck, &QCheckBox::toggled, this, &SectionEditorDialog::refreshBitTable);
    radioRow->addWidget(m_fdCheck);
    radioRow->addStretch();
    canGrid->addLayout(radioRow, r, 1, 1, 3);
    connect(m_extendedRadio, &QRadioButton::toggled, this, [this]() {
        reformatAddress();
        onAddressEdited();
        reformatBitmask();
    });
    ++r;

    canGrid->addWidget(new QLabel(tr("Base Address :")), r, 0);
    m_addressEdit = new QLineEdit;
    m_addressEdit->setValidator(new QRegularExpressionValidator(
        QRegularExpression(QStringLiteral("(0[xX])?[0-9A-Fa-f]{1,8}")), this));
    connect(m_addressEdit, &QLineEdit::textChanged, this, &SectionEditorDialog::onAddressEdited);
    connect(m_addressEdit, &QLineEdit::editingFinished, this,
            &SectionEditorDialog::reformatAddress);
    canGrid->addWidget(m_addressEdit, r, 1);
    m_addressDecLabel = new QLabel;
    canGrid->addWidget(m_addressDecLabel, r, 2, 1, 2);
    ++r;

    m_lengthLabel = new QLabel(tr("Message Length :"));
    canGrid->addWidget(m_lengthLabel, r, 0);
    m_lengthEdit = new QLineEdit(QString::number(m_section.messageLengthBytes));
    m_lengthEdit->setValidator(new QRegularExpressionValidator(
        QRegularExpression(QStringLiteral("[0-9]{1,2}")), this));
    // Live, per keystroke: the layout map is how you judge whether a length is
    // the right one, so it has to follow the field as it is typed.
    connect(m_lengthEdit, &QLineEdit::textChanged, this, &SectionEditorDialog::refreshBitTable);
    canGrid->addWidget(m_lengthEdit, r, 1);
    m_lengthUnitLabel = new QLabel(tr("bytes"));
    canGrid->addWidget(m_lengthUnitLabel, r, 2);
    ++r;

    reformatAddress();

    canGrid->addWidget(new QLabel(tr("Transmission :")), r, 0);
    m_cyclicRadio = new QRadioButton(tr("Cyclic"));
    m_triggeredRadio = new QRadioButton(tr("Triggered"));
    auto *transmissionGroup = new QButtonGroup(this);
    transmissionGroup->addButton(m_cyclicRadio);
    transmissionGroup->addButton(m_triggeredRadio);
    m_cyclicRadio->setChecked(m_section.cyclic);
    m_triggeredRadio->setChecked(!m_section.cyclic);
    auto *txRow = new QHBoxLayout;
    txRow->addWidget(m_cyclicRadio);
    txRow->addWidget(m_triggeredRadio);
    txRow->addStretch();
    canGrid->addLayout(txRow, r, 1, 1, 3);
    ++r;

    canGrid->addWidget(new QLabel(tr("Transmit Rate :")), r, 0);
    m_rateCombo = new QComboBox;
    // 200 Hz is the ceiling: the device's transmit scheduler runs 5 ms slots
    // (engine_core.c ENGINE_TX_SERVICE_MS), so that is the fastest period it
    // can honour rather than merely accept.
    for (int hz : {1, 2, 5, 10, 20, 50, 100, 200})
        m_rateCombo->addItem(tr("%1 Hz").arg(hz), hz);
    // A device Get can carry a rate outside the presets (odd period_ms);
    // insert it rather than silently snapping to 1 Hz.
    if (m_section.transmitRateHz > 0 && m_rateCombo->findData(m_section.transmitRateHz) < 0) {
        int pos = 0;
        while (pos < m_rateCombo->count()
               && m_rateCombo->itemData(pos).toInt() < m_section.transmitRateHz)
            ++pos;
        m_rateCombo->insertItem(pos, tr("%1 Hz").arg(m_section.transmitRateHz),
                                m_section.transmitRateHz);
    }
    m_rateCombo->setCurrentIndex(qMax(0, m_rateCombo->findData(m_section.transmitRateHz)));
    canGrid->addWidget(m_rateCombo, r, 1);
    ++r;

    // Triggered transmit. A combo listing the document's User Conditions rather
    // than the channel picker, because the requirement is literally "only User
    // Conditions": the picker offers the whole catalogue by design and has no
    // filter, and adding one would put a New Channel… button in front of a
    // choice where creating a channel means nothing.
    //
    // The value stored is the condition's OUTPUT CHANNEL — see CommsSection —
    // so the combo's userData is a channel name, not a row index.
    m_txConditionLabel = new QLabel(tr("Transmit Condition :"));
    canGrid->addWidget(m_txConditionLabel, r, 0);
    m_txConditionCombo = new QComboBox;
    for (int i = 0; i < m_config->conditionRows.size(); ++i) {
        const ct::ConditionRow &c = m_config->conditionRows[i];
        if (!c.active || c.outputChannel.isEmpty())
            continue;
        m_txConditionCombo->addItem(tr("%1 — %2").arg(i + 1).arg(
                                        m_config->catalog().labelFor(c.outputChannel)),
                                    c.outputChannel);
    }
    // A condition this document no longer has — deleted since, or a Get from a
    // device whose conditions this build could not name. Kept as a selectable
    // entry rather than silently dropped: losing it here would quietly rewrite
    // the section to a different trigger the next time anyone opened it, and
    // validation is where a dangling reference should be reported.
    if (!m_section.transmitCondition.isEmpty()
        && m_txConditionCombo->findData(m_section.transmitCondition) < 0)
        m_txConditionCombo->addItem(tr("%1 (missing)").arg(m_section.transmitCondition),
                                    m_section.transmitCondition);
    if (m_txConditionCombo->count() == 0)
        m_txConditionCombo->addItem(tr("(no User Conditions defined)"), QString());
    m_txConditionCombo->setCurrentIndex(
        qMax(0, m_txConditionCombo->findData(m_section.transmitCondition)));
    canGrid->addWidget(m_txConditionCombo, r, 1, 1, 3);
    ++r;


    connect(m_triggeredRadio, &QRadioButton::toggled, this,
            &SectionEditorDialog::updateTriggerControls);

    // Compound transmit cadence: Batch (all variants each period) vs Sequential
    // (one variant per period, round-robin). Shown only for compound transmit.
    m_txModeLabel = new QLabel(tr("Transmit Mode :"));
    canGrid->addWidget(m_txModeLabel, r, 0);
    m_txModeCombo = new QComboBox;
    m_txModeCombo->addItem(tr("Batch (all IDs each period)"), int(CompoundTxMode::Batch));
    m_txModeCombo->addItem(tr("Sequential (one ID per period, round-robin)"),
                           int(CompoundTxMode::Sequential));
    m_txModeCombo->setCurrentIndex(qMax(0, m_txModeCombo->findData(int(m_section.compoundTxMode))));
    canGrid->addWidget(m_txModeCombo, r, 1, 1, 3);
    ++r;

    layout->addWidget(canGroup);

    // CAN Triple-specific: gateway routing of this message.
    m_routeGroup = new QGroupBox(tr("Gateway Routing (CAN Triple)"));
    auto *routeRow = new QHBoxLayout(m_routeGroup);
    m_routeEnableCheck = new QCheckBox(tr("Route this message to :"));
    m_routeEnableCheck->setChecked(m_section.routeEnable);
    routeRow->addWidget(m_routeEnableCheck);
    for (int i = 0; i < 3; ++i) {
        m_routeBusCheck[i] = new QCheckBox(tr("CAN %1").arg(i + 1));
        m_routeBusCheck[i]->setChecked(m_section.routeBusMask & (1 << i));
        if (i == m_busIndex)
            m_routeBusCheck[i]->setToolTip(tr("Source bus — never routed back"));
        routeRow->addWidget(m_routeBusCheck[i]);
    }
    routeRow->addStretch();
    // Bus checkboxes are selectable only while routing is enabled (the source
    // bus stays disabled regardless) — and only while the tier permits editing
    // at all. The tier is ANDed in here as well as in
    // applyDeviceKindEnablement, because this lambda runs on every toggle of the
    // Route box and would otherwise hand back three checkboxes the protection
    // pass has just taken away.
    const auto updateRouteBuses = [this]() {
        const bool enabled = m_routeEnableCheck->isChecked() && protocolFieldsEnabled();
        for (int i = 0; i < 3; ++i)
            m_routeBusCheck[i]->setEnabled(enabled && i != m_busIndex);
    };
    connect(m_routeEnableCheck, &QCheckBox::toggled, this, updateRouteBuses);
    updateRouteBuses();
    layout->addWidget(m_routeGroup);

    // Message Relay (v11): a masked-ID gateway rule. Shown only in relay mode.
    m_relayGroup = new QGroupBox(tr("Message Relay"));
    auto *relayGrid = new QGridLayout(m_relayGroup);
    int rr = 0;
    m_bitmaskLabel = new QLabel(tr("Message Bitmask :"));
    relayGrid->addWidget(m_bitmaskLabel, rr, 0);
    m_bitmaskEdit = new QLineEdit;
    m_bitmaskEdit->setValidator(new QRegularExpressionValidator(
        QRegularExpression(QStringLiteral("(0[xX])?[0-9A-Fa-f]{1,8}")), this));
    m_bitmaskEdit->setToolTip(
        tr("A frame matches when (its ID & this mask) == (Base Address & this mask)."));
    connect(m_bitmaskEdit, &QLineEdit::textChanged, this, &SectionEditorDialog::onBitmaskEdited);
    connect(m_bitmaskEdit, &QLineEdit::editingFinished, this,
            &SectionEditorDialog::reformatBitmask);
    relayGrid->addWidget(m_bitmaskEdit, rr, 1);
    m_bitmaskDecLabel = new QLabel;
    relayGrid->addWidget(m_bitmaskDecLabel, rr, 2, 1, 2);
    ++rr;
    m_relayInvertCheck = new QCheckBox(tr("Invert Result (forward the non-matching frames)"));
    m_relayInvertCheck->setChecked(m_section.relayInvert);
    relayGrid->addWidget(m_relayInvertCheck, rr, 0, 1, 4);
    ++rr;
    auto *fwdRow = new QHBoxLayout;
    fwdRow->addWidget(new QLabel(tr("Forward to :")));
    for (int i = 0; i < 3; ++i) {
        m_relayBusCheck[i] = new QCheckBox(tr("CAN %1").arg(i + 1));
        m_relayBusCheck[i]->setChecked(m_section.routeBusMask & (1 << i));
        // A relay listens on this section's bus and forwards to the OTHER two;
        // the source bus is never a forward target.
        m_relayBusCheck[i]->setVisible(i != m_busIndex);
        fwdRow->addWidget(m_relayBusCheck[i]);
    }
    fwdRow->addStretch();
    relayGrid->addLayout(fwdRow, rr, 0, 1, 4);
    m_bitmaskEdit->setText(
        QStringLiteral("0x")
        + QStringLiteral("%1").arg(m_section.relayBitmask, m_section.extended ? 8 : 3, 16,
                                   QLatin1Char('0')).toUpper());
    layout->addWidget(m_relayGroup);

    layout->addStretch();
    onAddressEdited();
    onBitmaskEdited();
}

// Which controls the MESSAGE TYPE allows. Kept apart from onDeviceChanged so
// updateProtectionUi can re-run it: the protection lock disables a set of
// widgets, and when the tier drops back to None every one of them has to be
// restored to whatever the message type says it should be — not blanket-enabled,
// which would hand a relay a Transmit Rate combo. This function is the single
// answer to "what would this control be if nothing were protected?", so the two
// passes cannot disagree about it.
//
// `editable` is the tier's verdict, ANDed into every line rather than applied as
// a second pass, so there is no window in which a locked field is briefly live.
void SectionEditorDialog::applyDeviceKindEnablement(bool editable)
{
    const auto device = SectionDevice(m_deviceCombo->currentData().toInt());
    // Transmit CRC8 is a transmit message in every respect this function rules
    // on — CommsSection::isTransmit() answers true for it — so it joins every
    // `transmit` line below. Left out, the CRC variant would arrive with its
    // Transmit Rate and Message Length dead, which is how "behaves exactly like
    // Transmit Message" quietly stops being true.
    const bool transmit =
        device == SectionDevice::TransmitMessage || device == SectionDevice::TransmitCrc8;
    const bool relay = device == SectionDevice::MessageRelay;
    // A relay is a whole-frame gateway rule — none of the message framing,
    // timeout, alignment, length or channel controls apply to it.
    m_deviceCombo->setEnabled(editable);
    m_cyclicRadio->setEnabled(editable && transmit);
    m_triggeredRadio->setEnabled(editable && transmit);
    m_rateCombo->setEnabled(editable && transmit);
    // Enabled by message kind and tier here; SHOWN or hidden by
    // updateTriggerControls according to Cyclic/Triggered. Both passes have to
    // name these, or a tier dropping back to None re-enables a control the
    // message kind never allowed.
    m_txConditionLabel->setEnabled(editable && transmit);
    m_txConditionCombo->setEnabled(editable && transmit);
    m_timeoutSpin->setEnabled(editable && !transmit && !relay);
    m_defaultTimeoutCheck->setEnabled(editable && !transmit && !relay);
    m_alignmentCombo->setEnabled(editable && !relay);
    m_fdCheck->setEnabled(editable && !relay && m_fdAllowed);
    // Message length is the DLC of a frame you compose; a receive message's
    // length comes from the sender, so only a transmit message sets it.
    m_lengthLabel->setEnabled(editable && transmit);
    m_lengthEdit->setEnabled(editable && transmit);
    m_lengthUnitLabel->setEnabled(editable && transmit);
    // Address format, address and the relay's match rule: always meaningful,
    // always governed by the tier alone.
    m_standardRadio->setEnabled(editable);
    m_extendedRadio->setEnabled(editable);
    m_addressEdit->setEnabled(editable);
    m_bitmaskEdit->setEnabled(editable);
    m_relayInvertCheck->setEnabled(editable);
    m_routeEnableCheck->setEnabled(editable);
    for (int i = 0; i < 3; ++i) {
        // The source bus is never a forward target, whatever the tier says.
        m_routeBusCheck[i]->setEnabled(editable && m_routeEnableCheck->isChecked()
                                       && i != m_busIndex);
        m_relayBusCheck[i]->setEnabled(editable && i != m_busIndex);
    }
    // Single/Compound reshapes the whole frame, so it is protocol like the rest.
    // Null until buildChannelsTab has run.
    if (m_singleRadio) {
        m_singleRadio->setEnabled(editable);
        m_compoundRadio->setEnabled(editable);
    }
    if (m_txModeCombo)
        m_txModeCombo->setEnabled(editable);
    if (m_tabs && m_tabs->count() > 1) {
        m_tabs->setTabText(1, transmit ? tr("Transmitted Channels") : tr("Received Channels"));
        // Neither a relay nor an OFF message carries channels the device acts
        // on: a relay is a whole-frame gateway rule, and an Off message is not
        // laid out at all — findLayoutClashes and mapToDevice both skip it. So
        // the tab that lists them has nothing to offer either way.
        //
        // Grayed rather than hidden, unlike the CRC8 tab beside it, and the
        // difference is not cosmetic: the CRC8 recipe belongs to one message
        // type and does not exist for the others, while these rows DO still
        // exist — switching a configured receive message to Off and back must
        // return them. syncParametersFromUi clears a section's rows for a relay
        // and ONLY for a relay, for exactly that reason.
        m_tabs->setTabEnabled(1, !relay && device != SectionDevice::Off);
    }
    // The CRC8 tab exists for exactly one message type, and for the others it
    // is GONE, not grayed — a disabled tab advertises a feature the message
    // type does not have. Hidden rather than removed (see the constructor:
    // setTabVisible keeps the index claimed). The tier does NOT gate the tab
    // itself: like Parameters, a locked section still SHOWS its recipe, with
    // every widget inside dead. applyCrcEnablement handles those.
    if (m_tabs && m_tabs->count() > 2)
        m_tabs->setTabVisible(2, device == SectionDevice::TransmitCrc8);
    applyCrcEnablement(editable);
}

void SectionEditorDialog::onDeviceChanged()
{
    updateTxModeControls();
    updateTriggerControls();
    updateRelayControls();
    // Receive and transmit lay the frame out differently (see refreshBitTable),
    // so switching between them redraws the map. Null on the first call: the
    // Parameters tab is built before the Channels tab that owns the map.
    refreshBitTable();
    // Applies the message-type rules AND the tier's, in that order and in one
    // pass. Both decide the same widgets' enabled state for different reasons,
    // and only this order gives the tier the final word.
    updateProtectionUi();
}

void SectionEditorDialog::updateRelayControls()
{
    const bool relay =
        SectionDevice(m_deviceCombo->currentData().toInt()) == SectionDevice::MessageRelay;
    if (m_relayGroup)
        m_relayGroup->setVisible(relay);
    // Gateway routing and message relay share the concept of forwarding buses;
    // only one applies at a time, so hide gateway routing in relay mode.
    if (m_routeGroup)
        m_routeGroup->setVisible(!relay);
}

void SectionEditorDialog::updateTriggerControls()
{
    const auto device = SectionDevice(m_deviceCombo->currentData().toInt());
    const bool transmit =
        device == SectionDevice::TransmitMessage || device == SectionDevice::TransmitCrc8;
    // Hidden rather than merely disabled on a Cyclic message: the two settings
    // have no meaning at all without a trigger, and a greyed-out control invites
    // the reader to wonder what would happen if they could reach it.
    const bool show = transmit && m_triggeredRadio && m_triggeredRadio->isChecked();
    if (m_txConditionLabel)
        m_txConditionLabel->setVisible(show);
    if (m_txConditionCombo)
        m_txConditionCombo->setVisible(show);
}

void SectionEditorDialog::updateTxModeControls()
{
    const auto device = SectionDevice(m_deviceCombo->currentData().toInt());
    // Transmit CRC8 included: a compound CRC message still needs its cadence
    // chosen, exactly as a plain compound transmit does.
    const bool transmit =
        device == SectionDevice::TransmitMessage || device == SectionDevice::TransmitCrc8;
    const bool show = transmit && m_compoundRadio && m_compoundRadio->isChecked();
    if (m_txModeLabel)
        m_txModeLabel->setVisible(show);
    if (m_txModeCombo)
        m_txModeCombo->setVisible(show);
}

// Presents the address in the conventional width for the ID format:
// standard -> 0x%03X, extended -> 0x%08X.
void SectionEditorDialog::reformatAddress()
{
    QString text = m_addressEdit->text();
    if (text.startsWith(QLatin1String("0x"), Qt::CaseInsensitive))
        text = text.mid(2);
    bool ok = false;
    quint32 value = text.toUInt(&ok, 16);
    if (!ok)
        value = m_section.baseAddress; // initial fill / unparsable text
    const int width = m_extendedRadio->isChecked() ? 8 : 3;
    const QSignalBlocker blocker(m_addressEdit);
    m_addressEdit->setText(
        QStringLiteral("0x")
        + QStringLiteral("%1").arg(value, width, 16, QLatin1Char('0')).toUpper());
    onAddressEdited();
}

void SectionEditorDialog::onAddressEdited()
{
    QString text = m_addressEdit->text();
    if (text.startsWith(QLatin1String("0x"), Qt::CaseInsensitive))
        text = text.mid(2);
    bool ok = false;
    const quint32 value = text.toUInt(&ok, 16);
    const quint32 maxId = m_extendedRadio->isChecked() ? 0x1FFFFFFFu : 0x7FFu;
    if (!ok)
        m_addressDecLabel->setText(tr("(invalid)"));
    else if (value > maxId)
        m_addressDecLabel->setText(tr("= %1  (exceeds %2 range!)")
                                       .arg(value)
                                       .arg(m_extendedRadio->isChecked() ? tr("29-bit") : tr("11-bit")));
    else
        m_addressDecLabel->setText(tr("= %1 decimal").arg(value));
}

// Same width convention as the address: standard -> 0x%03X, extended ->
// 0x%08X. Used for the relay's match mask.
void SectionEditorDialog::reformatBitmask()
{
    QString text = m_bitmaskEdit->text();
    if (text.startsWith(QLatin1String("0x"), Qt::CaseInsensitive))
        text = text.mid(2);
    bool ok = false;
    quint32 value = text.toUInt(&ok, 16);
    if (!ok)
        value = m_section.relayBitmask;
    const int width = m_extendedRadio->isChecked() ? 8 : 3;
    const QSignalBlocker blocker(m_bitmaskEdit);
    m_bitmaskEdit->setText(
        QStringLiteral("0x")
        + QStringLiteral("%1").arg(value, width, 16, QLatin1Char('0')).toUpper());
    onBitmaskEdited();
}

void SectionEditorDialog::onBitmaskEdited()
{
    QString text = m_bitmaskEdit->text();
    if (text.startsWith(QLatin1String("0x"), Qt::CaseInsensitive))
        text = text.mid(2);
    bool ok = false;
    const quint32 value = text.toUInt(&ok, 16);
    if (!ok)
        m_bitmaskDecLabel->setText(tr("(invalid)"));
    else if (value == 0)
        m_bitmaskDecLabel->setText(tr("= 0  (matches every frame)"));
    else
        m_bitmaskDecLabel->setText(tr("= 0x%1").arg(QString::number(value, 16).toUpper()));
}

// ---------------------------------------------------------------- protection

bool SectionEditorDialog::identityFieldsEnabled() const
{
    // A section that arrived unmarked belongs to whoever is editing it, so the
    // name stays live even after they tick a box — which matters, because the
    // tick-time advice tells them to change a name that discloses the CAN ID and
    // a disabled field would make that impossible to act on.
    //
    // Past that it is the per-section question, not the document-wide one. This
    // used to accept hasCommsPassword() && commsRevealed() — the Protected
    // Comms holder — over EVERY tier, which is the master-key substitution the
    // spec does not authorise at any of them. maySectionLower() now requires this
    // section's own password at all three and accepts nothing else.
    //
    // A KEYLESS section is live, and that arm is load-bearing rather than a
    // convenience. maySectionLower() fails closed for one, so without this the
    // Message Password field would be DISABLED on exactly the sections that need
    // a first password typed into them — every section a Get produces — and the
    // one repair available for that state would be unreachable from the dialog
    // that demands it. Nothing is given away: there is no owner to take the
    // section from, and applyBusSections treats a first key as an addition.
    return m_openedTier == CommsProtection::None || m_ownPasswordProved
           || m_section.messageKey == kNoAccessKey || m_config->maySectionLower(m_section);
}

// Every proof m_openedTier demands, all of it. Communications Setup turns a true
// here into a grant on the document, and a grant stands for the WHOLE of a tier's
// challenge — so a Protected section that has given up only its own password must
// answer false, or the device half is handed out for nothing.
bool SectionEditorDialog::protectionUnlocked() const
{
    if (m_openedTier == CommsProtection::None)
        return false; // nothing was guarded, so nothing was proved
    const Configuration::SectionProofs need = Configuration::proofsRequiredFor(m_openedTier);
    if (need.sectionPassword && !m_ownPasswordProved)
        return false;
    if (need.deviceProof && !m_deviceProved)
        return false;
    return true;
}

// Rule 1's precondition. The stored key belongs to m_openedTier; anywhere else
// the user has to say what guards the tier they have just chosen.
bool SectionEditorDialog::sectionPasswordSatisfied() const
{
    if (m_tier == CommsProtection::None)
        return true; // nothing marked, nothing to guard
    if (m_pendingKey != kNoAccessKey)
        return true; // chosen here, for the marking now selected
    return m_tier == m_openedTier && m_section.messageKey != kNoAccessKey;
}

// The ladder. `level` is the box that was clicked and `ticked` which way it
// went; the LEVEL is recomputed from that alone, never by reading the other two
// boxes back — they are display, and updateProtectionUi() repaints them.
void SectionEditorDialog::onTierBoxToggled(CommsProtection level, bool ticked)
{
    // Ticking box k means "the marking IS k". Unticking it means "unmarked".
    // The boxes no longer nest: a section carries one marking or none, so an
    // untick can only go to None and there is no k-1 to step down to.
    const CommsProtection wanted = ticked ? level : CommsProtection::None;

    // ONE MARKING AT A TIME, AND THE WAY THROUGH IS NAMED. Ticking a second box
    // over a marking that is already set is refused here rather than quietly
    // reinterpreted, because the two markings are guarded by DIFFERENT
    // passwords: the section's key belongs to the marking it was chosen for,
    // and sliding sideways would carry a secret onto a marking nobody chose it
    // for. Clearing is the deliberate act that gives the section back, and it
    // costs the current password.
    if (ticked && m_tier != CommsProtection::None && m_tier != level) {
        // NO SPECIAL CASE FOR A KEYLESS MARKING HERE. The message below says to
        // release the current protection first, and releasing IS unticking the box
        // that is ticked - which is where a keyless section is offered the password
        // that makes releasing possible. One route, and it handles both.
        // THE WORDING NAMES THE STATE AND THE WAY OUT, in that order: what the
        // message currently is, and that the current protection has to be
        // released before another can be taken. It does not send anyone to a
        // button - releasing is unticking the box that is ticked.
        QMessageBox::warning(
            this, windowTitle(),
            tr("Object is currently %1, release Current Message Protection before "
               "enabling another protection model.\n\nUntick %1 - which asks for the "
               "password guarding it - then tick %2 and choose the password for that.")
                .arg(protectionTierWord(m_tier), protectionTierWord(level)));
        updateProtectionUi(); // put the box back
        return;
    }
    if (wanted == m_tier) {
        updateProtectionUi();
        return;
    }

    // ANY move off the tier this section arrived with is the change rule 2
    // guards, in EITHER direction. Raising used to be free, which meant a Read
    // Only section — the tier whose whole promise is "this needs my password to
    // change" — could be walked up to Hidden or Protected by anyone holding the
    // file. One path for both directions, so the two cannot drift apart again.
    //
    // Wandering while already off it is free: the proof is about leaving
    // m_openedTier and it has been given. Coming back TO m_openedTier is free for
    // the same reason, and it also restores the stored password's meaning — see
    // sectionPasswordSatisfied().
    if (wanted != m_openedTier && !authoriseTierChange(m_tier, wanted)) {
        updateProtectionUi(); // put the boxes back the way they were
        return;
    }

    // A FIRST MARKING ASKS FOR ITS PASSWORD THERE AND THEN. Every marked
    // message needs one, and this used to be a note saying so followed by a
    // refusal in accept() several fields later — which is a long way from the
    // tick that caused it. Asked here, the box and the password are one action,
    // and cancelling leaves the section exactly as it was rather than marked and
    // unguarded.
    //
    // Only when there is nothing to guard it already: a section that arrived
    // with a key, or one whose password has been typed into the field during
    // this edit, has an answer to "what protects this" and is not asked twice.
    //
    // "Nothing to guard it" is not the same as "no key stored". A key belongs to
    // the marking it was chosen for, so a section that arrived Hidden and has
    // just been unmarked still HOLDS that key while having nothing it counts
    // for. Asking only when the key is absent let that case through to accept(),
    // which refused it several fields later with a message about a marking the
    // user thought they had just chosen a password for. The question is whether
    // the marking being taken has a password that counts, and only the marking
    // the section ARRIVED on does.
    // PROTECT COMMUNICATION ALWAYS ASKS. The other two skip the prompt when a
    // password that counts is already in hand - the arrived-on marking's stored
    // key, or one chosen earlier this visit - but Protected is the tier whose
    // whole meaning is "deliberate", and ticking it states the password every
    // time. The stored key still never travels un-chosen: for the lesser tiers
    // the skip only ever reuses a key that was entered for THIS marking.
    const bool mustChoose =
        level == CommsProtection::Protected
        || (m_pendingKey == kNoAccessKey
            && (m_section.messageKey == kNoAccessKey || level != m_openedTier));
    if (ticked && mustChoose) {
        if (!promptForFirstSectionPassword(level)) {
            updateProtectionUi(); // cancelled: the box goes back up
            return;
        }
    }

    // A RELEASED MARKING TAKES ITS CHOSEN PASSWORD WITH IT. The pending key
    // was entered for the marking being given up, and letting it sit would hand
    // it silently to whatever box is ticked next - the carry-over the prompt
    // exists to prevent. (The untick that is refused into the repair offer
    // returns above and keeps the key the repair just chose.)
    if (wanted == CommsProtection::None)
        m_pendingKey = kNoAccessKey;

    m_tier = wanted;

    // Two things worth saying at the moment of RAISING, while they can still be
    // acted on.
    //
    // First, that this tier now needs a password of its own before the dialog
    // will close. Said once per editor and never blocking, because marking a
    // message up before choosing the password that will guard it is a normal
    // order to work in — accept() is what finally insists, and saying it here is
    // what stops that refusal arriving as a surprise ten fields later.
    //
    // Second, the default section name is "Receive 0x640", which discloses the
    // very ID Hidden and Protected exist to withhold. No amount of suppression
    // elsewhere helps if the name in the sections list spells it out.
    if (ticked) {
        if (!m_warnedNoPassword && !sectionPasswordSatisfied()) {
            m_warnedNoPassword = true;
            QMessageBox::information(
                this, windowTitle(),
                m_tier == CommsProtection::Protected
                    ? tr("Protect Communication needs TWO things, and this section has neither "
                         "yet.\n\nFill in Message Password below — every marked message carries "
                         "its own, and this dialog will not close until it does.\n\nUnticking the "
                         "box again also needs the Protected Comms password confirmed by a "
                         "connected CAN Triple. Set that one under Online > Set Access "
                         "Passwords….")
                    : tr("This section has no password of its own yet.\n\nFill in Message "
                         "Password below: every marked message carries one, and this dialog will "
                         "not close while a box is ticked and that field is empty."));
        }
        QString typed = m_addressEdit ? m_addressEdit->text() : QString();
        if (typed.startsWith(QLatin1String("0x"), Qt::CaseInsensitive))
            typed = typed.mid(2);
        const QString address =
            QStringLiteral("0x") + QString::number(typed.toUInt(nullptr, 16), 16).toUpper();
        // Only for the tiers that actually withhold the ID. Read Only shows the
        // whole message, so a name containing the ID gives nothing away.
        if (m_tier >= CommsProtection::Hidden
            && (m_nameEdit->text().isEmpty()
                || m_nameEdit->text().contains(address, Qt::CaseInsensitive))) {
            QMessageBox::information(
                this, windowTitle(),
                tr("This message's name is \"%1\", which contains its CAN ID — and the name is "
                   "still shown while the rest of the message is withheld.\n\n"
                   "Give it a name that does not disclose the ID, such as a description of what "
                   "it carries.")
                    .arg(m_nameEdit->text().isEmpty() ? tr("(automatic)") : m_nameEdit->text()));
            m_nameEdit->setFocus();
            m_nameEdit->selectAll();
        }
    }

    updateProtectionUi();
}

// The password a NEW marking is guarded by, asked at the moment the box is
// ticked. Returns false if the user backed out, and writes nothing in that case.
//
// The typed text is derived and validated HERE, immediately: deriveAccessKey()
// turns it into a key, commsPasswordSlotFor() checks it names one of the
// document's four slots, and the result is stashed in m_pendingKey - accept()
// consumes that, it does not re-derive. There is no Message Password field on
// the tab any more.
// THE PASSWORD MUST BE ONE OF THE DOCUMENT'S FOUR, and this is where that is
// enforced. A message does not carry a password of its own any more: it NAMES
// one of four, and the wire carries the slot number rather than a key. A typed
// password that matches none of them has no slot to travel as, which is why
// validation reports it as an Error and why the honest thing here is to refuse
// rather than store something unsendable.
bool SectionEditorDialog::promptForFirstSectionPassword(CommsProtection level)
{
    // NOTHING TO MATCH AGAINST is a different problem and gets a different
    // answer. Sending someone to a password prompt they cannot possibly satisfy
    // is worse than telling them where the passwords are set.
    if (m_config->commsPasswordsInUse() == 0) {
        QMessageBox::warning(
            this, windowTitle(),
            tr("No Message Passwords are set for this configuration, so there is nothing "
               "to guard a marking with.\n\nSet one on the Passwords tab of "
               "Communications Setup, then mark this message."));
        return false;
    }

    const QString who = m_section.name.isEmpty() ? tr("this message") : m_section.name;
    for (;;) {
        bool ok = false;
        const QString typed = QInputDialog::getText(
            this, windowTitle(),
            tr("Marking %1 as %2 guards it with one of this configuration's four Message "
               "Passwords.\n\nType the password that will guard it:")
                .arg(who, protectionTierWord(level)),
            QLineEdit::Password, QString(), &ok);
        if (!ok)
            return false; // backed out: the caller unticks the box again

        const AccessKey key = deriveAccessKey(typed);
        const int slot = m_config->commsPasswordSlotFor(key);
        if (slot != 0) {
            m_pendingKey = key;
            return true;
        }
        // Re-asked rather than refused outright, so a mistyped password does not
        // cost the marking as well. The message says which of the two things is
        // wrong, because "that is not one of the four" and "that is too short to
        // be any of them" send a user to different places.
        QMessageBox::warning(
            this, windowTitle(),
            typed.length() < kMinPasswordLength
                ? tr("A Message Password is at least %1 characters, so that is not one of "
                     "the four.")
                      .arg(kMinPasswordLength)
                : tr("That is not one of this configuration's four Message Passwords.\n\n"
                     "Use one of them, or add it on the Passwords tab of Communications "
                     "Setup first."));
    }
}


// ONE path for every tier change: prove what is being GIVEN UP, then whatever the
// destination additionally demands. Reports its own failures; true means the move
// may proceed, and the answers are remembered for the rest of this editor so a
// user changing their mind twice is not interrogated twice.
bool SectionEditorDialog::authoriseTierChange(CommsProtection from, CommsProtection to)
{
    // 0. A KEYLESS marking cannot be LOWERED, by anybody, and this dialog has to
    //    say so itself rather than letting the user untick the box and meet
    //    Configuration::applyBusSections' refusal at OK time, three dialogs
    //    later, with an evening's other edits riding on the same button.
    //
    //    RAISING is not blocked HERE, and this is no longer the whole story:
    //    the model still treats a raise as free at every tier, and nothing is
    //    given away by marking a message up, so this function permits it. What
    //    stops it on a MARKED section is the one-marking-at-a-time guard in
    //    onTierBoxToggled, which runs before this and never reaches it. The
    //    dialog is therefore strictly stricter than Configuration about raising,
    //    and deliberately so: a message wears one marking, and swapping it is
    //    two decisions rather than one. Lowering is where the two must agree,
    //    and there they still do.
    //
    //    The two-step repair below is the whole of the way out, and it is offered
    //    here unconditionally because REACHING THIS LINE MEANS THE EDITOR IS
    //    ALREADY OPEN — which is the one thing a keyless concealed section cannot
    //    arrange for itself. Communications Setup refuses to open one, so the only
    //    routes here are a keyless Read Only section (which conceals nothing and
    //    always opens) and a keyless section raised inside this window and lowered
    //    again in the same sitting. Configuration::applyBusSections says less than
    //    this, and deliberately: it cannot know an editor is open, so at the
    //    concealing tiers it names removal alone.
    //
    //    Setting a FIRST password is free — accept() takes it, applyBusSections
    //    takes it — and the untick then has something to be authorised by on the
    //    next visit.
    if (to < from && from != CommsProtection::None
        && m_section.messageKey == kNoAccessKey && m_pendingKey == kNoAccessKey) {
        // THE REPAIR IS OFFERED HERE, not described and left to the user to
        // arrange. With no Message Password field on the tab there is nowhere
        // else it could be offered, and this is the ONE state that needs it:
        // every section a Get returns arrives marked and keyless, and a marking
        // is released by proving the password guarding it, which does not exist.
        //
        // Giving it one is free - Configuration::applyBusSections calls a first
        // password "always free" - and it is what makes the release possible on
        // the next visit. TWO VISITS IS STRUCTURAL, not a UI choice:
        // maySectionLower asks the DOCUMENT's copy of the section, so the key
        // has to be committed before it can authorise anything.
        const auto answer = QMessageBox::question(
            this, windowTitle(),
            tr("\"%1\" is marked %2 and has no Message Password - it arrived that way, from "
               "a configuration read back off a device or from a file written before "
               "markings carried passwords.\n\nReleasing a marking is authorised by the "
               "password guarding it, and there is none, so there is nothing that could "
               "authorise this.\n\nGive it a Message Password now? It costs nothing, and "
               "the marking can be released with it the next time this message is opened.")
                .arg(m_section.name.isEmpty() ? tr("This message") : m_section.name,
                     protectionTierWord(from)),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
        if (answer == QMessageBox::Yes && promptForFirstSectionPassword(from)) {
            QMessageBox::information(
                this, windowTitle(),
                tr("\"%1\" now has a Message Password.\n\nPress OK to keep it, then open "
                   "this message again - releasing %2 will ask for it.")
                    .arg(m_section.name.isEmpty() ? tr("This message") : m_section.name,
                         protectionTierWord(from)));
        }
        // Either way the move does not happen in this visit.
        return false;
    }

    // 1. Give up `from`. Rule 2: "prove the CURRENT password first". This is the
    //    half that used to run only when the tier went DOWN, which left raising
    //    Read Only to Hidden free to anyone holding the file.
    if (!proveSectionPassword(from))
        return false;

    // 2. THE DEVICE IS NO LONGER ASKED AT THE BOUNDARY. Crossing Protect
    //    Communication used to demand the Protected Comms password confirmed by
    //    a connected unit, in both directions - which made the tier impossible
    //    to set up offline, on the bench where configurations are actually
    //    written. The enforcement MOVED rather than disappeared, to the two
    //    places that still hold:
    //
    //      * SEND: a configuration carrying Protect Communication goes only to
    //        a unit that proves the same Protected Comms password - the gate in
    //        onSendConfiguration, which is where a device is guaranteed to be
    //        present and where the tier's promise actually bites.
    //      * OPEN: an existing Protected message is still unlocked with its
    //        Message Password AND the device confirmation, so releasing one
    //        that a configuration arrived with still involves the hardware -
    //        the editor cannot be reached without it.
    //
    //    What is genuinely free now is TICKING the box on a message you can
    //    already see, guarded by the Message Password prompt alone. The old
    //    symmetry argument (free to enter, hardware to leave, is a trap) is
    //    answered the same way: entering and leaving inside one editor session
    //    cost the same, and the hardware stands at the doors it can stand at.

    // 3. Rule 2 again: the old key does not follow the section onto a new tier.
    //    The FIELD is cleared so a half-typed replacement cannot be mistaken for
    //    the answer to the prompt that just ran; the STORED key is left alone and
    //    stops counting all by itself, because sectionPasswordSatisfied() ties it
    //    to m_openedTier. accept() is what refuses to close over the gap.
    //
    //    Not destroyed even so — see syncParametersFromUi. An unmarked section
    //    carrying a stale key guards nothing, and a user who moves the tier back
    //    where it was gets their password's meaning back with it.
    if (to != m_openedTier && to != CommsProtection::None)
        m_pendingKey = kNoAccessKey;
    return true;
}

// This section's own password, checked here against messageKey, with no device
// involved at any tier.
//
// There is deliberately NO fallback to the document's Protected Comms
// password. It is not a master key over Read Only and Hidden and never was
// authorised as one; and since 2.3.1 it is not a substitute for Protected's
// section password either — that tier needs both halves, and this is the half a
// file can answer. Accepting it here also meant a document with no such password
// — the normal shape when per-section passwords are used — unlocked every section
// for free, because commsRevealed() is true whenever nothing is set.
bool SectionEditorDialog::proveSectionPassword(CommsProtection from)
{
    if (m_ownPasswordProved)
        return true;
    if (!Configuration::proofsRequiredFor(from).sectionPassword) {
        // from == None: there is no tier being given up, so nothing to prove.
        m_ownPasswordProved = true;
        return true;
    }
    if (m_section.messageKey == kNoAccessKey) {
        // Nothing guards it, so there is nothing to ASK for — and this no longer
        // means "so help yourself", which is the reversal 2.3.2 made. The only
        // moves that reach here on a keyless section are a raise from UNMARKED
        // (free, and nothing is given away by marking a message up) and
        // accept()'s first-password check (free, and it is the repair). A raise
        // from one MARKING to another no longer arrives at all: onTierBoxToggled
        // refuses it above, and on a keyless section it names the same two-visit
        // repair step 0 does rather than sending the user to Clear... for it.
        // A LOWERING is
        // refused before this runs, in authoriseTierChange step 0, because
        // Configuration::maySectionLower fails closed for a keyless marking and
        // the two must give the same answer.
        m_ownPasswordProved = true;
        return true;
    }

    QString prompt = tr("\"%1\" is marked %2. Enter this section's own Message Password to "
                        "change that.")
                         .arg(m_section.name, protectionTierWord(from));
    for (;;) {
        bool ok = false;
        const QString typed =
            QInputDialog::getText(this, windowTitle(),
                                  prompt + tr("\n\nMessage Password :"), QLineEdit::Password,
                                  QString(), &ok);
        if (!ok)
            return false;
        // Derived and compared, never held: the same treatment every other
        // password in this application gets. An empty answer cannot be right —
        // kNoAccessKey is the "no password" sentinel and was ruled out above.
        if (!typed.isEmpty() && deriveAccessKey(typed) == m_section.messageKey) {
            m_ownPasswordProved = true;
            return true;
        }
        // Says nothing about how wrong it was — no "close", no length hint.
        prompt = tr("That password is not correct.");
    }
}

// THE point of the Protected tier, and the one thing that makes it stronger than
// Hidden: the password has to be checked by a live device, not by a verifier
// sitting in the same file as the thing it protects. Holding the .ct3 is
// deliberately not enough, so there is no offline fallback here and there must
// never be one.
//
// In particular there is deliberately NO short-circuit on
// Configuration::maySectionLower() / commsRevealed(). That is the document's own
// Protected Comms grant, checked against a verifier stored in the same file
// as the message it guards — precisely the local check this tier exists to be
// stronger than.
bool SectionEditorDialog::proveDeviceForProtected()
{
    if (m_deviceProved)
        return true;
    if (!m_prover) {
        QMessageBox::warning(
            this, windowTitle(),
            tr("Moving \"%1\" on or off Protect Communication needs the Protected Comms "
               "password checked by a connected CAN Triple, and this window has no device to "
               "ask.\n\n"
               "Open Connections > Communications from the main window with a device connected. "
               "The section can still be removed without one.")
                .arg(m_section.name));
        return false;
    }
    // The prover reports its own failures — no device, wrong password, a unit
    // with no such password set — so there is nothing to add here.
    if (!m_prover())
        return false;
    m_deviceProved = true;
    return true;
}

// Everything the tier governs, repainted from m_tier in one pass. Called after
// every transition and at the end of onDeviceChanged(), which decides the same
// widgets' enabled state for a different reason — this runs last so the lock
// wins over the message-type rules rather than racing them.
void SectionEditorDialog::updateProtectionUi()
{
    // Guarded on the LAST widget buildParametersTab creates, not the first one
    // this function touches: applyDeviceKindEnablement below reaches the relay
    // controls at the bottom of that tab, and a guard on the checkboxes at the
    // top would let a mid-construction call through to a null pointer.
    if (!m_relayInvertCheck || !m_protectionNote)
        return;
    {
        // Blocked, or setting the boxes to match the level would re-enter
        // onTierBoxToggled and recompute the level from the repaint.
        const QSignalBlocker b1(m_readOnlyCheck);
        const QSignalBlocker b2(m_hiddenCheck);
        const QSignalBlocker b3(m_protectCheck);
        // ONE BOX, NOT A LADDER. These used to be nested — >= rather than == —
        // so a Protected section showed all three ticked and the reader had to
        // know that Protected "contains" Hidden to make sense of it. The tier is
        // a single value in the model and it reads as one here now: exactly one
        // box, or none.
        m_readOnlyCheck->setChecked(m_tier == CommsProtection::ReadOnly);
        m_hiddenCheck->setChecked(m_tier == CommsProtection::Hidden);
        m_protectCheck->setChecked(m_tier == CommsProtection::Protected);
    }

    // Everything that describes the protocol, in one pass that also restores it
    // when the tier drops back to None. A Read Only section now OPENS here —
    // that is the change this release is for — and it opens with all of this
    // dead, which is what "visible but not editable" has to look like from the
    // inside.
    const bool editable = protocolFieldsEnabled();
    applyDeviceKindEnablement(editable);
    // The channel rows are the message's bit layout, so they move with it, and
    // the identifier table on a compound message is the multiplexor — arguably
    // the most sensitive part of the protocol. updateButtons() owns all five
    // buttons and already ANDs the tier in, so it is called rather than
    // duplicated: two places deciding one button's enabled state is how one of
    // them ends up stale. Null before buildChannelsTab has run.
    if (m_addButton)
        updateButtons();

    const bool identity = identityFieldsEnabled();
    m_nameEdit->setEnabled(identity);

    QString note;
    switch (m_tier) {
    case CommsProtection::None:
        note = tr("Not protected. Everything on this page is editable and the whole message "
                  "is shown everywhere.");
        break;
    case CommsProtection::ReadOnly:
        note = tr("Read Only — visible to everyone, not editable. Accident prevention, not "
                  "security: the message can be read in full and removed, so removing and "
                  "retyping it reproduces it without the password.");
        break;
    case CommsProtection::Hidden:
        note = tr("Hidden — the CAN ID, frame layout, timing and bit positions are withheld "
                  "from anyone without this section's password. Channel names stay visible. "
                  "A convention of this application: a plain .ct3 still contains every field "
                  "in clear text, and so does the device.");
        break;
    case CommsProtection::Protected:
        note = tr("Protect Communication — withheld exactly as Hidden, and moving it on or off "
                  "needs BOTH this section's own Message Password and the Protected Comms "
                  "password proved against a connected device. The device enforces nothing else; "
                  "it carries the marking so a Get followed by a Send cannot launder this "
                  "message into an ordinary one.");
        break;
    }
    // Said last, because it is the thing that will stop OK working and the user
    // needs it in front of them while the field is still empty.
    if (!sectionPasswordSatisfied())
        note += tr("  Fill in Message Password: this dialog will not close while a box is "
                   "ticked and that field is empty.");
    if (!editable && (m_ownPasswordProved || m_deviceProved))
        note += tr("  You have unlocked this section for this session: untick the boxes to "
                   "edit it.");
    else if (!editable && m_openedTier != CommsProtection::None)
        note += tr("  Untick to edit — that is what the password is for.");
    m_protectionNote->setText(note);
}

void SectionEditorDialog::syncParametersFromUi()
{
    m_section.device = SectionDevice(m_deviceCombo->currentData().toInt());
    m_section.alignment = SectionAlignment(m_alignmentCombo->currentData().toInt());
    m_section.receiveTimeoutMs = m_timeoutSpin->value();
    m_section.defaultValueOnTimeout = m_defaultTimeoutCheck->isChecked();
    // ONE ordered level, taken from the ladder rather than reassembled from the
    // three boxes — the boxes are the level's display, not its storage.
    m_section.protection = m_tier;
    // Empty means "keep the current password", which is what lets a marked
    // message be opened, retimed and accepted without retyping one. A typed
    // password replaces it; the length rule is checked in accept() before we get
    // here, and so is rule 1 — by the time this runs, an empty field on a marked
    // tier can only mean the tier is still m_openedTier and the stored key is the
    // one that was chosen to guard it (sectionPasswordSatisfied()). The stale-key
    // case this cannot reach is the one accept() refuses.
    if (m_pendingKey != kNoAccessKey)
        m_section.messageKey = m_pendingKey;
    // The password is deliberately NOT destroyed when the boxes clear. It is the
    // secret that AUTHORISES the untick, and wiping it in the same breath as the
    // untick would delete the one thing standing between the next person and a
    // free re-tick-and-untick — and would silently lose a password the user may
    // well intend to re-arm the section with a moment later. An unmarked section
    // carrying a key guards nothing; that is harmless, and it is recoverable.
    // Note it stops COUNTING for a marked tier the moment the tier moves off the
    // one it was chosen for — that is sectionPasswordSatisfied()'s job, and it is
    // how "do not silently carry the old key onto the new attribute" is enforced
    // without throwing the key away.
    m_section.extended = m_extendedRadio->isChecked();
    QString text = m_addressEdit->text();
    if (text.startsWith(QLatin1String("0x"), Qt::CaseInsensitive))
        text = text.mid(2);
    m_section.baseAddress = text.toUInt(nullptr, 16);
    m_section.fd = m_fdCheck->isChecked();
    bool lengthOk = false;
    const int length = m_lengthEdit->text().toInt(&lengthOk);
    if (lengthOk) // keep the previous value while the field is cleared mid-edit
        m_section.messageLengthBytes = length;
    m_section.cyclic = m_cyclicRadio->isChecked();
    const int newRate = m_rateCombo->currentData().toInt();
    // Changing the dropdown states new intent, so derive the period from it;
    // an untouched rate keeps the exact device period from a Get.
    if (newRate != m_section.transmitRateHz)
        m_section.transmitPeriodMs = 0;
    m_section.transmitRateHz = newRate;
    // Read back whatever the combo holds, even on a Cyclic message where the row
    // is hidden. Clearing it here would throw away the user's choice the moment
    // they flipped to Cyclic and back, and mapToDevice ignores the field unless
    // the section is Triggered anyway.
    m_section.transmitCondition = m_txConditionCombo->currentData().toString();
    m_section.routeEnable = m_routeEnableCheck->isChecked();
    m_section.compound = m_compoundRadio->isChecked();
    m_section.compoundTxMode = CompoundTxMode(m_txModeCombo->currentData().toInt());

    // The forward-bus set lives in routeBusMask for both gateway routing and
    // relays; read it from whichever group is active. The source bus is never a
    // target either way.
    int mask = 0;
    if (m_section.isRelay()) {
        for (int i = 0; i < 3; ++i)
            if (m_relayBusCheck[i]->isChecked() && i != m_busIndex)
                mask |= 1 << i;
    } else {
        for (int i = 0; i < 3; ++i)
            if (m_routeBusCheck[i]->isChecked() && i != m_busIndex)
                mask |= 1 << i;
    }
    m_section.routeBusMask = mask;

    m_section.relayInvert = m_relayInvertCheck->isChecked();
    QString maskText = m_bitmaskEdit->text();
    if (maskText.startsWith(QLatin1String("0x"), Qt::CaseInsensitive))
        maskText = maskText.mid(2);
    m_section.relayBitmask = maskText.toUInt(nullptr, 16);

    // A relay is a whole-frame gateway rule and carries no channels. If this
    // section was re-typed from a Receive/Transmit (possibly compound) message,
    // drop its now-meaningless channel data so it can't leak into the channel
    // usage report or the sections list as phantom channels.
    if (m_section.isRelay()) {
        m_section.compound = false;
        m_section.rows.clear();
        m_section.identifiers.clear();
    }

    // The CRC recipe is read back only for the device it belongs to. For every
    // other type the stored fields are LEFT ALONE rather than cleared — the
    // message password's treatment, and for the same reason: a section switched
    // off Transmit CRC8 and back gets its recipe back, and nothing stale can
    // leak out because toJson and the mapper only carry these fields when the
    // device is TransmitCrc8. Widget-guarded like the rest of this function is
    // combo-guarded: sync runs from the row editors' paths too, and must not
    // assume which tabs exist yet.
    if (m_section.isCrc8() && m_crcChannelEdit) {
        m_section.crcChannel = ct::channelField(m_crcChannelEdit);
        m_section.crcByteLocation = m_crcByteCombo->currentData().toInt();
        // Like the length above: keep the previous value while a field is
        // cleared mid-edit. accept() is where an unparsable byte is refused.
        bool ok = false;
        int value = parseHexByte(m_crcPolyEdit->text(), &ok);
        if (ok)
            m_section.crcPolynomial = value;
        value = parseHexByte(m_crcInitEdit->text(), &ok);
        if (ok)
            m_section.crcInitValue = value;
        value = parseHexByte(m_crcXorEdit->text(), &ok);
        if (ok)
            m_section.crcFinalXor = value;
        m_section.crcRefIn = m_crcRefInCheck->isChecked();
        m_section.crcRefOut = m_crcRefOutCheck->isChecked();

        QList<CommsSection::CrcElement> elements;
        const int count = m_crcCountSpin->value();
        elements.reserve(count);
        for (int i = 0; i < count; ++i) {
            CommsSection::CrcElement e;
            e.type = m_crcElemType[i]->currentData().toInt();
            if (e.type == CommsSection::CrcElement::Id) {
                e.value = m_crcElemId[i]->currentData().toInt();
            } else if (e.type == CommsSection::CrcElement::Raw) {
                bool rawOk = false;
                const int raw = parseHexByte(m_crcElemRaw[i]->text(), &rawOk);
                const bool wasRaw = i < m_section.crcElements.size()
                                    && m_section.crcElements.at(i).type
                                           == CommsSection::CrcElement::Raw;
                e.value = rawOk ? raw : (wasRaw ? m_section.crcElements.at(i).value : 0);
            } else {
                e.value = m_crcElemData[i]->currentData().toInt();
            }
            elements.append(e);
        }
        m_section.crcElements = elements;
    }

    m_section.name = m_nameEdit->text().trimmed();

    // A RELEASED PLACEHOLDER GETS A REAL NAME BACK. A Get into a fresh document
    // names a concealing section "Hidden message 1" / "Protected message 1",
    // because the ordinary generated name spells out the CAN ID the marking
    // withholds. Once the marking no longer conceals the ID - released, or
    // lowered to Read Only, which shows the whole message - the placeholder is
    // doing nothing except being wrong, so it is treated as no name at all and
    // regenerated below. Matched EXACTLY (the stem and a number), so a name a
    // user actually typed is never touched; someone who genuinely calls a
    // message "Hidden message 1" and then unhides it loses only a name that was
    // indistinguishable from the placeholder.
    if (m_tier < CommsProtection::Hidden) {
        static const QRegularExpression placeholder(
            QStringLiteral("^(hidden|protected) message [0-9]+$"),
            QRegularExpression::CaseInsensitiveOption);
        if (placeholder.match(m_section.name).hasMatch())
            m_section.name.clear();
    }

    if (m_section.name.isEmpty()) {
        const QString direction = m_section.isTransmit() ? tr("Transmit")
                                  : m_section.isRelay()  ? tr("Relay")
                                                         : tr("Receive");
        m_section.name = QStringLiteral("%1 0x%2").arg(direction,
            QString::number(m_section.baseAddress, 16).toUpper());
    }
}

void SectionEditorDialog::buildChannelsTab(QWidget *page)
{
    auto *pageLayout = new QVBoxLayout(page);
    auto *layout = new QHBoxLayout;
    // 1:2 — spare height goes mostly to the frame map. The identifier pane is
    // empty in Single mode, so an even split would grow a blank box instead.
    pageLayout->addLayout(layout, 1);

    // Left column: message type + (compound) identifiers
    auto *leftColumn = new QVBoxLayout;
    auto *typeGroup = new QGroupBox(tr("Message Type"));
    auto *typeRow = new QHBoxLayout(typeGroup);
    m_singleRadio = new QRadioButton(tr("Single"));
    m_compoundRadio = new QRadioButton(tr("Compound"));
    m_singleRadio->setChecked(!m_section.compound);
    m_compoundRadio->setChecked(m_section.compound);
    typeRow->addWidget(m_singleRadio);
    typeRow->addWidget(m_compoundRadio);
    typeRow->addStretch();
    leftColumn->addWidget(typeGroup);
    connect(m_singleRadio, &QRadioButton::toggled, this, &SectionEditorDialog::onMessageTypeToggled);

    m_typeStack = new QStackedWidget;
    auto *singlePage = new QWidget; // empty — single mode has no extra controls
    m_typeStack->addWidget(singlePage);

    auto *compoundPage = new QWidget;
    auto *compoundLayout = new QVBoxLayout(compoundPage);
    compoundLayout->setContentsMargins(0, 0, 0, 0);
    compoundLayout->addWidget(new QLabel(tr("Identifiers :")));
    m_identifierTree = new QTreeWidget;
    m_identifierTree->setHeaderLabels({tr("Number"), tr("Offset"), tr("ID"), tr("ID Mask")});
    m_identifierTree->setRootIsDecorated(false);
    connect(m_identifierTree, &QTreeWidget::currentItemChanged, this, [this]() {
        rebuildChannelList();
        updateButtons();
    });
    connect(m_identifierTree, &QTreeWidget::itemDoubleClicked, this,
            &SectionEditorDialog::onEditIdentifier);
    compoundLayout->addWidget(m_identifierTree);
    auto *identButtons = new QHBoxLayout;
    m_identChangeButton = new QPushButton(tr("Change…"));
    connect(m_identChangeButton, &QPushButton::clicked, this,
            &SectionEditorDialog::onEditIdentifier);
    m_identClearButton = new QPushButton(tr("Clear"));
    connect(m_identClearButton, &QPushButton::clicked, this,
            &SectionEditorDialog::onClearIdentifier);
    identButtons->addWidget(m_identChangeButton);
    identButtons->addWidget(m_identClearButton);
    identButtons->addStretch();
    compoundLayout->addLayout(identButtons);
    m_typeStack->addWidget(compoundPage);

    leftColumn->addWidget(m_typeStack, 1);
    layout->addLayout(leftColumn, 1);

    // Right column: channels of the section / selected identifier
    auto *rightColumn = new QVBoxLayout;
    m_channelListLabel = new QLabel(tr("Channels"));
    rightColumn->addWidget(m_channelListLabel);
    m_channelList = new QListWidget;
    // Each row carries its signal's colour from the frame map, so the list and
    // the map read as one thing. ColorItemDelegate keeps that colour under
    // hover and selection, which the stock delegate would paint over.
    m_channelList->setItemDelegate(new ColorItemDelegate(m_channelList));
    connect(m_channelList, &QListWidget::currentRowChanged, this, [this]() {
        updateButtons();
        updateBitSelection();
    });
    connect(m_channelList, &QListWidget::itemDoubleClicked, this,
            &SectionEditorDialog::onChangeRow);
    rightColumn->addWidget(m_channelList, 1);
    auto *rowButtons = new QHBoxLayout;
    m_addButton = new QPushButton(tr("Add…"));
    connect(m_addButton, &QPushButton::clicked, this, &SectionEditorDialog::onAddRow);
    m_changeButton = new QPushButton(tr("Change…"));
    connect(m_changeButton, &QPushButton::clicked, this, &SectionEditorDialog::onChangeRow);
    m_removeButton = new QPushButton(tr("Remove"));
    connect(m_removeButton, &QPushButton::clicked, this, &SectionEditorDialog::onRemoveRow);
    rowButtons->addWidget(m_addButton);
    rowButtons->addWidget(m_changeButton);
    rowButtons->addWidget(m_removeButton);
    rowButtons->addStretch();
    rightColumn->addLayout(rowButtons);
    layout->addLayout(rightColumn, 1);

    // Frame layout map, under both panes so it gets the full dialog width.
    m_bitGroup = new QGroupBox(tr("Frame Layout"));
    auto *bitLayout = new QVBoxLayout(m_bitGroup);
    m_bitTable = new BitLayoutTable;
    // A scroll area's own sizeHint is generous; left alone it would push the
    // dialog's minimum height past a 768-tall screen. It scrolls, so a small
    // floor is enough — the frame map is the first thing given spare height.
    m_bitTable->setMinimumHeight(110);
    m_bitTable->setSizeAdjustPolicy(QAbstractScrollArea::AdjustIgnored);
    // The map drives the list as well as following it: clicking a coloured cell
    // is often how you find out which channel owns a bit you didn't expect.
    connect(m_bitTable, &BitLayoutTable::rowClicked, this, [this](int index) {
        if (index >= 0 && index < m_channelList->count())
            m_channelList->setCurrentRow(index);
    });
    bitLayout->addWidget(m_bitTable, 1);
    m_bitCaption = new QLabel;
    m_bitCaption->setWordWrap(true);
    bitLayout->addWidget(m_bitCaption);
    pageLayout->addWidget(m_bitGroup, 1);
}

// The CRC8 tab: the recipe for the checksum a Transmit CRC8 section stamps into
// one byte of the frame it composes. Populated straight from m_section like the
// other two tabs, and read back in syncParametersFromUi.
void SectionEditorDialog::buildCrcTab(QWidget *page)
{
    // kCrcElementSlots restates the WIRE's maximum so the header can size the
    // widget arrays without including the wire header. If the firmware ever
    // grows the element table this trips at compile time, instead of the UI
    // silently truncating every recipe to fifteen.
    static_assert(kCrcElementSlots == CRC8_MAX_ELEMENTS,
                  "the pre-built element rows must match the wire's element table");

    auto *layout = new QVBoxLayout(page);

    auto *paramGroup = new QGroupBox(tr("CRC8 Checksum"));
    auto *grid = new QGridLayout(paramGroup);
    grid->setColumnStretch(1, 1);
    int r = 0;

    // Channel + Select… — the AddChannelDialog pattern, and its rule with it:
    // the box DISPLAYS "Name Unit" and REMEMBERS the bare name, so it is set
    // through setChannelField() and read through channelField(), never text().
    grid->addWidget(new QLabel(tr("Channel :")), r, 0);
    m_crcChannelEdit = new QLineEdit;
    m_crcChannelEdit->setReadOnly(true);
    ct::setChannelField(m_crcChannelEdit, m_section.crcChannel, m_config->catalog());
    grid->addWidget(m_crcChannelEdit, r, 1, 1, 2);
    m_crcSelectButton = new QPushButton(tr("Select…"));
    m_crcSelectButton->setToolTip(
        tr("The channel the computed checksum is published to, so the value on the wire "
           "is watchable like any other channel."));
    connect(m_crcSelectButton, &QPushButton::clicked, this,
            &SectionEditorDialog::onSelectCrcChannel);
    grid->addWidget(m_crcSelectButton, r, 3);
    ++r;

    grid->addWidget(new QLabel(tr("CRC8 Byte Location :")), r, 0);
    m_crcByteCombo = new QComboBox;
    // "Byte N" — the same words the frame map's row headers use, because this
    // combo and the map's shaded row are one fact in two places.
    for (int b = 0; b < 8; ++b)
        m_crcByteCombo->addItem(tr("Byte %1").arg(b), b);
    m_crcByteCombo->setCurrentIndex(qBound(0, m_section.crcByteLocation, 7));
    m_crcByteCombo->setToolTip(
        tr("Which byte of the frame the checksum is stamped into, after every other byte "
           "is final. Shown shaded in the Channels tab's frame layout map; it must lie "
           "inside the Message Length."));
    // The map's shading has to follow this combo live: the map is how the user
    // judges whether the location collides with a channel, and a map that only
    // catches up on OK would defeat the point of shading it at all.
    connect(m_crcByteCombo, &QComboBox::currentIndexChanged, this,
            &SectionEditorDialog::refreshBitTable);
    grid->addWidget(m_crcByteCombo, r, 1);
    ++r;

    // Polynomial / Init / Final XOR: the standard CRC-8 parameterisation, as
    // hex line edits rather than spin boxes — CRC constants are quoted in hex
    // everywhere they are published, and 29 is not how anyone knows 0x1D.
    // The fallback is a pointer-to-member so editingFinished reformats against
    // the value the SECTION currently holds (which syncParametersFromUi keeps
    // fresh), the same recovery reformatAddress gives the base address.
    const auto addHexRow = [&](const QString &label, const QString &tip,
                               int CommsSection::*field) {
        grid->addWidget(new QLabel(label), r, 0);
        auto *edit = new QLineEdit;
        edit->setValidator(new QRegularExpressionValidator(
            QRegularExpression(QStringLiteral("(0[xX])?[0-9A-Fa-f]{1,2}")), this));
        edit->setToolTip(tip);
        reformatHexByteEdit(edit, m_section.*field); // initial fill, canonical 0xNN
        connect(edit, &QLineEdit::editingFinished, this,
                [this, edit, field]() { reformatHexByteEdit(edit, m_section.*field); });
        grid->addWidget(edit, r, 1);
        ++r;
        return edit;
    };
    m_crcPolyEdit = addHexRow(
        tr("CRC8 Polynomial :"),
        tr("The generator polynomial with the x^8 term implicit — the firmware's "
           "convention. 0x1D is SAE J1850, 0x07 is plain CCITT."),
        &CommsSection::crcPolynomial);
    m_crcInitEdit = addHexRow(tr("Init Value :"),
                              tr("The register's starting value. SAE J1850 uses 0xFF, "
                                 "plain CCITT uses 0x00."),
                              &CommsSection::crcInitValue);
    m_crcXorEdit = addHexRow(tr("Final XOR :"),
                             tr("XORed onto the register after the last element. SAE "
                                "J1850 uses 0xFF, plain CCITT uses 0x00."),
                             &CommsSection::crcFinalXor);

    grid->addWidget(new QLabel(tr("Ref In :")), r, 0);
    m_crcRefInCheck = new QCheckBox;
    m_crcRefInCheck->setToolTip(
        tr("Reflect (bit-reverse) each input byte before it enters the register."));
    m_crcRefInCheck->setChecked(m_section.crcRefIn);
    grid->addWidget(m_crcRefInCheck, r, 1);
    ++r;
    grid->addWidget(new QLabel(tr("Ref Out :")), r, 0);
    m_crcRefOutCheck = new QCheckBox;
    m_crcRefOutCheck->setToolTip(tr("Reflect the register before the final XOR."));
    m_crcRefOutCheck->setChecked(m_section.crcRefOut);
    grid->addWidget(m_crcRefOutCheck, r, 1);
    ++r;

    layout->addWidget(paramGroup);

    auto *elemGroup = new QGroupBox(tr("Checksum Input"));
    auto *elemLayout = new QVBoxLayout(elemGroup);

    auto *countRow = new QHBoxLayout;
    countRow->addWidget(new QLabel(tr("Element Count :")));
    m_crcCountSpin = new QSpinBox;
    m_crcCountSpin->setRange(0, kCrcElementSlots);
    // Typed, not spun — the arrows are off by request, this is a number you
    // know, not one you nudge. The range starts at ZERO: an element-less
    // recipe is legal (nothing feeds the register, so the stamp degenerates to
    // the constant init/final-XOR transform — the validator flags it in case
    // it is a half-finished recipe, the mapper transports it). A fresh section
    // therefore opens with no element rows, and rows appear as a count is
    // typed.
    m_crcCountSpin->setButtonSymbols(QAbstractSpinBox::NoButtons);
    // Stripped of its buttons the spin box's width hint collapses to a slot
    // barely one digit wide. Sized off the font rather than in pixels so "15"
    // types comfortably at any DPI.
    m_crcCountSpin->setMinimumWidth(
        m_crcCountSpin->fontMetrics().horizontalAdvance(QStringLiteral("000")) + 24);
    m_crcCountSpin->setValue(qBound(0, int(m_section.crcElements.size()), kCrcElementSlots));
    m_crcCountSpin->setToolTip(
        tr("How many of the element rows below feed the checksum, in order. Each "
           "element contributes one byte."));
    connect(m_crcCountSpin, &QSpinBox::valueChanged, this,
            [this]() { applyCrcEnablement(protocolFieldsEnabled()); });
    countRow->addWidget(m_crcCountSpin);
    countRow->addStretch();
    elemLayout->addLayout(countRow);

    // Fifteen pre-built rows in one grid, a per-row stack for the value side.
    // Deliberately no add/remove churn: rows past Element Count are HIDDEN
    // (applyCrcEnablement), but their widgets keep their contents — a count
    // lowered by accident and raised again hands the user their elements back
    // instead of fresh defaults.
    auto *rowsHost = new QWidget;
    auto *rowsGrid = new QGridLayout(rowsHost);
    rowsGrid->setContentsMargins(0, 0, 0, 0);
    // No stretch on the widget columns: every field hugs its own text — the
    // type combo already did, the value side must match — and the leftover
    // width pools in the empty trailing column instead of ballooning the
    // right-hand field across the tab.
    rowsGrid->setColumnStretch(3, 1);
    for (int i = 0; i < kCrcElementSlots; ++i) {
        // Data / Byte 0 for a slot the section does not fill — the default the
        // contract names, and safer than a zeroed struct's accidents would be if
        // the encoding ever changed: a frame byte, not a surprise ID byte.
        CommsSection::CrcElement initial;
        if (i < m_section.crcElements.size())
            initial = m_section.crcElements.at(i);

        m_crcElemLabel[i] = new QLabel(tr("Element %1 :").arg(i + 1));
        rowsGrid->addWidget(m_crcElemLabel[i], i, 0);

        m_crcElemType[i] = new QComboBox;
        // Item order matches CrcElement::Type's encoding, and the stack's pages
        // are added in the same order — which is what lets an index drive all
        // three with no mapping table to fall out of step.
        m_crcElemType[i]->addItem(tr("ID"), int(CommsSection::CrcElement::Id));
        m_crcElemType[i]->addItem(tr("Data"), int(CommsSection::CrcElement::Data));
        m_crcElemType[i]->addItem(tr("Raw Value"), int(CommsSection::CrcElement::Raw));
        m_crcElemType[i]->setSizeAdjustPolicy(QComboBox::AdjustToContents);
        rowsGrid->addWidget(m_crcElemType[i], i, 1);

        m_crcElemStack[i] = new QStackedWidget;
        // The stack's default policy lets it grow vertically, and inside a
        // widgetResizable scroll area a nearly-empty grid has height to hand
        // out — the combos refuse it (their vertical hint is Fixed), the stack
        // would take it and tower over its own row. Its faces are all one
        // combo tall; hold it there.
        m_crcElemStack[i]->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        m_crcElemId[i] = new QComboBox;
        // Which byte of the CAN identifier feeds the CRC — spelt as the shift
        // the firmware performs, because that is how OEM checksum specs quote it.
        m_crcElemId[i]->addItem(tr("ID"), 0);
        m_crcElemId[i]->addItem(tr("ID >> 8"), 1);
        m_crcElemId[i]->addItem(tr("ID >> 16"), 2);
        m_crcElemId[i]->addItem(tr("ID >> 24"), 3);
        m_crcElemId[i]->setSizeAdjustPolicy(QComboBox::AdjustToContents);
        m_crcElemStack[i]->addWidget(m_crcElemId[i]);
        m_crcElemData[i] = new QComboBox;
        for (int b = 0; b < 8; ++b)
            m_crcElemData[i]->addItem(tr("Byte %1").arg(b), b);
        m_crcElemData[i]->setSizeAdjustPolicy(QComboBox::AdjustToContents);
        m_crcElemStack[i]->addWidget(m_crcElemData[i]);
        m_crcElemRaw[i] = new QLineEdit;
        m_crcElemRaw[i]->setValidator(new QRegularExpressionValidator(
            QRegularExpression(QStringLiteral("(0[xX])?[0-9A-Fa-f]{1,2}")), this));
        m_crcElemRaw[i]->setToolTip(tr("A literal byte fed to the CRC as-is (e.g. 0x5A)."));
        // A QLineEdit's default hint is paragraph-wide; the domain is four
        // characters. Font-sized like the count spin, so the value column's
        // width is the widest of its three faces, not the widest of the tab.
        m_crcElemRaw[i]->setFixedWidth(
            m_crcElemRaw[i]->fontMetrics().horizontalAdvance(QStringLiteral("0x00")) + 24);
        m_crcElemStack[i]->addWidget(m_crcElemRaw[i]);
        rowsGrid->addWidget(m_crcElemStack[i], i, 2);

        // Every side is primed, not only the one the type names: the element's
        // value lands on its own side, the other two show defaults, so trying a
        // different type mid-edit starts somewhere sensible instead of at
        // whatever the last visit left behind.
        m_crcElemId[i]->setCurrentIndex(
            initial.type == CommsSection::CrcElement::Id ? qBound(0, initial.value, 3) : 0);
        m_crcElemData[i]->setCurrentIndex(
            initial.type == CommsSection::CrcElement::Data ? qBound(0, initial.value, 7) : 0);
        reformatHexByteEdit(m_crcElemRaw[i],
                            initial.type == CommsSection::CrcElement::Raw ? initial.value : 0);
        connect(m_crcElemRaw[i], &QLineEdit::editingFinished, this, [this, i]() {
            // Fall back to what the section holds for this slot — kept fresh by
            // syncParametersFromUi — matching the other hex fields' recovery.
            const auto &elems = m_section.crcElements;
            const bool wasRaw = i < elems.size()
                                && elems.at(i).type == CommsSection::CrcElement::Raw;
            reformatHexByteEdit(m_crcElemRaw[i], wasRaw ? elems.at(i).value : 0);
        });

        const int typeIndex = qBound(0, initial.type, 2);
        m_crcElemType[i]->setCurrentIndex(typeIndex);
        m_crcElemStack[i]->setCurrentIndex(typeIndex);
        connect(m_crcElemType[i], &QComboBox::currentIndexChanged, this,
                [this, i](int index) { m_crcElemStack[i]->setCurrentIndex(index); });
    }
    // The vertical twin of the trailing stretch column: with most rows hidden
    // behind a low Element Count, the viewport's spare height pools in this
    // empty bottom row, so the visible rows pack to the top at combo height
    // instead of drifting apart to share the space.
    rowsGrid->setRowStretch(kCrcElementSlots, 1);

    // Fifteen rows of combos outrun a 768-tall screen once the recipe group
    // sits above them, and a grid does not scroll — the same arithmetic that
    // capped the frame map's size hint. Scrolling here keeps this tab from
    // setting the whole dialog's minimum height.
    auto *scroll = new QScrollArea;
    scroll->setWidget(rowsHost);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    elemLayout->addWidget(scroll, 1);
    layout->addWidget(elemGroup, 1);
}

void SectionEditorDialog::onSelectCrcChannel()
{
    // The CRC WRITES this channel — the stamped checksum is published through
    // it — so this is an OUTPUT pick and gets pickOutput's duplicate-writer
    // rule, exactly as a receive row's channel does. Synced first so liveView()
    // shows the picker this section as it is on screen, not as it was on the
    // last add-row.
    syncParametersFromUi();
    const QString current = ct::channelField(m_crcChannelEdit);
    const QString picked = SelectChannelDialog::pickOutput(m_config, current, this, liveView());
    if (picked.isEmpty())
        return;
    // Relabelled AFTER the pick (add_channel_dialog does the same): the picker's
    // New…/Edit… can change a channel's unit, and the field must show the unit
    // it now has. A rename needs nothing extra here — renameOpenChannelFields
    // re-points every field set through setChannelField, this one included.
    ct::setChannelField(m_crcChannelEdit, picked, m_config->catalog());
}

// The tier's verdict over the whole CRC tab, plus the element rows' own gate.
// Widget-level like the Parameters sweep, so a Read Only section still SHOWS
// its recipe with everything dead — the same "visible but not editable" the
// rest of the dialog gives.
void SectionEditorDialog::applyCrcEnablement(bool editable)
{
    // The Parameters tab fires onDeviceChanged during construction, before this
    // tab exists — the same window the m_singleRadio guard covers.
    if (!m_crcChannelEdit)
        return;
    const bool live = editable
                      && SectionDevice(m_deviceCombo->currentData().toInt())
                             == SectionDevice::TransmitCrc8;
    m_crcChannelEdit->setEnabled(live);
    m_crcSelectButton->setEnabled(live);
    m_crcByteCombo->setEnabled(live);
    m_crcPolyEdit->setEnabled(live);
    m_crcInitEdit->setEnabled(live);
    m_crcXorEdit->setEnabled(live);
    m_crcRefInCheck->setEnabled(live);
    m_crcRefOutCheck->setEnabled(live);
    m_crcCountSpin->setEnabled(live);
    const int count = m_crcCountSpin->value();
    for (int i = 0; i < kCrcElementSlots; ++i) {
        // Rows past Element Count are not shown at all — the tab presents
        // exactly the recipe's length, because only the first `count` rows are
        // read back. The hidden widgets keep their contents (see buildCrcTab).
        // Enablement stays the tier's: a locked section shows its rows dead.
        const bool rowUsed = i < count;
        m_crcElemLabel[i]->setVisible(rowUsed);
        m_crcElemType[i]->setVisible(rowUsed);
        m_crcElemStack[i]->setVisible(rowUsed);
        m_crcElemLabel[i]->setEnabled(live);
        m_crcElemType[i]->setEnabled(live);
        m_crcElemStack[i]->setEnabled(live);
    }
}

// Re-lays the whole map. The frame shape is read straight off the Parameters
// widgets rather than m_section, because the map has to follow a length being
// typed — the section itself is only synced when a row is added or on OK.
void SectionEditorDialog::refreshBitTable()
{
    if (!m_bitTable)
        return;
    const QList<CommsChannelRow> *rows = activeRowList();
    const auto device = SectionDevice(m_deviceCombo->currentData().toInt());
    // A Transmit CRC8 section composes a frame like any transmit message, so the
    // map is its Message Length, not the bus's frame kind.
    const bool transmit =
        device == SectionDevice::TransmitMessage || device == SectionDevice::TransmitCrc8;
    bool lengthOk = false;
    const int typed = m_lengthEdit->text().toInt(&lengthOk);
    const int length = qBound(0, lengthOk ? typed : m_section.messageLengthBytes, 64);

    // A transmit message IS the frame it composes, so its Message Length is the
    // whole story. A receive message takes whatever the sender puts on the
    // wire, so lay out the frame the bus can carry — 8 bytes classic, 64 CAN FD
    // — and grey the bytes past this message's own length, which is the point
    // beyond which nothing is extracted. qMax keeps a length that exceeds the
    // frame kind (its own validation Error) visible rather than cropping the
    // signals sitting in it.
    const int frameBytes = transmit ? length : (m_fdCheck->isChecked() ? 64 : 8);
    const int byteCount = qBound(1, qMax(frameBytes, length), 64);

    const auto alignment = SectionAlignment(m_alignmentCombo->currentData().toInt());
    const QList<CommsChannelRow> frameRows = rows ? *rows : QList<CommsChannelRow>();

    // Units for the names the map is about to draw in its tooltips and caption.
    // Set BEFORE setFrame(), which recolours — and so reads them — on its way
    // out. The rows themselves keep the bare name; this map is display only.
    QVariantMap labels;
    for (const CommsChannelRow &row : frameRows)
        labels.insert(row.channelName, m_config->catalog().labelFor(row.channelName));
    m_bitTable->setProperty(kChannelLabelsProperty, labels);

    // The CRC byte is reserved BEFORE any channel is placed, which is the whole
    // reason the map is told about it: the user routing channels around it needs
    // to see it while placing, not meet it as a validation finding after OK.
    // Read from the combo so the shading follows a location being changed live;
    // from m_section only in the construction window before buildCrcTab has run
    // (the section is only synced on add-row / OK, like the length above).
    const int crcByte = device != SectionDevice::TransmitCrc8
                            ? -1
                            : (m_crcByteCombo ? m_crcByteCombo->currentData().toInt()
                                              : m_section.crcByteLocation);

    // A compound identifier's SELECTOR is reserved for exactly the same reason
    // and was not being shown at all. Built through ct::reservedBits() so the
    // bits the map shades and the bits accept() refuses a channel on are the
    // same set by construction, computed once in frame_layout.cpp.
    //
    // Against a PROBE rather than m_section: the device kind and the CRC byte
    // are read from live widgets (the section is only synced on add-row / OK),
    // while the identifier's offset and mask are already live in m_section
    // because CompoundIdentifierDialog::apply() writes straight into it.
    //
    // Only the CURRENT identifier's selector, matching the map's one-variant-at-
    // a-time view above: another identifier's selector is stamped into another
    // frame, and shading it here would claim bits this variant leaves free.
    CommsSection probe = m_section;
    probe.device = device;
    if (crcByte >= 0)
        probe.crcByteLocation = crcByte;
    const int reservedIdent = m_section.compound ? currentIdentRole() : -1;
    m_bitTable->setReservedBits(reservedBits(probe, reservedIdent));
    m_bitTable->setFrame(frameRows, alignment, byteCount, length);

    // A compound section's identifiers are mutually exclusive frame variants —
    // they may reuse the same bits on purpose — so the map shows ONE identifier
    // at a time and says which, rather than piling every variant into one grid
    // and inventing overlaps. This is the same slice validation checks.
    QString title = transmit
                        ? tr("Frame Layout — %1-byte message, %2").arg(length).arg(
                              alignmentWord(alignment))
                        : tr("Frame Layout — %1-byte frame, this message reads %2, %3")
                              .arg(byteCount)
                              .arg(length == 1 ? tr("1 byte") : tr("%1 bytes").arg(length))
                              .arg(alignmentWord(alignment));
    if (m_section.compound) {
        const int role = currentIdentRole();
        title += role >= 0 ? tr("  ·  ID %1 only").arg(role + 1)
                           : tr("  ·  select an identifier");
    }
    // The legend for the shaded byte. In the TITLE, not only the cell tooltips:
    // a tooltip explains a cell after the user wonders about it, and the one job
    // of the marking is to be understood before a channel lands there.
    if (crcByte >= 0)
        title += tr("  ·  CRC8 stamped into Byte %1").arg(crcByte);
    // The same argument as the CRC legend: a tooltip explains a cell after the
    // user wonders about it, and the one job of marking the selector is to be
    // understood before a channel lands on it.
    if (reservedIdent >= 0 && !identifierBitPositions(m_section.identifiers.at(reservedIdent))
                                   .isEmpty())
        title += tr("  ·  ID %1 selector reserved").arg(reservedIdent + 1);
    m_bitGroup->setTitle(title);
    updateBitSelection();
}

// Moves the highlight to whatever the channel list has picked. The placeholder
// item in an empty list is not a row, so anything out of range clears it.
void SectionEditorDialog::updateBitSelection()
{
    if (!m_bitTable)
        return;
    const QList<CommsChannelRow> *rows = activeRowList();
    const int row = m_channelList->currentRow();
    const bool valid = rows && row >= 0 && row < rows->size();
    m_bitTable->setSelectedRow(valid ? row : -1);
    m_bitCaption->setText(valid ? m_bitTable->selectionSummary()
                                : tr("Pick a channel to see where it sits in the frame. "
                                     "Cell numbers are Start Bit values."));
}

void SectionEditorDialog::onMessageTypeToggled()
{
    const bool wantCompound = m_compoundRadio->isChecked();
    if (wantCompound == m_section.compound) {
        m_typeStack->setCurrentIndex(wantCompound ? 1 : 0);
        rebuildChannelList();
        updateButtons();
        updateTxModeControls();
        return;
    }
    bool hadConfiguredIdents = false;
    for (const CompoundIdentifier &ident : m_section.identifiers)
        if (ident.configured)
            hadConfiguredIdents = true;
    const bool hadChannels = !m_section.allRows().isEmpty();
    if (hadChannels || hadConfiguredIdents) {
        const auto answer = QMessageBox::question(
            this, tr("CAN Communications Setup"),
            tr("Changing the message type will remove all channels and identifiers.\n"
               "Are you sure you want to change the message type?"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes) {
            // Revert the radio without re-triggering.
            QSignalBlocker blockSingle(m_singleRadio);
            QSignalBlocker blockCompound(m_compoundRadio);
            m_singleRadio->setChecked(m_section.compound ? false : true);
            m_compoundRadio->setChecked(m_section.compound);
            updateTxModeControls();
            return;
        }
    }
    m_section.compound = wantCompound;
    m_section.rows.clear();
    m_section.identifiers.clear();
    if (wantCompound) {
        // Pre-create empty identifier slots for the numbered table.
        for (int i = 0; i < 16; ++i)
            m_section.identifiers.append(CompoundIdentifier{});
    }
    m_typeStack->setCurrentIndex(wantCompound ? 1 : 0);
    rebuildIdentifierTable();
    rebuildChannelList();
    updateButtons();
    updateTxModeControls();
}

int SectionEditorDialog::currentIdentRole() const
{
    QTreeWidgetItem *item = m_identifierTree->currentItem();
    return item ? item->data(0, Qt::UserRole).toInt() : -2;
}

QList<CommsChannelRow> *SectionEditorDialog::activeRowList()
{
    if (!m_section.compound)
        return &m_section.rows;
    // Compound sections carry channels only inside identifiers — no shared
    // always-present set. Every channel belongs to a multiplexor value.
    const int role = currentIdentRole();
    if (role >= 0 && role < m_section.identifiers.size())
        return &m_section.identifiers[role].rows;
    return nullptr;
}

// Where the row with this key sits once the list is sorted, or -1.
//
// Identity by the SORT KEY rather than by the whole row: the key is what
// decides the position, and two rows the comparator calls equal are
// interchangeable for the one thing this is for — keeping the row that was just
// added or edited selected after it has moved.
static int indexOfSortedRow(const QList<CommsChannelRow> &rows, const CommsChannelRow &row)
{
    for (int i = 0; i < rows.size(); ++i)
        if (!commsRowPrecedes(rows[i], row) && !commsRowPrecedes(row, rows[i]))
            return i;
    return -1;
}

// Every row list this section owns put into frame order, not just the one on
// screen. A compound section's other identifiers are not visible from here, and
// leaving them alone would save a message whose variants were sorted or not
// according to which ones the user happened to click on.
//
// CALLED WHERE THE ROWS CHANGE — the constructor and the three row handlers —
// and deliberately NOT from rebuildChannelList(), which would have been the
// tidier-looking place. rebuildChannelList() also runs from the channelRenamed
// handler, and that fires WHILE the row editor is open: a rename made through
// its channel picker walks m_section, and a name is the third sort key. Sorting
// there would move rows out from under the index onChangeRow is holding across
// the modal, and the edit would land on whichever row had slid into that place
// — overwriting a channel the user never opened. So the order is settled
// after each edit, never during one.
void SectionEditorDialog::sortSectionRows()
{
    sortCommsRows(m_section.rows);
    for (CompoundIdentifier &ident : m_section.identifiers)
        sortCommsRows(ident.rows);
}

void SectionEditorDialog::rebuildChannelList()
{
    // Each row is tinted with the colour its bits carry in the frame map below,
    // so "which block is this channel" needs no legend. The placeholder is left
    // uncoloured — ColorItemDelegate passes anything without a background
    // straight through to the stock painter.
    const auto addRows = [this](const QList<CommsChannelRow> &rows) {
        const bool receive = m_section.isReceive();
        for (int i = 0; i < rows.size(); ++i) {
            auto *item = new QListWidgetItem(rowSummary(rows[i], m_config->catalog(), receive),
                                             m_channelList);
            const QColor fill = BitLayoutTable::signalColour(i, palette());
            item->setBackground(fill);
            item->setForeground(BitLayoutTable::signalTextColour(fill));
        }
    };

    m_channelList->clear();
    if (m_section.compound) {
        const int role = currentIdentRole();
        if (role >= 0 && role < m_section.identifiers.size()) {
            m_channelListLabel->setText(tr("Channels (ID %1)").arg(role + 1));
            addRows(m_section.identifiers[role].rows);
        } else {
            m_channelListLabel->setText(tr("Channels"));
            updateButtons();
            refreshBitTable();
            return;
        }
    } else {
        m_channelListLabel->setText(tr("Channels"));
        addRows(m_section.rows);
        if (m_section.rows.isEmpty()) {
            auto *placeholder = new QListWidgetItem(tr("(No Channels selected)"), m_channelList);
            placeholder->setFlags(Qt::NoItemFlags);
        }
    }
    updateButtons();
    refreshBitTable();
}

void SectionEditorDialog::rebuildIdentifierTable()
{
    m_identifierTree->clear();
    for (int i = 0; i < m_section.identifiers.size(); ++i) {
        const CompoundIdentifier &ident = m_section.identifiers[i];
        auto *item = new QTreeWidgetItem(m_identifierTree);
        item->setText(0, QString::number(i + 1));
        item->setData(0, Qt::UserRole, i);
        const bool used = ident.configured || !ident.rows.isEmpty();
        if (used) {
            item->setText(1, QString::number(ident.byteOffset));
            item->setText(2, QStringLiteral("0x") + QString::number(ident.id, 16).toUpper());
            item->setText(3, QStringLiteral("0x") + QString::number(ident.idMask, 16).toUpper());
        }
    }
    if (m_identifierTree->topLevelItemCount() > 0)
        m_identifierTree->setCurrentItem(m_identifierTree->topLevelItem(0));
}

void SectionEditorDialog::onEditIdentifier()
{
    const int role = currentIdentRole();
    if (role < 0)
        return;
    CompoundIdentifierDialog dialog(m_section.identifiers[role],
                                    m_section.messageLengthBytes, this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    dialog.apply(&m_section.identifiers[role]);
    rebuildIdentifierTable();
    m_identifierTree->setCurrentItem(m_identifierTree->topLevelItem(role));
    // The selector moved, so the reserved bits did. Without this the map keeps
    // shading where the identifier USED to be, which is worse than not shading
    // at all: the user routes around a phantom and into the real one.
    refreshBitTable();
}

void SectionEditorDialog::onClearIdentifier()
{
    const int role = currentIdentRole();
    if (role < 0)
        return;
    m_section.identifiers[role] = CompoundIdentifier{};
    rebuildIdentifierTable();
    // Keep the cleared slot selected so a follow-up Change… edits it, not slot 1.
    m_identifierTree->setCurrentItem(m_identifierTree->topLevelItem(role));
    rebuildChannelList();
    refreshBitTable(); // a cleared slot reserves nothing
}

// Communications Setup's view of the buses with this dialog's in-progress
// section written over the top, so a row added or deleted here counts from the
// moment it happens. Everything is captured by value — the row editor holds the
// patch, and must not reach back into a dialog that may be mid-teardown.
ConfigPatch SectionEditorDialog::liveView() const
{
    return [parent = m_livePatch, section = m_section, bus = m_busIndex,
            index = m_sectionIndex](Configuration &c) {
        if (parent)
            parent(c);
        QList<CommsSection> &sections = c.bus[bus].sections;
        if (index >= 0 && index < sections.size())
            sections[index] = section;
        else
            sections.append(section); // a section still being created
    };
}

void SectionEditorDialog::onAddRow()
{
    QList<CommsChannelRow> *rows = activeRowList();
    if (!rows)
        return;
    syncParametersFromUi();
    // The RAW TIER travels, not isEditLocked(): the dialog locks on `!= None`
    // exactly as before, but it also names the tier in its title, and passing
    // the bool made every locked row claim "Read Only". Both call sites took the
    // default until 2.3.0, which was safe only for as long as this editor
    // refused to open at all for a marked section.
    // The bits this variant already gives away. currentIdentRole() for a
    // compound section, so a row being added under identifier 2 is refused on
    // identifier 2's selector and not on some other variant's.
    AddChannelDialog dialog(m_config, CommsChannelRow{}, m_section.alignment,
                            m_section.messageLengthBytes, m_section.isTransmit(), liveView(),
                            this, m_section.protection,
                            reservedBits(m_section,
                                         m_section.compound ? currentIdentRole() : -1));
    if (dialog.exec() != QDialog::Accepted)
        return;
    // Re-fetched, never the pointer from before exec(): a rename committed
    // through this dialog's picker (Configuration::channelRenamed) walks
    // m_section mutably, which detaches its implicitly-shared lists from the
    // live-view copies — a compound section's pre-exec pointer would then aim
    // at the abandoned buffer and the append would land in the wrong list.
    rows = activeRowList();
    if (!rows)
        return;
    const CommsChannelRow added = dialog.row();
    rows->append(added);
    sortSectionRows();    // so the new row is not necessarily last
    rebuildChannelList();
    if (const QList<CommsChannelRow> *sorted = activeRowList())
        m_channelList->setCurrentRow(indexOfSortedRow(*sorted, added));
}

void SectionEditorDialog::onChangeRow()
{
    QList<CommsChannelRow> *rows = activeRowList();
    const int row = m_channelList->currentRow();
    if (!rows || row < 0 || row >= rows->size())
        return;
    syncParametersFromUi();
    // See onAddRow: the section's tier travels, so Change… on a marked message
    // shows the row, writes nothing, and says which marking stopped it.
    AddChannelDialog dialog(m_config, rows->at(row), m_section.alignment,
                            m_section.messageLengthBytes, m_section.isTransmit(), liveView(),
                            this, m_section.protection,
                            reservedBits(m_section,
                                         m_section.compound ? currentIdentRole() : -1));
    if (dialog.exec() != QDialog::Accepted)
        return;
    // Re-fetched — see onAddRow for the detach this dodges.
    rows = activeRowList();
    if (!rows || row >= rows->size())
        return;
    const CommsChannelRow edited = dialog.row();
    (*rows)[row] = edited;
    sortSectionRows();
    rebuildChannelList();
    // NOT `row` again: changing a Start Bit or a Bit Length moves the row, and
    // re-selecting the old index would leave the highlight on whichever channel
    // slid into that place — with the frame map following the highlight, the
    // shading would then be pointing at the wrong signal.
    if (const QList<CommsChannelRow> *sorted = activeRowList())
        m_channelList->setCurrentRow(indexOfSortedRow(*sorted, edited));
}

void SectionEditorDialog::onRemoveRow()
{
    QList<CommsChannelRow> *rows = activeRowList();
    const int row = m_channelList->currentRow();
    if (!rows || row < 0 || row >= rows->size())
        return;
    rows->removeAt(row);
    rebuildChannelList();
}

void SectionEditorDialog::updateButtons()
{
    QList<CommsChannelRow> *rows = activeRowList();
    const bool haveList = rows != nullptr;
    const int row = m_channelList->currentRow();
    const bool haveRow = haveList && row >= 0 && row < rows->size();
    // The tier is ANDed in here as well as in updateProtectionUi, because this
    // function runs on every selection change and would otherwise re-enable the
    // three buttons a locked message has just had taken away.
    const bool editable = protocolFieldsEnabled();
    m_addButton->setEnabled(haveList && editable);
    m_changeButton->setEnabled(haveRow && editable);
    m_removeButton->setEnabled(haveRow && editable);
    if (m_section.compound) {
        const bool haveIdent = currentIdentRole() >= 0; // not the always-present row
        m_identChangeButton->setEnabled(haveIdent && editable);
        m_identClearButton->setEnabled(haveIdent && editable);
    }
}

void SectionEditorDialog::accept()
{
    const bool relay =
        SectionDevice(m_deviceCombo->currentData().toInt()) == SectionDevice::MessageRelay;
    QString addressText = m_addressEdit->text();
    if (addressText.startsWith(QLatin1String("0x"), Qt::CaseInsensitive))
        addressText = addressText.mid(2);
    bool addressOk = false;
    addressText.toUInt(&addressOk, 16);
    if (!addressOk) {
        QMessageBox::warning(this, windowTitle(),
                             tr("Enter a valid hexadecimal base address (e.g. 0x640)."));
        return;
    }
    if (relay) {
        QString maskText = m_bitmaskEdit->text();
        if (maskText.startsWith(QLatin1String("0x"), Qt::CaseInsensitive))
            maskText = maskText.mid(2);
        bool maskOk = false;
        maskText.toUInt(&maskOk, 16);
        if (!maskOk) {
            QMessageBox::warning(this, windowTitle(),
                                 tr("Enter a valid hexadecimal message bitmask (e.g. 0x7F0)."));
            return;
        }
        int fwdCount = 0;
        for (int i = 0; i < 3; ++i)
            if (m_relayBusCheck[i]->isChecked() && i != m_busIndex)
                ++fwdCount;
        if (fwdCount == 0) {
            QMessageBox::warning(this, windowTitle(),
                                 tr("Select at least one bus to forward matching frames to."));
            return;
        }
    }
    bool lengthOk = false;
    const int length = m_lengthEdit->text().toInt(&lengthOk);
    static const QList<int> kFdLengths = {0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64};
    const bool fd = m_fdCheck->isChecked();
    if (!relay && (!lengthOk || (fd ? !kFdLengths.contains(length) : (length < 0 || length > 8)))) {
        QMessageBox::warning(this, windowTitle(),
                             fd ? tr("Message length must be 0–8, 12, 16, 20, 24, 32, 48 or "
                                     "64 bytes for a CAN FD frame.")
                                : tr("Message length must be 0 to 8 bytes (enable CAN FD for "
                                     "longer frames)."));
        return;
    }
    // Transmit CRC8: only what this dialog's own widgets can answer. The model's
    // validation owns the cross-checks — the CRC byte colliding with a channel,
    // and the rest — and restating them here is how the two drift apart. After
    // the length check on purpose, so the byte-location rule below compares
    // against a length already known to be a real one.
    if (SectionDevice(m_deviceCombo->currentData().toInt()) == SectionDevice::TransmitCrc8) {
        if (ct::channelField(m_crcChannelEdit).isEmpty()) {
            QMessageBox::warning(this, windowTitle(),
                                 tr("Select the channel the CRC publishes to."));
            m_tabs->setCurrentIndex(2); // the field lives on the CRC8 tab
            m_crcSelectButton->setFocus();
            return;
        }
        const int crcByte = m_crcByteCombo->currentData().toInt();
        if (crcByte >= length) {
            QMessageBox::warning(
                this, windowTitle(),
                tr("The CRC8 byte location is Byte %1, but the message is only %2 byte(s) "
                   "long — there is no such byte to stamp the checksum into.")
                    .arg(crcByte)
                    .arg(length));
            m_tabs->setCurrentIndex(2);
            m_crcByteCombo->setFocus();
            return;
        }
        // Nearly unreachable — the validator filters and editingFinished
        // reformats — but Return inside a cleared field skips editingFinished
        // (an Intermediate value never emits it) and lands straight here, and
        // that path must not hand syncParametersFromUi garbage to guess at.
        for (int i = 0; i < m_crcCountSpin->value(); ++i) {
            if (m_crcElemType[i]->currentData().toInt() != CommsSection::CrcElement::Raw)
                continue;
            bool rawOk = false;
            parseHexByte(m_crcElemRaw[i]->text(), &rawOk);
            if (!rawOk) {
                QMessageBox::warning(this, windowTitle(),
                                     tr("Element %1: enter the raw value as one hexadecimal "
                                        "byte (e.g. 0x5A).")
                                         .arg(i + 1));
                m_tabs->setCurrentIndex(2);
                m_crcElemRaw[i]->setFocus();
                m_crcElemRaw[i]->selectAll();
                return;
            }
        }
    }
    // The length rule is enforced where the password is now CHOSEN — in the
    // prompt, which re-asks rather than accepting a short one — so there is no
    // late refusal to make here. What remains is the privilege check below,
    // about who may REPLACE a password rather than what a password may be.
    if (m_pendingKey != kNoAccessKey) {
        // REPLACING a password is exactly as privileged as unticking the box.
        // Both hand the message to somebody else, and only one of them used to
        // be guarded: Read Only never conceals, so its editor opens with no
        // challenge at all, and typing anything here made the section yours —
        // the tier never moved, so rule 2's ladder never ran. Reopen, untick
        // Read Only with YOUR password, and the message is legible and editable
        // with its owner never asked for anything.
        //
        // proveSectionPassword(), the SAME path rule 2 uses, so there is one
        // implementation of "prove this section's current password" and one place
        // for it to be got wrong. It answers true for free when the section
        // carries no key — nothing to prove, and setting a first password must
        // stay free or every section a Get produces would be unfixable — and true
        // again if the ladder already asked this session, so changing the tier
        // and the password in one visit costs one prompt rather than two.
        //
        // m_openedTier, not m_tier: the stored key belongs to the tier the
        // section ARRIVED on, so that is the tier being given up. Configuration::
        // applyBusSections refuses the same swap on the model side and does not
        // need this to have run — this is what makes the refusal a prompt the
        // owner can answer instead of a wall.
        if (m_section.messageKey != kNoAccessKey && !proveSectionPassword(m_openedTier))
            return;
    }
    // RULE 1: no marked tier leaves this dialog without a password of its own.
    // All three tiers, Protect Communication included — its device round trip is
    // an addition to the section password, not a replacement for it.
    //
    // There is no longer a SEAM here to explain away, and the comment that used
    // to sit in this spot is worth remembering as the shape of the bug: it said
    // Configuration::isSectionRevealed() treats a marked section with no
    // messageKey as OPEN, because every section a Get produces is keyless (the
    // wire carries reserved[4] and no key) and demanding a password of them would
    // turn them into bricks. That reasoning is what shipped a Get whose Hidden
    // messages were padlocked in the list and fully legible on a double-click.
    // The predicate fails closed now, and this refusal is what keeps the two
    // rules pointing the same way: you cannot CREATE a keyless marking, and one
    // that already exists conceals rather than opens.
    //
    // Refused in accept() and NOWHERE else on the way in. Cancel must still work
    // from any state — someone who opens a keyless marked section to look at it
    // has to be able to leave — so nothing here may live in a toggled() handler
    // or a field validator.
    if (!sectionPasswordSatisfied()) {
        const QString who =
            m_nameEdit->text().isEmpty() ? tr("This message") : m_nameEdit->text();
        QString why;
        if (m_tier == m_openedTier) {
            // Opened marked, keyless, and being kept that way — a migrated file or
            // anything a Get produced. It no longer offers "or untick the box":
            // unticking a keyless marking is refused in authoriseTierChange, and
            // Configuration::maySectionLower refuses it too, so naming it here
            // would send the user at a door this dialog has already locked.
            why = tr("\"%1\" is marked %2 and has no Message Password.\n\nFill one in. A marking "
                     "with nothing behind it can be neither opened nor unticked — nobody holds a "
                     "password for it, so nothing can authorise either — and giving it one is "
                     "what makes it a lock rather than a label.\n\nIf you do not want the "
                     "marking at all, Remove the message: that is allowed at every tier.")
                      .arg(who, protectionTierWord(m_tier));
        } else if (m_openedTier == CommsProtection::None) {
            why = tr("\"%1\" is now marked %2, and every marked message carries a password of "
                     "its own.\n\nUntick the box and tick it again to choose one.")
                      .arg(who, protectionTierWord(m_tier));
        } else {
            why = tr("\"%1\" has moved from %2 to %3, and the password it had guarded %2.\n\n"
                     "Untick %3 and tick it again to choose the password that will guard it — "
                     "it does not carry over — or put the marking back where it was.")
                      .arg(who, protectionTierWord(m_openedTier), protectionTierWord(m_tier));
        }
        // A BACKSTOP NOW, not a routine refusal. Taking a marking asks for its
        // password at the tick, so the ordinary path cannot reach here; what can
        // is a section that arrived marked and keyless. It says what to do rather
        // than pointing at a field that no longer exists.
        QMessageBox::warning(this, windowTitle(), why);
        return;
    }
    syncParametersFromUi();
    const quint32 maxId = m_section.extended ? 0x1FFFFFFFu : 0x7FFu;
    if (m_section.baseAddress > maxId) {
        QMessageBox::warning(this, windowTitle(),
                             tr("Base address 0x%1 exceeds the %2 identifier range.")
                                 .arg(QString::number(m_section.baseAddress, 16).toUpper(),
                                      m_section.extended ? tr("extended (29-bit)")
                                                         : tr("standard (11-bit)")));
        return;
    }
    // BIT COLLISIONS, last, on the section as it will actually be written.
    //
    // Last because it needs syncParametersFromUi() to have run — the checks
    // above read widgets, but this one reads the finished layout, and a rule
    // about where channels sit has to judge the channels the user is really
    // saving.
    //
    // Which collisions refuse is frame_layout.h's decision, not this dialog's:
    // channel-vs-channel on a RECEIVE message is a warning and closes normally,
    // because two receive rows reading the same bits is legitimate. Everything
    // else — any overlap on a transmit message, any channel under a compound
    // identifier's selector, any channel in the CRC8's byte — is written by the
    // device over the top of the channel, so the frame would not carry what
    // this configuration says it carries.
    QStringList blocking;
    for (const LayoutClash &clash : findLayoutClashes(m_section))
        if (clash.blocking)
            blocking << clash.message();
    if (!blocking.isEmpty()) {
        // The Channels tab, because that is where the offending rows are and
        // where the frame map shows the collision in colour. The first refusal
        // in this dialog to switch there.
        m_tabs->setCurrentIndex(1);
        QMessageBox::warning(
            this, windowTitle(),
            tr("This message cannot be saved as it stands:\n\n%1\n\n"
               "Each of these is written into the frame AFTER the channels, so the channel "
               "does not merely share those bits — it is replaced by them. Move the "
               "channel, or change what is written over it. The Frame Layout map shades "
               "every bit that is already spoken for.")
                .arg(blocking.join(QStringLiteral("\n"))));
        return;
    }
    QDialog::accept();
}

} // namespace ct
