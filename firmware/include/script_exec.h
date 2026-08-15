/*
 * script_exec — the device script interpreter.
 *
 * Runs verified bytecode (script_vm.h) inside the engine tick. Compiled for
 * BOTH the device and the desktop simulator: the simulator is this same file,
 * so "simulate on the desktop, trust it on the device" is a property of the
 * build rather than a promise about two implementations.
 *
 * It implements INTERPRETER REQUIREMENTS R1-R5 from script_vm.h. Those are not
 * advisory — the verifier deliberately admits infinite self-jumps, so the
 * liveness half of the safety argument lives entirely in this file:
 *
 *   R1  every instruction is charged script_op_cost(op)
 *   R2  the budget is tested BEFORE dispatch, never after
 *   R3  the budget is per tick, shared by all hooks, with no credit carried
 *   R4  state registers are snapshotted at hook entry and restored on a fault
 *   R5  a faulted script is suspended until the config is reloaded
 *
 * The interpreter performs NO bounds checks in its dispatch loop. That is
 * deliberate and safe: script_verify() proved every register, signal, state and
 * jump index in range before script_exec_load() accepted the image, so there is
 * provably nothing left to check. Never execute an image this module has not
 * verified.
 */
#ifndef SCRIPT_EXEC_H
#define SCRIPT_EXEC_H

#include <stdbool.h>
#include <stdint.h>

#include "protocol.h"    /* ScriptStatus — the CMD_SCRIPT_STATUS reply shape */
#include "script_vm.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Reading a signal slot and writing one. Injected rather than called directly
 * so the simulator can run a script against recorded or seeded values with no
 * engine present — the same shape as the flash-store and preserve drivers.
 * The VM only ever passes indices script_verify() bounded against max_signals,
 * so an implementation may index without re-checking. */
typedef struct {
    float (*read_signal)(uint16_t idx);
    void  (*write_signal)(uint16_t idx, float value);
} ScriptHost;

/* Point the interpreter at its host. Also clears all state (as a reload does).
 * Must be called before script_exec_load. */
void script_exec_init(const ScriptHost *host);

/* Verify and adopt a script image. `image` is the bytecode (header first),
 * `avail` the bytes actually present, `max_signals` the host's value-table
 * size. Returns the script_verify() code: on SCRIPT_OK the script is loaded and
 * will run, on anything else nothing is loaded and the reason is reported by
 * script_exec_status().
 *
 * Clears persistent state and the suspension (R5): a freshly loaded script
 * starts from zeroed state with a clean slate, which is what makes re-Sending a
 * fixed script the way to recover from a fault. */
uint8_t script_exec_load(const void *image, uint32_t avail, uint16_t max_signals);

/* Forget any loaded script and clear all state. */
void script_exec_clear(void);

/* Start a new tick's budget (R3). Call once per engine tick, before the hooks. */
void script_exec_begin_tick(void);

/* Run the on_tick hook, if a script is loaded, not suspended, and declares one.
 * Returns the fault code; SCRIPT_FAULT_NONE on a clean HALT or when there was
 * nothing to run. */
uint8_t script_exec_on_tick(void);

/* Fill a status report for CMD_SCRIPT_STATUS. */
void script_exec_status(ScriptStatus *out);

/* True when a verified script is loaded and has not faulted. */
bool script_exec_running(void);

/* Read one persistent state register (0 when out of range).
 *
 * For the desktop simulator's state view — seeing what a state machine is
 * actually holding is most of debugging one — and for the tests that prove R4's
 * rollback, which cannot be observed through the script itself: a faulted
 * script is suspended, and reloading it to look would clear the very state
 * being checked. */
float script_exec_state(uint16_t idx);

#ifdef __cplusplus
}
#endif

#endif /* SCRIPT_EXEC_H */
