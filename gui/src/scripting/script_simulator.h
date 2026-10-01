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

#include "../protocol/capacity.h"
#include "../protocol/wire_structs.h"

namespace ct {

// THE SCRIPT COST MODEL of the firmware a document is for: what each opcode
// costs and the tick budget, which the compiler (a script's straight-line
// cost), the disassembler (each line's charge) and the simulator (what a tick
// spends, and where it stops) all charge. The CAN Triple 1.x's is compiled
// into the VM's C (script_vm.c, script_exec.c). A CAN Triple 2.0 charges its
// own, measured on its FPU, and states it in its capacity report
// (DeviceCapacity::scriptCosts and scriptBudget).
//
// That C is the firmware's own and holds the model in globals, so a
// capacity's model is put in force for a scope, and the one before it put back
// when the scope ends:
//
//     const ScriptCostScope costs(config.capacity());
//     ... compile, disassemble, simulate ...
//
// A capacity that states no model (the CAN Triple 1.x, builtIn(), the first
// 2.0 builds) puts the 1.x one in force, which is what such a unit charges.
class ScriptCostScope
{
public:
    explicit ScriptCostScope(const DeviceCapacity &target);
    ~ScriptCostScope();
    ScriptCostScope(const ScriptCostScope &) = delete;
    ScriptCostScope &operator=(const ScriptCostScope &) = delete;

private:
    QByteArray m_costs; // the VM keeps a pointer into this, not a copy
    const quint8 *m_previousCosts = nullptr;
    quint32 m_previousCount = 0;
    quint32 m_previousBudget = 0;
};

// What the VM charges now, under whichever model is in force: one opcode, and
// the tick budget a tick stops at.
quint32 scriptOpCost(quint8 op);
quint32 scriptTickBudget();

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
    // ready to run. Clears all signal and state values. `signalSlots` is the
    // channel table the script was compiled against — the target firmware's
    // Signals capacity — and sizes the simulated table to match, so a script
    // built for a variant with more channels than this build's constant runs
    // here exactly as it will there. The default is the built-in number.
    quint8 load(const QByteArray &image, int signalSlots = MAX_SIGNALS);

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
    // The tick budget a tick stops at: the CAN Triple 1.x's, or the target's
    // while a ScriptCostScope (script_cost_model.h) is in force.
    quint32 budget() const;

private:
    int m_tick = 0;
    bool m_loaded = false;
    int m_signalSlots = MAX_SIGNALS; // the table size the image was loaded with
    QByteArray m_image;   // kept alive: the VM holds pointers into it
};

} // namespace ct
