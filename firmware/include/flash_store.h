/*
 * flash_store.h — flash-resident configuration store (v7).
 *
 * The engine runs its config DIRECTLY from flash rather than a RAM copy, so
 * the ~85 KB of config tables no longer occupy RAM. The region holds a small
 * header followed by fixed, 8-byte-padded record slots for each table:
 *
 *   [ header ][ messages[] ][ signals[] ][ math[] ][ conditions[] ]
 *            [ counters[] ][ timers[] ][ constants[] ][ relays[] ]
 *            [ tables2x16Def[] ][ tables2x16Out[] ]
 *            [ tables8x8Def[] ][ tables8x8Row[] ][ integrators[] ]
 *
 * Each slot sits at a fixed offset (base + index * padded_item_size), so any
 * record can be programmed independently — STM32 flash programs 64-bit
 * doublewords, each once per erase, and padded slots never share a doubleword.
 * A transaction is: erase (CLEAR) → program records (WRITE_*) → commit header
 * (SAVE). The header carries the per-table counts + bus setup + a CRC over the
 * header and all live records; an image with no/invalid header reads as empty
 * (so an interrupted upload boots to defaults).
 *
 * The flash driver is injected so host tests exercise the same layout against
 * a RAM buffer.
 */
#ifndef FLASH_STORE_H
#define FLASH_STORE_H

#include <stdbool.h>
#include <stdint.h>

#include "protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 128 KB of padded record slots (131072 = 32 * 4096). The worst-case image is
 * CFG_TOTAL in flash_store.c, which is generated from FLASH_TABLE_LIST and
 * asserted against this value — read it there rather than trusting a figure
 * quoted here, since every record-size change moves it (v14's conditions alone
 * added 2,400 B).
 *
 * 52 -> 96 KB at the capacity revision (1000 64-byte signals are 64,000 B on
 * their own). 96 -> 128 KB at flash map v2, for the script table: CFG_TOTAL
 * is 86,800 today, and the 32 KB the scripting plan reserves for bytecode
 * (docs/SCRIPTING-PLAN.md) lands the layout at 119,568 — the same ~11 KB
 * margin the 96 KB region had, kept deliberately: less made every record
 * change a region move. The room came out of the staging/app slots (152 ->
 * 120 KB each, fw_image.h), taken while re-provisioning meant one bench board
 * rather than a fleet.
 *
 * Growing the region did NOT bump FLASH_STORE_VERSION: tables are laid out
 * from the region's base and the base did not move, so a stored image is
 * byte-identical under either capacity and a device carries its configuration
 * across the map change. The version bumps when the script TABLE is added,
 * because FLASH_NUM_TABLES changes and every offset shifts.
 *
 * MUST stay a multiple of 4096: the single-bank layout places the region at
 * (top of flash - capacity) and erases it by 4 KB page number, so a capacity
 * that is not page-aligned would compute a page that starts below the region.
 * Both layouts are asserted in user_code.c. */
#define FLASH_STORE_CAPACITY 131072u
#define FLASH_STORE_MAGIC 0x33544143u /* "CAT3" */
/* Reset to 1 alongside PROTOCOL_VERSION: nothing has shipped, and the old
 * ladder numbered a history no customer has seen. Any previously stored image is
 * rejected rather than misread, so a device flashed with an earlier build falls
 * back to bus defaults on first boot and its configuration must be sent again.
 *
 * The header holds the AccessKeyRecord (13 B) and the configuration's version
 * number (2 B). It does NOT hold the fleet identity: that is compiled into the
 * firmware (fleet_identity.h) and so cannot be erased, re-badged over the wire,
 * or lost when the config region is cleared.
 *
 * Both header-resident items are inside its CRC: editing either in a flash dump
 * invalidates the image, which is weaker than a signature but real — a tampered
 * header does not boot, it reads as empty. */
/* v2: Advanced Math grew MathConfig 18 -> 24 bytes. The slot geometry did not
 * move (the slot was already PAD8(18) = 24), but imageCrc() hashes item_size
 * bytes per live record, so a v1 image holding any math records would fail the
 * CRC under this build anyway — rejected safely, but only for SOME configs.
 * The bump makes the rejection uniform: every v1 image reads as empty and the
 * host re-sends, instead of survival depending on whether the config happened
 * to use math. */
/* v3: the header gained DeviceChannelsConfig (where the firmware publishes
 * Device OnTime) and CounterConfig grew a rate_hz byte. Either alone would
 * invalidate a stored image -- the header layout moved, and imageCrc() spans
 * item_size bytes per record -- so the bump is doing what it has always done:
 * making the rejection uniform rather than leaving it to depend on whether a
 * particular configuration happened to use counters. */
/* v4: the capacity revision, and this is the bump that would matter even if the
 * store had no version field to spare. Every previous bump made an ALREADY-
 * DOOMED image fail uniformly — the CRC would have rejected it anyway, and the
 * version just made the rejection independent of which tables a config used.
 * This one is different in kind. MAX_MESSAGES 250 -> 500, MAX_SIGNALS 768 ->
 * 1000, MAX_TIMERS 20 -> 50, CanSignalConfig 48 -> 64, the 4x4 table replaced by
 * an 8x8 Def+Row pair: every table offset after `messages` MOVES. A v3 image
 * read by this build would not be caught as corrupt — the header CRC is computed
 * over records at the offsets THIS layout believes in, so the read would be
 * record-for-record MISALIGNED before the CRC ever ran, and what it hashed would
 * be one table's bytes interpreted as another's. The version check is what stops
 * that at the door, and it is the only thing that does: the field has sat at the
 * same offset in every layout this store has had, which is precisely why it can
 * still be trusted to describe a layout that has otherwise entirely moved. */
/* v5: the SCRIPT table. FLASH_NUM_TABLES 13 -> 14, which grows the header's
 * per-table `counts` array, which moves the record area and therefore every
 * table offset — the same misalignment hazard the v4 note above describes, and
 * the same reason the version must move with it. (Growing the region 96 -> 128
 * KB at flash map v2 did NOT need a bump: capacity is not layout, the base and
 * every offset stayed put, and a stored config read identically. This does.) */
/* v6: CanMessageConfig grew 10 -> 14 bytes to carry the per-message access key
 * behind the v20 message-protection flags. (Both the key and those flag names
 * are gone as of 2.3.0 — see the note below the version — but the four bytes
 * stay, which is why this history still matters.) Messages are the FIRST
 * record table, so every offset after them shifts: a v5 image read as v6 would
 * not merely fail its CRC, it would be misread record for record, with the
 * signal table starting in the middle of a message. The same misalignment
 * hazard as v4 and v5, and the same answer.
 *
 * The consequence, stated plainly because a user meets it: a unit updated to
 * this firmware reads its stored configuration as ABSENT and runs bus defaults
 * until one is sent again. That is the documented behaviour for a store-format
 * change, and Get Configuration already explains it. */
/* 2.3.0 RETIRED that per-message key and did NOT bump this. Read that twice
 * before "correcting" it: retiring a field normally is a layout change, and a
 * layout change normally must move the version or every table after `messages`
 * is misread record-for-record.
 *
 * It is safe here because the field was not REMOVED. `key[4]` became
 * `reserved[4]` in place, so CanMessageConfig is still 14 bytes and not one
 * offset moves. And removing it would have bought nothing: this store pads each
 * record row to 8, and PAD8(14) == PAD8(10) == 16, so a 10-byte record occupies
 * exactly the same flash a 14-byte one does. The choice was therefore between
 * zero bytes saved at the cost of every field unit losing its stored
 * configuration, or zero bytes saved at no cost. See CanMessageConfig in
 * protocol.h.
 *
 * The version stays 6 in BOTH directions — the GUI's EXPECTED_STORE_VERSION
 * too. A 2.2.1 image is read correctly by 2.3.0 and vice versa, which is the
 * whole point: the update ships as firmware only, with no reconfiguration. */
/* v7: the CAN diagnostic device channels. DeviceChannelsConfig went from a
 * single uint16 to signal_idx[DEVCH_COUNT] — 2 bytes to 62 — and it lives in
 * the HEADER, ahead of the record area. So this is the v4/v5/v6 hazard exactly:
 * every record offset shifts by 60 bytes, and a v6 image read under this build
 * would be misread record-for-record rather than merely failing its CRC, with
 * the message table starting 60 bytes into where the header now ends.
 *
 * Note what does NOT save a v6 image here, because it is the tempting argument:
 * OnTime's destination stayed at offset 0 of the struct, so the FIRST two bytes
 * still mean what they used to. That makes the old WIRE payload a valid prefix
 * of the new one (which is why the write handler can accept a short payload),
 * and it does nothing whatever for a stored image, where the problem is the 60
 * bytes that follow shoving every table along. Prefix compatibility on the wire
 * and layout compatibility in flash are different questions; this bump answers
 * the second one.
 *
 * Same user-visible consequence as v6: a unit updated to this firmware reads
 * its stored configuration as ABSENT and runs bus defaults until one is sent
 * again. Get Configuration explains it. */
/* v8: the transmit-CRC8 table (Crc8Config, 20 records) appended after the
 * script chunks. Appending to the END moves no existing table's offset, but
 * FLASH_NUM_TABLES grows the header's per-table `counts` array, which moves
 * the record area wholesale — the v5 hazard exactly, and the same answer: a
 * v7 image read under this build would be misread record-for-record, so the
 * version refuses it instead.
 *
 * Same user-visible consequence as v6/v7: a unit updated to this firmware
 * reads its stored configuration as ABSENT and runs bus defaults until one is
 * sent again. Get Configuration explains it. */
/* v9: the MCU health device channels. DeviceChannelsConfig grows DEVCH_COUNT
 * 31 -> 36, and that struct sits in the HEADER, ahead of the record area — the
 * v7 hazard exactly: ten extra bytes shove every table offset along, so a v8
 * image read under this build would be misread wholesale and is refused by
 * version instead.
 *
 * Same user-visible consequence as ever: one re-Send after the update. */
/* v10: MAX_CONDITIONS 100 -> 250. Conditions are the FOURTH table in
 * FLASH_TABLE_LIST, so the table grows 4,000 -> 10,000 bytes and every table
 * after it — counters, timers, constants, relays, both lookup-table pairs,
 * integrators, the script region and the CRC8 rules — shifts 6,000 bytes down.
 * That is the v4 hazard exactly: a v9 image read under this build would be
 * misread record-for-record before its CRC ever ran, so the version refuses it
 * instead.
 *
 * Triggered transmit rides along and costs nothing. It claimed three of the four
 * retired bytes inside CanMessageConfig in place, so item_size stays 14 and no
 * offset moves on its account — by itself it would not have needed a bump at
 * all. Shipping the two together means the field pays the re-Send once.
 *
 * Same user-visible consequence as ever: one re-Send after the update. */
/* v11: User Conditions grew a second expression. ConditionConfig goes 35 -> 56
 * bytes to carry a Reset alongside the Set, plus the mode, the two comparison
 * counts, the two joiner sets and the Momentary latch frequency; ConditionTerm
 * goes 10 -> 8 because input_b became a union, which is what kept the record at
 * 56 rather than 72 and kept MAX_CONDITIONS at 250.
 *
 * Both hazards apply at once, so this is the least ambiguous bump in the list.
 * The record SIZE changed, so imageCrc() hashes a different span per record
 * (the v2 rule); and conditions are the fourth table, so every table after them
 * shifts (the v4 rule). A v10 image would be misread twice over.
 *
 * v10 was BUILT but never released — it exists only on bench hardware — which
 * is why this is a bump rather than a redefinition of 10. Reusing the number
 * would leave those units silently misreading a stored configuration whose
 * layout no longer matches, and "nothing shipped" is not the same as "nothing
 * exists".
 *
 * Same user-visible consequence as ever: one re-Send after the update. */
#define FLASH_STORE_VERSION 11u

/* Config tables, in the order they are laid out in flash. Matches the engine's
 * EngineTable enum values (0..14) — 12 became 13 when the single 4x4 table was
 * replaced by the 8x8's Def + Row pair, 13 became 14 with the script bytecode
 * table, and 14 became 15 with the transmit-CRC8 rules. */
#define FLASH_NUM_TABLES 15

typedef struct {
    /* Erase the whole config region (all bytes -> 0xFF). */
    bool (*erase)(void);
    /* Program `length` bytes at byte `offset` inside the region. Offset and
     * length are 8-byte aligned; each location is programmed at most once
     * between erases — the store upholds that itself by reading before it
     * programs (a retransmitted chunk that already landed is skipped, never
     * re-programmed), so the driver stays a dumb "program onto erased flash"
     * wrapper. */
    bool (*program)(uint32_t offset, const uint8_t *data, uint32_t length);
    /* Directly readable pointer to the region (memory-mapped flash / RAM). */
    const uint8_t *(*data)(void);
} FlashStoreDriver;

void flash_store_init(const FlashStoreDriver *driver);

/* Per-table geometry (unpadded record size / max records). */
int flash_store_item_size(int table);
int flash_store_capacity(int table);

/* Erase the whole region (CLEAR): invalidates the image. */
bool flash_store_erase(void);

/* Program `count` records of `table` (packed, item_size each) into their slots
 * starting at index `start`. The region must have been erased first — but a
 * slot already holding EXACTLY these bytes is skipped and counts as success,
 * because the protocol retransmits a WRITE whose ACK went missing and the
 * retry must not double-program slots the first attempt filled (real flash
 * programs each doubleword once per erase and PROGERRs a second attempt). A
 * slot holding anything else fails the write: that is two different payloads
 * contending for one erase, not a retry. */
bool flash_store_write(int table, uint16_t start, uint16_t count, const uint8_t *src);

/* Copy `count` records of `table` from index `start` into `dst` (packed). */
bool flash_store_read(int table, uint16_t start, uint16_t count, uint8_t *dst);

/* Direct const pointer to record `index` of `table` in memory-mapped flash.
 * Returns NULL if no driver is set or the index is out of range. */
const void *flash_store_slot(int table, uint16_t index);

/* Commit the header (SAVE): writes magic/version/counts/bus_setup/name and a
 * CRC over the header + every live record, marking the image valid. `counts`
 * gives the number of records per table (in FLASH_NUM_TABLES order); `name` is
 * the CONFIG_NAME_LEN-byte configuration name (may be NULL for none). */
bool flash_store_commit(const uint16_t counts[FLASH_NUM_TABLES],
                        const ControlCanPayload bus_setup[3], const char name[CONFIG_NAME_LEN],
                        const AccessKeyRecord *access, uint16_t config_version,
                        const uint8_t bound_uid[CONFIG_UID_LEN],
                        const DeviceChannelsConfig *device_channels);

/* Validate the stored image (magic/version/CRC). On success, fills counts_out,
 * bus_setup_out, and name_out (any may be NULL) and returns true. */
bool flash_store_validate(uint16_t counts_out[FLASH_NUM_TABLES],
                          ControlCanPayload bus_setup_out[3], char name_out[CONFIG_NAME_LEN],
                          AccessKeyRecord *access_out, uint16_t *config_version_out,
                          uint8_t bound_uid_out[CONFIG_UID_LEN],
                          DeviceChannelsConfig *device_channels_out);

/* Tells the store which chip it is running on, for the v18 device binding. Call
 * once at init, before any validate. Without it a BOUND image is refused —
 * never accepted — so a build that forgets to wire this up fails closed. */
void flash_store_set_device_uid(const uint8_t uid[CONFIG_UID_LEN]);

/* Why the last flash_store_validate said no: one of CONFIG_STATUS_*. Lets the
 * device distinguish "no configuration" from "a configuration meant for a
 * different chip", which otherwise look identical from the outside. */
uint8_t flash_store_config_status(void);

/* True when the region holds a valid image. */
bool flash_store_present(void);

/* The stored image's header CRC — a compact identity for the current config,
 * used by preserve_store to tag retained values so a config change invalidates
 * them. Returns 0 when no valid image is present. */
uint16_t flash_store_config_crc(void);

#ifdef __cplusplus
}
#endif

#endif /* FLASH_STORE_H */
