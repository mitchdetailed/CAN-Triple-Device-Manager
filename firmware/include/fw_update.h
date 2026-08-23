/*
 * fw_update — the application's half of the firmware update: receive an image
 * into the bank-2 staging slot, verify it, and arm the bootloader.
 *
 * The application does the downloading, not the bootloader. That is the whole
 * shape of this design: the bootloader has no serial port, no protocol and no
 * buffers, because the image that must never break is the one with the least
 * code in it. The cost is that a truly dead application cannot be rescued over
 * the wire — that needs SWD — and the benefit is that the rescuer itself has
 * almost no surface to be broken by.
 *
 * Nothing here can damage the running firmware. Every write goes to the
 * staging slot in bank 2; the application slot is not touched by this file at
 * all. A transfer that is corrupt, truncated or simply abandoned leaves the
 * device running exactly what it was running before, and the only cost is the
 * time spent.
 */
#ifndef FW_UPDATE_H
#define FW_UPDATE_H

#include <stdbool.h>
#include <stdint.h>

#include "fw_image.h"
#include "protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Same shape as BcbDriver, and on the device it is wired to the same two
 * functions. Kept as its own type so the host tests can hand this module a
 * RAM-backed fake without also faking the boot control block. */
typedef struct {
    bool (*erase)(uint32_t addr, uint32_t len);
    bool (*program)(uint32_t addr, const void *src, uint32_t len);
} FwUpdateDriver;

void fw_update_init(const FwUpdateDriver *driver);

/* Declare an incoming image and erase the staging slot to receive it. Erases
 * only as many pages as the declared size needs, which for a typical image is
 * a few hundred milliseconds rather than the 1.7 s a full-slot erase costs —
 * still far longer than the default command timeout, so the host must use its
 * flash timeout here. Returns ERR_OK or a NACK code. */
uint8_t fw_update_begin(const FwUpdateBeginPayload *begin);

/* Program one chunk. offset and len must both be 8-byte aligned and the run
 * must lie within the declared size. Re-sending a chunk already written is
 * explicitly allowed — that is what a retransmit after a lost ACK looks like,
 * and the driver skips doublewords that already hold the right value. */
uint8_t fw_update_data(uint32_t offset, const uint8_t *data, uint16_t len);

/* Verify the staged image and arm the bootloader. Validates independently of
 * anything the host claimed: full header check plus a CRC32 over every staged
 * byte. Only on success is the boot control block written. */
uint8_t fw_update_end(void);

/* Cancel: invalidate the staged image and clear any pending flag. Erases the
 * first staging page, which removes the header magic — leaving the flag clear
 * alone would not be enough, because the bootloader installs a valid staged
 * image on its own initiative when the application slot is damaged. */
uint8_t fw_update_abort(void);

/* Fill a status report: what bootloader is present, what is staged, what the
 * bootloader made of the last commit, and what this running image is. */
void fw_update_status(FwUpdateStatus *out);

#ifdef __cplusplus
}
#endif

#endif /* FW_UPDATE_H */
