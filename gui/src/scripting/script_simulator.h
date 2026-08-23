// Running a compiled script on the desktop, exactly as the device would.
//
// This is not a model of the VM — it IS the VM. script_exec.c and
// engine_math.c are compiled into the configurator and driven here against a
// RAM signal table, so "it worked in the simulator" and "it works on the unit"
// are the same statement about the same code. The determinism rules in
// script_vm.h (no transcendentals, no non-finite constants, correctly-rounded
// ops only) are what make that true bit for bit rather than approximately.
//
// One consequence worth knowing: script_exec.c holds ONE VM instance in file
// statics, because a device runs one script. So there is one simulator at a
// time, and running it disturbs any script state the same process had loaded.
// That is fine for a modal editor and would not be for a background service.
#pragma once

#include <QHash>
#include <QString>
#include <QVector>

namespace ct {

class ScriptSimulator
{
public:
    // A channel's value going into a tick, and what the script left there.
    struct ChannelValue {
        QString name;
        quint16 index = 0;
        float value = 0;
        bool writtenByScript = false;
    };

    struct TickResult {
        int tick = 0;
        quint8 fault = 0;          // SCRIPT_FAULT_*
        quint32 cost = 0;          // budget units this tick actually spent
        QVector<ChannelValue> channels;
        QVector<float> state;      // the persistent registers after the tick
    };

    ScriptSimulator();
    ~ScriptSimulator();

    // Load compiled bytecode. Returns the script_verify() code; SCRIPT_OK means
    // ready to run. Clears all signal and state values.
    quint8 load(const QByteArray &image);

    // Seed an input before running. Channels the script writes will be
    // overwritten by it; channels it only reads keep what is set here, which is
    // how a "what happens at 96 degrees?" question is asked.
    void setSignal(quint16 index, float value);
    float signal(quint16 index) const;

    // Run one tick and report what changed. `watched` is the channel set the
    // caller wants back (name -> signal index), so the result carries names
    // rather than raw slots.
    TickResult step(const QHash<QString, quint16> &watched);

    // Reset signals AND persistent state, as a fresh config load does.
    void reset();

    int tickCount() const { return m_tick; }
    quint32 budget() const;

private:
    int m_tick = 0;
    bool m_loaded = false;
    QByteArray m_image;   // kept alive: the VM holds pointers into it
};

} // namespace ct
