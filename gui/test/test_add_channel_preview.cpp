// What "Clamp to Signal Limit" says the field can hold.
//
// The preview under Add Comms Channel states the range the FIELD ON THE WIRE
// can carry, in the channel's units. Two things decide it and only two: the
// DBC Type chosen in this dialog, and the Bit Length beside it. The CHANNEL's
// own data type must not come into it at all — a signed channel packed into an
// unsigned 6-bit field can carry 0..63, and saying -32..31 there would be
// describing a field that does not exist.
//
// Worth its own suite because it is arithmetic nobody checks by hand: the
// number is read, believed, and used to decide a scaling factor.

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QSpinBox>

#include <cstdio>

#include "../src/model/configuration.h"
#include "../src/ui/add_channel_dialog.h"

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

// A channel whose OWN data type is deliberately the opposite of what the field
// will be, so anything reading the channel instead of the field shows up.
void buildConfig(Configuration &config, const QString &dataType, double lo = -1e9,
                 double hi = 1e9)
{
    config.clear();
    Channel c;
    c.name = QStringLiteral("Signal");
    c.dataType = dataType;
    c.baseResolution = 1.0;
    c.decimalPlaces = 0;
    c.minValue = lo;
    c.maxValue = hi;
    c.userDefined = true;
    config.catalog().addOrUpdateUserChannel(c);
}

// Drive the dialog's widgets exactly as a user would, then read the preview
// line back off the label. Everything goes through the real controls so the
// answer is the one on screen.
QString previewFor(Configuration &config, int startBit, int bitLength, DbcType type,
                   double factor, double offset, bool clamp, bool transmit = true)
{
    CommsChannelRow initial;
    initial.channelName = QStringLiteral("Signal");
    initial.startBit = startBit;
    initial.bitLength = bitLength;
    initial.dbcType = int(type);
    initial.dbcFactor = factor;
    initial.dbcOffset = offset;
    initial.clampToRange = clamp;

    AddChannelDialog dialog(&config, initial, SectionAlignment::WordSwap, 8, transmit);

    // Set through the widgets rather than trusting the constructor, so the
    // recompute path a user triggers is the one under test.
    for (QComboBox *combo : dialog.findChildren<QComboBox *>()) {
        const int idx = combo->findData(int(type));
        if (idx >= 0 && combo->count() == 3)
            combo->setCurrentIndex(idx);
    }
    if (QCheckBox *box = dialog.findChild<QCheckBox *>())
        box->setChecked(clamp);

    // Anchored on the one word both forms of the headline share. It was
    // "Physical = raw", which is the RECEIVE form - once the transmit row
    // started stating its own mapping ("raw = (Physical + o) / f") that string
    // stopped appearing on the very rows this suite drives, and every case
    // silently read an empty label.
    for (QLabel *label : dialog.findChildren<QLabel *>())
        if (label->text().contains(QStringLiteral("Physical")))
            return label->text();
    return {};
}

// Pull "A to B" out of the preview line.
QString rangeIn(const QString &preview)
{
    const int at = preview.indexOf(QStringLiteral("bits hold "));
    if (at < 0)
        return {};
    const QString tail = preview.mid(at + 10);
    const int dash = tail.indexOf(QStringLiteral(" — "));
    return dash < 0 ? tail.trimmed() : tail.left(dash).trimmed();
}

void testTheHeadlineStatesThisRowsOwnMapping()
{
    // The line above the range used to print the RECEIVE formula on both kinds
    // of row, so a transmit row was shown one rule and, two lines down, a range
    // computed from a different one. The two contradicted each other and the
    // wrong one looked like the answer - which is how a reported offset of 12
    // at resolution 0.001 came to be read as +12 when the device would apply
    // 0.012 of it.
    Configuration config;
    buildConfig(config, QStringLiteral("u16"));

    const QString tx = previewFor(config, 0, 16, DbcType::Unsigned, 0.001, 12.0, true, true);
    REQUIRE(!tx.isEmpty());
    std::printf("  transmit headline              : %s\n",
                qPrintable(tx.section(QLatin1Char('\n'), 0, 0)));
    CHECK(tx.contains(QStringLiteral("raw = (Physical")));
    CHECK(!tx.contains(QStringLiteral("Physical = raw")));

    const QString rx = previewFor(config, 0, 16, DbcType::Unsigned, 0.001, 12.0, true, false);
    REQUIRE(!rx.isEmpty());
    std::printf("  receive headline               : %s\n",
                qPrintable(rx.section(QLatin1Char('\n'), 0, 0)));
    CHECK(rx.contains(QStringLiteral("Physical = raw")));
    CHECK(!rx.contains(QStringLiteral("raw = (Physical")));
}

void testTheReportedCase()
{
    // THE BUG AS REPORTED: a SIGNED channel, 6-bit UNSIGNED field, factor 1,
    // offset 0. Six unsigned bits hold 0..63. Nothing about the channel being
    // signed changes that — the field is what goes on the wire.
    Configuration config;
    buildConfig(config, QStringLiteral("s16"));
    const QString preview = previewFor(config, 10, 6, DbcType::Unsigned, 1.0, 0.0, true);
    REQUIRE(!preview.isEmpty());
    std::printf("  unsigned 6-bit, signed channel : %s\n", qPrintable(rangeIn(preview)));
    CHECK(rangeIn(preview) == QStringLiteral("0 to 63"));
}

void testSignedIsTheOtherHalf()
{
    // And the same field declared SIGNED holds -32..31 — not -31..32. Two's
    // complement is asymmetric: one more value below zero than above it.
    Configuration config;
    buildConfig(config, QStringLiteral("u16"));
    const QString preview = previewFor(config, 10, 6, DbcType::Signed, 1.0, 0.0, true);
    REQUIRE(!preview.isEmpty());
    std::printf("  signed 6-bit, unsigned channel : %s\n", qPrintable(rangeIn(preview)));
    CHECK(rangeIn(preview) == QStringLiteral("-32 to 31"));
}

void testTheChannelTypeNeverChangesTheAnswer()
{
    // The same field against four differently-typed channels. If any of them
    // moves the number, the preview is reading the channel where it should be
    // reading the field.
    QString first;
    for (const char *dataType : {"s8", "u8", "s32", "float"}) {
        Configuration config;
        buildConfig(config, QString::fromLatin1(dataType));
        const QString got = rangeIn(previewFor(config, 0, 8, DbcType::Unsigned, 1.0, 0.0, true));
        if (first.isEmpty())
            first = got;
        CHECK(got == QStringLiteral("0 to 255"));
        if (got != QStringLiteral("0 to 255"))
            std::printf("       channel %-6s gave %s\n", dataType, qPrintable(got));
    }
}

void testScalingMovesTheEndsButNotTheirOrder()
{
    Configuration config;
    buildConfig(config, QStringLiteral("u16"));
    // Offset is a bias in RAW COUNTS applied on the way out, so the physical
    // ends are (raw - offset) x factor.
    const QString shifted = rangeIn(previewFor(config, 0, 8, DbcType::Unsigned, 1.0, 10.0, true));
    std::printf("  8-bit unsigned, offset 10      : %s\n", qPrintable(shifted));
    CHECK(shifted == QStringLiteral("-10 to 245"));

    // THE CASE THAT SEPARATES THE TWO RULES, and the reporter's own numbers:
    // 16 bits, resolution 0.001, offset 12. The offset is in channel units, so
    // the ends are raw * 0.001 - 12. Under the old raw-count rule the same row
    // read -0.012 to 65.523, which is what sent them looking.
    const QString milli = rangeIn(previewFor(config, 0, 16, DbcType::Unsigned, 0.001, 12.0, true));
    std::printf("  16-bit, 0.001, offset 12       : %s\n", qPrintable(milli));
    CHECK(milli == QStringLiteral("-12 to 53.535"));

    // A NEGATIVE factor flips which end is which; the line must still read
    // low-to-high rather than printing them in field order.
    const QString flipped = rangeIn(previewFor(config, 0, 8, DbcType::Unsigned, -1.0, 0.0, true));
    std::printf("  8-bit unsigned, factor -1      : %s\n", qPrintable(flipped));
    CHECK(flipped == QStringLiteral("-255 to 0"));
}

void testTheChannelsOwnRangeIsNamedWhenItBindsFirst()
{
    // THE REAL GAP. On a non-wrapping row the firmware clamps to the CHANNEL's
    // declared range FIRST (engine_core.c, inverseSignalScaling), then into
    // what the field can represent. So a channel ranged -32..31 feeding a
    // 6-bit UNSIGNED field can never send 32..63 — and the preview used to
    // promise "0 to 63", describing a field the value cannot reach.
    Configuration config;
    buildConfig(config, QStringLiteral("s16"), -32, 31);
    const QString preview = previewFor(config, 10, 6, DbcType::Unsigned, 1.0, 0.0, true);
    REQUIRE(!preview.isEmpty());
    std::printf("  channel -32..31 into 6-bit unsigned:\n    %s\n",
                qPrintable(preview.section(QLatin1Char('\n'), 1, 1)));
    // The field's capacity is still stated — it is a real fact and the user is
    // choosing a bit length — but the number that BINDS is named too.
    CHECK(preview.contains(QStringLiteral("0 to 63")));
    CHECK(preview.contains(QStringLiteral("-32 to 31")));
    CHECK(preview.contains(QStringLiteral("is what is sent")));
}

void testAWiderChannelDoesNotChangeTheLine()
{
    // The control: when the channel is the wider of the two, the field really
    // is the whole story and the sentence must stay the simple one.
    Configuration config;
    buildConfig(config, QStringLiteral("s16"), -1000, 1000);
    const QString preview = previewFor(config, 10, 6, DbcType::Unsigned, 1.0, 0.0, true);
    CHECK(rangeIn(preview) == QStringLiteral("0 to 63"));
    CHECK(!preview.contains(QStringLiteral("is what is sent")));
}

void testAWrappingRowIgnoresTheChannelRange()
{
    // Unticked, the firmware SKIPS the channel clamp on purpose, so the field
    // is the whole story again even with a narrow channel.
    Configuration config;
    buildConfig(config, QStringLiteral("s16"), -32, 31);
    const QString preview = previewFor(config, 10, 6, DbcType::Unsigned, 1.0, 0.0, false);
    CHECK(rangeIn(preview) == QStringLiteral("0 to 63"));
    CHECK(preview.contains(QStringLiteral("rolls over")));
    CHECK(!preview.contains(QStringLiteral("is what is sent")));
}

void testANonOverlappingRangeIsNamedAsAFault()
{
    // A channel entirely outside the field: every value pins to one end and the
    // row sends a constant. Printed as the fault it is, not as an inside-out
    // range like "100 to 63".
    Configuration config;
    buildConfig(config, QStringLiteral("u16"), 100, 200);
    const QString preview = previewFor(config, 0, 6, DbcType::Unsigned, 1.0, 0.0, true);
    std::printf("  channel 100..200 into 6-bit unsigned:\n    %s\n",
                qPrintable(preview.section(QLatin1Char('\n'), 1, 1)));
    CHECK(preview.contains(QStringLiteral("entirely outside what the field can carry")));
    CHECK(!preview.contains(QStringLiteral("100 to 63")));
}

void testOneBitAndSixtyFour()
{
    Configuration config;
    buildConfig(config, QStringLiteral("u16"));
    CHECK(rangeIn(previewFor(config, 0, 1, DbcType::Unsigned, 1.0, 0.0, true))
          == QStringLiteral("0 to 1"));
    // One signed bit holds -1 and 0. Degenerate, but it is a length the spin
    // box offers and the arithmetic should not produce nonsense at the edge.
    CHECK(rangeIn(previewFor(config, 0, 1, DbcType::Signed, 1.0, 0.0, true))
          == QStringLiteral("-1 to 0"));
}

} // namespace

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    testTheHeadlineStatesThisRowsOwnMapping();
    testTheReportedCase();
    testSignedIsTheOtherHalf();
    testTheChannelTypeNeverChangesTheAnswer();
    testScalingMovesTheEndsButNotTheirOrder();
    testTheChannelsOwnRangeIsNamedWhenItBindsFirst();
    testAWiderChannelDoesNotChangeTheLine();
    testAWrappingRowIgnoresTheChannelRange();
    testANonOverlappingRangeIsNamedAsAFault();
    testOneBitAndSixtyFour();

    if (fails == 0)
        std::printf("test_add_channel_preview: all checks passed\n");
    else
        std::printf("test_add_channel_preview: %d FAILURES\n", fails);
    return fails == 0 ? 0 : 1;
}
