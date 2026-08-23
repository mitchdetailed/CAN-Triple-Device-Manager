// Packed mirrors of the CAN Triple firmware's protocol.h / config_types.h.
// Any change here must match ../../src (firmware) byte for byte.
#pragma once

#include <cstdint>
#include <cstring>

namespace ct {

// Command IDs
constexpr uint8_t CMD_GET_STATUS       = 0x01;
constexpr uint8_t CMD_WRITE_MSG_CFG    = 0x02;
constexpr uint8_t CMD_READ_MSG_CFG     = 0x03;
constexpr uint8_t CMD_WRITE_SIG_CFG    = 0x04;
constexpr uint8_t CMD_READ_SIG_CFG     = 0x05;
constexpr uint8_t CMD_WRITE_MATH_CFG   = 0x06;
constexpr uint8_t CMD_READ_MATH_CFG    = 0x07;
constexpr uint8_t CMD_WRITE_COND_CFG   = 0x08;
constexpr uint8_t CMD_READ_COND_CFG    = 0x09;
constexpr uint8_t CMD_SAVE_TO_FLASH    = 0x0A;
// 0x0B retired (was LOAD_FROM_FLASH). Its purpose — reloading a stored
// backup image over the live tables after a bad upload — predates the
// flash-resident single-copy store, under which the live tables ARE the
// stored image and there is no second copy to load. Do not reuse the opcode.
constexpr uint8_t CMD_CLEAR_CONFIG     = 0x0C;
constexpr uint8_t CMD_CONTROL_CAN      = 0x0D; // firmware v2+
constexpr uint8_t CMD_INJECT_CAN_FRAME = 0x0E;
constexpr uint8_t CMD_STREAM_VALUES     = 0x0F; // firmware v2+: payload u8 mask
constexpr uint8_t CMD_WRITE_COUNTER_CFG = 0x10; // firmware v3+: up/down counter table
constexpr uint8_t CMD_READ_COUNTER_CFG  = 0x11;
constexpr uint8_t CMD_WRITE_TIMER_CFG   = 0x12; // firmware v3+: timer table
constexpr uint8_t CMD_READ_TIMER_CFG    = 0x13;
constexpr uint8_t CMD_WRITE_CONST_CFG   = 0x14; // firmware v6+: constants table
constexpr uint8_t CMD_READ_CONST_CFG    = 0x15;
constexpr uint8_t CMD_WRITE_CONFIG_NAME = 0x16; // firmware v7+: 32-byte config name
constexpr uint8_t CMD_READ_CONFIG_NAME  = 0x17;
constexpr uint8_t CMD_RESET_DEVICE      = 0x18; // firmware v7+: reboot after ACK
constexpr uint8_t CMD_WRITE_RELAY_CFG   = 0x19; // firmware v11+: message relay table
constexpr uint8_t CMD_READ_RELAY_CFG    = 0x1A;
// 0x1B/0x1C were the v12 2x8 lookup table, retired in v13 and NOT reused: the
// v13 definition record is also 70 bytes, so a v12 device would accept one and
// misread it. Talking to v12 firmware now fails cleanly on ERR_INVALID_CMD.
// 0x1D/0x1E were the v12 4x4 lookup table. RETIRED with the record itself when
// the 8x8 replaced it, and deliberately NOT reused — the same discipline, for
// the same reason. It happens that a stale 4x4 host would fail the length check
// against an 8x8 device anyway (105-byte records against a 73-byte Def), but
// that is a coincidence of two sizes, not a guarantee: the next record to be
// given these ids might match, and a retired id that answers is a silent
// misread waiting for one. The 8x8 pair took fresh ids at 0x34 (see below).
constexpr uint8_t CMD_WRITE_TABLE2X16_DEF = 0x1F; // firmware v13+: 2x16 axis/dest/sites
constexpr uint8_t CMD_READ_TABLE2X16_DEF  = 0x20;
constexpr uint8_t CMD_WRITE_TABLE2X16_OUT = 0x21; // firmware v13+: 2x16 output values
constexpr uint8_t CMD_READ_TABLE2X16_OUT  = 0x22;
constexpr uint8_t CMD_WRITE_INTEG_CFG     = 0x23; // firmware v16+: integrators
constexpr uint8_t CMD_READ_INTEG_CFG      = 0x24;
// 0x25-0x28 were the v18 single configuration password. RETIRED in v19 and not
// reused — see firmware/include/protocol.h for why a retired ID stays retired.
// firmware v18+: device binding. GET_DEVICE_ID answers the 96-bit unique chip
// ID plus a status byte saying why the stored configuration is or is not
// running — the only way to tell "no configuration" from "a configuration
// belonging to another device", which look identical otherwise.
constexpr uint8_t CMD_GET_DEVICE_ID       = 0x29;
constexpr uint8_t CMD_WRITE_CONFIG_BINDING = 0x2A;
constexpr int CONFIG_UID_LEN            = 12; // 96-bit STM32 unique device ID
// firmware v19+: per-function access keys. READ_ACCESS_KEYS answers WHICH of
// the three passwords are set and never what they are; proving one is
// challenge-response over the 4-byte key.
constexpr uint8_t CMD_READ_ACCESS_KEYS    = 0x2B;
constexpr uint8_t CMD_WRITE_ACCESS_KEYS   = 0x2C;
constexpr uint8_t CMD_ACCESS_CHALLENGE    = 0x2D;
constexpr uint8_t CMD_ACCESS_RESPONSE     = 0x2E;
// RETIRED in 2.3.0 with the per-message key it proved. The id is kept — never
// reused — because what matters about it now is that the device answers
// ERR_INVALID_CMD, and something has to name the opcode that asserts it.
//
// The value of ERR_INVALID_CMD here is load-bearing, not incidental. Shipped
// 2.2.x Managers still send this before every Send; ERR_INVALID_CMD maps to
// wrongPassword = false, which those builds read as "keyless" and proceed past
// silently. Answer ERR_LOCKED instead and every one of them enters an
// unescapable password prompt loop. Nothing in this build sends it.
constexpr uint8_t CMD_MSG_ACCESS_RESPONSE = 0x40;
// Fleet identity — who the device IS. Readable unconditionally, because
// deciding whether an update belongs on a unit must not require reading the
// configuration it is meant to protect. There is no write: the identity is
// compiled into the firmware, so it cannot be changed over the wire at all.
// 0x30 was reserved for that write; it was never implemented and the opcode has
// since been reused by CMD_READ_CAN_SETUP below.
constexpr uint8_t CMD_READ_FLEET_ID       = 0x2F;
constexpr uint8_t CMD_FLEET_ID_PROVE      = 0x31;
// Reads back what the three buses are running (mode / rate / FD rate /
// termination) as ControlCanPayload[3], bus 1..3 in order. CONTROL_CAN is
// write-only, so before this a Get had to assume the bring-up rates and say so.
constexpr uint8_t CMD_READ_CAN_SETUP      = 0x30;
// Device channels: values the DEVICE produces about itself, as opposed to the
// rest of a configuration, which tells it what to do with values from the bus.
// The host sends only a destination -- which signal slot the firmware should
// publish each into -- so this is configuration and rides with SAVE_TO_FLASH,
// not telemetry. Old firmware NACKs both with ERR_INVALID_CMD; the Send and Get
// steps are marked optional so that reads as "this device has no device
// channels" rather than as a failure.
constexpr uint8_t CMD_WRITE_DEVICE_CHANNELS = 0x32;
constexpr uint8_t CMD_READ_DEVICE_CHANNELS  = 0x33;
// The 8x8 lookup table — the 4x4's replacement — carried in the shape v13 gave
// the 2x16: a definition record plus the grid, split because a combined record
// would be 73 + 8*32 = 329 bytes. Note WHICH limit forbids that, because it is
// no longer the obvious one: 329 fits the raised 496-byte payload cap. It does
// not fit MAX_PADDED_RECORD — flash_store programs one padded slot at a time
// through a 112-byte buffer and PAD8(329) is 336 — and that limit did not move.
//
// The grid is one record PER ROW rather than one blob, and that is the whole
// point of the shape. The device pads every record slot to 8 bytes, and
// PAD8(32) == 32, so table t's eight rows sit at row indices t*8..t*8+7 as 256
// BYTE-CONTIGUOUS bytes: the engine takes one pointer at row t*8 and indexes
// grid[y*8 + x], exactly as the retired 4x4's outputs[y*4 + x] did. No
// reassembly buffer, no cross-record arithmetic, no RAM. Chunking it any other
// way loses that.
//
// Fresh ids rather than the 4x4's — see the retirement note at 0x1D above.
constexpr uint8_t CMD_WRITE_TABLE8X8_DEF = 0x34;
constexpr uint8_t CMD_READ_TABLE8X8_DEF  = 0x35;
constexpr uint8_t CMD_WRITE_TABLE8X8_ROW = 0x36;
constexpr uint8_t CMD_READ_TABLE8X8_ROW  = 0x37;
// Transmit CRC8: a rule that stamps a CRC-8 into one byte of a transmit
// message's frame, after every other byte is final. See Crc8Config below.
// Ids sit past the script block — 0x38..0x3F are taken (FW update + scripts)
// and 0x40 is the retired CMD_MSG_ACCESS_RESPONSE above, which must keep
// answering ERR_INVALID_CMD for as long as 2.2.x Managers keep sending it.
constexpr uint8_t CMD_WRITE_CRC8_CFG     = 0x41;
// v16: the four document-wide message passwords. The READ returns the keys, not
// a set mask - see protocol.h for why this one is deliberately unlike
// CMD_READ_ACCESS_KEYS.
constexpr uint8_t CMD_WRITE_MSG_PASSWORDS = 0x43;
constexpr uint8_t CMD_READ_MSG_PASSWORDS  = 0x44;
constexpr uint8_t CMD_READ_CRC8_CFG      = 0x42;

// Firmware update (v2 bootloader). The RUNNING application receives the image
// into its bank-2 staging slot and the bootloader installs it on the next
// boot, so none of these ever touches the executing firmware — an upload that
// fails costs a transfer and nothing else.
//
// BEGIN erases staging and takes several hundred milliseconds, so it must be
// issued with DeviceLink::kFlashTimeoutMs rather than the default. STATUS is a
// READ whose reply echoes the command; it therefore has to appear in
// DeviceLink::isReadResponse(), and test_firmware_link checks that it does —
// the list below it records five previous commands that forgot.
constexpr uint8_t CMD_FW_UPDATE_BEGIN  = 0x38;
constexpr uint8_t CMD_FW_UPDATE_DATA   = 0x39;
constexpr uint8_t CMD_FW_UPDATE_END    = 0x3A;
constexpr uint8_t CMD_FW_UPDATE_STATUS = 0x3B;
constexpr uint8_t CMD_FW_UPDATE_ABORT  = 0x3C;

// Device scripts. The bytecode rides in the configuration as an ordinary table
// of 64-byte chunks, so WRITE/READ_SCRIPT are plain range commands and Send,
// Get, Verify and Clear carry it for free. SCRIPT_STATUS is a READ whose reply
// echoes the command — so it belongs in DeviceLink::isReadResponse(), which is
// where five earlier commands learned that lesson the expensive way.
constexpr uint8_t CMD_WRITE_SCRIPT     = 0x3D;
constexpr uint8_t CMD_READ_SCRIPT      = 0x3E;
constexpr uint8_t CMD_SCRIPT_STATUS    = 0x3F;

// Why the device's stored configuration is or is not running.
constexpr uint8_t CONFIG_STATUS_OK           = 0x00;
constexpr uint8_t CONFIG_STATUS_NONE         = 0x01; // blank, or bad magic/version/CRC
constexpr uint8_t CONFIG_STATUS_WRONG_DEVICE = 0x02; // valid, but bound to another chip
constexpr int CONFIG_NAME_LEN           = 32;

// CanSignalConfig::label is 32 bytes, so a channel name reaches the device as at
// most 31 UTF-8 bytes + NUL. The name dialogs enforce this budget up front — a
// longer name would silently truncate on the device and two names sharing a
// truncated prefix would merge back into one on a Get.
//
// v15 had cut this to 16 bytes to buy signal capacity; the capacity expansion
// bought that back from the flash region instead and put the 32 bytes back,
// which also lands the record on 64 bytes exactly (PAD8(64) = 64, zero padding
// waste). Every site that enforces the budget derives it from here rather than
// writing 31 or taking sizeof by accident — the dialogs, the DBC importer,
// device_mapper's label fill and its collision check — because a site that
// misses a change to this number silently truncates or rejects a legal name.
constexpr int SIGNAL_LABEL_LEN          = 32;
constexpr int MAX_CHANNEL_NAME_BYTES    = SIGNAL_LABEL_LEN - 1;

constexpr uint8_t CMD_ACK            = 0x80;
constexpr uint8_t CMD_NACK           = 0x81;
constexpr uint8_t CMD_MONITOR_STREAM = 0x82;
constexpr uint8_t CMD_VALUE_STREAM   = 0x83;
constexpr uint8_t CMD_LOG            = 0x90; // firmware v2+: framed ASCII log

constexpr uint8_t STREAM_ENABLE_VALUES  = 0x01;
constexpr uint8_t STREAM_ENABLE_MONITOR = 0x02;

// The protocol this build speaks, and the only version there has ever been as
// far as anything outside this repository is concerned.
//
// There used to be a V2..V17 ladder here. It was deleted rather than renumbered:
// nothing has shipped, so it recorded the development history of a product no
// customer has held, and — the deciding point — not one of those constants was
// ever compared against anything. They read like feature gates and gated
// nothing, which is worse than having no ladder at all, because the next person
// to add a real gate would have copied a pattern that does not work.
//
// If a genuine compatibility break ever ships, add V2 and an actual comparison
// at the same time.
constexpr uint8_t PROTOCOL_VERSION_V1 = 1;
constexpr uint8_t PROTOCOL_VERSION_CURRENT = PROTOCOL_VERSION_V1;

constexpr uint8_t ERR_OK            = 0x00;
constexpr uint8_t ERR_INVALID_CMD   = 0x01;
constexpr uint8_t ERR_INVALID_LEN   = 0x02;
constexpr uint8_t ERR_INVALID_CRC   = 0x03;
constexpr uint8_t ERR_OUT_OF_BOUNDS = 0x04;
constexpr uint8_t ERR_FLASH_WRITE   = 0x05;
constexpr uint8_t ERR_BUS_BUSY      = 0x06;
// v18: the device's stored configuration is password protected and this serial
// session has not proved the password. Returned for reads when read protection
// is set and for writes when write protection is set, so the two are
// distinguishable from what was asked.
constexpr uint8_t ERR_LOCKED        = 0x07;
// v2 bootloader: a firmware image was refused. One code rather than one per
// reason — there are ten reasons (FW_RESULT_* below) and they would crowd a
// NACK space every command shares. Read CMD_FW_UPDATE_STATUS for which one,
// and for the bootloader's verdict on the last commit, which no NACK could
// have carried.
constexpr uint8_t ERR_FW_REJECTED   = 0x08;

// Bootloader state and verdicts, mirroring fw_image.h. The GUI only reports
// these; the device is the one that acts on them.
constexpr uint8_t FW_STATE_IDLE    = 0;
constexpr uint8_t FW_STATE_PENDING = 1;

constexpr uint8_t FW_RESULT_NONE           = 0;
constexpr uint8_t FW_RESULT_OK             = 1;
constexpr uint8_t FW_RESULT_BAD_MAGIC      = 2;
constexpr uint8_t FW_RESULT_WRONG_PRODUCT  = 3;
constexpr uint8_t FW_RESULT_BAD_SIZE       = 4;
constexpr uint8_t FW_RESULT_BAD_CRC        = 5;
constexpr uint8_t FW_RESULT_BL_TOO_OLD     = 6;
constexpr uint8_t FW_RESULT_ERASE_FAILED   = 7;
constexpr uint8_t FW_RESULT_PROGRAM_FAILED = 8;
constexpr uint8_t FW_RESULT_VERIFY_FAILED  = 9;
constexpr uint8_t FW_RESULT_GAVE_UP        = 10;

// The image header itself is NOT mirrored here, unlike every other structure
// in this file. src/protocol/firmware_image.* includes the firmware's real
// fw_image.h and links its real fw_image.c instead.
//
// The reason is that a mirror plus a static_assert catches a struct whose SIZE
// drifted, and that is all it catches. What matters for an image header is the
// CRC32 — and a second implementation of a CRC agrees with the first by
// inspection only, which is exactly the sort of agreement that turns out to be
// false at the worst moment. Linking the device's own validator means the
// answer the GUI gives about a file is the answer the bootloader will give
// about it, by construction rather than by review.

// 250 -> 500. The binding limit on this axis is now the SIGNAL record, not the
// flash region: sig_msg_idx is a 9-bit field (SIG_MSG_IDX_MASK, 0..510) whose
// all-ones value 511 is the SIG_MSG_NONE "virtual signal" marker, so 0..510 are
// the usable message indices. 500 fits with 11 to spare; 512 would NOT, and
// raising this past 510 means widening the packed field on both sides of the
// wire, not just changing this number.
constexpr int MAX_MESSAGES          = 500; // receive AND transmit share this table
constexpr int MAX_SIGNALS           = 1000; // 96 KB of config flash bought the room
constexpr int MAX_MATH_COMPUTATIONS = 100;
// 100 -> 250 at store v10, then 250 -> 200 at store v13, which is a REDUCTION
// and the only one in this list. It bought the "for" qualifier: a 60-byte record
// pads to a 64-byte slot, and 200 of those are 12,800 B against the 14,000 B
// that 250 fifty-six-byte records cost — so the feature came in 1,200 B under
// what it replaced.
//
// 200 is not a number the flash forced; 250 would have fitted at 16,000 B if
// something else had paid. It is what the signal table can carry: each active
// condition owns one of the MAX_SIGNALS slots above, and 250 of them could claim
// a quarter of that table for boolean outputs alone.
constexpr int MAX_CONDITIONS        = 200;
constexpr int MAX_COUNTERS          = 50;  // firmware v3+
constexpr int MAX_TIMERS            = 50;  // firmware v3+ (20 -> 50)
constexpr int MAX_CONSTANTS         = 100; // firmware v6+
constexpr int MAX_RELAYS            = 32;  // firmware v11+: message relay rules
constexpr int MAX_TABLES_2X16       = 8;   // firmware v13+: 1-axis 16-site tables
constexpr int TABLE_2X16_SITES      = 16;  // axis sites / outputs per 2x16 table
// The 8x8 lookup table, which replaced the 4x4 (MAX_TABLES_4X4 is gone with it).
// Def and Row are two parallel device tables, but they are NOT indexed in
// lockstep the way the 2x16 pair is: table t owns Def index t and ROW indices
// t*8 .. t*8+7, so the row table's capacity is 8 tables * 8 rows = 64.
constexpr int MAX_TABLES_8X8        = 8;
constexpr int TABLE_8X8_SITES       = 8;   // X sites, Y sites and grid width
constexpr int MAX_TABLE_8X8_ROWS    = MAX_TABLES_8X8 * TABLE_8X8_SITES; // 8*8 = 64
constexpr int MAX_INTEGRATORS       = 8;   // firmware v16+: rate accumulators (v17: 4 -> 8)
constexpr int MAX_CRC8_MESSAGES     = 20;  // transmit-CRC8 rules, one per stamped message
// v16: the four DOCUMENT-WIDE message passwords. A marked message names one by
// slot rather than carrying a key of its own, which is what lets a marking
// survive a Get for three bits and sixteen header bytes instead of a table.
// Named kMsgPasswordSlots, not MSG_PASSWORD_SLOTS: the firmware header defines
// the latter as a MACRO, and test_firmware_link includes both. A constexpr of
// the same name would be textually replaced by the macro before the compiler
// ever saw a declaration.
constexpr int kMsgPasswordSlots      = 4;
// Script bytecode chunks: 512 * 64 = 32,768 bytes of compiled script, the cap
// the firmware's flash table is sized for (protocol.h MAX_SCRIPT_CHUNKS).
constexpr int MAX_SCRIPT_CHUNKS     = 512;
// The engine ticks at 100 Hz, so one step per tick is the ceiling — an
// integrator cannot add more often than the evaluation pass runs.
constexpr int INTEGRATOR_MAX_HZ     = 100;

constexpr uint8_t START_MARKER = 0x55;

// CanSignalConfig::msg_idx (v3): 0..499 = the message this signal belongs to
// (parsed if that message is receive, packed if transmit); 0xFFFF = virtual.
constexpr uint16_t SIG_MSG_NONE = 0xFFFF;

// MonitorStreamPayload.flags bits (raw-frame monitor stream)
constexpr uint8_t MONFLAG_EXTENDED = 0x01;
constexpr uint8_t MONFLAG_FD       = 0x02;
constexpr uint8_t MONFLAG_BRS      = 0x04; // CAN FD bit-rate switch used
constexpr uint8_t MONFLAG_ESI      = 0x08; // CAN FD error-state indicator (passive)
// One or more frames were LOST between the previous frame and this one. The
// monitor stream is best-effort — it shares the serial link with everything
// else, and a bus busier than the link can describe will overrun it — so the
// requirement is not that it never drops but that it never drops SILENTLY. A
// trace missing half its frames without saying so invites the reader to
// conclude a message was never sent when it was only never reported.
constexpr uint8_t MONFLAG_GAP      = 0x10;

// The monitor stream sends MONITOR_HEADER_BYTES + data_len, not the whole
// struct: it reserves the CAN FD maximum, so a fixed-size frame spent 76 bytes
// describing a 4-byte message. Trimming takes that frame from 85 bytes on the
// wire to 25, which is the difference between a link that can describe a busy
// bus and one that drops two thirds of it.
//
// Read data_len from the payload and treat anything past it as absent. A
// payload of exactly sizeof(MonitorStreamPayload) is the older fixed-size form
// and must keep working, because a current Manager still has to read a device
// that has not been updated.
constexpr int MONITOR_HEADER_BYTES = 12; // through data_len, before data[]

// Message flags bits
constexpr uint8_t MSGFLAG_EXTENDED = 0x01;
constexpr uint8_t MSGFLAG_FD       = 0x02;
constexpr uint8_t MSGFLAG_ROUTING  = 0x04;
constexpr uint8_t MSGFLAG_ACTIVE   = 0x08;
constexpr uint8_t MSGFLAG_TRANSMIT = 0x10; // v3: transmit (compose+send) vs receive
constexpr uint8_t MSGFLAG_TX_SEQUENTIAL = 0x20; // v10: compound tx one-variant-per-period (else batch)

// CanMessageConfig::tx_trigger_flags — Triggered transmit. A separate byte and
// not two more MSGFLAG_* bits because this byte is full: 0x01..0x20 are taken
// and 0x40/0x80 are the protection level, whose values are pinned.
//
// Clear TXTRIG_ENABLED means Cyclic — transmit every period_ms, as always.
constexpr uint8_t TXTRIG_ENABLED     = 0x01; // transmit only while the condition holds
// 0x02 was TXTRIG_RESET_ON_TX — "Reset User Condition once Triggered". Retired
// before it ever shipped, superseded by the Reset expression a Set/Reset
// condition now carries: "send once when the request arrives" is a condition
// set on Message Received and reset on Message Transmitted, which says so where
// the user can see it instead of reaching across from a transmit message to
// rewrite a calculation's output. See protocol.h.
// tx_trigger_cond when no condition is named. TXTRIG_ENABLED is what decides
// whether the message is gated; this only stops an unset field from reading as
// condition 0 after a round trip.
constexpr uint16_t TX_TRIGGER_COND_NONE = 0xFFFFu;
// 2.3.0: bits 6-7 are ONE ORDERED PROTECTION LEVEL, not two independent flags.
// level = (flags & MSGPROT_MASK) >> 6, and that number IS CommsProtection:
//
//   0  None       ordinary message
//   1  ReadOnly   viewable, not editable
//   2  Hidden     not viewable, not editable
//   3  Protected  Hidden, plus the untick costs Protected Comms proved
//                 against a connected device
//
// THE DEVICE ENFORCES NONE OF THIS. All three tiers are conventions of this
// application; the bits are on the wire for ROUND-TRIP FIDELITY ONLY, so a Get
// followed by a Send cannot launder a Hidden message into an ordinary one.
//
// The VALUES are not free choices — this is the only assignment under which the
// two patterns that exist in shipped 2.2.x flash decode to the right new tier,
// which is what kept the protection change itself off the store version (it
// has since moved to 7 for the CAN diagnostic device channels, which is a
// layout change and a different question):
//
//   0x80  2.2.x "Read-only"             -> HIDDEN. 2.2.x concealed those in the
//                                         Manager, so decoding 0x80 as the new
//                                         VISIBLE Read Only would print the CAN
//                                         ID, layout and every channel's bit
//                                         position of every marked message on
//                                         the first Get. Over-restricting
//                                         annoys; under-restricting leaks.
//   0xC0  2.2.x "Protect Communication" -> PROTECTED. Exact match.
//   0x40  never emitted by 2.2.x        -> READ ONLY. Free precisely because
//                                         nothing shipped could produce it: the
//                                         old 0x40 flag was only ever sent
//                                         together with 0x80.
//
// MSGFLAG_PROTECTED and MSGFLAG_READONLY are DELETED as names, not renamed.
// Keeping them would be actively dangerous: 0x40 alone now means the WEAKEST
// tier, the inverse of what the name MSGFLAG_PROTECTED asserts.
//
// Must stay byte-for-byte identical to protocol.h. See the mirror note above.
constexpr uint8_t MSGPROT_MASK      = 0xC0;
constexpr uint8_t MSGPROT_NONE      = 0x00;
constexpr uint8_t MSGPROT_READONLY  = 0x40;
constexpr uint8_t MSGPROT_HIDDEN    = 0x80;
constexpr uint8_t MSGPROT_PROTECTED = 0xC0;
// The width of CanMessageConfig::reserved, and of an access key. Declared here
// rather than with the access-key block below because CanMessageConfig needs it
// first; the block below is unchanged.
constexpr int ACCESS_KEY_LEN         = 4;

// RelayConfig flags (v11)
constexpr uint8_t RELAYFLAG_EXTENDED = 0x01; // acts on extended frames (else standard)
constexpr uint8_t RELAYFLAG_INVERT   = 0x02; // forward the NON-matching frames
constexpr uint8_t RELAYFLAG_ACTIVE   = 0x04;
// 2.3.0: bits 6-7 of a relay's flags carry the SAME MSGPROT_* level, same
// values, same meaning. Free here because RELAYFLAG_* only used bits 0-2.
// Transported, not enforced — exactly like messages. It closes the gap where a
// marked relay section concealed in the GUI and reached the device carrying
// nothing, so a Get read it back as ordinary.

// Table2x16Def / Table8x8Def flags (v12). Axis interpolate bit set = linear
// interpolation; clear = discrete-centered (nearest site, midpoint transitions).
constexpr uint8_t TABLEFLAG_ACTIVE   = 0x01;
constexpr uint8_t TABLEFLAG_X_INTERP = 0x02;
constexpr uint8_t TABLEFLAG_Y_INTERP = 0x04; // two-axis (8x8) only

// Counter flags / mode (v3)
constexpr uint8_t COUNTERFLAG_ROLL     = 0x01;
constexpr uint8_t COUNTERFLAG_PRESERVE = 0x02;
constexpr uint8_t COUNTERFLAG_ACTIVE   = 0x04;
// COUNTER_MODE_RATE only: step down instead of up.
constexpr uint8_t COUNTERFLAG_RATE_DOWN = 0x08;
constexpr uint8_t COUNTER_MODE_UPDOWN  = 0;
constexpr uint8_t COUNTER_MODE_FOLLOW  = 1;
// Driven by the clock instead of by a channel: one step every 1/rate_hz
// seconds. Up/Down/Follow are ignored; Enable and Reset still apply.
constexpr uint8_t COUNTER_MODE_RATE    = 2;

// Steps per second offered for COUNTER_MODE_RATE. The engine ticks at 100 Hz,
// which is the ceiling; every entry divides 100 exactly, so each is a whole
// number of ticks per step and the rate is exact rather than averaged. The
// firmware accepts any 1..COUNTER_MAX_HZ (its phase accumulator handles the
// awkward ones), so this list is the host's editorial choice about what is
// worth offering, not a limit of the device.
constexpr int COUNTER_MAX_HZ = 100;
inline constexpr int kCounterRateChoices[] = {1, 2, 5, 10, 20, 50, 100};

// Timer flags (v3)
constexpr uint8_t TIMERFLAG_COUNTDOWN    = 0x01;
constexpr uint8_t TIMERFLAG_ROLLOVER     = 0x02;
constexpr uint8_t TIMERFLAG_SET_ON_START = 0x04;
constexpr uint8_t TIMERFLAG_SET_ON_STOP  = 0x08;
constexpr uint8_t TIMERFLAG_ACTIVE       = 0x10;

// Integrator flags (v16; COUNT_DOWN and PRESERVE added in v17)
constexpr uint8_t INTEGFLAG_ACTIVE      = 0x01;
constexpr uint8_t INTEGFLAG_CONST_INPUT = 0x02; // accumulate input_const, not a channel
constexpr uint8_t INTEGFLAG_COUNT_DOWN  = 0x04; // subtract instead of add (a "decrementor")
constexpr uint8_t INTEGFLAG_PRESERVE    = 0x08; // retain across power cycles (shares the
                                                // counters' 20-entry ring; works in both
                                                // of the device's flash modes)

// Signal value types
enum SignalDataType : uint8_t {
    SIGNAL_TYPE_UINT8  = 0x01,
    SIGNAL_TYPE_UINT16 = 0x02,
    SIGNAL_TYPE_UINT32 = 0x04,
    SIGNAL_TYPE_INT8   = 0x11,
    SIGNAL_TYPE_INT16  = 0x12,
    SIGNAL_TYPE_INT32  = 0x14,
    SIGNAL_TYPE_FLOAT  = 0x34,
    SIGNAL_TYPE_DOUBLE = 0x38,
};

enum MathOp : uint8_t {
    MATH_OP_ADD = 0, MATH_OP_SUB = 1, MATH_OP_MUL = 2, MATH_OP_DIV = 3,
    MATH_OP_SCALE = 4, MATH_OP_MIN = 5, MATH_OP_MAX = 6, MATH_OP_AND = 7, MATH_OP_OR = 8,
    // Advanced math (24-byte record). Booleans follow the true = value > 0
    // convention; comparisons with a NaN operand take the false branch (IEEE),
    // so LNOT(NaN) is 1. EQ/NE are EXACT float comparisons.
    MATH_OP_ABS    = 9,  // fabsf(a)
    MATH_OP_NEG    = 10, // -a
    MATH_OP_SQRT   = 11, // a < 0 ? 0 : sqrtf(a)  (guard mirrors DIV-by-0 -> 0)
    MATH_OP_FLOOR  = 12, // floorf(a)
    MATH_OP_CEIL   = 13, // ceilf(a)
    MATH_OP_ROUND  = 14, // roundf(a)
    MATH_OP_MOD    = 15, // b != 0 ? fmodf(a, b) : 0
    MATH_OP_XOR    = 16, // (float)(floatToI32Sat(a) ^ floatToI32Sat(b))
    MATH_OP_LAND   = 17, // (a > 0 && b > 0) ? 1 : 0
    MATH_OP_LOR    = 18, // (a > 0 || b > 0) ? 1 : 0
    MATH_OP_LNOT   = 19, // (a > 0) ? 0 : 1
    MATH_OP_GT     = 20, // (a > b) ? 1 : 0
    MATH_OP_GE     = 21, // (a >= b) ? 1 : 0
    MATH_OP_LT     = 22, // (a < b) ? 1 : 0
    MATH_OP_LE     = 23, // (a <= b) ? 1 : 0
    MATH_OP_EQ     = 24, // (a == b) ? 1 : 0  (exact float compare)
    MATH_OP_NE     = 25, // (a != b) ? 1 : 0
    MATH_OP_MULADD = 26, // a * b + c  (scale WITH offset — see FIRMWARE-NOTES #8)
    MATH_OP_CLAMP  = 27, // c > b ? min(max(a, b), c) : a — hi <= lo disables clamping
    MATH_OP_LERP   = 28, // a + (b - a) * c
    MATH_OP_SELECT = 29, // (a > 0) ? b : c
    MATH_OP_WRAP   = 30, // clampRoll(a, b, c) — the engine's NaN-safe wrap helper
};

// The NAME of each math op, and the ONE table that maps names to values and
// back. It sits here beside the enum, and beside mathOpArity() below, because
// two unrelated places need it and neither may keep a copy: the Lua binding (a
// name written in a script, and the ct.ops table it publishes) and the script
// disassembler — a script opcode in 0x00..0x1E IS a MathOp value handed
// straight to the same evaluator (script_vm.h, OPCODES), so naming an
// instruction and naming a math row are the same question. A second table would
// drift the moment an op was added, and the drift would surface as a
// disassembly listing confidently naming the wrong operation.
struct MathOpEntry {
    const char *name;
    int value;
};

// Compile-time references to the wire constants, so a value here can never
// drift from the firmware's. A NEW op does have to be added by hand; the count
// assert below is what catches forgetting.
constexpr MathOpEntry kMathOps[] = {
    { "ADD", MATH_OP_ADD },       { "SUB", MATH_OP_SUB },
    { "MUL", MATH_OP_MUL },       { "DIV", MATH_OP_DIV },
    { "SCALE", MATH_OP_SCALE },   { "MIN", MATH_OP_MIN },
    { "MAX", MATH_OP_MAX },       { "AND", MATH_OP_AND },
    { "OR", MATH_OP_OR },         { "ABS", MATH_OP_ABS },
    { "NEG", MATH_OP_NEG },       { "SQRT", MATH_OP_SQRT },
    { "FLOOR", MATH_OP_FLOOR },   { "CEIL", MATH_OP_CEIL },
    { "ROUND", MATH_OP_ROUND },   { "MOD", MATH_OP_MOD },
    { "XOR", MATH_OP_XOR },       { "LAND", MATH_OP_LAND },
    { "LOR", MATH_OP_LOR },       { "LNOT", MATH_OP_LNOT },
    { "GT", MATH_OP_GT },         { "GE", MATH_OP_GE },
    { "LT", MATH_OP_LT },         { "LE", MATH_OP_LE },
    { "EQ", MATH_OP_EQ },         { "NE", MATH_OP_NE },
    { "MULADD", MATH_OP_MULADD }, { "CLAMP", MATH_OP_CLAMP },
    { "LERP", MATH_OP_LERP },     { "SELECT", MATH_OP_SELECT },
    { "WRAP", MATH_OP_WRAP },
};
static_assert(sizeof(kMathOps) / sizeof(kMathOps[0]) == MATH_OP_WRAP + 1,
              "kMathOps must list every math op - a new opcode was added without "
              "a name");

// The mnemonic for `op`, or an empty string when it is not a math op. A walk
// rather than kMathOps[op] so the table's ORDER is not load-bearing: an entry
// inserted in the wrong place would otherwise rename every op after it.
constexpr const char *mathOpName(int op)
{
    for (const MathOpEntry &e : kMathOps) {
        if (e.value == op) {
            return e.name;
        }
    }
    return "";
}

// Operand count of a math op: 1 = A only, 2 = A and B, 3 = A, B and C. Unused
// operands are ignored by the engine and carried as defaults on the wire. The
// SINGLE source of truth for arity — the row editor (which operand groups to
// show), the device mapper, the validator (which references to check) and the
// Config Summary all ask this; nothing else may keep its own table.
constexpr int mathOpArity(int op)
{
    switch (op) {
    case MATH_OP_ABS: case MATH_OP_NEG: case MATH_OP_SQRT: case MATH_OP_FLOOR:
    case MATH_OP_CEIL: case MATH_OP_ROUND: case MATH_OP_LNOT:
        return 1;
    case MATH_OP_MULADD: case MATH_OP_CLAMP: case MATH_OP_LERP:
    case MATH_OP_SELECT: case MATH_OP_WRAP:
        return 3;
    default:
        return 2;
    }
}

enum ConditionOp : uint8_t {
    COND_OP_EQ = 0, COND_OP_NEQ = 1, COND_OP_LT = 2, COND_OP_LTE = 3,
    COND_OP_GT = 4, COND_OP_GTE = 5,
    // The two MESSAGE operators. A term carrying one of these is not a
    // comparison: input_a_signal_idx holds a MESSAGE index, and input_b is
    // unused and written zero. True only on the evaluation pass in which a
    // frame actually happened — see protocol.h for the receive/transmit
    // asymmetry, which is visible to a user.
    COND_OP_MSG_RX = 6,
    COND_OP_MSG_TX = 7,
};

// Does this operator take a MESSAGE index in input_a rather than a signal? The
// bounds check differs, so every site validating a term has to ask.
inline bool condOpIsMessage(uint8_t op)
{
    return op == COND_OP_MSG_RX || op == COND_OP_MSG_TX;
}

// The logical negation of a comparison, which is what migrates a pre-modes
// condition into a Set/Reset: the Reset expression is the inverse of the Set,
// and inverting an expression means flipping every operator and every joiner.
//
// All six comparisons invert WITHIN the set, so the migration is lossless. The
// message operators have no negation — "a frame did not arrive this pass" is
// not a thing a Set/Reset wants to latch on — and are returned unchanged;
// callers must not offer them to the migration.
inline uint8_t condOpNegate(uint8_t op)
{
    switch (op) {
    case COND_OP_EQ:  return COND_OP_NEQ;
    case COND_OP_NEQ: return COND_OP_EQ;
    case COND_OP_LT:  return COND_OP_GTE;
    case COND_OP_GTE: return COND_OP_LT;
    case COND_OP_LTE: return COND_OP_GT;
    case COND_OP_GT:  return COND_OP_LTE;
    default:          return op;
    }
}

// v14: how one comparison of a condition joins to the next.
enum ConditionJoin : uint8_t {
    COND_JOIN_AND = 0, COND_JOIN_OR = 1,
};
constexpr int COND_MAX_TERMS = 3; // comparisons per condition, per expression

// ConditionConfig::flags
constexpr uint8_t CONDFLAG_ACTIVE   = 0x01;
constexpr uint8_t CONDFLAG_SETRESET = 0x02; // set = Set/Reset latch, clear = Momentary

// Momentary hold ceiling. The hold is spent against elapsed_ms on the device's
// 10 ms calculation pass, so 100 Hz is the frequency whose period is one tick
// and nothing finer can be represented.
constexpr uint8_t COND_LATCH_MAX_HZ = 100;


#pragma pack(push, 1)

struct CanMessageConfig {
    uint32_t can_id;
    uint8_t flags;          // MSGFLAG_* â€” bit4 selects transmit vs receive
    uint8_t src_bus;        // source bus (receive) / target bus (transmit), 1..3
    uint8_t route_bus_mask; // receive only: bit0=CAN1 bit1=CAN2 bit2=CAN3
    uint8_t dlc;
    uint16_t period_ms;     // transmit: send period >= 10. receive (v4): receive
                            // timeout in ms (0 = no timeout / defaults off)
    // Triggered transmit (store v10). Three of the four retired per-message key
    // bytes, claimed in place: the record is still 14 bytes, so no offset moved,
    // no chunk constant changed and the feature cost no config flash at all.
    // TXTRIG_ENABLED clear — an all-zero field — means Cyclic, which is exactly
    // what every message did before this existed.
    uint16_t tx_trigger_cond;  // condition index gating this message, or
                               // TX_TRIGGER_COND_NONE
    uint8_t tx_trigger_flags;  // TXTRIG_*
    // v16: WHICH OF THE DOCUMENT'S FOUR MESSAGE PASSWORDS GUARDS THIS MESSAGE.
    // 0 = none, 1..MSG_PASSWORD_SLOTS. Three bits of a byte that already existed.
    //
    // The same byte carried the last octet of the v20 PER-MESSAGE key, retired
    // in 2.3.0 because live key material on the device made it a distribution
    // channel for the secret. What sits here now is a REFERENCE: the four keys
    // live once in the config header, not once per message. The device clamps
    // anything above MSG_PASSWORD_SLOTS to 0 on both the write and the read
    // path, which keeps the spare bits from becoming private host storage —
    // the reason the old scrub existed, carried over.
    uint8_t password_slot;
};

// 64 bytes: label 32 + five floats 20 + four u16 8 + one u32 4.
//
// v15 took this record 72 -> 48 by cutting the label 32 -> 16, bit-packing the
// eight small fields and narrowing mux_id/mux_mask to 16 bits (a 2-byte selector
// window). The bit packing and the narrow mux fields STAY — they cost nothing
// and the accessors below depend on them. Only the label went back to 32, paid
// for out of the enlarged flash region rather than out of the record, because a
// 15-character channel name is a limit users hit and a 31-character one is not.
// 64 also happens to be the free size: slots pad to 8 bytes and PAD8(64) = 64,
// so the wider label wastes not one byte of padding.
// MUST stay byte-identical to firmware/include/protocol.h; the accessors below
// mirror the ones there and test_firmware_link cross-checks that both agree.
struct CanSignalConfig {
    char label[SIGNAL_LABEL_LEN]; // MAX_CHANNEL_NAME_BYTES chars + NUL
    float factor;
    float offset;
    float min_val;
    float max_val;
    float default_value;     // v4: physical value applied on receive-message timeout
    uint16_t mux_id;         // v8/v15: compound gating â€” see mux_mask
    uint16_t mux_mask;       // 0 = always active; else extract only while
                             // (2-byte LE window at mux_byte_offset & mux_mask)
                             //   == (mux_id & mux_mask)
    uint16_t msg_and_flags;  // msg_idx(9) | byte_order<<9 | is_active<<10
                             //   | tx_wrap<<11 | selector_only<<12.
                             //   Bits 13-15 are free.
    uint16_t tx_source;      // transmit source signal index + 1; 0 = own slot
    uint32_t bits;           // start_bit(9) | (bit_length-1)(6)<<9 | value_type(6)<<15
                             //   | decimal_places(4)<<21 | mux_byte_offset(6)<<25
};

// 18 -> 24 bytes for the third operand (advanced math). Offsets 0-17 are
// UNCHANGED, so a legacy record is the first 18 bytes of this one; bytes 18-23
// of pre-existing flash slots are pad (0xFF), and only arity-3 ops ever read C
// — mapFromDevice normalises anyway. Device-side this rode a FLASH_STORE_VERSION
// bump (1 -> 2): the boot CRC hashes item_size bytes per stored record, so a v1
// image with math rows would fail it under the new firmware regardless; the bump
// just makes every v1 image read back as empty uniformly (FIRMWARE-NOTES #22).
// input_c_val is four RAW bytes, hand-packed like the bit-packed signal fields
// (FIRMWARE-NOTES #9b discipline — no union, no bitfield): the accessors below
// are the only place the layout is spelled out.
struct MathConfig {
    uint8_t op;
    uint8_t input_a_type;   // 0 = const, 1 = signal
    uint16_t input_a_idx;
    float input_a_const;
    uint8_t input_b_type;
    uint16_t input_b_idx;
    float input_b_const;
    uint16_t dest_signal_idx;
    uint8_t is_active;
    uint8_t input_c_type;   // 0 = const, 1 = signal
    uint8_t input_c_val[4]; // type 0: float32 LE; type 1: u16 idx in [0..1], [2..3] = 0
    uint8_t reserved0;      // write 0
};

// C-operand pack/unpack. Byte assembly is explicit so the wire stays
// little-endian regardless of the host.
inline void mathSetInputCConst(MathConfig &m, float v)
{
    uint32_t u = 0;
    static_assert(sizeof(u) == sizeof(v), "float32 wire format");
    std::memcpy(&u, &v, sizeof(u));
    m.input_c_type = 0;
    m.input_c_val[0] = uint8_t(u);
    m.input_c_val[1] = uint8_t(u >> 8);
    m.input_c_val[2] = uint8_t(u >> 16);
    m.input_c_val[3] = uint8_t(u >> 24);
}
inline void mathSetInputCSignal(MathConfig &m, uint16_t idx)
{
    m.input_c_type = 1;
    m.input_c_val[0] = uint8_t(idx);
    m.input_c_val[1] = uint8_t(idx >> 8);
    m.input_c_val[2] = 0;
    m.input_c_val[3] = 0;
}
inline float mathInputCConst(const MathConfig &m)
{
    const uint32_t u = uint32_t(m.input_c_val[0]) | (uint32_t(m.input_c_val[1]) << 8)
                       | (uint32_t(m.input_c_val[2]) << 16) | (uint32_t(m.input_c_val[3]) << 24);
    float v = 0;
    std::memcpy(&v, &u, sizeof(v));
    return v;
}
inline uint16_t mathInputCIdx(const MathConfig &m)
{
    return uint16_t(uint16_t(m.input_c_val[0]) | (uint16_t(m.input_c_val[1]) << 8));
}

// One "A op B" comparison of a condition — or, for the message operators, one
// event test.
//
// B IS A UNION, and that is what pays for the second expression. The members
// were always mutually exclusive (input_b_type says which is live) so
// overlapping them takes the term 10 -> 8 bytes, which takes a six-term
// ConditionConfig 72 -> 56 and is the difference between 250 conditions fitting
// and not fitting.
//
// ZERO THE WHOLE UNION BEFORE WRITING input_b_idx. The upper two bytes travel
// on the wire and are otherwise whatever the writer left there; value-
// initialising the record does it.
struct ConditionTerm {
    uint16_t input_a_signal_idx; // signal index — or MESSAGE index for the
                                 // message operators
    uint8_t op;
    uint8_t input_b_type;   // 0 = const, 1 = signal; ignored for message ops
    union {
        uint16_t input_b_idx;
        float input_b_const;
    } b;
};

// A User Condition drives a boolean output slot, in one of TWO MODES. See
// protocol.h for the full contract; the short version:
//
//   MOMENTARY (CONDFLAG_SETRESET clear) — the RISING EDGE of the Set expression
//     drives the output to 1 and it holds for one period of latch_hz (10 Hz is
//     100 ms), then drops on its own. Retriggerable: a fresh edge RELOADS the
//     hold. reset_terms unused.
//   SET / RESET (CONDFLAG_SETRESET set) — a latch. Set drives 1, Reset drives 0,
//     it HOLDS in between, and RESET IS DOMINANT. latch_hz unused.
//
// The plain level a condition used to be is gone, and every configuration
// written before the modes was migrated to a Set/Reset whose Reset expression is
// the logical inverse of its Set — which behaves identically. See condOpNegate.
//
// Both expressions fold up to COND_MAX_TERMS comparisons STRICTLY LEFT TO RIGHT
// — ((t0 J0 t1) J1 t2) — matching the bracketing the editor shows rather than
// C's precedence for &&. Each joiner byte holds one ConditionJoin BIT per gap.
struct ConditionConfig {
    ConditionTerm set_terms[COND_MAX_TERMS];
    ConditionTerm reset_terms[COND_MAX_TERMS];
    // v14: the "for" qualifier, ONE PER EXPRESSION, in CENTISECONDS. That side
    // must be continuously true for this long before it counts as true; 0
    // disables it. Reset defaults to 0 — immediate — because a latch that
    // cannot be cleared promptly is the wrong way for one to fail, but delay is
    // available for "clear only once the fault has been gone a while".
    //
    // Centiseconds is what makes the second pair FREE: two u32 plus two masks is
    // a 66-byte record and a 72-byte slot, 1,600 B the region has not got. Two
    // u16 is 62 and the slot stays 64. The ceiling is 655.35 s at 10 ms
    // resolution, which is the calculation tick. See protocol.h.
    uint16_t set_qualify_cs;
    uint16_t reset_qualify_cs;
    // Which comparisons each qualifier applies to — bit i for term i of that
    // expression. 0 means the whole folded expression; non-zero qualifies those
    // terms individually and leaves the rest instant.
    uint8_t set_qualify_terms;
    uint8_t reset_qualify_terms;
    uint16_t dest_signal_idx; // boolean output: 1.0 while held, else 0.0
    uint8_t flags;            // CONDFLAG_*
    uint8_t set_count;        // 1..COND_MAX_TERMS
    uint8_t set_joiners;
    uint8_t reset_count;      // Set/Reset only
    uint8_t reset_joiners;
    uint8_t latch_hz;         // Momentary only, 1..COND_LATCH_MAX_HZ
};

// v16: rate accumulator — `out += input` (or `-=` with COUNT_DOWN, v17),
// rate_hz times per second. RAW accumulation, deliberately not `input * dt`:
// the rate scales the result, which is what makes it worth configuring. To turn
// a rate channel into a total, pre-scale it with a Math channel so one step
// moves the right slice.
// The engine ticks at 100 Hz, so rate_hz is clamped to 1..INTEGRATOR_MAX_HZ; a
// rate that does not divide 1000 is carried in a Hz*ms phase accumulator, so it
// averages exactly rate_hz steps per second instead of drifting.
// v17: a "decrementor" is COUNT_DOWN plus start_value at the peak — the same
// record and the same engine pass, differing by a sign and a seed.
struct IntegratorConfig {
    uint16_t input_signal_idx;  // accumulated each step (unused if CONST_INPUT)
    uint16_t reset_signal_idx;  // rising edge -> reset_value (0xFFFF = unused)
    uint16_t enable_signal_idx; // gate: accumulates only while > 0 (0xFFFF = always)
    uint16_t dest_signal_idx;   // output value slot
    float input_const;          // accumulated instead when CONST_INPUT is set
    float min_value;            // clamp; max <= min disables clamping
    float max_value;
    float reset_value;          // loaded on the reset input's rising edge
    float start_value;          // v17: seeded into the slot when the config loads
                                // (a restored PRESERVE value overrides it)
    uint8_t rate_hz;            // steps per second, 1..INTEGRATOR_MAX_HZ
    uint8_t flags;              // INTEGFLAG_*
};

// Transmit CRC8 rule (CMD_WRITE/READ_CRC8_CFG). Binds to a MESSAGE table
// entry; the composer stamps the computed byte into the frame LAST — after
// the channels and, on a compound message, the variant's selector — and
// publishes it to dest_signal_idx. The parameterisation is the standard
// CRC-8 set (x^8 implicit): SAE J1850 = 0x1D/0xFF/0xFF, CCITT = 0x07/0/0.
constexpr uint8_t CRC8FLAG_ACTIVE  = 0x01;
constexpr uint8_t CRC8FLAG_REF_IN  = 0x02; // reflect each input byte first
constexpr uint8_t CRC8FLAG_REF_OUT = 0x04; // reflect the register before final XOR

constexpr int CRC8_MAX_ELEMENTS = 15;

// elem_type values: what one element feeds into the CRC, in element order.
constexpr uint8_t CRC8_ELEM_ID   = 0; // elem_value = shift 0..3: (id >> 8*v) & 0xFF
constexpr uint8_t CRC8_ELEM_DATA = 1; // elem_value = frame byte 0..7 (0 past the DLC)
constexpr uint8_t CRC8_ELEM_RAW  = 2; // elem_value fed as-is

// v16: the four document-wide message passwords, as they ride in the config
// header. A slot holding kNoAccessKey (0) is empty.
//
// UNLIKE the device's own access keys, these COME BACK OUT on a Get. Those gate
// the device and are write-only by design; these are the lock on a marking the
// Device Manager enforces, and a configuration read back without them has
// marked messages nobody can open, edit or release.
struct MessagePasswordRecord {
    uint32_t key[kMsgPasswordSlots];
};

struct Crc8Config {
    uint16_t msg_idx;          // the message table entry this rule stamps
    uint16_t dest_signal_idx;  // slot the CRC publishes to (SIG_MSG_NONE = none)
    uint8_t byte_location;     // frame byte receiving the CRC, 0..7
    uint8_t polynomial;
    uint8_t init_value;
    uint8_t final_xor;
    uint8_t flags;             // CRC8FLAG_*
    uint8_t element_count;     // 1..CRC8_MAX_ELEMENTS
    uint8_t elem_type[CRC8_MAX_ELEMENTS];
    uint8_t elem_value[CRC8_MAX_ELEMENTS];
};

struct DeviceStatus {
    uint32_t uptime_ms;
    uint32_t rx_count[3];
    uint32_t tx_count[3];
    uint8_t bus_state[3];
    uint16_t active_msg_count;
    uint16_t active_sig_count;
    uint16_t active_math_count;
    uint16_t active_cond_count;
};

struct ControlCanPayload {
    uint8_t bus_idx;         // 1..3
    uint8_t mode;            // 0 = off, 1 = active, 2 = listen-only
    uint32_t baud_rate;
    uint32_t data_baud_rate; // > baud_rate enables CAN FD with BRS
    uint8_t termination;     // v9: 1 = enable the bus termination resistor
};

// kbps <-> Hz for the bus rates, in ONE place because 83 is the label that
// lies: "83.3k" is GMLAN's 83,333 bit/s (1 Mbit / 12), and the kbps*1000 the
// other rates use would program 83,000 — 0.4% slow, enough that a real GMLAN
// node answers with error frames instead of ACKs. Every conversion in either
// direction goes through this pair so the two directions cannot disagree.
constexpr uint32_t busRateHz(int kbps)
{
    return kbps == 83 ? 83333u : uint32_t(kbps) * 1000u;
}
constexpr int busRateKbpsFromHz(uint32_t hz)
{
    return hz == 83333u ? 83 : int(hz / 1000u);
}

// v19: the access keys as they sit in the device's flash header. Mirrors
// AccessKeyRecord in firmware/include/protocol.h byte for byte — the pair is
// asserted equal in test_firmware_link, the only place both headers are visible.
// The keys are write-only: nothing reads them back, so the wire never carries
// one and a serial capture yields nothing to replay.
constexpr int ACCESS_CHALLENGE_LEN   = 16;
constexpr uint8_t ACCESS_FN_SEND       = 0;
constexpr uint8_t ACCESS_FN_GET        = 1;
constexpr uint8_t ACCESS_FN_EDIT_COMMS = 2;
constexpr int ACCESS_FN_COUNT        = 3;
constexpr uint8_t ACCESS_MASK_SEND       = 1u << ACCESS_FN_SEND;
constexpr uint8_t ACCESS_MASK_GET        = 1u << ACCESS_FN_GET;
constexpr uint8_t ACCESS_MASK_EDIT_COMMS = 1u << ACCESS_FN_EDIT_COMMS;

struct AccessKeyRecord {
    uint8_t set_mask;                              // ACCESS_MASK_*; 0 = no passwords
    uint8_t keys[ACCESS_FN_COUNT][ACCESS_KEY_LEN]; // never read back off the device
    // v17: Protected Comms slots 2..4 (slot 1 is keys[ACCESS_FN_EDIT_COMMS]).
    // An all-zero key is an empty slot; any non-empty slot proves the function.
    uint8_t prot_comms_extra[3][ACCESS_KEY_LEN];
};

struct AccessKeyWritePayload {
    uint8_t function;               // ACCESS_FN_*
    uint8_t clear;                  // non-zero = remove the password
    uint8_t key[ACCESS_KEY_LEN];
    // v17: which Protected Comms slot (1..4); ignored for Send and Get. The
    // device also accepts the old 6-byte payload as slot 1.
    uint8_t slot;
};

// The fleet identity as CMD_READ_FLEET_ID returns it. Mirrors
// FleetIdentityPublic in firmware/include/protocol.h byte for byte; the pair is
// asserted equal in test_firmware_link.
//
// fleet_key is absent: it is compiled into the firmware and never comes back
// off the device — CMD_FLEET_ID_PROVE is how a host learns the device holds it.
//
// The two strings are fixed-width and NUL-PADDED, not NUL-terminated, so all 16
// bytes are usable. Read them as counted fields.
constexpr int FLEET_VENDOR_ID_LEN = 16;
constexpr int FLEET_MODEL_ID_LEN  = 16;
constexpr int FLEET_KEY_LEN       = 4;

struct FleetIdentityPublic {
    char vendor_id[FLEET_VENDOR_ID_LEN];
    char model_id[FLEET_MODEL_ID_LEN];
    uint32_t serial_number;
    uint16_t config_version; // from the flash header, not the build
    uint16_t flags;
    uint8_t key_present;
};

struct InjectCanPayload {
    uint8_t bus_idx;        // 1..3
    uint32_t can_id;
    uint8_t flags;          // bit0 = extended, bit1 = fd
    uint8_t data_len;       // 0..64 (physical TX truncates to 8)
    uint8_t data[64];
};

struct MonitorStreamPayload {
    uint32_t timestamp_ms;
    uint8_t bus_idx;        // 1..3
    uint8_t direction;      // 0 = Rx, 1 = Tx
    uint32_t can_id;
    uint8_t flags;          // MONFLAG_* (extended / FD / BRS / ESI)
    uint8_t data_len;
    uint8_t data[64];
};

struct SignalValueEntry {
    uint16_t signal_idx;
    float physical_value;
};

// v3: up/down counter â€” edge-triggered on the rising edge of boolean channels.
struct CounterConfig {
    uint16_t up_signal_idx;     // 0xFFFF = unused
    uint16_t down_signal_idx;
    uint16_t follow_signal_idx;
    uint16_t reset_signal_idx;
    uint16_t enable_signal_idx; // 0xFFFF = always enabled
    uint16_t dest_signal_idx;
    float min_value;
    float max_value;
    float reset_value;
    float step;
    uint8_t mode;               // COUNTER_MODE_*
    uint8_t flags;              // COUNTERFLAG_*
    // COUNTER_MODE_RATE: steps per second, 1..COUNTER_MAX_HZ. The record grew
    // 30 -> 31 for this at no cost in device flash (slots pad to 8, and both
    // 30 and 31 pad to 32). On the wire it was free at the old 112-byte cap
    // (4 + 3*31 = 97, still 3 records per frame) and costs exactly one record
    // per frame at 496: WRITE_CHUNK_COUNTERS is 15 where a 30-byte record would
    // have fitted 16. Fifty counters is 4 frames either way.
    uint8_t rate_hz;
    // v15: where each input comes from, two bits each. A SIGNAL input reads the
    // channel at *_signal_idx; a MESSAGE input treats that same field as a
    // MESSAGE index and is true only on the pass the frame happened. Follow has
    // no kind — it reads a value, and a frame arriving has none.
    //
    // Free, and by the same accident the rate_hz note above describes: 31 bytes
    // and 32 both pad to a 32-byte slot, so the byte was already being written.
    // That slack is now spent; the next field costs 400 B.
    uint8_t input_kinds;
};

// Two bits per counter input, in input_kinds.
enum CounterSrc : uint8_t {
    COUNTER_SRC_SIGNAL = 0,
    COUNTER_SRC_MSG_RX = 1,
    COUNTER_SRC_MSG_TX = 2,
};
constexpr int kCounterSrcShiftUp = 0;
constexpr int kCounterSrcShiftDown = 2;
constexpr int kCounterSrcShiftReset = 4;
constexpr int kCounterSrcShiftEnable = 6;

// CMD_WRITE/READ_DEVICE_CHANNELS payload — one destination slot per device
// channel, SIG_MSG_NONE for the ones this configuration does not read.
// Indexed by the DEVCH_* ids below, which the channel catalogue also carries,
// so the mapper walks the catalogue instead of naming each channel.
//
// DEVCH_ONTIME is 0 and must stay there: the struct began as a bare
// `uint16_t ontime_signal_idx`, so keeping OnTime first makes the old 2-byte
// payload a valid PREFIX of this one, which is what lets device firmware accept
// a short write from an older GUI rather than NACKing it. Mirrors protocol.h.
enum DeviceChannelBusField {
    DEVCH_BUS_RX_ERRORS     = 0, // REC, 0..127
    DEVCH_BUS_TX_ERRORS     = 1, // TEC, 0..255
    DEVCH_BUS_WARNING       = 2, // 1 once either counter reaches 96
    DEVCH_BUS_ERROR_PASSIVE = 3, // 1 while the node is error-passive
    DEVCH_BUS_BUS_OFF       = 4, // 1 while the node is bus-off
    DEVCH_BUS_ERROR_FRAMES  = 5, // accumulated protocol errors since boot
    DEVCH_BUS_RX_COUNT      = 6, // frames received since boot
    DEVCH_BUS_TX_COUNT      = 7, // frames transmitted since boot
    DEVCH_BUS_LOAD          = 8, // estimated bus utilisation, 0..100 %
    // Times the firmware has RESTARTED this bus after bus-off — the attempt,
    // not the outcome, because a restart on a still-faulty bus is undone faster
    // than one sample. Reads with DEVCH_BUS_BUS_OFF, which carries the outcome:
    // off=1 with this climbing is a bus being retried and failing every time,
    // off=0 with this climbing is one that is flapping.
    DEVCH_BUS_OFF_RECOVERIES = 9,
    DEVCH_PER_BUS            = 10
};

// bus0 is 0-BASED (bus 1 is 0), unlike the 1-based bus_idx used on the wire.
constexpr int DEVCH_ONTIME = 0;
constexpr int DEVCH_BUS_BASE = 1;
constexpr int DEVCH_BUS_COUNT = 3;
// Store v9: the MCU health block, APPENDED after the bus blocks so every index
// above survives — the same rule that keeps OnTime at 0, applied at the other
// end. Temperature and VDDA come off the ADC's internal sensors with the
// factory calibration points applied; the MIN/MAX pair are the excursions
// since BOOT, not since config load — they survive CLEAR_CONFIG the way the
// CAN error totals do — and the reset reason is read once from the RCC flags
// at boot and latched.
constexpr int DEVCH_MCU_TEMP     = DEVCH_BUS_BASE + DEVCH_BUS_COUNT * DEVCH_PER_BUS; // 31
constexpr int DEVCH_MCU_VDDA     = DEVCH_MCU_TEMP + 1; // 32
constexpr int DEVCH_MCU_VDDA_MIN = DEVCH_MCU_TEMP + 2; // 33
constexpr int DEVCH_MCU_TEMP_MAX = DEVCH_MCU_TEMP + 3; // 34
constexpr int DEVCH_RESET_REASON = DEVCH_MCU_TEMP + 4; // 35
constexpr int DEVCH_COUNT        = DEVCH_MCU_TEMP + 5; // 36
constexpr int devChBus(int bus0, int field)
{
    return DEVCH_BUS_BASE + bus0 * DEVCH_PER_BUS + field;
}

// Why the last reset happened, for DEVCH_RESET_REASON. The numbering is WIRE
// CONTRACT, not implementation convenience: the channel catalogue carries these
// as the display enumeration for Device Last Reset Reason, so every label the
// GUI shows is only true if these values are. POWER_ON vs BROWNOUT: the RCC
// cannot tell them apart (a true power-on raises the brownout flag too), so the
// firmware disambiguates with a .noinit RAM magic — random garbage after a cold
// power-up, intact after a brownout-depth dip that reset the core but never
// drained the SRAM.
constexpr uint8_t RESET_REASON_UNKNOWN   = 0;
constexpr uint8_t RESET_REASON_POWER_ON  = 1;
constexpr uint8_t RESET_REASON_BROWNOUT  = 2;
constexpr uint8_t RESET_REASON_NRST      = 3; // the external reset pin
constexpr uint8_t RESET_REASON_SOFTWARE  = 4; // NVIC_SystemReset — CMD_RESET_DEVICE, a fault handler
constexpr uint8_t RESET_REASON_IWDG      = 5; // independent watchdog
constexpr uint8_t RESET_REASON_WWDG      = 6; // window watchdog
constexpr uint8_t RESET_REASON_LOW_POWER = 7;

struct DeviceChannelsConfig {
    uint16_t signal_idx[DEVCH_COUNT];
};

// A DeviceChannelsConfig with every channel marked unused.
//
// Use this instead of `DeviceChannelsConfig x{}` or `{SIG_MSG_NONE}`, and read
// the reason before replacing it with either: "unused" is 0xFFFF, but
// aggregate initialisation ZERO-fills whatever it does not name — and slot 0 is
// a perfectly valid destination. Both shorthands therefore produce a config
// that points 35 or 36 device channels at signal 0 and has the device overwrite
// it a hundred times a second. This was a one-field struct where `{SIG_MSG_NONE}`
// was correct; it is not one any more.
inline DeviceChannelsConfig unusedDeviceChannels()
{
    DeviceChannelsConfig c;
    for (int i = 0; i < DEVCH_COUNT; ++i)
        c.signal_idx[i] = SIG_MSG_NONE;
    return c;
}

// v3: timer â€” accumulates seconds while running, started/stopped on edges.
// v12: start and stop are CONDITION TERMS, the same 8-byte comparison record a
// User Condition is built from, so a timer can key off "RPM > 4000" or "this
// message was received" without a condition row standing in between. The edge is
// unchanged: the term yields a boolean and the timer triggers on its RISING
// edge, so the pre-v12 behaviour is exactly the term (channel NEQ 0).
//
// Field order matches the firmware exactly, which matters for the WIRE even
// though it does not affect the size: these structs are #pragma pack(1), so a
// record is the sum of its fields and 2*8 + 3*4 + 2 + 1 + 1 is 32 whatever the
// order. v12's comment claimed the order was what bought the 32; it was not.
struct TimerConfig {
    ConditionTerm start_term;
    ConditionTerm stop_term;
    float limit_value;
    float start_value;
    float stop_value;
    uint16_t dest_signal_idx;
    uint8_t flags;              // TIMERFLAG_*
    uint8_t reserved;
};

// A term half that is not in use. Out of range is how a term says "never", the
// same thing the old 0xFFFF in start_signal_idx said.
constexpr uint16_t kTimerTermUnused = 0xFFFFu;

// v6: constant â€” writes a fixed value into a generated channel's slot.
struct ConstantConfig {
    uint16_t dest_signal_idx;
    float value;
    uint8_t is_active;
};

// v11: message relay â€” masked-ID gateway rule, checked on every received frame.
struct RelayConfig {
    uint32_t address;
    uint32_t bitmask;
    uint8_t flags;            // RELAYFLAG_*
    uint8_t src_bus;          // bus this rule listens on (1..3)
    uint8_t forward_bus_mask; // bit0=CAN1 bit1=CAN2 bit2=CAN3
};

// v13: 2x16 lookup table â€” one axis, up to 16 ascending sites (x_count active).
// Split across two records at the SAME index in two parallel tables. The reason
// given when it was written — a combined 134-byte record cannot fit the
// 112-byte payload cap — expired when the cap went to 496; 134 would fit a
// frame now. The split stays because of the OTHER limit, which did not move:
// the device pads every record to an 8-byte slot and MAX_PADDED_RECORD is 112,
// so a 134-byte record has nowhere to live in flash regardless of how it
// travels. The device evaluates table t only when both tables hold a record at
// index t.
struct Table2x16Def {
    uint16_t x_signal_idx;    // input axis channel
    uint16_t dest_signal_idx; // output value slot
    uint8_t flags;            // TABLEFLAG_ACTIVE | TABLEFLAG_X_INTERP
    uint8_t x_count;          // active sites, 1..16
    float x_sites[TABLE_2X16_SITES];
};

struct Table2x16Out {
    float outputs[TABLE_2X16_SITES];
};

// 8x8 lookup table — X and Y axes, up to 8 ascending sites each (x_count /
// y_count active), replacing the v12 4x4. Split like the 2x16, and for what is
// now the 2x16's surviving reason: a combined record is
// 2+2+2+1+1+1 + 8*4 + 8*4 + 64*4 = 329 bytes, which the 496-byte payload cap
// would carry but MAX_PADDED_RECORD (112) cannot store — PAD8(329) is 336.
//
// The split is Def + one record per grid ROW, NOT Def + one grid blob. Table t
// owns Def index t and Row indices t*8 .. t*8+7, and because PAD8(32) == 32
// those eight slots are byte-contiguous in the device's flash: the engine takes
// one pointer at row t*8 and reads grid[y*8 + x], exactly as the 4x4's
// outputs[y*4 + x] did. That property is the reason for this shape — any other
// chunking costs a reassembly buffer in RAM the device does not have.
//
// TORN-UPLOAD SAFETY, inherited from the 2x16 and equally load-bearing here:
// only the Def carries TABLEFLAG_ACTIVE, the Rows carry no flags of their own,
// and the device evaluates table t only once it holds a Def at t AND at least
// (t+1)*8 rows. Unprogrammed flash reads 0xFF, which as a float is NaN, so that
// guard is what stops a half-uploaded grid poisoning an output channel. The host
// side of the same guarantee is the write ORDER — rows before def — in
// config_transfer.cpp.
struct Table8x8Def {
    uint16_t x_signal_idx;
    uint16_t y_signal_idx;
    uint16_t dest_signal_idx;
    uint8_t flags;            // TABLEFLAG_ACTIVE | X_INTERP | Y_INTERP
    uint8_t x_count;          // active X sites, 1..8
    uint8_t y_count;          // active Y sites, 1..8
    float x_sites[TABLE_8X8_SITES];
    float y_sites[TABLE_8X8_SITES];
};

// ONE grid row: the outputs at a fixed y, x ascending. Record index t*8 + y.
//
// The firmware calls this struct Table8x8Row. The GUI cannot: ct::Table8x8Row is
// already the DOCUMENT row in comms_types.h — one whole table, one line in the
// Tables dialog — and device_mapper.h has both headers open at once, so the two
// meanings of "row" would collide in one namespace. GridRow says which row is
// meant. Same 32 bytes, same field, same order; test_firmware_link asserts the
// pair against the firmware header, which is where the names are reconciled.
struct Table8x8GridRow {
    float v[TABLE_8X8_SITES];
};

// --- Firmware update -------------------------------------------------------
// Mirrors protocol.h. See the CMD_FW_UPDATE_* block near the top of this file
// for the sequence.

struct FwUpdateBeginPayload {
    uint32_t image_size;   // total bytes, 8-byte aligned, <= staging_capacity
    uint32_t image_crc32;  // what the device checks the staged bytes against
    uint16_t product_id;   // FW_PRODUCT_CAN_TRIPLE; anything else is refused
    uint16_t version_major;
    uint16_t version_minor;
    uint16_t version_patch;
};

// One chunk of script bytecode. A bare byte array on purpose: the structure
// inside is the VM's business (script_vm.h), and 64 is load-bearing because
// PAD8(64) == 64 keeps chunks byte-contiguous in device flash.
struct ScriptChunk {
    uint8_t b[64];
};

// CMD_SCRIPT_STATUS reply — what the device made of the stored script.
struct ScriptStatus {
    uint8_t  present;
    uint8_t  verify_result;  // SCRIPT_OK, or why the script was rejected
    uint8_t  fault;          // SCRIPT_FAULT_* from the last run
    uint8_t  suspended;      // faulted; will not run again until reload
    uint32_t code_bytes;
    uint32_t last_cost;      // budget units spent in the most recent tick
    uint32_t peak_cost;      // highest single-tick spend since load
    uint16_t num_state;
    uint16_t budget;         // the device's SCRIPT_TICK_BUDGET
    // Appended after the first shipping VM — every field above keeps its
    // offset. Budget units bound execution; these say what the bound is worth
    // in time (170 MHz, so a 10 ms tick is 1,700,000 cycles). cycles_valid is 0
    // on a part with no usable counter, and the two counts must NOT then be
    // shown as a script that costs nothing.
    //
    // Read replies with >= not ==: firmware older than this answers 20 bytes.
    uint32_t last_cycles;
    uint32_t peak_cycles;
    uint8_t  cycles_valid;
};

struct FwUpdateStatus {
    uint8_t  bootloader_version;   // 0 = this unit has no bootloader
    uint8_t  state;                // FW_STATE_*
    uint8_t  attempts;             // commit attempts against the staged image
    uint8_t  last_result;          // FW_RESULT_* — why the last commit failed
    uint32_t staged_size;
    uint32_t staged_crc32;
    uint16_t running_major;
    uint16_t running_minor;
    uint16_t running_patch;
    uint16_t running_store_version; // FLASH_STORE_VERSION of the running image
    uint32_t app_base;
    uint32_t staging_capacity;     // largest image this unit can accept
    uint8_t  staged_valid;
    uint8_t  reserved[3];
};

// The FLASH_STORE_VERSION this build of the configurator speaks. Must equal the
// firmware's; test_firmware_link asserts the two are the same, because it is the
// one place that sees both headers.
//
// It exists so a Send can say "this device's firmware is older than this app"
// instead of letting the device answer "invalid length". The record sizes ARE
// version-checked on the wire — the device tests length == 4 + count*item_size
// and NACKs cleanly, which is what stops a mismatched write being MISREAD — but
// a clean NACK is only half the job. ERR_INVALID_LEN reaching a user as "invalid
// length" tells them nothing about what to do, and the answer ("update the
// firmware") is not something they can guess from it. v20 grew
// CanMessageConfig 10 -> 14 and made that gap real.
//
// 2.3.0 retired the per-message key WITHOUT moving this. `key[4]` became
// `reserved[4]` in place, so the record is still 14 bytes and no stored offset
// moves — a 2.2.1 unit keeps its configuration across the firmware update, and
// this build talks to 2.2.1 and 2.3.0 devices identically. See flash_store.h.
//
// 7: the CAN diagnostic device channels. DeviceChannelsConfig went 2 -> 62
// bytes and it lives in the config HEADER, so every record offset after it
// shifts — the hazard the version field exists for, and unlike the 2.3.0 case
// above it is not avoidable by keeping a size. A unit updated to this firmware
// reads its stored configuration as absent and runs bus defaults until one is
// sent again; the Send/Get paths already say so.
//
// 8: the transmit-CRC8 table. Appended after the script chunks, which moves no
// existing table's offset — but FLASH_NUM_TABLES grows the header's per-table
// counts array, which moves the record area wholesale. Same consequence as 7.
//
// 9: the MCU health device channels. DEVCH_COUNT 31 -> 36 grows
// DeviceChannelsConfig 62 -> 72 bytes, and that struct sits in the config
// HEADER — the v7 hazard exactly: ten extra bytes shove every table offset
// along, so a v8 image would be misread wholesale and is refused by version
// instead. Same consequence as ever: one re-Send after the update.
//
// 10: MAX_CONDITIONS 100 -> 250. Conditions are the fourth table, so the 6,000
// extra bytes shift counters, timers, constants, relays, the lookup tables,
// integrators, the script region and the CRC8 rules all down — a v9 image would
// be misread record-for-record. Triggered transmit shipped in the same release
// and needed no bump of its own: it claimed three retired bytes inside
// CanMessageConfig in place, so the record is still 14 bytes and nothing moved
// on its account. Same consequence as ever: one re-Send after the update.
// 11: the condition record grew a second expression (35 -> 56 bytes) and
// ConditionTerm shrank 10 -> 8 to pay for it. Both store hazards at once —
// the record size changed AND every table after conditions shifted — so a
// v10 image would be misread twice over. v10 was built but never released.
constexpr uint16_t EXPECTED_STORE_VERSION = 17;

#pragma pack(pop)

static_assert(sizeof(CanMessageConfig) == 14, "must match firmware");
static_assert(sizeof(CanSignalConfig) == 64, "must match firmware"); // 32+20+8+4

// --- packed-field accessors -------------------------------------------------
// These MIRROR the static inline versions in firmware/include/protocol.h. The
// bit layout is the wire format, so the two implementations must agree exactly;
// test_firmware_link includes both headers and cross-checks them.
constexpr uint16_t SIG_MSG_IDX_MASK   = 0x1FF; // 0..510; 511 == SIG_MSG_NONE
constexpr uint32_t SIG_START_BIT_MASK = 0x1FF; // 0..511 (64-byte FD frame)
constexpr uint32_t SIG_BITLEN_MASK    = 0x3F;  // stored as length-1, so 1..64
constexpr uint32_t SIG_VALTYPE_MASK   = 0x3F;  // SignalDataType 0x01..0x38
constexpr uint32_t SIG_DECIMALS_MASK  = 0xF;   // 0..8
constexpr uint32_t SIG_MUXOFF_MASK    = 0x3F;  // 0..63
// msg_and_flags bit 11: on TRANSMIT, send the low bit_length bits of the
// converted value rather than clamping it to what the field can hold. Clear
// is the old behaviour AND the default, which is why the bit means WRAP and
// not CLAMP — every record written before it existed has a zero there and
// keeps clamping exactly as it did. Receive ignores it. See the firmware's
// protocol.h for why wrapping skips the channel-range clamp as well.
constexpr uint16_t SIG_FLAG_TX_WRAP   = 0x0800;
// msg_and_flags bit 12: declares its compound identifier and packs nothing, so
// a transmit variant with no channels of its own still reaches the wire. The
// device infers a compound message's variants by walking its signals, so an
// identifier with nothing bound to it used to infer nothing and never went out.
// See the firmware's protocol.h.
constexpr uint16_t SIG_FLAG_SELECTOR_ONLY = 0x1000;

inline uint16_t sigMsgIdx(const CanSignalConfig &s)
{
    const uint16_t v = uint16_t(s.msg_and_flags & SIG_MSG_IDX_MASK);
    return v == SIG_MSG_IDX_MASK ? SIG_MSG_NONE : v;
}
inline uint8_t sigByteOrder(const CanSignalConfig &s) { return uint8_t((s.msg_and_flags >> 9) & 1); }
inline uint8_t sigIsActive(const CanSignalConfig &s)  { return uint8_t((s.msg_and_flags >> 10) & 1); }
inline bool sigTxWrap(const CanSignalConfig &s) { return (s.msg_and_flags & SIG_FLAG_TX_WRAP) != 0; }
inline bool sigSelectorOnly(const CanSignalConfig &s)
{ return (s.msg_and_flags & SIG_FLAG_SELECTOR_ONLY) != 0; }
inline uint16_t sigStartBit(const CanSignalConfig &s) { return uint16_t(s.bits & SIG_START_BIT_MASK); }
inline uint8_t sigBitLength(const CanSignalConfig &s)
{ return uint8_t(((s.bits >> 9) & SIG_BITLEN_MASK) + 1); }
inline uint8_t sigValueType(const CanSignalConfig &s)
{ return uint8_t((s.bits >> 15) & SIG_VALTYPE_MASK); }
inline int8_t sigDecimalPlaces(const CanSignalConfig &s)
{ return int8_t((s.bits >> 21) & SIG_DECIMALS_MASK); }
inline uint8_t sigMuxByteOffset(const CanSignalConfig &s)
{ return uint8_t((s.bits >> 25) & SIG_MUXOFF_MASK); }

inline void sigSetHeader(CanSignalConfig &s, uint16_t msgIdx, uint8_t byteOrder, uint8_t isActive)
{
    const uint16_t m = msgIdx == SIG_MSG_NONE ? SIG_MSG_IDX_MASK
                                              : uint16_t(msgIdx & SIG_MSG_IDX_MASK);
    // Preserves the rest of the word so sigSetTxWrap may be called on either
    // side of this — mirroring the firmware, where assigning the whole word
    // made the two setters order-dependent.
    s.msg_and_flags = uint16_t((s.msg_and_flags & uint16_t(~0x07FF)) | m
                               | uint16_t((byteOrder & 1) << 9) | uint16_t((isActive & 1) << 10));
}
inline void sigSetTxWrap(CanSignalConfig &s, bool wrap)
{
    s.msg_and_flags = uint16_t(wrap ? (s.msg_and_flags | SIG_FLAG_TX_WRAP)
                                    : (s.msg_and_flags & uint16_t(~SIG_FLAG_TX_WRAP)));
}
inline void sigSetSelectorOnly(CanSignalConfig &s, bool on)
{
    s.msg_and_flags = uint16_t(on ? (s.msg_and_flags | SIG_FLAG_SELECTOR_ONLY)
                                  : (s.msg_and_flags & uint16_t(~SIG_FLAG_SELECTOR_ONLY)));
}
inline void sigSetBits(CanSignalConfig &s, uint16_t startBit, uint8_t bitLength, uint8_t valueType,
                       int8_t decimals, uint8_t muxByteOffset)
{
    const uint32_t len = uint32_t((bitLength ? bitLength : 1) - 1) & SIG_BITLEN_MASK;
    const uint32_t dp = uint32_t(decimals < 0 ? 0 : decimals) & SIG_DECIMALS_MASK;
    s.bits = (uint32_t(startBit) & SIG_START_BIT_MASK) | (len << 9)
             | ((uint32_t(valueType) & SIG_VALTYPE_MASK) << 15) | (dp << 21)
             | ((uint32_t(muxByteOffset) & SIG_MUXOFF_MASK) << 25);
}

// Single-field updates, for the paths that stamp a type onto an already-built
// virtual slot without disturbing the rest of the packed word.
inline void sigSetValueType(CanSignalConfig &s, uint8_t valueType)
{
    s.bits = (s.bits & ~(SIG_VALTYPE_MASK << 15))
             | ((uint32_t(valueType) & SIG_VALTYPE_MASK) << 15);
}
inline void sigSetBitLength(CanSignalConfig &s, uint8_t bitLength)
{
    const uint32_t len = uint32_t((bitLength ? bitLength : 1) - 1) & SIG_BITLEN_MASK;
    s.bits = (s.bits & ~(SIG_BITLEN_MASK << 9)) | (len << 9);
}
inline void sigSetDecimalPlaces(CanSignalConfig &s, int8_t decimals)
{
    const uint32_t dp = uint32_t(decimals < 0 ? 0 : decimals) & SIG_DECIMALS_MASK;
    s.bits = (s.bits & ~(SIG_DECIMALS_MASK << 21)) | (dp << 21);
}
inline void sigSetMsgIdx(CanSignalConfig &s, uint16_t msgIdx)
{
    const uint16_t m = msgIdx == SIG_MSG_NONE ? SIG_MSG_IDX_MASK
                                              : uint16_t(msgIdx & SIG_MSG_IDX_MASK);
    s.msg_and_flags = uint16_t((s.msg_and_flags & ~SIG_MSG_IDX_MASK) | m);
}

static_assert(sizeof(MathConfig) == 24, "must match firmware");
static_assert(sizeof(ConditionTerm) == 8, "must match firmware");
static_assert(sizeof(ConditionConfig) == 62, "must match firmware");
static_assert(sizeof(DeviceStatus) == 39, "must match firmware");
static_assert(sizeof(InjectCanPayload) == 71, "must match firmware");
static_assert(sizeof(MonitorStreamPayload) == 76, "must match firmware");
// The wire length is built from this, so it has to BE the offset of data[]
// rather than agree with it by inspection: a field added ahead of data[] without
// moving this would truncate every payload by the size of the new field, and the
// result would still parse.
static_assert(offsetof(MonitorStreamPayload, data) == MONITOR_HEADER_BYTES,
              "MONITOR_HEADER_BYTES must equal offsetof(MonitorStreamPayload, data)");
static_assert(sizeof(SignalValueEntry) == 6, "must match firmware");
static_assert(sizeof(CounterConfig) == 32, "must match firmware");
static_assert(sizeof(DeviceChannelsConfig) == 72, "must match firmware"); // 36 * 2
static_assert(DEVCH_ONTIME == 0, "Device OnTime must stay at offset 0 — see above");
static_assert(sizeof(TimerConfig) == 32, "must match firmware");
static_assert(sizeof(ConstantConfig) == 7, "must match firmware");
static_assert(sizeof(RelayConfig) == 11, "must match firmware");
static_assert(sizeof(Table2x16Def) == 70, "must match firmware");
static_assert(sizeof(Table2x16Out) == 64, "must match firmware");
// 6 indices + 3 counts + 8*4 X sites + 8*4 Y sites = 73 (padded slot 80), and
// 8*4 = 32 (padded slot 32 — the contiguity the row split exists for).
static_assert(sizeof(Table8x8Def) == 73, "must match firmware");
static_assert(sizeof(Table8x8GridRow) == 32, "must match firmware Table8x8Row");
static_assert(sizeof(IntegratorConfig) == 30, "must match firmware");
static_assert(sizeof(Crc8Config) == 40, "must match firmware");
static_assert(sizeof(ControlCanPayload) == 11, "must match firmware");
static_assert(sizeof(FwUpdateBeginPayload) == 16, "must match firmware");
static_assert(sizeof(FwUpdateStatus) == 32, "must match firmware");
static_assert(sizeof(ScriptChunk) == 64, "must match firmware");
static_assert(sizeof(ScriptStatus) == 29, "must match firmware");

// Host->device wire frame limit. This was 127 for years because the firmware's
// v1 UART RX DMA mangled bursts of 128 bytes or more (FIRMWARE-NOTES.md #5).
// That bug is FIXED, and the 127 outlived it — every Send was paying four times
// the round trips for a hardware defect that no longer existed. Raising it is
// not a one-line change though, and #5 is the authority on what moves with it:
// the firmware's rxBuffer goes 256 -> 1024 in main.c (a frame bigger than that
// buffer can be lapped before the callback copies it out; 1024 is >= 2 frames of
// slack, 512 would be exactly one and is not enough), while MAX_RAW_PACKET, the
// decode/accumulate buffers and the 2 KB serial ring were already 2048 and need
// nothing. 512 rather than 2048 here because the win is nearly all in the first
// step — 7 signals per frame instead of 2 — and a smaller ceiling keeps a single
// corrupted frame cheap to retransmit.
constexpr int MAX_TX_WIRE_BYTES = 512;
// 512 wire - 2 delimiters - COBS overhead (1 code byte + 1 per 254 raw bytes)
// - 4 header - 2 CRC  =>  payload cap used for chunking. 496 keeps the encoded
// worst case at 506, which is what the assertion below actually proves:
constexpr int MAX_TX_PAYLOAD = 496;

// Prove the cap actually holds, at compile time, rather than trusting the
// arithmetic in the comment above. A frame is:
//     1 delimiter + COBS(4 header + payload + 2 CRC) + 1 delimiter
// and COBS adds at most one code byte per block plus one per 254 bytes. Every
// WRITE_CHUNK_* below is sized against MAX_TX_PAYLOAD, so this one assertion
// covers all of them — and it fires at build time if someone raises the cap
// without re-checking, which is the only moment the mistake is cheap. Getting
// this wrong does not look like a size error at run time: the firmware's RX DMA
// mangles the burst and the device answers ERR_INVALID_CRC.
constexpr int kMaxRawFrameBytes = 4 + MAX_TX_PAYLOAD + 2;
constexpr int kMaxCobsFrameBytes =
    kMaxRawFrameBytes + 1 + kMaxRawFrameBytes / 254 + 2 /* both delimiters */;
static_assert(kMaxCobsFrameBytes <= MAX_TX_WIRE_BYTES,
              "a full-size frame would exceed the firmware's safe RX burst");

// Write chunk sizes (items per WRITE_* command): the largest n satisfying
// 4 + n*item_size <= MAX_TX_PAYLOAD, the 4 being the start/count prefix. Every
// one of these was recomputed when the cap went 112 -> 496; the "next n would
// be" figure is written out so the next record-size change can be checked
// against it without redoing the division.
constexpr int WRITE_CHUNK_MESSAGES   = 35; // 4 + 35*14 = 494 (36 -> 508 > 496).
                                           // Was 49 while the record was 10 bytes;
                                           // v20 added the per-message key.
constexpr int WRITE_CHUNK_SIGNALS    = 7;  // 4 + 7*64  = 452 (64 B signal; 8 -> 516).
                                           // 2/frame at the old 112-byte cap, so a
                                           // full-table Send is 3.5x fewer round trips
constexpr int WRITE_CHUNK_MATH       = 20; // 4 + 20*24 = 484 (21 -> 508)
// 14 -> 8 with the record's growth to 56 bytes: 4 + 8*56 = 452, where 9 would
// be 508 and overrun MAX_TX_PAYLOAD. Not a free choice, and the device would
// NACK ERR_INVALID_LEN rather than misread it — but a Send that cannot deliver
// a condition table is not a failure mode worth discovering in the field.
constexpr int WRITE_CHUNK_CONDITIONS = 7;  // 4 + 7*62 = 438 (8 -> 500), store v14
constexpr int WRITE_CHUNK_COUNTERS   = 15; // 4 + 15*32 = 484 (16 -> 516), store v15
constexpr int WRITE_CHUNK_TIMERS     = 15; // 4 + 15*32 = 484 (16 -> 516), store v12
constexpr int WRITE_CHUNK_CONSTANTS  = 70; // 4 + 70*7  = 494 (71 -> 501)
constexpr int WRITE_CHUNK_RELAYS     = 44; // 4 + 44*11 = 488 (45 -> 499)
constexpr int WRITE_CHUNK_TABLES_2X16_DEF = 7; // 4 + 7*70 = 494 (8 -> 564)
constexpr int WRITE_CHUNK_TABLES_2X16_OUT = 7; // 4 + 7*64 = 452 (8 -> 516)
constexpr int WRITE_CHUNK_TABLES_8X8_DEF  = 6; // 4 + 6*73 = 442 (7 -> 515)
constexpr int WRITE_CHUNK_TABLES_8X8_ROW  = 15; // 4 + 15*32 = 484 (16 -> 516).
                                           // Rows are NOT grouped by table: the
                                           // range is flat 0..63 and a chunk may
                                           // straddle two tables, which is fine
                                           // because a table only goes live when
                                           // its Def lands and that is sent last
constexpr int WRITE_CHUNK_INTEGRATORS = 16; // 4 + 16*30 = 484 (17 -> 514)
constexpr int WRITE_CHUNK_SCRIPT      = 7;  // 4 + 7*64  = 452 (8 -> 516)

// Read chunk sizes (items per READ_* request) bounded by the firmware's
// ~2030-byte response cap (FIRMWARE-NOTES: TX COBS buffer overflow margin).
// MAX_RESPONSE_PAYLOAD did NOT move with the host->device cap — the device's TX
// path was never the constrained one — so these are 4 + n*item_size <= 2030,
// further capped by the table's own capacity where a whole table fits in one
// request.
constexpr int READ_CHUNK_MESSAGES   = 125; // 4 + 125*14 = 1754; the cap allows 144,
                                           // 125 splits MAX_MESSAGES exactly 4 ways.
                                           // Was 200 while the record was 10 bytes;
                                           // v20 added the per-message key.
constexpr int READ_CHUNK_SIGNALS    = 31;  // 4 + 31*64  = 1988 (64 B signal;
                                           // 32 -> 2052 > 2030)
constexpr int READ_CHUNK_MATH       = 84;  // 4 + 84*24  = 2020 (24 B with operand C)
// 50 -> 36 for the same reason: 4 + 36*56 = 2020 against the device's
// MAX_RESPONSE_PAYLOAD of 2030, where 37 would be 2076 and be refused.
constexpr int READ_CHUNK_CONDITIONS = 32;  // 4 + 32*62 = 1988 (33 -> 2050), store v14
constexpr int READ_CHUNK_COUNTERS   = 50;  // 4 + 50*31  = 1554 (the whole table)
constexpr int READ_CHUNK_TIMERS     = 50;  // 4 + 50*20  = 1004 (the whole table now
                                           // that MAX_TIMERS is 50)
constexpr int READ_CHUNK_CONSTANTS  = 100; // 4 + 100*7  = 704 (the whole table)
constexpr int READ_CHUNK_RELAYS     = 32;  // 4 + 32*11  = 356 (the whole table)
constexpr int READ_CHUNK_TABLES_2X16_DEF = 8; // 4 + 8*70 = 564
constexpr int READ_CHUNK_TABLES_2X16_OUT = 8; // 4 + 8*64 = 516
constexpr int READ_CHUNK_TABLES_8X8_DEF  = 8; // 4 + 8*73 = 588 (the whole table)
constexpr int READ_CHUNK_TABLES_8X8_ROW  = 32; // 4 + 32*32 = 1028. All 64 rows would
                                           // be 2052 > 2030, so this is two even
                                           // halves rather than the arithmetic
                                           // maximum of 63 plus a stray 1
constexpr int READ_CHUNK_INTEGRATORS = 8;     // 4 + 8*30 = 244
constexpr int READ_CHUNK_SCRIPT      = 31;    // 4 + 31*64 = 1988 (32 -> 2052 > 2030)

} // namespace ct
