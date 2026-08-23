/* Hardware random numbers, for the configuration-password unlock challenge.
 *
 * The challenge is a nonce the device has never issued before. If it repeated,
 * a serial capture of one successful unlock could be replayed to open the
 * device again — which is the whole reason the exchange is challenge-response
 * rather than "send me the verifier".
 *
 * Source: the STM32G4's TRNG peripheral, clocked from HSI48. HSI48 is a
 * standalone RC oscillator, so enabling it leaves the PLL — and therefore the
 * 170 MHz core clock, FDCAN bit timing and the 7.3728 Mbaud USART — completely
 * alone. The RNG could not have been clocked from this PLL anyway: the VCO runs
 * at 340 MHz and PLLQ divides only by 2/4/6/8.
 *
 * rng_bytes returns false rather than filling the buffer with anything
 * predictable. A caller that cannot get randomness must refuse to issue a
 * challenge, not issue a guessable one.
 */
#ifndef CT_RNG_H
#define CT_RNG_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Brings up the TRNG. Safe to call once at boot; returns false if the
 * peripheral will not start, after which rng_bytes always fails. */
bool rng_init(void);

/* Fills dst with length random bytes. False on any hardware fault (seed or
 * clock error), leaving dst zeroed rather than partly filled. */
bool rng_bytes(uint8_t *dst, uint16_t length);

#ifdef __cplusplus
}
#endif

#endif /* CT_RNG_H */
