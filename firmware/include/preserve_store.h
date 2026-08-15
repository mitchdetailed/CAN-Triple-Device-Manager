/*
 * preserve_store.h — tiny two-page flash ring for "retained" counter values.
 *
 * Counters flagged COUNTERFLAG_PRESERVE keep their value across power cycles.
 * At most PRESERVE_MAX values are retained; the value set is written at a slow
 * cadence (the caller flushes every 60 s) into a 2-page append log.
 *
 * The whole design is tuned to MINIMISE FLASH ERASES, because the page erase is
 * the wear-limiting operation (~10 k cycle endurance). The levers, in order of
 * impact:
 *
 *   1. Change detection — preserve_sync() compares each value against what is
 *      already stored and appends ONLY the ones that changed. A flush in which
 *      nothing changed touches no flash at all (zero wear when idle).
 *   2. Small 8-byte records — one STM32 doubleword each, so a 2 KB page holds
 *      ~254 records: many generations of updates before the page must recycle.
 *   3. Minimal-carry compaction — when the active page fills, only the LATEST
 *      record per live key (<= PRESERVE_MAX) is carried into the other page, so
 *      the fresh page starts almost empty and lasts a long time before the next
 *      erase.
 *   4. Compact/erase only when genuinely full, and at most once per flush.
 *
 * Result: an erase happens roughly once per (254 - liveKeys) appends, i.e. once
 * every few thousand changed-value writes, spread across two pages. See
 * preserve_erase_count() to observe wear in the field.
 *
 * Power-loss safety: the page's commit word (seq + config tag) is programmed
 * LAST, after the carried-over records are in place, so a compaction interrupted
 * by power loss leaves the previous page as the newest valid one — the in-flight
 * flush is lost, nothing else. Each record carries a check byte so a torn append
 * is detected and the page repaired (recompacted) on the next open. A double-bit
 * ECC error from a torn doubleword is handled the same way the config store is:
 * the NMI resets the MCU; the store validates/reformats on the next boot. This
 * matches flash_store's robustness level and deliberately avoids ST's heavier
 * EEPROM-emulation ECC machinery.
 *
 * The flash backing is injected (like flash_store) so host tests exercise the
 * exact ring logic against a pair of RAM pages.
 */
#ifndef PRESERVE_STORE_H
#define PRESERVE_STORE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Retained-value ceiling. Small on purpose: fewer records per flush means the
 * active page holds far more generations before it must recycle, which is the
 * dominant lever on erase count. */
#define PRESERVE_MAX 20

/* Tagged value. Counter outputs are not all floats — a u32 counter can exceed
 * float32's exact-integer range (2^24) — so each record self-describes its type
 * (a SignalDataType from protocol.h) and stores the value in its native width. */
typedef union {
    float    f;
    uint32_t u;
    int32_t  i;
} PreserveVal;

typedef struct {
    uint16_t    key;   /* stable identity within a config (the counter index) */
    uint8_t     type;  /* SignalDataType of the value */
    PreserveVal val;
} PreserveEntry;

/* Two-page flash backing. Pages are addressed by index (0/1); off and len are
 * 8-byte aligned. erase_page is the ONLY wear-inducing operation. */
typedef struct {
    void (*read)(int page, uint32_t off, void *dst, uint32_t len);
    bool (*program)(int page, uint32_t off, const void *src, uint32_t len);
    bool (*erase_page)(int page);
    uint32_t page_size; /* bytes per page (2048 on the dual-bank G4) */
} PreserveDriver;

void preserve_init(const PreserveDriver *drv);

/* Boot / config-change entry. Adopts the newest valid page whose config tag ==
 * cfg_crc and loads its latest value per key into RAM (available via
 * preserve_get). Returns true when a matching store was found (values to
 * restore). Returns false — after formatting a fresh empty page tagged cfg_crc
 * — when the store is empty, corrupt, or tagged with a different config (the
 * "invalidate on config change" path). Erases at most one page. */
bool preserve_begin(uint16_t cfg_crc);

/* After preserve_begin: fetch a restored value by key; false if none stored. */
bool preserve_get(uint16_t key, PreserveEntry *out);

/* Erase to a fresh empty store tagged cfg_crc WITHOUT reading the old pages.
 * Unlike preserve_begin this never scans records, so it is safe to call for
 * recovery when a prior scan is suspected to have faulted on a torn/corrupt
 * record (the boot-loop guard uses it). */
void preserve_format(uint16_t cfg_crc);

/* Change-detected write of the current value set. Only entries that differ from
 * what is already stored are appended; if none changed, NO flash is touched.
 * When the deltas don't fit on the active page, the live set is compacted into
 * the other page and the vacated page erased — the sole steady-state erase.
 * Returns the number of records written (0 => no flash activity this call). */
int preserve_sync(const PreserveEntry *entries, int n);

/* --- wear observability ------------------------------------------------- */

/* Page erases since the store was formatted for the current config. Equal to
 * the number of compactions (+1 for the initial format). */
uint32_t preserve_erase_count(void);

/* Free record slots left on the active page (a compaction happens when the
 * next batch of deltas would exceed this). */
uint16_t preserve_records_free(void);

#ifdef __cplusplus
}
#endif

#endif /* PRESERVE_STORE_H */
