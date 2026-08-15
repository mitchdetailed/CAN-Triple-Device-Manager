/*
 * script_vm.h — the device script bytecode contract.
 *
 * COMPILED INTO ALL THREE COMPONENTS, like fw_image.h: the device VM, the
 * host compiler that emits bytecode, and the desktop simulator that runs the
 * same VM against recorded values. It defines the instruction format, the
 * limits, and the ONE verifier all three trust. A user's script is compiled on
 * the desktop (docs/SCRIPTING-PLAN.md — real Lua parses it, this is the target)
 * and shipped to the device as a configuration table; nothing here parses Lua.
 *
 * ---------------------------------------------------------------------------
 * WHAT THIS RUNS ON, AND WHY IT IS SHAPED THIS WAY
 * ---------------------------------------------------------------------------
 *
 * A script's on_tick hook runs inside engine_tick(), on a gateway that is also
 * servicing three CAN buses and a 7.37 Mbaud receive ring at 100 Hz. Two
 * properties are therefore non-negotiable, and the whole design falls out of
 * them:
 *
 *   1. It CANNOT run away. A buggy or hostile script must not be able to stall
 *      the tick. Enforced by a COST BUDGET spent in the interpreter loop: each
 *      instruction charges its script_op_cost() weight, and when a hook's
 *      spend reaches the budget, execution stops and the script is faulted.
 *      Liveness is a runtime property, bounded by spending. See INTERPRETER
 *      REQUIREMENTS below — the budget only works if the interpreter obeys them.
 *
 *   2. It CANNOT touch memory it should not. Every array access — register,
 *      signal slot, state slot, jump target — is proven in range by
 *      script_verify() BEFORE the first instruction executes. Safety is a
 *      static property, bounded by verification. The interpreter then does no
 *      bounds checks in its hot path because there is provably nothing to
 *      check; a verified program cannot make an out-of-range access.
 *
 * That split — budget for liveness, verifier for safety — is the same shape as
 * the bootloader (re-verify the image, then trust it) and is what lets the
 * interpreter be both small and fast.
 *
 * There is NO stack, NO heap, NO calls, NO recursion. A flat register machine
 * has nothing to overflow. Registers are floats, matching the engine's value
 * model (g_signal_values is float, the build is soft-float), so arithmetic is
 * the SAME evaluator executeMath() already uses — the VM inherits 31 tested
 * math operations rather than reimplementing them.
 *
 * ---------------------------------------------------------------------------
 * INTERPRETER REQUIREMENTS — the liveness half of the contract
 * ---------------------------------------------------------------------------
 *
 * The verifier deliberately ACCEPTS backward jumps and self-loops (JMP to its
 * own index passes cleanly — it is a legal, if useless, program). So the
 * verifier does NOT bound execution time; the interpreter does, and only if it
 * meets these requirements exactly. They are part of the contract because the
 * safety argument collapses without them:
 *
 *   R1. Charge EVERY instruction. Before dispatching each instruction, the
 *       interpreter adds script_op_cost(op) to the hook's running spend. No
 *       instruction is free — not JMP, not a taken JZ/JNZ, not HALT. An
 *       interpreter that charged only "work" ops would loop forever on a
 *       self-jump.
 *   R2. Test BEFORE dispatch. If the spend would meet or exceed the tick
 *       budget, stop: do not execute the instruction, set the fault, return.
 *       Testing after dispatch lets the last (possibly expensive) instruction
 *       run past the ceiling.
 *   R3. The budget is per TICK, shared across all hooks that run this tick, and
 *       carries no credit between ticks. A hook resumes from its entry next
 *       tick with the tick's fresh budget, never mid-instruction.
 *   R4. State is transactional across a fault. The interpreter snapshots the
 *       SCRIPT_NUM_STATE state registers at hook entry and, if the hook faults
 *       (budget or otherwise), restores them — so a killed hook cannot leave
 *       half-updated persistent state to corrupt the next tick. Signal writes
 *       (STORESIG) go straight to the value table and are NOT rolled back:
 *       buffering 1000 signals is not worth the RAM, and a faulted script is
 *       suspended and reported anyway, so downstream must treat a faulted
 *       script's outputs as stale. A clean HALT keeps everything.
 *   R5. A faulted script does not run again until the config is reloaded
 *       (SAVE/boot) — it is disabled and its fault code is readable, rather
 *       than re-run every tick to fault again.
 *
 * ---------------------------------------------------------------------------
 * DETERMINISM — why "simulate on the desktop, trust on the device" holds
 * ---------------------------------------------------------------------------
 *
 * The desktop simulator and the device must produce identical results from
 * identical bytecode, or the simulator is a lie. Two things guarantee it:
 *
 *   - The simulator compiles the SAME evaluator source (script_op_cost, the
 *     interpreter, and engine_math_eval) as the device — not a reimplementation
 *     and not a second math library.
 *   - The reused arithmetic op set (0x00..0x1E) is restricted to operations
 *     IEEE-754 requires to be correctly rounded: +, -, *, /, sqrt, fmod, plus
 *     min/max/abs/neg/floor/ceil/round and exact comparisons. There are NO
 *     transcendentals (no sin/cos/exp/log/pow), which are the ops libm may
 *     round differently on two targets. So the device's soft-float result and
 *     the host's SSE-single result agree bit-for-bit, and a following JZ/JNZ
 *     cannot branch differently. (x87 80-bit intermediates would break this;
 *     x86-64 uses SSE single precision, so they do not arise.)
 *   - LOADK rejects non-finite immediates (see the verifier), so a NaN or Inf
 *     cannot be injected as a constant to propagate non-portably.
 *
 * ---------------------------------------------------------------------------
 * INSTRUCTION FORMAT — 8 bytes, fixed, naturally aligned
 * ---------------------------------------------------------------------------
 *
 *     offset 0   op     opcode
 *     offset 1   dst    destination register (0..SCRIPT_NUM_REGS-1)
 *     offset 2   a      source register 1
 *     offset 3   b      source register 2
 *     offset 4   imm    u32: source register 3 (ternary ops, low byte) OR a
 *            ..7        float32 immediate OR a jump target OR a signal/state
 *                       index — one field, meaning set by the opcode
 *
 * Fixed width is the point. Instruction count is code_bytes / 8, decode needs
 * no length table, and a jump target is simply an instruction index the
 * verifier checks against that count. A variable-length encoding would make
 * "is this jump target the middle of an instruction?" a real question; here it
 * cannot be asked.
 *
 * The union of the last four bytes is deliberate: no instruction needs both a
 * third source register AND a 4-byte immediate. Ternary math ops
 * (MULADD/CLAMP/LERP/SELECT/WRAP) use op,dst,a,b and imm's low byte as the
 * third register; LOADK uses op,dst and imm as float bits; jumps use op,a and
 * imm as the target. Everything fits 8 bytes with the immediate 4-aligned.
 *
 * ---------------------------------------------------------------------------
 * OPCODES
 * ---------------------------------------------------------------------------
 *
 *   0x00 .. 0x1E  ARITHMETIC. The opcode value IS the MathOp (protocol.h): the
 *                 platform hands (op, reg[a], reg[b], reg[c]) to its shared
 *                 math evaluator and stores the result in reg[dst]. Binary ops
 *                 ignore c; unary ops ignore b and c. The firmware asserts this
 *                 range equals MathOp where it can see both (engine_core.c).
 *
 *   0x20 LOADK    reg[dst] = imm as float32
 *   0x21 LOADSIG  reg[dst] = signal[imm]         imm < max_signals
 *   0x22 STORESIG signal[imm] = reg[a]           imm < max_signals
 *   0x23 LOADST   reg[dst] = state[imm]          imm < num_state
 *   0x24 STOREST  state[imm] = reg[a]            imm < num_state
 *   0x25 MOV      reg[dst] = reg[a]
 *   0x26 JMP      pc = imm                       imm < instruction count
 *   0x27 JZ       if reg[a] == 0 then pc = imm
 *   0x28 JNZ      if reg[a] != 0 then pc = imm
 *   0x2F HALT     end this hook
 *
 * Comparisons are the arithmetic ops GT/GE/LT/LE/EQ/NE, which yield 1.0/0.0;
 * JZ/JNZ on their result give conditionals, so there is no compare-and-branch
 * opcode. Branches plus the persistent state registers are a state machine.
 *
 * The frame build/parse opcodes and the on_rx/on_tx hooks (SCRIPTING-PLAN.md
 * phase 3) are NOT in bytecode version 1. Their opcode range (0x40+) and their
 * entry slots exist in the header below so a version-1 device rejects a
 * version-2 image cleanly rather than misreading it.
 */
#ifndef SCRIPT_VM_H
#define SCRIPT_VM_H

#include <stdint.h>

#ifdef __cplusplus
#  define SCRIPT_STATIC_ASSERT(c, m) static_assert(c, m)
extern "C" {
#else
#  define SCRIPT_STATIC_ASSERT(c, m) _Static_assert(c, m)
#endif

/* --------------------------------------------------------------------------
 * Limits
 * -------------------------------------------------------------------------- */

#define SCRIPT_MAGIC              0x43535443u  /* 'CTSC' little-endian */

/* The bytecode format version this build EMITS and runs, and the oldest it
 * still accepts. Split so the version gate is a RANGE, not a hard-equals — the
 * same shape as the bootloader's min_bootloader_version. A device rejects an
 * image whose version is NEWER than it understands (SCRIPT_ERR_VERSION_NEW) or
 * OLDER than it still supports (SCRIPT_ERR_VERSION_OLD); everything between runs.
 * At v1 the two are equal, so only v1 is accepted — but the structure is right
 * for the v1->v2 transition, which is exactly where a hard-equals gate would
 * have wrongly rejected a still-valid older script and reported it as "too new".
 * Raise MIN only when support for an old format is genuinely dropped. */
#define SCRIPT_BYTECODE_VERSION       1u
#define SCRIPT_MIN_SUPPORTED_VERSION  1u

/* 64 scratch + 64 persistent state registers = 512 B of float RAM. Scratch is
 * cleared to 0 at the start of every hook invocation; state survives across
 * ticks (and, later, optionally across power cycles via the preserve ring —
 * SCRIPTING-PLAN.md open question). A register index is one byte, so the ceiling
 * could be 256; 64 is chosen so the whole register file is small and so a
 * malformed index (>= 64) is caught rather than addressing real state. */
#define SCRIPT_NUM_REGS           64u
#define SCRIPT_NUM_STATE          64u

/* One instruction is 8 bytes; see the format note above. */
#define SCRIPT_INSTR_SIZE         8u

/* The COST budget for ALL script hooks in one 100 Hz tick, together, in the
 * cost units script_op_cost() charges.
 *
 * Why cost, not raw instruction count: on this soft-float build a MOD, SQRT,
 * CLAMP or WRAP is a libm call an order of magnitude dearer than an add, so a
 * loop of 4000 MODs and a loop of 4000 adds have wildly different wall-clock —
 * counting instructions uniformly would let the expensive loop blow the tick
 * while the counter reported "within budget". Charging each op its weight makes
 * the budget bound WALL-CLOCK regardless of the op mix, which is the property
 * that actually matters.
 *
 * MEASURED ON SILICON (STM32G473 @ 170 MHz, soft-float, DWT cycle counter;
 * firmware/tools/hwtest/script_cost.py reproduces the whole table). Marginal
 * cycles per instruction, taken as the MINIMUM over many ticks so interrupt
 * time does not inflate the figure, and checked for linearity at N and 2N:
 *
 *     cheap op (mov / load / store / add / mul / cmp / min)   67 - 100
 *     DIV   (__aeabi_fdiv)                                       ~250
 *     FLOOR (floorf)                                             ~340
 *     SQRT  (sqrtf)                                              ~515
 *     WRAP / CLAMP (fmodf inside clampRoll)                      ~580
 *
 * Two things fell out of that, and the old constants had both wrong:
 *
 *   1. A "heavy" op is 4-7x a cheap one, not 20x. Charging 20 made the budget
 *      mean five different things depending on the op mix — 4000 units of adds
 *      ran 3.3 ms, 4000 units of WRAP ran 0.7 ms. Over-charging does not make a
 *      budget "safer" when the budget is the only thing between a runaway and
 *      the tick: it makes it imprecise, and the imprecision lands on the wrong
 *      side for the ops a runaway is most likely to be made of.
 *   2. DIV was charged 1 and costs 3, so a loop of divides overran the intended
 *      bound by 3x. It now has its own tier, SCRIPT_COST_DIVIDE.
 *
 * THE BUDGET DOES NOT BOUND WALL-CLOCK, AND CANNOT. An earlier version of this
 * comment claimed the tiers made the dearest unit ~100 cycles "whatever the
 * mix". That was measured on benign operands and it is false. fmodf on this
 * build is a shift-and-subtract loop running ONE BIT PER ITERATION, with the
 * count equal to the operands' exponent difference — so MOD, WRAP and CLAMP
 * cost whatever the script chooses to hand them, up to ~276 iterations.
 * Measured on the board: eight WRAPs over a narrow exponent spread cost 11,347
 * cycles, and the same eight over a wide one cost 26,711. No static charge can
 * bound a quantity chosen at run time, and charging the worst case would ration
 * the normal case by thirty times.
 *
 * So the budget's job is narrower than it looks, and correct within it: it is a
 * DETERMINISTIC, PORTABLE RATION ON WORK. It is identical on the device and in
 * the desktop simulator, which is what lets the script editor say "this fits"
 * and be believed. What bounds TIME is SCRIPT_CYCLE_CEILING, below.
 *
 * 2000 units is set so the two stops are the same order of magnitude — a
 * cheap-op runaway trips the budget at about the moment a WRAP-heavy one trips
 * the ceiling — and so that a part with no cycle counter, where the backstop is
 * inert, still has a bound worth having.
 *
 * The budget exists to stop a runaway, not to ration normal work: a real state
 * machine spends a few dozen units. The example script in the help page
 * measures 23 units / 6,272 cycles / 37 us — 0.4% of a tick. */
#define SCRIPT_TICK_BUDGET        2000u

/* Cost of a "heavy" arithmetic op — one that lowers to a libm call markedly
 * dearer than a soft-float add: SQRT, MOD, FLOOR, CEIL, ROUND, CLAMP, WRAP
 * (the last two via fmodf inside clampRoll). Measured at 4-7x a cheap op; 8 is
 * the next round number above that, so the charge still errs high without
 * distorting the budget the way 20 did. */
#define SCRIPT_COST_HEAVY         8u

/* Cost of a floating-point DIVIDE. Measurement put it between the two tiers
 * (~250 cycles against a cheap op's <=100), and it gets its own constant rather
 * than being rounded into a neighbour because it is the one op whose old charge
 * of 1 let a script exceed the budget's wall-clock bound outright. */
#define SCRIPT_COST_DIVIDE        4u

/* --------------------------------------------------------------------------
 * The cycle backstop — what ACTUALLY bounds the tick
 * --------------------------------------------------------------------------
 *
 * The cost table above cannot carry the wall-clock guarantee, and measurement
 * is what settled it. fmodf on this build is a shift-and-subtract loop running
 * one bit per iteration, with the count equal to the operands' exponent
 * difference — so WRAP, CLAMP and MOD cost whatever the SCRIPT decides to hand
 * them. Measured on the board: eight WRAPs over a small exponent spread cost
 * 11,347 cycles; the same eight over a wide one cost 26,711, and the spread
 * used was not even the worst fmodf allows. A static charge cannot bound a
 * quantity the script chooses at run time. Charging the worst case instead
 * would ration the normal case by 30x, which is not a fix, it is a different
 * failure.
 *
 * So the two jobs are split, and each is given to the mechanism that can
 * actually do it:
 *
 *   COST UNITS are a deterministic, portable RATION. They are identical on the
 *   device and in the desktop simulator, which is what lets the editor say "this
 *   script fits" and be believed. They bound WORK, not time.
 *
 *   THE CYCLE CEILING is the wall-clock bound. The interpreter samples the
 *   cycle counter every SCRIPT_CYCLE_CHECK ops and stops the hook when it has
 *   spent this many. It bounds the thing being bounded, measured as it happens,
 *   and it does not care what operands fmodf got, whether the instruction cache
 *   missed, or whether anyone found the worst case in advance.
 *
 * 170,000 cycles is 1 ms, a tenth of the 100 Hz tick.
 *
 * Overshoot is bounded and small: the check runs every 8 dispatches, so the
 * worst overrun is 8 further ops beyond the ceiling. At the dearest op measured
 * (~2,300 cycles for an adversarial WRAP) that is ~18,000 cycles, 0.11 ms — so
 * the real ceiling is ~1.11 ms. Checking every op would cost a DWT read on
 * every dispatch to buy back a tenth of a millisecond nobody can perceive.
 *
 * ON A PART WITH NO CYCLE COUNTER the backstop is inert (script_cycles_init
 * proves the counter moves before enabling it), and the cost budget is the only
 * bound left. That is the degraded mode, and it is why SCRIPT_TICK_BUDGET is
 * still set conservatively rather than being left wide open now that something
 * else carries the guarantee. */
#define SCRIPT_CYCLE_CEILING      170000u

/* Dispatches between cycle checks. A power of two so the test is a mask. */
#define SCRIPT_CYCLE_CHECK        8u

/* PHASE-3 NOTE, recorded now because it is an ABI decision the budget freezes:
 * the budget is a SINGLE pool shared by every hook that runs in a tick. In v1
 * only on_tick runs, so nothing competes for it. When phase 3 adds on_rx/on_tx,
 * a burst off the 7.37 Mbaud RX ring could spend the whole pool on on_rx before
 * on_tick — the safety-relevant hook — runs. Decide the policy then: a per-hook
 * sub-budget, or a reserved minimum for on_tick. It is called out here so the
 * choice is made deliberately rather than inherited by accident. */

/* Largest bytecode a device accepts, bounding the verifier's and the
 * interpreter's work. The script config table holds SCRIPT_MAX_CHUNKS 64-byte
 * chunks (see flash_store); this is that capacity minus the header, floored to
 * a whole instruction. Kept here as the authority the compiler checks against.
 * 8 KB = 1024 instructions, far more than a state machine needs and comfortably
 * inside the 32 KB script region the flash map reserves. */
#define SCRIPT_MAX_CODE_BYTES     8192u
#define SCRIPT_MAX_INSTRUCTIONS   (SCRIPT_MAX_CODE_BYTES / SCRIPT_INSTR_SIZE)

/* --------------------------------------------------------------------------
 * Opcodes
 * -------------------------------------------------------------------------- */

/* Arithmetic occupies 0x00..0x1E and mirrors MathOp exactly — the platform's
 * math evaluator is handed the opcode directly. Kept as a bound rather than a
 * duplicate of every MathOp name so this header need not include protocol.h;
 * the firmware asserts SCRIPT_OP_ARITH_MAX == MATH_OP_WRAP where both are
 * visible. */
#define SCRIPT_OP_ARITH_MAX       0x1Eu

enum {
    SCRIPT_OP_LOADK    = 0x20,
    SCRIPT_OP_LOADSIG  = 0x21,
    SCRIPT_OP_STORESIG = 0x22,
    SCRIPT_OP_LOADST   = 0x23,
    SCRIPT_OP_STOREST  = 0x24,
    SCRIPT_OP_MOV      = 0x25,
    SCRIPT_OP_JMP      = 0x26,
    SCRIPT_OP_JZ       = 0x27,
    SCRIPT_OP_JNZ      = 0x28,
    SCRIPT_OP_HALT     = 0x2F,
    /* 0x40+ reserved for phase-3 frame ops; rejected by a v1 verifier. */
};

typedef struct {
    uint8_t  op;
    uint8_t  dst;
    uint8_t  a;
    uint8_t  b;
    uint32_t imm;   /* reg c (low byte) | float bits | jump target | index */
} ScriptInstr;

SCRIPT_STATIC_ASSERT(sizeof(ScriptInstr) == SCRIPT_INSTR_SIZE,
                     "ScriptInstr must be exactly 8 bytes with no padding");

/* --------------------------------------------------------------------------
 * Image header
 * -------------------------------------------------------------------------- */

/* Sits at the front of the script image, followed by code_bytes of bytecode.
 * Entry points are INSTRUCTION INDICES (not byte offsets) of each hook, or
 * SCRIPT_NO_ENTRY when the script does not implement that hook. Only on_tick
 * runs in bytecode version 1; the other two slots are reserved so the format
 * does not have to change when phase 3 adds them. */
#define SCRIPT_NO_ENTRY  0xFFFFFFFFu

typedef struct {
    uint32_t magic;         /* SCRIPT_MAGIC */
    uint16_t version;       /* SCRIPT_BYTECODE_VERSION */
    uint16_t num_state;     /* persistent state registers used, <= SCRIPT_NUM_STATE */
    uint32_t code_bytes;    /* bytecode length, a multiple of SCRIPT_INSTR_SIZE */
    uint32_t code_crc32;    /* CRC32 over the code that follows this header */
    uint32_t entry_tick;    /* on_tick instruction index, or SCRIPT_NO_ENTRY */
    uint32_t entry_rx;      /* phase 3; SCRIPT_NO_ENTRY in v1 */
    uint32_t entry_tx;      /* phase 3; SCRIPT_NO_ENTRY in v1 */
    uint32_t reserved;      /* write 0 */
} ScriptHeader;

SCRIPT_STATIC_ASSERT(sizeof(ScriptHeader) == 32, "ScriptHeader must be 32 bytes");

/* The smallest thing that could be a valid image: a header and a lone HALT. */
#define SCRIPT_MIN_IMAGE_BYTES (sizeof(ScriptHeader) + SCRIPT_INSTR_SIZE)

/* --------------------------------------------------------------------------
 * Verifier results
 * -------------------------------------------------------------------------- */

/* Every way a script image can be rejected. script_verify() returns exactly
 * one, and it is reported to the configurator so a refusal says WHY. The
 * ordering is stable ABI — the GUI maps these to messages. */
enum {
    SCRIPT_OK               = 0,
    SCRIPT_ERR_MAGIC        = 1,  /* not a script image / erased flash */
    SCRIPT_ERR_VERSION      = 2,  /* bytecode NEWER than this device runs */
    SCRIPT_ERR_SIZE         = 3,  /* code_bytes zero, unaligned, or too large */
    SCRIPT_ERR_CRC          = 4,  /* code does not match its checksum */
    SCRIPT_ERR_STATE_COUNT  = 5,  /* num_state > SCRIPT_NUM_STATE */
    SCRIPT_ERR_OPCODE       = 6,  /* an unknown opcode (or a phase-3 one) */
    SCRIPT_ERR_REGISTER     = 7,  /* a register field >= SCRIPT_NUM_REGS */
    SCRIPT_ERR_SIGNAL       = 8,  /* a signal index >= max_signals */
    SCRIPT_ERR_STATE_INDEX  = 9,  /* a state index >= num_state */
    SCRIPT_ERR_JUMP         = 10, /* a jump target outside the code */
    SCRIPT_ERR_ENTRY        = 11, /* an entry point outside the code */
    SCRIPT_ERR_NO_HALT      = 12, /* the code does not end in HALT */
    /* Appended, never renumbered — these are stable ABI the GUI maps to text. */
    SCRIPT_ERR_VERSION_OLD  = 13, /* bytecode OLDER than this device still supports */
    SCRIPT_ERR_NONFINITE    = 14, /* a LOADK immediate is NaN or Inf */
    SCRIPT_ERR_RESERVED     = 15, /* a reserved header field is not its v1 value */
};

/* --------------------------------------------------------------------------
 * Runtime fault codes
 * -------------------------------------------------------------------------- */

/* After a hook runs, the VM reports one of these — read back by the
 * configurator so a misbehaving script can say what happened. A verified image
 * can only fault at runtime by exceeding the budget: every memory access was
 * proven safe, and arithmetic degrades (div-by-zero -> 0, like executeMath)
 * rather than trapping. */
enum {
    SCRIPT_FAULT_NONE    = 0,
    SCRIPT_FAULT_BUDGET  = 1,  /* the tick budget was exhausted mid-hook */
    /* The cycle ceiling was reached: this hook was taking too long in real
     * time, whatever the cost table thought it was spending. Distinct from
     * BUDGET on purpose — the two mean genuinely different things to whoever is
     * reading the status. BUDGET says "your script does too much work" and is
     * reproducible in the desktop simulator. OVERRUN says "your script did work
     * that is dearer on the device than its cost suggests", which in practice
     * means MOD, WRAP or CLAMP over a wide exponent range, and which the
     * simulator cannot reproduce because host cycles are not device cycles. */
    SCRIPT_FAULT_OVERRUN = 2,
};

/* --------------------------------------------------------------------------
 * The shared verifier and CRC
 * -------------------------------------------------------------------------- */

/* CRC32 over the code, identical algorithm to fw_crc32 (IEEE, reflected) so
 * the host's zlib and the device agree with no adapter. Declared here rather
 * than reused from fw_image.h to keep this contract self-contained; the .c
 * defines it once. */
uint32_t script_crc32(const void *data, uint32_t len);

/* The budget cost of one opcode — 1 for a cheap op, SCRIPT_COST_HEAVY for a
 * libm-heavy one (see SCRIPT_COST_HEAVY). Shared so the device interpreter and
 * the desktop simulator charge identically, which is what keeps a simulated
 * "this script fits the budget" honest about the device. The interpreter MUST
 * call this for every instruction (INTERPRETER REQUIREMENT R1). */
uint32_t script_op_cost(uint8_t op);

/* Prove an image at `base` (`avail` bytes available) is SAFE to execute on a
 * device with `max_signals` value slots. Returns SCRIPT_OK, or the first
 * reason it was rejected. This is the whole safety guarantee: after SCRIPT_OK,
 * the interpreter needs no bounds checks because none can fail.
 *
 * Run by the device before it will execute a stored script, AND by the host
 * compiler on its own output (a compiler that emits an image the device would
 * reject is a compiler bug, caught on the desktop). One implementation, so the
 * two verdicts cannot differ. */
uint8_t script_verify(const void *base, uint32_t avail, uint16_t max_signals);

/* The header inside an image, or NULL if `avail` is too small to hold one.
 * Does not validate — use script_verify for that. */
static inline const ScriptHeader *script_header(const void *base, uint32_t avail)
{
    if (avail < sizeof(ScriptHeader)) {
        return (const ScriptHeader *)0;
    }
    return (const ScriptHeader *)base;
}

/* The first instruction, given a validated image. */
static inline const ScriptInstr *script_code(const void *base)
{
    return (const ScriptInstr *)((const uint8_t *)base + sizeof(ScriptHeader));
}

#ifdef __cplusplus
}
#endif

#endif /* SCRIPT_VM_H */
