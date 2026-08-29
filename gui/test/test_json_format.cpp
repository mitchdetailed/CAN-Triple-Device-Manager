// Saving as JSON, and the three formats a document can be in.
//
// A .ct3 is a sealed binary container. JSON is the deliberate exception: the
// user picks it from Save As, and what lands on disk is the indented, legible
// shape format-1 .ct3 files had. That makes two things worth pinning, and they
// pull in opposite directions:
//
//   IT MUST REALLY BE LEGIBLE. That is the whole reason to choose it. If the
//   bytes were not readable and greppable the option would be pointless, so
//   this suite goes looking for the plaintext and requires it PRESENT —
//   the exact inverse of what test_comms_template asserts about .ct3t files,
//   and deliberately so.
//
//   IT MUST NOT HAPPEN BY ACCIDENT. Sealing is the default and every path that
//   does not explicitly ask for JSON has to stay sealed — a new document, a
//   Save on a .ct3, a Save on a .ct3s. A format that leaks into files nobody
//   chose it for is the failure this option was weighed against before it was
//   built.
//
// And the round trip, because a save format you cannot reopen is a trap rather
// than a feature.

#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <cstdio>

#include "../src/model/config_file.h"
#include "../src/model/configuration.h"
#include "../src/model/secure_file.h"

static int fails = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);              \
            ++fails;                                                                 \
        }                                                                            \
    } while (0)

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

// A document with enough in it that the round trip has something to lose: a
// receive message with two scaled signals, on a named bus, plus a channel.
void build(Configuration &config)
{
    config.clear();
    config.bus[0].enabled = true;
    config.bus[0].rateKbps = 500;

    Channel c;
    c.name = QStringLiteral("Coolant Temp");
    c.unit = QStringLiteral("°C");
    c.dataType = QStringLiteral("u16");
    c.baseResolution = 0.1;
    c.decimalPlaces = 1;
    c.userDefined = true;
    config.catalog().addOrUpdateUserChannel(c);

    CommsSection s;
    s.name = QStringLiteral("ECU Temps");
    s.device = SectionDevice::ReceiveMessage;
    s.baseAddress = 0x640;
    s.messageLengthBytes = 8;
    CommsChannelRow r;
    r.channelName = QStringLiteral("Coolant Temp");
    r.startBit = 0;
    r.bitLength = 16;
    r.dbcFactor = 0.1;
    r.dbcOffset = -40;
    s.rows << r;
    config.bus[0].sections.append(s);
}

QByteArray bytesOf(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

void testJsonIsActuallyReadable(const QTemporaryDir &dir)
{
    Configuration config;
    build(config);
    const QString path = dir.filePath(QStringLiteral("legible.json"));
    QString error;
    REQUIRE(config.saveJsonToFile(path, &error));
    CHECK(error.isEmpty());

    const QByteArray bytes = bytesOf(path);
    REQUIRE(!bytes.isEmpty());

    // THE POINT OF THE FORMAT. Every one of these is absent from a .ct3 by
    // design; here they must be present, or choosing JSON bought nothing.
    for (const char *needle : {"CANTripleConfig", "ECU Temps", "Coolant Temp", "baseAddress",
                               "startBit", "dbcFactor"})
        if (!bytes.contains(needle)) {
            std::printf("FAIL %s:%d  JSON does not contain \"%s\"\n", __FILE__, __LINE__, needle);
            ++fails;
        }

    // Indented, not one endless line — legible to a person, and diffable.
    CHECK(bytes.contains('\n'));
    CHECK(bytes.count('\n') > 20);
    // And it really is JSON, not merely text with words in it.
    CHECK(QJsonDocument::fromJson(bytes).isObject());

    // NOT the sealed container, and not the secure one: the two readers that
    // route on magic must both decline it, or Open would take the wrong path.
    CHECK(!isBinaryConfigFile(path));
    CHECK(!isSecureFile(path));
}

void testJsonRoundTrips(const QTemporaryDir &dir)
{
    Configuration source;
    build(source);
    const QString path = dir.filePath(QStringLiteral("round.json"));
    REQUIRE(source.saveJsonToFile(path, nullptr));

    // Reopened with no new reader: loadFromFile routes on what the file starts
    // with, and '{' is the format-1 path every pre-container .ct3 still takes.
    Configuration back;
    QString error;
    REQUIRE(back.loadFromFile(path, &error));
    CHECK(error.isEmpty());
    REQUIRE(back.bus[0].sections.size() == 1);
    const CommsSection &s = back.bus[0].sections.first();
    CHECK(s.name == QStringLiteral("ECU Temps"));
    CHECK(s.baseAddress == 0x640u);
    REQUIRE(s.rows.size() == 1);
    CHECK(s.rows.first().channelName == QStringLiteral("Coolant Temp"));
    CHECK(s.rows.first().dbcOffset == -40.0);
    CHECK(back.catalog().findByName(QStringLiteral("Coolant Temp")).baseResolution == 0.1);

    // Compared whole, so a field this test never thought to name is covered
    // too: what came back must equal what went out.
    CHECK(back.bus[0].sections.first().toJson() == source.bus[0].sections.first().toJson());
}

void testAJsonDocumentKeepsSavingAsJson(const QTemporaryDir &dir)
{
    // The half of "a save option" that is easy to leave out: after opening a
    // .json, plain Save must write JSON again. Silently re-sealing it would
    // change who can read the file while it keeps its name and its place on
    // disk — and the user watching version control would see the diff turn to
    // binary with nothing having said so.
    Configuration source;
    build(source);
    const QString path = dir.filePath(QStringLiteral("sticky.json"));
    REQUIRE(source.saveJsonToFile(path, nullptr));

    Configuration back;
    REQUIRE(back.loadFromFile(path, nullptr));
    CHECK(back.fileFormat() == Configuration::FileFormat::Json);
    CHECK(!back.isSecureFile());

    // A plain re-save through the same route MainWindow::onSave takes.
    REQUIRE(back.saveJsonToFile(back.filePath(), nullptr));
    CHECK(bytesOf(path).contains("ECU Temps"));
    CHECK(!isBinaryConfigFile(path));
}

void testSealedIsStillTheDefaultEverywhere(const QTemporaryDir &dir)
{
    // THE GUARD. JSON is opt-in, and every path that did not opt in stays
    // sealed. A new document first.
    Configuration config;
    build(config);
    CHECK(config.fileFormat() == Configuration::FileFormat::Sealed);

    // saveToFile is what Save As gives you without touching the dropdown.
    const QString ct3 = dir.filePath(QStringLiteral("sealed.ct3"));
    REQUIRE(config.saveToFile(ct3, nullptr));
    CHECK(config.fileFormat() == Configuration::FileFormat::Sealed);
    CHECK(isBinaryConfigFile(ct3));
    const QByteArray sealed = bytesOf(ct3);
    // The same strings the JSON test requires PRESENT must be absent here.
    for (const char *needle : {"ECU Temps", "Coolant Temp", "baseAddress", "dbcFactor"})
        if (sealed.contains(needle)) {
            std::printf("FAIL %s:%d  sealed .ct3 leaks \"%s\"\n", __FILE__, __LINE__, needle);
            ++fails;
        }

    // And a document that has been JSON does not drag the format along when it
    // is saved sealed afterwards.
    const QString json = dir.filePath(QStringLiteral("was.json"));
    REQUIRE(config.saveJsonToFile(json, nullptr));
    CHECK(config.fileFormat() == Configuration::FileFormat::Json);
    const QString again = dir.filePath(QStringLiteral("again.ct3"));
    REQUIRE(config.saveToFile(again, nullptr));
    CHECK(config.fileFormat() == Configuration::FileFormat::Sealed);
    CHECK(isBinaryConfigFile(again));
}

void testNewDocumentDoesNotInheritAFormat(const QTemporaryDir &dir)
{
    Configuration config;
    build(config);
    REQUIRE(config.saveJsonToFile(dir.filePath(QStringLiteral("first.json")), nullptr));
    CHECK(config.fileFormat() == Configuration::FileFormat::Json);

    // File > New. An untitled document has no format yet, and inheriting the
    // last one would make its first Save write a legible file nobody chose.
    config.clear();
    CHECK(config.fileFormat() == Configuration::FileFormat::Sealed);
    CHECK(config.filePath().isEmpty());
}

void testASecureDocumentIsNotDowngradedByOpeningJson(const QTemporaryDir &dir)
{
    // The three formats have to be told apart on LOAD, not guessed from a bool.
    // Before the format became a tri-state, "not secure" covered both a sealed
    // .ct3 and a legible one, and a Save could not tell which it was looking at.
    Configuration config;
    build(config);
    const QString secure = dir.filePath(QStringLiteral("locked.ct3s"));
    SecureSaveOptions options;
    REQUIRE(config.saveSecureToFile(secure, options, nullptr));
    CHECK(config.fileFormat() == Configuration::FileFormat::Secure);
    CHECK(config.isSecureFile());

    Configuration reopened;
    REQUIRE(reopened.loadFromFile(secure, nullptr));
    CHECK(reopened.fileFormat() == Configuration::FileFormat::Secure);

    const QString sealed = dir.filePath(QStringLiteral("plain.ct3"));
    REQUIRE(config.saveToFile(sealed, nullptr));
    Configuration reopenedSealed;
    REQUIRE(reopenedSealed.loadFromFile(sealed, nullptr));
    CHECK(reopenedSealed.fileFormat() == Configuration::FileFormat::Sealed);
    CHECK(!reopenedSealed.isSecureFile());
}

void testAMarkedMessageStaysMarkedThroughJson(const QTemporaryDir &dir)
{
    // Stated rather than discovered: JSON writes a Hidden message's protocol
    // out in the clear. The MARKING still travels, so the app padlocks it again
    // on reload and the password is still needed to open it — but the CAN ID and
    // bit layout are in the file for anything that is not this app. That is the
    // cost of the format and the test says so out loud.
    Configuration config;
    build(config);
    config.bus[0].sections[0].protection = CommsProtection::Hidden;
    config.bus[0].sections[0].messageKey = deriveAccessKey(QStringLiteral("supplier"));

    const QString path = dir.filePath(QStringLiteral("marked.json"));
    REQUIRE(config.saveJsonToFile(path, nullptr));

    Configuration back;
    REQUIRE(back.loadFromFile(path, nullptr));
    REQUIRE(back.bus[0].sections.size() == 1);
    CHECK(back.bus[0].sections.first().protection == CommsProtection::Hidden);
    CHECK(back.bus[0].sections.first().messageKey
          == deriveAccessKey(QStringLiteral("supplier")));

    // And the protocol really is legible, which is the honest half.
    CHECK(bytesOf(path).contains("640"));
}

void testAWriteFailureIsReported(const QTemporaryDir &dir)
{
    // A save that quietly did nothing would clear the dirty flag and lose the
    // work at exit. Windows refuses a file whose parent is a FILE, which is a
    // failure available without needing a privileged directory.
    Configuration config;
    build(config);
    const QString blocker = dir.filePath(QStringLiteral("not-a-directory"));
    QFile f(blocker);
    REQUIRE(f.open(QIODevice::WriteOnly));
    f.write("x");
    f.close();

    QString error;
    CHECK(!config.saveJsonToFile(blocker + QStringLiteral("/child.json"), &error));
    CHECK(!error.isEmpty());
    // The document is untouched: still dirty, still pointing where it was.
    CHECK(config.filePath() != blocker + QStringLiteral("/child.json"));
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    QTemporaryDir dir;
    if (!dir.isValid()) {
        std::printf("FAIL could not create a temporary directory\n");
        return 1;
    }

    testJsonIsActuallyReadable(dir);
    testJsonRoundTrips(dir);
    testAJsonDocumentKeepsSavingAsJson(dir);
    testSealedIsStillTheDefaultEverywhere(dir);
    testNewDocumentDoesNotInheritAFormat(dir);
    testASecureDocumentIsNotDowngradedByOpeningJson(dir);
    testAMarkedMessageStaysMarkedThroughJson(dir);
    testAWriteFailureIsReported(dir);

    if (fails == 0)
        std::printf("test_json_format: all checks passed\n");
    else
        std::printf("test_json_format: %d FAILURES\n", fails);
    return fails == 0 ? 0 : 1;
}
