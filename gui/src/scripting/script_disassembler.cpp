#include "script_disassembler.h"

#include <QCoreApplication>
#include <QStringList>

#include <cstring>

#include "../model/device_mapper.h" // validateScriptImage / scriptVerifyText
#include "../protocol/wire_structs.h"

// The contract this decodes. Every field offset, opcode value and limit below
// is from here and nowhere else.
extern "C" {
#include "script_vm.h"
}

namespace ct {

namespace {

QString reg(quint8 r)
{
    return QStringLiteral("r%1").arg(r);
}

// A LOADK immediate is the float's BIT PATTERN, not its value — memcpy, not a
// cast, because reading a float through a uint32_t* is undefined and the
// optimiser is entitled to notice.
QString immediateAsFloat(quint32 imm)
{
    float value = 0.0f;
    std::memcpy(&value, &imm, sizeof(value));
    // 'g' with 7 significant digits: enough to round-trip a float32 in almost
    // every case, short enough that 1.0 prints as "1" rather than "1.0000000".
    return QString::number(double(value), 'g', 7);
}

// "signal[10]" — with the document's current name for the slot appended when it
// has one. The NUMBER is always shown and always first, because the number is
// what the image carries: a document whose channels were edited after the Get
// can name slot 10 something the device has never called it, and a listing that
// showed only the name would be quietly describing a different script.
QString signalRef(quint32 imm, const QHash<quint16, QString> &channelNames)
{
    const QString slot = QStringLiteral("signal[%1]").arg(imm);
    if (imm > 0xFFFF) {
        return slot; // cannot be a slot index; the verifier would have refused it
    }
    const QString name = channelNames.value(quint16(imm));
    return name.isEmpty() ? slot : QStringLiteral("%1 (%2)").arg(slot, name);
}

} // namespace

ScriptListing disassembleScriptImage(const QByteArray &image,
                                     const QHash<quint16, QString> &channelNames)
{
    ScriptListing out;
    out.maxInstructions = int(SCRIPT_MAX_INSTRUCTIONS);
    out.imageBytes = image.size();

    if (image.isEmpty()) {
        return out; // no script; valid stays false and error stays empty
    }

    // The DEVICE's verifier decides whether these bytes are decodable, not this
    // function. That is not belt-and-braces: an image that fails verification
    // may have a code_bytes that runs off the end of the buffer, an opcode that
    // is not an instruction, or a jump into the middle of nothing — and a
    // decoder that walked it anyway would print a plausible listing of
    // instructions the device would never run. Refusing, and saying why, is the
    // honest answer and it is the same verdict the Send path gives.
    QString reason;
    if (!validateScriptImage(image, &reason)) {
        out.error = reason;
        return out;
    }

    ScriptHeader header{};
    std::memcpy(&header, image.constData(), sizeof(header));
    out.valid = true;
    out.stateCount = int(header.num_state);
    out.codeBytes = int(header.code_bytes);
    out.instructionCount = int(header.code_bytes / SCRIPT_INSTR_SIZE);
    out.hasTickEntry = header.entry_tick != SCRIPT_NO_ENTRY;
    out.entryTick = out.hasTickEntry ? int(header.entry_tick) : 0;

    for (int i = 0; i < out.instructionCount; ++i) {
        ScriptInstr instr{};
        std::memcpy(&instr, image.constData() + sizeof(ScriptHeader) + i * SCRIPT_INSTR_SIZE,
                    sizeof(instr));

        ScriptListingLine line;
        line.index = i;
        line.cost = script_op_cost(instr.op);
        line.isEntry = out.hasTickEntry && i == out.entryTick;

        if (instr.op <= SCRIPT_OP_ARITH_MAX) {
            // The opcode IS the MathOp, so the mnemonic is the math op's name
            // and the arity is mathOpArity() — the same two answers the math
            // grid gives, from the same table, so the listing cannot call an
            // op something the rest of the program does not.
            //
            // Register c rides in imm's LOW BYTE (script_vm.h, INSTRUCTION
            // FORMAT); the verifier has already proved the other three bytes
            // are zero, so masking is a read of the field rather than a repair.
            line.mnemonic = QString::fromLatin1(mathOpName(instr.op));
            const int arity = mathOpArity(instr.op);
            QStringList args{ reg(instr.a) };
            if (arity >= 2) {
                args << reg(instr.b);
            }
            if (arity >= 3) {
                args << reg(quint8(instr.imm & 0xFFu));
            }
            line.operands = QStringLiteral("%1 <- (%2)").arg(reg(instr.dst), args.join(
                QStringLiteral(", ")));
        } else {
            switch (instr.op) {
            case SCRIPT_OP_LOADK:
                line.mnemonic = QStringLiteral("LOADK");
                line.operands = QStringLiteral("%1 = %2").arg(reg(instr.dst),
                                                              immediateAsFloat(instr.imm));
                break;
            case SCRIPT_OP_LOADSIG:
                line.mnemonic = QStringLiteral("LOADSIG");
                line.operands = QStringLiteral("%1 = %2").arg(reg(instr.dst),
                                                              signalRef(instr.imm, channelNames));
                break;
            case SCRIPT_OP_STORESIG:
                line.mnemonic = QStringLiteral("STORESIG");
                line.operands = QStringLiteral("%1 = %2").arg(signalRef(instr.imm, channelNames),
                                                              reg(instr.a));
                break;
            case SCRIPT_OP_LOADST:
                line.mnemonic = QStringLiteral("LOADST");
                line.operands = QStringLiteral("%1 = state[%2]").arg(reg(instr.dst)).arg(instr.imm);
                break;
            case SCRIPT_OP_STOREST:
                line.mnemonic = QStringLiteral("STOREST");
                line.operands = QStringLiteral("state[%1] = %2").arg(instr.imm).arg(reg(instr.a));
                break;
            case SCRIPT_OP_MOV:
                line.mnemonic = QStringLiteral("MOV");
                line.operands = QStringLiteral("%1 = %2").arg(reg(instr.dst), reg(instr.a));
                break;
            case SCRIPT_OP_JMP:
                line.mnemonic = QStringLiteral("JMP");
                // An INSTRUCTION index, which is why the listing is numbered in
                // instructions: "-> 12" has to name a line the reader can find.
                line.operands = QStringLiteral("-> %1").arg(instr.imm);
                break;
            case SCRIPT_OP_JZ:
                line.mnemonic = QStringLiteral("JZ");
                line.operands = QStringLiteral("if %1 == 0 -> %2").arg(reg(instr.a)).arg(instr.imm);
                break;
            case SCRIPT_OP_JNZ:
                line.mnemonic = QStringLiteral("JNZ");
                line.operands = QStringLiteral("if %1 != 0 -> %2").arg(reg(instr.a)).arg(instr.imm);
                break;
            case SCRIPT_OP_HALT:
                line.mnemonic = QStringLiteral("HALT");
                break;
            default:
                // Unreachable: script_verify() returns ERR_OPCODE for anything
                // not handled above, and this function returned early on that.
                // Kept so a future opcode added to the verifier and not to this
                // switch shows up as a named gap instead of a blank line.
                line.mnemonic = QStringLiteral("?");
                line.operands = QStringLiteral("unknown opcode 0x%1")
                                    .arg(instr.op, 2, 16, QLatin1Char('0'));
                break;
            }
        }
        out.lines.append(line);
    }
    return out;
}

QString scriptListingSummary(const ScriptListing &listing)
{
    if (!listing.valid) {
        if (listing.imageBytes == 0) {
            return QCoreApplication::translate("ct::ScriptDisassembler",
                                               "This document holds no compiled script.");
        }
        // The verifier's words, because they are the same words the Send would
        // refuse with — a user who sees "checksum mismatch" here and "checksum
        // mismatch" there is looking at one fault, not two.
        return QCoreApplication::translate(
                   "ct::ScriptDisassembler",
                   "This is not a script image the device would accept (%1), so it cannot "
                   "be listed. %2 bytes.")
            .arg(listing.error)
            .arg(listing.imageBytes);
    }

    // The instruction count against the device's ceiling, because that ceiling
    // is the one a script actually runs into (SCRIPT_MAX_INSTRUCTIONS), and a
    // listing that showed a count with nothing to measure it against would
    // leave the reader to guess whether 900 was a lot.
    QString text = QCoreApplication::translate(
                       "ct::ScriptDisassembler",
                       "%1 of %2 instructions, %3 state register(s), %4 bytes of bytecode "
                       "(%5 bytes with the header).")
                       .arg(listing.instructionCount)
                       .arg(listing.maxInstructions)
                       .arg(listing.stateCount)
                       .arg(listing.codeBytes)
                       .arg(listing.imageBytes);
    if (listing.hasTickEntry) {
        text += QCoreApplication::translate("ct::ScriptDisassembler",
                                            " on_tick starts at instruction %1.")
                    .arg(listing.entryTick);
    } else {
        // A legal image that never runs. Worth saying plainly: it verifies, it
        // loads, the device reports a script present — and nothing happens.
        text += QCoreApplication::translate(
            "ct::ScriptDisassembler",
            " This image implements no on_tick, so the device would load it and never "
            "run it.");
    }
    return text;
}

} // namespace ct
