// End-to-end integration test: the GUI's protocol stack talking to the REAL
// firmware v2 core (engine_core.c / serial_proto.c / flash_store.c compiled
// for the host). Frames built by the GUI are fed into the firmware's parser;
// the firmware's responses are parsed by the GUI's splitter. Exits 0 on
// success.
#include <QByteArray>
#include <QCoreApplication>
#include <QList>

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <limits>

// GUI side (namespace ct)
#include "../src/model/access_keys.h"
#include "../src/model/config_lock.h"
#include "../src/model/configuration.h"
#include "../src/model/device_mapper.h"
#include "../src/protocol/cobs.h"
#include "../src/protocol/device_link.h"
#include "../src/protocol/device_session.h"
#include "../src/protocol/framer.h"

// Firmware side (global namespace; its macros collide with ct:: constant
// NAMES, so undef everything after including)
extern "C" {
#include "bcb.h"
#include "engine_core.h"
#include "flash_store.h"
#include "fw_host_stub.h"
#include "fw_image.h"
#include "fw_update.h"
#include "serial_proto.h"
#include "sha256.h"
}

// Snapshot the firmware's numeric constants BEFORE the #undef sweep below.
// Once those macros are gone there is no way left to compare them against the
// GUI's ct:: values of the same name â€” and those pairs being equal is exactly
// what makes the two headers one wire format.
namespace fw {
constexpr int kProtocolVersion = PROTOCOL_VERSION;
constexpr int kMaxIntegrators = MAX_INTEGRATORS;
constexpr int kIntegratorMaxHz = INTEGRATOR_MAX_HZ;
constexpr unsigned kIntegFlagActive = INTEGFLAG_ACTIVE;
constexpr unsigned kIntegFlagConstInput = INTEGFLAG_CONST_INPUT;
constexpr unsigned kIntegFlagCountDown = INTEGFLAG_COUNT_DOWN;
constexpr unsigned kIntegFlagPreserve = INTEGFLAG_PRESERVE;
constexpr int kPreserveKeyIntegratorBase = PRESERVE_KEY_INTEGRATOR_BASE;
constexpr int kPreserveKeyCount = PRESERVE_KEY_COUNT;
// v19 per-function access keys. Snapshotted here for the same reason as the
// rest: the two sides define these under identical names, and three
// independent locks are worthless if the host and the device disagree about a
// key width or about which bit means which function â€” a device that gated Get
// on the Send password would look like it worked right up until it mattered.
constexpr int kAccessKeyLen = ACCESS_KEY_LEN;
constexpr int kAccessChallengeLen = ACCESS_CHALLENGE_LEN;
constexpr unsigned kAccessFnSend = ACCESS_FN_SEND;
constexpr unsigned kAccessFnGet = ACCESS_FN_GET;
constexpr unsigned kAccessFnEditComms = ACCESS_FN_EDIT_COMMS;
constexpr unsigned kAccessFnCount = ACCESS_FN_COUNT;
constexpr unsigned kAccessMaskSend = ACCESS_MASK_SEND;
constexpr unsigned kAccessMaskGet = ACCESS_MASK_GET;
constexpr unsigned kAccessMaskEditComms = ACCESS_MASK_EDIT_COMMS;
constexpr unsigned kCmdReadAccessKeys = CMD_READ_ACCESS_KEYS;
constexpr unsigned kCmdWriteAccessKeys = CMD_WRITE_ACCESS_KEYS;
constexpr unsigned kCmdAccessChallenge = CMD_ACCESS_CHALLENGE;
constexpr unsigned kCmdAccessResponse = CMD_ACCESS_RESPONSE;
constexpr unsigned kErrLocked = ERR_LOCKED;
constexpr unsigned kErrInvalidCmd = ERR_INVALID_CMD;
constexpr unsigned kErrInvalidLen = ERR_INVALID_LEN;
// The fleet identity. Snapshotted for the same reason as everything else here:
// the two headers declare these under identical names, and a field width that
// had drifted would put the model id where the serial belongs â€” a device that
// reads as a plausible member of a fleet it has nothing to do with.
constexpr int kFleetVendorIdLen = FLEET_VENDOR_ID_LEN;
constexpr int kFleetModelIdLen = FLEET_MODEL_ID_LEN;
constexpr int kFleetKeyLen = FLEET_KEY_LEN;
constexpr unsigned kCmdReadFleetId = CMD_READ_FLEET_ID;
constexpr unsigned kCmdFleetIdProve = CMD_FLEET_ID_PROVE;
// The commit that carries the configuration's version number with it.
constexpr unsigned kCmdSaveToFlash = CMD_SAVE_TO_FLASH;
// v18 device binding.
constexpr int kUidLen = CONFIG_UID_LEN;
constexpr unsigned kStatusOk = CONFIG_STATUS_OK;
constexpr unsigned kStatusNone = CONFIG_STATUS_NONE;
constexpr unsigned kStatusWrongDevice = CONFIG_STATUS_WRONG_DEVICE;
constexpr unsigned kCmdGetDeviceId = CMD_GET_DEVICE_ID;
constexpr unsigned kCmdWriteBinding = CMD_WRITE_CONFIG_BINDING;
// Counter rate mode and device channels, snapshotted for the same reason as
// everything above: the macros are undefined below, and these pairs being equal
// is what makes the two headers one wire format.
constexpr unsigned kCounterFlagRateDown = COUNTERFLAG_RATE_DOWN;
constexpr int kCounterMaxHz = COUNTER_MAX_HZ;
constexpr unsigned kCmdWriteDeviceChannels = CMD_WRITE_DEVICE_CHANNELS;
constexpr unsigned kCmdReadDeviceChannels = CMD_READ_DEVICE_CHANNELS;
// The table capacities. These have never been snapshotted before because
// nothing compared them; the capacity expansion makes that a real hazard â€”
// every one of them sizes a flash region on the device AND a "will it fit"
// check in the host, and a host that thinks the device holds 250 messages when
// it holds 500 simply refuses configurations the hardware would have taken,
// while the reverse sends records into slots that do not exist.
constexpr int kMaxMessages = MAX_MESSAGES;
constexpr int kMaxSignals = MAX_SIGNALS;
constexpr int kMaxTimers = MAX_TIMERS;
constexpr int kMaxCounters = MAX_COUNTERS;
constexpr int kMaxConditions = MAX_CONDITIONS;
constexpr int kMaxTables2x16 = MAX_TABLES_2X16;
// The 8x8 replaces the 4x4. Def + one record per grid ROW, so two capacities:
// the number of tables, and the number of rows (MAX_TABLES_8X8 * TABLE_8X8_SITES).
constexpr int kMaxTables8x8 = MAX_TABLES_8X8;
constexpr int kTable8x8Sites = TABLE_8X8_SITES;
constexpr unsigned kCmdWriteTable8x8Def = CMD_WRITE_TABLE8X8_DEF;
constexpr unsigned kCmdReadTable8x8Def = CMD_READ_TABLE8X8_DEF;
constexpr unsigned kCmdWriteTable8x8Row = CMD_WRITE_TABLE8X8_ROW;
constexpr unsigned kCmdReadTable8x8Row = CMD_READ_TABLE8X8_ROW;
// The table flag bits are shared by the 2x16 and the 8x8, and the GUI declares
// them under the same names â€” so they need the snapshot/#undef treatment before
// anything can write ct::TABLEFLAG_ACTIVE without the macro eating the scope.
constexpr unsigned kTableFlagActive = TABLEFLAG_ACTIVE;
constexpr unsigned kTableFlagXInterp = TABLEFLAG_X_INTERP;
constexpr unsigned kTableFlagYInterp = TABLEFLAG_Y_INTERP;
// 2.3.0: the two-bit protection LEVEL in bits 6-7 of CanMessageConfig::flags
// and RelayConfig::flags. Snapshotted and #undef'd like the rest, and for a
// sharper reason than most: the two sides declare these under identical names,
// and the assignment is not arbitrary — it was chosen so the two patterns
// shipped 2.2.x flash actually contains (0x80 and 0xC0) decode to the right new
// tier without a store-version bump. If the host and the device ever disagreed
// about which number means Hidden, a Get would silently launder a concealed
// message into an ordinary one, which is the single failure this encoding
// exists to prevent. Nothing compared them until now.
constexpr unsigned kMsgProtMask = MSGPROT_MASK;
constexpr unsigned kMsgProtNone = MSGPROT_NONE;
constexpr unsigned kMsgProtReadOnly = MSGPROT_READONLY;
constexpr unsigned kMsgProtHidden = MSGPROT_HIDDEN;
constexpr unsigned kMsgProtProtected = MSGPROT_PROTECTED;
// The signal label width, now 32. It is the one field of CanSignalConfig that
// the engine never reads and the host cares about most: it is where a channel
// name lives, so the two sides disagreeing about its width shifts every
// numeric field of every signal record by the difference.
constexpr int kSignalLabelLen = SIGNAL_LABEL_LEN;
// The 9-bit message index inside msg_and_flags. This is the ceiling on
// MAX_MESSAGES and the reason 500 was chosen over 512 â€” see the check below.
constexpr unsigned kSigMsgIdxMask = SIG_MSG_IDX_MASK;
// v8 Transmit CRC8: the command pair, both capacities and the flag bits,
// snapshotted like everything above so the pins below can hold the mirrors
// equal after the #undef sweep.
constexpr unsigned kCmdWriteCrc8Cfg = CMD_WRITE_CRC8_CFG;
constexpr unsigned kCmdReadCrc8Cfg = CMD_READ_CRC8_CFG;
constexpr int kMaxCrc8Messages = MAX_CRC8_MESSAGES;
constexpr int kCrc8MaxElements = CRC8_MAX_ELEMENTS;
constexpr unsigned kCrc8FlagActive = CRC8FLAG_ACTIVE;
constexpr unsigned kCrc8FlagRefIn = CRC8FLAG_REF_IN;
constexpr unsigned kCrc8FlagRefOut = CRC8FLAG_REF_OUT;
// Every id protocol.h defines, requests and responses alike, held pairwise
// DISTINCT at compile time. A duplicate id is not an error any compiler
// reports — the dispatch switch just tests one claimant first, and the other
// command's feature breaks wholesale. CRC8 config landing on 0x38/0x39 took
// the firmware updater down with it, and only the updater's own runtime tests
// noticed. A new command goes in this list.
constexpr unsigned kAllCommandIds[] = {
    CMD_GET_STATUS, CMD_WRITE_MSG_CFG, CMD_READ_MSG_CFG, CMD_WRITE_SIG_CFG,
    CMD_READ_SIG_CFG, CMD_WRITE_MATH_CFG, CMD_READ_MATH_CFG, CMD_WRITE_COND_CFG,
    CMD_READ_COND_CFG, CMD_SAVE_TO_FLASH, CMD_CLEAR_CONFIG, CMD_CONTROL_CAN,
    CMD_INJECT_CAN_FRAME, CMD_STREAM_VALUES, CMD_WRITE_COUNTER_CFG,
    CMD_READ_COUNTER_CFG, CMD_WRITE_TIMER_CFG, CMD_READ_TIMER_CFG,
    CMD_WRITE_CONST_CFG, CMD_READ_CONST_CFG, CMD_WRITE_CONFIG_NAME,
    CMD_READ_CONFIG_NAME, CMD_RESET_DEVICE, CMD_WRITE_RELAY_CFG,
    CMD_READ_RELAY_CFG, CMD_WRITE_TABLE2X16_DEF, CMD_READ_TABLE2X16_DEF,
    CMD_WRITE_TABLE2X16_OUT, CMD_READ_TABLE2X16_OUT, CMD_WRITE_INTEG_CFG,
    CMD_READ_INTEG_CFG, CMD_GET_DEVICE_ID, CMD_WRITE_CONFIG_BINDING,
    CMD_READ_ACCESS_KEYS, CMD_WRITE_ACCESS_KEYS, CMD_ACCESS_CHALLENGE,
    CMD_ACCESS_RESPONSE, CMD_READ_FLEET_ID, CMD_READ_CAN_SETUP,
    CMD_FLEET_ID_PROVE, CMD_WRITE_DEVICE_CHANNELS, CMD_READ_DEVICE_CHANNELS,
    CMD_WRITE_TABLE8X8_DEF, CMD_READ_TABLE8X8_DEF, CMD_WRITE_TABLE8X8_ROW,
    CMD_READ_TABLE8X8_ROW, CMD_WRITE_CRC8_CFG, CMD_READ_CRC8_CFG,
    CMD_FW_UPDATE_BEGIN, CMD_FW_UPDATE_DATA, CMD_FW_UPDATE_END,
    CMD_FW_UPDATE_STATUS, CMD_FW_UPDATE_ABORT, CMD_WRITE_SCRIPT,
    CMD_READ_SCRIPT, CMD_SCRIPT_STATUS, CMD_ACK, CMD_NACK, CMD_MONITOR_STREAM,
    CMD_VALUE_STREAM, CMD_LOG,
    // The RETIRED ids, in the set for the same reason the live ones are: each
    // is a number some shipped host still speaks, and it must keep answering
    // ERR_INVALID_CMD — 0x40 most of all, which 2.2.x Managers send before
    // every Send and read that answer as "keyless". The firmware defines no
    // macro for a retired id (it must fall through to the default NACK), so
    // they are literals here. 0x0B LOAD_FROM_FLASH; 0x1B/0x1C the 2x8 table;
    // 0x1D/0x1E the 4x4 table; 0x25..0x28 the v18 config lock; 0x40
    // MSG_ACCESS_RESPONSE.
    0x0B, 0x1B, 0x1C, 0x1D, 0x1E, 0x25, 0x26, 0x27, 0x28, 0x40,
};
constexpr bool commandIdsDistinct()
{
    constexpr size_t n = sizeof(kAllCommandIds) / sizeof(kAllCommandIds[0]);
    for (size_t i = 0; i < n; ++i)
        for (size_t j = i + 1; j < n; ++j)
            if (kAllCommandIds[i] == kAllCommandIds[j])
                return false;
    return true;
}
static_assert(commandIdsDistinct(),
              "two protocol commands share an id -- the dispatch switch would eat one of them");
} // namespace fw

#undef CMD_GET_STATUS
#undef CMD_WRITE_MSG_CFG
#undef CMD_READ_MSG_CFG
#undef CMD_WRITE_SIG_CFG
#undef CMD_READ_SIG_CFG
#undef CMD_WRITE_MATH_CFG
#undef CMD_READ_MATH_CFG
#undef CMD_WRITE_COND_CFG
#undef CMD_READ_COND_CFG
#undef CMD_SAVE_TO_FLASH
#undef CMD_CLEAR_CONFIG
#undef CMD_CONTROL_CAN
#undef CMD_INJECT_CAN_FRAME
#undef CMD_STREAM_VALUES
#undef CMD_WRITE_COUNTER_CFG
#undef CMD_READ_COUNTER_CFG
#undef CMD_WRITE_TIMER_CFG
#undef CMD_READ_TIMER_CFG
#undef CMD_WRITE_CONST_CFG
#undef CMD_READ_CONST_CFG
#undef CMD_WRITE_RELAY_CFG
#undef CMD_READ_RELAY_CFG
#undef CMD_WRITE_TABLE2X16_DEF
#undef CMD_READ_TABLE2X16_DEF
#undef CMD_WRITE_TABLE2X16_OUT
#undef CMD_READ_TABLE2X16_OUT
// The 8x8 pair that replaced the 4x4. (0x1D/0x1E, the 4x4's own ids, are gone
// from both headers â€” retired, not reused â€” so there is nothing left to undef
// for them; the retirement itself is checked against the live device below.)
#undef CMD_WRITE_TABLE8X8_DEF
#undef CMD_READ_TABLE8X8_DEF
#undef CMD_WRITE_TABLE8X8_ROW
#undef CMD_READ_TABLE8X8_ROW
#undef CMD_WRITE_INTEG_CFG
#undef CMD_READ_INTEG_CFG
#undef CMD_READ_ACCESS_KEYS
#undef CMD_WRITE_ACCESS_KEYS
#undef CMD_ACCESS_CHALLENGE
#undef CMD_ACCESS_RESPONSE
// 0x40, v20's CMD_MSG_ACCESS_RESPONSE, is gone from protocol.h - retired with
// the per-message key it proved, not reused - so there is nothing to undef for
// it; the retirement itself is checked against the live device below. Same
// convention as the 4x4 pair above. The host still NAMES the opcode
// (ct::CMD_MSG_ACCESS_RESPONSE in wire_structs.h) so the retirement is
// spellable, and an #undef here read as if a firmware macro were still
// shadowing it.
#undef CMD_READ_FLEET_ID
#undef CMD_FLEET_ID_PROVE
#undef CMD_READ_CAN_SETUP
#undef ACCESS_KEY_LEN
#undef ACCESS_CHALLENGE_LEN
#undef ACCESS_FN_SEND
#undef ACCESS_FN_GET
#undef ACCESS_FN_EDIT_COMMS
#undef ACCESS_FN_COUNT
#undef ACCESS_MASK_SEND
#undef ACCESS_MASK_GET
#undef ACCESS_MASK_EDIT_COMMS
#undef FLEET_VENDOR_ID_LEN
#undef FLEET_MODEL_ID_LEN
#undef FLEET_KEY_LEN
#undef CMD_GET_DEVICE_ID
#undef CMD_WRITE_CONFIG_BINDING
#undef CONFIG_UID_LEN
#undef CONFIG_STATUS_OK
#undef CONFIG_STATUS_NONE
#undef CONFIG_STATUS_WRONG_DEVICE
#undef TABLE_2X16_SITES
#undef TABLE_8X8_SITES
#undef COND_MAX_TERMS
// Condition modes: firmware macros, GUI ct:: constexprs of the same name.
#undef CONDFLAG_ACTIVE
#undef CONDFLAG_SETRESET
#undef COND_LATCH_MAX_HZ
#undef COND_OP_IS_MESSAGE
// Shared by both table shapes, and declared under these names on both sides.
#undef TABLEFLAG_ACTIVE
#undef TABLEFLAG_X_INTERP
#undef TABLEFLAG_Y_INTERP
// The signal label is 32 bytes now and BOTH headers name the width. Without
// this undef, ct::SIGNAL_LABEL_LEN expands to ct::32 and does not compile â€”
// which is the pleasant failure mode; the unpleasant one is a constant that
// silently means the firmware's value everywhere the GUI's was intended.
#undef SIGNAL_LABEL_LEN
// v15 packed-signal masks: the firmware defines these as macros and the GUI as
// ct:: constexprs of the same NAME, so they must be undef'd like the rest.
#undef SIG_MSG_IDX_BITS
#undef SIG_MSG_IDX_MASK
#undef SIG_START_BIT_MASK
#undef SIG_BITLEN_MASK
#undef SIG_VALTYPE_MASK
#undef SIG_DECIMALS_MASK
#undef SIG_MUXOFF_MASK
#undef CMD_WRITE_CONFIG_NAME
#undef CMD_READ_CONFIG_NAME
#undef CMD_RESET_DEVICE
#undef CMD_ACK
#undef CMD_NACK
#undef CMD_MONITOR_STREAM
#undef CMD_VALUE_STREAM
#undef CMD_LOG
#undef ERR_OK
#undef ERR_INVALID_CMD
#undef ERR_INVALID_LEN
#undef ERR_INVALID_CRC
#undef ERR_OUT_OF_BOUNDS
#undef ERR_FLASH_WRITE
#undef ERR_BUS_BUSY
#undef ERR_LOCKED
#undef MAX_MESSAGES
#undef MAX_SIGNALS
#undef MAX_MATH_COMPUTATIONS
#undef MAX_CONDITIONS
#undef MAX_COUNTERS
#undef MAX_TIMERS
#undef MAX_CONSTANTS
#undef MAX_RELAYS
#undef MAX_TABLES_2X16
#undef MAX_TABLES_8X8
// The GUI derives the ROW table's capacity as a named constant; the firmware
// may or may not, since its flash table list can write the product inline.
// Undefining a macro that was never defined is a no-op, and getting this wrong
// in the other direction costs an unreadable expansion error.
#undef MAX_TABLE_8X8_ROWS
#undef MAX_INTEGRATORS
#undef INTEGRATOR_MAX_HZ
#undef CONFIG_NAME_LEN
#undef MSGFLAG_EXTENDED
#undef MSGFLAG_FD
#undef MSGFLAG_ROUTING
#undef MSGFLAG_ACTIVE
#undef MSGFLAG_TRANSMIT
#undef MSGFLAG_TX_SEQUENTIAL
// Triggered transmit: firmware macros, GUI ct:: constexprs of the same name.
#undef TXTRIG_ENABLED
#undef TX_TRIGGER_COND_NONE
// The monitor flags need the same sweep for the same reason: the firmware
// defines them as macros and wire_structs.h as ct:: constexprs of identical
// name, so `ct::MONFLAG_GAP` expands to `ct::0x10` without this.
#undef MONFLAG_EXTENDED
#undef MONFLAG_FD
#undef MONFLAG_BRS
#undef MONFLAG_ESI
#undef MONFLAG_GAP
#undef MONITOR_HEADER_BYTES
// MSGFLAG_PROTECTED / MSGFLAG_READONLY are gone from BOTH headers in 2.3.0 —
// deleted as names, not renamed, because 0x40 alone now means the WEAKEST tier
// and code testing `flags & MSGFLAG_PROTECTED` would read Protected as true for
// a Read Only message. Their replacement is the MSGPROT_* level, which the
// firmware defines as macros and wire_structs.h as ct:: constexprs of the same
// name, so it needs the sweep the old pair did.
#undef MSGPROT_MASK
#undef MSGPROT_NONE
#undef MSGPROT_READONLY
#undef MSGPROT_HIDDEN
#undef MSGPROT_PROTECTED
#undef RELAYFLAG_EXTENDED
#undef RELAYFLAG_INVERT
#undef RELAYFLAG_ACTIVE
#undef SIG_MSG_NONE
#undef COUNTERFLAG_ROLL
#undef COUNTERFLAG_PRESERVE
#undef COUNTERFLAG_ACTIVE
#undef COUNTERFLAG_RATE_DOWN
#undef COUNTER_MAX_HZ
// v15: the counter input-source macros collide with ct::CounterSrc's enumerators
// by name, which is exactly what this sweep is for.
#undef COUNTER_SRC_SIGNAL
#undef COUNTER_SRC_MSG_RX
#undef COUNTER_SRC_MSG_TX
#undef COUNTER_SRC_SHIFT_UP
#undef COUNTER_SRC_SHIFT_DOWN
#undef COUNTER_SRC_SHIFT_RESET
#undef COUNTER_SRC_SHIFT_ENABLE
#undef COUNTER_SRC_AT
#undef CMD_WRITE_DEVICE_CHANNELS
#undef CMD_READ_DEVICE_CHANNELS
// Firmware update. These MUST be swept like the rest: wire_structs.h declares
// ct::CMD_FW_UPDATE_* as constexpr under the same names, and a surviving macro
// both mangles that declaration and rewrites every `ct::CMD_FW_UPDATE_BEGIN`
// in this file into `ct::0x38`.
#undef CMD_FW_UPDATE_BEGIN
#undef CMD_FW_UPDATE_DATA
#undef CMD_FW_UPDATE_END
#undef CMD_FW_UPDATE_STATUS
#undef CMD_FW_UPDATE_ABORT
#undef ERR_FW_REJECTED
#undef COUNTER_MODE_UPDOWN
#undef COUNTER_MODE_FOLLOW
#undef TIMERFLAG_COUNTDOWN
#undef TIMERFLAG_ROLLOVER
#undef TIMERFLAG_SET_ON_START
#undef TIMERFLAG_SET_ON_STOP
#undef TIMERFLAG_ACTIVE
#undef INTEGFLAG_ACTIVE
#undef INTEGFLAG_CONST_INPUT
#undef INTEGFLAG_COUNT_DOWN
#undef INTEGFLAG_PRESERVE
#undef CMD_WRITE_CRC8_CFG
#undef CMD_READ_CRC8_CFG
#undef CRC8FLAG_ACTIVE
#undef CRC8FLAG_REF_IN
#undef CRC8FLAG_REF_OUT
#undef CRC8_MAX_ELEMENTS
#undef MAX_CRC8_MESSAGES
#undef STREAM_ENABLE_VALUES
#undef STREAM_ENABLE_MONITOR
#undef PROTO_START_MARKER
#undef PROTOCOL_VERSION

static int failures = 0;

#define CHECK(cond)                                                                              \
    do {                                                                                         \
        if (!(cond)) {                                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                          \
            ++failures;                                                                          \
        }                                                                                        \
    } while (0)

// ---------------------------------------------------------------- fixtures

// Everything the "device" emits lands here.
static QByteArray g_wire;
// Accepts everything, so the protocol tests see every byte the device framed.
// g_wireRefuse lets testMonitorGapMarking below turn this into a transport that
// is full, which is the only way the gap path can be reached.
static bool g_wireRefuse = false;
static bool captureBytes(const uint8_t *data, uint16_t length)
{
    if (g_wireRefuse)
        return false;
    g_wire.append(reinterpret_cast<const char *>(data), length);
    return true;
}

struct CapturedTx {
    uint8_t bus;
    uint32_t id;
    uint8_t ext, fd, len;
    uint8_t data[64];
};
static QList<CapturedTx> g_txFrames;
// Accepts everything: these tests measure what the engine COMPOSES, so the
// queue must never be the thing under test here. testTxFairness below installs
// its own callback that refuses, which is where the drop path is exercised.
static bool captureTransmit(uint8_t dest_bus, uint32_t can_id, uint8_t is_extended,
                            uint8_t is_fd, const uint8_t *data, uint8_t len)
{
    CapturedTx t{};
    t.bus = dest_bus;
    t.id = can_id;
    t.ext = is_extended;
    t.fd = is_fd;
    t.len = len;
    if (data && len)
        std::memcpy(t.data, data, len);
    g_txFrames.append(t);
    return true;
}

// A transmit callback with a per-tick budget, standing in for a full outgoing
// queue. Everything past the budget is refused, exactly as the ring buffer does.
static int g_txBudget = 0;
static QList<quint32> g_txAccepted;
static bool budgetedTransmit(uint8_t dest_bus, uint32_t can_id, uint8_t is_extended,
                             uint8_t is_fd, const uint8_t *data, uint8_t len)
{
    Q_UNUSED(dest_bus);
    Q_UNUSED(is_extended);
    Q_UNUSED(is_fd);
    Q_UNUSED(data);
    Q_UNUSED(len);
    if (g_txBudget <= 0)
        return false;
    --g_txBudget;
    g_txAccepted.append(can_id);
    return true;
}

// RAM-backed "flash" that mirrors the STM32 constraints the real driver obeys:
// doubleword-aligned programming, and each doubleword accepts new bytes only
// while it still reads erased (0xFF). The one exception is a doubleword
// re-presented with EXACTLY the bytes it already holds â€” that succeeds without
// touching anything, because it is the no-op the store's retransmit handling
// legalises (a WRITE whose ACK was lost is re-sent byte-identical, and
// flash_store skips the program after comparing). DIFFERENT bytes over a
// programmed doubleword fail, exactly as PROGERR does on hardware. The old
// model here tracked a per-doubleword "programmed" flag, which was strictly
// write-once but blind to content; comparing content instead models the
// contract the board soak proved matters â€” what flash refuses is CHANGING
// bits it has already spent this erase's program on, and a memcpy model (or a
// flag model consulted after the store's own skip) would hide exactly the
// double-program that killed every 250th chunk on the wire.
static uint8_t g_flash[FLASH_STORE_CAPACITY];
static bool flashErase(void)
{
    std::memset(g_flash, 0xFF, sizeof(g_flash));
    return true;
}
static bool flashProgram(uint32_t offset, const uint8_t *data, uint32_t length)
{
    if (offset + length > sizeof(g_flash) || (offset & 7) || (length & 7))
        return false;
    for (uint32_t i = 0; i < length; i += 8) {
        const uint8_t *cur = g_flash + offset + i;
        bool blank = true;
        for (int b = 0; b < 8; ++b)
            blank = blank && cur[b] == 0xFF;
        if (!blank && std::memcmp(cur, data + i, 8) != 0)
            return false; // programmed with other bytes since the erase: PROGERR
    }
    std::memcpy(g_flash + offset, data, length);
    return true;
}
static const uint8_t *flashData(void)
{
    return g_flash;
}

static uint32_t fakeUptime(void)
{
    return 123456;
}
static ControlCanPayload g_lastControl{};
// Stands in for user_code.c's g_bus_setup: the glue owns the live bus state
// because it owns the peripherals, and CMD_READ_CAN_SETUP only serialises it.
static ControlCanPayload g_busSetup[3]{};
static bool fakeControlCan(const ControlCanPayload *p)
{
    if (p) {
        g_lastControl = *p;
        if (p->bus_idx >= 1 && p->bus_idx <= 3)
            g_busSetup[p->bus_idx - 1] = *p;
    }
    return true;
}
static void fakeReadBusSetup(ControlCanPayload out[3])
{
    std::memcpy(out, g_busSetup, sizeof(g_busSetup));
}
static bool g_resetRequested = false;
static void fakeRequestReset(void)
{
    g_resetRequested = true;
}

// Stand-in for the device's TRNG. Deliberately NOT cryptographic â€” on hardware
// this is the STM32G4's true RNG (firmware/src/rng.c). All the protocol needs
// of it, and all the replay checks below turn on, is that no two calls return
// the same bytes.
static uint32_t g_fakeRandomState = 0x12345678u;
static bool fakeRandomBytes(uint8_t *dst, uint16_t length)
{
    for (uint16_t i = 0; i < length; ++i) {
        g_fakeRandomState = g_fakeRandomState * 1664525u + 1013904223u;
        dst[i] = uint8_t(g_fakeRandomState >> 24);
    }
    return true;
}

// Send one GUI-built frame into the firmware; return the framed responses.
static QList<ct::Packet> exchange(quint8 cmd, const QByteArray &payload)
{
    static ct::FrameSplitter splitter;
    g_wire.clear();
    const QByteArray frame = ct::buildFrame(cmd, payload);
    serial_proto_feed(reinterpret_cast<const uint8_t *>(frame.constData()),
                      uint16_t(frame.size()));
    return splitter.feed(g_wire);
}

// ACK payload is now [ERR_OK, req_crc_hi, req_crc_lo]: the status byte plus the
// CRC16 of the request being answered, echoed so the host can discard a stale
// duplicate. Assert both — the echo matching the request's own CRC is the whole
// point of the change, and a firmware that echoed the wrong value would defeat
// the host's de-duplication silently.
static bool expectAck(quint8 cmd, const QByteArray &payload)
{
    const auto packets = exchange(cmd, payload);
    if (!(packets.size() == 1 && packets[0].cmd == ct::CMD_ACK))
        return false;
    const QByteArray &ap = packets[0].payload;
    if (ap.size() != 3 || ap[0] != char(0))
        return false;
    const quint16 echo = quint16((quint8(ap[1]) << 8) | quint8(ap[2]));
    return echo == ct::frameCrc(cmd, payload);
}

static bool expectNack(quint8 cmd, const QByteArray &payload, quint8 error)
{
    const auto packets = exchange(cmd, payload);
    if (!(packets.size() == 1 && packets[0].cmd == ct::CMD_NACK))
        return false;
    const QByteArray &np = packets[0].payload;
    // A NACK for a frame that authenticated (which exchange() always builds)
    // echoes the request CRC just as an ACK does; the two "bad frame" codes,
    // which carry 0, are exercised by the framing tests rather than here.
    if (np.size() != 3 || quint8(np[0]) != error)
        return false;
    const quint16 echo = quint16((quint8(np[1]) << 8) | quint8(np[2]));
    return echo == ct::frameCrc(cmd, payload);
}

template <typename T>
static QByteArray writeChunk(quint16 start, const QVector<T> &items, int from, int count)
{
    QByteArray b(4, 0);
    b[0] = char(start & 0xFF);
    b[1] = char(start >> 8);
    b[2] = char(count & 0xFF);
    b[3] = char(count >> 8);
    const int old = b.size();
    b.resize(old + count * int(sizeof(T)));
    std::memcpy(b.data() + old, items.constData() + from, size_t(count) * sizeof(T));
    return b;
}

template <typename T>
static bool sendTable(quint8 cmd, const QVector<T> &items, int chunk)
{
    for (int i = 0; i < items.size(); i += chunk) {
        const int count = qMin(chunk, int(items.size()) - i);
        if (!expectAck(cmd, writeChunk(quint16(i), items, i, count)))
            return false;
    }
    return true;
}

static QByteArray readRange(quint8 cmd, quint16 start, quint16 count)
{
    QByteArray req(4, 0);
    req[0] = char(start & 0xFF);
    req[1] = char(start >> 8);
    req[2] = char(count & 0xFF);
    req[3] = char(count >> 8);
    const auto packets = exchange(cmd, req);
    if (packets.size() != 1 || packets[0].cmd != cmd)
        return {};
    return packets[0].payload;
}

// Read a whole signal table as RECORD BYTES, chunked the way Get Configuration
// chunks it.
//
// One request cannot carry the answer any more. A signal record is 64 bytes and
// MAX_RESPONSE_PAYLOAD caps a reply at 1988, so 31 records is the most the
// device can return at once — which is exactly why READ_CHUNK_SIGNALS is 31 and
// why config_transfer has always looped. A single-shot read was fine while a
// test document mapped two or four signals; it stopped being fine when every
// Send began carrying the 31 device channels as well. The product path was
// never wrong here, only this shortcut.
static QByteArray readAllSignals(int count)
{
    QByteArray out;
    for (int i = 0; i < count; i += ct::READ_CHUNK_SIGNALS) {
        const int n = std::min(ct::READ_CHUNK_SIGNALS, count - i);
        const QByteArray part =
            readRange(ct::CMD_READ_SIG_CFG, quint16(i), quint16(n));
        if (part.size() != 4 + n * int(sizeof(ct::CanSignalConfig)))
            return {}; // caller's size check reports it
        out += part.mid(4); // drop the echoed start/count header
    }
    return out;
}

// ---------------------------------------------------- 8x8 lookup fixture
//
// The engine tests for the 8x8 build their records BY HAND instead of going
// through the document and the mapper, which is a deliberate break from how the
// 2x16 block below works. Two reasons. The expected values are hand-computed
// from literal sites and literal cells, and being able to read the arithmetic
// next to the answer is most of what those tests are worth. And the thing under
// test IS the grid indexing â€” a fixture that asked the mapper to lay the grid
// out would be checking that layout against itself.
//
// Slot 0 carries the X axis, slot 1 the Y axis, slots 2 and 3 are table
// outputs. The axes arrive as two 16-bit Intel fields of one receive message on
// CAN 1, so the lookup runs where it runs in service: inside
// engine_process_can, immediately after the constants pass.
namespace t8 {
constexpr quint16 kX = 0, kY = 1, kOut0 = 2, kOut1 = 3;
constexpr quint32 kCanId = 0x320;

// One receive message and its two axis signals. Both are unsigned 16-bit with
// factor 1 and offset 0, so the number fed in below IS the physical value on
// the axis and every site can be written as an integer.
static bool installAxes()
{
    ct::CanMessageConfig msg{};
    msg.can_id = kCanId;
    msg.flags = ct::MSGFLAG_ACTIVE;
    msg.src_bus = 1;
    msg.dlc = 8;

    ct::CanSignalConfig sig[2]{};
    for (int i = 0; i < 2; ++i) {
        sig[i].factor = 1.0f;
        sig[i].min_val = -1.0e9f; // a real span: min == max would clamp to a point
        sig[i].max_val = 1.0e9f;
        ct::sigSetHeader(sig[i], 0, 0, 1); // message 0, Intel byte order, active
        ct::sigSetBits(sig[i], quint16(i * 16), 16, ct::SIGNAL_TYPE_UINT16, 0, 0);
    }
    std::memcpy(sig[0].label, "X", 2);
    std::memcpy(sig[1].label, "Y", 2);

    return engine_table_write(ENGINE_TABLE_MESSAGES, 0, 1,
                              reinterpret_cast<const uint8_t *>(&msg))
           && engine_table_write(ENGINE_TABLE_SIGNALS, 0, 2,
                                 reinterpret_cast<const uint8_t *>(sig));
}

static void feed(int x, int y)
{
    const uint8_t f[8] = {uint8_t(x & 0xFF), uint8_t((x >> 8) & 0xFF),
                          uint8_t(y & 0xFF), uint8_t((y >> 8) & 0xFF), 0, 0, 0, 0};
    engine_process_can(1, kCanId, 0, 0, f, 8);
}

// Sites ascend 10, 20, ... 10*count on both axes. Only the first x_count /
// y_count entries are written; the rest stay zero, which is what a partially
// filled table looks like on the wire and is exactly the case the count fields
// exist to bound.
static ct::Table8x8Def makeDef(quint16 dest, quint8 flags, int xCount, int yCount)
{
    ct::Table8x8Def d{};
    d.x_signal_idx = kX;
    d.y_signal_idx = kY;
    d.dest_signal_idx = dest;
    d.flags = flags;
    d.x_count = quint8(xCount);
    d.y_count = quint8(yCount);
    for (int k = 0; k < xCount; ++k)
        d.x_sites[k] = 10.0f * float(k + 1);
    for (int k = 0; k < yCount; ++k)
        d.y_sites[k] = 10.0f * float(k + 1);
    return d;
}

// Eight row records holding cell(x, y) = base + x + 10*y. Every cell is
// distinct, so any mis-indexed read lands on a number that identifies exactly
// which cell was read instead. `base` separates one table's grid from another's
// by a thousand, which is how the tests below tell "table 1 read table 1's
// rows" from "table 1 read table 0's".
static void makeRows(ct::Table8x8GridRow rows[ct::TABLE_8X8_SITES], float base)
{
    for (int y = 0; y < ct::TABLE_8X8_SITES; ++y)
        for (int x = 0; x < ct::TABLE_8X8_SITES; ++x)
            rows[y].v[x] = base + float(x) + 10.0f * float(y);
}
} // namespace t8

// ------------------------------------------------------------------ tests

// The firmware's SHA-256 against published vectors, and against Qt's
// implementation on the PC side. Both halves of the password protocol hash the
// same bytes and compare the results, so a one-bit disagreement here would mean
// no password ever verified â€” or, far worse, that the wrong one did.
static void testFirmwareSha256()
{
    const auto hex = [](const uint8_t *d, int n) {
        return QByteArray(reinterpret_cast<const char *>(d), n).toHex();
    };
    uint8_t out[SHA256_DIGEST_LEN];

    // FIPS 180-4 / NIST: "abc"
    sha256(reinterpret_cast<const uint8_t *>("abc"), 3, out);
    CHECK(hex(out, 32)
          == QByteArrayLiteral("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));

    // The empty message â€” the padding-only path.
    sha256(reinterpret_cast<const uint8_t *>(""), 0, out);
    CHECK(hex(out, 32)
          == QByteArrayLiteral("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));

    // 448 bits: exactly the length that forces the padding into a SECOND block,
    // which is the case hand-written implementations get wrong.
    {
        const char *m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
        sha256(reinterpret_cast<const uint8_t *>(m), 56, out);
        CHECK(hex(out, 32)
              == QByteArrayLiteral(
                     "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    }

    // A million 'a' â€” exercises the streaming path over many blocks.
    {
        Sha256Ctx ctx;
        sha256_init(&ctx);
        const QByteArray chunk(1000, 'a');
        for (int i = 0; i < 1000; ++i)
            sha256_update(&ctx, reinterpret_cast<const uint8_t *>(chunk.constData()), 1000);
        sha256_final(&ctx, out);
        CHECK(hex(out, 32)
              == QByteArrayLiteral(
                     "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
    }

    // Chunked updates must equal a single-shot hash at every split, including
    // block boundaries.
    {
        QByteArray data;
        for (int i = 0; i < 200; ++i)
            data.append(char(i));
        uint8_t once[32];
        sha256(reinterpret_cast<const uint8_t *>(data.constData()), data.size(), once);
        for (int split : {1, 55, 56, 63, 64, 65, 127, 128, 199}) {
            Sha256Ctx ctx;
            sha256_init(&ctx);
            sha256_update(&ctx, reinterpret_cast<const uint8_t *>(data.constData()), split);
            sha256_update(&ctx, reinterpret_cast<const uint8_t *>(data.constData()) + split,
                          data.size() - split);
            sha256_final(&ctx, out);
            CHECK(std::memcmp(out, once, 32) == 0);
        }
    }

    // HMAC-SHA256, RFC 4231 test case 1.
    {
        const QByteArray key(20, char(0x0b));
        hmac_sha256(reinterpret_cast<const uint8_t *>(key.constData()), key.size(),
                    reinterpret_cast<const uint8_t *>("Hi There"), 8, out);
        CHECK(hex(out, 32)
              == QByteArrayLiteral(
                     "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"));
    }
    // RFC 4231 test case 2 â€” a key shorter than the block.
    {
        hmac_sha256(reinterpret_cast<const uint8_t *>("Jefe"), 4,
                    reinterpret_cast<const uint8_t *>("what do ya want for nothing?"), 28, out);
        CHECK(hex(out, 32)
              == QByteArrayLiteral(
                     "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"));
    }
    // RFC 4231 test case 6 â€” a key LONGER than the block, so it is hashed down
    // first. That branch is only reachable with an over-long key.
    {
        const QByteArray key(131, char(0xaa));
        const char *msg = "Test Using Larger Than Block-Size Key - Hash Key First";
        hmac_sha256(reinterpret_cast<const uint8_t *>(key.constData()), key.size(),
                    reinterpret_cast<const uint8_t *>(msg), 54, out);
        CHECK(hex(out, 32)
              == QByteArrayLiteral(
                     "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"));
    }

    // The device and the PC must agree: the firmware's HMAC has to match Qt's,
    // because the access challenge-response compares one against the other.
    // The key is only FOUR bytes here, and the byte order they go in is part of
    // the agreement â€” accessKeyBytes() writes them big-endian precisely so this
    // comparison, and the device's own, hash the same string.
    {
        const ct::AccessKey key = 0x1A2B3C4Du;
        const QByteArray keyBytes = ct::accessKeyBytes(key);
        const QByteArray challenge(ct::ACCESS_CHALLENGE_LEN, char(0xa5));
        CHECK(keyBytes.size() == ct::ACCESS_KEY_LEN);
        hmac_sha256(reinterpret_cast<const uint8_t *>(keyBytes.constData()), keyBytes.size(),
                    reinterpret_cast<const uint8_t *>(challenge.constData()), challenge.size(),
                    out);
        const QByteArray fromPc = ct::accessResponse(key, challenge);
        CHECK(fromPc.size() == 32);
        CHECK(std::memcmp(out, fromPc.constData(), 32) == 0);
    }

    // Constant-time compare still compares.
    {
        const uint8_t a[4] = {1, 2, 3, 4};
        const uint8_t b[4] = {1, 2, 3, 4};
        const uint8_t c[4] = {1, 2, 3, 5};
        CHECK(sha256_equal_ct(a, b, 4));
        CHECK(!sha256_equal_ct(a, c, 4));
        CHECK(sha256_equal_ct(a, c, 3)); // only the compared span counts
    }
}

// The device half of the access passwords, driven through the real serial
// protocol. This is the bypass the whole feature exists to close: a file-side
// lock is worth nothing if Get Configuration reads the configuration straight
// back off the hardware.
//
// Runs against the live firmware modules, so it also proves the two halves of
// the protocol agree â€” the PC folds a typed password into four bytes, the
// device stores them, and an HMAC over the device's own nonce is what carries
// the proof between them.
//
// The INDEPENDENCE of the three is checked here rather than in a unit test
// because it is a property of the device's state machine and nowhere else: no
// amount of host-side testing could tell you that proving Send left Get shut.
static void testDeviceAccess(const SerialProtoCallbacks *restore)
{
    // The wire format is only one format if both headers agree about it. This
    // is the only place both are visible, so it is the only place that can
    // prove it â€” a key width or a function number that had drifted would mean a
    // device that silently never unlocks, or worse, one that opens the wrong
    // half of itself.
    static_assert(sizeof(ct::AccessKeyRecord) == sizeof(::AccessKeyRecord),
                  "GUI and firmware access key records differ in size");
    static_assert(sizeof(ct::AccessKeyRecord) == 25,
                  "AccessKeyRecord must be 25 bytes (v17: four Protected Comms slots)");
    static_assert(sizeof(ct::AccessKeyWritePayload) == sizeof(::AccessKeyWritePayload),
                  "GUI and firmware access key write payloads differ in size");
    static_assert(sizeof(ct::AccessKeyWritePayload) == 7,
                  "AccessKeyWritePayload must be 7 bytes (v17: the slot)");

    // Field by field, because sizeof alone would sail past a reordering and put
    // one function's key under another's index. Same trick the signal and
    // integrator records use above: fill the GUI's struct, reinterpret the raw
    // bytes as the firmware's, read every field back.
    {
        ct::AccessKeyRecord g{};
        g.set_mask = ct::ACCESS_MASK_SEND | ct::ACCESS_MASK_EDIT_COMMS;
        for (int fn = 0; fn < ct::ACCESS_FN_COUNT; ++fn)
            for (int b = 0; b < ct::ACCESS_KEY_LEN; ++b)
                g.keys[fn][b] = uint8_t(0x10 * (fn + 1) + b);
        ::AccessKeyRecord f;
        std::memcpy(&f, &g, sizeof(f));
        CHECK(f.set_mask == (fw::kAccessMaskSend | fw::kAccessMaskEditComms));
        bool keysMatch = true;
        for (unsigned fn = 0; fn < fw::kAccessFnCount; ++fn)
            for (int b = 0; b < fw::kAccessKeyLen; ++b)
                keysMatch = keysMatch && f.keys[fn][b] == uint8_t(0x10 * (fn + 1) + b);
        CHECK(keysMatch);

        ct::AccessKeyWritePayload gw{};
        gw.function = ct::ACCESS_FN_GET;
        gw.clear = 1;
        for (int b = 0; b < ct::ACCESS_KEY_LEN; ++b)
            gw.key[b] = uint8_t(0xA0 + b);
        ::AccessKeyWritePayload fwWrite;
        std::memcpy(&fwWrite, &gw, sizeof(fwWrite));
        CHECK(fwWrite.function == fw::kAccessFnGet);
        CHECK(fwWrite.clear == 1);
        CHECK(fwWrite.key[0] == 0xA0 && fwWrite.key[3] == 0xA3);
    }

    CHECK(ct::ACCESS_KEY_LEN == fw::kAccessKeyLen);
    CHECK(ct::ACCESS_CHALLENGE_LEN == fw::kAccessChallengeLen);
    CHECK(unsigned(ct::ACCESS_FN_COUNT) == fw::kAccessFnCount);
    CHECK(ct::ACCESS_FN_SEND == fw::kAccessFnSend);
    CHECK(ct::ACCESS_FN_GET == fw::kAccessFnGet);
    CHECK(ct::ACCESS_FN_EDIT_COMMS == fw::kAccessFnEditComms);
    CHECK(ct::ACCESS_MASK_SEND == fw::kAccessMaskSend);
    CHECK(ct::ACCESS_MASK_GET == fw::kAccessMaskGet);
    CHECK(ct::ACCESS_MASK_EDIT_COMMS == fw::kAccessMaskEditComms);
    CHECK(ct::CMD_READ_ACCESS_KEYS == fw::kCmdReadAccessKeys);
    CHECK(ct::CMD_WRITE_ACCESS_KEYS == fw::kCmdWriteAccessKeys);
    CHECK(ct::CMD_ACCESS_CHALLENGE == fw::kCmdAccessChallenge);
    CHECK(ct::CMD_ACCESS_RESPONSE == fw::kCmdAccessResponse);
    CHECK(ct::ERR_LOCKED == fw::kErrLocked);
    CHECK(ct::ERR_INVALID_CMD == fw::kErrInvalidCmd);
    // ...and the MODEL's view of the same numbers, since deriveAccessKey and
    // accessResponse are what produce the bytes the device compares. Those two
    // sit behind access_keys.h and never see wire_structs.h, so nothing else
    // would notice if the two definitions drifted apart.
    CHECK(ct::kAccessKeyBytes == ct::ACCESS_KEY_LEN);
    CHECK(ct::kAccessChallengeBytes == ct::ACCESS_CHALLENGE_LEN);
    CHECK(ct::kAccessFunctionCount == ct::ACCESS_FN_COUNT);
    CHECK(int(ct::AccessFunction::SendConfiguration) == ct::ACCESS_FN_SEND);
    CHECK(int(ct::AccessFunction::GetConfiguration) == ct::ACCESS_FN_GET);
    CHECK(int(ct::AccessFunction::EditProtectedComms) == ct::ACCESS_FN_EDIT_COMMS);

    // Real keys folded from real passwords. Literals would test the firmware
    // against numbers no user could ever produce, and would skip the one
    // derivation that has to match on every machine in a fleet.
    const ct::AccessKey sendKey = ct::deriveAccessKey(QStringLiteral("send-me"));
    const ct::AccessKey getKey = ct::deriveAccessKey(QStringLiteral("get-me"));
    const ct::AccessKey wrongKey = ct::deriveAccessKey(QStringLiteral("nope"));
    CHECK(sendKey != ct::kNoAccessKey && getKey != ct::kNoAccessKey);
    CHECK(sendKey != getKey && wrongKey != sendKey);

    const auto writeKeyPayload = [](quint8 function, ct::AccessKey key, bool clear) {
        ct::AccessKeyWritePayload p{};
        p.function = function;
        p.clear = clear ? 1 : 0;
        const QByteArray bytes = ct::accessKeyBytes(key);
        if (bytes.size() == ct::ACCESS_KEY_LEN)
            std::memcpy(p.key, bytes.constData(), ct::ACCESS_KEY_LEN);
        return QByteArray(reinterpret_cast<const char *>(&p), sizeof(p));
    };
    const auto getChallenge = []() {
        const auto packets = exchange(ct::CMD_ACCESS_CHALLENGE, QByteArray());
        return (packets.size() == 1 && packets[0].cmd == ct::CMD_ACCESS_CHALLENGE)
                   ? packets[0].payload
                   : QByteArray();
    };
    const auto responsePayload = [](quint8 function, ct::AccessKey key,
                                    const QByteArray &challenge) {
        return QByteArray(1, char(function)) + ct::accessResponse(key, challenge);
    };
    const auto prove = [&](quint8 function, ct::AccessKey key) {
        return expectAck(ct::CMD_ACCESS_RESPONSE,
                         responsePayload(function, key, getChallenge()));
    };
    // 0xFF is not a legal mask (only three bits exist), so a malformed answer
    // fails whatever it is compared against instead of reading as "none set".
    const auto accessMask = []() {
        const auto packets = exchange(ct::CMD_READ_ACCESS_KEYS, QByteArray());
        return (packets.size() == 1 && packets[0].cmd == ct::CMD_READ_ACCESS_KEYS
                && !packets[0].payload.isEmpty())
                   ? quint8(packets[0].payload[0])
                   : quint8(0xFF);
    };

    const auto readCmd = ct::CMD_READ_MSG_CFG;
    const auto writeCmd = ct::CMD_WRITE_MSG_CFG;
    // A well-formed 0-record read, so anything that comes back is about the
    // access keys and not about a malformed request.
    const QByteArray emptyRange(4, char(0));
    // Writes need a real record, and each one needs a slot of its own. A
    // 0-record write used to serve as the same kind of throwaway payload, but
    // the firmware now refuses one as malformed: it wrote nothing and still
    // advanced the engine's active-record prefix, so the prefix could be dragged
    // out over slots nothing ever programmed. What these lines are testing is
    // the access gate rather than the payload, so any write the device would
    // otherwise accept will do â€” except that STM32 flash programs each
    // doubleword once per erase, and the model at the top of this file enforces
    // it. Re-sending record 0 would therefore fail on the flash instead of on
    // the gate, which would prove nothing about either. Walking the slot forward
    // keeps every one of these a first program.
    uint16_t nextWriteSlot = 0;
    auto oneRecordWrite = [&] {
        QByteArray b(4, char(0));
        b[0] = char(nextWriteSlot & 0xFF);
        b[1] = char(nextWriteSlot >> 8);
        b[2] = char(1); // exactly one record
        ++nextWriteSlot;
        b.append(QByteArray(int(sizeof(ct::CanMessageConfig)), char(0)));
        return b;
    };

    // ---- an unprovisioned device is open, and says so ----
    CHECK(accessMask() == 0);
    CHECK(exchange(readCmd, emptyRange).size() == 1);
    CHECK(expectAck(writeCmd, oneRecordWrite()));

    // ---- installing the Send password ----
    // An unprotected function accepts a key freely; that is how the first
    // password ever gets applied.
    CHECK(expectAck(ct::CMD_WRITE_ACCESS_KEYS,
                    writeKeyPayload(ct::ACCESS_FN_SEND, sendKey, false)));
    {
        const auto packets = exchange(ct::CMD_READ_ACCESS_KEYS, QByteArray());
        CHECK(packets.size() == 1);
        if (packets.size() == 1) {
            // WHICH keys are set is answerable; what they ARE is not. Two bytes
            // as of v17 - the mask, then the Protected Comms slot mask - and no
            // command returns the keys themselves.
            CHECK(packets[0].payload.size() == 2);
            CHECK(quint8(packets[0].payload[0]) == ct::ACCESS_MASK_SEND);

            // The GUI's own parser against the bytes the firmware just
            // produced. Asserting the layout by hand only proves the firmware
            // is self-consistent; this proves the two sides agree.
            ct::device_session::AccessState parsed;
            CHECK(ct::device_session::parseAccessState(packets[0].payload, &parsed));
            CHECK(parsed.supported && parsed.any());
            CHECK(parsed.isSet(ct::AccessFunction::SendConfiguration));
            CHECK(!parsed.isSet(ct::AccessFunction::GetConfiguration));
            CHECK(!parsed.isSet(ct::AccessFunction::EditProtectedComms));
            CHECK(!ct::device_session::parseAccessState(QByteArray(), &parsed));
        }
    }
    // Whoever just set the password holds it, so the rest of the Send goes
    // through â€” the upload would otherwise NACK against the lock it had
    // installed two frames earlier.
    CHECK(expectAck(writeCmd, oneRecordWrite()));

    // ---- a power cycle re-locks: proving says who is on the wire NOW ----
    serial_proto_init(nullptr);
    CHECK(expectNack(writeCmd, emptyRange, ct::ERR_LOCKED));
    CHECK(expectNack(ct::CMD_CLEAR_CONFIG, QByteArray(), ct::ERR_LOCKED));
    CHECK(expectNack(ct::CMD_SAVE_TO_FLASH, QByteArray(), ct::ERR_LOCKED));
    CHECK(expectNack(ct::CMD_WRITE_CONFIG_NAME, QByteArray(ct::CONFIG_NAME_LEN, 'x'),
                     ct::ERR_LOCKED));
    // Changing or removing the password must not be a way around not knowing it.
    CHECK(expectNack(ct::CMD_WRITE_ACCESS_KEYS,
                     writeKeyPayload(ct::ACCESS_FN_SEND, getKey, false), ct::ERR_LOCKED));
    CHECK(expectNack(ct::CMD_WRITE_ACCESS_KEYS,
                     writeKeyPayload(ct::ACCESS_FN_SEND, ct::kNoAccessKey, true),
                     ct::ERR_LOCKED));
    // Only Send is protected, so everything else is untouched. A device that
    // locked reads because a WRITE password was set would be unusable and would
    // look, from the outside, exactly like a correct one.
    CHECK(exchange(readCmd, emptyRange).size() == 1);
    CHECK(exchange(ct::CMD_READ_CONFIG_NAME, QByteArray()).size() == 1);
    // Status stays answerable: a locked device must still be identifiable.
    CHECK(exchange(ct::CMD_GET_STATUS, QByteArray()).size() == 1);
    CHECK(accessMask() == ct::ACCESS_MASK_SEND);

    // ---- challenge / response ----
    // A response with no challenge outstanding is refused.
    CHECK(expectNack(ct::CMD_ACCESS_RESPONSE,
                     QByteArray(1, char(ct::ACCESS_FN_SEND)) + QByteArray(32, char(0)),
                     ct::ERR_LOCKED));

    const QByteArray challenge1 = getChallenge();
    CHECK(challenge1.size() == ct::ACCESS_CHALLENGE_LEN);
    // A wrong password produces a wrong answer, and burns the nonce.
    CHECK(expectNack(ct::CMD_ACCESS_RESPONSE,
                     responsePayload(ct::ACCESS_FN_SEND, wrongKey, challenge1),
                     ct::ERR_LOCKED));
    CHECK(expectNack(writeCmd, emptyRange, ct::ERR_LOCKED)); // still locked
    // One nonce, one attempt: even the RIGHT answer to a spent challenge fails.
    CHECK(expectNack(ct::CMD_ACCESS_RESPONSE,
                     responsePayload(ct::ACCESS_FN_SEND, sendKey, challenge1),
                     ct::ERR_LOCKED));

    // A fresh challenge differs, which is what stops a captured reply being
    // replayed on the next connection.
    const QByteArray challenge2 = getChallenge();
    CHECK(challenge2.size() == ct::ACCESS_CHALLENGE_LEN);
    CHECK(challenge2 != challenge1);
    CHECK(expectNack(ct::CMD_ACCESS_RESPONSE,
                     responsePayload(ct::ACCESS_FN_SEND, sendKey, challenge1),
                     ct::ERR_LOCKED));

    // The right password against a live challenge opens the Send half.
    CHECK(prove(ct::ACCESS_FN_SEND, sendKey));
    CHECK(expectAck(writeCmd, oneRecordWrite()));
    CHECK(expectAck(ct::CMD_WRITE_CONFIG_NAME, QByteArray(ct::CONFIG_NAME_LEN, 'x')));

    // ---- proving one function does not open another ----
    // This is the property v19 exists for. v18 had a single password, which
    // meant the weakest of the three was the only one that mattered; here a
    // host that has shown it may Send has shown nothing about whether it may
    // Get, and the device is the only thing that can be asked.
    CHECK(expectAck(ct::CMD_WRITE_ACCESS_KEYS,
                    writeKeyPayload(ct::ACCESS_FN_GET, getKey, false)));
    CHECK(accessMask() == (ct::ACCESS_MASK_SEND | ct::ACCESS_MASK_GET));
    serial_proto_init(nullptr); // both shut again
    CHECK(expectNack(readCmd, emptyRange, ct::ERR_LOCKED));
    CHECK(expectNack(writeCmd, emptyRange, ct::ERR_LOCKED));

    CHECK(prove(ct::ACCESS_FN_SEND, sendKey));
    CHECK(expectAck(writeCmd, oneRecordWrite()));                // Send is open...
    CHECK(expectNack(readCmd, emptyRange, ct::ERR_LOCKED)); // ...and Get is not
    CHECK(expectNack(ct::CMD_READ_CONFIG_NAME, QByteArray(), ct::ERR_LOCKED));
    // Nor is the Send key an answer to the Get challenge: they are different
    // secrets, not one secret under two labels.
    CHECK(expectNack(ct::CMD_ACCESS_RESPONSE,
                     responsePayload(ct::ACCESS_FN_GET, sendKey, getChallenge()),
                     ct::ERR_LOCKED));
    CHECK(expectNack(readCmd, emptyRange, ct::ERR_LOCKED));
    // ...and the right one opens only its own half.
    CHECK(prove(ct::ACCESS_FN_GET, getKey));
    CHECK(exchange(readCmd, emptyRange).size() == 1);
    CHECK(exchange(ct::CMD_READ_CONFIG_NAME, QByteArray()).size() == 1);

    // A function number outside the three is refused rather than shifted into
    // somebody else's bit â€” the shift is only defined for 0..2.
    CHECK(!expectAck(ct::CMD_ACCESS_RESPONSE,
                     responsePayload(quint8(ct::ACCESS_FN_COUNT), getKey, getChallenge())));
    CHECK(!expectAck(ct::CMD_WRITE_ACCESS_KEYS,
                     writeKeyPayload(quint8(ct::ACCESS_FN_COUNT), getKey, false)));
    CHECK(!expectAck(ct::CMD_ACCESS_RESPONSE, QByteArray())); // no function byte at all
    CHECK(accessMask() == (ct::ACCESS_MASK_SEND | ct::ACCESS_MASK_GET));

    // ---- clearing a password puts that function back to open, alone ----
    CHECK(prove(ct::ACCESS_FN_SEND, sendKey));
    CHECK(expectAck(ct::CMD_WRITE_ACCESS_KEYS,
                    writeKeyPayload(ct::ACCESS_FN_SEND, ct::kNoAccessKey, true)));
    CHECK(accessMask() == ct::ACCESS_MASK_GET);
    serial_proto_init(nullptr);
    CHECK(expectAck(writeCmd, oneRecordWrite()));                 // no Send password left
    CHECK(expectNack(readCmd, emptyRange, ct::ERR_LOCKED)); // Get survived the clear
    CHECK(prove(ct::ACCESS_FN_GET, getKey));
    CHECK(expectAck(ct::CMD_WRITE_ACCESS_KEYS,
                    writeKeyPayload(ct::ACCESS_FN_GET, ct::kNoAccessKey, true)));
    CHECK(accessMask() == 0);
    serial_proto_init(nullptr);
    CHECK(exchange(readCmd, emptyRange).size() == 1);
    CHECK(expectAck(writeCmd, oneRecordWrite()));

    // ---- the v18 commands are gone, not merely ignored ----
    // 0x25-0x28 were READ/WRITE_CONFIG_LOCK and LOCK_CHALLENGE/LOCK_RESPONSE.
    // They are deliberately not reused, so a v18 host gets a clean "I do not
    // know that command" instead of a plausible-looking answer from a device
    // that no longer means the same thing by it.
    for (quint8 cmd : {quint8(0x25), quint8(0x26), quint8(0x27), quint8(0x28)})
        CHECK(expectNack(cmd, QByteArray(), ct::ERR_INVALID_CMD));

    // ---- a device with no entropy refuses to issue a challenge ----
    {
        CHECK(expectAck(ct::CMD_WRITE_ACCESS_KEYS,
                        writeKeyPayload(ct::ACCESS_FN_GET, getKey, false)));
        SerialProtoCallbacks noRng{};
        noRng.send_bytes = captureBytes;
        noRng.uptime_ms = fakeUptime;
        serial_proto_init(&noRng); // random_bytes left NULL
        CHECK(getChallenge().isEmpty());
        // Refusing to issue one leaves it locked, which is the safe direction â€”
        // far better than a predictable challenge that could be pre-computed.
        CHECK(expectNack(readCmd, emptyRange, ct::ERR_LOCKED));
    }

    // Put the fixture back as it was found: real callbacks, no passwords, so
    // this test cannot leave a later one talking to a locked device.
    engine_set_access_keys(nullptr);
    serial_proto_init(restore);
    CHECK(accessMask() == 0);
}

// The fleet identity: who the device IS. The block exists so a host can decide
// whether an update belongs on a unit WITHOUT reading its configuration â€” the
// one question a locked-down device still has to answer.
//
// Almost none of it is state any more. Five of the six fields are COMPILED INTO
// the firmware (firmware/include/fleet_identity.h), so there is nothing here to
// program and nothing to round-trip through flash; only config_version lives in
// the header, and that is exercised in testConfigVersion below. What is left to
// prove is that the two halves agree on the layout, that the answer really is
// the built-in identity, that the fleet key never comes back out of it, and
// that 0x30 â€” the write this revision deleted â€” is gone rather than ignored.
//
// One limitation, stated plainly. This executable is linked with NO -DCT_*
// build flags (see CMakeLists.txt), so the unit it faces reports itself
// UNPROVISIONED. Every assertion below is therefore written against
// fleet_identity() rather than against literals, and the handful that need a
// real fleet key are guarded on fleet_key_is_set(). They light up the
// moment the test target is built with CT_FLEET_KEY set, and there is no other
// honest way to reach them: no command can put an identity on a device, which
// is the entire point of moving it into the binary.
static void testFleetIdentityBlock(const SerialProtoCallbacks *restore)
{
    static_assert(sizeof(ct::FleetIdentityPublic) == sizeof(::FleetIdentityPublic),
                  "GUI and firmware fleet identity records differ in size");
    static_assert(sizeof(ct::FleetIdentityPublic) == 41,
                  "FleetIdentityPublic must be 41 bytes: 16 + 16 + 4 + 2 + 2 + 1");
    CHECK(ct::FLEET_VENDOR_ID_LEN == fw::kFleetVendorIdLen);
    CHECK(ct::FLEET_MODEL_ID_LEN == fw::kFleetModelIdLen);
    CHECK(ct::FLEET_KEY_LEN == fw::kFleetKeyLen);
    // One key width, two uses: the fleet key is HMAC'd exactly like an access
    // key, and ct::accessKeyBytes/accessResponse are what the host computes with.
    CHECK(ct::FLEET_KEY_LEN == ct::ACCESS_KEY_LEN);
    CHECK(ct::CMD_READ_FLEET_ID == fw::kCmdReadFleetId);
    CHECK(ct::CMD_FLEET_ID_PROVE == fw::kCmdFleetIdProve);
    // ...and the MODEL's view of the two string budgets, since
    // FleetIdentity::clampToWire is handed these and is the only thing standing
    // between a long vendor name and a field cut in half mid-character.
    CHECK(ct::kFleetVendorIdBytes == fw::kFleetVendorIdLen);
    CHECK(ct::kFleetModelIdBytes == fw::kFleetModelIdLen);

    // Field by field through the firmware's own view of the same bytes, the way
    // the signal and integrator records are checked in main(). sizeof alone
    // would sail past a reordering and land the config version in flags â€” which
    // is precisely the field that decides whether an update goes on.
    {
        ct::FleetIdentityPublic g{};
        for (int i = 0; i < ct::FLEET_VENDOR_ID_LEN; ++i)
            g.vendor_id[i] = char('A' + i);
        for (int i = 0; i < ct::FLEET_MODEL_ID_LEN; ++i)
            g.model_id[i] = char('a' + i);
        g.serial_number = 0x01020304u;
        g.config_version = 0x1234;
        g.flags = 0x5678;
        g.key_present = 1;

        ::FleetIdentityPublic f;
        std::memcpy(&f, &g, sizeof(f));
        CHECK(std::memcmp(f.vendor_id, g.vendor_id, fw::kFleetVendorIdLen) == 0);
        CHECK(std::memcmp(f.model_id, g.model_id, fw::kFleetModelIdLen) == 0);
        CHECK(f.serial_number == 0x01020304u);
        CHECK(f.config_version == 0x1234);
        CHECK(f.flags == 0x5678);
        CHECK(f.key_present == 1);
        // NUL-PADDED, not NUL-terminated: all sixteen bytes are usable, so the
        // last one here is a character and not a terminator. A reader that used
        // strcmp would be right by accident on short names and wrong on the one
        // case that matters.
        CHECK(f.vendor_id[fw::kFleetVendorIdLen - 1] == char('A' + 15));
        CHECK(f.model_id[fw::kFleetModelIdLen - 1] == char('a' + 15));
    }

    // The build-time identity this binary actually carries, and the two
    // questions the firmware answers about it.
    const ::FleetIdentity *built = fleet_identity();
    const bool keySet = fleet_key_is_set();
    const QByteArray fleetKeyBytes(reinterpret_cast<const char *>(built->fleet_key),
                                    fw::kFleetKeyLen);
    const ct::AccessKey fleetKey = ct::accessKeyFromBytes(fleetKeyBytes);
    CHECK(keySet == (fleetKey != ct::kNoAccessKey));

    // The one runtime field, put somewhere recognisable first â€” otherwise "did
    // the flash header's version reach the answer?" is not a question the
    // exchange below can be asked.
    engine_set_config_version(0x4321);

    // ---- over the wire: READ_FLEET_ID answers, and never with the key ----
    {
        const auto packets = exchange(ct::CMD_READ_FLEET_ID, QByteArray());
        CHECK(packets.size() == 1);
        if (packets.size() == 1) {
            CHECK(packets[0].cmd == ct::CMD_READ_FLEET_ID);
            CHECK(packets[0].payload.size() == int(sizeof(ct::FleetIdentityPublic)));

            ct::FleetIdentityPublic pub{};
            std::memcpy(&pub, packets[0].payload.constData(),
                        qMin(size_t(packets[0].payload.size()), sizeof(pub)));
            CHECK(std::memcmp(pub.vendor_id, built->vendor_id, fw::kFleetVendorIdLen) == 0);
            CHECK(std::memcmp(pub.model_id, built->model_id, fw::kFleetModelIdLen) == 0);
            CHECK(pub.serial_number == built->serial_number);
            CHECK(pub.flags == built->flags);
            CHECK(pub.key_present == (keySet ? 1 : 0));
            // ...and the one field that does NOT come from the build.
            CHECK(pub.config_version == 0x4321);

            // The assertion this command exists to satisfy. A host learns that
            // the device HOLDS a fleet key, never what it is â€” with the key,
            // anyone could impersonate the fleet it names. Guarded, because on
            // an unprovisioned unit the "key" is four zero bytes and a serial
            // number of zero would make the search hit for the wrong reason.
            if (keySet)
                CHECK(!packets[0].payload.contains(fleetKeyBytes));

            // The GUI's own parser against the bytes the firmware just emitted.
            // Asserting the layout by hand only proves the firmware is
            // self-consistent; this proves the two sides agree.
            ct::device_session::FleetIdentityState parsed;
            CHECK(ct::device_session::parseFleetIdentity(packets[0].payload, &parsed));
            CHECK(parsed.supported);
            CHECK(parsed.keyPresent == keySet);
            // Never, under any circumstances, populated from a read.
            CHECK(parsed.identity.fleetKey == ct::kNoAccessKey);
            CHECK(parsed.identity.serialNumber == built->serial_number);
            CHECK(parsed.identity.flags == built->flags);
            CHECK(parsed.identity.configVersion == 0x4321);
            // The counted fields arrive as QStrings with the padding gone: read
            // to the first NUL or the sixteenth byte, whichever comes first.
            CHECK(parsed.identity.vendorId
                  == QString::fromUtf8(built->vendor_id,
                                       int(qstrnlen(built->vendor_id, fw::kFleetVendorIdLen))));
            CHECK(parsed.identity.modelId
                  == QString::fromUtf8(built->model_id,
                                       int(qstrnlen(built->model_id, fw::kFleetModelIdLen))));
            // Host and device must agree on what "unprovisioned" means, or the
            // uploader either refuses a legitimate update or stops checking a
            // device it should have checked.
            CHECK(parsed.identity.isSet() == fleet_identity_is_set());

            // A short payload is refused rather than read past its end.
            CHECK(!ct::device_session::parseFleetIdentity(packets[0].payload.left(3), &parsed));
            CHECK(!ct::device_session::parseFleetIdentity(QByteArray(), &parsed));
        }
    }

    // ---- the one exchange that runs device -> host ----
    // Reading the identity says what the device CLAIMS to be. This says it
    // actually holds the fleet secret, which is what stops a look-alike
    // collecting an update meant for somebody else's fleet.
    {
        QByteArray challenge(ct::ACCESS_CHALLENGE_LEN, char(0));
        for (int i = 0; i < challenge.size(); ++i)
            challenge[i] = char(0x30 + i);

        if (keySet) {
            const auto packets = exchange(ct::CMD_FLEET_ID_PROVE, challenge);
            CHECK(packets.size() == 1);
            if (packets.size() == 1) {
                CHECK(packets[0].cmd == ct::CMD_FLEET_ID_PROVE);
                // The device's HMAC and ct::accessResponse are two independent
                // implementations of one construction, over a key laid out
                // big-endian by both. This is the only place they meet, and a
                // disagreement here would look from either side like a fleet
                // key somebody typed in wrong.
                CHECK(packets[0].payload == ct::accessResponse(fleetKey, challenge));
                CHECK(packets[0].payload != ct::accessResponse(fleetKey + 1, challenge));
            }
        } else {
            // An unprovisioned unit attests to NOTHING. All-zero is "no key
            // set", the same convention the host uses; without it a blank unit
            // would happily answer under a key of zeroes â€” which anyone can
            // compute â€” and pass the one check meant to catch a look-alike.
            CHECK(fleetKey == ct::kNoAccessKey);
            CHECK(!expectAck(ct::CMD_FLEET_ID_PROVE, challenge));
        }

        // A challenge of the wrong length is refused rather than padded into an
        // answer the host would then believe. True whether or not a key is set.
        CHECK(expectNack(ct::CMD_FLEET_ID_PROVE, challenge.left(4), ct::ERR_INVALID_LEN));
        CHECK(expectNack(ct::CMD_FLEET_ID_PROVE, QByteArray(), ct::ERR_INVALID_LEN));
        CHECK(expectNack(ct::CMD_FLEET_ID_PROVE, challenge + QByteArray(1, char(0)),
                         ct::ERR_INVALID_LEN));
        CHECK(ct::ERR_INVALID_LEN == fw::kErrInvalidLen);
    }

    // ---- both stay answerable behind a Send password ----
    // The whole point of the block: deciding whether an update belongs on this
    // unit must not require the password that guards its configuration. A
    // customer holding a locked-down device still has to be able to take
    // updates, and the alternative â€” handing out the Get password so the host
    // can read the config and work it out â€” gives away far more.
    {
        // A virgin header for the key write to land in. Setting a password now
        // commits it in the same breath (see testAccessKeyDurability), and
        // STM32 flash programs each doubleword once between erases.
        engine_clear_config();

        const ct::AccessKey sendKey = ct::deriveAccessKey(QStringLiteral("send-me"));
        ct::AccessKeyWritePayload set{};
        set.function = ct::ACCESS_FN_SEND;
        std::memcpy(set.key, ct::accessKeyBytes(sendKey).constData(), ct::ACCESS_KEY_LEN);
        CHECK(expectAck(ct::CMD_WRITE_ACCESS_KEYS,
                        QByteArray(reinterpret_cast<const char *>(&set), sizeof(set))));
        serial_proto_init(nullptr); // a fresh session has proved nothing

        // The lock really is on â€” without this the two lines after it would
        // pass on a device that simply had no password.
        CHECK(expectNack(ct::CMD_WRITE_MSG_CFG, QByteArray(4, char(0)), ct::ERR_LOCKED));
        CHECK(exchange(ct::CMD_READ_FLEET_ID, QByteArray()).size() == 1);
        CHECK(exchange(ct::CMD_FLEET_ID_PROVE,
                       QByteArray(ct::ACCESS_CHALLENGE_LEN, char(0x22)))
                  .size()
              == 1);
    }

    // ---- there is no way to WRITE an identity ----
    // The identity used to be runtime state a host could program, on 0x30. It is
    // compiled into the firmware now â€” re-badging a unit means building and
    // flashing it â€” and 0x30 has since been reused for CMD_READ_CAN_SETUP.
    //
    // So the thing to pin is no longer "0x30 is unknown", which would now be
    // false; it is that NOTHING accepts a fleet identity as a payload. Offering
    // one to the command that occupies the old slot must not be mistaken for
    // programming an identity: READ_CAN_SETUP ignores its payload entirely and
    // answers the bus setup, which is a wrong answer to the wrong question
    // rather than a write.
    CHECK(ct::ERR_INVALID_CMD == fw::kErrInvalidCmd);
    {
        const auto reply =
            exchange(quint8(0x30), QByteArray(int(sizeof(ct::FleetIdentityPublic)), char(0x5A)));
        CHECK(reply.size() == 1);
        CHECK(reply[0].cmd == ct::CMD_READ_CAN_SETUP);
        CHECK(reply[0].payload.size() == 3 * int(sizeof(ct::ControlCanPayload)));
        // And the identity is untouched by having been offered. This build sets
        // no CT_* flags, so it must still read as unprovisioned.
        const QByteArray zeroed(ct::FLEET_VENDOR_ID_LEN, char(0));
        CHECK(std::memcmp(fleet_identity()->vendor_id, zeroed.constData(),
                          size_t(ct::FLEET_VENDOR_ID_LEN))
              == 0);
        CHECK(!fleet_identity_is_set());
    }

    // Leave nothing behind: no keys, no version, an erased store and the real
    // callbacks, so the tests after this one talk to a plain device.
    engine_set_access_keys(nullptr);
    engine_set_config_version(0);
    flashErase();
    serial_proto_init(restore);
}

// The configuration's version number â€” the one part of the fleet identity that
// is NOT compiled in, because it has to move every time a configuration is
// released. It rides CMD_SAVE_TO_FLASH rather than having a command of its own
// so the two cannot separate: a version that landed without its configuration,
// or a configuration that landed without its version, would each make the
// host's "is this update newer?" check lie, in opposite directions.
//
// Note the erases scattered through this. They are not housekeeping â€” STM32
// flash programs each doubleword once between erases, so a second commit needs
// a virgin header, exactly as on the device where a Send always follows a CLEAR.
static void testConfigVersion(const SerialProtoCallbacks *restore)
{
    CHECK(ct::CMD_SAVE_TO_FLASH == fw::kCmdSaveToFlash);

    EngineCallbacks cb{};
    cb.transmit_can = captureTransmit;
    engine_init(&cb);
    engine_set_access_keys(nullptr);
    serial_proto_init(restore);
    engine_clear_config();
    CHECK(engine_config_version() == 0);

    // ---- a two-byte payload sets the version, and the commit carries it ----
    // Little-endian, like every other multi-byte field on this wire.
    QByteArray version(2, char(0));
    version[0] = char(0x39);
    version[1] = char(0x05); // 0x0539 = 1337
    CHECK(expectAck(ct::CMD_SAVE_TO_FLASH, version));
    CHECK(engine_config_version() == 1337);
    {
        uint16_t stored = 0xFFFF;
        CHECK(flash_store_validate(nullptr, nullptr, nullptr, nullptr, &stored, nullptr, nullptr, nullptr));
        CHECK(stored == 1337);
        CHECK(flash_store_config_status() == ct::CONFIG_STATUS_OK);
    }
    // ...and it comes back with the image. Without that a device could not
    // answer "which revision am I running?" after a power cycle, which is the
    // only moment the answer is ever needed.
    engine_set_config_version(0);
    CHECK(engine_load_config(nullptr));
    CHECK(engine_config_version() == 1337);

    // ---- no payload leaves the stored version alone ----
    // A Send that is not a release must not silently renumber the unit to
    // revision zero â€” every later update would then look newer than it is.
    flashErase();
    CHECK(expectAck(ct::CMD_SAVE_TO_FLASH, QByteArray()));
    CHECK(engine_config_version() == 1337);
    {
        uint16_t stored = 0xFFFF;
        CHECK(flash_store_validate(nullptr, nullptr, nullptr, nullptr, &stored, nullptr, nullptr, nullptr));
        CHECK(stored == 1337);
    }

    // ---- one byte, or three, is a malformed commit and not a partial one ----
    // Guessing at the missing byte would put a number the host never sent into
    // the header, and the host would go on to believe the device was running it.
    flashErase();
    CHECK(expectNack(ct::CMD_SAVE_TO_FLASH, version.left(1), ct::ERR_INVALID_LEN));
    CHECK(expectNack(ct::CMD_SAVE_TO_FLASH, version + QByteArray(1, char(0)),
                     ct::ERR_INVALID_LEN));
    CHECK(engine_config_version() == 1337);
    // A refused commit must not have written a header either: the point of the
    // NACK is that nothing happened, not that something half did.
    CHECK(!flash_store_validate(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr));

    // ---- CLEAR_CONFIG drops the version and keeps the keys ----
    // The two deliberately differ, and the difference is easy to get backwards,
    // so it is worth a test rather than a comment. A device with no
    // configuration is running revision nothing: reporting a stale version
    // would answer the host's "is this newer?" question about a configuration
    // that is not there. The access keys go the other way â€” clearing the tables
    // must never be a way to shed the protection over them.
    {
        const ct::AccessKey sendKey = ct::deriveAccessKey(QStringLiteral("still-here"));
        const QByteArray sendKeyBytes = ct::accessKeyBytes(sendKey);
        ::AccessKeyRecord keys{};
        keys.set_mask = ct::ACCESS_MASK_SEND;
        std::memcpy(keys.keys[ct::ACCESS_FN_SEND], sendKeyBytes.constData(), ct::ACCESS_KEY_LEN);
        engine_set_access_keys(&keys);
        engine_set_config_version(4242);
        CHECK(engine_config_version() == 4242);

        engine_clear_config();
        CHECK(engine_config_version() == 0);
        CHECK(engine_access_keys()->set_mask == ct::ACCESS_MASK_SEND);
        CHECK(std::memcmp(engine_access_keys()->keys[ct::ACCESS_FN_SEND],
                          sendKeyBytes.constData(), ct::ACCESS_KEY_LEN)
              == 0);
    }

    engine_set_access_keys(nullptr);
    engine_set_config_version(0);
    flashErase();
    serial_proto_init(restore);
}

// Setting a password has to STICK. It used to reach flash only on the next
// SAVE_TO_FLASH, which meant a device powered off between the two came back
// with no password at all â€” the one failure a password must not have, because
// nothing about it is visible from the outside: the device looks fine, answers
// everything, and the protection is simply gone.
//
// A rate counter is driven by the clock, so its whole contract is a timing one:
// exactly rate_hz steps per second, gated by Enable, zeroed by Reset, and
// clamped like any other counter. Ticking the real engine is the only way to
// check that â€” a struct round-trip says nothing about whether the phase
// accumulator spends its 1000 Hz*ms correctly.
static void testCounterRateMode()
{
    EngineCallbacks cb{};
    cb.transmit_can = captureTransmit;
    engine_init(&cb);
    engine_clear_config();

    // Two counters in one pass, written together: flash programs each slot once
    // between erases, so a record cannot be rewritten mid-test. Enable and Reset
    // are left unused here â€” they are the counter's existing shared edge logic,
    // already exercised by the up/down counter, and nothing about them changes
    // in rate mode except that the phase freezes rather than the value.
    //
    // 0: 10 Hz counting up from 0.  1: 100 Hz counting down from 5, floor 0.
    constexpr quint16 kUp = 0, kDown = 1;
    CounterConfig c[2]{};
    for (CounterConfig &x : c) {
        x.up_signal_idx = ct::SIG_MSG_NONE;
        x.down_signal_idx = ct::SIG_MSG_NONE;
        x.follow_signal_idx = ct::SIG_MSG_NONE;
        x.reset_signal_idx = ct::SIG_MSG_NONE;
        x.enable_signal_idx = ct::SIG_MSG_NONE; // always enabled
        x.step = 1.0f;
        x.mode = COUNTER_MODE_RATE;
    }
    c[0].dest_signal_idx = kUp;
    c[0].min_value = 0.0f;
    c[0].max_value = 1000.0f;
    c[0].flags = ct::COUNTERFLAG_ACTIVE;
    c[0].rate_hz = 10; // one step every 100 ms, i.e. every 10 ticks
    c[1].dest_signal_idx = kDown;
    c[1].min_value = 0.0f;
    c[1].max_value = 1000.0f;
    c[1].flags = quint8(ct::COUNTERFLAG_ACTIVE | fw::kCounterFlagRateDown);
    c[1].rate_hz = 100; // one step per tick â€” the ceiling
    CHECK(engine_table_write(ENGINE_TABLE_COUNTERS, 0, 2, (const quint8 *)c));

    // Nine ticks is 90 ms and must NOT have stepped; the tenth completes the
    // first period. This is the off-by-one the phase accumulator exists for.
    for (int i = 0; i < 9; ++i)
        engine_tick(10);
    CHECK(qAbs(engine_signal_value(kUp) - 0.0f) < 0.001f);
    engine_tick(10);
    CHECK(qAbs(engine_signal_value(kUp) - 1.0f) < 0.001f);

    // A full second from a standing start is exactly ten steps â€” not nine and
    // not eleven â€” because the phase is SPENT (minus 1000) rather than zeroed.
    for (int i = 0; i < 90; ++i)
        engine_tick(10);
    CHECK(qAbs(engine_signal_value(kUp) - 10.0f) < 0.001f);

    // The down counter ran for those same 100 ticks at one step per tick, from
    // a starting value of 0, so it has spent the whole time clamped at its
    // floor rather than running negative.
    CHECK(qAbs(engine_signal_value(kDown) - 0.0f) < 0.001f);

    // 100 Hz really is one step per 10 ms tick, in the other direction: give the
    // down counter headroom by rolling instead of clamping is not needed â€” just
    // check it does not move past its floor while the up one keeps climbing.
    const float upBefore = engine_signal_value(kUp);
    engine_tick(10);
    CHECK(qAbs(engine_signal_value(kUp) - upBefore) < 0.001f); // 10 Hz: no step yet
    for (int i = 0; i < 9; ++i)
        engine_tick(10);
    CHECK(qAbs(engine_signal_value(kUp) - (upBefore + 1.0f)) < 0.001f);

    engine_clear_config();
}

// STORE v12: a timer's trigger is a COMPARISON, not "this channel is non-zero".
//
// Run through the real engine rather than asserted against the mapper, because
// the claim being made is about what the DEVICE does: the timer pass hands its
// terms to evalConditionTerm(), the same function a User Condition's
// comparisons go through, so a threshold on a timer and the identical threshold
// on a condition cannot disagree.
//
// Two timers either side of one threshold, because a comparison that is always
// true and a comparison that is always false are the two ways to get this wrong
// and one of them passes a test that only checks the true case.
//
// The inputs are CONSTANTS held at fixed values rather than a signal driven up
// and down mid-test: the RAM flash programs each slot once between erases, so a
// record cannot be rewritten while the engine is running.
static void testTimerComparisonTrigger()
{
    EngineCallbacks cb{};
    cb.transmit_can = captureTransmit;
    engine_init(&cb);
    engine_clear_config();

    constexpr quint16 kLow = 0, kHigh = 1, kOutLow = 2, kOutHigh = 3;
    ct::ConstantConfig k[2]{};
    k[0].dest_signal_idx = kLow;
    k[0].value = 1000.0f;
    k[0].is_active = 1;
    k[1].dest_signal_idx = kHigh;
    k[1].value = 5000.0f;
    k[1].is_active = 1;
    CHECK(engine_table_write(ENGINE_TABLE_CONSTANTS, 0, 2, (const quint8 *)k));

    TimerConfig t[2]{};
    for (TimerConfig &x : t) {
        x.start_term.op = quint8(ct::COND_OP_GT);
        x.start_term.input_b_type = 0;
        x.start_term.b.input_b_const = 4000.0f;
        // No stop half. Out of range is how a term says "never", so these run
        // once started and nothing stops them — which is what an empty Stop
        // channel always meant.
        x.stop_term.input_a_signal_idx = ct::kTimerTermUnused;
        x.flags = quint8(ct::TIMERFLAG_ACTIVE);
    }
    t[0].start_term.input_a_signal_idx = kLow;
    t[0].dest_signal_idx = kOutLow;
    t[1].start_term.input_a_signal_idx = kHigh;
    t[1].dest_signal_idx = kOutHigh;
    CHECK(engine_table_write(ENGINE_TABLE_TIMERS, 0, 2, (const quint8 *)t));

    for (int i = 0; i < 100; ++i) // one second
        engine_tick(10);

    // 1000 is not greater than 4000, so that timer never started and its output
    // never moved. This is the half that a "does the comparison fire" test
    // written on its own would miss.
    CHECK(qAbs(engine_signal_value(kOutLow)) < 0.001f);
    // 5000 is, so it started on the first pass and has run about a second.
    CHECK(engine_signal_value(kOutHigh) > 0.95f);
    CHECK(engine_signal_value(kOutHigh) < 1.05f);
}

// STORE v13: the "for" qualifier on a User Condition, in both of its shapes.
//
// Run on the real engine, because the claim is about time and the only honest
// way to test time is to advance it. Three conditions in one pass:
//
//   A  X > 4000, qualified 500 ms over the WHOLE expression
//   B  (X > 4000 AND Y > 10), qualified 500 ms on TERM 0 ONLY
//   C  X > 4000, no qualifier at all — the control, proving a pre-v13 condition
//      is untouched by any of this
//
// X is held high by a constant and Y high by another, so both comparisons are
// true from the first pass and the only variable is elapsed time.
static void testConditionForQualifier()
{
    EngineCallbacks cb{};
    cb.transmit_can = captureTransmit;
    engine_init(&cb);
    engine_clear_config();

    constexpr quint16 kX = 0, kY = 1, kOutA = 2, kOutB = 3, kOutC = 4;
    ct::ConstantConfig k[2]{};
    k[0].dest_signal_idx = kX;
    k[0].value = 5000.0f;
    k[0].is_active = 1;
    k[1].dest_signal_idx = kY;
    k[1].value = 50.0f;
    k[1].is_active = 1;
    CHECK(engine_table_write(ENGINE_TABLE_CONSTANTS, 0, 2, (const quint8 *)k));

    ConditionConfig c[3]{};
    for (ConditionConfig &x : c) {
        x.set_terms[0].input_a_signal_idx = kX;
        x.set_terms[0].op = quint8(ct::COND_OP_GT);
        x.set_terms[0].input_b_type = 0;
        x.set_terms[0].b.input_b_const = 4000.0f;
        x.set_count = 1;
        x.flags = quint8(ct::CONDFLAG_ACTIVE | ct::CONDFLAG_SETRESET);
        x.latch_hz = 10;
    }
    c[0].dest_signal_idx = kOutA;
    c[0].set_qualify_cs = 50; // 500 ms, in centiseconds
    c[0].set_qualify_terms = 0; // the whole expression

    c[1].dest_signal_idx = kOutB;
    c[1].set_terms[1].input_a_signal_idx = kY;
    c[1].set_terms[1].op = quint8(ct::COND_OP_GT);
    c[1].set_terms[1].input_b_type = 0;
    c[1].set_terms[1].b.input_b_const = 10.0f;
    c[1].set_count = 2;
    c[1].set_joiners = 0; // AND
    c[1].set_qualify_cs = 50;
    c[1].set_qualify_terms = 0x01; // term 0 only

    c[2].dest_signal_idx = kOutC;
    c[2].set_qualify_cs = 0; // unqualified control
    CHECK(engine_table_write(ENGINE_TABLE_CONDITIONS, 0, 3, (const quint8 *)c));

    // First pass. The control is already true; neither qualified one is.
    engine_tick(10);
    CHECK(engine_signal_value(kOutC) > 0.5f);
    CHECK(engine_signal_value(kOutA) < 0.5f);
    CHECK(engine_signal_value(kOutB) < 0.5f);

    // 490 ms total: still short of 500.
    for (int i = 0; i < 48; ++i)
        engine_tick(10);
    CHECK(engine_signal_value(kOutA) < 0.5f);
    CHECK(engine_signal_value(kOutB) < 0.5f);

    // The tick that crosses 500 ms is the one that qualifies both.
    engine_tick(10);
    CHECK(engine_signal_value(kOutA) > 0.5f);
    CHECK(engine_signal_value(kOutB) > 0.5f);
    CHECK(engine_signal_value(kOutC) > 0.5f);
}


// STORE v14: Reset carries its own duration, independent of Set's.
//
// The property worth pinning is that the two clocks are SEPARATE. A condition
// that sets instantly and clears only after the fault has been gone a while is
// the shape this was asked for, and it is also the shape that a single shared
// accumulator would get wrong — Set going true would charge the clock Reset
// later reads.
// A RECORD THAT LIES ABOUT HOW MANY COMPARISONS IT HAS must not be believed.
//
// set_count is a byte off the wire, and the per-term qualify branch used to
// loop over it raw — past a three-entry stack array, past the
// [MAX_CONDITIONS][2][COND_MAX_TERMS] accumulator, and past the end of the
// 62-byte record. foldConditionExpr() and conditionExprInRange() both clamp
// their own copy, which is why the unqualified and whole-expression paths were
// never affected and why the gap survived: conditionExprInRange() checks three
// terms, says yes, and hands the raw 255 straight on. Nothing between the wire
// and here inspects the field — engine_table_write() forwards to
// flash_store_write() without reading record contents.
//
// WHAT THIS TEST DOES AND DOES NOT PROVE. It pins the specified behaviour: a
// count above the maximum is treated AS the maximum, so the record folds its
// three real comparisons and nothing else. It does NOT demonstrate the overrun.
// That was measured, not assumed — with the clamp deleted this test still
// passed, because reading and writing past those arrays is undefined behaviour
// and this build happened to survive it. A test cannot reliably detect UB by
// its symptoms, so do not read a pass here as evidence the clamp is redundant;
// read it as the contract the clamp exists to keep.
static void testConditionTermCountIsClamped()
{
    EngineCallbacks cb{};
    cb.transmit_can = captureTransmit;
    engine_init(&cb);
    engine_clear_config();

    constexpr quint16 kX = 0, kLiar = 1, kCanary = 2, kShort = 3, kCanary2 = 4;
    ct::ConstantConfig k{};
    k.dest_signal_idx = kX;
    k.value = 5000.0f;
    k.is_active = 1;
    CHECK(engine_table_write(ENGINE_TABLE_CONSTANTS, 0, 1, (const quint8 *)&k));

    ConditionConfig c[2]{};
    for (ConditionConfig &x : c) {
        for (int t = 0; t < ct::COND_MAX_TERMS; ++t) {
            x.set_terms[t].input_a_signal_idx = kX;
            x.set_terms[t].op = quint8(ct::COND_OP_GT);
            x.set_terms[t].input_b_type = 0;
            x.set_terms[t].b.input_b_const = 4000.0f;
        }
        x.set_joiners = 0; // AND throughout
        x.flags = quint8(ct::CONDFLAG_ACTIVE);
        x.latch_hz = 10;
        x.set_qualify_cs = 50;      // 500 ms
        x.set_qualify_terms = 0x01; // the per-term branch, the one that overran
    }
    c[0].dest_signal_idx = kLiar;
    c[0].set_count = 255; // the lie
    c[1].dest_signal_idx = kCanary;
    c[1].set_count = 1;
    CHECK(engine_table_write(ENGINE_TABLE_CONDITIONS, 0, 2, (const quint8 *)c));

    // Short of the hold, both are false.
    for (int i = 0; i < 49; ++i)
        engine_tick(10);
    CHECK(engine_signal_value(kLiar) < 0.5f);
    CHECK(engine_signal_value(kCanary) < 0.5f);

    // The tick that crosses 500 ms qualifies both.
    engine_tick(10);
    CHECK(engine_signal_value(kLiar) > 0.5f);
    CHECK(engine_signal_value(kCanary) > 0.5f);

    // And the clamp folds three REAL comparisons rather than stopping at one:
    // make the third false under AND and the liar must go false with it. A
    // clamp that truncated to one term, or a fold that ran off the record and
    // read whatever followed, both fail here. These go in FRESH slots: a
    // written flash record cannot be rewritten without an erase, so reusing 0
    // and 1 would fail the write rather than test the fold.
    ConditionConfig again[2]{};
    for (ConditionConfig &x : again) {
        for (int t = 0; t < ct::COND_MAX_TERMS; ++t) {
            x.set_terms[t].input_a_signal_idx = kX;
            x.set_terms[t].op = quint8(ct::COND_OP_GT);
            x.set_terms[t].input_b_type = 0;
            x.set_terms[t].b.input_b_const = 4000.0f;
        }
        x.set_joiners = 0;
        x.flags = quint8(ct::CONDFLAG_ACTIVE);
        x.latch_hz = 10;
        x.set_qualify_cs = 50;
        x.set_qualify_terms = 0x01;
    }
    // X is 5000, so "> 9000" is false and the AND cannot hold.
    again[0].set_terms[2].b.input_b_const = 9000.0f;
    again[0].dest_signal_idx = kShort;
    again[0].set_count = 255;
    again[1].dest_signal_idx = kCanary2;
    again[1].set_count = 1;
    CHECK(engine_table_write(ENGINE_TABLE_CONDITIONS, 2, 2, (const quint8 *)again));

    // Caught ON the crossing tick — these are Momentary, so the output is a
    // pulse one latch period wide, not a level to read later.
    for (int i = 0; i < 49; ++i)
        engine_tick(10);
    CHECK(engine_signal_value(kShort) < 0.5f);
    CHECK(engine_signal_value(kCanary2) < 0.5f);

    engine_tick(10);
    CHECK(engine_signal_value(kShort) < 0.5f);   // third comparison is false
    CHECK(engine_signal_value(kCanary2) > 0.5f); // one true comparison, held
}

static void testConditionResetQualifier()
{
    EngineCallbacks cb{};
    cb.transmit_can = captureTransmit;
    engine_init(&cb);
    engine_clear_config();

    constexpr quint16 kX = 0, kOut = 1;
    // X sits at 5000 the whole test. Set (X > 4000) is true throughout; Reset
    // (X < 1000) is false throughout, so the latch should set at once and never
    // clear — and the Reset duration must not delay the SET.
    ct::ConstantConfig k{};
    k.dest_signal_idx = kX;
    k.value = 5000.0f;
    k.is_active = 1;
    CHECK(engine_table_write(ENGINE_TABLE_CONSTANTS, 0, 1, (const quint8 *)&k));

    ConditionConfig c{};
    c.set_terms[0].input_a_signal_idx = kX;
    c.set_terms[0].op = quint8(ct::COND_OP_GT);
    c.set_terms[0].b.input_b_const = 4000.0f;
    c.set_count = 1;
    c.reset_terms[0].input_a_signal_idx = kX;
    c.reset_terms[0].op = quint8(ct::COND_OP_LT);
    c.reset_terms[0].b.input_b_const = 1000.0f;
    c.reset_count = 1;
    c.dest_signal_idx = kOut;
    c.flags = quint8(ct::CONDFLAG_ACTIVE | ct::CONDFLAG_SETRESET);
    c.latch_hz = 10;
    c.set_qualify_cs = 0;    // set immediately
    c.reset_qualify_cs = 50; // but only clear after 500 ms of the reset holding
    CHECK(engine_table_write(ENGINE_TABLE_CONDITIONS, 0, 1, (const quint8 *)&c));

    // Sets on the first pass: a Reset duration must not hold the SET back.
    engine_tick(10);
    CHECK(engine_signal_value(kOut) > 0.5f);

    // And stays set, because Reset is never true here. If the two sides shared
    // an accumulator, Set's continuous truth would have charged Reset's clock
    // and this is where it would clear.
    for (int i = 0; i < 200; ++i)
        engine_tick(10);
    CHECK(engine_signal_value(kOut) > 0.5f);
}

// STORE v15: a counter can step on a MESSAGE rather than a channel.
//
// "Count every frame the ECU sends" was a User Condition and a generated channel
// before this; now the counter says it. Run on the real engine because the claim
// is about frames arriving, and the only honest way to test that is to push
// frames in.
//
// The property that matters and is easy to get wrong: ONE FRAME, ONE STEP. The
// event flag is true for exactly the pass in which the frame happened, so the
// existing rising-edge detector does the right thing without a special case —
// but a flag left set for two passes, or cleared before the counter ran, would
// give two steps or none.
static void testCounterMessageInput()
{
    EngineCallbacks cb{};
    cb.transmit_can = captureTransmit;
    engine_init(&cb);
    engine_clear_config();

    constexpr quint32 kId = 0x123;
    constexpr quint16 kOut = 0;

    ct::CanMessageConfig msg{};
    msg.can_id = kId;
    msg.flags = ct::MSGFLAG_ACTIVE;
    msg.src_bus = 1;
    msg.dlc = 8;
    CHECK(engine_table_write(ENGINE_TABLE_MESSAGES, 0, 1, (const quint8 *)&msg));

    CounterConfig c{};
    c.up_signal_idx = 0; // MESSAGE index 0, not a signal index
    c.down_signal_idx = ct::SIG_MSG_NONE;
    c.follow_signal_idx = ct::SIG_MSG_NONE;
    c.reset_signal_idx = ct::SIG_MSG_NONE;
    c.enable_signal_idx = ct::SIG_MSG_NONE;
    c.dest_signal_idx = kOut;
    c.min_value = 0.0f;
    c.max_value = 1000.0f;
    c.step = 1.0f;
    c.mode = ct::COUNTER_MODE_UPDOWN;
    c.flags = quint8(ct::COUNTERFLAG_ACTIVE);
    c.input_kinds = quint8(ct::COUNTER_SRC_MSG_RX << ct::kCounterSrcShiftUp);
    CHECK(engine_table_write(ENGINE_TABLE_COUNTERS, 0, 1, (const quint8 *)&c));

    // Ticks with no traffic move nothing.
    for (int i = 0; i < 5; ++i)
        engine_tick(10);
    CHECK(qAbs(engine_signal_value(kOut)) < 0.001f);

    // Three frames, each followed by the pass that sees it: three steps.
    const uint8_t f[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    for (int i = 0; i < 3; ++i) {
        engine_process_can(1, kId, 0, 0, f, 8);
        engine_tick(10);
    }
    CHECK(qAbs(engine_signal_value(kOut) - 3.0f) < 0.001f);

    // And it STOPS. A flag that outlived its pass would keep stepping here,
    // which is the failure this half exists to catch.
    for (int i = 0; i < 10; ++i)
        engine_tick(10);
    CHECK(qAbs(engine_signal_value(kOut) - 3.0f) < 0.001f);
}

// A counter with max <= min runs UNLIMITED — that is clampRoll's "clamping
// off" convention, and the normal counting path honours it. The reset seed
// must honour it too: with min = max = 0 (the natural encoding of an unlimited
// counter) and a reset value of 100, the old unconditional clamp replaced the
// 100 with max_value = 0, silently destroying the configured reset. The
// integrator's reset always had the guard; this proves the counter's matches.
static void testCounterResetUnlimited()
{
    EngineCallbacks cb{};
    cb.transmit_can = captureTransmit;
    engine_init(&cb);
    engine_clear_config();

    // Signal 6 drives both resets, held at 1 by a constant. Constants execute
    // before counters in the same pass and g_counter_prev starts zeroed, so
    // the very first tick is the rising edge — no mid-test record rewrite,
    // which the RAM flash (correctly) refuses.
    constexpr quint16 kUnlimited = 4, kLimited = 5, kResetInput = 6;
    ct::ConstantConfig k{};
    k.dest_signal_idx = kResetInput;
    k.value = 1.0f;
    k.is_active = 1;
    CHECK(engine_table_write(ENGINE_TABLE_CONSTANTS, 0, 1, (const quint8 *)&k));

    CounterConfig c[2]{};
    for (CounterConfig &x : c) {
        x.up_signal_idx = ct::SIG_MSG_NONE;
        x.down_signal_idx = ct::SIG_MSG_NONE;
        x.follow_signal_idx = ct::SIG_MSG_NONE;
        x.enable_signal_idx = ct::SIG_MSG_NONE;
        x.reset_signal_idx = kResetInput;
        x.reset_value = 100.0f;
        x.step = 1.0f;
        x.flags = ct::COUNTERFLAG_ACTIVE;
    }
    c[0].dest_signal_idx = kUnlimited;
    c[0].min_value = 0.0f; // max <= min: limits off
    c[0].max_value = 0.0f;
    c[1].dest_signal_idx = kLimited;
    c[1].min_value = 0.0f; // a real span: the seed must still clamp into it
    c[1].max_value = 50.0f;
    CHECK(engine_table_write(ENGINE_TABLE_COUNTERS, 0, 2, (const quint8 *)c));

    engine_tick(10);

    // The unlimited counter keeps the configured seed; the limited one clamps
    // it to its ceiling exactly as before.
    CHECK(qAbs(engine_signal_value(kUnlimited) - 100.0f) < 0.001f);
    CHECK(qAbs(engine_signal_value(kLimited) - 50.0f) < 0.001f);

    engine_clear_config();
}

// Device OnTime is the first value the DEVICE produces about itself rather than
// deriving from the bus, so what needs proving is the plumbing: that a
// destination reaches the engine, that the published number is seconds since
// boot quantised to 0.01, and â€” the part that is easy to get wrong â€” that the
// clock is not configuration and therefore survives a config clear.
// The firmware-side counterpart of ct::unusedDeviceChannels(), and it exists for
// exactly the same reason: `::DeviceChannelsConfig d{}` ZERO-fills, slot 0 is a
// valid destination, and a zero-filled config therefore asks the device to
// publish 28 channels into signal 0 rather than none of them. Every test here
// builds its config through this.
static ::DeviceChannelsConfig unusedDeviceChannelsFw()
{
    ::DeviceChannelsConfig c;
    for (int i = 0; i < DEVCH_COUNT; ++i)
        c.signal_idx[i] = ct::SIG_MSG_NONE; // the firmware's own macro is #undef'd above
    return c;
}

static void testDeviceOnTime()
{
    EngineCallbacks cb{};
    cb.transmit_can = captureTransmit;
    engine_init(&cb);
    engine_clear_config();

    // Nothing published until a configuration asks for it.
    CHECK(engine_device_channels()->signal_idx[DEVCH_ONTIME] == ct::SIG_MSG_NONE);

    constexpr quint16 kSlot = 5;
    // unusedDeviceChannels(), never `DeviceChannelsConfig dc{}` — a zero-filled
    // config points all 28 channels at signal 0, which is a VALID slot, and the
    // device would then overwrite it 100 times a second. The factory exists for
    // this and the test uses it for the same reason production code does.
    DeviceChannelsConfig dc = unusedDeviceChannelsFw();
    dc.signal_idx[DEVCH_ONTIME] = kSlot;
    engine_set_device_channels(&dc);
    CHECK(engine_device_channels()->signal_idx[DEVCH_ONTIME] == kSlot);

    // Deltas, not absolutes: engine_init deliberately does NOT zero the clock â€”
    // it is device on-time, and re-initialising the engine is not a power
    // cycle â€” so by now it already carries whatever earlier tests ticked.
    engine_tick(10);
    const float base = engine_signal_value(kSlot);
    CHECK(base >= 0.0f);

    // 250 ticks of 10 ms is 2.50 s exactly. Exactly, not approximately: the
    // clock is integer milliseconds and the published value is hundredths, so
    // there is no rounding to absorb here.
    for (int i = 0; i < 250; ++i)
        engine_tick(10);
    CHECK(qAbs(engine_signal_value(kSlot) - (base + 2.50f)) < 0.0001f);

    // Quantised to hundredths rather than carrying milliseconds: five more
    // ticks is 50 ms, landing exactly on +2.55 rather than somewhere near it.
    for (int i = 0; i < 5; ++i)
        engine_tick(10);
    CHECK(qAbs(engine_signal_value(kSlot) - (base + 2.55f)) < 0.0001f);
    const float beforeClear = engine_signal_value(kSlot);

    // Clearing the configuration drops the DESTINATION but not the clock: this
    // is device on-time, and a Send is not a power cycle. Re-point it and the
    // reading must carry on from where it was, never restart at zero.
    engine_clear_config();
    CHECK(engine_device_channels()->signal_idx[DEVCH_ONTIME] == ct::SIG_MSG_NONE);
    engine_set_device_channels(&dc);
    engine_tick(10);
    CHECK(qAbs(engine_signal_value(kSlot) - (beforeClear + 0.01f)) < 0.0001f);

    // A destination the engine cannot honour is stored as unused rather than
    // kept and re-checked every tick.
    DeviceChannelsConfig bad = unusedDeviceChannelsFw();
    bad.signal_idx[DEVCH_ONTIME] = quint16(ct::MAX_SIGNALS + 10);
    engine_set_device_channels(&bad);
    CHECK(engine_device_channels()->signal_idx[DEVCH_ONTIME] == ct::SIG_MSG_NONE);
    engine_tick(10); // must not write anywhere, and must not crash

    engine_set_device_channels(nullptr);
    CHECK(engine_device_channels()->signal_idx[DEVCH_ONTIME] == ct::SIG_MSG_NONE);
    engine_clear_config();
}

// A monitor stream that loses frames says so.
//
// It is best-effort by nature — it shares the serial link with everything else,
// and a bus busier than the link can describe will overrun it. The requirement
// is not that it never drops but that it never drops SILENTLY, and it used to:
// the transport's overflow branch held a comment saying an error should be
// logged and nothing else, so a full buffer discarded the frame and told nobody.
// A trace missing half its frames without saying so invites exactly the wrong
// conclusion — that a message was never sent, when it was only never reported.
// The flags byte from a single COBS-framed monitor packet, or -1 if the wire
// does not hold exactly one. Decoded through the real framing rather than by
// scanning for a byte, so the test is checking the format it claims to.
static int monitorFlags(const QByteArray &wire)
{
    QByteArray w = wire;
    while (w.startsWith('\0'))
        w.remove(0, 1);
    while (w.endsWith('\0'))
        w.chop(1);
    const QByteArray dec = ct::cobsDecode(w);
    // PacketHeader is 4 bytes; flags sits 10 bytes into MonitorStreamPayload,
    // after timestamp_ms, bus_idx, direction and can_id.
    if (dec.size() < 4 + 11 || quint8(dec[1]) != ct::CMD_MONITOR_STREAM)
        return -1;
    return quint8(dec[4 + 10]);
}

static bool monitorFlagsSaysGap(const QByteArray &wire)
{
    const int f = monitorFlags(wire);
    return f >= 0 && (f & ct::MONFLAG_GAP) != 0;
}

static void testMonitorGapMarking()
{
    SerialProtoCallbacks cb{};
    cb.send_bytes = captureBytes;
    cb.uptime_ms = fakeUptime;
    serial_proto_init(&cb);

    const auto sendOne = [](quint32 id) {
        const uint8_t data[4] = {1, 2, 3, 4};
        serial_proto_stream_monitor(1, 1, id, 0, 0, 0, 0, data, sizeof(data));
    };

    // A frame that gets through cleanly carries no gap.
    g_wireRefuse = false;
    g_wire.clear();
    sendOne(0x100);
    CHECK(!g_wire.isEmpty());
    CHECK(!monitorFlagsSaysGap(g_wire));

    // Frames refused by a full transport produce nothing on the wire at all —
    // they are lost, which is the situation being reported, not hidden.
    g_wireRefuse = true;
    g_wire.clear();
    sendOne(0x101);
    sendOne(0x102);
    sendOne(0x103);
    CHECK(g_wire.isEmpty());

    // The first frame to get through afterwards carries the gap, so the hole is
    // marked where it happened rather than left for the reader to not notice.
    g_wireRefuse = false;
    g_wire.clear();
    sendOne(0x104);
    CHECK(!g_wire.isEmpty());
    CHECK(monitorFlagsSaysGap(g_wire));

    // And only that one: the gap is an event, not a mode.
    g_wire.clear();
    sendOne(0x105);
    CHECK(!monitorFlagsSaysGap(g_wire));

    // The flag must not displace the real ones. An extended FD frame still
    // reports extended and FD alongside the gap.
    g_wireRefuse = true;
    sendOne(0x106);
    g_wireRefuse = false;
    g_wire.clear();
    {
        const uint8_t data[8] = {0};
        serial_proto_stream_monitor(1, 1, 0x107, 1 /*ext*/, 1 /*fd*/, 1 /*brs*/, 0,
                                    data, sizeof(data));
    }
    const int flags = monitorFlags(g_wire);
    CHECK(flags >= 0);
    CHECK((flags & ct::MONFLAG_GAP) != 0);
    CHECK((flags & ct::MONFLAG_EXTENDED) != 0);
    CHECK((flags & ct::MONFLAG_FD) != 0);
    CHECK((flags & ct::MONFLAG_BRS) != 0);
}

// Messages sharing a period do not all fire on the same tick.
//
// Every period accumulator starting at zero means a table of same-rate messages
// comes due together: the whole table is composed in one tick and the bus is
// idle for the rest of the period. That is the shape that forced the transmit
// rings up, and — the part no buffer can fix — it makes every message behind
// the first late by however long the ones ahead take to reach the wire. 500
// eight-byte frames need 65 ms of a 1 Mbit/s bus, so the last one is 65 ms off
// its schedule however deep the queue is.
//
// Twenty messages at 100 ms should therefore land two per tick across the ten
// ticks the period contains, not twenty on one tick and nothing for nine.
static void testTxPhaseSpreading()
{
    EngineCallbacks cb{};
    cb.transmit_can = budgetedTransmit;
    engine_init(&cb);
    engine_clear_config();

    constexpr int kMsgs = 20;
    constexpr int kTicksPerPeriod = 10; // 100 ms at a 10 ms tick
    ct::CanMessageConfig msgs[kMsgs]{};
    for (int i = 0; i < kMsgs; ++i) {
        msgs[i].can_id = quint32(0x200 + i);
        msgs[i].flags = ct::MSGFLAG_ACTIVE | ct::MSGFLAG_TRANSMIT;
        msgs[i].src_bus = 1;
        msgs[i].dlc = 8;
        msgs[i].period_ms = 100;
    }
    CHECK(engine_table_write(ENGINE_TABLE_MESSAGES, 0, kMsgs,
                             reinterpret_cast<const uint8_t *>(msgs)));

    // One full period, counting how many frames each tick produced.
    QList<int> perTick;
    for (int t = 0; t < kTicksPerPeriod; ++t) {
        g_txBudget = 1000; // accept everything: this measures WHEN, not whether
        g_txAccepted.clear();
        engine_tick(10);
        perTick.append(g_txAccepted.size());
    }

    int total = 0, worst = 0;
    for (int n : std::as_const(perTick)) {
        total += n;
        worst = std::max(worst, n);
    }
    // Every message went exactly once over the period — spreading must not cost
    // or duplicate transmissions, only move them.
    CHECK(total == kMsgs);
    // And no tick carried more than a small share. Without spreading this is 20
    // on one tick, which is what the assertion is really guarding.
    CHECK(worst <= kMsgs / kTicksPerPeriod + 1);
    // Which means no tick was empty either, for this even a split.
    int empty = 0;
    for (int n : std::as_const(perTick))
        if (n == 0)
            ++empty;
    CHECK(empty == 0);

    // Still exactly one transmission per message per period, second time round:
    // the offsets have to persist, not decay back into alignment.
    g_txAccepted.clear();
    for (int t = 0; t < kTicksPerPeriod; ++t) {
        g_txBudget = 1000;
        engine_tick(10);
    }
    CHECK(g_txAccepted.size() == kMsgs);
    QSet<quint32> distinct;
    for (quint32 id : std::as_const(g_txAccepted))
        distinct.insert(id);
    CHECK(distinct.size() == kMsgs);

    // ORDER, which is the half a spread can silently cost. The section order in
    // a document is a design decision — someone numbering sections 1..N with
    // ascending ids expects them on the wire that way, and a receiver may depend
    // on it. The first spread staggered by index-modulo-ticks, which solved the
    // burst and interleaved the table to do it: message 0 went out next to
    // message 10. Spreading by POSITION keeps both.
    for (int i = 1; i < g_txAccepted.size(); ++i)
        CHECK(g_txAccepted.at(i) > g_txAccepted.at(i - 1));

    engine_clear_config();
}

// Each bus spreads across the WHOLE period, independently of the others.
//
// Every bus is its own wire with its own ring and its own controller, so the
// spread has to be computed per bus. Counting them together looks equivalent
// and is not: with the buses laid out one after another in the message table —
// which is exactly how mapToDevice emits them, CAN1's sections then CAN2's —
// a shared count hands CAN1 the first half of the period and CAN2 the second.
// Each bus then transmits at twice its average rate for half the period and
// sits idle for the other half, which is the burst the seeding exists to
// prevent, reappearing along the bus axis instead of the time axis.
//
// Ten messages on CAN1 followed by ten on CAN2, all at 100 ms. Each bus must
// put one message on each of the ten ticks. Shared counting gives CAN1 ticks
// 1-5 and CAN2 ticks 6-10, with two per tick and five empty ticks per bus.
static void testTxPhaseSpreadingIsPerBus()
{
    EngineCallbacks cb{};
    cb.transmit_can = budgetedTransmit;
    engine_init(&cb);
    engine_clear_config();

    constexpr int kPerBus = 10;
    constexpr int kTicks = 10; // 100 ms at a 10 ms tick
    ct::CanMessageConfig msgs[2 * kPerBus]{};
    for (int i = 0; i < 2 * kPerBus; ++i) {
        msgs[i].can_id = quint32(0x300 + i);
        msgs[i].flags = ct::MSGFLAG_ACTIVE | ct::MSGFLAG_TRANSMIT;
        // Laid out the way the mapper emits them: all of bus 1, then all of bus 2.
        msgs[i].src_bus = quint8(i < kPerBus ? 1 : 2);
        msgs[i].dlc = 8;
        msgs[i].period_ms = 100;
    }
    CHECK(engine_table_write(ENGINE_TABLE_MESSAGES, 0, 2 * kPerBus,
                             reinterpret_cast<const uint8_t *>(msgs)));

    QList<int> bus1PerTick, bus2PerTick;
    for (int t = 0; t < kTicks; ++t) {
        g_txBudget = 1000;
        g_txAccepted.clear();
        engine_tick(10);
        int b1 = 0, b2 = 0;
        for (quint32 id : std::as_const(g_txAccepted))
            ((id - 0x300) < kPerBus ? b1 : b2)++;
        bus1PerTick.append(b1);
        bus2PerTick.append(b2);
    }

    // Every tick carries exactly one message from EACH bus. This is the
    // assertion that fails on a shared count: it gives 2,2,2,2,2,0,0,0,0,0 for
    // bus 1 and the mirror image for bus 2.
    for (int t = 0; t < kTicks; ++t) {
        CHECK(bus1PerTick.at(t) == 1);
        CHECK(bus2PerTick.at(t) == 1);
    }
    int total = 0;
    for (int t = 0; t < kTicks; ++t)
        total += bus1PerTick.at(t) + bus2PerTick.at(t);
    CHECK(total == 2 * kPerBus); // each message once per period, no more

    engine_clear_config();
}

// An oversubscribed bus degrades FAIRLY, and reports what it actually sent.
//
// The scan used to start at message 0 every tick. When the outgoing queue is
// full that means the same lowest-numbered messages are served every time and
// everything past them is dropped for ever — so a configuration asking for more
// than the wire can carry does not slow down, it goes partly silent, with the
// first few IDs still running at full rate. On a real device that looked like a
// dead configuration rather than an oversubscribed one, and it is invisible
// unless you know which IDs to miss.
//
// Eight messages, all due every tick, against a queue that accepts two per
// tick. Fair sharing gives each message a quarter of the ticks. Starting at 0
// every tick gives messages 0 and 1 everything and the other six nothing, which
// is what this fails on.
static void testTxFairnessUnderSaturation()
{
    EngineCallbacks cb{};
    cb.transmit_can = budgetedTransmit;
    engine_init(&cb);
    engine_clear_config();
    g_txAccepted.clear();

    constexpr int kMsgs = 8;
    constexpr int kBudget = 2;
    constexpr int kTicks = 200;

    ct::CanMessageConfig msgs[kMsgs]{};
    for (int i = 0; i < kMsgs; ++i) {
        msgs[i].can_id = quint32(0x100 + i);
        msgs[i].flags = ct::MSGFLAG_ACTIVE | ct::MSGFLAG_TRANSMIT;
        msgs[i].src_bus = 1;
        msgs[i].dlc = 8;
        msgs[i].period_ms = 10; // due every tick
    }
    CHECK(engine_table_write(ENGINE_TABLE_MESSAGES, 0, kMsgs,
                             reinterpret_cast<const uint8_t *>(msgs)));

    for (int t = 0; t < kTicks; ++t) {
        g_txBudget = kBudget;
        engine_tick(10);
    }

    QHash<quint32, int> sent;
    for (quint32 id : std::as_const(g_txAccepted))
        sent[id]++;

    // Every message got out. This is the assertion that fails without the
    // rotating cursor — six of the eight would sit at zero.
    int lo = kTicks, hi = 0;
    for (int i = 0; i < kMsgs; ++i) {
        const int n = sent.value(quint32(0x100 + i), 0);
        CHECK(n > 0);
        lo = std::min(lo, n);
        hi = std::max(hi, n);
    }
    // And roughly equally: with a budget of 2 over 8 messages each should get
    // about a quarter of the ticks. Checked as a spread rather than a figure —
    // the point is that no message is starved, not that the rotation lands on
    // an exact schedule.
    CHECK(hi - lo <= 2);
    CHECK(lo >= (kTicks * kBudget) / kMsgs - 2);

    // The queue accepted exactly its budget every tick, and not one frame more.
    CHECK(g_txAccepted.size() == kTicks * kBudget);

    // Tx Count reports what went OUT, not what was composed. Counting refused
    // frames would have the device claim throughput it never achieved, on the
    // exact configuration where that number is being read to find out why.
    DeviceChannelsConfig dc = unusedDeviceChannelsFw();
    dc.signal_idx[DEVCH_BUS(0, DEVCH_BUS_TX_COUNT)] = 900;
    engine_set_device_channels(&dc);
    g_txBudget = 0; // nothing more accepted, so the count must stand still
    engine_tick(10);
    const float counted = engine_signal_value(900);
    CHECK(counted == float(kTicks * kBudget));

    engine_clear_config();
}

// A transmit callback standing in for one dead bus among healthy ones: bus 1
// refuses everything (nothing attached, so its ring never drains — an
// error-passive lone transmitter retries forever and completes nothing), the
// other buses accept everything.
static QList<quint32> g_liveBusAccepted;
static bool deadBus1Transmit(uint8_t dest_bus, uint32_t can_id, uint8_t is_extended,
                             uint8_t is_fd, const uint8_t *data, uint8_t len)
{
    Q_UNUSED(is_extended);
    Q_UNUSED(is_fd);
    Q_UNUSED(data);
    Q_UNUSED(len);
    if (dest_bus == 1)
        return false;
    g_liveBusAccepted.append(can_id);
    return true;
}

// A dead bus must not silence the healthy ones. The outgoing queues are per
// bus, so a refusal from bus 1's ring says nothing about bus 2's — but with a
// single shared full-flag the scheduler treated the first refusal as global:
// the fairness cursor parked on the dead bus's message, every subsequent tick
// started there, and composition stopped for the whole table before any
// bus 2 message was tried. Unplugging CAN 1 silenced CAN 2 and CAN 3.
static void testTxDeadBusDoesNotSilenceOthers()
{
    EngineCallbacks cb{};
    cb.transmit_can = deadBus1Transmit;
    engine_init(&cb);
    engine_clear_config();
    g_liveBusAccepted.clear();

    constexpr int kBus2Msgs = 4;
    constexpr int kTicks = 50;

    // The dead bus's message sits FIRST in the table, due every tick — the
    // arrangement that parks the shared cursor on it immediately.
    ct::CanMessageConfig msgs[1 + kBus2Msgs]{};
    msgs[0].can_id = 0x100;
    msgs[0].flags = ct::MSGFLAG_ACTIVE | ct::MSGFLAG_TRANSMIT;
    msgs[0].src_bus = 1;
    msgs[0].dlc = 8;
    msgs[0].period_ms = 10;
    for (int i = 0; i < kBus2Msgs; ++i) {
        msgs[1 + i].can_id = quint32(0x200 + i);
        msgs[1 + i].flags = ct::MSGFLAG_ACTIVE | ct::MSGFLAG_TRANSMIT;
        msgs[1 + i].src_bus = 2;
        msgs[1 + i].dlc = 8;
        msgs[1 + i].period_ms = 10;
    }
    CHECK(engine_table_write(ENGINE_TABLE_MESSAGES, 0, 1 + kBus2Msgs,
                             reinterpret_cast<const uint8_t *>(msgs)));

    for (int t = 0; t < kTicks; ++t)
        engine_tick(10);

    QHash<quint32, int> sent;
    for (quint32 id : std::as_const(g_liveBusAccepted))
        sent[id]++;

    // Every bus 2 message went out on every tick it was due, with bus 1 dead
    // the whole time. This is the assertion that fails with a shared
    // full-flag — all four would sit at zero.
    for (int i = 0; i < kBus2Msgs; ++i)
        CHECK(sent.value(quint32(0x200 + i), 0) == kTicks);

    engine_clear_config();
}

// ---- Triggered transmit: a message that speaks only while a User Condition
// holds, at no more than its own rate.
//
// Driven through the two engine halves SEPARATELY — engine_tick_calc at 10 ms,
// engine_service_transmit at 5 ms — because that is what the device's own glue
// does (user_code.c events_100Hz / events_200Hz) and the whole claim of the
// feature is about the difference between the two rates. Calling engine_tick()
// would drive both at one cadence and prove nothing about the 5 ms check.
//
// The condition's input arrives as a received signal, which is not incidental:
// it is the case a trigger is actually for, and engine_process_can re-evaluates
// conditions on every matching frame, so the value under test moves the way it
// moves in service rather than only on a calculation tick. ----
namespace trig {
constexpr quint16 kSrc = 0, kCondOut = 1;
constexpr quint32 kRxId = 0x300, kTxId = 0x301;
constexpr int kPeriodMs = 1000; // 1 Hz on purpose: 200x the slot the gate is checked in

// One comparison of a Set or Reset expression, against a constant.
static ct::ConditionTerm cmpTerm(quint16 signalIdx, quint8 op, float k)
{
    ct::ConditionTerm t{};
    t.input_a_signal_idx = signalIdx;
    t.op = op;
    t.input_b_type = 0;
    t.b.input_b_const = k;
    return t;
}

// One term that asks whether a message happened. input_a is a MESSAGE index
// here, and b is left zero.
static ct::ConditionTerm msgTerm(quint16 msgIdx, quint8 op)
{
    ct::ConditionTerm t{};
    t.input_a_signal_idx = msgIdx;
    t.op = op;
    return t;
}

// A Set/Reset latch on one comparison each, or a Momentary if latchHz > 0.
static ct::ConditionConfig makeCondition(quint16 dest, const ct::ConditionTerm &set,
                                         const ct::ConditionTerm *reset, quint8 latchHz)
{
    ct::ConditionConfig c{};
    c.flags = ct::CONDFLAG_ACTIVE;
    c.dest_signal_idx = dest;
    c.set_terms[0] = set;
    c.set_count = 1;
    if (reset) {
        c.flags |= ct::CONDFLAG_SETRESET;
        c.reset_terms[0] = *reset;
        c.reset_count = 1;
    }
    c.latch_hz = latchHz;
    return c;
}

// One receive message feeding one 8-bit signal, one transmit message gated on
// condition 0, and the condition itself — a Set/Reset whose Reset is the
// inverse of its Set, which is what a migrated pre-modes condition looks like
// and therefore what a triggered message has always been gated on.
static bool install(const ct::ConditionConfig *condition = nullptr)
{
    ct::CanMessageConfig msgs[2]{};
    msgs[0].can_id = kRxId;
    msgs[0].flags = ct::MSGFLAG_ACTIVE;
    msgs[0].src_bus = 1;
    msgs[0].dlc = 8;
    msgs[0].tx_trigger_cond = ct::TX_TRIGGER_COND_NONE;

    msgs[1].can_id = kTxId;
    msgs[1].flags = ct::MSGFLAG_ACTIVE | ct::MSGFLAG_TRANSMIT;
    msgs[1].src_bus = 1;
    msgs[1].dlc = 8;
    msgs[1].period_ms = kPeriodMs;
    msgs[1].tx_trigger_cond = 0;
    msgs[1].tx_trigger_flags = ct::TXTRIG_ENABLED;

    ct::CanSignalConfig sig[2]{};
    for (auto &s : sig) {
        s.factor = 1.0f;
        s.min_val = -1.0e9f;
        s.max_val = 1.0e9f;
        ct::sigSetBits(s, 0, 8, ct::SIGNAL_TYPE_UINT8, 0, 0);
    }
    ct::sigSetHeader(sig[kSrc], 0, 0, 1); // message 0 (receive), Intel, active
    std::memcpy(sig[kSrc].label, "Src", 4);
    ct::sigSetHeader(sig[kCondOut], ct::SIG_MSG_NONE, 0, 1);
    std::memcpy(sig[kCondOut].label, "Cond", 5);

    // The default is a Set/Reset whose Reset is the inverse of its Set — a
    // migrated pre-modes condition, and what a triggered message has always
    // been gated on. A caller wanting a different shape passes it in rather
    // than writing over slot 0 afterwards: the flash model programs each
    // doubleword once per erase, so an in-place rewrite fails before anything
    // under test can run.
    const ct::ConditionTerm set = cmpTerm(kSrc, ct::COND_OP_GT, 0.5f);
    const ct::ConditionTerm reset = cmpTerm(kSrc, ct::COND_OP_LTE, 0.5f);
    const ct::ConditionConfig fallback = makeCondition(kCondOut, set, &reset, 0);
    const ct::ConditionConfig cond = condition ? *condition : fallback;

    return engine_table_write(ENGINE_TABLE_MESSAGES, 0, 2,
                              reinterpret_cast<const uint8_t *>(msgs))
           && engine_table_write(ENGINE_TABLE_SIGNALS, 0, 2,
                                 reinterpret_cast<const uint8_t *>(sig))
           && engine_table_write(ENGINE_TABLE_CONDITIONS, 0, 1,
                                 reinterpret_cast<const uint8_t *>(&cond));
}

static void arm(bool on)
{
    const uint8_t f[8] = {uint8_t(on ? 1 : 0), 0, 0, 0, 0, 0, 0, 0};
    engine_process_can(1, kRxId, 0, 0, f, 8);
}

// `count` five-millisecond transmit services, with a calculation tick folded in
// every second one so the 10 ms chain keeps running underneath — exactly the
// interleaving events_100Hz and events_200Hz produce.
//
// Not named `slots`: Qt defines that as an empty macro for moc, so a parameter
// by that name vanishes and the loop stops compiling for reasons that read like
// nonsense.
static void run(int count)
{
    for (int i = 0; i < count; ++i) {
        if (i % 2 == 1)
            engine_tick_calc(10);
        engine_service_transmit(5);
    }
}

static int sent()
{
    int n = 0;
    for (const CapturedTx &t : std::as_const(g_txFrames))
        if (t.id == kTxId)
            ++n;
    return n;
}
} // namespace trig

static void testTriggeredTransmit()
{
    EngineCallbacks cb{};
    cb.transmit_can = captureTransmit;
    engine_init(&cb);
    engine_clear_config();
    CHECK(trig::install());

    // ---- Disarmed is SILENT, not slow. Two full periods with the condition
    // false produce nothing at all — the message's own rate never gets a look
    // in, which is the difference between a trigger and a rate limit. ----
    g_txFrames.clear();
    trig::arm(false);
    trig::run(400); // 2,000 ms
    CHECK(trig::sent() == 0);

    // ---- The rising edge transmits AT ONCE, not at the next grid boundary.
    // One 5 ms slot after the condition goes true, the frame is out — for a
    // 1 Hz message. That is the 200 Hz check doing its job. ----
    g_txFrames.clear();
    trig::arm(true);
    trig::run(1);
    CHECK(trig::sent() == 1);

    // ---- Then the rate governs, and the interval is phased from the TRIGGER.
    // 995 ms later there is still exactly one frame; at 1,000 ms there are two.
    // A free-running grid would have fired somewhere inside that window. ----
    trig::run(199); // 995 ms since the frame that the trigger sent
    CHECK(trig::sent() == 1);
    trig::run(1); // 1,000 ms — the period, measured from the trigger
    CHECK(trig::sent() == 2);

    // ---- Dropping the condition stops it, mid-period and permanently. ----
    trig::arm(false);
    g_txFrames.clear();
    trig::run(600); // 3,000 ms
    CHECK(trig::sent() == 0);

    // ---- And re-arming starts a fresh interval rather than resuming the old
    // phase: immediate frame, then one period later. ----
    trig::arm(true);
    trig::run(1);
    CHECK(trig::sent() == 1);
    trig::run(199);
    CHECK(trig::sent() == 1);
    trig::run(1);
    CHECK(trig::sent() == 2);

    // ---- The run continues for as long as the condition holds. ----
    trig::run(1200);
    CHECK(trig::sent() == 8);
    CHECK(engine_signal_value(trig::kCondOut) == 1.0f);

    engine_clear_config();
}

// A Set/Reset condition is a LATCH: it holds its output between edges, and a
// Reset beats a Set that is still true.
//
// The hold is the whole difference from the level a condition used to be, and
// it is what a migrated configuration must NOT accidentally acquire — which is
// why the migration pairs a Set with its exact inverse, so the two can never
// both be false and the latch can never hold anything.
static void testConditionSetResetLatch()
{
    EngineCallbacks cb{};
    cb.transmit_can = captureTransmit;
    engine_init(&cb);
    engine_clear_config();

    // Two independent inputs, so Set and Reset can be driven separately —
    // which a migrated condition, whose Reset is the inverse of its Set, cannot
    // do. Both arrive on one receive message.
    constexpr quint16 kSetIn = 0, kResetIn = 1, kOut = 2;
    ct::CanMessageConfig msg{};
    msg.can_id = trig::kRxId;
    msg.flags = ct::MSGFLAG_ACTIVE;
    msg.src_bus = 1;
    msg.dlc = 8;
    msg.tx_trigger_cond = ct::TX_TRIGGER_COND_NONE;

    ct::CanSignalConfig sig[3]{};
    for (auto &s : sig) {
        s.factor = 1.0f;
        s.min_val = -1.0e9f;
        s.max_val = 1.0e9f;
        ct::sigSetBits(s, 0, 8, ct::SIGNAL_TYPE_UINT8, 0, 0);
    }
    ct::sigSetHeader(sig[kSetIn], 0, 0, 1);
    ct::sigSetBits(sig[kSetIn], 0, 8, ct::SIGNAL_TYPE_UINT8, 0, 0);
    ct::sigSetHeader(sig[kResetIn], 0, 0, 1);
    ct::sigSetBits(sig[kResetIn], 8, 8, ct::SIGNAL_TYPE_UINT8, 0, 0);
    ct::sigSetHeader(sig[kOut], ct::SIG_MSG_NONE, 0, 1);

    const ct::ConditionTerm set = trig::cmpTerm(kSetIn, ct::COND_OP_GT, 0.5f);
    const ct::ConditionTerm reset = trig::cmpTerm(kResetIn, ct::COND_OP_GT, 0.5f);
    const ct::ConditionConfig cond = trig::makeCondition(kOut, set, &reset, 0);

    CHECK(engine_table_write(ENGINE_TABLE_MESSAGES, 0, 1,
                             reinterpret_cast<const uint8_t *>(&msg)));
    CHECK(engine_table_write(ENGINE_TABLE_SIGNALS, 0, 3,
                             reinterpret_cast<const uint8_t *>(sig)));
    CHECK(engine_table_write(ENGINE_TABLE_CONDITIONS, 0, 1,
                             reinterpret_cast<const uint8_t *>(&cond)));

    const auto feed = [](int setIn, int resetIn) {
        const uint8_t f[8] = {uint8_t(setIn), uint8_t(resetIn), 0, 0, 0, 0, 0, 0};
        engine_process_can(1, trig::kRxId, 0, 0, f, 8);
    };

    feed(0, 0);
    CHECK(engine_signal_value(kOut) == 0.0f);
    feed(1, 0); // set
    CHECK(engine_signal_value(kOut) == 1.0f);
    // THE LATCH: Set goes away and the output STAYS. A level would have dropped.
    feed(0, 0);
    CHECK(engine_signal_value(kOut) == 1.0f);
    engine_tick_calc(10); // and it survives a calculation pass too
    CHECK(engine_signal_value(kOut) == 1.0f);
    feed(0, 1); // reset
    CHECK(engine_signal_value(kOut) == 0.0f);
    feed(0, 0); // and stays reset
    CHECK(engine_signal_value(kOut) == 0.0f);

    // RESET IS DOMINANT: both true, output 0, whichever order they arrived in.
    feed(1, 0);
    CHECK(engine_signal_value(kOut) == 1.0f);
    feed(1, 1);
    CHECK(engine_signal_value(kOut) == 0.0f);
    feed(1, 1);
    CHECK(engine_signal_value(kOut) == 0.0f);
    // Dropping the Reset with the Set still true sets it again — the Set is a
    // level, not an edge, on this side.
    feed(1, 0);
    CHECK(engine_signal_value(kOut) == 1.0f);

    engine_clear_config();
}

// A Momentary condition pulses: the rising edge of Set drives the output to 1
// and it drops on its own one period of latch_hz later, whatever Set does in
// between.
static void testConditionMomentaryHold()
{
    EngineCallbacks cb{};
    cb.transmit_can = captureTransmit;
    engine_init(&cb);
    engine_clear_config();
    // A Momentary on the same input. 10 Hz, so the hold is 100 ms — ten
    // calculation passes.
    const ct::ConditionTerm set = trig::cmpTerm(trig::kSrc, ct::COND_OP_GT, 0.5f);
    const ct::ConditionConfig mom = trig::makeCondition(trig::kCondOut, set, nullptr, 10);
    CHECK(trig::install(&mom));

    trig::arm(false);
    engine_tick_calc(10);
    CHECK(engine_signal_value(trig::kCondOut) == 0.0f);

    // Rising edge: high at once.
    trig::arm(true);
    CHECK(engine_signal_value(trig::kCondOut) == 1.0f);

    // Still high 90 ms in, with the input HELD — the hold is a duration, not a
    // follower, so nothing about the input matters until it goes false and true
    // again.
    for (int i = 0; i < 9; ++i)
        engine_tick_calc(10);
    CHECK(engine_signal_value(trig::kCondOut) == 1.0f);
    // 100 ms: dropped by itself, while the Set expression is STILL TRUE. This
    // is the assertion that separates Momentary from every other mode.
    engine_tick_calc(10);
    CHECK(engine_signal_value(trig::kCondOut) == 0.0f);
    for (int i = 0; i < 50; ++i)
        engine_tick_calc(10);
    CHECK(engine_signal_value(trig::kCondOut) == 0.0f);

    // Re-arming needs a real edge: false, then true.
    trig::arm(false);
    engine_tick_calc(10);
    trig::arm(true);
    CHECK(engine_signal_value(trig::kCondOut) == 1.0f);

    // RETRIGGER RELOADS rather than extends. Half way through the hold, a fresh
    // edge restarts the full 100 ms — so it is high 90 ms after the SECOND
    // edge, which it would not be if the remainder had merely been topped up.
    for (int i = 0; i < 5; ++i)
        engine_tick_calc(10); // 50 ms in
    trig::arm(false);
    trig::arm(true); // fresh edge, no time passing
    for (int i = 0; i < 9; ++i)
        engine_tick_calc(10); // 90 ms after it
    CHECK(engine_signal_value(trig::kCondOut) == 1.0f);
    engine_tick_calc(10);
    CHECK(engine_signal_value(trig::kCondOut) == 0.0f);

    // A receive pass carries no time, so bus traffic cannot shorten a hold.
    trig::arm(false);
    engine_tick_calc(10);
    trig::arm(true);
    for (int i = 0; i < 200; ++i)
        trig::arm(true); // 200 receive passes, zero elapsed
    CHECK(engine_signal_value(trig::kCondOut) == 1.0f);

    engine_clear_config();
}

// "was received" and "was transmitted": true only on the pass the frame
// actually happened.
static void testConditionMessageEvents()
{
    EngineCallbacks cb{};
    cb.transmit_can = captureTransmit;
    engine_init(&cb);
    engine_clear_config();
    // Condition 0 latches on "message 0 was received" and clears on "message 1
    // was transmitted" — the request/response pattern the operators exist for,
    // and the reason the transmit-side tickbox could be retired.
    const ct::ConditionTerm set = trig::msgTerm(0, ct::COND_OP_MSG_RX);
    const ct::ConditionTerm reset = trig::msgTerm(1, ct::COND_OP_MSG_TX);
    const ct::ConditionConfig cond = trig::makeCondition(trig::kCondOut, set, &reset, 0);
    CHECK(trig::install(&cond));

    g_txFrames.clear();
    engine_tick_calc(10);
    CHECK(engine_signal_value(trig::kCondOut) == 0.0f);

    // A frame for message 0 arrives. The event is recorded and evaluated in the
    // same call, so the latch sets immediately.
    trig::arm(false); // any frame for kRxId; the payload is irrelevant here
    CHECK(engine_signal_value(trig::kCondOut) == 1.0f);

    // The event is spent. A calculation pass with no new frame does not re-set
    // it — but the LATCH holds, which is the point.
    engine_tick_calc(10);
    CHECK(engine_signal_value(trig::kCondOut) == 1.0f);

    // The gated message goes out, which fires the Reset on the NEXT evaluation
    // — the transmit service does not evaluate conditions itself.
    trig::run(1);
    CHECK(trig::sent() == 1);
    engine_tick_calc(10);
    CHECK(engine_signal_value(trig::kCondOut) == 0.0f);

    // And it stays clear until the next request arrives.
    for (int i = 0; i < 20; ++i)
        engine_tick_calc(10);
    CHECK(engine_signal_value(trig::kCondOut) == 0.0f);
    trig::arm(false);
    CHECK(engine_signal_value(trig::kCondOut) == 1.0f);

    // Clear it again, which takes a WHOLE PERIOD of transmit services and not
    // one — the message is 1 Hz, its accumulator was zeroed by the send above,
    // and calculation ticks do not advance the transmit scheduler. The reset
    // arrives when the message actually goes out, not when the latch would
    // like it to.
    trig::run(200); // 1,000 ms
    CHECK(trig::sent() == 2);
    engine_tick_calc(10);
    CHECK(engine_signal_value(trig::kCondOut) == 0.0f);

    // A frame matching no receive message produces no event at all: the receive
    // scan returns before anything is recorded, so there is nothing for a Set
    // to see.
    const uint8_t f[8] = {0};
    engine_process_can(1, 0x7FE, 0, 0, f, 8); // an ID no message claims
    CHECK(engine_signal_value(trig::kCondOut) == 0.0f);

    engine_clear_config();
}

// A BATCH compound message emits EVERY variant before its transmit event fires,
// so a condition reset on "was transmitted" clears after the whole batch and not
// after the first frame.
//
// That ordering is the whole point of pairing a batch message with a Set/Reset:
// set on the request arriving, send every variant, reset on the send. If the
// event were recorded per frame instead of per message, the reset would land
// after variant 1 and variants 2 and 3 would go out with the condition already
// clear — or not go out at all.
//
// Two DISTINCT selector values, because that is what makes two variants. Three
// identifiers all claiming selector 0 collapse into one, which is a
// configuration the host now refuses rather than a shape the device can honour.
static void testBatchCompoundEmitsAllVariantsBeforeItsEvent()
{
    EngineCallbacks cb{};
    cb.transmit_can = captureTransmit;
    engine_init(&cb);
    engine_clear_config();

    constexpr quint16 kSrc = 0, kCondOut = 1, kVarA = 2, kVarB = 3;

    ct::CanMessageConfig msgs[2]{};
    msgs[0].can_id = trig::kRxId;
    msgs[0].flags = ct::MSGFLAG_ACTIVE;
    msgs[0].src_bus = 1;
    msgs[0].dlc = 8;
    msgs[0].tx_trigger_cond = ct::TX_TRIGGER_COND_NONE;
    // Compound, BATCH (no MSGFLAG_TX_SEQUENTIAL), triggered on condition 0.
    msgs[1].can_id = trig::kTxId;
    msgs[1].flags = ct::MSGFLAG_ACTIVE | ct::MSGFLAG_TRANSMIT;
    msgs[1].src_bus = 1;
    msgs[1].dlc = 8;
    msgs[1].period_ms = trig::kPeriodMs;
    msgs[1].tx_trigger_cond = 0;
    msgs[1].tx_trigger_flags = ct::TXTRIG_ENABLED;

    ct::CanSignalConfig sig[4]{};
    for (auto &s : sig) {
        s.factor = 1.0f;
        s.min_val = -1.0e9f;
        s.max_val = 1.0e9f;
    }
    ct::sigSetHeader(sig[kSrc], 0, 0, 1);
    ct::sigSetBits(sig[kSrc], 0, 8, ct::SIGNAL_TYPE_UINT8, 0, 0);
    ct::sigSetHeader(sig[kCondOut], ct::SIG_MSG_NONE, 0, 1);
    ct::sigSetBits(sig[kCondOut], 0, 8, ct::SIGNAL_TYPE_UINT8, 0, 0);
    // The two variants: same message, one selector byte at 0, data clear of it.
    for (quint16 v : {kVarA, kVarB}) {
        ct::sigSetHeader(sig[v], 1, 0, 1); // message 1 (the transmit one)
        ct::sigSetBits(sig[v], 8, 8, ct::SIGNAL_TYPE_UINT8, 0, 0); // byte 1, selector at 0
        sig[v].mux_mask = 0xFF;
    }
    sig[kVarA].mux_id = 1;
    sig[kVarB].mux_id = 2;

    const ct::ConditionTerm set = trig::msgTerm(0, ct::COND_OP_MSG_RX);
    const ct::ConditionTerm reset = trig::msgTerm(1, ct::COND_OP_MSG_TX);
    const ct::ConditionConfig cond = trig::makeCondition(kCondOut, set, &reset, 0);

    CHECK(engine_table_write(ENGINE_TABLE_MESSAGES, 0, 2,
                             reinterpret_cast<const uint8_t *>(msgs)));
    CHECK(engine_table_write(ENGINE_TABLE_SIGNALS, 0, 4,
                             reinterpret_cast<const uint8_t *>(sig)));
    CHECK(engine_table_write(ENGINE_TABLE_CONDITIONS, 0, 1,
                             reinterpret_cast<const uint8_t *>(&cond)));

    g_txFrames.clear();
    engine_tick_calc(10);
    CHECK(engine_signal_value(kCondOut) == 0.0f);

    // The request arrives: the latch sets in the same pass.
    trig::arm(true);
    CHECK(engine_signal_value(kCondOut) == 1.0f);

    // One transmit service sends the WHOLE batch — both variants, one call —
    // and the condition is still set while they go, because the event has not
    // been evaluated yet.
    trig::run(1);
    CHECK(trig::sent() == 2);
    CHECK(engine_signal_value(kCondOut) == 1.0f);

    // The next evaluation sees the transmit and clears it.
    engine_tick_calc(10);
    CHECK(engine_signal_value(kCondOut) == 0.0f);

    // And nothing more goes out until the next request, however long we wait —
    // the batch was one event, not two.
    trig::run(600); // 3,000 ms, three periods
    CHECK(trig::sent() == 2);

    engine_clear_config();
}

// A trigger that names nothing usable makes the message SILENT, never cyclic.
//
// The direction matters more than the mechanism. Falling back to "no gate"
// would put a message on a customer's bus at full rate precisely when the
// configuration has stopped making sense, and a stream of unexpected frames is
// a far worse failure than a message that does not appear.
static void testTriggeredTransmitBrokenReferenceIsSilent()
{
    EngineCallbacks cb{};
    cb.transmit_can = captureTransmit;
    engine_init(&cb);

    const auto runWith = [](quint16 condIdx, bool presentButInactive) {
        engine_clear_config();
        ct::CanMessageConfig msg{};
        msg.can_id = trig::kTxId;
        msg.flags = ct::MSGFLAG_ACTIVE | ct::MSGFLAG_TRANSMIT;
        msg.src_bus = 1;
        msg.dlc = 8;
        msg.period_ms = 100;
        msg.tx_trigger_cond = condIdx;
        msg.tx_trigger_flags = ct::TXTRIG_ENABLED;
        CHECK(engine_table_write(ENGINE_TABLE_MESSAGES, 0, 1,
                                 reinterpret_cast<const uint8_t *>(&msg)));
        if (presentButInactive) {
            ct::CanSignalConfig sig{};
            sig.factor = 1.0f;
            sig.min_val = -1.0e9f;
            sig.max_val = 1.0e9f;
            ct::sigSetHeader(sig, ct::SIG_MSG_NONE, 0, 1);
            CHECK(engine_table_write(ENGINE_TABLE_SIGNALS, 0, 1,
                                     reinterpret_cast<const uint8_t *>(&sig)));
            // Present, and its Set would be true (0 > -1), but not active.
            ct::ConditionConfig cond =
                trig::makeCondition(0, trig::cmpTerm(0, ct::COND_OP_GT, -1.0f), nullptr, 10);
            cond.flags &= quint8(~ct::CONDFLAG_ACTIVE);
            CHECK(engine_table_write(ENGINE_TABLE_CONDITIONS, 0, 1,
                                     reinterpret_cast<const uint8_t *>(&cond)));
        }
        g_txFrames.clear();
        trig::run(400); // 2,000 ms — twenty periods' worth
        return trig::sent();
    };

    CHECK(runWith(ct::TX_TRIGGER_COND_NONE, false) == 0); // the unset sentinel
    CHECK(runWith(7, false) == 0);                        // past the end of an empty table
    CHECK(runWith(0, true) == 0);                         // present, but inactive

    engine_clear_config();
}

// Give a condition the Reset that makes it behave like the level these tests
// were written against: the exact inverse of its Set, which is also what the
// migration writes into every configuration loaded from an older file.
static void giveInverseReset(ct::ConditionRow &c)
{
    CHECK(ct::invertConditionExpr(c.setTerms, c.setJoiners, &c.resetTerms, &c.resetJoiners));
}

// Reference CRC-8, written here independently of the engine's implementation
// so the two can disagree. Anchored below against PUBLISHED catalogue check
// values (crc("123456789")) before it is trusted to judge anything: an
// engine tested only against a reference that shares its bugs proves nothing.
static quint8 crc8Ref(const QByteArray &bytes, quint8 poly, quint8 init,
                      bool refIn, bool refOut, quint8 xorOut)
{
    const auto reflect = [](quint8 b) {
        quint8 r = 0;
        for (int i = 0; i < 8; ++i)
            if (b & (1u << i))
                r |= quint8(1u << (7 - i));
        return r;
    };
    quint8 reg = init;
    for (const char c : bytes) {
        quint8 in = quint8(c);
        if (refIn)
            in = reflect(in);
        reg ^= in;
        for (int i = 0; i < 8; ++i)
            reg = (reg & 0x80u) ? quint8(quint8(reg << 1) ^ poly) : quint8(reg << 1);
    }
    if (refOut)
        reg = reflect(reg);
    return reg ^ xorOut;
}

// The Transmit CRC8 stamp, end to end through the real composer: golden
// catalogue values for the algorithm, frame bytes for the ORDERING (the CRC
// must cover the bytes the channels actually packed, which is only true if
// the stamp runs last), ID elements for the identifier path, and the
// published channel for the monitor's view of it.
// Clamp vs roll-over on the way out, driven through the real composer.
//
// Six 8-bit fields in one frame, three clamping and three rolling over, fed by
// constants that each sit OUTSIDE what the field can carry. Byte for byte, the
// frame is the whole contract:
//
//   byte 0  clamp,  unsigned, 256      -> 255   (the biggest 8 bits hold)
//   byte 1  roll,   unsigned, 256      -> 0     (256 & 0xFF)
//   byte 2  clamp,  unsigned, -1       -> 0     (the smallest)
//   byte 3  roll,   unsigned, -1       -> 255   (two's complement, truncated)
//   byte 4  clamp,  signed,   200      -> 127   (0x7F)
//   byte 5  roll,   signed,   200      -> 200   (0xC8, which reads back as -56)
//
// Byte 6 is the case the flag exists for and the one a field-width-only
// implementation would get wrong: a channel RANGED 0..255 carrying 300. The
// physical clamp runs first and would pin it to 255 before the field width was
// ever consulted, so a rolling row has to skip that clamp too or the roll-over
// never happens. It must read 300 & 0xFF = 44.
//
// Byte 7 is byte 6's mirror and guards the other direction of the same edit: a
// CLAMPING row on a channel ranged 0..100 carrying 300. Here the channel's
// range is the thing that decides — 100, not the 255 the field could hold — so
// deleting the physical clamp outright, rather than merely making it
// conditional, fails here while byte 6 still passes. The two together pin the
// clamp to exactly the rows that asked for it.
//
// Byte 8 proves the resolution is applied before the truncation, not after:
// 30.0 at 0.1 per count is raw 300, so it must also read 44 — not 30, and not
// the 255 a clamping row would send.
//
// Bytes 9 and 10 drive the int64/NaN guard, which nothing else reaches. A
// factor of 1e-30 sends the quotient far past int64, where llround would be
// undefined; the guard saturates at the largest double below 2^63, whose low
// bits are zero, so a rolling row sends 0 and a clamping one sends 255. Byte 11
// feeds a NaN through a constant: it resolves to 0 rather than to whatever a
// conversion would have produced.
// A compound variant with no channels of its own still goes out.
//
// The device works out which variants a compound message has by walking its
// SIGNALS — the identifier list is nowhere else — so an identifier with nothing
// bound to it used to imply nothing and never reached the bus. A selector-only
// signal declares it and packs nothing, which is the request/ping frame shape:
// the ID byte IS the message.
//
// The frame is laid out so that "packs nothing" is FALSIFIABLE rather than
// merely true-looking. Byte 1 holds an ALWAYS-PRESENT signal (mux_mask 0, in
// every variant) carrying 0x77, and the two selector-only signals declare that
// same byte 1 while sitting LATER in the table. Signals pack in table order and
// the last writer wins, so a composer that packed a selector-only signal would
// write its own zero over the 0x77 — byte 1 would read 0x00 in variants 1 and
// 3. Expecting 0x77 there is what makes the skip observable.
// The transmit offset ADDS, and lands in raw counts.
//
// Reported from the bench: a channel at 1, resolution 1, offset 64, signed
// 16-bit, sent 0xFFC1 (-63) where 0x0041 (65) was wanted. That was the
// algebraic inverse of the receive path — (physical - offset) / resolution —
// which is correct arithmetic and the wrong answer to what the offset FIELD is
// asking. It now reads raw = physical / resolution + offset.
//
// Four 16-bit signed fields, all fed from one channel holding 1:
//
//   bytes 0-1  res 1,   offset  64, roll over  -> 65   0x0041  (the report)
//   bytes 2-3  res 1,   offset -64, roll over  -> -63  0xFFC1  (its mirror)
//   bytes 4-5  res 0.1, offset  64, roll over  -> 74   0x004A  (order: after)
//   bytes 6-7  res 1,   offset  64, CLAMPED    -> 65   0x0041  (clamp agrees)
//
// The third field is the one that fixes the ORDER and cannot be got right by
// accident: after the resolution it is 1/0.1 + 64 = 74, where offsetting first
// would be (1 + 64)/0.1 = 650 and the old inverse would be (1 - 64)/0.1 = -630.
// Three distinct answers, so the assertion picks exactly one convention.
static void testTransmitOffsetAddsOnTheWayOut()
{
    EngineCallbacks cb{};
    cb.transmit_can = captureTransmit;
    engine_init(&cb);
    engine_clear_config();
    g_txFrames.clear();

    ct::CanMessageConfig msg{};
    msg.can_id = 0x460;
    msg.flags = ct::MSGFLAG_ACTIVE | ct::MSGFLAG_TRANSMIT;
    msg.src_bus = 1;
    msg.dlc = 8;
    msg.period_ms = 10;
    msg.tx_trigger_cond = ct::TX_TRIGGER_COND_NONE;
    CHECK(engine_table_write(ENGINE_TABLE_MESSAGES, 0, 1,
                             reinterpret_cast<const uint8_t *>(&msg)));

    struct Field { float res, off; bool wrap; int expect; };
    const Field fields[4] = {
        {1.0f,  64.0f, true,   65},
        {1.0f, -64.0f, true,  -63},
        {0.1f,  64.0f, true,   74},
        {1.0f,  64.0f, false,  65},
    };

    ct::CanSignalConfig sig[5]{};
    for (int i = 0; i < 4; ++i) {
        sig[i].factor = fields[i].res;
        sig[i].offset = fields[i].off;
        sig[i].min_val = -1.0e9f;
        sig[i].max_val = 1.0e9f;
        ct::sigSetTxWrap(sig[i], fields[i].wrap);
        ct::sigSetHeader(sig[i], 0, 0, 1); // Intel byte order, active
        ct::sigSetBits(sig[i], quint16(i * 16), 16, ct::SIGNAL_TYPE_INT16, 0, 0);
        sig[i].tx_source = 5; // the value slot below, encoded +1
    }
    // The channel every field sends: a plain 1.
    sig[4].factor = 1.0f;
    sig[4].min_val = -1.0e9f;
    sig[4].max_val = 1.0e9f;
    ct::sigSetHeader(sig[4], ct::SIG_MSG_NONE, 0, 1);
    std::memcpy(sig[4].label, "Value", 6);
    CHECK(engine_table_write(ENGINE_TABLE_SIGNALS, 0, 5,
                             reinterpret_cast<const uint8_t *>(sig)));

    ct::ConstantConfig k{};
    k.dest_signal_idx = 4;
    k.value = 1.0f;
    k.is_active = 1;
    CHECK(engine_table_write(ENGINE_TABLE_CONSTANTS, 0, 1,
                             reinterpret_cast<const uint8_t *>(&k)));

    engine_tick(10);
    CHECK(g_txFrames.size() == 1);
    const CapturedTx &tx = g_txFrames.first();
    CHECK(tx.len == 8);
    for (int i = 0; i < 4; ++i) {
        // Intel: low byte first, so the pair reassembles little-endian.
        const int raw = int(qint16(quint16(tx.data[i * 2]) | (quint16(tx.data[i * 2 + 1]) << 8)));
        CHECK(raw == fields[i].expect);
    }
    // Spelt out for the two the report named, so a failure says which is wrong
    // rather than only that one of four is.
    CHECK(tx.data[0] == 0x41 && tx.data[1] == 0x00); // offset  64 -> 0x0041
    CHECK(tx.data[2] == 0xC1 && tx.data[3] == 0xFF); // offset -64 -> 0xFFC1
}

static void testSelectorOnlyVariantsAreTransmitted()
{
    EngineCallbacks cb{};
    cb.transmit_can = captureTransmit;
    engine_init(&cb);
    engine_clear_config();
    g_txFrames.clear();

    ct::CanMessageConfig msg{};
    msg.can_id = 0x440;
    msg.flags = ct::MSGFLAG_ACTIVE | ct::MSGFLAG_TRANSMIT; // batch (not SEQUENTIAL)
    msg.src_bus = 1;
    msg.dlc = 8;
    msg.period_ms = 10;
    msg.tx_trigger_cond = ct::TX_TRIGGER_COND_NONE;
    CHECK(engine_table_write(ENGINE_TABLE_MESSAGES, 0, 1,
                             reinterpret_cast<const uint8_t *>(&msg)));

    // 0: always-present (mask 0) in byte 1, value 0x77 — in every variant.
    // 1: selector-only, id 1, DECLARING byte 1, later in the table than 0.
    // 2: a real gated channel, id 2, byte 2, value 0xAB.
    // 3: selector-only, id 3, also declaring byte 1.
    ct::CanSignalConfig sig[4]{};
    for (auto &g : sig) {
        g.factor = 1.0f;
        g.min_val = -1.0e9f;
        g.max_val = 1.0e9f;
        ct::sigSetHeader(g, 0, 0, 1);
    }
    ct::sigSetBits(sig[0], 8, 8, ct::SIGNAL_TYPE_UINT8, 0, 0);
    sig[0].mux_mask = 0; // always present
    sig[0].tx_source = 5; // constant slot 4, encoded +1

    ct::sigSetSelectorOnly(sig[1], true);
    ct::sigSetBits(sig[1], 8, 8, ct::SIGNAL_TYPE_UINT8, 0, 0);
    sig[1].mux_id = 1;
    sig[1].mux_mask = 0xFF;

    ct::sigSetBits(sig[2], 16, 8, ct::SIGNAL_TYPE_UINT8, 0, 0);
    sig[2].mux_id = 2;
    sig[2].mux_mask = 0xFF;
    sig[2].tx_source = 6; // constant slot 5, encoded +1

    ct::sigSetSelectorOnly(sig[3], true);
    ct::sigSetBits(sig[3], 8, 8, ct::SIGNAL_TYPE_UINT8, 0, 0);
    sig[3].mux_id = 3;
    sig[3].mux_mask = 0xFF;

    CHECK(engine_table_write(ENGINE_TABLE_SIGNALS, 0, 4,
                             reinterpret_cast<const uint8_t *>(sig)));
    ct::ConstantConfig k[2]{};
    k[0].dest_signal_idx = 4;
    k[0].value = 0x77;
    k[0].is_active = 1;
    k[1].dest_signal_idx = 5;
    k[1].value = 0xAB;
    k[1].is_active = 1;
    CHECK(engine_table_write(ENGINE_TABLE_CONSTANTS, 0, 2,
                             reinterpret_cast<const uint8_t *>(k)));

    engine_tick(10);
    // Three variants, three frames — not one.
    CHECK(g_txFrames.size() == 3);
    int seen[4] = {0, 0, 0, 0};
    for (const CapturedTx &t : std::as_const(g_txFrames)) {
        CHECK(t.id == 0x440);
        CHECK(t.len == 8);
        const int sel = t.data[0];
        CHECK(sel >= 1 && sel <= 3);
        ++seen[sel];
        // The always-present byte survives in EVERY variant, including the two
        // whose only other content is their selector. This is the assertion the
        // composer's skip earns: without it the selector-only signals pack a
        // zero here and 0x77 is lost.
        CHECK(t.data[1] == 0x77);
        CHECK(t.data[2] == (sel == 2 ? 0xAB : 0x00));
        for (int b = 3; b < 8; ++b)
            CHECK(t.data[b] == 0x00);
    }
    CHECK(seen[1] == 1 && seen[2] == 1 && seen[3] == 1);

    // Sequential mode rotates over all three too — a selector-only variant is a
    // full member of the rotation, not a gap in it. From a CLEAN engine: the
    // flash model programs each doubleword once per erase, so rewriting message
    // slot 0 in place fails before anything under test can run.
    engine_init(&cb);
    engine_clear_config();
    g_txFrames.clear();
    ct::CanMessageConfig seqMsg = msg;
    seqMsg.flags |= ct::MSGFLAG_TX_SEQUENTIAL;
    CHECK(engine_table_write(ENGINE_TABLE_MESSAGES, 0, 1,
                             reinterpret_cast<const uint8_t *>(&seqMsg)));
    CHECK(engine_table_write(ENGINE_TABLE_SIGNALS, 0, 4,
                             reinterpret_cast<const uint8_t *>(sig)));
    CHECK(engine_table_write(ENGINE_TABLE_CONSTANTS, 0, 2,
                             reinterpret_cast<const uint8_t *>(k)));
    for (int i = 0; i < 3; ++i)
        engine_tick(10);
    CHECK(g_txFrames.size() == 3);
    int rotation[4] = {0, 0, 0, 0};
    for (const CapturedTx &t : std::as_const(g_txFrames)) {
        const int sel = t.data[0];
        CHECK(sel >= 1 && sel <= 3);
        ++rotation[sel];
        CHECK(t.data[1] == 0x77);
    }
    CHECK(rotation[1] == 1 && rotation[2] == 1 && rotation[3] == 1);
}

static void testTransmitClampOrRollOver()
{
    EngineCallbacks cb{};
    cb.transmit_can = captureTransmit;
    engine_init(&cb);
    engine_clear_config();
    g_txFrames.clear();

    ct::CanMessageConfig msg{};
    msg.can_id = 0x420;
    msg.flags = ct::MSGFLAG_ACTIVE | ct::MSGFLAG_TRANSMIT;
    msg.src_bus = 1;
    msg.dlc = 12; // CAN FD, so all twelve fields fit one frame
    msg.period_ms = 10;
    msg.tx_trigger_cond = ct::TX_TRIGGER_COND_NONE;
    msg.flags |= ct::MSGFLAG_FD;
    CHECK(engine_table_write(ENGINE_TABLE_MESSAGES, 0, 1,
                             reinterpret_cast<const uint8_t *>(&msg)));

    struct Row {
        float value;      // what the constant writes to the slot
        bool wrap;        // the row's choice
        bool isSigned;
        float factor;
        float off;        // the DBC offset, which the transmit path subtracts
        float lo, hi;     // the CHANNEL's declared range
        int expect;       // the byte the frame must carry
    };
    const float kNaN = std::numeric_limits<float>::quiet_NaN();
    const Row rows[12] = {
        {256.0f, false, false, 1.0f,     0.0f, -1.0e9f, 1.0e9f, 255},
        {256.0f, true,  false, 1.0f,     0.0f, -1.0e9f, 1.0e9f, 0},
        {-1.0f,  false, false, 1.0f,     0.0f, -1.0e9f, 1.0e9f, 0},
        {-1.0f,  true,  false, 1.0f,     0.0f, -1.0e9f, 1.0e9f, 255},
        {200.0f, false, true,  1.0f,     0.0f, -1.0e9f, 1.0e9f, 127},
        {200.0f, true,  true,  1.0f,     0.0f, -1.0e9f, 1.0e9f, 200},
        {300.0f, true,  false, 1.0f,     0.0f, 0.0f,    255.0f, 44},
        {300.0f, false, false, 1.0f,     0.0f, 0.0f,    100.0f, 100},
        {30.0f,  true,  false, 0.1f,     0.0f, -1.0e9f, 1.0e9f, 44},
        {1.0f,   true,  false, 1.0e-30f, 0.0f, -1.0e9f, 1.0e9f, 0},
        {1.0f,   false, false, 1.0e-30f, 0.0f, -1.0e9f, 1.0e9f, 255},
        // The offset of 5 is what makes this row falsifiable. NaN must resolve
        // to 0 in the GUARD, after the offset has been subtracted — if instead
        // the NaN were scrubbed to a physical 0 somewhere upstream, the row
        // would compute (0 - 5) / 1 = -5 and roll over to 251. Expecting 0
        // tells the two apart; expecting it with offset 0 would not have.
        {kNaN,   true,  false, 1.0f,     5.0f, -1.0e9f, 1.0e9f, 0},
    };

    ct::CanSignalConfig sig[12]{};
    ct::ConstantConfig k[12]{};
    for (int i = 0; i < 12; ++i) {
        sig[i].factor = rows[i].factor;
        sig[i].offset = rows[i].off;
        sig[i].min_val = rows[i].lo;
        sig[i].max_val = rows[i].hi;
        // Wrap set FIRST, header second, deliberately. sigSetHeader writes the
        // same 16-bit word, and the version this feature replaced ASSIGNED it
        // whole — so if it ever goes back to doing that, the CHECK below fails
        // here. Setting the header first would have hidden exactly that.
        ct::sigSetTxWrap(sig[i], rows[i].wrap);
        ct::sigSetHeader(sig[i], 0, 0, 1); // message 0, Intel byte order, active
        ct::sigSetBits(sig[i], quint16(i * 8), 8,
                       rows[i].isSigned ? ct::SIGNAL_TYPE_INT8 : ct::SIGNAL_TYPE_UINT8, 0, 0);
        CHECK(ct::sigTxWrap(sig[i]) == rows[i].wrap);
        CHECK(ct::sigIsActive(sig[i]) == 1);
        CHECK(ct::sigMsgIdx(sig[i]) == 0);
        k[i].dest_signal_idx = quint16(i);
        k[i].value = rows[i].value;
        k[i].is_active = 1;
    }
    CHECK(engine_table_write(ENGINE_TABLE_SIGNALS, 0, 12,
                             reinterpret_cast<const uint8_t *>(sig)));
    CHECK(engine_table_write(ENGINE_TABLE_CONSTANTS, 0, 12,
                             reinterpret_cast<const uint8_t *>(k)));

    engine_tick(10);
    CHECK(g_txFrames.size() == 1);
    const CapturedTx &tx = g_txFrames.first();
    CHECK(tx.id == 0x420);
    CHECK(tx.len == 12);
    for (int i = 0; i < 12; ++i)
        CHECK(int(tx.data[i]) == rows[i].expect);

    // And the other order, on a record of its own: the header first and the
    // wrap bit after must reach the same place. Both directions matter because
    // device_mapper writes them in this order and the tests above in the other.
    ct::CanSignalConfig both{};
    ct::sigSetHeader(both, 7, 1, 1);
    ct::sigSetTxWrap(both, true);
    CHECK(ct::sigTxWrap(both));
    CHECK(ct::sigMsgIdx(both) == 7);
    CHECK(ct::sigByteOrder(both) == 1);
    CHECK(ct::sigIsActive(both) == 1);
    ct::sigSetHeader(both, 9, 0, 1); // a second header write must not drop it
    CHECK(ct::sigTxWrap(both));
    CHECK(ct::sigMsgIdx(both) == 9);
    ct::sigSetTxWrap(both, false);
    CHECK(!ct::sigTxWrap(both));
    CHECK(ct::sigMsgIdx(both) == 9); // clearing it must not disturb the rest
    CHECK(ct::sigIsActive(both) == 1);

    // The signed roll-over byte read back as a signed field really is -56, so
    // the receiver of such a frame sees the value the sender rolled to and not
    // an unsigned 200. This is the sense in which roll-over is lossless: it is
    // the same eight bits, read with the same convention.
    CHECK(int(int8_t(tx.data[5])) == -56);
}

// Wrapping is a TRANSMIT rule. The receive path extracts bit_length bits and
// then clamps to the channel's range, and neither half consults the flag —
// so a receive signal carrying it behaves exactly as one without it.
static void testRollOverIsIgnoredOnReceive()
{
    EngineCallbacks cb{};
    cb.transmit_can = captureTransmit;
    engine_init(&cb);
    engine_clear_config();

    ct::CanMessageConfig msg{};
    msg.can_id = 0x421;
    msg.flags = ct::MSGFLAG_ACTIVE;
    msg.src_bus = 1;
    msg.dlc = 8;
    msg.tx_trigger_cond = ct::TX_TRIGGER_COND_NONE;
    CHECK(engine_table_write(ENGINE_TABLE_MESSAGES, 0, 1,
                             reinterpret_cast<const uint8_t *>(&msg)));

    // Two identical 8-bit receive signals but for the flag, both on a channel
    // ranged 0..100 so the clamp has something to do.
    ct::CanSignalConfig sig[2]{};
    for (int i = 0; i < 2; ++i) {
        sig[i].factor = 1.0f;
        sig[i].min_val = 0.0f;
        sig[i].max_val = 100.0f;
        ct::sigSetHeader(sig[i], 0, 0, 1);
        ct::sigSetTxWrap(sig[i], i == 1);
        ct::sigSetBits(sig[i], quint16(i * 8), 8, ct::SIGNAL_TYPE_UINT8, 0, 0);
    }
    CHECK(engine_table_write(ENGINE_TABLE_SIGNALS, 0, 2,
                             reinterpret_cast<const uint8_t *>(sig)));

    const uint8_t frame[8] = {200, 200, 0, 0, 0, 0, 0, 0};
    engine_process_can(1, 0x421, 0, 0, frame, 8);
    // Both clamp to the channel's 100, the flagged one included.
    CHECK(qAbs(engine_signal_value(0) - 100.0f) < 0.01f);
    CHECK(qAbs(engine_signal_value(1) - 100.0f) < 0.01f);
}

static void testTransmitCrc8Stamping()
{
    // The reference must earn its authority first. CRC-8/SAE-J1850
    // (0x1D/FF/FF, no reflection) and CRC-8/ROHC (0x07/FF, both reflections)
    // have published check values over "123456789"; SMBus's plain 0x07/00/00
    // rounds out the set. If any of these three CHECKs fail, every
    // conclusion below is void — the reference is broken, not the engine.
    const QByteArray check = QByteArrayLiteral("123456789");
    CHECK(crc8Ref(check, 0x1D, 0xFF, false, false, 0xFF) == 0x4B);
    CHECK(crc8Ref(check, 0x07, 0x00, false, false, 0x00) == 0xF4);
    CHECK(crc8Ref(check, 0x07, 0xFF, true, true, 0x00) == 0xD0);

    EngineCallbacks cb{};
    cb.transmit_can = captureTransmit;
    engine_init(&cb);
    engine_clear_config();
    g_txFrames.clear();

    // Three messages, three rules — the flash model programs each slot once,
    // so each scenario gets its own message rather than rewriting one.
    //   0: RAW elements spelling "123456789", J1850 — the catalogue anchor.
    //   1: a 16-bit channel in bytes 0-1, CRC over Data 0-1 into byte 2 —
    //      the ordering proof: the CRC must match a reference run over the
    //      bytes the composer PACKED, whatever the packing convention.
    //   2: extended ID fed through all four ID elements, ROHC — the
    //      identifier path and both reflections on silicon-identical code.
    ct::CanMessageConfig msgs[3]{};
    msgs[0].can_id = 0x300;
    msgs[0].flags = ct::MSGFLAG_ACTIVE | ct::MSGFLAG_TRANSMIT;
    msgs[0].src_bus = 1;
    msgs[0].dlc = 8;
    msgs[0].period_ms = 10;
    msgs[1] = msgs[0];
    msgs[1].can_id = 0x301;
    msgs[1].dlc = 4;
    msgs[2] = msgs[0];
    msgs[2].can_id = 0x12345678;
    msgs[2].flags = ct::MSGFLAG_ACTIVE | ct::MSGFLAG_TRANSMIT | ct::MSGFLAG_EXTENDED;
    msgs[2].dlc = 2;
    CHECK(engine_table_write(ENGINE_TABLE_MESSAGES, 0, 3,
                             reinterpret_cast<const uint8_t *>(msgs)));

    // Message 1's payload: one 16-bit signal in bytes 0-1, fed by a constant
    // writing its slot (slot 5) — constants run before the composer in the
    // same tick, so the frame carries a known non-zero pattern.
    ct::CanSignalConfig sig{};
    sig.factor = 1.0f;
    sig.min_val = -1.0e9f;
    sig.max_val = 1.0e9f;
    ct::sigSetHeader(sig, 1, 0, 1); // message 1, Intel byte order, active
    ct::sigSetBits(sig, 0, 16, ct::SIGNAL_TYPE_UINT16, 0, 0);
    std::memcpy(sig.label, "CrcPayload", 11);
    CHECK(engine_table_write(ENGINE_TABLE_SIGNALS, 5, 1,
                             reinterpret_cast<const uint8_t *>(&sig)));
    ct::ConstantConfig k{};
    k.dest_signal_idx = 5;
    k.value = 48879.0f; // 0xBEEF
    k.is_active = 1;
    CHECK(engine_table_write(ENGINE_TABLE_CONSTANTS, 0, 1, (const quint8 *)&k));

    ct::Crc8Config rules[3]{};
    rules[0].msg_idx = 0;
    rules[0].dest_signal_idx = 900;
    rules[0].byte_location = 0;
    rules[0].polynomial = 0x1D;
    rules[0].init_value = 0xFF;
    rules[0].final_xor = 0xFF;
    rules[0].flags = ct::CRC8FLAG_ACTIVE;
    rules[0].element_count = 9;
    for (int i = 0; i < 9; ++i) {
        rules[0].elem_type[i] = ct::CRC8_ELEM_RAW;
        rules[0].elem_value[i] = quint8('1' + i);
    }
    rules[1].msg_idx = 1;
    rules[1].dest_signal_idx = 901;
    rules[1].byte_location = 2;
    rules[1].polynomial = 0x07;
    rules[1].flags = ct::CRC8FLAG_ACTIVE;
    rules[1].element_count = 2;
    rules[1].elem_type[0] = ct::CRC8_ELEM_DATA;
    rules[1].elem_value[0] = 0;
    rules[1].elem_type[1] = ct::CRC8_ELEM_DATA;
    rules[1].elem_value[1] = 1;
    rules[2].msg_idx = 2;
    rules[2].dest_signal_idx = 902;
    rules[2].byte_location = 0;
    rules[2].polynomial = 0x07;
    rules[2].init_value = 0xFF;
    rules[2].flags = ct::CRC8FLAG_ACTIVE | ct::CRC8FLAG_REF_IN | ct::CRC8FLAG_REF_OUT;
    rules[2].element_count = 4;
    for (int i = 0; i < 4; ++i) {
        rules[2].elem_type[i] = ct::CRC8_ELEM_ID;
        rules[2].elem_value[i] = quint8(i);
    }
    // The rules go in over the WIRE, unlike the frames above: 0x41/0x42
    // routing through the dispatch switch is part of what this test holds.
    // The first draft of this feature sat on 0x38/0x39, which belong to
    // FW_UPDATE_BEGIN/DATA, and only the updater's own tests noticed the
    // dispatch eating its frames.
    QVector<ct::Crc8Config> ruleVec;
    for (const ct::Crc8Config &r : rules)
        ruleVec.append(r);
    CHECK(expectAck(ct::CMD_WRITE_CRC8_CFG, writeChunk(quint16(0), ruleVec, 0, 3)));
    const QByteArray rb = readRange(ct::CMD_READ_CRC8_CFG, 0, 3);
    CHECK(rb.size() == 4 + 3 * int(sizeof(ct::Crc8Config)));
    CHECK(std::memcmp(rb.constData() + 4, rules, 3 * sizeof(ct::Crc8Config)) == 0);

    engine_tick(10);

    CapturedTx f0{}, f1{}, f2{};
    bool have0 = false, have1 = false, have2 = false;
    for (const CapturedTx &t : std::as_const(g_txFrames)) {
        if (t.id == 0x300) { f0 = t; have0 = true; }
        if (t.id == 0x301) { f1 = t; have1 = true; }
        if (t.id == 0x12345678) { f2 = t; have2 = true; }
    }
    CHECK(have0 && have1 && have2);

    // The catalogue anchor, now off the engine: J1850 over "123456789".
    CHECK(f0.data[0] == 0x4B);
    CHECK(qAbs(engine_signal_value(900) - float(0x4B)) < 0.001f);

    // Ordering: the stamped byte matches a reference run over the bytes the
    // composer actually packed. Also proves those bytes are non-zero — a
    // composer that packed nothing would produce a "CRC" over zeros that a
    // matching reference would happily agree with.
    CHECK(f1.data[0] != 0 || f1.data[1] != 0);
    const quint8 expect1 = crc8Ref(QByteArray(reinterpret_cast<const char *>(f1.data), 2),
                                   0x07, 0x00, false, false, 0x00);
    CHECK(f1.data[2] == expect1);
    CHECK(qAbs(engine_signal_value(901) - float(expect1)) < 0.001f);

    // The ID path with both reflections: bytes 78 56 34 12, low byte first.
    const char idBytes[4] = {0x78, 0x56, 0x34, 0x12};
    const quint8 expect2 = crc8Ref(QByteArray(idBytes, 4), 0x07, 0xFF, true, true, 0x00);
    CHECK(f2.data[0] == expect2);
    CHECK(qAbs(engine_signal_value(902) - float(expect2)) < 0.001f);

    engine_clear_config();
}

// The CAN diagnostic device channels. Three things are worth proving and the
// rest is the same plumbing testDeviceOnTime already covers:
//
//   1. Each bus's block lands in its OWN slots. An off-by-one in the DEVCH_BUS
//      macro would put bus 2's error counters where bus 1's belong, and every
//      value would still look plausible.
//   2. The error-frame TOTAL accumulates the per-sample deltas. The register it
//      comes from clears on read, so the running total only exists here.
//   3. The totals survive a config clear while the DESTINATIONS do not — the
//      same split OnTime has, and the one somebody "tidying up" resetRuntime
//      would most easily break.
static void testDeviceCanDiagnostics()
{
    EngineCallbacks cb{};
    cb.transmit_can = captureTransmit;
    engine_init(&cb);
    engine_clear_config();

    // One slot per (bus, field), laid out so a cross-wired index is visible as a
    // value landing in the wrong place rather than as a plausible number.
    DeviceChannelsConfig dc = unusedDeviceChannelsFw();
    const auto slotFor = [](int bus0, int field) { return quint16(100 + bus0 * 16 + field); };
    for (int b = 0; b < DEVCH_BUS_COUNT; ++b)
        for (int f = 0; f < DEVCH_PER_BUS; ++f)
            dc.signal_idx[DEVCH_BUS(b, f)] = slotFor(b, f);
    engine_set_device_channels(&dc);

    // Distinct values per bus, so bus 2 reading bus 1's counter fails here.
    for (int b = 0; b < DEVCH_BUS_COUNT; ++b) {
        BusDiagnostics d{};
        d.rx_errors = quint8(10 + b);
        d.tx_errors = quint8(20 + b);
        d.flags = BUSDIAG_WARNING;
        d.error_delta = quint8(b + 1);
        engine_set_bus_diagnostics(quint8(b + 1), &d);
    }
    engine_tick(10);

    for (int b = 0; b < DEVCH_BUS_COUNT; ++b) {
        CHECK(engine_signal_value(slotFor(b, DEVCH_BUS_RX_ERRORS)) == float(10 + b));
        CHECK(engine_signal_value(slotFor(b, DEVCH_BUS_TX_ERRORS)) == float(20 + b));
        CHECK(engine_signal_value(slotFor(b, DEVCH_BUS_WARNING)) == 1.0f);
        CHECK(engine_signal_value(slotFor(b, DEVCH_BUS_ERROR_PASSIVE)) == 0.0f);
        CHECK(engine_signal_value(slotFor(b, DEVCH_BUS_BUS_OFF)) == 0.0f);
        CHECK(engine_signal_value(slotFor(b, DEVCH_BUS_ERROR_FRAMES)) == float(b + 1));
    }

    // The flags are independent, not a severity ladder: a bus-off node is also
    // error-passive and also warning, and all three must read true at once.
    {
        BusDiagnostics d{};
        d.flags = quint8(BUSDIAG_WARNING | BUSDIAG_ERROR_PASSIVE | BUSDIAG_BUS_OFF);
        engine_set_bus_diagnostics(1, &d);
    }
    engine_tick(10);
    CHECK(engine_signal_value(slotFor(0, DEVCH_BUS_WARNING)) == 1.0f);
    CHECK(engine_signal_value(slotFor(0, DEVCH_BUS_ERROR_PASSIVE)) == 1.0f);
    CHECK(engine_signal_value(slotFor(0, DEVCH_BUS_BUS_OFF)) == 1.0f);

    // Error frames ACCUMULATE. Three more samples of 2 on top of bus 1's
    // existing 1 is 7 — a level, not a delta, would read 2.
    for (int i = 0; i < 3; ++i) {
        BusDiagnostics d{};
        d.error_delta = 2;
        engine_set_bus_diagnostics(1, &d);
    }
    engine_tick(10);
    CHECK(engine_signal_value(slotFor(0, DEVCH_BUS_ERROR_FRAMES)) == 7.0f);

    // Bus-off RECOVERIES count RESTARTS THE GLUE REPORTS, not the bus-off flag
    // clearing.
    //
    // This was the other way round and it read zero on hardware, which is worth
    // recording because the reasoning for inferring it was superficially good.
    // A restarted bus that is still faulty is back in bus-off within about five
    // milliseconds — thirty-two transmit errors, and at 800 kbit/s that is far
    // inside one 100 Hz tick — so the flag never actually reads clear. Watching
    // for the transition therefore saw NOTHING while the device dutifully
    // restarted the bus once a second, on exactly the bus the reading exists
    // for. The restart is the event; the glue knows it happened; it reports it.
    const auto setBusOff = [](quint8 bus, bool off) {
        BusDiagnostics d{};
        d.flags = off ? quint8(BUSDIAG_BUS_OFF) : quint8(0);
        engine_set_bus_diagnostics(bus, &d);
    };
    setBusOff(2, false); // bus 2 has never been bus-off; start from a known state
    engine_tick(10);
    const float recBefore = engine_signal_value(slotFor(1, DEVCH_BUS_OFF_RECOVERIES));

    // The flag alone moves nothing, in either direction. Down, up, down, up —
    // all of it is silent without a reported restart, which is the regression
    // guard for going back to inferring it.
    for (int i = 0; i < 5; ++i)
        setBusOff(2, true);
    engine_tick(10);
    CHECK(engine_signal_value(slotFor(1, DEVCH_BUS_BUS_OFF)) == 1.0f);
    CHECK(engine_signal_value(slotFor(1, DEVCH_BUS_OFF_RECOVERIES)) == recBefore);
    setBusOff(2, false);
    engine_tick(10);
    CHECK(engine_signal_value(slotFor(1, DEVCH_BUS_OFF_RECOVERIES)) == recBefore);

    // A reported restart counts, once each.
    engine_note_bus_restart(2);
    engine_tick(10);
    CHECK(engine_signal_value(slotFor(1, DEVCH_BUS_OFF_RECOVERIES)) == recBefore + 1.0f);

    // The case that made this change necessary: a bus restarted while it never
    // once reads healthy. The flag stays set the whole time — the restart is
    // undone before the next sample — and the count must still climb.
    setBusOff(2, true);
    for (int i = 0; i < 3; ++i) {
        engine_note_bus_restart(2);
        setBusOff(2, true);
        engine_tick(10);
    }
    CHECK(engine_signal_value(slotFor(1, DEVCH_BUS_BUS_OFF)) == 1.0f);
    CHECK(engine_signal_value(slotFor(1, DEVCH_BUS_OFF_RECOVERIES)) == recBefore + 4.0f);

    // Per-bus, like everything else in this block: bus 3 saw none of it.
    CHECK(engine_signal_value(slotFor(2, DEVCH_BUS_OFF_RECOVERIES)) == 0.0f);
    // And an out-of-range bus is ignored rather than scribbling past the array.
    engine_note_bus_restart(0);
    engine_note_bus_restart(4);
    engine_tick(10);
    CHECK(engine_signal_value(slotFor(1, DEVCH_BUS_OFF_RECOVERIES)) == recBefore + 4.0f);

    // Frame counts come from the same counters GET_STATUS reports.
    const float rxBefore = engine_signal_value(slotFor(1, DEVCH_BUS_RX_COUNT));
    engine_count_rx(2, 0 /* standard */, 0 /* classic */, 8);
    engine_tick(10);
    CHECK(engine_signal_value(slotFor(1, DEVCH_BUS_RX_COUNT)) == rxBefore + 1.0f);

    // Bus load: a 500 kbit/s bus carrying one 8-byte standard frame every 10 ms
    // is 100 frames of ~130 bits per second, so ~13 kbit of 500 kbit — about
    // 2.6 %.
    //
    // TWO HUNDRED ticks, not one hundred, and the reason is the whole reason
    // this line is worth a comment. The averaging window is free-running and its
    // phase is NOT reset by engine_init — it is device state, like the clock —
    // so by the time this test runs it sits at whatever earlier tests left it
    // at. One window's worth of ticks would therefore close the window at an
    // arbitrary point and latch a partial count. Two windows' worth guarantees
    // one FULL window falls entirely inside the loop, whatever the phase started
    // at, and it is that window's figure the check sees.
    //
    // Checked as a RANGE deliberately: the figure includes a modelled stuff-bit
    // allowance and is documented as an estimate, so pinning it to a digit would
    // make the test a restatement of the formula rather than a check that the
    // plumbing works and the scale is right.
    engine_set_bus_bitrate(3, 500000, 500000);
    for (int i = 0; i < 200; ++i) {
        engine_count_rx(3, 0, 0, 8);
        engine_tick(10);
    }
    const float load = engine_signal_value(slotFor(2, DEVCH_BUS_LOAD));
    CHECK(load > 2.0f && load < 3.5f);

    // A bus with no bit rate reported publishes 0 rather than dividing by it.
    engine_set_bus_bitrate(1, 0, 0);
    for (int i = 0; i < 101; ++i)
        engine_tick(10);
    CHECK(engine_signal_value(slotFor(0, DEVCH_BUS_LOAD)) == 0.0f);

    // CAN FD, and specifically that the two rate domains are really separate.
    //
    // The classic case above cannot tell a working split from a model that
    // charges every bit to the nominal rate, because on a classic bus those are
    // the same arithmetic — so the FD branch was reachable only from hardware
    // and went unmeasured. Drive IDENTICAL frames onto two buses that differ in
    // NOTHING but their data rate: a 64-byte standard BRS frame is 36 bits of
    // arbitration plus 546 bits of data phase, so at 100 frames/s it is 3.45 %
    // of a 500 k bus whose data phase runs at 2 M, and 11.64 % when the data
    // phase runs at 500 k as well. A model that ignored data_baud, or charged
    // the payload to the wrong domain, reports the same figure for both and
    // fails here.
    //
    // This is the arithmetic behind the flag meaning BRS rather than "FD
    // format": read the flag off the frame's length and a non-switched FD frame
    // lands in the fast column, reporting 3.45 % for a bus genuinely at 11.64 %.
    engine_set_bus_bitrate(1, 500000, 2000000); // bit rate switched
    engine_set_bus_bitrate(2, 500000, 500000);  // same frames, one rate
    for (int i = 0; i < 200; ++i) {
        engine_count_rx(1, 0 /* standard */, 1 /* BRS */, 64);
        engine_count_rx(2, 0 /* standard */, 1 /* BRS */, 64);
        engine_tick(10);
    }
    const float fdSwitched = engine_signal_value(slotFor(0, DEVCH_BUS_LOAD));
    const float fdSingleRate = engine_signal_value(slotFor(1, DEVCH_BUS_LOAD));
    CHECK(fdSwitched > 3.2f && fdSwitched < 3.7f);
    CHECK(fdSingleRate > 11.2f && fdSingleRate < 12.1f);

    // The destinations go with the configuration; the TOTALS do not. A
    // technician watching an error count climb must not have it zeroed by the
    // host re-sending a config.
    engine_clear_config();
    for (int f = 0; f < DEVCH_PER_BUS; ++f)
        CHECK(engine_device_channels()->signal_idx[DEVCH_BUS(0, f)] == ct::SIG_MSG_NONE);
    engine_set_device_channels(&dc);
    engine_tick(10);
    CHECK(engine_signal_value(slotFor(0, DEVCH_BUS_ERROR_FRAMES)) == 7.0f);
    // The recovery count is device history too, for the same reason: a bus that
    // flapped while the host was mid-Send still flapped.
    CHECK(engine_signal_value(slotFor(1, DEVCH_BUS_OFF_RECOVERIES)) == recBefore + 4.0f);

    engine_clear_config();
}

// The v9 MCU health block: current temperature and VDDA land in their slots,
// the excursions seed from the FIRST sample (a unit that boots hot must not
// claim a 0 °C maximum), track correctly afterwards, and — like the CAN error
// totals — survive a configuration clear. The reset reason is a latched
// enumeration and out-of-range values collapse to UNKNOWN, because the GUI
// shows NAMES for this channel and an unnamed number would read as a reason
// the enumeration forgot.
static void testMcuHealth()
{
    EngineCallbacks cb{};
    cb.transmit_can = captureTransmit;
    engine_init(&cb);
    engine_clear_config();

    DeviceChannelsConfig dc = unusedDeviceChannelsFw();
    dc.signal_idx[DEVCH_MCU_TEMP] = 300;
    dc.signal_idx[DEVCH_MCU_VDDA] = 301;
    dc.signal_idx[DEVCH_MCU_VDDA_MIN] = 302;
    dc.signal_idx[DEVCH_MCU_TEMP_MAX] = 303;
    dc.signal_idx[DEVCH_RESET_REASON] = 304;
    engine_set_device_channels(&dc);

    // First sample seeds the excursions — max == current, min == current.
    engine_set_reset_reason(RESET_REASON_BROWNOUT);
    engine_set_mcu_health(41.5f, 3.301f);
    engine_tick(10);
    CHECK(engine_signal_value(300) == 41.5f);
    CHECK(qAbs(engine_signal_value(301) - 3.301f) < 1e-6f);
    CHECK(qAbs(engine_signal_value(302) - 3.301f) < 1e-6f);
    CHECK(engine_signal_value(303) == 41.5f);
    CHECK(engine_signal_value(304) == float(RESET_REASON_BROWNOUT));

    // A hotter, browner sample moves both excursions; a milder one moves
    // neither, and the currents always follow.
    engine_set_mcu_health(55.0f, 3.245f);
    engine_set_mcu_health(48.0f, 3.290f);
    engine_tick(10);
    CHECK(engine_signal_value(300) == 48.0f);
    CHECK(qAbs(engine_signal_value(301) - 3.290f) < 1e-6f);
    CHECK(qAbs(engine_signal_value(302) - 3.245f) < 1e-6f);
    CHECK(engine_signal_value(303) == 55.0f);

    // The destinations go with the configuration; the READINGS do not. Same
    // rule, same reason as the CAN totals above: a reconfigure mid-diagnosis
    // must not erase what the unit has been through.
    engine_clear_config();
    engine_set_device_channels(&dc);
    engine_tick(10);
    CHECK(engine_signal_value(302) < 3.246f); // the excursion is still there
    CHECK(engine_signal_value(303) == 55.0f);
    CHECK(engine_signal_value(304) == float(RESET_REASON_BROWNOUT));

    // Out-of-range collapses to UNKNOWN rather than passing through.
    engine_set_reset_reason(99);
    engine_tick(10);
    CHECK(engine_signal_value(304) == float(RESET_REASON_UNKNOWN));

    engine_clear_config();
}

// The 200 Hz transmit split. engine_tick is now calc + service in one clock;
// the device drives the halves separately — the chain at 10 ms, the transmit
// scheduler at 5 ms — and a 5 ms period must yield exactly two frames per
// 10 ms window on that split, CRC8-stamped messages included, because the
// stamp rides composition and composition rides the service.
static void testTransmitAt200Hz()
{
    EngineCallbacks cb{};
    cb.transmit_can = captureTransmit;
    engine_init(&cb);
    engine_clear_config();

    ct::CanMessageConfig msgs[2]{};
    msgs[0].can_id = 0x500;
    msgs[0].flags = ct::MSGFLAG_ACTIVE | ct::MSGFLAG_TRANSMIT;
    msgs[0].src_bus = 1;
    msgs[0].dlc = 2;
    msgs[0].period_ms = 5;
    msgs[1] = msgs[0];
    msgs[1].can_id = 0x501;
    CHECK(engine_table_write(ENGINE_TABLE_MESSAGES, 0, 2,
                             reinterpret_cast<const uint8_t *>(msgs)));

    ct::Crc8Config rule{};
    rule.msg_idx = 1;
    rule.dest_signal_idx = 950;
    rule.byte_location = 0;
    rule.polynomial = 0x07;
    rule.flags = ct::CRC8FLAG_ACTIVE;
    rule.element_count = 1;
    rule.elem_type[0] = ct::CRC8_ELEM_RAW;
    rule.elem_value[0] = 0x42;
    CHECK(engine_table_write(ENGINE_TABLE_CRC8, 0, 1,
                             reinterpret_cast<const uint8_t *>(&rule)));
    const quint8 want = crc8Ref(QByteArrayLiteral("\x42"), 0x07, 0x00, false, false, 0x00);

    g_txFrames.clear();
    // Forty 10 ms windows, split exactly as the device runs them: the chain
    // once, the service twice. Phase seeding staggers the first window, so
    // the count is checked as a rate over many windows rather than pinned
    // per-window.
    for (int w = 0; w < 40; ++w) {
        engine_tick_calc(10);
        engine_service_transmit(5);
        engine_service_transmit(5);
    }
    int n500 = 0, n501 = 0, badCrc = 0;
    for (const CapturedTx &t : std::as_const(g_txFrames)) {
        if (t.id == 0x500)
            ++n500;
        if (t.id == 0x501) {
            ++n501;
            if (t.data[0] != want)
                ++badCrc;
        }
    }
    // 40 windows x 2 slots = 80 due points per message, +/- the seeding phase.
    CHECK(n500 >= 78 && n500 <= 81);
    CHECK(n501 >= 78 && n501 <= 81);
    CHECK(badCrc == 0);

    // The composed clock keeps its old meaning: one engine_tick(10) fires a
    // 5 ms message once, not twice — the accumulator clamp forbids burst
    // catch-up, which is exactly the "freshest value or nothing" promise.
    g_txFrames.clear();
    for (int w = 0; w < 20; ++w)
        engine_tick(10);
    int nComposed = 0;
    for (const CapturedTx &t : std::as_const(g_txFrames))
        if (t.id == 0x500)
            ++nComposed;
    CHECK(nComposed >= 19 && nComposed <= 21);

    engine_clear_config();
}

// Reading it back through flash_store_validate rather than through
// engine_access_keys() is the whole test. The RAM copy was never in doubt.
// Bus modes and rates survive the trip out and back. Before CMD_READ_CAN_SETUP
// existed, CONTROL_CAN was write-only and a Get had to ASSUME the bring-up
// rates â€” so a configuration read off a device came back subtly different from
// the one that went onto it, in the one place nothing could check.
//
// Driving CONTROL_CAN and then reading it back through the real serial layer is
// what proves the pair agree: the payload is the same 11-byte record in both
// directions, and a padding or endianness slip would show up here as a rate that
// changed on the way home.
static void testBusSetupReadback(const SerialProtoCallbacks *restore)
{
    EngineCallbacks cb{};
    cb.transmit_can = captureTransmit;
    engine_init(&cb);
    engine_set_access_keys(nullptr);
    serial_proto_init(restore);
    std::memset(g_busSetup, 0, sizeof(g_busSetup));

    struct Want { quint8 mode; quint32 baud; quint32 dataBaud; quint8 term; };
    const Want wanted[3] = {
        {1, 1000000u, 0u, 1},       // active, 1M classic, terminated
        {2, 500000u, 2000000u, 0},  // listen-only, 500k + 2M FD, unterminated
        {0, 250000u, 0u, 1},        // off, but still terminated
    };

    for (int i = 0; i < 3; ++i) {
        ct::ControlCanPayload req{};
        req.bus_idx = quint8(i + 1);
        req.mode = wanted[i].mode;
        req.baud_rate = wanted[i].baud;
        req.data_baud_rate = wanted[i].dataBaud;
        req.termination = wanted[i].term;
        CHECK(expectAck(ct::CMD_CONTROL_CAN,
                        QByteArray(reinterpret_cast<const char *>(&req), sizeof(req))));
    }

    const auto packets = exchange(ct::CMD_READ_CAN_SETUP, QByteArray());
    CHECK(packets.size() == 1);
    CHECK(packets[0].cmd == ct::CMD_READ_CAN_SETUP);
    CHECK(packets[0].payload.size() == 3 * int(sizeof(ct::ControlCanPayload)));

    for (int i = 0; i < 3; ++i) {
        ct::ControlCanPayload got{};
        std::memcpy(&got, packets[0].payload.constData() + i * int(sizeof(got)), sizeof(got));
        // Positional: slot i must describe bus i + 1, and the firmware stamps
        // the index itself so a host cross-checking it cannot be misled.
        CHECK(got.bus_idx == quint8(i + 1));
        CHECK(got.mode == wanted[i].mode);
        CHECK(got.baud_rate == wanted[i].baud);
        CHECK(got.data_baud_rate == wanted[i].dataBaud);
        CHECK(got.termination == wanted[i].term);
    }

    // Firmware without the callback must NACK ERR_INVALID_CMD, not answer
    // zeroes â€” the host reads that as "cannot tell you" and falls back to
    // assuming bring-up rates. Answering all-zero would instead assert that
    // every bus is off at 0 kbit/s, which a Send would then faithfully apply.
    {
        SerialProtoCallbacks noRead = *restore;
        noRead.read_can_setup = nullptr;
        serial_proto_init(&noRead);
        CHECK(expectNack(ct::CMD_READ_CAN_SETUP, QByteArray(), ct::ERR_INVALID_CMD));
    }

    std::memset(g_busSetup, 0, sizeof(g_busSetup));
    serial_proto_init(restore);
}

// Every read the host waits on must be COMPLETABLE by DeviceLink's matching
// rule against the bytes the firmware really sends. This exists because it once
// was not, and the failure was invisible from either side alone.
//
// handlePacket() discards a data reply whose first four bytes do not repeat the
// request's, so a retry's late duplicate cannot be mistaken for the answer. That
// echo only exists on RANGE reads. The rule used to be written as "every read
// except GET_STATUS and READ_CONFIG_NAME", which was true when those were the
// only two â€” and then GET_DEVICE_ID, READ_ACCESS_KEYS, ACCESS_CHALLENGE,
// READ_FLEET_ID and FLEET_ID_PROVE were added, each answering with a fixed-shape
// reply that echoes nothing, each therefore discarded, each timing out. Send
// Configuration failed outright with "no response from device".
//
// Neither half was wrong on its own: the firmware answered correctly and the
// host's guard was correct for the commands it was written for. Only the two
// together are testable, which is what this file is for.
static void testReadResponseMatching(const SerialProtoCallbacks *restore)
{
    EngineCallbacks cb{};
    cb.transmit_can = captureTransmit;
    engine_init(&cb);
    engine_set_access_keys(nullptr); // no gate: every read must be answerable
    serial_proto_init(restore);
    flashErase();

    QByteArray rangeReq(4, char(0)); // start 0, count 1
    rangeReq[2] = char(1);
    const QByteArray challenge(ct::ACCESS_CHALLENGE_LEN, char(0x5A));

    const struct {
        quint8 cmd;
        QByteArray request;
        const char *name;
    } reads[] = {
        {ct::CMD_GET_STATUS, QByteArray(), "GET_STATUS"},
        {ct::CMD_READ_CONFIG_NAME, QByteArray(), "READ_CONFIG_NAME"},
        {ct::CMD_GET_DEVICE_ID, QByteArray(), "GET_DEVICE_ID"},
        {ct::CMD_READ_ACCESS_KEYS, QByteArray(), "READ_ACCESS_KEYS"},
        {ct::CMD_ACCESS_CHALLENGE, QByteArray(), "ACCESS_CHALLENGE"},
        {ct::CMD_READ_FLEET_ID, QByteArray(), "READ_FLEET_ID"},
        {ct::CMD_READ_CAN_SETUP, QByteArray(), "READ_CAN_SETUP"},
        {ct::CMD_FW_UPDATE_STATUS, QByteArray(), "FW_UPDATE_STATUS"},
        {ct::CMD_READ_MSG_CFG, rangeReq, "READ_MSG_CFG (range control)"},
    };

    for (const auto &r : reads) {
        // Anything the host is prepared to wait for must be declared a read.
        CHECK(ct::DeviceLink::isReadResponse(r.cmd));
        const auto packets = exchange(r.cmd, r.request);
        bool answered = false;
        for (const ct::Packet &p : packets) {
            if (p.cmd != r.cmd)
                continue;
            answered = true;
            // The exact condition handlePacket() applies, against real bytes.
            const bool accepted =
                !ct::DeviceLink::echoesRequestRange(r.cmd)
                || (p.payload.size() >= 4 && p.payload.left(4) == r.request.left(4));
            if (!accepted)
                std::fprintf(stderr, "  read %s would be discarded by the echo guard\n", r.name);
            CHECK(accepted);
        }
        if (!answered)
            std::fprintf(stderr, "  read %s produced no data reply at all\n", r.name);
        CHECK(answered);
    }

    // FLEET_ID_PROVE is the one read whose request HAS four bytes to echo and
    // deliberately does not echo them â€” it answers an HMAC over the challenge.
    // If it were ever listed as a range read the guard would compare the HMAC's
    // first four bytes against the challenge's and discard it, so pin it.
    CHECK(!ct::DeviceLink::echoesRequestRange(ct::CMD_FLEET_ID_PROVE));
    {
        const auto packets = exchange(ct::CMD_FLEET_ID_PROVE, challenge);
        // Unprovisioned firmware has no fleet key, so this NACKs rather than
        // answering â€” which is itself the correct behaviour and worth pinning.
        CHECK(packets.size() == 1);
        CHECK(packets[0].cmd == ct::CMD_NACK);
        CHECK(quint8(packets[0].payload[0]) == ct::ERR_LOCKED);
    }

    serial_proto_init(restore);
}

// 2.3.0: what the DEVICE enforces about message protection, which is NOTHING.
//
// This replaces v20's testPerMessageProtection, and it is deliberately a
// rewrite rather than a deletion. v20 gated message writes on a per-message
// key; v21.1 gated CMD_CLEAR_CONFIG on "any stored message is keyed". Both are
// gone, along with g_msg_proved, msgProved, msgWriteBlocked,
// anyStoredMessageLocked and the whole CMD_MSG_ACCESS_RESPONSE handler. All
// three protection tiers are now conventions of the Device Manager. The device
// carries the MSGPROT_* level on the wire for ROUND-TRIP FIDELITY ONLY — so a
// Get followed by a Send cannot launder a Hidden message into an ordinary one —
// and never reads it to decide anything.
//
// A deleted gate leaves no trace in a build, so these assertions are the only
// thing that will notice one coming back. Every acceptance below was an
// ERR_LOCKED before, and each is the regression test for a specific deleted
// rule: re-add the rule and the matching CHECK fails. That is the whole reason
// this is written as acceptances rather than simply removed.
//
// Three things carried over from the old function unchanged, because they are
// still true and still load-bearing:
//
//   * marked records are READABLE. Reads have been ungated since v21, when
//     refusing them made a whole configuration unretrievable over one marked
//     message and broke backup, inspection and re-flash.
//   * bytes 10..13 of every record read back are ZERO. That outlives the
//     retirement of the per-message key field: a 2.2.1 unit updated in place
//     keeps its flash image, that image holds live PBKDF2 key material at those
//     offsets, and the read path is ungated. Delete the zeroing and the first
//     Get after the update leaks it.
//   * opcode 0x40 answers ERR_INVALID_CMD, not ERR_LOCKED.
//
// The negative control at the end matters as much as the acceptances: a fixture
// that could not produce a refusal at all would pass every acceptance here
// while proving nothing. It makes "the clear was ACKed" mean "no message gate
// fired" rather than "this test cannot see a gate fire".
//
// One thing this function CANNOT express, so that nobody adds it later and is
// puzzled: a modifying in-place rewrite. STM32 flash programs each doubleword
// once per erase and fw_host_stub.c models that faithfully, so a write that
// changes a stored record's bytes fails on the FLASH (ERR_FLASH_WRITE) before
// any gate could have an opinion. Only the identical rewrite — the store's
// idempotent retransmit path — reaches a gate, and that is the one v20 refused,
// so it is the one inverted here. The bit-clearing write is exercised the way
// it actually happens on a device: CLEAR_CONFIG, then Send. Which is precisely
// why the v20 write gate was theatre and is precisely what DECISIONS D3 says.
// A VALID PASSWORD SLOT SURVIVES THE DEVICE, which is the half the feature
// needs. The scrub this replaced zeroed the byte unconditionally; had it stayed,
// every marked message would have come home pointing at no password and the
// round trip would still be lossy. Both directions are exercised, because a
// clamp on the way IN and a blanking on the way OUT look identical from here.
// v17: FOUR PROTECTED COMMS SLOTS, ANY OF WHICH OPENS. One unit accepts
// configurations sealed under any of four device passwords; per-vendor
// separation lives in the per-configuration message passwords, not here.
// Also pinned: the mask bit means "at least one slot", a slot clears
// individually, and the old 6-byte write payload still lands in slot 1.
static void testProtectedCommsSlots(const SerialProtoCallbacks *restore)
{
    EngineCallbacks cb{};
    cb.transmit_can = captureTransmit;
    engine_init(&cb);
    engine_set_access_keys(nullptr);
    serial_proto_init(restore);
    flashErase();

    const auto writeKey = [&](quint8 slot, quint32 key, bool clear, bool legacy) {
        QByteArray p;
        p.append(char(2)); // ACCESS_FN_EDIT_COMMS
        p.append(char(clear ? 1 : 0));
        p.append(clear ? QByteArray(int(ct::ACCESS_KEY_LEN), char(0))
                       : ct::accessKeyBytes(ct::AccessKey(key)));
        if (!legacy)
            p.append(char(slot));
        return expectAck(ct::CMD_WRITE_ACCESS_KEYS, p);
    };
    const auto slotMask = [&]() -> int {
        const auto packets = exchange(ct::CMD_READ_ACCESS_KEYS, QByteArray());
        if (packets.size() != 1 || packets[0].payload.size() != 2)
            return -1;
        return quint8(packets[0].payload[1]);
    };
    const auto prove = [&](quint32 key) {
        const auto ch = exchange(ct::CMD_ACCESS_CHALLENGE, QByteArray());
        if (ch.size() != 1)
            return false;
        QByteArray p;
        p.append(char(2));
        p.append(ct::accessResponse(ct::AccessKey(key), ch[0].payload));
        return expectAck(ct::CMD_ACCESS_RESPONSE, p);
    };

    // The LEGACY 6-byte payload lands in slot 1 - a pre-slot host keeps meaning
    // what it always meant.
    CHECK(writeKey(0, 0x11111111u, false, true));
    CHECK(slotMask() == 0x1);
    // Slots 3 and 4 through the new payload.
    CHECK(writeKey(3, 0x33333333u, false, false));
    CHECK(writeKey(4, 0x44444444u, false, false));
    CHECK(slotMask() == 0xD); // 1, 3, 4

    // ANY slot opens; a key in no slot does not.
    CHECK(prove(0x11111111u));
    CHECK(prove(0x33333333u));
    CHECK(prove(0x44444444u));
    CHECK(!prove(0x22222222u));

    // Clearing one slot leaves the others proving and the mask bit standing -
    // the bit means "at least one slot set".
    CHECK(writeKey(1, 0, true, false));
    CHECK(slotMask() == 0xC);
    CHECK(!prove(0x11111111u));
    CHECK(prove(0x33333333u));
    {
        const auto packets = exchange(ct::CMD_READ_ACCESS_KEYS, QByteArray());
        CHECK(packets.size() == 1 && packets[0].payload.size() == 2
              && (quint8(packets[0].payload[0]) & ct::ACCESS_MASK_EDIT_COMMS) != 0);
    }

    // Clearing the last two drops the function's bit with them.
    CHECK(writeKey(3, 0, true, false));
    CHECK(writeKey(4, 0, true, false));
    CHECK(slotMask() == 0x0);
    {
        const auto packets = exchange(ct::CMD_READ_ACCESS_KEYS, QByteArray());
        CHECK(packets.size() == 1 && packets[0].payload.size() == 2
              && (quint8(packets[0].payload[0]) & ct::ACCESS_MASK_EDIT_COMMS) == 0);
    }

    // A slot past the four is refused, not wrapped.
    {
        QByteArray p;
        p.append(char(2));
        p.append(char(0));
        p.append(QByteArray(4, char(0x55)));
        p.append(char(5));
        CHECK(expectNack(ct::CMD_WRITE_ACCESS_KEYS, p, ct::ERR_OUT_OF_BOUNDS));
    }
}

static void testMessagePasswordSlotSurvivesTheDevice(const SerialProtoCallbacks *restore)
{
    EngineCallbacks cb{};
    cb.transmit_can = captureTransmit;
    engine_init(&cb);
    serial_proto_init(restore);
    flashErase();

    const auto rec = [](quint32 id, quint8 slot) {
        ct::CanMessageConfig m{};
        m.can_id = id;
        m.flags = quint8(ct::MSGFLAG_ACTIVE | ct::MSGPROT_HIDDEN);
        m.src_bus = 1;
        m.dlc = 8;
        m.password_slot = slot;
        return m;
    };
    const auto put = [&](quint16 idx, const ct::CanMessageConfig &m) {
        QByteArray p;
        p.append(char(idx & 0xFF));
        p.append(char((idx >> 8) & 0xFF));
        p.append(char(1));
        p.append(char(0));
        p.append(reinterpret_cast<const char *>(&m), int(sizeof(m)));
        return p;
    };
    CHECK(expectAck(ct::CMD_WRITE_MSG_CFG, put(0, rec(0x640, 3))));
    CHECK(expectAck(ct::CMD_WRITE_MSG_CFG, put(1, rec(0x641, MSG_PASSWORD_SLOTS))));
    CHECK(expectAck(ct::CMD_WRITE_MSG_CFG, put(2, rec(0x642, MSG_PASSWORD_SLOTS + 1))));

    QByteArray req;
    req.append(char(0));
    req.append(char(0));
    req.append(char(3));
    req.append(char(0));
    const auto packets = exchange(ct::CMD_READ_MSG_CFG, req);
    CHECK(packets.size() == 1 && packets[0].cmd == ct::CMD_READ_MSG_CFG);
    const QByteArray all = packets.isEmpty() ? QByteArray() : packets[0].payload;
    CHECK(all.size() == 4 + 3 * int(sizeof(ct::CanMessageConfig)));
    if (all.size() == 4 + 3 * int(sizeof(ct::CanMessageConfig))) {
        ct::CanMessageConfig r[3]{};
        memcpy(r, all.constData() + 4, sizeof(r));
        CHECK(r[0].password_slot == 3);                      // came back intact
        CHECK(r[1].password_slot == MSG_PASSWORD_SLOTS); // the boundary holds
        CHECK(r[2].password_slot == 0);                      // one past it clamps
        CHECK((r[0].flags & ct::MSGPROT_MASK) == ct::MSGPROT_HIDDEN);
    }
}

static void testMessageProtectionIsHostOnly(const SerialProtoCallbacks *restore)
{
    EngineCallbacks cb{};
    cb.transmit_can = captureTransmit;
    engine_init(&cb);
    engine_set_access_keys(nullptr); // start from a device holding no passwords at all
    serial_proto_init(restore);
    flashErase();

    // `fill` lands in the ONE byte of the old per-message key[4] that is still
    // reserved. Store v10 claimed the other three for tx_trigger_cond and
    // tx_trigger_flags, so the scrub narrowed to this byte with them — and the
    // narrowing is exactly why filling it still has to be tested rather than
    // assumed. It is deliberately NON-ZERO where it is used: a host stashing
    // private data in a retired field would otherwise have the device hand it
    // back to every other host that asked, and a reserved field that is only
    // usually zero has stopped being reserved.
    const auto record = [](quint32 id, quint8 prot, quint8 fill) {
        ct::CanMessageConfig m{};
        m.can_id = id;
        m.flags = quint8(ct::MSGFLAG_ACTIVE | prot);
        m.src_bus = 1;
        m.dlc = 8;
        m.password_slot = fill;
        return m;
    };
    const auto writeOne = [](quint16 idx, const ct::CanMessageConfig &m) {
        QByteArray p;
        p.append(reinterpret_cast<const char *>(&idx), 2);
        const quint16 one = 1;
        p.append(reinterpret_cast<const char *>(&one), 2);
        p.append(reinterpret_cast<const char *>(&m), sizeof(m));
        return p;
    };
    const auto readRange = [](quint16 start, quint16 count) {
        QByteArray p;
        p.append(reinterpret_cast<const char *>(&start), 2);
        p.append(reinterpret_cast<const char *>(&count), 2);
        return p;
    };

    // Read one record back. Returns false only if the device did not answer with
    // a well-formed record at all, which is a different failure from the
    // record's contents being wrong and is worth telling apart.
    const auto readOne = [&](quint16 idx, ct::CanMessageConfig *out) {
        const auto packets = exchange(ct::CMD_READ_MSG_CFG, readRange(idx, 1));
        if (packets.size() != 1 || packets[0].cmd != ct::CMD_READ_MSG_CFG
            || packets[0].payload.size() < 4 + int(sizeof(ct::CanMessageConfig)))
            return false;
        std::memcpy(out, packets[0].payload.constData() + 4, sizeof(*out));
        return true;
    };
    // The device-wide key writes this function needs. Not shared with
    // testDeviceAccess's copy on purpose: that one is the fixture for the access
    // system itself, and a helper travelling between the two would let a change
    // made for one silently rewrite what the other is asserting.
    const auto writeKeyPayload = [](quint8 function, ct::AccessKey k) {
        ct::AccessKeyWritePayload p{};
        p.function = function;
        p.clear = 0;
        const QByteArray bytes = ct::accessKeyBytes(k);
        if (bytes.size() == ct::ACCESS_KEY_LEN)
            std::memcpy(p.key, bytes.constData(), ct::ACCESS_KEY_LEN);
        return QByteArray(reinterpret_cast<const char *>(&p), sizeof(p));
    };

    // ---- every tier installs freely, and comes back as ITSELF ----
    // Round-trip fidelity is the ONLY reason the level is on the wire. A Get
    // that dropped it would let the next Send store the message unmarked, which
    // is the laundering the transport exists to prevent — so each tier has to
    // survive as itself, not merely as "something protected".
    CHECK(expectAck(ct::CMD_WRITE_MSG_CFG,
                    writeOne(0, record(0x100, ct::MSGPROT_PROTECTED, 0xAB))));
    CHECK(expectAck(ct::CMD_WRITE_MSG_CFG, writeOne(1, record(0x200, ct::MSGPROT_HIDDEN, 0xCD))));
    CHECK(expectAck(ct::CMD_WRITE_MSG_CFG,
                    writeOne(2, record(0x300, ct::MSGPROT_READONLY, 0xEF))));
    CHECK(expectAck(ct::CMD_WRITE_MSG_CFG, writeOne(3, record(0x400, ct::MSGPROT_NONE, 0x5A))));
    {
        const auto packets = exchange(ct::CMD_READ_MSG_CFG, readRange(0, 4));
        CHECK(packets.size() == 1 && packets[0].cmd == ct::CMD_READ_MSG_CFG);
        if (packets.size() == 1
            && packets[0].payload.size() >= 4 + 4 * int(sizeof(ct::CanMessageConfig))) {
            const quint8 expected[4] = {ct::MSGPROT_PROTECTED, ct::MSGPROT_HIDDEN,
                                        ct::MSGPROT_READONLY, ct::MSGPROT_NONE};
            for (int i = 0; i < 4; ++i) {
                ct::CanMessageConfig got{};
                std::memcpy(&got, packets[0].payload.constData() + 4
                                      + i * int(sizeof(ct::CanMessageConfig)),
                            sizeof(got));
                // Marked records are READABLE, at every tier. Concealing the
                // details is the Manager's job and always was; refusing the read
                // made one marked message enough to lose a whole configuration.
                CHECK(got.can_id == quint32(0x100 * (i + 1)));
                CHECK((got.flags & ct::MSGPROT_MASK) == expected[i]);
                // The engine-evaluated bits are untouched by the level. That is
                // what makes the top two bits usable for this at all.
                CHECK(got.flags & ct::MSGFLAG_ACTIVE);
                // Byte 13 is password_slot as of store v16, and it is CLAMPED
                // rather than scrubbed: a value the device could not resolve to
                // one of its four message passwords comes back 0. The assertion
                // is the same shape it always was and it still protects the same
                // thing — a host cannot stash private data in the spare bits and
                // have the device hand it back — but the reason is now the range
                // check rather than a blanket blanking. A VALID slot survives;
                // that is testMessagePasswordSlotSurvivesTheDevice.
                CHECK(got.password_slot == 0);
                // And the three bytes beside it are NOT scrubbed any more. This
                // record went in with no trigger, so it comes back with none —
                // but it comes back because the device stored what it was sent,
                // not because something blanked the field on the way past. The
                // scrub narrowing is the whole reason Triggered transmit
                // survives a round trip at all.
                CHECK(got.tx_trigger_flags == 0);
            }
        }
    }

    // ---- INVERSE 1: an in-place rewrite of a Protected record is ACCEPTED ----
    // Identical bytes, same slot, so the ONLY variable is the gate: v20 answered
    // ERR_LOCKED here until the record's own key had been proved, and the same
    // write against an unmarked record was accepted. Now nothing distinguishes
    // them. (Identical rather than modifying because the store's idempotent
    // retransmit path is the only route back into a programmed slot — see the
    // note at the head of this function.)
    CHECK(expectAck(ct::CMD_WRITE_MSG_CFG,
                    writeOne(0, record(0x100, ct::MSGPROT_PROTECTED, 0xAB))));
    CHECK(expectAck(ct::CMD_WRITE_MSG_CFG, writeOne(1, record(0x200, ct::MSGPROT_HIDDEN, 0xCD))));

    // ---- INVERSE 2: setting Protected Comms changes nothing here ----
    // DECISIONS D3 declined a replacement write gate keyed on
    // ACCESS_FN_EDIT_COMMS — "keep enforcing writes on the device too" was
    // offered and not chosen — so this block guards that decision rather than
    // the deleted v20 code. A device holding a SET and UNPROVED EDIT_COMMS
    // password accepts every write above unchanged. If anybody adds
    // protectedWriteBlocked() back, this is what fails.
    //
    // EDIT_COMMS is still the password the HOST proves before it will let a user
    // untick a Protected box (MainWindow::proveProtectedCommsForEdit, then
    // Configuration::grantSectionAccess). The device answers the challenge; it
    // does not act on the answer.
    const ct::AccessKey editKey = ct::deriveAccessKey(QStringLiteral("edit-comms-secret"));
    CHECK(editKey != ct::kNoAccessKey);
    CHECK(expectAck(ct::CMD_WRITE_ACCESS_KEYS,
                    writeKeyPayload(ct::ACCESS_FN_EDIT_COMMS, editKey)));
    serial_proto_init(restore); // power cycle: the key is set and no longer proved
    {
        const auto packets = exchange(ct::CMD_READ_ACCESS_KEYS, QByteArray());
        CHECK(packets.size() == 1 && !packets[0].payload.isEmpty());
        if (packets.size() == 1 && !packets[0].payload.isEmpty())
            CHECK(quint8(packets[0].payload[0]) == ct::ACCESS_MASK_EDIT_COMMS);
    }
    CHECK(expectAck(ct::CMD_WRITE_MSG_CFG,
                    writeOne(0, record(0x100, ct::MSGPROT_PROTECTED, 0xAB))));

    // ---- INVERSE 3, the headline: CLEAR_CONFIG succeeds with an unproved
    //      Protected record stored ----
    // v21.1 refused this, reasoning that destroying a message is a way of
    // changing it. The spec says the opposite in as many words — removal is
    // permitted at every tier — and the old rule's real effect was that a unit
    // whose per-message password had been lost could be neither cleared nor
    // reconfigured short of a reflash that invalidated its store. Those units
    // are un-bricked by this release.
    //
    // The ACK alone would not be enough: an ACK for an erase that did not happen
    // is the failure engine_clear_config's own comment warns about, because the
    // host reads it as permission to stream records into flash it believes is
    // virgin. So the record has to be GONE afterwards.
    CHECK(expectAck(ct::CMD_CLEAR_CONFIG, QByteArray()));
    {
        ct::CanMessageConfig got{};
        CHECK(readOne(0, &got));
        CHECK(got.can_id == 0u);
        CHECK((got.flags & ct::MSGFLAG_ACTIVE) == 0);
        CHECK((got.flags & ct::MSGPROT_MASK) == ct::MSGPROT_NONE);
    }

    // ---- INVERSE 4: and the bits can then be written away ----
    // v20's single most important refusal was the write that CLEARS the level
    // bits — the write its gate existed to stop, and the one that pinned the
    // judge-the-STORED-record discipline. Here is the same message, at the same
    // index, coming back unprotected, with no password proved at any point.
    //
    // Two things this is deliberately NOT hiding. The sequence is clear-then-
    // Send rather than a direct overwrite, because that is what the flash allows
    // and what the Manager does anyway — and it is exactly the sequence that
    // walked past the v20 gate on real hardware, which is why the gate is gone
    // rather than merely relocated. And EDIT_COMMS is still set and still
    // unproved throughout: the tier's device password buys nothing here.
    CHECK(expectAck(ct::CMD_WRITE_MSG_CFG, writeOne(0, record(0x100, ct::MSGPROT_NONE, 0))));
    {
        ct::CanMessageConfig got{};
        CHECK(readOne(0, &got));
        CHECK(got.can_id == 0x100u);
        CHECK((got.flags & ct::MSGPROT_MASK) == ct::MSGPROT_NONE);
    }

    // ---- opcode 0x40 is RETIRED, and must answer ERR_INVALID_CMD ----
    // The one assertion the v20 function got right, kept verbatim and for the
    // same reason. Shipped 2.2.x Managers send CMD_MSG_ACCESS_RESPONSE before
    // every Send. ERR_INVALID_CMD maps to wrongPassword=false in
    // device_session.cpp, which those hosts read as "this message is keyless"
    // and walk past silently. ERR_LOCKED would set wrongPassword=true and trap
    // every one of them in a password prompt they cannot answer — the key they
    // would be proving does not exist anywhere any more — and cannot escape.
    // So 0x40 must keep falling through to default: sendNack(ERR_INVALID_CMD),
    // and must never be reused for anything.
    {
        const auto chal = exchange(ct::CMD_ACCESS_CHALLENGE, QByteArray());
        CHECK(chal.size() == 1);
        if (chal.size() == 1) {
            QByteArray body;
            const quint16 idx = 0;
            body.append(reinterpret_cast<const char *>(&idx), 2);
            body.append(ct::accessResponse(ct::deriveAccessKey(QStringLiteral("msgpass")),
                                           chal[0].payload));
            CHECK(expectNack(ct::CMD_MSG_ACCESS_RESPONSE, body, ct::ERR_INVALID_CMD));
        }
    }

    // ---- relays carry the tier too, and are equally ungated ----
    // Before 2.3.0 a relay section marked in the GUI reached the device bare:
    // the mapper's relay branch had no protection code at all, so a Get read the
    // rule back unmarked and the next Send stored it that way. The level rides
    // RelayConfig::flags under the same 0xC0 mask — free, because RELAYFLAG_*
    // only ever used bits 0..2 — and it must survive the round trip for the same
    // reason the message level must. Written here with EDIT_COMMS still set and
    // unproved, so this covers the relay half of INVERSE 2 as well.
    {
        const auto relayWrite = [](quint16 idx, const ct::RelayConfig &rl) {
            QByteArray p;
            const quint16 one = 1;
            p.append(reinterpret_cast<const char *>(&idx), 2);
            p.append(reinterpret_cast<const char *>(&one), 2);
            p.append(reinterpret_cast<const char *>(&rl), sizeof(rl));
            return p;
        };
        ct::RelayConfig marked{};
        marked.address = 0x600;
        marked.bitmask = 0x7FF;
        marked.flags = quint8(ct::RELAYFLAG_ACTIVE | ct::MSGPROT_PROTECTED);
        marked.src_bus = 1;
        marked.forward_bus_mask = 0x02;
        CHECK(expectAck(ct::CMD_WRITE_RELAY_CFG, relayWrite(0, marked)));

        // The same rule with the level stripped, into a slot of its own. Nothing
        // refuses it, and nothing refused the marked one either.
        ct::RelayConfig bare = marked;
        bare.address = 0x610;
        bare.flags = quint8(ct::RELAYFLAG_ACTIVE);
        CHECK(expectAck(ct::CMD_WRITE_RELAY_CFG, relayWrite(1, bare)));

        const auto packets = exchange(ct::CMD_READ_RELAY_CFG, readRange(0, 2));
        CHECK(packets.size() == 1 && packets[0].cmd == ct::CMD_READ_RELAY_CFG);
        if (packets.size() == 1
            && packets[0].payload.size() >= 4 + 2 * int(sizeof(ct::RelayConfig))) {
            ct::RelayConfig got0{}, got1{};
            std::memcpy(&got0, packets[0].payload.constData() + 4, sizeof(got0));
            std::memcpy(&got1, packets[0].payload.constData() + 4 + int(sizeof(ct::RelayConfig)),
                        sizeof(got1));
            CHECK(got0.address == 0x600u);
            CHECK((got0.flags & ct::MSGPROT_MASK) == ct::MSGPROT_PROTECTED);
            CHECK(got0.flags & ct::RELAYFLAG_ACTIVE);
            CHECK(got1.address == 0x610u);
            CHECK((got1.flags & ct::MSGPROT_MASK) == ct::MSGPROT_NONE);
            CHECK(got1.flags & ct::RELAYFLAG_ACTIVE);
        }
    }

    // ---- negative control: the gate that DOES still exist ----
    // Everything above is an acceptance, and a fixture unable to produce a
    // refusal would pass all of it while proving nothing. So install the
    // device-wide Send password, power-cycle to un-prove it, and watch the very
    // same CLEAR_CONFIG and the very same message write turn into ERR_LOCKED.
    // That is what makes "the clear was ACKed" mean "no message gate fired".
    //
    // It also states the one surviving rule plainly: CMD_CLEAR_CONFIG is gated
    // on ACCESS_FN_SEND and on nothing else. Whoever may Send may clear, at
    // every protection tier. (testDeviceAccess covers that gate in full; what is
    // here is only enough to prove this fixture can see it fire.)
    {
        const ct::AccessKey sendKey = ct::deriveAccessKey(QStringLiteral("send-me"));
        CHECK(expectAck(ct::CMD_WRITE_ACCESS_KEYS,
                        writeKeyPayload(ct::ACCESS_FN_SEND, sendKey)));
        serial_proto_init(restore);
        CHECK(expectNack(ct::CMD_CLEAR_CONFIG, QByteArray(), ct::ERR_LOCKED));
        // Slot 1, not some arbitrary index: the clear above left one record
        // live, and the firmware refuses a write that would open a GAP in the
        // active prefix (start > engine_table_used) with ERR_OUT_OF_BOUNDS. That
        // check sits after the access check, so a hole would still NACK
        // ERR_LOCKED here and would then fail the acceptance below for a reason
        // having nothing to do with any gate.
        CHECK(expectNack(ct::CMD_WRITE_MSG_CFG, writeOne(1, record(0x500, ct::MSGPROT_NONE, 0)),
                         ct::ERR_LOCKED));
        // ...and proving it puts both back, so the refusals above were the Send
        // gate rather than some other accident of the fixture.
        const auto chal = exchange(ct::CMD_ACCESS_CHALLENGE, QByteArray());
        CHECK(chal.size() == 1);
        if (chal.size() == 1)
            CHECK(expectAck(ct::CMD_ACCESS_RESPONSE,
                            QByteArray(1, char(ct::ACCESS_FN_SEND))
                                + ct::accessResponse(sendKey, chal[0].payload)));
        CHECK(expectAck(ct::CMD_WRITE_MSG_CFG,
                        writeOne(1, record(0x500, ct::MSGPROT_PROTECTED, 0))));
        CHECK(expectAck(ct::CMD_CLEAR_CONFIG, QByteArray()));
    }

    // Put the fixture back as it was found: no passwords, real callbacks, empty
    // flash. A later test finding a locked device would fail for reasons that
    // have nothing to do with what it is testing.
    engine_set_access_keys(nullptr);
    serial_proto_init(restore);
    flashErase();
}

// The ACK CRC echo, both halves: the firmware puts the request's CRC in its ACK,
// and the host uses it to throw away a duplicate left over from a retransmit
// instead of completing the wrong command. This is the fix for the stale-ACK
// desync — two writes back to back, the first retransmitted, its late duplicate
// ACK arriving while the second is in flight.
static void testAckCrcEcho(const SerialProtoCallbacks *restore)
{
    EngineCallbacks cb{};
    cb.transmit_can = captureTransmit;
    engine_init(&cb);
    engine_set_access_keys(nullptr);
    serial_proto_init(restore);
    flashErase();

    // Two DIFFERENT commands, so their frame CRCs differ — the essential
    // property the de-duplication rides on.
    const QByteArray nameA = QByteArray("Config A").leftJustified(ct::CONFIG_NAME_LEN, char(0));
    const QByteArray nameB = QByteArray("Config B").leftJustified(ct::CONFIG_NAME_LEN, char(0));
    const quint16 crcA = ct::frameCrc(ct::CMD_WRITE_CONFIG_NAME, nameA);
    const quint16 crcB = ct::frameCrc(ct::CMD_WRITE_CONFIG_NAME, nameB);
    CHECK(crcA != crcB);

    // The firmware really echoes each request's own CRC.
    CHECK(expectAck(ct::CMD_WRITE_CONFIG_NAME, nameA));
    CHECK(expectAck(ct::CMD_WRITE_CONFIG_NAME, nameB));

    // The host's rule, exercised directly. Build the ACK bytes the firmware
    // sends for request A: [ERR_OK, crcA_hi, crcA_lo].
    auto ackFor = [](quint16 crc) {
        QByteArray p(3, char(0));
        p[1] = char((crc >> 8) & 0xFF);
        p[2] = char(crc & 0xFF);
        return p;
    };
    // A's ACK completes A.
    CHECK(ct::DeviceLink::replyEchoMatches(ackFor(crcA), crcA));
    // A's DUPLICATE ACK, arriving while B is in flight, must NOT complete B —
    // this is the whole bug: without the echo it would.
    CHECK(!ct::DeviceLink::replyEchoMatches(ackFor(crcA), crcB));
    // B's own ACK completes B.
    CHECK(ct::DeviceLink::replyEchoMatches(ackFor(crcB), crcB));
    // A 1-byte ACK (firmware that predates the echo) is accepted against any
    // request — old behaviour, no de-duplication but no regression.
    CHECK(ct::DeviceLink::replyEchoMatches(QByteArray(1, char(0)), crcB));
    // Malformed lengths are not a valid answer.
    CHECK(!ct::DeviceLink::replyEchoMatches(QByteArray(), crcA));
    CHECK(!ct::DeviceLink::replyEchoMatches(QByteArray(2, char(0)), crcA));

    serial_proto_init(restore);
}

static void testAccessKeyDurability(const SerialProtoCallbacks *restore)
{
    EngineCallbacks cb{};
    cb.transmit_can = captureTransmit;
    engine_init(&cb);
    engine_set_access_keys(nullptr);
    serial_proto_init(restore);
    // A virgin header for the write to commit into. This is exactly the state a
    // device is in before its first password is ever set.
    engine_clear_config();
    CHECK(!flash_store_validate(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr));

    const ct::AccessKey sendKey = ct::deriveAccessKey(QStringLiteral("stick-around"));
    const QByteArray sendKeyBytes = ct::accessKeyBytes(sendKey);
    ct::AccessKeyWritePayload set{};
    set.function = ct::ACCESS_FN_SEND;
    std::memcpy(set.key, sendKeyBytes.constData(), ct::ACCESS_KEY_LEN);
    CHECK(expectAck(ct::CMD_WRITE_ACCESS_KEYS,
                    QByteArray(reinterpret_cast<const char *>(&set), sizeof(set))));

    // The key is in force for this session IMMEDIATELY â€” that part is not in
    // doubt and never was.
    CHECK(engine_access_keys()->set_mask == ct::ACCESS_MASK_SEND);

    // ...but it is NOT in flash yet, and this is the assertion that pins the
    // real contract rather than the one we wish were true. The keys live in the
    // flash header, and STM32 flash programs each doubleword once per erase, so
    // WRITE_ACCESS_KEYS cannot commit on its own: a header already written since
    // the last erase cannot be rewritten. Committing here would work for the
    // first password after an erase and fail for every change after it, which is
    // worse than not committing at all â€” it works exactly long enough to be
    // trusted. So the key reaches flash with the next configuration commit, and
    // CAN Triple Device Manager says so at the moment the password is set.
    //
    // If this line ever starts failing because someone gave the keys a flash
    // page of their own (the right fix, an append-log like preserve_store.c),
    // delete it and assert durability directly. Until then it is load-bearing:
    // it stops the "obvious" one-line commit being reintroduced.
    CHECK(!flash_store_validate(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr));

    // A commit is what makes it durable. This is the sequence CAN Triple
    // Device Manager performs, and the only one that persists a password.
    CHECK(expectAck(ct::CMD_SAVE_TO_FLASH, QByteArray()));
    {
        ::AccessKeyRecord stored{};
        CHECK(flash_store_validate(nullptr, nullptr, nullptr, &stored, nullptr, nullptr, nullptr, nullptr));
        CHECK(stored.set_mask == ct::ACCESS_MASK_SEND);
        CHECK(std::memcmp(stored.keys[ct::ACCESS_FN_SEND], sendKeyBytes.constData(),
                          ct::ACCESS_KEY_LEN)
              == 0);
    }

    // ...and the power cycle it exists to survive: drop the RAM copy, reload
    // from the image, and the device is still protected.
    engine_set_access_keys(nullptr);
    CHECK(engine_access_keys()->set_mask == 0);
    CHECK(engine_load_config(nullptr));
    CHECK(engine_access_keys()->set_mask == ct::ACCESS_MASK_SEND);
    serial_proto_init(nullptr); // a fresh session has proved nothing
    CHECK(expectNack(ct::CMD_WRITE_MSG_CFG, QByteArray(4, char(0)), ct::ERR_LOCKED));

    // Put the fixture back by hand rather than over the wire: clearing the
    // password would want a second commit, and the header is already programmed.
    engine_set_access_keys(nullptr);
    engine_set_config_version(0);
    flashErase();
    serial_proto_init(restore);
}

// v18 device binding: a configuration carrying another chip's unique ID must
// not run. This is the copy-protection scenario end to end â€” commit an image
// bound to device A, then present it to device B and watch the store refuse it,
// with a status that says WHY rather than "no configuration".
static void testDeviceBinding(const SerialProtoCallbacks *restore)
{
    static_assert(ct::CONFIG_UID_LEN == 12, "UID is the 96-bit STM32 unique ID");
    // Both headers must mean the same thing by it, like every other shared
    // constant here.
    CHECK(ct::CONFIG_UID_LEN == fw::kUidLen);
    CHECK(ct::CONFIG_STATUS_OK == fw::kStatusOk);
    CHECK(ct::CONFIG_STATUS_NONE == fw::kStatusNone);
    CHECK(ct::CONFIG_STATUS_WRONG_DEVICE == fw::kStatusWrongDevice);
    CHECK(ct::CMD_GET_DEVICE_ID == fw::kCmdGetDeviceId);
    CHECK(ct::CMD_WRITE_CONFIG_BINDING == fw::kCmdWriteBinding);

    uint8_t uidA[ct::CONFIG_UID_LEN];
    uint8_t uidB[ct::CONFIG_UID_LEN];
    for (int i = 0; i < ct::CONFIG_UID_LEN; ++i) {
        uidA[i] = uint8_t(0x10 + i);
        uidB[i] = uint8_t(0x90 + i); // a different part
    }
    uint16_t counts[FLASH_NUM_TABLES] = {0};

    // ---- unbound (all-zero) runs anywhere: every pre-v18 config ---------
    {
        flash_store_set_device_uid(uidA);
        flashErase();
        CHECK(flash_store_commit(counts, nullptr, nullptr, nullptr, 0, nullptr, nullptr, nullptr));
        CHECK(flash_store_validate(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr));
        CHECK(flash_store_config_status() == ct::CONFIG_STATUS_OK);
        flash_store_set_device_uid(uidB); // same image, different chip
        CHECK(flash_store_validate(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr));
        CHECK(flash_store_config_status() == ct::CONFIG_STATUS_OK);
    }

    // ---- bound to A: runs on A ----------------------------------------
    {
        flash_store_set_device_uid(uidA);
        flashErase();
        CHECK(flash_store_commit(counts, nullptr, nullptr, nullptr, 0, uidA, nullptr, nullptr));
        uint8_t readBack[ct::CONFIG_UID_LEN] = {0};
        CHECK(flash_store_validate(nullptr, nullptr, nullptr, nullptr, nullptr, readBack, nullptr, nullptr));
        CHECK(flash_store_config_status() == ct::CONFIG_STATUS_OK);
        CHECK(std::memcmp(readBack, uidA, ct::CONFIG_UID_LEN) == 0);
    }

    // ---- ...and the SAME image refuses to run on B ---------------------
    // Nothing about the flash contents changed here: this is exactly what
    // happens when a flash image is lifted off one device and written to
    // another, which is the case the binding exists for.
    {
        flash_store_set_device_uid(uidB);
        CHECK(!flash_store_validate(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr));
        CHECK(flash_store_config_status() == ct::CONFIG_STATUS_WRONG_DEVICE);
        CHECK(!flash_store_present());
        // The engine must come up empty rather than running someone else's
        // messages.
        CHECK(!engine_load_config(nullptr));
    }

    // ---- a device with no identity refuses a bound image (fails closed) --
    {
        flash_store_set_device_uid(nullptr);
        CHECK(!flash_store_validate(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr));
        CHECK(flash_store_config_status() == ct::CONFIG_STATUS_WRONG_DEVICE);
    }

    // ---- corrupt beats bound: a damaged image reports NONE, not the wrong
    // device, so the two diagnoses never get confused --------------------
    {
        flash_store_set_device_uid(uidA);
        CHECK(flash_store_validate(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr));
        g_flash[8] ^= 0xFF; // inside the counts, so the CRC fails
        CHECK(!flash_store_validate(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr));
        CHECK(flash_store_config_status() == ct::CONFIG_STATUS_NONE);
        g_flash[8] ^= 0xFF;
        CHECK(flash_store_validate(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr));
    }

    // ---- over the wire: GET_DEVICE_ID reports identity + why ------------
    {
        SerialProtoCallbacks cb = *restore;
        cb.device_uid = [](uint8_t out[ct::CONFIG_UID_LEN]) {
            for (int i = 0; i < ct::CONFIG_UID_LEN; ++i)
                out[i] = uint8_t(0x10 + i); // device A
        };
        serial_proto_init(&cb);

        flash_store_set_device_uid(uidA);
        CHECK(flash_store_validate(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr));
        auto packets = exchange(ct::CMD_GET_DEVICE_ID, QByteArray());
        CHECK(packets.size() == 1);
        CHECK(packets[0].payload.size() == ct::CONFIG_UID_LEN + 1);
        CHECK(std::memcmp(packets[0].payload.constData(), uidA, ct::CONFIG_UID_LEN) == 0);
        CHECK(quint8(packets[0].payload[ct::CONFIG_UID_LEN]) == ct::CONFIG_STATUS_OK);

        // The GUI's parser against the firmware's own bytes.
        ct::device_session::Identity parsed;
        CHECK(ct::device_session::parseIdentity(packets[0].payload, &parsed));
        CHECK(parsed.supported);
        CHECK(parsed.configStatus == ct::CONFIG_STATUS_OK);
        CHECK(!parsed.boundElsewhere());
        CHECK(parsed.uid == QByteArray(reinterpret_cast<const char *>(uidA), ct::CONFIG_UID_LEN));
        // Printed most-significant-byte first, so two units sort and compare
        // the way a human reads them.
        CHECK(parsed.uidText() == QStringLiteral("1B1A19181716151413121110"));
        CHECK(!ct::device_session::parseIdentity(packets[0].payload.left(3), &parsed));

        // Present the same image to a different chip: the host is told the
        // configuration belongs elsewhere, not that there isn't one.
        flash_store_set_device_uid(uidB);
        (void)flash_store_validate(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
        packets = exchange(ct::CMD_GET_DEVICE_ID, QByteArray());
        CHECK(packets.size() == 1);
        CHECK(quint8(packets[0].payload[ct::CONFIG_UID_LEN]) == ct::CONFIG_STATUS_WRONG_DEVICE);
        // This is what the Device Manager keys its "belongs to a different
        // CAN Triple" message off, so it has to come through the parser too.
        ct::device_session::Identity elsewhere;
        CHECK(ct::device_session::parseIdentity(packets[0].payload, &elsewhere));
        CHECK(elsewhere.boundElsewhere());

        // Binding a configuration to a chip changes what the device will run,
        // so it is a write like any other: it answers with an ACK, and on a
        // device carrying a Send password it would answer ERR_LOCKED (there is
        // none set here, which is what makes the ACK meaningful).
        QByteArray bind(reinterpret_cast<const char *>(uidB), ct::CONFIG_UID_LEN);
        CHECK(expectAck(ct::CMD_WRITE_CONFIG_BINDING, bind));
        CHECK(expectNack(ct::CMD_WRITE_CONFIG_BINDING, QByteArray(4, char(0)),
                         ct::ERR_INVALID_LEN));
        CHECK(std::memcmp(engine_config_binding(), uidB, ct::CONFIG_UID_LEN) == 0);
        // Re-binding to this chip makes the image run here â€” the legitimate
        // "hardware was replaced" path. The erase is not incidental: STM32
        // flash programs each location once between erases, so a re-commit
        // without one fails, exactly as it would on the device.
        flashErase();
        uint16_t zeroCounts[FLASH_NUM_TABLES] = {0};
        CHECK(flash_store_commit(zeroCounts, nullptr, nullptr, nullptr, 0,
                                 engine_config_binding(), nullptr, nullptr));
        CHECK(flash_store_validate(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr));
        CHECK(flash_store_config_status() == ct::CONFIG_STATUS_OK);
    }

    // Leave the fixture unbound so nothing after this trips over it.
    engine_set_config_binding(nullptr);
    flashErase();
    serial_proto_init(restore);
}

// The 7.37 Mbaud soak failure, replayed end to end. Roughly one WRITE chunk in
// 250 died on the wire AFTER its records had landed (a config-store program in
// the same bank the core executes from stalls the CPU, the RX ring overruns,
// the response is lost) and the host retransmitted â€” straight into slots whose
// one program per erase the first attempt had already spent. Real flash
// answers that with PROGERR, and the firmware then compounded it by NACKing
// ERR_OUT_OF_BOUNDS, which sent the diagnosis chasing range bugs. So three
// promises are pinned here: a byte-identical retransmit is an ACK and a no-op;
// DIFFERENT bytes into programmed slots without an erase fail, and fail as
// ERR_FLASH_WRITE; and a genuine range violation still answers
// ERR_OUT_OF_BOUNDS, so the two diagnoses can never be confused again.
static void testRetransmitSafety()
{
    CHECK(expectAck(ct::CMD_CLEAR_CONFIG, {}));

    QVector<ct::ConstantConfig> consts(2);
    consts[0].dest_signal_idx = 1;
    consts[0].value = 42.5f;
    consts[0].is_active = 1;
    consts[1].dest_signal_idx = 2;
    consts[1].value = -7.0f;
    consts[1].is_active = 1;
    const QByteArray chunk = writeChunk(quint16(0), consts, 0, 2);
    CHECK(expectAck(ct::CMD_WRITE_CONST_CFG, chunk));

    // The lost-ACK retransmit: the identical frame again. On the old firmware
    // this is the double-program that PROGERRed on the bench; now it must ACK
    // without touching flash...
    CHECK(expectAck(ct::CMD_WRITE_CONST_CFG, chunk));

    // ...and the readback must be byte-identical to what was sent â€” a skip
    // that ACKed but left other bytes behind would be worse than the failure.
    const QByteArray rb = readRange(ct::CMD_READ_CONST_CFG, 0, 2);
    CHECK(rb.size() == 4 + 2 * int(sizeof(ct::ConstantConfig)));
    CHECK(std::memcmp(rb.constData() + 4, consts.constData(),
                      2 * sizeof(ct::ConstantConfig))
          == 0);

    // Different bytes into the same slots without an erase is NOT a retransmit
    // â€” it is a payload flash physically cannot hold (programming only clears
    // bits). The write must fail, and it must fail as a FLASH refusal, because
    // the indices are perfectly in range.
    consts[0].value = 43.0f;
    CHECK(expectNack(ct::CMD_WRITE_CONST_CFG, writeChunk(quint16(0), consts, 0, 2),
                     ct::ERR_FLASH_WRITE));

    // And the slots still hold the FIRST payload â€” the failed write changed
    // nothing, which is what lets the host trust the NACK and start over with
    // a CLEAR instead of wondering what state it left behind.
    const QByteArray rb2 = readRange(ct::CMD_READ_CONST_CFG, 0, 1);
    CHECK(rb2.size() == 4 + int(sizeof(ct::ConstantConfig)));
    float held = 0.0f;
    std::memcpy(&held, rb2.constData() + 4 + offsetof(ct::ConstantConfig, value),
                sizeof(held));
    CHECK(held == 42.5f);

    // A genuine range violation keeps its own answer: ERR_OUT_OF_BOUNDS means
    // "no such slot", never "the flash would not take it".
    CHECK(expectNack(ct::CMD_WRITE_CONST_CFG,
                     writeChunk(quint16(ct::MAX_CONSTANTS), consts, 0, 1),
                     ct::ERR_OUT_OF_BOUNDS));

    // Leave the region erased so nothing after this inherits half a table.
    CHECK(expectAck(ct::CMD_CLEAR_CONFIG, {}));
}

// ---------------------------------------------------------------- firmware update
//
// Drives the real fw_update.c and bcb.c against a NOR-accurate RAM model of
// bank 2 (test/fw_host_stub.c). Worth doing off-hardware because the failure
// mode of this subsystem is a device that will not boot, and most of what can
// go wrong is pure logic: which sizes are refused, which offsets leave a hole,
// whether END verifies what actually landed rather than what was promised.

// Build a valid image the way ctfpack does: fill the header, write the size,
// THEN compute the CRC over everything except the CRC field itself. The order
// matters â€” image_size is inside the CRC's span.
static QByteArray makeFirmwareImage(quint32 size, quint16 major, quint16 minor,
                                    quint16 patch, quint16 productId = FW_PRODUCT_CAN_TRIPLE)
{
    QByteArray img(int(size), char(0));
    for (int i = 0; i < img.size(); ++i)
        img[i] = char((i * 7 + 11) & 0xFF); // recognisable filler

    FwImageHeader hdr{};
    hdr.magic = FW_IMAGE_MAGIC;
    hdr.header_version = FW_IMAGE_HDR_VERSION;
    hdr.product_id = productId;
    hdr.image_size = size;
    hdr.image_crc32 = 0;
    hdr.fw_version_major = major;
    hdr.fw_version_minor = minor;
    hdr.fw_version_patch = patch;
    hdr.flash_store_version = FLASH_STORE_VERSION;
    hdr.min_bootloader_version = 1;
    std::memcpy(img.data() + FW_IMAGE_HEADER_OFFSET, &hdr, sizeof(hdr));

    const quint32 crcOff = FW_IMAGE_CRC_OFFSET;
    quint32 crc = fw_crc32_update(FW_CRC32_INIT, img.constData(), crcOff);
    crc = fw_crc32_update(crc, img.constData() + crcOff + 4, size - crcOff - 4);
    crc = fw_crc32_final(crc);
    std::memcpy(img.data() + crcOff, &crc, sizeof(crc));
    return img;
}

static QByteArray beginPayload(const QByteArray &image,
                               quint16 productId = FW_PRODUCT_CAN_TRIPLE)
{
    FwImageHeader hdr{};
    std::memcpy(&hdr, image.constData() + FW_IMAGE_HEADER_OFFSET, sizeof(hdr));

    ct::FwUpdateBeginPayload p{};
    p.image_size = quint32(image.size());
    p.image_crc32 = hdr.image_crc32;
    p.product_id = productId;
    p.version_major = hdr.fw_version_major;
    p.version_minor = hdr.fw_version_minor;
    p.version_patch = hdr.fw_version_patch;
    return QByteArray(reinterpret_cast<const char *>(&p), sizeof(p));
}

static QByteArray dataPayload(quint32 offset, const QByteArray &image, int from, int len)
{
    QByteArray out;
    out.append(reinterpret_cast<const char *>(&offset), 4);
    out.append(image.constData() + from, len);
    return out;
}

// Send the whole image in chunks the way FirmwareUpdater does.
static bool sendAllChunks(const QByteArray &image)
{
    constexpr int kChunk = 488;
    const int total = int(image.size());
    for (int off = 0; off < total; off += kChunk) {
        const int n = std::min(kChunk, total - off);
        if (!expectAck(ct::CMD_FW_UPDATE_DATA, dataPayload(quint32(off), image, off, n)))
            return false;
    }
    return true;
}

static ct::FwUpdateStatus readFwStatus()
{
    const auto packets = exchange(ct::CMD_FW_UPDATE_STATUS, {});
    ct::FwUpdateStatus st{};
    for (const ct::Packet &p : packets) {
        if (p.cmd == ct::CMD_FW_UPDATE_STATUS
            && p.payload.size() == int(sizeof(st))) {
            std::memcpy(&st, p.payload.constData(), sizeof(st));
        }
    }
    return st;
}

static void testFirmwareUpdate()
{
    fw_host_reset();
    static const FwUpdateDriver drv = {fw_host_erase, fw_host_program};
    static const BcbDriver bcbDrv = {fw_host_erase, fw_host_program};
    bcb_init(&bcbDrv);
    fw_update_init(&drv);

    const QByteArray image = makeFirmwareImage(8192, 3, 1, 4);

    // --- the shape of the status reply, and that it is classified as a read ---
    CHECK(ct::DeviceLink::isReadResponse(ct::CMD_FW_UPDATE_STATUS));
    {
        const ct::FwUpdateStatus st = readFwStatus();
        CHECK(st.app_base == FW_APP_BASE);
        CHECK(st.staging_capacity == FW_STAGING_SIZE);
        CHECK(st.running_store_version == FLASH_STORE_VERSION);
        CHECK(st.state == FW_STATE_IDLE);
        CHECK(st.staged_valid == 0); // nothing staged yet
    }

    // --- refusals that must happen BEFORE any flash is touched ---------------
    // A foreign product. Installing another board's firmware is the mistake
    // with the worst outcome and the least obvious symptom, so it is refused on
    // the declaration rather than after a whole transfer.
    CHECK(expectNack(ct::CMD_FW_UPDATE_BEGIN,
                     beginPayload(image, 0x1234), ct::ERR_FW_REJECTED));
    // Larger than the staging slot.
    {
        ct::FwUpdateBeginPayload p{};
        p.image_size = FW_STAGING_SIZE + 8;
        p.product_id = FW_PRODUCT_CAN_TRIPLE;
        CHECK(expectNack(ct::CMD_FW_UPDATE_BEGIN,
                         QByteArray(reinterpret_cast<const char *>(&p), sizeof(p)),
                         ct::ERR_OUT_OF_BOUNDS));
    }
    // Not a whole number of doublewords â€” this part cannot program it.
    {
        ct::FwUpdateBeginPayload p{};
        p.image_size = 8194;
        p.product_id = FW_PRODUCT_CAN_TRIPLE;
        CHECK(expectNack(ct::CMD_FW_UPDATE_BEGIN,
                         QByteArray(reinterpret_cast<const char *>(&p), sizeof(p)),
                         ct::ERR_OUT_OF_BOUNDS));
    }
    // DATA before BEGIN has no declared size to bound it against.
    CHECK(expectNack(ct::CMD_FW_UPDATE_DATA, dataPayload(0, image, 0, 8),
                     ct::ERR_INVALID_CMD));

    // --- the happy path ------------------------------------------------------
    CHECK(expectAck(ct::CMD_FW_UPDATE_BEGIN, beginPayload(image)));

    // A chunk starting past the filled mark would strand the bytes before it.
    // The CRC at END would eventually catch that, but only after the whole
    // transfer, and it would look like a corrupt file rather than a transport
    // bug â€” so it is refused where it happens.
    CHECK(expectNack(ct::CMD_FW_UPDATE_DATA, dataPayload(4096, image, 4096, 8),
                     ct::ERR_OUT_OF_BOUNDS));
    // Misaligned offset and misaligned length, both refused.
    CHECK(expectNack(ct::CMD_FW_UPDATE_DATA, dataPayload(4, image, 0, 8),
                     ct::ERR_INVALID_LEN));
    CHECK(expectNack(ct::CMD_FW_UPDATE_DATA, dataPayload(0, image, 0, 5),
                     ct::ERR_INVALID_LEN));
    // Running off the end of the declared image.
    CHECK(expectNack(ct::CMD_FW_UPDATE_DATA,
                     dataPayload(quint32(image.size() - 8), image, 0, 16),
                     ct::ERR_OUT_OF_BOUNDS));

    // END before every byte has arrived must refuse rather than verify a
    // half-written slot.
    CHECK(expectAck(ct::CMD_FW_UPDATE_DATA, dataPayload(0, image, 0, 488)));
    CHECK(expectNack(ct::CMD_FW_UPDATE_END, {}, ct::ERR_OUT_OF_BOUNDS));

    // That refusal ends the session, so start over and send it all.
    CHECK(expectAck(ct::CMD_FW_UPDATE_BEGIN, beginPayload(image)));
    CHECK(sendAllChunks(image));

    // Re-sending a chunk already delivered is what a retransmit after a lost
    // ACK looks like. It must ACK and, because the bytes are already correct,
    // cost no flash writes at all â€” the property that keeps a retransmit from
    // PROGERRing the way the config store once did.
    const int writesBefore = fw_host_doublewords_written();
    CHECK(expectAck(ct::CMD_FW_UPDATE_DATA, dataPayload(0, image, 0, 488)));
    CHECK(fw_host_doublewords_written() == writesBefore);

    CHECK(expectAck(ct::CMD_FW_UPDATE_END, {}));

    // The staged bytes must be byte-identical to the file.
    {
        const uint8_t *staged = fw_host_at(FW_STAGING_BASE, quint32(image.size()));
        CHECK(staged != nullptr);
        CHECK(std::memcmp(staged, image.constData(), size_t(image.size())) == 0);
    }

    // And the boot control block must now be armed with THIS image's size and
    // CRC, attempts reset. A fresh image gets a fresh set of attempts, or a
    // previous failure would eat the new one's chances.
    {
        const ct::FwUpdateStatus st = readFwStatus();
        CHECK(st.state == FW_STATE_PENDING);
        CHECK(st.attempts == 0);
        CHECK(st.staged_size == quint32(image.size()));
        CHECK(st.staged_valid == 1);

        FwImageHeader hdr{};
        std::memcpy(&hdr, image.constData() + FW_IMAGE_HEADER_OFFSET, sizeof(hdr));
        CHECK(st.staged_crc32 == hdr.image_crc32);
    }

    // --- END must verify what LANDED, not what was promised ------------------
    // Send an image, then corrupt one staged byte behind the protocol's back
    // and re-run END. This is the transport fault that slipped past the
    // per-frame CRC, and it must be caught while the running firmware is still
    // intact and able to report it.
    {
        fw_host_reset();
        CHECK(expectAck(ct::CMD_FW_UPDATE_BEGIN, beginPayload(image)));
        CHECK(sendAllChunks(image));
        uint8_t *staged = fw_host_at(FW_STAGING_BASE + 1024, 1);
        CHECK(staged != nullptr);
        *staged ^= 0x01;
        CHECK(expectNack(ct::CMD_FW_UPDATE_END, {}, ct::ERR_FW_REJECTED));
    }

    // --- a declaration that does not describe the image sent -----------------
    // The bytes are internally valid, but they are not the file BEGIN said was
    // coming. Refused: succeeding here would install something other than what
    // the operator chose.
    {
        fw_host_reset();
        const QByteArray other = makeFirmwareImage(8192, 9, 9, 9);
        CHECK(expectAck(ct::CMD_FW_UPDATE_BEGIN, beginPayload(image)));
        CHECK(sendAllChunks(other));
        CHECK(expectNack(ct::CMD_FW_UPDATE_END, {}, ct::ERR_FW_REJECTED));
    }

    // --- abort must make the staged image UNINSTALLABLE ----------------------
    // Clearing the pending flag is not enough: the bootloader installs a valid
    // staged image on its own initiative when the application slot is damaged,
    // so an aborted image left intact could be installed later without anyone
    // asking. Erasing the header page is what actually retires it.
    {
        fw_host_reset();
        CHECK(expectAck(ct::CMD_FW_UPDATE_BEGIN, beginPayload(image)));
        CHECK(sendAllChunks(image));
        CHECK(expectAck(ct::CMD_FW_UPDATE_END, {}));
        CHECK(readFwStatus().state == FW_STATE_PENDING);

        CHECK(expectAck(ct::CMD_FW_UPDATE_ABORT, {}));
        const ct::FwUpdateStatus st = readFwStatus();
        CHECK(st.state == FW_STATE_IDLE);
        CHECK(st.staged_valid == 0);
    }

    // --- the boot control block survives its own page filling ----------------
    // Records are appended, and when a page fills the OTHER page is erased,
    // written, and only then is the old one dropped. Walk well past one page of
    // slots and require the state to be readable at every step â€” a swap that
    // erased first would show up here as a lost record.
    {
        fw_host_reset();
        for (int i = 0; i < FW_BCB_SLOTS_PER_PAGE * 2 + 5; ++i) {
            const quint8 state = (i % 2) ? FW_STATE_PENDING : FW_STATE_IDLE;
            CHECK(bcb_set_result(state, quint8(i & 0xFF), FW_RESULT_OK));
            BootControlRecord rec{};
            CHECK(bcb_read(&rec));
            CHECK(rec.state == state);
            CHECK(rec.attempts == quint8(i & 0xFF));
        }
    }

    fw_host_reset();
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    testFirmwareSha256();

    // ---- v15: the GUI and the firmware each hand-pack CanSignalConfig's bit
    // fields from their own header. Those two implementations ARE the wire
    // format, so they must agree exactly â€” a one-bit disagreement would corrupt
    // every signal silently. This is the only place both headers are visible,
    // so it is the only place that can prove it: pack with the GUI's setters,
    // reinterpret the raw bytes as the firmware's struct, read back with the
    // firmware's accessors. ----
    {
        static_assert(sizeof(ct::CanSignalConfig) == sizeof(::CanSignalConfig),
                      "GUI and firmware signal records differ in size");
        // 32 (label) + 20 (five floats) + 8 (four u16) + 4 (bits) = 64, and
        // PAD8(64) is 64 â€” the record fills its flash slot exactly, with no
        // padding waste, which is why the label could go back to 32 bytes
        // without costing a single byte beyond the label itself.
        static_assert(sizeof(ct::CanSignalConfig) == 64,
                      "CanSignalConfig must be 64 bytes on the wire");
        // Every offset pinned to a literal on BOTH sides. sizeof agreeing is
        // not enough: widening the label by 16 and losing 16 somewhere else
        // would still be 64 bytes, and the resulting record would put a
        // channel name's tail where `factor` belongs â€” every signal scaled by
        // a float assembled from text. The literals below ARE the wire format.
#define SIG_OFFSET_AGREES(field, off)                                                              \
    static_assert(offsetof(ct::CanSignalConfig, field) == (off)                                    \
                      && offsetof(::CanSignalConfig, field) == (off),                              \
                  "CanSignalConfig." #field " must sit at offset " #off " in both headers")
        SIG_OFFSET_AGREES(label, 0);
        SIG_OFFSET_AGREES(factor, 32);
        SIG_OFFSET_AGREES(offset, 36);
        SIG_OFFSET_AGREES(min_val, 40);
        SIG_OFFSET_AGREES(max_val, 44);
        SIG_OFFSET_AGREES(default_value, 48);
        SIG_OFFSET_AGREES(mux_id, 52);
        SIG_OFFSET_AGREES(mux_mask, 54);
        SIG_OFFSET_AGREES(msg_and_flags, 56);
        SIG_OFFSET_AGREES(tx_source, 58);
        SIG_OFFSET_AGREES(bits, 60);
#undef SIG_OFFSET_AGREES
        // The label width itself is named on both sides, and the host's
        // name-length budget is derived from it rather than typed in â€” a
        // hand-written 31 somewhere would survive a future widening and start
        // truncating names the device could hold.
        static_assert(sizeof(ct::CanSignalConfig::label) == 32
                          && sizeof(::CanSignalConfig::label) == 32,
                      "the label field itself is 32 bytes in both headers");
        CHECK(ct::SIGNAL_LABEL_LEN == fw::kSignalLabelLen);
        CHECK(ct::SIGNAL_LABEL_LEN == 32);
        CHECK(ct::MAX_CHANNEL_NAME_BYTES == 31); // 31 chars + NUL
        CHECK(ct::MAX_CHANNEL_NAME_BYTES == ct::SIGNAL_LABEL_LEN - 1);

        // A full-width name must survive the wire byte for byte, NUL included.
        // The interesting case is the boundary: 31 bytes is legal and 32 is
        // not, and the difference between "fits" and "truncates to something
        // that collides with another channel" is one byte.
        {
            ct::CanSignalConfig g{};
            const QByteArray longest(ct::MAX_CHANNEL_NAME_BYTES, 'W'); // 31 bytes
            std::memcpy(g.label, longest.constData(), size_t(longest.size()));
            ::CanSignalConfig f;
            std::memcpy(&f, &g, sizeof(f));
            CHECK(std::memcmp(f.label, longest.constData(), size_t(longest.size())) == 0);
            CHECK(f.label[31] == '\0'); // the terminator the 32nd byte is reserved for
        }

        struct PackCase {
            quint16 msgIdx; quint8 byteOrder; quint8 isActive; quint16 startBit;
            quint8 bitLen; quint8 valueType; qint8 decimals; quint8 muxOff;
        };
        const PackCase cases[] = {
            {0, 0, 1, 0, 1, ct::SIGNAL_TYPE_UINT8, 0, 0},        // all-minimum
            // The top message index at the new capacity. 499 needs 9 bits and
            // so is the case that would break first if msg_idx were ever
            // narrowed â€” 249 fitted in 8 and proved nothing about the field.
            {499, 1, 1, 511, 64, ct::SIGNAL_TYPE_DOUBLE, 8, 63}, // all-maximum
            {ct::SIG_MSG_NONE, 0, 1, 0, 32, ct::SIGNAL_TYPE_FLOAT, 3, 0}, // sentinel
            {ct::SIG_MSG_NONE, 1, 0, 137, 17, ct::SIGNAL_TYPE_INT16, 5, 41},
            {1, 1, 0, 8, 16, ct::SIGNAL_TYPE_UINT16, 0, 7},
            {100, 0, 1, 256, 33, ct::SIGNAL_TYPE_INT32, 8, 32},
        };
        for (const PackCase &c : cases) {
            ct::CanSignalConfig g{};
            ct::sigSetHeader(g, c.msgIdx, c.byteOrder, c.isActive);
            ct::sigSetBits(g, c.startBit, c.bitLen, c.valueType, c.decimals, c.muxOff);

            ::CanSignalConfig f;
            std::memcpy(&f, &g, sizeof(f)); // identical bytes, firmware's view

            CHECK(sig_msg_idx(&f) == c.msgIdx);
            CHECK(sig_byte_order(&f) == c.byteOrder);
            CHECK(sig_is_active(&f) == c.isActive);
            CHECK(sig_start_bit(&f) == c.startBit);
            CHECK(sig_bit_length(&f) == c.bitLen);
            CHECK(sig_value_type(&f) == c.valueType);
            CHECK(sig_decimal_places(&f) == c.decimals);
            CHECK(sig_mux_byte_offset(&f) == c.muxOff);
            // ...and the GUI reads back its own packing identically.
            CHECK(ct::sigMsgIdx(g) == c.msgIdx);
            CHECK(ct::sigStartBit(g) == c.startBit);
            CHECK(ct::sigBitLength(g) == c.bitLen);
            CHECK(ct::sigValueType(g) == c.valueType);
            CHECK(ct::sigMuxByteOffset(g) == c.muxOff);
        }
    }

    // ---- The 8x8 lookup table, which replaces the 4x4. It ships as TWO
    // records â€” a Def and one record per grid ROW â€” because the naive combined
    // form is 329 bytes: over the host->device payload cap AND over
    // MAX_PADDED_RECORD, so it could neither be sent nor stored. The split
    // follows the v13 2x16 precedent, but the ROW shape is chosen for one
    // specific property, and this block is where that property is pinned:
    // sizeof(Table8x8Row) is 32 and PAD8(32) is 32, so a table's eight row
    // slots are BYTE-CONTIGUOUS in flash. The engine takes one pointer at row
    // t*8 and indexes grid[y*8 + x] exactly as the 4x4 indexed
    // outputs[y*4 + x] â€” no reassembly buffer, no cross-record arithmetic, no
    // RAM. A row record that grew by even one byte would pad to 40 and quietly
    // turn that single pointer into a read across strangers' memory.
    //
    // Same fill-and-reinterpret discipline as the records above, with every
    // offset pinned to a literal: sizeof agreeing tells you nothing about
    // whether y_sites starts where the other side thinks it does, and a
    // 4-byte slip there is a table that interpolates X against Y's
    // breakpoints â€” plausible-looking output, wrong everywhere.
    //
    // This is also the ONE place the row record's two names are reconciled. The
    // firmware calls it Table8x8Row; the GUI calls it Table8x8GridRow, because
    // ct::Table8x8Row is already the document row â€” one whole table, one line in
    // the Tables dialog â€” and device_mapper.h has both headers open at once. The
    // asserts below are what make the rename safe: the same bytes under two
    // names is a naming choice, and the same name over two layouts is a bug. ----
    {
        static_assert(sizeof(ct::Table8x8Def) == sizeof(::Table8x8Def),
                      "GUI and firmware 8x8 definition records differ in size");
        static_assert(sizeof(ct::Table8x8GridRow) == sizeof(::Table8x8Row),
                      "GUI and firmware 8x8 row records differ in size");
        // 2 + 2 + 2 + 1 + 1 + 1 + 32 + 32 = 73, padding to 80 in flash.
        static_assert(sizeof(ct::Table8x8Def) == 73, "Table8x8Def must be 73 bytes on the wire");
        // 8 floats and nothing else. THE number this whole shape exists for.
        static_assert(sizeof(ct::Table8x8GridRow) == 32, "Table8x8Row must be exactly 32 bytes");
        static_assert(sizeof(ct::Table8x8GridRow) % 8 == 0,
                      "PAD8(sizeof(Table8x8Row)) must equal sizeof(Table8x8Row): the eight row "
                      "slots of a table are only contiguous while the record needs no padding");

#define T8_OFFSET_AGREES(field, off)                                                               \
    static_assert(offsetof(ct::Table8x8Def, field) == (off)                                        \
                      && offsetof(::Table8x8Def, field) == (off),                                  \
                  "Table8x8Def." #field " must sit at offset " #off " in both headers")
        T8_OFFSET_AGREES(x_signal_idx, 0);
        T8_OFFSET_AGREES(y_signal_idx, 2);
        T8_OFFSET_AGREES(dest_signal_idx, 4);
        T8_OFFSET_AGREES(flags, 6);
        T8_OFFSET_AGREES(x_count, 7);
        T8_OFFSET_AGREES(y_count, 8);
        T8_OFFSET_AGREES(x_sites, 9);  // 9 + 8*4 = 41
        T8_OFFSET_AGREES(y_sites, 41); // 41 + 8*4 = 73
#undef T8_OFFSET_AGREES
        static_assert(offsetof(ct::Table8x8GridRow, v) == 0 && offsetof(::Table8x8Row, v) == 0,
                      "Table8x8Row.v must sit at offset 0 in both headers");

        // Both sides must agree on the geometry, and on the ROW table's
        // capacity being the product â€” table t owns rows t*8..t*8+7, so a host
        // that thought there were 8 row slots instead of 64 would write every
        // table's grid on top of table 0's.
        CHECK(ct::MAX_TABLES_8X8 == fw::kMaxTables8x8);
        CHECK(ct::TABLE_8X8_SITES == fw::kTable8x8Sites);
        CHECK(ct::MAX_TABLES_8X8 == 8);
        CHECK(ct::TABLE_8X8_SITES == 8);
        CHECK(ct::MAX_TABLE_8X8_ROWS == ct::MAX_TABLES_8X8 * ct::TABLE_8X8_SITES);
        CHECK(ct::MAX_TABLE_8X8_ROWS == 64);

        ct::Table8x8Def g{};
        g.x_signal_idx = 0x1234;
        g.y_signal_idx = 0x2345;
        g.dest_signal_idx = 0x3456;
        g.flags = quint8(ct::TABLEFLAG_ACTIVE | ct::TABLEFLAG_X_INTERP | ct::TABLEFLAG_Y_INTERP);
        g.x_count = 8;
        g.y_count = 5; // deliberately different from x_count: a swap must show
        for (int k = 0; k < ct::TABLE_8X8_SITES; ++k) {
            g.x_sites[k] = float(k) + 0.5f;      //  0.5 .. 7.5
            g.y_sites[k] = 100.0f * float(k) + 1.0f; //  1 .. 701
        }

        ::Table8x8Def f;
        std::memcpy(&f, &g, sizeof(f));
        CHECK(f.x_signal_idx == 0x1234);
        CHECK(f.y_signal_idx == 0x2345);
        CHECK(f.dest_signal_idx == 0x3456);
        CHECK(f.flags
              == (fw::kTableFlagActive | fw::kTableFlagXInterp | fw::kTableFlagYInterp));
        CHECK(f.x_count == 8);
        CHECK(f.y_count == 5);
        for (int k = 0; k < fw::kTable8x8Sites; ++k) {
            CHECK(f.x_sites[k] == float(k) + 0.5f);
            CHECK(f.y_sites[k] == 100.0f * float(k) + 1.0f);
        }
        // The flag bits are shared with the 2x16 and must mean the same thing
        // on both sides; Y_INTERP is the one the retired 4x4 also used, and it
        // is the bit an 8x8 needs most.
        CHECK(ct::TABLEFLAG_ACTIVE == fw::kTableFlagActive);
        CHECK(ct::TABLEFLAG_X_INTERP == fw::kTableFlagXInterp);
        CHECK(ct::TABLEFLAG_Y_INTERP == fw::kTableFlagYInterp);
        CHECK((ct::TABLEFLAG_ACTIVE | ct::TABLEFLAG_X_INTERP | ct::TABLEFLAG_Y_INTERP) == 0x07);

        ct::Table8x8GridRow gr{};
        for (int k = 0; k < ct::TABLE_8X8_SITES; ++k)
            gr.v[k] = -1.0f * float(k) - 0.25f;
        ::Table8x8Row fr;
        std::memcpy(&fr, &gr, sizeof(fr));
        for (int k = 0; k < fw::kTable8x8Sites; ++k)
            CHECK(fr.v[k] == -1.0f * float(k) - 0.25f);

        // Eight rows laid end to end are one flat grid of 64 floats â€” the
        // reinterpretation the engine performs. If the record ever gained
        // padding this loop would read the pad, and the CHECK below is the
        // cheapest possible statement of "that must not happen".
        ct::Table8x8GridRow block[ct::TABLE_8X8_SITES]{};
        for (int y = 0; y < ct::TABLE_8X8_SITES; ++y)
            for (int x = 0; x < ct::TABLE_8X8_SITES; ++x)
                block[y].v[x] = float(y * 8 + x);
        const float *flat = reinterpret_cast<const float *>(block);
        for (int i = 0; i < 64; ++i)
            CHECK(flat[i] == float(i));
        static_assert(sizeof(block) == 256, "a table's eight row slots are 256 contiguous bytes");
    }

    // ---- v16: IntegratorConfig is declared twice â€” once in the GUI's
    // wire_structs.h, once in the firmware's protocol.h â€” and the bytes on the
    // wire are whatever those two agree on. Same trick as above: fill the GUI's
    // struct, reinterpret the raw bytes as the firmware's, and read every field
    // back. A reordered or differently-padded field would sail past a
    // sizeof-only check and land the rate in the flags byte. ----
    {
        static_assert(sizeof(ct::IntegratorConfig) == sizeof(::IntegratorConfig),
                      "GUI and firmware integrator records differ in size");
        ct::IntegratorConfig g{};
        g.input_signal_idx = 0x1234;
        g.reset_signal_idx = 0x2345;
        g.enable_signal_idx = 0x3456;
        g.dest_signal_idx = 0x4567;
        g.input_const = 1.5f;
        g.min_value = -2.5f;
        g.max_value = 300.25f;
        g.reset_value = 7.75f;
        g.start_value = 91.5f; // v17 â€” distinct from reset_value, they are separate fields
        g.rate_hz = 37;
        g.flags = ct::INTEGFLAG_ACTIVE | ct::INTEGFLAG_CONST_INPUT
                  | ct::INTEGFLAG_COUNT_DOWN | ct::INTEGFLAG_PRESERVE;

        ::IntegratorConfig f;
        std::memcpy(&f, &g, sizeof(f));
        CHECK(f.input_signal_idx == 0x1234);
        CHECK(f.reset_signal_idx == 0x2345);
        CHECK(f.enable_signal_idx == 0x3456);
        CHECK(f.dest_signal_idx == 0x4567);
        CHECK(f.input_const == 1.5f);
        CHECK(f.min_value == -2.5f);
        CHECK(f.max_value == 300.25f);
        CHECK(f.reset_value == 7.75f);
        CHECK(f.start_value == 91.5f);
        CHECK(f.rate_hz == 37);
        CHECK(f.flags == (fw::kIntegFlagActive | fw::kIntegFlagConstInput
                          | fw::kIntegFlagCountDown | fw::kIntegFlagPreserve));
        // The two headers must also agree on the flag bits, the table capacity
        // and the rate ceiling â€” all of which bound what the GUI will send.
        CHECK(ct::INTEGFLAG_ACTIVE == fw::kIntegFlagActive);
        CHECK(ct::INTEGFLAG_CONST_INPUT == fw::kIntegFlagConstInput);
        CHECK(ct::INTEGFLAG_COUNT_DOWN == fw::kIntegFlagCountDown);
        CHECK(ct::INTEGFLAG_PRESERVE == fw::kIntegFlagPreserve);
        CHECK(ct::MAX_INTEGRATORS == fw::kMaxIntegrators);
        CHECK(ct::INTEGRATOR_MAX_HZ == fw::kIntegratorMaxHz);
        // Every flag must be a distinct bit â€” two colliding values would make
        // "count down" and "preserve" the same switch.
        CHECK((ct::INTEGFLAG_ACTIVE | ct::INTEGFLAG_CONST_INPUT
               | ct::INTEGFLAG_COUNT_DOWN | ct::INTEGFLAG_PRESERVE) == 0x0F);
    }

    // ---- CounterConfig crosses the wire too, and it just grew rate_hz. The
    // byte is at the very end of the record, which is precisely where a padding
    // disagreement hides: sizeof would still match if one side padded to 32 and
    // the other packed to 31. Fill from the GUI, read back through the
    // firmware's declaration. ----
    {
        static_assert(sizeof(ct::CounterConfig) == sizeof(::CounterConfig),
                      "GUI and firmware counter records differ in size");
        static_assert(sizeof(ct::CounterConfig) == 32, "counter record is 32 bytes");
        ct::CounterConfig g{};
        g.up_signal_idx = 0x1111;
        g.down_signal_idx = 0x2222;
        g.follow_signal_idx = 0x3333;
        g.reset_signal_idx = 0x4444;
        g.enable_signal_idx = 0x5555;
        g.dest_signal_idx = 0x6666;
        g.min_value = -12.5f;
        g.max_value = 500.75f;
        g.reset_value = 3.25f;
        g.step = 0.5f;
        g.mode = ct::COUNTER_MODE_RATE;
        g.flags = quint8(ct::COUNTERFLAG_ACTIVE | ct::COUNTERFLAG_ROLL
                         | ct::COUNTERFLAG_RATE_DOWN);
        g.rate_hz = 50;

        ::CounterConfig f;
        std::memcpy(&f, &g, sizeof(f));
        CHECK(f.up_signal_idx == 0x1111);
        CHECK(f.down_signal_idx == 0x2222);
        CHECK(f.follow_signal_idx == 0x3333);
        CHECK(f.reset_signal_idx == 0x4444);
        CHECK(f.enable_signal_idx == 0x5555);
        CHECK(f.dest_signal_idx == 0x6666);
        CHECK(f.min_value == -12.5f);
        CHECK(f.max_value == 500.75f);
        CHECK(f.reset_value == 3.25f);
        CHECK(f.step == 0.5f);
        CHECK(f.mode == COUNTER_MODE_RATE); // enum, not a macro â€” survives the undef sweep
        CHECK(f.rate_hz == 50);
        CHECK((f.flags & fw::kCounterFlagRateDown) != 0);
        // Mode values and the new flag bit must agree across the two headers,
        // and RATE_DOWN must not collide with an existing counter flag.
        CHECK(ct::COUNTER_MODE_UPDOWN == COUNTER_MODE_UPDOWN);
        CHECK(ct::COUNTER_MODE_FOLLOW == COUNTER_MODE_FOLLOW);
        CHECK(ct::COUNTER_MODE_RATE == COUNTER_MODE_RATE);
        CHECK(ct::COUNTERFLAG_RATE_DOWN == fw::kCounterFlagRateDown);
        CHECK((ct::COUNTERFLAG_ROLL | ct::COUNTERFLAG_PRESERVE | ct::COUNTERFLAG_ACTIVE
               | ct::COUNTERFLAG_RATE_DOWN) == 0x0F);
        CHECK(ct::COUNTER_MAX_HZ == fw::kCounterMaxHz);
        // Every rate the host offers must be one the device accepts, and must
        // divide the 100 Hz tick exactly â€” otherwise the phase accumulator is
        // averaging where the UI implies an exact period.
        for (int hz : ct::kCounterRateChoices) {
            CHECK(hz >= 1 && hz <= fw::kCounterMaxHz);
            CHECK((100 % hz) == 0);
        }
        // The chunk arithmetic has to survive the extra byte.
        CHECK(4 + ct::WRITE_CHUNK_COUNTERS * int(sizeof(ct::CounterConfig)) <= ct::MAX_TX_PAYLOAD);
        CHECK(4 + ct::READ_CHUNK_COUNTERS * int(sizeof(ct::CounterConfig)) <= 2030);
    }

    // ---- Device channels: a record that rides in the config header rather than
    // in a table. One destination slot per channel, indexed by DEVCH_*. ----
    {
        static_assert(sizeof(ct::DeviceChannelsConfig) == sizeof(::DeviceChannelsConfig),
                      "GUI and firmware device-channel records differ in size");
        // The two sides must agree on the LAYOUT, not just the total size — the
        // struct is memcpy'd across the wire, so a GUI that thought bus 2 began
        // one field later than the firmware does would mis-address nine channels
        // while every size check still passed.
        CHECK(ct::DEVCH_COUNT == DEVCH_COUNT);
        CHECK(ct::DEVCH_PER_BUS == DEVCH_PER_BUS);
        CHECK(ct::DEVCH_ONTIME == DEVCH_ONTIME);
        CHECK(ct::DEVCH_BUS_BASE == DEVCH_BUS_BASE);
        CHECK(ct::devChBus(1, ct::DEVCH_BUS_TX_ERRORS) == DEVCH_BUS(1, DEVCH_BUS_TX_ERRORS));
        CHECK(ct::devChBus(2, ct::DEVCH_BUS_LOAD) == DEVCH_BUS(2, DEVCH_BUS_LOAD));
        // OnTime at index 0 is load-bearing, not incidental: it is what makes
        // the old two-byte payload a valid prefix of this one, which is what
        // lets a device accept a short write from a GUI that predates the CAN
        // diagnostics instead of NACKing the step.
        CHECK(ct::DEVCH_ONTIME == 0);
        CHECK(offsetof(ct::DeviceChannelsConfig, signal_idx) == 0);

        ct::DeviceChannelsConfig g = ct::unusedDeviceChannels();
        g.signal_idx[ct::DEVCH_ONTIME] = 0x0142;
        g.signal_idx[ct::devChBus(2, ct::DEVCH_BUS_LOAD)] = 0x0207;
        ::DeviceChannelsConfig f;
        std::memcpy(&f, &g, sizeof(f));
        CHECK(f.signal_idx[DEVCH_ONTIME] == 0x0142);
        CHECK(f.signal_idx[DEVCH_BUS(2, DEVCH_BUS_LOAD)] == 0x0207);
        // Everything not named is UNUSED, not slot zero. Zero is a valid
        // destination, so a factory that zero-filled would have the device
        // publish 27 channels into signal 0.
        CHECK(f.signal_idx[DEVCH_BUS(0, DEVCH_BUS_RX_ERRORS)] == ct::SIG_MSG_NONE);
        // "Unused" is the same value on both sides, and it is the erased-flash
        // value too â€” which is why an image that predates the field still reads
        // as "publishes nothing" rather than as slot 65535.
        CHECK(ct::CMD_WRITE_DEVICE_CHANNELS == fw::kCmdWriteDeviceChannels);
        CHECK(ct::CMD_READ_DEVICE_CHANNELS == fw::kCmdReadDeviceChannels);
        CHECK(ct::CMD_WRITE_DEVICE_CHANNELS == 0x32);
        CHECK(ct::CMD_READ_DEVICE_CHANNELS == 0x33);
        // And on the protocol version itself: the GUI advertises what it speaks,
        // so a table added to one side without bumping both would go unnoticed.
        CHECK(ct::PROTOCOL_VERSION_CURRENT == fw::kProtocolVersion);
        // Pinned to the number as well as to each other, because "equal to each
        // other" is also true of two sides that were both left behind. The
        // ladder was reset to 1 when the fleet identity moved into the firmware:
        // nothing has shipped, so the old numbering recorded the development
        // history of a product no customer has held.
        CHECK(ct::PROTOCOL_VERSION_CURRENT == 1);
        CHECK(ct::PROTOCOL_VERSION_CURRENT == ct::PROTOCOL_VERSION_V1);
        // The flash image was reset alongside it, and for the same reason. It is
        // a separate number on purpose â€” the two have moved independently before
        // â€” so it is worth pinning separately. And move independently it did:
        // Advanced Math grew MathConfig 18 -> 24, and because imageCrc() hashes
        // item_size bytes per stored record, a v1 image holding math rows would
        // fail the CRC anyway â€” the bump to 2 makes every v1 image reject
        // uniformly instead of only the ones that happened to use math
        // (FIRMWARE-NOTES #22). The wire version above deliberately did NOT move.
        // v3: the header gained DeviceChannelsConfig and CounterConfig grew
        // rate_hz. Either alone invalidates a stored image â€” the header layout
        // moved, and imageCrc() spans item_size bytes per record â€” so the bump
        // makes the rejection uniform rather than config-dependent. The wire
        // version above again did NOT move.
        // v4: the capacity expansion. Every table offset in the region moved â€”
        // CanSignalConfig went 48 -> 64, MAX_MESSAGES 250 -> 500, MAX_SIGNALS
        // 768 -> 1000, MAX_TIMERS 20 -> 50 â€” and the 4x4 table was replaced by
        // the 8x8 pair, so FLASH_NUM_TABLES went 12 -> 13 and the header's
        // counts[] array grew with it. An older image read under this build
        // would not be subtly wrong, it would be reading the signal table where
        // the message table used to end. The wire version STILL does not move:
        // nothing is deployed, and the length check (4 + count*item_size) is
        // what makes a mismatched host NACK cleanly.
        // v5: the SCRIPT table. FLASH_NUM_TABLES 13 -> 14 grows the header's
        // counts[] array, which moves the record area and therefore every table
        // offset again — the same misalignment hazard as v4, and the same
        // reason the version has to move with it. Note what did NOT need a bump
        // between the two: growing the region 96 -> 128 KB at flash map v2.
        // Capacity is not layout — the base and every offset stayed put, and a
        // stored config read identically across it.
        // v6: CanMessageConfig 10 -> 14 bytes for the per-message access key.
        // Messages are the FIRST record table, so every offset after them
        // shifts — a v5 image read as v6 would be misread record for record,
        // with signals starting mid-message, not merely fail its CRC. Same
        // hazard as v4 and v5, same answer. The WIRE version still does not
        // move: the length check (4 + count*item_size) makes a 10-byte-record
        // host NACK cleanly against a 14-byte device.
        // v7: DeviceChannelsConfig 2 -> 56 bytes for the CAN diagnostic device
        // channels. It sits in the HEADER, ahead of the record area, so all 54
        // extra bytes shove every table offset along — the same misalignment
        // hazard, and note what does NOT rescue a v6 image here: OnTime staying
        // at index 0 makes the old payload a valid prefix ON THE WIRE and does
        // nothing whatever for a stored layout.
        // v8: the Transmit CRC8 table. FLASH_NUM_TABLES 14 -> 15 grows the
        // header's counts[] array again — v5's hazard exactly: the record area
        // and every table offset behind it move, so a v7 image is refused
        // rather than misread. The cost is one re-Send per updated device; the
        // WIRE version once more does not move.
        // v9: the MCU health device channels. DeviceChannelsConfig grows
        // DEVCH_COUNT 31 -> 36 and that struct sits in the HEADER — v7's
        // hazard exactly: ten extra bytes shove every table offset along, so a
        // v8 image is refused rather than misread. The WIRE stays put once
        // more, and for the device-channels write in particular the old 62-byte
        // payload remains a valid PREFIX of the new 72 — the same rule that has
        // protected that command through every growth it has had.
        // v11: the condition record grew to carry a second expression, and
        // ConditionTerm shrank 10 -> 8 to pay for it. BOTH store hazards at
        // once: the record size changed (so imageCrc hashes a different span,
        // the v2 rule) and conditions are the FOURTH table (so everything
        // after them shifts, the v4 rule). v10 was built but never released.
        //
        // v10: MAX_CONDITIONS 100 -> 250. Conditions are the FOURTH table, so
        // the 6,000 extra bytes shift every table behind them — v4's hazard,
        // and a v9 image is refused rather than misread. Triggered transmit
        // rode along in the same release and forced nothing: it claimed three
        // retired bytes INSIDE CanMessageConfig in place, so item_size stayed
        // 14, no offset moved, and by itself it would not have needed a bump at
        // all. The WIRE version stays put once more.
        CHECK(FLASH_STORE_VERSION == 17u);
        // The configurator carries its own copy so a Send can check the device
        // before writing to it. This file is the only place that sees both, so
        // it is the only place the two can be held equal.
        CHECK(ct::EXPECTED_STORE_VERSION == FLASH_STORE_VERSION);
    }

    // ---- Table capacities. Each of these numbers sizes a flash region on the
    // device AND a "does this configuration fit" check in the host, and the two
    // are written down in different files. Equal to each other is not enough,
    // so each is also pinned to its literal: two sides that were both left
    // behind agree perfectly and are both wrong. ----
    {
        CHECK(ct::MAX_MESSAGES == fw::kMaxMessages);
        CHECK(ct::MAX_SIGNALS == fw::kMaxSignals);
        CHECK(ct::MAX_TIMERS == fw::kMaxTimers);
        CHECK(ct::MAX_COUNTERS == fw::kMaxCounters);
        CHECK(ct::MAX_TABLES_2X16 == fw::kMaxTables2x16);
        CHECK(ct::MAX_CONDITIONS == fw::kMaxConditions);
        CHECK(ct::MAX_MESSAGES == 500);
        CHECK(ct::MAX_SIGNALS == 1000);
        CHECK(ct::MAX_TIMERS == 50);
        // 100 -> 250 at store v10. Pinned to the literal like the rest, and
        // worth pinning for a second reason: every active condition also owns a
        // signal slot, so this number and MAX_SIGNALS above are spending the
        // same budget. 250 conditions can claim a quarter of the signal table.
        CHECK(ct::MAX_CONDITIONS == 200);

        // THE binding limit on the message axis, and the reason 500 rather than
        // a round 512. A signal's parent message index is a 9-BIT field inside
        // msg_and_flags: SIG_MSG_IDX_MASK is 0x1FF, values 0..510 are usable and
        // 511 is the SIG_MSG_NONE sentinel that marks a virtual signal. 500
        // leaves 11 spare; 512 would not fit at all, and the failure would not
        // be a rejected configuration â€” message 511's signals would read back as
        // virtual and detach from their message silently. Widening the axis
        // again means widening the field first.
        CHECK(ct::SIG_MSG_IDX_MASK == fw::kSigMsgIdxMask);
        CHECK(ct::SIG_MSG_IDX_MASK == 0x1FF);
        CHECK(ct::MAX_MESSAGES <= int(ct::SIG_MSG_IDX_MASK)); // 500 <= 511, the sentinel
        CHECK(ct::MAX_MESSAGES - 1 < int(ct::SIG_MSG_IDX_MASK)); // top index is not the sentinel
        static_assert(512 > 0x1FF, "512 message slots would need a 10-bit msg_idx field");

        // The engine's view of the same numbers. engine_table_capacity reads the
        // generated flash geometry, so this catches a FLASH_TABLE_LIST line that
        // was not updated with the constant it names.
        CHECK(engine_table_capacity(ENGINE_TABLE_MESSAGES) == ct::MAX_MESSAGES);
        CHECK(engine_table_capacity(ENGINE_TABLE_SIGNALS) == ct::MAX_SIGNALS);
        CHECK(engine_table_capacity(ENGINE_TABLE_TIMERS) == ct::MAX_TIMERS);
        CHECK(engine_table_capacity(ENGINE_TABLE_CONDITIONS) == ct::MAX_CONDITIONS);
        CHECK(engine_table_capacity(ENGINE_TABLE_TABLES_2X16_DEF) == ct::MAX_TABLES_2X16);
        CHECK(engine_table_capacity(ENGINE_TABLE_TABLES_8X8_DEF) == ct::MAX_TABLES_8X8);
        // Table t owns rows t*8..t*8+7, so the ROW table holds the product, not
        // the table count. Getting this wrong caps the device at one table's
        // worth of grid and every table past the first writes over the first.
        CHECK(engine_table_capacity(ENGINE_TABLE_TABLES_8X8_ROW)
              == ct::MAX_TABLE_8X8_ROWS);
        CHECK(engine_table_item_size(ENGINE_TABLE_SIGNALS) == int(sizeof(ct::CanSignalConfig)));
        CHECK(engine_table_item_size(ENGINE_TABLE_TABLES_8X8_DEF) == int(sizeof(ct::Table8x8Def)));
        CHECK(engine_table_item_size(ENGINE_TABLE_TABLES_8X8_ROW) == int(sizeof(ct::Table8x8GridRow)));
    }

    // ---- 2.3.0: the protection LEVEL in bits 6-7 of a message's (and a
    // relay's) flags. The two headers each write these numbers down separately,
    // and unlike most such pairs the VALUES carry meaning of their own: the
    // assignment was picked so that the only two patterns shipped 2.2.x flash
    // can contain decode to the right new tier with no store-version bump.
    // 0x80 was 2.2.x "Read-only", which CONCEALED, so it must land on Hidden and
    // not on the new visible Read Only; 0xC0 was "Protect Communication" and is
    // an exact match; 0x40 was never emitted by 2.2.x, which is what leaves it
    // free for the new weakest tier. Pinned to the literals as well as to each
    // other, because two sides that were both left behind agree perfectly.
    //
    // The failure this guards is quiet: a host that decoded 0x80 as Read Only
    // would print the CAN ID, frame layout and every bit position of every
    // message a field unit has concealed, on the first Get, with nothing on
    // screen to say it had happened. ----
    {
        CHECK(ct::MSGPROT_MASK == fw::kMsgProtMask);
        CHECK(ct::MSGPROT_NONE == fw::kMsgProtNone);
        CHECK(ct::MSGPROT_READONLY == fw::kMsgProtReadOnly);
        CHECK(ct::MSGPROT_HIDDEN == fw::kMsgProtHidden);
        CHECK(ct::MSGPROT_PROTECTED == fw::kMsgProtProtected);
        CHECK(ct::MSGPROT_MASK == 0xC0);
        CHECK(ct::MSGPROT_NONE == 0x00);
        CHECK(ct::MSGPROT_READONLY == 0x40);   // never emitted by 2.2.x
        CHECK(ct::MSGPROT_HIDDEN == 0x80);     // was 2.2.x "Read-only" (concealing)
        CHECK(ct::MSGPROT_PROTECTED == 0xC0);  // was 2.2.x "Protect Communication"
        // The level occupies bits 6-7 and NOTHING else may. Every MSGFLAG_* the
        // engine actually evaluates lives in bits 0-5, so a widened flag word
        // that reached up into the level would start reading as concealment.
        CHECK((ct::MSGFLAG_EXTENDED | ct::MSGFLAG_FD | ct::MSGFLAG_ROUTING
               | ct::MSGFLAG_ACTIVE | ct::MSGFLAG_TRANSMIT | ct::MSGFLAG_TX_SEQUENTIAL)
              == 0x3F);
        CHECK((ct::MSGPROT_MASK & 0x3F) == 0);
        // Relays carry the SAME level in the same two bits — that parity is what
        // stops a relay section marked Hidden in the GUI reaching the device
        // bare. RELAYFLAG_* must therefore stay clear of the mask too.
        CHECK(((ct::RELAYFLAG_EXTENDED | ct::RELAYFLAG_INVERT | ct::RELAYFLAG_ACTIVE)
               & ct::MSGPROT_MASK) == 0);
        // The tier IS the number: CommsProtection is defined as the shifted
        // level, so the model cannot drift from the wire.
        CHECK(quint8(ct::CommsProtection::None) == (ct::MSGPROT_NONE >> 6));
        CHECK(quint8(ct::CommsProtection::ReadOnly) == (ct::MSGPROT_READONLY >> 6));
        CHECK(quint8(ct::CommsProtection::Hidden) == (ct::MSGPROT_HIDDEN >> 6));
        CHECK(quint8(ct::CommsProtection::Protected) == (ct::MSGPROT_PROTECTED >> 6));
    }

    // ---- Command ids: the four new ones, and the two the 4x4 took with it.
    //
    // The v13 discipline (see protocol.h): a replaced table's ids are RETIRED,
    // NOT REUSED. The 4x4's 0x1D/0x1E must now answer ERR_INVALID_CMD. The
    // length check (4 + count*item_size) would probably have caught a stale
    // host anyway â€” 105-byte records against a 73-byte Def â€” but "probably" is
    // not what a replaced table gets, and the two failures do not read the same
    // to whoever is holding the cable: ERR_INVALID_CMD says the device does not
    // have this feature, while a length NACK says the record is malformed. ----
    {
        CHECK(ct::CMD_WRITE_TABLE8X8_DEF == fw::kCmdWriteTable8x8Def);
        CHECK(ct::CMD_READ_TABLE8X8_DEF == fw::kCmdReadTable8x8Def);
        CHECK(ct::CMD_WRITE_TABLE8X8_ROW == fw::kCmdWriteTable8x8Row);
        CHECK(ct::CMD_READ_TABLE8X8_ROW == fw::kCmdReadTable8x8Row);
        // 0x33 was the last id in use before this change, so the four new ones
        // start at 0x34 â€” allocated fresh rather than filling the 4x4's hole.
        CHECK(ct::CMD_WRITE_TABLE8X8_DEF == 0x34);
        CHECK(ct::CMD_READ_TABLE8X8_DEF == 0x35);
        CHECK(ct::CMD_WRITE_TABLE8X8_ROW == 0x36);
        CHECK(ct::CMD_READ_TABLE8X8_ROW == 0x37);
        // (That the DEVICE has actually forgotten 0x1D/0x1E is checked once the
        // serial fixture is up, below â€” a constant comparison here could only
        // prove the host stopped saying them.)
    }

    // ---- v8 Transmit CRC8. The command pair is 0x41/0x42 — the THIRD
    // allocation this feature needed, and both wrong turns are worth the ink.
    // 0x38/0x39 belong to FW_UPDATE_BEGIN/DATA: table dispatch ate the
    // updater's frames and every staging test failed at once. 0x40 is retired
    // ground: shipped 2.2.x Managers still send CMD_MSG_ACCESS_RESPONSE
    // before every Send and depend on ERR_INVALID_CMD coming back (the
    // retirement test above). fw::kAllCommandIds now holds live AND retired
    // ids pairwise-distinct at compile time, so the next allocation cannot
    // repeat either mistake. ----
    {
        CHECK(ct::CMD_WRITE_CRC8_CFG == fw::kCmdWriteCrc8Cfg);
        CHECK(ct::CMD_READ_CRC8_CFG == fw::kCmdReadCrc8Cfg);
        CHECK(ct::CMD_WRITE_CRC8_CFG == 0x41);
        CHECK(ct::CMD_READ_CRC8_CFG == 0x42);
        CHECK(ct::MAX_CRC8_MESSAGES == fw::kMaxCrc8Messages);
        CHECK(ct::MAX_CRC8_MESSAGES == 20);
        CHECK(ct::CRC8_MAX_ELEMENTS == fw::kCrc8MaxElements);
        CHECK(ct::CRC8_MAX_ELEMENTS == 15);
        CHECK(ct::CRC8FLAG_ACTIVE == fw::kCrc8FlagActive);
        CHECK(ct::CRC8FLAG_REF_IN == fw::kCrc8FlagRefIn);
        CHECK(ct::CRC8FLAG_REF_OUT == fw::kCrc8FlagRefOut);
    }

    // ---- Chunk arithmetic against the RAISED payload cap.
    //
    // MAX_TX_PAYLOAD went 112 -> 496 and MAX_TX_WIRE_BYTES 127 -> 512. Both
    // numbers only ever existed because of the v1 RX-DMA fault described in
    // FIRMWARE-NOTES #5, which has been fixed for a long time; raising them is
    // what finally collects on that fix. Every chunk constant had to be
    // RECOMPUTED from its record size rather than scaled, and every one of them
    // is checked here two ways: it fits, and one more record would not. A chunk
    // that merely fits is not wrong, it is slow â€” and quietly so, since the only
    // symptom is a Send that takes more round trips than the wire requires. ----
    {
        CHECK(ct::MAX_TX_PAYLOAD == 496);
        CHECK(ct::MAX_TX_WIRE_BYTES == 512);
        // The device->host direction did NOT change; reads are still bounded by
        // the firmware's ~2030-byte response cap.
        constexpr int kReadCap = 2030;

        // fits, and is maximal: (n+1) records would overflow the cap.
#define WRITE_CHUNK_IS_MAXIMAL(chunk, type)                                                        \
    do {                                                                                           \
        CHECK(4 + (chunk) * int(sizeof(type)) <= ct::MAX_TX_PAYLOAD);                              \
        CHECK(4 + ((chunk) + 1) * int(sizeof(type)) > ct::MAX_TX_PAYLOAD);                         \
    } while (0)
        WRITE_CHUNK_IS_MAXIMAL(ct::WRITE_CHUNK_MESSAGES, ct::CanMessageConfig);   // 4+49*10 = 494
        WRITE_CHUNK_IS_MAXIMAL(ct::WRITE_CHUNK_SIGNALS, ct::CanSignalConfig);     // 4+7*64  = 452
        WRITE_CHUNK_IS_MAXIMAL(ct::WRITE_CHUNK_MATH, ct::MathConfig);             // 4+20*24 = 484
        WRITE_CHUNK_IS_MAXIMAL(ct::WRITE_CHUNK_CONDITIONS, ct::ConditionConfig);  // 4+14*35 = 494
        WRITE_CHUNK_IS_MAXIMAL(ct::WRITE_CHUNK_COUNTERS, ct::CounterConfig);      // 4+15*31 = 469
        WRITE_CHUNK_IS_MAXIMAL(ct::WRITE_CHUNK_TIMERS, ct::TimerConfig);          // 4+24*20 = 484
        WRITE_CHUNK_IS_MAXIMAL(ct::WRITE_CHUNK_CONSTANTS, ct::ConstantConfig);    // 4+70*7  = 494
        WRITE_CHUNK_IS_MAXIMAL(ct::WRITE_CHUNK_RELAYS, ct::RelayConfig);          // 4+44*11 = 488
        WRITE_CHUNK_IS_MAXIMAL(ct::WRITE_CHUNK_TABLES_2X16_DEF, ct::Table2x16Def);// 4+7*70  = 494
        WRITE_CHUNK_IS_MAXIMAL(ct::WRITE_CHUNK_TABLES_2X16_OUT, ct::Table2x16Out);// 4+7*64  = 452
        WRITE_CHUNK_IS_MAXIMAL(ct::WRITE_CHUNK_TABLES_8X8_DEF, ct::Table8x8Def);  // 4+6*73  = 442
        WRITE_CHUNK_IS_MAXIMAL(ct::WRITE_CHUNK_TABLES_8X8_ROW, ct::Table8x8GridRow);  // 4+15*32 = 484
        WRITE_CHUNK_IS_MAXIMAL(ct::WRITE_CHUNK_INTEGRATORS, ct::IntegratorConfig);// 4+16*30 = 484
#undef WRITE_CHUNK_IS_MAXIMAL
        // Pinned to their literals as well, so a future record-size change has
        // to come back through this line rather than quietly re-deriving.
        CHECK(ct::WRITE_CHUNK_SIGNALS == 7);
        CHECK(ct::WRITE_CHUNK_MESSAGES == 35); // was 49 at a 10-byte record; v20 made it 14
        CHECK(ct::WRITE_CHUNK_TIMERS == 15); // was 24 at a 20-byte record; v12 made it 32
        CHECK(ct::WRITE_CHUNK_TABLES_8X8_DEF == 6);
        CHECK(ct::WRITE_CHUNK_TABLES_8X8_ROW == 15);

        // Reads are capped by the response limit, and additionally by the table
        // itself â€” asking for more records than exist is a NACK, so a read chunk
        // is never larger than its capacity even when the cap would allow it.
#define READ_CHUNK_FITS(chunk, type) CHECK(4 + (chunk) * int(sizeof(type)) <= kReadCap)
        READ_CHUNK_FITS(ct::READ_CHUNK_MESSAGES, ct::CanMessageConfig);
        READ_CHUNK_FITS(ct::READ_CHUNK_SIGNALS, ct::CanSignalConfig);
        READ_CHUNK_FITS(ct::READ_CHUNK_MATH, ct::MathConfig);
        READ_CHUNK_FITS(ct::READ_CHUNK_CONDITIONS, ct::ConditionConfig);
        READ_CHUNK_FITS(ct::READ_CHUNK_COUNTERS, ct::CounterConfig);
        READ_CHUNK_FITS(ct::READ_CHUNK_TIMERS, ct::TimerConfig);
        READ_CHUNK_FITS(ct::READ_CHUNK_CONSTANTS, ct::ConstantConfig);
        READ_CHUNK_FITS(ct::READ_CHUNK_RELAYS, ct::RelayConfig);
        READ_CHUNK_FITS(ct::READ_CHUNK_TABLES_2X16_DEF, ct::Table2x16Def);
        READ_CHUNK_FITS(ct::READ_CHUNK_TABLES_2X16_OUT, ct::Table2x16Out);
        READ_CHUNK_FITS(ct::READ_CHUNK_TABLES_8X8_DEF, ct::Table8x8Def);
        READ_CHUNK_FITS(ct::READ_CHUNK_TABLES_8X8_ROW, ct::Table8x8GridRow);
        READ_CHUNK_FITS(ct::READ_CHUNK_INTEGRATORS, ct::IntegratorConfig);
#undef READ_CHUNK_FITS
        CHECK(ct::READ_CHUNK_SIGNALS <= ct::MAX_SIGNALS);
        CHECK(ct::READ_CHUNK_TIMERS <= ct::MAX_TIMERS);
        CHECK(ct::READ_CHUNK_TABLES_8X8_DEF <= ct::MAX_TABLES_8X8);
        CHECK(ct::READ_CHUNK_TABLES_8X8_ROW <= ct::MAX_TABLE_8X8_ROWS);
        // The signal record at 64 bytes is the one read chunk the raise did NOT
        // help: 42 records of 48 fitted, 31 of 64 do, and 32 would be 2052 â€” six
        // bytes past the response cap and a silently truncated Get.
        CHECK(ct::READ_CHUNK_SIGNALS == 31);
        CHECK(4 + 31 * int(sizeof(ct::CanSignalConfig)) == 1988);
        CHECK(4 + 32 * int(sizeof(ct::CanSignalConfig)) > kReadCap);
        // Timers now read in one request (4 + 50*20 = 1004), and a whole
        // table's worth of 8x8 rows in one too (4 + 32*32 = 1028; 64 would be
        // 2052, so the 64-row table takes exactly two requests).
        CHECK(ct::READ_CHUNK_TIMERS == 50);
        CHECK(ct::READ_CHUNK_TABLES_8X8_DEF == 8);
        CHECK(ct::READ_CHUNK_TABLES_8X8_ROW == 32);
        CHECK(4 + 64 * int(sizeof(ct::Table8x8GridRow)) > kReadCap);
    }

    // ---- Advanced Math: MathConfig also lives in both headers, and it just
    // grew 18 -> 24 bytes for the C operand. Beyond the usual fill/reinterpret
    // check, every offset is pinned to the layout table's literal numbers,
    // because the C operand rides in four RAW bytes â€” a one-byte slip would
    // put a constant's mantissa in reserved0 and only misbehave on ternary
    // ops, the newest and least-exercised path. ----
    {
        static_assert(sizeof(ct::MathConfig) == sizeof(::MathConfig),
                      "GUI and firmware math records differ in size");
        static_assert(sizeof(ct::MathConfig) == 24, "MathConfig must be 24 bytes on the wire");
#define MATH_OFFSET_AGREES(field, off)                                                           \
    static_assert(offsetof(ct::MathConfig, field) == (off)                                       \
                      && offsetof(::MathConfig, field) == (off),                                 \
                  "MathConfig." #field " must sit at offset " #off " in both headers")
        MATH_OFFSET_AGREES(op, 0);
        MATH_OFFSET_AGREES(input_a_type, 1);
        MATH_OFFSET_AGREES(input_a_idx, 2);
        MATH_OFFSET_AGREES(input_a_const, 4);
        MATH_OFFSET_AGREES(input_b_type, 8);
        MATH_OFFSET_AGREES(input_b_idx, 9);
        MATH_OFFSET_AGREES(input_b_const, 11);
        MATH_OFFSET_AGREES(dest_signal_idx, 15);
        MATH_OFFSET_AGREES(is_active, 17);
        MATH_OFFSET_AGREES(input_c_type, 18);
        MATH_OFFSET_AGREES(input_c_val, 19);
        MATH_OFFSET_AGREES(reserved0, 23);
#undef MATH_OFFSET_AGREES

        // The GUI's op combo is indexed by these values ("the order here IS
        // the order there"), so each enumerator is pinned to its literal
        // number on BOTH sides â€” "equal to each other" is also true of two
        // enums that both drifted.
#define MATH_OP_AGREES(name, val)                                                                \
    static_assert(int(ct::MATH_OP_##name) == (val) && int(::MATH_OP_##name) == (val),            \
                  "MATH_OP_" #name " must be " #val " in both headers")
        MATH_OP_AGREES(ABS, 9);
        MATH_OP_AGREES(NEG, 10);
        MATH_OP_AGREES(SQRT, 11);
        MATH_OP_AGREES(FLOOR, 12);
        MATH_OP_AGREES(CEIL, 13);
        MATH_OP_AGREES(ROUND, 14);
        MATH_OP_AGREES(MOD, 15);
        MATH_OP_AGREES(XOR, 16);
        MATH_OP_AGREES(LAND, 17);
        MATH_OP_AGREES(LOR, 18);
        MATH_OP_AGREES(LNOT, 19);
        MATH_OP_AGREES(GT, 20);
        MATH_OP_AGREES(GE, 21);
        MATH_OP_AGREES(LT, 22);
        MATH_OP_AGREES(LE, 23);
        MATH_OP_AGREES(EQ, 24);
        MATH_OP_AGREES(NE, 25);
        MATH_OP_AGREES(MULADD, 26);
        MATH_OP_AGREES(CLAMP, 27);
        MATH_OP_AGREES(LERP, 28);
        MATH_OP_AGREES(SELECT, 29);
        MATH_OP_AGREES(WRAP, 30);
#undef MATH_OP_AGREES

        // The arity table gates which operands the GUI resolves, validates
        // and shows; a wrong entry silently drops (or invents) an operand.
        static_assert(ct::mathOpArity(ct::MATH_OP_ABS) == 1 && ct::mathOpArity(ct::MATH_OP_NEG) == 1
                          && ct::mathOpArity(ct::MATH_OP_SQRT) == 1
                          && ct::mathOpArity(ct::MATH_OP_FLOOR) == 1
                          && ct::mathOpArity(ct::MATH_OP_CEIL) == 1
                          && ct::mathOpArity(ct::MATH_OP_ROUND) == 1
                          && ct::mathOpArity(ct::MATH_OP_LNOT) == 1,
                      "unary ops read A alone");
        static_assert(ct::mathOpArity(ct::MATH_OP_ADD) == 2 && ct::mathOpArity(ct::MATH_OP_MOD) == 2
                          && ct::mathOpArity(ct::MATH_OP_XOR) == 2
                          && ct::mathOpArity(ct::MATH_OP_LAND) == 2
                          && ct::mathOpArity(ct::MATH_OP_LOR) == 2
                          && ct::mathOpArity(ct::MATH_OP_GT) == 2
                          && ct::mathOpArity(ct::MATH_OP_GE) == 2
                          && ct::mathOpArity(ct::MATH_OP_LT) == 2
                          && ct::mathOpArity(ct::MATH_OP_LE) == 2
                          && ct::mathOpArity(ct::MATH_OP_EQ) == 2
                          && ct::mathOpArity(ct::MATH_OP_NE) == 2,
                      "binary ops read A and B");
        static_assert(ct::mathOpArity(ct::MATH_OP_MULADD) == 3
                          && ct::mathOpArity(ct::MATH_OP_CLAMP) == 3
                          && ct::mathOpArity(ct::MATH_OP_LERP) == 3
                          && ct::mathOpArity(ct::MATH_OP_SELECT) == 3
                          && ct::mathOpArity(ct::MATH_OP_WRAP) == 3,
                      "the five ternary ops are why the record grew");

        // Fill with the GUI's packers, reinterpret as the firmware's struct.
        // C first carries a CONSTANT: the four raw bytes must hold the f32
        // little-endian BY VALUE â€” 1.0f is 00 00 80 3F â€” because two helpers
        // byte-swapped in the same direction would still pass a pack/unpack
        // round-trip.
        ct::MathConfig g{};
        g.op = ct::MATH_OP_MULADD;
        g.input_a_type = 1;
        g.input_a_idx = 0x1234;
        g.input_a_const = 1.5f;
        g.input_b_type = 0;
        g.input_b_idx = 0x2345;
        g.input_b_const = -3.25f;
        g.dest_signal_idx = 0x4567;
        g.is_active = 1;
        ct::mathSetInputCConst(g, 1.0f);

        ::MathConfig f;
        std::memcpy(&f, &g, sizeof(f));
        CHECK(f.op == uint8_t(::MATH_OP_MULADD));
        CHECK(f.input_a_type == 1);
        CHECK(f.input_a_idx == 0x1234);
        CHECK(f.input_a_const == 1.5f);
        CHECK(f.input_b_type == 0);
        CHECK(f.input_b_idx == 0x2345);
        CHECK(f.input_b_const == -3.25f);
        CHECK(f.dest_signal_idx == 0x4567);
        CHECK(f.is_active == 1);
        CHECK(f.input_c_type == 0);
        CHECK(f.input_c_val[0] == 0x00 && f.input_c_val[1] == 0x00);
        CHECK(f.input_c_val[2] == 0x80 && f.input_c_val[3] == 0x3F);
        float cBack = 0.0f;
        std::memcpy(&cBack, f.input_c_val, sizeof(cBack)); // the firmware's own decode
        CHECK(cBack == 1.0f);
        CHECK(ct::mathInputCConst(g) == 1.0f);
        CHECK(f.reserved0 == 0);

        // ...then a SIGNAL: u16 index in bytes [0..1], [2..3] zeroed. They
        // held 0x80 0x3F a moment ago, so this catches a helper that only
        // overwrites the low half.
        ct::mathSetInputCSignal(g, 0x0203);
        std::memcpy(&f, &g, sizeof(f));
        CHECK(f.input_c_type == 1);
        CHECK(f.input_c_val[0] == 0x03 && f.input_c_val[1] == 0x02);
        CHECK(f.input_c_val[2] == 0x00 && f.input_c_val[3] == 0x00);
        CHECK(ct::mathInputCIdx(g) == 0x0203);
    }

    EngineCallbacks engineCb{};
    engineCb.transmit_can = captureTransmit;
    engine_init(&engineCb);

    SerialProtoCallbacks protoCb{};
    protoCb.send_bytes = captureBytes;
    protoCb.uptime_ms = fakeUptime;
    protoCb.flash_save = []() { return engine_save_config(nullptr); };
    protoCb.control_can = fakeControlCan;
    protoCb.read_can_setup = fakeReadBusSetup;
    protoCb.request_reset = fakeRequestReset;
    protoCb.random_bytes = fakeRandomBytes;
    serial_proto_init(&protoCb);

    FlashStoreDriver flashDriver{};
    flashDriver.erase = flashErase;
    flashDriver.program = flashProgram;
    flashDriver.data = flashData;
    flash_store_init(&flashDriver);
    flashErase();

    // ---- The 4x4's command ids are RETIRED, not reused, and the device is
    // where that has to be true. 0x1D/0x1E were CMD_WRITE/READ_TABLE4X4_CFG;
    // the v13 rule (protocol.h) is that a replaced table's ids are never handed
    // to its replacement, so a host still speaking the old table gets a clean
    // "I do not know that command" rather than a plausible answer from a device
    // that means something else by it. An empty payload is a well-formed
    // request, so anything other than ERR_INVALID_CMD here means the id still
    // reaches a handler. ----
    for (quint8 cmd : {quint8(0x1D), quint8(0x1E)})
        CHECK(expectNack(cmd, QByteArray(), ct::ERR_INVALID_CMD));

    // ---- Build a configuration with the GUI's own model + mapper ----
    ct::Configuration config;
    // Buses default Off; messages on an Off bus upload deactivated, and the
    // engine only runs active messages.
    config.bus[0].enabled = true;
    config.bus[1].enabled = true;

    // All channels are user-created now (no predefined catalog); register the
    // ones this test binds. Engine Temperature max 250 backs the clamp check.
    const auto addChannel = [&config](const char *name, const char *unit, double baseRes,
                                      double minV, double maxV) {
        ct::Channel c;
        c.name = QString::fromUtf8(name);
        c.unit = QString::fromUtf8(unit);
        c.baseResolution = baseRes;
        c.minValue = minV;
        c.maxValue = maxV;
        c.userDefined = true;
        config.catalog().addOrUpdateUserChannel(c);
    };
    addChannel("Engine RPM", "rpm", 1.0, 0, 20000);
    addChannel("Engine Temperature", "\xC2\xB0""C", 0.1, -50, 250);
    addChannel("GP Raw 3", "", 1.0, -1e9, 1e9);
    addChannel("GP Raw 4", "", 1.0, -1e9, 1e9);

    ct::CommsSection rx;
    rx.name = QStringLiteral("Receive 0x640");
    rx.device = ct::SectionDevice::ReceiveMessage;
    rx.alignment = ct::SectionAlignment::WordSwap;
    rx.baseAddress = 0x640;
    rx.messageLengthBytes = 8;
    rx.defaultValueOnTimeout = true; // revert to default after the receive timeout
    rx.receiveTimeoutMs = 2200;
    ct::CommsChannelRow rpm;
    rpm.channelName = QStringLiteral("Engine RPM");
    rpm.startBit = 0;
    rpm.bitLength = 16;
    rpm.dbcType = int(ct::DbcType::Unsigned);
    rpm.dbcFactor = 1.0;
    rpm.defaultValue = 1234.0; // physical RPM applied on receive timeout
    rx.rows.append(rpm);
    config.bus[0].sections.append(rx);

    // Big-endian (Normal) receive â€” DBC scaling raw*0.75 + 48 into Engine
    // Temperature. The 16-bit field occupies bytes 2 (MSB) and 3 (LSB); the
    // start bit is the signal's LSB = byte 3 bit 0 = bit 24, and the walk
    // (24..31 in data[3], then 16..23 in data[2]) gives
    // value = data[2]<<8 | data[3].
    ct::CommsSection rxBe;
    rxBe.name = QStringLiteral("Receive 0x650");
    rxBe.device = ct::SectionDevice::ReceiveMessage;
    rxBe.alignment = ct::SectionAlignment::Normal;
    rxBe.baseAddress = 0x650;
    rxBe.messageLengthBytes = 8;
    ct::CommsChannelRow temp;
    temp.channelName = QStringLiteral("Engine Temperature");
    temp.startBit = 24;
    temp.bitLength = 16;
    temp.dbcType = int(ct::DbcType::Unsigned);
    temp.dbcFactor = 0.75;
    temp.dbcOffset = 48.0;
    rxBe.rows.append(temp);
    config.bus[0].sections.append(rxBe);

    // Transmit section on CAN 2 re-broadcasting Engine RPM at 20 Hz.
    ct::CommsSection tx;
    tx.name = QStringLiteral("Transmit 0x700");
    tx.device = ct::SectionDevice::TransmitMessage;
    tx.alignment = ct::SectionAlignment::WordSwap;
    tx.baseAddress = 0x700;
    tx.messageLengthBytes = 8;
    tx.transmitRateHz = 20;
    ct::CommsChannelRow txRpm;
    txRpm.channelName = QStringLiteral("Engine RPM");
    txRpm.startBit = 0;
    txRpm.bitLength = 16;
    txRpm.dbcType = int(ct::DbcType::Unsigned);
    txRpm.dbcFactor = 1.0;
    tx.rows.append(txRpm);
    config.bus[1].sections.append(tx);

    // v11 message relay on CAN 1: forward frames whose ID matches 0x7AA
    // (exact mask) to CAN 2 + CAN 3. The match ID is chosen so no other test
    // frame on CAN 1 (0x640/0x650/0x200/compound) triggers it.
    ct::CommsSection relay;
    relay.name = QStringLiteral("Relay 0x7AA");
    relay.device = ct::SectionDevice::MessageRelay;
    relay.baseAddress = 0x7AA;
    relay.relayBitmask = 0x7FF;
    relay.routeBusMask = (1 << 1) | (1 << 2); // forward to CAN 2 + CAN 3
    config.bus[0].sections.append(relay);

    // v3 counter: counts up on the Brake Switch rising edge.
    ct::CounterRow counter;
    counter.outputChannel = QStringLiteral("GP Raw 4");
    counter.upChannel = QStringLiteral("Engine RPM"); // >0 becomes "true"
    counter.minValue = 0;
    counter.maxValue = 100;
    counter.step = 1;
    config.counterRows.append(counter);

    // v3 timer: accumulates once Engine RPM goes non-zero. Store v12 spells the
    // trigger as a comparison, and this is the exact shape a pre-v12 timer
    // migrates into — the same edge on the same channel.
    ct::TimerRow timer;
    timer.outputChannel = QStringLiteral("GP Raw 3");
    timer.startTerm.aChannel = QStringLiteral("Engine RPM");
    timer.startTerm.op = ct::COND_OP_NEQ;
    config.timerRows.append(timer);

    // v5 condition (boolean logic channel): RPM High = (Engine RPM > 5000).
    ct::ConditionRow cond;
    cond.setTerms[0].aChannel = QStringLiteral("Engine RPM");
    cond.setTerms[0].op = ct::COND_OP_GT;
    cond.setTerms[0].bIsChannel = false;
    cond.setTerms[0].bConst = 5000;
    giveInverseReset(cond);
    cond.outputChannel = QStringLiteral("RPM High");
    config.conditionRows.append(cond);

    // v6 constant: Boost Target = 12.5 (float, 1 decimal) into its own channel.
    ct::ConstantRow konst;
    konst.name = QStringLiteral("Boost Target");
    konst.dataType = QStringLiteral("float");
    konst.decimalPlaces = 1;
    konst.value = 12.5;
    config.constantRows.append(konst);

    // Advanced Math through the real wire: a ternary op whose C operand rides
    // in raw bytes 19..22 â€” the record the 18-byte format could not carry.
    ct::MathRow madd;
    madd.op = ct::MATH_OP_MULADD;
    madd.aIsChannel = true;
    madd.aChannel = QStringLiteral("Engine RPM");
    madd.bIsChannel = false;
    madd.bConst = 0.5;
    madd.cIsChannel = false;
    madd.cConst = 100.0;
    madd.destChannel = QStringLiteral("RPM Scaled");
    config.mathRows.append(madd);

    const ct::MappingResult mapped = ct::mapToDevice(config);
    CHECK(mapped.ok());
    // Unified table: 2 receive + 1 transmit message.
    CHECK(mapped.tables.messages.size() == 3);
    CHECK(mapped.tables.counters.size() == 1);
    CHECK(mapped.tables.timers.size() == 1);
    CHECK(mapped.tables.constants.size() == 1);
    CHECK(mapped.tables.math.size() == 1);
    if (mapped.tables.math.size() == 1) {
        const ct::MathConfig &mc = mapped.tables.math[0];
        CHECK(mc.op == ct::MATH_OP_MULADD);
        CHECK(mc.input_c_type == 0);
        CHECK(ct::mathInputCConst(mc) == 100.0f);
        CHECK(mc.reserved0 == 0);
    }
    CHECK(mapped.tables.relays.size() == 1);
    if (mapped.tables.relays.size() == 1) {
        const auto &rl = mapped.tables.relays[0];
        CHECK(rl.address == 0x7AA);
        CHECK(rl.bitmask == 0x7FF);
        CHECK(rl.src_bus == 1);
        CHECK(rl.forward_bus_mask == 0x6); // CAN 2 + CAN 3, source masked out
        CHECK(rl.flags & ct::RELAYFLAG_ACTIVE);
        CHECK(!(rl.flags & ct::RELAYFLAG_EXTENDED));
        CHECK(!(rl.flags & ct::RELAYFLAG_INVERT));
    }
    int txMsgCount = 0;
    for (const auto &m : mapped.tables.messages)
        if (m.flags & ct::MSGFLAG_TRANSMIT)
            ++txMsgCount;
    CHECK(txMsgCount == 1);

    // ---- Upload through the real wire protocol ----
    CHECK(expectAck(ct::CMD_CLEAR_CONFIG, {}));
    CHECK(sendTable(ct::CMD_WRITE_MSG_CFG, mapped.tables.messages, ct::WRITE_CHUNK_MESSAGES));
    CHECK(sendTable(ct::CMD_WRITE_SIG_CFG, mapped.tables.signalConfigs, ct::WRITE_CHUNK_SIGNALS));
    CHECK(sendTable(ct::CMD_WRITE_MATH_CFG, mapped.tables.math, ct::WRITE_CHUNK_MATH));
    CHECK(sendTable(ct::CMD_WRITE_COND_CFG, mapped.tables.conditions, ct::WRITE_CHUNK_CONDITIONS));
    CHECK(sendTable(ct::CMD_WRITE_COUNTER_CFG, mapped.tables.counters, ct::WRITE_CHUNK_COUNTERS));
    CHECK(sendTable(ct::CMD_WRITE_TIMER_CFG, mapped.tables.timers, ct::WRITE_CHUNK_TIMERS));
    CHECK(sendTable(ct::CMD_WRITE_CONST_CFG, mapped.tables.constants, ct::WRITE_CHUNK_CONSTANTS));
    CHECK(sendTable(ct::CMD_WRITE_RELAY_CFG, mapped.tables.relays, ct::WRITE_CHUNK_RELAYS));

    // ---- Read back and compare byte-for-byte ----
    const QByteArray readMsgs =
        readRange(ct::CMD_READ_MSG_CFG, 0, quint16(mapped.tables.messages.size()));
    CHECK(readMsgs.size() == 4 + mapped.tables.messages.size() * int(sizeof(ct::CanMessageConfig)));
    CHECK(std::memcmp(readMsgs.constData() + 4, mapped.tables.messages.constData(),
                      size_t(readMsgs.size() - 4)) == 0);

    const QByteArray readSigs = readAllSignals(mapped.tables.signalConfigs.size());
    CHECK(readSigs.size()
          == mapped.tables.signalConfigs.size() * int(sizeof(ct::CanSignalConfig)));
    CHECK(std::memcmp(readSigs.constData(), mapped.tables.signalConfigs.constData(),
                      size_t(readSigs.size())) == 0);

    // The 24-byte math record through the generic length-checked write/read
    // paths (WRITE_CHUNK_MATH and READ_CHUNK_MATH are sized for 24 now â€” an
    // 18-byte sender NACKs on length, which is the whole cross-version story).
    const QByteArray readMath =
        readRange(ct::CMD_READ_MATH_CFG, 0, quint16(mapped.tables.math.size()));
    CHECK(readMath.size() == 4 + mapped.tables.math.size() * int(sizeof(ct::MathConfig)));
    CHECK(std::memcmp(readMath.constData() + 4, mapped.tables.math.constData(),
                      size_t(readMath.size() - 4)) == 0);

    const QByteArray readCounters =
        readRange(ct::CMD_READ_COUNTER_CFG, 0, quint16(mapped.tables.counters.size()));
    CHECK(readCounters.size() == 4 + mapped.tables.counters.size() * int(sizeof(ct::CounterConfig)));
    CHECK(std::memcmp(readCounters.constData() + 4, mapped.tables.counters.constData(),
                      size_t(readCounters.size() - 4)) == 0);

    const QByteArray readTimers =
        readRange(ct::CMD_READ_TIMER_CFG, 0, quint16(mapped.tables.timers.size()));
    CHECK(readTimers.size() == 4 + mapped.tables.timers.size() * int(sizeof(ct::TimerConfig)));

    const QByteArray readConsts =
        readRange(ct::CMD_READ_CONST_CFG, 0, quint16(mapped.tables.constants.size()));
    CHECK(readConsts.size() == 4 + mapped.tables.constants.size() * int(sizeof(ct::ConstantConfig)));
    CHECK(std::memcmp(readConsts.constData() + 4, mapped.tables.constants.constData(),
                      size_t(readConsts.size() - 4)) == 0);

    const QByteArray readRelays =
        readRange(ct::CMD_READ_RELAY_CFG, 0, quint16(mapped.tables.relays.size()));
    CHECK(readRelays.size() == 4 + mapped.tables.relays.size() * int(sizeof(ct::RelayConfig)));
    CHECK(std::memcmp(readRelays.constData() + 4, mapped.tables.relays.constData(),
                      size_t(readRelays.size() - 4)) == 0);

    // ---- v11 message relay: a matching frame on the source bus forwards to the
    // target buses; non-matching ID, wrong source bus and wrong frame type do
    // not forward. ----
    {
        const uint8_t payload[8] = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04};

        g_txFrames.clear();
        engine_process_can(1, 0x7AA, 0, 0, payload, 8); // match on CAN 1
        CHECK(g_txFrames.size() == 2);                  // forwarded to CAN 2 + CAN 3
        bool toBus2 = false, toBus3 = false;
        for (const CapturedTx &f : g_txFrames) {
            if (f.bus == 2) toBus2 = true;
            if (f.bus == 3) toBus3 = true;
            CHECK(f.id == 0x7AA);
            CHECK(f.len == 8);
            CHECK(std::memcmp(f.data, payload, 8) == 0);
        }
        CHECK(toBus2);
        CHECK(toBus3);

        g_txFrames.clear();
        engine_process_can(1, 0x7AB, 0, 0, payload, 8); // ID misses the exact mask
        CHECK(g_txFrames.isEmpty());

        g_txFrames.clear();
        engine_process_can(2, 0x7AA, 0, 0, payload, 8); // relay listens on CAN 1 only
        CHECK(g_txFrames.isEmpty());

        g_txFrames.clear();
        engine_process_can(1, 0x7AA, 1, 0, payload, 8); // extended frame, relay is standard
        CHECK(g_txFrames.isEmpty());
    }

    // ---- GET_STATUS carries the version tag ----
    {
        const auto packets = exchange(ct::CMD_GET_STATUS, {});
        CHECK(packets.size() == 1);
        if (packets.size() == 1) {
            CHECK(packets[0].cmd == ct::CMD_GET_STATUS);
            CHECK(packets[0].payload.size() == int(sizeof(ct::DeviceStatus)) + 2);
            // The firmware's reported version must match what the GUI speaks;
            // comparing against the constant keeps this honest across bumps.
            CHECK(quint8(packets[0].payload[sizeof(ct::DeviceStatus)])
                  == ct::PROTOCOL_VERSION_CURRENT);
        }
    }

    // ---- Termination resistor (protocol v9): CONTROL_CAN carries the new
    // termination byte and the old (10-byte) length is rejected. (The flash-
    // header persistence of termination is checked in its own block at the end,
    // once the config-dependent tests are done.) ----
    {
        ct::ControlCanPayload cc{};
        cc.bus_idx = 2;
        cc.mode = 1;
        cc.baud_rate = 500000;
        cc.data_baud_rate = 2000000;
        cc.termination = 1;
        const QByteArray payload(reinterpret_cast<const char *>(&cc), sizeof(cc));
        CHECK(payload.size() == 11);
        g_lastControl = ControlCanPayload{};
        CHECK(expectAck(ct::CMD_CONTROL_CAN, payload));
        CHECK(g_lastControl.bus_idx == 2);
        CHECK(g_lastControl.termination == 1);
        // An old-format (10-byte) CONTROL_CAN payload must NACK on length.
        CHECK(!expectAck(ct::CMD_CONTROL_CAN, payload.left(10)));
    }

    // ---- Feed CAN frames into the engine, check parsed values ----
    const int rpmIdx = mapped.channelToSignal.value(QStringLiteral("engine rpm"), -1);
    const int tempIdx = mapped.channelToSignal.value(QStringLiteral("engine temperature"), -1);
    CHECK(rpmIdx >= 0);
    CHECK(tempIdx >= 0);

    const uint8_t rpmFrame[8] = {0x10, 0x27, 0, 0, 0, 0, 0, 0}; // 10000 LE
    engine_process_can(1, 0x640, 0, 0, rpmFrame, 8);
    CHECK(qAbs(engine_signal_value(quint16(rpmIdx)) - 10000.0f) < 0.01f);

    // Big-endian: bytes 2..3 = 0x00C8 = 200 raw -> 200*0.75 + 48 = 198.0 Â°C
    const uint8_t tempFrame[8] = {0, 0, 0x00, 0xC8, 0, 0, 0, 0};
    engine_process_can(1, 0x650, 0, 0, tempFrame, 8);
    CHECK(qAbs(engine_signal_value(quint16(tempIdx)) - 198.0f) < 0.01f);

    // Out-of-range values clamp to the channel's physical range (max 250 Â°C).
    const uint8_t hotFrame[8] = {0, 0, 0x01, 0x2C, 0, 0, 0, 0}; // 300 raw -> 273
    engine_process_can(1, 0x650, 0, 0, hotFrame, 8);
    CHECK(qAbs(engine_signal_value(quint16(tempIdx)) - 250.0f) < 0.01f);

    // ---- TX composer: ticks accumulate to the 50 ms period, then the
    // frame carries the live RPM value packed back little-endian ----
    g_txFrames.clear();
    for (int i = 0; i < 5; ++i)
        engine_tick(10);
    CHECK(g_txFrames.size() == 1);
    if (g_txFrames.size() == 1) {
        CHECK(g_txFrames[0].bus == 2);
        CHECK(g_txFrames[0].id == 0x700);
        CHECK(g_txFrames[0].len == 8);
        CHECK(g_txFrames[0].data[0] == 0x10);
        CHECK(g_txFrames[0].data[1] == 0x27);
    }

    // ---- Counter + timer behaviour (engine tick semantics) ----
    // Engine RPM is currently 10000 (> 0 => "true"). The prior ticks already
    // set the counter/timer prev-state, so the counter's up edge has fired once
    // and the timer has been running ~50 ms.
    const int countIdx = mapped.channelToSignal.value(QStringLiteral("gp raw 4"), -1);
    const int timerIdx = mapped.channelToSignal.value(QStringLiteral("gp raw 3"), -1);
    CHECK(countIdx >= 0);
    CHECK(timerIdx >= 0);
    // Counter: rising edge counted once (RPM went 0 -> 10000 on the first tick).
    CHECK(qAbs(engine_signal_value(quint16(countIdx)) - 1.0f) < 0.01f);
    // Drop RPM to 0 then back up: one more rising edge -> count == 2.
    const uint8_t zeroFrame[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    engine_process_can(1, 0x640, 0, 0, zeroFrame, 8);
    engine_tick(10); // sees RPM 0 (falling edge), timer stops accumulating
    engine_process_can(1, 0x640, 0, 0, rpmFrame, 8);
    engine_tick(10); // rising edge again
    CHECK(qAbs(engine_signal_value(quint16(countIdx)) - 2.0f) < 0.01f);
    // Timer accumulated only while running (RPM > 0): started at tick 1, so it
    // is strictly positive and did not advance during the RPM==0 tick.
    CHECK(engine_signal_value(quint16(timerIdx)) > 0.0f);

    // ---- Condition (v5 boolean logic channel): RPM High = (Engine RPM > 5000) ----
    {
        const int condIdx = mapped.channelToSignal.value(QStringLiteral("rpm high"), -1);
        CHECK(condIdx >= 0);
        // RPM is 10000 (> 5000) -> output true.
        CHECK(qAbs(engine_signal_value(quint16(condIdx)) - 1.0f) < 0.01f);
        // Drop RPM below the threshold -> output false.
        const uint8_t lowFrame[8] = {0xE8, 0x03, 0, 0, 0, 0, 0, 0}; // 1000
        engine_process_can(1, 0x640, 0, 0, lowFrame, 8);
        engine_tick(10);
        CHECK(qAbs(engine_signal_value(quint16(condIdx))) < 0.01f);
        // Restore RPM so the later timeout test starts from the live value.
        engine_process_can(1, 0x640, 0, 0, rpmFrame, 8);
        engine_tick(10);
        CHECK(qAbs(engine_signal_value(quint16(condIdx)) - 1.0f) < 0.01f);
    }

    // ---- Constant (v6): its channel holds the fixed value every pass ----
    {
        const int boostIdx = mapped.channelToSignal.value(QStringLiteral("boost target"), -1);
        CHECK(boostIdx >= 0);
        engine_tick(10);
        CHECK(qAbs(engine_signal_value(quint16(boostIdx)) - 12.5f) < 0.01f);
    }

    // ---- Streams arrive framed and parse with the GUI splitter ----
    {
        ct::FrameSplitter splitter;
        g_wire.clear();
        serial_proto_stream_monitor(1, 0, 0x640, 0, 0, 0, 0, rpmFrame, 8);
        serial_proto_stream_values();
        serial_proto_log("hello from firmware", 19);
        const auto packets = splitter.feed(g_wire);
        int monitor = 0, values = 0, logs = 0;
        for (const auto &p : packets) {
            if (p.cmd == ct::CMD_MONITOR_STREAM) {
                ++monitor;
                // Trimmed to what the frame carries, not the struct's FD-sized
                // reservation: this one has 8 data bytes, so 12 + 8 rather than
                // 76. That is most of the stream's cost on a bus of short
                // frames, and the stream is the one that overruns first.
                CHECK(p.payload.size() == ct::MONITOR_HEADER_BYTES + 8);
                // And the Manager's own parser accepts what the firmware framed
                // -- the two halves of the change agreeing on one buffer, rather
                // than each being checked against its own idea of the format.
                ct::MonitorStreamPayload decoded{};
                CHECK(ct::DeviceLink::parseMonitorPayload(p.payload, decoded));
                CHECK(decoded.can_id == 0x640 && decoded.data_len == 8);
            } else if (p.cmd == ct::CMD_VALUE_STREAM) {
                ++values;
            } else if (p.cmd == ct::CMD_LOG) {
                ++logs;
                CHECK(p.payload == QByteArray("hello from firmware"));
            }
        }
        CHECK(monitor == 1);
        CHECK(values >= 1);
        CHECK(logs == 1);
    }

    // ---- Stream gating ----
    CHECK(expectAck(ct::CMD_STREAM_VALUES, QByteArray(1, char(0))));
    {
        ct::FrameSplitter splitter;
        g_wire.clear();
        serial_proto_stream_monitor(1, 0, 0x640, 0, 0, 0, 0, rpmFrame, 8);
        serial_proto_stream_values();
        CHECK(splitter.feed(g_wire).isEmpty());
    }
    CHECK(expectAck(ct::CMD_STREAM_VALUES,
                    QByteArray(1, char(ct::STREAM_ENABLE_VALUES | ct::STREAM_ENABLE_MONITOR))));

    // ---- Configuration name (v7): set it, read it back verbatim ----
    {
        QByteArray name(ct::CONFIG_NAME_LEN, '\0');
        std::memcpy(name.data(), "EngineMap_A", 11);
        CHECK(expectAck(ct::CMD_WRITE_CONFIG_NAME, name));
        const auto pkts = exchange(ct::CMD_READ_CONFIG_NAME, {});
        CHECK(pkts.size() == 1);
        if (pkts.size() == 1) {
            CHECK(pkts[0].cmd == ct::CMD_READ_CONFIG_NAME);
            CHECK(pkts[0].payload.size() == ct::CONFIG_NAME_LEN);
            CHECK(pkts[0].payload.left(11) == QByteArray("EngineMap_A"));
        }
    }

    // ---- Reset (v7): ACKs and schedules the glue's reset callback ----
    {
        g_resetRequested = false;
        CHECK(expectAck(ct::CMD_RESET_DEVICE, {}));
        CHECK(g_resetRequested);
    }

    // ---- Flash persistence (v7 flash-resident): SAVE commits the header so
    // the image is valid; the records were programmed in place by the WRITEs
    // and stay readable. (A simulated reboot + hard CLEAR are checked at the
    // very end, once the config-dependent tests are done.) ----
    CHECK(expectAck(ct::CMD_SAVE_TO_FLASH, {}));
    CHECK(flash_store_present()); // header committed, CRC over the live records valid
    {
        const QByteArray savedMsgs =
            readRange(ct::CMD_READ_MSG_CFG, 0, quint16(mapped.tables.messages.size()));
        CHECK(savedMsgs.size() == 4 + mapped.tables.messages.size() * int(sizeof(ct::CanMessageConfig)));
        CHECK(std::memcmp(savedMsgs.constData() + 4, mapped.tables.messages.constData(),
                          size_t(savedMsgs.size() - 4)) == 0);
        const QByteArray savedSigs = readAllSignals(mapped.tables.signalConfigs.size());
        CHECK(savedSigs.size()
              == mapped.tables.signalConfigs.size() * int(sizeof(ct::CanSignalConfig)));
        CHECK(std::memcmp(savedSigs.constData(), mapped.tables.signalConfigs.constData(),
                          size_t(savedSigs.size())) == 0);
    }
    // Reading PAST the written prefix (as Get Configuration does over the full
    // table capacity) must return inactive/zeroed records, not erased 0xFF â€”
    // otherwise the GUI would reconstruct un-programmed slots as active garbage.
    {
        const int written = mapped.tables.messages.size();
        const int n = written + 8;
        const QByteArray tail = readRange(ct::CMD_READ_MSG_CFG, 0, quint16(n));
        CHECK(tail.size() == 4 + n * int(sizeof(ct::CanMessageConfig)));
        for (int i = written; i < n; ++i) {
            ct::CanMessageConfig m;
            std::memcpy(&m, tail.constData() + 4 + i * int(sizeof(ct::CanMessageConfig)), sizeof(m));
            CHECK((m.flags & ct::MSGFLAG_ACTIVE) == 0); // inactive, not 0xFF garbage
        }
    }

    // ---- Inject: processed as RX and physically transmitted ----
    g_txFrames.clear();
    {
        ct::InjectCanPayload inject{};
        inject.bus_idx = 1;
        inject.can_id = 0x640;
        inject.flags = 0;
        inject.data_len = 8;
        std::memcpy(inject.data, rpmFrame, 8);
        QByteArray payload(reinterpret_cast<const char *>(&inject), sizeof(inject));
        // Inject answers with a monitor-stream frame for the injected RX
        // followed by the ACK.
        const auto packets = exchange(ct::CMD_INJECT_CAN_FRAME, payload);
        bool acked = false, monitored = false;
        for (const auto &p : packets) {
            // ACK is now [status, crc_hi, crc_lo]; this test only cares that it
            // is an OK ACK, not the echo (testAckCrcEcho covers that).
            if (p.cmd == ct::CMD_ACK && !p.payload.isEmpty() && p.payload[0] == char(0))
                acked = true;
            if (p.cmd == ct::CMD_MONITOR_STREAM)
                monitored = true;
        }
        CHECK(acked);
        CHECK(monitored);
        CHECK(g_txFrames.size() >= 1); // physical TX on bus 1
    }

    // ---- Unknown command NACKs; bad CRC NACKs; noise between frames is
    // survived by the firmware's own splitter ----
    {
        const auto packets = exchange(0x7E, {});
        CHECK(packets.size() == 1 && packets[0].cmd == ct::CMD_NACK);
    }

    // ---- Receive timeout (protocol v4): a receive message not seen within its
    // timeout reverts its signals to the configured default; a fresh frame
    // clears the timeout and restores the live value. ----
    {
        engine_process_can(1, 0x640, 0, 0, rpmFrame, 8); // live again, window reset
        CHECK(qAbs(engine_signal_value(quint16(rpmIdx)) - 10000.0f) < 0.01f);
        engine_tick(3000); // 3 s with no frame > 2200 ms timeout -> default
        CHECK(qAbs(engine_signal_value(quint16(rpmIdx)) - 1234.0f) < 0.01f);
        engine_process_can(1, 0x640, 0, 0, rpmFrame, 8);
        engine_tick(10);
        CHECK(qAbs(engine_signal_value(quint16(rpmIdx)) - 10000.0f) < 0.01f);
    }

    // ---- Flash-resident reboot + hard clear: the committed image reloads
    // after a simulated power cycle; CLEAR erases it (single-copy model, so a
    // later reload finds nothing). Resets runtime, so it runs after the
    // config-dependent checks above. ----
    {
        CHECK(engine_load_config(nullptr)); // "reboot": header + records still valid
        const QByteArray reMsgs =
            readRange(ct::CMD_READ_MSG_CFG, 0, quint16(mapped.tables.messages.size()));
        CHECK(std::memcmp(reMsgs.constData() + 4, mapped.tables.messages.constData(),
                          size_t(reMsgs.size() - 4)) == 0);
        // The relay table survives the reload byte-for-byte...
        const QByteArray reRelays =
            readRange(ct::CMD_READ_RELAY_CFG, 0, quint16(mapped.tables.relays.size()));
        CHECK(std::memcmp(reRelays.constData() + 4, mapped.tables.relays.constData(),
                          size_t(reRelays.size() - 4)) == 0);
        // ...and still forwards after a reboot (concern: relays dead after a
        // disconnected power cycle if g_count[ENGINE_TABLE_RELAYS] didn't reload).
        {
            const uint8_t p[8] = {0x55, 0x66, 0x77, 0x88, 0, 0, 0, 0};
            g_txFrames.clear();
            engine_process_can(1, 0x7AA, 0, 0, p, 8);
            CHECK(g_txFrames.size() == 2); // forwarded to CAN 2 + CAN 3 post-reload
        }
        // The configuration name persisted through save + reload too.
        const auto namePkts = exchange(ct::CMD_READ_CONFIG_NAME, {});
        CHECK(namePkts.size() == 1);
        if (namePkts.size() == 1)
            CHECK(namePkts[0].payload.left(11) == QByteArray("EngineMap_A"));
        CHECK(expectAck(ct::CMD_CLEAR_CONFIG, {})); // erase the image
        CHECK(!engine_load_config(nullptr));        // nothing left to reload
    }

    // ---- Compound / multiplexed message (protocol v8): one CAN ID carries
    // several sub-messages selected by a multiplexor byte. Gated signals extract
    // only while their selector matches; always-present signals extract always.
    // Rebuilds engine state, so it runs after the config-dependent checks. ----
    {
        EngineCallbacks cb{};
        cb.transmit_can = captureTransmit;
        engine_init(&cb);
        engine_clear_config();

        ct::Configuration ccfg;
        ccfg.bus[0].enabled = true;
        const auto addCh = [&ccfg](const char *n, double minV, double maxV) {
            ct::Channel c;
            c.name = QString::fromUtf8(n);
            c.baseResolution = 1.0;
            c.minValue = minV;
            c.maxValue = maxV;
            c.userDefined = true;
            ccfg.catalog().addOrUpdateUserChannel(c);
        };
        addCh("A Value", 0, 65535);
        addCh("B Value", 0, 65535);

        ct::CommsSection cs;
        cs.name = QStringLiteral("Receive 0x200");
        cs.device = ct::SectionDevice::ReceiveMessage;
        cs.alignment = ct::SectionAlignment::WordSwap; // Intel
        cs.baseAddress = 0x200;
        cs.messageLengthBytes = 8;
        cs.compound = true;
        // Channels live only inside identifiers (no always-present set).
        // mux == 1 -> A Value; mux == 2 -> B Value. Both live in bytes 1..2, so
        // they deliberately overlap â€” the mux keeps them mutually exclusive.
        ct::CompoundIdentifier id1;
        id1.byteOffset = 0;
        id1.id = 1;
        id1.idMask = 0xFF;
        id1.configured = true;
        ct::CommsChannelRow a;
        a.channelName = QStringLiteral("A Value");
        a.startBit = 8;
        a.bitLength = 16;
        a.dbcFactor = 1.0;
        id1.rows.append(a);
        cs.identifiers.append(id1);
        ct::CompoundIdentifier id2;
        id2.byteOffset = 0;
        id2.id = 2;
        id2.idMask = 0xFF;
        id2.configured = true;
        ct::CommsChannelRow b;
        b.channelName = QStringLiteral("B Value");
        b.startBit = 8;
        b.bitLength = 16;
        b.dbcFactor = 1.0;
        id2.rows.append(b);
        cs.identifiers.append(id2);
        ccfg.bus[0].sections.append(cs);

        const ct::MappingResult cm = ct::mapToDevice(ccfg);
        CHECK(cm.ok());
        CHECK(cm.tables.messages.size() == 1);
        // Two from the document, then the device channels every Send carries.
        // They are allocated last, so the two below keep indices 0 and 1.
        CHECK(cm.tables.signalConfigs.size() == 2 + ct::DEVCH_COUNT);

        CHECK(expectAck(ct::CMD_CLEAR_CONFIG, {}));
        CHECK(sendTable(ct::CMD_WRITE_MSG_CFG, cm.tables.messages, ct::WRITE_CHUNK_MESSAGES));
        CHECK(sendTable(ct::CMD_WRITE_SIG_CFG, cm.tables.signalConfigs, ct::WRITE_CHUNK_SIGNALS));

        const int aIdx = cm.channelToSignal.value(QStringLiteral("a value"), -1);
        const int bIdx = cm.channelToSignal.value(QStringLiteral("b value"), -1);
        CHECK(aIdx >= 0);
        CHECK(bIdx >= 0);

        // mux = 1, payload 100 in bytes 1..2 -> A updates, B untouched (0).
        const uint8_t f1[8] = {0x01, 0x64, 0x00, 0, 0, 0, 0, 0};
        engine_process_can(1, 0x200, 0, 0, f1, 8);
        CHECK(qAbs(engine_signal_value(quint16(aIdx)) - 100.0f) < 0.01f);
        CHECK(qAbs(engine_signal_value(quint16(bIdx)) - 0.0f) < 0.01f);

        // mux = 2, payload 200 -> B updates; A holds its previous value (100).
        const uint8_t f2[8] = {0x02, 0xC8, 0x00, 0, 0, 0, 0, 0};
        engine_process_can(1, 0x200, 0, 0, f2, 8);
        CHECK(qAbs(engine_signal_value(quint16(bIdx)) - 200.0f) < 0.01f);
        CHECK(qAbs(engine_signal_value(quint16(aIdx)) - 100.0f) < 0.01f); // held

        // An unknown mux value matches no identifier: nothing updates.
        const uint8_t f3[8] = {0x09, 0xFF, 0xFF, 0, 0, 0, 0, 0};
        engine_process_can(1, 0x200, 0, 0, f3, 8);
        CHECK(qAbs(engine_signal_value(quint16(aIdx)) - 100.0f) < 0.01f); // held
        CHECK(qAbs(engine_signal_value(quint16(bIdx)) - 200.0f) < 0.01f); // held

        // Get reconstruction: two identifiers, no rows outside them.
        ct::Configuration recon;
        ct::mapFromDevice(cm.tables, recon);
        bool found = false;
        for (int bi = 0; bi < 3 && !found; ++bi)
            for (const ct::CommsSection &s : recon.bus[bi].sections)
                if (s.baseAddress == 0x200) {
                    found = true;
                    CHECK(s.compound);
                    CHECK(s.rows.isEmpty());          // no always-present set
                    CHECK(s.identifiers.size() == 2); // A Value, B Value
                }
        CHECK(found);
    }

    // ---- v13 lookup tables: 2x16 interpolated + discrete-centered, split
    // across the Def/Out record pair; axis inputs are received signals so the
    // lookup runs in engine_process_can. (The 2-axis table is now the 8x8 and
    // has its own block below.) Self-contained (own engine_init + erase). ----
    {
        EngineCallbacks cb{};
        cb.transmit_can = captureTransmit;
        engine_init(&cb);
        engine_clear_config();

        ct::Configuration tcfg;
        tcfg.bus[0].enabled = true;
        const auto addC = [&tcfg](const char *n) {
            ct::Channel c;
            c.name = QString::fromUtf8(n);
            c.baseResolution = 1.0;
            c.minValue = -1e9;
            c.maxValue = 1e9;
            c.userDefined = true;
            tcfg.catalog().addOrUpdateUserChannel(c);
        };
        addC("X");
        addC("Y");

        ct::CommsSection rx;
        rx.name = QStringLiteral("Receive 0x300");
        rx.device = ct::SectionDevice::ReceiveMessage;
        rx.alignment = ct::SectionAlignment::WordSwap; // Intel LE
        rx.baseAddress = 0x300;
        rx.messageLengthBytes = 8;
        ct::CommsChannelRow rowX;
        rowX.channelName = QStringLiteral("X");
        rowX.startBit = 0;
        rowX.bitLength = 16;
        rowX.dbcFactor = 1.0;
        ct::CommsChannelRow rowY;
        rowY.channelName = QStringLiteral("Y");
        rowY.startBit = 16;
        rowY.bitLength = 16;
        rowY.dbcFactor = 1.0;
        rx.rows.append(rowX);
        rx.rows.append(rowY);
        tcfg.bus[0].sections.append(rx);

        // Uses the FULL 16-site width (v13): sites 0,10,..,150 -> 0,100,..,1500.
        // The top sites exercise the range the old 8-site table could not reach.
        ct::Table2x16Row t1; // interpolated: out = 100 * (X/10) between sites
        t1.outputChannel = QStringLiteral("Out1");
        t1.xChannel = QStringLiteral("X");
        t1.xInterp = true;
        for (int k = 0; k < ct::TABLE_2X16_SITES; ++k) {
            t1.xSites.append(k * 10);
            t1.outputs.append(k * 100);
        }
        tcfg.table2x16Rows.append(t1);
        ct::Table2x16Row t2 = t1; // discrete-centered, same sites/outputs
        t2.outputChannel = QStringLiteral("Out2");
        t2.xInterp = false;
        tcfg.table2x16Rows.append(t2);

        const ct::MappingResult tm = ct::mapToDevice(tcfg);
        CHECK(tm.ok());
        CHECK(tm.tables.tables2x16Def.size() == 2);
        // The two halves of a 2x16 table are always emitted in lockstep.
        CHECK(tm.tables.tables2x16Out.size() == tm.tables.tables2x16Def.size());

        CHECK(expectAck(ct::CMD_CLEAR_CONFIG, {}));
        CHECK(sendTable(ct::CMD_WRITE_MSG_CFG, tm.tables.messages, ct::WRITE_CHUNK_MESSAGES));
        CHECK(sendTable(ct::CMD_WRITE_SIG_CFG, tm.tables.signalConfigs, ct::WRITE_CHUNK_SIGNALS));
        // Values first, definitions second â€” the order the GUI uses so a table
        // can never go live against outputs that have not landed yet.
        CHECK(sendTable(ct::CMD_WRITE_TABLE2X16_OUT, tm.tables.tables2x16Out,
                        ct::WRITE_CHUNK_TABLES_2X16_OUT));
        CHECK(sendTable(ct::CMD_WRITE_TABLE2X16_DEF, tm.tables.tables2x16Def,
                        ct::WRITE_CHUNK_TABLES_2X16_DEF));

        // Byte-exact readback of every table.
        const QByteArray rDef =
            readRange(ct::CMD_READ_TABLE2X16_DEF, 0, quint16(tm.tables.tables2x16Def.size()));
        CHECK(rDef.size() == 4 + tm.tables.tables2x16Def.size() * int(sizeof(ct::Table2x16Def)));
        CHECK(std::memcmp(rDef.constData() + 4, tm.tables.tables2x16Def.constData(),
                          size_t(rDef.size() - 4)) == 0);
        const QByteArray rOut =
            readRange(ct::CMD_READ_TABLE2X16_OUT, 0, quint16(tm.tables.tables2x16Out.size()));
        CHECK(rOut.size() == 4 + tm.tables.tables2x16Out.size() * int(sizeof(ct::Table2x16Out)));
        CHECK(std::memcmp(rOut.constData() + 4, tm.tables.tables2x16Out.constData(),
                          size_t(rOut.size() - 4)) == 0);
        const int o1 = tm.channelToSignal.value(QStringLiteral("out1"), -1);
        const int o2 = tm.channelToSignal.value(QStringLiteral("out2"), -1);
        CHECK(o1 >= 0 && o2 >= 0);

        const auto feed = [&](int x, int y) {
            const uint8_t f[8] = {uint8_t(x & 0xFF), uint8_t((x >> 8) & 0xFF),
                                  uint8_t(y & 0xFF), uint8_t((y >> 8) & 0xFF), 0, 0, 0, 0};
            engine_process_can(1, 0x300, 0, 0, f, 8);
        };

        feed(15, 0);
        CHECK(qAbs(engine_signal_value(quint16(o1)) - 150.0f) < 0.01f); // interp midway
        CHECK(qAbs(engine_signal_value(quint16(o2)) - 200.0f) < 0.01f); // discrete: at midpoint -> upper
        feed(14, 0);
        CHECK(qAbs(engine_signal_value(quint16(o2)) - 100.0f) < 0.01f); // discrete: below midpoint -> lower
        // Sites 8..15 only exist at the v13 width â€” an 8-site table would have
        // clamped to 700 well below these inputs.
        feed(95, 0);
        CHECK(qAbs(engine_signal_value(quint16(o1)) - 950.0f) < 0.01f); // interp in the upper half
        feed(150, 0);
        CHECK(qAbs(engine_signal_value(quint16(o1)) - 1500.0f) < 0.01f); // exactly the top site
        feed(100000, 0);
        CHECK(qAbs(engine_signal_value(quint16(o1)) - 1500.0f) < 0.01f); // clamp above top site
    }

    // ---- v13 partial 2x16 image: a definition written WITHOUT its outputs (an
    // upload interrupted between the two writes) must not be evaluated. The Out
    // slot is un-programmed flash â€” all 0xFF, which reinterprets as NaN â€” so a
    // missing pair guard would poison the output channel instead of leaving it
    // at its previous value. Self-contained (own engine_init + erase). ----
    {
        EngineCallbacks cb{};
        cb.transmit_can = captureTransmit;
        engine_init(&cb);
        engine_clear_config();

        ct::Configuration pcfg;
        pcfg.bus[0].enabled = true;
        ct::Channel xc;
        xc.name = QStringLiteral("PX");
        xc.baseResolution = 1.0;
        xc.minValue = -1e9;
        xc.maxValue = 1e9;
        xc.userDefined = true;
        pcfg.catalog().addOrUpdateUserChannel(xc);

        ct::CommsSection rx;
        rx.name = QStringLiteral("Receive 0x301");
        rx.device = ct::SectionDevice::ReceiveMessage;
        rx.alignment = ct::SectionAlignment::WordSwap;
        rx.baseAddress = 0x301;
        rx.messageLengthBytes = 8;
        ct::CommsChannelRow rowX;
        rowX.channelName = QStringLiteral("PX");
        rowX.startBit = 0;
        rowX.bitLength = 16;
        rowX.dbcFactor = 1.0;
        rx.rows.append(rowX);
        pcfg.bus[0].sections.append(rx);

        ct::Table2x16Row pt;
        pt.outputChannel = QStringLiteral("POut");
        pt.xChannel = QStringLiteral("PX");
        pt.xInterp = true;
        for (int k = 0; k < 4; ++k) {
            pt.xSites.append(k * 10);
            pt.outputs.append(k * 100);
        }
        pcfg.table2x16Rows.append(pt);

        const ct::MappingResult pm = ct::mapToDevice(pcfg);
        CHECK(pm.ok());
        CHECK(expectAck(ct::CMD_CLEAR_CONFIG, {}));
        CHECK(sendTable(ct::CMD_WRITE_MSG_CFG, pm.tables.messages, ct::WRITE_CHUNK_MESSAGES));
        CHECK(sendTable(ct::CMD_WRITE_SIG_CFG, pm.tables.signalConfigs, ct::WRITE_CHUNK_SIGNALS));
        // Deliberately write ONLY the definition â€” the interrupted-upload case.
        CHECK(sendTable(ct::CMD_WRITE_TABLE2X16_DEF, pm.tables.tables2x16Def,
                        ct::WRITE_CHUNK_TABLES_2X16_DEF));

        const int po = pm.channelToSignal.value(QStringLiteral("pout"), -1);
        CHECK(po >= 0);
        const uint8_t f[8] = {15, 0, 0, 0, 0, 0, 0, 0};
        engine_process_can(1, 0x301, 0, 0, f, 8);
        const float v = engine_signal_value(quint16(po));
        CHECK(v == v);        // not NaN â€” the table was skipped, not evaluated
        CHECK(qAbs(v) < 0.01f); // slot untouched, still its initial 0

        // Completing the pair makes the very same table evaluate normally.
        CHECK(sendTable(ct::CMD_WRITE_TABLE2X16_OUT, pm.tables.tables2x16Out,
                        ct::WRITE_CHUNK_TABLES_2X16_OUT));
        engine_process_can(1, 0x301, 0, 0, f, 8);
        CHECK(qAbs(engine_signal_value(quint16(po)) - 150.0f) < 0.01f);
    }

    // ---- The 8x8 lookup table, bilinear over the full grid. Sites ascend
    // 10..80 on both axes and cell(x, y) = x + 10*y, so every one of the 64
    // cells is a distinct number that names itself: a mis-indexed read does not
    // come back approximately right, it comes back as a different cell.
    //
    // Every expectation is computed by hand from axisResolve's contract â€”
    // value = out[i0]*(1-w) + out[i1]*w per axis, bilinear across the four
    // bracketed cells â€” and every one of them is exactly representable in
    // float32, so the tolerances below are catching mistakes, not rounding.
    // Self-contained (own engine_init + erase). ----
    {
        EngineCallbacks cb{};
        cb.transmit_can = captureTransmit;
        engine_init(&cb);
        engine_clear_config();
        CHECK(t8::installAxes());

        const ct::Table8x8Def def = t8::makeDef(
            t8::kOut0, quint8(ct::TABLEFLAG_ACTIVE | ct::TABLEFLAG_X_INTERP | ct::TABLEFLAG_Y_INTERP),
            8, 8);
        ct::Table8x8GridRow rows[ct::TABLE_8X8_SITES];
        t8::makeRows(rows, 0.0f);
        // Rows before Def, the ordering the host uses â€” a table must never be
        // able to go live against a grid that has not landed.
        CHECK(engine_table_write(ENGINE_TABLE_TABLES_8X8_ROW, 0, ct::TABLE_8X8_SITES,
                                 reinterpret_cast<const uint8_t *>(rows)));
        CHECK(engine_table_write(ENGINE_TABLE_TABLES_8X8_DEF, 0, 1,
                                 reinterpret_cast<const uint8_t *>(&def)));

        const auto out0 = [] { return engine_signal_value(t8::kOut0); };

        // Exactly on a site on both axes: the cell itself, no blending.
        t8::feed(10, 10);
        CHECK(qAbs(out0() - 0.0f) < 0.01f); // cell(0,0)
        // The far corner. This is the one that proves the eighth row slot is
        // where the engine thinks it is â€” with a padded row record the pointer
        // walk would land 56 bytes short of it.
        t8::feed(80, 80);
        CHECK(qAbs(out0() - 77.0f) < 0.01f); // cell(7,7) = 7 + 70
        // One axis midway, the other on a site: a plain linear blend.
        t8::feed(15, 10);
        CHECK(qAbs(out0() - 0.5f) < 0.01f); // (0 + 1)/2
        t8::feed(10, 15);
        CHECK(qAbs(out0() - 5.0f) < 0.01f); // (0 + 10)/2
        // Both axes midway â€” the genuinely bilinear case. Cells 0, 1, 10, 11
        // at weight 0.5 each way: top = 0.5, bottom = 10.5, result 5.5.
        t8::feed(15, 15);
        CHECK(qAbs(out0() - 5.5f) < 0.01f);
        // Bilinear away from the origin, where a row-vs-column transposition
        // stops being symmetric and shows: cells 31, 32, 41, 42 -> 36.5.
        t8::feed(25, 45);
        CHECK(qAbs(out0() - 36.5f) < 0.01f);
        // X pinned to the LAST site while Y blends: cells 7 and 17 -> 12.
        t8::feed(80, 15);
        CHECK(qAbs(out0() - 12.0f) < 0.01f);
        // Y clamped past the top while X blends inside row 7: 73 and 74 -> 73.5.
        t8::feed(45, 1000);
        CHECK(qAbs(out0() - 73.5f) < 0.01f);

        // Clamping past the end sites, in every direction. Inputs outside the
        // axis hold the end cell rather than extrapolating â€” an extrapolating
        // table drives an output channel somewhere no cell of the grid ever
        // said it should go.
        t8::feed(0, 0);
        CHECK(qAbs(out0() - 0.0f) < 0.01f); // below both -> cell(0,0)
        t8::feed(0, 80);
        CHECK(qAbs(out0() - 70.0f) < 0.01f); // X below, Y at the top -> cell(0,7)
        t8::feed(1000, 0);
        CHECK(qAbs(out0() - 7.0f) < 0.01f); // X above, Y below -> cell(7,0)
        t8::feed(1000, 1000);
        CHECK(qAbs(out0() - 77.0f) < 0.01f); // above both -> cell(7,7)
    }

    // ---- Discrete-centered axes, and the row indexing of a SECOND table.
    //
    // Two tables in one image: table 0 discrete on both axes, table 1 discrete
    // on Y and interpolated on X. Table 1's grid is offset by 1000, and table 1
    // owns rows 8..15 â€” so if the engine read table t's grid from row 0 instead
    // of row t*8, the answers here would come back around 10 instead of around
    // 1010. That is the arithmetic the contiguous-row layout depends on, and it
    // is worth one whole table to state it. Self-contained. ----
    {
        EngineCallbacks cb{};
        cb.transmit_can = captureTransmit;
        engine_init(&cb);
        engine_clear_config();
        CHECK(t8::installAxes());

        const ct::Table8x8Def defs[2] = {
            t8::makeDef(t8::kOut0, quint8(ct::TABLEFLAG_ACTIVE), 8, 8),
            t8::makeDef(t8::kOut1, quint8(ct::TABLEFLAG_ACTIVE | ct::TABLEFLAG_X_INTERP), 8, 8),
        };
        ct::Table8x8GridRow rows[2 * ct::TABLE_8X8_SITES];
        t8::makeRows(rows, 0.0f);                        // table 0: rows 0..7
        t8::makeRows(rows + ct::TABLE_8X8_SITES, 1000.0f); // table 1: rows 8..15
        CHECK(engine_table_write(ENGINE_TABLE_TABLES_8X8_ROW, 0, 2 * ct::TABLE_8X8_SITES,
                                 reinterpret_cast<const uint8_t *>(rows)));
        CHECK(engine_table_write(ENGINE_TABLE_TABLES_8X8_DEF, 0, 2,
                                 reinterpret_cast<const uint8_t *>(defs)));

        const auto out0 = [] { return engine_signal_value(t8::kOut0); };
        const auto out1 = [] { return engine_signal_value(t8::kOut1); };

        // Exactly at the midpoint between two sites, discrete-centered takes
        // the UPPER site (frac < 0.5 keeps the lower one, and 0.5 is not < 0.5).
        // That boundary is the entire behavioural difference between the two
        // axis modes at a single input, so it gets checked from both sides.
        t8::feed(15, 15);
        CHECK(qAbs(out0() - 11.0f) < 0.01f); // cell(1,1), both axes stepped up
        CHECK(qAbs(out1() - 1010.5f) < 0.01f); // Y -> row 1; X blends 1010, 1011
        t8::feed(14, 14);
        CHECK(qAbs(out0() - 0.0f) < 0.01f); // frac 0.4: both hold the lower site
        CHECK(qAbs(out1() - 1000.4f) < 0.01f); // Y -> row 0; X blends 1000, 1001 at 0.4
        // Further up the grid, where a table that read the wrong rows would be
        // off by exactly 1000 rather than by a rounding error.
        t8::feed(25, 25);
        CHECK(qAbs(out0() - 22.0f) < 0.01f); // cell(2,2)
        // Row 2 of TABLE 1's grid, X blending cells 1 and 2: (1021 + 1022)/2.
        // Read out of table 0's rows instead, this would come back as 21.5.
        CHECK(qAbs(out1() - 1021.5f) < 0.01f);
        // A discrete axis clamps like an interpolated one.
        t8::feed(1000, 1000);
        CHECK(qAbs(out0() - 77.0f) < 0.01f);
        CHECK(qAbs(out1() - 1077.0f) < 0.01f);
    }

    // ---- A partially filled table: 3 X sites and 2 Y sites of the 8 each
    // axis has room for. Every cell outside that 3x2 corner is poisoned with a
    // sentinel, so "the counts bound the lookup" is not an assertion about
    // intent â€” if x_count or y_count is ignored, the poison comes out of the
    // output channel. Self-contained. ----
    {
        EngineCallbacks cb{};
        cb.transmit_can = captureTransmit;
        engine_init(&cb);
        engine_clear_config();
        CHECK(t8::installAxes());

        constexpr float kPoison = -999.0f;
        const ct::Table8x8Def def = t8::makeDef(
            t8::kOut0, quint8(ct::TABLEFLAG_ACTIVE | ct::TABLEFLAG_X_INTERP | ct::TABLEFLAG_Y_INTERP),
            3, 2); // sites: X 10,20,30 and Y 10,20 â€” the rest left at zero
        ct::Table8x8GridRow rows[ct::TABLE_8X8_SITES];
        for (int y = 0; y < ct::TABLE_8X8_SITES; ++y)
            for (int x = 0; x < ct::TABLE_8X8_SITES; ++x)
                rows[y].v[x] = (x < 3 && y < 2) ? float(x) + 10.0f * float(y) : kPoison;
        CHECK(engine_table_write(ENGINE_TABLE_TABLES_8X8_ROW, 0, ct::TABLE_8X8_SITES,
                                 reinterpret_cast<const uint8_t *>(rows)));
        CHECK(engine_table_write(ENGINE_TABLE_TABLES_8X8_DEF, 0, 1,
                                 reinterpret_cast<const uint8_t *>(&def)));

        const auto out0 = [] { return engine_signal_value(t8::kOut0); };

        t8::feed(15, 10);
        CHECK(qAbs(out0() - 0.5f) < 0.01f); // cells 0 and 1 at half weight
        t8::feed(15, 15);
        CHECK(qAbs(out0() - 5.5f) < 0.01f); // the 2x2 corner, bilinear
        // THE check. Bounded by x_count = 3, an input of 35 is simply past the
        // last used site and clamps to cell(2,0) = 2. A lookup that walked all
        // eight sites would be walking 10, 20, 30, 0, 0, 0, 0, 0 â€” an axis that
        // no longer ascends â€” and would land somewhere in the poisoned cells.
        // Either way it is not 2, which is the point: the counts are what make
        // the unused half of the record unreachable rather than merely unused.
        t8::feed(35, 5);
        CHECK(qAbs(out0() - 2.0f) < 0.01f);
        // Same argument on the Y axis, and both at once.
        t8::feed(5, 35);
        CHECK(qAbs(out0() - 10.0f) < 0.01f); // clamps to cell(0,1)
        t8::feed(1000, 1000);
        CHECK(qAbs(out0() - 12.0f) < 0.01f); // cell(2,1), the last used corner
    }

    // ---- The torn-upload guard, which is the reason the Def carries the
    // ACTIVE flag and the rows carry nothing. Un-programmed flash reads 0xFF,
    // which reinterprets as NaN, and a NaN written into an output channel
    // propagates through every calculation downstream of it â€” so a table whose
    // grid has not fully landed must be skipped, not evaluated.
    //
    // The guard is per table: table t evaluates only while
    // count[DEF] > t AND count[ROW] >= (t+1)*8. A global min() of the two
    // counts would pass the first half of this block and fail the second.
    // Self-contained. ----
    {
        EngineCallbacks cb{};
        cb.transmit_can = captureTransmit;
        engine_init(&cb);
        engine_clear_config();
        CHECK(t8::installAxes());

        const quint8 bothInterp =
            quint8(ct::TABLEFLAG_ACTIVE | ct::TABLEFLAG_X_INTERP | ct::TABLEFLAG_Y_INTERP);
        const ct::Table8x8Def def0 = t8::makeDef(t8::kOut0, bothInterp, 8, 8);
        const ct::Table8x8Def def1 = t8::makeDef(t8::kOut1, bothInterp, 8, 8);
        ct::Table8x8GridRow rows0[ct::TABLE_8X8_SITES];
        ct::Table8x8GridRow rows1[ct::TABLE_8X8_SITES];
        t8::makeRows(rows0, 0.0f);
        t8::makeRows(rows1, 1000.0f);

        // An upload interrupted after the Def and half the rows: the worst
        // possible order, and the one the Def-last convention exists to make
        // impossible â€” but the engine may not rely on the host's good manners.
        CHECK(engine_table_write(ENGINE_TABLE_TABLES_8X8_ROW, 0, 4,
                                 reinterpret_cast<const uint8_t *>(rows0)));
        CHECK(engine_table_write(ENGINE_TABLE_TABLES_8X8_DEF, 0, 1,
                                 reinterpret_cast<const uint8_t *>(&def0)));

        // y = 80 clamps to row 7, which is un-programmed â€” so an evaluation
        // here reads 0xFF bytes as NaN. Feeding a value that stays inside the
        // written rows would let a broken guard pass.
        t8::feed(15, 80);
        const float torn = engine_signal_value(t8::kOut0);
        CHECK(torn == torn);          // not NaN: the table was skipped, not evaluated
        CHECK(qAbs(torn) < 0.01f);    // the slot still holds its initial zero

        // Completing the grid makes the very same table evaluate normally:
        // row 7, cells 70 and 71 at half weight.
        CHECK(engine_table_write(ENGINE_TABLE_TABLES_8X8_ROW, 4, 4,
                                 reinterpret_cast<const uint8_t *>(rows0 + 4)));
        t8::feed(15, 80);
        CHECK(qAbs(engine_signal_value(t8::kOut0) - 70.5f) < 0.01f);

        // Now a SECOND Def with none of its rows. count[DEF] is 2 and
        // count[ROW] is 8: table 0 is still complete and must keep evaluating,
        // while table 1 â€” which needs rows through index 15 â€” must not.
        CHECK(engine_table_write(ENGINE_TABLE_TABLES_8X8_DEF, 1, 1,
                                 reinterpret_cast<const uint8_t *>(&def1)));
        t8::feed(15, 80);
        const float unfinished = engine_signal_value(t8::kOut1);
        CHECK(unfinished == unfinished);
        CHECK(qAbs(unfinished) < 0.01f);
        CHECK(qAbs(engine_signal_value(t8::kOut0) - 70.5f) < 0.01f); // unaffected

        // ...and once table 1's rows land it evaluates too, from ITS eight rows.
        CHECK(engine_table_write(ENGINE_TABLE_TABLES_8X8_ROW, ct::TABLE_8X8_SITES,
                                 ct::TABLE_8X8_SITES,
                                 reinterpret_cast<const uint8_t *>(rows1)));
        t8::feed(15, 80);
        CHECK(qAbs(engine_signal_value(t8::kOut1) - 1070.5f) < 0.01f);
        CHECK(qAbs(engine_signal_value(t8::kOut0) - 70.5f) < 0.01f);
    }

    // ---- The 8x8 across the REAL wire: the four new commands, the chunking,
    // and a byte-exact readback.
    //
    // Everything above writes records straight into the store, which proves the
    // evaluation but would not notice a serial_proto that never learned
    // 0x34-0x37 â€” the table would evaluate perfectly and be unreachable from a
    // host, which is the failure the relay and integrator tables each shipped
    // with once. Two tables, so the 16 row records cross the write chunk
    // (15/frame) and the multi-frame path is exercised rather than assumed. ----
    {
        EngineCallbacks cb{};
        cb.transmit_can = captureTransmit;
        engine_init(&cb);
        CHECK(expectAck(ct::CMD_CLEAR_CONFIG, {}));
        CHECK(t8::installAxes());

        const quint8 bothInterp =
            quint8(ct::TABLEFLAG_ACTIVE | ct::TABLEFLAG_X_INTERP | ct::TABLEFLAG_Y_INTERP);
        QVector<ct::Table8x8Def> defs;
        defs.append(t8::makeDef(t8::kOut0, bothInterp, 8, 8));
        defs.append(t8::makeDef(t8::kOut1, bothInterp, 8, 8));
        QVector<ct::Table8x8GridRow> rows(2 * ct::TABLE_8X8_SITES);
        t8::makeRows(rows.data(), 0.0f);
        t8::makeRows(rows.data() + ct::TABLE_8X8_SITES, 1000.0f);
        CHECK(rows.size() > ct::WRITE_CHUNK_TABLES_8X8_ROW); // more than one frame

        // Rows first, definitions second â€” the order config_transfer uses, and
        // the order that makes the torn-upload guard unnecessary in practice
        // rather than merely sufficient.
        CHECK(sendTable(ct::CMD_WRITE_TABLE8X8_ROW, rows, ct::WRITE_CHUNK_TABLES_8X8_ROW));
        CHECK(sendTable(ct::CMD_WRITE_TABLE8X8_DEF, defs, ct::WRITE_CHUNK_TABLES_8X8_DEF));

        const QByteArray rDef = readRange(ct::CMD_READ_TABLE8X8_DEF, 0, quint16(defs.size()));
        CHECK(rDef.size() == 4 + defs.size() * int(sizeof(ct::Table8x8Def)));
        CHECK(std::memcmp(rDef.constData() + 4, defs.constData(), size_t(rDef.size() - 4)) == 0);
        const QByteArray rRow = readRange(ct::CMD_READ_TABLE8X8_ROW, 0, quint16(rows.size()));
        CHECK(rRow.size() == 4 + rows.size() * int(sizeof(ct::Table8x8GridRow)));
        CHECK(std::memcmp(rRow.constData() + 4, rows.constData(), size_t(rRow.size() - 4)) == 0);

        // ...and the records that came back over the wire evaluate.
        t8::feed(15, 15);
        CHECK(qAbs(engine_signal_value(t8::kOut0) - 5.5f) < 0.01f);
        CHECK(qAbs(engine_signal_value(t8::kOut1) - 1005.5f) < 0.01f);

        // The ROW table's capacity is 64, not 8 â€” a write starting past it is
        // out of bounds, and the device is where that has to be true. (Index 8
        // is table 1's first row and perfectly legal, which is exactly the
        // confusion this checks against.)
        CHECK(expectNack(ct::CMD_WRITE_TABLE8X8_ROW,
                         writeChunk(quint16(ct::MAX_TABLE_8X8_ROWS), rows, 0, 1),
                         ct::ERR_OUT_OF_BOUNDS));
        CHECK(expectNack(ct::CMD_WRITE_TABLE8X8_DEF,
                         writeChunk(quint16(ct::MAX_TABLES_8X8), defs, 0, 1),
                         ct::ERR_OUT_OF_BOUNDS));
        // A payload that does not match `4 + count*item_size` is a length
        // error, and that check is the whole cross-version guard: a host still
        // sending 105-byte 4x4 records can never have them read as some number
        // of 73-byte definitions. 105 payload bytes against a count of 2 is
        // exactly that shape of mismatch.
        CHECK(expectNack(ct::CMD_WRITE_TABLE8X8_DEF,
                         writeChunk(quint16(0), defs, 0, 2).left(4 + 105),
                         ct::ERR_INVALID_LEN));

        CHECK(expectAck(ct::CMD_CLEAR_CONFIG, {})); // leave the region erased
    }

    // ---- v14 multi-term conditions on the real engine. Two things are being
    // pinned here: that AND/OR combine correctly, and that the fold is STRICTLY
    // LEFT TO RIGHT rather than C's "&& binds tighter than ||" â€” the two
    // disagree, so a precedence regression cannot hide. Self-contained (own
    // engine_init + erase). ----
    {
        EngineCallbacks cb{};
        cb.transmit_can = captureTransmit;
        engine_init(&cb);
        engine_clear_config();

        ct::Configuration mcfg;
        mcfg.bus[0].enabled = true;
        const auto addCh = [&mcfg](const char *n) {
            ct::Channel c;
            c.name = QString::fromUtf8(n);
            c.baseResolution = 1.0;
            c.minValue = -1e9;
            c.maxValue = 1e9;
            c.userDefined = true;
            mcfg.catalog().addOrUpdateUserChannel(c);
        };
        addCh("RPM");
        addCh("TPS");

        ct::CommsSection rx;
        rx.name = QStringLiteral("Inputs");
        rx.device = ct::SectionDevice::ReceiveMessage;
        rx.alignment = ct::SectionAlignment::WordSwap;
        rx.baseAddress = 0x400;
        rx.messageLengthBytes = 8;
        ct::CommsChannelRow rRpm;
        rRpm.channelName = QStringLiteral("RPM");
        rRpm.startBit = 0;
        rRpm.bitLength = 16;
        rRpm.dbcFactor = 1.0;
        ct::CommsChannelRow rTps;
        rTps.channelName = QStringLiteral("TPS");
        rTps.startBit = 16;
        rTps.bitLength = 16;
        rTps.dbcFactor = 1.0;
        rx.rows.append(rRpm);
        rx.rows.append(rTps);
        mcfg.bus[0].sections.append(rx);

        // The user's example: RPM < 1500 AND RPM > 100 OR TPS < 1.
        const auto term = [](const char *ch, int op, double k) {
            ct::ConditionTermRow t;
            t.aChannel = QString::fromUtf8(ch);
            t.op = op;
            t.bIsChannel = false;
            t.bConst = k;
            return t;
        };
        ct::ConditionRow band;
        band.setTerms = {term("RPM", ct::COND_OP_LT, 1500), term("RPM", ct::COND_OP_GT, 100),
                      term("TPS", ct::COND_OP_LT, 1)};
        band.setJoiners = {int(ct::COND_JOIN_AND), int(ct::COND_JOIN_OR)};
        giveInverseReset(band);
        band.outputChannel = QStringLiteral("InBand");
        mcfg.conditionRows.append(band);

        // OR then AND â€” where left-to-right and C precedence DIVERGE.
        // Left-to-right: (RPM > 100 OR TPS < 1) AND RPM < 1500.
        // C precedence:   RPM > 100 OR (TPS < 1 AND RPM < 1500).
        // At RPM 5000, TPS 0: left-to-right gives (T or T) and F = FALSE;
        // C precedence would give T or (T and F) = TRUE.
        ct::ConditionRow mixed;
        mixed.setTerms = {term("RPM", ct::COND_OP_GT, 100), term("TPS", ct::COND_OP_LT, 1),
                       term("RPM", ct::COND_OP_LT, 1500)};
        mixed.setJoiners = {int(ct::COND_JOIN_OR), int(ct::COND_JOIN_AND)};
        giveInverseReset(mixed);
        mixed.outputChannel = QStringLiteral("Mixed");
        mcfg.conditionRows.append(mixed);

        const ct::MappingResult mm = ct::mapToDevice(mcfg);
        CHECK(mm.ok());
        CHECK(mm.tables.conditions.size() == 2);
        if (mm.tables.conditions.size() == 2) {
            CHECK(mm.tables.conditions[0].set_count == 3);
            // joiners bit0 = AND (0), bit1 = OR (1)  ->  0b10 = 2
            CHECK(mm.tables.conditions[0].set_joiners == 2);
            // joiners bit0 = OR (1), bit1 = AND (0)  ->  0b01 = 1
            CHECK(mm.tables.conditions[1].set_joiners == 1);
        }

        CHECK(expectAck(ct::CMD_CLEAR_CONFIG, {}));
        CHECK(sendTable(ct::CMD_WRITE_MSG_CFG, mm.tables.messages, ct::WRITE_CHUNK_MESSAGES));
        CHECK(sendTable(ct::CMD_WRITE_SIG_CFG, mm.tables.signalConfigs, ct::WRITE_CHUNK_SIGNALS));
        CHECK(sendTable(ct::CMD_WRITE_COND_CFG, mm.tables.conditions,
                        ct::WRITE_CHUNK_CONDITIONS));
        // Byte-exact readback of the widened record.
        const QByteArray rc =
            readRange(ct::CMD_READ_COND_CFG, 0, quint16(mm.tables.conditions.size()));
        CHECK(rc.size() == 4 + mm.tables.conditions.size() * int(sizeof(ct::ConditionConfig)));
        CHECK(std::memcmp(rc.constData() + 4, mm.tables.conditions.constData(),
                          size_t(rc.size() - 4)) == 0);

        const int inBand = mm.channelToSignal.value(QStringLiteral("inband"), -1);
        const int mixedIdx = mm.channelToSignal.value(QStringLiteral("mixed"), -1);
        CHECK(inBand >= 0 && mixedIdx >= 0);
        const auto feed = [&](int rpm, int tps) {
            const uint8_t f[8] = {uint8_t(rpm & 0xFF), uint8_t((rpm >> 8) & 0xFF),
                                  uint8_t(tps & 0xFF), uint8_t((tps >> 8) & 0xFF), 0, 0, 0, 0};
            engine_process_can(1, 0x400, 0, 0, f, 8);
        };

        // (RPM<1500 AND RPM>100) OR TPS<1
        feed(800, 50);   // in the band -> true
        CHECK(qAbs(engine_signal_value(quint16(inBand)) - 1.0f) < 0.01f);
        feed(5000, 50);  // out of band, TPS high -> false
        CHECK(qAbs(engine_signal_value(quint16(inBand))) < 0.01f);
        feed(5000, 0);   // out of band but TPS < 1 -> the OR rescues it
        CHECK(qAbs(engine_signal_value(quint16(inBand)) - 1.0f) < 0.01f);
        feed(50, 50);    // below the band (RPM>100 fails) -> false
        CHECK(qAbs(engine_signal_value(quint16(inBand))) < 0.01f);

        // ((RPM>100 OR TPS<1) AND RPM<1500) â€” the precedence discriminator.
        feed(5000, 0);
        CHECK(qAbs(engine_signal_value(quint16(mixedIdx))) < 0.01f); // C precedence would be TRUE
        feed(800, 50);   // (T or F) and T -> true
        CHECK(qAbs(engine_signal_value(quint16(mixedIdx)) - 1.0f) < 0.01f);

        // A 1-comparison condition still behaves exactly as it did pre-v14.
        ct::ConditionRow single;
        single.setTerms = {term("RPM", ct::COND_OP_GT, 1000)};
        single.setJoiners.clear();
        giveInverseReset(single);
        single.outputChannel = QStringLiteral("Simple");
        mcfg.conditionRows = {single};
        const ct::MappingResult sm = ct::mapToDevice(mcfg);
        CHECK(sm.ok());
        CHECK(sm.tables.conditions.size() == 1);
        if (sm.tables.conditions.size() == 1) {
            CHECK(sm.tables.conditions[0].set_count == 1);
            CHECK(sm.tables.conditions[0].set_joiners == 0);
        }
    }

    // ---- Termination persists in the flash header (v9): SAVE stores the bus
    // setup, a reload recovers each bus's termination flag. Self-contained
    // (own engine_init + erase), so it runs after the shared config tests. ----
    {
        EngineCallbacks cb{};
        cb.transmit_can = captureTransmit;
        engine_init(&cb);
        engine_clear_config(); // erase -> virgin flash

        ControlCanPayload setup[3]{};
        setup[0].bus_idx = 1; setup[0].mode = 1; setup[0].baud_rate = 1000000; setup[0].termination = 0;
        setup[1].bus_idx = 2; setup[1].mode = 1; setup[1].baud_rate = 500000;  setup[1].termination = 1;
        setup[2].bus_idx = 3; setup[2].mode = 0; setup[2].baud_rate = 500000;  setup[2].termination = 1;
        CHECK(engine_save_config(setup)); // commit header (0 records) + bus setup

        ControlCanPayload loaded[3]{};
        CHECK(engine_load_config(loaded));
        CHECK(loaded[0].termination == 0);
        CHECK(loaded[1].termination == 1);
        CHECK(loaded[2].termination == 1);
        CHECK(loaded[1].baud_rate == 500000); // mode/baud still round-trip too
    }

    // ---- Compound TRANSMIT (protocol v10): one CAN ID, several variant frames
    // selected by a multiplexor. Batch sends every identifier each period;
    // Sequential sends one per period, round-robin. Always-present signals ride
    // in every variant. Self-contained (own engine_init + upload). ----
    {
        EngineCallbacks cb{};
        cb.transmit_can = captureTransmit;
        engine_init(&cb);

        const auto buildCompoundTx = [](ct::CompoundTxMode mode) {
            ct::Configuration cfg;
            cfg.bus[0].enabled = true;
            const auto addCh = [&cfg](const char *n) {
                ct::Channel c;
                c.name = QString::fromUtf8(n);
                c.minValue = 0;
                c.maxValue = 65535;
                c.userDefined = true;
                cfg.catalog().addOrUpdateUserChannel(c);
            };
            addCh("TxA");
            addCh("TxB");
            addCh("TxCommon");
            // Constants drive the value slots the TX composer reads.
            const auto addConst = [&cfg](const char *n, double v) {
                ct::ConstantRow k;
                k.name = QString::fromUtf8(n);
                k.dataType = QStringLiteral("u16");
                k.value = v;
                cfg.constantRows.append(k);
            };
            addConst("TxA", 100);
            addConst("TxB", 200);
            addConst("TxCommon", 50);

            ct::CommsSection cs;
            cs.name = QStringLiteral("Transmit 0x300");
            cs.device = ct::SectionDevice::TransmitMessage;
            cs.alignment = ct::SectionAlignment::WordSwap; // Intel
            cs.baseAddress = 0x300;
            cs.messageLengthBytes = 8;
            cs.transmitRateHz = 50; // period 20 ms
            cs.compound = true;
            cs.compoundTxMode = mode;
            // TxCommon must appear in every variant, so it is defined in each
            // identifier (compound has no always-present set). TxCommon at
            // bytes 3..4.
            ct::CommsChannelRow common;
            common.channelName = QStringLiteral("TxCommon");
            common.startBit = 24;
            common.bitLength = 16;
            // Identifier 1 (selector byte 0 == 1): TxCommon + TxA at bytes 1..2.
            ct::CompoundIdentifier id1;
            id1.byteOffset = 0; id1.id = 1; id1.idMask = 0xFF; id1.configured = true;
            ct::CommsChannelRow a;
            a.channelName = QStringLiteral("TxA"); a.startBit = 8; a.bitLength = 16;
            id1.rows.append(common);
            id1.rows.append(a);
            cs.identifiers.append(id1);
            // Identifier 2 (selector byte 0 == 2): TxCommon + TxB at bytes 1..2.
            ct::CompoundIdentifier id2;
            id2.byteOffset = 0; id2.id = 2; id2.idMask = 0xFF; id2.configured = true;
            ct::CommsChannelRow b;
            b.channelName = QStringLiteral("TxB"); b.startBit = 8; b.bitLength = 16;
            id2.rows.append(common);
            id2.rows.append(b);
            cs.identifiers.append(id2);
            cfg.bus[0].sections.append(cs);
            return ct::mapToDevice(cfg);
        };

        const auto upload = [&](const ct::MappingResult &mr) {
            CHECK(mr.ok());
            CHECK(expectAck(ct::CMD_CLEAR_CONFIG, {}));
            CHECK(sendTable(ct::CMD_WRITE_MSG_CFG, mr.tables.messages, ct::WRITE_CHUNK_MESSAGES));
            CHECK(sendTable(ct::CMD_WRITE_SIG_CFG, mr.tables.signalConfigs, ct::WRITE_CHUNK_SIGNALS));
            CHECK(sendTable(ct::CMD_WRITE_CONST_CFG, mr.tables.constants, ct::WRITE_CHUNK_CONSTANTS));
        };

        // ---- Batch: one period emits BOTH variant frames ----
        {
            const ct::MappingResult mr = buildCompoundTx(ct::CompoundTxMode::Batch);
            // The mapper leaves MSGFLAG_TX_SEQUENTIAL clear for Batch.
            CHECK((mr.tables.messages[0].flags & ct::MSGFLAG_TX_SEQUENTIAL) == 0);
            upload(mr);
            g_txFrames.clear();
            engine_tick(20);
            CHECK(g_txFrames.size() == 2);
            bool sawA = false, sawB = false;
            for (const CapturedTx &f : g_txFrames) {
                CHECK(f.id == 0x300);
                CHECK(f.data[3] == 0x32 && f.data[4] == 0x00); // TxCommon=50 in every variant
                if (f.data[0] == 1) { sawA = true; CHECK(f.data[1] == 0x64 && f.data[2] == 0x00); } // TxA=100
                if (f.data[0] == 2) { sawB = true; CHECK(f.data[1] == 0xC8 && f.data[2] == 0x00); } // TxB=200
            }
            CHECK(sawA && sawB);
        }

        // ---- Sequential: one variant per period, round-robin ----
        {
            engine_init(&cb); // fresh: resets the round-robin cursor
            const ct::MappingResult mr = buildCompoundTx(ct::CompoundTxMode::Sequential);
            CHECK((mr.tables.messages[0].flags & ct::MSGFLAG_TX_SEQUENTIAL) != 0);
            upload(mr);
            g_txFrames.clear();
            engine_tick(20);
            CHECK(g_txFrames.size() == 1);
            engine_tick(20);
            CHECK(g_txFrames.size() == 2);
            engine_tick(20);
            CHECK(g_txFrames.size() == 3);
            if (g_txFrames.size() == 3) {
                // Two distinct variants, then wrap back to the first.
                CHECK(g_txFrames[0].data[0] != g_txFrames[1].data[0]);
                CHECK(g_txFrames[0].data[0] == g_txFrames[2].data[0]);
                // Every variant still carries the always-present TxCommon.
                for (int i = 0; i < 3; ++i)
                    CHECK(g_txFrames[i].data[3] == 0x32);
            }
        }

        // ---- Masked-id dedup: two identifiers whose ids differ only OUTSIDE
        // the mask are the same selector, so Batch emits a single variant ----
        {
            engine_init(&cb);
            ct::Configuration cfg;
            cfg.bus[0].enabled = true;
            ct::Channel c;
            c.name = QStringLiteral("MV");
            c.minValue = 0;
            c.maxValue = 255;
            c.userDefined = true;
            cfg.catalog().addOrUpdateUserChannel(c);
            ct::ConstantRow k;
            k.name = QStringLiteral("MV");
            k.dataType = QStringLiteral("u8");
            k.value = 7;
            cfg.constantRows.append(k);
            ct::CommsSection cs;
            cs.name = QStringLiteral("Tx Mask");
            cs.device = ct::SectionDevice::TransmitMessage;
            cs.alignment = ct::SectionAlignment::WordSwap;
            cs.baseAddress = 0x301;
            cs.messageLengthBytes = 8;
            cs.transmitRateHz = 50;
            cs.compound = true;
            cs.compoundTxMode = ct::CompoundTxMode::Batch;
            ct::CompoundIdentifier a;
            a.byteOffset = 0; a.id = 0x01; a.idMask = 0x0F; a.configured = true;
            ct::CommsChannelRow ra;
            ra.channelName = QStringLiteral("MV"); ra.startBit = 8; ra.bitLength = 8;
            a.rows.append(ra);
            cs.identifiers.append(a);
            ct::CompoundIdentifier b;
            b.byteOffset = 0; b.id = 0x11; b.idMask = 0x0F; b.configured = true; // 0x11 & 0x0F == 0x01
            ct::CommsChannelRow rb;
            rb.channelName = QStringLiteral("MV"); rb.startBit = 16; rb.bitLength = 8;
            b.rows.append(rb);
            cs.identifiers.append(b);
            cfg.bus[0].sections.append(cs);

            upload(ct::mapToDevice(cfg));
            g_txFrames.clear();
            engine_tick(20);
            CHECK(g_txFrames.size() == 1); // one masked selector -> one variant, not two
            if (!g_txFrames.isEmpty())
                CHECK((g_txFrames[0].data[0] & 0x0F) == 0x01);
        }
    }

    // ---- v16 integrators on the real engine. The rate is the whole point of
    // the feature, so what is pinned here is that a step lands exactly rate_hz
    // times a second and keeps doing so over a long run â€” including for a rate
    // that does not divide 1000 evenly, where a truncated integer period would
    // visibly over-fire. Self-contained (own engine_init + erase). ----
    {
        EngineCallbacks cb{};
        cb.transmit_can = captureTransmit;
        engine_init(&cb);
        engine_clear_config();

        // Slot 0 = the value being accumulated, slot 1 = an enable gate, both
        // driven by constants. Slots 2..5 are the integrator outputs.
        constexpr quint16 kIn = 0, kGate = 1, kOut10 = 2, kOut3 = 3, kOut7 = 4, kOutGated = 5;

        ct::IntegratorConfig base{};
        base.input_signal_idx = kIn;
        base.reset_signal_idx = 0xFFFF;
        base.enable_signal_idx = 0xFFFF; // always on
        base.min_value = 0;
        base.max_value = 0; // max <= min: clamping off
        base.reset_value = 0;
        base.flags = ct::INTEGFLAG_ACTIVE;

        ct::IntegratorConfig at10 = base; // 100 ms, a whole number of ticks
        at10.dest_signal_idx = kOut10;
        at10.rate_hz = 10;

        ct::IntegratorConfig at3 = base;  // 333.33 ms
        at3.dest_signal_idx = kOut3;
        at3.rate_hz = 3;

        ct::IntegratorConfig at7 = base;  // 142.86 ms â€” the worst truncation case
        at7.dest_signal_idx = kOut7;
        at7.rate_hz = 7;

        ct::IntegratorConfig gated = base;
        gated.dest_signal_idx = kOutGated;
        gated.rate_hz = 10;
        gated.enable_signal_idx = kGate;

        const ct::IntegratorConfig all[4] = {at10, at3, at7, gated};
        CHECK(engine_table_write(ENGINE_TABLE_INTEGRATORS, 0, 4,
                                 reinterpret_cast<const uint8_t *>(all)));
        CHECK(engine_table_capacity(ENGINE_TABLE_INTEGRATORS) == ct::MAX_INTEGRATORS);

        // A constant holds the input at 2.0 so every expected total is exact.
        ct::ConstantConfig kc{};
        kc.dest_signal_idx = kIn;
        kc.value = 2.0f;
        kc.is_active = 1;
        CHECK(engine_table_write(ENGINE_TABLE_CONSTANTS, 0, 1,
                                 reinterpret_cast<const uint8_t *>(&kc)));

        // One second, gate shut throughout.
        for (int t = 0; t < 100; ++t)
            engine_tick(10);
        // 10 Hz x 1 s x 2.0 = 20. The rate scales the total â€” this is the
        // behaviour that distinguishes a rate accumulator from a time integral,
        // where the rate would not appear in the answer at all.
        CHECK(qAbs(engine_signal_value(kOut10) - 20.0f) < 0.01f);
        CHECK(qAbs(engine_signal_value(kOut3) - 6.0f) < 0.01f);
        CHECK(qAbs(engine_signal_value(kOut7) - 14.0f) < 0.01f);
        CHECK(qAbs(engine_signal_value(kOutGated)) < 0.01f); // gate shut: no steps

        // 99 more seconds (100 s total). This is where a truncated integer
        // period separates from the Hz*ms carry: 7 Hz stored as a 142 ms period
        // fires 100000/142 = 704 times instead of 700, so the 7 Hz total would
        // read 1408. One second of ticks could never show that.
        for (int t = 0; t < 9900; ++t)
            engine_tick(10);
        CHECK(qAbs(engine_signal_value(kOut10) - 2000.0f) < 0.01f);  // 10 Hz x 100 s x 2
        CHECK(qAbs(engine_signal_value(kOut3) - 600.0f) < 0.01f);    // 3 Hz  x 100 s x 2
        CHECK(qAbs(engine_signal_value(kOut7) - 1400.0f) < 0.01f);   // 7 Hz  x 100 s x 2
        CHECK(qAbs(engine_signal_value(kOutGated)) < 0.01f);         // still shut

        // Open the gate for one second: the gated integrator accumulates at its
        // full rate and gains exactly one second's worth, nothing more.
        ct::ConstantConfig gc{};
        gc.dest_signal_idx = kGate;
        gc.value = 1.0f;
        gc.is_active = 1;
        CHECK(engine_table_write(ENGINE_TABLE_CONSTANTS, 1, 1,
                                 reinterpret_cast<const uint8_t *>(&gc)));
        for (int t = 0; t < 100; ++t)
            engine_tick(10);
        CHECK(qAbs(engine_signal_value(kOutGated) - 20.0f) < 0.01f);
    }

    // ---- v16: clamping and edge-triggered reset. Separate engine so the
    // totals above are not disturbed. ----
    {
        EngineCallbacks cb{};
        cb.transmit_can = captureTransmit;
        engine_init(&cb);
        engine_clear_config();

        constexpr quint16 kIn = 0, kReset = 1, kOut = 2;
        ct::IntegratorConfig ic{};
        ic.input_signal_idx = kIn;
        ic.reset_signal_idx = kReset;
        ic.enable_signal_idx = 0xFFFF;
        ic.dest_signal_idx = kOut;
        ic.min_value = 0;
        ic.max_value = 5; // a real span, so clamping is live
        ic.reset_value = 1;
        ic.rate_hz = 100; // one step per tick
        ic.flags = ct::INTEGFLAG_ACTIVE;
        CHECK(engine_table_write(ENGINE_TABLE_INTEGRATORS, 0, 1,
                                 reinterpret_cast<const uint8_t *>(&ic)));
        ct::ConstantConfig kc{};
        kc.dest_signal_idx = kIn;
        kc.value = 2.0f;
        kc.is_active = 1;
        CHECK(engine_table_write(ENGINE_TABLE_CONSTANTS, 0, 1,
                                 reinterpret_cast<const uint8_t *>(&kc)));

        for (int t = 0; t < 10; ++t)
            engine_tick(10); // would reach 20 unclamped
        CHECK(qAbs(engine_signal_value(kOut) - 5.0f) < 0.01f);

        // Raise the reset line: the rising edge reloads reset_value. It is a
        // constant, so it stays high â€” proving the reset is edge-triggered and
        // not level-held, since accumulation resumes on the following ticks.
        ct::ConstantConfig rc{};
        rc.dest_signal_idx = kReset;
        rc.value = 1.0f;
        rc.is_active = 1;
        CHECK(engine_table_write(ENGINE_TABLE_CONSTANTS, 1, 1,
                                 reinterpret_cast<const uint8_t *>(&rc)));
        engine_tick(10);
        CHECK(qAbs(engine_signal_value(kOut) - 1.0f) < 0.01f); // reloaded
        engine_tick(10);
        CHECK(qAbs(engine_signal_value(kOut) - 3.0f) < 0.01f); // held high, still counts
    }

    // ---- v17 decrementors: COUNT_DOWN plus a start value. What is pinned is
    // that the start value reaches the slot at CONFIG LOAD (not merely on a
    // reset edge â€” a decrementor that boots at zero is useless), that it counts
    // down at its rate, and that it floors at min_value. Needs a real
    // save/load cycle, so it builds a committed image. ----
    {
        EngineCallbacks cb{};
        cb.transmit_can = captureTransmit;
        engine_init(&cb);
        engine_clear_config();

        constexpr quint16 kIn = 0, kOut = 1;
        ct::IntegratorConfig dec{};
        dec.input_signal_idx = kIn;
        dec.reset_signal_idx = 0xFFFF;
        dec.enable_signal_idx = 0xFFFF;
        dec.dest_signal_idx = kOut;
        dec.min_value = 0;
        dec.max_value = 100; // a real span, so the floor is live
        dec.reset_value = 100;
        dec.start_value = 60; // deliberately != reset_value: separate fields
        dec.rate_hz = 10;
        dec.flags = ct::INTEGFLAG_ACTIVE | ct::INTEGFLAG_COUNT_DOWN;
        CHECK(engine_table_write(ENGINE_TABLE_INTEGRATORS, 0, 1,
                                 reinterpret_cast<const uint8_t *>(&dec)));
        // The start value must land as soon as the RECORD does, not only at the
        // next config load. An upload starts with CLEAR_CONFIG (which zeroes
        // every slot), so without this a freshly-sent decrementor would read 0
        // â€” sitting on its floor â€” until the device was rebooted.
        CHECK(qAbs(engine_signal_value(kOut) - 60.0f) < 0.01f);

        ct::ConstantConfig kc{};
        kc.dest_signal_idx = kIn;
        kc.value = 2.0f;
        kc.is_active = 1;
        CHECK(engine_table_write(ENGINE_TABLE_CONSTANTS, 0, 1,
                                 reinterpret_cast<const uint8_t *>(&kc)));
        CHECK(engine_save_config(nullptr));
        CHECK(engine_load_config(nullptr));

        // ...and again after a load, which zeroes the slots on the way through.
        CHECK(qAbs(engine_signal_value(kOut) - 60.0f) < 0.01f);

        // One second: 10 Hz x 2.0 = 20 removed, counting DOWN from 60.
        for (int t = 0; t < 100; ++t)
            engine_tick(10);
        CHECK(qAbs(engine_signal_value(kOut) - 40.0f) < 0.01f);

        // Two more seconds would reach -40; it must floor at min_value instead.
        for (int t = 0; t < 200; ++t)
            engine_tick(10);
        CHECK(qAbs(engine_signal_value(kOut)) < 0.01f);
    }

    // ---- v17 preserved integrators. The keys of two tables now share one ring,
    // so what matters is that an integrator's value survives a power cycle AND
    // lands back in the right slot â€” not in the counter that shares its index.
    // Uses the engine's enumerate/seed directly, which is exactly what the board
    // glue calls at flush and boot. ----
    {
        EngineCallbacks cb{};
        cb.transmit_can = captureTransmit;
        engine_init(&cb);
        engine_clear_config();

        constexpr quint16 kIn = 0, kCounterOut = 1, kIntegOut = 2;

        // Counter 0 and integrator 0 both preserved â€” the same table index in
        // two tables, which is precisely the collision the key base prevents.
        ct::CounterConfig cc{};
        cc.up_signal_idx = 0xFFFF; cc.down_signal_idx = 0xFFFF;
        cc.follow_signal_idx = 0xFFFF; cc.reset_signal_idx = 0xFFFF;
        cc.enable_signal_idx = 0xFFFF;
        cc.dest_signal_idx = kCounterOut;
        cc.min_value = 0; cc.max_value = 1000; cc.step = 1;
        cc.mode = ct::COUNTER_MODE_UPDOWN;
        cc.flags = ct::COUNTERFLAG_ACTIVE | ct::COUNTERFLAG_PRESERVE;
        CHECK(engine_table_write(ENGINE_TABLE_COUNTERS, 0, 1,
                                 reinterpret_cast<const uint8_t *>(&cc)));

        ct::IntegratorConfig ig{};
        ig.input_signal_idx = kIn;
        ig.reset_signal_idx = 0xFFFF;
        ig.enable_signal_idx = 0xFFFF;
        ig.dest_signal_idx = kIntegOut;
        ig.min_value = 0; ig.max_value = 10000;
        ig.start_value = 500;
        ig.rate_hz = 10;
        ig.flags = ct::INTEGFLAG_ACTIVE | ct::INTEGFLAG_PRESERVE;
        CHECK(engine_table_write(ENGINE_TABLE_INTEGRATORS, 0, 1,
                                 reinterpret_cast<const uint8_t *>(&ig)));

        ct::ConstantConfig kc{};
        kc.dest_signal_idx = kIn;
        kc.value = 3.0f;
        kc.is_active = 1;
        CHECK(engine_table_write(ENGINE_TABLE_CONSTANTS, 0, 1,
                                 reinterpret_cast<const uint8_t *>(&kc)));
        CHECK(engine_save_config(nullptr));
        CHECK(engine_load_config(nullptr));
        CHECK(qAbs(engine_signal_value(kIntegOut) - 500.0f) < 0.01f); // start value

        for (int t = 0; t < 100; ++t)
            engine_tick(10); // +10 x 3.0 = 30 -> 530

        PreserveEntry entries[PRESERVE_MAX];
        const int n = engine_preserve_enumerate(entries, PRESERVE_MAX);
        CHECK(n == 2); // one counter + one integrator
        bool sawIntegratorKey = false;
        float storedIntegrator = 0.0f;
        for (int i = 0; i < n; ++i) {
            if (entries[i].key == fw::kPreserveKeyIntegratorBase + 0) {
                sawIntegratorKey = true;
                storedIntegrator = entries[i].val.f;
            }
            // Nothing may land outside the shared key space, or the boot loop
            // (which walks 0..PRESERVE_KEY_COUNT) would silently skip it.
            CHECK(entries[i].key < fw::kPreserveKeyCount);
        }
        CHECK(sawIntegratorKey);
        CHECK(qAbs(storedIntegrator - 530.0f) < 0.01f);

        // Simulate the power cycle: reload the config (which re-seeds the START
        // value, 500) and then replay the restore. The retained 530 must win â€”
        // if the ordering were reversed the total would silently reset on every
        // boot, which is the whole failure Preserve exists to prevent.
        CHECK(engine_load_config(nullptr));
        CHECK(qAbs(engine_signal_value(kIntegOut) - 500.0f) < 0.01f);
        for (int i = 0; i < n; ++i)
            engine_preserve_seed(entries[i].key, entries[i].type, entries[i].val);
        CHECK(qAbs(engine_signal_value(kIntegOut) - 530.0f) < 0.01f);

        // A key in the integrator range must never touch the counter's slot.
        const float counterBefore = engine_signal_value(kCounterOut);
        PreserveVal bogus;
        bogus.f = 999.0f;
        engine_preserve_seed(quint16(fw::kPreserveKeyIntegratorBase), ct::SIGNAL_TYPE_FLOAT, bogus);
        CHECK(qAbs(engine_signal_value(kCounterOut) - counterBefore) < 0.01f);
        CHECK(qAbs(engine_signal_value(kIntegOut) - 999.0f) < 0.01f);
    }

    // ---- Advanced Math ops on the real engine. One row per case, constant
    // operands, one tick. What is pinned: every new op's arithmetic, the
    // DIV-style 0-guards (SQRT of a negative, MOD by zero), the "true = value
    // > 0" boolean convention at its boundaries, the IEEE false branch on NaN
    // comparisons (no scrubbing), the clampRoll conventions CLAMP and WRAP
    // inherit, and same-pass chaining through the C operand. Self-contained
    // (own engine_init + erase). ----
    {
        EngineCallbacks cb{};
        cb.transmit_can = captureTransmit;
        engine_init(&cb);
        engine_clear_config();

        const float kNan = std::numeric_limits<float>::quiet_NaN();
        QList<ct::MathConfig> rows;
        // Each row writes signal slot == its own table index, so a case's
        // handle doubles as its output slot (and as the C-signal index the
        // chain case reads).
        const auto add = [&rows](quint8 op, float a, float b) {
            ct::MathConfig mc{};
            mc.op = op;
            mc.is_active = 1;
            mc.input_a_type = 0;
            mc.input_a_const = a;
            mc.input_b_type = 0;
            mc.input_b_const = b;
            mc.dest_signal_idx = quint16(rows.size());
            rows.append(mc);
            return int(rows.size() - 1);
        };
        const auto addC = [&rows, &add](quint8 op, float a, float b, float c) {
            const int i = add(op, a, b);
            ct::mathSetInputCConst(rows[i], c);
            return i;
        };

        const int iAbs = add(ct::MATH_OP_ABS, -3.5f, 99.0f); // B ignored on unary ops
        const int iNeg = add(ct::MATH_OP_NEG, 2.5f, 0.0f);
        const int iSqrt = add(ct::MATH_OP_SQRT, 9.0f, 0.0f);
        const int iSqrtNeg = add(ct::MATH_OP_SQRT, -4.0f, 0.0f);
        const int iFloor = add(ct::MATH_OP_FLOOR, -2.25f, 0.0f);
        const int iCeil = add(ct::MATH_OP_CEIL, 2.25f, 0.0f);
        const int iRound = add(ct::MATH_OP_ROUND, 2.5f, 0.0f);
        const int iRoundNeg = add(ct::MATH_OP_ROUND, -2.5f, 0.0f);
        const int iMod = add(ct::MATH_OP_MOD, 7.0f, 3.0f);
        const int iModNeg = add(ct::MATH_OP_MOD, -7.0f, 3.0f);
        const int iModZero = add(ct::MATH_OP_MOD, 7.0f, 0.0f);
        const int iXor = add(ct::MATH_OP_XOR, 12.0f, 10.0f);
        const int iLandT = add(ct::MATH_OP_LAND, 2.0f, 0.5f);
        const int iLandF = add(ct::MATH_OP_LAND, 2.0f, 0.0f);
        const int iLorT = add(ct::MATH_OP_LOR, 0.0f, 0.5f);
        const int iLorF = add(ct::MATH_OP_LOR, 0.0f, -1.0f);
        const int iLnotT = add(ct::MATH_OP_LNOT, 2.0f, 0.0f);
        const int iLnotZero = add(ct::MATH_OP_LNOT, 0.0f, 0.0f);
        const int iLnotNan = add(ct::MATH_OP_LNOT, kNan, 0.0f);
        const int iGt = add(ct::MATH_OP_GT, 3.0f, 2.0f);
        const int iGtEqual = add(ct::MATH_OP_GT, 2.0f, 2.0f);
        const int iGtNan = add(ct::MATH_OP_GT, kNan, 2.0f);
        const int iGe = add(ct::MATH_OP_GE, 2.0f, 2.0f);
        const int iLt = add(ct::MATH_OP_LT, 2.0f, 3.0f);
        const int iLe = add(ct::MATH_OP_LE, 3.0f, 2.0f);
        const int iEq = add(ct::MATH_OP_EQ, 2.0f, 2.0f);
        const int iEqNan = add(ct::MATH_OP_EQ, kNan, kNan);
        const int iNe = add(ct::MATH_OP_NE, 2.0f, 3.0f);
        const int iMulAdd = addC(ct::MATH_OP_MULADD, 20.0f, 0.5f, 3.0f);
        const int iClampHi = addC(ct::MATH_OP_CLAMP, 15.0f, 0.0f, 10.0f);
        const int iClampLo = addC(ct::MATH_OP_CLAMP, -5.0f, 0.0f, 10.0f);
        const int iClampIn = addC(ct::MATH_OP_CLAMP, 5.0f, 0.0f, 10.0f);
        const int iClampOff = addC(ct::MATH_OP_CLAMP, 15.0f, 10.0f, 0.0f);
        const int iLerp0 = addC(ct::MATH_OP_LERP, 10.0f, 20.0f, 0.0f);
        const int iLerp1 = addC(ct::MATH_OP_LERP, 10.0f, 20.0f, 1.0f);
        const int iLerpQ = addC(ct::MATH_OP_LERP, 10.0f, 20.0f, 0.25f);
        const int iSelTrue = addC(ct::MATH_OP_SELECT, 0.5f, 7.0f, 9.0f);
        const int iSelZero = addC(ct::MATH_OP_SELECT, 0.0f, 7.0f, 9.0f);
        const int iWrap = addC(ct::MATH_OP_WRAP, 370.0f, 0.0f, 360.0f);
        const int iWrapNeg = addC(ct::MATH_OP_WRAP, -30.0f, 0.0f, 360.0f);
        const int iWrapOff = addC(ct::MATH_OP_WRAP, 370.0f, 360.0f, 0.0f);
        // Same-pass chaining THROUGH C: rows evaluate in table order within
        // one pass, so row N+1's C operand sees row N's result from THIS
        // pass. One tick proves it â€” a stale read would still see 0.
        const int iChainSrc = addC(ct::MATH_OP_MULADD, 2.0f, 3.0f, 4.0f);
        const int iChain = add(ct::MATH_OP_SELECT, 0.0f, -1.0f); // A == 0 -> C
        ct::mathSetInputCSignal(rows[iChain], quint16(iChainSrc));

        CHECK(int(rows.size()) <= engine_table_capacity(ENGINE_TABLE_MATH));
        CHECK(engine_table_write(ENGINE_TABLE_MATH, 0, quint16(rows.size()),
                                 reinterpret_cast<const uint8_t *>(rows.constData())));
        engine_tick(10); // ONE pass â€” the chain case depends on that

        const auto at = [](int slot) { return engine_signal_value(quint16(slot)); };
        // Every expected value below is exactly representable, so the checks
        // are exact â€” a tolerance would let a wrong op sneak through (GE vs
        // GT differ by exactly 1.0 at the boundary).
        CHECK(at(iAbs) == 3.5f);
        CHECK(at(iNeg) == -2.5f);
        CHECK(at(iSqrt) == 3.0f);
        CHECK(at(iSqrtNeg) == 0.0f); // domain guard, DIV-by-0 style
        CHECK(at(iFloor) == -3.0f);  // floor goes DOWN for negatives
        CHECK(at(iCeil) == 3.0f);
        CHECK(at(iRound) == 3.0f);   // roundf: halfway cases away from zero
        CHECK(at(iRoundNeg) == -3.0f);
        CHECK(at(iMod) == 1.0f);
        CHECK(at(iModNeg) == -1.0f); // fmodf keeps the dividend's sign
        CHECK(at(iModZero) == 0.0f); // B == 0 guard
        CHECK(at(iXor) == 6.0f);     // 12 ^ 10
        CHECK(at(iLandT) == 1.0f);
        CHECK(at(iLandF) == 0.0f);
        CHECK(at(iLorT) == 1.0f);
        CHECK(at(iLorF) == 0.0f);    // negative is false, not just zero
        CHECK(at(iLnotT) == 0.0f);
        CHECK(at(iLnotZero) == 1.0f); // 0 is false
        CHECK(at(iLnotNan) == 1.0f);  // NaN > 0 is false -> NOT gives 1
        CHECK(at(iGt) == 1.0f);
        CHECK(at(iGtEqual) == 0.0f);  // strictly greater
        CHECK(at(iGtNan) == 0.0f);    // NaN comparison takes the false branch
        CHECK(at(iGe) == 1.0f);
        CHECK(at(iLt) == 1.0f);
        CHECK(at(iLe) == 0.0f);
        CHECK(at(iEq) == 1.0f);       // exact compare
        CHECK(at(iEqNan) == 0.0f);    // NaN equals nothing, itself included
        CHECK(at(iNe) == 1.0f);
        CHECK(at(iMulAdd) == 13.0f);  // 20 * 0.5 + 3 â€” scale WITH offset
        CHECK(at(iClampHi) == 10.0f);
        CHECK(at(iClampLo) == 0.0f);
        CHECK(at(iClampIn) == 5.0f);
        CHECK(at(iClampOff) == 15.0f); // hi <= lo disables clamping (clampRoll)
        CHECK(at(iLerp0) == 10.0f);    // t = 0 -> exactly A
        CHECK(at(iLerp1) == 20.0f);    // t = 1 -> exactly B
        CHECK(at(iLerpQ) == 12.5f);
        CHECK(at(iSelTrue) == 7.0f);
        CHECK(at(iSelZero) == 9.0f);   // A == 0 takes the false branch
        CHECK(at(iWrap) == 10.0f);     // 370 into [0, 360)
        CHECK(at(iWrapNeg) == 330.0f); // fmodf sign fix-up
        CHECK(at(iWrapOff) == 370.0f); // hi <= lo passes through (clampRoll)
        CHECK(at(iChain) == 10.0f);    // 2*3+4 read same-pass through C
    }

    // ---- Regression: a rolling counter seeded with a huge value must NOT hang
    // the tick (clampRoll uses modulo, not a while-loop). Wipes engine state,
    // so it must run last. ----
    {
        EngineCallbacks cb{};
        cb.transmit_can = captureTransmit;
        engine_init(&cb);       // fresh engine
        engine_clear_config();  // erase flash so the writes below hit virgin slots
        ct::CounterConfig rc{};
        rc.up_signal_idx = 0xFFFF; rc.down_signal_idx = 0xFFFF;
        rc.follow_signal_idx = 0xFFFF; rc.reset_signal_idx = 0xFFFF;
        rc.enable_signal_idx = 0xFFFF; // always enabled
        rc.dest_signal_idx = 5;
        rc.min_value = 0; rc.max_value = 10; rc.step = 1;
        rc.mode = ct::COUNTER_MODE_UPDOWN;
        rc.flags = ct::COUNTERFLAG_ACTIVE | ct::COUNTERFLAG_ROLL;
        engine_table_write(ENGINE_TABLE_COUNTERS, 0, 1, reinterpret_cast<const uint8_t *>(&rc));
        // Math writes an enormous value into the counter's dest slot every tick.
        ct::MathConfig mc{};
        mc.op = ct::MATH_OP_MUL; mc.is_active = 1;
        mc.input_a_type = 0; mc.input_a_const = 3.0e7f;
        mc.input_b_type = 0; mc.input_b_const = 1.0f;
        mc.dest_signal_idx = 5;
        engine_table_write(ENGINE_TABLE_MATH, 0, 1, reinterpret_cast<const uint8_t *>(&mc));
        engine_tick(10); // must return (would hang forever with the old while-loop)
        const float v = engine_signal_value(5);
        CHECK(v >= 0.0f && v < 10.0f); // wrapped into [min, max)
    }

    // Last: these re-initialise the protocol layer several times, deliberately
    // install a callback set with no RNG, and rewrite the flash image, so each
    // restores the fixture on the way out rather than leaving a locked or
    // re-badged device behind for the next one.
    testDeviceAccess(&protoCb);
    testFleetIdentityBlock(&protoCb);
    testConfigVersion(&protoCb);
    testBusSetupReadback(&protoCb);
    testCounterRateMode();
    testCounterResetUnlimited();
    testTimerComparisonTrigger();
    testConditionForQualifier();
    testConditionTermCountIsClamped();
    testConditionResetQualifier();
    testCounterMessageInput();
    testDeviceOnTime();
    testMonitorGapMarking();
    testTxPhaseSpreading();
    testTxPhaseSpreadingIsPerBus();
    testTxFairnessUnderSaturation();
    testTxDeadBusDoesNotSilenceOthers();
    testTriggeredTransmit();
    testConditionSetResetLatch();
    testConditionMomentaryHold();
    testConditionMessageEvents();
    testBatchCompoundEmitsAllVariantsBeforeItsEvent();
    testTriggeredTransmitBrokenReferenceIsSilent();
    testTransmitOffsetAddsOnTheWayOut();
    testSelectorOnlyVariantsAreTransmitted();
    testTransmitClampOrRollOver();
    testRollOverIsIgnoredOnReceive();
    testTransmitCrc8Stamping();
    testDeviceCanDiagnostics();
    testMcuHealth();
    testTransmitAt200Hz();
    testReadResponseMatching(&protoCb);
    testAckCrcEcho(&protoCb);
    testProtectedCommsSlots(&protoCb);
    testMessagePasswordSlotSurvivesTheDevice(&protoCb);
    testMessageProtectionIsHostOnly(&protoCb);
    testAccessKeyDurability(&protoCb);
    testDeviceBinding(&protoCb);
    testRetransmitSafety();
    testFirmwareUpdate();

    if (failures == 0)
        std::printf("ALL FIRMWARE-LINK TESTS PASSED\n");
    else
        std::printf("%d FAILURES\n", failures);
    return failures == 0 ? 0 : 1;
}
