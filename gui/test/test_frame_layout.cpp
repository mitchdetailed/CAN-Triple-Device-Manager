// Bit collisions in a CAN frame, and which of them refuse.
//
// This suite exists because the rule it tests was, until now, computed in four
// places and one of them disagreed. The disagreement was not stylistic: the
// channel-vs-identifier check treated a row as a CONTIGUOUS run of absolute bit
// positions, which is true only under WordSwap alignment and NOT under Normal,
// which is the default. So it reported clashes that were not there and missed
// the ones that were — and that is the very rule this release makes blocking.
//
// A refusal computed the wrong way is worse than no refusal: it stops correct
// work and waves through broken work. So the first group below pins the
// geometry against the device's own walk, with a worked example that fails
// under the old rule and passes under the new one.
//
// The second group is the specification the user gave, one test per line of it:
//
//     Receive,  single,   two channels overlap        warn, allowed
//     Receive,  compound, two channels overlap        warn, allowed
//     Receive,  compound, channel over identifier     REFUSED
//     Transmit, single,   two channels overlap        REFUSED
//     Transmit, compound, two channels overlap        REFUSED
//     Transmit, compound, channel over identifier     REFUSED
//     Transmit CRC8,      channel over CRC byte       REFUSED
//                         channel over identifier     REFUSED

#include <QAbstractButton>
#include <QApplication>
#include <QDialogButtonBox>
#include <QMessageBox>
#include <QPushButton>
#include <QTabWidget>
#include <QTimer>

#include <cstdio>

#include "../src/model/comms_types.h"
#include "../src/model/configuration.h"
#include "../src/model/device_mapper.h"
#include "../src/model/frame_layout.h"
#include "../src/ui/add_channel_dialog.h"
#include "../src/ui/section_editor_dialog.h"

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

CommsChannelRow row(const QString &name, int startBit, int bits)
{
    CommsChannelRow r;
    r.channelName = name;
    r.startBit = startBit;
    r.bitLength = bits;
    return r;
}

CompoundIdentifier identifier(int byteOffset, quint32 id, quint32 mask)
{
    CompoundIdentifier ident;
    ident.byteOffset = byteOffset;
    ident.id = id;
    ident.idMask = mask;
    ident.configured = true;
    return ident;
}

int count(const QList<LayoutClash> &clashes, LayoutClash::Kind kind)
{
    int n = 0;
    for (const LayoutClash &c : clashes)
        if (c.kind == kind)
            ++n;
    return n;
}

// -------------------------------------------------------------- the geometry

void testTheSelectorMatchesTheDevice()
{
    // engine_core.c's muxSelected() builds the selector as a LITTLE-ENDIAN
    // two-byte window at byteOffset:
    //     for (i = 0; i < 2; ++i) sel |= data[byteOffset + i] << (8 * i);
    // so mask bit b is bit (b % 8) of byte (byteOffset + b / 8).
    const QList<int> low = identifierBitPositions(identifier(2, 0x03, 0xFF));
    REQUIRE(low.size() == 8);
    for (int b = 0; b < 8; ++b)
        CHECK(low.at(b) == 2 * 8 + b); // byte 2, bits 0..7

    // A mask reaching into the SECOND byte of the window lands in byte 3, not
    // somewhere in byte 2 — the window really is two bytes wide.
    const QList<int> high = identifierBitPositions(identifier(2, 0x0100, 0x0100));
    REQUIRE(high.size() == 1);
    CHECK(high.first() == 3 * 8 + 0);

    // A mask of zero is the device's "always active": it never reads the
    // window, so it reserves nothing at all.
    CHECK(identifierBitPositions(identifier(2, 0, 0)).isEmpty());

    // Only the bits the mask names, not the whole window.
    const QList<int> sparse = identifierBitPositions(identifier(0, 0x0, 0x05));
    REQUIRE(sparse.size() == 2);
    CHECK(sparse.at(0) == 0);
    CHECK(sparse.at(1) == 2);
}

void testNormalAlignmentIsNotAContiguousRange()
{
    // THE WORKED EXAMPLE the old rule got wrong. Under Normal (Motorola), start
    // bit 8 length 16 ascends through byte 1 and then steps to the PREVIOUS
    // byte, so it occupies bytes 1 and 0 — not bytes 1 and 2.
    const CommsChannelRow r = row(QStringLiteral("Wide"), 8, 16);
    const QList<int> normal = rowBitPositions(r, SectionAlignment::Normal);
    REQUIRE(normal.size() == 16);
    int lo = normal.first(), hi = normal.first();
    for (int p : normal) {
        lo = qMin(lo, p);
        hi = qMax(hi, p);
    }
    CHECK(lo == 0);  // byte 0
    CHECK(hi == 15); // byte 1
    // The old linear rule would have said [8, 23] — bytes 1 and 2.
    CHECK(hi < 16);

    // WordSwap really is contiguous, which is why the old rule looked right to
    // whoever wrote it.
    const QList<int> intel = rowBitPositions(r, SectionAlignment::WordSwap);
    REQUIRE(intel.size() == 16);
    CHECK(intel.first() == 8);
    CHECK(intel.last() == 23);
}

void testTheOldLinearRuleWouldHaveBeenWrongBothWays()
{
    // Built so the two rules give OPPOSITE answers, in both directions. This is
    // the regression guard: if anyone reintroduces the linear span, one of
    // these two flips.
    const auto linearSaysClash = [](const CommsChannelRow &r, const CompoundIdentifier &ident) {
        const int first = r.startBit;
        const int last = r.startBit + qMax(1, r.bitLength) - 1;
        for (int b = 0; b < 16; ++b) {
            if (!((ident.idMask >> b) & 1u))
                continue;
            const int selBit = ident.byteOffset * 8 + b;
            if (selBit >= first && selBit <= last)
                return true;
        }
        return false;
    };

    CommsSection s;
    s.device = SectionDevice::ReceiveMessage;
    s.alignment = SectionAlignment::Normal; // the default
    s.messageLengthBytes = 8;
    s.compound = true;

    // (1) FALSE POSITIVE under the old rule. The row occupies bytes 1 and 0;
    //     the selector sits in byte 2. Linear span [8,23] touches byte 2 and
    //     would have refused a layout that is perfectly correct.
    {
        CompoundIdentifier ident = identifier(2, 0x01, 0xFF);
        ident.rows << row(QStringLiteral("Wide"), 8, 16);
        s.identifiers = {ident};
        CHECK(linearSaysClash(ident.rows.first(), ident)); // the old rule: clash
        CHECK(count(findLayoutClashes(s), LayoutClash::ChannelIdentifier) == 0); // truth: none
    }

    // (2) FALSE NEGATIVE under the old rule. Same row, selector in byte 0 —
    //     which the row really does cover. Linear span [8,23] misses byte 0
    //     entirely, so the old rule waved through a channel the selector
    //     overwrites on every frame.
    {
        CompoundIdentifier ident = identifier(0, 0x01, 0xFF);
        ident.rows << row(QStringLiteral("Wide"), 8, 16);
        s.identifiers = {ident};
        CHECK(!linearSaysClash(ident.rows.first(), ident)); // the old rule: no clash
        CHECK(count(findLayoutClashes(s), LayoutClash::ChannelIdentifier) == 1); // truth: one
    }
}

void testAnIeee754RowIsAlwaysThirtyTwoBits()
{
    // bitLength says 8; the device packs 32 regardless. Any rule reading
    // bitLength directly under-measures the field by three bytes.
    CommsSection s;
    s.device = SectionDevice::ReceiveMessage;
    s.alignment = SectionAlignment::WordSwap; // contiguous, so the span is easy to reason about
    s.messageLengthBytes = 8;
    s.compound = true;

    CommsChannelRow f = row(QStringLiteral("Float"), 0, 8);
    f.dbcType = int(DbcType::IEEE754);
    CompoundIdentifier ident = identifier(3, 0x01, 0xFF); // byte 3 — inside 0..31, outside 0..7
    ident.rows << f;
    s.identifiers = {ident};

    CHECK(rowBitPositions(f, SectionAlignment::WordSwap).size() == 32);
    CHECK(count(findLayoutClashes(s), LayoutClash::ChannelIdentifier) == 1);
}

// ------------------------------------------------- the specification, line by line

// A section carrying two rows that overlap each other at bits 0..7.
CommsSection twoOverlappingRows(SectionDevice device, bool compound)
{
    CommsSection s;
    s.device = device;
    s.alignment = SectionAlignment::WordSwap; // contiguous: the overlap is obvious by eye
    s.messageLengthBytes = 8;
    s.compound = compound;
    const CommsChannelRow a = row(QStringLiteral("A"), 0, 8);
    const CommsChannelRow b = row(QStringLiteral("B"), 4, 8); // shares bits 4..7
    if (compound) {
        CompoundIdentifier ident = identifier(7, 0x01, 0xFF); // selector well clear of both
        ident.rows << a << b;
        s.identifiers = {ident};
    } else {
        s.rows << a << b;
    }
    return s;
}

void testReceiveSingleTwoChannelsOverlapIsAllowed()
{
    const CommsSection s = twoOverlappingRows(SectionDevice::ReceiveMessage, false);
    const QList<LayoutClash> clashes = findLayoutClashes(s);
    REQUIRE(clashes.size() == 1);
    CHECK(clashes.first().kind == LayoutClash::ChannelChannel);
    // Reported, so the user is told — but NOT blocking. Two receive rows reading
    // the same bits is legitimate: reading a bit does not consume it.
    CHECK(!clashes.first().blocking);
    CHECK(!hasBlockingLayoutClash(s));
}

void testReceiveCompoundTwoChannelsOverlapIsAllowed()
{
    const CommsSection s = twoOverlappingRows(SectionDevice::ReceiveMessage, true);
    const QList<LayoutClash> clashes = findLayoutClashes(s);
    REQUIRE(clashes.size() == 1);
    CHECK(clashes.first().kind == LayoutClash::ChannelChannel);
    CHECK(!clashes.first().blocking);
    CHECK(!hasBlockingLayoutClash(s));
}

void testTransmitSingleTwoChannelsOverlapIsRefused()
{
    const CommsSection s = twoOverlappingRows(SectionDevice::TransmitMessage, false);
    const QList<LayoutClash> clashes = findLayoutClashes(s);
    REQUIRE(clashes.size() == 1);
    CHECK(clashes.first().kind == LayoutClash::ChannelChannel);
    // On transmit the same overlap is two values packed into one place: the row
    // packed last wins and the other never leaves the device.
    CHECK(clashes.first().blocking);
    CHECK(hasBlockingLayoutClash(s));
}

void testTransmitCompoundTwoChannelsOverlapIsRefused()
{
    const CommsSection s = twoOverlappingRows(SectionDevice::TransmitMessage, true);
    const QList<LayoutClash> clashes = findLayoutClashes(s);
    REQUIRE(clashes.size() == 1);
    CHECK(clashes.first().blocking);
    CHECK(hasBlockingLayoutClash(s));
}

// A compound section whose row sits under its own identifier's selector.
CommsSection rowUnderSelector(SectionDevice device)
{
    CommsSection s;
    s.device = device;
    s.alignment = SectionAlignment::WordSwap;
    s.messageLengthBytes = 8;
    s.compound = true;
    CompoundIdentifier ident = identifier(0, 0x01, 0xFF); // selector = byte 0
    ident.rows << row(QStringLiteral("Under"), 0, 8);     // the same byte
    s.identifiers = {ident};
    return s;
}

void testReceiveCompoundChannelOverIdentifierIsRefused()
{
    // The one the old code did not check AT ALL: the whole selector block sat
    // inside `if (s.isTransmit())`, so a compound RECEIVE section got no
    // identifier check. A receive row under the selector decodes the identifier
    // value instead of its own, which is just as wrong as the transmit case.
    const CommsSection s = rowUnderSelector(SectionDevice::ReceiveMessage);
    const QList<LayoutClash> clashes = findLayoutClashes(s);
    REQUIRE(clashes.size() == 1);
    CHECK(clashes.first().kind == LayoutClash::ChannelIdentifier);
    CHECK(clashes.first().blocking);
    CHECK(hasBlockingLayoutClash(s));
}

void testTransmitCompoundChannelOverIdentifierIsRefused()
{
    const CommsSection s = rowUnderSelector(SectionDevice::TransmitMessage);
    const QList<LayoutClash> clashes = findLayoutClashes(s);
    REQUIRE(clashes.size() == 1);
    CHECK(clashes.first().kind == LayoutClash::ChannelIdentifier);
    CHECK(clashes.first().blocking);
}

void testTransmitCrc8ChannelOverTheStampedByteIsRefused()
{
    CommsSection s;
    s.device = SectionDevice::TransmitCrc8;
    s.alignment = SectionAlignment::WordSwap;
    s.messageLengthBytes = 8;
    s.crcByteLocation = 7;
    s.rows << row(QStringLiteral("Late"), 56, 8); // byte 7, the stamped one
    const QList<LayoutClash> clashes = findLayoutClashes(s);
    REQUIRE(clashes.size() == 1);
    CHECK(clashes.first().kind == LayoutClash::ChannelCrc);
    // Was a Warning, on the grounds that "the section still maps". It maps and
    // then ships a frame that does not carry what it claims.
    CHECK(clashes.first().blocking);
    CHECK(hasBlockingLayoutClash(s));
}

void testTransmitCrc8ChannelOverAnIdentifierIsRefused()
{
    CommsSection s;
    s.device = SectionDevice::TransmitCrc8;
    s.alignment = SectionAlignment::WordSwap;
    s.messageLengthBytes = 8;
    s.crcByteLocation = 7;
    s.compound = true;
    CompoundIdentifier ident = identifier(0, 0x01, 0xFF);
    ident.rows << row(QStringLiteral("Under"), 0, 8);
    s.identifiers = {ident};
    const QList<LayoutClash> clashes = findLayoutClashes(s);
    REQUIRE(clashes.size() == 1);
    CHECK(clashes.first().kind == LayoutClash::ChannelIdentifier);
    CHECK(clashes.first().blocking);
}

// ------------------------------------------------------ what must NOT be flagged

void testDifferentIdentifiersMayReuseTheSameBits()
{
    // THE case that would make every compound message an error if the check
    // walked the section rather than each variant. Identifiers are
    // mutually-exclusive frame variants; two of them on the same bits is the
    // entire point of multiplexing.
    CommsSection s;
    s.device = SectionDevice::TransmitMessage; // the strict side, to be sure
    s.alignment = SectionAlignment::WordSwap;
    s.messageLengthBytes = 8;
    s.compound = true;
    CompoundIdentifier one = identifier(0, 0x01, 0xFF);
    one.rows << row(QStringLiteral("Speed"), 8, 16);
    CompoundIdentifier two = identifier(0, 0x02, 0xFF);
    two.rows << row(QStringLiteral("Temp"), 8, 16); // exactly the same bits
    s.identifiers = {one, two};
    CHECK(findLayoutClashes(s).isEmpty());
    CHECK(!hasBlockingLayoutClash(s));
}

void testAnotherIdentifiersSelectorIsNotReservedAgainstThisOne()
{
    // Identifier 2's selector is stamped only into identifier 2's frame, so a
    // row of identifier 1 sitting on those bits is not a collision.
    CommsSection s;
    s.device = SectionDevice::TransmitMessage;
    s.alignment = SectionAlignment::WordSwap;
    s.messageLengthBytes = 8;
    s.compound = true;
    CompoundIdentifier one = identifier(0, 0x01, 0xFF); // selector byte 0
    one.rows << row(QStringLiteral("Payload"), 8, 8);   // byte 1
    CompoundIdentifier two = identifier(1, 0x02, 0xFF); // selector byte 1 (!)
    two.rows << row(QStringLiteral("Other"), 16, 8);
    s.identifiers = {one, two};
    // one's row is in byte 1, which is TWO's selector. Not a clash.
    CHECK(findLayoutClashes(s).isEmpty());

    // And reservedBits agrees, per identifier.
    CHECK(!reservedBits(s, 0).contains(8));  // byte 1 is not reserved for ident 1
    CHECK(reservedBits(s, 1).contains(8));   // it is for ident 2
}

void testAnUnconfiguredIdentifierReservesNothing()
{
    CommsSection s;
    s.device = SectionDevice::TransmitMessage;
    s.alignment = SectionAlignment::WordSwap;
    s.messageLengthBytes = 8;
    s.compound = true;
    CompoundIdentifier ident = identifier(0, 0x01, 0xFF);
    ident.configured = false; // a slot the user never set up
    ident.rows << row(QStringLiteral("Under"), 0, 8);
    s.identifiers = {ident};
    CHECK(count(findLayoutClashes(s), LayoutClash::ChannelIdentifier) == 0);
    CHECK(reservedBits(s, 0).isEmpty());
}

void testRelayAndOffCarryNothing()
{
    for (SectionDevice device : {SectionDevice::MessageRelay, SectionDevice::Off}) {
        CommsSection s = twoOverlappingRows(SectionDevice::TransmitMessage, false);
        s.device = device;
        CHECK(findLayoutClashes(s).isEmpty());
        CHECK(!hasBlockingLayoutClash(s));
    }
}

void testARowThatCannotBeLaidOutIsNotReportedAsAClash()
{
    // computeExtraction already refuses these with a better message, and a row
    // with no valid position cannot be said to collide with anything.
    CommsSection s;
    s.device = SectionDevice::TransmitMessage;
    s.alignment = SectionAlignment::WordSwap;
    s.messageLengthBytes = 8;
    s.rows << row(QStringLiteral("Good"), 0, 8)
           << row(QStringLiteral("Nonsense"), 0, 0); // zero length: no positions
    CHECK(findLayoutClashes(s).isEmpty());

    // A row with no channel chosen is likewise skipped.
    CommsSection blank;
    blank.device = SectionDevice::TransmitMessage;
    blank.alignment = SectionAlignment::WordSwap;
    blank.messageLengthBytes = 8;
    blank.rows << row(QStringLiteral("Good"), 0, 8) << row(QString(), 0, 8);
    CHECK(findLayoutClashes(blank).isEmpty());
}

void testTheMessagesNameWhatIsWrong()
{
    // The sentence is what the user reads at OK and again in Check Channels, so
    // it has to name the things involved.
    const CommsSection over = rowUnderSelector(SectionDevice::TransmitMessage);
    const QList<LayoutClash> a = findLayoutClashes(over);
    REQUIRE(a.size() == 1);
    CHECK(a.first().message().contains(QStringLiteral("Under")));
    CHECK(a.first().message().contains(QStringLiteral("identifier 1"))); // 1-based

    const CommsSection pair = twoOverlappingRows(SectionDevice::TransmitMessage, false);
    const QList<LayoutClash> b = findLayoutClashes(pair);
    REQUIRE(b.size() == 1);
    CHECK(b.first().message().contains(QStringLiteral("A")));
    CHECK(b.first().message().contains(QStringLiteral("B")));
}

void testReservedBitsCoverTheSelectorAndTheCrcByte()
{
    CommsSection s;
    s.device = SectionDevice::TransmitCrc8;
    s.alignment = SectionAlignment::WordSwap;
    s.messageLengthBytes = 8;
    s.crcByteLocation = 7;
    s.compound = true;
    s.identifiers = {identifier(0, 0x01, 0xFF)};

    const QHash<int, QString> reserved = reservedBits(s, 0);
    for (int b = 0; b < 8; ++b) {
        CHECK(reserved.contains(b));      // byte 0, the selector
        CHECK(reserved.contains(56 + b)); // byte 7, the CRC stamp
    }
    CHECK(!reserved.contains(8)); // byte 1 is ordinary frame
    // Each reason says which thing spoke for the bit.
    CHECK(reserved.value(0).contains(QStringLiteral("identifier 1")));
    CHECK(reserved.value(56).contains(QStringLiteral("CRC8")));

    // A simple section has no identifier, so -1 is the right index and only the
    // CRC byte is reserved.
    CommsSection simple = s;
    simple.compound = false;
    simple.identifiers.clear();
    const QHash<int, QString> plain = reservedBits(simple, -1);
    CHECK(!plain.contains(0));
    CHECK(plain.contains(56));
}

// ------------------------------------------- the refusal, through the real dialog

// The rule above is proven; this is the part a USER meets. accept() has ten
// other refusals ahead of this one, syncParametersFromUi() has to have run
// first, and the message box is modal — none of which a model-level test can
// speak to. So the dialog is built for real and OK is pressed for real.
class BoxCatcher : public QObject
{
public:
    QStringList texts;
    bool wedged = false;

    BoxCatcher()
    {
        connect(&m_timer, &QTimer::timeout, this, &BoxCatcher::tick);
        m_timer.start(5);
    }
    QString all() const { return texts.join(QLatin1Char('\n')); }

private:
    void tick()
    {
        QWidget *w = QApplication::activeModalWidget();
        auto *box = qobject_cast<QMessageBox *>(w);
        if (!box) {
            if (w && ++m_stuck > 200) { // something unexpected and unclosable
                wedged = true;
                w->close();
                m_stuck = 0;
            }
            return;
        }
        m_stuck = 0;
        texts << box->text();
        if (QAbstractButton *b = box->button(QMessageBox::Ok))
            b->click();
        else if (!box->buttons().isEmpty())
            box->buttons().first()->click();
    }

    QTimer m_timer;
    int m_stuck = 0;
};

// Open `section` in the real editor and press OK. Returns true when the dialog
// CLOSED (accepted), false when it refused.
bool okIsAccepted(const CommsSection &section, QString *saidWhat)
{
    Configuration config;
    config.clear();
    config.bus[0].enabled = true;
    for (const CommsChannelRow &r : section.allRows())
        if (!r.channelName.isEmpty() && !config.catalog().findByName(r.channelName).isValid()) {
            Channel c;
            c.name = r.channelName;
            c.dataType = QStringLiteral("u16");
            c.userDefined = true;
            config.catalog().addOrUpdateUserChannel(c);
        }

    SectionEditorDialog dialog(&config, section, 0, {}, -1);
    BoxCatcher catcher;
    bool closed = false;
    QObject::connect(&dialog, &QDialog::accepted, [&closed]() { closed = true; });
    QMetaObject::invokeMethod(&dialog, "accept", Qt::DirectConnection);
    if (saidWhat)
        *saidWhat = catcher.all();
    if (catcher.wedged)
        std::printf("       (a modal dialog had to be forced closed)\n");
    return closed;
}

void testTheEditorRefusesATransmitOverlapAtOk()
{
    QString said;
    CHECK(!okIsAccepted(twoOverlappingRows(SectionDevice::TransmitMessage, false), &said));
    CHECK(said.contains(QStringLiteral("overlap")));
    CHECK(said.contains(QStringLiteral("A")));
    CHECK(said.contains(QStringLiteral("B")));
}

void testTheEditorAllowsAReceiveOverlapAtOk()
{
    // The one case that must still close. If this ever starts refusing, the
    // rule has been applied too widely and ordinary receive configurations
    // stop being editable.
    QString said;
    CHECK(okIsAccepted(twoOverlappingRows(SectionDevice::ReceiveMessage, false), &said));
    CHECK(!said.contains(QStringLiteral("cannot be saved")));
}

void testTheEditorRefusesAChannelUnderTheSelectorAtOk()
{
    // Both directions: this is the case the old code never checked on receive
    // at all, because the whole selector block sat inside `if (isTransmit())`.
    for (SectionDevice device : {SectionDevice::ReceiveMessage, SectionDevice::TransmitMessage}) {
        QString said;
        CHECK(!okIsAccepted(rowUnderSelector(device), &said));
        CHECK(said.contains(QStringLiteral("identifier 1")));
        CHECK(said.contains(QStringLiteral("Under")));
    }
}

void testTheEditorRefusesAChannelInTheCrcByteAtOk()
{
    CommsSection s;
    s.name = QStringLiteral("Stamped");
    s.device = SectionDevice::TransmitCrc8;
    s.alignment = SectionAlignment::WordSwap;
    s.messageLengthBytes = 8;
    s.crcByteLocation = 7;
    s.crcChannel = QStringLiteral("Checksum");
    s.crcElements.append({CommsSection::CrcElement::Data, 0});
    s.rows << row(QStringLiteral("Late"), 56, 8);
    QString said;
    CHECK(!okIsAccepted(s, &said));
    CHECK(said.contains(QStringLiteral("CRC8")));
}

// Is OK live on an Add/Change Channel dialog holding `initial`, with `reserved`
// off limits?
bool addChannelWouldAccept(const CommsChannelRow &initial, const QHash<int, QString> &reserved)
{
    Configuration config;
    config.clear();
    Channel c;
    c.name = initial.channelName;
    c.dataType = QStringLiteral("u16");
    c.userDefined = true;
    config.catalog().addOrUpdateUserChannel(c);

    AddChannelDialog dialog(&config, initial, SectionAlignment::WordSwap, 8, /*transmit=*/true,
                            {}, nullptr, CommsProtection::None, reserved);
    auto *box = dialog.findChild<QDialogButtonBox *>();
    if (!box || !box->button(QDialogButtonBox::Ok))
        return false;
    return box->button(QDialogButtonBox::Ok)->isEnabled();
}

void testAChannelCannotBePLACEDOnAReservedBit()
{
    // THE "masked from the ability to write or add" half. Refusing only at the
    // section editor's OK would let the user finish the row, close the editor,
    // and be told at the very end that the placement they just made is not
    // allowed — so the placement itself is refused, live, while the start bit is
    // being chosen, and OK on this dialog goes dead.
    const CommsSection s = rowUnderSelector(SectionDevice::TransmitMessage);
    const QHash<int, QString> reserved = reservedBits(s, 0); // identifier 1: bits 0..7
    REQUIRE(!reserved.isEmpty());

    // On the selector.
    CHECK(!addChannelWouldAccept(row(QStringLiteral("Sig"), 0, 8), reserved));
    // Partly on it — one shared bit is enough, because one overwritten bit is
    // enough to change the value that arrives.
    CHECK(!addChannelWouldAccept(row(QStringLiteral("Sig"), 7, 8), reserved));

    // CLEAR of it, which is the control: without this the test would pass just
    // as happily on a dialog that never enables OK at all.
    CHECK(addChannelWouldAccept(row(QStringLiteral("Sig"), 16, 8), reserved));
    // And with nothing reserved, the same placement that was refused is fine.
    CHECK(addChannelWouldAccept(row(QStringLiteral("Sig"), 0, 8), {}));
}

void testACleanSectionStillCloses()
{
    // The control. Without it every test above would pass just as happily on a
    // dialog that refused everything.
    CommsSection s;
    s.name = QStringLiteral("Fine");
    s.device = SectionDevice::TransmitMessage;
    s.alignment = SectionAlignment::WordSwap;
    s.messageLengthBytes = 8;
    s.rows << row(QStringLiteral("A"), 0, 8) << row(QStringLiteral("B"), 8, 8);
    QString said;
    CHECK(okIsAccepted(s, &said));
    CHECK(said.isEmpty());
}

} // namespace

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv); // widgets below, and translate() wants an instance
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    testTheSelectorMatchesTheDevice();
    testNormalAlignmentIsNotAContiguousRange();
    testTheOldLinearRuleWouldHaveBeenWrongBothWays();
    testAnIeee754RowIsAlwaysThirtyTwoBits();

    testReceiveSingleTwoChannelsOverlapIsAllowed();
    testReceiveCompoundTwoChannelsOverlapIsAllowed();
    testTransmitSingleTwoChannelsOverlapIsRefused();
    testTransmitCompoundTwoChannelsOverlapIsRefused();
    testReceiveCompoundChannelOverIdentifierIsRefused();
    testTransmitCompoundChannelOverIdentifierIsRefused();
    testTransmitCrc8ChannelOverTheStampedByteIsRefused();
    testTransmitCrc8ChannelOverAnIdentifierIsRefused();

    testDifferentIdentifiersMayReuseTheSameBits();
    testAnotherIdentifiersSelectorIsNotReservedAgainstThisOne();
    testAnUnconfiguredIdentifierReservesNothing();
    testRelayAndOffCarryNothing();
    testARowThatCannotBeLaidOutIsNotReportedAsAClash();
    testTheMessagesNameWhatIsWrong();
    testReservedBitsCoverTheSelectorAndTheCrcByte();

    testTheEditorRefusesATransmitOverlapAtOk();
    testTheEditorAllowsAReceiveOverlapAtOk();
    testTheEditorRefusesAChannelUnderTheSelectorAtOk();
    testTheEditorRefusesAChannelInTheCrcByteAtOk();
    testAChannelCannotBePLACEDOnAReservedBit();
    testACleanSectionStillCloses();

    if (fails == 0)
        std::printf("test_frame_layout: all checks passed\n");
    else
        std::printf("test_frame_layout: %d FAILURES\n", fails);
    return fails == 0 ? 0 : 1;
}
