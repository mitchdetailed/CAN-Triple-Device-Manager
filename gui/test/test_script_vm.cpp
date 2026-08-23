// The script VM verifier and cost model, tested exhaustively. This is the
// safety foundation the whole scripting feature rests on: after script_verify()
// returns SCRIPT_OK the device interpreter does NO bounds checks, so every hole
// this test does not close is a hole in that guarantee.
//
// Structure: build one valid image, then for each rejection reason corrupt
// exactly that field of a fresh copy and require the specific code. A test that
// only checked the happy path would pass against a verifier that accepted
// everything — and accepting everything is precisely the failure mode.
//
// The additions past the original 15 checks came out of an adversarial review
// of the contract (five safety lenses): the version RANGE gate, non-finite
// LOADK rejection, and the pinned reserved header fields. Each has a test here
// so the fix cannot silently regress.

#include <cstdio>
#include <cstring>

extern "C" {
#include "script_vm.h"
}

#define MAX_SIG 1000

static uint8_t buf[4096];
static int fails = 0;

// Assemble header + code, fixing up code_bytes and the CRC. Returns total size.
static uint32_t build(const ScriptInstr *code, uint32_t ninstr)
{
    ScriptHeader h;
    std::memset(&h, 0, sizeof(h));
    h.magic = SCRIPT_MAGIC;
    h.version = SCRIPT_BYTECODE_VERSION;
    h.num_state = 4;
    h.code_bytes = ninstr * SCRIPT_INSTR_SIZE;
    h.entry_tick = 0;
    h.entry_rx = SCRIPT_NO_ENTRY;
    h.entry_tx = SCRIPT_NO_ENTRY;
    h.reserved = 0;
    std::memcpy(buf, &h, sizeof(h));
    std::memcpy(buf + sizeof(h), code, h.code_bytes);
    ((ScriptHeader *)buf)->code_crc32 = script_crc32(buf + sizeof(h), h.code_bytes);
    return sizeof(h) + h.code_bytes;
}

static const char *name(uint8_t rc)
{
    static const char *n[] = { "OK", "MAGIC", "VERSION", "SIZE", "CRC", "STATE_COUNT",
        "OPCODE", "REGISTER", "SIGNAL", "STATE_INDEX", "JUMP", "ENTRY", "NO_HALT",
        "VERSION_OLD", "NONFINITE", "RESERVED" };
    return rc <= SCRIPT_ERR_RESERVED ? n[rc] : "?";
}

static void expect(const char *label, uint8_t got, uint8_t want)
{
    if (got != want) {
        std::printf("FAIL  %-34s got %s, want %s\n", label, name(got), name(want));
        ++fails;
    }
}

static uint32_t f2b(float f)
{
    uint32_t b;
    std::memcpy(&b, &f, 4);
    return b;
}

int main()
{
    // A small valid program:
    //   0: LOADSIG  r0 = signal[10]
    //   1: LOADK    r1 = 2.0
    //   2: MUL      r0 = r0 * r1
    //   3: STORESIG signal[11] = r0
    //   4: HALT
    ScriptInstr code[5];
    std::memset(code, 0, sizeof(code));
    code[0] = ScriptInstr{ SCRIPT_OP_LOADSIG, 0, 0, 0, 10 };
    code[1] = ScriptInstr{ SCRIPT_OP_LOADK, 1, 0, 0, f2b(2.0f) };
    code[2] = ScriptInstr{ 0x02 /*MUL*/, 0, 0, 1, 0 };
    code[3] = ScriptInstr{ SCRIPT_OP_STORESIG, 0, 0, 0, 11 };
    code[4] = ScriptInstr{ SCRIPT_OP_HALT, 0, 0, 0, 0 };

    uint32_t total = build(code, 5);
    expect("valid image", script_verify(buf, total, MAX_SIG), SCRIPT_OK);

    ScriptHeader *h = (ScriptHeader *)buf;

    build(code, 5); h->magic = 0xDEAD;
    expect("bad magic", script_verify(buf, total, MAX_SIG), SCRIPT_ERR_MAGIC);

    build(code, 5); h->version = 99; // newer than we run
    expect("version too new", script_verify(buf, total, MAX_SIG), SCRIPT_ERR_VERSION);

    build(code, 5); h->version = 0; // older than we support (min is 1)
    expect("version too old", script_verify(buf, total, MAX_SIG), SCRIPT_ERR_VERSION_OLD);

    build(code, 5); h->num_state = 200;
    expect("too many state regs", script_verify(buf, total, MAX_SIG), SCRIPT_ERR_STATE_COUNT);

    build(code, 5); h->code_bytes = 7;
    expect("unaligned code_bytes", script_verify(buf, total, MAX_SIG), SCRIPT_ERR_SIZE);

    build(code, 5); h->code_bytes = 99999;
    expect("oversize code_bytes", script_verify(buf, total, MAX_SIG), SCRIPT_ERR_SIZE);

    build(code, 5); buf[sizeof(ScriptHeader) + 8] ^= 0x01;
    expect("flipped code byte", script_verify(buf, total, MAX_SIG), SCRIPT_ERR_CRC);

    { ScriptInstr c[5]; std::memcpy(c, code, sizeof(code)); c[2].a = 200;
      total = build(c, 5);
      expect("arith reg >= 64", script_verify(buf, total, MAX_SIG), SCRIPT_ERR_REGISTER); }

    { ScriptInstr c[5]; std::memcpy(c, code, sizeof(code)); c[0].imm = 5000;
      total = build(c, 5);
      expect("signal idx >= max", script_verify(buf, total, MAX_SIG), SCRIPT_ERR_SIGNAL); }

    { ScriptInstr c[6]; std::memcpy(c, code, 5 * sizeof(ScriptInstr));
      c[4] = ScriptInstr{ SCRIPT_OP_LOADST, 2, 0, 0, 50 }; // header says num_state 4
      c[5] = ScriptInstr{ SCRIPT_OP_HALT, 0, 0, 0, 0 };
      total = build(c, 6);
      expect("state idx >= num_state", script_verify(buf, total, MAX_SIG), SCRIPT_ERR_STATE_INDEX); }

    { ScriptInstr c[6]; std::memcpy(c, code, 5 * sizeof(ScriptInstr));
      c[4] = ScriptInstr{ SCRIPT_OP_JMP, 0, 0, 0, 999 };
      c[5] = ScriptInstr{ SCRIPT_OP_HALT, 0, 0, 0, 0 };
      total = build(c, 6);
      expect("jump out of range", script_verify(buf, total, MAX_SIG), SCRIPT_ERR_JUMP); }

    { ScriptInstr c[5]; std::memcpy(c, code, sizeof(code)); c[2].op = 0x40; // phase-3
      total = build(c, 5);
      expect("phase-3 opcode rejected", script_verify(buf, total, MAX_SIG), SCRIPT_ERR_OPCODE); }

    build(code, 5); h->entry_tick = 999;
    expect("entry out of range", script_verify(buf, total, MAX_SIG), SCRIPT_ERR_ENTRY);

    { ScriptInstr c[5]; std::memcpy(c, code, sizeof(code)); c[4].op = SCRIPT_OP_MOV;
      total = build(c, 5);
      expect("no trailing HALT", script_verify(buf, total, MAX_SIG), SCRIPT_ERR_NO_HALT); }

    // --- review-driven: non-finite constants and pinned reserved fields ---

    { ScriptInstr c[5]; std::memcpy(c, code, sizeof(code)); c[1].imm = 0x7F800000u; // +Inf
      total = build(c, 5);
      expect("LOADK +Inf", script_verify(buf, total, MAX_SIG), SCRIPT_ERR_NONFINITE); }
    { ScriptInstr c[5]; std::memcpy(c, code, sizeof(code)); c[1].imm = 0x7FC00005u; // NaN
      total = build(c, 5);
      expect("LOADK NaN", script_verify(buf, total, MAX_SIG), SCRIPT_ERR_NONFINITE); }
    { ScriptInstr c[5]; std::memcpy(c, code, sizeof(code)); c[1].imm = 0x7F7FFFFFu; // FLT_MAX
      total = build(c, 5);
      expect("LOADK FLT_MAX finite ok", script_verify(buf, total, MAX_SIG), SCRIPT_OK); }

    build(code, 5); h->entry_rx = 0;
    expect("entry_rx not NO_ENTRY", script_verify(buf, total, MAX_SIG), SCRIPT_ERR_RESERVED);
    build(code, 5); h->reserved = 0xDEADBEEF;
    expect("reserved not zero", script_verify(buf, total, MAX_SIG), SCRIPT_ERR_RESERVED);

    std::memset(buf, 0xFF, sizeof(buf));
    expect("erased flash", script_verify(buf, 512, MAX_SIG), SCRIPT_ERR_MAGIC);

    // --- cost model: cheap ops 1, DIVIDE its own tier, libm-heavy ops HEAVY ---
    if (script_op_cost(0x02) != 1u ||           // MUL
        script_op_cost(SCRIPT_OP_LOADK) != 1u ||
        script_op_cost(SCRIPT_OP_JMP) != 1u ||
        script_op_cost(0x03) != SCRIPT_COST_DIVIDE || // DIV
        script_op_cost(0x0B) != SCRIPT_COST_HEAVY || // SQRT
        script_op_cost(0x0F) != SCRIPT_COST_HEAVY || // MOD
        script_op_cost(0x1E) != SCRIPT_COST_HEAVY) { // WRAP
        std::printf("FAIL  op cost model\n");
        ++fails;
    }

    // The tiers must stay ORDERED and distinct. Measured on hardware at roughly
    // 1 : 3 : 6 cycles, so a build that collapsed two tiers — or inverted them —
    // would be charging a fiction while still compiling. DIV in particular was
    // charged 1 until it was measured, and a loop of divides then overran the
    // intended bound by three times.
    if (!(1u < SCRIPT_COST_DIVIDE && SCRIPT_COST_DIVIDE < SCRIPT_COST_HEAVY)) {
        std::printf("FAIL  cost tiers are not ordered 1 < DIVIDE < HEAVY\n");
        ++fails;
    }

    // The cycle backstop's sampling interval must be a power of two: run_hook
    // tests it with a counter compare that assumes a regular interval, and the
    // overshoot bound quoted in script_vm.h ("at most 8 further ops") is only
    // true if this is what it says it is.
    if (SCRIPT_CYCLE_CHECK == 0u
        || (SCRIPT_CYCLE_CHECK & (SCRIPT_CYCLE_CHECK - 1u)) != 0u) {
        std::printf("FAIL  SCRIPT_CYCLE_CHECK must be a power of two\n");
        ++fails;
    }
    // A ceiling above the tick itself would be no ceiling at all. 170 MHz x
    // 10 ms = 1,700,000 cycles is the whole tick; the ceiling has to leave the
    // engine the majority of it.
    if (SCRIPT_CYCLE_CEILING == 0u || SCRIPT_CYCLE_CEILING > 1700000u / 4u) {
        std::printf("FAIL  SCRIPT_CYCLE_CEILING is not a fraction of a tick\n");
        ++fails;
    }

    if (fails == 0) {
        std::printf("ALL SCRIPT VM TESTS PASSED\n");
    } else {
        std::printf("%d FAILURES\n", fails);
    }
    return fails ? 1 : 0;
}
