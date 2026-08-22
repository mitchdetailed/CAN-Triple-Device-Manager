// The desktop simulator and the script's path to a device.
//
// Two things are on trial here, and they are the two that let a person trust a
// script they have never seen run on hardware:
//
//   1. THE SIMULATOR IS THE DEVICE. ScriptSimulator drives script_exec.c — the
//      firmware's own interpreter, compiled into the configurator — so a tick
//      here and a tick on the unit are the same code over the same bytes. These
//      tests check the things a wrapper can still get wrong around that: state
//      persisting across ticks, reset() genuinely reproducing a config load,
//      the cost figure being THIS tick's, and a runaway faulting instead of
//      hanging.
//
//   2. THE SCRIPT ACTUALLY TRAVELS. A script that compiles, simulates and then
//      never reaches the device is worse than no script at all, because
//      everything looked right. mapWithScript() is what a Send calls, so it is
//      what is tested: the chunks it produces are reassembled and handed back to
//      the DEVICE's verifier, which is exactly what the firmware will do with
//      them.

#include <QByteArray>
#include <QFile>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "../src/model/channel.h"
#include "../src/model/config_file.h"
#include "../src/model/configuration.h"
#include "../src/model/device_mapper.h"
#include "../src/scripting/script_compiler.h"
#include "../src/scripting/script_disassembler.h"
#include "../src/scripting/script_simulator.h"

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

// The body of a format-2 .ct3, for the checks that assert on what the writer
// PUT IN THE FILE rather than on what a reload produces. Reading the file as
// JSON is what this used to do and it no longer works: the body is sealed, and
// going through the container is the only honest way to look at it.
static QJsonObject configBodyOf(const QString &path)
{
    QByteArray plain;
    if (!ct::readBinaryConfigFile(path, &plain, nullptr, nullptr))
        return QJsonObject();
    return QJsonDocument::fromJson(plain).object();
}

#define CHECK_NEAR(a, b)                                                             \
    do {                                                                             \
        const double va = double(a), vb = double(b);                                 \
        if (std::fabs(va - vb) > 1e-4) {                                             \
            std::printf("FAIL %s:%d  %s == %g, expected %g\n", __FILE__, __LINE__,   \
                        #a, va, vb);                                                 \
            ++fails;                                                                 \
        }                                                                            \
    } while (0)

namespace {

using namespace ct;

ScriptSymbols symbols()
{
    ScriptSymbols s;
    s.signalIndex.insert(QStringLiteral("Engine RPM"), 10);
    s.signalIndex.insert(QStringLiteral("Fan Request"), 12);
    s.signalIndex.insert(QStringLiteral("Out A"), 13);
    return s;
}

QByteArray compileOrDie(const char *src)
{
    const auto r = ScriptCompiler::compile(QString::fromUtf8(src), symbols());
    if (!r.ok) {
        std::printf("FAIL  compile: line %d: %s\n", r.errorLine,
                    r.error.toUtf8().constData());
        ++fails;
        return QByteArray();
    }
    return r.image;
}

// ---------------------------------------------------------------------------

void testStepDrivesChannels()
{
    ScriptSimulator sim;
    const QByteArray img = compileOrDie(
        "function on_tick()\n"
        "    setSig(\"Fan Request\", sig(\"Engine RPM\") * 2)\n"
        "end\n");
    CHECK(sim.load(img) == SCRIPT_OK);

    sim.setSignal(10, 1500);
    const auto r = sim.step(symbols().signalIndex);
    CHECK(r.fault == SCRIPT_FAULT_NONE);
    CHECK_NEAR(sim.signal(12), 3000);

    // The channel list is the watched set, sorted by name, and carries the
    // write flag the editor tints on. "Engine RPM" was seeded, not written;
    // "Fan Request" was written. If those two were indistinguishable the editor
    // could not show cause and effect, which is most of the point of stepping.
    CHECK(r.channels.size() == 3);
    bool sawSeeded = false, sawWritten = false;
    for (const auto &cv : r.channels) {
        if (cv.name == QLatin1String("Engine RPM")) {
            sawSeeded = !cv.writtenByScript;
            CHECK_NEAR(cv.value, 1500);
        }
        if (cv.name == QLatin1String("Fan Request")) {
            sawWritten = cv.writtenByScript;
            CHECK_NEAR(cv.value, 3000);
        }
    }
    CHECK(sawSeeded);
    CHECK(sawWritten);
}

void testStatePersistsAcrossTicks()
{
    ScriptSimulator sim;
    const QByteArray img = compileOrDie(
        "local count = state(0)\n"
        "function on_tick()\n"
        "    count = count + 1\n"
        "    setSig(\"Out A\", count)\n"
        "end\n");
    CHECK(sim.load(img) == SCRIPT_OK);

    for (int i = 0; i < 5; ++i) {
        sim.step(symbols().signalIndex);
    }
    CHECK_NEAR(sim.signal(13), 5);
    CHECK(sim.tickCount() == 5);

    // reset() must RELOAD, not merely zero the registers: a state() initialiser
    // runs at load time, so a reset that only cleared memory would leave a
    // script whose counter starts at 0 indistinguishable from one whose counter
    // starts at 100 — and only the second is wrong.
    sim.reset();
    CHECK(sim.tickCount() == 0);
    CHECK_NEAR(sim.signal(13), 0);
    const auto r = sim.step(symbols().signalIndex);
    CHECK(r.fault == SCRIPT_FAULT_NONE);
    CHECK_NEAR(sim.signal(13), 1);
}

void testResetRerunsInitialisers()
{
    ScriptSimulator sim;
    const QByteArray img = compileOrDie(
        "local seed = state(100)\n"
        "function on_tick()\n"
        "    setSig(\"Out A\", seed)\n"
        "    seed = seed + 1\n"
        "end\n");
    CHECK(sim.load(img) == SCRIPT_OK);
    sim.step(symbols().signalIndex);
    sim.step(symbols().signalIndex);
    CHECK_NEAR(sim.signal(13), 101);   // second tick saw the incremented value
    sim.reset();
    sim.step(symbols().signalIndex);
    CHECK_NEAR(sim.signal(13), 100);   // back to the initialiser, not to zero
}

void testCostIsThisTick()
{
    // The cost the editor displays has to be the tick just run. It used to be
    // the one before it: the VM stamped last_cost at the START of a tick, so
    // every reader between a tick and the next saw stale accounting — which for
    // a budget display is not a rounding error, it is the wrong answer to "am I
    // close to the limit?". A cheap script and an expensive one are compared
    // here because a constant-but-wrong figure would pass either alone.
    ScriptSimulator cheapSim;
    CHECK(cheapSim.load(compileOrDie("function on_tick()\n"
                                     "    setSig(\"Out A\", 1)\n"
                                     "end\n")) == SCRIPT_OK);
    const quint32 cheap = cheapSim.step(symbols().signalIndex).cost;
    CHECK(cheap > 0);
    // Stable tick to tick — the same work costs the same, every time.
    CHECK(cheapSim.step(symbols().signalIndex).cost == cheap);

    ScriptSimulator loopSim;
    CHECK(loopSim.load(compileOrDie("function on_tick()\n"
                                    "    local total = 0\n"
                                    "    for i = 1, 50 do total = total + i end\n"
                                    "    setSig(\"Out A\", total)\n"
                                    "end\n")) == SCRIPT_OK);
    const auto loop = loopSim.step(symbols().signalIndex);
    CHECK(loop.fault == SCRIPT_FAULT_NONE);
    CHECK_NEAR(loopSim.signal(13), 1275);   // 50*51/2
    CHECK(loop.cost > cheap * 10);
    // And it is stable too, which is the actual regression guard: a stamp taken
    // at the start of a tick would report the CHEAP figure on the first loop
    // tick and the loop figure thereafter.
    CHECK(loopSim.step(symbols().signalIndex).cost == loop.cost);
}

void testRunawayFaultsRatherThanHangs()
{
    ScriptSimulator sim;
    // A loop bounded by a state register the script keeps raising: legal
    // bytecode, verifiable, and unbounded at run time. Only the interpreter's
    // per-dispatch budget stops it — which is exactly the situation the editor
    // has to be able to show a user without freezing.
    const QByteArray img = compileOrDie(
        "local limit = state(0)\n"
        "function on_tick()\n"
        "    limit = limit + 100000\n"
        "    local i = 0\n"
        "    while i < limit do i = i + 1 end\n"
        "    setSig(\"Out A\", i)\n"
        "end\n");
    CHECK(sim.load(img) == SCRIPT_OK);
    const auto r = sim.step(symbols().signalIndex);
    CHECK(r.fault == SCRIPT_FAULT_BUDGET);
    CHECK(r.cost > 0);
    CHECK(r.cost <= sim.budget());

    // R4: the state the faulted tick touched is rolled back, so `limit` is
    // still 0 rather than 100000. And R5: it stays suspended, so the next tick
    // costs nothing at all rather than burning the budget again forever.
    const auto after = sim.step(symbols().signalIndex);
    CHECK(after.fault == SCRIPT_FAULT_NONE);
    CHECK(after.cost == 0);
    CHECK(after.state.value(0) == 0.0f);
}

void testEmptyAndUnloadedAreQuiet()
{
    ScriptSimulator sim;
    // Stepping with nothing loaded must not crash or invent a fault — the
    // editor steps before the first compile if the user is quick.
    const auto r = sim.step(symbols().signalIndex);
    CHECK(r.fault == SCRIPT_FAULT_NONE);
    CHECK(r.cost == 0);

    // An empty hook is legal, costs almost nothing and does nothing. (A script
    // with NO hook at all is a compile error rather than a no-op — the VM would
    // accept it, but a file that can never run is far more likely a mistake than
    // an intention, so the compiler refuses it.)
    CHECK(sim.load(compileOrDie("function on_tick()\nend\n")) == SCRIPT_OK);
    const auto empty = sim.step(symbols().signalIndex);
    CHECK(empty.fault == SCRIPT_FAULT_NONE);
    CHECK(empty.cost > 0 && empty.cost < 10);

    // Garbage is refused rather than run.
    CHECK(sim.load(QByteArray(64, '\x01')) != SCRIPT_OK);
}

// ------------------------------------------------------------ the send path

// A configuration the mapper will accept, with one transmit message carrying
// the channels a script can name.
void buildConfig(Configuration &config, const char *script)
{
    config.clear();
    config.bus[0].enabled = true;
    config.bus[0].rateKbps = 500;

    CommsSection section;
    section.name = QStringLiteral("Script Out");
    section.device = SectionDevice::TransmitMessage;
    // Intel byte order: a DBC start bit is then the field's LSB and counts
    // upward, so 0 and 16 are two whole 16-bit fields in an 8-byte message.
    // Motorola (the model's default) counts the other way and would put a
    // 16-bit field at start bit 0 off the front of the frame.
    section.alignment = SectionAlignment::WordSwap;
    section.baseAddress = 0x400;
    section.transmitRateHz = 10;
    section.messageLengthBytes = 8;
    int bit = 0;
    for (const char *name : { "Engine RPM", "Fan Request" }) {
        Channel ch;
        ch.name = QString::fromUtf8(name);
        ch.dataType = QStringLiteral("u16");
        ch.baseResolution = 1.0;
        ch.decimalPlaces = 0;
        config.catalog().addOrUpdateUserChannel(ch);

        CommsChannelRow row;
        row.channelName = ch.name;
        row.startBit = bit;
        row.bitLength = 16;
        section.rows.append(row);
        bit += 16;
    }
    config.bus[0].sections.append(section);
    config.setScriptSource(QString::fromUtf8(script));
}

void testMapWithScriptChunks()
{
    Configuration config;
    buildConfig(config,
                "function on_tick()\n"
                "    setSig(\"Fan Request\", sig(\"Engine RPM\") > 6000)\n"
                "end\n");

    const MappingResult mapped = mapWithScript(config);
    if (!mapped.ok()) {
        std::printf("FAIL  mapWithScript: %s\n",
                    mapped.errors.join(QStringLiteral("; ")).toUtf8().constData());
        ++fails;
        return;
    }
    CHECK(!mapped.tables.scriptChunks.isEmpty());

    // Reassemble the chunks the way the FIRMWARE does — one contiguous run of
    // 64-byte slots, length = chunk count * 64 — and hand the result to the
    // device's own verifier. This is the assertion that says the script will be
    // accepted by the unit, rather than merely produced by the compiler.
    QByteArray image;
    for (const ct::ScriptChunk &c : mapped.tables.scriptChunks) {
        image.append(reinterpret_cast<const char *>(c.b), int(sizeof(c.b)));
    }
    CHECK(image.size() % 64 == 0);
    CHECK(script_verify(image.constData(), quint32(image.size()), MAX_SIGNALS)
          == SCRIPT_OK);

    // And it runs, producing the value the source asked for. Compiling to
    // something verifiable but wrong is the failure this catches.
    ScriptSimulator sim;
    CHECK(sim.load(image) == SCRIPT_OK);
    ScriptSymbols syms;
    CHECK(ScriptSymbols::fromConfiguration(config, &syms, nullptr));
    sim.setSignal(syms.signalIndex.value(QStringLiteral("Engine RPM")), 7000);
    sim.step(syms.signalIndex);
    CHECK_NEAR(sim.signal(syms.signalIndex.value(QStringLiteral("Fan Request"))), 1);
}

void testNoScriptMeansNoChunks()
{
    Configuration config;
    buildConfig(config, "");
    const MappingResult mapped = mapWithScript(config);
    CHECK(mapped.ok());
    // Zero chunks, not one empty one. The count IS the presence flag on the
    // device, so an empty script and a stub script are different things: this
    // is what CLEARS a script off a unit.
    CHECK(mapped.tables.scriptChunks.isEmpty());
}

void testBadScriptBlocksTheSend()
{
    Configuration config;
    buildConfig(config,
                "function on_tick()\n"
                "    setSig(\"No Such Channel\", 1)\n"
                "end\n");
    const MappingResult mapped = mapWithScript(config);
    // A compile failure has to arrive as a MAPPING error, because that is what
    // every Send path already checks. Reported some other way it would be a
    // warning nobody reads, and the unit would be left running the PREVIOUS
    // script against the NEW tables.
    CHECK(!mapped.ok());
    bool named = false;
    for (const QString &e : mapped.errors) {
        named = named || e.contains(QLatin1String("No Such Channel"));
    }
    CHECK(named);
}

void testChunkingIsExact()
{
    Configuration config;
    buildConfig(config,
                "function on_tick()\n"
                "    setSig(\"Fan Request\", sig(\"Engine RPM\"))\n"
                "end\n");
    ScriptSymbols syms;
    CHECK(ScriptSymbols::fromConfiguration(config, &syms, nullptr));

    const auto r = ScriptCompiler::compile(config.scriptSource(), syms);
    CHECK(r.ok);

    // ct::ScriptChunk, not the firmware's: protocol.h (via script_exec.h) puts
    // an identical struct in the global namespace, and `using namespace ct`
    // makes the bare name ambiguous. They are the same 64 bytes by
    // static_assert — the qualification is only to say which spelling.
    QVector<ct::ScriptChunk> chunks;
    QString error;
    CHECK(ScriptCompiler::attachTo(config, syms, &chunks, &error));
    // Exactly enough chunks to hold the image and no more; the tail is zeroed
    // rather than left as whatever the vector was reused from, because those
    // bytes are written to flash.
    CHECK(chunks.size() == (r.image.size() + 63) / 64);
    QByteArray image;
    for (const ct::ScriptChunk &c : chunks) {
        image.append(reinterpret_cast<const char *>(c.b), int(sizeof(c.b)));
    }
    CHECK(image.left(r.image.size()) == r.image);
    for (int i = r.image.size(); i < image.size(); ++i) {
        CHECK(image[i] == '\0');
    }
}

// ---------------------------------------------------------------------------
// Retaining a device's compiled script through Get -> Send
// ---------------------------------------------------------------------------
//
// A Get used to throw the device's bytecode away and warn about it; the next
// Send then silently removed a running script from the unit. These tests are
// the whole of the replacement, and the one that matters is the byte
// comparison: a LENGTH check would pass against a host that quietly re-encoded
// the image on the way through, which is precisely the failure "byte for byte"
// exists to exclude.

// The chunk table as one contiguous run of bytes — the shape the firmware
// stores and the only shape in which "identical" is a meaningful claim.
QByteArray chunkBytes(const QVector<ct::ScriptChunk> &chunks)
{
    QByteArray out;
    for (const ct::ScriptChunk &c : chunks) {
        out.append(reinterpret_cast<const char *>(c.b), int(sizeof(c.b)));
    }
    return out;
}

// What a Get REALLY comes back with. A device answers a range read over the
// table's full capacity and zero-fills every slot past what it has stored
// (engine_core.c, engine_table_read), so the script table arrives as 512 chunks
// of which most are zeros — never as the handful the Send wrote. Padding here
// is what makes these tests exercise the trimming; without it they would prove
// the round trip only for the one input shape that never occurs.
DeviceTables asReadFromDevice(const DeviceTables &sent)
{
    DeviceTables read = sent;
    read.scriptChunks.resize(MAX_SCRIPT_CHUNKS);
    for (int i = sent.scriptChunks.size(); i < read.scriptChunks.size(); ++i) {
        std::memset(&read.scriptChunks[i], 0, sizeof(ct::ScriptChunk));
    }
    return read;
}

const char kEchoScript[] =
    "function on_tick()\n"
    "    setSig(\"Fan Request\", sig(\"Engine RPM\") > 6000)\n"
    "end\n";

// A document that has been read off a device running kEchoScript: no source, a
// retained image. `compiledOut` receives the bytes the compiler actually
// emitted, which is what the retained image has to equal.
void getFromDevice(Configuration *out, QByteArray *compiledOut, QStringList *notesOut)
{
    Configuration authored;
    buildConfig(authored, kEchoScript);

    ScriptSymbols syms;
    CHECK(ScriptSymbols::fromConfiguration(authored, &syms, nullptr));
    const auto compiled = ScriptCompiler::compile(authored.scriptSource(), syms);
    CHECK(compiled.ok);
    *compiledOut = compiled.image;

    const MappingResult sent = mapWithScript(authored);
    CHECK(sent.ok());
    mapFromDevice(asReadFromDevice(sent.tables), *out, notesOut);
}

void testGetKeepsTheBytecodeAndSendsItBackByteForByte()
{
    Configuration authored;
    buildConfig(authored, kEchoScript);
    const MappingResult sent = mapWithScript(authored);
    CHECK(sent.ok());

    Configuration fresh;   // FRESH: nothing of the authored document leaks in
    QByteArray compiledImage;
    QStringList notes;
    getFromDevice(&fresh, &compiledImage, &notes);

    // What a Get can and cannot recover. There is no source — bytecode does not
    // decompile — and the document says so by holding none.
    CHECK(fresh.scriptSource().isEmpty());
    CHECK(!fresh.hasScriptSource());
    // THE ASSERTION. Not a size, not a verifier verdict: the exact bytes the
    // compiler emitted, trimmed of the chunk padding the device added.
    CHECK(fresh.scriptBytecode() == compiledImage);

    bool told = false;
    for (const QString &n : notes) {
        told = told || n.contains(QLatin1String("byte for byte"));
    }
    CHECK(told);

    // And back out again. Same chunk count, same bytes, including the zero
    // padding — a host that re-encoded, re-padded or re-CRCed anything would
    // produce something the device would still accept and this would still
    // catch.
    const MappingResult resent = mapWithScript(fresh);
    CHECK(resent.ok());
    CHECK(resent.tables.scriptChunks.size() == sent.tables.scriptChunks.size());
    CHECK(chunkBytes(resent.tables.scriptChunks) == chunkBytes(sent.tables.scriptChunks));

    // A third trip, because the trimming is what stops the image growing by a
    // chunk of padding on every pass and only a second round trip can show it.
    Configuration again;
    mapFromDevice(asReadFromDevice(resent.tables), again, nullptr);
    CHECK(again.scriptBytecode() == compiledImage);
    const MappingResult third = mapWithScript(again);
    CHECK(third.ok());
    CHECK(chunkBytes(third.tables.scriptChunks) == chunkBytes(sent.tables.scriptChunks));
}

void testASourceSupersedesTheRetainedBytecode()
{
    Configuration doc;
    QByteArray retainedImage;
    getFromDevice(&doc, &retainedImage, nullptr);
    CHECK(doc.scriptBytecode() == retainedImage);

    // Writing a script is what the Script Editor does. The retained image must
    // be GONE from the document at that instant, not merely out-ranked at Send
    // time: a document holding both would have to pick a winner everywhere it
    // was asked, and one site picking differently from another puts a script
    // nobody has read into a vehicle.
    const char kReplacement[] =
        "function on_tick()\n"
        "    setSig(\"Fan Request\", 0)\n"
        "end\n";
    doc.setScriptSource(QString::fromUtf8(kReplacement));
    CHECK(doc.scriptBytecode().isEmpty());

    ScriptSymbols syms;
    CHECK(ScriptSymbols::fromConfiguration(doc, &syms, nullptr));
    const auto compiled = ScriptCompiler::compile(doc.scriptSource(), syms);
    CHECK(compiled.ok);

    const MappingResult mapped = mapWithScript(doc);
    CHECK(mapped.ok());
    const QByteArray out = chunkBytes(mapped.tables.scriptChunks);
    // The COMPILED SOURCE went, and the retained image did not. Both halves are
    // asserted: "it sent the source" alone would pass if the two images happened
    // to be the same length and the comparison were sloppy.
    CHECK(out.left(compiled.image.size()) == compiled.image);
    CHECK(out.left(retainedImage.size()) != retainedImage);
}

void testEveryWriterKeepsThePairInStep()
{
    Configuration doc;
    QByteArray retainedImage;
    getFromDevice(&doc, &retainedImage, nullptr);
    CHECK(!doc.scriptBytecode().isEmpty());

    // A copy carries BOTH halves. Copying only the source is the mistake this
    // shape of change has made before (m_accessVerifiers), and here it would
    // make a live view or a scratch copy map as a document with no script — so
    // Verify would report the unit's script as an unexplained difference.
    Configuration copy;
    doc.copyContentTo(copy);
    CHECK(copy.scriptBytecode() == retainedImage);
    CHECK(copy.scriptSource().isEmpty());

    // A copy of a SOURCE document carries no bytecode, which is the same
    // invariant seen from the other side.
    Configuration authored;
    buildConfig(authored, kEchoScript);
    Configuration authoredCopy;
    authored.copyContentTo(authoredCopy);
    CHECK(authoredCopy.scriptSource() == authored.scriptSource());
    CHECK(authoredCopy.scriptBytecode().isEmpty());

    // clearContent() — what a Get calls before it maps — clears both, so an
    // incoming device image can never land beside the outgoing document's
    // source.
    doc.clearContent();
    CHECK(doc.scriptBytecode().isEmpty());
    CHECK(doc.scriptSource().isEmpty());

    // clear() goes through clearContent(), so File > New does too.
    Configuration doc2;
    QByteArray image2;
    getFromDevice(&doc2, &image2, nullptr);
    CHECK(!doc2.scriptBytecode().isEmpty());
    doc2.clear();
    CHECK(doc2.scriptBytecode().isEmpty());

    // Whitespace is NOT a source. If setScript used a different emptiness test
    // from the one attachTo compiles by, a source of "  " would drop the
    // retained image here and then compile to nothing at Send — silently
    // stripping the device, which is the bug this feature exists to remove.
    Configuration doc3;
    QByteArray image3;
    getFromDevice(&doc3, &image3, nullptr);
    doc3.setScriptBytecode(image3);
    CHECK(doc3.scriptBytecode() == image3);
}

void testACorruptImageIsRefusedAtGet()
{
    Configuration authored;
    buildConfig(authored, kEchoScript);
    const MappingResult sent = mapWithScript(authored);
    CHECK(sent.ok());

    DeviceTables damaged = asReadFromDevice(sent.tables);
    // One bit, in the code rather than the header, so the CRC is what catches
    // it — the check that stands between a truncated or mangled transfer and a
    // device executing it.
    damaged.scriptChunks[1].b[0] ^= 0xFF;

    Configuration doc;
    QStringList notes;
    mapFromDevice(damaged, doc, &notes);

    // NOTHING retained. Storing a corrupt image would move the failure to the
    // next Send, after CLEAR_CONFIG has erased the unit.
    CHECK(doc.scriptBytecode().isEmpty());
    // And it is SAID. A silent drop is the old behaviour under a new name.
    bool told = false;
    for (const QString &n : notes) {
        told = told || n.contains(QLatin1String("not a valid script image"));
    }
    CHECK(told);
    // The rest of the Get still landed: one bad table must not cost the user
    // the configuration they were reading back.
    CHECK(!doc.bus[0].sections.isEmpty());

    // A device with NO script is not a device with a broken one. Every slot
    // past the stored prefix reads back zero, so a unit with a perfectly good
    // configuration and no script hands over 32 KB of zeros — which must be
    // silence, not a corruption warning on every Get.
    DeviceTables scriptless = sent.tables;
    scriptless.scriptChunks.clear();
    scriptless = asReadFromDevice(scriptless);
    CHECK(scriptless.scriptChunks.size() == MAX_SCRIPT_CHUNKS);
    Configuration quiet;
    QStringList quietNotes;
    mapFromDevice(scriptless, quiet, &quietNotes);
    CHECK(quiet.scriptBytecode().isEmpty());
    for (const QString &n : quietNotes) {
        CHECK(!n.contains(QLatin1String("script")));
    }
}

void testACorruptImageIsRefusedBeforeTheSendClearsTheDevice()
{
    // The image reaches this Send through a FILE, which is the only way a
    // corrupt one can exist in a document at all: the setter validates nothing
    // and the Get refuses anything invalid, so a .ct3 that was hand-edited,
    // truncated by whatever copied it, or written by a future format is the real
    // route. loadFromFile deliberately does not verify — it has nowhere to say
    // so — and this is the check that catches it instead.
    Configuration doc;
    QByteArray image;
    getFromDevice(&doc, &image, nullptr);

    QTemporaryDir dir;
    CHECK(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("retained.ct3"));
    QString error;
    CHECK(doc.saveToFile(path, &error));

    // Corrupt the stored image the way a damaged file would: same length, wrong
    // bytes, so nothing but the verifier can tell.
    QJsonObject root = configBodyOf(path);
    QByteArray stored =
        QByteArray::fromBase64(root.value(QStringLiteral("scriptBytecode")).toString().toLatin1());
    CHECK(stored == image);
    // Bail rather than index into it if the file did not carry the image: a
    // regression in the writer would otherwise crash this test instead of
    // failing it, and a crash takes the other checks' output with it.
    if (stored.size() <= int(sizeof(ScriptHeader)) + 3) {
        return;
    }
    stored[int(sizeof(ScriptHeader)) + 3] = char(stored.at(int(sizeof(ScriptHeader)) + 3) ^ 0x5A);
    root[QStringLiteral("scriptBytecode")] = QString::fromLatin1(stored.toBase64());
    // Written back through the container, not as raw JSON. Re-sealing keeps
    // this a test of the CURRENT format: a rewrite as JSON would still load,
    // because format 1 is still read, and the case would have quietly stopped
    // covering the writer it was aimed at.
    CHECK(ct::writeBinaryConfigFile(path,
                                    QJsonDocument(root).toJson(QJsonDocument::Compact),
                                    root.value(QStringLiteral("fileVersion")).toInt(),
                                    QStringLiteral("test"), &error));

    Configuration loaded;
    CHECK(loaded.loadFromFile(path, &error));
    // The load KEPT it — the refusal has to come from the Send, not from a
    // loader that quietly emptied the field and left the user wondering.
    CHECK(loaded.scriptBytecode() == stored);

    const MappingResult mapped = mapWithScript(loaded);
    // Refused as a MAPPING error, because that is what every Send path already
    // checks before it builds a transfer — so the refusal lands before
    // CLEAR_CONFIG erases the unit, not after.
    CHECK(!mapped.ok());
    bool named = false;
    for (const QString &e : mapped.errors) {
        named = named || e.contains(QLatin1String("not a valid script image"));
    }
    CHECK(named);
    // Refusing is not editing. The document still holds exactly what it held.
    CHECK(loaded.scriptBytecode() == stored);
    CHECK(loaded.scriptSource().isEmpty());

    // Negative control: the SAME document with the image intact sends fine, so
    // the refusal above is about the corruption and not about the path.
    CHECK(mapWithScript(doc).ok());
}

// ---------------------------------------------------------------------------
// The read-only disassembly
// ---------------------------------------------------------------------------
//
// A document that came off a device holds bytecode and no source, so the only
// way a user can see what their unit is running is this listing. That makes it
// a claim about the bytes, and a wrong claim here is worse than no listing:
// somebody would read it, believe it, and send the image back anyway.
//
// So it is checked against a HAND-ASSEMBLED image — every opcode placed by
// hand, field by field, the way firmware/tools/hwtest/script_asm.py builds one
// from script_vm.h — rather than against whatever the compiler happens to emit.
// Decoding the compiler's output and comparing it with the compiler's own idea
// of what it emitted would agree even if both sides read the instruction format
// the same wrong way; a hand-built image is a third reading, and the operand
// text below is what script_asm.disassemble() prints for the same bytes.

QByteArray oneInstruction(quint8 op, quint8 dst, quint8 a, quint8 b, quint32 imm)
{
    ScriptInstr in{};
    in.op = op;
    in.dst = dst;
    in.a = a;
    in.b = b;
    in.imm = imm;
    QByteArray out(int(sizeof(in)), '\0');
    std::memcpy(out.data(), &in, sizeof(in));
    return out;
}

// LOADK carries a float's BIT PATTERN in imm, not its value.
quint32 floatBits(float value)
{
    quint32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

// A header in front of `code`, with every field at the only value a v1 device
// accepts. The CRC is the firmware's own script_crc32 over the code alone.
QByteArray imageAround(const QByteArray &code, quint16 numState, quint32 entryTick)
{
    ScriptHeader h{};
    h.magic = SCRIPT_MAGIC;
    h.version = SCRIPT_BYTECODE_VERSION;
    h.num_state = numState;
    h.code_bytes = quint32(code.size());
    h.code_crc32 = script_crc32(code.constData(), quint32(code.size()));
    h.entry_tick = entryTick;
    h.entry_rx = SCRIPT_NO_ENTRY;
    h.entry_tx = SCRIPT_NO_ENTRY;
    h.reserved = 0;
    QByteArray out(int(sizeof(h)), '\0');
    std::memcpy(out.data(), &h, sizeof(h));
    return out + code;
}

void testTheListingNamesEveryOpcode()
{
    // One of each opcode the device runs, plus one arithmetic op of each arity,
    // because arity is what decides how many registers an instruction's operand
    // text may show — a listing that printed three for a unary op would be
    // inventing two operands the machine never reads.
    QByteArray code;
    code += oneInstruction(SCRIPT_OP_LOADK, 0, 0, 0, floatBits(2.5f));   // 0
    code += oneInstruction(SCRIPT_OP_LOADSIG, 1, 0, 0, 10);              // 1
    code += oneInstruction(SCRIPT_OP_STORESIG, 0, 1, 0, 12);             // 2
    code += oneInstruction(SCRIPT_OP_LOADST, 2, 0, 0, 0);                // 3
    code += oneInstruction(SCRIPT_OP_STOREST, 0, 2, 0, 0);               // 4
    code += oneInstruction(SCRIPT_OP_MOV, 3, 2, 0, 0);                   // 5
    // ct::-qualified: this file also sees the firmware's own protocol.h through
    // script_exec.h, which declares the same MathOp names in the global
    // namespace. Same values, two declarations — the qualification says which
    // header the test is written against, and it is the GUI's.
    code += oneInstruction(ct::MATH_OP_ADD, 4, 0, 1, 0);                 // 6  binary
    code += oneInstruction(ct::MATH_OP_ABS, 5, 0, 0, 0);                 // 7  unary
    code += oneInstruction(ct::MATH_OP_CLAMP, 6, 0, 1, 2);               // 8  ternary
    code += oneInstruction(SCRIPT_OP_JMP, 0, 0, 0, 10);                  // 9
    code += oneInstruction(SCRIPT_OP_JZ, 0, 0, 0, 11);                   // 10
    code += oneInstruction(SCRIPT_OP_JNZ, 0, 0, 0, 12);                  // 11
    code += oneInstruction(SCRIPT_OP_HALT, 0, 0, 0, 0);                  // 12
    const QByteArray image = imageAround(code, /*numState=*/1, /*entryTick=*/0);

    // Only slot 10 is named, so the listing is checked BOTH ways: a slot the
    // document can name and a slot it cannot. The number is present either way,
    // because the number is the only thing the image actually carries.
    QHash<quint16, QString> names;
    names.insert(10, QStringLiteral("Engine RPM"));

    const ScriptListing listing = disassembleScriptImage(image, names);
    CHECK(listing.valid);
    CHECK(listing.error.isEmpty());
    CHECK(listing.instructionCount == 13);
    CHECK(listing.lines.size() == 13);
    CHECK(listing.maxInstructions == int(SCRIPT_MAX_INSTRUCTIONS));
    CHECK(listing.stateCount == 1);
    CHECK(listing.codeBytes == 13 * int(SCRIPT_INSTR_SIZE));
    CHECK(listing.hasTickEntry);
    CHECK(listing.entryTick == 0);

    const char *wantMnemonic[] = { "LOADK", "LOADSIG", "STORESIG", "LOADST", "STOREST",
                                   "MOV",   "ADD",     "ABS",      "CLAMP",  "JMP",
                                   "JZ",    "JNZ",     "HALT" };
    const char *wantOperands[] = {
        "r0 = 2.5",
        "r1 = signal[10] (Engine RPM)",
        "signal[12] = r1",
        "r2 = state[0]",
        "state[0] = r2",
        "r3 = r2",
        "r4 <- (r0, r1)",
        "r5 <- (r0)",
        "r6 <- (r0, r1, r2)",
        "-> 10",
        "if r0 == 0 -> 11",
        "if r0 != 0 -> 12",
        "",
    };
    for (int i = 0; i < listing.lines.size() && i < 13; ++i) {
        const ScriptListingLine &line = listing.lines[i];
        CHECK(line.index == i);
        CHECK(line.mnemonic == QLatin1String(wantMnemonic[i]));
        CHECK(line.operands == QLatin1String(wantOperands[i]));
        // Every instruction is charged something: the interpreter's requirement
        // R1 is that no dispatch is free, HALT and JMP included, and a column of
        // zeros beside a jump would tell a reader the opposite.
        CHECK(line.cost >= 1);
        CHECK(line.isEntry == (i == 0));
    }
    // CLAMP is in the heavy set, so its charge must not be 1 — otherwise the
    // column is a constant dressed up as a measurement.
    CHECK(listing.lines[8].cost == script_op_cost(ct::MATH_OP_CLAMP));
    CHECK(listing.lines[8].cost > listing.lines[6].cost);

    // The summary measures the script against the limit it will actually hit.
    const QString summary = scriptListingSummary(listing);
    CHECK(summary.contains(QLatin1String("13 of 1024 instructions")));
    CHECK(summary.contains(QLatin1String("on_tick starts at instruction 0")));

    // An image with no on_tick verifies, loads, and never runs. The listing has
    // to say so rather than leave the reader assuming instruction 0 runs.
    const QByteArray noEntry = imageAround(code, 1, SCRIPT_NO_ENTRY);
    const ScriptListing dormant = disassembleScriptImage(noEntry);
    CHECK(dormant.valid);
    CHECK(!dormant.hasTickEntry);
    CHECK(scriptListingSummary(dormant).contains(QLatin1String("never run it")));
    for (const ScriptListingLine &line : dormant.lines) {
        CHECK(!line.isEntry);
    }
}

void testTheListingRefusesWhatTheDeviceWouldRefuse()
{
    QByteArray code;
    code += oneInstruction(SCRIPT_OP_LOADK, 0, 0, 0, floatBits(1.0f));
    code += oneInstruction(SCRIPT_OP_HALT, 0, 0, 0, 0);
    QByteArray image = imageAround(code, 0, 0);
    CHECK(disassembleScriptImage(image).valid);   // control: intact, it lists

    // One flipped byte in the code. The header still claims the old CRC, so this
    // is exactly the damage a truncated transfer or a mangled file produces —
    // and a decoder that walked it anyway would print instructions that are not
    // in the image the device holds.
    image[int(sizeof(ScriptHeader)) + 1] = char(image.at(int(sizeof(ScriptHeader)) + 1) ^ 0x5A);
    const ScriptListing bad = disassembleScriptImage(image);
    CHECK(!bad.valid);
    CHECK(bad.lines.isEmpty());
    // The verifier's own words, so the reason reads the same here as it does in
    // the Get note and the refused Send.
    CHECK(bad.error == scriptVerifyText(SCRIPT_ERR_CRC));
    CHECK(scriptListingSummary(bad).contains(bad.error));

    // No script at all is not a fault, and must not be reported as one.
    const ScriptListing none = disassembleScriptImage(QByteArray());
    CHECK(!none.valid);
    CHECK(none.error.isEmpty());
    CHECK(none.lines.isEmpty());
    CHECK(scriptListingSummary(none).contains(QLatin1String("no compiled script")));
}

void testTheRetainedImageListsAsTheScriptItWas()
{
    // The listing exists for exactly this document: read off a device, no
    // source, and the only view of the script it carries. What is asserted is
    // that it describes the SAME instructions the compiler emitted — because
    // the retained image is those bytes, and if the two listings differed, one
    // of the two paths would have altered the image.
    Configuration doc;
    QByteArray compiledImage;
    getFromDevice(&doc, &compiledImage, nullptr);
    CHECK(doc.scriptBytecode() == compiledImage);

    // Recompiled from the SAME source against the SAME configuration the Get
    // was taken from — not against this file's fixed symbol map, whose signal
    // slots are its own and would put different indices in LOADSIG/STORESIG.
    Configuration authored;
    buildConfig(authored, kEchoScript);
    ScriptSymbols syms;
    CHECK(ScriptSymbols::fromConfiguration(authored, &syms, nullptr));
    const auto compiled = ScriptCompiler::compile(QString::fromUtf8(kEchoScript), syms);
    CHECK(compiled.ok);

    const ScriptListing retained = disassembleScriptImage(doc.scriptBytecode());
    const ScriptListing fresh = disassembleScriptImage(compiled.image);
    CHECK(retained.valid);
    CHECK(retained.instructionCount == compiled.instructionCount);
    CHECK(retained.stateCount == fresh.stateCount);
    CHECK(retained.lines.size() == fresh.lines.size());
    for (int i = 0; i < retained.lines.size() && i < fresh.lines.size(); ++i) {
        CHECK(retained.lines[i].mnemonic == fresh.lines[i].mnemonic);
        CHECK(retained.lines[i].operands == fresh.lines[i].operands);
    }
    // A real script ends in HALT — the verifier insists on it — so this also
    // pins that the listing runs to the end of the code rather than stopping at
    // the first thing it did not understand.
    CHECK(!retained.lines.isEmpty());
    if (!retained.lines.isEmpty()) {
        CHECK(retained.lines.last().mnemonic == QLatin1String("HALT"));
    }
}

} // namespace

int main()
{
    testStepDrivesChannels();
    testStatePersistsAcrossTicks();
    testResetRerunsInitialisers();
    testCostIsThisTick();
    testRunawayFaultsRatherThanHangs();
    testEmptyAndUnloadedAreQuiet();

    testMapWithScriptChunks();
    testNoScriptMeansNoChunks();
    testBadScriptBlocksTheSend();
    testChunkingIsExact();

    testGetKeepsTheBytecodeAndSendsItBackByteForByte();
    testASourceSupersedesTheRetainedBytecode();
    testEveryWriterKeepsThePairInStep();
    testACorruptImageIsRefusedAtGet();
    testACorruptImageIsRefusedBeforeTheSendClearsTheDevice();

    testTheListingNamesEveryOpcode();
    testTheListingRefusesWhatTheDeviceWouldRefuse();
    testTheRetainedImageListsAsTheScriptItWas();

    if (fails == 0) {
        std::printf("test_script_sim: all checks passed\n");
        return 0;
    }
    std::printf("test_script_sim: %d check(s) failed\n", fails);
    return 1;
}
