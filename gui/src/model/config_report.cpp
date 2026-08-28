#include "config_report.h"

#include <QCoreApplication>
#include <QDate>
#include <QPair>
#include <QSet>

#include "../protocol/wire_structs.h"
#include "configuration.h"

namespace ct {

namespace {

// ------------------------------------------------------------- usage walk

struct UsageCollector {
    ChannelUsage usage;
    QSet<QString> dormant; // Off-section rows / inactive calc references

    static QString key(const QString &name) { return name.trimmed().toLower(); }

    void remember(const QString &name)
    {
        const QString k = key(name);
        if (!k.isEmpty() && !usage.displayName.contains(k))
            usage.displayName.insert(k, name.trimmed());
    }
    void markUsedOrder(const QString &name)
    {
        const QString k = key(name);
        if (!k.isEmpty() && !usage.used.contains(usage.displayName.value(k, name)))
            usage.used.append(usage.displayName.value(k, name));
    }
    void generator(const QString &name, const QString &where)
    {
        if (key(name).isEmpty())
            return;
        remember(name);
        usage.generators[key(name)].append(where);
        markUsedOrder(name);
    }
    void user(const QString &name, const QString &where)
    {
        if (key(name).isEmpty())
            return;
        remember(name);
        usage.users[key(name)].append(where);
        markUsedOrder(name);
    }
    void dormantRef(const QString &name)
    {
        if (key(name).isEmpty())
            return;
        remember(name);
        dormant.insert(key(name));
    }
};

// ---------------------------------------------------------- text helpers

QString hexId(const CommsSection &s)
{
    return QStringLiteral("0x") + QString::number(s.baseAddress, 16).toUpper();
}

QString num(double v)
{
    return QString::number(v, 'g', 10);
}

QString dbcTypeName(int type)
{
    switch (DbcType(type)) {
    case DbcType::Signed:  return QStringLiteral("signed");
    case DbcType::IEEE754: return QStringLiteral("ieee754");
    case DbcType::Unsigned: break;
    }
    return QStringLiteral("unsigned");
}

// Per-row extraction/packing detail — the report analog of the Add Comms
// Channel fields. physical = raw × factor + offset.
QString rowDetail(const CommsChannelRow &row, const CommsSection &s)
{
    const int len = row.dbcType == int(DbcType::IEEE754) ? 32 : row.bitLength;
    QString d = QStringLiteral("bit: %1, len: %2, %3, res: %4, +: %5")
                    .arg(row.startBit)
                    .arg(len)
                    .arg(dbcTypeName(row.dbcType))
                    .arg(num(row.dbcFactor))
                    .arg(num(row.dbcOffset));
    if (s.isReceive() && s.defaultValueOnTimeout && s.receiveTimeoutMs > 0)
        d += QStringLiteral(", dflt: %1").arg(num(row.defaultValue));
    // Only worth a word when it is NOT the default, and only on the side that
    // acts on it. Silence means clamping, which is what it has always meant.
    if (!s.isReceive() && !row.clampToRange)
        d += QStringLiteral(", rolls over");
    return d;
}

// The CRC8 recipe, one line: everything the editor's third tab holds. This is
// protocol the same way a row's bit positions are, so the caller withholds it
// for a concealed section and prints only the published channel's name.
QString crc8Detail(const CommsSection &s)
{
    const auto hex = [](int v) {
        return QStringLiteral("0x")
               + QString::number(v, 16).toUpper().rightJustified(2, QLatin1Char('0'));
    };
    QStringList elems;
    for (const CommsSection::CrcElement &e : s.crcElements) {
        if (e.type == CommsSection::CrcElement::Id)
            elems << (e.value == 0 ? QStringLiteral("ID")
                                   : QStringLiteral("ID>>%1").arg(e.value * 8));
        else if (e.type == CommsSection::CrcElement::Data)
            elems << QStringLiteral("Byte %1").arg(e.value);
        else
            elems << hex(e.value);
    }
    QString d = QStringLiteral("CRC8 into byte %1: poly %2, init %3, xor %4")
                    .arg(s.crcByteLocation)
                    .arg(hex(s.crcPolynomial), hex(s.crcInitValue), hex(s.crcFinalXor));
    if (s.crcRefIn)
        d += QStringLiteral(", ref in");
    if (s.crcRefOut)
        d += QStringLiteral(", ref out");
    if (!elems.isEmpty())
        d += QStringLiteral(", over ") + elems.join(QStringLiteral(" + "));
    return d;
}

QString alignmentName(SectionAlignment a)
{
    return a == SectionAlignment::WordSwap
               ? QStringLiteral("Word Swap (Intel, little-endian)")
               : QStringLiteral("Normal (Motorola, big-endian)");
}

// "[receive, ID 0x640, 8 bytes, Word Swap (Intel, little-endian), ...]"
QString sectionDetail(const CommsSection &s, int busIndex, bool revealed)
{
    QStringList parts;
    if (s.device == SectionDevice::Off)
        return QStringLiteral("[off]");
    // A CONCEALED message contributes its name and its channels to the report
    // and nothing else. The report is the easiest way to walk off with a
    // protocol — it is printable and exportable — so it withholds the same
    // fields the dialogs do, and asks displayDetail() rather than deciding for
    // itself what a concealed message may say about itself.
    //
    // Read Only falls straight through to the full detail below. isConcealed()
    // is false for it, which is the whole point of the split: a Read Only
    // message is protected against EDITING and against nothing else, so a report
    // that hid its ID would be withholding something the reader can see on
    // screen a click away.
    if (s.isConcealed(revealed))
        return QStringLiteral("[") + s.displayDetail(revealed) + QStringLiteral("]");
    if (s.isRelay()) {
        parts << QStringLiteral("relay");
        QString id = QStringLiteral("ID ") + hexId(s);
        if (s.extended)
            id += QStringLiteral(" (extended)");
        parts << id;
        parts << QStringLiteral("mask 0x%1").arg(QString::number(s.relayBitmask, 16).toUpper());
        if (s.relayInvert)
            parts << QStringLiteral("inverted");
        QStringList dests;
        for (int t = 0; t < 3; ++t)
            if ((s.routeBusMask & (1 << t)) && t != busIndex)
                dests << QStringLiteral("CAN %1").arg(t + 1);
        parts << QStringLiteral("forwards to %1")
                     .arg(dests.isEmpty() ? QStringLiteral("(none)")
                                          : dests.join(QStringLiteral("+")));
        return QStringLiteral("[") + parts.join(QStringLiteral(", ")) + QStringLiteral("]");
    }
    // A CRC8 section names itself here the way displayDetail() does, so the
    // report and the dialogs use one vocabulary. The recipe itself is NOT
    // squeezed into this line — it prints as the section's "Generates Channel"
    // row below (crc8Detail), where the concealment rules already govern it.
    parts << (s.isCrc8()       ? QStringLiteral("transmit CRC8")
              : s.isTransmit() ? QStringLiteral("transmit")
                               : QStringLiteral("receive"));
    QString id = QStringLiteral("ID ") + hexId(s);
    if (s.extended)
        id += QStringLiteral(" (extended)");
    parts << id;
    parts << QStringLiteral("%1%2 bytes")
                 .arg(s.fd ? QStringLiteral("CAN FD ") : QString())
                 .arg(s.messageLengthBytes);
    parts << alignmentName(s.alignment);
    if (s.isTransmit()) {
        if (s.transmitPeriodMs > 0)
            parts << QStringLiteral("every %1 ms").arg(s.transmitPeriodMs);
        else
            parts << QStringLiteral("%1 Hz").arg(s.transmitRateHz);
        if (s.compound)
            parts << (s.compoundTxMode == CompoundTxMode::Sequential
                          ? QStringLiteral("compound, sequential")
                          : QStringLiteral("compound, batch"));
        // Cyclic prints nothing — it is what a transmit message has always been,
        // and every line in this report would otherwise carry a word that says
        // "normal". A trigger is the exception worth a reader's attention, so it
        // names itself and the condition it waits on.
        if (!s.cyclic)
            parts << QStringLiteral("triggered on %1")
                         .arg(s.transmitCondition.isEmpty() ? QStringLiteral("(none)")
                                                            : s.transmitCondition);
    } else {
        if (s.defaultValueOnTimeout && s.receiveTimeoutMs > 0)
            parts << QStringLiteral("timeout %1 ms -> defaults").arg(s.receiveTimeoutMs);
        if (s.routeEnable) {
            QStringList dests;
            for (int t = 0; t < 3; ++t)
                if ((s.routeBusMask & (1 << t)) && t != busIndex)
                    dests << QStringLiteral("CAN %1").arg(t + 1);
            if (!dests.isEmpty())
                parts << QStringLiteral("routes to %1").arg(dests.join(QStringLiteral("+")));
        }
        if (s.compound)
            parts << QStringLiteral("compound");
    }
    return QStringLiteral("[") + parts.join(QStringLiteral(", ")) + QStringLiteral("]");
}

QString mathHeadline(const MathRow &m)
{
    const QString a = m.aIsChannel ? m.aChannel : num(m.aConst);
    const QString b = m.bIsChannel ? m.bChannel : num(m.bConst);
    const QString c = m.cIsChannel ? m.cChannel : num(m.cConst);
    switch (m.op) {
    case MATH_OP_ADD: return QStringLiteral("%1 = %2 + %3").arg(m.destChannel, a, b);
    case MATH_OP_SUB: return QStringLiteral("%1 = %2 - %3").arg(m.destChannel, a, b);
    case MATH_OP_MUL: return QStringLiteral("%1 = %2 * %3").arg(m.destChannel, a, b);
    case MATH_OP_DIV: return QStringLiteral("%1 = %2 / %3").arg(m.destChannel, a, b);
    case MATH_OP_SCALE: return QStringLiteral("%1 = %2 * %3 (scale)").arg(m.destChannel, a, b);
    case MATH_OP_MIN: return QStringLiteral("%1 = min(%2, %3)").arg(m.destChannel, a, b);
    case MATH_OP_MAX: return QStringLiteral("%1 = max(%2, %3)").arg(m.destChannel, a, b);
    case MATH_OP_AND: return QStringLiteral("%1 = %2 & %3").arg(m.destChannel, a, b);
    case MATH_OP_OR:  return QStringLiteral("%1 = %2 | %3").arg(m.destChannel, a, b);
    case MATH_OP_ABS:   return QStringLiteral("%1 = |%2|").arg(m.destChannel, a);
    case MATH_OP_NEG:   return QStringLiteral("%1 = -%2").arg(m.destChannel, a);
    case MATH_OP_SQRT:  return QStringLiteral("%1 = sqrt(%2)").arg(m.destChannel, a);
    case MATH_OP_FLOOR: return QStringLiteral("%1 = floor(%2)").arg(m.destChannel, a);
    case MATH_OP_CEIL:  return QStringLiteral("%1 = ceil(%2)").arg(m.destChannel, a);
    case MATH_OP_ROUND: return QStringLiteral("%1 = round(%2)").arg(m.destChannel, a);
    case MATH_OP_MOD:   return QStringLiteral("%1 = %2 mod %3").arg(m.destChannel, a, b);
    case MATH_OP_XOR:   return QStringLiteral("%1 = %2 xor %3 (bitwise)").arg(m.destChannel, a, b);
    case MATH_OP_LAND:  return QStringLiteral("%1 = %2 and %3").arg(m.destChannel, a, b);
    case MATH_OP_LOR:   return QStringLiteral("%1 = %2 or %3").arg(m.destChannel, a, b);
    case MATH_OP_LNOT:  return QStringLiteral("%1 = not %2").arg(m.destChannel, a);
    case MATH_OP_GT: return QStringLiteral("%1 = %2 > %3").arg(m.destChannel, a, b);
    case MATH_OP_GE: return QStringLiteral("%1 = %2 >= %3").arg(m.destChannel, a, b);
    case MATH_OP_LT: return QStringLiteral("%1 = %2 < %3").arg(m.destChannel, a, b);
    case MATH_OP_LE: return QStringLiteral("%1 = %2 <= %3").arg(m.destChannel, a, b);
    // == and != are EXACT float comparisons on the device (no epsilon).
    case MATH_OP_EQ: return QStringLiteral("%1 = %2 == %3").arg(m.destChannel, a, b);
    case MATH_OP_NE: return QStringLiteral("%1 = %2 != %3").arg(m.destChannel, a, b);
    case MATH_OP_MULADD: return QStringLiteral("%1 = %2 × %3 + %4").arg(m.destChannel, a, b, c);
    case MATH_OP_CLAMP:  return QStringLiteral("%1 = clamp(%2, %3, %4)").arg(m.destChannel, a, b, c);
    case MATH_OP_LERP:   return QStringLiteral("%1 = lerp(%2, %3, %4)").arg(m.destChannel, a, b, c);
    case MATH_OP_SELECT: return QStringLiteral("%1 = %2 ? %3 : %4").arg(m.destChannel, a, b, c);
    case MATH_OP_WRAP:   return QStringLiteral("%1 = wrap(%2 into %3..%4)").arg(m.destChannel, a, b, c);
    }
    return QStringLiteral("%1 = f(%2, %3)").arg(m.destChannel, a, b);
}

QString conditionOpName(int op)
{
    switch (op) {
    case COND_OP_EQ:  return QStringLiteral("==");
    case COND_OP_NEQ: return QStringLiteral("!=");
    case COND_OP_LT:  return QStringLiteral("<");
    case COND_OP_LTE: return QStringLiteral("<=");
    case COND_OP_GT:  return QStringLiteral(">");
    case COND_OP_GTE: return QStringLiteral(">=");
    }
    return QStringLiteral("?");
}

// ------------------------------------------------------------- rendering

// Report accumulated as lines with a style tag, rendered to plain text (dashed
// heading rules) or HTML (bold/underline headings in a
// courier <pre> block).
class ReportBuilder {
public:
    void heading(const QString &text) { m_lines.append({text, Heading}); }
    void subhead(const QString &text) { m_lines.append({text, Subhead}); }
    void line(const QString &text = QString()) { m_lines.append({text, Plain}); }

    // Two-column line: name at `indent`, detail at column `kDetailCol`.
    void twoCol(int indent, const QString &left, const QString &right)
    {
        QString l = QString(indent, QLatin1Char(' ')) + left;
        if (!right.isEmpty()) {
            if (l.size() < kDetailCol)
                l += QString(kDetailCol - l.size(), QLatin1Char(' '));
            else
                l += QStringLiteral("  ");
            l += right;
        }
        m_lines.append({l, Plain});
    }
    // The "Generates Channels / From"-style table header + rule.
    void tableHead(int indent, const QString &left, const QString &right)
    {
        twoCol(indent, left, right);
        m_lines.append({QString(indent, QLatin1Char(' '))
                            + QString(kRuleWidth - indent, QLatin1Char('-')),
                        Plain});
    }

    QString toText() const
    {
        QStringList out;
        for (const Line &l : m_lines) {
            out << l.text;
            if (l.style == Heading)
                out << QString(kRuleWidth, QLatin1Char('-'));
        }
        return out.join(QLatin1Char('\n')) + QLatin1Char('\n');
    }

    QString toHtml() const
    {
        QString out = QStringLiteral(
            "<html><body><pre style=\"font-family:'Courier New',Consolas,monospace;"
            " font-size:9pt;\">");
        for (const Line &l : m_lines) {
            const QString escaped = l.text.toHtmlEscaped();
            switch (l.style) {
            case Heading:
                out += QStringLiteral("<b><u>%1</u></b>\n").arg(escaped);
                break;
            case Subhead:
                out += QStringLiteral("<b>%1</b>\n").arg(escaped);
                break;
            case Plain:
                out += escaped + QLatin1Char('\n');
                break;
            }
        }
        out += QStringLiteral("</pre></body></html>");
        return out;
    }

    static constexpr int kDetailCol = 44;
    static constexpr int kRuleWidth = 96;

private:
    enum Style { Heading, Subhead, Plain };
    struct Line {
        QString text;
        Style style;
    };
    QList<Line> m_lines;
};

QString noneMarker()
{
    return QStringLiteral("*** None ***");
}

// The full report; shared by the text and HTML entry points.
ReportBuilder buildReport(const Configuration &config)
{
    const ChannelUsage usage = analyzeChannelUsage(config);
    ReportBuilder r;
    // Messages marked Hidden or Protect Communication give up their detail only
    // to a viewer holding the password THAT TIER demands — the section's own for
    // Hidden, that plus a device confirming Protected Comms for Protected.
    // Read Only messages are printed in full regardless: they conceal nothing.
    // A marked section with NO password of its own gives up nothing to anybody,
    // which is Configuration::isSectionRevealed() failing closed.
    //
    // Asked per section below, via Configuration::isSectionRevealed(). There is
    // deliberately no document-wide `revealed` shortcut hoisted out of the loop
    // any more: it was config.commsRevealed(), which is true for every document
    // with no Protected Comms password, so it ORed away the per-section
    // answer and printed every Hidden message's full layout into the report.

    const QString title = config.effectiveTitle().isEmpty() ? config.displayName()
                                                            : config.effectiveTitle();
    r.line(QStringLiteral("CAN Triple Device Manager - %1 Channel Summary Report").arg(title));
    r.line();

    // ---- Summary Information ----
    r.heading(QStringLiteral("Summary Information"));
    r.line(QStringLiteral("File name : %1")
               .arg(config.filePath().isEmpty() ? QStringLiteral("(not saved)")
                                                : config.filePath()));
    r.line(QStringLiteral("Title     : %1")
               .arg(config.effectiveTitle().isEmpty() ? QStringLiteral("(none)")
                                                      : config.effectiveTitle()));
    r.line(QStringLiteral("Date      : %1")
               .arg(QDate::currentDate().toString(QStringLiteral("M/d/yyyy"))));
    QString app = QStringLiteral("CAN Triple Device Manager");
    if (!QCoreApplication::applicationVersion().isEmpty())
        app += QLatin1Char(' ') + QCoreApplication::applicationVersion();
    r.line(QStringLiteral("Version   : %1 (protocol v%2)").arg(app).arg(PROTOCOL_VERSION_CURRENT));
    if (config.isDirty())
        r.line(QStringLiteral("State     : unsaved changes"));
    r.line();

    // ---- Configuration Comments ----
    r.heading(QStringLiteral("Configuration Comments"));
    if (config.comments.trimmed().isEmpty()) {
        r.line(noneMarker());
    } else {
        const QStringList commentLines = config.comments.split(QLatin1Char('\n'));
        for (const QString &l : commentLines)
            r.line(l);
    }
    r.line();

    // ---- CAN Bus Setup ----
    r.heading(QStringLiteral("CAN Bus Setup"));
    for (int b = 0; b < 3; ++b) {
        const BusConfig &bus = config.bus[b];
        QString detail;
        if (!bus.enabled) {
            detail = QStringLiteral("Off");
        } else {
            detail = QStringLiteral("CAN, %1 kbit/s").arg(busRateLabel(bus.rateKbps));
            if (bus.dataRateKbps > 0)
                detail += QStringLiteral(", FD data %1 kbit/s")
                              .arg(busRateLabel(bus.dataRateKbps));
        }
        detail += bus.termination ? QStringLiteral(", termination ON")
                                  : QStringLiteral(", termination off");
        r.twoCol(0, QStringLiteral("CAN %1").arg(b + 1), detail);
    }
    r.line();

    // ---- Used Channels ----
    r.heading(QStringLiteral("Used Channels"));
    if (usage.used.isEmpty())
        r.line(noneMarker());
    else
        for (const QString &name : usage.used)
            r.line(name);
    r.line();

    // ---- Channels By Function ----
    r.heading(QStringLiteral("Channels By Function"));
    r.subhead(QStringLiteral("Communications"));
    bool anySection = false;
    for (int b = 0; b < 3; ++b) {
        const BusConfig &bus = config.bus[b];
        if (bus.sections.isEmpty())
            continue;
        anySection = true;
        r.subhead(QStringLiteral("  CAN %1").arg(b + 1));
        for (int si = 0; si < bus.sections.size(); ++si) {
            const CommsSection &s = bus.sections[si];
            // Per section, and ONLY per section — and per BUS, because this walk
            // knows which one it is on and a grant taken for a same-named section
            // on another bus must not print this one's protocol.
            const bool sectionRevealed = config.isSectionRevealed(s, b);
            const bool concealed = s.isConcealed(sectionRevealed);
            r.subhead(QStringLiteral("    (%1) %2  %3")
                          .arg(si + 1)
                          .arg(s.name, sectionDetail(s, b, sectionRevealed)));
            if (!s.diagnosticChannel.isEmpty())
                r.twoCol(6, QStringLiteral("Diagnostic channel:"), s.diagnosticChannel);
            if (s.device == SectionDevice::Off) {
                r.line();
                continue;
            }
            if (s.isRelay()) {
                // A relay forwards whole frames; it has no channels to list.
                r.line();
                continue;
            }
            const bool tx = s.isTransmit();
            const QString head = tx ? QStringLiteral("Uses Channels")
                                    : QStringLiteral("Generates Channels");
            const QString from = tx ? QStringLiteral("For") : QStringLiteral("From");
            // Channel names still listed, per-row layout withheld: the outputs
            // are what a customer needs, the bit positions are what they must
            // not have.
            //
            // Hidden and Protected are named apart, matching the sections list
            // and displayDetail(). They are not the same promise — one is opened
            // by this section's own password, the other only by a device
            // confirming Protected Comms — and a reader deciding whether it
            // is worth going to find a password needs to know which.
            const QString withheld = s.protection == CommsProtection::Protected
                                         ? QStringLiteral("(protected)")
                                         : QStringLiteral("(hidden)");
            const auto emitRows = [&](const QList<CommsChannelRow> &rows) {
                for (const CommsChannelRow &row : rows)
                    r.twoCol(8, row.channelName, concealed ? withheld : rowDetail(row, s));
            };
            if (s.compound) {
                bool emitted = false;
                for (int i = 0; i < s.identifiers.size(); ++i) {
                    const CompoundIdentifier &ident = s.identifiers[i];
                    if (!ident.configured && ident.rows.isEmpty())
                        continue;
                    // The multiplexor offset/id/mask IS the protocol on a
                    // compound message — arguably the most sensitive part.
                    QString idLine =
                        concealed
                            ? QStringLiteral("      Id[%1]  %2").arg(i + 1).arg(withheld)
                            : QStringLiteral("      Id[%1]  offset %2, id 0x%3, mask 0x%4")
                                  .arg(i + 1)
                                  .arg(ident.byteOffset)
                                  .arg(QString::number(ident.id, 16).toUpper(),
                                       QString::number(ident.idMask, 16).toUpper());
                    if (ident.idMask == 0 && !concealed)
                        idLine += QStringLiteral("  [mask 0 - skipped, not sent to the device]");
                    r.line(idLine);
                    emitted = true;
                    if (ident.rows.isEmpty()) {
                        r.line(QStringLiteral("        (no channels)"));
                        continue;
                    }
                    r.tableHead(8, head, from);
                    emitRows(ident.rows);
                }
                if (!emitted)
                    r.line(QStringLiteral("      (no channels)"));
            } else if (!s.rows.isEmpty()) {
                r.tableHead(8, head, from);
                emitRows(s.rows);
            } else {
                r.line(QStringLiteral("      (no channels)"));
            }
            // A CRC8 section also GENERATES its checksum channel. The name is
            // listed like every channel name; the recipe is protocol and gets
            // the same withholding the rows above do.
            if (s.isCrc8()) {
                r.tableHead(8, QStringLiteral("Generates Channel"), QStringLiteral("From"));
                r.twoCol(8,
                         s.crcChannel.isEmpty() ? QStringLiteral("(no channel)") : s.crcChannel,
                         concealed ? withheld : crc8Detail(s));
            }
            r.line();
        }
    }
    if (!anySection) {
        r.line(QStringLiteral("  %1").arg(noneMarker()));
        r.line();
    }

    // ---- Calculations ----
    r.subhead(QStringLiteral("Calculations"));
    const bool anyCalc = !config.mathRows.isEmpty() || !config.conditionRows.isEmpty()
        || !config.counterRows.isEmpty() || !config.timerRows.isEmpty()
        || !config.integratorRows.isEmpty()
        || !config.constantRows.isEmpty() || !config.table2x16Rows.isEmpty()
        || !config.table8x8Rows.isEmpty();
    if (!anyCalc)
        r.line(QStringLiteral("  %1").arg(noneMarker()));

    const auto usesGenerates = [&](const QList<QPair<QString, QString>> &uses,
                                   const QList<QPair<QString, QString>> &generates) {
        QList<QPair<QString, QString>> nonEmpty;
        for (const auto &u : uses)
            if (!u.first.isEmpty())
                nonEmpty.append(u);
        if (!nonEmpty.isEmpty()) {
            r.tableHead(8, QStringLiteral("Uses Channels"), QStringLiteral("For"));
            for (const auto &u : nonEmpty)
                r.twoCol(8, u.first, u.second);
        }
        r.tableHead(8, QStringLiteral("Generates Channels"), QStringLiteral("From"));
        for (const auto &g : generates)
            if (!g.first.isEmpty())
                r.twoCol(8, g.first, g.second);
    };
    const auto inactiveTag = [](bool active) {
        return active ? QString() : QStringLiteral("  [inactive]");
    };

    if (!config.mathRows.isEmpty()) {
        r.subhead(QStringLiteral("  Math Channels"));
        for (int i = 0; i < config.mathRows.size(); ++i) {
            const MathRow &m = config.mathRows[i];
            r.subhead(QStringLiteral("    (%1) %2%3")
                          .arg(i + 1)
                          .arg(mathHeadline(m), inactiveTag(m.active)));
            // Only the operands the op reads count as uses (mathOpArity).
            const int arity = mathOpArity(m.op);
            usesGenerates({{m.aIsChannel ? m.aChannel : QString(), QStringLiteral("input A")},
                           {arity >= 2 && m.bIsChannel ? m.bChannel : QString(),
                            QStringLiteral("input B")},
                           {arity >= 3 && m.cIsChannel ? m.cChannel : QString(),
                            QStringLiteral("input C")}},
                          {{m.destChannel, QStringLiteral("result")}});
        }
        r.line();
    }
    if (!config.conditionRows.isEmpty()) {
        r.subhead(QStringLiteral("  User Conditions"));
        for (int i = 0; i < config.conditionRows.size(); ++i) {
            const ConditionRow &c = config.conditionRows[i];
            QList<QPair<QString, QString>> inputs;
            // One expression, rendered the way the editor renders it and
            // collecting its channel references as it goes. Shared by Set and
            // Reset so the report cannot describe the two differently.
            const auto renderExpr = [&](const QList<ConditionTermRow> &terms,
                                        const QList<int> &joiners,
                                        const QString &side) -> QString {
                QStringList termTexts;
                for (int t = 0; t < terms.size(); ++t) {
                    const ConditionTermRow &term = terms[t];
                    const QString which = terms.size() > 1
                                              ? QStringLiteral("%1 comparison %2 ")
                                                    .arg(side).arg(t + 1)
                                              : side + QLatin1Char(' ');
                    if (term.isMessageOp()) {
                        termTexts << QStringLiteral("CAN %1 · %2 %3")
                                         .arg(term.aMessageBus)
                                         .arg(term.aMessage, conditionOpName(term.op));
                        continue; // a message is not a channel reference
                    }
                    const QString b = term.bIsChannel ? term.bChannel : num(term.bConst);
                    termTexts << QStringLiteral("%1 %2 %3")
                                     .arg(term.aChannel, conditionOpName(term.op), b);
                    inputs << qMakePair(term.aChannel, which + QStringLiteral("input A"));
                    if (term.bIsChannel)
                        inputs << qMakePair(term.bChannel, which + QStringLiteral("input B"));
                }
                return joinConditionTerms(termTexts, joiners, QStringLiteral("AND"),
                                          QStringLiteral("OR"));
            };

            const QString setText = renderExpr(c.setTerms, c.setJoiners, QStringLiteral("set"));
            if (c.mode == ConditionMode::Momentary) {
                // The hold is printed in milliseconds as well as hertz, because
                // the hold is the thing being configured and 1/f is not a
                // conversion a reader should be doing in their head.
                r.subhead(QStringLiteral("    (%1) %2 = momentary (%3), hold %4 ms (%5 Hz)%6")
                              .arg(i + 1)
                              .arg(c.outputChannel, setText)
                              .arg(c.latchHz > 0 ? 1000 / c.latchHz : 0)
                              .arg(c.latchHz)
                              .arg(inactiveTag(c.active)));
            } else {
                const QString resetText =
                    renderExpr(c.resetTerms, c.resetJoiners, QStringLiteral("reset"));
                r.subhead(QStringLiteral("    (%1) %2 = set (%3), reset (%4)%5")
                              .arg(i + 1)
                              .arg(c.outputChannel, setText, resetText, inactiveTag(c.active)));
            }
            usesGenerates(inputs,
                          {{c.outputChannel, QStringLiteral("condition result (boolean)")}});
        }
        r.line();
    }
    if (!config.counterRows.isEmpty()) {
        r.subhead(QStringLiteral("  Up / Down Counters"));
        for (int i = 0; i < config.counterRows.size(); ++i) {
            const CounterRow &c = config.counterRows[i];
            // Roll and Preserve change what the counter does at its limits and
            // across a power cycle, so an exported config has to record them.
            QStringList opts;
            if (c.rollAtLimits)
                opts << QStringLiteral("roll at limits");
            if (c.preserveValue)
                opts << QStringLiteral("preserved across power cycles");
            const QString optText =
                opts.isEmpty() ? QString() : QStringLiteral(", %1").arg(opts.join(QStringLiteral(", ")));
            const bool isRate = (c.mode == ct::COUNTER_MODE_RATE);
            const QString kind =
                isRate ? QStringLiteral("%1 Hz %2")
                             .arg(c.rateHz)
                             .arg(c.rateCountDown ? QStringLiteral("decrementing")
                                                  : QStringLiteral("incrementing"))
                : c.mode == ct::COUNTER_MODE_FOLLOW ? QStringLiteral("follow-changes")
                                                    : QStringLiteral("up/down");
            r.subhead(QStringLiteral("    (%1) %2 - %3 counter (min %4, max %5, step %6%7)%8")
                          .arg(i + 1)
                          .arg(c.outputChannel, kind,
                               num(c.minValue), num(c.maxValue), num(c.step), optText,
                               inactiveTag(c.active)));
            // Only the inputs the MODE reads are listed as channels it uses —
            // asked through the row so this cannot drift from the mapper. Rate
            // reads none of the three; Follow and Up/Down each read only their
            // own, and used to be shown each other's leftovers.
            usesGenerates({{c.readsUpDown() ? c.upChannel : QString(), QStringLiteral("up input")},
                           {c.readsUpDown() ? c.downChannel : QString(),
                            QStringLiteral("down input")},
                           {c.readsFollow() ? c.followChannel : QString(),
                            QStringLiteral("follow input")},
                           {c.resetChannel, QStringLiteral("reset input")},
                           {c.enableChannel, QStringLiteral("enable input")}},
                          {{c.outputChannel, QStringLiteral("counter output")}});
        }
        r.line();
    }
    if (!config.timerRows.isEmpty()) {
        r.subhead(QStringLiteral("  Timers"));
        for (int i = 0; i < config.timerRows.size(); ++i) {
            const TimerRow &t = config.timerRows[i];
            r.subhead(QStringLiteral("    (%1) %2 - timer (counts %3)%4")
                          .arg(i + 1)
                          .arg(t.outputChannel,
                               t.countDown ? QStringLiteral("down") : QStringLiteral("up"),
                               inactiveTag(t.active)));
            QList<std::pair<QString, QString>> uses;
            if (!t.startTerm.isMessageOp() && !t.startTerm.aChannel.isEmpty())
                uses.append({t.startTerm.aChannel, QStringLiteral("start input")});
            if (t.startTerm.bIsChannel && !t.startTerm.bChannel.isEmpty())
                uses.append({t.startTerm.bChannel, QStringLiteral("start compared against")});
            if (!t.stopTerm.isMessageOp() && !t.stopTerm.aChannel.isEmpty())
                uses.append({t.stopTerm.aChannel, QStringLiteral("stop input")});
            if (t.stopTerm.bIsChannel && !t.stopTerm.bChannel.isEmpty())
                uses.append({t.stopTerm.bChannel, QStringLiteral("stop compared against")});
            usesGenerates(uses,
                          {{t.outputChannel, QStringLiteral("timer output (seconds)")}});
        }
        r.line();
    }
    if (!config.integratorRows.isEmpty()) {
        r.subhead(QStringLiteral("  Integrators"));
        for (int i = 0; i < config.integratorRows.size(); ++i) {
            const IntegratorRow &g = config.integratorRows[i];
            const QString what = g.inputIsChannel ? g.inputChannel : num(g.inputValue);
            // The rate is what scales the total, so it belongs in the headline —
            // the same input at 10 Hz moves ten times as fast as at 1 Hz.
            QString detail = QStringLiteral("%1 %2 at %3 Hz")
                                 .arg(g.countDown ? QStringLiteral("subtracts")
                                                  : QStringLiteral("adds"))
                                 .arg(what).arg(g.rateHz);
            // The starting value is the peak a decrementor counts down from, so
            // it is load-bearing rather than cosmetic on those rows.
            detail += QStringLiteral(", starts at %1").arg(num(g.startValue));
            if (g.maxValue > g.minValue)
                detail += QStringLiteral(", clamped %1..%2").arg(num(g.minValue), num(g.maxValue));
            else
                detail += QStringLiteral(", unclamped");
            if (g.preserveValue)
                detail += QStringLiteral(", preserved across power cycles");
            r.subhead(QStringLiteral("    (%1) %2 - %3 (%4)%5")
                          .arg(i + 1)
                          .arg(g.outputChannel,
                               g.countDown ? QStringLiteral("decrementor")
                                           : QStringLiteral("integrator"),
                               detail, inactiveTag(g.active)));
            usesGenerates({{g.inputIsChannel ? g.inputChannel : QString(),
                            QStringLiteral("accumulated input")},
                           {g.enableChannel, QStringLiteral("enable input")},
                           {g.resetChannel, QStringLiteral("reset input (to %1)").arg(num(g.resetValue))}},
                          {{g.outputChannel, g.countDown
                                                 ? QStringLiteral("decrementor remaining")
                                                 : QStringLiteral("integrator total")}});
        }
        r.line();
    }
    if (!config.constantRows.isEmpty()) {
        r.subhead(QStringLiteral("  Constants"));
        r.tableHead(8, QStringLiteral("Generates Channels"), QStringLiteral("From"));
        for (const ConstantRow &k : config.constantRows) {
            QString detail = QStringLiteral("constant %1").arg(num(k.value));
            if (!k.dataType.isEmpty())
                detail += QStringLiteral(" (%1, %2 dp)")
                              .arg(k.dataType, QString::number(k.decimalPlaces));
            if (!k.active)
                detail += QStringLiteral("  [inactive]");
            r.twoCol(8, k.name, detail);
        }
        r.line();
    }

    if (!config.table2x16Rows.isEmpty() || !config.table8x8Rows.isEmpty()) {
        r.subhead(QStringLiteral("  Tables"));
        r.tableHead(8, QStringLiteral("Generates Channels"), QStringLiteral("Lookup"));
        const auto axisMode = [](bool interp) {
            return interp ? QStringLiteral("interp") : QStringLiteral("discrete");
        };
        for (const Table2x16Row &t : config.table2x16Rows) {
            QString detail = QStringLiteral("2x16 lookup: %1 (%2)")
                                 .arg(t.xChannel, axisMode(t.xInterp));
            if (!t.active)
                detail += QStringLiteral("  [inactive]");
            r.twoCol(8, t.outputChannel, detail);
        }
        for (const Table8x8Row &t : config.table8x8Rows) {
            QString detail = QStringLiteral("8x8 lookup: X %1 (%2), Y %3 (%4)")
                                 .arg(t.xChannel, axisMode(t.xInterp), t.yChannel,
                                      axisMode(t.yInterp));
            if (!t.active)
                detail += QStringLiteral("  [inactive]");
            r.twoCol(8, t.outputChannel, detail);
        }
        r.line();
    }

    // ---- Incomplete Channels ----
    r.heading(QStringLiteral("Incomplete Channels"));
    if (usage.incomplete.isEmpty()) {
        r.line(noneMarker());
    } else {
        for (const QString &name : usage.incomplete)
            r.twoCol(0, name,
                     QStringLiteral("used by: %1")
                         .arg(usage.users.value(UsageCollector::key(name))
                                  .join(QStringLiteral(", "))));
    }
    r.line();

    // ---- Unused Channels ----
    r.heading(QStringLiteral("Unused Channels"));
    if (usage.unused.isEmpty()) {
        r.line(noneMarker());
    } else {
        for (const QString &name : usage.unused) {
            const Channel ch = config.catalog().findByName(name);
            QStringList info;
            if (!ch.dataType.isEmpty())
                info << ch.dataType;
            if (!ch.unit.isEmpty())
                info << ch.unit;
            r.twoCol(0, name, info.join(QStringLiteral(", ")));
        }
    }

    return r;
}

} // namespace

// ------------------------------------------------------------ public API

ChannelUsage analyzeChannelUsage(const Configuration &config)
{
    UsageCollector c;

    for (int b = 0; b < 3; ++b) {
        for (const CommsSection &s : config.bus[b].sections) {
            const QString where = QStringLiteral("CAN %1 · %2").arg(b + 1).arg(s.name);
            if (s.device == SectionDevice::Off) {
                for (const CommsChannelRow &row : s.allRows())
                    c.dormantRef(row.channelName);
                c.dormantRef(s.diagnosticChannel); // still referenced — protect from cleanup
                c.dormantRef(s.crcChannel);
                continue;
            }
            if (s.isRelay()) {
                // A relay forwards whole frames and generates/uses no channels.
                // Any rows left over from a section re-typed to Relay are dormant
                // (mapToDevice emits no relay signals) — protect from cleanup but
                // don't count them as generated/used, matching the device.
                for (const CommsChannelRow &row : s.allRows())
                    c.dormantRef(row.channelName);
                c.dormantRef(s.diagnosticChannel);
                c.dormantRef(s.crcChannel);
                continue;
            }
            if (!s.diagnosticChannel.isEmpty())
                c.user(s.diagnosticChannel, where + QStringLiteral(" diagnostic"));
            if (s.isTransmit()) {
                for (const CommsChannelRow &row : s.allRows())
                    c.user(row.channelName, where);
                // The CRC8 stamp also PUBLISHES its computed value: the device
                // writes crcChannel's slot on every compose, so the channel is
                // generated here the way a receive channel is. Leaving it out
                // lets Check Channels call a channel the device is writing
                // "unused".
                if (s.isCrc8())
                    c.generator(s.crcChannel, where + QStringLiteral(" CRC8"));
            } else if (s.compound) {
                // Compound sections carry channels only inside identifiers; any
                // legacy rows outside an identifier are ignored on Send (dormant).
                for (const CommsChannelRow &row : s.rows)
                    c.dormantRef(row.channelName);
                for (const CompoundIdentifier &ident : s.identifiers) {
                    // The device mapper skips zero-mask identifiers (see
                    // mapToDevice), so their rows generate nothing on the
                    // device — dormant, not generated.
                    for (const CommsChannelRow &row : ident.rows) {
                        if (ident.idMask == 0)
                            c.dormantRef(row.channelName);
                        else
                            c.generator(row.channelName, where);
                    }
                }
            } else {
                for (const CommsChannelRow &row : s.rows)
                    c.generator(row.channelName, where);
            }
        }
    }

    for (int i = 0; i < config.mathRows.size(); ++i) {
        const MathRow &m = config.mathRows[i];
        const QString where = QStringLiteral("Math %1").arg(i + 1);
        // An operand the op does not read (mathOpArity) is no reference at
        // all — the device ignores it, so the usage map must too.
        const int arity = mathOpArity(m.op);
        if (!m.active) {
            if (m.aIsChannel)
                c.dormantRef(m.aChannel);
            if (arity >= 2 && m.bIsChannel)
                c.dormantRef(m.bChannel);
            if (arity >= 3 && m.cIsChannel)
                c.dormantRef(m.cChannel);
            c.dormantRef(m.destChannel);
            continue;
        }
        if (m.aIsChannel)
            c.user(m.aChannel, where + QStringLiteral(" input A"));
        if (arity >= 2 && m.bIsChannel)
            c.user(m.bChannel, where + QStringLiteral(" input B"));
        if (arity >= 3 && m.cIsChannel)
            c.user(m.cChannel, where + QStringLiteral(" input C"));
        c.generator(m.destChannel, where);
    }
    for (int i = 0; i < config.conditionRows.size(); ++i) {
        const ConditionRow &cond = config.conditionRows[i];
        const QString where = QStringLiteral("User Condition %1").arg(i + 1);
        if (!cond.active) {
            for (const QString &n : cond.inputChannels())
                c.dormantRef(n);
            c.dormantRef(cond.outputChannel);
            continue;
        }
        for (int t = 0; t < cond.setTerms.size(); ++t) {
            const ConditionTermRow &term = cond.setTerms[t];
            if (term.isMessageOp())
                continue; // names a message, not a channel
            const QString which = cond.setTerms.size() > 1
                                      ? QStringLiteral(" comparison %1").arg(t + 1)
                                      : QString();
            c.user(term.aChannel, where + which + QStringLiteral(" input A"));
            if (term.bIsChannel)
                c.user(term.bChannel, where + which + QStringLiteral(" input B"));
        }
        c.generator(cond.outputChannel, where);
    }
    for (int i = 0; i < config.counterRows.size(); ++i) {
        const CounterRow &cnt = config.counterRows[i];
        const QString where = QStringLiteral("Counter %1").arg(i + 1);
        // Only the inputs the MODE reads: the cross-reference must agree with
        // the mapper, or Check Channels reports a channel as still in use by a
        // counter that never looks at it. Asked through the row for that reason
        // — the two used to agree only about Rate.
        const QList<QPair<QString, QString>> inputs = {
            {cnt.readsUpDown() ? cnt.upChannel : QString(), QStringLiteral(" up")},
            {cnt.readsUpDown() ? cnt.downChannel : QString(), QStringLiteral(" down")},
            {cnt.readsFollow() ? cnt.followChannel : QString(), QStringLiteral(" follow")},
            {cnt.resetChannel, QStringLiteral(" reset")},
            {cnt.enableChannel, QStringLiteral(" enable")},
        };
        if (!cnt.active) {
            for (const auto &in : inputs)
                c.dormantRef(in.first);
            c.dormantRef(cnt.outputChannel);
            continue;
        }
        for (const auto &in : inputs)
            if (!in.first.isEmpty())
                c.user(in.first, where + in.second);
        c.generator(cnt.outputChannel, where);
    }
    for (int i = 0; i < config.timerRows.size(); ++i) {
        const TimerRow &t = config.timerRows[i];
        const QString where = QStringLiteral("Timer %1").arg(i + 1);
        if (!t.active) {
            for (const QString &n : t.inputChannels())
                c.dormantRef(n);
            c.dormantRef(t.outputChannel);
            continue;
        }
        // Both operands of both terms, so a channel that only ever appears as
        // the right-hand side of a timer's comparison still counts as used.
        if (!t.startTerm.isMessageOp() && !t.startTerm.aChannel.isEmpty())
            c.user(t.startTerm.aChannel, where + QStringLiteral(" start"));
        if (t.startTerm.bIsChannel && !t.startTerm.bChannel.isEmpty())
            c.user(t.startTerm.bChannel, where + QStringLiteral(" start"));
        if (!t.stopTerm.isMessageOp() && !t.stopTerm.aChannel.isEmpty())
            c.user(t.stopTerm.aChannel, where + QStringLiteral(" stop"));
        if (t.stopTerm.bIsChannel && !t.stopTerm.bChannel.isEmpty())
            c.user(t.stopTerm.bChannel, where + QStringLiteral(" stop"));
        c.generator(t.outputChannel, where);
    }
    for (int i = 0; i < config.integratorRows.size(); ++i) {
        const IntegratorRow &g = config.integratorRows[i];
        const QString where = QStringLiteral("Integrator %1").arg(i + 1);
        // A constant-input integrator reads no channel, so its input reference
        // is skipped entirely rather than recorded as an empty one.
        const QString input = g.inputIsChannel ? g.inputChannel : QString();
        if (!g.active) {
            c.dormantRef(input);
            c.dormantRef(g.enableChannel);
            c.dormantRef(g.resetChannel);
            c.dormantRef(g.outputChannel);
            continue;
        }
        if (!input.isEmpty())
            c.user(input, where + QStringLiteral(" input"));
        if (!g.enableChannel.isEmpty())
            c.user(g.enableChannel, where + QStringLiteral(" enable"));
        if (!g.resetChannel.isEmpty())
            c.user(g.resetChannel, where + QStringLiteral(" reset"));
        c.generator(g.outputChannel, where);
    }
    for (int i = 0; i < config.constantRows.size(); ++i) {
        const ConstantRow &k = config.constantRows[i];
        if (!k.active) {
            c.dormantRef(k.name);
            continue;
        }
        c.generator(k.name, QStringLiteral("Constant %1").arg(i + 1));
    }
    for (int i = 0; i < config.table2x16Rows.size(); ++i) {
        const Table2x16Row &t = config.table2x16Rows[i];
        const QString where = QStringLiteral("Table 2x16 %1").arg(i + 1);
        // An inactive OR empty table generates nothing on the device (the mapper
        // skips empty tables) — treat its channels as dormant references.
        if (!t.active || t.xSites.isEmpty()) {
            c.dormantRef(t.xChannel);
            c.dormantRef(t.outputChannel);
            continue;
        }
        if (!t.xChannel.isEmpty())
            c.user(t.xChannel, where + QStringLiteral(" axis"));
        c.generator(t.outputChannel, where);
    }
    for (int i = 0; i < config.table8x8Rows.size(); ++i) {
        const Table8x8Row &t = config.table8x8Rows[i];
        const QString where = QStringLiteral("Table 8x8 %1").arg(i + 1);
        if (!t.active || t.xSites.isEmpty() || t.ySites.isEmpty()) {
            c.dormantRef(t.xChannel);
            c.dormantRef(t.yChannel);
            c.dormantRef(t.outputChannel);
            continue;
        }
        if (!t.xChannel.isEmpty())
            c.user(t.xChannel, where + QStringLiteral(" X axis"));
        if (!t.yChannel.isEmpty())
            c.user(t.yChannel, where + QStringLiteral(" Y axis"));
        c.generator(t.outputChannel, where);
    }

    // Incomplete: consumed but never generated.
    for (auto it = c.usage.users.constBegin(); it != c.usage.users.constEnd(); ++it)
        if (!c.usage.generators.contains(it.key()))
            c.usage.incomplete.append(c.usage.displayName.value(it.key(), it.key()));
    c.usage.incomplete.sort(Qt::CaseInsensitive);

    // Unused: in the catalogue, but nothing generates, uses, or dormantly
    // references it.
    for (const Channel &ch : config.catalog().userChannels()) {
        const QString k = UsageCollector::key(ch.name);
        if (!c.usage.generators.contains(k) && !c.usage.users.contains(k)
            && !c.dormant.contains(k))
            c.usage.unused.append(ch.name);
    }
    c.usage.unused.sort(Qt::CaseInsensitive);

    return c.usage;
}

QString configSummaryText(const Configuration &config)
{
    return buildReport(config).toText();
}

QString configSummaryHtml(const Configuration &config)
{
    return buildReport(config).toHtml();
}

} // namespace ct
