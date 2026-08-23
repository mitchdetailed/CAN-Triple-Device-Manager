// scriptc — compile a device script to bytecode, from the command line.
//
// The configurator compiles scripts inside a dialog, which is the right place
// for a person and the wrong place for a test. The hardware suite needs an
// image produced by the REAL compiler, as a file, so it can send it to a board
// and compare what the board does against what the desktop simulator predicted.
// Without this tool that comparison could only be made by driving a GUI.
//
// It is deliberately not a general-purpose build tool. There is no project
// format, no include path and no optimiser — one .lua in, one .bin out, and a
// symbol table given as text because a real one comes from a configuration and
// a test wants to state its channels in two lines.
//
//   scriptc script.lua symbols.txt out.bin
//
// symbols.txt is one "name=index" per line; blank lines and lines starting with
// '#' are ignored. The index is the DEVICE SIGNAL SLOT, which is what sig() and
// setSig() resolve to — the same numbering the value stream reports, so a test
// can name a channel here and recognise it coming back.
#include <QByteArray>
#include <QCoreApplication>
#include <QFile>
#include <QString>
#include <QStringList>
#include <QTextStream>

#include <cstdio>
#include <cstring>

#include "../scripting/script_compiler.h"
#include "../scripting/script_simulator.h"

namespace {

// stderr, not qWarning: this is a command-line tool and its diagnostics belong
// on the stream a shell can redirect independently of the numbers on stdout.
void err(const QString &s)
{
    QTextStream(stderr) << s << "\n";
}

bool readSymbols(const QString &path, ct::ScriptSymbols *out, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        *error = QStringLiteral("cannot read %1: %2").arg(path, f.errorString());
        return false;
    }
    QTextStream in(&f);
    int lineNo = 0;
    while (!in.atEnd()) {
        ++lineNo;
        const QString line = in.readLine().trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) {
            continue;
        }
        const int eq = line.indexOf(QLatin1Char('='));
        if (eq <= 0) {
            *error = QStringLiteral("%1:%2: expected name=index").arg(path).arg(lineNo);
            return false;
        }
        const QString name = line.left(eq).trimmed();
        bool ok = false;
        const uint idx = line.mid(eq + 1).trimmed().toUInt(&ok);
        if (!ok || idx > 0xFFFFu) {
            *error = QStringLiteral("%1:%2: '%3' is not a signal index")
                         .arg(path).arg(lineNo).arg(line.mid(eq + 1).trimmed());
            return false;
        }
        out->signalIndex.insert(name, quint16(idx));
    }
    return true;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QStringList args = QCoreApplication::arguments();

    // --sim/--ticks are pulled out before the positional check so the three
    // required arguments stay in fixed places whether or not simulation is
    // asked for.
    QVector<QPair<quint16, float>> seeds;
    int simTicks = 0;
    bool simulate = false;
    for (int i = args.size() - 1; i >= 1; --i) {
        if (args[i] == QLatin1String("--sim") && i + 1 < args.size()) {
            simulate = true;
            for (const QString &kv : args[i + 1].split(QLatin1Char(','),
                                                       Qt::SkipEmptyParts)) {
                const int eq = kv.indexOf(QLatin1Char('='));
                if (eq > 0) {
                    seeds.append({ quint16(kv.left(eq).toUInt()),
                                   kv.mid(eq + 1).toFloat() });
                }
            }
            args.removeAt(i + 1);
            args.removeAt(i);
        } else if (args[i] == QLatin1String("--ticks") && i + 1 < args.size()) {
            simTicks = args[i + 1].toInt();
            args.removeAt(i + 1);
            args.removeAt(i);
        }
    }

    if (args.size() != 4) {
        err(QStringLiteral(
            "usage: scriptc <script.lua> <symbols.txt> <out.bin>\n"
            "                 [--sim slot=value[,slot=value...]] [--ticks N]\n"
            "\n"
            "  symbols.txt: one 'Channel Name=slot' per line; # comments allowed.\n"
            "\n"
            "Writes the raw script image (ScriptHeader + bytecode) to out.bin and\n"
            "prints its shape to stdout. Exit 1 on a compile error, 2 on I/O.\n"
            "\n"
            "With --sim, also runs the image on the DEVICE'S OWN interpreter\n"
            "(script_exec.c, compiled in) for --ticks ticks with the given slots\n"
            "seeded, and prints each slot's resulting value as raw float bits.\n"
            "Bits, not decimals: the point of the comparison against a board is\n"
            "that the two agree exactly, and a decimal rendering would hide the\n"
            "last few bits that claim rests on."));
        return 2;
    }

    QFile src(args[1]);
    if (!src.open(QIODevice::ReadOnly | QIODevice::Text)) {
        err(QStringLiteral("cannot read %1: %2").arg(args[1], src.errorString()));
        return 2;
    }
    const QString source = QString::fromUtf8(src.readAll());

    ct::ScriptSymbols symbols;
    QString error;
    if (!readSymbols(args[2], &symbols, &error)) {
        err(error);
        return 2;
    }

    const ct::ScriptCompiler::Result r = ct::ScriptCompiler::compile(source, symbols);
    if (!r.ok) {
        // The line number goes in the conventional file:line: form so an editor
        // or a test log can point at the offending line without parsing prose.
        err(r.errorLine > 0
                ? QStringLiteral("%1:%2: %3").arg(args[1]).arg(r.errorLine).arg(r.error)
                : QStringLiteral("%1: %2").arg(args[1], r.error));
        return 1;
    }
    for (const QString &w : r.warnings) {
        err(QStringLiteral("%1: warning: %2").arg(args[1], w));
    }

    QFile out(args[3]);
    if (!out.open(QIODevice::WriteOnly)) {
        err(QStringLiteral("cannot write %1: %2").arg(args[3], out.errorString()));
        return 2;
    }
    if (out.write(r.image) != r.image.size()) {
        err(QStringLiteral("short write to %1").arg(args[3]));
        return 2;
    }
    out.close();

    // Machine-readable on purpose: "key=value" lines a test can parse without a
    // regex over prose, and a person can still read.
    QTextStream(stdout)
        << "bytes=" << r.image.size() << "\n"
        << "instructions=" << r.instructionCount << "\n"
        << "state_declared=" << r.stateDeclared << "\n"
        << "state_used=" << r.stateUsed << "\n"
        << "registers=" << r.registersUsed << "\n"
        << "straight_line_cost=" << r.straightLineCost << "\n"
        << "loops=" << (r.loopsPresent ? 1 : 0) << "\n";

    if (simulate) {
        ct::ScriptSimulator sim;
        const quint8 rc = sim.load(r.image);
        if (rc != 0) {
            err(QStringLiteral("simulator refused the image: code %1").arg(rc));
            return 1;
        }
        for (const auto &s : seeds) {
            sim.setSignal(s.first, s.second);
        }
        // Named slots only, so the output is comparable against a device's
        // value stream without the caller having to know the whole table.
        QHash<QString, quint16> watched = symbols.signalIndex;
        ct::ScriptSimulator::TickResult last;
        for (int i = 0; i < qMax(1, simTicks); ++i) {
            // Re-seed every tick: on the device these slots are refreshed by
            // the receive path each tick too, and a script that WRITES one it
            // also reads would otherwise diverge for a reason that has nothing
            // to do with the VM.
            for (const auto &s : seeds) {
                sim.setSignal(s.first, s.second);
            }
            last = sim.step(watched);
        }
        QTextStream out(stdout);
        out << "sim_ticks=" << qMax(1, simTicks) << "\n"
            << "sim_fault=" << last.fault << "\n"
            << "sim_cost=" << last.cost << "\n";
        for (const auto &cv : last.channels) {
            quint32 bits = 0;
            std::memcpy(&bits, &cv.value, sizeof(bits));
            out << "sim_slot_" << cv.index << "=0x"
                << QString::number(bits, 16).rightJustified(8, QLatin1Char('0'))
                << "\n";
        }
    }
    return 0;
}
