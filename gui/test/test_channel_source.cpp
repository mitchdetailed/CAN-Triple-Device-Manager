// The Channel Editor's Source column: where a value COMES FROM.
//
// The column answers one question — what produces this channel — and the thing
// that made it wrong is that a comms message can sit on either side of that.
// A RECEIVE message decodes the frame and writes the channel, so it is a
// source. A TRANSMIT message reads the channel and puts it on the wire; it is
// where the value GOES. Naming a transmit message here inverted the direction
// of the whole column, and for a channel that is only transmitted it also
// answered a question nobody asked — which message carries it.
//
// The fix was not a special case but a deletion: the column had its own walk of
// the document, a second implementation of a rule analyzeChannelUsage() already
// owns for Check Channels and the Config Summary. Four more disagreements came
// out with the transmit one, and the cases below pin all five — a relay's
// leftover rows, a zero-mask identifier, an inactive calculation, and an Off
// section — because each was a way of naming as a source something the device
// never generates.
//
// The one transmit message that IS a source stays: a Transmit CRC8 publishes
// its computed checksum into a channel, and the device really does write it.

#include <QApplication>
#include <QHeaderView>
#include <QTreeWidget>

#include <cstdio>

#include "../src/model/comms_types.h"
#include "../src/model/configuration.h"
#include "../src/ui/channel_editor_dialog.h"

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

void addChannel(Configuration &config, const char *name)
{
    Channel c;
    c.name = QString::fromLatin1(name);
    c.dataType = QStringLiteral("u16");
    c.baseResolution = 1.0;
    c.minValue = 0;
    c.maxValue = 1000;
    c.userDefined = true;
    config.catalog().addOrUpdateUserChannel(c);
}

CommsChannelRow row(const char *name, int startBit = 0)
{
    CommsChannelRow r;
    r.channelName = QString::fromLatin1(name);
    r.startBit = startBit;
    r.bitLength = 8;
    return r;
}

// The Source cell for a channel, read off the real dialog's table.
QString sourceOf(Configuration &config, const QString &channel)
{
    ChannelEditorDialog dialog(&config);
    auto *tree = dialog.findChild<QTreeWidget *>();
    if (!tree)
        return QStringLiteral("<no table>");
    int sourceCol = -1;
    for (int c = 0; c < tree->columnCount(); ++c)
        if (tree->headerItem()->text(c) == QStringLiteral("Source"))
            sourceCol = c;
    if (sourceCol < 0)
        return QStringLiteral("<no Source column>");
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *item = tree->topLevelItem(i);
        if (item->data(0, Qt::UserRole).toString().compare(channel, Qt::CaseInsensitive) == 0)
            return item->text(sourceCol);
    }
    return QStringLiteral("<channel not listed>");
}

void report(const char *what, const QString &got)
{
    std::printf("  %-34s %s\n", what, qPrintable(got));
}

// ------------------------------------------------------------ the report

void testATransmitMessageIsNotASource()
{
    // THE BUG AS REPORTED. "Speed" is produced by nothing and transmitted by
    // CAN 1. The column used to name that message; it is the destination.
    Configuration config;
    config.clear();
    addChannel(config, "Speed");
    config.bus[0].enabled = true;
    CommsSection tx;
    tx.name = QStringLiteral("Dash Out");
    tx.device = SectionDevice::TransmitMessage;
    tx.baseAddress = 0x700;
    tx.messageLengthBytes = 8;
    tx.rows << row("Speed");
    config.bus[0].sections.append(tx);

    const QString got = sourceOf(config, QStringLiteral("Speed"));
    report("transmit-only channel", got);
    CHECK(!got.contains(QStringLiteral("Dash Out")));
    CHECK(!got.contains(QStringLiteral("Transmit"), Qt::CaseInsensitive));
    CHECK(got == QStringLiteral("not generated"));
    // AND NOT "unused". Check Channels offers to delete exactly the unused set,
    // and this channel is carried by a message — saying unused here would point
    // the user at deleting something a transmit row depends on.
    CHECK(got != QStringLiteral("unused"));
}

void testAReceiveMessageIsASource()
{
    Configuration config;
    config.clear();
    addChannel(config, "Rpm");
    config.bus[0].enabled = true;
    CommsSection rx;
    rx.name = QStringLiteral("ECU Broadcast");
    rx.device = SectionDevice::ReceiveMessage;
    rx.baseAddress = 0x640;
    rx.messageLengthBytes = 8;
    rx.rows << row("Rpm");
    config.bus[0].sections.append(rx);

    const QString got = sourceOf(config, QStringLiteral("Rpm"));
    report("received channel", got);
    CHECK(got.contains(QStringLiteral("ECU Broadcast")));
    CHECK(got.contains(QStringLiteral("CAN 1")));
}

void testTheReceiveSideWinsWhenAChannelIsBoth()
{
    // Received on CAN 1, re-transmitted on CAN 2 — the ordinary gateway shape.
    // Exactly one of those two is a source, and the column must name it alone.
    Configuration config;
    config.clear();
    addChannel(config, "Rpm");
    config.bus[0].enabled = true;
    config.bus[1].enabled = true;
    CommsSection rx;
    rx.name = QStringLiteral("ECU Broadcast");
    rx.device = SectionDevice::ReceiveMessage;
    rx.baseAddress = 0x640;
    rx.messageLengthBytes = 8;
    rx.rows << row("Rpm");
    config.bus[0].sections.append(rx);
    CommsSection tx;
    tx.name = QStringLiteral("Dash Out");
    tx.device = SectionDevice::TransmitMessage;
    tx.baseAddress = 0x700;
    tx.messageLengthBytes = 8;
    tx.rows << row("Rpm");
    config.bus[1].sections.append(tx);

    const QString got = sourceOf(config, QStringLiteral("Rpm"));
    report("received then re-transmitted", got);
    CHECK(got.contains(QStringLiteral("ECU Broadcast")));
    CHECK(!got.contains(QStringLiteral("Dash Out")));
}

void testTheCrc8PublishChannelStaysASource()
{
    // The deliberate exception, and the reason "transmit" is not the test:
    // a Transmit CRC8 section COMPUTES a checksum and writes it into a channel
    // on every compose. That channel really is generated, by a transmit
    // message. Dropping it would have Check Channels call a channel the device
    // is writing "unused".
    Configuration config;
    config.clear();
    addChannel(config, "Speed");
    addChannel(config, "Checksum");
    config.bus[0].enabled = true;
    CommsSection tx;
    tx.name = QStringLiteral("Guarded Out");
    tx.device = SectionDevice::TransmitCrc8;
    tx.baseAddress = 0x700;
    tx.messageLengthBytes = 8;
    tx.crcChannel = QStringLiteral("Checksum");
    tx.crcByteLocation = 7;
    tx.rows << row("Speed");
    config.bus[0].sections.append(tx);

    const QString crc = sourceOf(config, QStringLiteral("Checksum"));
    report("CRC8 publish channel", crc);
    CHECK(crc.contains(QStringLiteral("Guarded Out")));
    CHECK(crc.contains(QStringLiteral("CRC8")));
    // Its ordinary rows are still only transmitted.
    const QString sent = sourceOf(config, QStringLiteral("Speed"));
    report("row of the same CRC8 message", sent);
    CHECK(sent == QStringLiteral("not generated"));
}

// ------------------------------------------- the four that came out with it

void testAnInactiveCalculationGeneratesNothing()
{
    Configuration config;
    config.clear();
    addChannel(config, "Derived");
    MathRow m;
    m.destChannel = QStringLiteral("Derived");
    m.active = false;
    config.mathRows.append(m);

    const QString got = sourceOf(config, QStringLiteral("Derived"));
    report("inactive math output", got);
    CHECK(!got.contains(QStringLiteral("Math")));

    config.mathRows[0].active = true;
    const QString live = sourceOf(config, QStringLiteral("Derived"));
    report("the same row, active", live);
    CHECK(live.contains(QStringLiteral("Math")));
}

void testAZeroMaskIdentifierGeneratesNothing()
{
    // mapToDevice skips an identifier whose ID Mask is 0, so its rows are never
    // decoded on the device and nothing they name is produced.
    Configuration config;
    config.clear();
    addChannel(config, "Variant");
    config.bus[0].enabled = true;
    CommsSection rx;
    rx.name = QStringLiteral("Multiplexed");
    rx.device = SectionDevice::ReceiveMessage;
    rx.baseAddress = 0x640;
    rx.messageLengthBytes = 8;
    rx.compound = true;
    for (int i = 0; i < 16; ++i)
        rx.identifiers.append(CompoundIdentifier{});
    rx.identifiers[0].configured = true;
    rx.identifiers[0].idMask = 0; // never matches; the mapper drops it
    rx.identifiers[0].rows << row("Variant", 16);
    config.bus[0].sections.append(rx);

    const QString got = sourceOf(config, QStringLiteral("Variant"));
    report("row under a zero-mask ID", got);
    CHECK(!got.contains(QStringLiteral("Multiplexed")));

    config.bus[0].sections[0].identifiers[0].idMask = 0xFF;
    const QString live = sourceOf(config, QStringLiteral("Variant"));
    report("the same row, mask 0xFF", live);
    CHECK(live.contains(QStringLiteral("Multiplexed")));
}

void testARelaysLeftoverRowsGenerateNothing()
{
    // A section re-typed to Message Relay keeps its rows in the file, but the
    // device emits no relay signals — nothing is decoded, nothing produced.
    Configuration config;
    config.clear();
    addChannel(config, "Stale");
    config.bus[0].enabled = true;
    CommsSection relay;
    relay.name = QStringLiteral("Gateway Rule");
    relay.device = SectionDevice::MessageRelay;
    relay.baseAddress = 0x640;
    relay.messageLengthBytes = 8;
    relay.rows << row("Stale");
    config.bus[0].sections.append(relay);

    const QString got = sourceOf(config, QStringLiteral("Stale"));
    report("leftover row on a relay", got);
    CHECK(!got.contains(QStringLiteral("Gateway Rule")));
}

void testAnOffSectionGeneratesNothing()
{
    Configuration config;
    config.clear();
    addChannel(config, "Dormant");
    config.bus[0].enabled = true;
    CommsSection off;
    off.name = QStringLiteral("Switched Off");
    off.device = SectionDevice::Off;
    off.baseAddress = 0x640;
    off.messageLengthBytes = 8;
    off.rows << row("Dormant");
    config.bus[0].sections.append(off);

    const QString got = sourceOf(config, QStringLiteral("Dormant"));
    report("row on an Off message", got);
    CHECK(!got.contains(QStringLiteral("Switched Off")));
    // Referenced but dormant: not generated, and NOT offered for cleanup.
    CHECK(got != QStringLiteral("unused"));
}

// --------------------------------------------------------- the other ends

void testTwoWritersAreBothNamed()
{
    // A duplicate-writer conflict, which Check Channels reports as a warning:
    // both write one slot and whichever runs last wins. Showing only the first
    // would hide the half that explains the reading.
    Configuration config;
    config.clear();
    addChannel(config, "Rpm");
    config.bus[0].enabled = true;
    CommsSection rx;
    rx.name = QStringLiteral("ECU Broadcast");
    rx.device = SectionDevice::ReceiveMessage;
    rx.baseAddress = 0x640;
    rx.messageLengthBytes = 8;
    rx.rows << row("Rpm");
    config.bus[0].sections.append(rx);
    MathRow m;
    m.destChannel = QStringLiteral("Rpm");
    m.active = true;
    config.mathRows.append(m);

    const QString got = sourceOf(config, QStringLiteral("Rpm"));
    report("written by a message AND math", got);
    CHECK(got.contains(QStringLiteral("ECU Broadcast")));
    CHECK(got.contains(QStringLiteral("Math")));
}

void testADeviceChannelIsInternal()
{
    Configuration config;
    config.clear();
    const QList<Channel> device = ChannelCatalog::deviceChannels();
    REQUIRE(!device.isEmpty());
    const QString name = device.first().name;
    const QString got = sourceOf(config, name);
    report(qPrintable(QStringLiteral("device channel %1").arg(name)), got);
    CHECK(got == QStringLiteral("Internal"));
}

void testAChannelNothingTouchesIsUnused()
{
    // The word has to keep meaning what Check Channels means by it, or the
    // cleanup it offers stops matching what this column shows.
    Configuration config;
    config.clear();
    addChannel(config, "Orphan");
    const QString got = sourceOf(config, QStringLiteral("Orphan"));
    report("referenced by nothing", got);
    CHECK(got == QStringLiteral("unused"));
}

} // namespace

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    testATransmitMessageIsNotASource();
    testAReceiveMessageIsASource();
    testTheReceiveSideWinsWhenAChannelIsBoth();
    testTheCrc8PublishChannelStaysASource();
    testAnInactiveCalculationGeneratesNothing();
    testAZeroMaskIdentifierGeneratesNothing();
    testARelaysLeftoverRowsGenerateNothing();
    testAnOffSectionGeneratesNothing();
    testTwoWritersAreBothNamed();
    testADeviceChannelIsInternal();
    testAChannelNothingTouchesIsUnused();

    if (fails == 0)
        std::printf("test_channel_source: all checks passed\n");
    else
        std::printf("test_channel_source: %d FAILURES\n", fails);
    return fails == 0 ? 0 : 1;
}
