/* A cycle counter for the script VM, and nothing else.
 *
 * The VM charges each instruction a synthetic COST and kills a hook that spends
 * too much (script_vm.h). Those units bound execution, but they are a currency
 * with no exchange rate: 1,240 units tells nobody whether a script is
 * comfortable or nearly out of time. Cycles do. At 170 MHz a tick is 1,700,000
 * of them, so "this script took 31,000" is "1.8% of a tick", which is a
 * sentence a person can act on.
 *
 * It also closes the loop on the cost model itself. SCRIPT_COST_HEAVY and
 * SCRIPT_TICK_BUDGET were originally estimated, and an early version of that
 * estimate was wrong by about six times because the arithmetic assumed a
 * gigahertz core. An estimate that can never be compared against a measurement
 * stays wrong quietly; this is the measurement.
 *
 * PORTABILITY. script_exec.c is compiled into the Windows configurator as well
 * as into the firmware — that is what makes the desktop simulator the device's
 * own VM rather than a model of it — so this header has to compile on a host
 * with no DWT and no CMSIS. On anything that is not ARM it reports "no counter"
 * and zero, which the host side then does not display. A host cycle count would
 * be worse than none: it would look like a device measurement and mean nothing
 * about the device.
 */
#ifndef SCRIPT_CYCLES_H
#define SCRIPT_CYCLES_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Start the counter. Safe to call more than once, and safe to call when the
 * counter is unavailable. */
void script_cycles_init(void);

/* True once a counter has been seen to actually advance. Not merely "the enable
 * bit was set": DWT can be present and gated, and a counter stuck at a constant
 * would otherwise be reported as a script that costs nothing at all — the most
 * reassuring possible reading of a broken instrument. */
bool script_cycles_available(void);

/* The free-running count. Meaningless in absolute terms; only differences mean
 * anything, and those are correct across the 32-bit wrap (~25 s at 170 MHz)
 * because unsigned subtraction is modular. Returns 0 when unavailable. */
uint32_t script_cycles_now(void);

#ifdef __cplusplus
}
#endif

#endif /* SCRIPT_CYCLES_H */
