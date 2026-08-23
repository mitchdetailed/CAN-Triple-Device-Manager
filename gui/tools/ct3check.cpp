// ct3check — load a .ct3/.ct3s, map it to the device tables, and report what it
// costs and what is wrong with it, without a device or a GUI.
//
// The Manager will tell you all of this, but only interactively and only on a
// machine with Qt in front of a person. This is the same three calls the Manager
// makes on a Send — loadFromFile, validateConfiguration, mapWithScript — so a
// configuration this accepts is one the Manager will accept, and the table
// counts it prints are the ones the device will be asked for. mapWithScript
// rather than plain mapToDevice: it compiles the document's Lua (or re-verifies
// a retained image) and reports a script that will not build as a mapping
// error, exactly as it would block a Send.
//
//   ct3check <file.ct3> [--verbose]
//
// Exit status is 0 when the file loads and maps with no errors, 1 otherwise, so
// it can gate a script.

#include <QCoreApplication>
#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <QTextStream>

#include <cstdio>

#include "../src/model/configuration.h"
#include "../src/model/device_mapper.h"
#include "../src/model/validation.h"
#include "../src/protocol/wire_structs.h"
#include "../src/scripting/script_compiler.h"   // mapWithScript

using namespace ct;

namespace {

void row(QTextStream &out, const QString &name, int used, int cap)
{
    const double pct = cap > 0 ? 100.0 * used / cap : 0.0;
    out << QStringLiteral("  %1 %2 / %3  %4%\n")
               .arg(name.leftJustified(22, QLatin1Char(' ')))
               .arg(used, 5)
               .arg(cap, -5)
               .arg(pct, 5, 'f', 1);
    if (used > cap)
        out << QStringLiteral("      ^^ OVER CAPACITY\n");
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);

    QStringList args = QCoreApplication::arguments().mid(1);
    const bool verbose = args.removeAll(QStringLiteral("--verbose")) > 0;
    if (args.isEmpty()) {
        out << "usage: ct3check <file.ct3> [--verbose]\n";
        return 2;
    }
    const QString path = args.first();

    Configuration cfg;
    QString error;
    if (!cfg.loadFromFile(path, &error)) {
        out << "LOAD FAILED: " << error << "\n";
        return 1;
    }
    out << QStringLiteral("%1\n").arg(QFileInfo(path).fileName());
    out << QStringLiteral("  title: %1\n\n").arg(cfg.configTitle());

    // What the document asks for, before mapping: bus modes and section shape,
    // because "500 transmit messages" is a claim about the document and the
    // table counts below are what it turns into.
    int tx = 0, rx = 0, relay = 0, crc8 = 0;
    for (int b = 0; b < 3; ++b) {
        for (const CommsSection &s : cfg.bus[b].sections) {
            if (s.device == SectionDevice::TransmitMessage)
                ++tx;
            else if (s.device == SectionDevice::ReceiveMessage)
                ++rx;
            else if (s.device == SectionDevice::MessageRelay)
                ++relay;
            else if (s.device == SectionDevice::TransmitCrc8)
                ++crc8; // its own count, not folded into tx: a CRC8 section
                        // costs a message slot AND a CRC8 rule, and the second
                        // capacity is the one this line has to make visible
        }
        out << QStringLiteral("  CAN%1: %2, %3 kbit/s, %4 sections\n")
                   .arg(b + 1)
                   .arg(cfg.bus[b].enabled ? "on " : "off")
                   .arg(cfg.bus[b].rateKbps)
                   .arg(cfg.bus[b].sections.size());
    }
    out << QStringLiteral("  sections: %1 transmit, %2 receive, %3 relay, %4 transmit CRC8\n\n")
               .arg(tx).arg(rx).arg(relay).arg(crc8);

    // mapWithScript, never plain mapToDevice: every real Send path compiles the
    // document's script (or re-verifies retained bytecode) and a failure lands
    // in m.errors, so a .ct3 with a broken script must read NOT OK here too.
    const MappingResult m = mapWithScript(cfg);

    out << "device tables:\n";
    row(out, "messages", m.tables.messages.size(), MAX_MESSAGES);
    row(out, "signals", m.tables.signalConfigs.size(), MAX_SIGNALS);
    row(out, "math", m.tables.math.size(), MAX_MATH_COMPUTATIONS);
    row(out, "conditions", m.tables.conditions.size(), MAX_CONDITIONS);
    row(out, "counters", m.tables.counters.size(), MAX_COUNTERS);
    row(out, "timers", m.tables.timers.size(), MAX_TIMERS);
    row(out, "constants", m.tables.constants.size(), MAX_CONSTANTS);
    row(out, "relays", m.tables.relays.size(), MAX_RELAYS);
    row(out, "tables 2x16", m.tables.tables2x16Def.size(), MAX_TABLES_2X16);
    row(out, "tables 8x8", m.tables.tables8x8Def.size(), MAX_TABLES_8X8);
    row(out, "integrators", m.tables.integrators.size(), MAX_INTEGRATORS);
    row(out, "crc8", m.tables.crc8.size(), MAX_CRC8_MESSAGES);

    // Transmit load, because a configuration that maps cleanly can still ask for
    // more than the wire can carry, and that is invisible in the table counts.
    //
    // Two pools per bus, mirroring the firmware's chargeFrameBits
    // (engine_core.c): classic frames clock every bit at the nominal rate; an
    // FD frame straddles two rates, so its arbitration and tail bits land in
    // the nominal pool and the BRS-to-CRC stretch in the data pool. Folding FD
    // into the classic formula had this tool mis-reading a 500k+2M bus by the
    // whole data-phase speedup.
    double busArbBits[3] = {0, 0, 0};  // bit/s clocked at the nominal rate
    double busDataBits[3] = {0, 0, 0}; // bit/s clocked at the FD data rate
    int txMsgs = 0;
    for (int mi = 0; mi < m.tables.messages.size(); ++mi) {
        const CanMessageConfig &msg = m.tables.messages[mi];
        if (!(msg.flags & MSGFLAG_TRANSMIT) || !(msg.flags & MSGFLAG_ACTIVE))
            continue;
        if (msg.src_bus < 1 || msg.src_bus > 3)
            continue;
        ++txMsgs;
        const int period = msg.period_ms < 10 ? 10 : msg.period_ms;

        // A compound Batch message sends one frame PER IDENTIFIER each period,
        // and charging it as one frame hid all but a sliver of its real load.
        // Count the distinct selectors exactly as the firmware's
        // collectMuxIdentifiers does — offset + mask + MASKED id, the first
        // MAX_TX_MUX_IDS only — so the estimate charges the frames the device
        // will actually send. Sequential rotates: one frame per period.
        QList<quint64> selectors;
        for (const CanSignalConfig &sig : m.tables.signalConfigs) {
            if (!sigIsActive(sig) || int(sigMsgIdx(sig)) != mi || sig.mux_mask == 0)
                continue;
            const quint64 sel = (quint64(sigMuxByteOffset(sig)) << 32)
                                | (quint64(sig.mux_mask) << 16)
                                | quint64(sig.mux_id & sig.mux_mask);
            if (!selectors.contains(sel)) {
                if (selectors.size() >= MAX_TX_MUX_IDS)
                    break; // the composer serves no more than this
                selectors.append(sel);
            }
        }
        const int framesPerPeriod =
            (msg.flags & MSGFLAG_TX_SEQUENTIAL) ? 1 : qMax(1, int(selectors.size()));
        const double perSecond = framesPerPeriod * (1000.0 / period);

        const int dataBits = msg.dlc * 8;
        const bool ext = (msg.flags & MSGFLAG_EXTENDED) != 0;
        if (msg.flags & MSGFLAG_FD) {
            // chargeFrameBits' FD split: ESI+DLC+payload+stuff-count+CRC ride
            // the data rate (CRC is 17 bits to 16 data bytes, 21 beyond);
            // arbitration, with the /5 stuff allowance, and the tail ride the
            // nominal rate.
            const int arb = ext ? 50 : 30;
            const int crc = msg.dlc > 16 ? 21 : 17;
            busArbBits[msg.src_bus - 1] += (arb + arb / 5.0) * perSecond;
            busDataBits[msg.src_bus - 1] += (9 + dataBits + 4 + crc) * perSecond;
        } else {
            // Same geometry the firmware's bus-load estimate uses: frame
            // overhead, an average stuff allowance, and the interframe space.
            const int overhead = ext ? 64 : 44;
            const int stuffable = (ext ? 54 : 34) + dataBits;
            busArbBits[msg.src_bus - 1] += (overhead + dataBits + 3 + stuffable / 5.0) * perSecond;
        }
    }
    out << QStringLiteral("\n  active transmit messages: %1\n").arg(txMsgs);
    for (int b = 0; b < 3; ++b) {
        if (busArbBits[b] <= 0.0 && busDataBits[b] <= 0.0)
            continue;
        const double rate = cfg.bus[b].rateKbps * 1000.0;
        // 0 = classic; the firmware, too, falls back to the nominal rate when
        // no data rate was configured (engine_set_bus_bitrate).
        const double dataRate =
            cfg.bus[b].dataRateKbps > 0 ? cfg.bus[b].dataRateKbps * 1000.0 : rate;
        // Load is bus TIME, so each pool divides by its own rate. The bit/s
        // column expresses that time at the nominal rate — identical to the raw
        // bit count for classic traffic — so the two printed numbers still
        // divide to the percentage.
        const double load =
            rate > 0.0 ? busArbBits[b] / rate + busDataBits[b] / dataRate : 0.0;
        out << QStringLiteral("  CAN%1 offered load: %2 bit/s of %3 = %4%%5\n")
                   .arg(b + 1)
                   .arg(qint64(load * rate))
                   .arg(qint64(rate))
                   .arg(100.0 * load, 0, 'f', 1)
                   .arg(load > 1.0 ? "  ** OVERSUBSCRIBED **" : "");
    }

    const QList<ValidationIssue> issues = validateConfiguration(cfg);
    // Info counted separately from Warning: lumping them together makes the
    // summary line disagree with what gets printed, which is how two issues
    // went unexplained the first time this ran.
    int errs = 0, warns = 0, infos = 0;
    for (const ValidationIssue &i : issues) {
        if (i.severity == ValidationIssue::Error)
            ++errs;
        else if (i.severity == ValidationIssue::Warning)
            ++warns;
        else
            ++infos;
    }

    out << QStringLiteral("\n  mapping: %1 error(s), %2 warning(s)\n")
               .arg(m.errors.size()).arg(m.warnings.size());
    out << QStringLiteral("  validation: %1 error(s), %2 warning(s), %3 info\n")
               .arg(errs).arg(warns).arg(infos);

    const auto dump = [&](const QString &label, const QStringList &list, int limit) {
        for (int i = 0; i < list.size() && (verbose || i < limit); ++i)
            out << "    " << label << ": " << list.at(i) << "\n";
        if (!verbose && list.size() > limit)
            out << QStringLiteral("    ... %1 more (--verbose)\n").arg(list.size() - limit);
    };
    dump(QStringLiteral("mapping error"), m.errors, 10);
    dump(QStringLiteral("mapping warning"), m.warnings, 5);

    // Errors first and always; warnings only under --verbose, because a large
    // configuration produces a steady trickle of them and burying the one error
    // that stops a Send in forty lines of advice helps nobody.
    for (const int wanted : {int(ValidationIssue::Error), int(ValidationIssue::Warning),
                             int(ValidationIssue::Info)}) {
        if (wanted != ValidationIssue::Error && !verbose)
            continue;
        int shown = 0;
        const int total = wanted == ValidationIssue::Error     ? errs
                          : wanted == ValidationIssue::Warning ? warns
                                                               : infos;
        for (const ValidationIssue &i : issues) {
            if (int(i.severity) != wanted)
                continue;
            if (!verbose && shown >= 10) {
                out << QStringLiteral("    ... %1 more (--verbose)\n").arg(total - shown);
                break;
            }
            out << (wanted == ValidationIssue::Error     ? "    validation error: "
                    : wanted == ValidationIssue::Warning ? "    validation warning: "
                                                         : "    validation info: ")
                << i.location << " — " << i.message << "\n";
            ++shown;
        }
    }

    const bool good = m.ok() && errs == 0;
    out << (good ? "\n  OK\n" : "\n  NOT OK\n");
    out.flush();
    return good ? 0 : 1;
}
