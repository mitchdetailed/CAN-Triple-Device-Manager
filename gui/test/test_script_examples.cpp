// Every device-script example that ships with the program, compiled and run.
//
// The examples in examples/scripts/ are shipped into the installed program and
// offered by the editor's "Load Script…" button, so a user's first contact with
// scripting may well be loading one and pressing Send. An example that does not
// compile, or that the engine kills for exceeding its tick budget, is worse than
// no example at all: it reads as a broken product and the user has no way to know
// the fault is not theirs.
//
// Nothing here knows what any individual script DOES — that is the author's
// business and the comment block at the top of each file. What is checked is the
// contract every one of them must meet:
//
//   * it compiles against the real ScriptCompiler,
//   * the real device VM verifies the image,
//   * and over a long run, on several different input patterns, it never faults
//     and never exceeds SCRIPT_TICK_BUDGET.
//
// The input patterns matter. A script is easy to write so that it behaves on
// mid-range values and divides by zero on a cold start, so the sweep deliberately
// includes all-zero (every channel unread, engine stopped, vehicle stationary)
// and a saturating value, not just something comfortable.
//
// Symbols are derived from the source itself: every sig("X") and setSig("X") name
// is collected and given a slot. That keeps an example a single self-contained
// file with no companion symbol table to drift out of step with it.

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QString>
#include <QStringList>

#include <cstdio>

#include "../src/scripting/script_compiler.h"
#include "../src/scripting/script_simulator.h"

#include "script_vm.h"

static int fails = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);              \
            ++fails;                                                                 \
        }                                                                            \
    } while (0)

namespace {

using namespace ct;

// Every channel the script names, in the order first seen. Both sig() and
// setSig() take a literal name — the compiler requires it — so a regular
// expression over the source is a complete answer here, not an approximation.
ScriptSymbols symbolsFor(const QString &source)
{
    ScriptSymbols syms;
    // Custom delimiter: the pattern itself contains )" which would otherwise
    // end the raw string early.
    static const QRegularExpression re(
        QStringLiteral(R"RX((?:setSig|sig)\s*\(\s*"([^"]*)")RX"));
    quint16 next = 0;
    auto it = re.globalMatch(source);
    while (it.hasNext()) {
        const QString name = it.next().captured(1);
        if (!name.isEmpty() && !syms.signalIndex.contains(name))
            syms.signalIndex.insert(name, next++);
    }
    return syms;
}

struct Pattern {
    const char *what;
    float value;
};

// Seeds applied to every channel at once. All-zero is the cold-start case that
// finds unguarded division; the large value finds accumulators that saturate or
// wrap.
const Pattern kPatterns[] = {
    { "all zero", 0.0f },
    { "all one", 1.0f },
    { "mid-range", 100.0f },
    { "large", 4000.0f },
};

void checkExample(const QString &path)
{
    const QString name = QFileInfo(path).fileName();

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        std::printf("FAIL  %s: cannot read\n", qPrintable(name));
        ++fails;
        return;
    }
    const QString source = QString::fromUtf8(f.readAll());
    f.close();

    const ScriptSymbols syms = symbolsFor(source);
    const ScriptCompiler::Result r = ScriptCompiler::compile(source, syms);
    if (!r.ok) {
        std::printf("FAIL  %s: does not compile: %s\n", qPrintable(name),
                    qPrintable(r.error));
        ++fails;
        return;
    }

    ScriptSimulator sim;
    const quint8 verify = sim.load(r.image);
    if (verify != SCRIPT_OK) {
        std::printf("FAIL  %s: the device verifier rejects it (code %u)\n",
                    qPrintable(name), unsigned(verify));
        ++fails;
        return;
    }

    // What to read back — every channel, so the simulator reports them all.
    QHash<QString, quint16> watched = syms.signalIndex;

    quint32 peak = 0;
    for (const Pattern &p : kPatterns) {
        sim.reset();
        for (auto it = syms.signalIndex.constBegin(); it != syms.signalIndex.constEnd(); ++it)
            sim.setSignal(it.value(), p.value);

        // Long enough for a slow accumulator or a multi-tick state machine to
        // come round several times: 500 ticks is five seconds of device time.
        for (int tick = 0; tick < 500; ++tick) {
            const ScriptSimulator::TickResult t = sim.step(watched);
            peak = qMax(peak, t.cost);
            if (t.fault != 0) {
                std::printf("FAIL  %s: fault %u on tick %d with %s inputs "
                            "(cost %u)\n",
                            qPrintable(name), unsigned(t.fault), tick, p.what,
                            unsigned(t.cost));
                ++fails;
                return;
            }
            if (t.cost > SCRIPT_TICK_BUDGET) {
                std::printf("FAIL  %s: cost %u exceeds the %u budget with %s inputs\n",
                            qPrintable(name), unsigned(t.cost),
                            unsigned(SCRIPT_TICK_BUDGET), p.what);
                ++fails;
                return;
            }
        }
    }

    std::printf("  ok  %-28s peak cost %4u / %u\n", qPrintable(name), unsigned(peak),
                unsigned(SCRIPT_TICK_BUDGET));
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    const QString dir = argc > 1 ? QString::fromLocal8Bit(argv[1])
                                 : QStringLiteral(CT_EXAMPLES_DIR);
    QDir d(dir);
    const QStringList files = d.entryList({QStringLiteral("*.lua")}, QDir::Files, QDir::Name);

    // A sweep over an empty directory passes every assertion in it and proves
    // nothing — the one way this test could rot into a no-op is by being pointed
    // somewhere the examples are not. So the count is itself an assertion.
    if (files.isEmpty()) {
        std::printf("FAIL  no example scripts found in %s\n", qPrintable(d.absolutePath()));
        return 1;
    }
    std::printf("examples in %s\n", qPrintable(d.absolutePath()));

    for (const QString &f : files)
        checkExample(d.filePath(f));

    if (fails == 0) {
        std::printf("test_script_examples: %lld example(s), all compile and stay in budget\n",
                    static_cast<long long>(files.size()));
        return 0;
    }
    std::printf("test_script_examples: %d check(s) failed\n", fails);
    return 1;
}
