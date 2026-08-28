/*
 * protocol.h — CAN Triple serial protocol, version 1.
 *
 * The version numbering was RESET to 1 here. Nothing has shipped, so the v2-v19
 * ladder recorded the development history of a product no customer has ever
 * held — and a version number whose only readers are two halves of the same
 * repository, rebuilt together, is a liability rather than a compatibility
 * guarantee. v1 is the first protocol anyone outside this repo will meet.
 *
 * The .ct3 FILE schema was deliberately NOT reset with it: configuration files
 * written by earlier builds exist on disk, and renumbering would make the
 * "saved by a newer version" guard refuse them. A file format's version answers
 * to the files in the world, not to the product's release history.
 *
 * Still v1 ON THE WIRE: the capacity revision raises MAX_MESSAGES 250 -> 500,
 * MAX_SIGNALS 768 -> 1000, MAX_TIMERS 20 -> 50 and the signal label 16 -> 32
 * bytes (CanSignalConfig 48 -> 64), and replaces the 4x4 lookup table with an
 * 8x8 one. No PROTOCOL_VERSION bump, by the v14 rule: the generic write handler
 * checks length == 4 + count*item_size, so a 48-byte-record host NACKs cleanly
 * against a 64-byte device rather than being misread. FLASH_STORE_VERSION does
 * go 3 -> 4, because unlike the wire the stored LAYOUT really moves — every
 * table offset after messages shifts, so a v3 image would be misread
 * record-for-record rather than merely failing its CRC (see flash_store.h).
 *
 * What paid for the bigger records is the host->device payload cap going
 * 112 -> 496 bytes (MAX_TX_PAYLOAD / MAX_TX_WIRE_BYTES in the GUI's
 * wire_structs.h). That cap only ever existed because of the v1 DMA fault in
 * FIRMWARE-NOTES #5, which the circular-DMA rework fixed; the note stayed stale
 * long enough to cost a debugging session, so read it before assuming 112 is
 * load-bearing. FW main.c's rxBuffer grows 256 -> 1024 alongside it.
 *
 * Still v1 ON THE WIRE: the Advanced Math revision appended MathOp values 9-30
 * (unary, comparison and logic ops, plus five three-operand ops) and grew
 * MathConfig 18 -> 24 bytes to carry the C operand. No PROTOCOL_VERSION bump,
 * and deliberately so — the v14 rule applies: the generic write handler checks
 * length == 4 + count*item_size, so an 18-byte-record host NACKs cleanly
 * against a 24-byte device (and vice versa), which is everything a bump would
 * buy while no older build is deployed. FLASH_STORE_VERSION did go 1 -> 2,
 * because the boot CRC spans item_size bytes per stored record (see the note
 * on MathConfig below and in flash_store.h). The C operand's byte layout is
 * documented on MathConfig below.
 *
 * v19 replaces the single configuration password with THREE per-function access
 * keys, exposed in the GUI as "Online > Set Access Passwords": one for sending a
 * configuration, one for getting one back, one for revealing and editing
 * protected communications. Each is a 4-byte key the host folds out of a typed
 * password (PBKDF2, fixed application salt) and proves by challenge-response;
 * the device stores the keys write-only and gates the matching commands on them.
 * Commands 0x25-0x28 (the v18 lock) are RETIRED and not reused.
 *
 * v19 also adds the update identity block (0x2F-0x31): vendor / product /
 * series / config version plus a series key the device can prove it holds. It
 * exists so a host can decide whether an update belongs on a unit WITHOUT
 * reading its configuration — the whole point of shipping a locked config to a
 * customer who must still be able to take updates.
 *
 * Note the wire version skips 18: the v18 revision changed FLASH_STORE_VERSION
 * only (it added the lock record to the flash header) and left PROTOCOL_VERSION
 * at 17. Moving straight to 19 keeps "v19" meaning one thing across the
 * firmware, the GUI and the docs rather than three near-miss numbers.
 *
 * v17 makes integrators bidirectional and retainable. MAX_INTEGRATORS goes
 * 4 -> 8, IntegratorConfig gains `start_value` (seeded into the output slot
 * when the config loads) and two flags: INTEGFLAG_COUNT_DOWN subtracts instead
 * of adding, and INTEGFLAG_PRESERVE retains the total across power cycles via
 * the same ring the counters use. A "decrementor" is therefore just an
 * integrator with COUNT_DOWN set and start_value at its peak — the engine path
 * is one loop, not two. The record grew 26 -> 30 bytes, which costs NOTHING in
 * flash: the padded slot was already 32. Preserve keys now namespace both
 * tables (see PRESERVE_KEY_INTEGRATOR_BASE in engine_core.h).
 * v16 added an Integrators table (0x23/0x24): accumulators that add an input to
 * their output channel at a configurable rate — `out += input`, rate_hz times
 * per second. This is RAW accumulation, not a time integral: the rate scales
 * how fast the output climbs, which is exactly what makes it worth configuring.
 * Each has an optional enable gate, an edge-triggered reset to a set value, and
 * min/max clamping.
 * v15 shrinks CanSignalConfig 72 -> 48 bytes and raises MAX_SIGNALS 500 -> 768.
 * The signal table was 69% of the whole config image, so this is where the only
 * meaningful capacity was. See the struct for the three changes (label 32 -> 16,
 * the small fields bit-packed behind accessors, mux_id/mux_mask 32 -> 16 bits)
 * — the label half of that has since been REVERSED by the capacity revision
 * above, which bought its 32 bytes back with a bigger flash region instead of
 * with shorter channel names; the bit-packing and the 16-bit mux fields stand —
 * and note the one functional loss: a compound-message selector now reads a
 * 2-byte window instead of 4, so a multiplexor field must sit within 16 bits of
 * its byte offset. WRITE_CHUNK_SIGNALS doubles from 1 to 2 records per frame as
 * a side effect, halving the round trips a full Send spends on signals.
 *
 * v14 widens a Condition from ONE comparison to up to COND_MAX_TERMS (3),
 * joined by AND/OR. ConditionConfig becomes ConditionTerm terms[3] (10 B each)
 * plus dest_signal_idx / term_count / joiners / is_active, growing 13 -> 35 B;
 * `joiners` packs one ConditionJoin bit per gap. The fold is STRICTLY LEFT TO
 * RIGHT — ((t0 J0 t1) J1 t2) — not C's "&& binds tighter than ||", because the
 * editor prints that bracketing and the two must agree. Commands 0x08/0x09 are
 * deliberately REUSED here (unlike v13's retired 0x1B/0x1C): the record size
 * changed, and the generic write handler checks length == 4 + count*item_size,
 * so a v13 host's 4+13k-byte payload can never satisfy a v14 device's 4+35k —
 * the mismatch NACKs cleanly instead of being misread.
 * v13 widens the v12 "2x8" lookup table to "2x16" (16 axis sites instead of 8).
 * A 16-site table needs 134 bytes, which could not cross the 112-byte
 * host->device payload cap OF THE TIME (MAX_TX_PAYLOAD is 496 now; the split
 * still stands on MAX_PADDED_RECORD, see Table2x16Def), so the record is SPLIT
 * INTO TWO PARALLEL TABLES sharing an index:
 *   Table2x16Def (70 B, cmds 0x1F/0x20) — axis input, destination, flags, site
 *                                         count and the 16 axis breakpoints
 *   Table2x16Out (64 B, cmds 0x21/0x22) — the 16 output values
 * Table t is evaluated only when BOTH tables hold a record at index t, so an
 * interrupted upload (Def written, Out not) never evaluates against
 * un-programmed flash. The v12 2x8 commands 0x1B/0x1C and Table2x8Config are
 * RETIRED — those IDs are deliberately not reused, so a v12 host talking to v13
 * firmware gets a clean ERR_INVALID_CMD instead of having its 70-byte 2x8 record
 * silently misread as a 70-byte 2x16 definition. The 4x4 table was unchanged
 * here; the capacity revision above later retired it the same way, for the 8x8.
 * v12 added lookup Tables (Calculations > Tables): 8 "2x8" tables (one input
 * axis, 8 sites -> output) and 8 "4x4" tables (X and Y axes, 4 sites each ->
 * 16-cell output), each writing an interpolated/discrete value into a generated
 * channel every evaluation pass. Each axis is Interpolated (linear; bilinear for
 * 4x4 when both interpolate) or Discrete-centered (holds a site's value, switches
 * at the midpoint between sites); inputs clamp to the end sites. Commands
 * 0x1D/0x1E (4x4) survived here; the 2x8 pair was replaced in v13, and 0x1D/0x1E
 * were themselves retired by the capacity revision above when the 8x8 replaced
 * the 4x4. The axis semantics are the part that outlived both records.
 * v11 adds a Message Relay table (0x19/0x1A): masked-ID gateway rules checked
 * on every received frame, independent of the message table. A relay forwards
 * a frame whole to the selected buses (never back to the source) when
 * (arbitration_id & bitmask) == (address & bitmask) among frames of its
 * extended-ness; RELAYFLAG_INVERT forwards the non-matching frames instead.
 * v10 adds compound (multiplexed) TRANSMIT. A transmit message with mux-gated
 * signals is composed one variant frame per identifier (offset/id/mask): the
 * always-present signals plus that identifier's signals, with the identifier's
 * selector value written into the frame. MSGFLAG_TX_SEQUENTIAL picks the cadence
 * — clear = Batch (every variant each period), set = Sequential (one variant per
 * period, round-robin). No struct sizes changed.
 * v9 adds a per-bus termination-resistor control: ControlCanPayload carries a
 * `termination` byte (0/1) applied by CONTROL_CAN and persisted in the flash
 * header alongside mode/baud, so a terminated bus stays terminated across
 * reboots. ControlCanPayload grew 10 -> 11 bytes.
 * v8 adds first-class compound (multiplexed) messages. Each signal carries an
 * optional mux selector (mux_byte_offset / mux_id / mux_mask); when mux_mask is
 * non-zero the signal is only extracted from a received frame while
 * (selector & mux_mask) == (mux_id & mux_mask), where selector is the up-to-
 * 4-byte little-endian window of the frame starting at mux_byte_offset. A
 * mux_mask of 0 means "always active" (ordinary signals). This lets one CAN ID
 * carry several sub-messages selected by a multiplexor field, matching the GUI's
 * compound-section model. CanSignalConfig grew 63 -> 72 bytes.
 * v7 adds a 32-byte configuration name (0x16/0x17), stored in the flash header
 * alongside the config it names. (v7 also moved the config tables into a
 * flash-resident image so they no longer occupy RAM — that is internal to the
 * firmware and does not change the wire format.)
 * v6 adds a Constants table (0x14/0x15): a calculation that writes
 * a fixed value to a generated channel every evaluation pass. Each entry names
 * a destination signal slot and the constant float; the engine applies them
 * before math so downstream calculations see them.
 * v5 reframes conditions as pure boolean logic channels: a condition
 * evaluates "A op B" and drives a boolean output channel (dest_signal_idx)
 * true/false, replacing the v2-v4 action model (block/force routing, set
 * value, mute bus). ConditionConfig shrank 18 -> 13 bytes.
 * v4 adds receive-timeout defaults: a receive message reuses its (otherwise
 * unused) period_ms field as a receive timeout in milliseconds, and each
 * signal carries a default_value applied to its value slot when the parent
 * message has not been received within that timeout (0 = feature off).
 * v3 unifies receive and transmit into ONE message table (direction is a flag
 * on each message; transmit messages carry a period), and adds up/down
 * counters (0x10/0x11) and timers (0x12/0x13) as calculations.
 * v2 added CONTROL_CAN / STREAM_VALUES and framed all device output (telemetry
 * + logs); those are unchanged.
 *
 * Wire format (both directions):
 *   0x00  COBS( [0x55][cmd][len u16 LE][payload...][crc16 hi][crc16 lo] )  0x00
 * CRC16-CCITT-FALSE (poly 0x1021, init 0xFFFF) over header+payload,
 * appended big-endian. All other multi-byte fields little-endian.
 */
#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>

#include "fleet_identity.h"

#define PROTOCOL_VERSION 1

/* Defined here rather than with the rest of the access-key block far below,
 * because v20 put a key of this width inside CanMessageConfig and a struct
 * cannot use a macro that has not been seen yet. The block below documents what
 * four bytes does and does not buy; this is only the width. */
#define ACCESS_KEY_LEN            4u

/* Command IDs (host -> device) */
#define CMD_GET_STATUS          0x01
#define CMD_WRITE_MSG_CFG       0x02
#define CMD_READ_MSG_CFG        0x03
#define CMD_WRITE_SIG_CFG       0x04
#define CMD_READ_SIG_CFG        0x05
#define CMD_WRITE_MATH_CFG      0x06
#define CMD_READ_MATH_CFG       0x07
#define CMD_WRITE_COND_CFG      0x08
#define CMD_READ_COND_CFG       0x09
/* Commits the header, making whatever is in the tables the stored configuration.
 * Payload is OPTIONAL: two bytes carry the configuration's version number, which
 * lands in the header alongside the tables it belongs to. Empty payload leaves
 * the stored version alone.
 *
 * The version rides the commit rather than having a command of its own so the
 * two cannot separate — a version that landed without its configuration, or a
 * configuration that landed without its version, would each make the "is this
 * update newer?" check lie in a different direction. */
#define CMD_SAVE_TO_FLASH       0x0A
/* 0x0B retired (was LOAD_FROM_FLASH): it reloaded a stored BACKUP image over
 * the live tables, and the flash-resident single-copy store removed the
 * backup copy it read from. The boot path still loads the store
 * (engine_load_config); no wire command re-runs it. Do not reuse. */
/* Gated by ACCESS_FN_SEND, and by nothing else. There is deliberately no
 * per-message gate here: ALL THREE protection tiers permit removal, which is
 * the stated spec, so a clear can never be refused for a protection reason. See
 * MSGPROT_MASK. 2.2.x did refuse a clear while a keyed message was stored, and
 * the cost of that was a unit whose per-message password had been lost could
 * not be cleared OR reconfigured short of a store-invalidating reflash. */
#define CMD_CLEAR_CONFIG        0x0C
#define CMD_CONTROL_CAN         0x0D  /* implemented in v2 */
#define CMD_INJECT_CAN_FRAME    0x0E
#define CMD_STREAM_VALUES       0x0F  /* implemented in v2: payload u8 mask */
#define CMD_WRITE_COUNTER_CFG   0x10  /* v3: up/down counter table */
#define CMD_READ_COUNTER_CFG    0x11
#define CMD_WRITE_TIMER_CFG     0x12  /* v3: timer table */
#define CMD_READ_TIMER_CFG      0x13
#define CMD_WRITE_CONST_CFG     0x14  /* v6: constants table */
#define CMD_READ_CONST_CFG      0x15
#define CMD_WRITE_CONFIG_NAME   0x16  /* v7: 32-byte configuration name */
#define CMD_READ_CONFIG_NAME    0x17
#define CMD_RESET_DEVICE        0x18  /* v7: ACK, then reboot the MCU */
#define CMD_WRITE_RELAY_CFG     0x19  /* v11: message relay (masked forward) table */
#define CMD_READ_RELAY_CFG      0x1A
/* 0x1B/0x1C were the v12 2x8 lookup table. RETIRED in v13 and deliberately NOT
 * reused: the v13 definition record is also 70 bytes, so honouring the old IDs
 * would let a v12 host's 2x8 record pass the length check and be misread. */
/* 0x1D/0x1E were the v12 4x4 lookup table. RETIRED by the capacity revision and
 * deliberately NOT reused: the 8x8 that replaced it is a split Def+Row pair with
 * its own IDs (0x34-0x37 below), so honouring the old ones could only ever mean
 * accepting a 105-byte 4x4 record into a table that no longer exists. A host
 * still holding 4x4 tables gets ERR_INVALID_CMD, which is the honest answer —
 * its records must be migrated into 8x8s host-side, not half-landed here. */
#define CMD_WRITE_TABLE2X16_DEF 0x1F  /* v13: 2x16 axis/dest/flags/count + sites */
#define CMD_READ_TABLE2X16_DEF  0x20
#define CMD_WRITE_TABLE2X16_OUT 0x21  /* v13: 2x16 output values */
#define CMD_READ_TABLE2X16_OUT  0x22
#define CMD_WRITE_INTEG_CFG     0x23  /* v16: integrator (rate accumulator) table */
#define CMD_READ_INTEG_CFG      0x24
/* 0x25-0x28 were the v18 single configuration password (READ/WRITE_CONFIG_LOCK,
 * LOCK_CHALLENGE, LOCK_RESPONSE). RETIRED in v19 in favour of the per-function
 * access keys below, and deliberately NOT reused: a v18 host would otherwise get
 * a plausible-looking answer from a device that no longer means the same thing
 * by it, and would conclude the device is unprotected. */
/* v18: device binding. GET_DEVICE_ID answers the 96-bit unique chip ID plus a
 * byte saying why the stored configuration is or is not running — so a host can
 * tell "this device has no config" from "this config belongs to another
 * device", which otherwise look identical. Always answerable: it carries no
 * configuration detail, and a host that cannot ask has no way to diagnose a
 * device that has gone inert. */
#define CMD_GET_DEVICE_ID       0x29  /* -> uid[CONFIG_UID_LEN] + u8 config status */
#define CMD_WRITE_CONFIG_BINDING 0x2A /* uid[CONFIG_UID_LEN]; all zero = unbound */
/* v19: per-function access keys — the three passwords a host must prove before
 * the device will do the matching thing. READ_ACCESS_KEYS answers WHICH keys are
 * set and never what they are; the keys themselves are write-only, exactly as
 * the v18 verifier was.
 *
 * ACCESS_CHALLENGE / ACCESS_RESPONSE prove one function at a time, so a session
 * that has earned the right to Send has not thereby earned the right to Get. */
#define CMD_READ_ACCESS_KEYS    0x2B  /* -> u8 set_mask (bit per ACCESS_FN_*) */
#define CMD_WRITE_ACCESS_KEYS   0x2C  /* AccessKeyWritePayload; needs the old key proved */
#define CMD_ACCESS_CHALLENGE    0x2D  /* -> ACCESS_CHALLENGE_LEN random bytes */
#define CMD_ACCESS_RESPONSE     0x2E  /* u8 function + HMAC-SHA256(key, challenge) */
/* 0x40 was v20's CMD_MSG_ACCESS_RESPONSE — prove ONE message's own key. RETIRED
 * in 2.3.0 with the per-message keys themselves, and NEVER REUSED.
 *
 * It must continue to fall through to `default: sendNack(ERR_INVALID_CMD)`, and
 * that is load-bearing rather than tidy. Shipped 2.2.x Managers still send this
 * before every Send. ERR_INVALID_CMD maps to wrongPassword=false in the host's
 * device_session, which the old unlockDeviceMessagesForSend reads as "this
 * message has no key", drops the index, and proceeds silently — so an old
 * Manager talking to a 2.3.0 device just works.
 *
 * If this id ever answered ERR_LOCKED instead, the same host sets
 * wrongPassword=true and every shipped Manager enters an unescapable password
 * prompt loop against a password that no longer exists anywhere. That is the
 * failure mode; it is why 0x40 gets a retirement note and not a new meaning.
 * Same reasoning as the 0x1B-0x1E and 0x25-0x28 retirements above. */
/* Fleet identity. Answerable unconditionally and by design — its whole job is to
 * let a host decide whether an update belongs on this device WITHOUT reading the
 * configuration, which is the one question a locked-down unit still has to be
 * able to answer. It carries no protocol detail: a vendor string, a model
 * string, a serial, a series, and a key that never leaves the chip.
 *
 * There is no WRITE. The identity is compiled into the firmware (see
 * fleet_identity.h), so re-badging a unit means building and flashing it, not
 * sending it a packet. That is the point: an identity no command can change is
 * one no attacker can change either, and one no flash erase can lose. */
#define CMD_READ_FLEET_ID       0x2F  /* -> FleetIdentityPublic (no fleet_key) */
/* The one exchange that runs the other way: the HOST sends a challenge and the
 * DEVICE answers HMAC-SHA256(fleet_key, challenge). Reading the identity says
 * what the device CLAIMS to be; this says it actually holds the series secret,
 * which is what stops a look-alike from collecting an update meant for someone
 * else's fleet. */
#define CMD_FLEET_ID_PROVE      0x31  /* challenge[ACCESS_CHALLENGE_LEN] -> HMAC-SHA256 */
/* Read back the three buses' mode / rate / FD rate / termination.
 *
 * CONTROL_CAN has always been write-only, so a Get could recover every message
 * and channel and then have to GUESS what the buses were running — the host
 * assumed the bring-up rates and warned the user to check. A configuration that
 * comes back subtly different from the one that went out is a bad answer to
 * "what is on this device", and the guess was the only part of a Get that was
 * not simply true.
 *
 * Answers the LIVE setup — what the buses are actually running right now, which
 * is what CONTROL_CAN last applied. After a boot that equals the stored image;
 * mid-session, after a Send that has not been saved, it does not, and live is
 * the honest answer to what the device is doing. Gated on ACCESS_FN_GET like
 * every other read of configuration content: a bus map is protocol detail.
 *
 * (0x30 was WRITE_UPDATE_ID before the identity moved into the firmware build.
 * Reused rather than retired: this is protocol v1 and nothing is deployed, so
 * no host anywhere holds an older meaning for it.) */
#define CMD_READ_CAN_SETUP      0x30  /* -> ControlCanPayload[3], bus 1..3 in order */

/* Device channels: values the DEVICE itself produces, as opposed to everything
 * else in a configuration, which describes what the device should do with
 * values the host or the bus supplies. There is one so far — Device OnTime —
 * and the payload is deliberately a struct rather than a bare index so a second
 * one costs a field here and not a third command id.
 *
 * The host does not send a value, only a destination: which signal slot the
 * firmware should publish OnTime into. That is why this is configuration and
 * rides with SAVE_TO_FLASH like the rest of the header, rather than being
 * telemetry. A slot of SIG_MSG_NONE means the configuration does not use the
 * channel and the firmware writes nothing.
 *
 * Gated like every other table: WRITE on ACCESS_FN_SEND, READ on ACCESS_FN_GET.
 * Old firmware NACKs both with ERR_INVALID_CMD, which the host treats as "this
 * device cannot offer device channels" and carries on. */
#define CMD_WRITE_DEVICE_CHANNELS 0x32
#define CMD_READ_DEVICE_CHANNELS  0x33

/* The 8x8 lookup table, which replaces the 4x4 (0x1D/0x1E, retired above). Fresh
 * IDs at the top of the allocated range — 0x34 was the next free one, 0x01-0x33
 * being either live or retired — rather than the pair the 4x4 gave up, for the
 * v13 reason: an ID that changes meaning is worse than one that stops answering.
 *
 * Two commands per direction because the record is SPLIT, exactly as the 2x16 is
 * (0x1F-0x22): a combined 8x8 would be 73 + 256 = 329 bytes, which the raised
 * payload cap could carry but MAX_PADDED_RECORD cannot store. Def carries the
 * axes and the flags; each Row carries one grid row of 8 floats, and table t
 * owns rows t*8 .. t*8+7. See Table8x8Def below for why rows and no other
 * chunking. */
#define CMD_WRITE_TABLE8X8_DEF  0x34
#define CMD_READ_TABLE8X8_DEF   0x35
#define CMD_WRITE_TABLE8X8_ROW  0x36
#define CMD_READ_TABLE8X8_ROW   0x37

/* Transmit CRC8: a rule that stamps a CRC-8 into one byte of a transmit
 * message's frame, computed over a configurable sequence of ID bytes, frame
 * bytes and literals — AFTER every other byte of the frame is final, which is
 * the only order under which a checksum means anything. See Crc8Config.
 *
 * The ids live past the script block: 0x38..0x3F are all spoken for below
 * (firmware update + scripts), and a duplicate id is not a compile error —
 * it silently reroutes whichever command the dispatch switch tests first.
 * 0x40 is retired ground and skipped: it was CMD_MSG_ACCESS_RESPONSE, and
 * shipped 2.2.x Managers still send it before every Send, reading the
 * ERR_INVALID_CMD answer as "keyless" — see the retirement note in the GUI's
 * wire_structs.h. So the first ids actually free are these. */
#define CMD_WRITE_CRC8_CFG      0x41
#define CMD_READ_CRC8_CFG       0x42
/* v16: the four document-wide message passwords. 0x43/0x44 rather than reusing
 * 0x40, which is retired ground and stays retired — see the note above it.
 *
 * READ RETURNS THE KEYS THEMSELVES, unlike CMD_READ_ACCESS_KEYS which answers a
 * set mask and nothing more. The difference is what each protects: an access key
 * gates THIS DEVICE, so letting one out would hand over the gate. These gate
 * nothing here — they are the lock on a marking the configurator enforces — and
 * a configuration read back without them has marked messages nobody can open,
 * edit or release. Returning them is the whole reason they are stored. */
#define CMD_WRITE_MSG_PASSWORDS 0x43
#define CMD_READ_MSG_PASSWORDS  0x44

/* Firmware update. See FwUpdateBeginPayload near the bottom of this file for
 * the sequence and for why the staging slot lives in bank 2.
 *
 * BEGIN erases as much of the staging slot as the declared image needs — a
 * flash operation of several hundred milliseconds, so the host must allow the
 * long flash timeout for it rather than the default. DATA and END are quick.
 * STATUS is a READ: its reply echoes the command and carries FwUpdateStatus,
 * which means it has to be listed in the GUI's DeviceLink::isReadResponse().
 * Forgetting that is a mistake this protocol has now made five times; the
 * symptom is every STATUS timing out. */
#define CMD_FW_UPDATE_BEGIN     0x38
#define CMD_FW_UPDATE_DATA      0x39
#define CMD_FW_UPDATE_END       0x3A
#define CMD_FW_UPDATE_STATUS    0x3B
#define CMD_FW_UPDATE_ABORT     0x3C

/* Device scripts (script_vm.h). The bytecode rides in the configuration as an
 * ordinary table of 64-byte chunks, so it Sends, Gets, Verifies, Saves and
 * Clears through the machinery every other table already uses — WRITE/READ_
 * SCRIPT are plain range commands over ENGINE_TABLE_SCRIPT.
 *
 * SCRIPT_STATUS is a READ (its reply echoes the command and carries
 * ScriptStatus), so it must appear in the GUI's DeviceLink::isReadResponse().
 * That list records five commands that learned this the hard way. */
#define CMD_WRITE_SCRIPT        0x3D
#define CMD_READ_SCRIPT         0x3E
#define CMD_SCRIPT_STATUS       0x3F

/* Response / telemetry IDs (device -> host) */
/* ACK/NACK payload: [status, req_crc_hi, req_crc_lo] — 3 bytes.
 *
 * status is the byte it always was: ERR_OK for an ACK, the error code for a
 * NACK. The two bytes after it echo the CRC16 of the REQUEST being answered
 * (big-endian, same convention as the frame's own CRC), so the host can tell a
 * genuine ACK from a duplicate left behind by a retransmit: a stale ACK carries
 * the previous command's CRC and is discarded rather than completing the wrong
 * command. The one exception is the two "bad frame" NACKs (ERR_INVALID_CRC,
 * ERR_INVALID_LEN): the frame did not authenticate, so there is no request CRC
 * to echo and the field is 0 — the host matches those by code, not CRC.
 *
 * The field is ADDITIVE, not versioned: a host that predates it reads the first
 * byte and ignores the rest, and the field carries no meaning that host needs.
 * A host that predates it entirely — one that hard-checks a 1-byte ACK — will
 * not talk to this firmware, but nothing shipped at protocol version 1 did, so
 * there is nothing in the field to break. */
#define CMD_ACK                 0x80  /* [ERR_OK, req_crc_hi, req_crc_lo] */
#define CMD_NACK                0x81  /* [error, req_crc_hi, req_crc_lo] */
#define CMD_MONITOR_STREAM      0x82
#define CMD_VALUE_STREAM        0x83
#define CMD_LOG                 0x90  /* v2: framed ASCII log text */

/* NACK error codes */
#define ERR_OK                  0x00
#define ERR_INVALID_CMD         0x01
#define ERR_INVALID_LEN         0x02
#define ERR_INVALID_CRC         0x03
#define ERR_OUT_OF_BOUNDS       0x04
#define ERR_FLASH_WRITE         0x05
#define ERR_BUS_BUSY            0x06
/* v19: this command needs an access password that the session has not proved.
 * Which one is implied by what was asked — a read NACKs against ACCESS_FN_GET,
 * a write (and a clear, and a commit) against ACCESS_FN_SEND — so the host can
 * name the right password in its prompt without the device having to say which
 * key it tripped over.
 *
 * 2.3.0: there are now exactly TWO keys this code can mean. The third clause
 * this comment used to carry — "a protected-comms operation against
 * ACCESS_FN_EDIT_COMMS" — is gone with the per-message write gate. Nothing on
 * the device is refused for a message-protection reason any more, so a host
 * that sees ERR_LOCKED on a message write must prompt for Send and never for
 * Edit Protected Comms. See MSGPROT_MASK. */
#define ERR_LOCKED              0x07

/* v2 bootloader: a firmware image was refused. Deliberately ONE code rather
 * than one per reason — the reasons are FW_RESULT_* in fw_image.h, there are
 * ten of them, and burning ten NACK codes on a single command would crowd a
 * space every other command shares. The host reads CMD_FW_UPDATE_STATUS to
 * find out which, and that reply also carries the bootloader's verdict on the
 * LAST commit, which no NACK could have told it. */
#define ERR_FW_REJECTED         0x08

/* CMD_STREAM_VALUES payload bits */
#define STREAM_ENABLE_VALUES    0x01
#define STREAM_ENABLE_MONITOR   0x02

/* Table capacities */
/* receive AND transmit share this table.
 *
 * 500 IS THE PRACTICAL CEILING ON THIS AXIS, and the binding constraint is not
 * flash — it is SIG_MSG_IDX_MASK below. A signal names its parent message in a
 * NINE-BIT packed field, so the representable indices are 0..510 with 511
 * reserved as SIG_MSG_NONE (the "virtual signal" marker). 500 fits with 11 to
 * spare; 512 would NOT, and would not fail loudly either — sig_set_header masks
 * the index, so message 512 would silently become message 0 and every signal
 * bound to it would be parsed out of the wrong frame. Going past 510 means
 * widening the field, which means moving byte layout the GUI mirrors. */
/* v16: HOW MANY MESSAGE PASSWORDS A CONFIGURATION HAS. Four, document-wide,
 * referenced by every marked message rather than carried by each one.
 *
 * Four keys plus a set mask is 17 bytes, and they ride in the config header
 * where 52 bytes were free — so the marking costs the record region NOTHING. A
 * key per message needed its own table: 100 sparse 8-byte slots, 800 B of the
 * 1,504 the region had spare, and a per-message field would have cost 4,000. */
#define MSG_PASSWORD_SLOTS      4

/* The four keys themselves, as they ride in the config header. A slot holding
 * kNoAccessKey (0) is empty; deriveAccessKey never folds a real password to 0,
 * so no separate set-mask is needed and 16 bytes is the whole cost.
 *
 * UNLIKE AccessKeyRecord, THESE COME BACK OUT. The device's own access keys are
 * write-only on purpose — CMD_READ_ACCESS_KEYS answers which are set and never
 * what they are — because they gate the device itself. These gate nothing here:
 * they are the lock on a marking the CONFIGURATOR enforces, and a configuration
 * read back without them is a configuration whose marked messages nobody can
 * open, edit or release. Returning them is the entire point. */
typedef struct {
    uint32_t key[MSG_PASSWORD_SLOTS];
} MessagePasswordRecord; /* 4 * 4 = 16 bytes */

#define MAX_MESSAGES            500
/* v15 got 768 out of a 52 KB region with a 48-byte record. The capacity
 * revision spends flash instead of bytes: the region is 96 KB now and the record
 * is back to 64 (the label is 32 again), so 1000 signals cost 64,000 B of a
 * region with ~11 KB still spare. Round number on purpose — the old 768 was the
 * largest that fitted, which made it look like a protocol constant rather than
 * the arithmetic result it was. (The hard maximum is asserted in flash_store.c:
 * CFG_TOTAL <= FLASH_STORE_CAPACITY is what actually binds this.) */
#define MAX_SIGNALS             1000
#define MAX_MATH_COMPUTATIONS   100
/* 100 -> 250. PAD8(35) is 40, so the table costs 10,000 B where it cost 4,000,
 * and CFG_TOTAL lands at 126,368 of 131,072 — 4,704 B still spare. That is the
 * whole budget story; the assert in flash_store.c is what actually enforces it.
 *
 * The raise is NOT free the way the triggered-transmit fields were: conditions
 * are the 4th table in FLASH_TABLE_LIST, so every table after them shifts and
 * FLASH_STORE_VERSION goes 9 -> 10. A unit updated to this firmware reads its
 * stored configuration as absent and needs one re-Send — the same price v6, v7,
 * v8 and v9 each charged, paid once here for both changes in this release.
 *
 * 250 also spends 250 of the 1,000 MAX_SIGNALS slots if every condition is used,
 * because each one owns an output slot. That is the real ceiling on going
 * further: another 250 signals would cost 16,000 B against 4,704 B spare. */
#define MAX_CONDITIONS          200
#define MAX_COUNTERS            50
/* 20 -> 50, matching MAX_COUNTERS: 50 * PAD8(20) = 1,200 B of flash, up from
 * 480. Timers were the one calculation table an ordinary configuration could
 * exhaust, and there was never a reason for them to be the small one. */
#define MAX_TIMERS              50
#define MAX_CONSTANTS           100   /* v6 */
#define MAX_RELAYS              32    /* v11: message relay (masked forward) rules */
#define MAX_TABLES_2X16         8     /* v13: 1-axis 16-site lookup tables */
#define TABLE_2X16_SITES        16    /* v13: axis sites / outputs per 2x16 table */
/* The 8x8 replaces the v12 4x4 (MAX_TABLES_4X4, removed with Table4x4Config and
 * commands 0x1D/0x1E). Same eight tables, 64 cells each instead of 16. The Row
 * table's capacity is MAX_TABLES_8X8 * TABLE_8X8_SITES = 64 records, because a
 * table owns one Row record per Y site. */
#define MAX_TABLES_8X8          8     /* 2-axis 8x8 lookup tables */
#define TABLE_8X8_SITES         8     /* axis sites per side; the grid is 8 x 8 */
#define MAX_INTEGRATORS         8     /* v16: rate accumulators; v17: 4 -> 8, up or down */
#define MAX_CRC8_MESSAGES       20    /* transmit-CRC8 rules, one per stamped message */
/* Device script bytecode (script_vm.h), carried as a table of fixed 64-byte
 * chunks. 512 chunks = 32,768 bytes, the script region the flash map reserved
 * at map v2 — and CFG_TOTAL lands at 119,568 of 131,072, keeping the same
 * ~11 KB slack the store had before.
 *
 * 64 is not arbitrary: PAD8(64) == 64, so consecutive chunk slots are
 * BYTE-CONTIGUOUS in flash, and the VM takes ONE pointer at chunk 0 and treats
 * the whole script as a flat array. No reassembly buffer, no RAM copy — the
 * same property the 8x8 Row table depends on. A chunk size that padded would
 * scatter the bytecode into fragments with holes between them.
 *
 * The script's own length lives in its ScriptHeader; the table's used count is
 * simply how many chunks that image occupies, so a partially uploaded script
 * fails its CRC rather than executing a truncated tail. */
#define MAX_SCRIPT_CHUNKS       512
#define SCRIPT_CHUNK_BYTES      64
/* The engine ticks at 100 Hz, so one step per tick is the ceiling: an
 * integrator cannot add more often than the evaluation pass runs. */
#define INTEGRATOR_MAX_HZ       100
#define CONFIG_NAME_LEN         32    /* v7: configuration name (Save/SaveAs base name) */
/* Signal label field, 31 chars + NUL. Use this everywhere rather than a bare 32
 * or an incidental sizeof: the host's channel-name length check, the DBC import
 * truncation and this array have to agree, and the two times they have not, a
 * legal name was silently shortened at one end of the wire. Note the check that
 * matters is on UTF-8 BYTES, not characters — a 20-character name with accented
 * letters can exceed 31 bytes and must be rejected on the byte count. */
#define SIGNAL_LABEL_LEN        32

#define PROTO_START_MARKER      0x55

/* CanMessageConfig.flags bits */
#define MSGFLAG_EXTENDED        0x01
#define MSGFLAG_FD              0x02
#define MSGFLAG_ROUTING         0x04  /* receive only: gateway this message */
#define MSGFLAG_ACTIVE          0x08
#define MSGFLAG_TRANSMIT        0x10  /* v3: compose+transmit periodically (else receive) */
#define MSGFLAG_TX_SEQUENTIAL   0x20  /* v10: compound tx cadence — set = one variant per
                                       * period (round-robin), clear = all variants each period */

/* CanMessageConfig.tx_trigger_flags bits — Triggered transmit.
 *
 * These are a SEPARATE byte rather than two more MSGFLAG_* bits because there
 * are no MSGFLAG_* bits left: 0x01..0x20 are taken and 0x40/0x80 are the
 * MSGPROT_* level, whose values are pinned (see MSGPROT_MASK) and cannot be
 * borrowed. The byte comes out of the retired per-message key, so it is free.
 *
 * A transmit message with TXTRIG_ENABLED clear is CYCLIC — it transmits every
 * period_ms as it always has, and tx_trigger_cond is not read. */
#define TXTRIG_ENABLED          0x01  /* transmit only while the named condition holds */
/* 0x02 was TXTRIG_RESET_ON_TX — "Reset User Condition once Triggered", which
 * forced the named condition's output to 0 after a transmission so the message
 * fired once per rising edge. RETIRED before it ever shipped, and removed
 * rather than reserved because nothing in the field has ever seen it.
 *
 * It went because User Conditions grew a Reset expression, which does the same
 * job explicitly and better. "Send once when the request arrives" is now a
 * Set/Reset condition set on Message Received and reset on Message Transmitted:
 * the reset is visible in the editor beside the thing it resets, it can name a
 * DIFFERENT message than the one it gates, and it does not reach across from a
 * transmit message to quietly rewrite a calculation's output. Two ways to do
 * one thing, where one of them acted at a distance, was not worth carrying. */
/* tx_trigger_cond when no condition is named. Not a magic "disabled" value —
 * TXTRIG_ENABLED is what decides that — but a marker that survives a round trip
 * so a Get cannot invent condition 0 out of an unset field. */
#define TX_TRIGGER_COND_NONE    0xFFFFu
/* 2.3.0: bits 6-7 are ONE ORDERED PROTECTION LEVEL, not two independent flags.
 * level = (flags & MSGPROT_MASK) >> 6, and that number is the tier directly:
 *
 *   0  None       ordinary message
 *   1  Read Only  viewable, not editable
 *   2  Hidden     not viewable, not editable
 *   3  Protected  Hidden, plus the untick costs Edit Protected Comms proved
 *                 against a connected device
 *
 * THE DEVICE ENFORCES NONE OF THIS. All three tiers are conventions of the
 * Device Manager, and the bits are on the wire for ROUND-TRIP FIDELITY ONLY: a
 * Get followed by a Send must not launder a Hidden message into an ordinary
 * one. Nothing in serial_proto.c reads these bits, the engine never did, and
 * reads are ungated — anyone with this header and a serial cable gets the whole
 * protocol back. That is a documented weak point, not an oversight; see the
 * long note on the table-read path in serial_proto.c.
 *
 * The VALUES are not free choices. This is the only assignment under which the
 * two bit patterns that exist in shipped 2.2.x flash decode to the right new
 * tier, which is what lets FLASH_STORE_VERSION stay at 6:
 *
 *   0x80  2.2.x "Read-only"             -> HIDDEN.  2.2.x CONCEALED these in the
 *                                         Manager, so decoding 0x80 as the new
 *                                         Read Only — which is VISIBLE — would
 *                                         print the CAN ID, frame layout and
 *                                         every channel's bit position of every
 *                                         message an existing unit marked, on
 *                                         the first Get, with no indication.
 *                                         Over-restricting annoys; under-
 *                                         restricting leaks.
 *   0xC0  2.2.x "Protect Communication" -> PROTECTED. Exact match.
 *   0x40  never emitted by 2.2.x        -> READ ONLY. Free for the new tier
 *                                         precisely because nothing shipped
 *                                         could produce it: the old
 *                                         MSGFLAG_PROTECTED was only ever sent
 *                                         together with MSGFLAG_READONLY.
 *
 * MSGFLAG_READONLY and MSGFLAG_PROTECTED are DELETED as names, not renamed.
 * Keeping them would be actively dangerous: 0x40 alone now means the WEAKEST
 * tier, the exact inverse of what the name MSGFLAG_PROTECTED asserts, and code
 * that tested `flags & MSGFLAG_PROTECTED` would read Protected as true for
 * Read Only and false for Hidden. */
#define MSGPROT_MASK            0xC0
#define MSGPROT_NONE            0x00
#define MSGPROT_READONLY        0x40
#define MSGPROT_HIDDEN          0x80
#define MSGPROT_PROTECTED       0xC0

/* RelayConfig.flags bits (v11) */
#define RELAYFLAG_EXTENDED      0x01  /* acts on extended frames (else standard) */
#define RELAYFLAG_INVERT        0x02  /* forward the NON-matching frames instead */
#define RELAYFLAG_ACTIVE        0x04
/* 2.3.0: bits 6-7 of a relay's flags carry the SAME MSGPROT_* level, with the
 * same values and the same meaning — see the MSGPROT_ block above. Free here
 * because RELAYFLAG_* only ever used bits 0-2.
 *
 * Not symmetry for its own sake: it closes a real gap. A relay section marked
 * in the Manager concealed in the GUI and then reached the device carrying
 * nothing, so a Get read it back as ordinary and the next Save wrote the
 * concealment away. Relays are transported, not enforced, exactly like
 * messages. */

/* Table2x16Def / Table8x8Def flags bits (v12, inherited unchanged by the 8x8
 * from the 4x4 it replaces). An axis "interpolate" bit set = linear
 * interpolation; clear = discrete-centered (hold the site value, switch at the
 * midpoint between sites).
 *
 * TABLEFLAG_ACTIVE lives on the DEF record of a split table, never on the Row /
 * Out records, which carry no flags of their own. That is deliberate: the Def is
 * what the host writes LAST, so the bit that switches a table on cannot be set
 * while half its values are still un-programmed flash. */
#define TABLEFLAG_ACTIVE        0x01
#define TABLEFLAG_X_INTERP      0x02
#define TABLEFLAG_Y_INTERP      0x04  /* two-axis tables (8x8) only */

/* MonitorStreamPayload.flags bits (raw-frame monitor stream) */
#define MONFLAG_EXTENDED        0x01
#define MONFLAG_FD              0x02
#define MONFLAG_BRS             0x04  /* CAN FD bit-rate switch used */
#define MONFLAG_ESI             0x08  /* CAN FD error-state indicator (passive) */
/* One or more monitor frames were LOST between the previous frame and this one.
 *
 * The monitor stream is best-effort and always was: it rides the same UART as
 * everything else, and a bus busier than the link can describe will overrun it.
 * What it must not do is overrun QUIETLY. A trace that silently omits half the
 * traffic is worse than one with visible holes, because the holes are the only
 * thing that stops somebody concluding a message was never sent when in fact it
 * was never reported — and that conclusion is exactly what a monitor is used to
 * reach.
 *
 * Set on the first frame that gets through after any loss, so the gap is
 * attributed to the point in the trace where it happened. It carries no count:
 * knowing frames are missing HERE is what a reader needs, and a count would
 * cost a wire-format change that older hosts could not parse. */
#define MONFLAG_GAP             0x10

/* CanSignalConfig.msg_idx: 0..499 = the message this signal belongs to
 * (parsed if that message is receive, packed if it is transmit); 0xFFFF =
 * virtual signal (math/counter/timer destination or condition target). */
#define SIG_MSG_NONE            0xFFFF

#pragma pack(push, 1)

typedef struct {
    uint8_t start_marker;   /* 0x55 */
    uint8_t cmd;
    uint16_t length;        /* payload length, little-endian */
} PacketHeader;

typedef struct {
    uint32_t can_id;
    uint8_t flags;          /* MSGFLAG_* in bits 0-5 — bit4 selects transmit vs
                             * receive. Bits 6-7 are the MSGPROT_* level, which
                             * nothing on the device reads. */
    uint8_t src_bus;        /* the bus: source for receive, target for transmit (1..3) */
    uint8_t route_bus_mask; /* receive only: bit0=CAN1 bit1=CAN2 bit2=CAN3 */
    uint8_t dlc;
    uint16_t period_ms;     /* transmit: send period >= 10 ms. receive (v4):
                             * receive timeout in ms — after this long without a
                             * frame the message's signals revert to their
                             * default_value; 0 = no timeout. */
    /* Triggered transmit. These three bytes are the first three of the retired
     * v20 per-message key, claimed in place at store v10 — see `reserved` below
     * for why claiming them costs nothing and why it is only safe now.
     *
     * A transmit message is CYCLIC unless TXTRIG_ENABLED is set, which is the
     * whole backward story: an all-zero field is exactly the behaviour every
     * message had before this existed. */
    uint16_t tx_trigger_cond;  /* index into the condition table whose boolean
                                * output gates this message. Only meaningful with
                                * TXTRIG_ENABLED; TX_TRIGGER_COND_NONE when unset. */
    uint8_t tx_trigger_flags;  /* TXTRIG_* */
    /* v16: WHICH OF THE DOCUMENT'S FOUR MESSAGE PASSWORDS GUARDS THIS MESSAGE.
     * 0 = none, 1..MSG_PASSWORD_SLOTS = a slot in the header's table. Three bits
     * of a byte that was already here.
     *
     * There is a symmetry worth noticing: this is the same byte that carried the
     * last octet of the v20 PER-MESSAGE key, retired in 2.3.0 because storing
     * live key material on the device made it a distribution channel for the
     * secret. What sits here now is a REFERENCE, not a secret — the four keys
     * themselves live once in the config header instead of once per message,
     * which is what makes a marking survive a Get without costing a table.
     *
     * The upper five bits stay zero and are still scrubbed on the way out. */
    uint8_t password_slot;     /* was: the last byte of the v20 per-message key; RETIRED
                                * in 2.3.0 and still retired. Always written zero,
                                * by the host and by this firmware, and zeroed
                                * again on the way out (see serial_proto.c).
                                *
                                * The other three bytes carried live PBKDF2 key
                                * material on a 2.2.1 unit updated in place, which
                                * is why the scrub existed and why claiming them
                                * had to wait: a stored image from that era is a
                                * store-v6 image, and this firmware is store v10,
                                * so it is refused at load and its bytes can no
                                * longer reach either the engine or a Get. The
                                * scrub survives on this byte alone, because a
                                * reserved field that is only USUALLY zero stops
                                * being reserved. */
} CanMessageConfig;         /* v20: 14 bytes (was 10). PROTOCOL_VERSION does NOT move —
                             * the generic write handler checks
                             * length == 4 + count*item_size, so a 10-byte-record host
                             * NACKs cleanly against a 14-byte device instead of being
                             * misread, which is the v14 rule. FLASH_STORE_VERSION DOES
                             * move (5 -> 6): every stored table after messages shifts,
                             * so an old image would be misread record-for-record rather
                             * than merely failing its CRC. See flash_store.h.
                             *
                             * 2.3.0: STILL 14. The key field became `reserved` in
                             * place rather than being removed, so no offset moves
                             * and FLASH_STORE_VERSION stays 6 — a shipped unit
                             * keeps its configuration across the update. See the
                             * field's own comment for why shrinking would cost
                             * something and save nothing.
                             *
                             * Triggered transmit: STILL 14, and that is the point.
                             * Three of the four retired key bytes became named
                             * fields IN PLACE, so item_size does not move, no
                             * table offset moves, CFG_TOTAL is unchanged and every
                             * chunk constant on both sides stays as it was. The
                             * feature costs zero flash. PROTOCOL_VERSION does not
                             * move either: the record is the same size, so an
                             * older host's write still satisfies the length check
                             * — and it writes zeros there, which decodes to
                             * "cyclic", the behaviour it intended. */

typedef enum {
    SIGNAL_TYPE_UINT8   = 0x01,
    SIGNAL_TYPE_UINT16  = 0x02,
    SIGNAL_TYPE_UINT32  = 0x04,
    SIGNAL_TYPE_INT8    = 0x11,
    SIGNAL_TYPE_INT16   = 0x12,
    SIGNAL_TYPE_INT32   = 0x14,
    SIGNAL_TYPE_FLOAT   = 0x34,
    SIGNAL_TYPE_DOUBLE  = 0x38
} SignalDataType;

/* v15: the signal record was 72 B and dominated the config image (500 x 72 =
 * 36 KB, 69% of it). It went to 48 B, so the same flash region held 778 signals
 * instead of 518. Three changes got there:
 *   - label 32 -> 16 bytes. The ENGINE never reads the label; it exists purely
 *     so a Get Configuration can reconstruct channel names, so it was 44% of
 *     the record doing no runtime work.
 *   - the eight small fields are bit-packed into msg_and_flags + bits.
 *   - mux_id/mux_mask narrow 32 -> 16 bits, so the compound-message selector
 *     window is 2 bytes rather than 4 (see muxSelected).
 * unit_type/unit_val are gone: they were a split little-endian u16 holding
 * "transmit source index + 1", which is now the honest tx_source field.
 *
 * The capacity revision took the LABEL back to 32 (SIGNAL_LABEL_LEN), and only
 * the label: 32 + 20 + 8 + 4 = 64 bytes, and PAD8(64) is 64, so the record still
 * wastes not one byte of its flash slot — the property that made 48 a good size,
 * at the next power of two up. What changed is v15's premise, not its
 * arithmetic: it was rationing a 52 KB region and 15-character channel names
 * were the price. The region is 96 KB now, so the trade is 16 KB of flash
 * (1000 x 16 B) for names long enough to say what a channel is. The bit-packing
 * and the 16-bit mux fields stand — those cost nothing legible.
 *
 * Bit fields are packed by hand rather than with C bitfields, whose layout is
 * ABI-defined rather than standard — the GUI mirrors this record in
 * wire_structs.h and the two MUST agree byte for byte. The accessors below are
 * the only sanctioned way to touch a packed field; test_firmware_link includes
 * both headers and cross-checks that the two implementations agree. */
typedef struct {
    char label[SIGNAL_LABEL_LEN]; /* 31 chars + NUL; host metadata, unread by the engine */
    float factor;
    float offset;
    float min_val;           /* clamp — always applied */
    float max_val;
    float default_value;     /* v4: physical value written to this signal's slot
                              * when its receive message times out (see
                              * CanMessageConfig.period_ms). */
    uint16_t mux_id;         /* v8/v15: compound (multiplexed) gating. When
                              * mux_mask is non-zero the signal is extracted
                              * from a frame only while
                              * (selector & mux_mask) == (mux_id & mux_mask),
                              * where selector is the up-to-2-byte little-endian
                              * window at data[mux_byte_offset..].
                              * mux_mask == 0 -> always active. */
    uint16_t mux_mask;
    uint16_t msg_and_flags;  /* msg_idx(9) | byte_order<<9 | is_active<<10
                              * | tx_wrap<<11 | selector_only<<12.
                              * Bits 13-15 are free. */
    uint16_t tx_source;      /* transmit source signal index + 1; 0 = own slot */
    uint32_t bits;           /* start_bit(9) | (bit_length-1)(6)<<9
                              * | value_type(6)<<15 | decimal_places(4)<<21
                              * | mux_byte_offset(6)<<25 */
} CanSignalConfig;           /* 32 + 20 + 8 + 4 = 64 bytes */

/* --- packed-field accessors (mirrored in the GUI's wire_structs.h) --------- */
#define SIG_MSG_IDX_BITS   9
/* 0..510; 511 == SIG_MSG_NONE. These nine bits are what caps MAX_MESSAGES at
 * 510, not flash — see the note on the #define. */
#define SIG_MSG_IDX_MASK   0x1FFu
#define SIG_START_BIT_MASK 0x1FFu   /* 0..511 — a 64-byte FD frame is 512 bits */
#define SIG_BITLEN_MASK    0x3Fu    /* stored as length-1, so 1..64 */
#define SIG_VALTYPE_MASK   0x3Fu    /* SignalDataType, 0x01..0x38 */
#define SIG_DECIMALS_MASK  0xFu     /* 0..8 */
#define SIG_MUXOFF_MASK    0x3Fu    /* 0..63 */

/* msg_and_flags bit 11: on TRANSMIT, send the low bit_length bits of the
 * converted value instead of clamping it to what the field can hold.
 *
 * Clear is the old behaviour and the default, which is why the bit means WRAP
 * rather than CLAMP: every configuration written before this bit existed has a
 * zero there, and every one of them keeps clamping exactly as it did. An older
 * host writing this record also writes zero, so it cannot accidentally ask for
 * wrapping it does not know about. The record does not change size, so
 * PROTOCOL_VERSION does not move (the v14 rule) and neither does any table
 * offset or chunk constant — the flag costs zero flash.
 *
 * Wrapping skips BOTH clamps in inverseSignalScaling, not just the field-width
 * one: the physical clamp is the CHANNEL's declared range, so a channel ranged
 * 0..255 would never present 256 to an 8-bit field to wrap in the first place.
 * "Do not clamp" has to mean the whole way out or it means nothing.
 *
 * Receive ignores this bit. A received field is bit_length bits wide by
 * construction, so there is nothing to wrap; the clamp on that side is the
 * channel's range and stays unconditional. */
#define SIG_FLAG_TX_WRAP   0x0800u

/* msg_and_flags bit 12: this signal DECLARES its compound identifier and packs
 * nothing. It exists so a transmit variant with no channels of its own still
 * reaches the wire.
 *
 * The device learns which variants a compound message has by walking its
 * SIGNALS (collectMuxIdentifiers) — the identifier list is not stored anywhere
 * else, and storing it per message would cost MAX_MESSAGES x MAX_TX_MUX_IDS
 * records for a list the signals already imply. The cost of implying it was
 * that an identifier with no channels implied nothing: a variant whose whole
 * content is its selector — a request or a ping frame, where the ID byte IS the
 * message — had no signal to be inferred from and never went out.
 *
 * One of these per such identifier fixes that for the price of a signal slot.
 * composeVariant skips it when packing, so the frame carries its selector over
 * a zeroed payload, which is exactly what was asked for. Clear on every record
 * ever written, and a signal that packs nothing is invisible to any reader that
 * ignores the bit, so nothing older misreads it. */
#define SIG_FLAG_SELECTOR_ONLY 0x1000u

static inline uint16_t sig_msg_idx(const CanSignalConfig *s)
{
    const uint16_t v = (uint16_t)(s->msg_and_flags & SIG_MSG_IDX_MASK);
    /* The 9-bit field's all-ones value is the wire's 0xFFFF "virtual" marker. */
    return (v == SIG_MSG_IDX_MASK) ? SIG_MSG_NONE : v;
}
static inline uint8_t sig_byte_order(const CanSignalConfig *s)
{ return (uint8_t)((s->msg_and_flags >> 9) & 1u); }
static inline uint8_t sig_is_active(const CanSignalConfig *s)
{ return (uint8_t)((s->msg_and_flags >> 10) & 1u); }
static inline uint8_t sig_tx_wrap(const CanSignalConfig *s)
{ return (uint8_t)((s->msg_and_flags & SIG_FLAG_TX_WRAP) ? 1u : 0u); }
static inline uint8_t sig_selector_only(const CanSignalConfig *s)
{ return (uint8_t)((s->msg_and_flags & SIG_FLAG_SELECTOR_ONLY) ? 1u : 0u); }
static inline uint16_t sig_start_bit(const CanSignalConfig *s)
{ return (uint16_t)(s->bits & SIG_START_BIT_MASK); }
static inline uint8_t sig_bit_length(const CanSignalConfig *s)
{ return (uint8_t)(((s->bits >> 9) & SIG_BITLEN_MASK) + 1u); }
static inline uint8_t sig_value_type(const CanSignalConfig *s)
{ return (uint8_t)((s->bits >> 15) & SIG_VALTYPE_MASK); }
static inline int8_t sig_decimal_places(const CanSignalConfig *s)
{ return (int8_t)((s->bits >> 21) & SIG_DECIMALS_MASK); }
static inline uint8_t sig_mux_byte_offset(const CanSignalConfig *s)
{ return (uint8_t)((s->bits >> 25) & SIG_MUXOFF_MASK); }

static inline void sig_set_header(CanSignalConfig *s, uint16_t msg_idx,
                                  uint8_t byte_order, uint8_t is_active)
{
    const uint16_t m = (msg_idx == SIG_MSG_NONE) ? (uint16_t)SIG_MSG_IDX_MASK
                                                 : (uint16_t)(msg_idx & SIG_MSG_IDX_MASK);
    /* Writes the three fields it names and PRESERVES the rest of the word, so
     * sig_set_tx_wrap can be called on either side of it. The alternative —
     * assigning the whole word — made the two setters order-dependent, which
     * is a silent wrong answer rather than a compile error. Every caller
     * starts from a zeroed record, so preserving costs nothing there. */
    s->msg_and_flags = (uint16_t)((s->msg_and_flags & (uint16_t)~0x07FFu) | m
                                  | ((uint16_t)(byte_order & 1u) << 9)
                                  | ((uint16_t)(is_active & 1u) << 10));
}
static inline void sig_set_tx_wrap(CanSignalConfig *s, uint8_t wrap)
{
    if (wrap)
        s->msg_and_flags |= (uint16_t)SIG_FLAG_TX_WRAP;
    else
        s->msg_and_flags &= (uint16_t)~SIG_FLAG_TX_WRAP;
}
static inline void sig_set_selector_only(CanSignalConfig *s, uint8_t on)
{
    if (on)
        s->msg_and_flags |= (uint16_t)SIG_FLAG_SELECTOR_ONLY;
    else
        s->msg_and_flags &= (uint16_t)~SIG_FLAG_SELECTOR_ONLY;
}
static inline void sig_set_bits(CanSignalConfig *s, uint16_t start_bit, uint8_t bit_length,
                                uint8_t value_type, int8_t decimal_places,
                                uint8_t mux_byte_offset)
{
    const uint32_t len = (uint32_t)((bit_length ? bit_length : 1u) - 1u) & SIG_BITLEN_MASK;
    const uint32_t dp = (uint32_t)(decimal_places < 0 ? 0 : decimal_places) & SIG_DECIMALS_MASK;
    s->bits = ((uint32_t)start_bit & SIG_START_BIT_MASK) | (len << 9)
              | (((uint32_t)value_type & SIG_VALTYPE_MASK) << 15) | (dp << 21)
              | (((uint32_t)mux_byte_offset & SIG_MUXOFF_MASK) << 25);
}

typedef enum {
    MATH_OP_ADD = 0, MATH_OP_SUB = 1, MATH_OP_MUL = 2, MATH_OP_DIV = 3,
    MATH_OP_SCALE = 4, /* result = A * B (kept for compatibility) */
    MATH_OP_MIN = 5, MATH_OP_MAX = 6, MATH_OP_AND = 7, MATH_OP_OR = 8,
    /* Advanced Math (still protocol v1 — see the header comment). Unary ops
     * read A alone; binary ops read A and B; the five ternary ops are why the
     * record grew a C operand. The GUI's op list is indexed by these values,
     * so the order here IS the order there. Guards follow DIV's example — an
     * undefined input yields 0, never a trap — and NaN is not scrubbed:
     * comparisons with a NaN operand take the false branch, per IEEE. */
    MATH_OP_ABS = 9,     /* |A| */
    MATH_OP_NEG = 10,    /* -A */
    MATH_OP_SQRT = 11,   /* A < 0 -> 0 (the DIV-by-0 guard, applied to the domain) */
    MATH_OP_FLOOR = 12,  /* floorf(A) */
    MATH_OP_CEIL = 13,   /* ceilf(A) */
    MATH_OP_ROUND = 14,  /* roundf(A) — halfway cases away from zero */
    MATH_OP_MOD = 15,    /* fmodf(A, B); B == 0 -> 0 */
    MATH_OP_XOR = 16,    /* bitwise, via the same saturating i32 cast as AND/OR */
    MATH_OP_LAND = 17,   /* (A > 0 && B > 0) ? 1 : 0 — boolean is "value > 0" */
    MATH_OP_LOR = 18,    /* (A > 0 || B > 0) ? 1 : 0 */
    MATH_OP_LNOT = 19,   /* A > 0 ? 0 : 1 (so LNOT(NaN) = 1) */
    MATH_OP_GT = 20,     /* A > B  ? 1 : 0 */
    MATH_OP_GE = 21,     /* A >= B ? 1 : 0 */
    MATH_OP_LT = 22,     /* A < B  ? 1 : 0 */
    MATH_OP_LE = 23,     /* A <= B ? 1 : 0 */
    /* EQ/NE are EXACT float compares — no epsilon, unlike a Condition's "=".
     * Fine against constants and booleans, treacherous against arithmetic. */
    MATH_OP_EQ = 24,     /* A == B ? 1 : 0 */
    MATH_OP_NE = 25,     /* A != B ? 1 : 0 */
    MATH_OP_MULADD = 26, /* A * B + C — scale with offset in one channel */
    MATH_OP_CLAMP = 27,  /* A held to [B, C]; C <= B disables (clampRoll convention) */
    MATH_OP_LERP = 28,   /* A + (B - A) * C */
    MATH_OP_SELECT = 29, /* A > 0 ? B : C */
    MATH_OP_WRAP = 30    /* A wrapped into [B, C) — clampRoll's modulo wrap */
} MathOp;

/* Advanced Math grew this record 18 -> 24 bytes: a third operand (C) for the
 * ternary ops (MULADD, CLAMP, LERP, SELECT, WRAP). Offsets 0-17 are unchanged;
 * the new bytes are:
 *
 *   18  u8     input_c_type   0 = const, 1 = signal
 *   19  u8[4]  input_c_val    type 0: the f32 constant, little-endian
 *                             type 1: u16 signal index in bytes [0..1], [2..3] zero
 *   23  u8     reserved0      write 0
 *
 * input_c_val is a raw byte array, NOT a union or a bitfield: the GUI mirrors
 * this record in wire_structs.h and the two must agree byte for byte, so the
 * packing is by hand for the same reason the v15 signal record's is. Assemble
 * and read it with memcpy / explicit byte arithmetic only.
 *
 * The flash slot was already PAD8(18) = 24, so the growth costs zero config
 * flash and no table moved. FLASH_STORE_VERSION still went 1 -> 2: imageCrc()
 * hashes item_size bytes per live record, so a v1 image holding math records
 * would fail the CRC under this build regardless (its header CRC covered 18
 * bytes per record, this build hashes 24 — the extra 6 being the 0xFF pad the
 * old write path left). That rejection is safe but config-dependent; the bump
 * makes it uniform. Bytes 18-23 of a legacy slot are 0xFF, which is why the
 * GUI's mapFromDevice normalizes C for ops that do not read it. */
typedef struct {
    uint8_t op;              /* MathOp */
    uint8_t input_a_type;    /* 0 = const, 1 = signal */
    uint16_t input_a_idx;
    float input_a_const;
    uint8_t input_b_type;
    uint16_t input_b_idx;
    float input_b_const;
    uint16_t dest_signal_idx;
    uint8_t is_active;
    uint8_t input_c_type;    /* 0 = const, 1 = signal */
    uint8_t input_c_val[4];  /* see the layout note above */
    uint8_t reserved0;       /* write 0 */
} MathConfig;                /* 18 + 6 = 24 bytes */

typedef enum {
    COND_OP_EQ = 0, COND_OP_NEQ = 1, COND_OP_LT = 2, COND_OP_LTE = 3,
    COND_OP_GT = 4, COND_OP_GTE = 5,
    /* The two MESSAGE operators. A term carrying one of these is not a
     * comparison at all: input_a_signal_idx holds a MESSAGE index instead of a
     * signal index, and input_b is unused and written zero.
     *
     * They are true only on the evaluation pass in which a frame actually
     * happened, which is what makes them usable as a Set — "the request
     * arrived" — and as a Reset — "the reply went out". A level ("is this
     * message alive") is a different question and the receive timeout already
     * answers it.
     *
     * The asymmetry between them is real and worth knowing. A received frame
     * sets its flag and runs executeConditions in the SAME call, so every frame
     * gets its own pass and none is ever missed. A transmitted frame is
     * recorded in the 200 Hz service, which does not evaluate conditions, so it
     * is seen by the NEXT pass — up to 10 ms later on a quiet bus — and two
     * transmissions of one message inside a single 10 ms window collapse into
     * one event. A 5 ms cyclic message, or a batch compound message emitting
     * every identifier in one service, will do exactly that. See
     * executeConditions in engine_core.c. */
    COND_OP_MSG_RX = 6,  /* a frame for this message was received this pass */
    COND_OP_MSG_TX = 7   /* a frame for this message was transmitted */
} ConditionOp;

/* Does this operator take a MESSAGE index in input_a rather than a signal? The
 * bounds check differs (MAX_MESSAGES, not MAX_SIGNALS), so every site that
 * validates a term has to ask. */
#define COND_OP_IS_MESSAGE(op) ((op) == COND_OP_MSG_RX || (op) == COND_OP_MSG_TX)

/* v14: how two comparisons are joined. */
typedef enum {
    COND_JOIN_AND = 0,
    COND_JOIN_OR  = 1
} ConditionJoin;

#define COND_MAX_TERMS 3

/* ConditionConfig.flags bits. */
#define CONDFLAG_ACTIVE     0x01
#define CONDFLAG_SETRESET   0x02  /* set = Set/Reset latch, clear = Momentary */

/* Momentary hold ceiling, in Hz. The hold is spent against elapsed_ms on the
 * calculation pass, which arrives every ENGINE_TICK_MS, so one tick is the
 * finest hold that exists and 100 Hz is the frequency whose period IS one tick.
 * A record asking for more is clamped rather than refused, for the reason
 * COUNTER_MAX_HZ gives: a condition running at a neighbouring rate is a better
 * failure than one that silently never fires. */
#define COND_LATCH_MAX_HZ   100u

/* One "A op B" comparison — or, for the message operators, one event test.
 *
 * B IS A UNION, and that is what pays for the second expression. The two
 * members were always mutually exclusive: input_b_type says which one is live,
 * and nothing has ever read the other. Overlapping them takes the term from 10
 * bytes to 8, which takes a six-term ConditionConfig from 72 bytes to 56 and is
 * the difference between 250 conditions fitting and not fitting.
 *
 * BOTH SIDES MUST ZERO THE WHOLE UNION BEFORE WRITING input_b_idx, because the
 * upper two bytes are otherwise whatever the writer left there and they travel
 * on the wire. Value-initialising the record does it; so does memset. */
typedef struct {
    uint16_t input_a_signal_idx; /* signal index — or MESSAGE index when
                                  * COND_OP_IS_MESSAGE(op) */
    uint8_t op;              /* ConditionOp */
    uint8_t input_b_type;    /* 0 = const, 1 = signal; ignored for message ops */
    union {
        uint16_t input_b_idx;
        float input_b_const;
    } b;
} ConditionTerm;             /* 2 + 1 + 1 + 4 = 8 bytes */

/* A User Condition drives a boolean output slot. It has TWO MODES, and the mode
 * is the difference between a shape and a level.
 *
 * The level it used to be is gone. Up to the modes revision a condition simply
 * published its expression: 1.0 while the comparisons held, 0.0 otherwise, with
 * no memory. That is still expressible — a Set/Reset whose Reset expression is
 * the logical inverse of its Set behaves identically, which is exactly what
 * every configuration written before this was migrated into — but it is no
 * longer the only thing a condition can be.
 *
 *   MOMENTARY (CONDFLAG_SETRESET clear)
 *     The RISING EDGE of the Set expression drives the output to 1, and it
 *     holds for one period of latch_hz — 10 Hz is 100 ms — then drops on its
 *     own. Retriggerable: a fresh rising edge while it is still high RELOADS
 *     the hold rather than topping it up, the same rule a counter's reset edge
 *     applies to its phase accumulator. The Reset expression is unused.
 *
 *     The hold is decremented by elapsed_ms on the CALCULATION pass only, so
 *     the achievable quantum is ENGINE_TICK_MS. COND_LATCH_MAX_HZ is 100 for
 *     that reason and no other, exactly as COUNTER_MAX_HZ and
 *     INTEGRATOR_MAX_HZ are.
 *
 *   SET / RESET (CONDFLAG_SETRESET set)
 *     A latch. The Set expression drives the output to 1, the Reset expression
 *     drives it to 0, and it HOLDS in between. RESET IS DOMINANT: with both
 *     expressions true the output is 0. That is the conventional safe bias and
 *     it is the one the timer stage already uses — a stop edge is applied after
 *     a start edge in executeTimers, so a simultaneous pair leaves the timer
 *     stopped. A Reset that means "stop" must not be defeatable by a Set that
 *     is stuck on. latch_hz is unused.
 *
 * BOTH expressions evaluate up to COND_MAX_TERMS comparisons joined by AND/OR,
 * STRICTLY LEFT TO RIGHT — ((t0 J0 t1) J1 t2) — deliberately NOT C's "&& binds
 * tighter than ||", because the editor shows the expression with that exact
 * bracketing and a config tool should read the way it looks. Every term is
 * evaluated (no short-circuit) so the pass takes constant time. The joiner bytes
 * hold one ConditionJoin BIT per gap: bit 0 joins term 0 to term 1, bit 1 joins
 * term 1 to term 2.
 *
 * Both modes are STATEFUL, and that is the one property this revision took away
 * from the engine. Evaluation used to be pure, which is why executeConditions
 * could run from the calculation tick AND from the receive path without caring
 * how often. It still runs from both, but it now carries memory — a latched
 * output, a previous-Set edge flag, a remaining hold — so it takes elapsed_ms
 * and the receive path passes 0. A received frame is an instant, not a duration.
 *
 * The runtime state lives in engine_core.c and NOT in this record, which stays
 * read-only flash: g_cond_state, g_cond_prev_set and g_cond_hold_ms. Nothing is
 * persisted, so a power cycle re-arms every condition. */
typedef struct {
    ConditionTerm set_terms[COND_MAX_TERMS];   /* 24 */
    ConditionTerm reset_terms[COND_MAX_TERMS]; /* 24 — Set/Reset mode only */
    /* v14: THE "FOR" QUALIFIER, ONE PER EXPRESSION, in CENTISECONDS. 0 disables
     * a side, and a condition with both at 0 behaves exactly as every condition
     * did before v13.
     *
     * That expression must be CONTINUOUSLY true for this long before it counts
     * as true at all. "X > 5 for 5 seconds" is one condition; it used to be a
     * timer accumulating how long X had been high plus a condition comparing
     * that timer against 5.
     *
     * SET AND RESET GET THEIR OWN, which v13 did not allow. v13 argued Reset
     * should stay immediate because Reset is dominant and a latch that cannot
     * be cleared promptly is the wrong way for one to fail. That argument is
     * still true and is why reset_qualify_cs DEFAULTS TO 0 — but it is a reason
     * to make delay opt-in, not a reason to make it impossible, and "clear only
     * after the fault has been gone a while" is a real requirement.
     *
     * CENTISECONDS, NOT MILLISECONDS, and that is what makes the second pair
     * free. Two u32 durations plus two masks is a 66-byte record, PAD8 rounds
     * the slot to 72, and 200 of those need 1,600 B the region has not got. Two
     * u16 plus two masks is 62, the slot stays 64, and the table does not move
     * at all. The cost is a ceiling of 655.35 s — ten minutes fifty-five — at
     * 10 ms resolution, which IS the calculation tick, so nothing is lost below
     * and anything above is a timer's job. */
    uint16_t set_qualify_cs;
    uint16_t reset_qualify_cs;
    /* WHICH comparisons each qualifier applies to — bit i for term i of that
     * expression.
     *
     * 0 means the WHOLE expression: the folded result has to hold. "X > 5 AND
     * Y > 2, for five seconds" — the conjunction is what must persist, and it
     * does not matter that X went high a minute earlier.
     *
     * Non-zero qualifies the NAMED TERMS INDIVIDUALLY and leaves the rest
     * instant: "X > 5 for five seconds AND Y > 2 right now" is bit 0 alone.
     * Each qualified term keeps its own accumulator, so they can mature at
     * different moments.
     *
     * The two readings are genuinely different under AND and both get asked
     * for, which is why these are masks and not flags. */
    uint8_t set_qualify_terms;
    uint8_t reset_qualify_terms;
    uint16_t dest_signal_idx; /* boolean output slot: 1.0 when held, else 0.0 */
    uint8_t flags;            /* CONDFLAG_* */
    uint8_t set_count;        /* comparisons in the Set expression, 1..COND_MAX_TERMS */
    uint8_t set_joiners;      /* ConditionJoin per gap, bit i joins set term i -> i+1 */
    uint8_t reset_count;      /* Set/Reset mode: comparisons in the Reset expression */
    uint8_t reset_joiners;    /* ConditionJoin per gap of the Reset expression */
    uint8_t latch_hz;         /* Momentary mode: the output holds for ONE PERIOD of
                               * this, so 10 Hz is 100 ms. 1..COND_LATCH_MAX_HZ,
                               * clamped rather than rejected — see rate_hz. */
} ConditionConfig;            /* 24 + 24 + 2 + 2 + 1 + 1 + 2 + 6 = 62 bytes — the
                               * wire structs are #pragma pack(1), so a record is
                               * the sum of its fields and field ORDER never
                               * changes its size. PAD8(61) is 64, so the flash
                               * slot carries three spare bytes.
                               * v13 spent the padded slot's spare four bytes on
                               * qualify_ms and left none: a u16 there would have
                               * wasted two of them AND capped the duration at
                               * eleven minutes. The wider slot is paid for by
                               * MAX_CONDITIONS 250 -> 200, which takes the table
                               * 14,000 -> 12,800 B — so the feature costs 1,200
                               * bytes LESS than what it replaced. */

/* v3: up/down counter. Inputs are edge-triggered on the rising edge of a
 * boolean channel (value crosses from <=0 to >0). */
typedef enum {
    COUNTER_MODE_UPDOWN = 0, /* count on Up/Down rising edges */
    COUNTER_MODE_FOLLOW = 1, /* count every change of the Follow input */
    /* Driven by the clock rather than by a channel: one step every 1/rate_hz
     * seconds, up or down per COUNTERFLAG_RATE_DOWN. The Up/Down/Follow inputs
     * are ignored in this mode; Enable and Reset still apply, so a rate counter
     * is a gateable, resettable ramp. */
    COUNTER_MODE_RATE = 2
} CounterMode;

/* Steps per second in COUNTER_MODE_RATE. The engine ticks at 100 Hz, so 100 is
 * the ceiling — one step per tick. The host offers 1, 2, 5, 10, 20, 50 and 100,
 * every one of which divides 100 exactly, but the firmware does NOT require
 * that: the phase accumulator below counts in Hz*ms and so averages exactly
 * rate_hz steps per second for any value in range, the same way an integrator
 * handles 3 Hz or 7 Hz. A record with rate_hz outside 1..COUNTER_MAX_HZ is
 * clamped rather than rejected, because a counter that silently stops is worse
 * than one running at a neighbouring rate. */
#define COUNTER_MAX_HZ 100u

/* CounterConfig.flags bits */
#define COUNTERFLAG_ROLL     0x01  /* wrap at min/max instead of clamping */
/* Retain this counter's value ACROSS POWER CYCLES: it is flushed to the
 * preserve_store ring (~60 s cadence, change-detected) and seeded back into its
 * output slot at boot. NOT a runtime behaviour — a counter already holds its
 * value while disabled whether or not this is set, so the flag's only effect is
 * to select the counter for persistence (see engine_preserve_enumerate).
 * At most PRESERVE_MAX (20) values are retained, shared with the v17
 * integrators (see engine_core.h's key-space note).
 * Works in BOTH flash modes: user_code.c derives the ring's base and page size
 * from the DBANK option bit at runtime and asserts both layouts. (This was
 * dual-bank-only once, and this comment used to say so long after it stopped
 * being true — see FIRMWARE-NOTES #18.) The one real asymmetry is that a
 * single-bank erase sits in the executing bank and halts the core for its
 * duration — at most one 100 Hz tick, at most once per 60 s flush, and only
 * when the active page fills. */
#define COUNTERFLAG_PRESERVE 0x02
#define COUNTERFLAG_ACTIVE   0x04
/* COUNTER_MODE_RATE only: subtract each step instead of adding. A separate flag
 * rather than a negative `step` because step is shared with the edge modes,
 * where its sign already means something, and because the host shows this as an
 * Up/Down choice rather than as arithmetic. */
#define COUNTERFLAG_RATE_DOWN 0x08

/* v15: WHERE A COUNTER INPUT COMES FROM. Two bits each for up, down, reset and
 * enable, packed into one byte:
 *
 *   COUNTER_SRC_SIGNAL  the matching *_signal_idx is a SIGNAL index, and the
 *                       input reads that channel's value — what every counter
 *                       written before v15 does, which is why it is 0.
 *   COUNTER_SRC_MSG_RX  the index is a MESSAGE index; the input is true only on
 *                       the pass that message was received.
 *   COUNTER_SRC_MSG_TX  the same, for transmitted.
 *
 * The two message kinds are the operators a User Condition already has, reaching
 * counters directly: "count every frame from the ECU" was a condition row and a
 * generated channel before, for something a counter can now say itself.
 *
 * FOLLOW IS DELIBERATELY ABSENT, and not for space. Follow tracks a channel's
 * VALUE, and "a frame arrived" has no value — only a moment. There is nothing
 * for it to follow.
 *
 * Four inputs at two bits is exactly one byte, and one byte is exactly what the
 * record had spare: 31 bytes in a PAD8 slot of 32. The counters table does not
 * grow at all. */
#define COUNTER_SRC_SIGNAL 0u
#define COUNTER_SRC_MSG_RX 1u
#define COUNTER_SRC_MSG_TX 2u
#define COUNTER_SRC_SHIFT_UP     0
#define COUNTER_SRC_SHIFT_DOWN   2
#define COUNTER_SRC_SHIFT_RESET  4
#define COUNTER_SRC_SHIFT_ENABLE 6
#define COUNTER_SRC_AT(kinds, shift) (((kinds) >> (shift)) & 0x3u)

typedef struct {
    uint16_t up_signal_idx;    /* rising edge -> +step  (0xFFFF = unused) */
    uint16_t down_signal_idx;  /* rising edge -> -step  (0xFFFF = unused) */
    uint16_t follow_signal_idx;/* FOLLOW mode source     (0xFFFF = unused) */
    uint16_t reset_signal_idx; /* rising edge -> reset_value (0xFFFF = unused) */
    uint16_t enable_signal_idx;/* gate: counts only while >0 (0xFFFF = always) */
    uint16_t dest_signal_idx;  /* output value slot */
    float min_value;
    float max_value;
    float reset_value;
    float step;                /* increment amount (usually 1) */
    uint8_t mode;              /* CounterMode */
    uint8_t flags;             /* COUNTERFLAG_* */
    uint8_t rate_hz;           /* COUNTER_MODE_RATE: steps per second, 1..COUNTER_MAX_HZ */
    uint8_t input_kinds;       /* v15: COUNTER_SRC_* per input, see above */
} CounterConfig;               /* 6*2 + 4*4 + 4 = 32 bytes */
/* The record grew 30 -> 31 for rate_hz at ZERO cost in config flash: slots are
 * padded to 8 bytes and PAD8(30) and PAD8(31) are both 32, so the byte was
 * already being erased and written as pad. Same trick the v17 integrator used
 * (FIRMWARE-NOTES #9d). It does cost one byte per record on the wire, which the
 * host's WRITE_CHUNK_COUNTERS arithmetic absorbs without dropping a record per
 * frame (4 + 3*31 = 97, under the 112-byte payload cap of the day — the cap is
 * 496 now, so the chunk is no longer anywhere near it). */

/* v3: timer. Accumulates elapsed time (seconds) while running.
 *
 * v12: START AND STOP ARE CONDITION TERMS, not signal indices. A timer used to
 * take two channel slots and start on the rising edge of "that channel is
 * non-zero", which meant every threshold — "over 4000 rpm", "this message
 * arrived" — had to be spelled as a User Condition first and pointed at from
 * here. The term is the same 8-byte record a condition's comparison is, so a
 * timer now says "start when RPM > 4000" or "start when message 7 was
 * received" directly, and the two features cannot drift apart about what a
 * comparison means: engine_core.c evaluates both through evalConditionTerm().
 *
 * THE EDGE IS UNCHANGED. The term produces a boolean and the timer still
 * triggers on its RISING edge, so the pre-v12 behaviour is exactly the term
 * (channel NEQ 0) — which is what the host writes when it migrates an old
 * configuration, and why the migration needs no special case in the engine.
 *
 * FIELD ORDER IS NOT LOAD-BEARING HERE, and v12 briefly claimed it was. These
 * structs are #pragma pack(1): a record is the sum of its fields, so moving
 * dest_signal_idx after the floats changed nothing about the size. 2*8 + 3*4 + 2
 * + 1 + 1 is 32 in any order. The order below is kept because it reads well —
 * the two triggers, then the three values, then the small fields — not because
 * anything depends on it.
 *
 * What IS load-bearing is the 32 itself: PAD8(32) is 32, so the slot is exactly
 * the record and 50 timers cost 1,600 B against the 1,200 they cost at 20. */
#define TIMERFLAG_COUNTDOWN   0x01 /* count down from limit instead of up */
#define TIMERFLAG_ROLLOVER    0x02 /* wrap at the limit instead of holding */
#define TIMERFLAG_SET_ON_START 0x04 /* load start_value when started */
#define TIMERFLAG_SET_ON_STOP  0x08 /* load stop_value when stopped */
#define TIMERFLAG_ACTIVE      0x10

typedef struct {
    ConditionTerm start_term;  /* rising edge of this starts the timer */
    ConditionTerm stop_term;   /* rising edge of this stops it */
    float limit_value;         /* rollover/countdown limit */
    float start_value;         /* value loaded on start (if flagged) */
    float stop_value;          /* value loaded on stop  (if flagged) */
    uint16_t dest_signal_idx;  /* output value slot (seconds) */
    uint8_t flags;             /* TIMERFLAG_* */
    uint8_t reserved;
} TimerConfig;                 /* 2*8 + 3*4 + 2 + 2 = 32 bytes, no padding */

/* An UNUSED half. input_a_signal_idx out of range is how a term says "never",
 * which is what the old 0xFFFF in start_signal_idx meant and is checked the
 * same way: conditionExprInRange() refuses it and the engine reads the side as
 * false forever. A timer with no stop term simply never stops itself. */
#define TIMER_TERM_UNUSED 0xFFFFu

/* v6: constant — writes a fixed physical value into a generated channel's
 * value slot on every evaluation pass (applied before math). */
typedef struct {
    uint16_t dest_signal_idx;  /* output value slot */
    float value;               /* physical value written to the slot */
    uint8_t is_active;
} ConstantConfig;              /* 2 + 4 + 1 = 7 bytes */

/* v11: message relay — a masked-ID gateway rule checked on every received frame.
 * Forwards the frame whole to forward_bus_mask (bit0=CAN1..bit2=CAN3, never the
 * source bus) when, among frames of its extended-ness,
 * (arbitration_id & bitmask) == (address & bitmask); RELAYFLAG_INVERT forwards
 * the non-matching frames instead. */
typedef struct {
    uint32_t address;
    uint32_t bitmask;
    uint8_t flags;             /* RELAYFLAG_* */
    uint8_t src_bus;           /* the bus this rule listens on (1..3) */
    uint8_t forward_bus_mask;  /* bit0=CAN1 bit1=CAN2 bit2=CAN3 */
} RelayConfig;                 /* 4 + 4 + 1 + 1 + 1 = 11 bytes */

/* v13: 2x16 lookup table — one input axis (x_signal_idx) with up to 16 ascending
 * sites maps to output values, written to dest_signal_idx every evaluation pass.
 * Only the first x_count sites/outputs are used (a partially-filled table),
 * 1..16. TABLEFLAG_X_INTERP selects linear interpolation vs discrete-centered.
 * Inputs below the first / above the last used site clamp to the end output.
 *
 * The table is split across two records at the SAME index in two parallel
 * tables, because a combined 134-byte record does not fit one. When v13 split it
 * the binding limit was the 112-byte host->device payload cap; that cap is 496
 * now, but PAD8(134) = 136 still exceeds MAX_PADDED_RECORD (112, flash_store's
 * one-slot write scratch), so the split stands on the other constraint and the
 * layout does not move. Table t is evaluated only when both tables hold a
 * record at index t. */
typedef struct {
    uint16_t x_signal_idx;     /* input axis channel */
    uint16_t dest_signal_idx;  /* output value slot */
    uint8_t flags;             /* TABLEFLAG_ACTIVE | TABLEFLAG_X_INTERP */
    uint8_t x_count;           /* number of active sites, 1..16 */
    float x_sites[TABLE_2X16_SITES]; /* breakpoints, ascending (first x_count used) */
} Table2x16Def;                /* 2 + 2 + 1 + 1 + 64 = 70 bytes */

typedef struct {
    float outputs[TABLE_2X16_SITES]; /* value at each site (first x_count used) */
} Table2x16Out;                /* 64 bytes */

/* 8x8 lookup table — X and Y input axes (up to 8 ascending sites each,
 * x_count/y_count active) map to a 64-cell output grid (row-major:
 * grid[y*8 + x]; only x<x_count, y<y_count are used). Each axis independently
 * interpolates (bilinear when both do) or is discrete-centered; inputs clamp to
 * the end used sites. Those are the v12 4x4's semantics exactly — this record
 * replaces that one (Table4x4Config, commands 0x1D/0x1E, both retired), and the
 * evaluation pass is the same bilinear blend over a wider grid.
 *
 * Combined it would be 73 + 256 = 329 bytes. That WOULD now fit the raised
 * 496-byte host->device payload cap — the cap is no longer what forbids it. What
 * forbids it is MAX_PADDED_RECORD: flash_store programs one padded slot at a
 * time through a 112-byte stack buffer, and PAD8(329) is 336. So it is SPLIT on
 * the v13 2x16 precedent: a Def, plus ONE RECORD PER GRID ROW in a second
 * parallel table. Table t owns row indices t*8 .. t*8+7, so the Row table's
 * capacity is MAX_TABLES_8X8 * TABLE_8X8_SITES = 64.
 *
 * WHY ROWS AND NOT SOME OTHER CHUNKING, because this is the whole reason the
 * shape is worth it: a Row is 32 bytes and PAD8(32) is 32, so consecutive Row
 * slots are BYTE-CONTIGUOUS in flash. Table t's eight rows are therefore one
 * unbroken 256-byte block, and the engine takes a single pointer at row t*8 and
 * indexes grid[y*8 + x] — precisely what Table4x4Config.outputs[y*4 + x] did
 * when the grid was one record. No reassembly buffer, no per-record offset
 * arithmetic, no RAM. Any chunking whose padded size is not its real size (a
 * 3-row chunk at 96, say, or a 20-byte record padded to 24) loses that and
 * forces a copy into RAM on every evaluation pass.
 *
 * Torn-upload safety works as it does for the 2x16: the host writes ROWS FIRST
 * and the Def last, TABLEFLAG_ACTIVE lives only on the Def, and the engine
 * evaluates table t only when the Def count exceeds t AND the Row count reaches
 * (t+1)*8. Un-programmed flash reads 0xFF, which as a float is NaN, and one NaN
 * cell would propagate through the blend into the output channel. */
typedef struct {
    uint16_t x_signal_idx;
    uint16_t y_signal_idx;
    uint16_t dest_signal_idx;
    uint8_t flags;             /* TABLEFLAG_ACTIVE | X_INTERP | Y_INTERP */
    uint8_t x_count;           /* active X sites, 1..8 */
    uint8_t y_count;           /* active Y sites, 1..8 */
    float x_sites[TABLE_8X8_SITES];
    float y_sites[TABLE_8X8_SITES];
} Table8x8Def;                 /* 2 + 2 + 2 + 1 + 1 + 1 + 32 + 32 = 73 bytes (PAD8 80) */

/* ONE grid row: the cells grid[y*8 + 0 .. y*8 + 7] at a fixed y. Record index
 * t*8 + y belongs to table t, row y. */
typedef struct {
    float v[TABLE_8X8_SITES];
} Table8x8Row;                 /* 32 bytes (PAD8 32 — see the note above) */

/* v16: integrator — a rate accumulator. Every step it moves its output channel
 * by its input:
 *
 *     out += input,  rate_hz times per second      (COUNT_DOWN clear)
 *     out -= input,  rate_hz times per second      (COUNT_DOWN set, v17)
 *
 * RAW accumulation, deliberately not `input * dt`: the rate scales the result,
 * which is the point of making it configurable. To integrate a rate channel
 * into a total, pre-scale it (a Math channel) so one step moves the right slice.
 *
 * v17: a DECREMENTOR is just COUNT_DOWN plus a start_value at the peak, which
 * is why there is no second table — the two differ by a sign and a seed, and
 * duplicating the pass would be duplicating a bug surface.
 *
 * start_value is written into the output slot when the config LOADS at boot,
 * so a decrementor begins full instead of at zero. A restored
 * PRESERVE value is seeded afterwards and therefore wins — see
 * engine_seed_integrators / preserveRestoreAtBoot for that ordering.
 *
 * The engine ticks at 100 Hz, so rate_hz is clamped to 1..INTEGRATOR_MAX_HZ.
 * A rate that does not divide 1000 (3 Hz, 7 Hz, ...) is carried in a Hz*ms
 * phase accumulator, so it averages exactly rate_hz steps per second rather
 * than drifting on a truncated integer period.
 *
 * The enable gate FREEZES the phase, so gating costs no steps rather than
 * silently skipping them. Reset is edge-triggered and applies even while
 * disabled (as on a counter), and restarts the phase so the first step after a
 * reset is a full period away. */
#define INTEGFLAG_ACTIVE        0x01
#define INTEGFLAG_CONST_INPUT   0x02  /* accumulate input_const, not a channel */
#define INTEGFLAG_COUNT_DOWN    0x04  /* v17: subtract instead of add */
/* v17: retain this total across power cycles, via the same preserve ring the
 * counters use — PRESERVE_MAX = 20 entries TOTAL across both tables (see
 * COUNTERFLAG_PRESERVE). Note an integrator usually changes every step, so
 * unlike an event-driven counter it appends a record on essentially every 60 s
 * flush; that makes it the dominant consumer of both the ring's capacity and
 * its erase budget, and on single-bank parts the dominant source of the brief
 * erase stall. */
#define INTEGFLAG_PRESERVE      0x08

typedef struct {
    uint16_t input_signal_idx;  /* accumulated each step (unused if CONST_INPUT) */
    uint16_t reset_signal_idx;  /* rising edge -> reset_value (0xFFFF = unused) */
    uint16_t enable_signal_idx; /* gate: accumulates only while > 0 (0xFFFF = always) */
    uint16_t dest_signal_idx;   /* output value slot */
    float input_const;          /* accumulated instead when CONST_INPUT is set */
    float min_value;            /* clamp; max <= min disables clamping */
    float max_value;
    float reset_value;          /* loaded on the reset input's rising edge */
    float start_value;          /* v17: seeded into the slot when the config loads */
    uint8_t rate_hz;            /* steps per second, 1..INTEGRATOR_MAX_HZ */
    uint8_t flags;              /* INTEGFLAG_* */
} IntegratorConfig;             /* 4*2 + 5*4 + 2 = 30 bytes (padded slot stays 32) */

/* CMD_WRITE/READ_CRC8_CFG record — a CRC-8 stamped into a transmit message.
 *
 * The rule BINDS TO a message (msg_idx) rather than living inside
 * CanMessageConfig: at most MAX_CRC8_MESSAGES messages carry a checksum, and
 * growing all 500 message records by forty bytes to serve twenty of them
 * would double the message table's flash for nothing.
 *
 * The transmit composer applies the rule LAST, after the message's channels
 * (and, for a compound message, the variant's selector) have been packed —
 * per FRAME, so each variant of a compound message is stamped over its own
 * bytes. The computed value is also published to dest_signal_idx, so the CRC
 * the wire carried is a channel like any other: watchable in Monitor
 * Channels, usable by calculations.
 *
 * The algorithm is the standard bitwise CRC-8: register seeded with
 * init_value, each input byte (reflected first under REF_IN) XORed in,
 * eight shifts against `polynomial` (x^8 implicit, MSB-first), the final
 * register reflected under REF_OUT and XORed with final_xor. polynomial /
 * init_value / final_xor cover the automotive set — SAE J1850 is
 * 0x1D/0xFF/0xFF, AUTOSAR 2F is 0x2F/0xFF/0xFF, plain CCITT is
 * 0x07/0x00/0x00. */
#define CRC8FLAG_ACTIVE   0x01
#define CRC8FLAG_REF_IN   0x02  /* reflect each input byte before feeding it */
#define CRC8FLAG_REF_OUT  0x04  /* reflect the register before the final XOR */

#define CRC8_MAX_ELEMENTS 15

/* What one element feeds into the CRC, in element order. */
enum {
    CRC8_ELEM_ID   = 0,  /* a byte of the CAN identifier: elem_value is the
                          * shift index 0..3, the byte fed is
                          * (can_id >> 8*elem_value) & 0xFF */
    CRC8_ELEM_DATA = 1,  /* a byte of the frame: elem_value is the byte index
                          * 0..7; an index at or past the DLC feeds 0 */
    CRC8_ELEM_RAW  = 2,  /* a literal: elem_value is fed as-is */
};

typedef struct {
    uint16_t msg_idx;          /* the message table entry this rule stamps */
    uint16_t dest_signal_idx;  /* slot the computed CRC is published to
                                * (SIG_MSG_NONE = compute and stamp only) */
    uint8_t byte_location;     /* frame byte that receives the CRC, 0..7 */
    uint8_t polynomial;        /* x^8 implicit: 0x1D = SAE J1850, 0x07 = CCITT */
    uint8_t init_value;        /* register seed */
    uint8_t final_xor;         /* XORed into the result last */
    uint8_t flags;             /* CRC8FLAG_* */
    uint8_t element_count;     /* 1..CRC8_MAX_ELEMENTS */
    uint8_t elem_type[CRC8_MAX_ELEMENTS];   /* CRC8_ELEM_*, per element */
    uint8_t elem_value[CRC8_MAX_ELEMENTS];  /* meaning depends on elem_type */
} Crc8Config;                  /* 2*2 + 6 + 2*15 = 40 bytes (PAD8: slot stays 40) */

/* CMD_WRITE/READ_DEVICE_CHANNELS payload — where the device publishes the
 * values it produces about itself. See the command definitions above.
 *
 * One uint16 destination slot per device channel, SIG_MSG_NONE for the ones
 * this configuration does not read. The array is indexed by DeviceChannelId and
 * the GUI's channel catalogue carries the same ids, so adding a channel is an
 * enum entry on each side rather than a new named field and a new special case
 * in the mapper — which is what the single-field version of this struct cost
 * every time something new wanted publishing.
 *
 * DEVCH_ONTIME is deliberately 0. The struct began life as a bare
 * `uint16_t ontime_signal_idx`, so keeping OnTime at offset 0 makes the old
 * 2-byte payload a valid PREFIX of this one, and that is what lets the write
 * handler accept a short payload from an older host instead of NACKing it (see
 * CMD_WRITE_DEVICE_CHANNELS in serial_proto.c). Do not reorder it.
 *
 * Device OnTime is SECONDS SINCE BOOT, quantised to 0.01 s to match the channel
 * the host defines for it (u32, 2 decimal places). It counts the device being
 * powered and running, not the configuration being loaded: clearing or
 * re-sending a configuration re-points the slot but never restarts the clock,
 * and only a reset or a power cycle takes it back to zero.
 *
 * Worth knowing before building anything on OnTime's low digits: the value slots
 * are float32, so 0.01 stops being exactly representable once the reading passes
 * about 2^24 hundredths -- roughly 46 hours of uptime -- after which the
 * hundredths coarsen and eventually the tenths do too. The counter behind it is
 * integer milliseconds and does not drift; the loss is in the slot, and it is
 * inherent to every channel the engine carries, not special to this one. The CAN
 * diagnostic channels below are free of it for a reason worth stating: REC, TEC
 * and the three bus-state flags are small integers a float32 holds EXACTLY
 * forever, so the caveat that dominates OnTime does not transfer to them. The
 * two frame counters do inherit it — they are unbounded and coarsen past 2^24
 * frames — and Bus Load is a percentage that never leaves 0..100. */

/* Per-bus diagnostic fields, in the order they occupy each bus's block. */
typedef enum {
    DEVCH_BUS_RX_ERRORS     = 0, /* REC, 0..127 */
    DEVCH_BUS_TX_ERRORS     = 1, /* TEC, 0..255 */
    DEVCH_BUS_WARNING       = 2, /* 1 once either counter reaches 96 */
    DEVCH_BUS_ERROR_PASSIVE = 3, /* 1 while the node is error-passive */
    DEVCH_BUS_BUS_OFF       = 4, /* 1 while the node is bus-off */
    DEVCH_BUS_ERROR_FRAMES  = 5, /* accumulated protocol errors since boot */
    DEVCH_BUS_RX_COUNT      = 6, /* frames received since boot */
    DEVCH_BUS_TX_COUNT      = 7, /* frames transmitted since boot */
    DEVCH_BUS_LOAD          = 8, /* estimated bus utilisation, 0..100 % */
    /* How many times the firmware has RESTARTED this bus after bus-off. The
     * attempt, not the outcome: a restart on a still-faulty bus is undone
     * within about five milliseconds, so a count of lasting reconnections reads
     * 0 on the very bus being diagnosed. Read it with DEVCH_BUS_BUS_OFF, which
     * carries the outcome: off=1 with this climbing is a bus being retried and
     * failing every time, off=0 with this climbing is one that is flapping —
     * a fault the state flag cannot show alone, since it reads 0 between
     * events. */
    DEVCH_BUS_OFF_RECOVERIES = 9,
    DEVCH_PER_BUS            = 10
} DeviceChannelBusField;

/* Deliberately an ENUM rather than the #defines these started as. The GUI
 * mirrors these names as `constexpr int` inside namespace ct, and the host test
 * includes both headers into one translation unit: an object-like macro would
 * rewrite `ct::DEVCH_COUNT` into `ct::(1 + 3 * 10)` and fail to compile in a way
 * that points at this header rather than at the line that used it. Enum
 * constants respect the namespace and cannot do that. DEVCH_BUS below stays a
 * macro because it takes arguments, so it only expands where it is called. */
enum {
    DEVCH_ONTIME    = 0, /* must stay 0 — see above */
    DEVCH_BUS_BASE  = 1,
    DEVCH_BUS_COUNT = 3,
    /* v9 (store): the MCU health block, APPENDED after the bus blocks so every
     * index above survives — the same rule that keeps OnTime at 0, applied at
     * the other end. What the silicon reports about itself:
     *
     *   TEMP       die temperature in °C, published at 0.1 °C resolution from
     *              the ADC's internal sensor with the factory calibration
     *              points (the raw sensor is ±2 °C at best; the tenth is
     *              resolution, not accuracy).
     *   VDDA       the analogue supply in volts, at 0.001 V, measured against
     *              the factory-calibrated internal reference.
     *   VDDA_MIN / TEMP_MAX
     *              the excursions since BOOT, not since config load: they
     *              survive CLEAR_CONFIG the way the CAN error totals do,
     *              because "what has this unit been through" is precisely the
     *              question a mid-diagnosis reconfigure must not erase.
     *   RESET_REASON
     *              why the last reset happened, RESET_REASON_* below. Read
     *              once at boot from the RCC flags and latched.
     */
    DEVCH_MCU_TEMP     = DEVCH_BUS_BASE + DEVCH_BUS_COUNT * DEVCH_PER_BUS, /* 31 */
    DEVCH_MCU_VDDA     = DEVCH_MCU_TEMP + 1, /* 32 */
    DEVCH_MCU_VDDA_MIN = DEVCH_MCU_TEMP + 2, /* 33 */
    DEVCH_MCU_TEMP_MAX = DEVCH_MCU_TEMP + 3, /* 34 */
    DEVCH_RESET_REASON = DEVCH_MCU_TEMP + 4, /* 35 */
    DEVCH_COUNT        = DEVCH_MCU_TEMP + 5  /* 36 */
};

/* Why the last reset happened, for DEVCH_RESET_REASON. The numbering is the
 * GUI's enumeration — it displays these names for the value — so it is wire
 * contract, not implementation convenience. POWER_ON vs BROWNOUT: the RCC
 * cannot tell them apart (a true power-on raises the brownout flag too), so
 * the firmware disambiguates with a .noinit RAM magic — random garbage after
 * a cold power-up, intact after a brownout-depth dip that reset the core but
 * never drained the SRAM. */
#define RESET_REASON_UNKNOWN    0u
#define RESET_REASON_POWER_ON   1u
#define RESET_REASON_BROWNOUT   2u
#define RESET_REASON_NRST       3u /* the external reset pin */
#define RESET_REASON_SOFTWARE   4u /* NVIC_SystemReset — CMD_RESET_DEVICE, a fault handler */
#define RESET_REASON_IWDG       5u /* independent watchdog */
#define RESET_REASON_WWDG       6u /* window watchdog */
#define RESET_REASON_LOW_POWER  7u

/* The flat index of a per-bus field. bus0 is 0-based (bus 1 is 0), unlike the
 * 1-based bus_idx the wire protocol uses everywhere else — the conversion is
 * done once at each call site rather than hidden in here, so an index built from
 * a raw bus_idx is a visible mistake instead of a silent off-by-one block. */
#define DEVCH_BUS(bus0, field) (DEVCH_BUS_BASE + (bus0) * DEVCH_PER_BUS + (field))

typedef struct {
    uint16_t signal_idx[DEVCH_COUNT]; /* SIG_MSG_NONE = configuration does not use it */
} DeviceChannelsConfig;               /* 72 bytes (DEVCH_COUNT 36 * 2) */

/* One bus's error state, sampled off the FDCAN peripheral by the glue layer and
 * handed to the (HAL-free) engine. Everything here comes from ECR and PSR.
 *
 * error_delta is a DELTA, not a level, because the register it comes from —
 * FDCAN's CAN Error Logging counter — is cleared by the act of reading it. The
 * glue reads it and passes what it saw; the engine accumulates. Splitting it
 * this way keeps the running total in the engine, where it survives a
 * configuration change and can be tested on the host, and keeps the
 * read-clears-it hazard confined to the one line that touches the register. */
#define BUSDIAG_WARNING       0x01
#define BUSDIAG_ERROR_PASSIVE 0x02
#define BUSDIAG_BUS_OFF       0x04

typedef struct {
    uint8_t rx_errors;   /* REC */
    uint8_t tx_errors;   /* TEC */
    uint8_t flags;       /* BUSDIAG_* */
    uint8_t error_delta; /* CEL increments since the previous sample */
} BusDiagnostics;

typedef struct {
    uint32_t uptime_ms;
    uint32_t rx_count[3];
    uint32_t tx_count[3];
    uint8_t bus_state[3];    /* 0 = off, 1 = active, 2 = listen-only */
    uint16_t active_msg_count;
    uint16_t active_sig_count;
    uint16_t active_math_count;
    uint16_t active_cond_count;
} DeviceStatus;              /* GET_STATUS appends: u8 protocol_version, u8 fw_minor */

typedef struct {
    uint8_t bus_idx;         /* 1..3 */
    uint8_t mode;            /* 0 = off, 1 = active, 2 = listen-only */
    uint32_t baud_rate;
    uint32_t data_baud_rate; /* > baud_rate enables CAN FD with BRS */
    uint8_t termination;     /* v9: 1 = enable the bus termination resistor */
} ControlCanPayload;         /* 11 bytes */

/* v19: access keys — the device half of "Online > Set Access Passwords".
 *
 * Three functions can each carry their own password. The host turns a typed
 * password into a 4-byte key (PBKDF2-HMAC-SHA256 over a fixed application salt,
 * folded to 4 bytes — see src/model/access_keys.h) and the device stores that
 * key. The device never sees a password, and the key is write-only: nothing
 * reads it back off the wire, so the only way to learn one is to dump flash.
 *
 * Proving a key is challenge-response — device issues a random nonce, host
 * answers HMAC-SHA256(key, nonce) — so a serial capture is worth nothing on the
 * next connection, and a wrong guess costs a full round trip.
 *
 * Be exact about what 4 bytes buys, because it is the deliberate trade this
 * design makes:
 *   * Against someone GUESSING THE PASSWORD it is strong: PBKDF2 at
 *     ACCESS_KEY_ITERATIONS rounds makes each candidate expensive, and the
 *     device only answers one guess per round trip.
 *   * Against someone who DUMPS FLASH it is nothing at all — they have the key
 *     itself and never need the password. The backstop is STM32G4 readout
 *     protection (RDP), a programming step, not a code one.
 *   * The key space is 2^32. Online that is unreachable (one round trip per
 *     try); offline, against a captured challenge/response pair, it is not.
 *     Four bytes is what the hardware compares, so this is a floor on the
 *     protection, not a bug in it. */
/* ACCESS_KEY_LEN is defined near PROTOCOL_VERSION at the top — CanMessageConfig
 * needs it long before this block. */
#define ACCESS_CHALLENGE_LEN      16u
#define ACCESS_FN_SEND            0u  /* send a configuration to the device */
#define ACCESS_FN_GET             1u  /* get a configuration back off the device */
/* 2.3.0: this key gates NOTHING on the device. It exists so a host can ask the
 * device to CONFIRM the password — CMD_ACCESS_RESPONSE(ACCESS_FN_EDIT_COMMS)
 * answers ACK or ERR_LOCKED — and that confirmation is the whole difference
 * between the Protected tier and the Hidden one: Hidden is unlocked against the
 * document, Protected requires a connected unit to agree. No command here is
 * refused on it, so it never appears in an accessBlocked() call. */
#define ACCESS_FN_EDIT_COMMS      2u  /* prove-only: confirms the Edit Protected
                                       * Comms password for a host */
#define ACCESS_FN_COUNT           3u
#define ACCESS_MASK_SEND          (1u << ACCESS_FN_SEND)
#define ACCESS_MASK_GET           (1u << ACCESS_FN_GET)
#define ACCESS_MASK_EDIT_COMMS    (1u << ACCESS_FN_EDIT_COMMS)

/* v18: device binding. A configuration can name the chip it was built for —
 * the STM32's 96-bit unique device ID, which is read-only silicon and differs
 * on every part. flash_store_validate refuses an image whose binding is not
 * this chip's, so a configuration lifted off one device and written to another
 * (by a flash programmer, or by any tool that is not CAN Triple Device Manager) does
 * not run: the device falls back to bring-up defaults.
 *
 * An all-zero binding means "not bound" and runs anywhere, which is the
 * default and what every existing configuration is.
 *
 * Worth being exact about what this is: the UID is stored, not signed. It stops
 * a config being COPIED between devices. Someone who reverse-engineers this
 * header can patch in another UID and recompute the image CRC — defeating that
 * needs the binding to be a MAC under a key the attacker does not hold, and
 * that key must not ship in the customer's copy of the Manager. Nor does this
 * stop whole-chip cloning; the backstop for that is STM32G4 readout protection
 * (RDP), a programming step rather than a code one. */
#define CONFIG_UID_LEN 12u /* 96-bit STM32 unique device ID */

/* Why the stored configuration is or is not running (CMD_GET_DEVICE_ID). */
#define CONFIG_STATUS_OK           0x00u /* a valid image is loaded */
#define CONFIG_STATUS_NONE         0x01u /* no valid image (blank, or bad magic/version/CRC) */
#define CONFIG_STATUS_WRONG_DEVICE 0x02u /* valid, but bound to a different chip */

/* v17: PROTECTED COMMS HAS FOUR SLOTS, so one unit can accept configurations
 * sealed under any of four different Protected Comms passwords \u2014 several
 * vendors' configs on one device. SLOT 1 IS keys[ACCESS_FN_EDIT_COMMS], exactly
 * where the single password always lived; slots 2..4 are prot_comms_extra. An
 * all-zero key is an empty slot (deriveAccessKey never yields zero), and the
 * set_mask's EDIT_COMMS bit means "at least one slot set".
 *
 * ANY SLOT OPENS. A prove against ACCESS_FN_EDIT_COMMS is tried against every
 * non-empty slot, so anyone holding any accepted password can open every
 * Protected message on the unit \u2014 stated as the chosen trade, not an
 * oversight: per-vendor separation lives in the four per-configuration message
 * passwords, not here. Send and Get remain single-slot. */
typedef struct {
    uint8_t set_mask;                                 /* ACCESS_MASK_*; 0 = no passwords */
    uint8_t keys[ACCESS_FN_COUNT][ACCESS_KEY_LEN];    /* never leave the device */
    uint8_t prot_comms_extra[3][ACCESS_KEY_LEN];      /* EDIT_COMMS slots 2..4 */
} AccessKeyRecord;                                    /* 1 + 12 + 12 = 25 bytes */

/* CMD_WRITE_ACCESS_KEYS payload. Sets or clears ONE function at a time, which
 * is what the host's dialog does and what keeps "change this one password" from
 * needing every other one in hand. `clear` set means remove the password for
 * `function` and ignore `key`. */
typedef struct {
    uint8_t function;                  /* ACCESS_FN_* */
    uint8_t clear;                     /* non-zero = remove the password */
    uint8_t key[ACCESS_KEY_LEN];
    /* v17: which Protected Comms slot (1..4). Ignored for Send and Get. The
     * device also accepts the old 6-byte payload \u2014 slot absent reads as slot
     * 1 \u2014 so a host or bench script that predates the slots keeps working and
     * keeps meaning what it always meant. */
    uint8_t slot;
} AccessKeyWritePayload;               /* 7 bytes (6 accepted as slot 1) */

/* What CMD_READ_FLEET_ID gives back.
 *
 * Five of these six fields come from the BUILD (fleet_identity.h) and cannot
 * change at runtime. config_version is the exception and comes from the flash
 * header, because it is the one thing that has to move when a configuration is
 * released: it says which revision the unit is running now, and the host refuses
 * to send an image whose version is not NEWER, so a customer cannot be walked
 * backwards onto a config whose problems are already known.
 *
 * fleet_key is absent by design — key_present says only that there is one, and
 * CMD_FLEET_ID_PROVE is the only way to learn the device really holds it.
 *
 * The two strings are NUL-PADDED and not necessarily NUL-terminated: all 16
 * bytes are usable. Compare them as counted fields, never with strcmp. */
typedef struct {
    char vendor_id[FLEET_VENDOR_ID_LEN];
    char model_id[FLEET_MODEL_ID_LEN];
    uint32_t serial_number;
    uint16_t config_version;           /* from the flash header, not the build */
    uint16_t flags;
    uint8_t key_present;               /* 1 = a series key is compiled in */
} FleetIdentityPublic;                 /* 16+16+4+2+2+1 = 41 bytes */

typedef struct {
    uint8_t bus_idx;         /* 1..3 */
    uint32_t can_id;
    uint8_t flags;           /* bit0 = extended, bit1 = fd */
    uint8_t data_len;        /* 0..64 */
    uint8_t data[64];
} InjectCanPayload;

typedef struct {
    uint32_t timestamp_ms;
    uint8_t bus_idx;         /* 1..3 */
    uint8_t direction;       /* 0 = Rx, 1 = Tx */
    uint32_t can_id;
    uint8_t flags;           /* MONFLAG_* (extended / fd / brs / esi) */
    uint8_t data_len;
    uint8_t data[64];
} MonitorStreamPayload;

/* The monitor stream sends only the bytes a frame actually carries:
 * MONITOR_HEADER_BYTES + data_len, NOT sizeof(MonitorStreamPayload).
 *
 * The struct reserves the CAN FD maximum, so a fixed-size frame spent 76 bytes
 * describing an 8-byte message and 76 describing a 4-byte one. That is most of
 * the stream's cost, and the stream is the thing that overruns first: a 4-byte
 * frame goes from 85 bytes on the wire to 25, so a bus the link could only
 * describe a third of becomes one it can describe completely.
 *
 * A reader must therefore take data_len from the payload rather than assume,
 * and treat anything past it as absent. A payload of exactly
 * sizeof(MonitorStreamPayload) is the older fixed-size form and stays valid —
 * hosts should accept both, because a new host still has to read a device that
 * has not been updated. The reverse does NOT hold: an older host checking for
 * the fixed size sees nothing from a device that trims. */
#define MONITOR_HEADER_BYTES 12u /* through data_len, before data[] */

typedef struct {
    uint16_t signal_idx;
    float physical_value;
} SignalValueEntry;

/* One chunk of script bytecode. Deliberately a bare byte array: the structure
 * inside is ScriptHeader + instructions (script_vm.h), and the flash store has
 * no business knowing about it — chunks are transport and storage, the VM's
 * verifier is what gives them meaning. PAD8(64) == 64, so chunks are
 * byte-contiguous in flash and the image is readable through one pointer. */
typedef struct {
    uint8_t b[SCRIPT_CHUNK_BYTES];
} ScriptChunk;

/* CMD_SCRIPT_STATUS reply: what the device made of the stored script.
 *
 * verify_result is the SCRIPT_* code from script_verify() — the reason a script
 * is not running, which is the difference between "fix your bytecode" and "the
 * upload was truncated". peak_cost is the highest single-tick budget spend seen
 * since load: the number that tells a user how close their script runs to the
 * ceiling, which nothing else can reveal. */
typedef struct {
    uint8_t  present;        /* a script image is stored (chunk count > 0) */
    uint8_t  verify_result;  /* SCRIPT_OK or why it was rejected */
    uint8_t  fault;          /* SCRIPT_FAULT_* from the last run */
    uint8_t  suspended;      /* faulted; will not run again until reload */
    uint32_t code_bytes;     /* bytecode length from its header, 0 if unusable */
    uint32_t last_cost;      /* budget units spent in the most recent tick */
    uint32_t peak_cost;      /* highest single-tick spend since load */
    uint16_t num_state;      /* persistent state registers the script declares */
    uint16_t budget;         /* SCRIPT_TICK_BUDGET, so the host need not assume */
    /* --- appended after the first shipping VM ------------------------------
     * What the hook cost in CPU CYCLES, which is what turns the budget's
     * synthetic units into time a person can reason about: at 170 MHz a 10 ms
     * tick is 1,700,000 cycles, so peak_cycles against that is the share of the
     * tick a script is taking.
     *
     * APPENDED, not inserted. Every field above keeps its offset, so a host
     * built before these existed reads the reply it always did and ignores the
     * tail — which is why hosts must check reply length with >=, never ==.
     * cycles_valid is 0 when the part has no usable cycle counter; the two
     * counts are then 0 and mean nothing, and a host must not present them as a
     * script that costs nothing. */
    uint32_t last_cycles;
    uint32_t peak_cycles;
    uint8_t  cycles_valid;
} ScriptStatus;              /* 29 bytes */

/* --- Firmware update (CMD_FW_UPDATE_*, 0x38-0x3C) -------------------------
 *
 * The RUNNING APPLICATION receives the new image and writes it into the bank-2
 * staging slot; the bootloader installs it on the next boot. Nothing here
 * touches the application slot, so an upload that is corrupt, truncated or
 * simply abandoned costs the device nothing — it carries on running the
 * firmware it booted with, and the worst outcome is a wasted transfer.
 *
 * Staging being in bank 2 is what makes this cheap: read-while-write means
 * programming it does not stall the core, so CAN keeps being serviced and the
 * 7.37 Mbaud receive ring keeps draining for the whole download. An update
 * staged into bank 1 would stall on every doubleword and reintroduce exactly
 * the frame corruption that drove the move to dual-bank in the first place.
 *
 * Sequence: BEGIN (declare and erase) -> DATA x N (program) -> END (verify and
 * arm) -> RESET_DEVICE. STATUS is readable at any time and is how the host
 * finds out what the bootloader did with the last attempt.
 */

typedef struct {
    uint32_t image_size;    /* total bytes, 8-byte aligned, <= FW_STAGING_SIZE */
    uint32_t image_crc32;   /* what END will check the staged bytes against */
    uint16_t product_id;    /* FW_PRODUCT_CAN_TRIPLE; refused otherwise */
    uint16_t version_major;
    uint16_t version_minor;
    uint16_t version_patch;
} FwUpdateBeginPayload;     /* 16 bytes */

/* DATA carries a 4-byte offset followed by the bytes. Offset and length must
 * both be 8-byte aligned — this part programs 64 bits at a time — and the run
 * must lie inside the size declared by BEGIN. Re-sending a chunk is safe: the
 * flash driver skips doublewords that already hold the target value, so a
 * retransmit after a lost ACK does not PROGERR the way a blind reprogram
 * would. */
typedef struct {
    uint32_t offset;        /* from FW_STAGING_BASE */
    /* uint8_t data[]; follows, length implied by the frame */
} FwUpdateDataHeader;       /* 4 bytes */

typedef struct {
    uint8_t  bootloader_version;   /* 0 = no bootloader present on this unit */
    uint8_t  state;                /* FW_STATE_* from fw_image.h */
    uint8_t  attempts;             /* commit attempts against the staged image */
    uint8_t  last_result;          /* FW_RESULT_* — why the last commit failed */
    uint32_t staged_size;
    uint32_t staged_crc32;
    uint16_t running_major;
    uint16_t running_minor;
    uint16_t running_patch;
    uint16_t running_store_version; /* FLASH_STORE_VERSION of the running image */
    uint32_t app_base;              /* where the running image is linked */
    uint32_t staging_capacity;      /* largest image this unit can accept */
    uint8_t  staged_valid;          /* 1 when staging holds a fully valid image */
    uint8_t  reserved[3];
} FwUpdateStatus;           /* 32 bytes */

#pragma pack(pop)

/* CRC16-CCITT-FALSE, identical to v1. */
static inline uint16_t compute_crc16(const uint8_t *data, uint32_t length)
{
    uint16_t crc = 0xFFFF;
    for (uint32_t i = 0; i < length; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (uint8_t j = 0; j < 8; j++)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021)
                                 : (uint16_t)(crc << 1);
    }
    return crc;
}

#endif /* PROTOCOL_H */
