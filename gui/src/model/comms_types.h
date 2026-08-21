// Document model for communications sections (MoTeC-style messages) and the
// channel rows inside them. JSON (de)serialization lives in configuration.cpp.
#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>

#include "access_keys.h" // AccessKey / kNoAccessKey for the per-section password

namespace ct {

// Raw data type of a comms field (Add Comms Channel "DBC Type").
enum class DbcType { Unsigned = 0, Signed = 1, IEEE754 = 2 };

// One signal packed into / extracted from a message frame, defined DBC-style:
// Start Bit + Bit Length + DBC Type, with linear scaling that reads DIFFERENTLY
// in each direction:
//
//     receive:   physical = raw × Bit Resolution + Offset
//     transmit:  raw      = physical ÷ Bit Resolution + Offset
//
// The RESOLUTION means one thing both ways — 0.1 is a tenth per count whether
// the message is received or transmitted — which is why the editor calls it
// "Bit Resolution" rather than "DBC Factor".
//
// The OFFSET is always ADDED, and that is the point of it: "Offset 64" means
// put 64 more on the wire, the same way "+40" means add forty. Transmit used to
// run the algebraic inverse, (physical − Offset) ÷ resolution, which made the
// same field SUBTRACT on the way out — correct arithmetic, wrong answer to the
// question the field asks. It lands after the resolution, so on transmit it is
// counted in RAW COUNTS: value 1 at resolution 0.1 with Offset 64 sends 74, not
// (1 + 64) ÷ 0.1 = 650.
//
// Consequence, and it is not an accident: the two directions are NOT inverses.
// Two CAN Triples wired together with the SAME row apply the offset twice.
// Negate it on one of the two rows to get a value back unchanged.
//
// The FIELDS keep their dbcFactor/dbcOffset names here and in the .ct3, because
// renaming a label costs nothing and renaming a stored key would invalidate
// every saved configuration.
//
// Byte order (Intel vs Motorola) is the section's Alignment. These map 1:1 onto
// the device's signal record, so a row round-trips through Get exactly — the one
// caveat is that both are stored on the wire as float32, so a double like 0.1
// comes back as its nearest float (0.1 → 0.100000001…). That float value then
// round-trips stably.
struct CommsChannelRow {
    QString channelName;
    double defaultValue = 0.0;   // physical units, applied on receive timeout
    int startBit = 0;            // DBC start bit — the signal's LSB (both byte orders)
    int bitLength = 16;          // 1..64 (forced 32 when dbcType == IEEE754)
    int dbcType = int(DbcType::Unsigned);
    double dbcFactor = 1.0;      // physical = raw × factor + offset
    double dbcOffset = 0.0;

    // TRANSMIT only: clamp the outgoing value to what the field can hold, or
    // send the low bitLength bits of it. Clamping is the default and was the
    // only behaviour before this existed, so an older .ct3 (no key) loads as
    // true and behaves exactly as it always did.
    //
    // Unticked, 256 into an 8-bit field with a resolution of 1 sends 0, not
    // 255 — the count rolls over, which is what a rolling counter, a checksum
    // input or a wrapping angle actually wants. It also stops the CHANNEL's
    // range clamping first: a channel ranged 0..255 would otherwise never
    // present 256 for the field to roll over.
    //
    // Receive rows ignore it and always clamp — a received field is bitLength
    // bits wide by construction, so there is nothing to roll over, and the
    // clamp on that side is the channel's declared range.
    //
    // Stored INVERTED on the wire (SIG_FLAG_TX_WRAP, set = wrap) so that a
    // record written before the flag existed, whose bits are all zero, means
    // clamp. The single inversion lives in device_mapper.
    bool clampToRange = true;

    QJsonObject toJson() const;
    static CommsChannelRow fromJson(const QJsonObject &o);
};

// One compound sub-message: rows apply when (data[offset..] & idMask) == id.
struct CompoundIdentifier {
    int byteOffset = 0;
    quint32 id = 0;
    quint32 idMask = 0xFF;
    bool configured = false; // user has explicitly set this slot (a default-valued
                             // identifier is otherwise indistinguishable from unused)
    QList<CommsChannelRow> rows;

    QJsonObject toJson() const;
    static CompoundIdentifier fromJson(const QJsonObject &o);
};

enum class SectionDevice { Off, ReceiveMessage, TransmitMessage, MessageRelay, TransmitCrc8 };
enum class SectionAlignment { Normal, WordSwap }; // Normal = big-endian, WordSwap = little-endian

// Cadence for a compound (multiplexed) TRANSMIT section (v10 firmware):
// Batch = send every identifier's frame each transmit period; Sequential = send
// one identifier per period, round-robin. No effect on receive/simple sections.
enum class CompoundTxMode { Batch = 0, Sequential = 1 };

// How much of a message's protocol this document withholds, and from whom. ONE
// ORDERED LEVEL, never a bit-combination: the values are the wire encoding
// (MSGPROT_MASK >> 6) and they compare, so every consumer asks ">= Hidden" or
// "!= None" rather than reassembling a rule out of two booleans.
enum class CommsProtection : quint8 {
    None      = 0,
    ReadOnly  = 1, // viewable, not editable
    Hidden    = 2, // not viewable, not editable
    Protected = 3, // Hidden, plus the untick needs a live device proof
};

// Tier <-> the stable JSON token written from schema 14 onward. None has NO
// token: it is spelled by omitting the key, so an ordinary message carries no
// trace of the feature — the same idiom the old readOnlyComms used.
QString commsProtectionToken(CommsProtection protection);
// An UNRECOGNISED token clamps to Protected. A .ct3 is unauthenticated JSON, so
// a hand-edited or truncated value is not a hypothetical; Protected is the only
// direction the guess can fail in that does not leak a protocol.
CommsProtection commsProtectionFromToken(const QString &token);

// Tier <-> the top two bits of CanMessageConfig::flags and RelayConfig::flags.
// Defined in configuration.cpp next to static_asserts pinning them to the
// MSGPROT_* constants, so the model's idea of the encoding cannot drift from
// the wire header's without failing the build.
quint8 commsProtectionToWire(CommsProtection protection);
CommsProtection commsProtectionFromWire(quint8 flags);

// A communications section = one CAN message slot on a bus.
struct CommsSection {
    QString name;                     // display name in the Sections list
    SectionDevice device = SectionDevice::ReceiveMessage;
    SectionAlignment alignment = SectionAlignment::Normal;
    int receiveTimeoutMs = 2200;
    bool defaultValueOnTimeout = true;
    QString diagnosticChannel;

    bool extended = false;
    bool fd = false;                  // CAN FD frame (allows 12/16/20/24/32/48/64-byte lengths)
    quint32 baseAddress = 0;
    bool routeEnable = false;         // CAN Triple extra: gateway this message
    int routeBusMask = 0;             // bit0=CAN1 bit1=CAN2 bit2=CAN3
    bool cyclic = true;               // transmit only: false = Triggered
    int transmitRateHz = 50;          // transmit only (UI field)
    int transmitPeriodMs = 0;         // authoritative period when > 0 (else derived
                                      // from rate); set on Get so any device period
                                      // survives a round-trip exactly

    // Triggered transmit. `cyclic == false` means the message only goes out while
    // this User Condition holds; the rate above still caps how often.
    //
    // The condition is named by its OUTPUT CHANNEL, not by its row number, even
    // though the wire carries an index. A ConditionRow has no name and no stable
    // id — every other part of the document identifies one by position — so an
    // index here would silently re-point at a different condition the moment a
    // row was inserted, deleted or reordered above it. The output channel is the
    // one handle a condition really has, it is unique because two conditions
    // writing one channel is already a validation warning, and it comes free
    // with the rename walk that repoints every other channel reference.
    // mapToDevice resolves it to the index the device wants; mapFromDevice
    // resolves it back.
    //
    // There is no "reset it once triggered" tickbox beside this any more. A
    // Set/Reset User Condition carries its own Reset expression, so "send once
    // when the request arrives" is written where it can be read — set on
    // Message Received, reset on Message Transmitted — instead of a transmit
    // message reaching sideways to rewrite a calculation's output.
    QString transmitCondition;
    int messageLengthBytes = 8;       // DLC

    bool compound = false;
    CompoundTxMode compoundTxMode = CompoundTxMode::Batch; // compound transmit cadence
    QList<CommsChannelRow> rows;              // single-message mode
    QList<CompoundIdentifier> identifiers;    // compound mode

    // Message Relay (v11): a masked-ID gateway rule. baseAddress is the match
    // address, extended selects the frame type, routeBusMask is the set of buses
    // to forward matching frames to (never this section's own bus). A frame
    // matches when (id & relayBitmask) == (baseAddress & relayBitmask);
    // relayInvert forwards the non-matching frames instead.
    quint32 relayBitmask = 0;
    bool relayInvert = false;

    // 2.3.0: how much of this message's protocol is withheld, and what it takes
    // to stop withholding it. One ordered tier replaces the v19 "Protect
    // Communication" flag and the v20 "Read-only" flag, which were independent
    // bools describing nested behaviours — 8 representable states for 4 real
    // ones, with the implication (`readOnlyComms = readOnly || protect`)
    // maintained by hand in ONE dialog and walked around by a dozen other paths
    // into the document. An ordered level makes that invariant unrepresentable.
    //
    // What each tier means and what it takes to untick it:
    //
    //   ReadOnly   Viewable by anyone, not editable. Accident prevention, not
    //              security, and it must be described that way: the viewer reads
    //              every field and may remove the section, so remove-and-retype
    //              reproduces the message without the password.
    //   Hidden     Not viewable, not editable. Substantively stronger than
    //              ReadOnly for the one reason that matters — a viewer who cannot
    //              SEE the message cannot retype it, so removing it destroys
    //              rather than reveals.
    //   Protected  As Hidden, and ALSO needs the Edit Protected Comms password
    //              PROVED AGAINST A CONNECTED DEVICE. Holding the file is not
    //              enough. That round trip is the only thing that makes this tier
    //              stronger than Hidden, so it is the whole point of it and must
    //              not decay into a local check.
    //
    // MOVING a marking — ticking a box as well as unticking one — costs THIS
    // SECTION's own password (messageKey) at all three tiers, and for Protected
    // the device round trip on top. Raising used to be free, which meant the tier
    // whose whole promise is "this needs my password to change" could be walked
    // up to a stronger one by anyone holding the file. The proof requirements per
    // tier are Configuration::proofsRequiredFor(); nothing switches on the tier
    // itself to answer that question.
    //
    // Removal is permitted at EVERY tier. Nothing in the host, and nothing in
    // the firmware, refuses it.
    //
    // Withheld means withheld everywhere it would otherwise be shown: the
    // sections list, the section editor, the Channel Summary report and Check
    // Channels. Check Channels is the subtle one — its findings quote start bits
    // and frame lengths in order to be useful, so a concealed message's findings
    // collapse into one entry that keeps the SEVERITY (an Error still blocks
    // Send) and drops the detail. Channel NAMES stay visible at every tier on
    // purpose: the point is to protect the protocol, not the outputs, so a
    // customer can still feed "Engine RPM" into their own math without ever
    // learning which bits it came from.
    //
    // Editing is locked at every tier, INCLUDING ReadOnly, and the channels a
    // locked message produces are locked with it — data type, base resolution,
    // decimal places, range and units. Those are not cosmetic: change a
    // channel's resolution and the message silently starts decoding to different
    // numbers with nothing on screen to say why. Locking the message and leaving
    // its channels open would protect the secret and break the function.
    //
    // ALL THREE TIERS ARE CONVENTIONS OF THIS APPLICATION. As of 2.3.0 the
    // device enforces nothing at all about message protection; it carries the
    // tier on the wire (MSGPROT_MASK) so a Get followed by a Send cannot launder
    // a Hidden message into an ordinary one.
    //
    // That saves the TOKEN and not the LOCK, and the difference has to be said
    // plainly because the sentence here used to claim the round trip whole. The
    // wire has no room for a key at all — the record's field is `reserved[4]`,
    // read and written as zero — so a section that comes back off a device is
    // keyless.
    //
    // A marked keyless section is CONCEALED (Configuration::isSectionRevealed),
    // and this comment used to say the opposite: "arrives MARKED and fully
    // READABLE". That was the user's bug written down as if it were a design.
    // Retrieved into a document that never held the message, a Hidden section
    // shows its name, a padlock and nothing else, and no password opens it
    // because none exists. What survives a Get in practice is whatever THIS
    // DOCUMENT already knew: mapFromDevice snapshots each section's key and
    // re-applies it to the rebuilt section, so a Get into the document the
    // configuration was BUILT in keeps every password and reads normally. That
    // is a fact about the open document and not about the hardware or the
    // protocol, and it is the difference between "Get in my own window" and "Get
    // on a fresh install".
    //
    // A plain .ct3 is unauthenticated JSON, so a text editor defeats all three,
    // as does any other serial tool talking to the device. Only a sealed .ct3s
    // makes the bytes themselves unreadable. The help has claimed otherwise twice.
    CommsProtection protection = CommsProtection::None;

    // The 4-byte PBKDF2 key for THIS section's tier, or kNoAccessKey for "no
    // password on this section". Never the password itself, exactly like every
    // other key in this app.
    //
    // It is a DOCUMENT secret and only that now. 2.3.0 retired the device's
    // per-message key outright — the wire field is `reserved[4]` and is written
    // and read back as zero in both directions — so this is never sent and never
    // returned. It survives a Get because mapFromDevice SNAPSHOTS it (and the
    // user's chosen name) before rebuilding and re-applies it to the rebuilt
    // section, matched first on the section's identity on the wire — bus, CAN ID
    // and direction — and, where that names nothing, on the bus and the name.
    // The second match is what stops a RENUMBERED message losing its password:
    // editing a base address changes exactly the handle the first match is made
    // of. Not by "merging over the open document": clearContent() empties
    // BusConfig::sections outright, so before that snapshot existed every key
    // came back kNoAccessKey and a Get opened every Hidden section in the
    // document with no password at all.
    //
    // REQUIRED on every marked section as of 2.3.1, Protected included — the
    // section editor's OK refuses while a tier is ticked and the field is empty.
    // The key belongs to the MARKING rather than to the message, so moving the
    // tier costs the old password and then a new one; the editor ties the stored
    // key to the tier it was chosen for and demands a fresh one anywhere else.
    //
    // With no key set, NOBODY may view a marked section and nobody may lower its
    // marking. That is the 2.3.2 reversal and it replaces the rule this comment
    // used to state — "anyone may move the marking and anyone may view the
    // section" — which was the whole of the reported bug: every section a Get
    // produces arrives keyless (the wire carries reserved[4] and no key), so a
    // configuration read back off a unit handed over every message its author had
    // marked. There is no password in existence for such a section, and "nothing
    // can open it" is not a reason to show it to everybody.
    //
    // What a keyless marked section can still do, so it is not a brick: it can be
    // REMOVED, at any tier; it can be reordered and sent as it stands; its
    // channels can be used anywhere else in the document; and it can be given a
    // FIRST password, which is free and is what makes it lowerable afterwards.
    // See Configuration::isSectionRevealed and maySectionLower.
    AccessKey messageKey = kNoAccessKey;

    // Transmit CRC8 (device == TransmitCrc8): the stamped checksum's recipe.
    // The section is a transmit message in every other respect — isTransmit()
    // deliberately answers true for it, so the channels tab, the scheduler
    // fields and the mapper's transmit path all apply unchanged; these fields
    // add the checksum on top. Schema 16.
    //
    // The CRC is computed over crcElements IN ORDER — each element one byte:
    // a byte of the CAN identifier (type Id, value = shift index 0..3), a
    // byte of the composed frame (type Data, value = byte index 0..7), or a
    // literal (type Raw, value = the byte) — then stamped into
    // crcByteLocation after every other byte of the frame is final, and
    // published to crcChannel so the wire's checksum is watchable like any
    // other channel. Polynomial/init/xor are the standard CRC-8
    // parameterisation (x^8 implicit): SAE J1850 is 0x1D/0xFF/0xFF with no
    // reflection, plain CCITT is 0x07/0x00/0x00.
    struct CrcElement {
        enum Type { Id = 0, Data = 1, Raw = 2 }; // the wire encoding (CRC8_ELEM_*)
        int type = Data;
        int value = 0;
    };
    QString crcChannel;
    int crcByteLocation = 0;   // 0..7
    int crcPolynomial = 0x00;  // 0x00..0xFF
    int crcInitValue = 0x00;
    int crcFinalXor = 0x00;
    bool crcRefIn = false;
    bool crcRefOut = false;
    QList<CrcElement> crcElements; // 1..15 (CRC8_MAX_ELEMENTS)

    bool isReceive() const { return device == SectionDevice::ReceiveMessage; }
    bool isTransmit() const
    { return device == SectionDevice::TransmitMessage || device == SectionDevice::TransmitCrc8; }
    bool isCrc8() const { return device == SectionDevice::TransmitCrc8; }
    bool isRelay() const { return device == SectionDevice::MessageRelay; }
    // Compound sections carry channels only inside identifiers. Fold any legacy
    // always-present `rows` (from an older file or device image) into every used
    // identifier so they stay visible/editable and survive a re-Send.
    void normalizeCompound();
    // All rows regardless of mode (for channel listings / validation).
    QList<CommsChannelRow> allRows() const;
    QStringList channelNames() const;
    // "Does this section mention that channel?", case-insensitively, WITHOUT
    // building the name list. Same answer as channelNames().contains(name,
    // CaseInsensitive) and the same rows (a compound section's channels live
    // only inside identifiers), but no heap allocation — which matters because
    // Configuration's four channel predicates ask it once per section per
    // channel, and channelNames() allocated a fresh QStringList every time.
    bool mentionsChannel(const QString &channelName) const;

    // What a marked section is allowed to say about itself. One place, so the
    // sections list, the report and Check Channels cannot each decide
    // differently what counts as a detail — the leak would be whichever one
    // forgot. `revealed` is the caller's "this viewer holds the password".
    QString displayDetail(bool revealed) const;

    // Concealed from THIS viewer. ReadOnly NEVER conceals: that is the entire
    // difference between it and Hidden, and it is the inverse of what v21 did,
    // where marking a message meant exactly one thing to a viewer — no detail
    // shown. Every suppression site asks this rather than comparing the tier
    // itself, so "which tiers hide things" is decided once.
    bool isConcealed(bool revealed) const
    {
        return protection >= CommsProtection::Hidden && !revealed;
    }

    // Edit-locked. Deliberately NOT lifted by revealing: the rule is that the
    // password lets you UNTICK the box, and unticking is what allows editing.
    // Revealing buys viewing and the right to untick, nothing more — so a
    // ReadOnly section's editor opens with every protocol field disabled, and
    // stays that way until the tier itself is lowered.
    bool isEditLocked() const { return protection != CommsProtection::None; }

    QJsonObject toJson() const;
    // `fileVersion` is the .ct3's schema number, and it is not optional. The
    // pre-14 keys migrate to a tier only when the file actually predates 14;
    // applying the migration unconditionally would re-read a v14 file's own
    // "protection" alongside absent legacy keys and ratchet ReadOnly toward
    // Hidden on every load. See the rules at the definition.
    static CommsSection fromJson(const QJsonObject &o, int fileVersion);
};

// Per-bus settings, applied via CONTROL_CAN on Send Configuration
// (firmware v2; v1 NACKs and keeps its hardcoded bring-up).
struct BusConfig {
    bool enabled = false; // Mode Off until the user turns the bus on
    int rateKbps = 1000;
    int dataRateKbps = 0; // 0 = classic CAN; > rateKbps = CAN FD with BRS
    bool termination = false; // 120Ω termination resistor on this bus (v9 firmware)
    QList<CommsSection> sections;

    // The DEVICE's own test, mirrored: the firmware picks the FD bring-up only
    // when the data rate EXCEEDS the nominal rate, so a data rate at or below
    // it IS classic, whatever the field says — and every gate on this side has
    // to agree with the hardware about that.
    bool isFd() const { return dataRateKbps > rateKbps; }

    QJsonObject toJson() const;
    // Forwards `fileVersion` to every section; see CommsSection::fromJson.
    static BusConfig fromJson(const QJsonObject &o, int fileVersion);
};

// The display spelling of a bus rate. 83 is stored for GMLAN's 83.333 kbit/s
// (ct::busRateHz sends 83,333 for it), so the label says what the wire runs.
inline QString busRateLabel(int kbps)
{
    return kbps == 83 ? QStringLiteral("83.3") : QString::number(kbps);
}

// Math channel row (Calculations > Math Channels...). Channel references are
// by name; the device mapper resolves indices. How many of A/B/C the op reads
// is mathOpArity() (wire_structs.h) — unused operands sit at their defaults
// (const 0), which is also what the wire carries for them.
struct MathRow {
    int op = 0;                  // ct::MathOp
    bool aIsChannel = true;
    QString aChannel;
    double aConst = 0;
    bool bIsChannel = false;
    QString bChannel;
    double bConst = 0;
    bool cIsChannel = false;     // third operand, arity-3 ops only
    QString cChannel;
    double cConst = 0;
    QString destChannel;
    bool active = true;

    QJsonObject toJson() const;
    static MathRow fromJson(const QJsonObject &o);
};

// User Condition rows (Calculations > User Conditions...). A condition drives a
// boolean output channel, and its MODE decides the shape of that output rather
// than merely its value — see ConditionRow below. The output is an ordinary
// generated channel, so it can feed counters/timers, math, transmit signals, a
// message's transmit trigger, or another condition.
//
// One comparison inside a condition: "A op B", or — for the two message
// operators — "this message was received / was transmitted".
struct ConditionTermRow {
    QString aChannel;
    int op = 0;                  // ct::ConditionOp
    bool bIsChannel = false;
    QString bChannel;
    double bConst = 0;

    // The two MESSAGE operators (COND_OP_MSG_RX / COND_OP_MSG_TX) take a
    // message where a channel comparison takes input A, and ignore B entirely.
    //
    // A message is named by its BUS and its section NAME, not by an index, for
    // the reason a transmit condition names a channel rather than a row: an
    // index is permuted by reordering, shifted by deleting or Off-ing any
    // earlier section, and rewritten wholesale by a Get. (bus, name) is also
    // not a new idea here — the Get reconciliation pass, the protection untick
    // path and the section access grants had each already settled on it.
    int aMessageBus = 0;         // 1..3; 0 = unset
    QString aMessage;            // the section's name, compared case-insensitively

    bool isMessageOp() const;

    QJsonObject toJson() const;
    static ConditionTermRow fromJson(const QJsonObject &o);
};

// Fold already-rendered comparison texts with a condition's joiners, bracketing
// LEFT TO RIGHT so the displayed expression reads exactly the way the device
// evaluates it — ((A and B) or C), never C's "&& binds tighter". Callers supply
// their own comparison strings, so the report can print "==" while the editor
// shows "=". Shared so those two can never disagree about grouping.
QString joinConditionTerms(const QStringList &termTexts, const QList<int> &joiners,
                           const QString &andWord, const QString &orWord);

// How a User Condition shapes its output. There is no plain level any more: a
// condition that simply follows its comparisons is a Set/Reset whose Reset is
// the inverse of its Set, which is exactly what every pre-modes configuration
// was migrated into.
enum class ConditionMode {
    Momentary = 0, // rising edge of Set -> 1, holds one period of latchHz
    SetReset = 1,  // Set -> 1, Reset -> 0, holds between; RESET IS DOMINANT
};

// A User Condition drives a boolean output channel.
//
// Both expressions hold 1..COND_MAX_TERMS comparisons with one joiner per gap
// (joiners.size() == terms.size() - 1), folded STRICTLY LEFT TO RIGHT —
// ((t0 J0 t1) J1 t2) — which is what the editor displays.
//
// Momentary uses setTerms and latchHz; Set/Reset uses setTerms and resetTerms.
// The unused half is KEPT rather than cleared when the mode changes, so
// switching back and forth in the editor does not destroy what was typed.
// mapToDevice is what decides which half reaches the device.
struct ConditionRow {
    ConditionMode mode = ConditionMode::SetReset;
    QList<ConditionTermRow> setTerms{ConditionTermRow{}};
    QList<int> setJoiners;       // ct::ConditionJoin per gap
    QList<ConditionTermRow> resetTerms{ConditionTermRow{}};
    QList<int> resetJoiners;
    // Momentary hold, as a frequency: the output holds for ONE PERIOD of this,
    // so 10 Hz is 100 ms. Capped at COND_LATCH_MAX_HZ because the device spends
    // the hold on its 10 ms calculation pass and cannot resolve finer.
    int latchHz = 10;
    QString outputChannel;       // boolean output: 1 while the condition holds
    bool active = true;

    // Every channel the condition reads, across BOTH expressions, for
    // rename/validation walks. Message operands are not channels and are not
    // included; see messageRefs().
    QStringList inputChannels() const;
    // Every message either expression names, as (bus, section name) pairs.
    QList<QPair<int, QString>> messageRefs() const;

    QJsonObject toJson() const;
    // Accepts the modes form, the v14 "terms" form (migrated to a Set/Reset
    // whose Reset is the inverse of the Set) and the older single-comparison
    // shape.
    static ConditionRow fromJson(const QJsonObject &o);
};

// The logical inverse of an expression: negate every comparison and flip every
// joiner. Exact for a strictly left-to-right fold, by De Morgan applied at each
// gap, and lossless because all six comparison operators invert within the set.
//
// This is what migrates a pre-modes condition, so it has to be right: the
// result must be true exactly when the input is false, for every combination of
// operators and joiners. Refuses (returns false, leaving `out` untouched) if any
// term carries a message operator, which has no negation.
bool invertConditionExpr(const QList<ConditionTermRow> &terms, const QList<int> &joiners,
                         QList<ConditionTermRow> *outTerms, QList<int> *outJoiners);

// Up/Down counter (Calculations > Up / Down Counters). Inputs are boolean
// channels (edge-triggered); the output channel is generated by the counter.
struct CounterRow {
    QString outputChannel;
    int mode = 0;                // 0 = up/down, 1 = follow changes, 2 = rate
    QString upChannel;
    QString downChannel;
    QString followChannel;       // FOLLOW mode source
    QString resetChannel;
    QString enableChannel;
    double minValue = 0;
    double maxValue = 1000000;
    double resetValue = 0;
    double step = 1;
    bool rollAtLimits = false;
    bool preserveValue = false;
    // Rate mode only: how many steps a second, and which way. One of
    // ct::kCounterRateChoices. Ignored by the other two modes, and written to
    // the device as 0 there so a mode change cannot leave a stale rate behind
    // in a record the user can no longer see.
    int rateHz = 1;
    bool rateCountDown = false;
    bool active = true;

    QJsonObject toJson() const;
    static CounterRow fromJson(const QJsonObject &o);
};

// Timer (Calculations > Timers). Accumulates seconds while running; started
// and stopped on the rising edge of a boolean channel. Output is generated.
struct TimerRow {
    QString outputChannel;
    QString startChannel;
    QString stopChannel;
    bool countDown = false;
    bool rollover = false;
    double limitValue = 0;
    bool setOnStart = false;
    double startValue = 0;
    bool setOnStop = false;
    double stopValue = 0;
    bool active = true;

    QJsonObject toJson() const;
    static TimerRow fromJson(const QJsonObject &o);
};

// Integrator (Calculations > Integrators). A rate accumulator: every step it
// moves its output channel by its input — `output += input` (or `-=` when
// countDown), rateHz times per second. RAW accumulation, not `input * dt`, so
// the rate scales the result. Input is a channel or a fixed value; the optional
// Enable gate freezes it and the optional Reset channel reloads resetValue on a
// rising edge.
//
// A DECREMENTOR is this same row with countDown set and startValue at the peak:
// it loads full, counts down, and floors at minValue. There is no separate type
// because the device runs both through one table and one evaluation pass.
struct IntegratorRow {
    QString outputChannel;
    bool inputIsChannel = true;
    QString inputChannel;
    double inputValue = 0;       // used when inputIsChannel is false
    int rateHz = 10;             // steps per second, 1..INTEGRATOR_MAX_HZ
    bool countDown = false;      // subtract instead of add
    double startValue = 0;       // value the device loads when the config loads
    QString enableChannel;       // accumulates only while > 0 (empty = always)
    QString resetChannel;        // rising edge reloads resetValue (empty = none)
    double resetValue = 0;
    double minValue = 0;
    double maxValue = 1000000;   // max <= min disables clamping
    // Retain the running total across power cycles. Shares the device's
    // 20-entry preserve ring with counters — see CounterRow::preserveValue.
    bool preserveValue = false;
    bool active = true;

    QJsonObject toJson() const;
    static IntegratorRow fromJson(const QJsonObject &o);
};

// Constant (Calculations > Constants). Essentially a custom channel (name +
// data type + decimals, with range derived from the type) that carries a fixed
// Value written into its generated channel every evaluation pass. Unlike a full
// channel it has no Channel Type or Display Units.
struct ConstantRow {
    QString name;                    // generated channel name
    QString dataType;                // u8/u16/u32/s8/s16/s32/float/boolean
    int decimalPlaces = 0;
    double value = 0;
    bool active = true;

    QJsonObject toJson() const;
    static ConstantRow fromJson(const QJsonObject &o);
};

// Lookup tables (Calculations > Tables). The output is a generated channel
// (name + data type + decimals, like a Constant). Each axis input is a channel;
// per axis the lookup is Interpolated (linear; bilinear for 8x8 when both
// interpolate) or Discrete-centered (holds a site's value, switches at the
// midpoint between sites). Inputs clamp to the end sites. Sites must ascend.
// Tables are partial: only the populated sites are used (empty tables start
// with no sites).

// 2x16: one input axis with up to 16 sites -> matching output values. xSites and
// outputs are the same length (0..16); the axis provides one value per site.
// One logical row here; the device splits it across two wire records (v13).
struct Table2x16Row {
    QString outputChannel;           // generated channel name
    QString dataType = QStringLiteral("float"); // u8/u16/u32/s8/s16/s32/float/boolean
    int decimalPlaces = 0;
    QString xChannel;                // input axis channel
    bool xInterp = true;             // true = interpolated, false = discrete-centered
    QList<double> xSites;            // ascending breakpoints (0..16)
    QList<double> outputs;           // one output per site (same length as xSites)
    bool active = true;

    QJsonObject toJson() const;
    // Accepts both the v13 "tables2x16" form and older 8-site "tables2x8" files.
    static Table2x16Row fromJson(const QJsonObject &o);
};

// 8x8: X and Y input axes with up to 8 sites each -> a grid of the populated
// sites. `outputs` is row-major over the X width: outputs[y*xSites.size() + x],
// length xSites.size() * ySites.size(). One logical row here; the device splits
// it across a Def record and one record per grid ROW (see Table8x8Def).
//
// This replaced the 4x4, whose model row had exactly these fields — the site
// lists were always variable-length and always packed over the ACTUAL X width,
// never a fixed 4 — which is why the .ct3 migration below is a straight parse
// rather than a reshaping: a 3x2 table written as a 4x4 is already, key for key
// and cell for cell, the same 3x2 table read as an 8x8. It simply occupies the
// top-left of a grid that can now grow to 8x8 (64 cells instead of 16).
struct Table8x8Row {
    QString outputChannel;
    QString dataType = QStringLiteral("float");
    int decimalPlaces = 0;
    QString xChannel;
    QString yChannel;
    bool xInterp = true;
    bool yInterp = true;
    QList<double> xSites;            // ascending X breakpoints (0..8)
    QList<double> ySites;            // ascending Y breakpoints (0..8)
    QList<double> outputs;           // row-major: outputs[y*xSites.size() + x]
    bool active = true;

    QJsonObject toJson() const;
    static Table8x8Row fromJson(const QJsonObject &o);
    // A "tables4x4" entry from a schema-11 (or older) file, loaded into the
    // top-left of an 8x8. Named rather than folded into fromJson so the call
    // site at the old key reads as the migration it is; see the note above for
    // why the two forms parse identically.
    static Table8x8Row fromTable4x4Json(const QJsonObject &o);
};

// Rewrite every channel reference in one row family from `oldName` to
// `newName` (case-insensitively, like all channel matching), returning how
// many references changed. One overload per family, defined side by side in
// configuration.cpp so the field lists cannot drift apart.
//
// Free functions over bare row lists rather than private Configuration
// helpers, because the document is not the only holder of rows:
// Configuration::renameChannelReferences is their sum over the document, and
// each grid dialog runs the matching walk over its private working copy when
// Configuration::channelRenamed fires — see that signal for why it must.
int renameChannelRefs(CommsSection &section, const QString &oldName, const QString &newName);
int renameChannelRefs(QList<CommsSection> &sections, const QString &oldName,
                      const QString &newName);
int renameChannelRefs(QList<MathRow> &rows, const QString &oldName, const QString &newName);
int renameChannelRefs(QList<ConditionRow> &rows, const QString &oldName, const QString &newName);
int renameChannelRefs(QList<CounterRow> &rows, const QString &oldName, const QString &newName);
int renameChannelRefs(QList<TimerRow> &rows, const QString &oldName, const QString &newName);
int renameChannelRefs(QList<IntegratorRow> &rows, const QString &oldName, const QString &newName);
int renameChannelRefs(QList<ConstantRow> &rows, const QString &oldName, const QString &newName);
int renameChannelRefs(QList<Table2x16Row> &rows, const QString &oldName, const QString &newName);
int renameChannelRefs(QList<Table8x8Row> &rows, const QString &oldName, const QString &newName);

} // namespace ct
