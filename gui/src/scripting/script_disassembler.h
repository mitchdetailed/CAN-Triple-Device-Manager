// A READ-ONLY listing of a compiled device script — index, mnemonic, operands.
//
// It exists for one situation: a document whose script is an image read back
// off a device (Configuration::scriptBytecode()). There is no Lua behind that
// image and there never will be — bytecode does not decompile — so without this
// the Script Editor would open on an empty box over a document that is carrying
// a working script. An empty editor that disagrees with the document is exactly
// the class of bug the retention work was done to remove, so the listing is how
// the editor SHOWS what it holds.
//
// READ-ONLY, deliberately, and not a step towards an assembler. Making the
// listing editable would need a validating assembler and a cost model on the
// host side to be safe, and the rule the whole feature rests on is that editing
// or compiling LUA is what changes what gets sent. The listing is a view.
//
// WHERE THE ENCODING COMES FROM. firmware/include/script_vm.h is the contract:
// 8-byte instructions (op, dst, a, b, imm), the opcode table, and the 32-byte
// header. This decoder is written from that header and cross-checked against
// firmware/tools/hwtest/script_asm.py, which is an INDEPENDENT assembler and
// disassembler for the same contract written from the same header — not from
// the C++ compiler's internals. Two readings of one specification is the only
// arrangement in which a misreading shows up as a disagreement rather than as
// two components being confidently wrong together. The operand text follows
// script_asm.disassemble()'s conventions for that reason.
#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>

namespace ct {

struct ScriptListingLine {
    // The INSTRUCTION index, not a byte offset. That is what a jump target
    // names (script_vm.h: "a jump target is simply an instruction index"), so
    // a listing numbered in bytes would make every JMP look wrong.
    int index = 0;
    QString mnemonic;   // ADD, LOADSIG, JZ, HALT…
    QString operands;   // "r0 <- (r1, r2)", "signal[10] = r3", "-> 4"
    quint32 cost = 0;   // script_op_cost(), the budget charge for one dispatch
    bool isEntry = false; // on_tick enters here
};

struct ScriptListing {
    // False when the image is not one the DEVICE would accept. Nothing is
    // decoded in that case: walking bytes the verifier rejected is how a
    // listing invents instructions that are not there. `error` is the
    // verifier's own verdict in words.
    bool valid = false;
    QString error;

    QList<ScriptListingLine> lines;
    int instructionCount = 0;
    int maxInstructions = 0;   // SCRIPT_MAX_INSTRUCTIONS, so callers need not include script_vm.h
    int stateCount = 0;        // header num_state
    int codeBytes = 0;
    int imageBytes = 0;
    bool hasTickEntry = false; // false when the image implements no on_tick
    int entryTick = 0;
};

// Decode `image` — a ScriptHeader followed by code_bytes of bytecode, exactly
// what Configuration::scriptBytecode() and ScriptCompiler::Result::image hold.
//
// `channelNames` maps a signal slot to the name THIS DOCUMENT currently gives
// it, and is used only to annotate LOADSIG/STORESIG. The numeric slot is always
// shown as well, because the image carries the number and nothing else: a
// document edited since the Get may name a slot something the device does not.
ScriptListing disassembleScriptImage(const QByteArray &image,
                                     const QHash<quint16, QString> &channelNames = {});

// One line describing the whole image: instructions against the device's
// ceiling, state registers, size, and where on_tick starts. Separate from the
// listing so the caller can put it beside the table rather than in it.
QString scriptListingSummary(const ScriptListing &listing);

} // namespace ct
