// Communications templates — Save… / Load… in Communications Setup.
//
// A template is the file you HAND TO SOMEONE, which is what makes the two
// properties below worth pinning rather than assuming.
//
// IT MUST GIVE UP NOTHING TO A TEXT SEARCH. That is the entire reason it is not
// JSON, and it is the one claim a user is told out loud, in the box that
// appears after a save. A test that only checked "the bytes round-trip" would
// pass just as happily on a plain-text file, so several checks here go looking
// for the plaintext — channel names, the CAN ID, the JSON keys — and require it
// absent. (The container's own limit is stated honestly in comms_template.h:
// the key travels in the file, so this is opacity, not secrecy against someone
// holding the source. Opacity is the property under test.)
//
// IT MUST NEVER REWRITE A CHANNEL THE TARGET DOCUMENT ALREADY HAD. A template
// arrives naming "Coolant Temp", and so does the configuration it lands in;
// letting the incoming definition win would silently re-scale every other
// message already using that channel, with nothing on screen to say why. The
// merge renames the incoming one instead — visible, reversible, and reported.
//
// The last group drives the real dialog offscreen, because the button order and
// what Save aims at are as much of the feature as the file format is.

#include <QAbstractButton>
#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QJsonDocument>
#include <QMessageBox>
#include <QPushButton>
#include <QSet>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeWidget>

#include <algorithm>
#include <cstdio>

#include "../src/model/channel_catalog.h"
#include "../src/protocol/wire_structs.h" // MAX_MESSAGES
#include "../src/model/comms_template.h"
#include "../src/model/configuration.h"
#include "../src/model/user_paths.h"
#include "../src/ui/communications_dialog.h"

static int fails = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);              \
            ++fails;                                                                 \
        }                                                                            \
    } while (0)

// CHECK reports and carries on, which is right for independent assertions and
// wrong the moment the next line indexes what the failed one was counting: a
// first() on an empty list is a crash in the test rather than a report from it,
// and a crashed suite says nothing about the checks it never reached. REQUIRE
// is CHECK that leaves.
#define REQUIRE(cond)                                                                \
    do {                                                                             \
        if (!(cond)) {                                                               \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);              \
            ++fails;                                                                 \
            return;                                                                  \
        }                                                                            \
    } while (0)

namespace {

using namespace ct;

Channel makeChannel(const QString &name, const QString &unit, double resolution)
{
    Channel c;
    c.name = name;
    c.quantity = QStringLiteral("Temperature");
    c.unit = unit;
    c.dataType = QStringLiteral("u16");
    c.baseResolution = resolution;
    c.decimalPlaces = 1;
    c.minValue = -40;
    c.maxValue = 215;
    c.category = QStringLiteral("User Channels");
    c.userDefined = true;
    return c;
}

CommsChannelRow makeRow(const QString &channel, int startBit, int bits)
{
    CommsChannelRow r;
    r.channelName = channel;
    r.startBit = startBit;
    r.bitLength = bits;
    r.dbcFactor = 0.5;
    r.dbcOffset = -40;
    r.defaultValue = 7.5;
    return r;
}

// One receive message on 0x640 carrying two signals, and the two channels it
// decodes into. The shape most tests below start from.
void buildSource(Configuration &config)
{
    config.clear();
    config.bus[0].enabled = true;
    config.bus[0].rateKbps = 500;
    config.bus[0].dataRateKbps = 0;
    config.bus[0].termination = true;

    config.catalog().addOrUpdateUserChannel(
        makeChannel(QStringLiteral("Coolant Temp"), QStringLiteral("°C"), 0.1));
    config.catalog().addOrUpdateUserChannel(
        makeChannel(QStringLiteral("Oil Temp"), QStringLiteral("°C"), 0.1));

    CommsSection s;
    s.name = QStringLiteral("ECU Temps");
    s.device = SectionDevice::ReceiveMessage;
    s.baseAddress = 0x640;
    s.messageLengthBytes = 8;
    s.receiveTimeoutMs = 1500;
    s.rows << makeRow(QStringLiteral("Coolant Temp"), 0, 16)
           << makeRow(QStringLiteral("Oil Temp"), 16, 16);
    config.bus[0].sections.append(s);
}

QString templatePath(const QTemporaryDir &dir, const char *name)
{
    return dir.filePath(QLatin1String(name) + QStringLiteral(".ct3t"));
}

QByteArray fileBytes(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

// Save `config`'s CAN 1 sections and read them straight back. Every merge test
// below starts from this rather than passing the struct along, because a field
// that does not survive serialisation is exactly the bug worth catching and a
// test that skips the file would never see it.
bool roundTrip(const Configuration &config, const QString &path, CommsTemplate *out)
{
    if (!writeCommsTemplate(path,
                            buildCommsTemplate(config.catalog(), config.bus[0],
                                               config.bus[0].sections, QStringLiteral("T")),
                            nullptr))
        return false;
    return readCommsTemplate(path, out, nullptr);
}

// ---------------------------------------------------------------- the file

void testRoundTripPreservesEveryField(const QTemporaryDir &dir)
{
    Configuration config;
    buildSource(config);
    const CommsSection original = config.bus[0].sections.first();

    const CommsTemplate saved = buildCommsTemplate(
        config.catalog(), config.bus[0], config.bus[0].sections, QStringLiteral("ECU Temps"));
    const QString path = templatePath(dir, "roundtrip");
    QString error;
    REQUIRE(writeCommsTemplate(path, saved, &error));
    CHECK(error.isEmpty());

    CommsTemplate loaded;
    REQUIRE(readCommsTemplate(path, &loaded, &error));
    REQUIRE(loaded.sections.size() == 1);

    // Compared through toJson() rather than field by field, deliberately: a
    // field added to CommsSection later is one this test starts covering for
    // free, where a hand-written list of members is one somebody has to
    // remember to extend and nobody will.
    CHECK(loaded.sections.first().toJson() == original.toJson());

    // The channel definitions travel too, or the rows above decode into nothing
    // on arrival.
    CHECK(loaded.channels.size() == 2);
    CHECK(loaded.configSchema == configSchemaVersion());

    // The bus the messages came off, so a 500k device does not arrive on a 1M
    // bus with nothing said about it.
    CHECK(loaded.hasBusSettings);
    CHECK(loaded.rateKbps == 500);
    CHECK(loaded.termination);
}

void testTheFileIsOpaque(const QTemporaryDir &dir)
{
    Configuration config;
    buildSource(config);
    const QString path = templatePath(dir, "opaque");
    CommsTemplate ignored;
    REQUIRE(roundTrip(config, path, &ignored));

    const QByteArray bytes = fileBytes(path);
    REQUIRE(!bytes.isEmpty());

    // Nothing a person could recognise: the names, the message, the JSON keys
    // the body is built from, and the CAN ID.
    //
    // Every needle is at least eight bytes long, on purpose. The file is mostly
    // CSPRNG noise, so a THREE-byte needle like "640" turns up by chance about
    // once in eight thousand runs — a test that fails at that rate is one people
    // learn to re-run rather than read. The CAN ID is therefore looked for in
    // the shape the body actually spells it, key and all.
    const char *mustBeAbsent[] = {"Coolant Temp",           "Oil Temp",    "ECU Temps",
                                  "\"baseAddress\":\"640\"", "baseAddress", "startBit",
                                  "fileType",               "CANTriple",   "dbcFactor"};
    for (const char *needle : mustBeAbsent)
        if (bytes.contains(needle)) {
            std::printf("FAIL %s:%d  the template leaks \"%s\"\n", __FILE__, __LINE__, needle);
            ++fails;
        }

    // And it is not text at all: a body that happened to parse as JSON would
    // mean the container had been bypassed entirely.
    CHECK(!QJsonDocument::fromJson(bytes).isObject());

    // Two saves of the same template share no bytes, because the file key and
    // the noise carrier are drawn fresh each time. Without this, someone holding
    // two templates could diff them and learn where they differ, which is a
    // shape even when the content is unreadable.
    const QString second = templatePath(dir, "opaque2");
    REQUIRE(roundTrip(config, second, &ignored));
    CHECK(fileBytes(second) != bytes);
}

void testATemplateIsNotAConfiguration(const QTemporaryDir &dir)
{
    Configuration config;
    buildSource(config);
    const QString path = templatePath(dir, "notaconfig");
    CommsTemplate tmpl;
    REQUIRE(roundTrip(config, path, &tmpl));

    // THE failure this guards against: without the marker check, a template fed
    // to File > Open decrypts perfectly, parses as a configuration with every
    // key missing, and REPLACES the open document with an empty one. A wrong
    // file must produce a sentence.
    Configuration target;
    buildSource(target);
    QString error;
    CHECK(!target.loadFromFile(path, &error));
    CHECK(error.contains(QStringLiteral("template")));
    CHECK(target.bus[0].sections.size() == 1); // the open document is untouched

    // And the other direction: a secure configuration offered to Load….
    const QString secure = dir.filePath(QStringLiteral("real.ct3s"));
    SecureSaveOptions options;
    REQUIRE(config.saveSecureToFile(secure, options, &error));
    CHECK(!readCommsTemplate(secure, &tmpl, &error));
    CHECK(error.contains(QStringLiteral(".ct3s")));

    // A file that is not a container at all — the body's marker in plain JSON,
    // which is precisely what someone would produce if they thought the format
    // was text.
    const QString plain = dir.filePath(QStringLiteral("plain.ct3t"));
    QFile f(plain);
    REQUIRE(f.open(QIODevice::WriteOnly));
    f.write("{\"fileType\":\"CANTripleCommsTemplate\",\"formatVersion\":1}");
    f.close();
    CHECK(!readCommsTemplate(plain, &tmpl, &error));

    // A truncated one. The payload's tag is what catches this, and it must be
    // caught whole rather than half-parsed into a partial message list.
    QByteArray bytes = fileBytes(path);
    bytes.truncate(bytes.size() / 2);
    const QString torn = dir.filePath(QStringLiteral("torn.ct3t"));
    QFile t(torn);
    REQUIRE(t.open(QIODevice::WriteOnly));
    t.write(bytes);
    t.close();
    CHECK(!readCommsTemplate(torn, &tmpl, &error));
}

// --------------------------------------------------------------- the merge

void testChannelsAreCreatedInAnEmptyDocument(const QTemporaryDir &dir)
{
    Configuration source;
    buildSource(source);
    CommsTemplate tmpl;
    REQUIRE(roundTrip(source, templatePath(dir, "create"), &tmpl));

    Configuration target;
    target.clear();
    CommsTemplateMerge merge;
    REQUIRE(mergeCommsTemplate(target.catalog(), 0, tmpl, &merge, nullptr));
    CHECK(merge.channelsCreated == 2);
    CHECK(merge.channelsReused == 0);
    CHECK(merge.notes.isEmpty());
    CHECK(merge.sections.size() == 1);

    // The channels exist WITH their definitions — a name alone would decode the
    // message to the wrong numbers rather than to none, which is worse.
    const Channel coolant = target.catalog().findByName(QStringLiteral("Coolant Temp"));
    REQUIRE(coolant.isValid());
    CHECK(coolant.baseResolution == 0.1);
    CHECK(coolant.unit == QStringLiteral("°C"));
    CHECK(coolant.decimalPlaces == 1);
}

void testAnIdenticalChannelIsReusedNotDuplicated(const QTemporaryDir &dir)
{
    Configuration source;
    buildSource(source);
    CommsTemplate tmpl;
    REQUIRE(roundTrip(source, templatePath(dir, "reuse"), &tmpl));

    // The target already has both channels and means exactly the same thing by
    // them, so loading must not litter the catalogue with copies.
    Configuration target;
    buildSource(target);
    CommsTemplateMerge merge;
    REQUIRE(mergeCommsTemplate(target.catalog(), 0, tmpl, &merge, nullptr));
    CHECK(merge.channelsCreated == 0);
    CHECK(merge.channelsReused == 2);
    CHECK(target.catalog().userChannels().size() == 2);
    REQUIRE(!merge.sections.isEmpty() && !merge.sections.first().rows.isEmpty());
    CHECK(merge.sections.first().rows.first().channelName == QStringLiteral("Coolant Temp"));
}

void testADisagreeingChannelIsRenamedAndTheDocumentsIsLeftAlone(const QTemporaryDir &dir)
{
    Configuration source;
    buildSource(source);
    CommsTemplate tmpl;
    REQUIRE(roundTrip(source, templatePath(dir, "rename"), &tmpl));

    // Same name, DIFFERENT meaning: this document's Coolant Temp counts whole
    // degrees Fahrenheit. Overwriting it would silently change what every other
    // message already using it decodes to.
    Configuration target;
    target.clear();
    target.catalog().addOrUpdateUserChannel(
        makeChannel(QStringLiteral("Coolant Temp"), QStringLiteral("°F"), 1.0));

    CommsTemplateMerge merge;
    REQUIRE(mergeCommsTemplate(target.catalog(), 0, tmpl, &merge, nullptr));

    const Channel kept = target.catalog().findByName(QStringLiteral("Coolant Temp"));
    CHECK(kept.baseResolution == 1.0);        // untouched
    CHECK(kept.unit == QStringLiteral("°F")); // untouched
    const Channel added = target.catalog().findByName(QStringLiteral("Coolant Temp 2"));
    REQUIRE(added.isValid());
    CHECK(added.baseResolution == 0.1);

    // And the row follows the channel it actually got.
    REQUIRE(!merge.sections.isEmpty() && !merge.sections.first().rows.isEmpty());
    CHECK(merge.sections.first().rows.first().channelName == QStringLiteral("Coolant Temp 2"));
    CHECK(merge.notes.size() == 1);
}

void testADeviceChannelNameCannotBeTaken(const QTemporaryDir &dir)
{
    // A user channel SHADOWING a device channel. The catalogue permits this on
    // purpose — such a channel predates the device one, and re-typing it out
    // from under the configuration would be worse than the shadowing — so a
    // real document can hold one and a template has to cope.
    //
    // Both halves matter here. The export must carry the shadowing channel's
    // definition, because a template that skipped it as "that's the device's
    // name" would land the row on the value the FIRMWARE publishes, and the
    // message would decode into a channel the device rewrites every tick. The
    // merge must then NOT recreate the shadow in a document that does not have
    // one: creating a fresh clash is a different thing from preserving an old.
    const QString deviceName = ChannelCatalog::deviceOnTimeName();
    REQUIRE(ChannelCatalog::isDeviceChannel(deviceName));

    Configuration source;
    source.clear();
    source.catalog().addOrUpdateUserChannel(makeChannel(deviceName, QStringLiteral("s"), 0.25));
    CommsSection s;
    s.name = QStringLiteral("Uptime Rx");
    s.device = SectionDevice::ReceiveMessage;
    s.baseAddress = 0x100;
    s.rows << makeRow(deviceName, 0, 16);
    source.bus[0].sections.append(s);

    CommsTemplate tmpl;
    REQUIRE(roundTrip(source, templatePath(dir, "device"), &tmpl));
    // The definition travelled — this is the export half.
    REQUIRE(tmpl.channels.size() == 1);
    CHECK(tmpl.channels.first().name == deviceName);
    CHECK(tmpl.channels.first().baseResolution == 0.25);

    Configuration target;
    target.clear();
    CommsTemplateMerge merge;
    REQUIRE(mergeCommsTemplate(target.catalog(), 0, tmpl, &merge, nullptr));
    REQUIRE(!merge.sections.isEmpty() && !merge.sections.first().rows.isEmpty());
    // And the merge half: the row must NOT be left pointing at the device
    // channel, and the channel it does point at must carry the resolution the
    // template saved rather than whatever the firmware publishes.
    const QString landed = merge.sections.first().rows.first().channelName;
    CHECK(landed != deviceName);
    CHECK(target.catalog().findByName(landed).baseResolution == 0.25);
    REQUIRE(merge.notes.size() == 1);
    CHECK(merge.notes.first().contains(QStringLiteral("device channel")));
}

void testTwoOverlongNamesDoNotClipOntoEachOther()
{
    // A name longer than the device's label budget is shortened, and the
    // shortened form is what can collide. Two names that differ only past the
    // cut clip to the SAME thing — so the disambiguator has to be applied after
    // the clip, not instead of it. Test the original name against what is taken
    // and clip afterwards, and this case slips through with no disambiguator at
    // all: two rows, one channel, and the second signal silently overwriting the
    // first every time the message arrives.
    //
    // Hand-built, like the device-channel case, and for the same reason: the
    // channel editor will not let you type a name this long, so a template
    // carrying one came from somewhere that could. Coping with files it did not
    // write is the whole job of a reader.
    const QString stem = QString(MAX_CHANNEL_NAME_BYTES, QLatin1Char('x'));
    const QString first = stem + QStringLiteral("AAA");
    const QString second = stem + QStringLiteral("BBB");
    REQUIRE(first.toUtf8().size() > MAX_CHANNEL_NAME_BYTES);
    REQUIRE(first != second);

    CommsTemplate tmpl;
    tmpl.configSchema = configSchemaVersion();
    tmpl.channels << makeChannel(first, QStringLiteral("°C"), 0.1)
                  << makeChannel(second, QStringLiteral("°C"), 0.5);
    CommsSection s;
    s.name = QStringLiteral("Long Names");
    s.device = SectionDevice::ReceiveMessage;
    s.baseAddress = 0x500;
    s.rows << makeRow(first, 0, 16) << makeRow(second, 16, 16);
    tmpl.sections << s;

    Configuration target;
    target.clear();
    CommsTemplateMerge merge;
    REQUIRE(mergeCommsTemplate(target.catalog(), 0, tmpl, &merge, nullptr));
    REQUIRE(merge.sections.size() == 1);
    const QList<CommsChannelRow> rows = merge.sections.first().rows;
    REQUIRE(rows.size() == 2);

    CHECK(rows.at(0).channelName != rows.at(1).channelName);
    CHECK(rows.at(0).channelName.toUtf8().size() <= MAX_CHANNEL_NAME_BYTES);
    CHECK(rows.at(1).channelName.toUtf8().size() <= MAX_CHANNEL_NAME_BYTES);
    CHECK(merge.channelsCreated == 2);
    CHECK(target.catalog().userChannels().size() == 2);
    // Still the right two, so they did not merely get distinct names.
    CHECK(target.catalog().findByName(rows.at(0).channelName).baseResolution == 0.1);
    CHECK(target.catalog().findByName(rows.at(1).channelName).baseResolution == 0.5);
}

void testARenameDoesNotCollideWithAnotherTemplateChannel(const QTemporaryDir &dir)
{
    // THE aliasing bug the two-pass mapping exists to prevent. The template
    // carries "Temp" and "Temp 2"; the target already has a DIFFERENT "Temp", so
    // the incoming one must move — and the obvious name to move it to is the one
    // the template's OTHER channel already holds. Renaming references one at a
    // time would then land both rows on the same channel, and the message would
    // decode two signals into one place.
    Configuration source;
    source.clear();
    source.catalog().addOrUpdateUserChannel(
        makeChannel(QStringLiteral("Temp"), QStringLiteral("°C"), 0.1));
    source.catalog().addOrUpdateUserChannel(
        makeChannel(QStringLiteral("Temp 2"), QStringLiteral("°C"), 0.5));
    CommsSection s;
    s.name = QStringLiteral("Two Temps");
    s.device = SectionDevice::ReceiveMessage;
    s.baseAddress = 0x200;
    s.rows << makeRow(QStringLiteral("Temp"), 0, 16)
           << makeRow(QStringLiteral("Temp 2"), 16, 16);
    source.bus[0].sections.append(s);

    CommsTemplate tmpl;
    REQUIRE(roundTrip(source, templatePath(dir, "alias"), &tmpl));

    Configuration target;
    target.clear();
    target.catalog().addOrUpdateUserChannel(
        makeChannel(QStringLiteral("Temp"), QStringLiteral("kPa"), 1.0));

    CommsTemplateMerge merge;
    REQUIRE(mergeCommsTemplate(target.catalog(), 0, tmpl, &merge, nullptr));
    REQUIRE(merge.sections.size() == 1);
    const QList<CommsChannelRow> rows = merge.sections.first().rows;
    REQUIRE(rows.size() == 2);

    // Whatever names they ended up with, the two rows must still be two
    // DIFFERENT channels, and neither may be the target's own "Temp".
    CHECK(rows.at(0).channelName != rows.at(1).channelName);
    CHECK(rows.at(0).channelName.compare(QStringLiteral("Temp"), Qt::CaseInsensitive) != 0);
    CHECK(rows.at(1).channelName.compare(QStringLiteral("Temp"), Qt::CaseInsensitive) != 0);
    // Each row's channel exists carrying the resolution it was saved with — so
    // the two did not merely get distinct names, they got the RIGHT ones.
    CHECK(target.catalog().findByName(rows.at(0).channelName).baseResolution == 0.1);
    CHECK(target.catalog().findByName(rows.at(1).channelName).baseResolution == 0.5);
    CHECK(target.catalog().findByName(QStringLiteral("Temp")).baseResolution == 1.0);
}

void testACompoundSectionKeepsItsIdentifiers(const QTemporaryDir &dir)
{
    Configuration source;
    source.clear();
    source.catalog().addOrUpdateUserChannel(
        makeChannel(QStringLiteral("Mux A"), QStringLiteral("°C"), 0.1));
    source.catalog().addOrUpdateUserChannel(
        makeChannel(QStringLiteral("Mux B"), QStringLiteral("°C"), 0.1));

    CommsSection s;
    s.name = QStringLiteral("Multiplexed");
    s.device = SectionDevice::ReceiveMessage;
    s.baseAddress = 0x7E8;
    s.compound = true;
    CompoundIdentifier one;
    one.id = 1;
    one.configured = true;
    one.rows << makeRow(QStringLiteral("Mux A"), 8, 16);
    CompoundIdentifier two;
    two.id = 2;
    two.configured = true;
    two.rows << makeRow(QStringLiteral("Mux B"), 8, 16);
    s.identifiers << one << two;
    source.bus[0].sections.append(s);

    CommsTemplate tmpl;
    REQUIRE(roundTrip(source, templatePath(dir, "compound"), &tmpl));

    Configuration target;
    target.clear();
    CommsTemplateMerge merge;
    REQUIRE(mergeCommsTemplate(target.catalog(), 0, tmpl, &merge, nullptr));
    // Channels inside identifiers are the ones a rows-only walk misses — a
    // compound section carries no always-present rows at all.
    CHECK(merge.channelsCreated == 2);
    REQUIRE(!merge.sections.isEmpty());
    REQUIRE(merge.sections.first().identifiers.size() == 2);
    REQUIRE(!merge.sections.first().identifiers.at(1).rows.isEmpty());
    CHECK(merge.sections.first().identifiers.at(1).rows.first().channelName
          == QStringLiteral("Mux B"));
}

void testARelayLoadedOntoItsOwnTargetBusIsCorrected(const QTemporaryDir &dir)
{
    Configuration source;
    source.clear();
    CommsSection s;
    s.name = QStringLiteral("Gateway");
    s.device = SectionDevice::MessageRelay;
    s.baseAddress = 0x300;
    s.routeBusMask = 0b110; // CAN 2 and CAN 3, saved from CAN 1
    source.bus[0].sections.append(s);

    CommsTemplate tmpl;
    REQUIRE(roundTrip(source, templatePath(dir, "relay"), &tmpl));

    Configuration target;
    target.clear();

    // Loaded onto CAN 2, which the rule forwards to. A bus cannot relay to
    // itself, and the mask is meaningful only relative to the bus it sits on.
    CommsTemplateMerge merge;
    REQUIRE(mergeCommsTemplate(target.catalog(), 1, tmpl, &merge, nullptr));
    REQUIRE(!merge.sections.isEmpty());
    CHECK(merge.sections.first().routeBusMask == 0b100);
    CHECK(merge.notes.size() == 1);

    // Onto CAN 1, where it came from, nothing changes and nothing is said.
    CommsTemplateMerge same;
    REQUIRE(mergeCommsTemplate(target.catalog(), 0, tmpl, &same, nullptr));
    REQUIRE(!same.sections.isEmpty());
    CHECK(same.sections.first().routeBusMask == 0b110);
    CHECK(same.notes.isEmpty());
}

void testAMarkedMessageStaysMarked(const QTemporaryDir &dir)
{
    Configuration source;
    buildSource(source);
    source.bus[0].sections[0].protection = CommsProtection::Hidden;
    source.bus[0].sections[0].messageKey = deriveAccessKey(QStringLiteral("supplier"));

    CommsTemplate tmpl;
    REQUIRE(roundTrip(source, templatePath(dir, "marked"), &tmpl));
    REQUIRE(!tmpl.sections.isEmpty());

    // The tier AND the password survive, so a template dropped into a fresh
    // document produces a padlocked row rather than an open one. That is the
    // point of templates for a supplier: hand over the message, not its
    // protocol.
    CHECK(tmpl.sections.first().protection == CommsProtection::Hidden);
    CHECK(tmpl.sections.first().messageKey == deriveAccessKey(QStringLiteral("supplier")));

    Configuration target;
    target.clear();
    CommsTemplateMerge merge;
    REQUIRE(mergeCommsTemplate(target.catalog(), 0, tmpl, &merge, nullptr));
    REQUIRE(!merge.sections.isEmpty());
    CHECK(merge.sections.first().isConcealed(/*revealed=*/false));
}

void testATriggerConditionIsReportedRatherThanInvented(const QTemporaryDir &dir)
{
    Configuration source;
    source.clear();
    source.catalog().addOrUpdateUserChannel(
        makeChannel(QStringLiteral("Payload"), QStringLiteral("kPa"), 0.1));
    source.catalog().addOrUpdateUserChannel(
        makeChannel(QStringLiteral("Fan Request"), QString(), 1.0));

    CommsSection s;
    s.name = QStringLiteral("Fan Command");
    s.device = SectionDevice::TransmitMessage;
    s.baseAddress = 0x400;
    s.cyclic = false;
    s.transmitCondition = QStringLiteral("Fan Request");
    s.rows << makeRow(QStringLiteral("Payload"), 0, 16);
    source.bus[0].sections.append(s);

    const CommsTemplate built = buildCommsTemplate(source.catalog(), source.bus[0],
                                                   source.bus[0].sections, QStringLiteral("T"));
    // The condition's OUTPUT channel is deliberately not exported. A template
    // carries no User Conditions, so creating the channel would make the
    // condition look present while nothing drove it, and the message would sit
    // there never transmitting with no clue on screen.
    REQUIRE(built.channels.size() == 1);
    CHECK(built.channels.first().name == QStringLiteral("Payload"));

    const QString path = templatePath(dir, "trigger");
    REQUIRE(writeCommsTemplate(path, built, nullptr));
    CommsTemplate tmpl;
    REQUIRE(readCommsTemplate(path, &tmpl, nullptr));

    Configuration target;
    target.clear();
    CommsTemplateMerge merge;
    REQUIRE(mergeCommsTemplate(target.catalog(), 0, tmpl, &merge, nullptr));
    CHECK(!target.catalog().findByName(QStringLiteral("Fan Request")).isValid());
    REQUIRE(merge.notes.size() == 1);
    CHECK(merge.notes.first().contains(QStringLiteral("User Condition")));
    // The binding is KEPT, not cleared: the user's repair is to build the
    // condition, and a message that silently became Cyclic instead would start
    // transmitting continuously onto a live bus rather than waiting to be fixed.
    REQUIRE(!merge.sections.isEmpty());
    CHECK(merge.sections.first().transmitCondition == QStringLiteral("Fan Request"));
    CHECK(!merge.sections.first().cyclic);
}

void testTheFolderLayout()
{
    // TWO ROOTS, and which side of the line each folder falls on is the part
    // worth pinning — it is what a user sees in Explorer, and five features now
    // depend on user_paths.h agreeing with itself where three separate files
    // each used to spell a folder out for themselves.
    //
    // The product's own libraries sit beside the executable; the two folders
    // that shipped in an earlier release stay under Documents, because moving
    // that root would not move the files already in it, it would abandon them.
    struct Leaf
    {
        QString path;
        QString root;
        const char *name;
    };
    const Leaf leaves[] = {
        {configurationsDirectory(), programRoot(), "Configurations"},
        {commsTemplatesDirectory(), programRoot(), "Communications Templates"},
        {firmwareImagesDirectory(), programRoot(), "Firmware"},
        {firmwareBackupsDirectory(), userFilesRoot(), "Firmware Update Backups"},
        {deviceScriptsDirectory(), userFilesRoot(), "Scripts"},
    };
    QSet<QString> seen;
    for (const Leaf &leaf : leaves) {
        CHECK(leaf.path == leaf.root + QLatin1Char('/') + QLatin1String(leaf.name));
        // Distinct, which is the failure a shared root invites: two features
        // handed one folder would interleave their files with nothing to say so.
        CHECK(!seen.contains(leaf.path));
        seen.insert(leaf.path);
    }

    CHECK(programRoot() == QCoreApplication::applicationDirPath());
    CHECK(userFilesRoot().endsWith(QStringLiteral("/CAN Triple Device Manager")));
    // The two that stayed must NOT have followed the program: a user's existing
    // firmware backups and scripts are already sitting under Documents.
    CHECK(!firmwareBackupsDirectory().startsWith(programRoot()));
    CHECK(!deviceScriptsDirectory().startsWith(programRoot()));

    // And the write must be PROVED, not assumed. mkpath answers true for a
    // folder that merely exists, which is exactly the state an installed copy
    // is in when Setup made the folder and no permission was granted on it —
    // the probe inside ensureWritableDirectory is what separates the two.
    QString error;
    CHECK(ensureWritableDirectory(commsTemplatesDirectory(), &error));
    CHECK(error.isEmpty());
    CHECK(QDir(commsTemplatesDirectory()).exists());

    // A folder nothing may write to has to come back false WITH a sentence,
    // rather than true because the directory happened to exist. Windows refuses
    // a file under a path whose parent is a FILE, which is a refusal available
    // to a test without needing a privileged directory to aim at.
    QTemporaryDir sandbox;
    if (sandbox.isValid()) {
        const QString blocker = sandbox.filePath(QStringLiteral("not-a-directory"));
        QFile f(blocker);
        if (f.open(QIODevice::WriteOnly)) {
            f.write("x");
            f.close();
            QString why;
            CHECK(!ensureWritableDirectory(blocker + QStringLiteral("/child"), &why));
            CHECK(!why.isEmpty());
        }
    }
}

// --------------------------------------------------------------- the dialog

QWidget *busPage(CommunicationsDialog &d, int bus)
{
    auto *tabs = d.findChild<QTabWidget *>();
    return tabs ? tabs->widget(bus) : nullptr;
}

QPushButton *busButton(CommunicationsDialog &d, const QString &objectName, int bus = 0)
{
    QWidget *page = busPage(d, bus);
    return page ? page->findChild<QPushButton *>(objectName) : nullptr;
}

QTreeWidget *busTree(CommunicationsDialog &d, int bus = 0)
{
    QWidget *page = busPage(d, bus);
    return page ? page->findChild<QTreeWidget *>() : nullptr;
}

void testTheButtonsAreInTheAskedForOrder()
{
    Configuration config;
    buildSource(config);
    CommunicationsDialog d(&config);
    // Shown so the layout really runs: the order under test is a POSITION in a
    // column, and reading it out of findChildren() would be reading creation
    // order and calling it layout.
    d.show();
    QApplication::processEvents();

    QWidget *page = busPage(d, 0);
    REQUIRE(page != nullptr);
    QList<QPushButton *> buttons = page->findChildren<QPushButton *>();
    const auto topOf = [page](QPushButton *b) { return b->mapTo(page, QPoint(0, 0)).y(); };
    std::sort(buttons.begin(), buttons.end(),
              [&topOf](QPushButton *a, QPushButton *b) { return topOf(a) < topOf(b); });

    QStringList labels;
    int previous = -1;
    bool increasing = true;
    for (QPushButton *b : buttons) {
        // Strictly increasing, which is what makes the comparison below mean
        // anything: if the layout never ran, every button sits at y = 0, the
        // sort is a no-op, and a list that happened to match would pass without
        // having tested the column at all.
        const int y = topOf(b);
        if (y <= previous)
            increasing = false;
        previous = y;
        labels << b->text();
    }
    CHECK(increasing);

    const QStringList expected{QStringLiteral("Select…"),   QStringLiteral("Import DBC…"),
                               QStringLiteral("New…"),      QStringLiteral("Edit…"),
                               QStringLiteral("Save…"),     QStringLiteral("Load…"),
                               QStringLiteral("↑ Move Up"), QStringLiteral("↓ Move Down"),
                               QStringLiteral("Remove"),    QStringLiteral("Remove All")};
    CHECK(labels == expected);
    if (labels != expected)
        std::printf("       got: %s\n", qPrintable(labels.join(QStringLiteral(" | "))));
    d.hide();
}

void testSaveNeedsASelectionAndLoadNeverDoes()
{
    Configuration empty;
    empty.clear();
    CommunicationsDialog d(&empty);
    QPushButton *save = busButton(d, QStringLiteral("saveTemplateButton"));
    REQUIRE(save != nullptr);
    CHECK(!save->isEnabled()); // nothing in the list, so nothing to save

    // Load never needs one: it is how an empty bus stops being empty, and a
    // greyed-out Load on a fresh document would hide the feature at exactly the
    // moment it is most useful.
    QPushButton *load = busButton(d, QStringLiteral("loadTemplateButton"));
    REQUIRE(load != nullptr);
    CHECK(load->isEnabled());
}

void testSaveFollowsTheSelection()
{
    Configuration config;
    buildSource(config);
    // A second message, so "the selection" and "the bus" are different answers.
    CommsSection other;
    other.name = QStringLiteral("Other");
    other.device = SectionDevice::ReceiveMessage;
    other.baseAddress = 0x641;
    config.bus[0].sections.append(other);

    CommunicationsDialog d(&config);
    QTreeWidget *tree = busTree(d, 0);
    QPushButton *save = busButton(d, QStringLiteral("saveTemplateButton"));
    REQUIRE(tree != nullptr);
    REQUIRE(save != nullptr);

    // Save takes the SELECTION rather than the bus, which is a real difference
    // the moment there is more than one message — and the tooltip is where the
    // user is told which. Driven through the tree, so what is checked is the
    // dialog's own answer rather than a re-implementation of it.
    tree->clearSelection();
    tree->setCurrentItem(tree->topLevelItem(1));
    tree->topLevelItem(1)->setSelected(true);
    CHECK(save->isEnabled());
    CHECK(save->toolTip().contains(QStringLiteral("Other")));
    CHECK(!save->toolTip().contains(QStringLiteral("ECU Temps")));

    tree->topLevelItem(0)->setSelected(true); // both rows now
    CHECK(save->toolTip().contains(QStringLiteral("2 selected messages")));
}

// DRIVING THE TWO BUTTONS FOR REAL.
//
// Everything above tests the model behind Save… and Load…. The handlers
// themselves — what they collect, whether the extension is forced on, whether
// the capacity check fires, what the summary says — are only reachable by
// pressing the buttons, and pressing them opens modal dialogs that would
// otherwise stop the test dead.
//
// So this drives them. Under the offscreen platform Qt has no native file
// dialog and falls back to its own widget one, which is an ordinary QDialog and
// can be filled in and accepted from a timer running inside the modal loop. The
// message boxes go the same way. What is exercised is therefore the real click
// path, not a re-implementation of it.
class DialogPilot : public QObject
{
public:
    QString filePath;                                // handed to the file dialog
    QMessageBox::StandardButton answer = QMessageBox::Yes;
    int fileDialogs = 0;
    QStringList boxes;                               // every message box's text
    QString lastDetails;                             // and the last one's details
    bool wedged = false;

    explicit DialogPilot(QObject *parent = nullptr) : QObject(parent)
    {
        connect(&m_timer, &QTimer::timeout, this, &DialogPilot::tick);
        m_timer.start(5);
    }

    QString allText() const { return boxes.join(QLatin1Char('\n')); }

private:
    void tick()
    {
        QWidget *w = QApplication::activeModalWidget();
        if (!w) {
            m_stuck = 0;
            return;
        }
        // A dialog this pilot does not recognise, or one that will not close,
        // would otherwise hang the suite forever. Give it a second, then shut
        // it and record that something went wrong — a test that hangs tells you
        // nothing, and CI has no one to press Cancel.
        if (++m_stuck > 200) {
            wedged = true;
            w->close(); // QDialog::close() rejects, whatever kind it is
            m_stuck = 0;
            return;
        }
        if (auto *fd = qobject_cast<QFileDialog *>(w)) {
            ++fileDialogs;
            fd->selectFile(filePath);
            // Through the meta-object: QFileDialog re-declares accept() as a
            // protected override, so the public QDialog slot is not reachable
            // by name from out here even though it is the same slot.
            QMetaObject::invokeMethod(fd, "accept", Qt::DirectConnection);
            m_stuck = 0;
        } else if (auto *mb = qobject_cast<QMessageBox *>(w)) {
            boxes << mb->text();
            lastDetails = mb->detailedText();
            QAbstractButton *b = mb->button(answer);
            if (!b)
                b = mb->button(QMessageBox::Ok);
            if (!b && !mb->buttons().isEmpty())
                b = mb->buttons().first();
            if (b)
                b->click();
            m_stuck = 0;
        }
    }

    QTimer m_timer;
    int m_stuck = 0;
};

void testTheSaveButtonWritesAFile(const QTemporaryDir &dir)
{
    Configuration config;
    buildSource(config);
    CommunicationsDialog d(&config);
    QPushButton *save = busButton(d, QStringLiteral("saveTemplateButton"));
    REQUIRE(save != nullptr);

    DialogPilot pilot;
    // Deliberately WITHOUT the extension, to pin the handler putting it back on:
    // Qt only appends the filter's suffix while that filter is selected, and a
    // template saved as "ECU Temps" with no suffix is one Load… will not list.
    pilot.filePath = dir.filePath(QStringLiteral("From The Button"));
    save->click();

    CHECK(!pilot.wedged);
    CHECK(pilot.fileDialogs == 1);
    const QString written = dir.filePath(QStringLiteral("From The Button.ct3t"));
    REQUIRE(QFile::exists(written));

    // And it really is a template, with the message that was selected in it.
    CommsTemplate back;
    REQUIRE(readCommsTemplate(written, &back, nullptr));
    REQUIRE(back.sections.size() == 1);
    CHECK(back.sections.first().name == QStringLiteral("ECU Temps"));
    CHECK(back.channels.size() == 2);
    // The box tells the user what was written and where.
    CHECK(pilot.allText().contains(QStringLiteral("From The Button.ct3t")));
}

void testTheSaveButtonRefusesAConcealedMessage(const QTemporaryDir &dir)
{
    Configuration config;
    buildSource(config);
    config.bus[0].sections[0].protection = CommsProtection::Hidden;
    config.bus[0].sections[0].messageKey = deriveAccessKey(QStringLiteral("supplier"));

    CommunicationsDialog d(&config);
    QPushButton *save = busButton(d, QStringLiteral("saveTemplateButton"));
    REQUIRE(save != nullptr);

    DialogPilot pilot;
    const QString path = dir.filePath(QStringLiteral("never.ct3t"));
    pilot.filePath = path;
    save->click();

    CHECK(!pilot.wedged);
    // Refused BEFORE the file dialog: the user is not asked where to put a file
    // that is not going to be written.
    CHECK(pilot.fileDialogs == 0);
    CHECK(!QFile::exists(path));
    CHECK(pilot.allText().contains(QStringLiteral("concealed")));
    CHECK(pilot.allText().contains(QStringLiteral("ECU Temps")));
}

void testTheLoadButtonAddsTheMessages(const QTemporaryDir &dir)
{
    Configuration source;
    buildSource(source);
    const QString path = templatePath(dir, "button-load");
    CommsTemplate ignored;
    REQUIRE(roundTrip(source, path, &ignored));

    // An empty document on a bus running at a different rate, so the bus
    // question fires as well as the load itself.
    Configuration target;
    target.clear();
    target.bus[1].rateKbps = 1000;
    CommunicationsDialog d(&target);
    auto *tabs = d.findChild<QTabWidget *>();
    REQUIRE(tabs != nullptr);
    tabs->setCurrentIndex(1); // CAN 2
    QPushButton *load = busButton(d, QStringLiteral("loadTemplateButton"), 1);
    REQUIRE(load != nullptr);

    DialogPilot pilot;
    pilot.filePath = path;
    pilot.answer = QMessageBox::Yes; // yes, apply the template's bus settings
    load->click();

    CHECK(!pilot.wedged);
    CHECK(pilot.fileDialogs == 1);

    // The messages are in the list…
    QTreeWidget *tree = busTree(d, 1);
    REQUIRE(tree != nullptr);
    REQUIRE(tree->topLevelItemCount() == 1);
    CHECK(tree->topLevelItem(0)->text(1).contains(QStringLiteral("ECU Temps")));
    // …selected, so Remove and the Move buttons aim at what just arrived…
    CHECK(tree->topLevelItem(0)->isSelected());
    // …their channels are in the document…
    CHECK(target.catalog().findByName(QStringLiteral("Coolant Temp")).isValid());
    CHECK(target.catalog().findByName(QStringLiteral("Oil Temp")).isValid());
    CHECK(target.isDirty());
    // …the bus question was asked and answered…
    CHECK(pilot.allText().contains(QStringLiteral("bus settings")));
    // …and the summary reported the work.
    CHECK(pilot.allText().contains(QStringLiteral("CAN 2")));

    // OK writes the working copies through, rate included. Invoked through the
    // meta-object for the same reason the file dialog is: the override is
    // private, and reaching around it with a cast would be reaching around the
    // access control rather than through the slot the button uses.
    QMetaObject::invokeMethod(&d, "accept", Qt::DirectConnection);
    REQUIRE(target.bus[1].sections.size() == 1);
    CHECK(target.bus[1].rateKbps == 500);
    CHECK(target.bus[0].sections.isEmpty()); // it went to the tab that was open
}

void testTheLoadButtonRefusesTheWrongKindOfFile(const QTemporaryDir &dir)
{
    Configuration config;
    buildSource(config);
    const QString secure = dir.filePath(QStringLiteral("button.ct3s"));
    QString error;
    SecureSaveOptions options;
    REQUIRE(config.saveSecureToFile(secure, options, &error));

    Configuration target;
    target.clear();
    CommunicationsDialog d(&target);
    QPushButton *load = busButton(d, QStringLiteral("loadTemplateButton"));
    REQUIRE(load != nullptr);

    DialogPilot pilot;
    pilot.filePath = secure;
    load->click();

    CHECK(!pilot.wedged);
    CHECK(pilot.allText().contains(QStringLiteral(".ct3t")) ||
          pilot.allText().contains(QStringLiteral(".ct3s")));
    QTreeWidget *tree = busTree(d, 0);
    REQUIRE(tree != nullptr);
    CHECK(tree->topLevelItemCount() == 0); // nothing was added
}

void testLoadRefusesWhenTheDeviceIsFull(const QTemporaryDir &dir)
{
    Configuration source;
    buildSource(source);
    const QString path = templatePath(dir, "button-full");
    CommsTemplate ignored;
    REQUIRE(roundTrip(source, path, &ignored));

    // Every message slot on the device already spoken for, so the template's one
    // more is one too many. The refusal has to come BEFORE anything is created:
    // the alternative is a document over the limit and a Send that refuses it,
    // with the repair being to work out which of the messages just added are the
    // surplus.
    Configuration target;
    target.clear();
    for (int i = 0; i < MAX_MESSAGES; ++i) {
        CommsSection filler;
        filler.name = QStringLiteral("Filler %1").arg(i);
        filler.device = SectionDevice::ReceiveMessage;
        filler.baseAddress = quint32(0x100 + i);
        target.bus[0].sections.append(filler);
    }
    CommunicationsDialog d(&target);
    QPushButton *load = busButton(d, QStringLiteral("loadTemplateButton"));
    REQUIRE(load != nullptr);

    DialogPilot pilot;
    pilot.filePath = path;
    load->click();

    CHECK(!pilot.wedged);
    CHECK(pilot.allText().contains(QString::number(MAX_MESSAGES)));
    QTreeWidget *tree = busTree(d, 0);
    REQUIRE(tree != nullptr);
    CHECK(tree->topLevelItemCount() == MAX_MESSAGES); // nothing was added
    // And nothing was created on the way to refusing — a load that half-happened
    // would leave channels behind for messages that never arrived.
    CHECK(!target.catalog().findByName(QStringLiteral("Coolant Temp")).isValid());
    CHECK(!target.isDirty());
}

void testLoadReportsItsNotesInTheDetailsPane(const QTemporaryDir &dir)
{
    Configuration source;
    buildSource(source);
    const QString path = templatePath(dir, "button-notes");
    CommsTemplate ignored;
    REQUIRE(roundTrip(source, path, &ignored));

    // A target that disagrees about what "Coolant Temp" means, so the merge has
    // something to report.
    Configuration target;
    target.clear();
    target.catalog().addOrUpdateUserChannel(
        makeChannel(QStringLiteral("Coolant Temp"), QStringLiteral("°F"), 1.0));
    CommunicationsDialog d(&target);
    QPushButton *load = busButton(d, QStringLiteral("loadTemplateButton"));
    REQUIRE(load != nullptr);

    DialogPilot pilot;
    pilot.filePath = path;
    load->click();

    CHECK(!pilot.wedged);
    CHECK(pilot.allText().contains(QStringLiteral("note")));
    // The renames go in the scrollable details pane, not the box body: a
    // thirty-message template produces a lot of them.
    CHECK(pilot.lastDetails.contains(QStringLiteral("Coolant Temp 2")));
    CHECK(target.catalog().findByName(QStringLiteral("Coolant Temp")).baseResolution == 1.0);
}

void testLoadedMessagesReachTheDocumentThroughOk(const QTemporaryDir &dir)
{
    Configuration source;
    buildSource(source);
    CommsTemplate tmpl;
    REQUIRE(roundTrip(source, templatePath(dir, "through"), &tmpl));

    Configuration target;
    target.clear();
    target.bus[1].enabled = true;

    // The merge, then the dialog's own commit path, so what is checked is a
    // document that really has the messages rather than a working copy that did.
    CommsTemplateMerge merge;
    REQUIRE(mergeCommsTemplate(target.catalog(), 1, tmpl, &merge, nullptr));
    QString refusal;
    REQUIRE(target.applyBusSections(1, merge.sections, &refusal));
    REQUIRE(target.bus[1].sections.size() == 1);
    CHECK(target.bus[1].sections.first().baseAddress == 0x640);
    CHECK(target.bus[1].sections.first().rows.size() == 2);
    CHECK(target.catalog().findByName(QStringLiteral("Oil Temp")).isValid());
}

} // namespace

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    // Unbuffered, so a crash mid-suite still shows which checks got that far.
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    QTemporaryDir dir;
    if (!dir.isValid()) {
        std::printf("FAIL could not create a temporary directory\n");
        return 1;
    }

    // Save… and Load… create the templates folder under the real Documents,
    // which is the behaviour under test and cannot be faked without testing
    // something else. Noted here and undone at the end — but only if this run
    // is what created it, and only while it is still empty, so a developer's
    // own templates are never in reach of the cleanup.
    const QString templateDir = commsTemplatesDirectory();
    const bool templateDirExisted = QDir(templateDir).exists();

    testRoundTripPreservesEveryField(dir);
    testTheFileIsOpaque(dir);
    testATemplateIsNotAConfiguration(dir);

    testChannelsAreCreatedInAnEmptyDocument(dir);
    testAnIdenticalChannelIsReusedNotDuplicated(dir);
    testADisagreeingChannelIsRenamedAndTheDocumentsIsLeftAlone(dir);
    testADeviceChannelNameCannotBeTaken(dir);
    testARenameDoesNotCollideWithAnotherTemplateChannel(dir);
    testTwoOverlongNamesDoNotClipOntoEachOther();
    testACompoundSectionKeepsItsIdentifiers(dir);
    testARelayLoadedOntoItsOwnTargetBusIsCorrected(dir);
    testAMarkedMessageStaysMarked(dir);
    testATriggerConditionIsReportedRatherThanInvented(dir);
    testTheFolderLayout();

    testTheButtonsAreInTheAskedForOrder();
    testSaveNeedsASelectionAndLoadNeverDoes();
    testSaveFollowsTheSelection();
    testLoadedMessagesReachTheDocumentThroughOk(dir);

    testTheSaveButtonWritesAFile(dir);
    testTheSaveButtonRefusesAConcealedMessage(dir);
    testTheLoadButtonAddsTheMessages(dir);
    testTheLoadButtonRefusesTheWrongKindOfFile(dir);
    testLoadRefusesWhenTheDeviceIsFull(dir);
    testLoadReportsItsNotesInTheDetailsPane(dir);

    if (!templateDirExisted && QDir(templateDir).exists() && QDir(templateDir).isEmpty())
        QDir().rmdir(templateDir);

    if (fails == 0)
        std::printf("test_comms_template: all checks passed\n");
    else
        std::printf("test_comms_template: %d FAILURES\n", fails);
    return fails == 0 ? 0 : 1;
}
