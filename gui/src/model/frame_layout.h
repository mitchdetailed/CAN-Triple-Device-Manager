// What occupies which bits of a CAN frame, and which collisions are refusals.
//
// A message's 8 (or 64) bytes are shared by three kinds of thing: the channel
// rows the user places, a compound identifier's SELECTOR, and a Transmit CRC8's
// stamped checksum byte. Only the first is visible in the channel list, and the
// other two are written by the device AFTER the channels — so a channel sharing
// bits with either does not merely look wrong, it is overwritten on every
// frame and arrives as something else entirely.
//
// ---------------------------------------------------------------------------
// WHY THIS IS ONE MODULE RATHER THAN A CHECK IN EACH PLACE THAT NEEDS ONE
//
// Before it, the answer to "do these two things collide?" was computed in four
// places, and one of them disagreed with the other three.
//
//   validation.cpp     channel-vs-channel, via rowBitPositions()      correct
//   validation.cpp     channel-vs-CRC-byte, via rowBitPositions()     correct
//   bit_layout_table   the colouring, via rowBitPositions()           correct
//   validation.cpp     channel-vs-IDENTIFIER: rowFirst = startBit,
//                      rowLast = startBit + bitLength - 1             WRONG
//
// The wrong one treated a row as a CONTIGUOUS run of absolute bit positions.
// That is true only under WordSwap (Intel) alignment. Under Normal (Motorola),
// which is the DEFAULT, a field ascends within its byte and then steps to the
// PREVIOUS byte — so start bit 8, length 16 really occupies bytes 1 and 0,
// while the linear rule computed bytes 1 and 2. It therefore reported a clash
// with a selector the row never touched, and missed the one it did. It also
// read row.bitLength directly, which is wrong for an IEEE754 row: those are
// always 32 bits whatever the field says.
//
// That rule is the one this release makes BLOCKING, and a refusal computed the
// wrong way is worse than no refusal at all — it stops work that is correct and
// waves through work that is not. So the geometry is settled once, here, on top
// of the walk the device itself uses (ct::rowBitPositions, device_mapper.h), and
// every caller asks this module instead of doing its own arithmetic.
//
// ---------------------------------------------------------------------------
// THE RULE: WHICH COLLISIONS REFUSE
//
//   Receive,  channel vs channel      warn only
//   Transmit, channel vs channel      REFUSE
//   any,      channel vs identifier   REFUSE
//   any,      channel vs CRC8 byte    REFUSE
//
// The single asymmetry is channel-vs-channel on a RECEIVE message, and it is
// deliberate: two receive rows reading the same bits is a legitimate thing to
// do — the same byte decoded twice at different scalings, or a raw copy kept
// beside a cooked one — and nothing is lost, because reading a bit does not
// consume it. On TRANSMIT the same overlap is two values packed into one place:
// whichever row is packed last wins and the other silently never leaves the
// device.
//
// A compound section is checked PER IDENTIFIER and never across them, because
// identifiers are mutually-exclusive frame variants — two of them reusing the
// same bits is the entire point of multiplexing. Only the identifier's OWN
// selector is reserved against its own rows, for the same reason.
#pragma once

#include <QHash>
#include <QList>
#include <QString>

#include "comms_types.h"

namespace ct {

// The absolute bit positions a compound identifier's selector occupies.
//
// Straight from the device: engine_core.c's muxSelected() builds the selector
// as a LITTLE-ENDIAN two-byte window at byteOffset —
//
//     uint16_t sel = 0;
//     for (int i = 0; i < 2; ++i) sel |= data[byteOffset + i] << (8 * i);
//     return (sel & mux_mask) == (mux_id & mux_mask);
//
// so bit b of the mask is bit (b % 8) of byte (byteOffset + b / 8), which is
// absolute position byteOffset * 8 + b. Only the bits idMask names are read on
// receive or written on transmit; the rest of the window is ordinary frame.
//
// An identifier with idMask == 0 reserves nothing — the device treats that as
// "always active" and never consults the window at all.
QList<int> identifierBitPositions(const CompoundIdentifier &ident);

// Every bit of the frame that is spoken for by something OTHER than a channel,
// as bit position -> a short reason fit to show in a tooltip.
//
// `identifierIndex` selects which identifier's selector applies and must be the
// one whose rows are being laid out; pass -1 for a simple (non-compound)
// section. Passing the wrong one would reserve bits that this identifier's
// frame never carries.
QHash<int, QString> reservedBits(const CommsSection &section, int identifierIndex = -1);

// One collision, and whether it refuses.
struct LayoutClash
{
    enum Kind {
        ChannelChannel,    // two rows in the same frame variant
        ChannelIdentifier, // a row under a compound identifier's selector
        ChannelCrc,        // a row in the byte a Transmit CRC8 stamps
    };

    Kind kind = ChannelChannel;
    QString channel;          // the row at fault
    QString other;            // the row it collides with (ChannelChannel only)
    int identifierIndex = -1; // 0-based, for ChannelIdentifier; -1 in a simple section
    // TRUE when this collision must stop OK. See the rule table above.
    bool blocking = false;

    // The sentence shown to the user, in the section editor and in the
    // validation report alike. One wording, so the message a user reads at OK
    // is the message they find again in Check Channels.
    QString message() const;
};

// Every collision in `section`, in a stable order: simple sections first by row
// order, compound sections identifier by identifier.
//
// Rows that cannot be laid out at all — off the end of the frame, a nonsense
// bit length — are SKIPPED rather than reported here. computeExtraction()
// already refuses those with a better message, and a row with no valid position
// cannot meaningfully be said to collide with anything.
QList<LayoutClash> findLayoutClashes(const CommsSection &section);

// Does `section` hold any collision that must stop OK?
bool hasBlockingLayoutClash(const CommsSection &section);

} // namespace ct
