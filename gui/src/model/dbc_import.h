// Minimal DBC (Vector CANdb) parser and mapping into the app's comms model.
//
// Scope: BO_ messages and SG_ signals (including the M / m<n> multiplexor
// markers), plus SIG_VALTYPE_ float markers. Comments (CM_), value tables
// (VAL_), attributes (BA_) and nodes are ignored. The parser is pure model code (no widgets) so it is unit-tested.
//
// The one subtlety is the start-bit convention. A DBC file stores a signal's
// start bit as its LEAST significant bit for little-endian (Intel, @1) signals
// — which matches this app's convention directly — but as its MOST significant
// bit for big-endian (Motorola, @0) signals. dbcStartBitToLsb() converts the
// Motorola MSB into the app's LSB start bit (Intel is identity).
#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include "channel.h"
#include "comms_types.h"

namespace ct {

// One SG_ signal line.
struct DbcSignal {
    QString name;
    int startBit = 0;         // as written in the DBC (Intel: LSB; Motorola: MSB)
    int bitLength = 1;
    bool bigEndian = false;   // @0 = Motorola / big-endian; @1 = Intel / little-endian
    bool isSigned = false;    // '-' sign flag
    double factor = 1.0;      // physical = raw * factor + offset
    double offset = 0.0;
    double minValue = 0.0;
    double maxValue = 0.0;
    QString unit;
    // SIG_VALTYPE_ marker: 0 = integer (default), 1 = IEEE754 float (32-bit),
    // 2 = IEEE754 double (64-bit — unsupported by the device, warned at parse).
    int valueType = 0;
    // Multiplexing: exactly one of these is set for a muxed message's members.
    bool isMultiplexor = false; // 'M' — its value selects the active sub-message
    bool isMultiplexed = false; // 'm<n>' — present only while multiplexor == muxValue
    int muxValue = -1;          // the <n> for a multiplexed signal
};

// One BO_ message and its signals.
struct DbcMessage {
    quint32 rawId = 0;  // the BO_ id verbatim (bit 31 may carry the extended flag)
    quint32 canId = 0;  // masked to the 11- or 29-bit identifier
    bool extended = false;
    QString name;
    int dlc = 8;
    QString transmitter;
    QList<DbcSignal> signalList; // "signals" is a Qt keyword macro

    bool hasMultiplexing() const;
    // First multiplexor ('M') signal, or nullptr if the message isn't multiplexed.
    const DbcSignal *multiplexor() const;
};

struct DbcFile {
    QString version;
    QList<DbcMessage> messages;
};

// Parse DBC text. Non-fatal issues (unparsable signal lines, etc.) are appended
// to `warnings` when provided.
DbcFile parseDbc(const QString &text, QStringList *warnings = nullptr);

// Convert a DBC start bit into the app's LSB-based start bit. Intel (little-
// endian) is identity; Motorola (big-endian) walks MSB -> LSB.
int dbcStartBitToLsb(int dbcStartBit, int bitLength, bool bigEndian);

// Section alignment implied by a signal's byte order.
SectionAlignment alignmentForDbcSignal(const DbcSignal &sig);

// Build a device extraction row / catalogue channel from a signal, using the
// given (already-unique) channel name.
// The channel name a DBC signal is IMPORTED under: underscores become spaces.
// DBC signal names are C identifiers, so a name that wants to read "Engine
// Speed" has to be written "Engine_Speed"; the underscore is the format's
// limitation, not the author's intent, and the catalogue has no such rule.
//
// Applied where the signal BECOMES a channel and nowhere earlier. DbcSignal::
// name keeps the file's spelling, because the file refers to its own signals by
// it — SIG_VALTYPE_ lines are matched on that name, and rewriting it at parse
// time would leave every float signal's value type unresolved.
//
// simplified(), so "Engine__Speed" does not import as "Engine  Speed" and
// "_Rpm" does not import with a leading space. A name that is nothing but
// underscores comes back empty, which the importer already handles — it falls
// back to "Signal".
QString channelNameFromDbcSignal(const QString &signalName);

CommsChannelRow rowFromDbcSignal(const DbcSignal &sig, const QString &channelName);
Channel channelFromDbcSignal(const DbcSignal &sig, const QString &channelName);

// The physical span an INTEGER-CODED signal can carry: both raw endpoints
// through physical = raw × factor + offset (a negative factor flips the
// ordering, and a negative offset can make an unsigned field's range negative).
// NOT meaningful for an IEEE754 signal (valueType == 1), whose raw bits are the
// value rather than an integer to scale — feeding one here yields a bogus
// 0..2^n-1 span that would clamp every negative reading to zero.
void dbcPhysicalRange(const DbcSignal &sig, double *lo, double *hi);

// Smallest channel storage type that represents every value in [lo, hi] at the
// given decimal precision, or "float" when no integer type can. An integer
// channel stores a scaled integer, so its reach is rawRange × 10^-decimals —
// the type must therefore be chosen from the physical range and the decimals,
// not from the signal's raw bit width.
QString storageTypeForRange(double lo, double hi, int decimals);

// True when a channel of `dataType` at `decimals` can actually represent every
// value in [lo, hi]. False means the channel is mis-sized — the device clamps
// to its range, so readings beyond what the type reaches are silently lost.
bool storageTypeHoldsRange(const QString &dataType, double lo, double hi, int decimals);

// Compute a compound identifier's little-endian selector (byteOffset / id /
// idMask) for a multiplexor signal and one of its values. Returns false (with a
// reason) when the multiplexor can't be expressed as a byte-window selector
// (e.g. a Motorola multiplexor that spans multiple bytes). messageLenBytes
// bounds the selector offset — pass the message's DLC in bytes (up to 64 for FD).
bool muxSelectorForValue(const DbcSignal &multiplexor, int value, int *byteOffset,
                         quint32 *id, quint32 *idMask, QString *reason = nullptr,
                         int messageLenBytes = 8);

// Best-effort Channel Type (quantity) guess from a DBC unit string; "Unitless"
// when unknown. The user can override it in the import dialog.
// What a DBC's unit string means in this application's terms.
//
// A .dbc writes its unit as free text and every tool spells it differently:
// "degC", "Deg C", "\u00b0C" and "Celsius" are one unit, and NONE of them is how the
// channel catalogue spells it ("C"). Importing the DBC's text verbatim gave
// channels units the app does not offer, which then could not be picked from
// any list, matched no other channel, and had to be retyped by hand.
//
// `quantity` and `unit` are always things the catalogue offers, so an imported
// channel is indistinguishable from a hand-made one. `recognised` is false when
// the unit was not empty and nothing matched it: the import cannot know what
// "Nm/deg" is, so it says so and lets the user choose rather than inventing an
// answer.
struct DbcUnit {
    QString quantity;   // a ChannelCatalog::quantities() entry
    QString unit;       // one of ChannelCatalog::unitsForQuantity(quantity)
    bool recognised = true; // false: the DBC said something we could not place
};
DbcUnit dbcUnitFor(const QString &rawUnit);

QString quantityForUnit(const QString &unit);

} // namespace ct
