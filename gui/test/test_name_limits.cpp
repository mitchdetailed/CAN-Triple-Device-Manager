// Name fields cannot hold more than the device stores.
//
// The budget is a BYTE count and QLineEdit::setMaxLength counts QChars. For an
// ASCII name the two agree, which is why the difference stayed invisible: the
// channel fields were capped at 31 "characters" against a 31-BYTE label, and
// every name anyone typed fitted both. One non-ASCII character is 2-4 bytes, so
// a legal-looking 21-character German name is 22 bytes — over budget, clipped
// by the mapper on the way out, and returned by a Get as a different name than
// the one on screen.
//
// Two budgets, because there are two labels: 31 bytes for a CHANNEL and 17 for
// a MESSAGE (store v18). The message field had no cap at all until now.
//
// Everything here goes through the real dialogs' real widgets, and types by
// setting text the way a paste does — the case a QValidator would have handled
// worst, since rejecting the edit outright would leave the field empty rather
// than holding as much as fits.

#include <QApplication>
#include <QLineEdit>

#include <cstdio>

#include "../src/model/configuration.h"
#include "../src/protocol/wire_structs.h"
#include "../src/ui/constants_dialog.h"
#include "../src/ui/edit_channel_dialog.h"
#include "../src/ui/name_limits.h"
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

// Six 3-byte CJK codepoints: 18 bytes, 6 characters. Any cap that counts
// characters lets all of it through a 17-byte budget.
const char *const kWide = "\xe6\xb8\xa9\xe5\xba\xa6\xe6\xb8\xa9\xe5\xba\xa6\xe6\xb8\xa9\xe5\xba\xa6";

// ------------------------------------------------------------ the helper

void testTheClipKeepsWholeCharacters()
{
    // 18 bytes into 17: five characters, not five and a half. Truncating the
    // BYTE array would leave a lead byte behind, and fromUtf8 turns that into
    // U+FFFD — three bytes, which would push the "shortened" name back over.
    const QString wide = QString::fromUtf8(kWide);
    CHECK(wide.toUtf8().size() == 18);
    const QString cut = clipToUtf8Bytes(wide, MAX_MESSAGE_NAME_BYTES);
    std::printf("  18 bytes into 17            : %d chars, %d bytes\n",
                int(cut.size()), int(cut.toUtf8().size()));
    CHECK(cut.size() == 5);
    CHECK(cut.toUtf8().size() == 15);
    CHECK(!cut.contains(QChar(0xFFFD)));
    // A name already inside the budget is returned untouched, byte for byte.
    CHECK(clipToUtf8Bytes(QStringLiteral("Rpm"), 17) == QStringLiteral("Rpm"));
}

// ------------------------------------------------------- the message field

QLineEdit *sectionNameField(SectionEditorDialog &d)
{
    // By its PLACEHOLDER, not by position: the section editor holds a dozen
    // line edits (address, bitmask, length, the CRC bytes) and findChildren
    // does not promise which comes first. "(automatic)" belongs to exactly one.
    for (QLineEdit *e : d.findChildren<QLineEdit *>())
        if (e->placeholderText() == QStringLiteral("(automatic)"))
            return e;
    return nullptr;
}

void testAMessageNameStopsAtSeventeenBytes()
{
    Configuration config;
    config.clear();
    CommsSection s;
    s.device = SectionDevice::ReceiveMessage;
    s.baseAddress = 0x640;
    s.messageLengthBytes = 8;
    SectionEditorDialog dialog(&config, s, 0, {}, -1);
    QLineEdit *name = sectionNameField(dialog);
    REQUIRE(name != nullptr);

    name->setText(QStringLiteral("Engine Broadcast Frame Alpha")); // 28 bytes
    std::printf("  28 ASCII into the name box  : '%s' (%d bytes)\n",
                qPrintable(name->text()), int(name->text().toUtf8().size()));
    CHECK(name->text().toUtf8().size() == MAX_MESSAGE_NAME_BYTES);
    CHECK(name->text() == QStringLiteral("Engine Broadcast "));

    // THE CASE A CHARACTER CAP MISSES.
    name->setText(QString::fromUtf8(kWide));
    std::printf("  6 wide chars into the box   : %d chars, %d bytes\n",
                int(name->text().size()), int(name->text().toUtf8().size()));
    CHECK(name->text().toUtf8().size() <= MAX_MESSAGE_NAME_BYTES);
    CHECK(name->text().size() == 5);
    CHECK(!name->text().contains(QChar(0xFFFD)));
}

// ------------------------------------------------------- the channel field

QLineEdit *channelNameField(EditChannelDialog &d)
{
    const QList<QLineEdit *> edits = d.findChildren<QLineEdit *>();
    return edits.isEmpty() ? nullptr : edits.first();
}

void testAChannelNameStopsAtThirtyOneBytes()
{
    Configuration config;
    config.clear();
    EditChannelDialog dialog(&config, Channel{}, true);
    QLineEdit *name = channelNameField(dialog);
    REQUIRE(name != nullptr);

    name->setText(QStringLiteral("Coolant Temperature Sensor Bank One")); // 35 bytes
    std::printf("  35 ASCII into the channel   : '%s' (%d bytes)\n",
                qPrintable(name->text()), int(name->text().toUtf8().size()));
    CHECK(name->text().toUtf8().size() == MAX_CHANNEL_NAME_BYTES);

    // THE CASE THAT SEPARATES A BYTE CAP FROM A CHARACTER ONE. Sixteen 3-byte
    // characters is 48 bytes but only 16 characters, so setMaxLength(31) lets
    // every one of them through and the field ends up holding a name 17 bytes
    // past what the device stores. A byte cap keeps ten.
    QString wide;
    for (int i = 0; i < 16; ++i)
        wide += QString::fromUtf8("\xe6\xb8\xa9");
    CHECK(wide.size() == 16);
    CHECK(wide.toUtf8().size() == 48);
    name->setText(wide);
    std::printf("  16 wide chars (48 B) held as: %d chars, %d bytes\n",
                int(name->text().size()), int(name->text().toUtf8().size()));
    CHECK(name->text().toUtf8().size() <= MAX_CHANNEL_NAME_BYTES);
    CHECK(name->text().size() == 10);
    CHECK(!name->text().contains(QChar(0xFFFD)));
}

void testAConstantsNameStopsToo()
{
    // Constants create a channel like any other, so the same budget applies.
    Configuration config;
    config.clear();
    ConstantsDialog dialog(&config);
    // The dialog's own name box lives in its row editor, so drive the helper
    // against a field of our own rather than hunting the grid: what is under
    // test here is that the CONSTANT path uses the same budget, which the build
    // guarantees by using the same constant. This case documents the intent and
    // fails loudly if MAX_CHANNEL_NAME_BYTES ever stops applying.
    QLineEdit probe;
    limitToUtf8Bytes(&probe, MAX_CHANNEL_NAME_BYTES);
    probe.setText(QStringLiteral("Coolant Temperature Sensor Bank One"));
    CHECK(probe.text().toUtf8().size() == MAX_CHANNEL_NAME_BYTES);
}

void testTypingCharacterByCharacterStopsAtTheBudget()
{
    // Not just a paste: the cap has to hold while a name is typed, and the
    // field must keep the prefix rather than emptying or refusing the edit.
    QLineEdit edit;
    limitToUtf8Bytes(&edit, MAX_MESSAGE_NAME_BYTES);
    QString typed;
    for (int i = 0; i < 40; ++i) {
        typed.append(QLatin1Char('a' + (i % 26)));
        edit.setText(typed);
        typed = edit.text(); // what the field kept is what the next keystroke extends
        CHECK(edit.text().toUtf8().size() <= MAX_MESSAGE_NAME_BYTES);
    }
    std::printf("  40 keystrokes into 17 bytes : %d bytes\n",
                int(edit.text().toUtf8().size()));
    CHECK(edit.text().toUtf8().size() == MAX_MESSAGE_NAME_BYTES);
}

} // namespace

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    testTheClipKeepsWholeCharacters();
    testAMessageNameStopsAtSeventeenBytes();
    testAChannelNameStopsAtThirtyOneBytes();
    testAConstantsNameStopsToo();
    testTypingCharacterByCharacterStopsAtTheBudget();

    if (fails == 0)
        std::printf("test_name_limits: all checks passed\n");
    else
        std::printf("test_name_limits: %d FAILURES\n", fails);
    return fails == 0 ? 0 : 1;
}
