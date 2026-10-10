/* The CAN Triple 2.0 unit's own answers about a plan, for test_plan_v2: the
 * capacity report it sends, what it would answer a plan's ask
 * (engine_plan_verdict, and the room its names take), and the dependency
 * references its SAVE counts, from the real store and engine (firmware/v2/src)
 * compiled for the host, against RAM models of the configuration flash and its
 * staging copy (firmware/v2/test/test_engine_plan.c's harness).
 *
 * A library of its own, built with only the 2.0 include directory: the 2.0
 * headers define the names the Manager's mirror does (CMD_*, the records), so
 * the two meet only through these plain functions. */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "engine_core.h"
#include "flash_store.h"
#include "label_store.h"
#include "retained_store.h" /* PreserveEntry */

/* ------------------------------------------------- the flash and staging */

#define PAGE 2048u
static uint8_t g_run[FLASH_STORE_CAPACITY];
static uint8_t g_stage[FLASH_STORE_CAPACITY];

static bool nor(uint8_t *region, uint32_t offset, const uint8_t *data, uint32_t length)
{
    if (offset + length > FLASH_STORE_CAPACITY || (offset & 7u) || (length & 7u))
        return false;
    for (uint32_t i = 0; i < length; ++i)
        region[offset + i] &= data[i];
    return true;
}

static bool runErase(void)
{
    memset(g_run, 0xFF, sizeof(g_run));
    return true;
}

static bool runEraseRange(uint32_t offset, uint32_t length)
{
    if (offset % PAGE || length % PAGE || offset + length > FLASH_STORE_CAPACITY)
        return false;
    memset(&g_run[offset], 0xFF, length);
    return true;
}

static bool runProgram(uint32_t offset, const uint8_t *data, uint32_t length)
{
    return nor(g_run, offset, data, length);
}

static const uint8_t *runData(void) { return g_run; }

static bool stageErase(void)
{
    memset(g_stage, 0xFF, sizeof(g_stage));
    return true;
}

static bool stageProgram(uint32_t offset, const uint8_t *data, uint32_t length)
{
    return nor(g_stage, offset, data, length);
}

static const uint8_t *stageData(void) { return g_stage; }

static const FlashStoreDriver kRun = {runErase, runProgram, runData, runEraseRange, PAGE};
static const FlashStoreDriver kStage = {stageErase, stageProgram, stageData, NULL, 0u};

static bool hostTransmit(uint8_t bus, uint32_t id, uint8_t ext, uint8_t fd, const uint8_t *data,
                         uint8_t len, uint32_t *ticket)
{
    (void)bus;
    (void)id;
    (void)ext;
    (void)fd;
    (void)data;
    (void)len;
    if (ticket)
        *ticket = 1u;
    return true;
}

/* A start with nothing stored, on the default plan, with staging. */
static void boot(void)
{
    static bool uid_set;
    if (!uid_set) {
        static const uint8_t uid[CONFIG_UID_LEN] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
        flash_store_set_device_uid(uid);
        uid_set = true;
    }
    memset(g_run, 0xFF, sizeof(g_run));
    memset(g_stage, 0xFF, sizeof(g_stage));
    flash_store_init(&kRun);
    flash_store_set_staging(&kStage);
    static const EngineCallbacks cb = {.transmit_can = hostTransmit};
    engine_init(&cb);
    ControlCanPayload out[3];
    (void)engine_load_config(out);
}

/* ------------------------------------------------------- the answers */

static const CapacityReport k_report = CAPACITY_REPORT_INIT;

/* The capacity report, as the unit sends it and its .ctf carries it. */
const unsigned char *v2o_report(unsigned *size)
{
    *size = (unsigned)sizeof(k_report);
    return (const unsigned char *)&k_report;
}

unsigned v2o_table_count(void) { return FLASH_NUM_TABLES; }

/* The verdict a Send declaring this plan would get from the engine and the
 * store (0, ERR_OUT_OF_BOUNDS, ERR_NO_ROOM), its configuration flash and the
 * RAM of its slices. The names' room is the glue's to judge: v2o_plan_names. */
unsigned v2o_plan_verdict(const uint16_t *cap, uint32_t *flash, uint32_t *arena)
{
    return engine_plan_verdict(cap, flash, arena);
}

/* The room this plan's names take in a staging half: its names, a unit for
 * each channel, `value_names` (phase 4, step 3b) and `notes` slots (Config
 * Notes), as a Send lays them out. */
uint32_t v2o_plan_names(const uint16_t *cap, unsigned value_names, unsigned notes)
{
    LabelGeometry g;
    label_geometry(&g, LABEL_KIND_COUNT, cap[ENGINE_TABLE_MESSAGES], cap[ENGINE_TABLE_SIGNALS],
                   cap[ENGINE_TABLE_RELAYS], (uint16_t)value_names, (uint16_t)notes);
    return label_region_used(&g);
}

/* The values the unit keeps across power cycles for these records, counted by
 * its own walk (engine_retained_next) once they are staged, saved, activated
 * and loaded: `channels` float channels of no message, those whose flagged[]
 * byte is set carrying SIG_BITS_RETAIN, and the counters and integrators given.
 * Negative when the Send could not be put in place. */
long v2o_retained(uint16_t channels, const uint8_t *flagged, const void *counters, unsigned nc,
                  const void *integrators, unsigned ni)
{
    boot();
    uint16_t cap[FLASH_NUM_TABLES];
    memset(cap, 0, sizeof(cap));
    cap[ENGINE_TABLE_SIGNALS] = channels;
    cap[ENGINE_TABLE_COUNTERS] = (uint16_t)nc;
    cap[ENGINE_TABLE_INTEGRATORS] = (uint16_t)ni;
    static CanSignalConfig sigs[2048];
    if (channels > sizeof(sigs) / sizeof(sigs[0]))
        return -1;
    for (unsigned s = 0; s < channels; ++s) {
        CanSignalConfig *c = &sigs[s];
        memset(c, 0, sizeof(*c));
        c->factor = 1.0f;
        c->min_val = -1.0e9f;
        c->max_val = 1.0e9f;
        sig_set_header(c, SIG_MSG_NONE, 0, 1);
        sig_set_bits(c, 0, 32, SIGNAL_TYPE_FLOAT, 0, 0);
        sig_set_retain(c, flagged[s]);
    }
    if (!engine_clear_config())
        return -2;
    engine_begin_config();
    if (engine_stage_plan_check(cap) != 0u || !flash_store_set_plan(cap)) {
        engine_abandon_send();
        return -3;
    }
#define WRITE(table, n, arr) ((n) == 0u || engine_table_write((table), 0, (uint16_t)(n), (const uint8_t *)(arr)))
    if (!WRITE(ENGINE_TABLE_SIGNALS, channels, sigs) || !WRITE(ENGINE_TABLE_COUNTERS, nc, counters)
        || !WRITE(ENGINE_TABLE_INTEGRATORS, ni, integrators)) {
        engine_abandon_send();
        return -4;
    }
#undef WRITE
    ControlCanPayload setup[3];
    memset(setup, 0, sizeof(setup));
    if (!engine_save_config(setup))
        return -5;
    if (engine_send_staged()) {
        uint16_t crc = 0u;
        uint32_t need = 0u, have = 0u;
        if (!flash_store_staged_valid(&crc) || !engine_staged_fits(&need, &have))
            return -6;
        engine_suspend();
        if (!flash_store_activate())
            return -7;
        ControlCanPayload out[3];
        if (!engine_load_config(out))
            return -8;
    }
    long n = 0;
    uint32_t cursor = 0u;
    PreserveEntry e;
    while (engine_retained_next(&cursor, &e))
        ++n;
    return n;
}

/* What a SAVE needs of the engine's RAM for these records, whole: a staged
 * Send of `channels` float channels of no message, the messages given (at
 * least four messages' room, for the message terms) and the records given,
 * each table planned at exactly its count. Its slices (the plan's), its
 * references and (phase 4, step 4c) its transmit queues' least, as
 * engine_staged_fits counts them; `slices` gets the plan's slices. Negative
 * when the Send could not be staged. The records are the 2.0's, byte for byte
 * the Manager's structs. */
long v2o_save_need(uint16_t channels, const void *constants, unsigned nk, const void *t2x16,
                   unsigned n2, const void *t8x8, unsigned n8, const void *math, unsigned nm,
                   const void *counters, unsigned nc, const void *conditions, unsigned nd,
                   const void *messages, unsigned nmsg, unsigned long *slices_out)
{
    boot();
    uint16_t cap[FLASH_NUM_TABLES];
    memset(cap, 0, sizeof(cap));
    cap[ENGINE_TABLE_MESSAGES] = (uint16_t)(nmsg > 4u ? nmsg : 4u);
    cap[ENGINE_TABLE_SIGNALS] = channels;
    cap[ENGINE_TABLE_CONSTANTS] = (uint16_t)nk;
    cap[ENGINE_TABLE_TABLES_2X16_DEF] = cap[ENGINE_TABLE_TABLES_2X16_OUT] = (uint16_t)n2;
    cap[ENGINE_TABLE_TABLES_8X8_DEF] = (uint16_t)n8;
    cap[ENGINE_TABLE_TABLES_8X8_ROW] = (uint16_t)(n8 * TABLE_8X8_SITES);
    cap[ENGINE_TABLE_MATH] = (uint16_t)nm;
    cap[ENGINE_TABLE_COUNTERS] = (uint16_t)nc;
    cap[ENGINE_TABLE_CONDITIONS] = (uint16_t)nd;
    uint32_t slices = 0u;
    if (!engine_plan_ram(cap, &slices, NULL))
        return -1;
    if (!engine_clear_config())
        return -2;
    engine_begin_config();
    if (engine_stage_plan_check(cap) != 0u || !flash_store_set_plan(cap)) {
        engine_abandon_send();
        return -3;
    }
    static CanSignalConfig sigs[2048];
    if (channels > sizeof(sigs) / sizeof(sigs[0])) {
        engine_abandon_send();
        return -4;
    }
    for (unsigned s = 0; s < channels; ++s) {
        CanSignalConfig *c = &sigs[s];
        memset(c, 0, sizeof(*c));
        c->factor = 1.0f;
        c->min_val = -1.0e9f;
        c->max_val = 1.0e9f;
        sig_set_header(c, SIG_MSG_NONE, 0, 1);
        sig_set_bits(c, 0, 32, SIGNAL_TYPE_FLOAT, 0, 0);
    }
#define WRITE(table, n, arr) ((n) == 0u || engine_table_write((table), 0, (uint16_t)(n), (const uint8_t *)(arr)))
    const bool sent = WRITE(ENGINE_TABLE_MESSAGES, nmsg, messages)
                      && WRITE(ENGINE_TABLE_SIGNALS, channels, sigs)
                      && WRITE(ENGINE_TABLE_CONSTANTS, nk, constants)
                      && WRITE(ENGINE_TABLE_TABLES_2X16_DEF, n2, t2x16)
                      && WRITE(ENGINE_TABLE_TABLES_8X8_DEF, n8, t8x8)
                      && WRITE(ENGINE_TABLE_MATH, nm, math)
                      && WRITE(ENGINE_TABLE_COUNTERS, nc, counters)
                      && WRITE(ENGINE_TABLE_CONDITIONS, nd, conditions);
#undef WRITE
    ControlCanPayload setup[3];
    memset(setup, 0, sizeof(setup));
    uint16_t crc = 0u;
    uint32_t need = 0u, have = 0u;
    if (!sent || !engine_save_config(setup) || !flash_store_staged_valid(&crc)) {
        engine_abandon_send();
        return -5;
    }
    (void)engine_staged_fits(&need, &have);
    engine_abandon_send();
    if (need < ((slices + 1u) & ~1u))
        return -6;
    if (slices_out)
        *slices_out = slices;
    return (long)need;
}
