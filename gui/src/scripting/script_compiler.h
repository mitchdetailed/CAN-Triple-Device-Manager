// Compiling a Lua subset to device bytecode (script_vm.h).
//
// WHY THIS IS NOT "just walk Lua's AST": Lua exposes no AST. Its parser
// (lparser.c) goes straight from source to Lua's own register bytecode, which
// is an internal format with upvalues, closures and metatables, and which
// changes between releases. Reverse-engineering it would be a worse dependency
// than a parser.
//
// So the split is: the embedded Lua is the SYNTAX authority — luaL_loadbuffer
// decides whether the text is valid Lua at all, and its error messages (with
// line numbers) are better than anything worth hand-writing — and the parser
// here is the SUBSET authority, walking the same text to emit bytecode. A
// script therefore fails in one of two clearly different ways: "that is not
// Lua" (from Lua) or "that is Lua this device cannot run" (from here), and the
// second names the construct and why.
//
// The compiler runs script_verify() on its own output before returning. A
// compiler that emitted an image the device would reject is a compiler bug, and
// it should be caught on the desktop rather than by a unit in a vehicle.
//
// ---------------------------------------------------------------------------
// THE LANGUAGE
// ---------------------------------------------------------------------------
//
//   -- Persistent state, declared at file scope. Survives across ticks.
//   local count = state(0)
//   local mode  = state(0)
//
//   function on_tick()
//       local rpm = sig("Engine RPM")        -- read a channel by NAME
//       local hot = rpm > 6000
//
//       if hot then
//           setSig("Fan Request", 1)
//       else
//           setSig("Fan Request", 0)
//       end
//
//       count = count + 1
//       if count >= 100 then count = 0 end
//   end
//
// Channels are addressed by name, resolved here against the configuration's
// signal map — a script that names a channel which does not exist is a compile
// error, not a silent read of slot 0.
//
// Available: local variables, arithmetic (+ - * / %), comparison
// (< <= > >= == ~=), and/or/not, if/elseif/else, while, numeric for with a
// CONSTANT step, break, bare return, and the intrinsics
//   sig(name) setSig(name, v) abs min max floor ceil round sqrt
//   clamp(x,lo,hi) lerp(a,b,t) select(c,a,b) wrap(x,lo,hi)
//
// Deliberately absent, each a compile error naming itself: tables, strings as
// values, closures, function definitions other than the hooks, varargs, goto,
// repeat, `#`, concatenation, and the transcendentals (sin/cos/exp/log/pow and
// `^`). The transcendentals are refused for a specific reason, not squeamishness
// — IEEE-754 does not require them to be correctly rounded, so device libm and
// host libm may differ by an ULP, and a comparison on that difference branches
// differently. Excluding them is what lets the desktop simulator's result be
// trusted on the device (see DETERMINISM in script_vm.h).
//
// `and`/`or` yield 1.0/0.0 rather than Lua's operand values, and do NOT
// short-circuit. Both are safe here because expressions in this subset have no
// side effects — sig() is a pure read and setSig() is a statement — so the only
// visible difference is the value, which the docs state.
#pragma once

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

// device_mapper.h, not just wire_structs.h: mapWithScript() below returns a
// MappingResult. The dependency runs one way — the mapper knows nothing about
// scripting — which is what keeps Lua out of everything that maps.
#include "../model/device_mapper.h"

namespace ct {

class Configuration;

// Channel name -> device signal slot. Built from the configuration's mapping so
// a script names channels the same way the rest of the program does.
struct ScriptSymbols {
    QHash<QString, quint16> signalIndex;

    // Build from a configuration by running the device mapper. Returns false
    // with *error when the configuration itself cannot be mapped (the same
    // failure a Send would hit), because a script cannot be resolved against a
    // configuration that has no valid signal layout.
    static bool fromConfiguration(const Configuration &config, ScriptSymbols *out,
                                  QString *error);
};

class ScriptCompiler
{
public:
    struct Result {
        bool ok = false;
        QByteArray image;      // ScriptHeader + bytecode, ready to chunk
        QString error;         // when !ok
        int errorLine = 0;     // 1-based; 0 when not line-specific
        QStringList warnings;

        // Reported so the editor can show the budget picture before anything is
        // sent. straightLineCost is the cost of one pass with every loop taken
        // ZERO times and every branch's cheaper arm chosen: a floor, not a
        // bound. A script with loops can cost far more, which is what the
        // simulator is for — hence loopsPresent, so the UI knows when to say so.
        int instructionCount = 0;
        // stateUsed is what the DEVICE allocates, which is one more than the
        // script declared whenever there are initialisers: the compiler spends a
        // slot on a once-only flag so `state(0)` sets the value on the first
        // tick and not on every one. stateDeclared is what the user wrote, and
        // is what a state view should show — the flag is bookkeeping and would
        // only be a register nobody can explain.
        int stateUsed = 0;
        int stateDeclared = 0;
        int registersUsed = 0;
        quint32 straightLineCost = 0;
        bool loopsPresent = false;
    };

    static Result compile(const QString &source, const ScriptSymbols &symbols);

    // Turn the document's script — however it holds one — into the chunks a Send
    // writes, and apply the precedence rule while doing it. This is the ONE
    // place a document's script becomes bytes for a device:
    //
    //   a source          -> compile it; that is the script
    //   a retained image  -> send it back verbatim, byte for byte
    //   neither           -> zero chunks, which is how a script is REMOVED from
    //                        a device: the Send writes the table as it finds it
    //
    // Returns false with *error when the script cannot be sent, and a Send must
    // treat that as blocking rather than as a warning. Two ways it happens:
    //
    //   - the source does not compile. Installing a configuration whose script
    //     does not build would leave the device running the PREVIOUS script
    //     against NEW tables, a combination nobody has tested.
    //   - the retained image does not pass the device's verifier. It is refused
    //     HERE, before the transfer's CLEAR_CONFIG erases the unit, because a
    //     corrupt image written back leaves the device with neither the old
    //     script nor a new one.
    static bool attachTo(const Configuration &config, const ScriptSymbols &symbols,
                         QVector<ScriptChunk> *chunks, QString *error);
};

// mapToDevice() with the document's script attached to tables.scriptChunks —
// compiled from source, or the image a Get retained, whichever the document
// holds. THIS is what a Send, a Verify or anything else comparing against a
// device should call; plain mapToDevice() leaves scriptChunks empty, which reads
// as "this configuration has no script" and would quietly send a document's
// script nowhere, or report a device that has one as differing.
//
// A compile failure — or a retained image the device's verifier refuses — comes
// back as an ordinary entry in MappingResult::errors, so every existing
// `if (!mapped.ok())` path already blocks the Send before anything is erased.
//
// It lives here rather than in device_mapper.cpp because the compiler needs Lua
// and needs the signal map the mapper produces; putting it in the mapper would
// make the dependency circular and drag Lua into every test that links a
// mapping.
MappingResult mapWithScript(const Configuration &config);

} // namespace ct
