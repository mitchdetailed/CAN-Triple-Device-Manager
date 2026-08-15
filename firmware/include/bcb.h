/*
 * bcb — the boot control block: the handshake between the running application
 * and the bootloader.
 *
 * The application writes "there is an image in staging, install it" here and
 * reboots; the bootloader reads it, acts, and writes back what happened. It is
 * the only mutable state the two share, so it has to survive losing power at
 * any instant — including during its own update.
 *
 * HOW IT SURVIVES THAT
 *
 * Records are APPENDED, never overwritten. Flash cannot rewrite a word without
 * an erase, and the erase is exactly the moment the state would be missing —
 * so a naive "erase the page, write the new state" has a window in which a
 * power cut leaves the device with no idea what it was doing. Instead:
 *
 *   - two pages, 64 record slots each;
 *   - a write lands in the next free slot of whichever page is live;
 *   - when that page fills, the OTHER page is erased, the new record written
 *     there, and only THEN is the old page erased.
 *
 * At every instant at least one valid record exists somewhere, and the one
 * that counts is the highest seq whose own CRC checks out. A record half-
 * written when the power went is caught by that CRC and ignored, which
 * automatically falls back to the previous state — the correct answer, since
 * the transition never completed.
 *
 * SHARED BY BOTH IMAGES
 *
 * The bootloader and the application both need to read and write this, but
 * they reach flash through completely different drivers — the bootloader's
 * minimal bl_flash, the application's runtime-adaptive one in user_code.c. So
 * the driver is injected, the same way flash_store already does it, and there
 * is one implementation of the record format rather than two that must agree.
 */
#ifndef BCB_H
#define BCB_H

#include <stdbool.h>
#include <stdint.h>

#include "fw_image.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* Erase [addr, addr+len); both page-aligned. */
    bool (*erase)(uint32_t addr, uint32_t len);
    /* Program len bytes at addr; both 8-byte aligned. Must tolerate being
     * asked to write a value that is already there. */
    bool (*program)(uint32_t addr, const void *src, uint32_t len);
} BcbDriver;

void bcb_init(const BcbDriver *driver);

/* Read the current record. Returns false when there is none — a factory-fresh
 * device, or one whose block was just wiped. Callers must treat that as
 * "nothing pending", NOT as an error: a missing block is the normal state of a
 * device that has never been updated. */
bool bcb_read(BootControlRecord *out);

/* Append a new record. magic, seq and crc32 are filled in here; the caller
 * sets state, attempts, last_result and the staged_* fields. */
bool bcb_write(BootControlRecord *rec);

/* Convenience: record an outcome without disturbing the staged_* fields. */
bool bcb_set_result(uint8_t state, uint8_t attempts, uint8_t last_result);

#ifdef __cplusplus
}
#endif

#endif /* BCB_H */
