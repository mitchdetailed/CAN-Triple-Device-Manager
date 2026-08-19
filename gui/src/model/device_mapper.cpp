#include "device_mapper.h"

#include <QSet>
#include <QtMath>

#include <algorithm>
#include <cstring>

#include "configuration.h"
#include "dbc_import.h" // storageTypeForRange — one sizing rule for both directions

// The DEVICE's script verifier, compiled in — not a host-side reimplementation
// of the format. A retained image is sent back to a unit unchanged, so the only
// question worth asking about it is the one the unit itself asks, and asking it
// with the unit's own code is what makes "this will be accepted" a fact rather
// than a belief. Same argument as fw_image.c in the firmware-update path.
extern "C" {
#include "script_vm.h"
}

namespace ct {

// The verifier's verdict in words. The enum ordering is stable ABI precisely so
// a host can do this (script_vm.h, "Verifier results"). Not file-static: the
// Script Editor reports the same verdicts about the images it compiles and the
// disassembler about the ones it refuses to list, and three tables would be
// three chances to describe one refusal three ways.
QString scriptVerifyText(quint8 code)
{
    switch (code) {
    case SCRIPT_OK: return QStringLiteral("valid");
    case SCRIPT_ERR_MAGIC: return QStringLiteral("not a script image");
    case SCRIPT_ERR_VERSION: return QStringLiteral("bytecode newer than this build runs");
    case SCRIPT_ERR_VERSION_OLD: return QStringLiteral("bytecode older than supported");
    case SCRIPT_ERR_SIZE: return QStringLiteral("the declared length does not fit the bytes present");
    case SCRIPT_ERR_CRC: return QStringLiteral("checksum mismatch");
    case SCRIPT_ERR_STATE_COUNT: return QStringLiteral("too many state registers");
    case SCRIPT_ERR_OPCODE: return QStringLiteral("unknown opcode");
    case SCRIPT_ERR_REGISTER: return QStringLiteral("register out of range");
    case SCRIPT_ERR_SIGNAL: return QStringLiteral("channel index out of range");
    case SCRIPT_ERR_STATE_INDEX: return QStringLiteral("state index out of range");
    case SCRIPT_ERR_JUMP: return QStringLiteral("jump out of range");
    case SCRIPT_ERR_ENTRY: return QStringLiteral("entry point out of range");
    case SCRIPT_ERR_NO_HALT: return QStringLiteral("the code does not end in HALT");
    case SCRIPT_ERR_NONFINITE: return QStringLiteral("a constant is NaN or infinite");
    case SCRIPT_ERR_RESERVED: return QStringLiteral("a reserved header field is set");
    default: return QStringLiteral("verifier code %1").arg(code);
    }
}

bool validateScriptImage(const QByteArray &image, QString *reason)
{
    // avail is the image's OWN length, so a header claiming more code than it
    // was given is rejected by the size check rather than walking the verifier
    // off the end of a QByteArray.
    const quint8 verdict = script_verify(image.constData(), quint32(image.size()), MAX_SIGNALS);
    if (verdict != SCRIPT_OK) {
        if (reason)
            *reason = scriptVerifyText(verdict);
        return false;
    }
    return true;
}

RetainedScript scriptImageFromChunks(const QVector<ScriptChunk> &chunks)
{
    RetainedScript out;
    if (chunks.isEmpty())
        return out; // firmware too old to answer the read, or nothing stored

    const QByteArray raw(reinterpret_cast<const char *>(chunks.constData()),
                         chunks.size() * int(sizeof(ScriptChunk)));

    // ALL ZERO is "this device has no script", not "this device has a broken
    // one", and the distinction is the difference between silence and a false
    // alarm on every Get. engine_table_read zero-fills every slot past the
    // active prefix and a Get reads the table's full capacity, so a unit with a
    // perfectly good configuration and no script answers this read with 32 KB of
    // zeros. Checked before the verifier, which would call the same bytes
    // ERR_MAGIC and could not tell the two apart.
    if (raw.count('\0') == raw.size())
        return out;
    out.present = true;

    QString reason;
    if (!validateScriptImage(raw, &reason)) {
        out.error = reason;
        return out;
    }

    // Trim to the image the compiler emitted: header + code_bytes, with the
    // chunk padding dropped. This is what makes the round trip BYTE-IDENTICAL
    // rather than merely equivalent — the padding is a function of the chunk
    // size, so re-chunking a trimmed image reproduces the original chunks
    // exactly, while re-chunking a padded one would grow the image by a chunk
    // every trip through a device. The verifier above has already proven the
    // length fits.
    ScriptHeader header{};
    std::memcpy(&header, raw.constData(), sizeof(header));
    out.image = raw.left(int(sizeof(ScriptHeader) + header.code_bytes));
    return out;
}

bool computeExtraction(const CommsChannelRow &row, SectionAlignment alignment,
                       int messageLengthBytes, ExtractionFields *out, QString *reason)
{
    auto fail = [&](const QString &why) {
        if (reason)
            *reason = why;
        return false;
    };

    const bool ieee = row.dbcType == int(DbcType::IEEE754);
    const int bitLen = ieee ? 32 : row.bitLength; // IEEE-754 is always 32-bit
    if (bitLen < 1 || bitLen > 64)
        return fail(QStringLiteral("bit length must be 1–64 (32 for IEEE754)"));
    if (row.startBit < 0)
        return fail(QStringLiteral("start bit must be ≥ 0"));

    // Alignment picks the byte order the whole message uses:
    // Word Swap = Intel (little-endian), Normal = Motorola (big-endian).
    const quint8 byteOrder = (alignment == SectionAlignment::WordSwap) ? 0 : 1;

    // DBC bit numbering: bit S sits at byte S/8, bit S%8 (bits 0..7 right-to-
    // left within a byte, bytes left-to-right). The START BIT IS THE SIGNAL'S
    // LSB for both byte orders; the walk ascends the bit within the byte and
    // steps to the NEXT byte (Intel / Word Swap) or the PREVIOUS byte
    // (Motorola / Normal) on each byte boundary — the same traversal the
    // firmware's engine_extract_raw / dbc_decode use. Reject any step that
    // leaves the frame.
    {
        int byteIndex = row.startBit / 8;
        int bitIndex = row.startBit % 8;
        for (int i = 0; i < bitLen; ++i) {
            if (byteIndex < 0 || byteIndex >= messageLengthBytes)
                return fail(QStringLiteral("field (start bit %1, %2 bits, %3) does not fit the "
                                           "%4-byte message")
                                .arg(row.startBit).arg(bitLen)
                                .arg(byteOrder == 0 ? QStringLiteral("Intel")
                                                    : QStringLiteral("Motorola"))
                                .arg(messageLengthBytes));
            if (++bitIndex == 8) {
                bitIndex = 0;
                byteIndex += byteOrder == 0 ? 1 : -1;
            }
        }
    }

    ExtractionFields f;
    f.startBit = quint16(row.startBit);
    f.bitLength = quint8(bitLen);
    f.byteOrder = byteOrder;
    if (ieee)
        f.valueType = SIGNAL_TYPE_FLOAT;
    else if (row.dbcType == int(DbcType::Signed))
        f.valueType = bitLen <= 8 ? SIGNAL_TYPE_INT8
                                  : bitLen <= 16 ? SIGNAL_TYPE_INT16 : SIGNAL_TYPE_INT32;
    else
        f.valueType = bitLen <= 8 ? SIGNAL_TYPE_UINT8
                                  : bitLen <= 16 ? SIGNAL_TYPE_UINT16 : SIGNAL_TYPE_UINT32;

    if (out)
        *out = f;
    return true;
}

QList<int> rowBitPositions(const CommsChannelRow &row, SectionAlignment alignment)
{
    QList<int> bits;
    const bool ieee = row.dbcType == int(DbcType::IEEE754);
    const int bitLen = ieee ? 32 : row.bitLength; // IEEE-754 is always 32-bit
    if (bitLen < 1 || bitLen > 64 || row.startBit < 0)
        return bits;
    bits.reserve(bitLen);
    const bool intel = alignment == SectionAlignment::WordSwap;
    int byteIndex = row.startBit / 8;
    int bitIndex = row.startBit % 8;
    for (int i = 0; i < bitLen; ++i) {
        const int pos = byteIndex * 8 + bitIndex;
        if (byteIndex >= 0 && pos < MAX_FRAME_BITS)
            bits.append(pos);
        if (++bitIndex == 8) {
            bitIndex = 0;
            byteIndex += intel ? 1 : -1;
        }
    }
    return bits;
}

// Stamp a section's protection tier into a message's or a relay's flags. ONE
// helper for all three emit sites — transmit, receive and relay — because three
// near-identical copies is precisely how one of them gets missed, and the one
// that was missed was the relay: a marked relay section concealed in the GUI and
// reached the device carrying nothing, so a Get read it back as ordinary and the
// next Send wrote it back that way.
//
// It reads section.protection, the FACT, and never Configuration's
// isChannelConcealed()/isChannelEditLocked(), which answer the different,
// per-viewer question "should this session be shown the detail" and are false
// for anyone holding the password. Sending from an unlocked session would then
// strip the protection off the device. This distinction has to be restated at
// every call site's expense because getting it wrong looks like a simplification.
//
// The bits are transported for ROUND-TRIP FIDELITY ONLY. As of 2.3.0 the device
// enforces nothing about them; what they buy is that a Get followed by a Send
// cannot launder a Hidden message into an ordinary one.
static void applyProtection(quint8 &flags, CommsProtection protection)
{
    flags = quint8((flags & ~MSGPROT_MASK) | commsProtectionToWire(protection));
}

// Storage data-type name (Channel::dataType / ConstantRow::dataType) -> wire
// value_type. Blank/unknown falls back to 0 (the "unset" virtual-slot marker).
static quint8 valueTypeForDataType(const QString &dataType)
{
    if (dataType == QLatin1String("boolean") || dataType == QLatin1String("u8"))
        return SIGNAL_TYPE_UINT8;
    if (dataType == QLatin1String("u16"))
        return SIGNAL_TYPE_UINT16;
    if (dataType == QLatin1String("u32"))
        return SIGNAL_TYPE_UINT32;
    if (dataType == QLatin1String("s8"))
        return SIGNAL_TYPE_INT8;
    if (dataType == QLatin1String("s16"))
        return SIGNAL_TYPE_INT16;
    if (dataType == QLatin1String("s32"))
        return SIGNAL_TYPE_INT32;
    if (dataType == QLatin1String("float"))
        return SIGNAL_TYPE_FLOAT;
    return 0;
}

// How wide a virtual value slot of this type is described as. Every generated
// channel (constant, table output, device channel) stamps the same thing, so it
// lives here rather than being spelled out at each site.
static quint8 bitLengthForValueType(quint8 vt)
{
    if (vt == SIGNAL_TYPE_UINT8 || vt == SIGNAL_TYPE_INT8)
        return 8;
    if (vt == SIGNAL_TYPE_UINT16 || vt == SIGNAL_TYPE_INT16)
        return 16;
    return 32; // u32/s32/float
}

// Reverse of valueTypeForDataType (boolean is indistinguishable from u8 on the
// wire, so a UINT8 slot reads back as "u8").
static QString dataTypeForValueType(quint8 vt)
{
    switch (vt) {
    case SIGNAL_TYPE_UINT8:  return QStringLiteral("u8");
    case SIGNAL_TYPE_UINT16: return QStringLiteral("u16");
    case SIGNAL_TYPE_UINT32: return QStringLiteral("u32");
    case SIGNAL_TYPE_INT8:   return QStringLiteral("s8");
    case SIGNAL_TYPE_INT16:  return QStringLiteral("s16");
    case SIGNAL_TYPE_INT32:  return QStringLiteral("s32");
    case SIGNAL_TYPE_FLOAT:  return QStringLiteral("float");
    default:                 return {};
    }
}

// Clip a name to the device label budget without cutting a multi-byte UTF-8
// codepoint in half. The name dialogs already refuse anything longer; this is
// the backstop for configurations saved before that limit existed.
static QByteArray clipToLabel(const QString &name)
{
    QByteArray utf8 = name.toUtf8();
    if (utf8.size() > MAX_CHANNEL_NAME_BYTES) {
        // Walk back ONLY when the cut lands inside a codepoint, i.e. when the
        // first dropped byte is a continuation byte (10xxxxxx). If it is a lead
        // byte or ASCII the cut is already on a boundary and chopping would eat
        // a whole character that fits — the reason this is not just "chop while
        // the last byte is a continuation byte". That shorter form also leaves
        // the split codepoint's LEAD byte behind, which is not a shorter name
        // but an invalid string. Same test as config_transfer.cpp's config-name
        // clip; keep the two in step.
        const bool splitsCodepoint =
            (quint8(utf8[MAX_CHANNEL_NAME_BYTES]) & 0xC0) == 0x80;
        utf8.truncate(MAX_CHANNEL_NAME_BYTES);
        if (splitsCodepoint) {
            while (!utf8.isEmpty() && (quint8(utf8.back()) & 0xC0) == 0x80)
                utf8.chop(1);
            if (!utf8.isEmpty())
                utf8.chop(1); // the lead byte of the split codepoint
        }
    }
    return utf8;
}

// Writes the whole SIGNAL_LABEL_LEN-byte field (32 since the label widened from
// v15's 16): the clipped name, then NUL to the end. Both halves are sized off
// the struct rather than off a literal, so the field growing is a one-line
// change in wire_structs.h and not a hunt through here — and the memset is what
// keeps the tail of a reused stack record out of the frame.
static void fillLabel(CanSignalConfig &sig, const QString &name)
{
    const QByteArray utf8 = clipToLabel(name);
    std::memset(sig.label, 0, sizeof(sig.label));
    std::memcpy(sig.label, utf8.constData(), size_t(utf8.size()));
}

// The document's handle for a message: its bus and its name, lower-cased. Used
// by both directions of the condition mapping, so the two cannot disagree about
// what counts as the same message. Deliberately the same shape as
// sectionNameIdentity below, which the Get reconciliation pass already uses.
QString messageRefKey(int bus, const QString &name)
{
    return QStringLiteral("%1/%2").arg(bus).arg(name.trimmed().toLower());
}

MappingResult mapToDevice(const Configuration &config)
{
    MappingResult r;
    const ChannelCatalog &catalog = config.catalog();

    // Device labels are MAX_CHANNEL_NAME_BYTES UTF-8 bytes — 31, since the
    // record's label went back to 32 bytes with the capacity expansion (it was
    // 15 while v15 had it at 16). Distinct channels whose names truncate
    // identically would collide on read-back. Only reachable for configurations
    // saved before the name dialogs enforced the limit — the editors refuse
    // longer names now, and a file written under the 15-byte budget is
    // comfortably inside the 31-byte one, so widening the label can only ever
    // silence one of these warnings, never raise a new one.
    //
    // The budget is a BYTE count, not a character count: the dialogs cap typing
    // at 31 characters, but one non-ASCII character is 2-4 UTF-8 bytes, so a
    // legal-looking 31-character name can still overrun the label. That is the
    // case this check exists for.
    // Triggered transmit, deferred. A message names its User Condition by the
    // condition's OUTPUT CHANNEL, but the wire wants the condition's INDEX, and
    // the index does not exist yet: messages are mapped here at the top and the
    // condition table is built several hundred lines below. Rather than guess
    // the numbering twice — the condition loop skips inactive and malformed rows,
    // so predicting it would mean duplicating those rules and getting them to
    // stay duplicated — each transmit message parks what it wants here and the
    // bindings are resolved once the real indices are known.
    struct PendingTrigger {
        int msgIdx;
        QString conditionChannel;
        QString where;
    };
    QList<PendingTrigger> pendingTriggers;

    // (bus, lower-cased section name) -> the message index it landed at, for the
    // "was received" / "was transmitted" comparisons a User Condition can carry.
    //
    // Bus AND name, because a name alone is not unique across buses and a
    // cross-bus match would let a section on CAN 2 answer for one on CAN 1 —
    // the hazard sectionNameIdentity already documents. Lower-cased, because
    // every other name comparison in the document is case-insensitive.
    //
    // A duplicate name within one bus resolves to the FIRST section, which is
    // the same rule the device applies to two receive rows sharing a CAN ID.
    // Validation reports the ambiguity rather than this silently picking.
    QHash<QString, int> messageIndexByBusName;

    QHash<QByteArray, QString> truncatedLabels;
    auto checkLabelCollision = [&](const QString &channelName) {
        const QByteArray label = clipToLabel(channelName);
        if (label.size() != channelName.toUtf8().size())
            r.warnings.append(QStringLiteral(
                "channel '%1' is longer than the %2-byte device label and stores as '%3' — "
                "rename it to keep the name through a read-back")
                .arg(channelName).arg(MAX_CHANNEL_NAME_BYTES).arg(QString::fromUtf8(label)));
        const QByteArray key = label.toLower();
        const QString existing = truncatedLabels.value(key);
        if (existing.isEmpty())
            truncatedLabels.insert(key, channelName);
        else if (existing.compare(channelName, Qt::CaseInsensitive) != 0)
            r.warnings.append(QStringLiteral(
                "channels '%1' and '%2' share the same %3-byte device label — rename one "
                "or read-back will merge them")
                .arg(existing, channelName).arg(MAX_CHANNEL_NAME_BYTES));
    };

    // channel name (lower) -> allocated signal index
    auto signalFor = [&](const QString &channelName, bool *created = nullptr) -> int {
        const QString key = channelName.toLower();
        const auto it = r.channelToSignal.constFind(key);
        if (it != r.channelToSignal.constEnd()) {
            if (created)
                *created = false;
            return *it;
        }
        checkLabelCollision(channelName);
        const int idx = r.tables.signalConfigs.size();
        CanSignalConfig sig{};
        // virtual until a comms row claims it; byte order irrelevant while so
        sigSetHeader(sig, SIG_MSG_NONE, 0, 1);
        sig.factor = 1.0f;
        sig.offset = 0.0f;
        const Channel ch = catalog.findByName(channelName);
        sig.min_val = float(ch.isValid() ? ch.minValue : -1e9);
        sig.max_val = float(ch.isValid() ? ch.maxValue : 1e9);
        fillLabel(sig, channelName);
        r.tables.signalConfigs.append(sig);
        r.channelToSignal.insert(key, idx);
        r.signalToChannel.insert(idx, channelName);
        if (created)
            *created = true;
        return idx;
    };


    for (int busIdx = 0; busIdx < 3; ++busIdx) {
        const BusConfig &busCfg = config.bus[busIdx];
        // Messages on an Off bus upload deactivated (no MSGFLAG_ACTIVE): the
        // stopped bus wouldn't run them anyway, and a later Get must not
        // mistake them for evidence that the bus is on.
        const bool busOn = busCfg.enabled;
        if (!busOn && std::any_of(busCfg.sections.cbegin(), busCfg.sections.cend(),
                                  [](const CommsSection &s) {
                                      return s.device != SectionDevice::Off;
                                  }))
            r.warnings.append(QStringLiteral(
                "CAN %1 mode is Off — its messages upload deactivated").arg(busIdx + 1));
        for (const CommsSection &section : busCfg.sections) {
            const QString where = QStringLiteral("CAN %1 · %2").arg(busIdx + 1).arg(section.name);
            if (section.device == SectionDevice::Off)
                continue;
            if (section.isTransmit()) {
                if (r.tables.messages.size() >= MAX_MESSAGES) {
                    r.errors.append(QStringLiteral("%1: device message table is full (%2)")
                                        .arg(where).arg(MAX_MESSAGES));
                    continue;
                }
                CanMessageConfig msg{};
                msg.can_id = section.baseAddress;
                msg.flags = (busOn ? MSGFLAG_ACTIVE : 0) | MSGFLAG_TRANSMIT;
                if (section.extended)
                    msg.flags |= MSGFLAG_EXTENDED;
                if (section.fd)
                    msg.flags |= MSGFLAG_FD;
                // v10: compound transmit cadence — Sequential sends one variant
                // per period (round-robin); Batch (default) sends all each period.
                if (section.compound && section.compoundTxMode == CompoundTxMode::Sequential)
                    msg.flags |= MSGFLAG_TX_SEQUENTIAL;
                applyProtection(msg.flags, section.protection);
                // The one surviving byte of the retired per-message key, written
                // zero. Value-initialised above, so this only says so on purpose
                // — the firmware scrubs it on the way in, and a field that is
                // only usually zero stops being reserved.
                msg.reserved = 0;
                // Cyclic until the resolve pass below says otherwise. Naming the
                // sentinel rather than leaning on the zero-init matters: 0 is a
                // perfectly good condition index, and a message that ended up
                // with tx_trigger_cond = 0 and no flag would read back from a
                // device as "triggered on condition 1" the moment anything set
                // the flag by accident.
                msg.tx_trigger_cond = TX_TRIGGER_COND_NONE;
                msg.tx_trigger_flags = 0;
                if (!section.cyclic) {
                    if (section.transmitCondition.isEmpty()) {
                        // Refused rather than silently sent as cyclic. The
                        // section says "only transmit on a condition" and names
                        // none; mapping it to a message that transmits
                        // CONTINUOUSLY is the one outcome the author certainly
                        // did not ask for.
                        r.errors.append(QStringLiteral(
                            "%1: Triggered transmit with no User Condition selected").arg(where));
                    } else {
                        pendingTriggers.append({r.tables.messages.size(),
                                                section.transmitCondition, where});
                    }
                }
                msg.src_bus = quint8(busIdx + 1);
                msg.dlc = quint8(qBound(0, section.messageLengthBytes, section.fd ? 64 : 8));
                // An explicit period (set on Get) is authoritative and survives
                // the round-trip exactly; otherwise derive it from the rate.
                // The 5 ms floor is the device's own: the transmit scheduler
                // runs 5 ms slots (ENGINE_TX_SERVICE_MS), so 200 Hz is the
                // fastest a message can actually be honoured at.
                const int rate = qBound(1, section.transmitRateHz, 200);
                msg.period_ms = section.transmitPeriodMs > 0
                                    ? quint16(qBound(5, section.transmitPeriodMs, 65535))
                                    : quint16(qMax(5, 1000 / rate));
                const int msgIdx = r.tables.messages.size();
                messageIndexByBusName.insert(messageRefKey(busIdx + 1, section.name),
                                             msgIdx);
                r.tables.messages.append(msg);

                // Transmit CRC8: the section's checksum recipe becomes one
                // Crc8Config bound to the message record just appended — emitted
                // here, inside the message walk, because msgIdx is the binding
                // and nowhere else knows it. The section is otherwise an
                // ordinary transmit message (isTransmit() is true for it, so
                // the channel/compound/routing paths below apply unchanged);
                // this block adds only the stamp.
                if (section.isCrc8()) {
                    if (r.tables.crc8.size() >= MAX_CRC8_MESSAGES) {
                        r.errors.append(QStringLiteral("%1: device CRC8 table is full (%2)")
                                            .arg(where).arg(MAX_CRC8_MESSAGES));
                    } else if (section.crcChannel.isEmpty()) {
                        // The editor refuses to close without a CRC channel, so
                        // this is a hand-edited .ct3. Mapping it anyway would
                        // mean dest SIG_MSG_NONE — the device stamps the frame
                        // and publishes nothing, silently — and a silent
                        // divergence from what every editor-built section does
                        // is worth a refusal that says why.
                        r.errors.append(QStringLiteral(
                            "%1: no CRC channel — the checksum needs a channel to publish to")
                                            .arg(where));
                    } else {
                        // The CRC channel's value slot. The DEVICE writes it (the
                        // composer publishes each stamped frame's checksum there),
                        // so it goes through the same allocator a receive row
                        // uses: the slot lands in signalToChannel and the monitor
                        // can watch the wire's checksum like any other channel.
                        const int destIdx = signalFor(section.crcChannel);
                        if (destIdx >= MAX_SIGNALS) {
                            r.errors.append(QStringLiteral(
                                "%1: device signal table is full (%2)")
                                                .arg(where).arg(MAX_SIGNALS));
                        } else {
                            Crc8Config cc{};
                            cc.msg_idx = quint16(msgIdx);
                            cc.dest_signal_idx = quint16(destIdx);
                            cc.byte_location = quint8(qBound(0, section.crcByteLocation, 7));
                            cc.polynomial = quint8(section.crcPolynomial & 0xFF);
                            cc.init_value = quint8(section.crcInitValue & 0xFF);
                            cc.final_xor = quint8(section.crcFinalXor & 0xFF);
                            // ALWAYS ACTIVE, even on an Off bus — the message's
                            // own ACTIVE flag is the on/off switch (an
                            // un-composed frame is never stamped), and !ACTIVE
                            // must stay free to mean "empty slot" on read-back.
                            // See the crc8 table's comment in device_mapper.h.
                            cc.flags = CRC8FLAG_ACTIVE;
                            if (section.crcRefIn)
                                cc.flags |= CRC8FLAG_REF_IN;
                            if (section.crcRefOut)
                                cc.flags |= CRC8FLAG_REF_OUT;
                            // 0..CRC8_MAX_ELEMENTS: zero is a real spelling.
                            // Nothing feeds the register, so the stamp is the
                            // constant init/final-XOR transform — the engine's
                            // element loop simply runs no iterations. The
                            // editor can build one (Element Count 0) and the
                            // validator warns about it; the mapper transports
                            // it faithfully.
                            const int n =
                                qBound(0, int(section.crcElements.size()), CRC8_MAX_ELEMENTS);
                            cc.element_count = quint8(n);
                            // Tails past element_count stay at the record's
                            // zero-init, so a reused slot cannot leak a longer
                            // recipe's leftovers into a Verify comparison.
                            for (int e = 0; e < n; ++e) {
                                const CommsSection::CrcElement el = section.crcElements.value(e);
                                cc.elem_type[e] = quint8(el.type);
                                cc.elem_value[e] = quint8(el.value & 0xFF);
                            }
                            r.tables.crc8.append(cc);
                        }
                    }
                }

                // Emit one transmit signal, optionally mux-gated (compound). The
                // composer reads its channel's value slot (unit_type/unit_val)
                // and, for compound, writes muxId/muxMask into each variant frame.
                auto emitTxSignal = [&](const CommsChannelRow &row, quint8 muxOffset,
                                        quint16 muxId, quint16 muxMask) {
                    ExtractionFields fields;
                    QString reason;
                    if (!computeExtraction(row, section.alignment, section.messageLengthBytes,
                                           &fields, &reason)) {
                        r.errors.append(QStringLiteral("%1 · %2: %3")
                                            .arg(where, row.channelName, reason));
                        return;
                    }
                    // Canonical value slot for the channel (created virtual if
                    // nothing generates it yet), read by the TX composer.
                    const int sourceIdx = signalFor(row.channelName);
                    if (r.tables.signalConfigs.size() >= MAX_SIGNALS || sourceIdx >= MAX_SIGNALS) {
                        r.errors.append(QStringLiteral("%1: device signal table is full (%2)")
                                            .arg(where).arg(MAX_SIGNALS));
                        return;
                    }
                    const Channel ch = catalog.findByName(row.channelName);
                    CanSignalConfig sig{};
                    // belongs to this (transmit) message
                    sigSetHeader(sig, quint16(msgIdx), fields.byteOrder, 1);
                    // The ONE place the model's polarity meets the wire's. The
                    // row says "clamp" because that is the box the user ticks;
                    // the record says "wrap" because a zero bit has to mean the
                    // behaviour every configuration written before it had.
                    sigSetTxWrap(sig, !row.clampToRange);
                    sigSetBits(sig, fields.startBit, fields.bitLength, fields.valueType,
                               0 /* scaling is fully in factor/offset */, muxOffset);
                    sig.factor = float(row.dbcFactor); // DBC scaling: physical = raw × factor + offset
                    sig.offset = float(row.dbcOffset);
                    sig.min_val = float(ch.isValid() ? ch.minValue : -1e9);
                    sig.max_val = float(ch.isValid() ? ch.maxValue : 1e9);
                    sig.default_value = float(row.defaultValue); // preserved so Send→Get round-trips
                    sig.mux_id = muxId;
                    sig.mux_mask = muxMask;
                    // The value slot the composer reads to pack this field
                    // ("source index + 1"; 0 would mean this signal's own slot).
                    sig.tx_source = quint16(sourceIdx + 1);
                    fillLabel(sig, row.channelName);
                    r.tables.signalConfigs.append(sig);
                };

                // Non-compound: section.rows are the message's signals. Compound:
                // channels live only inside identifiers; each identifier's rows
                // are gated and written into that identifier's variant frame.
                if (!section.compound)
                    for (const CommsChannelRow &row : section.rows)
                        emitTxSignal(row, 0, 0, 0);
                if (section.compound) {
                    // Distinct selectors this section actually emits, keyed the
                    // way the firmware's collectMuxIdentifiers keys them
                    // (offset + mask + MASKED id). Its MAX_TX_MUX_IDS cap is
                    // silent — variants past it are stored and never appear on
                    // the wire — so exceeding it must be reported here, from
                    // the same count the device will arrive at.
                    QList<quint64> selectors;
                    for (const CompoundIdentifier &ident : section.identifiers) {
                        if (ident.idMask > 0xFFFFu) {
                            // v15 narrowed the selector to 16 bits. Truncating
                            // would zero the mask, and a zero mask means
                            // "always active" — the signal would silently stop
                            // being gated rather than fail, so refuse instead.
                            r.errors.append(QStringLiteral(
                                "%1: compound identifier mask 0x%2 does not fit the device's "
                                "16-bit selector window — the multiplexor field must lie within "
                                "2 bytes of its offset")
                                    .arg(where)
                                    .arg(QString::number(ident.idMask, 16).toUpper()));
                            continue;
                        }
                        if (ident.idMask == 0) {
                            r.warnings.append(QStringLiteral(
                                "%1: compound identifier 0x%2 has a zero mask; its %3 channel(s) "
                                "cannot be transmitted as a distinct variant — skipped")
                                    .arg(where)
                                    .arg(QString::number(ident.id, 16).toUpper())
                                    .arg(ident.rows.size()));
                            continue;
                        }
                        if (!ident.rows.isEmpty() || ident.configured) {
                            // Counted when it reaches the wire format, which a
                            // channel-less identifier now does too — it emits a
                            // selector-only signal below, and the device collects
                            // that exactly like any other.
                            const quint64 sel = (quint64(quint8(ident.byteOffset)) << 32)
                                                | (quint64(ident.idMask) << 16)
                                                | quint64(ident.id & ident.idMask);
                            if (!selectors.contains(sel))
                                selectors.append(sel);
                        }
                        if (ident.rows.isEmpty()) {
                            // A variant whose whole content is its selector — a
                            // request or ping frame, where the ID byte IS the
                            // message. The device infers a compound message's
                            // variants by walking its SIGNALS, so an identifier
                            // with nothing bound to it used to infer nothing and
                            // never went out. One selector-only signal declares
                            // it; the composer packs no bits for it, so the frame
                            // carries its selector over a zeroed payload.
                            //
                            // Gated on `configured`: the editor hands out
                            // identifier SLOTS, and an untouched slot is not a
                            // sub-message somebody wants on the bus every period.
                            if (!ident.configured)
                                continue;
                            if (r.tables.signalConfigs.size() >= MAX_SIGNALS) {
                                r.errors.append(
                                    QStringLiteral("%1: device signal table is full (%2)")
                                        .arg(where).arg(MAX_SIGNALS));
                                continue;
                            }
                            CanSignalConfig sel{};
                            // Same rule computeExtraction applies (line 122): WordSwap is
                            // Intel, Normal is Motorola. It packs nothing, so this
                            // only matters because Get reads a message's alignment
                            // back off its signals.
                            const quint8 selOrder =
                                (section.alignment == SectionAlignment::WordSwap) ? 0 : 1;
                            sigSetHeader(sel, quint16(msgIdx), selOrder, 1);
                            sigSetSelectorOnly(sel, true);
                            // Width is a formality — nothing is packed from it —
                            // but a zero-length field is not a legal record, and
                            // the mux byte offset has to ride in the bits word.
                            sigSetBits(sel, 0, 1, SIGNAL_TYPE_UINT8, 0, quint8(ident.byteOffset));
                            sel.factor = 1.0f;
                            sel.mux_id = quint16(ident.id);
                            sel.mux_mask = quint16(ident.idMask);
                            // No label: it names no channel, and Get recognises
                            // it by the flag rather than by anything written here.
                            r.tables.signalConfigs.append(sel);
                            continue;
                        }
                        for (const CommsChannelRow &row : ident.rows)
                            emitTxSignal(row, quint8(ident.byteOffset), quint16(ident.id),
                                         quint16(ident.idMask));
                    }
                    if (selectors.size() > MAX_TX_MUX_IDS)
                        r.warnings.append(QStringLiteral(
                            "%1: %2 compound identifiers, but the firmware transmits only "
                            "the first %3 — the rest are sent to the device and never "
                            "appear on the wire")
                                .arg(where)
                                .arg(selectors.size())
                                .arg(MAX_TX_MUX_IDS));
                }
                continue;
            }
            if (section.isRelay()) {
                // v11: message relay — a masked-ID gateway rule, not a message.
                if (r.tables.relays.size() >= MAX_RELAYS) {
                    r.errors.append(QStringLiteral("%1: device relay table is full (%2)")
                                        .arg(where).arg(MAX_RELAYS));
                    continue;
                }
                RelayConfig rl{};
                rl.address = section.baseAddress;
                rl.bitmask = section.relayBitmask;
                rl.flags = busOn ? RELAYFLAG_ACTIVE : 0;
                if (section.extended)
                    rl.flags |= RELAYFLAG_EXTENDED;
                if (section.relayInvert)
                    rl.flags |= RELAYFLAG_INVERT;
                // Relays carry the tier too, in the same top two bits of their
                // own flags byte (RELAYFLAG_* only ever used bits 0-2). Without
                // this a marked relay concealed in the GUI, reached the device
                // bare, and came back from a Get as an ordinary relay — the
                // laundering the transport exists to prevent, on the one section
                // kind that had been left out of it.
                applyProtection(rl.flags, section.protection);
                rl.src_bus = quint8(busIdx + 1);
                // Never forward back onto the source bus — mask that bit out.
                rl.forward_bus_mask = quint8(section.routeBusMask & 0x7 & ~(1 << busIdx));
                r.tables.relays.append(rl);
                continue;
            }
            if (r.tables.messages.size() >= MAX_MESSAGES) {
                r.errors.append(QStringLiteral("%1: device message table is full (%2)")
                                    .arg(where).arg(MAX_MESSAGES));
                continue;
            }

            CanMessageConfig msg{};
            msg.can_id = section.baseAddress;
            msg.flags = busOn ? MSGFLAG_ACTIVE : 0;
            if (section.extended)
                msg.flags |= MSGFLAG_EXTENDED;
            if (section.fd)
                msg.flags |= MSGFLAG_FD;
            if (section.routeEnable) {
                msg.flags |= MSGFLAG_ROUTING;
                msg.route_bus_mask = quint8(section.routeBusMask & 0x7);
            }
            // Same helper as the transmit branch above, and the same reasoning.
            applyProtection(msg.flags, section.protection);
            msg.reserved = 0;
            // A receive message has no trigger, and says so rather than leaving
            // the sentinel to the zero-init — see the transmit branch.
            msg.tx_trigger_cond = TX_TRIGGER_COND_NONE;
            msg.tx_trigger_flags = 0;
            msg.src_bus = quint8(busIdx + 1);
            msg.dlc = quint8(qBound(0, section.messageLengthBytes, section.fd ? 64 : 8));
            // period_ms doubles as the receive timeout: signals revert to their
            // defaults after this long without a frame. 0 disables the feature
            // (the "Default value on timeout" box is unchecked or timeout is 0).
            msg.period_ms = (section.defaultValueOnTimeout && section.receiveTimeoutMs > 0)
                                ? quint16(qBound(0, section.receiveTimeoutMs, 65535))
                                : 0;
            const int msgIdx = r.tables.messages.size();
            messageIndexByBusName.insert(messageRefKey(busIdx + 1, section.name), msgIdx);
            r.tables.messages.append(msg);

            // Emit one receive signal, optionally mux-gated (compound). muxMask
            // == 0 means always active; otherwise the firmware extracts the
            // signal only while (frame selector & muxMask) == (muxId & muxMask).
            auto emitRxSignal = [&](const CommsChannelRow &row, quint8 muxOffset,
                                    quint16 muxId, quint16 muxMask) {
                ExtractionFields fields;
                QString reason;
                if (!computeExtraction(row, section.alignment, section.messageLengthBytes,
                                       &fields, &reason)) {
                    r.errors.append(QStringLiteral("%1 · %2: %3").arg(where, row.channelName, reason));
                    return;
                }
                const Channel ch = catalog.findByName(row.channelName);
                int sigIdx;
                if (muxMask != 0) {
                    // Compound identifier signal: a distinct receive signal per
                    // identifier occurrence, so the same channel may be defined in
                    // several identifiers (each decodes under its own selector).
                    // Its value slot is the first such signal; the name resolves
                    // there for calculations/monitoring.
                    if (r.tables.signalConfigs.size() >= MAX_SIGNALS) {
                        r.errors.append(QStringLiteral("%1: device signal table is full (%2)")
                                            .arg(where).arg(MAX_SIGNALS));
                        return;
                    }
                    sigIdx = r.tables.signalConfigs.size();
                    r.tables.signalConfigs.append(CanSignalConfig{});
                    const QString key = row.channelName.toLower();
                    if (!r.channelToSignal.contains(key)) {
                        checkLabelCollision(row.channelName); // distinct over-long names
                        r.channelToSignal.insert(key, sigIdx);
                        r.signalToChannel.insert(sigIdx, row.channelName);
                    }
                } else {
                    bool created = false;
                    sigIdx = signalFor(row.channelName, &created);
                    if (sigIdx >= MAX_SIGNALS) {
                        r.errors.append(QStringLiteral("%1: device signal table is full (%2)")
                                            .arg(where).arg(MAX_SIGNALS));
                        return;
                    }
                    if (!created && sigMsgIdx(r.tables.signalConfigs[sigIdx]) != SIG_MSG_NONE)
                        r.warnings.append(QStringLiteral(
                            "%1 · %2: channel is already generated by another comms row; "
                            "this row overrides it").arg(where, row.channelName));
                }
                CanSignalConfig &sig = r.tables.signalConfigs[sigIdx];
                sigSetHeader(sig, quint16(msgIdx), fields.byteOrder, 1);
                // Receive never wraps, and this slot may be one an earlier pass
                // created, so the bit is cleared rather than assumed clear.
                sigSetTxWrap(sig, false);
                // decimals 0: the scaling is fully expressed in factor/offset
                sigSetBits(sig, fields.startBit, fields.bitLength, fields.valueType, 0, muxOffset);
                sig.factor = float(row.dbcFactor); // DBC scaling: physical = raw × factor + offset
                sig.offset = float(row.dbcOffset);
                sig.min_val = float(ch.isValid() ? ch.minValue : -1e9);
                sig.max_val = float(ch.isValid() ? ch.maxValue : 1e9);
                // Physical value the firmware writes to this slot on timeout.
                sig.default_value = float(row.defaultValue);
                sig.mux_id = muxId;
                sig.mux_mask = muxMask;
                fillLabel(sig, row.channelName);
                if (!ch.isValid())
                    r.warnings.append(QStringLiteral(
                        "%1 · %2: channel not found in the catalogue; using base resolution 1")
                                          .arg(where, row.channelName));
            };

            // Non-compound: section.rows are the message's signals (all active).
            // Compound: channels live only inside identifiers (gated by each
            // identifier's byteOffset/id/mask) — section.rows is unused.
            if (!section.compound)
                for (const CommsChannelRow &row : section.rows)
                    emitRxSignal(row, 0, 0, 0);
            if (section.compound) {
                for (const CompoundIdentifier &ident : section.identifiers) {
                    if (ident.idMask > 0xFFFFu) {
                        // v15 narrowed the selector to 16 bits. Truncating
                        // would zero the mask, and a zero mask means
                        // "always active" — the signal would silently stop
                        // being gated rather than fail, so refuse instead.
                        r.errors.append(QStringLiteral(
                            "%1: compound identifier mask 0x%2 does not fit the device's "
                            "16-bit selector window — the multiplexor field must lie within "
                            "2 bytes of its offset")
                                .arg(where)
                                .arg(QString::number(ident.idMask, 16).toUpper()));
                        continue;
                    }
                    if (ident.idMask == 0) {
                        r.warnings.append(QStringLiteral(
                            "%1: compound identifier 0x%2 has a zero mask; its %3 channel(s) "
                            "would always match — skipped")
                                .arg(where)
                                .arg(QString::number(ident.id, 16).toUpper())
                                .arg(ident.rows.size()));
                        continue;
                    }
                    for (const CommsChannelRow &row : ident.rows)
                        emitRxSignal(row, quint8(ident.byteOffset), quint16(ident.id),
                                     quint16(ident.idMask));
                }
            }
        }
    }

    // Pre-allocate every generated output (math/condition/counter/timer/
    // integrator destination) so rows may reference each other regardless of
    // order (the
    // firmware evaluates all slots each pass; a reference sees the value with
    // one-pass delay).
    for (const MathRow &m : config.mathRows)
        if (m.active && !m.destChannel.isEmpty())
            signalFor(m.destChannel);
    for (const ConditionRow &c : config.conditionRows)
        if (c.active && !c.outputChannel.isEmpty())
            signalFor(c.outputChannel);
    for (const CounterRow &c : config.counterRows)
        if (c.active && !c.outputChannel.isEmpty())
            signalFor(c.outputChannel);
    for (const TimerRow &t : config.timerRows)
        if (t.active && !t.outputChannel.isEmpty())
            signalFor(t.outputChannel);
    for (const IntegratorRow &g : config.integratorRows)
        if (g.active && !g.outputChannel.isEmpty())
            signalFor(g.outputChannel);
    for (const ConstantRow &c : config.constantRows)
        if (c.active && !c.name.isEmpty())
            signalFor(c.name);
    for (const Table2x16Row &t : config.table2x16Rows)
        if (t.active && !t.outputChannel.isEmpty())
            signalFor(t.outputChannel);
    // Device channels are allocated only if the document reads them. Unlike
    // every other producer above, nothing in the configuration creates this
    // channel — the firmware does — so the trigger is a REFERENCE to it
    // anywhere, which signalFor() has already recorded by the time the rows
    // below are walked. Handled after the mapping loops, near the end of this
    // function, where the full reference set is known.
    for (const Table8x8Row &t : config.table8x8Rows)
        if (t.active && !t.outputChannel.isEmpty())
            signalFor(t.outputChannel);

    // Math rows
    for (int i = 0; i < config.mathRows.size(); ++i) {
        const MathRow &m = config.mathRows[i];
        if (!m.active)
            continue;
        const QString where = QStringLiteral("Math %1").arg(i + 1);
        if (r.tables.math.size() >= MAX_MATH_COMPUTATIONS) {
            r.errors.append(QStringLiteral("%1: device math table is full").arg(where));
            break;
        }
        MathConfig mc{};
        mc.op = quint8(m.op);
        mc.is_active = 1;
        auto resolveInput = [&](bool isChannel, const QString &name, double constVal,
                                quint8 &type, quint16 &idx, float &constOut) -> bool {
            if (!isChannel) {
                type = 0;
                idx = 0;
                constOut = float(constVal);
                return true;
            }
            if (name.isEmpty()) {
                r.errors.append(QStringLiteral("%1: no input channel selected").arg(where));
                return false;
            }
            // Reading a channel is always expressible: signalFor() hands back
            // its value slot, creating a virtual one holding the default if
            // nothing writes it — the same slot a transmit row reads from. The
            // math row reads that slot and leaves it alone, so this cannot
            // clash with whatever else reads or writes the channel.
            const int inputIdx = signalFor(name);
            if (inputIdx >= MAX_SIGNALS) {
                r.errors.append(QStringLiteral("%1: device signal table is full (%2)")
                                    .arg(where).arg(MAX_SIGNALS));
                return false;
            }
            type = 1;
            idx = quint16(inputIdx);
            constOut = 0;
            return true;
        };
        // Only the operands the op reads are resolved; the rest stay at the
        // zero-initialised defaults (const 0), which is what the engine
        // ignores and what a Get reads back.
        const int arity = mathOpArity(m.op);
        const bool okA = resolveInput(m.aIsChannel, m.aChannel, m.aConst, mc.input_a_type,
                                      mc.input_a_idx, mc.input_a_const);
        bool okB = true;
        if (arity >= 2)
            okB = resolveInput(m.bIsChannel, m.bChannel, m.bConst, mc.input_b_type,
                               mc.input_b_idx, mc.input_b_const);
        bool okC = true;
        if (arity == 3) {
            // Same resolution (and pre-allocation of a referenced channel) as
            // A/B; only the packing differs — C is four raw bytes on the wire.
            quint8 cType = 0;
            quint16 cIdx = 0;
            float cConst = 0;
            okC = resolveInput(m.cIsChannel, m.cChannel, m.cConst, cType, cIdx, cConst);
            if (okC) {
                if (cType == 1)
                    mathSetInputCSignal(mc, cIdx);
                else
                    mathSetInputCConst(mc, cConst);
            }
        }
        if (!okA || !okB || !okC)
            continue;
        if (m.destChannel.isEmpty()) {
            r.errors.append(QStringLiteral("%1: no destination channel").arg(where));
            continue;
        }
        const int destIdx = signalFor(m.destChannel);
        if (destIdx >= MAX_SIGNALS) {
            r.errors.append(QStringLiteral("%1: device signal table is full").arg(where));
            continue;
        }
        mc.dest_signal_idx = quint16(destIdx);
        r.tables.math.append(mc);
    }

    // Describe a generated value slot from its data type, so a later Get can
    // recover the channel's type and decimals. Only stamps a slot no other
    // generator has typed yet: a name shared between, say, a table output and a
    // condition must not have one silently rewrite the other's type — validation
    // flags the overlap instead.
    //
    // Defined HERE, above the conditions, rather than beside the lookup tables
    // that first needed it: conditions type their output too now, and they are
    // mapped first.
    auto typeOutputSignal = [&](int destIdx, const QString &dataType, int decimals) {
        CanSignalConfig &sig = r.tables.signalConfigs[destIdx];
        if (sigValueType(sig) != 0)
            return; // already typed by another generator (overlap flagged by validation)
        const quint8 vt = valueTypeForDataType(dataType);
        if (vt != 0) {
            sigSetValueType(sig, vt);
            sigSetBitLength(sig, (vt == SIGNAL_TYPE_FLOAT)                                ? 32
                                 : (vt == SIGNAL_TYPE_UINT8 || vt == SIGNAL_TYPE_INT8)   ? 8
                                 : (vt == SIGNAL_TYPE_UINT16 || vt == SIGNAL_TYPE_INT16) ? 16
                                                                                         : 32);
        }
        sigSetDecimalPlaces(sig, int8_t(qBound(0, decimals, 8)));
    };

    // Condition rows. The output channel of each one that actually reaches the
    // device, against the index it landed at — the lookup the Triggered-transmit
    // resolve pass below needs, recorded HERE because this loop is the only
    // thing that knows which rows survived and in what order.
    //
    // No deferral is needed for the MESSAGE operands, unlike the transmit
    // trigger: every bus has been walked by the time this loop runs, so
    // messageIndexByBusName is already complete.
    QHash<QString, int> conditionIndexByOutput;
    for (int i = 0; i < config.conditionRows.size(); ++i) {
        const ConditionRow &c = config.conditionRows[i];
        if (!c.active)
            continue;
        const QString where = QStringLiteral("User Condition %1").arg(i + 1);
        if (r.tables.conditions.size() >= MAX_CONDITIONS) {
            r.errors.append(QStringLiteral("%1: device condition table is full").arg(where));
            break;
        }
        if (c.outputChannel.isEmpty()) {
            r.errors.append(QStringLiteral("%1: no output channel selected").arg(where));
            continue;
        }
        ConditionConfig cc{};
        cc.flags = CONDFLAG_ACTIVE;
        if (c.mode == ConditionMode::SetReset)
            cc.flags |= CONDFLAG_SETRESET;
        cc.latch_hz = quint8(qBound(1, c.latchHz, int(COND_LATCH_MAX_HZ)));

        // One expression into its three wire slots. Shared by Set and Reset so
        // the two can never disagree about how a term is packed; `side` is
        // carried only to name the right one in an error a user has to act on.
        const auto mapExpr = [&](const QList<ConditionTermRow> &terms,
                                 const QList<int> &joiners, const QString &side,
                                 ConditionTerm *out, quint8 *countOut,
                                 quint8 *joinersOut) -> bool {
            const int n = qMin(int(terms.size()), COND_MAX_TERMS);
            if (n < 1) {
                r.errors.append(QStringLiteral("%1: no %2 comparisons").arg(where, side));
                return false;
            }
            for (int t = 0; t < n; ++t) {
                const ConditionTermRow &tr = terms[t];
                ConditionTerm &wt = out[t];
                wt.op = quint8(tr.op);
                if (tr.isMessageOp()) {
                    // input_a is a MESSAGE index for these, and input_b is
                    // unused — left zero by the value-initialised record above,
                    // which also zeroes the whole union.
                    if (tr.aMessage.isEmpty()) {
                        r.errors.append(
                            QStringLiteral("%1 %2 comparison %3: no message selected")
                                .arg(where, side).arg(t + 1));
                        return false;
                    }
                    const auto mit = messageIndexByBusName.constFind(
                        messageRefKey(tr.aMessageBus, tr.aMessage));
                    if (mit == messageIndexByBusName.constEnd()) {
                        // Refused, not skipped. A term that silently evaluated
                        // false would leave a Set that never sets or a Reset
                        // that never clears, and the configuration would look
                        // like it had mapped cleanly.
                        r.errors.append(
                            QStringLiteral("%1 %2 comparison %3: CAN %4 has no message "
                                           "named %5")
                                .arg(where, side).arg(t + 1).arg(tr.aMessageBus)
                                .arg(tr.aMessage));
                        return false;
                    }
                    wt.input_a_signal_idx = quint16(*mit);
                    continue;
                }
                // Both sides are reads, so any named channel resolves — to a
                // virtual default-valued slot when nothing writes it. Only a
                // term left blank is unmappable: there is no value to compare.
                const auto readSlot = [&](const QString &name) -> int {
                    if (name.isEmpty()) {
                        r.errors.append(
                            QStringLiteral("%1 %2 comparison %3: no input channel selected")
                                .arg(where, side).arg(t + 1));
                        return -1;
                    }
                    const int idx = signalFor(name);
                    if (idx >= MAX_SIGNALS) {
                        r.errors.append(QStringLiteral("%1: device signal table is full (%2)")
                                            .arg(where).arg(MAX_SIGNALS));
                        return -1;
                    }
                    return idx;
                };
                const int idxA = readSlot(tr.aChannel);
                if (idxA < 0)
                    return false;
                wt.input_a_signal_idx = quint16(idxA);
                if (tr.bIsChannel) {
                    const int idxB = readSlot(tr.bChannel);
                    if (idxB < 0)
                        return false;
                    wt.input_b_type = 1;
                    wt.b.input_b_idx = quint16(idxB);
                } else {
                    wt.input_b_type = 0;
                    wt.b.input_b_const = float(tr.bConst);
                }
            }
            *countOut = quint8(n);
            *joinersOut = 0;
            // One joiner bit per gap; a missing entry defaults to AND.
            for (int g = 0; g + 1 < n; ++g)
                if (joiners.value(g, int(COND_JOIN_AND)) == int(COND_JOIN_OR))
                    *joinersOut |= quint8(1u << g);
            return true;
        };

        // Any unresolved term invalidates the WHOLE condition — a partially
        // built expression would evaluate to something the user never wrote.
        if (!mapExpr(c.setTerms, c.setJoiners, QStringLiteral("Set"), cc.set_terms,
                     &cc.set_count, &cc.set_joiners))
            continue;
        // The Reset half is only sent for a Set/Reset. A Momentary keeps the
        // editor's Reset expression in the document so switching modes does not
        // destroy what was typed, but the device must not see it: reset_count 0
        // is how the engine is told there is no Reset, and a Momentary has none.
        if (c.mode == ConditionMode::SetReset) {
            if (!mapExpr(c.resetTerms, c.resetJoiners, QStringLiteral("Reset"), cc.reset_terms,
                         &cc.reset_count, &cc.reset_joiners))
                continue;
        }

        const int destIdx = signalFor(c.outputChannel);
        if (destIdx >= MAX_SIGNALS) {
            r.errors.append(QStringLiteral("%1: device signal table is full").arg(where));
            continue;
        }
        cc.dest_signal_idx = quint16(destIdx);
        // A User Condition's output is boolean by definition — the engine writes
        // 1.0 or 0.0 into it and nothing else ever has. Stamping the slot says
        // so on the wire, which matters for the round trip: a Get reads the type
        // back out, and an untyped slot comes home with no data type at all.
        //
        // Math, counter, timer and integrator outputs are equally untyped, and
        // deliberately so: what they produce depends on what the user configured,
        // so there is nothing for the mapper to declare. A condition is the case
        // where the type IS knowable without being declared, which is why leaving
        // this slot blank was a gap rather than a policy.
        typeOutputSignal(destIdx, QStringLiteral("boolean"), 0);
        conditionIndexByOutput.insert(c.outputChannel.toLower(), r.tables.conditions.size());
        r.tables.conditions.append(cc);
    }

    // Triggered transmit: bind each waiting message to the condition it named.
    //
    // Anything unresolved is an ERROR rather than a quiet fall back to cyclic,
    // for the reason the parking comment above gives — a message configured to
    // speak only on a condition must not become one that never stops.
    for (const PendingTrigger &pt : pendingTriggers) {
        const auto it = conditionIndexByOutput.constFind(pt.conditionChannel.toLower());
        if (it == conditionIndexByOutput.constEnd()) {
            r.errors.append(QStringLiteral(
                "%1: Triggered transmit names '%2', which is not the output of any active "
                "User Condition").arg(pt.where, pt.conditionChannel));
            continue;
        }
        CanMessageConfig &msg = r.tables.messages[pt.msgIdx];
        msg.tx_trigger_cond = quint16(*it);
        msg.tx_trigger_flags = TXTRIG_ENABLED;
    }

    // Resolves a boolean-input channel to a signal index (0xFFFF when empty,
    // which the device reads as "no trigger"). A named channel always resolves:
    // the trigger READS the slot, so nothing writing it yet just means the
    // trigger reads its default — the same thing that happens on the device
    // before the first frame arrives. Only a full signal table can fail.
    auto resolveBoolInput = [&](const QString &name, const QString &where,
                                const QString &label) -> quint16 {
        if (name.isEmpty())
            return SIG_MSG_NONE;
        const int idx = signalFor(name);
        if (idx >= MAX_SIGNALS) {
            r.errors.append(QStringLiteral("%1: %2 channel '%3': device signal table is full (%4)")
                                .arg(where, label, name).arg(MAX_SIGNALS));
            return SIG_MSG_NONE;
        }
        return quint16(idx);
    };

    // Counter rows
    for (int i = 0; i < config.counterRows.size(); ++i) {
        const CounterRow &c = config.counterRows[i];
        if (!c.active)
            continue;
        const QString where = QStringLiteral("Counter %1").arg(i + 1);
        if (r.tables.counters.size() >= MAX_COUNTERS) {
            r.errors.append(QStringLiteral("%1: device counter table is full (%2)")
                                .arg(where).arg(MAX_COUNTERS));
            break;
        }
        if (c.outputChannel.isEmpty()) {
            r.errors.append(QStringLiteral("%1: no output channel").arg(where));
            continue;
        }
        CounterConfig cfg{};
        // Rate mode is driven by the clock, so the three counting inputs are
        // not resolved at all: leaving them SIG_MSG_NONE keeps a stale channel
        // reference from a mode change out of the record, and out of the
        // reference walks that would otherwise report the counter as reading a
        // channel it no longer looks at. Enable and Reset still apply.
        const bool isRate = (c.mode == COUNTER_MODE_RATE);
        cfg.up_signal_idx = isRate ? SIG_MSG_NONE
                                   : resolveBoolInput(c.upChannel, where, QStringLiteral("up"));
        cfg.down_signal_idx = isRate ? SIG_MSG_NONE
                                     : resolveBoolInput(c.downChannel, where, QStringLiteral("down"));
        cfg.follow_signal_idx =
            isRate ? SIG_MSG_NONE
                   : resolveBoolInput(c.followChannel, where, QStringLiteral("follow"));
        cfg.reset_signal_idx = resolveBoolInput(c.resetChannel, where, QStringLiteral("reset"));
        cfg.enable_signal_idx = resolveBoolInput(c.enableChannel, where, QStringLiteral("enable"));
        cfg.dest_signal_idx = quint16(signalFor(c.outputChannel));
        cfg.min_value = float(c.minValue);
        cfg.max_value = float(c.maxValue);
        cfg.reset_value = float(c.resetValue);
        cfg.step = float(c.step);
        cfg.mode = quint8(c.mode == COUNTER_MODE_RATE     ? COUNTER_MODE_RATE
                          : c.mode == COUNTER_MODE_FOLLOW ? COUNTER_MODE_FOLLOW
                                                          : COUNTER_MODE_UPDOWN);
        // Zero outside rate mode so a record cannot carry a rate the editor no
        // longer shows -- a Get would otherwise read it back and resurrect it.
        cfg.rate_hz = isRate ? quint8(qBound(1, c.rateHz, COUNTER_MAX_HZ)) : quint8(0);
        cfg.flags = COUNTERFLAG_ACTIVE;
        if (isRate && c.rateCountDown)
            cfg.flags |= COUNTERFLAG_RATE_DOWN;
        if (c.rollAtLimits)
            cfg.flags |= COUNTERFLAG_ROLL;
        if (c.preserveValue)
            cfg.flags |= COUNTERFLAG_PRESERVE;
        r.tables.counters.append(cfg);
    }

    // Timer rows
    for (int i = 0; i < config.timerRows.size(); ++i) {
        const TimerRow &t = config.timerRows[i];
        if (!t.active)
            continue;
        const QString where = QStringLiteral("Timer %1").arg(i + 1);
        if (r.tables.timers.size() >= MAX_TIMERS) {
            r.errors.append(QStringLiteral("%1: device timer table is full (%2)")
                                .arg(where).arg(MAX_TIMERS));
            break;
        }
        if (t.outputChannel.isEmpty()) {
            r.errors.append(QStringLiteral("%1: no output channel").arg(where));
            continue;
        }
        TimerConfig cfg{};
        cfg.start_signal_idx = resolveBoolInput(t.startChannel, where, QStringLiteral("start"));
        cfg.stop_signal_idx = resolveBoolInput(t.stopChannel, where, QStringLiteral("stop"));
        cfg.dest_signal_idx = quint16(signalFor(t.outputChannel));
        cfg.limit_value = float(t.limitValue);
        cfg.start_value = float(t.startValue);
        cfg.stop_value = float(t.stopValue);
        cfg.flags = TIMERFLAG_ACTIVE;
        if (t.countDown)
            cfg.flags |= TIMERFLAG_COUNTDOWN;
        if (t.rollover)
            cfg.flags |= TIMERFLAG_ROLLOVER;
        if (t.setOnStart)
            cfg.flags |= TIMERFLAG_SET_ON_START;
        if (t.setOnStop)
            cfg.flags |= TIMERFLAG_SET_ON_STOP;
        r.tables.timers.append(cfg);
    }

    // Integrator rows (v16). The input side is deliberately NOT resolved with
    // resolveBoolInput: that helper returns SIG_MSG_NONE for an EMPTY name,
    // which the device reads as "no channel" — fine for a trigger, wrong here,
    // where the input is the value being accumulated. A blank channel input is
    // an error, and INTEGFLAG_CONST_INPUT (not a sentinel index) is what tells
    // the device to use the fixed value, so a blank name can never masquerade
    // as a constant.
    for (int i = 0; i < config.integratorRows.size(); ++i) {
        const IntegratorRow &g = config.integratorRows[i];
        if (!g.active)
            continue;
        const QString where = QStringLiteral("Integrator %1").arg(i + 1);
        if (r.tables.integrators.size() >= MAX_INTEGRATORS) {
            r.errors.append(QStringLiteral("%1: device integrator table is full (%2)")
                                .arg(where).arg(MAX_INTEGRATORS));
            break;
        }
        if (g.outputChannel.isEmpty()) {
            r.errors.append(QStringLiteral("%1: no output channel").arg(where));
            continue;
        }
        IntegratorConfig cfg{};
        cfg.flags = INTEGFLAG_ACTIVE;
        if (g.inputIsChannel) {
            if (g.inputChannel.isEmpty()) {
                r.errors.append(QStringLiteral("%1: no input channel").arg(where));
                continue;
            }
            const int inputIdx = signalFor(g.inputChannel);
            if (inputIdx >= MAX_SIGNALS) {
                r.errors.append(QStringLiteral("%1: device signal table is full (%2)")
                                    .arg(where).arg(MAX_SIGNALS));
                continue;
            }
            cfg.input_signal_idx = quint16(inputIdx);
        } else {
            cfg.flags |= INTEGFLAG_CONST_INPUT;
            cfg.input_signal_idx = SIG_MSG_NONE;
            cfg.input_const = float(g.inputValue);
        }
        if (g.countDown)
            cfg.flags |= INTEGFLAG_COUNT_DOWN;
        if (g.preserveValue)
            cfg.flags |= INTEGFLAG_PRESERVE;
        cfg.reset_signal_idx = resolveBoolInput(g.resetChannel, where, QStringLiteral("reset"));
        cfg.enable_signal_idx = resolveBoolInput(g.enableChannel, where, QStringLiteral("enable"));
        cfg.dest_signal_idx = quint16(signalFor(g.outputChannel));
        cfg.min_value = float(g.minValue);
        cfg.max_value = float(g.maxValue);
        cfg.reset_value = float(g.resetValue);
        cfg.start_value = float(g.startValue);
        cfg.rate_hz = quint8(qBound(1, g.rateHz, INTEGRATOR_MAX_HZ));
        r.tables.integrators.append(cfg);
    }

    // Constant rows. Each writes a fixed value into its generated channel slot;
    // it also describes that slot (data type / decimals / range) so Get can
    // reconstruct the constant's channel definition.
    for (int i = 0; i < config.constantRows.size(); ++i) {
        const ConstantRow &k = config.constantRows[i];
        if (!k.active)
            continue;
        const QString where = QStringLiteral("Constant %1").arg(i + 1);
        if (r.tables.constants.size() >= MAX_CONSTANTS) {
            r.errors.append(QStringLiteral("%1: device constant table is full (%2)")
                                .arg(where).arg(MAX_CONSTANTS));
            break;
        }
        if (k.name.isEmpty()) {
            r.errors.append(QStringLiteral("%1: no channel name").arg(where));
            continue;
        }
        const int destIdx = signalFor(k.name);
        if (destIdx >= MAX_SIGNALS) {
            r.errors.append(QStringLiteral("%1: device signal table is full").arg(where));
            continue;
        }
        CanSignalConfig &sig = r.tables.signalConfigs[destIdx];
        // A constant owns its slot: it writes it every pass. If the name is
        // already a communications signal (message-bound), the constant would
        // both overwrite that signal's live value and corrupt its extraction
        // fields below — reject rather than silently clobber.
        if (sigMsgIdx(sig) != SIG_MSG_NONE) {
            r.errors.append(QStringLiteral(
                "%1: channel '%2' is already a communications signal — give the "
                "constant its own channel name").arg(where, k.name));
            continue;
        }
        // Describe the (virtual) value slot from the constant's data type so a
        // later Get recovers the channel's type/decimals. Only stamp a slot no
        // other generator has typed yet, so a name shared with a math/counter/
        // timer output doesn't rewrite that output's type (the overlap is
        // flagged by validation's duplicate-generator check instead).
        if (sigValueType(sig) == 0) {
            const quint8 vt = valueTypeForDataType(k.dataType);
            if (vt != 0) {
                sigSetValueType(sig, vt);
                sigSetBitLength(sig, bitLengthForValueType(vt));
            }
            sigSetDecimalPlaces(sig, int8_t(qBound(0, k.decimalPlaces, 8)));
        }
        ConstantConfig cc{};
        cc.dest_signal_idx = quint16(destIdx);
        cc.value = float(k.value);
        cc.is_active = 1;
        r.tables.constants.append(cc);
    }

    // Resolve a table's named output into a typed generated signal slot. Returns
    // -1 (with an error appended) if the name is empty, the table is full, or the
    // name already belongs to a communications signal.
    auto resolveTableOutput = [&](const QString &name, const QString &dataType, int decimals,
                                  const QString &where) -> int {
        if (name.isEmpty()) {
            r.errors.append(QStringLiteral("%1: no output channel name").arg(where));
            return -1;
        }
        const int destIdx = signalFor(name);
        if (destIdx >= MAX_SIGNALS) {
            r.errors.append(QStringLiteral("%1: device signal table is full").arg(where));
            return -1;
        }
        if (sigMsgIdx(r.tables.signalConfigs[destIdx]) != SIG_MSG_NONE) {
            r.errors.append(QStringLiteral(
                "%1: channel '%2' is already a communications signal — give the table its "
                "own channel name").arg(where, name));
            return -1;
        }
        typeOutputSignal(destIdx, dataType, decimals);
        return destIdx;
    };
    // An axis READS its channel, so any named channel resolves — to a virtual
    // default-valued slot when nothing writes it yet. Only a blank axis, or a
    // full signal table, is unmappable.
    auto resolveAxis = [&](const QString &name, const QString &where, const QString &axis,
                           quint16 *idxOut) -> bool {
        if (name.isEmpty()) {
            r.errors.append(QStringLiteral("%1: no %2 axis channel").arg(where, axis));
            return false;
        }
        const int idx = signalFor(name);
        if (idx >= MAX_SIGNALS) {
            r.errors.append(QStringLiteral("%1: device signal table is full (%2)")
                                .arg(where).arg(MAX_SIGNALS));
            return false;
        }
        *idxOut = quint16(idx);
        return true;
    };

    for (int i = 0; i < config.table2x16Rows.size(); ++i) {
        const Table2x16Row &t = config.table2x16Rows[i];
        if (!t.active)
            continue;
        const QString where = QStringLiteral("Table 2x16 %1").arg(i + 1);
        if (r.tables.tables2x16Def.size() >= MAX_TABLES_2X16) {
            r.errors.append(QStringLiteral("%1: device 2x16 table is full (%2)")
                                .arg(where).arg(MAX_TABLES_2X16));
            break;
        }
        const int n = qBound(0, int(t.xSites.size()), TABLE_2X16_SITES);
        if (n < 1)
            continue; // empty table — nothing to send
        quint16 xIdx = 0;
        if (!resolveAxis(t.xChannel, where, QStringLiteral("input"), &xIdx))
            continue;
        const int destIdx = resolveTableOutput(t.outputChannel, t.dataType, t.decimalPlaces, where);
        if (destIdx < 0)
            continue;
        // The definition and its outputs are appended together so the two
        // tables stay index-aligned — the device pairs them by index.
        Table2x16Def def{};
        Table2x16Out out{};
        def.x_signal_idx = xIdx;
        def.dest_signal_idx = quint16(destIdx);
        def.flags = quint8(TABLEFLAG_ACTIVE | (t.xInterp ? TABLEFLAG_X_INTERP : 0));
        def.x_count = quint8(n);
        for (int k = 0; k < n; ++k) {
            def.x_sites[k] = float(t.xSites.value(k, 0.0));
            out.outputs[k] = float(t.outputs.value(k, 0.0));
        }
        r.tables.tables2x16Def.append(def);
        r.tables.tables2x16Out.append(out);
    }

    for (int i = 0; i < config.table8x8Rows.size(); ++i) {
        const Table8x8Row &t = config.table8x8Rows[i];
        if (!t.active)
            continue;
        const QString where = QStringLiteral("Table 8x8 %1").arg(i + 1);
        if (r.tables.tables8x8Def.size() >= MAX_TABLES_8X8) {
            r.errors.append(QStringLiteral("%1: device 8x8 table is full (%2)")
                                .arg(where).arg(MAX_TABLES_8X8));
            break;
        }
        // The model's grid is strided by the row's OWN X width, which is what
        // makes the top-left placement work: a table with 3 X sites reads
        // outputs[y*3 + x] out of the document and writes it into v[0..2] of
        // each wire row, leaving the rest of the 8-wide row at zero.
        const int stride = int(t.xSites.size());
        const int nx = qBound(0, stride, TABLE_8X8_SITES);
        const int ny = qBound(0, int(t.ySites.size()), TABLE_8X8_SITES);
        if (nx < 1 || ny < 1)
            continue; // empty table — nothing to send
        quint16 xIdx = 0, yIdx = 0;
        if (!resolveAxis(t.xChannel, where, QStringLiteral("X"), &xIdx))
            continue;
        if (!resolveAxis(t.yChannel, where, QStringLiteral("Y"), &yIdx))
            continue;
        const int destIdx = resolveTableOutput(t.outputChannel, t.dataType, t.decimalPlaces, where);
        if (destIdx < 0)
            continue;
        Table8x8Def def{};
        def.x_signal_idx = xIdx;
        def.y_signal_idx = yIdx;
        def.dest_signal_idx = quint16(destIdx);
        // ACTIVE lives on the Def and nowhere else: the rows carry no flags, so
        // the single record that switches the table on is the last one sent.
        def.flags = quint8(TABLEFLAG_ACTIVE | (t.xInterp ? TABLEFLAG_X_INTERP : 0)
                           | (t.yInterp ? TABLEFLAG_Y_INTERP : 0));
        def.x_count = quint8(nx);
        def.y_count = quint8(ny);
        for (int k = 0; k < nx; ++k)
            def.x_sites[k] = float(t.xSites.value(k, 0.0));
        for (int k = 0; k < ny; ++k)
            def.y_sites[k] = float(t.ySites.value(k, 0.0));
        // ALL EIGHT rows are appended, not just the ny in use. The device
        // addresses a grid row as t*8 + y, so a table that contributed fewer
        // would slide every later table's rows out from under their Def. The
        // unused rows are zeros, which the engine never reads (y < y_count).
        r.tables.tables8x8Def.append(def);
        for (int y = 0; y < TABLE_8X8_SITES; ++y) {
            Table8x8GridRow row{};
            if (y < ny)
                for (int x = 0; x < nx; ++x)
                    row.v[x] = float(t.outputs.value(y * stride + x, 0.0));
            r.tables.tables8x8Row.append(row);
        }
    }

    // Device channels. EVERY one gets a slot, whether or not the document
    // mentions it — deliberately, and not the way this once worked.
    //
    // It used to allocate only what something already referenced, on the
    // reasoning that an unread channel should cost nothing. That is true of a
    // user channel and false of these. A device channel that is not mapped is
    // not published, and one that is not published cannot appear in Monitor
    // Channels — which is precisely where somebody looks when a bus is
    // misbehaving. Requiring them to have predicted that need, and referenced
    // the channel beforehand, means the diagnostics are missing at the only
    // moment anyone wants them. They are a baseline, so they are allocated like
    // one.
    //
    // The cost is bounded and paid once: DEVCH_COUNT slots out of MAX_SIGNALS,
    // taken at the END of the table so no document channel's index moves, and
    // 6 bytes per channel per tick on the value stream — and only while a
    // monitor is actually open, since the stream is off otherwise. A
    // configuration that genuinely cannot spare them trips the capacity check
    // below, which is the honest place for that to surface.
    //
    // Walked by catalogue rather than by name: each device channel carries the
    // wire index it publishes into, so adding one is a catalogue row and
    // nothing here changes.
    for (const Channel &dev : ChannelCatalog::deviceChannels()) {
        if (dev.deviceChannelId < 0 || dev.deviceChannelId >= DEVCH_COUNT)
            continue;
        // signalFor() returns the EXISTING slot when the document already reads
        // this channel, so a referenced device channel is unaffected by the
        // change above — it does not get a second slot.
        const int idx = signalFor(dev.name);
        if (idx >= MAX_SIGNALS)
            continue;
        r.tables.deviceChannels.signal_idx[dev.deviceChannelId] = quint16(idx);
        // Type the slot from the built-in definition, exactly as a constant or
        // a table output types the slot it owns, so a later Get can rebuild the
        // channel. Only if nothing else has claimed it: a document whose own
        // user channel shadows the name keeps its own typing, and validation
        // reports the collision rather than the mapper silently picking a side.
        CanSignalConfig &sig = r.tables.signalConfigs[idx];
        if (sigValueType(sig) == 0) {
            const quint8 vt = valueTypeForDataType(dev.dataType);
            if (vt != 0) {
                sigSetValueType(sig, vt);
                sigSetBitLength(sig, bitLengthForValueType(vt));
            }
            sigSetDecimalPlaces(sig, int8_t(qBound(0, dev.decimalPlaces, 8)));
        }
    }

    if (r.tables.signalConfigs.size() > MAX_SIGNALS)
        r.errors.append(QStringLiteral("Configuration needs %1 signals; the device supports %2")
                            .arg(r.tables.signalConfigs.size()).arg(MAX_SIGNALS));

    return r;
}

namespace {

// What the DOCUMENT knows about a section that the DEVICE cannot tell us, and
// so has to be carried across a Get by hand.
//
// `consumed` is here because the same record is reachable through two indexes —
// its wire identity and its name — and it must be handed to exactly one rebuilt
// section. Without the flag a section matched by identity could have its key
// handed out a second time to an unrelated section that happens to share the
// name.
struct PriorSection {
    AccessKey messageKey = kNoAccessKey;
    QString name;
    bool consumed = false;
};

// The handle a section keeps across a Get: which bus it sits on, which CAN ID it
// carries, and whether it receives, transmits or relays. Everything in it comes
// off the wire, which is the point — the NAME cannot be the handle, because
// mapFromDevice regenerates names from the ID and a name-keyed lookup would
// therefore match nothing and preserve nothing.
QString sectionWireIdentity(int busIndex, const CommsSection &s)
{
    // All four kinds are distinguished, including Off. An Off section is never
    // sent, so it can share a CAN ID with a live receive section without the
    // document being wrong — and folding the two together would let the rebuilt
    // receive section consume the Off one's name and password.
    return QStringLiteral("%1/%2/%3")
        .arg(busIndex)
        .arg(s.baseAddress, 0, 16)
        .arg(int(s.device));
}

// The FALLBACK handle: the bus and the user's own name for the section, lower
// cased like every other name comparison in this application.
//
// Needed because the wire identity above is exactly the part of a section a user
// is most likely to have just edited. Retype a message's base address, Get, and
// the rebuilt section carries the new ID — so it matches no snapshot, and its
// password and name were silently discarded. That is a plain bug before it is a
// security one: renumbering a message must not cost its password.
//
// Scoped to the BUS, and that is not incidental. A name is a value someone else
// may choose; a cross-bus name match would let a section created on another bus
// donate ITS key to this one, which is the same shape as the session-grant hole
// keyed on a bare name. Moving a section to a different bus therefore does lose
// its key, and that is the right trade: it is rare, it is visible, and the
// alternative is a lookup an attacker can aim.
QString sectionNameIdentity(int busIndex, const QString &name)
{
    return QStringLiteral("%1/%2").arg(busIndex).arg(name.toLower());
}

QString sectionNameIdentity(int busIndex, const CommsSection &s)
{
    return sectionNameIdentity(busIndex, s.name);
}

// The LAST-RESORT handle: the bus, the direction, and the PAYLOAD — every channel
// the section carries, lower cased, in the order it carries them.
//
// It exists because the two handles above are both fields the user EDITS, and
// editing both in one sitting is not exotic. Rename a message to something a
// person would actually type, renumber it, then Get: the rebuilt section answers
// to neither handle, so before this it matched nothing at all. That cost more
// than the password. A concealing section that matches nothing is also given a
// generated name, so "Turbo pressure" came back as "Hidden message 1" with no
// key — which is a message that can no longer be opened, no longer be unmarked,
// and can no longer even be recognised, out of an edit nobody thinks of as
// destructive. The payload is the one part of a section that neither renaming nor
// renumbering touches, and the device stores the signal labels, so it is a thing
// both sides actually have to compare.
//
// Scoped to the BUS and to the DEVICE KIND, for the reason sectionNameIdentity is
// scoped to the bus: this is a fallback that hands a document secret to a section
// rebuilt from the wire, so it must not reach across a boundary the user can see.
// A section with NO channels gets an EMPTY handle and is neither indexed nor
// looked up — every relay and every signal-less message would otherwise share one
// handle and pair at random, which is worse than not pairing at all.
//
// WHAT THIS COSTS, stated rather than left to be discovered: a Get from somebody
// else's hardware whose message happens to carry the same channel names, on the
// same bus, in the same direction, will be handed the key of the document section
// it collided with. That is the same trade the name handle already makes — a
// foreign message named "Engine Data" collides the same way — and it is bounded
// the same way: the key stays inside this document, so the collision costs its
// owner a mis-paired section rather than costing anybody a secret. A document
// with no sections in it, which is what a Get into a fresh window has, still gets
// every section back keyless.
//
// Each channel name is written LENGTH-FIRST, and that is not decoration. Channel
// names are user text and may contain whatever separator this function picks; a
// plain join lets one name forge the boundary between two and pair a section with
// a record that is not its own. See sectionPairingKey in configuration.cpp, the
// same trap answered the same way.
QString sectionPayloadIdentity(int busIndex, const CommsSection &s)
{
    const QStringList names = s.channelNames();
    if (names.isEmpty())
        return {};
    QString key = QStringLiteral("%1/%2").arg(busIndex).arg(int(s.device));
    for (const QString &n : names) {
        const QString lower = n.toLower();
        key += QStringLiteral("/%1:%2").arg(lower.size()).arg(lower);
    }
    return key;
}

// A name for a rebuilt concealing section that the document has never seen. It
// must NOT contain the CAN ID: "Receive 0x640  (hidden)" in the sections list
// discloses the exact thing Hidden and Protected withhold, in the one place the
// user is guaranteed to look, and it is the disclosure the section editor warns
// about when the box is ticked. Unique within the bus so two rebuilt sections
// cannot share a name — applyBusSections and the session grants both key on it.
QString neutralSectionName(const BusConfig &bus, CommsProtection protection)
{
    const QString stem = protection == CommsProtection::Protected
                             ? QStringLiteral("Protected message")
                             : QStringLiteral("Hidden message");
    for (int n = 1;; ++n) {
        const QString candidate = QStringLiteral("%1 %2").arg(stem).arg(n);
        bool taken = false;
        for (const CommsSection &s : bus.sections)
            taken = taken || s.name.compare(candidate, Qt::CaseInsensitive) == 0;
        if (!taken)
            return candidate;
    }
}

} // namespace

// Rebuilds a document Configuration from the device tables. Comms rows come
// back verbatim (start bit / bit length / DBC type / factor / offset map 1:1
// onto the signal record); only unreadable bus rates are inferred.
void mapFromDevice(const DeviceTables &tables, Configuration &config, QStringList *notes,
                   const QVector<ControlCanPayload> &busSetup)
{
    // Snapshot the two per-section facts the device does not carry, BEFORE
    // clearContent() throws the sections away: the section's own password and
    // the name the user gave it.
    //
    // Neither survives on its own. The wire field that used to hold the key is
    // `reserved[4]` as of 2.3.0 and reads back zero, and clearContent() resets
    // every BusConfig outright (b = BusConfig{}), so there is nothing left to
    // "merge over" — mapFromDevice rebuilds the sections from nothing. Without
    // this snapshot every messageKey came back kNoAccessKey, which means "no
    // password on this section", which means the comms dialog and the section
    // editor both open a Hidden section with no prompt at all and the untick
    // goes through. A Get was the shortest way past every per-section password
    // in the document.
    //
    // A LIST per identity, consumed in document order, because two sections on
    // one bus may legitimately share a CAN ID and direction (two receive rules
    // for 0x123). Pairing them in order is the only ordering information there
    // is, and it is the one that matches how the device enumerated them.
    //
    // THREE indexes over ONE list of records. The wire identity is tried first,
    // the bus-scoped NAME next and the bus-scoped PAYLOAD last; all three hold
    // positions into `priors`, and the records themselves carry the consumed flag
    // so a record reached through one index cannot be handed out again through
    // another. See sectionNameIdentity and sectionPayloadIdentity for why each
    // fallback exists and why both are scoped to the bus.
    QList<PriorSection> priors;
    QHash<QString, QList<int>> byIdentity;
    QHash<QString, QList<int>> byName;
    QHash<QString, QList<int>> byPayload;
    for (int b = 0; b < 3; ++b)
        for (const CommsSection &s : config.bus[b].sections) {
            priors.append(PriorSection{s.messageKey, s.name, false});
            const int at = priors.size() - 1;
            byIdentity[sectionWireIdentity(b, s)].append(at);
            if (!s.name.isEmpty())
                byName[sectionNameIdentity(b, s)].append(at);
            const QString payload = sectionPayloadIdentity(b, s);
            if (!payload.isEmpty())
                byPayload[payload].append(at);
        }
    // The first record for `key` that nothing has taken yet, or -1. Consumed
    // entries are dropped off the front as they are found rather than searched
    // past every time, so the pairing stays in document order.
    const auto takeFirstUnconsumed = [&priors](QHash<QString, QList<int>> &index,
                                               const QString &key) -> int {
        const auto it = index.find(key);
        if (it == index.end())
            return -1;
        while (!it->isEmpty()) {
            const int at = it->takeFirst();
            if (!priors[at].consumed) {
                priors[at].consumed = true;
                return at;
            }
        }
        return -1;
    };
    // clearContent(), NOT clear(): this replaces what the DEVICE knows and must
    // leave what the DOCUMENT knows alone. clear() wipes the access verifiers,
    // and a Get that did that left hasCommsPassword() false, commsRevealed()
    // true, and every protection tier in the document freely lowerable — a round
    // trip through the hardware was the shortest way past the untick rule. It
    // also kept the session's Edit Protected Comms grant, so the sections the
    // device just handed back stay concealed from a viewer who has not proved
    // anything.
    config.clearContent();
    // Bus modes and rates. The message and signal tables carry none of this, so
    // it comes from CMD_READ_CAN_SETUP — a separate read that older firmware
    // does not answer. Both outcomes are handled here rather than left to the
    // caller, because a Get that quietly invented a bus rate and a Get that
    // truthfully read one must not look the same to whatever comes next.
    if (busSetup.size() == 3) {
        for (int i = 0; i < 3; ++i) {
            const ControlCanPayload &s = busSetup[i];
            // mode: 0 = off, 1 = active, 2 = listen-only. The document models
            // only enabled/disabled, so listen-only reads back as enabled —
            // recording it as OFF would be worse, because a Send would then
            // silently stop a bus that is running.
            config.bus[i].enabled = s.mode != 0;
            // busRateKbpsFromHz, not /1000: 83,333 Hz is stored as 83 (GMLAN's
            // 83.333k — see busRateHz), and plain division happens to agree
            // today but would silently drift if the special case ever moved.
            config.bus[i].rateKbps = busRateKbpsFromHz(s.baud_rate);
            // A data rate at or below the nominal rate is not CAN FD; the
            // firmware writes 0 for classic, but be tolerant of either.
            config.bus[i].dataRateKbps =
                s.data_baud_rate > s.baud_rate ? busRateKbpsFromHz(s.data_baud_rate) : 0;
            config.bus[i].termination = s.termination != 0;
        }
        if (notes) {
            // Worth saying even on success: this is what the buses are running
            // NOW, which after an unsaved Send is not what the stored image
            // holds. A user comparing this against a file needs to know which
            // of the two they are looking at.
            bool anyListenOnly = false;
            for (const ControlCanPayload &s : busSetup)
                anyListenOnly = anyListenOnly || s.mode == 2;
            if (anyListenOnly)
                notes->append(QStringLiteral(
                    "One or more buses are running listen-only. This document records them "
                    "as enabled, because that is the closest it can express — sending it back "
                    "will put those buses into normal mode."));
        }
    } else {
        // Assume the firmware bring-up rates (CAN1 1M, CAN2 500k + FD 2M, CAN3
        // 500k) rather than the new-document 1M/off defaults, so a Send straight
        // after this Get does not silently re-rate a running bus.
        config.bus[0].rateKbps = 1000; config.bus[0].dataRateKbps = 0;
        config.bus[1].rateKbps = 500;  config.bus[1].dataRateKbps = 2000;
        config.bus[2].rateKbps = 500;  config.bus[2].dataRateKbps = 0;
        if (notes)
            notes->append(QStringLiteral(
                "This firmware cannot report its bus modes and rates, so the bring-up rates "
                "were assumed — check Connections > Communications before sending."));
    }
    // A device script comes back as BYTECODE, and bytecode does not decompile
    // into the Lua that produced it — names, structure and comments are gone by
    // the time it is 8-byte instructions. So a Get still cannot fill in a
    // scriptSource. What it CAN do is keep the compiled image, so that sending
    // this document back puts the same script on the unit byte for byte.
    //
    // That replaces the note this used to raise, which said the script could not
    // be retrieved and that a Send would remove it. Both halves of that sentence
    // are now false, and the behaviour it described — a Get producing a
    // scriptless document that silently stripped a running script on the next
    // Send — is the footgun this retention exists to remove.
    //
    // The image is VALIDATED here, and nothing is kept unless it passes. A
    // device that answered with a short or garbled image must leave this
    // document with no bytecode at all rather than with an image that fails at
    // the next Send, when CLEAR_CONFIG has already erased the unit.
    const RetainedScript retained = scriptImageFromChunks(tables.scriptChunks);
    config.setScriptBytecode(retained.image);
    if (notes && retained.present) {
        if (!retained.image.isEmpty()) {
            // Four things, because each of them is a decision the user is about
            // to make: the script came back, it goes back unchanged, there is no
            // Lua to edit, and there IS somewhere to look at it. The last one is
            // the difference between "you have a script you cannot see" and a
            // listing one menu away — Calculations > Device Script opens on the
            // instructions when the document holds an image.
            notes->append(QStringLiteral(
                "This device is running a script, and its compiled bytecode has been kept in "
                "this document — sending this configuration back restores the same script "
                "byte for byte. There is no Lua source for it, and bytecode does not turn "
                "back into source, so this document and the unit are now the only copies. "
                "Calculations > Device Script lists the instructions; writing a script there "
                "REPLACES the compiled one, and asks first."));
        } else {
            notes->append(
                QStringLiteral(
                    "This device holds a script, but the bytecode it returned is not a valid "
                    "script image (%1), so none has been kept. Sending this configuration back "
                    "would REMOVE the script from the device — open the file the script was "
                    "written in instead, and re-read the device before trusting this document.")
                    .arg(retained.error));
        }
    }

    QList<Channel> userChannels;
    QSet<QString> channelNamesSeen;
    QHash<QString, int> channelIndexByName; // lower-case name -> userChannels index

    // Unified message table: direction comes from the MSGFLAG_TRANSMIT flag.
    QHash<int, int> msgSection; // message idx -> section index within its bus
    for (int m = 0; m < tables.messages.size(); ++m) {
        const CanMessageConfig &msg = tables.messages[m];
        // Get reads the table at its full capacity and the firmware zero-fills
        // past the used prefix, so the empties must be dropped — but ACTIVE is
        // not the emptiness test. mapToDevice deliberately uploads a disabled
        // bus's messages WITHOUT the flag, and skipping on it here destroyed
        // exactly those sections on the next Get. src_bus is 1..3 in every
        // record either side ever writes and 0 only in the zero-fill, so it is
        // what tells a deactivated record from no record at all.
        if (msg.src_bus < 1 || msg.src_bus > 3)
            continue;
        const bool active = msg.flags & MSGFLAG_ACTIVE;
        const bool transmit = msg.flags & MSGFLAG_TRANSMIT;
        const int busIdx = qBound(1, int(msg.src_bus), 3) - 1;
        CommsSection s;
        s.device = transmit ? SectionDevice::TransmitMessage : SectionDevice::ReceiveMessage;
        s.extended = msg.flags & MSGFLAG_EXTENDED;
        s.fd = msg.flags & MSGFLAG_FD;
        // Restore the tier rather than letting it fall to the CommsSection
        // default. A Get that dropped it would launder the message: a Hidden
        // message would land in the document as ordinary, with its full detail
        // on display, and the next Send would write it back to the hardware
        // unmarked. This is the entire reason the bits are on the wire — the
        // device enforces nothing with them.
        //
        // A 2.2.x image decodes correctly without a translation table: 0x80 was
        // its "Read-only", which CONCEALED, and lands on Hidden; 0xC0 was
        // "Protect Communication" and lands on Protected. See the encoding table
        // in wire_structs.h for why that fixed the values.
        s.protection = commsProtectionFromWire(msg.flags);
        // messageKey is deliberately NOT taken from the record, and there is no
        // longer anything there to take: 2.3.0 retired the device's per-message
        // key and the field is `reserved[4]`, written and read back as zero in
        // both directions. The section's password is a DOCUMENT secret now, and
        // it is restored — together with the user's own name for this section —
        // by the reconciliation pass at the end of this function, from the
        // snapshot taken before clearContent(). It is NOT restored by anything
        // clearContent() does: that function empties the buses, so a section
        // reaching this line always starts at kNoAccessKey.
        s.baseAddress = msg.can_id;
        // dlc 0 is a first-class length (DLC-0 heartbeats) — no ":8" fallback.
        s.messageLengthBytes = qMin(int(msg.dlc), 64);
        // Signal-less messages keep the Normal default; the first signal's
        // byte_order overrides this below for messages that have channels.
        s.alignment = SectionAlignment::Normal;
        if (transmit) {
            // Triggered survives a Get now. It used to be forced true here
            // because the flag reached the device nowhere and a read-back had
            // nothing to consult; the message record carries it, so the section
            // comes home the way it went out. The condition itself is resolved
            // from an index to a channel name at the end of this function, once
            // the condition rows exist.
            s.cyclic = !(msg.tx_trigger_flags & TXTRIG_ENABLED);
            s.transmitPeriodMs = msg.period_ms; // authoritative — exact re-send
            s.transmitRateHz = qBound(1, msg.period_ms ? 1000 / msg.period_ms : 50, 200);
            s.compoundTxMode = (msg.flags & MSGFLAG_TX_SEQUENTIAL) ? CompoundTxMode::Sequential
                                                                   : CompoundTxMode::Batch;
            s.name = QStringLiteral("Transmit 0x%1").arg(QString::number(msg.can_id, 16).toUpper());
        } else {
            s.routeEnable = msg.flags & MSGFLAG_ROUTING;
            s.routeBusMask = msg.route_bus_mask & 0x7;
            s.name = QStringLiteral("Receive 0x%1").arg(QString::number(msg.can_id, 16).toUpper());
            // period_ms carries the receive timeout for receive messages; a
            // non-zero value means "Default value on timeout" was enabled.
            s.defaultValueOnTimeout = msg.period_ms > 0;
            if (msg.period_ms > 0)
                s.receiveTimeoutMs = msg.period_ms;
            // Gateway destinations are evidently running too — keep them on so
            // the next Send doesn't stop the bus the routed frames go to. A
            // deactivated message routes nothing, so it is no such evidence.
            if (active && s.routeEnable)
                for (int t = 0; t < 3; ++t)
                    if (s.routeBusMask & (1 << t))
                        config.bus[t].enabled = true;
        }
        // The wire tables don't carry bus modes; a bus with active messages is
        // evidently running, so don't leave it at the Off document default
        // (a Send straight after this Get would shut the device's bus down).
        // A deactivated message says the opposite — its bus was Off when it
        // went up — so it keeps its section but casts no vote on the mode.
        if (active)
            config.bus[busIdx].enabled = true;
        config.bus[busIdx].sections.append(s);
        msgSection.insert(m, config.bus[busIdx].sections.size() - 1);
    }

    // Every slot the device says it publishes one of its own values into. Built
    // once rather than re-scanned per signal, and from the DEVICE's answer
    // rather than from the catalogue, so a slot the firmware publishes into is
    // recognised even if this build has no catalogue entry for that id — which
    // is what a Get from newer firmware looks like.
    QSet<int> devicePublishedSlots;
    for (int id = 0; id < DEVCH_COUNT; ++id) {
        const quint16 slot = tables.deviceChannels.signal_idx[id];
        if (slot < MAX_SIGNALS)
            devicePublishedSlots.insert(int(slot));
    }

    QHash<int, QString> signalNames;
    QHash<int, quint8> messageByteOrder; // msg idx -> byte_order of its first signal
    for (int i = 0; i < tables.signalConfigs.size(); ++i) {
        const CanSignalConfig &sig = tables.signalConfigs[i];
        if (!sigIsActive(sig))
            continue;
        // Declares an identifier and carries no value (SIG_FLAG_SELECTOR_ONLY).
        // It must still reach the section rebuild at the bottom of this loop —
        // that is the whole point of it — but it names no channel, so it takes
        // no catalogue entry and no signalNames slot. Its blank label would
        // otherwise become "Signal 12" and manufacture a channel for something
        // that is not one.
        const bool selectorOnly = sigSelectorOnly(sig);
        QString name;
        if (!selectorOnly) {
            name = QString::fromUtf8(sig.label, qstrnlen(sig.label, sizeof(sig.label))).trimmed();
            if (name.isEmpty())
                name = QStringLiteral("Signal %1").arg(i);
            signalNames.insert(i, name);
        }

        // Infer the storage data type from the field width + signedness so
        // reconstructed channels open cleanly in Edit Custom Channel.
        // Virtual value slots (math/counter/timer/integrator outputs, transmit sources)
        // have value_type 0 — an unset marker, not a real type — so they get
        // no inference; a later typed duplicate (the transmit field signal)
        // backfills the type.
        // Size the reconstructed channel from the value RANGE the device holds
        // for the slot, exactly as DBC import does — never from the raw field
        // width. A width guess contradicts the range it is paired with (an
        // 8-bit field with offset -40 reads back min_val -40, which no u8 can
        // hold), and since Get replaces the whole user catalog that guess would
        // overwrite a correctly-sized imported channel and re-introduce the
        // truncation on the next edit.
        const auto inferDataType = [](const CanSignalConfig &sig) -> QString {
            if (sigValueType(sig) == 0)
                return {};
            if (sigValueType(sig) & 0x20)
                return QStringLiteral("float");
            // Receive signals carry their scaling in factor/offset and store
            // decimal_places 0; a generated slot carries its own precision.
            const int decimals = qBound(0, int(sigDecimalPlaces(sig)), 8);
            return storageTypeForRange(sig.min_val, sig.max_val, decimals);
        };
        const QString nameKey = name.toLower();
        // A device channel is built into the catalogue, so reconstructing one as
        // a user channel would produce a duplicate the user can edit and delete
        // — and whose definition would then quietly diverge from the firmware's.
        // References to it by name keep working either way; the slot is still in
        // signalNames above. Recognised by the slot the device reports, not by
        // the label, so a user channel that merely shares the name is unaffected.
        //
        // Only the CATALOGUE entry is withheld, though — the signal itself must
        // keep walking. This used to `continue`, which also skipped the
        // section-row rebuild below, and a transmit row sending "Device CAN1
        // Bus Off" simply vanished on Get: its section came back with no rows
        // and the next Send transmitted zeros where the diagnostic had been.
        // The published slots themselves carry no message binding, so the
        // SIG_MSG_NONE test below still retires them exactly as the continue
        // did.
        if (selectorOnly) {
            // Names no channel, so it takes no catalogue entry — but it keeps
            // walking to the section rebuild below, which is what it is for.
        } else if (devicePublishedSlots.contains(i) || ChannelCatalog::isDeviceChannel(name)) {
            channelNamesSeen.insert(nameKey);
        } else if (!channelNamesSeen.contains(nameKey)) {
            channelNamesSeen.insert(nameKey);
            Channel ch;
            ch.name = name;
            ch.baseResolution = 1.0;
            ch.minValue = sig.min_val;
            ch.maxValue = sig.max_val;
            ch.dataType = inferDataType(sig);
            ch.userDefined = true;
            userChannels.append(ch);
            channelIndexByName.insert(nameKey, userChannels.size() - 1);
        } else if (channelIndexByName.contains(nameKey)) {
            Channel &existing = userChannels[channelIndexByName.value(nameKey)];
            if (existing.dataType.isEmpty())
                existing.dataType = inferDataType(sig);
        }

        // Locate the section this signal belongs to (receive or transmit).
        if (sigMsgIdx(sig) == SIG_MSG_NONE || !msgSection.contains(sigMsgIdx(sig)))
            continue; // virtual signal (math/counter/timer/integrator output, condition target)
        const CanMessageConfig &parentMsg = tables.messages[sigMsgIdx(sig)];
        const int busIdx = qBound(1, int(parentMsg.src_bus), 3) - 1;
        CommsSection *section = &config.bus[busIdx].sections[msgSection.value(sigMsgIdx(sig))];
        const int alignmentKey = sigMsgIdx(sig);

        // One section holds one alignment; if the device mixes byte orders in
        // a single message we keep the first and skip conflicting signals.
        if (!messageByteOrder.contains(alignmentKey)) {
            messageByteOrder.insert(alignmentKey, sigByteOrder(sig));
            section->alignment = sigByteOrder(sig) == 0 ? SectionAlignment::WordSwap
                                                     : SectionAlignment::Normal;
        } else if (messageByteOrder.value(alignmentKey) != sigByteOrder(sig)) {
            if (notes)
                notes->append(QStringLiteral(
                    "%1: mixes byte orders within one message — signal skipped").arg(name));
            continue;
        }

        // Start bit / length / factor / offset are stored verbatim on the
        // device, so the DBC row comes back exactly as it was sent.
        CommsChannelRow row;
        row.channelName = name;
        row.startBit = sigStartBit(sig);
        row.bitLength = sigBitLength(sig);
        row.defaultValue = sig.default_value;
        if (sigValueType(sig) & 0x20) {
            // Any IEEE-754 type (0x34 FLOAT, and 0x38 DOUBLE from a foreign
            // config) collapses to the model's single 32-bit float form so the
            // dialog and re-send stay self-consistent.
            row.dbcType = int(DbcType::IEEE754);
            row.bitLength = 32;
        } else if (sigValueType(sig) & 0x10) {
            row.dbcType = int(DbcType::Signed);
        } else {
            row.dbcType = int(DbcType::Unsigned);
        }
        row.dbcFactor = sig.factor;
        row.dbcOffset = sig.offset;
        // Inverted back out of the wire's polarity. A receive row's bit is
        // always clear, so this reads true for it, which is what receive does.
        row.clampToRange = !sigTxWrap(sig);

        // v8/v10: a signal with a non-zero mux mask is a gated compound
        // sub-message channel (receive OR transmit); group it under an identifier
        // keyed by (offset, id, mask). Ungated signals (mask 0) are the section's
        // always-present rows.
        if (sig.mux_mask == 0) {
            // A selector-only signal with no mask declares nothing — it cannot
            // name a variant — and appending it here would put a channel-less
            // row in the section. Only a foreign or hand-built table can
            // produce one; drop it rather than reconstruct a row from it.
            if (selectorOnly)
                continue;
            section->rows.append(row);
        } else {
            section->compound = true;
            CompoundIdentifier *ident = nullptr;
            for (CompoundIdentifier &ci : section->identifiers)
                if (ci.byteOffset == int(sigMuxByteOffset(sig)) && ci.id == sig.mux_id
                    && ci.idMask == sig.mux_mask) {
                    ident = &ci;
                    break;
                }
            if (!ident) {
                CompoundIdentifier ci;
                ci.byteOffset = int(sigMuxByteOffset(sig));
                ci.id = sig.mux_id;
                ci.idMask = sig.mux_mask;
                ci.configured = true;
                section->identifiers.append(ci);
                ident = &section->identifiers.last();
            }
            // A selector-only signal has just done its whole job: the identifier
            // above exists again. It carries no channel, so appending a row for
            // it would invent one — and the next Send would emit a real signal
            // where the author put nothing.
            if (!selectorOnly)
                ident->rows.append(row);
        }
    }

    // A device/foreign table may mix ungated (mux_mask 0) and gated signals in
    // one compound message; the ungated ones land in section.rows above. Fold
    // them into the identifiers so they stay visible and survive a re-Send.
    for (int b = 0; b < 3; ++b)
        for (CommsSection &s : config.bus[b].sections)
            s.normalizeCompound();

    // Transmit CRC8 rules: each ACTIVE record flips the section rebuilt for its
    // message into a TransmitCrc8 and restores the recipe. Runs after the
    // signal walk because the CRC channel's name lives in the signal table
    // (signalNames), and before the reconciliation pass because the wire
    // identity there includes the device kind — a document TransmitCrc8 section
    // must be matched as one, not as the plain transmit it briefly was above.
    for (const Crc8Config &cc : tables.crc8) {
        // ACTIVE is the emptiness test here, and unlike the message and relay
        // walks that is honest rather than lossy: a zero-filled slot has flags 0
        // and msg_idx 0 — a VALID message index — and this table has no src_bus
        // to consult instead. There is no inactive-but-real record to lose,
        // because the mapper above always writes ACTIVE (the stamped message's
        // own ACTIVE flag carries the Off-bus state), so !ACTIVE can only be
        // the zero-fill past the used prefix.
        if (!(cc.flags & CRC8FLAG_ACTIVE))
            continue;
        // A rule whose message record does not exist binds to nothing — a
        // foreign or torn table. Nothing to restore it onto.
        if (!msgSection.contains(cc.msg_idx))
            continue;
        const CanMessageConfig &parentMsg = tables.messages[cc.msg_idx];
        const int busIdx = qBound(1, int(parentMsg.src_bus), 3) - 1;
        CommsSection &s = config.bus[busIdx].sections[msgSection.value(cc.msg_idx)];
        // Only a transmit message can carry a stamp — the composer is the thing
        // that runs the rule. A rule aimed at a receive record is a foreign
        // table's inconsistency, and flipping the section would turn a listener
        // into a transmitter; say so and leave it alone.
        if (!s.isTransmit()) {
            if (notes)
                notes->append(QStringLiteral(
                    "%1: a CRC8 rule points at this receive message — ignored "
                    "(only transmit messages are stamped)").arg(s.name));
            continue;
        }
        s.device = SectionDevice::TransmitCrc8;
        s.crcByteLocation = qBound(0, int(cc.byte_location), 7);
        s.crcPolynomial = cc.polynomial;
        s.crcInitValue = cc.init_value;
        s.crcFinalXor = cc.final_xor;
        s.crcRefIn = cc.flags & CRC8FLAG_REF_IN;
        s.crcRefOut = cc.flags & CRC8FLAG_REF_OUT;
        s.crcElements.clear();
        // 0 restores as an empty element list — the same spelling mapToDevice
        // gives it, so an element-less recipe survives a Get unchanged.
        const int n = qBound(0, int(cc.element_count), CRC8_MAX_ELEMENTS);
        for (int e = 0; e < n; ++e) {
            CommsSection::CrcElement el;
            el.type = cc.elem_type[e] == CRC8_ELEM_ID    ? CommsSection::CrcElement::Id
                      : cc.elem_type[e] == CRC8_ELEM_RAW ? CommsSection::CrcElement::Raw
                                                         : CommsSection::CrcElement::Data;
            el.value = cc.elem_value[e];
            s.crcElements.append(el);
        }
        // SIG_MSG_NONE means "stamp only, publish nowhere". The mapper refuses
        // to WRITE that (an empty CRC channel is a mapping error), but the
        // firmware accepts it, so a foreign table can hold it — read it back as
        // the empty channel it is rather than inventing a name.
        s.crcChannel = cc.dest_signal_idx == SIG_MSG_NONE
                           ? QString()
                           : signalNames.value(cc.dest_signal_idx);
    }

    for (const MathConfig &mc : tables.math) {
        if (!mc.is_active)
            continue;
        MathRow m;
        m.op = mc.op;
        m.aIsChannel = mc.input_a_type == 1;
        m.aChannel = signalNames.value(mc.input_a_idx);
        m.aConst = mc.input_a_const;
        m.bIsChannel = mc.input_b_type == 1;
        m.bChannel = signalNames.value(mc.input_b_idx);
        m.bConst = mc.input_b_const;
        // C is only meaningful on arity-3 ops. For anything else the bytes are
        // whatever the slot carried — a record written before the 24-byte
        // format has 0xFF flash pad there — so normalise to an unused const 0
        // instead of decoding garbage.
        if (mathOpArity(mc.op) == 3) {
            m.cIsChannel = mc.input_c_type == 1;
            if (m.cIsChannel)
                m.cChannel = signalNames.value(mathInputCIdx(mc));
            else
                m.cConst = mathInputCConst(mc);
        }
        m.destChannel = signalNames.value(mc.dest_signal_idx);
        config.mathRows.append(m);
    }

    // Wire index -> the condition's output channel, for the Triggered-transmit
    // resolve below. Keyed on the position in the DEVICE's table, not on the
    // position in conditionRows, because the two can differ: an inactive record
    // is skipped here but still occupies its index on the device.
    QHash<int, QString> conditionOutputByIndex;
    for (int ci = 0; ci < tables.conditions.size(); ++ci) {
        const ConditionConfig &cc = tables.conditions[ci];
        if (!(cc.flags & CONDFLAG_ACTIVE))
            continue;
        ConditionRow c;
        c.mode = (cc.flags & CONDFLAG_SETRESET) ? ConditionMode::SetReset
                                                : ConditionMode::Momentary;
        c.latchHz = qBound(1, int(cc.latch_hz), int(COND_LATCH_MAX_HZ));

        // One expression back out of its three wire slots. A device record
        // always carries COND_MAX_TERMS of them; only the first `count` are
        // meaningful, and the rest are whatever the writer left there.
        const auto readExpr = [&](const ConditionTerm *src, int count, quint8 joinerBits,
                                  QList<ConditionTermRow> *terms, QList<int> *joiners) {
            terms->clear();
            joiners->clear();
            const int n = qBound(0, count, COND_MAX_TERMS);
            for (int t = 0; t < n; ++t) {
                const ConditionTerm &wt = src[t];
                ConditionTermRow tr;
                tr.op = wt.op;
                if (condOpIsMessage(wt.op)) {
                    // input_a is a message index. Turn it back into the (bus,
                    // name) pair the document holds, through the same section
                    // map the CRC8 and trigger paths use. An index naming
                    // nothing leaves the term blank rather than carrying a
                    // number no editor could show.
                    const int mi = int(wt.input_a_signal_idx);
                    if (msgSection.contains(mi) && mi < tables.messages.size()) {
                        const int busIdx = int(tables.messages[mi].src_bus) - 1;
                        if (busIdx >= 0 && busIdx <= 2) {
                            const CommsSection &ms =
                                config.bus[busIdx].sections[msgSection.value(mi)];
                            tr.aMessageBus = busIdx + 1;
                            tr.aMessage = ms.name;
                        }
                    }
                } else {
                    tr.aChannel = signalNames.value(wt.input_a_signal_idx);
                    tr.bIsChannel = wt.input_b_type == 1;
                    tr.bChannel = signalNames.value(wt.b.input_b_idx);
                    tr.bConst = wt.b.input_b_const;
                }
                terms->append(tr);
                if (t + 1 < n)
                    joiners->append(((joinerBits >> t) & 1u) ? int(COND_JOIN_OR)
                                                             : int(COND_JOIN_AND));
            }
            if (terms->isEmpty())
                terms->append(ConditionTermRow{});
        };

        readExpr(cc.set_terms, cc.set_count, cc.set_joiners, &c.setTerms, &c.setJoiners);
        // A Momentary sends reset_count 0, so this reads back as one empty
        // comparison — which is what a Momentary's Reset half looks like in a
        // freshly created row too. Nothing is lost that the device ever held.
        readExpr(cc.reset_terms, cc.reset_count, cc.reset_joiners, &c.resetTerms,
                 &c.resetJoiners);

        c.outputChannel = signalNames.value(cc.dest_signal_idx);
        conditionOutputByIndex.insert(ci, c.outputChannel);
        config.conditionRows.append(c);
    }

    // Triggered transmit, the other direction: turn each message's condition
    // INDEX back into the output-channel name the document holds.
    //
    // A message whose index names nothing — a device configured by some other
    // tool, or a condition record that came back inactive — keeps cyclic rather
    // than acquiring a dangling reference. The Get is describing a device, and a
    // trigger this build cannot name is one it cannot honestly show.
    for (const CanMessageConfig &msg : tables.messages) {
        if (!(msg.flags & MSGFLAG_TRANSMIT) || !(msg.tx_trigger_flags & TXTRIG_ENABLED))
            continue;
        const int m = int(&msg - tables.messages.constData());
        if (!msgSection.contains(m))
            continue;
        const int busIdx = int(msg.src_bus) - 1;
        if (busIdx < 0 || busIdx > 2)
            continue;
        const QString outputChannel = conditionOutputByIndex.value(int(msg.tx_trigger_cond));
        CommsSection &s = config.bus[busIdx].sections[msgSection.value(m)];
        if (outputChannel.isEmpty()) {
            s.cyclic = true;
            continue;
        }
        s.transmitCondition = outputChannel;
    }

    auto boolName = [&](quint16 idx) {
        return (idx == SIG_MSG_NONE) ? QString() : signalNames.value(idx);
    };

    for (const CounterConfig &cfg : tables.counters) {
        if (!(cfg.flags & COUNTERFLAG_ACTIVE))
            continue;
        CounterRow c;
        const bool isRate = (cfg.mode == COUNTER_MODE_RATE);
        c.outputChannel = signalNames.value(cfg.dest_signal_idx);
        c.mode = isRate ? COUNTER_MODE_RATE
                        : (cfg.mode == COUNTER_MODE_FOLLOW) ? COUNTER_MODE_FOLLOW
                                                            : COUNTER_MODE_UPDOWN;
        c.upChannel = boolName(cfg.up_signal_idx);
        c.downChannel = boolName(cfg.down_signal_idx);
        c.followChannel = boolName(cfg.follow_signal_idx);
        c.resetChannel = boolName(cfg.reset_signal_idx);
        c.enableChannel = boolName(cfg.enable_signal_idx);
        c.minValue = cfg.min_value;
        c.maxValue = cfg.max_value;
        c.resetValue = cfg.reset_value;
        c.step = cfg.step;
        c.rollAtLimits = cfg.flags & COUNTERFLAG_ROLL;
        c.preserveValue = cfg.flags & COUNTERFLAG_PRESERVE;
        // Only trust the rate fields in rate mode. Outside it the mapper writes
        // rate_hz 0, which is not a rate the editor can show — fall back to the
        // row default rather than putting a 0 Hz counter in front of the user.
        c.rateHz = isRate ? qBound(1, int(cfg.rate_hz), COUNTER_MAX_HZ) : 1;
        c.rateCountDown = isRate && (cfg.flags & COUNTERFLAG_RATE_DOWN);
        config.counterRows.append(c);
    }

    for (const TimerConfig &cfg : tables.timers) {
        if (!(cfg.flags & TIMERFLAG_ACTIVE))
            continue;
        TimerRow t;
        t.outputChannel = signalNames.value(cfg.dest_signal_idx);
        t.startChannel = boolName(cfg.start_signal_idx);
        t.stopChannel = boolName(cfg.stop_signal_idx);
        t.countDown = cfg.flags & TIMERFLAG_COUNTDOWN;
        t.rollover = cfg.flags & TIMERFLAG_ROLLOVER;
        t.limitValue = cfg.limit_value;
        t.setOnStart = cfg.flags & TIMERFLAG_SET_ON_START;
        t.startValue = cfg.start_value;
        t.setOnStop = cfg.flags & TIMERFLAG_SET_ON_STOP;
        t.stopValue = cfg.stop_value;
        config.timerRows.append(t);
    }

    for (const IntegratorConfig &cfg : tables.integrators) {
        if (!(cfg.flags & INTEGFLAG_ACTIVE))
            continue;
        IntegratorRow g;
        g.outputChannel = signalNames.value(cfg.dest_signal_idx);
        g.inputIsChannel = !(cfg.flags & INTEGFLAG_CONST_INPUT);
        if (g.inputIsChannel)
            g.inputChannel = signalNames.value(cfg.input_signal_idx);
        else
            g.inputValue = cfg.input_const;
        g.rateHz = qBound(1, int(cfg.rate_hz), INTEGRATOR_MAX_HZ);
        g.countDown = cfg.flags & INTEGFLAG_COUNT_DOWN;
        g.startValue = cfg.start_value;
        g.enableChannel = boolName(cfg.enable_signal_idx);
        g.resetChannel = boolName(cfg.reset_signal_idx);
        g.resetValue = cfg.reset_value;
        g.minValue = cfg.min_value;
        g.maxValue = cfg.max_value;
        g.preserveValue = cfg.flags & INTEGFLAG_PRESERVE;
        config.integratorRows.append(g);
    }

    for (const ConstantConfig &cc : tables.constants) {
        if (!cc.is_active)
            continue;
        ConstantRow k;
        k.name = signalNames.value(cc.dest_signal_idx);
        k.value = cc.value;
        if (cc.dest_signal_idx < tables.signalConfigs.size()) {
            const CanSignalConfig &sig = tables.signalConfigs[cc.dest_signal_idx];
            k.dataType = dataTypeForValueType(sigValueType(sig));
            k.decimalPlaces = qBound(0, int(sigDecimalPlaces(sig)), 8);
        }
        config.constantRows.append(k);
    }

    // Lookup tables. Axis + output signal indices map back to channel names
    // via signalNames; the output's data type / decimals come from its signal
    // slot (typed when the table was emitted).
    const auto typeFromSlot = [&](quint16 idx, QString *dataType, int *decimals) {
        if (idx < tables.signalConfigs.size()) {
            const CanSignalConfig &sig = tables.signalConfigs[idx];
            *dataType = dataTypeForValueType(sigValueType(sig));
            *decimals = qBound(0, int(sigDecimalPlaces(sig)), 8);
        }
    };
    // A 2x16 table is only reconstructed where BOTH records are present — the
    // device likewise ignores an index missing from either table.
    const int tables2x16 = qMin(tables.tables2x16Def.size(), tables.tables2x16Out.size());
    for (int i = 0; i < tables2x16; ++i) {
        const Table2x16Def &def = tables.tables2x16Def[i];
        const Table2x16Out &out = tables.tables2x16Out[i];
        if (!(def.flags & TABLEFLAG_ACTIVE))
            continue;
        Table2x16Row t;
        t.outputChannel = signalNames.value(def.dest_signal_idx);
        t.xChannel = signalNames.value(def.x_signal_idx);
        t.xInterp = def.flags & TABLEFLAG_X_INTERP;
        typeFromSlot(def.dest_signal_idx, &t.dataType, &t.decimalPlaces);
        const int n = qBound(0, int(def.x_count), TABLE_2X16_SITES);
        for (int k = 0; k < n; ++k) {
            t.xSites.append(def.x_sites[k]);
            t.outputs.append(out.outputs[k]);
        }
        config.table2x16Rows.append(t);
    }
    // An 8x8 table is only reconstructed where its Def AND all eight of its rows
    // came back — the device likewise refuses to evaluate table t until it holds
    // a Def at t and at least (t+1)*8 rows, so this mirrors the guard rather
    // than inventing one. Integer division is what expresses it: 20 rows read
    // back means two complete tables and half of a third, and the half is
    // dropped.
    const int tables8x8 =
        qMin(int(tables.tables8x8Def.size()), int(tables.tables8x8Row.size()) / TABLE_8X8_SITES);
    for (int i = 0; i < tables8x8; ++i) {
        const Table8x8Def &def = tables.tables8x8Def[i];
        if (!(def.flags & TABLEFLAG_ACTIVE))
            continue;
        Table8x8Row t;
        t.outputChannel = signalNames.value(def.dest_signal_idx);
        t.xChannel = signalNames.value(def.x_signal_idx);
        t.yChannel = signalNames.value(def.y_signal_idx);
        t.xInterp = def.flags & TABLEFLAG_X_INTERP;
        t.yInterp = def.flags & TABLEFLAG_Y_INTERP;
        typeFromSlot(def.dest_signal_idx, &t.dataType, &t.decimalPlaces);
        const int nx = qBound(0, int(def.x_count), TABLE_8X8_SITES);
        const int ny = qBound(0, int(def.y_count), TABLE_8X8_SITES);
        for (int k = 0; k < nx; ++k)
            t.xSites.append(def.x_sites[k]);
        for (int k = 0; k < ny; ++k)
            t.ySites.append(def.y_sites[k]);
        // Grid row y is its own record at i*TABLE_8X8_SITES + y; the model packs
        // the used cells row-major over nx.
        for (int y = 0; y < ny; ++y) {
            const Table8x8GridRow &row = tables.tables8x8Row[i * TABLE_8X8_SITES + y];
            for (int x = 0; x < nx; ++x)
                t.outputs.append(row.v[x]);
        }
        config.table8x8Rows.append(t);
    }

    // v11: message relays reconstruct as MessageRelay sections on their src bus.
    for (const RelayConfig &rl : tables.relays) {
        // The same emptiness test as the message walk, for the same reason: a
        // relay uploaded while its bus was Off carries no ACTIVE flag yet is a
        // real record, and only the zero-fill past the used prefix has
        // src_bus 0.
        if (rl.src_bus < 1 || rl.src_bus > 3)
            continue;
        const bool active = rl.flags & RELAYFLAG_ACTIVE;
        const int busIdx = qBound(1, int(rl.src_bus), 3) - 1;
        CommsSection s;
        s.device = SectionDevice::MessageRelay;
        s.extended = rl.flags & RELAYFLAG_EXTENDED;
        s.relayInvert = rl.flags & RELAYFLAG_INVERT;
        // Same top two bits, same decode as a message. Dropping it here would
        // reopen the relay gap from the other end.
        s.protection = commsProtectionFromWire(rl.flags);
        s.baseAddress = rl.address;
        s.relayBitmask = rl.bitmask;
        s.routeBusMask = rl.forward_bus_mask & 0x7;
        s.name = QStringLiteral("Relay 0x%1").arg(QString::number(rl.address, 16).toUpper());
        // An active relay means its bus is running; keep it (and its forward
        // targets) on so the next Send doesn't stop them. A deactivated one
        // says its bus was Off, and must not switch on the very buses whose
        // mode its missing flag records.
        if (active) {
            config.bus[busIdx].enabled = true;
            for (int t = 0; t < 3; ++t)
                if (s.routeBusMask & (1 << t))
                    config.bus[t].enabled = true;
        }
        config.bus[busIdx].sections.append(s);
    }

    // ---- reconcile the rebuilt sections with what the document already knew ----
    //
    // Runs last, after messages AND relays are appended, so both kinds get the
    // same treatment. Two things happen here, and the second only because the
    // first can come up empty:
    //
    //   1. A section the document already had, matched by its identity ON THE
    //      WIRE, gets its messageKey and its user-given name back. A section the
    //      device no longer has simply drops, exactly as before — a Get still
    //      says what the hardware holds.
    //   2. A section the document has never seen keeps the generated name, EXCEPT
    //      at the tiers that conceal. "Receive 0x640" spells out the CAN ID that
    //      Hidden and Protected exist to withhold, and the sections list would
    //      print it beside "(hidden)".
    //
    // The identity match runs FIRST and the weaker handles only where it comes up
    // empty, and that order is not a preference. mapFromDevice REGENERATES names
    // ("Receive 0x640") before this loop, so a name-first pass would pair a
    // rebuilt section against a generated name that has nothing to do with the
    // user's — and would mis-pair two sections whose names the user swapped.
    // Identity first is: "the same message, moved" is recognised only after "the
    // same message, where it was" has failed.
    //
    // TWO PASSES over the sections, not one, and this is the trap. Run as a
    // single pass, a section whose ID had NOT moved could reach the name index
    // ahead of the section that owned that name and take its record — the very
    // mis-pairing the identity-first rule exists to prevent. Every identity match
    // in the whole document has to be settled before any name match is offered
    // one. The second pass then tries its three weaker handles IN TURN PER
    // SECTION rather than as three more passes, because they are ranked by how
    // much the user chose them, not by how much they cost: a section that answers
    // to its own name has said more about itself than one that merely shares a
    // payload, so there is nothing a later section could learn that would make an
    // earlier one's own-name match wrong.
    // (bus, row) rather than pointers: the second pass renames sections through
    // these, and a pointer into a QList is one detach away from being the wrong
    // section's.
    QList<QPair<int, int>> unmatched;
    for (int b = 0; b < 3; ++b)
        for (int row = 0; row < config.bus[b].sections.size(); ++row) {
            CommsSection &s = config.bus[b].sections[row];
            const int at = takeFirstUnconsumed(byIdentity, sectionWireIdentity(b, s));
            if (at < 0) {
                unmatched.append({b, row});
                continue;
            }
            s.messageKey = priors[at].messageKey;
            // An empty prior name would be a section the user never named; the
            // generated one is better than nothing, and falls through to the
            // concealing-name rule below.
            if (!priors[at].name.isEmpty())
                s.name = priors[at].name;
            else if (s.protection >= CommsProtection::Hidden)
                s.name = neutralSectionName(config.bus[b], s.protection);
        }
    for (const auto &pending : unmatched) {
        const int b = pending.first;
        CommsSection &s = config.bus[b].sections[pending.second];
        // NOTHING IS ASSIGNED UNTIL EVERY LOOKUP HAS FAILED, and that ordering is
        // the fix for a bug this loop used to create rather than solve. It named
        // a concealing section FIRST — s.name = "Hidden message 1" — and then
        // asked the name index about the name it had just invented. The comment
        // that justified it claimed the document's copy "is under Hidden message
        // 1 too", which is true only of a section the user never named. Name one
        // "Turbo pressure", renumber it, Get: the lookup compared the generated
        // name against the typed one, found nothing, and the section came back
        // with no key, no user name and no way to open or unmark it ever again.
        //
        // The neutral name is computed here only because lookup (c) needs it as a
        // key. Computing it is free of side effects; s.name is written once, at
        // the bottom, after the answer is known.
        const QString neutral = s.protection >= CommsProtection::Hidden
                                    ? neutralSectionName(config.bus[b], s.protection)
                                    : QString();
        // (a) THE NAME THIS SECTION IS ACTUALLY CARRYING. mapFromDevice
        // regenerated it from the CAN ID ("Receive 0x640"), so this is the case of
        // a section the document knows under the name a previous Get gave it and
        // has since renumbered: the document still holds "Receive 0x640" while the
        // device still holds 0x640.
        int at = takeFirstUnconsumed(byName, sectionNameIdentity(b, s));
        // (b) THE PAYLOAD, which is what survives when the user has edited both
        // handles — renamed the message AND renumbered it. Neither name means
        // anything to the other side by then; the channels the message carries
        // still do. See sectionPayloadIdentity for why this is scoped the way it
        // is and why a channel-less section is excluded from it.
        if (at < 0) {
            const QString payload = sectionPayloadIdentity(b, s);
            if (!payload.isEmpty())
                at = takeFirstUnconsumed(byPayload, payload);
        }
        // (c) THE NEUTRAL NAME IT IS ABOUT TO BE GIVEN. This is the case (a) used
        // to be written as, and it is real: a concealing section the user never
        // renamed is under "Hidden message 1" on BOTH sides, because this function
        // generated that name last time too. It is tried LAST because it is the
        // weakest evidence — the generated names are a small shared vocabulary, so
        // matching on one says only "some concealing section on this bus" —
        // whereas (a) and (b) match something the user or the wire chose. It
        // cannot be dropped: a relay or a signal-less message has no payload to
        // match on, so for those it is the only fallback there is.
        if (at < 0 && !neutral.isEmpty())
            at = takeFirstUnconsumed(byName, sectionNameIdentity(b, neutral));
        // A section the document has NEVER seen matches nothing here and keeps
        // what it has, which is the same answer it always got — except that a
        // concealing one is renamed now rather than before, so it is never left
        // holding "Receive 0x640": that name spells out the exact CAN ID Hidden
        // and Protected exist to withhold, printed beside "(hidden)" in the one
        // list the user is guaranteed to look at.
        if (at < 0) {
            if (!neutral.isEmpty())
                s.name = neutral;
            continue;
        }
        s.messageKey = priors[at].messageKey;
        // The user's own name and capitalisation, which the match itself is blind
        // to. The payload index is the one that can hold an UNNAMED prior, so the
        // concealing fallback below is not dead code: a section matched through it
        // must still not be left carrying its CAN ID.
        if (!priors[at].name.isEmpty())
            s.name = priors[at].name;
        else if (!neutral.isEmpty())
            s.name = neutral;
    }

    config.catalog().setUserChannels(userChannels);
    // A User Condition's output is boolean, and a Get is the one path that could
    // quietly say otherwise. The device stores the type as SIGNAL_TYPE_UINT8 —
    // boolean and u8 are the same eight bits on the wire and always were — so
    // inferDataType reads every condition output back as "u8", and an untyped
    // slot from an older device reads back as nothing at all.
    //
    // The condition TABLE is what settles it. A channel a condition writes is a
    // boolean because a condition writes it, whatever the signal record's type
    // byte happens to say, so this runs after the catalogue is installed and
    // corrects the inference from the one piece of evidence that is not
    // ambiguous. Without it, one Get would un-type every condition output in
    // the document.
    config.forceConditionOutputsBoolean();
    config.setDirty(true);
}

} // namespace ct
