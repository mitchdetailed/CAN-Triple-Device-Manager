// The Lua-subset compiler, end to end.
//
// The valuable tests here are the ROUND TRIPS: compile Lua, hand the bytecode
// to the DEVICE's verifier, run it on the DEVICE's interpreter, and check the
// signal values. That exercises the whole chain in one assertion — compiler,
// contract and VM — and it is the only way to catch a compiler that emits
// something plausible-but-wrong, because the bytecode itself is unreadable by
// inspection.
//
// The rejection tests matter nearly as much. The subset's whole promise is that
// unsupported constructs are COMPILE errors rather than runtime surprises, so
// each one that must be refused has a case, and the message is checked for the
// substring that makes it actionable.

#include <QString>

#include <cstdio>
#include <cstring>

#include "../src/scripting/script_compiler.h"

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

namespace {

constexpr int kSignals = 1000;
float g_sig[kSignals];

float hostRead(uint16_t i) { return i < kSignals ? g_sig[i] : 0.0f; }
void hostWrite(uint16_t i, float v) { if (i < kSignals) g_sig[i] = v; }

ct::ScriptSymbols symbols()
{
    ct::ScriptSymbols s;
    s.signalIndex.insert(QStringLiteral("Engine RPM"), 10);
    s.signalIndex.insert(QStringLiteral("Coolant Temp"), 11);
    s.signalIndex.insert(QStringLiteral("Fan Request"), 12);
    s.signalIndex.insert(QStringLiteral("Out A"), 13);
    s.signalIndex.insert(QStringLiteral("Out B"), 14);
    return s;
}

ct::ScriptCompiler::Result compile(const char *src)
{
    return ct::ScriptCompiler::compile(QString::fromUtf8(src), symbols());
}

// Compile, verify, load, and run `ticks` ticks. Returns false (and reports) if
// any stage refuses.
bool runScript(const char *src, int ticks = 1)
{
    const auto r = compile(src);
    if (!r.ok) {
        std::printf("FAIL  compile: line %d: %s\n", r.errorLine,
                    r.error.toUtf8().constData());
        ++fails;
        return false;
    }
    // The compiler already ran the verifier internally; run it again here so a
    // regression that removed that self-check still gets caught.
    const uint8_t v = script_verify(r.image.constData(), uint32_t(r.image.size()),
                                    kSignals);
    if (v != SCRIPT_OK) {
        std::printf("FAIL  verifier rejected compiler output: code %u\n", v);
        ++fails;
        return false;
    }
    if (script_exec_load(r.image.constData(), uint32_t(r.image.size()), kSignals)
        != SCRIPT_OK) {
        std::printf("FAIL  script_exec_load refused the image\n");
        ++fails;
        return false;
    }
    for (int i = 0; i < ticks; ++i) {
        script_exec_begin_tick();
        const uint8_t fault = script_exec_on_tick();
        if (fault != SCRIPT_FAULT_NONE) {
            std::printf("FAIL  fault %u on tick %d\n", fault, i);
            ++fails;
            return false;
        }
    }
    return true;
}

// A compile must fail, and its message must contain `fragment` — so a test
// cannot pass because compilation failed for an unrelated reason.
void expectReject(const char *src, const char *fragment, const char *label)
{
    const auto r = compile(src);
    if (r.ok) {
        std::printf("FAIL  %s: compiled, but should have been rejected\n", label);
        ++fails;
        return;
    }
    if (!r.error.contains(QString::fromUtf8(fragment), Qt::CaseInsensitive)) {
        std::printf("FAIL  %s: message was \"%s\"\n  wanted it to mention: %s\n",
                    label, r.error.toUtf8().constData(), fragment);
        ++fails;
    }
}

// ------------------------------------------------------------------ tests

void testArithmeticRoundTrip()
{
    std::memset(g_sig, 0, sizeof(g_sig));
    g_sig[10] = 3000.0f;   // Engine RPM
    g_sig[11] = 90.0f;     // Coolant Temp

    CHECK(runScript(R"(
        function on_tick()
            local rpm = sig("Engine RPM")
            local t   = sig("Coolant Temp")
            setSig("Out A", rpm / 1000 + t * 2)
            setSig("Out B", (rpm - 1000) * 0.5)
        end
    )"));
    CHECK(g_sig[13] == 3.0f + 180.0f);
    CHECK(g_sig[14] == 1000.0f);

    // Precedence must match Lua's: 2 + 3 * 4 = 14, not 20.
    std::memset(g_sig, 0, sizeof(g_sig));
    CHECK(runScript(R"(
        function on_tick()
            setSig("Out A", 2 + 3 * 4)
            setSig("Out B", (2 + 3) * 4)
        end
    )"));
    CHECK(g_sig[13] == 14.0f);
    CHECK(g_sig[14] == 20.0f);

    // Unary minus, parentheses, modulo, and the intrinsics.
    std::memset(g_sig, 0, sizeof(g_sig));
    CHECK(runScript(R"(
        function on_tick()
            setSig("Out A", abs(-7) + min(3, 9) + max(3, 9) + floor(2.7))
            setSig("Out B", clamp(150, 0, 100) + sqrt(16) % 3)
        end
    )"));
    CHECK(g_sig[13] == 7.0f + 3.0f + 9.0f + 2.0f);
    CHECK(g_sig[14] == 100.0f + 1.0f);
}

void testComparisonsAndLogic()
{
    std::memset(g_sig, 0, sizeof(g_sig));
    g_sig[10] = 7000.0f;

    CHECK(runScript(R"(
        function on_tick()
            local rpm = sig("Engine RPM")
            setSig("Out A", rpm > 6000)
            setSig("Out B", rpm > 6000 and rpm < 8000)
        end
    )"));
    CHECK(g_sig[13] == 1.0f);
    CHECK(g_sig[14] == 1.0f);

    g_sig[10] = 3000.0f;
    script_exec_begin_tick();
    script_exec_on_tick();
    CHECK(g_sig[13] == 0.0f);
    CHECK(g_sig[14] == 0.0f);

    // not / or
    std::memset(g_sig, 0, sizeof(g_sig));
    CHECK(runScript(R"(
        function on_tick()
            setSig("Out A", not (1 > 2))
            setSig("Out B", (1 > 2) or (3 > 2))
        end
    )"));
    CHECK(g_sig[13] == 1.0f);
    CHECK(g_sig[14] == 1.0f);
}

void testBranching()
{
    std::memset(g_sig, 0, sizeof(g_sig));

    const char *src = R"(
        function on_tick()
            local t = sig("Coolant Temp")
            if t > 100 then
                setSig("Fan Request", 2)
            elseif t > 85 then
                setSig("Fan Request", 1)
            else
                setSig("Fan Request", 0)
            end
        end
    )";

    const struct { float temp; float want; } cases[] = {
        { 120, 2 }, { 90, 1 }, { 60, 0 }, { 100, 1 }, { 85, 0 },
    };
    for (const auto &c : cases) {
        g_sig[11] = c.temp;
        if (!runScript(src)) {
            return;
        }
        if (g_sig[12] != c.want) {
            std::printf("FAIL  temp %g -> fan %g, want %g\n", c.temp, g_sig[12], c.want);
            ++fails;
        }
    }
}

// The phase-2 headline: a state machine that remembers across ticks.
void testStateMachine()
{
    std::memset(g_sig, 0, sizeof(g_sig));

    // A counter that counts 0,1,2,0,1,2... and an edge-triggered latch.
    const char *src = R"(
        local count = state(0)

        function on_tick()
            count = count + 1
            if count >= 3 then
                count = 0
            end
            setSig("Out A", count)
        end
    )";
    const auto r = compile(src);
    CHECK(r.ok);
    CHECK(r.stateUsed >= 1);
    if (!r.ok) {
        std::printf("  compile error: %s\n", r.error.toUtf8().constData());
        return;
    }
    CHECK(script_exec_load(r.image.constData(), uint32_t(r.image.size()), kSignals)
          == SCRIPT_OK);

    const float expected[] = { 1, 2, 0, 1, 2, 0, 1, 2 };
    for (int i = 0; i < 8; ++i) {
        script_exec_begin_tick();
        CHECK(script_exec_on_tick() == SCRIPT_FAULT_NONE);
        if (g_sig[13] != expected[i]) {
            std::printf("FAIL  tick %d -> %g, want %g\n", i, g_sig[13], expected[i]);
            ++fails;
        }
    }
}

// state(N) initialisers must actually take effect, once, on the first tick.
void testStateInitialiser()
{
    std::memset(g_sig, 0, sizeof(g_sig));

    CHECK(runScript(R"(
        local level = state(100)

        function on_tick()
            setSig("Out A", level)
            level = level - 10
        end
    )", 1));
    CHECK(g_sig[13] == 100.0f);   // the initialiser, not 0

    script_exec_begin_tick();
    script_exec_on_tick();
    CHECK(g_sig[13] == 90.0f);    // and it decremented, not re-initialised
    script_exec_begin_tick();
    script_exec_on_tick();
    CHECK(g_sig[13] == 80.0f);
}

void testLoops()
{
    std::memset(g_sig, 0, sizeof(g_sig));

    // Numeric for, summing 1..10 = 55.
    CHECK(runScript(R"(
        function on_tick()
            local total = 0
            for i = 1, 10 do
                total = total + i
            end
            setSig("Out A", total)
        end
    )"));
    CHECK(g_sig[13] == 55.0f);

    // Descending, with a constant negative step.
    std::memset(g_sig, 0, sizeof(g_sig));
    CHECK(runScript(R"(
        function on_tick()
            local n = 0
            for i = 10, 1, -2 do
                n = n + 1
            end
            setSig("Out A", n)
        end
    )"));
    CHECK(g_sig[13] == 5.0f);   // 10,8,6,4,2

    // while + break
    std::memset(g_sig, 0, sizeof(g_sig));
    CHECK(runScript(R"(
        function on_tick()
            local n = 0
            while true do
                n = n + 1
                if n >= 7 then
                    break
                end
            end
            setSig("Out A", n)
        end
    )"));
    CHECK(g_sig[13] == 7.0f);

    const auto r = compile("function on_tick() local n = 0 while n < 3 do n = n + 1 end end");
    CHECK(r.ok);
    CHECK(r.loopsPresent);   // reported, so the UI can warn about cost
}

// A runaway loop compiles fine — the language allows it — and the BUDGET is
// what stops it. This is the compiler-side half of the liveness story.
void testRunawayLoopIsBudgetKilled()
{
    std::memset(g_sig, 0, sizeof(g_sig));
    const auto r = compile(R"(
        function on_tick()
            local n = 0
            while true do
                n = n + 1
            end
        end
    )");
    CHECK(r.ok);           // legal Lua, legal bytecode
    CHECK(r.loopsPresent);
    if (!r.ok) {
        return;
    }
    CHECK(script_exec_load(r.image.constData(), uint32_t(r.image.size()), kSignals)
          == SCRIPT_OK);
    script_exec_begin_tick();
    CHECK(script_exec_on_tick() == SCRIPT_FAULT_BUDGET);
    CHECK(!script_exec_running());
}

void testRejections()
{
    // Syntax comes from Lua itself, with its line number.
    {
        const auto r = compile("function on_tick()\n  local x = \nend");
        CHECK(!r.ok);
        CHECK(r.errorLine >= 2);
    }

    expectReject("function on_tick() setSig(\"No Such Channel\", 1) end",
                 "no channel named", "unknown channel");
    expectReject("function on_tick() setSig(\"Out A\", sin(1)) end",
                 "correctly rounded", "sin rejected with the determinism reason");
    expectReject("function on_tick() setSig(\"Out A\", 2 ^ 8) end",
                 "correctly rounded", "power operator rejected");
    expectReject("function on_tick() local t = {} end",
                 "expected a value", "table constructor");
    expectReject("function on_tick() setSig(\"Out A\", \"hi\") end",
                 "channel name", "string value");
    expectReject("function on_tick() print(1) end",
                 "setSig", "print suggests setSig");
    expectReject("function on_tick() setSig(\"Out A\", x) end",
                 "not declared", "undeclared variable");
    expectReject("function on_rx() end\nfunction on_tick() end",
                 "phase-3", "on_rx named as a future hook");
    expectReject("function nope() end",
                 "only hook", "non-hook function");
    expectReject("local x = 5\nfunction on_tick() end",
                 "state(0)", "top-level local must be state");
    expectReject("function on_tick() local x = state(0) end",
                 "persist across ticks", "state inside the hook");
    expectReject("function on_tick() for i = 1, 10, x do end end",
                 "constant", "non-constant for step");
    expectReject("function on_tick() for i = 1, 10, 0 do end end",
                 "never finish", "zero for step");
    // `break` outside a loop is a Lua SYNTAX error, so the pre-check catches it
    // and reports Lua's own wording ("break outside loop at line 1") before the
    // subset compiler is reached. That is the two-layer design working, so the
    // fragment matches Lua's message rather than the one in parseStatement —
    // which stays as the backstop for any path Lua would accept.
    expectReject("function on_tick() break end",
                 "outside loop", "break outside a loop (caught by Lua)");
    expectReject("local c = state(0)\nfunction on_tick() setSig(\"Out A\", 1) end\n"
                 "function on_tick() end",
                 "more than once", "duplicate on_tick");
    expectReject("setSig(\"Out A\", 1)",
                 "top level", "statement at file scope");
    expectReject("local c = state(0)",
                 "on_tick", "no hook defined");
    expectReject("function on_tick() setSig(\"Out A\", 1 .. 2) end",
                 "concatenation", "string concat");
}

// Registers are finite and there is nothing to spill to, so exhausting them
// must be a clear compile error rather than silent corruption.
void testRegisterExhaustion()
{
    QString src = QStringLiteral("function on_tick()\n");
    for (int i = 0; i < 80; ++i) {
        src += QStringLiteral("  local v%1 = %1\n").arg(i);
    }
    src += QStringLiteral("  setSig(\"Out A\", v0)\nend\n");
    const auto r = ct::ScriptCompiler::compile(src, symbols());
    CHECK(!r.ok);
    CHECK(r.error.contains(QStringLiteral("too many local"), Qt::CaseInsensitive));
}

// A realistic script: a fan controller with hysteresis and a minimum run timer.
// The kind of thing phase 2 exists for.
void testRealisticFanController()
{
    std::memset(g_sig, 0, sizeof(g_sig));

    const char *src = R"(
        -- Fan with hysteresis: on at 95, off at 88, and once on it stays on
        -- for at least 5 ticks.
        local fanOn   = state(0)
        local holdFor = state(0)

        function on_tick()
            local t = sig("Coolant Temp")

            if holdFor > 0 then
                holdFor = holdFor - 1
            end

            if fanOn > 0 then
                if t < 88 and holdFor <= 0 then
                    fanOn = 0
                end
            else
                if t > 95 then
                    fanOn = 1
                    holdFor = 5
                end
            end

            setSig("Fan Request", fanOn)
        end
    )";

    const auto r = compile(src);
    CHECK(r.ok);
    if (!r.ok) {
        std::printf("  compile error line %d: %s\n", r.errorLine,
                    r.error.toUtf8().constData());
        return;
    }
    std::printf("  info: fan controller = %d instructions, %d state, %d regs, "
                "straight-line cost %u\n",
                r.instructionCount, r.stateUsed, r.registersUsed,
                r.straightLineCost);
    CHECK(script_exec_load(r.image.constData(), uint32_t(r.image.size()), kSignals)
          == SCRIPT_OK);

    auto tick = [](float temp) {
        g_sig[11] = temp;
        script_exec_begin_tick();
        script_exec_on_tick();
        return g_sig[12];
    };

    // The hold counter is decremented at the TOP of the hook and tested later in
    // the same pass, so the tick that takes it 1 -> 0 is also the tick that
    // releases the fan. Four ticks of hold, not five — worth spelling out,
    // because the off-by-one here is in the script's logic, not the compiler's,
    // and it is exactly the kind of thing the simulator exists to reveal.
    CHECK(tick(80) == 0.0f);    // cold: off
    CHECK(tick(96) == 1.0f);    // over 95: on, holdFor = 5
    CHECK(tick(80) == 1.0f);    // 5 -> 4, still held
    CHECK(tick(80) == 1.0f);    // 4 -> 3
    CHECK(tick(80) == 1.0f);    // 3 -> 2
    CHECK(tick(80) == 1.0f);    // 2 -> 1
    CHECK(tick(80) == 0.0f);    // 1 -> 0, and 0 <= 0 releases it this same tick
    CHECK(tick(90) == 0.0f);    // between thresholds: stays off (hysteresis)
    CHECK(tick(96) == 1.0f);    // back over the upper threshold: on again
}

} // namespace

int main()
{
    const ScriptHost host = { hostRead, hostWrite };
    script_exec_init(&host);

    testArithmeticRoundTrip();
    testComparisonsAndLogic();
    testBranching();
    testStateMachine();
    testStateInitialiser();
    testLoops();
    testRunawayLoopIsBudgetKilled();
    testRejections();
    testRegisterExhaustion();
    testRealisticFanController();

    if (fails == 0) {
        std::printf("ALL SCRIPT COMPILER TESTS PASSED\n");
    } else {
        std::printf("%d FAILURES\n", fails);
    }
    return fails ? 1 : 0;
}
