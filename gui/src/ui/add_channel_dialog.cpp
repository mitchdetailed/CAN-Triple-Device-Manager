#include "add_channel_dialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QtMath>

#include "../model/device_mapper.h"
#include "../model/frame_layout.h"
#include "channel_field.h"
#include "select_channel_dialog.h"
#include "trimmed_spin_box.h"

namespace ct {

AddChannelDialog::AddChannelDialog(Configuration *config, const CommsChannelRow &initial,
                                   SectionAlignment alignment, int messageLengthBytes,
                                   bool transmit, const ConfigPatch &livePatch,
                                   QWidget *parent, CommsProtection sectionProtection,
                                   const QHash<int, QString> &reservedBits)
    : QDialog(parent)
    , m_config(config)
    , m_alignment(alignment)
    , m_messageLength(messageLengthBytes)
    , m_reservedBits(reservedBits)
    , m_transmit(transmit)
    , m_protection(sectionProtection)
    // Derived from the tier by the same rule CommsSection::isEditLocked() uses,
    // so the two cannot drift. It used to be ANDed with !commsRevealed() here,
    // on the reasoning that the password lifted the lock — which is no longer
    // true in either direction. Read Only conceals nothing, so there is no
    // password to have given; and revealing buys viewing and the right to untick
    // the tier, not the right to edit through it.
    , m_readOnly(sectionProtection != CommsProtection::None)
    , m_livePatch(livePatch)
    , m_row(initial)
{
    m_config->buildLiveView(m_live, m_livePatch);

    // The tier is NAMED, not summarised as "Read Only". Read Only is one of the
    // three tiers, so the old single title claimed the weakest and most
    // permissive one for Hidden and Protect Communication rows too — which also
    // misdirects about how to unlock: Read Only and Hidden take this section's
    // own password, Protect Communication takes Protected Comms proved
    // against a connected device. Spelt with the same words as the section
    // editor's three checkboxes so a user can match title to box.
    if (m_readOnly) {
        const QString tier = m_protection == CommsProtection::Protected
                                 ? tr("Protect Communication")
                             : m_protection == CommsProtection::Hidden ? tr("Hidden")
                                                                       : tr("Read Only");
        setWindowTitle(tr("Comms Channel — %1").arg(tier));
    } else {
        setWindowTitle(m_row.channelName.isEmpty() ? tr("Add Comms Channel")
                                                   : tr("Change Comms Channel"));
    }

    auto *layout = new QVBoxLayout(this);
    if (m_readOnly) {
        // "marked %1" rather than "protected": with three tiers, "protected"
        // reads as the Protect Communication tier specifically, and the sentence
        // that follows is only true of the tier actually in force — the Edit
        // Protected Comms password does nothing for Read Only or Hidden, and a
        // section password does nothing for Protect Communication.
        const QString how = m_protection == CommsProtection::Protected
                                ? tr("the Protected Comms password, proved against a "
                                     "connected device")
                                : tr("this message's own password");
        const QString tier = m_protection == CommsProtection::Protected
                                 ? tr("Protect Communication")
                             : m_protection == CommsProtection::Hidden ? tr("Hidden")
                                                                       : tr("Read Only");
        auto *notice = new QLabel(
            tr("🔒 This row belongs to a message marked %1. Its bit layout and scaling are "
               "read-only until that marking is unticked, which needs %2.")
                .arg(tier, how));
        notice->setWordWrap(true);
        notice->setMaximumWidth(460);
        layout->addWidget(notice);
    }
    auto *grid = new QGridLayout;
    grid->setColumnStretch(1, 1);
    int r = 0;

    // Channel + Select…
    grid->addWidget(new QLabel(tr("Channel :")), r, 0);
    // Shows "Coolant Temp °C" but refers to "Coolant Temp": the name goes in
    // through setChannelField() and comes back out through channelField(), never
    // through text() — see channel_field.h.
    m_channelEdit = new QLineEdit;
    m_channelEdit->setReadOnly(true);
    ct::setChannelField(m_channelEdit, m_row.channelName, m_config->catalog());
    grid->addWidget(m_channelEdit, r, 1, 1, 3);
    auto *selectButton = new QPushButton(tr("Select…"));
    connect(selectButton, &QPushButton::clicked, this, &AddChannelDialog::onSelectChannel);
    grid->addWidget(selectButton, r, 4);
    ++r;

    // Default value (physical units, applied on receive timeout)
    grid->addWidget(new QLabel(tr("Default Value :")), r, 0);
    m_defaultSpin = new TrimmedDoubleSpinBox;
    m_defaultSpin->setRange(-1e9, 1e9);
    m_defaultSpin->setDecimals(8);
    m_defaultSpin->setValue(m_row.defaultValue);
    m_defaultSpin->setEnabled(false); // until a channel is selected
    grid->addWidget(m_defaultSpin, r, 1);
    m_defaultUnitLabel = new QLabel;
    grid->addWidget(m_defaultUnitLabel, r, 2);
    ++r;

    // Start Bit / Bit Length.
    //
    // A NEW row starts with both EMPTY, and OK stays off until real numbers
    // are typed. They used to open prefilled (0 and 16), and a prefilled bit
    // position is worse than none: it looks chosen, it is always a plausible
    // layout, and a row accepted with it decodes the wrong bits while
    // validation finds nothing to object to. The emptiness is a sentinel one
    // below each field's real range, worn as blank text — the same
    // "starts blank, a choice is required" rule the Constants dialog applies
    // to its Data Type. Editing an existing row keeps its actual values;
    // emptying a number someone already chose would be manufacturing the
    // very ambiguity this exists to prevent.
    const bool adding = m_row.channelName.isEmpty();
    grid->addWidget(new QLabel(tr("Start Bit :")), r, 0);
    m_startBitSpin = new QSpinBox;
    if (adding) {
        m_startBitSpin->setRange(-1, 511); // -1 = not yet entered, shown blank
        m_startBitSpin->setSpecialValueText(QStringLiteral(" "));
        m_startBitSpin->setValue(-1);
    } else {
        m_startBitSpin->setRange(0, 511); // up to a 64-byte CAN FD frame
        m_startBitSpin->setValue(m_row.startBit);
    }
    m_startBitSpin->setToolTip(
        tr("Bit index of the signal's LEAST significant bit (bit S = byte S/8, bit S%8;\n"
           "bits count 0-7 right-to-left within a byte, bytes left-to-right from 0).\n"
           "Word Swap fields continue into higher bytes; Normal (big-endian) fields\n"
           "continue into lower bytes."));
    grid->addWidget(m_startBitSpin, r, 1);
    grid->addWidget(new QLabel(tr("Bit Length :")), r, 2);
    m_bitLengthSpin = new QSpinBox;
    if (adding) {
        m_bitLengthSpin->setRange(0, 64); // 0 = not yet entered, shown blank
        m_bitLengthSpin->setSpecialValueText(QStringLiteral(" "));
        m_bitLengthSpin->setValue(0);
    } else {
        m_bitLengthSpin->setRange(1, 64);
        m_bitLengthSpin->setValue(m_row.bitLength);
    }
    grid->addWidget(m_bitLengthSpin, r, 3);
    ++r;

    // DBC Type
    grid->addWidget(new QLabel(tr("DBC Type :")), r, 0);
    m_dbcTypeCombo = new QComboBox;
    m_dbcTypeCombo->addItem(tr("Unsigned"), int(DbcType::Unsigned));
    m_dbcTypeCombo->addItem(tr("Signed"), int(DbcType::Signed));
    m_dbcTypeCombo->addItem(tr("IEEE754"), int(DbcType::IEEE754));
    m_dbcTypeCombo->setCurrentIndex(qBound(0, m_row.dbcType, 2));
    grid->addWidget(m_dbcTypeCombo, r, 1, 1, 3);
    ++r;

    // Bit Resolution / Offset  (physical = raw × Bit Resolution + Offset)
    //
    // "Bit Resolution" rather than "DBC Factor" because that is what the number
    // MEANS to whoever types it: how much physical quantity one raw count is
    // worth. 0.1 is a tenth per count in both directions — a received count of
    // 7 is 0.7, and transmitting 0.7 puts 7 on the wire. The same number, read
    // the same way, whichever way the message goes.
    //
    // The stored key stays dbcFactor. Renaming a label costs nothing; renaming
    // a JSON key would invalidate every saved configuration to no purpose.
    grid->addWidget(new QLabel(tr("Bit Resolution :")), r, 0);
    m_factorSpin = new TrimmedDoubleSpinBox;
    m_factorSpin->setRange(-1e9, 1e9);
    m_factorSpin->setDecimals(8);
    m_factorSpin->setValue(m_row.dbcFactor);
    m_factorSpin->setToolTip(
        tr("How much one raw count is worth, in the channel's units. 0.1 means "
           "each count is a tenth: a received count of 7 reads 0.7, and "
           "transmitting 0.7 puts 7 on the wire."));
    grid->addWidget(m_factorSpin, r, 1);
    grid->addWidget(new QLabel(tr("Offset :")), r, 2);
    m_dbcOffsetSpin = new TrimmedDoubleSpinBox;
    m_dbcOffsetSpin->setRange(-1e9, 1e9);
    m_dbcOffsetSpin->setDecimals(8);
    m_dbcOffsetSpin->setValue(m_row.dbcOffset);
    m_dbcOffsetSpin->setToolTip(
        m_transmit
            ? tr("Added to the raw count on its way out, after the resolution has "
                 "been divided out. Offset 64 puts 64 more on the wire: a value of "
                 "1 at resolution 1 sends 65.\n\n"
                 "This is a bias in raw counts, not the reverse of a receive row's "
                 "offset. Sending to another CAN Triple that receives with the same "
                 "offset? Negate one of the two, or the offset is applied twice.")
            : tr("Added after scaling to reach the channel's units. -40 on a "
                 "temperature whose counts start at -40 makes a raw 0 read as "
                 "-40."));
    grid->addWidget(m_dbcOffsetSpin, r, 3);
    ++r;

    // Transmit only. A receive row has no such choice to make: the field it
    // reads is bitLength bits wide by construction, so nothing can overflow it,
    // and the clamp that does apply on that side is the channel's declared
    // range, which is not optional.
    if (m_transmit) {
        m_clampCheck = new QCheckBox(tr("Clamp to Signal Limit"));
        m_clampCheck->setChecked(m_row.clampToRange);
        m_clampCheck->setToolTip(
            tr("Ticked, a value too big for the field is sent as the biggest the "
               "field can hold — 256 into 8 bits sends 255. Unticked, only the "
               "low bits are sent, so the count rolls over and 256 sends 0. "
               "Roll-over is what a free-running counter or a wrapping angle "
               "wants; clamping is what a measurement wants.\n\n"
               "Unticked also stops the CHANNEL's range from clamping first, "
               "which it would otherwise do before the field width ever came "
               "into it."));
        grid->addWidget(m_clampCheck, r, 0, 1, 4);
        ++r;
    }

    layout->addLayout(grid);

    m_previewLabel = new QLabel;
    m_previewLabel->setWordWrap(true);
    layout->addWidget(m_previewLabel);

    // One note line under the preview. It says one of two very different
    // things, so it is coloured per message rather than always in the warning
    // colour: a transmit row referencing a channel nothing writes yet is plain
    // information (dimmed), while a receive row writing a channel something
    // else also writes is the one real conflict (warning colour). Neither
    // blocks OK.
    m_warnLabel = new QLabel;
    m_warnLabel->setWordWrap(true);
    layout->addWidget(m_warnLabel);

    m_errorLabel = new QLabel;
    m_errorLabel->setWordWrap(true);
    m_errorLabel->setStyleSheet(QStringLiteral("color: #b00020;"));
    layout->addWidget(m_errorLabel);

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(m_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(m_buttons);

    // Plain numeric inputs — no up/down arrows.
    for (QAbstractSpinBox *s :
         {static_cast<QAbstractSpinBox *>(m_defaultSpin),
          static_cast<QAbstractSpinBox *>(m_startBitSpin),
          static_cast<QAbstractSpinBox *>(m_bitLengthSpin),
          static_cast<QAbstractSpinBox *>(m_factorSpin),
          static_cast<QAbstractSpinBox *>(m_dbcOffsetSpin)})
        s->setButtonSymbols(QAbstractSpinBox::NoButtons);

    connect(m_startBitSpin, &QSpinBox::valueChanged, this, &AddChannelDialog::revalidate);
    connect(m_bitLengthSpin, &QSpinBox::valueChanged, this, &AddChannelDialog::revalidate);
    connect(m_factorSpin, &QDoubleSpinBox::valueChanged, this, &AddChannelDialog::revalidate);
    connect(m_dbcOffsetSpin, &QDoubleSpinBox::valueChanged, this, &AddChannelDialog::revalidate);
    connect(m_dbcTypeCombo, &QComboBox::currentIndexChanged, this,
            &AddChannelDialog::onDbcTypeChanged);
    if (m_clampCheck)
        connect(m_clampCheck, &QCheckBox::toggled, this, &AddChannelDialog::revalidate);

    onDbcTypeChanged(); // apply the IEEE754 length lock if needed
    revalidate();

    // After both of the above, which enable fields of their own. The channel's
    // name stays legible (m_channelEdit is read-only, never disabled) for the
    // same reason it does everywhere else: what is protected is the protocol,
    // not the fact that this message carries Engine RPM. revalidate() and
    // onDbcTypeChanged() keep the Default Value, Bit Length and OK button off
    // on their own.
    if (m_readOnly) {
        QList<QWidget *> locked{selectButton,   m_startBitSpin, m_bitLengthSpin,
                                m_dbcTypeCombo, m_factorSpin,   m_dbcOffsetSpin};
        if (m_clampCheck)
            locked.append(m_clampCheck);
        for (QWidget *w : locked)
            w->setEnabled(false);
    }
}

CommsChannelRow AddChannelDialog::row() const
{
    CommsChannelRow r = m_row;
    r.channelName = ct::channelField(m_channelEdit);
    r.defaultValue = m_defaultSpin->value();
    r.startBit = m_startBitSpin->value();
    r.dbcType = m_dbcTypeCombo->currentData().toInt();
    r.bitLength = (r.dbcType == int(DbcType::IEEE754)) ? 32 : m_bitLengthSpin->value();
    r.dbcFactor = m_factorSpin->value();
    r.dbcOffset = m_dbcOffsetSpin->value();
    // A receive row has no box, and keeps the model's default of clamping.
    if (m_clampCheck)
        r.clampToRange = m_clampCheck->isChecked();
    return r;
}

void AddChannelDialog::onSelectChannel()
{
    // A transmit row reads a channel; a receive row writes one. Two things
    // follow from that, and both live in ChannelRole rather than here: only the
    // write side has a conflict to guard (the duplicate-writer confirmation),
    // and only the write side offers New…. What a message transmits has to be
    // produced on the device first — a receive row, a calculation, a constant —
    // so a channel invented at the transmit row's picker would have nothing
    // writing it and the row would send its default value for ever.
    // The picker is given, and gives back, the BARE name.
    const QString current = ct::channelField(m_channelEdit);
    const QString picked =
        m_transmit ? SelectChannelDialog::pickInput(m_config, current, this, m_livePatch)
                   : SelectChannelDialog::pickOutput(m_config, current, this, m_livePatch);
    if (picked.isEmpty())
        return;
    // Edit… (and New… on the receive side) can add or rename a channel and
    // rewrite every reference to it, so the view this dialog judges against has
    // to follow —
    // and the field is relabelled after it, so a channel whose unit was just
    // changed in the picker shows the unit it now has.
    m_config->buildLiveView(m_live, m_livePatch);
    ct::setChannelField(m_channelEdit, picked, m_config->catalog());
    revalidate();
}

void AddChannelDialog::onDbcTypeChanged()
{
    const bool ieee = m_dbcTypeCombo->currentIndex() == int(DbcType::IEEE754);
    if (ieee) {
        // The 32 written here is the format's, not the user's. Remember when
        // the field was still blank, so trying IEEE754 and backing out does
        // not leave a length the user never entered — the exact laundering of
        // an unchosen value into a chosen-looking one the blank start exists
        // to prevent.
        m_lengthWasUnchosen = m_bitLengthSpin->minimum() == 0 && m_bitLengthSpin->value() == 0;
        m_bitLengthSpin->setValue(32); // IEEE-754 single precision is always 32 bits
    } else if (m_lengthWasUnchosen) {
        m_lengthWasUnchosen = false;
        m_bitLengthSpin->setValue(0); // back to blank
    }
    m_bitLengthSpin->setEnabled(!ieee && !m_readOnly);
    revalidate();
}

void AddChannelDialog::revalidate()
{
    const CommsChannelRow current = row();
    const Channel channel = m_config->catalog().findByName(current.channelName);

    // Default Value only makes sense once a channel sets its precision, and the
    // precision it sets is the channel's OWN Decimal Places — the same number
    // Edit Custom Channel shows, so a 1-dp channel cannot be given a default
    // with three. (This used to re-derive the count from baseResolution by
    // multiplying it up to a whole number, which agreed with Decimal Places
    // only as long as the two stayed the exact reciprocal the channel editor
    // makes them, and silently fell back to 0 dp for anything imported with a
    // resolution that isn't a power of ten.)
    m_defaultSpin->setEnabled(channel.isValid() && !m_readOnly);
    if (channel.isValid()) {
        m_defaultUnitLabel->setText(channel.unit);
        const int decimals = qBound(0, channel.decimalPlaces, 8);
        // Shown at full width — a 2-dp channel reads "0.00", not "0". Here the
        // zeros are the point: they state how precisely the channel holds the
        // value, which is exactly what a trimmed "0" hides. (Factor and Offset
        // beside it keep trimming; their 8 places are a ceiling, not a fact
        // about the value.)
        m_defaultSpin->setTrimTrailingZeros(false);
        // Rounds the current value to the new precision, which is the point:
        // the field can no longer hold digits the channel does not define.
        m_defaultSpin->setDecimals(decimals);
        // One click steps one count of the channel, floored at the smallest
        // digit on show: a resolution finer than the displayed precision would
        // step invisibly, and an unstated one must not jump by a whole unit on
        // a channel defined to two decimals.
        const double tick = qPow(10.0, -decimals);
        const double res = channel.baseResolution > 0 ? channel.baseResolution : tick;
        m_defaultSpin->setSingleStep(qMax(res, tick));
        m_defaultSpin->setToolTip(
            decimals > 0
                ? tr("Physical value in %1, to the channel's %2 decimal place(s).")
                      .arg(channel.unit.isEmpty() ? tr("channel units") : channel.unit)
                      .arg(decimals)
                : tr("Physical value in %1. This channel is defined with no decimal "
                     "places, so its default is a whole number.")
                      .arg(channel.unit.isEmpty() ? tr("channel units") : channel.unit));
    } else {
        m_defaultUnitLabel->clear();
        m_defaultSpin->setToolTip(QString());
        // No channel, so no precision to state. Back to trimmed, and the 8
        // places the field was built with are LEFT ALONE: this is a row naming
        // a channel outside the catalogue, and narrowing the field would round
        // a stored default that nothing here is qualified to round.
        m_defaultSpin->setTrimTrailingZeros(true);
    }

    // Note line. A TRANSMIT row only reads its channel — any channel may be
    // sent, in any number of messages, and sending it changes nothing — so the
    // only thing worth saying is when nothing writes it yet, and that is
    // information, not a warning. A RECEIVE row writes its channel, so naming
    // one something else already writes is the one genuine conflict.
    QString note;
    bool isWarning = false;
    if (!current.channelName.isEmpty()) {
        const bool generated =
            m_live.generatedChannelNames().contains(current.channelName, Qt::CaseInsensitive);
        const bool unchanged =
            current.channelName.compare(m_row.channelName, Qt::CaseInsensitive) == 0;
        // The note only ever reaches a human, so it names the channel the way
        // the rest of the UI does — with its unit. The two tests above are
        // identity, so they stay on the bare name.
        const QString label = m_config->catalog().labelFor(current.channelName);
        if (m_transmit && !generated) {
            note = tr("\"%1\" has no generator yet, so this row transmits its default value "
                      "until a receive row or a calculation writes it.")
                       .arg(label);
        } else if (!m_transmit && generated && !unchanged) {
            note = tr("⚠ \"%1\" is already written elsewhere — both rows write the same "
                      "channel slot and overwrite each other.")
                       .arg(label);
            isWarning = true;
        }
    }
    const bool darkUi = palette().color(QPalette::Window).lightness() < 128;
    m_warnLabel->setStyleSheet(
        isWarning ? (darkUi ? QStringLiteral("color: #ffa178;")
                            : QStringLiteral("color: #c03000;"))
                  : (darkUi ? QStringLiteral("color: #9aa0a6;")
                            : QStringLiteral("color: #606468;")));
    m_warnLabel->setText(note);

    QString error;
    if (current.channelName.isEmpty()) {
        error = tr("Select a channel.");
    } else if (m_startBitSpin->minimum() == -1 && m_startBitSpin->value() < 0) {
        // The blank sentinels of a new row (see the constructor). Checked
        // BEFORE the extraction math, which would otherwise judge the
        // sentinel values as a layout and report something confusing about a
        // field nobody has filled in yet.
        error = tr("Enter a start bit.");
    } else if (m_bitLengthSpin->minimum() == 0 && m_bitLengthSpin->value() == 0) {
        error = tr("Enter a bit length.");
    } else {
        ExtractionFields fields;
        QString reason;
        if (!computeExtraction(current, m_alignment, m_messageLength, &fields, &reason))
            error = reason;
    }

    // BITS THAT ARE ALREADY SPOKEN FOR. Refused here, while the row is being
    // placed, rather than waited for and reported at the section editor's OK:
    // the device writes a compound identifier's selector and a CRC8 stamp into
    // the frame AFTER the channels, so a row put here would not share those bits
    // with anything, it would be overwritten by them on every single frame.
    //
    // Checked only once the row is otherwise sound — a row that does not fit the
    // frame at all has a better thing to be told about it, and stacking two
    // complaints would bury the one that matters. rowBitPositions() is the
    // device's own walk, which is what makes this agree with the map's shading
    // and with the refusal at OK.
    if (error.isEmpty() && !m_reservedBits.isEmpty()) {
        QStringList claims;
        for (int pos : rowBitPositions(current, m_alignment)) {
            const QString why = m_reservedBits.value(pos);
            // One line per distinct claimant, however many bits it holds: a
            // selector across eight bits is one fact, not eight.
            if (!why.isEmpty() && !claims.contains(why))
                claims << why;
        }
        if (!claims.isEmpty())
            error = tr("This lands on bits that are already spoken for: %1. "
                       "Move the start bit, or shorten the field.")
                        .arg(claims.join(QStringLiteral("; ")));
    }

    if (error.isEmpty()) {
        const QString unit = channel.isValid() ? channel.unit : QString();
        QString preview = tr("Physical = raw × %1 + %2 %3")
                              .arg(current.dbcFactor)
                              .arg(current.dbcOffset)
                              .arg(unit);
        // What the field can carry, in the channel's own units, so the choice
        // above is judged against a number rather than an idea. IEEE754 is left
        // out: its 32 bits hold any float the channel can, so there is no edge
        // to name — the only clamp on such a row is the channel's range.
        if (m_clampCheck && current.dbcType != int(DbcType::IEEE754)) {
            const int len = qBound(1, current.bitLength, 64);
            const bool isSigned = current.dbcType == int(DbcType::Signed);
            // std::pow, not a shift: len can be 64, where 1<<64 is undefined,
            // and the ends are only ever displayed.
            const double span = qPow(2.0, len);
            const double rawLo = isSigned ? -span / 2.0 : 0.0;
            const double rawHi = (isSigned ? span / 2.0 : span) - 1.0;
            // Transmit packs raw = physical / resolution + offset, so the
            // physical values the field can carry are (raw - offset) * res.
            // NOT raw * res + offset — that is the RECEIVE mapping, and the two
            // stopped being inverses when the offset was made to add on the way
            // out. This line only ever renders on a transmit row (m_clampCheck
            // exists only there), so it states the transmit end of the deal.
            const double lo = (rawLo - current.dbcOffset) * current.dbcFactor;
            const double hi = (rawHi - current.dbcOffset) * current.dbcFactor;
            // Ordered, because a negative resolution swaps which end of the
            // field is the larger physical value.
            const double fieldLo = qMin(lo, hi);
            const double fieldHi = qMax(lo, hi);
            // Carries its own leading space, or is empty. A unitless channel
            // otherwise leaves "63 , but" and "63  —", which read as typos.
            const QString unitSuffix = unit.isEmpty() ? QString() : QLatin1Char(' ') + unit;

            if (!current.clampToRange) {
                // A WRAPPING row skips the channel's range outright — the firmware
                // does so deliberately, because a channel ranged 0..255 would
                // otherwise present 255 and an 8-bit field would have nothing
                // left to roll over. So the field really is the whole story
                // here, and this line says only what the field holds.
                preview += QLatin1String("\n")
                           + tr("%1 bits hold %2 to %3%4 — anything outside rolls over "
                                "into that range.")
                                 .arg(len)
                                 .arg(fieldLo)
                                 .arg(fieldHi)
                                 .arg(unitSuffix);
            } else {
                // TWO CLAMPS APPLY, AND THE PREVIEW USED TO NAME ONLY ONE.
                //
                // engine_core.c's inverseSignalScaling runs them in this order
                // on a non-wrapping row: the CHANNEL's declared Min/Max first,
                // then the raw value is scaled and clamped into what the FIELD
                // can represent. So the number that actually limits what goes
                // out is whichever of the two is narrower — and this line
                // stated the field's capacity alone, which is the right answer
                // only while the channel is the wider of the two.
                //
                // That is worth more than a footnote: a channel ranged -32..31
                // feeding a 6-bit UNSIGNED field can never send 32..63, and a
                // preview promising "0 to 63" is describing a field the value
                // cannot reach. The bits are honest; the sentence was not.
                double effLo = fieldLo;
                double effHi = fieldHi;
                bool channelBinds = false;
                if (channel.isValid()) {
                    if (channel.minValue > effLo) {
                        effLo = channel.minValue;
                        channelBinds = true;
                    }
                    if (channel.maxValue < effHi) {
                        effHi = channel.maxValue;
                        channelBinds = true;
                    }
                }
                if (effLo > effHi) {
                    // The two do not overlap at all: every value the channel
                    // can hold is outside what the field can carry, so the
                    // clamp pins the field to one end and the row sends a
                    // constant. Named as the fault it is rather than printed as
                    // an inside-out range.
                    preview +=
                        QLatin1String("\n")
                        + tr("%1 bits hold %2 to %3%4, but \"%5\" is ranged %6 to %7 — "
                             "entirely outside what the field can carry, so every value would "
                             "be sent as the nearer end.")
                              .arg(len)
                              .arg(fieldLo)
                              .arg(fieldHi)
                              .arg(unitSuffix)
                              .arg(channel.name)
                              .arg(channel.minValue)
                              .arg(channel.maxValue);
                } else if (channelBinds) {
                    preview += QLatin1String("\n")
                               + tr("%1 bits hold %2 to %3%4, but \"%5\" is ranged %6 to %7, so "
                                    "%8 to %9 is what is sent — anything outside is sent as "
                                    "the nearer end.")
                                     .arg(len)
                                     .arg(fieldLo)
                                     .arg(fieldHi)
                                     .arg(unitSuffix)
                                     .arg(channel.name)
                                     .arg(channel.minValue)
                                     .arg(channel.maxValue)
                                     .arg(effLo)
                                     .arg(effHi);
                } else {
                    preview += QLatin1String("\n")
                               + tr("%1 bits hold %2 to %3%4 — anything outside is sent as "
                                    "the nearer end.")
                                     .arg(len)
                                     .arg(fieldLo)
                                     .arg(fieldHi)
                                     .arg(unitSuffix);
                }
            }
        }
        m_previewLabel->setText(preview);
    } else {
        m_previewLabel->clear();
    }

    m_errorLabel->setText(error);
    // Read-only keeps OK off whatever the row validates as: the row is already
    // in the message and is not being asked to justify itself, it simply may
    // not be written back.
    m_buttons->button(QDialogButtonBox::Ok)->setEnabled(error.isEmpty() && !m_readOnly);
}

} // namespace ct
