// Host test for firmware/src/preserve_store.c — the 2-page retained-value ring.
// Exercises the ring logic against a pair of RAM "flash" pages, checking the
// wear-reduction behaviour (change detection, minimal-carry compaction), the
// invalidate-on-config-change path, cross-reboot persistence, and power-loss
// (commit dropped mid-compaction) safety.
//
// Build: added as the `test_preserve` target in the top-level CMakeLists.txt.

#include <cstdint>
#include <cstdio>
#include <cstring>

extern "C" {
#include "preserve_store.h"
}

// ---- SignalDataType values mirrored from firmware/include/protocol.h --------
static const uint8_t TYPE_U32 = 0x04;
static const uint8_t TYPE_I32 = 0x14;
static const uint8_t TYPE_FLOAT = 0x34;

// ---- RAM flash model --------------------------------------------------------
// Both geometries the firmware can run on: 2 KB dual-bank pages and 4 KB
// single-bank pages. Sized for the larger; g_page selects the active one.
static const uint32_t PAGE_MAX = 4096;
static uint32_t g_page = 2048;
static uint8_t g_pages[2][PAGE_MAX];
static uint32_t g_program_calls;
static bool g_drop_commit;   // simulate power loss: fail the commit doubleword
static bool g_write_once_ok;

static void reset_flash()
{
    memset(g_pages, 0xFF, sizeof(g_pages));
    g_program_calls = 0;
    g_drop_commit = false;
    g_write_once_ok = true;
}

static void t_read(int page, uint32_t off, void *dst, uint32_t len)
{
    memcpy(dst, &g_pages[page][off], len);
}

static bool t_program(int page, uint32_t off, const void *src, uint32_t len)
{
    // Real flash can only clear bits and programs each doubleword once per
    // erase: enforce that the target is still erased (0xFF) so a double-program
    // regression is caught here.
    for (uint32_t i = 0; i < len; ++i)
        if (g_pages[page][off + i] != 0xFF)
            g_write_once_ok = false;
    // The commit word sits at offset 8; drop it to model a power loss mid-write.
    if (g_drop_commit && off == 8)
        return false;
    memcpy(&g_pages[page][off], src, len);
    ++g_program_calls;
    return true;
}

static uint32_t g_erases;
static bool t_erase(int page)
{
    memset(&g_pages[page], 0xFF, g_page);
    ++g_erases;
    return true;
}

static PreserveDriver drv = {t_read, t_program, t_erase, 2048};

// ---- test harness -----------------------------------------------------------
static int g_fail;
#define CHECK(cond, msg)                                                        \
    do {                                                                        \
        if (!(cond)) {                                                          \
            printf("  FAIL: %s\n", msg);                                        \
            ++g_fail;                                                           \
        }                                                                       \
    } while (0)

static PreserveEntry entU(uint16_t key, uint32_t v)
{
    PreserveEntry e;
    e.key = key;
    e.type = TYPE_U32;
    e.val.u = v;
    return e;
}

static void reboot()
{
    // Simulate a power cycle: re-init the module (RAM state lost) but keep the
    // flash pages. A real boot then calls preserve_begin.
    preserve_init(&drv);
}

static int runSuite(uint32_t pageSize)
{
    g_page = pageSize;
    drv.page_size = pageSize;
    const int failBefore = g_fail;
    printf("  -- %u-byte pages --\n", (unsigned)pageSize);

    // --- format + empty ------------------------------------------------------
    reset_flash();
    preserve_init(&drv);
    g_erases = 0;
    bool restored = preserve_begin(0x1111);
    CHECK(!restored, "empty store restores nothing");
    CHECK(g_erases == 1, "format erases exactly one page");
    PreserveEntry got;
    CHECK(!preserve_get(0, &got), "no value before any write");
    CHECK(preserve_erase_count() == 1, "erase count 1 after format");

    // --- append + change detection ------------------------------------------
    PreserveEntry batch[3] = {entU(0, 10), entU(1, 20), entU(2, 30)};
    int w = preserve_sync(batch, 3);
    CHECK(w == 3, "first sync writes all three");
    w = preserve_sync(batch, 3);
    CHECK(w == 0, "unchanged sync writes nothing (change detection)");
    batch[1].val.u = 21;
    w = preserve_sync(batch, 3);
    CHECK(w == 1, "only the changed value is appended");
    CHECK(preserve_get(1, &got) && got.val.u == 21, "latest value read back");
    CHECK(g_erases == 1, "steady-state appends cause no erase");

    // --- persistence across reboot (no erase on resume) ---------------------
    uint32_t erasesBefore = g_erases;
    reboot();
    restored = preserve_begin(0x1111);
    CHECK(restored, "store restores after reboot");
    CHECK(preserve_get(0, &got) && got.val.u == 10, "key 0 survived reboot");
    CHECK(preserve_get(1, &got) && got.val.u == 21, "key 1 latest survived reboot");
    CHECK(g_erases == erasesBefore, "resuming an existing page does not erase");

    // --- config change invalidates ------------------------------------------
    reboot();
    restored = preserve_begin(0x2222); // different config tag
    CHECK(!restored, "different config tag invalidates the store");
    CHECK(!preserve_get(0, &got), "old values gone after config change");

    // --- preserve_format: recovery wipe without scanning --------------------
    reset_flash();
    preserve_init(&drv);
    preserve_begin(0x6666);
    PreserveEntry fe = entU(5, 55);
    preserve_sync(&fe, 1);
    preserve_format(0x6666); // boot-loop-guard recovery path: erase, no scan
    CHECK(!preserve_get(5, &got), "preserve_format clears all values");
    reboot();
    CHECK(!preserve_begin(0x6666), "formatted store restores nothing");
    CHECK(preserve_records_free() > 250, "formatted store is empty");

    // --- compaction: many writes, few erases (wear reduction) ---------------
    reset_flash();
    preserve_init(&drv);
    g_erases = 0;
    preserve_begin(0x3333); // format (1 erase)
    // Change one key 1000 times. A 2 KB page holds ~254 records, so this must
    // compact a handful of times — NOT erase per write. Sample the free space
    // right after each compaction to confirm the fresh page carries only the
    // (tiny) live set.
    uint32_t prevErase = preserve_erase_count();
    uint16_t freeAfterCompaction = 0;
    for (uint32_t v = 1; v <= 1000; ++v) {
        PreserveEntry e = entU(7, v);
        preserve_sync(&e, 1);
        if (preserve_erase_count() != prevErase) { // a compaction just happened
            prevErase = preserve_erase_count();
            freeAfterCompaction = preserve_records_free();
        }
    }
    CHECK(preserve_get(7, &got) && got.val.u == 1000, "latest value after 1000 writes");
    // 1 format + a few compactions. Way fewer than 1000 — proves the append-log
    // plus minimal-carry compaction is doing its job.
    printf("  info: %u erases for 1000 changed writes\n", g_erases);
    CHECK(g_erases <= 6, "1000 writes cost only a few erases");
    CHECK(g_write_once_ok, "no doubleword was ever programmed twice");
    CHECK(freeAfterCompaction > 240, "fresh page carries only the live set after a compaction");

    // value survives reboot after compactions
    reboot();
    preserve_begin(0x3333);
    CHECK(preserve_get(7, &got) && got.val.u == 1000, "compacted value survives reboot");

    // --- typed values: signed + float round-trip ----------------------------
    reset_flash();
    preserve_init(&drv);
    preserve_begin(0x4444);
    PreserveEntry mixed[2];
    mixed[0].key = 3;
    mixed[0].type = TYPE_I32;
    mixed[0].val.i = -12345;
    mixed[1].key = 4;
    mixed[1].type = TYPE_FLOAT;
    mixed[1].val.f = 3.14159f;
    preserve_sync(mixed, 2);
    reboot();
    preserve_begin(0x4444);
    CHECK(preserve_get(3, &got) && got.type == TYPE_I32 && got.val.i == -12345,
          "signed value round-trips");
    CHECK(preserve_get(4, &got) && got.type == TYPE_FLOAT && got.val.f == 3.14159f,
          "float value round-trips");

    // --- power loss during compaction: previous page stays authoritative ----
    reset_flash();
    preserve_init(&drv);
    preserve_begin(0x5555);
    // Fill the active page so the next changed write must compact.
    uint32_t lastGood = 0;
    for (uint32_t v = 1; preserve_records_free() > 0; ++v) {
        PreserveEntry e = entU(9, v);
        preserve_sync(&e, 1);
        lastGood = v;
    }
    CHECK(preserve_records_free() == 0, "active page filled");
    // Now the compaction's commit word will be dropped (power loss).
    g_drop_commit = true;
    PreserveEntry e = entU(9, 999999);
    preserve_sync(&e, 1); // triggers compaction; commit fails
    g_drop_commit = false;
    reboot();
    restored = preserve_begin(0x5555);
    CHECK(restored, "store still valid after interrupted compaction");
    CHECK(preserve_get(9, &got) && got.val.u == lastGood,
          "interrupted compaction keeps the last committed value, drops only the in-flight one");

    // --- failed compaction retried while RUNNING (shadow ahead of flash) ----
    // Same failure shape as above, but the device keeps running instead of
    // rebooting. The deltas were folded into the RAM shadow before the
    // compaction failed, so change detection — which compares against the
    // shadow — sees nothing left to do; without a pending-compaction retry the
    // value the engine believes is preserved never reaches flash until it
    // happens to change again, and a later power cycle loses it.
    reset_flash();
    preserve_init(&drv);
    preserve_begin(0x7777);
    for (uint32_t v = 1; preserve_records_free() > 0; ++v) {
        PreserveEntry fill = entU(11, v);
        preserve_sync(&fill, 1);
    }
    g_drop_commit = true;
    PreserveEntry owed = entU(11, 424242);
    preserve_sync(&owed, 1); // compaction fails; 424242 lives only in the shadow
    g_drop_commit = false;
    preserve_sync(&owed, 1); // unchanged — but the store still owes it to flash
    reboot();
    CHECK(preserve_begin(0x7777), "store valid after retried compaction");
    CHECK(preserve_get(11, &got) && got.val.u == 424242,
          "value folded into a failed compaction reaches flash on the next sync");

    return g_fail - failBefore;
}

int main()
{
    printf("test_preserve\n");
    // The retained store now works in BOTH flash modes, so the ring has to be
    // PROVEN page-size agnostic rather than assumed: 2 KB pages are the
    // dual-bank layout, 4 KB the single-bank one.
    runSuite(2048);
    runSuite(4096);
    if (g_fail == 0)
        printf("test_preserve: ALL PASS\n");
    else
        printf("test_preserve: %d FAILURE(S)\n", g_fail);
    return g_fail ? 1 : 0;
}
