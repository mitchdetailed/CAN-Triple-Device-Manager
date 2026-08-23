/*
 * engine_math — the arithmetic the engine and the script VM share.
 *
 * Split out of engine_core.c so the DESKTOP SIMULATOR can link the script VM
 * without dragging in the entire engine (and with it flash_store, the CAN
 * composer and every table walker). The configurator needs exactly these three
 * functions to run a script; it has no use for the rest, and a simulator that
 * pulled in the whole firmware would be one nobody could build.
 *
 * That is not the only reason, though. Having ONE implementation of the 31
 * MathOp operations is what makes three separate claims true at once:
 *
 *   - a math row's MULADD and a script's `a*b + c` compute the same thing,
 *     because they are the same code;
 *   - the desktop simulator's answer is the device's answer, because both
 *     compile this file rather than agreeing by inspection;
 *   - the CLAMP/WRAP opcodes and the counters' and integrators' limit handling
 *     round the same way, because clampRoll is shared rather than reimplemented.
 */
#ifndef ENGINE_MATH_H
#define ENGINE_MATH_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Saturating float -> int32 without undefined behaviour: NaN becomes 0 and
 * out-of-range values clamp. Used by the bitwise ops (AND/OR/XOR), which are
 * the only place the engine treats a float as an integer. */
int32_t engine_float_to_i32_sat(float x);

/* Clamp v into [lo, hi], or WRAP it into [lo, hi) when roll is set. `hi <= lo`
 * means "no limiting" and returns v untouched — the convention counters,
 * integrators and the CLAMP opcode all share.
 *
 * The wrap is a modulo, not a loop. A `while (v > hi) v -= span;` spins forever
 * at large |v|, because once the float ULP exceeds the span, `v - span == v`
 * (FIRMWARE-NOTES #9c). */
float engine_clamp_roll(float v, float lo, float hi, uint8_t roll);

/* The 31 MathOp operations (protocol.h), in one implementation.
 *
 * Returns false for an unknown op, leaving *out alone so the caller skips
 * rather than writing a fabricated 0.0f — which is what executeMath's original
 * `default: continue;` did, and what stops a config from a newer build silently
 * zeroing a channel. `c` is read only by the ternary ops (MULADD, CLAMP, LERP,
 * SELECT, WRAP) and ignored otherwise.
 *
 * Every operation here is one IEEE-754 requires to be correctly rounded, or a
 * comparison, or an exact integer bit op. There are deliberately NO
 * transcendentals: that is what lets the soft-float device and the host
 * simulator agree bit for bit (see DETERMINISM in script_vm.h), and it is why
 * the script compiler refuses sin/exp/pow by name. */
bool engine_math_eval(uint8_t op, float a, float b, float c, float *out);

#ifdef __cplusplus
}
#endif

#endif /* ENGINE_MATH_H */
