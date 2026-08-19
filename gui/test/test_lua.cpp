// The scripting layer, tested headlessly: the sandbox's fences, the ct.*
// bindings against a real Configuration, and the runner's rollback contract.
//
// The fences get the most attention. A sandbox is exactly as strong as its
// weakest forgotten hole, and each denial test here pins one hole shut: if an
// upgrade of the vendored Lua (or a refactor of lua_sandbox.cpp) reopens io,
// os.execute, require, load() or bytecode chunks, a test fails rather than a
// user finding out.

#include <QCoreApplication>

#include <cstdio>
#include <cstring>

#include "../src/model/configuration.h"
#include "../src/scripting/script_runner.h"

static int failures = 0;

#define CHECK(cond)                                                                              \
    do {                                                                                         \
        if (!(cond)) {                                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                          \
            ++failures;                                                                          \
        }                                                                                        \
    } while (0)

namespace {

QString g_output;

ct::ScriptResult runScript(ct::Configuration &config, const char *source,
                           qint64 timeLimitMs = 10000)
{
    g_output.clear();
    ct::ScriptRunner runner(config);
    runner.setTimeLimitMs(timeLimitMs);
    runner.setOutputHandler([](const QString &line) {
        g_output += line;
        g_output += QLatin1Char('\n');
    });
    return runner.run(QString::fromUtf8(source), QStringLiteral("test"));
}

// ------------------------------------------------------------------ sandbox

void testSandboxFences()
{
    ct::Configuration config;

    // Each entry is a script that MUST fail: it reaches for a capability the
    // sandbox removes. The expected fragment ties the failure to the right
    // cause, so a test that fails for an unrelated reason does not pass.
    const struct {
        const char *script;
        const char *expectFragment;
    } denied[] = {
        { "io.open('x')", "attempt to index" },              // io never opened
        { "os.execute('calc')", "attempt to call" },         // os is the reduced table
        { "os.remove('x')", "attempt to call" },
        { "os.getenv('PATH')", "attempt to call" },
        { "require('socket')", "attempt to call" },          // package never opened
        { "load('return 1')()", "attempt to call" },         // loaders removed
        { "loadfile('x')", "attempt to call" },
        { "dofile('x')", "attempt to call" },
        { "debug.getinfo(1)", "attempt to index" },          // debug never opened
        { "coroutine.create(function() end)", "attempt to index" }, // not opened
    };
    for (const auto &d : denied) {
        const ct::ScriptResult r = runScript(config, d.script);
        CHECK(!r.ok);
        if (!r.error.contains(QLatin1String(d.expectFragment))) {
            std::printf("  script %-38s error was: %s\n", d.script,
                        r.error.toUtf8().constData());
            ++failures;
        }
    }

    // What IS provided works.
    CHECK(runScript(config, "print(('abc'):upper(), math.floor(2.9), os.time())").ok);
    CHECK(g_output.startsWith(QLatin1String("ABC\t2\t")));

    // A precompiled chunk must be refused at load, before the VM sees a byte
    // of it — unvalidated bytecode is the canonical sandbox escape. \27 is
    // LUA_SIGNATURE's first byte; a text-mode load rejects it immediately.
    {
        ct::Configuration c2;
        const ct::ScriptResult r = runScript(c2, "\x1bLua-not-really-bytecode");
        CHECK(!r.ok);
    }

    // The time limit: an infinite loop dies with a clear message, quickly.
    {
        const ct::ScriptResult r = runScript(config, "while true do end", 300);
        CHECK(!r.ok);
        CHECK(r.error.contains(QLatin1String("time limit")));
        CHECK(r.elapsedMs < 5000); // died promptly, not at some OS timeout
    }

    // The memory cap: an allocation bomb dies as "not enough memory" long
    // before it troubles the machine (64 MB cap vs the gigabytes this would
    // otherwise reach within the time limit).
    {
        const ct::ScriptResult r = runScript(
            config, "local t = {} while true do t[#t+1] = string.rep('x', 1e6) end");
        CHECK(!r.ok);
        CHECK(r.error.contains(QLatin1String("not enough memory")));
    }
}

// ------------------------------------------------------------------ bindings

void testChannelBindings()
{
    ct::Configuration config;

    // The headline use case: generate channels in a loop.
    const ct::ScriptResult r = runScript(config, R"lua(
        for i = 1, 50 do
            ct.addChannel{ name = ('Cell Volt %02d'):format(i),
                           quantity = 'Voltage', unit = 'V',
                           dataType = 'u16', baseResolution = 0.001,
                           decimalPlaces = 3, minValue = 0, maxValue = 5 }
        end
        print(ct.channelCount())
    )lua");
    CHECK(r.ok);
    CHECK(r.mutated);
    CHECK(config.catalog().userChannels().size() == 50);
    CHECK(g_output.trimmed() == QLatin1String("50"));

    const ct::Channel c = config.catalog().findByName(QStringLiteral("Cell Volt 07"));
    CHECK(c.isValid());
    CHECK(c.dataType == QLatin1String("u16"));
    CHECK(c.baseResolution == 0.001);
    CHECK(c.decimalPlaces == 3);

    // Duplicates are an error, not a silent update...
    CHECK(!runScript(config, "ct.addChannel{ name = 'Cell Volt 07' }").ok);
    // ...device channels cannot be created or edited...
    CHECK(!runScript(config, "ct.addChannel{ name = 'Device OnTime' }").ok);
    // ...and a name over the wire limit is refused with the limit named.
    {
        const ct::ScriptResult over =
            runScript(config, "ct.addChannel{ name = ('x'):rep(40) }");
        CHECK(!over.ok);
        CHECK(over.error.contains(QLatin1String("31")));
    }

    // setChannel updates in place; removeChannel removes.
    CHECK(runScript(config, "ct.setChannel{ name = 'Cell Volt 07', unit = 'mV' }").ok);
    CHECK(config.catalog().findByName(QStringLiteral("Cell Volt 07")).unit
          == QLatin1String("mV"));
    CHECK(runScript(config, "ct.removeChannel('Cell Volt 50')").ok);
    CHECK(config.catalog().userChannels().size() == 49);

    // Reads see what C++ sees.
    CHECK(runScript(config, "print(ct.channel('Cell Volt 07').unit)").ok);
    CHECK(g_output.trimmed() == QLatin1String("mV"));
    CHECK(runScript(config, "print(ct.channel('No Such Channel'))").ok);
    CHECK(g_output.trimmed() == QLatin1String("nil"));
}

void testSectionBindingsAndRename()
{
    ct::Configuration config;

    const ct::ScriptResult r = runScript(config, R"lua(
        ct.addChannel{ name = 'Engine RPM', dataType = 'u16' }
        ct.addSection(1, {
            name = 'ECU Broadcast', device = 'receive', id = 0x640,
            lengthBytes = 8,
            rows = { { channel = 'Engine RPM', startBit = 0, bitLength = 16,
                       dbcType = 'unsigned', factor = 1, offset = 0 } },
        })
        ct.setBus(1, { enabled = true, rateKbps = 500 })
    )lua");
    CHECK(r.ok);
    CHECK(config.bus[0].enabled);
    CHECK(config.bus[0].rateKbps == 500);
    CHECK(config.bus[0].sections.size() == 1);
    CHECK(config.bus[0].sections[0].baseAddress == 0x640);
    CHECK(config.bus[0].sections[0].rows.size() == 1);
    CHECK(config.bus[0].sections[0].rows[0].channelName == QLatin1String("Engine RPM"));

    // Renaming a channel follows every reference — the same
    // renameChannelReferences walk the Channel Editor uses.
    const ct::ScriptResult ren =
        runScript(config, "print(ct.renameChannel('Engine RPM', 'RPM'))");
    CHECK(ren.ok);
    CHECK(config.bus[0].sections[0].rows[0].channelName == QLatin1String("RPM"));
    CHECK(g_output.trimmed().toInt() >= 1);

    // Duplicate section names on one bus are refused; reading a section back
    // round-trips its rows.
    CHECK(!runScript(config, "ct.addSection(1, { name = 'ECU Broadcast' })").ok);
    CHECK(runScript(config,
                    "print(ct.section(1, 'ECU Broadcast').rows[1].channel)").ok);
    CHECK(g_output.trimmed() == QLatin1String("RPM"));

    // Bus index range is enforced.
    CHECK(!runScript(config, "ct.sections(4)").ok);
    CHECK(!runScript(config, "ct.setBus(0, {})").ok);

    // A row's clamp/roll-over choice survives the read-modify-write this API
    // invites — ct.setSection(b, n, ct.section(b, n)) after changing one field.
    // It is the shape the binding's own comment blesses by name, and a key the
    // read half does not hand out is a key the write half cannot preserve: the
    // row would come back clamping, silently, and the next Send would put a
    // counter that should roll 255 -> 0 on the bus stuck at 255 instead.
    ct::Configuration txCfg;
    const ct::ScriptResult mk = runScript(txCfg, R"lua(
        ct.addChannel{ name = 'Rolling Count', dataType = 'u16' }
        ct.addSection(1, {
            name = 'Counter Out', device = 'transmit', id = 0x300,
            lengthBytes = 8, rateHz = 10,
            rows = { { channel = 'Rolling Count', startBit = 0, bitLength = 8,
                       dbcType = 'unsigned', factor = 1, offset = 0,
                       clampToRange = false } },
        })
    )lua");
    CHECK(mk.ok);
    CHECK(txCfg.bus[0].sections.size() == 1);
    // 1. A script can ASK for roll-over in the first place.
    CHECK(!txCfg.bus[0].sections[0].rows[0].clampToRange);
    // 2. It can SEE it.
    CHECK(runScript(txCfg,
                    "print(tostring(ct.section(1, 'Counter Out').rows[1].clampToRange))").ok);
    CHECK(g_output.trimmed() == QLatin1String("false"));
    // 3. And editing something else does not quietly take it away.
    const ct::ScriptResult rmw = runScript(txCfg, R"lua(
        local s = ct.section(1, 'Counter Out')
        s.rateHz = 20
        ct.setSection(1, 'Counter Out', s)
    )lua");
    CHECK(rmw.ok);
    CHECK(txCfg.bus[0].sections[0].transmitRateHz == 20);
    CHECK(!txCfg.bus[0].sections[0].rows[0].clampToRange);
    // 4. A row that says nothing about it clamps, as every row did before the
    //    option existed.
    ct::Configuration plainCfg;
    CHECK(runScript(plainCfg, R"lua(
        ct.addChannel{ name = 'Plain', dataType = 'u16' }
        ct.addSection(1, { name = 'Plain Out', device = 'transmit', id = 0x301,
            lengthBytes = 8, rateHz = 10,
            rows = { { channel = 'Plain', startBit = 0, bitLength = 8,
                       dbcType = 'unsigned', factor = 1, offset = 0 } } })
    )lua").ok);
    CHECK(plainCfg.bus[0].sections[0].rows[0].clampToRange);
}

void testProtectedComms()
{
    ct::Configuration config;

    // Build a protected section the way a locked document carries one, then
    // conceal it: set the password (allowed while revealed), then conceal.
    ct::CommsSection s;
    s.name = QStringLiteral("Proprietary");
    s.device = ct::SectionDevice::ReceiveMessage;
    s.baseAddress = 0x123;
    s.protection = ct::CommsProtection::Protected;
    // Its own Message Password, which is what actually guards it. The document's
    // Edit Protected Comms password reveals nothing at any tier — a marked
    // section is opened by a grant for THAT section and by nothing else — so a
    // keyless fixture would be concealed from everybody for ever and the
    // "revealed" half of this test could not run at all.
    s.messageKey = ct::deriveAccessKey(QStringLiteral("proprietary-own"));
    ct::CommsChannelRow row;
    row.channelName = QStringLiteral("Secret Value");
    s.rows.append(row);
    config.bus[0].sections.append(s);
    // The catalogue entry the row refers to. l_removeChannel deletes a
    // DEFINITION, so it needs one to exist; without this the removal below would
    // fail on "no channel named" and would look exactly like a protection
    // refusal, which is the assertion it is meant to be inverting.
    ct::Channel secretChannel;
    secretChannel.name = QStringLiteral("Secret Value");
    secretChannel.userDefined = true;
    config.catalog().addOrUpdateUserChannel(secretChannel);
    CHECK(config.setCommsPassword(QStringLiteral("hunter2")));
    config.concealProtectedComms();
    CHECK(!config.commsRevealed());

    // The list shows the name, the tier and the fact of concealment — nothing
    // else. `id` is nil because the CAN identifier is exactly the sort of
    // protocol detail Hidden and Protected withhold.
    CHECK(runScript(config, R"lua(
        local s = ct.sections(1)[1]
        print(s.name, s.protection, s.concealed, s.editLocked, s.id)
    )lua").ok);
    CHECK(g_output.trimmed() == QLatin1String("Proprietary\tprotected\ttrue\ttrue\tnil"));

    // Detail and edits are refused.
    CHECK(!runScript(config, "ct.section(1, 'Proprietary')").ok);
    CHECK(!runScript(config, "ct.setSection(1, 'Proprietary', { id = 0x456 })").ok);
    // The tier itself is not settable from a script IN EITHER DIRECTION, and
    // that refusal is mechanism rather than policy: unticking is authorised by a
    // password prompt or a live device proof, and neither has anywhere to happen
    // inside a batch run. Raising goes with it, so a script cannot conceal a
    // section from the operator running it either.
    CHECK(!runScript(config, "ct.setSection(1, 'Proprietary', { protection = 'none' })").ok);
    CHECK(!runScript(config, "ct.setSection(1, 'Proprietary', { protection = 'hidden' })").ok);

    // ---- unlocked, and the same reads work: the guard is the password ----
    // The SECTION's own password, recorded as a grant — which is what the two
    // dialogs do once they have run every challenge that tier demands. The
    // document-wide reveal is deliberately NOT what opens this: it is a verifier
    // sitting in the same file as the message it would be unlocking, it says
    // nothing about this section's own password, and accepting it here was the
    // master-key substitution the model refuses.
    //
    // Done BEFORE the removal below, deliberately. Removing the section first
    // leaves nothing to read back, and this half of the test then stops being
    // exercised without anything failing to say so.
    CHECK(config.revealProtectedComms(QStringLiteral("hunter2")));
    CHECK(!runScript(config, "ct.section(1, 'Proprietary')").ok); // still withheld
    config.grantSectionAccess(0, config.bus[0].sections[0]);
    CHECK(runScript(config, "print(ct.section(1, 'Proprietary').id)").ok);
    CHECK(g_output.trimmed().toInt() == 0x123);
    // Revealing buys VIEWING and the right to untick. It does not buy editing,
    // and it does not lift the script's refusal to move the tier.
    CHECK(!runScript(config, "ct.setSection(1, 'Proprietary', { id = 0x456 })").ok);
    CHECK(!runScript(config, "ct.setSection(1, 'Proprietary', { protection = 'none' })").ok);
    config.concealProtectedComms();

    // ---- Read Only: visible, still not editable ----
    // The one tier where `concealed` and `editLocked` disagree, and the one with
    // no coverage anywhere before 2.3.0. A script reads every scalar off it —
    // `id` is a real number, not nil — and setSection is still refused.
    {
        ct::CommsSection ro;
        ro.name = QStringLiteral("Calibration Feed");
        ro.device = ct::SectionDevice::ReceiveMessage;
        ro.baseAddress = 0x321;
        ro.protection = ct::CommsProtection::ReadOnly;
        config.bus[0].sections.append(ro);

        CHECK(runScript(config, R"lua(
            for _, s in ipairs(ct.sections(1)) do
                if s.name == 'Calibration Feed' then
                    print(s.protection, s.concealed, s.editLocked, s.id)
                end
            end
        )lua").ok);
        CHECK(g_output.trimmed() == QLatin1String("readOnly\tfalse\ttrue\t801")); // 801 == 0x321
        CHECK(runScript(config, "print(ct.section(1, 'Calibration Feed').id)").ok);
        CHECK(g_output.trimmed().toInt() == 0x321);
        CHECK(!runScript(config, "ct.setSection(1, 'Calibration Feed', { id = 0x456 })").ok);
    }

    // ---- removal is permitted at EVERY tier, concealed or not ----
    // These two calls were pinned as REFUSALS until 2.3.0. Inverting them is the
    // point: l_removeSection was the single place in the host that actually
    // refused a removal, and removal being allowed everywhere is the rule
    // DECISIONS.md repeats. Do not restore the guards to make anything else
    // pass.
    //
    // Why it is not a hole: removal is not a way to READ a concealed message. A
    // viewer who cannot see the protocol cannot retype it, so removing a Hidden
    // or Protected section DESTROYS it rather than revealing it. (For Read Only,
    // which is visible, remove-and-retype genuinely does reproduce the message —
    // which is exactly why Read Only is documented as accident prevention and
    // never as security.)
    CHECK(!config.commsRevealed()); // concealed again, so this is the hard case
    // The channel first, while the concealed section that carries it is still
    // there — otherwise this proves nothing about a channel belonging to a
    // protected message. Deleting the definition does not touch the message's
    // row or its bit layout, which is why it is not a route to changing what a
    // protected message decodes to.
    CHECK(runScript(config, "ct.removeChannel('Secret Value')").ok);
    CHECK(!config.catalog().findByName(QStringLiteral("Secret Value")).isValid());
    CHECK(runScript(config, "ct.removeSection(1, 'Proprietary')").ok);
    CHECK(config.bus[0].sections.size() == 1);
    if (config.bus[0].sections.size() == 1)
        CHECK(config.bus[0].sections[0].name == QLatin1String("Calibration Feed"));
    CHECK(runScript(config, "ct.removeSection(1, 'Calibration Feed')").ok);
    CHECK(config.bus[0].sections.isEmpty());
}

void testMathConstantsValidate()
{
    ct::Configuration config;

    const ct::ScriptResult r = runScript(config, R"lua(
        ct.addChannel{ name = 'Raw Pressure', dataType = 'u16' }
        ct.addChannel{ name = 'Pressure kPa', dataType = 'float' }
        ct.addConstant{ name = 'Cal Factor', dataType = 'float', value = 1.25 }
        ct.addMath{ op = ct.ops.MULADD, a = 'Raw Pressure', b = 0.75, c = 12.5,
                    dest = 'Pressure kPa' }
        print(#ct.mathRows(), ct.mathRows()[1].opName)
    )lua");
    CHECK(r.ok);
    CHECK(config.mathRows.size() == 1);
    CHECK(config.mathRows[0].op == 26); // MULADD
    CHECK(config.mathRows[0].aIsChannel);
    CHECK(config.mathRows[0].aChannel == QLatin1String("Raw Pressure"));
    CHECK(!config.mathRows[0].bIsChannel);
    CHECK(config.mathRows[0].bConst == 0.75);
    CHECK(config.constantRows.size() == 1);
    CHECK(config.constantRows[0].value == 1.25);
    CHECK(g_output.trimmed() == QLatin1String("1\tMULADD"));

    // Unknown op names are refused by name.
    CHECK(!runScript(config, "ct.addMath{ op = 'FROBNICATE', dest = 'X' }").ok);

    // validate() runs the real validator and reports in script-readable form.
    CHECK(runScript(config, R"lua(
        local issues = ct.validate()
        print(type(issues), #issues >= 0)
    )lua").ok);
    CHECK(g_output.trimmed() == QLatin1String("table\ttrue"));

    CHECK(runScript(config, "ct.removeMath(1)").ok);
    CHECK(config.mathRows.isEmpty());
    CHECK(!runScript(config, "ct.removeMath(1)").ok); // now out of range
}

// ------------------------------------------------------------------ rollback

void testRollbackOnError()
{
    ct::Configuration config;
    config.setConfigTitle(QStringLiteral("Before"));
    CHECK(runScript(config, "ct.addChannel{ name = 'Survivor' }").ok);

    // A script that mutates, then dies. Everything it did must be undone —
    // including the title, which copyContentTo does not carry and the runner
    // saves by hand (this test is what notices if that ever breaks).
    const ct::ScriptResult r = runScript(config, R"lua(
        ct.setTitle('After')
        ct.addChannel{ name = 'Doomed 1' }
        ct.addChannel{ name = 'Doomed 2' }
        ct.addSection(2, { name = 'Doomed Section', device = 'transmit' })
        error('boom')
    )lua");
    CHECK(!r.ok);
    CHECK(r.rolledBack);
    CHECK(r.error.contains(QLatin1String("boom")));
    CHECK(config.configTitle() == QLatin1String("Before"));
    CHECK(config.catalog().userChannels().size() == 1);
    CHECK(config.catalog().findByName(QStringLiteral("Survivor")).isValid());
    CHECK(config.bus[1].sections.isEmpty());

    // A failed script that never wrote anything reports rolledBack = false —
    // the console uses that to say "nothing was changed" honestly.
    const ct::ScriptResult clean = runScript(config, "error('early')");
    CHECK(!clean.ok);
    CHECK(!clean.rolledBack);

    // The time limit triggers the same rollback as error().
    const ct::ScriptResult loop = runScript(
        config, "ct.addChannel{ name = 'Doomed 3' } while true do end", 300);
    CHECK(!loop.ok);
    CHECK(loop.rolledBack);
    CHECK(!config.catalog().findByName(QStringLiteral("Doomed 3")).isValid());
}

// Fresh state per run: globals must not leak between runs.
void testStateIsolation()
{
    ct::Configuration config;
    CHECK(runScript(config, "leak = 42").ok);
    CHECK(runScript(config, "print(leak)").ok);
    CHECK(g_output.trimmed() == QLatin1String("nil"));
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    testSandboxFences();
    testChannelBindings();
    testSectionBindingsAndRename();
    testProtectedComms();
    testMathConstantsValidate();
    testRollbackOnError();
    testStateIsolation();

    if (failures == 0) {
        std::printf("ALL LUA SCRIPTING TESTS PASSED\n");
    } else {
        std::printf("%d FAILURES\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
