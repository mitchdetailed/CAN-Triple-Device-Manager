/*
 * engine_core.h — portable (HAL-free) gateway engine: message matching,
 * signal extraction/packing, math, conditions, routing decisions, and the
 * periodic transmit composer. Hardware I/O goes through EngineCallbacks so
 * the same code runs on the STM32 and inside host-side tests.
 */
#ifndef ENGINE_CORE_H
#define ENGINE_CORE_H

#include <stdbool.h>
#include <stdint.h>

#include "protocol.h"
#include "preserve_store.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* Physically transmit a frame on dest_bus (1..3). len up to 64.
     *
     * Returns whether the frame was actually ACCEPTED for transmission — false
     * when the outgoing queue is full and the frame has been dropped. Two
     * things depend on the answer and both used to be wrong without it:
     *
     *   - Tx Count, and the bus-load estimate under it, counted every frame the
     *     engine composed rather than every frame that went out. On a
     *     configuration asking for more than the bus can carry that is a
     *     diagnostic reporting throughput the device is not achieving, which is
     *     precisely the case somebody is looking at it to diagnose.
     *   - The transmit scheduler needs to know where it ran out of room, so it
     *     can resume there next tick instead of restarting at message 0 and
     *     serving the same few messages forever. */
    bool (*transmit_can)(uint8_t dest_bus, uint32_t can_id, uint8_t is_extended,
                         uint8_t is_fd, const uint8_t *data, uint8_t len);
} EngineCallbacks;

void engine_init(const EngineCallbacks *callbacks);
/* CLEAR_CONFIG: erase the flash image + reset state. Returns whether the erase
 * actually took — the runtime is dropped either way, but a caller that ACKs a
 * clear which silently did nothing sends the host off to program records into
 * flash the old configuration still occupies. See the definition. */
bool engine_clear_config(void);

/* Validate and activate the stored flash image (the boot path). On a
 * valid image, sets the active table counts and fills bus_setup_out (if
 * non-NULL); returns true. Otherwise leaves the engine empty and returns false. */
bool engine_load_config(ControlCanPayload bus_setup_out[3]);

/* Commit the just-uploaded config to flash (SAVE_TO_FLASH): writes the header
 * (counts + bus setup + name + CRC) marking the image valid. */
bool engine_save_config(const ControlCanPayload bus_setup[3]);

/* Configuration name (CONFIG_NAME_LEN bytes). Set by WRITE_CONFIG_NAME, saved
 * to flash on SAVE, and restored on load. engine_config_name() returns a
 * pointer to the internal CONFIG_NAME_LEN-byte buffer. */
void engine_set_config_name(const char *name);
const char *engine_config_name(void);

/* v19: the per-function access keys (protocol.h AccessKeyRecord), saved and
 * restored with the config. NULL clears every key. Survives engine_clear_config
 * on purpose — clearing the tables must not be a way to shed the protection. */
void engine_set_access_keys(const AccessKeyRecord *keys);
const AccessKeyRecord *engine_access_keys(void);

/* The stored configuration's version number — the one part of the fleet
 * identity that is NOT compiled in, because it has to move every time a
 * configuration is released. It arrives on CMD_SAVE_TO_FLASH with the commit it
 * belongs to and is read back with the image.
 *
 * Unlike the access keys this does NOT survive engine_clear_config(): the
 * version describes the configuration, and a device with no configuration is
 * running revision nothing. Reporting a stale version for tables that are no
 * longer there would make the host's "is this update newer?" check answer about
 * a config that does not exist. */
void engine_set_config_version(uint16_t version);
uint16_t engine_config_version(void);

/* v18: the chip this configuration is bound to (CONFIG_UID_LEN bytes). All zero
 * — the default — means it runs on any device. NULL clears it. Saved and
 * restored with the config, and like the lock it survives engine_clear_config.
 */
void engine_set_config_binding(const uint8_t uid[CONFIG_UID_LEN]);
const uint8_t *engine_config_binding(void);

/* Device channels: which value slots the firmware publishes its OWN values
 * into — Device OnTime, and the per-bus CAN diagnostics (REC, TEC, the three
 * bus-state flags, error frames, frame counts and bus load). Saved and restored
 * with the config like the name and the binding, and cleared by
 * engine_clear_config along with the tables that referenced the slots.
 *
 * The DESTINATIONS are configuration; the QUANTITIES are not. The clock, the
 * frame counters and the accumulated error count all keep running across a
 * clear or a re-send, and only a reset or a power cycle takes them back to zero
 * — clearing a configuration is not a power cycle, and a diagnostic counter that
 * restarted every time the host sent a config would be measuring the wrong
 * thing. A destination at or past MAX_SIGNALS is stored as "unused" rather than
 * kept and re-checked every tick. NULL clears. */
void engine_set_device_channels(const DeviceChannelsConfig *cfg);
const DeviceChannelsConfig *engine_device_channels(void);

/* Configuration tables (direct, index-checked access for the serial layer). */
typedef enum {
    ENGINE_TABLE_MESSAGES   = 0, /* receive AND transmit (direction is a flag) */
    ENGINE_TABLE_SIGNALS    = 1,
    ENGINE_TABLE_MATH       = 2,
    ENGINE_TABLE_CONDITIONS = 3,
    ENGINE_TABLE_COUNTERS   = 4,
    ENGINE_TABLE_TIMERS     = 5,
    ENGINE_TABLE_CONSTANTS  = 6, /* v6 */
    ENGINE_TABLE_RELAYS     = 7, /* v11: message relay (masked forward) rules */
    /* v13: the 1-axis 16-site lookup table, split across two parallel tables
     * indexed in lockstep (a combined record exceeds MAX_PADDED_RECORD). */
    ENGINE_TABLE_TABLES_2X16_DEF = 8,
    ENGINE_TABLE_TABLES_2X16_OUT = 9,
    /* The 2-axis 8x8 lookup table, which replaced the v12 4x4 (its
     * ENGINE_TABLE_TABLES_4X4 value is gone, not renumbered around). Split the
     * same way, but per ROW rather than in halves: the Def table holds
     * MAX_TABLES_8X8 records and the Row table MAX_TABLES_8X8 *
     * TABLE_8X8_SITES = 64, with table t owning rows t*8 .. t*8+7. These are
     * flash-layout indices as well as enum values, so the order here IS the
     * order in FLASH_TABLE_LIST. */
    ENGINE_TABLE_TABLES_8X8_DEF = 10,
    ENGINE_TABLE_TABLES_8X8_ROW = 11,
    ENGINE_TABLE_INTEGRATORS = 12, /* v16: rate accumulators */
    /* Device script bytecode as 64-byte chunks (script_vm.h). Not "records" the
     * engine iterates like the tables above — the whole table is ONE image, read
     * through a single pointer at chunk 0 because PAD8(64) == 64 makes the slots
     * byte-contiguous. Its used count is how many chunks the image occupies. */
    ENGINE_TABLE_SCRIPT = 13,
    ENGINE_TABLE_CRC8 = 14, /* transmit-CRC8 rules; stamped by the composer */
} EngineTable;

int engine_table_capacity(EngineTable table);     /* entries */
int engine_table_item_size(EngineTable table);    /* bytes per entry */
/* Both return false when [start, start+count) exceeds the table. */
bool engine_table_write(EngineTable table, uint16_t start, uint16_t count, const uint8_t *src);
bool engine_table_read(EngineTable table, uint16_t start, uint16_t count, uint8_t *dst);
/* The active PREFIX: how many records at the front of the table have been
 * written this session (0 when empty). Not "the last ACTIVE entry + 1", which
 * is what this comment used to claim — a table can hold ten written records of
 * which three carry an ACTIVE flag, and this returns ten. The distinction
 * matters because the serial layer uses this to refuse a write that would leave
 * a GAP in the prefix. */
uint16_t engine_table_used(EngineTable table);

/* Feed a received CAN frame (bus 1..3). Parses, runs math/conditions, and
 * routes via callbacks->transmit_can. */
void engine_process_can(uint8_t src_bus, uint32_t can_id, uint8_t is_extended,
                        uint8_t is_fd, const uint8_t *data, uint8_t data_len);

/* Inject a frame from the PC: processed as if received on bus_idx, then also
 * physically transmitted on bus_idx. */
void engine_inject(const InjectCanPayload *payload);

/* Periodic tick (call at 100 Hz with elapsed_ms = 10): runs math/conditions
 * on quiet buses and services the transmit composer. A composition of the two
 * halves below, kept whole for callers that drive everything as one clock —
 * the host tests foremost. */
void engine_tick(uint16_t elapsed_ms);

/* The two halves, for glue that runs them at DIFFERENT rates — which is how
 * the device reaches 200 Hz transmit. The calculation chain stays at 100 Hz
 * (its budgets and documentation mean per-tick at that rate); the transmit
 * scheduler runs at 5 ms resolution, so a cyclic message's period floor is
 * 5 ms. Drive engine_service_transmit with MEASURED elapsed milliseconds, not
 * counted calls, so a stalled dispatch slips no transmit time. */
void engine_tick_calc(uint16_t elapsed_ms);
void engine_service_transmit(uint16_t elapsed_ms);

/* --- retained (PRESERVE) values ------------------------------------------
 * A counter flagged COUNTERFLAG_PRESERVE or an integrator flagged
 * INTEGFLAG_PRESERVE keeps its output value across power cycles (persisted via
 * preserve_store). These bridge the engine's float value slots and the
 * tagged-union records: enumerate reads the current values (typed from each
 * output signal), seed writes a restored value back.
 *
 * Both tables share ONE uint16 key space so a single ring can hold either:
 * counters occupy 0..MAX_COUNTERS-1 and integrators start at
 * PRESERVE_KEY_INTEGRATOR_BASE. Keys only mean anything within one config, but
 * preserve_begin invalidates the whole store when the config CRC changes, so a
 * key can never reattach to a different slot after a re-map.
 *
 * PRESERVE_KEY_COUNT is the range a restore loop must walk. It is larger than
 * PRESERVE_MAX (20) on purpose: the key space covers every slot that COULD be
 * flagged, while the ring only ever holds the ones that are. */
#define PRESERVE_KEY_INTEGRATOR_BASE MAX_COUNTERS
#define PRESERVE_KEY_COUNT (MAX_COUNTERS + MAX_INTEGRATORS)

/* Fill out[] with one PreserveEntry per active PRESERVE counter and integrator
 * (type = the output signal's SignalDataType, val = current value in that
 * native type). Returns the count written (capped at max). */
int engine_preserve_enumerate(PreserveEntry *out, int max);

/* Seed a restored value into the slot its key identifies (no-op if that slot is
 * absent or not flagged PRESERVE). Call after engine_load_config at boot — it
 * deliberately runs after the integrator start_value seeding, so a retained
 * total wins over the configured starting point. */
void engine_preserve_seed(uint16_t key, uint8_t type, PreserveVal val);

/* Live values (MAX_SIGNALS = 1000 floats = 4,000 B of RAM, indexed like the
 * signal table). */
float engine_signal_value(uint16_t idx);
uint16_t engine_active_signal_count(void);
/* Iterate active signals: returns count written, fills idx/value pairs
 * starting from *cursor; sets *cursor past the last visited entry (wraps to
 * 0 at the end). Used by the value-stream emitter. */
uint16_t engine_collect_values(uint16_t *cursor, SignalValueEntry *out, uint16_t max);

void engine_fill_status(DeviceStatus *status, uint32_t uptime_ms);
/* Frame statistics from the glue layer. These carry the frame's GEOMETRY as
 * well as its bus because they feed the bus-load estimate as well as the frame
 * counts, and the shape of the frame is only known here — by the time the
 * engine has a value slot to publish into, the DLC and the ID width are gone.
 * A caller that only wants the count still passes them; there is no cheaper
 * overload, on purpose, so no call site can quietly stop feeding bus load.
 *
 * is_fd means BIT RATE SWITCHED, not merely "FD format". It selects which bits
 * are charged at the bus's data rate, and an FD frame sent without BRS has none
 * — every bit of it is clocked at the nominal rate exactly like a classic
 * frame's. Pass the frame's real BRS bit. Deriving it from the length instead is
 * a mistake in both directions, and a costly one: it under-reports a busy FD bus
 * by the ratio between the two rates, which is the direction that hides
 * saturation. */
void engine_count_rx(uint8_t bus, uint8_t is_extended, uint8_t is_fd, uint8_t data_len);
void engine_count_tx(uint8_t bus, uint8_t is_extended, uint8_t is_fd, uint8_t data_len);
void engine_set_bus_state(uint8_t bus, uint8_t state); /* 0 off / 1 active / 2 listen */

/* The bit rates a bus is running at, for the bus-load estimate. data_baud
 * matters only for CAN FD frames sent with BRS; pass it equal to nominal_baud
 * for a classic bus. Zero disables the estimate for that bus (the channel
 * publishes 0 rather than dividing by it). Call whenever the bus is
 * reconfigured — the engine cannot see setupCAN_bus from here. */
void engine_set_bus_bitrate(uint8_t bus, uint32_t nominal_baud, uint32_t data_baud);

/* One sample of a bus's FDCAN error state (protocol.h BusDiagnostics). Call
 * periodically from the glue; the engine latches the levels and accumulates
 * d->error_delta into the running error-frame total.
 *
 * Keep reporting bus-off for the WHOLE time a bus is down, including any period
 * the caller has deliberately stopped it in order to restart it. Passing the
 * peripheral's own view straight through would clear the flag the moment the
 * bus was stopped, and the channel would flicker rather than saying "down". */
void engine_set_bus_diagnostics(uint8_t bus, const BusDiagnostics *d);

/* One sample of the MCU's own health, for the DEVCH_MCU_* channels: die
 * temperature in °C and the analogue supply in volts. Call periodically from
 * the ADC glue; the engine latches the pair and tracks the since-boot
 * excursions (max temperature, min VDDA), which seed from the first sample
 * and — like the CAN error totals — survive a configuration clear. */
void engine_set_mcu_health(float temp_c, float vdda_v);

/* Why the last reset happened (protocol.h RESET_REASON_*), read from the RCC
 * flags once at boot and latched for the DEVCH_RESET_REASON channel. Values
 * past the enumeration store as UNKNOWN — the GUI shows these as names. */
void engine_set_reset_reason(uint8_t reason);

/* One completed restart of a bus after bus-off, for the Bus Off Recoveries
 * channel. Call it from whoever performs the restart, once per restart.
 *
 * This is counted by REPORT rather than inferred from the bus-off flag going
 * clear, and the difference is the whole reason the channel is trustworthy. A
 * restarted bus that is still faulty goes bus-off again in about five
 * milliseconds — thirty-two transmit errors at any real bit rate — which is
 * less than one 100 Hz tick. Watching for the flag to clear therefore sees
 * NOTHING on exactly the bus that is failing hardest, and the channel would sit
 * at zero while the device restarted the bus once a second all day. Counting
 * the restart itself is the only way the reading survives the case it exists
 * for.
 *
 * So the number means "times this bus was restarted after going bus-off", not
 * "times it came back and stayed". Whether it stayed is what the Bus Off flag
 * next to it answers. */
void engine_note_bus_restart(uint8_t bus);

/* What the device made of the stored script (CMD_SCRIPT_STATUS). A thin pass
 * through to script_exec_status(), so serial_proto need not know the VM exists
 * beyond this one call. */
void engine_script_status(ScriptStatus *out);

/* The shared MathOp evaluator moved to engine_math.h, which the script VM and
 * the desktop simulator link WITHOUT the rest of the engine. Included here so
 * every existing engine_core.h consumer still sees it. */
#include "engine_math.h"

/* Extraction helpers shared with tests. Bit S sits at byte S/8, bit S%8 (bits
 * 0..7 right-to-left per byte, bytes left-to-right from 0). The start bit is
 * the signal's LSB for BOTH byte orders; the walk ascends the bit within the
 * byte and steps to the next byte (Intel, byte_order 0) or the previous byte
 * (Motorola, byte_order 1) — the dbc_decode/dbc_encode convention. Both
 * directions validate the full span against data_len; a field that would step
 * outside the frame returns false (pack pre-walks, so no partial write). */
bool engine_extract_raw(const uint8_t *data, uint8_t data_len, uint16_t start_bit,
                        uint8_t bit_length, uint8_t byte_order, uint64_t *raw_out);
bool engine_pack_raw(uint8_t *data, uint8_t data_len, uint16_t start_bit,
                     uint8_t bit_length, uint8_t byte_order, uint64_t raw);

#ifdef __cplusplus
}
#endif

#endif /* ENGINE_CORE_H */
