#include "validation.h"

#include <algorithm>
#include <bitset>
#include <utility>

#include <QHash>
#include <QSet>

#include "config_report.h"
#include "configuration.h"
#include "device_mapper.h"

namespace ct {

QList<ValidationIssue> validateConfiguration(const Configuration &config)
{
    QList<ValidationIssue> issues;
    auto add = [&](ValidationIssue::Severity sev, const QString &loc, const QString &msg) {
        issues.append({sev, loc, msg});
    };

    // A message marked Hidden or Protect Communication gives up its detail only
    // to a viewer holding the password for THAT SECTION, given this session.
    // Configuration::isSectionRevealed() is the one answer every suppression
    // site here asks for, and it is asked PER SECTION rather than hoisted,
    // because it differs section by section. A marked section with no password
    // of its own gives up nothing to anybody: that predicate fails closed, since
    // no password for such a section exists to be held.
    //
    // READ ONLY IS NOT SUPPRESSED ANYWHERE BELOW, and that is deliberate rather
    // than an omission. isConcealed() is false for it, so its findings are
    // reported in full, its CAN ID is printed, and it may be NAMED as the owner
    // in a duplicate-ID clash. A Read Only message is visible by definition;
    // collapsing its findings would hide a fault the reader is entitled to see
    // and able to fix, and would tell them nothing they could not read off the
    // sections list anyway.

    // Per-section checks
    //
    // Whether the message that claimed an ID is concealed is remembered here
    // rather than looked up again when a clash is found: by then the only
    // handle on the earlier message is its name, and its name is precisely what
    // must not be printed when it turns out to be a protected one.
    struct IdOwner { QString name; bool concealed = false; };
    QHash<QString, IdOwner> idUsers; // "bus|ext|id" -> the section that claimed it
    // Transmit CRC8 sections, counted ACROSS the buses: the device's CRC8 table
    // is one table of MAX_CRC8_MESSAGES rules, not one per bus, so a per-bus
    // count would pass three buses of 15 stamps each. Reported after the loop.
    int crc8Sections = 0;
    for (int busIdx = 0; busIdx < 3; ++busIdx) {
        const BusConfig &bus = config.bus[busIdx];
        if (!bus.enabled) {
            for (const CommsSection &s : bus.sections)
                if (s.device != SectionDevice::Off) {
                    add(ValidationIssue::Warning, QStringLiteral("CAN %1").arg(busIdx + 1),
                        QStringLiteral("bus mode is Off — its messages stay inactive until "
                                       "Mode is set to CAN in Connections > Communications"));
                    break;
                }
        }
        // Almost every message-level finding names something a protected
        // message exists to withhold — its CAN ID, its frame length, a row's
        // start bit and width. Rather than sanitising each one (where the next
        // check added would simply forget), the whole section's findings are
        // produced normally and then collapsed into a single entry that says a
        // problem exists without saying what it is.
        const auto checkSection = [&](const CommsSection &s) {
            const QString loc = QStringLiteral("CAN %1 · %2").arg(busIdx + 1).arg(s.name);
            if (s.device == SectionDevice::Off)
                return;

            const quint32 maxId = s.extended ? 0x1FFFFFFFu : 0x7FFu;
            if (s.baseAddress > maxId)
                add(ValidationIssue::Error, loc,
                    QStringLiteral("CAN ID 0x%1 exceeds the %2 range")
                        .arg(QString::number(s.baseAddress, 16).toUpper(),
                             s.extended ? QStringLiteral("extended (29-bit)")
                                        : QStringLiteral("standard (11-bit)")));

            if (s.isRelay()) {
                // A relay is a whole-frame gateway rule, not a message: it has no
                // channels, length or timing. Check only its forwarding.
                if ((s.routeBusMask & 0x7 & ~(1 << busIdx)) == 0)
                    add(ValidationIssue::Error, loc,
                        QStringLiteral("message relay forwards to no bus — select at least one "
                                       "target bus"));
                if (s.routeBusMask & (1 << busIdx))
                    add(ValidationIssue::Warning, loc,
                        QStringLiteral("relay forward mask includes its own bus (ignored by "
                                       "the device)"));
                for (int t = 0; t < 3; ++t)
                    if ((s.routeBusMask & (1 << t)) && t != busIdx && !config.bus[t].enabled)
                        add(ValidationIssue::Warning, loc,
                            QStringLiteral("relays to CAN %1 whose mode is Off — forwarded "
                                           "frames would be dropped").arg(t + 1));
                if (s.relayBitmask == 0 && s.relayInvert)
                    add(ValidationIssue::Warning, loc,
                        QStringLiteral("bitmask 0 with Invert Result matches no frame — nothing "
                                       "is forwarded"));
                return; // skip all message/channel checks below
            }

            if (s.isReceive()) {
                const QString key = QStringLiteral("%1|%2|%3")
                                        .arg(busIdx).arg(s.extended ? 1 : 0).arg(s.baseAddress);
                const auto owner = idUsers.constFind(key);
                if (owner == idUsers.constEnd()) {
                    idUsers.insert(
                        key, IdOwner{s.name, s.isConcealed(config.isSectionRevealed(s, busIdx))});
                } else if (owner->concealed) {
                    // The clash is still reported — a duplicate ID makes the
                    // configuration wrong and the user has to know that even
                    // when they may not be told why — but the message that got
                    // there first is not named and its CAN ID is not printed.
                    // Without that suppression this check is an oracle: add a
                    // message, watch for this error, and a protected
                    // configuration gives up its addressing one guess at a
                    // time, which is most of what protecting a protocol was
                    // supposed to stop.
                    add(ValidationIssue::Error, loc,
                        QStringLiteral("this CAN ID is already used by a withheld message on "
                                       "this bus, and only the first message would match — "
                                       "open that message in Connections > Communications "
                                       "with its password to see which, or ask whoever "
                                       "supplied this configuration"));
                } else {
                    add(ValidationIssue::Error, loc,
                        QStringLiteral("duplicate CAN ID 0x%1 on this bus (also used by %2) — "
                                       "only the first message would match")
                            .arg(QString::number(s.baseAddress, 16).toUpper(), owner->name));
                }
            }

            static const QList<int> kFdLengths = {0, 1, 2, 3, 4, 5, 6, 7, 8,
                                                  12, 16, 20, 24, 32, 48, 64};
            if (s.fd) {
                if (!kFdLengths.contains(s.messageLengthBytes))
                    add(ValidationIssue::Error, loc,
                        QStringLiteral("message length must be 0–8, 12, 16, 20, 24, 32, 48 or "
                                       "64 bytes (CAN FD)"));
                // isFd() is the device's own test: the firmware brings a bus up
                // in FD only when the data rate EXCEEDS the base rate, so a
                // hand-edited equal-or-below data rate runs classic no matter
                // what the field says. The dialog cannot build one — its FD
                // choices at or below the base rate are disabled.
                if (!bus.isFd())
                    add(ValidationIssue::Warning, loc,
                        bus.dataRateKbps > 0
                            ? QStringLiteral("CAN FD frame, but the bus's FD data rate (%1k) "
                                             "does not exceed its base rate (%2k) — the device "
                                             "runs this bus classic")
                                  .arg(busRateLabel(bus.dataRateKbps),
                                       busRateLabel(bus.rateKbps))
                            : QStringLiteral("CAN FD frame on a bus without an FD data rate "
                                             "(set one in CAN Bus Setup)"));
            } else if (s.messageLengthBytes < 0 || s.messageLengthBytes > 8) {
                add(ValidationIssue::Error, loc,
                    QStringLiteral("message length must be 0–8 bytes (classic CAN — "
                                   "enable CAN FD for longer frames)"));
            }

            if (s.routeEnable) {
                if ((s.routeBusMask & 0x7) == 0)
                    add(ValidationIssue::Warning, loc,
                        QStringLiteral("routing enabled but no target bus selected"));
                if (s.routeBusMask & (1 << busIdx))
                    add(ValidationIssue::Warning, loc,
                        QStringLiteral("route mask includes the source bus (ignored by the device)"));
                for (int t = 0; t < 3; ++t)
                    if ((s.routeBusMask & (1 << t)) && t != busIdx && !config.bus[t].enabled)
                        add(ValidationIssue::Warning, loc,
                            QStringLiteral("routes to CAN %1 whose mode is Off — routed "
                                           "frames would be dropped").arg(t + 1));
            }

            // Row checks + overlap detection. Occupancy is a physical-bit
            // set: a contiguous big-endian mask is NOT contiguous in linear
            // numbering, so spans would over/under-report. rowBitPositions()
            // does the walk — shared with the section editor's frame layout
            // map, so what the user is shown and what is flagged here can
            // never drift apart. Sized for the largest CAN FD frame
            // (64 bytes = 512 bits).
            struct Occupancy { std::bitset<MAX_FRAME_BITS> bits; QString channel; };
            const auto occupancyOf = [&](const CommsChannelRow &row) {
                std::bitset<MAX_FRAME_BITS> bits;
                for (int pos : rowBitPositions(row, s.alignment))
                    bits.set(size_t(pos));
                return bits;
            };
            const auto checkRows = [&](const QList<CommsChannelRow> &rows, const QString &subLoc,
                                       QList<Occupancy> *collect) {
                for (const CommsChannelRow &row : rows) {
                    const QString rloc = subLoc.isEmpty()
                                             ? QStringLiteral("%1 · %2").arg(loc, row.channelName)
                                             : QStringLiteral("%1 · %2 · %3").arg(loc, subLoc, row.channelName);
                    if (row.channelName.isEmpty()) {
                        add(ValidationIssue::Error, rloc, QStringLiteral("no channel selected"));
                        continue;
                    }
                    ExtractionFields fields;
                    QString reason;
                    bool extractable =
                        computeExtraction(row, s.alignment, s.messageLengthBytes, &fields, &reason);
                    if (!extractable)
                        add(ValidationIssue::Error, rloc, reason);
                    if (row.dbcFactor == 0.0)
                        add(ValidationIssue::Warning, rloc,
                            QStringLiteral("bit resolution is zero — the channel is fixed at "
                                           "the offset"));
                    if (!row.clampToRange) {
                        // Two ways the roll-over box can be ticked on a row that
                        // will not honour it. Neither is reachable through the
                        // editor — it offers the box only on a transmit row, and
                        // the preview goes quiet on IEEE754 — but a .ct3 is
                        // hand-editable text and a Get can hand back a foreign
                        // configuration, so both are worth naming rather than
                        // letting the device quietly disagree with the file.
                        if (s.isReceive())
                            add(ValidationIssue::Warning, rloc,
                                QStringLiteral("set to roll over, but this is a receive row — "
                                               "the device always clamps on receive and will "
                                               "ignore it"));
                        else if (row.dbcType == int(DbcType::IEEE754))
                            add(ValidationIssue::Warning, rloc,
                                QStringLiteral("set to roll over, but an IEEE754 field has no "
                                               "range to roll over; only the channel's own "
                                               "clamp is skipped"));
                    }
                    if (!config.catalog().findByName(row.channelName).isValid())
                        add(ValidationIssue::Warning, rloc,
                            QStringLiteral("channel is not in the catalogue (base resolution "
                                           "assumed 1)"));
                    if (extractable && collect)
                        collect->append({occupancyOf(row), row.channelName});
                }
            };
            // Report overlapping fields within `occ`.
            const auto reportOverlaps = [&](const QList<Occupancy> &occ) {
                for (int i = 0; i < occ.size(); ++i)
                    for (int j = i + 1; j < occ.size(); ++j)
                        if ((occ[i].bits & occ[j].bits).any())
                            add(ValidationIssue::Warning, loc,
                                QStringLiteral("'%1' and '%2' overlap in the frame")
                                    .arg(occ[i].channel, occ[j].channel));
            };

            if (s.compound) {
                // Compound sections carry channels only inside identifiers — each
                // is a mutually-exclusive frame variant, so different identifiers
                // may legitimately reuse the same bits (overlap checked per one).
                if (!s.rows.isEmpty())
                    add(ValidationIssue::Warning, loc,
                        QStringLiteral("compound section has %1 channel(s) outside any "
                                       "identifier — they are ignored on Send; add them to "
                                       "an identifier").arg(s.rows.size()));
                for (int i = 0; i < s.identifiers.size(); ++i) {
                    const CompoundIdentifier &ident = s.identifiers[i];
                    if (!ident.configured && ident.rows.isEmpty())
                        continue; // unused slot
                    const QString iloc = QStringLiteral("%1 · ID %2").arg(loc).arg(i + 1);
                    if (ident.idMask == 0)
                        add(ValidationIssue::Warning, iloc,
                            QStringLiteral("identifier mask is 0 — this sub-message is "
                                           "skipped on Send (its channels are not sent to "
                                           "the device); set a non-zero mask"));
                    if (ident.byteOffset < 0 || ident.byteOffset >= s.messageLengthBytes)
                        add(ValidationIssue::Warning, iloc,
                            QStringLiteral("identifier offset %1 is outside the %2-byte message "
                                           "— the selector reads zero there")
                                .arg(ident.byteOffset).arg(s.messageLengthBytes));
                    QList<Occupancy> active;
                    checkRows(ident.rows, QStringLiteral("ID %1").arg(i + 1), &active);
                    reportOverlaps(active);
                }
            } else {
                QList<Occupancy> occ;
                checkRows(s.rows, QString(), &occ);
                reportOverlaps(occ);
            }

            if (s.isTransmit()) {
                if (s.transmitRateHz < 1 || s.transmitRateHz > 200)
                    add(ValidationIssue::Error, loc,
                        QStringLiteral("transmit rate must be 1–200 Hz"));
                // Triggered transmit. Errors, not warnings, in both directions:
                // a message set to speak only on a condition and given no usable
                // condition would otherwise map to one that transmits
                // continuously, which is the opposite of what was configured and
                // puts frames on a bus nobody asked for.
                if (!s.cyclic) {
                    if (s.transmitCondition.isEmpty()) {
                        add(ValidationIssue::Error, loc,
                            QStringLiteral("Triggered transmission with no User Condition "
                                           "selected"));
                    } else {
                        bool found = false;
                        for (const ConditionRow &c : config.conditionRows) {
                            if (c.active
                                && c.outputChannel.compare(s.transmitCondition,
                                                           Qt::CaseInsensitive) == 0) {
                                found = true;
                                break;
                            }
                        }
                        if (!found)
                            add(ValidationIssue::Error, loc,
                                QStringLiteral("transmit condition '%1' is not the output of "
                                               "any active User Condition")
                                    .arg(s.transmitCondition));
                    }
                } else if (!s.transmitCondition.isEmpty()) {
                    // Harmless — the mapper ignores the field on a Cyclic
                    // message — but worth saying, because it is still on
                    // screen and reads as if it were doing something.
                    add(ValidationIssue::Info, loc,
                        QStringLiteral("transmission is Cyclic, so the transmit condition is "
                                       "not used"));
                }
                if (s.compound) {
                    bool anyIdentRows = false;
                    for (const CompoundIdentifier &ident : s.identifiers)
                        if (!ident.rows.isEmpty()) {
                            anyIdentRows = true;
                            break;
                        }
                    if (!anyIdentRows) {
                        // Still worth saying, but it no longer means one blank
                        // frame: every CONFIGURED identifier now emits its own
                        // variant, carrying its selector over a zeroed payload.
                        // That is a legitimate configuration — a request or ping
                        // frame is exactly this — so the warning says what will
                        // go out rather than implying a mistake.
                        int declared = 0;
                        for (const CompoundIdentifier &ident : s.identifiers)
                            if (ident.configured && ident.idMask != 0)
                                ++declared;
                        if (declared > 0)
                            add(ValidationIssue::Info, loc,
                                QStringLiteral("no identifier has channels — each period the "
                                               "device sends %1 frame(s) carrying only their "
                                               "selectors, over an all-zero payload")
                                    .arg(declared));
                        else
                            add(ValidationIssue::Warning, loc,
                                QStringLiteral("compound transmit message has no identifier "
                                               "channels and no usable identifier — the device "
                                               "transmits an empty (all-zero) frame each "
                                               "period"));
                    }

                    // Two identifiers that select the same value ARE one
                    // variant, and the device says so by sending one frame
                    // where the author expected several. Nothing downstream can
                    // separate them either: the engine collects DISTINCT
                    // (byte, masked id, mask) triples, and a receiver reading
                    // the selector out of the frame has exactly the same
                    // problem. Compared on the MASKED value, because that is
                    // what both the gate and the wire actually carry.
                    for (int i = 0; i < s.identifiers.size(); ++i) {
                        const CompoundIdentifier &a = s.identifiers[i];
                        if (!a.configured || a.rows.isEmpty())
                            continue;
                        for (int j = i + 1; j < s.identifiers.size(); ++j) {
                            const CompoundIdentifier &b = s.identifiers[j];
                            if (!b.configured || b.rows.isEmpty())
                                continue;
                            if (a.byteOffset != b.byteOffset || a.idMask != b.idMask)
                                continue;
                            if ((a.id & a.idMask) != (b.id & b.idMask))
                                continue;
                            add(ValidationIssue::Error, loc,
                                QStringLiteral("compound identifiers %1 and %2 both select "
                                               "0x%3 — the device treats them as ONE variant "
                                               "and sends a single frame, not one per "
                                               "identifier")
                                    .arg(i + 1)
                                    .arg(j + 1)
                                    .arg(a.id & a.idMask, 2, 16, QLatin1Char('0')));
                        }
                    }

                    // The selector is written into the frame LAST, after the
                    // channels, so a channel sharing those bits is silently
                    // overwritten by the identifier value — the frame goes out
                    // carrying the selector where the data should be, which
                    // reads on a trace as a channel that is always zero.
                    for (int i = 0; i < s.identifiers.size(); ++i) {
                        const CompoundIdentifier &ident = s.identifiers[i];
                        if (!ident.configured)
                            continue;
                        // The selector window is the 2 bytes at byteOffset; only
                        // the bits the mask names are actually written.
                        const int selFirst = ident.byteOffset * 8;
                        for (const CommsChannelRow &row : ident.rows) {
                            const int rowFirst = row.startBit;
                            const int rowLast = row.startBit + qMax(1, row.bitLength) - 1;
                            bool clash = false;
                            for (int bit = 0; bit < 16 && !clash; ++bit) {
                                if (!((ident.idMask >> bit) & 1u))
                                    continue;
                                const int selBit = selFirst + bit;
                                clash = selBit >= rowFirst && selBit <= rowLast;
                            }
                            if (clash)
                                add(ValidationIssue::Error, loc,
                                    QStringLiteral("identifier %1 writes its selector over "
                                                   "channel '%2' — the selector is written "
                                                   "last, so that channel arrives as the "
                                                   "identifier value instead of its own")
                                        .arg(i + 1)
                                        .arg(row.channelName));
                        }
                    }
                }
            }
            if (s.isCrc8()) {
                ++crc8Sections;
                // The recipe's own coherence. These sit inside checkSection on
                // purpose: a concealed section's CRC findings collapse with the
                // rest of its detail, since a byte location or an element index
                // is exactly the kind of protocol fact concealment withholds.
                if (s.crcChannel.isEmpty())
                    add(ValidationIssue::Error, loc,
                        QStringLiteral("no CRC channel selected — the checksum has no channel "
                                       "to publish to"));
                if (s.crcByteLocation < 0 || s.crcByteLocation >= s.messageLengthBytes)
                    add(ValidationIssue::Error, loc,
                        QStringLiteral("CRC byte location %1 is outside the %2-byte message — "
                                       "the stamp has no byte to land in")
                            .arg(s.crcByteLocation).arg(s.messageLengthBytes));
                // A channel row packed into the CRC's byte is not an error the
                // way two rows overlapping is — the section still maps — but the
                // stamp runs LAST, so whatever the row put there leaves the
                // device overwritten. allRows(): on a compound section every
                // variant frame is stamped, so identifier rows are just as
                // exposed as the always-present ones.
                for (const CommsChannelRow &row : s.allRows()) {
                    bool touchesCrcByte = false;
                    for (int pos : rowBitPositions(row, s.alignment))
                        touchesCrcByte = touchesCrcByte || pos / 8 == s.crcByteLocation;
                    if (touchesCrcByte)
                        add(ValidationIssue::Warning, loc,
                            QStringLiteral("'%1' occupies byte %2, where the CRC is stamped — "
                                           "the stamp overwrites those bits in every frame")
                                .arg(row.channelName).arg(s.crcByteLocation));
                }
                // An element-less recipe maps and runs — the engine stamps the
                // constant init/final-XOR value — but a checksum that checks
                // nothing is far more often a recipe someone stopped halfway
                // through, so it is worth a warning that names what the device
                // will actually do.
                if (s.crcElements.isEmpty())
                    add(ValidationIssue::Warning, loc,
                        QStringLiteral("element count is 0 — nothing feeds the checksum, so "
                                       "the stamp is the constant init/final-XOR value in "
                                       "every frame"));
                // Element sanity. Both are warnings, not errors: the firmware
                // computes exactly what the recipe says, and some protocols
                // genuinely feed the pre-stamp byte — but each is far more
                // often a mis-typed index than a choice, so it is said out loud.
                for (int e = 0; e < s.crcElements.size(); ++e) {
                    const CommsSection::CrcElement &el = s.crcElements[e];
                    if (el.type != CommsSection::CrcElement::Data)
                        continue;
                    if (el.value >= s.messageLengthBytes)
                        add(ValidationIssue::Warning, loc,
                            QStringLiteral("CRC element %1 reads frame byte %2, past the "
                                           "%3-byte message — it feeds 0 into the checksum")
                                .arg(e + 1).arg(el.value).arg(s.messageLengthBytes));
                    else if (el.value == s.crcByteLocation)
                        add(ValidationIssue::Warning, loc,
                            QStringLiteral("CRC element %1 reads frame byte %2, the CRC's own "
                                           "byte — the stamp runs last, so this feeds the "
                                           "pre-stamp value, not the checksum")
                                .arg(e + 1).arg(el.value));
                }
            }
            if (s.allRows().isEmpty())
                add(ValidationIssue::Warning, loc, QStringLiteral("message has no channels"));
        };

        for (const CommsSection &s : bus.sections) {
            const int firstIssue = issues.size();
            checkSection(s);
            // Per section: one unlocked with its own password this session
            // reports in full while its neighbours stay collapsed. Read Only
            // never reaches the collapse at all.
            if (!s.isConcealed(config.isSectionRevealed(s, busIdx)) || issues.size() == firstIssue)
                continue;

            // Collapse to one entry. The SEVERITY is kept — a withheld message
            // with an Error still blocks Send, because a customer must know the
            // configuration cannot be used even when they cannot be told why.
            // Only the detail goes.
            ValidationIssue::Severity worst = ValidationIssue::Info;
            for (int i = firstIssue; i < issues.size(); ++i)
                if (issues[i].severity < worst) // Error < Warning < Info
                    worst = issues[i].severity;
            const int count = issues.size() - firstIssue;
            issues.erase(issues.begin() + firstIssue, issues.end());
            add(worst, QStringLiteral("CAN %1 · %2").arg(busIdx + 1).arg(s.name),
                QStringLiteral("this message reports %1 problem(s), and it is withheld — open "
                               "it in Connections > Communications with its password to see "
                               "them, or ask whoever supplied this configuration").arg(count));
        }
    }

    // One device table for every bus's stamps — see the counter's declaration.
    // Named the way the preserved-values overflow is: the total, the limit, and
    // how many to take back, because "over capacity" without a number sends the
    // user counting sections by hand.
    if (crc8Sections > MAX_CRC8_MESSAGES)
        add(ValidationIssue::Error, QStringLiteral("Transmit CRC8"),
            QStringLiteral("%1 messages stamp a CRC8, but the device runs at most %2 CRC8 "
                           "rules across all buses — remove the checksum from %3 of them")
                .arg(crc8Sections).arg(MAX_CRC8_MESSAGES)
                .arg(crc8Sections - MAX_CRC8_MESSAGES));

    // ---------------------------------------------------------------------
    // The one channel-level conflict that matters: two things WRITING the same
    // channel. The device has a single slot per channel, so two writers
    // overwrite each other and the value everything else reads depends on
    // evaluation order.
    //
    // Nothing below counts a REFERENCE. Transmitting a channel, feeding it to
    // math / a condition / a counter / a timer / an integrator / a table axis
    // all READ the slot and leave it exactly as they found it, so any number of
    // sites may reference the same channel and no combination of readers is a
    // conflict. Only the write side is collected here.
    QHash<QString, QStringList> writers;
    QHash<QString, QString> writerDisplay; // lower-cased key -> name as the user typed it
    const auto addWriter = [&](const QString &name, const QString &where) {
        if (name.isEmpty())
            return;
        const QString k = name.toLower();
        writers[k].append(where);
        if (!writerDisplay.contains(k))
            writerDisplay.insert(k, name);
    };
    for (int busIdx = 0; busIdx < 3; ++busIdx)
        for (const CommsSection &s : config.bus[busIdx].sections)
            if (s.isReceive() && s.device != SectionDevice::Off) {
                // Count each channel at most once per section — a compound
                // section may legitimately define the same channel in several
                // identifiers (one per multiplexor value).
                QSet<QString> seen;
                for (const QString &n : s.channelNames()) {
                    const QString k = n.toLower();
                    if (seen.contains(k))
                        continue;
                    seen.insert(k);
                    addWriter(n, QStringLiteral("CAN %1 · %2").arg(busIdx + 1).arg(s.name));
                }
            }
    // The firmware writes its own channels. Registering them as writers is what
    // turns "a math row also writes Device OnTime" into the two-writers warning
    // it deserves — the device wins that race every tick, so the row would
    // appear to do nothing.
    for (const Channel &c : ChannelCatalog::deviceChannels())
        addWriter(c.name, QStringLiteral("the device"));
    // The CRC channel is device-written too: the transmit composer publishes
    // every stamped frame's checksum into its slot. Collected exactly like a
    // receive row's channel so a calculation also writing the name earns the
    // shared-slot warning — the composer re-stamps on every transmit, so the
    // calculation's value would never be seen.
    for (int busIdx = 0; busIdx < 3; ++busIdx)
        for (const CommsSection &s : config.bus[busIdx].sections)
            if (s.isCrc8())
                addWriter(s.crcChannel,
                          QStringLiteral("CAN %1 · %2 (CRC)").arg(busIdx + 1).arg(s.name));
    for (int i = 0; i < config.mathRows.size(); ++i)
        if (config.mathRows[i].active)
            addWriter(config.mathRows[i].destChannel, QStringLiteral("Math %1").arg(i + 1));
    for (int i = 0; i < config.conditionRows.size(); ++i)
        if (config.conditionRows[i].active)
            addWriter(config.conditionRows[i].outputChannel,
                      QStringLiteral("User Condition %1").arg(i + 1));
    for (int i = 0; i < config.counterRows.size(); ++i)
        if (config.counterRows[i].active)
            addWriter(config.counterRows[i].outputChannel,
                      QStringLiteral("Counter %1").arg(i + 1));
    for (int i = 0; i < config.timerRows.size(); ++i)
        if (config.timerRows[i].active)
            addWriter(config.timerRows[i].outputChannel, QStringLiteral("Timer %1").arg(i + 1));
    for (int i = 0; i < config.integratorRows.size(); ++i)
        if (config.integratorRows[i].active)
            addWriter(config.integratorRows[i].outputChannel,
                      QStringLiteral("Integrator %1").arg(i + 1));
    for (int i = 0; i < config.constantRows.size(); ++i)
        if (config.constantRows[i].active)
            addWriter(config.constantRows[i].name, QStringLiteral("Constant %1").arg(i + 1));
    // Tables write their output channel too. Skipped when they generate nothing
    // on the device, matching generatedChannelNames() and the mapper: an
    // inactive table, or one with no sites.
    for (int i = 0; i < config.table2x16Rows.size(); ++i) {
        const Table2x16Row &t = config.table2x16Rows[i];
        if (t.active && !t.xSites.isEmpty())
            addWriter(t.outputChannel, QStringLiteral("Table 2x16 %1").arg(i + 1));
    }
    for (int i = 0; i < config.table8x8Rows.size(); ++i) {
        const Table8x8Row &t = config.table8x8Rows[i];
        if (t.active && !t.xSites.isEmpty() && !t.ySites.isEmpty())
            addWriter(t.outputChannel, QStringLiteral("Table 8x8 %1").arg(i + 1));
    }
    // Sorted, so two runs of the same document report in the same order rather
    // than in QHash bucket order.
    QStringList writtenKeys = writers.keys();
    writtenKeys.sort();
    for (const QString &k : std::as_const(writtenKeys)) {
        const QStringList &where = writers.value(k);
        if (where.size() > 1)
            add(ValidationIssue::Warning, where.first(),
                QStringLiteral("channel '%1' is written by %2 things at once (%3) — they share "
                               "one slot on the device and overwrite each other, so whichever "
                               "runs last wins")
                    .arg(writerDisplay.value(k, k))
                    .arg(where.size())
                    .arg(where.join(QStringLiteral(", "))));
    }

    // ---------------------------------------------------------------------
    // Reference checks. Referencing a channel reads its value; it never writes
    // it, so ANY channel may be referenced anywhere and doing so is never an
    // error. The one thing worth saying is that a channel nothing writes reads
    // its default value forever — Info, so it neither blocks Send nor competes
    // with the write conflict above, which is the warning that matters.
    const QSet<QString> generated = [&] {
        QSet<QString> set;
        // generatedChannelNames() includes the CRC publish channels: the
        // checksum's producer is the DEVICE, but the model lists it the way it
        // lists device channels, so every consumer — this set, the pickers,
        // the Lua listing — agrees that a row reading one is not reading a
        // default. No local re-walk here; there used to be one, and two lists
        // that must agree is how they stop agreeing.
        for (const QString &n : config.generatedChannelNames())
            set.insert(n.toLower());
        return set;
    }();
    const auto noteReference = [&](const QString &name, const QString &loc,
                                   const QString &label) {
        if (!name.isEmpty() && !generated.contains(name.toLower()))
            add(ValidationIssue::Info, loc,
                QStringLiteral("%1 channel '%2' has no generator yet — it reads its default "
                               "value until a receive row or a calculation writes it")
                    .arg(label, name));
    };
    for (int i = 0; i < config.mathRows.size(); ++i) {
        const MathRow &m = config.mathRows[i];
        if (!m.active)
            continue;
        const QString loc = QStringLiteral("Math %1").arg(i + 1);
        // A blank input set to "channel" is an unfinished row, which is still an
        // error; a filled-in one is a reference, which never is. Only the
        // operands the op actually reads (mathOpArity) are looked at — an
        // unused operand can hold anything without affecting the device.
        const int arity = mathOpArity(m.op);
        if (m.aIsChannel && m.aChannel.isEmpty())
            add(ValidationIssue::Error, loc, QStringLiteral("no input A channel selected"));
        else if (m.aIsChannel)
            noteReference(m.aChannel, loc, QStringLiteral("input A"));
        if (arity >= 2) {
            if (m.bIsChannel && m.bChannel.isEmpty())
                add(ValidationIssue::Error, loc, QStringLiteral("no input B channel selected"));
            else if (m.bIsChannel)
                noteReference(m.bChannel, loc, QStringLiteral("input B"));
        }
        if (arity >= 3) {
            if (m.cIsChannel && m.cChannel.isEmpty())
                add(ValidationIssue::Error, loc, QStringLiteral("no input C channel selected"));
            else if (m.cIsChannel)
                noteReference(m.cChannel, loc, QStringLiteral("input C"));
        }
        if (m.destChannel.isEmpty())
            add(ValidationIssue::Error, loc, QStringLiteral("no destination channel"));
    }
    for (int i = 0; i < config.conditionRows.size(); ++i) {
        const ConditionRow &c = config.conditionRows[i];
        if (!c.active)
            continue;
        const QString loc = QStringLiteral("User Condition %1").arg(i + 1);
        // Both expressions, each checked the same way and each named in its own
        // findings — a Reset with a dangling reference is a latch that never
        // clears, which is worth as much of the user's attention as a broken Set.
        //
        // A Momentary's Reset half is NOT checked: it is kept in the document so
        // switching modes does not destroy what was typed, but it reaches no
        // device and a half-finished expression there is not a fault.
        struct ExprToCheck {
            const QList<ConditionTermRow> *terms;
            const QList<int> *joiners;
            const char *label;
        };
        QList<ExprToCheck> exprs{{&c.setTerms, &c.setJoiners, "Set"}};
        if (c.mode == ConditionMode::SetReset)
            exprs.append({&c.resetTerms, &c.resetJoiners, "Reset"});

        for (const ExprToCheck &e : exprs) {
            const QString side = QString::fromLatin1(e.label) + QLatin1Char(' ');
            for (int t = 0; t < e.terms->size(); ++t) {
                const ConditionTermRow &term = e.terms->at(t);
                const QString which = e.terms->size() > 1
                                          ? QStringLiteral("comparison %1: ").arg(t + 1)
                                          : QString();
                if (term.isMessageOp()) {
                    // A message operand is not a channel reference, so it is
                    // checked against the document's messages instead. An empty
                    // one is a comparison the user never finished.
                    if (term.aMessage.isEmpty()) {
                        add(ValidationIssue::Error, loc,
                            QStringLiteral("%1%2no message selected").arg(side, which));
                    } else {
                        bool found = false;
                        const int b = term.aMessageBus - 1;
                        if (b >= 0 && b <= 2)
                            for (const CommsSection &ms : config.bus[b].sections)
                                if (ms.device != SectionDevice::Off && !ms.isRelay()
                                    && ms.name.compare(term.aMessage, Qt::CaseInsensitive) == 0) {
                                    found = true;
                                    break;
                                }
                        if (!found)
                            add(ValidationIssue::Error, loc,
                                QStringLiteral("%1%2CAN %3 has no message named '%4'")
                                    .arg(side, which)
                                    .arg(term.aMessageBus)
                                    .arg(term.aMessage));
                    }
                    continue;
                }
                // An EMPTY left side is still an error — that is a comparison
                // the user never finished, not a reference to something
                // ungenerated.
                if (term.aChannel.isEmpty())
                    add(ValidationIssue::Error, loc,
                        QStringLiteral("%1%2no input channel selected").arg(side, which));
                else
                    noteReference(term.aChannel, loc, side + which + QStringLiteral("input"));
                if (term.bIsChannel)
                    noteReference(term.bChannel, loc, side + which + QStringLiteral("input"));
            }
            if (e.terms->isEmpty())
                add(ValidationIssue::Error, loc,
                    QStringLiteral("%1expression has no comparisons").arg(side));
            if (e.joiners->size() != qMax(0, int(e.terms->size()) - 1))
                add(ValidationIssue::Error, loc,
                    QStringLiteral("%1expression is malformed (%2 comparisons but %3 AND/OR "
                                   "joins)")
                        .arg(side).arg(e.terms->size()).arg(e.joiners->size()));
        }
        if (c.mode == ConditionMode::Momentary
            && (c.latchHz < 1 || c.latchHz > int(COND_LATCH_MAX_HZ)))
            add(ValidationIssue::Error, loc,
                QStringLiteral("latch frequency must be 1–%1 Hz").arg(int(COND_LATCH_MAX_HZ)));
        if (c.outputChannel.isEmpty())
            add(ValidationIssue::Error, loc, QStringLiteral("no output channel"));
    }

    const auto &checkInput = noteReference; // trigger/enable/reset inputs are references
    for (int i = 0; i < config.counterRows.size(); ++i) {
        const CounterRow &c = config.counterRows[i];
        if (!c.active)
            continue;
        const QString loc = QStringLiteral("Counter %1").arg(i + 1);
        if (c.outputChannel.isEmpty())
            add(ValidationIssue::Error, loc, QStringLiteral("no output channel"));
        // A rate counter does not read the three counting inputs, and the
        // mapper drops them from the record. Walking them here anyway would
        // report the counter as referencing channels it demonstrably does not,
        // which is how an unused channel stays alive in Check Channels forever.
        const bool isRate = (c.mode == ct::COUNTER_MODE_RATE);
        if (!isRate) {
            checkInput(c.upChannel, loc, QStringLiteral("up"));
            checkInput(c.downChannel, loc, QStringLiteral("down"));
            checkInput(c.followChannel, loc, QStringLiteral("follow"));
        }
        checkInput(c.resetChannel, loc, QStringLiteral("reset"));
        checkInput(c.enableChannel, loc, QStringLiteral("enable"));
        if (c.mode == ct::COUNTER_MODE_UPDOWN && c.upChannel.isEmpty() && c.downChannel.isEmpty())
            add(ValidationIssue::Warning, loc,
                QStringLiteral("up/down counter has neither an Up nor a Down channel"));
        if (c.mode == ct::COUNTER_MODE_FOLLOW && c.followChannel.isEmpty())
            add(ValidationIssue::Warning, loc,
                QStringLiteral("follow-changes counter has no Follow channel"));
        if (isRate && !std::count(std::begin(ct::kCounterRateChoices),
                                  std::end(ct::kCounterRateChoices), c.rateHz))
            add(ValidationIssue::Warning, loc,
                QStringLiteral("rate of %1 Hz is not one of the offered rates").arg(c.rateHz));
        if (c.maxValue <= c.minValue)
            add(ValidationIssue::Warning, loc,
                QStringLiteral("maximum must exceed minimum"));
    }

    // "Preserve value" retains a value across power cycles in a small flash
    // store that holds at most 20 values TOTAL — counters and integrators share
    // one ring, so the budget has to be counted across both. Beyond that the
    // device keeps the counters and drops the excess integrators, which would
    // be a silent surprise, so it is an error here instead.
    {
        int counters = 0, integrators = 0;
        for (const CounterRow &c : config.counterRows)
            if (c.active && c.preserveValue)
                ++counters;
        for (const IntegratorRow &g : config.integratorRows)
            if (g.active && g.preserveValue)
                ++integrators;
        const int preserved = counters + integrators;
        constexpr int kMaxPreserved = 20;
        if (preserved > kMaxPreserved)
            add(ValidationIssue::Error, QStringLiteral("Preserved values"),
                QStringLiteral("%1 items have Preserve value enabled (%2 counters + %3 "
                               "integrators), but the device retains at most %4 across power "
                               "cycles — turn Preserve off on %5 of them")
                    .arg(preserved).arg(counters).arg(integrators).arg(kMaxPreserved)
                    .arg(preserved - kMaxPreserved));
        // Wear note. A counter is event-driven and often unchanged for a whole
        // minute, so a flush costs nothing; a running integrator changes every
        // step and so writes a record at essentially every 60 s flush. Worth
        // saying out loud, because the two look identical in the UI.
        if (integrators > 0)
            add(ValidationIssue::Info, QStringLiteral("Preserved values"),
                QStringLiteral("%1 preserved integrator(s) write to the retained-value store on "
                               "almost every 60 s flush, since their totals change constantly — "
                               "unlike counters, which usually don't. Expect roughly one flash "
                               "erase every %2 minutes of running")
                    .arg(integrators).arg(qMax(1, 254 / qMax(1, preserved))));
    }
    for (int i = 0; i < config.timerRows.size(); ++i) {
        const TimerRow &t = config.timerRows[i];
        if (!t.active)
            continue;
        const QString loc = QStringLiteral("Timer %1").arg(i + 1);
        if (t.outputChannel.isEmpty())
            add(ValidationIssue::Error, loc, QStringLiteral("no output channel"));
        checkInput(t.startChannel, loc, QStringLiteral("start"));
        checkInput(t.stopChannel, loc, QStringLiteral("stop"));
        if (t.startChannel.isEmpty() && t.stopChannel.isEmpty())
            add(ValidationIssue::Warning, loc,
                QStringLiteral("timer has neither a start nor a stop channel — it never runs"));
        if (t.rollover && t.limitValue <= 0.0)
            add(ValidationIssue::Warning, loc,
                QStringLiteral("roll-over needs a positive limit value"));
    }
    for (int i = 0; i < config.integratorRows.size(); ++i) {
        const IntegratorRow &g = config.integratorRows[i];
        if (!g.active)
            continue;
        const QString loc = QStringLiteral("Integrator %1").arg(i + 1);
        if (g.outputChannel.isEmpty())
            add(ValidationIssue::Error, loc, QStringLiteral("no output channel"));
        if (g.inputIsChannel) {
            if (g.inputChannel.isEmpty())
                add(ValidationIssue::Error, loc, QStringLiteral("no input channel"));
            else
                checkInput(g.inputChannel, loc, QStringLiteral("input"));
            // Accumulating a channel into itself doubles it every step rather
            // than integrating anything, and diverges within seconds. A warning
            // rather than an error: it is one row reading and writing its own
            // slot, not two things fighting over one, so it is the user's call.
            if (!g.inputChannel.isEmpty()
                && g.inputChannel.compare(g.outputChannel, Qt::CaseInsensitive) == 0)
                add(ValidationIssue::Warning, loc,
                    QStringLiteral("input and output are the same channel ('%1') — each step "
                                   "adds the total to itself, so the value doubles rather than "
                                   "integrates and diverges within seconds")
                        .arg(g.outputChannel));
        }
        checkInput(g.enableChannel, loc, QStringLiteral("enable"));
        checkInput(g.resetChannel, loc, QStringLiteral("reset"));
        if (g.rateHz < 1 || g.rateHz > INTEGRATOR_MAX_HZ)
            add(ValidationIssue::Error, loc,
                QStringLiteral("rate must be 1–%1 Hz (the engine evaluates at %1 Hz)")
                    .arg(INTEGRATOR_MAX_HZ));
        const bool clamped = g.maxValue > g.minValue;
        if (!clamped)
            add(ValidationIssue::Warning, loc,
                QStringLiteral("maximum does not exceed minimum, so the value is never "
                               "clamped and can run away"));
        // A count-down integrator that starts at or below its floor is already
        // finished before it begins — the classic "forgot to set the peak".
        if (g.countDown && clamped && g.startValue <= g.minValue)
            add(ValidationIssue::Warning, loc,
                QStringLiteral("counts down but starts at %1, which is already at or below "
                               "the minimum (%2) — set a starting value to count down from")
                    .arg(g.startValue).arg(g.minValue));
        if (!g.countDown && clamped && g.startValue >= g.maxValue)
            add(ValidationIssue::Warning, loc,
                QStringLiteral("counts up but starts at %1, which is already at or above "
                               "the maximum (%2)").arg(g.startValue).arg(g.maxValue));
        // Nothing resets an integrator by itself. Preserve makes this sharper:
        // a retained total survives the power cycle that would otherwise have
        // cleared it, so with no reset input there is no way back at all.
        if (g.resetChannel.isEmpty())
            add(ValidationIssue::Warning, loc,
                g.preserveValue
                    ? QStringLiteral("no reset channel, and the value is preserved across "
                                     "power cycles — nothing can ever clear this total")
                    : QStringLiteral("no reset channel — the total can only be cleared by "
                                     "power-cycling the device"));
    }
    for (int i = 0; i < config.constantRows.size(); ++i) {
        const ConstantRow &k = config.constantRows[i];
        if (!k.active)
            continue;
        const QString loc = QStringLiteral("Constant %1").arg(i + 1);
        if (k.name.isEmpty())
            add(ValidationIssue::Error, loc, QStringLiteral("no channel name"));
        if (k.dataType.isEmpty())
            add(ValidationIssue::Warning, loc,
                QStringLiteral("no data type chosen — the constant defaults to float"));
    }

    // Lookup tables. Only the populated sites are used; axis inputs must be
    // generated, sites must ascend, and each populated site needs an output.
    // An axis READS a channel, so any channel is fair game; only a blank axis is
    // an error, because the lookup then has no input at all.
    const auto checkAxis = [&](const QString &name, const QString &loc, const QString &axis) {
        if (name.isEmpty())
            add(ValidationIssue::Error, loc,
                QStringLiteral("no %1 axis channel selected").arg(axis));
        else
            noteReference(name, loc, QStringLiteral("%1 axis").arg(axis));
    };
    const auto checkAscending = [&](const QList<double> &sites, const QString &loc,
                                    const QString &axis) {
        for (int k = 0; k + 1 < sites.size(); ++k)
            if (sites.value(k + 1) <= sites.value(k)) {
                add(ValidationIssue::Warning, loc,
                    QStringLiteral("%1 axis sites are not strictly ascending — the lookup "
                                   "may bracket incorrectly").arg(axis));
                break;
            }
    };
    for (int i = 0; i < config.table2x16Rows.size(); ++i) {
        const Table2x16Row &t = config.table2x16Rows[i];
        if (!t.active)
            continue;
        const QString loc = QStringLiteral("Table 2x16 %1").arg(i + 1);
        if (t.outputChannel.isEmpty())
            add(ValidationIssue::Error, loc, QStringLiteral("no output channel name"));
        if (t.xSites.isEmpty()) {
            add(ValidationIssue::Warning, loc, QStringLiteral("has no sites — it generates nothing"));
            continue;
        }
        if (t.outputs.size() != t.xSites.size())
            add(ValidationIssue::Error, loc,
                QStringLiteral("each site needs an output (%1 sites, %2 outputs)")
                    .arg(t.xSites.size()).arg(t.outputs.size()));
        checkAxis(t.xChannel, loc, QStringLiteral("input"));
        checkAscending(t.xSites, loc, QStringLiteral("input"));
    }
    for (int i = 0; i < config.table8x8Rows.size(); ++i) {
        const Table8x8Row &t = config.table8x8Rows[i];
        if (!t.active)
            continue;
        const QString loc = QStringLiteral("Table 8x8 %1").arg(i + 1);
        if (t.outputChannel.isEmpty())
            add(ValidationIssue::Error, loc, QStringLiteral("no output channel name"));
        if (t.xSites.isEmpty() || t.ySites.isEmpty()) {
            add(ValidationIssue::Warning, loc, QStringLiteral("has no sites — it generates nothing"));
            continue;
        }
        if (t.outputs.size() != t.xSites.size() * t.ySites.size())
            add(ValidationIssue::Error, loc,
                QStringLiteral("the filled grid needs an output per cell (%1 X x %2 Y, %3 outputs)")
                    .arg(t.xSites.size()).arg(t.ySites.size()).arg(t.outputs.size()));
        checkAxis(t.xChannel, loc, QStringLiteral("X"));
        checkAxis(t.yChannel, loc, QStringLiteral("Y"));
        checkAscending(t.xSites, loc, QStringLiteral("X"));
        checkAscending(t.ySites, loc, QStringLiteral("Y"));
    }

    // Transmitting a channel READS it — every channel is transmittable, and
    // sending one changes nothing about its value, so this is never an error or
    // a warning. It is only worth noting when nothing writes the channel, since
    // the frame then carries its default value (allRows covers the
    // always-present rows + every compound identifier row).
    for (int busIdx = 0; busIdx < 3; ++busIdx)
        for (const CommsSection &s : config.bus[busIdx].sections)
            if (s.isTransmit() && s.device != SectionDevice::Off)
                for (const CommsChannelRow &row : s.allRows())
                    if (!row.channelName.isEmpty()
                        && !generated.contains(row.channelName.toLower()))
                        add(ValidationIssue::Info,
                            QStringLiteral("CAN %1 · %2 · %3")
                                .arg(busIdx + 1).arg(s.name, row.channelName),
                            QStringLiteral("transmitted channel has no generator yet "
                                           "(receive row or calculation) — the frame carries "
                                           "its default value"));

    // Channels defined in the catalogue but not generated, used, or referenced
    // by anything (typically left behind after removing a message). Info only —
    // they cost nothing on the device; Check Channels offers a cleanup.
    for (const QString &name : analyzeChannelUsage(config).unused)
        add(ValidationIssue::Info, QStringLiteral("Channels"),
            QStringLiteral("channel '%1' is defined but not used by anything "
                           "(unused — removable)").arg(name));

    // Capacity summary (via the mapper, which also surfaces anything it can't express)
    const MappingResult mapped = mapToDevice(config);
    int rxCount = 0, txCount = 0;
    for (const CanMessageConfig &m : mapped.tables.messages)
        (m.flags & MSGFLAG_TRANSMIT) ? ++txCount : ++rxCount;
    add(ValidationIssue::Info, QStringLiteral("Device usage"),
        // The 8x8 figure counts DEFINITIONS, which is the capacity a user can
        // exhaust. Its grid rows are a second device table of
        // MAX_TABLES_8X8 * TABLE_8X8_SITES entries, but every table always
        // contributes exactly eight of them, so that count can never run out
        // first and reporting it would be one more number saying the same thing.
        QStringLiteral("%1/%2 messages (%3 receive + %4 transmit), %5/%6 signals, "
                       "%7/%8 math, %9/%10 conditions, %11/%12 counters, %13/%14 timers, "
                       "%15/%16 constants, %17/%18 relays, %19/%20 2x16 + %21/%22 8x8 tables, "
                       "%23/%24 integrators, %25/%26 CRC8 rules")
            .arg(mapped.tables.messages.size()).arg(MAX_MESSAGES).arg(rxCount).arg(txCount)
            .arg(mapped.tables.signalConfigs.size()).arg(MAX_SIGNALS)
            .arg(mapped.tables.math.size()).arg(MAX_MATH_COMPUTATIONS)
            .arg(mapped.tables.conditions.size()).arg(MAX_CONDITIONS)
            .arg(mapped.tables.counters.size()).arg(MAX_COUNTERS)
            .arg(mapped.tables.timers.size()).arg(MAX_TIMERS)
            .arg(mapped.tables.constants.size()).arg(MAX_CONSTANTS)
            .arg(mapped.tables.relays.size()).arg(MAX_RELAYS)
            .arg(mapped.tables.tables2x16Def.size()).arg(MAX_TABLES_2X16)
            .arg(mapped.tables.tables8x8Def.size()).arg(MAX_TABLES_8X8)
            .arg(mapped.tables.integrators.size()).arg(MAX_INTEGRATORS)
            .arg(mapped.tables.crc8.size()).arg(MAX_CRC8_MESSAGES));

    return issues;
}

} // namespace ct
