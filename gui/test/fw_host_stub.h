/*
 * Host-side backing for the firmware-update modules, so test_firmware_link can
 * drive the REAL fw_update.c and bcb.c instead of a reimplementation.
 *
 * Those modules read the staging slot and the boot control block by absolute
 * flash address, and write through an injected driver. On the device both are
 * memory-mapped bank-2 flash; here they are one RAM array behaving the way NOR
 * flash behaves — erase sets 0xFF, program can only clear bits, and a second
 * program of the same cell to a DIFFERENT value fails. That last rule is not
 * pedantry: it is the exact behaviour that made retransmitted chunks poison
 * the config store before the drivers learned to skip writes that are already
 * correct, and a model that quietly allowed it would test nothing.
 */
#ifndef FW_HOST_STUB_H
#define FW_HOST_STUB_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Erase everything to 0xFF and clear the failure-injection counters. */
void fw_host_reset(void);

/* Driver functions matching BcbDriver / FwUpdateDriver. */
bool fw_host_erase(uint32_t addr, uint32_t len);
bool fw_host_program(uint32_t addr, const void *src, uint32_t len);

/* Direct access for test setup and inspection. Returns NULL for an address
 * outside the modelled region. */
uint8_t *fw_host_at(uint32_t addr, uint32_t len);

/* Make the next N program calls fail, to exercise the error paths. */
void fw_host_fail_programs_after(int n);

/* How many doublewords were actually written (skipped ones do not count), so a
 * test can prove that re-sending a chunk costs no flash writes. */
int fw_host_doublewords_written(void);

#ifdef __cplusplus
}
#endif

#endif /* FW_HOST_STUB_H */
