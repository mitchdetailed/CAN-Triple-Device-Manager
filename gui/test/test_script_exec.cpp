// The script VM interpreter: does it compute correctly, and — far more
// importantly — does it actually deliver INTERPRETER REQUIREMENTS R1-R5 from
// script_vm.h?
//
// The verifier (test_script_vm) covers memory safety. This covers the other
// half of the contract, liveness, which the verifier deliberately does NOT
// provide: script_verify() accepts an infinite self-jump as a legal program, so
// the ONLY thing standing between a bad script and a stalled 100 Hz tick is
// this interpreter charging every dispatch and testing before it. Each R gets
// a test that fails if the requirement is dropped.
//
// Runs the real script_exec.c against a RAM signal table — the same code the
// device runs and the desktop simulator will run.

#include <cstdio>
#include <cstring>

extern "C" {
#include "script_exec.h"
#include "script_vm.h"
}

static int fails = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);              \
            ++fails;                                                                 \
        }                                                                            \
    } while (0)

// ------------------------------------------------------------------ host

static constexpr int kSignals = 1000;
static float g_sig[kSignals];

static float hostRead(uint16_t idx) { return idx < kSignals ? g_sig[idx] : 0.0f; }
static void hostWrite(uint16_t idx, float v) { if (idx < kSignals) g_sig[idx] = v; }

// ------------------------------------------------------------------ assembly

static uint8_t g_image[SCRIPT_MAX_CODE_BYTES + sizeof(ScriptHeader)];

static uint32_t f2b(float f) { uint32_t b; std::memcpy(&b, &f, 4); return b; }

// Build an image from instructions and load it. Returns the verify code.
static uint8_t load(const ScriptInstr *code, uint32_t n, uint16_t numState = 8)
{
    ScriptHeader h {};
    h.magic = SCRIPT_MAGIC;
    h.version = SCRIPT_BYTECODE_VERSION;
    h.num_state = numState;
    h.code_bytes = n * SCRIPT_INSTR_SIZE;
    h.entry_tick = 0;
    h.entry_rx = SCRIPT_NO_ENTRY;
    h.entry_tx = SCRIPT_NO_ENTRY;
    h.reserved = 0;
    std::memcpy(g_image, &h, sizeof(h));
    std::memcpy(g_image + sizeof(h), code, h.code_bytes);
    ((ScriptHeader *)g_image)->code_crc32 =
        script_crc32(g_image + sizeof(h), h.code_bytes);
    return script_exec_load(g_image, sizeof(h) + h.code_bytes, kSignals);
}

static ScriptInstr I(uint8_t op, uint8_t dst, uint8_t a, uint8_t b, uint32_t imm)
{
    ScriptInstr in {};
    in.op = op; in.dst = dst; in.a = a; in.b = b; in.imm = imm;
    return in;
}

static uint8_t tick()
{
    script_exec_begin_tick();
    return script_exec_on_tick();
}

// ------------------------------------------------------------------ tests

// The headline: a script computes and drives a signal.
static void testArithmetic()
{
    std::memset(g_sig, 0, sizeof(g_sig));
    g_sig[10] = 21.0f;

    // signal[11] = signal[10] * 2.0
    const ScriptInstr code[] = {
        I(SCRIPT_OP_LOADSIG, 0, 0, 0, 10),
        I(SCRIPT_OP_LOADK, 1, 0, 0, f2b(2.0f)),
        I(0x02 /*MUL*/, 0, 0, 1, 0),
        I(SCRIPT_OP_STORESIG, 0, 0, 0, 11),
        I(SCRIPT_OP_HALT, 0, 0, 0, 0),
    };
    CHECK(load(code, 5) == SCRIPT_OK);
    CHECK(script_exec_running());
    CHECK(tick() == SCRIPT_FAULT_NONE);
    CHECK(g_sig[11] == 42.0f);

    // It runs every tick, tracking its input.
    g_sig[10] = 50.0f;
    CHECK(tick() == SCRIPT_FAULT_NONE);
    CHECK(g_sig[11] == 100.0f);

    // The ternary operand comes from imm's low byte: MULADD r0 = r0*r1 + r2.
    std::memset(g_sig, 0, sizeof(g_sig));
    g_sig[1] = 3.0f;
    const ScriptInstr tern[] = {
        I(SCRIPT_OP_LOADSIG, 0, 0, 0, 1),          // r0 = 3
        I(SCRIPT_OP_LOADK, 1, 0, 0, f2b(4.0f)),    // r1 = 4
        I(SCRIPT_OP_LOADK, 2, 0, 0, f2b(5.0f)),    // r2 = 5
        I(0x1A /*MULADD*/, 3, 0, 1, 2),            // r3 = 3*4 + 5 = 17
        I(SCRIPT_OP_STORESIG, 0, 3, 0, 2),
        I(SCRIPT_OP_HALT, 0, 0, 0, 0),
    };
    CHECK(load(tern, 6) == SCRIPT_OK);
    CHECK(tick() == SCRIPT_FAULT_NONE);
    CHECK(g_sig[2] == 17.0f);
}

// Branches + persistent state = a state machine, the phase-2 use case.
static void testStateMachineAndBranching()
{
    std::memset(g_sig, 0, sizeof(g_sig));

    // A counter that increments every tick and wraps at 3:
    //   s0 = s0 + 1 ; if s0 >= 3 then s0 = 0 ; signal[20] = s0
    const ScriptInstr code[] = {
        I(SCRIPT_OP_LOADST, 0, 0, 0, 0),           // 0: r0 = state[0]
        I(SCRIPT_OP_LOADK, 1, 0, 0, f2b(1.0f)),    // 1: r1 = 1
        I(0x00 /*ADD*/, 0, 0, 1, 0),               // 2: r0 = r0 + 1
        I(SCRIPT_OP_LOADK, 2, 0, 0, f2b(3.0f)),    // 3: r2 = 3
        I(0x15 /*GE*/, 3, 0, 2, 0),                // 4: r3 = (r0 >= 3)
        I(SCRIPT_OP_JZ, 0, 3, 0, 7),               // 5: if !r3 goto 7
        I(SCRIPT_OP_LOADK, 0, 0, 0, f2b(0.0f)),    // 6: r0 = 0
        I(SCRIPT_OP_STOREST, 0, 0, 0, 0),          // 7: state[0] = r0
        I(SCRIPT_OP_STORESIG, 0, 0, 0, 20),        // 8: signal[20] = r0
        I(SCRIPT_OP_HALT, 0, 0, 0, 0),             // 9
    };
    CHECK(load(code, 10) == SCRIPT_OK);

    const float expected[] = { 1, 2, 0, 1, 2, 0, 1 };
    for (int i = 0; i < 7; ++i) {
        CHECK(tick() == SCRIPT_FAULT_NONE);
        if (g_sig[20] != expected[i]) {
            std::printf("FAIL  tick %d: signal 20 = %g, want %g\n", i, g_sig[20],
                        expected[i]);
            ++fails;
        }
    }
}

// R1 + R2: a self-jump is a legal verified program and an infinite loop. The
// budget must stop it. This is THE liveness test — if it hangs, the contract's
// central claim is false.
static void testSelfJumpTerminates()
{
    const ScriptInstr code[] = {
        I(SCRIPT_OP_JMP, 0, 0, 0, 0),   // 0: jump to itself, forever
        I(SCRIPT_OP_HALT, 0, 0, 0, 0),  // 1: never reached
    };
    CHECK(load(code, 2) == SCRIPT_OK);  // the verifier accepts it, by design
    CHECK(tick() == SCRIPT_FAULT_BUDGET);

    // R5: it is suspended, not re-run every tick to burn the budget again.
    CHECK(!script_exec_running());
    ScriptStatus st {};
    script_exec_status(&st);
    CHECK(st.suspended == 1);
    CHECK(st.fault == SCRIPT_FAULT_BUDGET);
    CHECK(tick() == SCRIPT_FAULT_NONE); // suspended: nothing runs
}

// R1: heavy ops are charged their real weight, so a MOD loop cannot do as much
// work as an ADD loop before the budget stops it. Without weighting, both would
// run the same number of iterations and the MOD one would take ~20x the
// wall-clock — the exact hole the review found.
//
// Iterations are counted by storing the loop variable to a signal each pass:
// STORESIG is not rolled back on a fault, so after the budget kill the signal
// holds the final count.
static void testHeavyOpsCostMore()
{
    auto iterations = [](uint8_t op, uint16_t sig) {
        std::memset(g_sig, 0, sizeof(g_sig));
        const ScriptInstr code[] = {
            I(SCRIPT_OP_LOADK, 1, 0, 0, f2b(1.0f)),   // 0: r1 = 1
            I(SCRIPT_OP_LOADK, 2, 0, 0, f2b(7.0f)),   // 1: r2 = 7 (MOD divisor)
            I(0x00 /*ADD*/, 0, 0, 1, 0),              // 2: r0 = r0 + 1  (the counter)
            I(op, 3, 0, 2, 0),                        // 3: r3 = op(r0, r2)  <- the op under test
            I(SCRIPT_OP_STORESIG, 0, 0, 0, sig),      // 4: signal[sig] = r0
            I(SCRIPT_OP_JMP, 0, 0, 0, 2),             // 5: loop
            I(SCRIPT_OP_HALT, 0, 0, 0, 0),            // 6
        };
        CHECK(load(code, 7) == SCRIPT_OK);
        CHECK(tick() == SCRIPT_FAULT_BUDGET);   // both must terminate
        return g_sig[sig];
    };

    const float cheapIters = iterations(0x00 /*ADD*/, 70);
    const float heavyIters = iterations(0x0F /*MOD*/, 71);

    // Per iteration: cheap = ADD+ADD+STORESIG+JMP = 4 units;
    //                heavy = ADD+MOD+STORESIG+JMP = 3 + SCRIPT_COST_HEAVY.
    // So the heavy loop must complete markedly fewer passes.
    if (!(heavyIters < cheapIters)) {
        std::printf("FAIL  heavy loop ran %g iterations vs cheap %g - the cost "
                    "model is not charging heavy ops\n", heavyIters, cheapIters);
        ++fails;
    }
    // And roughly in the ratio the weights predict (allow slack, it is a model).
    const float ratio = cheapIters / (heavyIters > 0 ? heavyIters : 1.0f);
    if (ratio < 2.0f) {
        std::printf("FAIL  cheap/heavy iteration ratio only %.1f - heavy ops are "
                    "barely costed\n", ratio);
        ++fails;
    }
    std::printf("  info: %g cheap iterations vs %g heavy (ratio %.1f)\n",
                cheapIters, heavyIters, ratio);
}

// R4: a hook killed mid-way must not leave torn persistent state behind.
//
// This cannot be observed through the script — a faulted script is suspended
// (R5), and reloading it to look would clear the very state under test. Hence
// script_exec_state().
static void testStateRolledBackOnFault()
{
    std::memset(g_sig, 0, sizeof(g_sig));

    // Tick 1 stores 5 and halts cleanly. Tick 2 stores 99 and then spins, so it
    // is killed by the budget: the 99 must be undone and 5 must remain.
    //
    // The two ticks are told apart by state[1], which tick 1 sets to 1 — so the
    // branch at 5 falls through to the spin only on the SECOND pass. (An earlier
    // version of this test used a slot nothing ever wrote, so the branch always
    // taken meant the spin was unreachable and the fault path never ran at all.)
    const ScriptInstr code[] = {
        I(SCRIPT_OP_LOADK, 1, 0, 0, f2b(5.0f)),   // 0: r1 = 5
        I(SCRIPT_OP_STOREST, 0, 1, 0, 0),         // 1: state[0] = 5
        I(SCRIPT_OP_LOADST, 2, 0, 0, 1),          // 2: r2 = state[1] (0 on tick 1)
        I(SCRIPT_OP_LOADK, 3, 0, 0, f2b(1.0f)),   // 3: r3 = 1
        I(SCRIPT_OP_STOREST, 0, 3, 0, 1),         // 4: state[1] = 1 (mark "been here")
        I(SCRIPT_OP_JZ, 0, 2, 0, 9),              // 5: tick 1 -> clean HALT at 9
        I(SCRIPT_OP_LOADK, 4, 0, 0, f2b(99.0f)),  // 6: tick 2 onwards
        I(SCRIPT_OP_STOREST, 0, 4, 0, 0),         // 7: state[0] = 99  <- must roll back
        I(SCRIPT_OP_JMP, 0, 0, 0, 8),             // 8: spin -> budget fault
        I(SCRIPT_OP_HALT, 0, 0, 0, 0),            // 9
    };
    CHECK(load(code, 10) == SCRIPT_OK);

    CHECK(tick() == SCRIPT_FAULT_NONE);           // tick 1: clean
    CHECK(script_exec_state(0) == 5.0f);          // the clean store persisted
    CHECK(script_exec_state(1) == 1.0f);

    CHECK(tick() == SCRIPT_FAULT_BUDGET);         // tick 2: killed after storing 99
    // R4: every state write the killed hook made is undone — state[0] is back to
    // the 5 it held at hook entry, NOT the 99 the doomed pass wrote.
    if (script_exec_state(0) != 5.0f) {
        std::printf("FAIL  state[0] = %g after a faulted hook, want 5 (R4 rollback "
                    "did not happen)\n", script_exec_state(0));
        ++fails;
    }
    CHECK(script_exec_state(1) == 1.0f);          // and so is the other slot
}

// R3: the budget does not carry between ticks. A script that spends most of the
// budget every tick must keep running, not accumulate its way into a fault.
static void testBudgetResetsEachTick()
{
    std::memset(g_sig, 0, sizeof(g_sig));

    // ~100 cheap instructions per tick, well inside the budget, repeated far
    // more times than the budget would allow if spend accumulated.
    ScriptInstr code[102];
    for (int i = 0; i < 100; ++i) {
        code[i] = I(SCRIPT_OP_LOADK, 0, 0, 0, f2b(float(i)));
    }
    code[100] = I(SCRIPT_OP_STORESIG, 0, 0, 0, 40);
    code[101] = I(SCRIPT_OP_HALT, 0, 0, 0, 0);
    CHECK(load(code, 102) == SCRIPT_OK);

    for (int t = 0; t < 200; ++t) {
        if (tick() != SCRIPT_FAULT_NONE) {
            std::printf("FAIL  faulted on tick %d - budget is accumulating\n", t);
            ++fails;
            break;
        }
    }
    CHECK(script_exec_running());
    CHECK(g_sig[40] == 99.0f);

    ScriptStatus st {};
    script_exec_status(&st);
    CHECK(st.peak_cost == 102);          // exactly the instructions executed
    CHECK(st.budget == SCRIPT_TICK_BUDGET);
}

// A rejected image must not run, and must say why.
static void testRejectedImageDoesNotRun()
{
    std::memset(g_sig, 0, sizeof(g_sig));

    ScriptInstr code[] = {
        I(SCRIPT_OP_LOADK, 0, 0, 0, f2b(1.0f)),
        I(SCRIPT_OP_STORESIG, 0, 0, 0, 50),
        I(SCRIPT_OP_HALT, 0, 0, 0, 0),
    };
    CHECK(load(code, 3) == SCRIPT_OK);
    CHECK(tick() == SCRIPT_FAULT_NONE);
    CHECK(g_sig[50] == 1.0f);

    // Now corrupt it: a signal index past the table.
    g_sig[50] = 0.0f;
    code[1].imm = 5000;
    CHECK(load(code, 3) == SCRIPT_ERR_SIGNAL);
    CHECK(!script_exec_running());
    CHECK(tick() == SCRIPT_FAULT_NONE);
    CHECK(g_sig[50] == 0.0f);           // nothing ran

    ScriptStatus st {};
    script_exec_status(&st);
    CHECK(st.verify_result == SCRIPT_ERR_SIGNAL);
    CHECK(st.code_bytes == 0);
}

// A script with no on_tick entry is legal and simply does nothing.
static void testNoEntryIsLegal()
{
    const ScriptInstr code[] = { I(SCRIPT_OP_HALT, 0, 0, 0, 0) };
    ScriptHeader h {};
    h.magic = SCRIPT_MAGIC;
    h.version = SCRIPT_BYTECODE_VERSION;
    h.num_state = 0;
    h.code_bytes = SCRIPT_INSTR_SIZE;
    h.entry_tick = SCRIPT_NO_ENTRY;   // no hook
    h.entry_rx = SCRIPT_NO_ENTRY;
    h.entry_tx = SCRIPT_NO_ENTRY;
    std::memcpy(g_image, &h, sizeof(h));
    std::memcpy(g_image + sizeof(h), code, h.code_bytes);
    ((ScriptHeader *)g_image)->code_crc32 = script_crc32(g_image + sizeof(h), h.code_bytes);

    CHECK(script_exec_load(g_image, sizeof(h) + h.code_bytes, kSignals) == SCRIPT_OK);
    CHECK(script_exec_running());
    CHECK(tick() == SCRIPT_FAULT_NONE);
}

// Scratch registers must start zeroed each invocation, or a hook's behaviour
// would depend on invisible history and the simulator could not reproduce it.
static void testScratchIsFreshEachTick()
{
    std::memset(g_sig, 0, sizeof(g_sig));

    // Read r5 (never written this invocation) and report it. It must be 0 every
    // tick, even though a previous tick wrote it.
    const ScriptInstr code[] = {
        I(SCRIPT_OP_STORESIG, 0, 5, 0, 60),        // 0: signal[60] = r5 (fresh)
        I(SCRIPT_OP_LOADK, 5, 0, 0, f2b(77.0f)),   // 1: r5 = 77 for next time
        I(SCRIPT_OP_HALT, 0, 0, 0, 0),
    };
    CHECK(load(code, 3) == SCRIPT_OK);
    CHECK(tick() == SCRIPT_FAULT_NONE);
    CHECK(g_sig[60] == 0.0f);
    CHECK(tick() == SCRIPT_FAULT_NONE);
    CHECK(g_sig[60] == 0.0f);   // still zero: scratch did not survive
}

int main()
{
    const ScriptHost host = { hostRead, hostWrite };
    script_exec_init(&host);

    testArithmetic();
    testStateMachineAndBranching();
    testSelfJumpTerminates();
    testHeavyOpsCostMore();
    testStateRolledBackOnFault();
    testBudgetResetsEachTick();
    testRejectedImageDoesNotRun();
    testNoEntryIsLegal();
    testScratchIsFreshEachTick();

    if (fails == 0) {
        std::printf("ALL SCRIPT EXEC TESTS PASSED\n");
    } else {
        std::printf("%d FAILURES\n", fails);
    }
    return fails ? 1 : 0;
}
