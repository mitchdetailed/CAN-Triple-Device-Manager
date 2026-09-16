#include "script_simulator.h"

#include <cstring>

extern "C" {
#include "script_exec.h"
#include "script_vm.h"
}

namespace ct {
namespace {

// The simulated value table. Sized like the TARGET device's — the channel
// count the script was compiled against, handed to load() — so a script that
// writes the highest legal slot behaves the same here, and so script_verify()'s
// max_signals bound means the same thing on both sides. A vector rather than a
// MAX_SIGNALS array because a firmware variant may hold more channels than this
// build's constant, and the one place the Manager still sized itself from the
// constant would otherwise refuse, in the simulator, a script the unit runs.
// Never smaller than the built-in number: a script compiled for a smaller
// variant runs against the larger table without harm, and setSignal() callers
// that predate variants keep their range.
int g_signals = MAX_SIGNALS;
QVector<float> g_sig(MAX_SIGNALS);

float simRead(uint16_t idx) { return idx < g_signals ? g_sig[idx] : 0.0f; }

// Which slots the current tick wrote, so the UI can distinguish "the script set
// this" from "this is the value I seeded". The VM has no notion of this — it is
// purely a simulator affordance — so it is tracked by wrapping the host writer.
QVector<bool> g_written(MAX_SIGNALS);
bool g_trackWrites = false;

void trackingWrite(uint16_t idx, float v)
{
    if (idx < g_signals) {
        g_sig[idx] = v;
        if (g_trackWrites) {
            g_written[idx] = true;
        }
    }
}

void sizeTables(int signalSlots)
{
    g_signals = qMax(signalSlots, int(MAX_SIGNALS));
    g_sig.fill(0.0f, g_signals);
    g_written.fill(false, g_signals);
}

} // namespace

ScriptSimulator::ScriptSimulator()
{
    static const ScriptHost host = { simRead, trackingWrite };
    script_exec_init(&host);
    reset();
}

ScriptSimulator::~ScriptSimulator()
{
    // Leave the shared VM with nothing loaded: the image this object owns is
    // about to be freed, and the VM holds pointers into it.
    script_exec_clear();
}

quint8 ScriptSimulator::load(const QByteArray &image, int signalSlots)
{
    m_signalSlots = signalSlots;
    reset();
    // The VM keeps POINTERS into the image rather than copying it (on the
    // device it points straight at memory-mapped flash), so the buffer has to
    // outlive the load — hence the member copy.
    m_image = image;
    const quint8 rc = script_exec_load(m_image.constData(),
                                       quint32(m_image.size()), quint16(g_signals));
    m_loaded = (rc == SCRIPT_OK);
    return rc;
}

void ScriptSimulator::setSignal(quint16 index, float value)
{
    if (index < g_signals) {
        g_sig[index] = value;
    }
}

float ScriptSimulator::signal(quint16 index) const
{
    return index < g_signals ? g_sig[index] : 0.0f;
}

void ScriptSimulator::reset()
{
    sizeTables(m_signalSlots);
    m_tick = 0;
    // Reload rather than merely clearing state, so a reset genuinely reproduces
    // a fresh config load — including re-running the script's state()
    // initialisers on the next tick.
    if (!m_image.isEmpty()) {
        m_loaded = script_exec_load(m_image.constData(), quint32(m_image.size()),
                                    quint16(g_signals)) == SCRIPT_OK;
    }
}

ScriptSimulator::TickResult ScriptSimulator::step(const QHash<QString, quint16> &watched)
{
    TickResult r;
    r.tick = m_tick;
    if (!m_loaded) {
        return r;
    }

    g_written.fill(false);
    g_trackWrites = true;
    script_exec_begin_tick();
    r.fault = script_exec_on_tick();
    g_trackWrites = false;
    ++m_tick;

    // The cost the tick actually spent, straight from the VM's own accounting —
    // the same counter that would have faulted it, so what the simulator shows
    // is what the device would charge.
    // The VM's own struct, not wire_structs.h's mirror of it (both are in
    // scope here now that the header names MAX_SIGNALS).
    ::ScriptStatus st{};
    script_exec_status(&st);
    r.cost = st.last_cost;

    for (auto it = watched.constBegin(); it != watched.constEnd(); ++it) {
        ChannelValue cv;
        cv.name = it.key();
        cv.index = it.value();
        cv.value = signal(it.value());
        cv.writtenByScript = it.value() < g_signals && g_written[it.value()];
        r.channels.append(cv);
    }
    std::sort(r.channels.begin(), r.channels.end(),
              [](const ChannelValue &a, const ChannelValue &b) {
                  return a.name.localeAwareCompare(b.name) < 0;
              });

    for (unsigned i = 0; i < SCRIPT_NUM_STATE; ++i) {
        r.state.append(script_exec_state(quint16(i)));
    }
    return r;
}

quint32 ScriptSimulator::budget() const
{
    return SCRIPT_TICK_BUDGET;
}

} // namespace ct
