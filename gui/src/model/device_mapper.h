// Translates the document model into the firmware's four config tables and
// back. Keeps the signal-index <-> channel-name maps used to label live
// value-stream data.
#pragma once

#include <QByteArray>
#include <QHash>
#include <QStringList>
#include <QVector>

#include "../protocol/wire_structs.h"
#include "comms_types.h"

namespace ct {

class Configuration;

struct DeviceTables {
    QVector<CanMessageConfig> messages; // receive AND transmit (direction is a flag)
    QVector<CanSignalConfig> signalConfigs;
    QVector<MathConfig> math;
    QVector<ConditionConfig> conditions;
    QVector<CounterConfig> counters; // firmware v3+
    QVector<TimerConfig> timers;     // firmware v3+
    QVector<ConstantConfig> constants; // firmware v6+
    QVector<RelayConfig> relays;       // firmware v11+ (message relay rules)
    // firmware v13+: the 1-axis 16-site table, split across two parallel
    // tables indexed in lockstep. These two ALWAYS have the same size — the
    // device only evaluates indices present in both.
    QVector<Table2x16Def> tables2x16Def;
    QVector<Table2x16Out> tables2x16Out;
    // The 2-axis 8x8 table, which replaced the 4x4. These two are NOT
    // index-aligned the way the 2x16 pair is: tables8x8Def[t] owns
    // tables8x8Row[t*8 .. t*8+7], so tables8x8Row is always exactly
    // TABLE_8X8_SITES times the size of tables8x8Def. Every table contributes
    // all eight rows even when it uses fewer Y sites — the unused ones are
    // zero-filled — because the device addresses a row by t*8 + y and a short
    // table would slide every later one out of position.
    QVector<Table8x8Def> tables8x8Def;
    QVector<Table8x8GridRow> tables8x8Row;
    QVector<IntegratorConfig> integrators; // firmware v16+ (rate accumulators)
    // Transmit-CRC8 rules (firmware store v8): one Crc8Config per stamped
    // transmit message, bound to `messages` by msg_idx — which is why the
    // mapper fills this INSIDE its message walk, where that index is known.
    // Every record here carries CRC8FLAG_ACTIVE, deliberately: whether the
    // stamp runs is decided by the stamped MESSAGE's own ACTIVE flag (an Off
    // bus deactivates the message, and an un-composed frame is never
    // stamped), so a rule uploaded without ACTIVE would encode nothing —
    // except ambiguity on read-back, where !ACTIVE is the only way to tell a
    // zero-filled slot from a record (msg_idx 0 is a valid message index).
    QVector<Crc8Config> crc8;
    // Where the firmware publishes the values it produces about itself. Not a
    // table: one small record that rides in the config header. Each entry is
    // SIG_MSG_NONE unless something in the document actually reads that device
    // channel, so an unused device channel costs no signal slot and no bytes.
    // Initialised through the factory, not a brace list — see its comment.
    DeviceChannelsConfig deviceChannels = unusedDeviceChannels();
    // Compiled device-script bytecode, as 64-byte chunks (firmware store v5).
    // EMPTY when the document has no script — which is both the normal case and
    // what CLEARS a script off a device, since a Send writes each table as it
    // finds it.
    //
    // mapToDevice does NOT fill this. The compiler needs Lua and, more to the
    // point, needs the signal map that mapToDevice itself produces — so the
    // caller compiles and calls attachCompiledScript() below. Keeping the
    // circularity out of the mapper also keeps Lua out of every test that links
    // it.
    //
    // Going the OTHER way, a Get fills this from the device and mapFromDevice
    // keeps it: the image lands in Configuration::scriptBytecode() so a Send can
    // put the same script back byte for byte. That direction needs no Lua, only
    // the verifier — see scriptImageFromChunks() below.
    QVector<ScriptChunk> scriptChunks;
};

struct MappingResult {
    DeviceTables tables;
    QHash<int, QString> signalToChannel;          // signal idx -> channel name
    QHash<QString, int> channelToSignal;          // lower-case name -> signal idx
    QStringList errors;                           // blocking problems
    QStringList warnings;

    bool ok() const { return errors.isEmpty(); }
};

// Document -> firmware tables.
MappingResult mapToDevice(const Configuration &config);

// Firmware tables -> document (lossy; names come from signal labels).
// Existing document content is replaced.
// `busSetup` is the device's ControlCanPayload[3] when CMD_READ_CAN_SETUP
// answered, and EMPTY when it did not (older firmware). Empty means UNKNOWN, not
// "all buses off": the mapper then assumes the firmware bring-up rates and adds
// a note telling the user to check, which is what every Get did before the
// command existed. Passing a wrongly-sized vector is treated the same way.
void mapFromDevice(const DeviceTables &tables, Configuration &config,
                   QStringList *notes = nullptr,
                   const QVector<ControlCanPayload> &busSetup = {});

// ---- The device script, as an image ---------------------------------------
//
// A retained script image is bytes off a wire that are about to be written back
// into a unit, so it is validated at BOTH ends of the trip — when a Get produces
// it and again before a Send emits it. The second check is not redundant: the
// image may have travelled through a .ct3 (or a hand-edited one) since the Get,
// and the Send that would install it happens after CLEAR_CONFIG has already
// erased the device. Refusing late is refusing after the damage.

// Would the DEVICE accept `image` as a script? This runs script_verify() — the
// firmware's own verifier, byte for byte the function the unit runs before it
// will execute a stored script — rather than a host-side opinion about the
// format, so a yes here is the unit's yes. *reason, when given, gets the
// verifier's verdict in words.
bool validateScriptImage(const QByteArray &image, QString *reason = nullptr);

// One of the verifier's verdicts (SCRIPT_OK, SCRIPT_ERR_*) in words. Shared so
// that a refusal reads the same wherever it surfaces — the Get note, the Send
// refusal, the Script Editor's report on its own compiler's output, and the
// disassembler's reason for not listing an image. The enum's ordering is stable
// ABI precisely so a host can do this (script_vm.h, "Verifier results").
QString scriptVerifyText(quint8 code);

// What a Get made of the script chunks it read back.
//
// `present` and `image` answer two different questions and both are needed. A
// Get reads the script table's FULL capacity and the device zero-fills every
// slot past what it has stored (engine_table_read), so a unit with a perfectly
// good configuration and no script answers with 32 KB of zeros — which is not a
// fault and must not be reported as one. `present` is false there. It is true,
// with `image` empty and `error` set, only when the device really is holding
// something that is not a script this build would send back.
struct RetainedScript {
    QByteArray image;     // ScriptHeader + code, trimmed to code_bytes
    bool present = false; // the device's script table holds something
    QString error;        // why `image` is empty although something was there
};
RetainedScript scriptImageFromChunks(const QVector<ScriptChunk> &chunks);

// Helper shared with validation: computes the firmware extraction fields for
// a row. Returns false (with reason) when the current firmware cannot express
// the row (multi-byte big-endian, non-contiguous mask, span overflow).
struct ExtractionFields {
    quint16 startBit = 0;
    quint8 bitLength = 0;
    quint8 byteOrder = 0; // 0 Intel, 1 Motorola
    quint8 valueType = SIGNAL_TYPE_UINT16;
};
bool computeExtraction(const CommsChannelRow &row, SectionAlignment alignment,
                       int messageLengthBytes, ExtractionFields *out, QString *reason);

// The largest frame anything here can describe: 64 bytes of CAN FD.
constexpr int MAX_FRAME_BITS = 64 * 8;

// The most distinct compound identifiers the firmware's transmit composer
// serves per message — MAX_TX_MUX_IDS in firmware/src/engine_core.c, mirrored
// by hand because it is engine-internal and so has no protocol.h line for
// wire_structs.h to mirror. collectMuxIdentifiers stops collecting at this
// many and says nothing: variants past it are accepted on Send, stored, and
// never transmitted, so the host must do the warning (mapToDevice) and the
// load estimate (ct3check) from the same number.
constexpr int MAX_TX_MUX_IDS = 32;

// Which physical bits of the frame a row occupies, as absolute positions
// (byte × 8 + bit within the byte), in SIGNAL order — element 0 is the signal's
// LSB. The walk is the one computeExtraction validates and the firmware's
// engine_extract_raw performs: ascend the bit within the byte, then step to the
// next byte (Intel / Word Swap) or the previous one (Motorola / Normal).
//
// Deliberately frame-length agnostic, because its two callers ask different
// questions: the overlap check wants "which bits does this claim", the layout
// map wants "where do I draw it". Positions outside 0..MAX_FRAME_BITS-1 are
// dropped rather than wrapped, so a row that runs off either end of the frame
// comes back SHORTER than its bit length — which is how a caller detects it.
QList<int> rowBitPositions(const CommsChannelRow &row, SectionAlignment alignment);

} // namespace ct
