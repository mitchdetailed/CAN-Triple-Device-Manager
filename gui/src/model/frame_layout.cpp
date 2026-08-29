#include "frame_layout.h"

#include <QCoreApplication>

#include <bitset>

#include "device_mapper.h" // rowBitPositions, computeExtraction, MAX_FRAME_BITS

namespace ct {

namespace {

// A row's occupancy as a bitset, via the device's own walk. Every collision
// test in this file goes through here so none of them can invent its own idea
// of where a field sits — which is exactly the drift this module exists to end.
std::bitset<MAX_FRAME_BITS> occupancyOf(const CommsChannelRow &row, SectionAlignment alignment)
{
    std::bitset<MAX_FRAME_BITS> bits;
    for (int pos : rowBitPositions(row, alignment))
        bits.set(std::size_t(pos));
    return bits;
}

// Is this row worth testing at all? A row that cannot be extracted is already
// refused with a better message by computeExtraction, and one with no positions
// cannot collide with anything.
bool laidOut(const CommsChannelRow &row, const CommsSection &section)
{
    if (row.channelName.isEmpty())
        return false;
    return !rowBitPositions(row, section.alignment).isEmpty();
}

// THE RULE, in one place. Everything else in this file is geometry.
bool blocks(const CommsSection &section, LayoutClash::Kind kind)
{
    if (kind == LayoutClash::ChannelChannel)
        return section.isTransmit();
    return true;
}

// The bits the CRC8 stamp covers — the whole byte, because the stamp writes a
// whole byte whatever the channels around it are doing.
QList<int> crcBitPositions(const CommsSection &section)
{
    QList<int> bits;
    if (!section.isCrc8())
        return bits;
    for (int b = 0; b < 8; ++b)
        bits.append(section.crcByteLocation * 8 + b);
    return bits;
}

// One frame variant: a set of rows that really do share a frame, plus the
// identifier (if any) whose selector is stamped over them.
void clashesIn(const CommsSection &section, const QList<CommsChannelRow> &rows, int identifierIndex,
               QList<LayoutClash> *out)
{
    struct Placed
    {
        std::bitset<MAX_FRAME_BITS> bits;
        QString channel;
    };
    QList<Placed> placed;
    for (const CommsChannelRow &row : rows) {
        if (!laidOut(row, section))
            continue;
        placed.append({occupancyOf(row, section.alignment), row.channelName});
    }

    // Channel vs channel, each pair once.
    for (int i = 0; i < placed.size(); ++i) {
        for (int j = i + 1; j < placed.size(); ++j) {
            if ((placed[i].bits & placed[j].bits).none())
                continue;
            LayoutClash clash;
            clash.kind = LayoutClash::ChannelChannel;
            clash.channel = placed[i].channel;
            clash.other = placed[j].channel;
            clash.identifierIndex = identifierIndex;
            clash.blocking = blocks(section, clash.kind);
            out->append(clash);
        }
    }

    // Channel vs this variant's own selector. Never another identifier's: that
    // one's selector is only ever stamped into ITS frame, which these rows are
    // not in.
    if (identifierIndex >= 0 && identifierIndex < section.identifiers.size()) {
        const CompoundIdentifier &ident = section.identifiers.at(identifierIndex);
        // An unconfigured slot is one the user has not set up; it writes
        // nothing, so it reserves nothing.
        if (ident.configured) {
            std::bitset<MAX_FRAME_BITS> sel;
            for (int pos : identifierBitPositions(ident))
                if (pos >= 0 && pos < MAX_FRAME_BITS)
                    sel.set(std::size_t(pos));
            for (const Placed &p : placed) {
                if ((p.bits & sel).none())
                    continue;
                LayoutClash clash;
                clash.kind = LayoutClash::ChannelIdentifier;
                clash.channel = p.channel;
                clash.identifierIndex = identifierIndex;
                clash.blocking = blocks(section, clash.kind);
                out->append(clash);
            }
        }
    }

    // Channel vs the CRC8 stamp.
    if (section.isCrc8()) {
        std::bitset<MAX_FRAME_BITS> crc;
        for (int pos : crcBitPositions(section))
            if (pos >= 0 && pos < MAX_FRAME_BITS)
                crc.set(std::size_t(pos));
        for (const Placed &p : placed) {
            if ((p.bits & crc).none())
                continue;
            LayoutClash clash;
            clash.kind = LayoutClash::ChannelCrc;
            clash.channel = p.channel;
            clash.identifierIndex = identifierIndex;
            clash.blocking = blocks(section, clash.kind);
            out->append(clash);
        }
    }
}

} // namespace

QList<int> identifierBitPositions(const CompoundIdentifier &ident)
{
    QList<int> bits;
    if (ident.idMask == 0)
        return bits; // "always active" — the device never reads the window
    for (int b = 0; b < 16; ++b)
        if ((ident.idMask >> b) & 1u)
            bits.append(ident.byteOffset * 8 + b);
    return bits;
}

QHash<int, QString> reservedBits(const CommsSection &section, int identifierIndex)
{
    QHash<int, QString> reserved;
    if (identifierIndex >= 0 && identifierIndex < section.identifiers.size()) {
        const CompoundIdentifier &ident = section.identifiers.at(identifierIndex);
        if (ident.configured) {
            const QString why =
                QCoreApplication::translate("ct::FrameLayout",
                                            "identifier %1 writes its selector here")
                    .arg(identifierIndex + 1);
            for (int pos : identifierBitPositions(ident))
                reserved.insert(pos, why);
        }
    }
    if (section.isCrc8()) {
        const QString why = QCoreApplication::translate(
            "ct::FrameLayout", "the CRC8 checksum is stamped over this byte");
        for (int pos : crcBitPositions(section))
            reserved.insert(pos, why);
    }
    return reserved;
}

QString LayoutClash::message() const
{
    switch (kind) {
    case ChannelChannel:
        // Named the same way in both directions, because which of the two is
        // "first" is an artefact of list order and means nothing to the reader.
        return QCoreApplication::translate("ct::FrameLayout",
                                           "'%1' and '%2' overlap in the frame")
            .arg(channel, other);
    case ChannelIdentifier:
        // Says what goes wrong on the wire rather than merely that two things
        // touch: the selector is written after the channels, so the channel
        // does not get corrupted, it gets REPLACED by the identifier value.
        return QCoreApplication::translate(
                   "ct::FrameLayout",
                   "identifier %1 writes its selector over channel '%2' — the selector is "
                   "written last, so that channel carries the identifier value instead of "
                   "its own")
            .arg(identifierIndex + 1)
            .arg(channel);
    case ChannelCrc:
        return QCoreApplication::translate(
                   "ct::FrameLayout",
                   "'%1' sits in the byte the CRC8 is stamped into — the stamp overwrites "
                   "those bits in every frame")
            .arg(channel);
    }
    return {};
}

QList<LayoutClash> findLayoutClashes(const CommsSection &section)
{
    QList<LayoutClash> clashes;
    if (section.isRelay() || section.device == SectionDevice::Off)
        return clashes; // carries no channels the device reads or writes

    if (section.compound) {
        for (int i = 0; i < section.identifiers.size(); ++i) {
            const CompoundIdentifier &ident = section.identifiers.at(i);
            if (!ident.configured && ident.rows.isEmpty())
                continue; // an unused slot
            clashesIn(section, ident.rows, i, &clashes);
        }
        // section.rows in compound mode are ignored on Send — validation says so
        // in its own words — so they are not laid out and cannot collide.
        return clashes;
    }

    clashesIn(section, section.rows, -1, &clashes);
    return clashes;
}

bool hasBlockingLayoutClash(const CommsSection &section)
{
    for (const LayoutClash &clash : findLayoutClashes(section))
        if (clash.blocking)
            return true;
    return false;
}

} // namespace ct
