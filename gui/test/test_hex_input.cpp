// A decimal field reads a 0x spelling.
//
// Drives the two hex-aware boxes and the trimmed box the editors actually use,
// offscreen, the way a person does it: text is set, the box is asked what it
// now holds, and editing is finished so the fixup path runs. The parse helpers
// behind the message-length field and the Builder's serial field are pinned
// the same way. Nothing here needs a device.
#include <QApplication>
#include <QLineEdit>
#include <cstdio>

#include "../src/ui/hex_input.h"
#include "../src/ui/trimmed_spin_box.h"

using namespace ct;

static int fails = 0;
#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);              \
            ++fails;                                                                 \
        }                                                                            \
    } while (0)

namespace {

// What a keystroke sequence leaves in the box: set the text, then end editing
// as a Tab would, so Intermediate text meets fixup() exactly as it does live.
template <class Box>
void typeAndLeave(Box *box, const QString &text)
{
    // lineEdit() is protected on QAbstractSpinBox; the editor is its child.
    box->template findChild<QLineEdit *>()->setText(text);
    box->interpretText();
}

void testParseHelpers()
{
    double v = 0;
    CHECK(parseHexPrefixed(QStringLiteral("0x1F"), &v) == HexParse::Value && v == 31);
    CHECK(parseHexPrefixed(QStringLiteral(" 0X1f "), &v) == HexParse::Value && v == 31);
    CHECK(parseHexPrefixed(QStringLiteral("-0x10"), &v) == HexParse::Value && v == -16);
    CHECK(parseHexPrefixed(QStringLiteral("+0x10"), &v) == HexParse::Value && v == 16);
    CHECK(parseHexPrefixed(QStringLiteral("0x"), &v) == HexParse::Incomplete);
    CHECK(parseHexPrefixed(QStringLiteral("-0x"), &v) == HexParse::Incomplete);
    CHECK(parseHexPrefixed(QStringLiteral("0xg"), &v) == HexParse::NotHex);
    CHECK(parseHexPrefixed(QStringLiteral("127"), &v) == HexParse::NotHex);
    CHECK(parseHexPrefixed(QStringLiteral("0"), &v) == HexParse::NotHex);
    CHECK(parseHexPrefixed(QString(), &v) == HexParse::NotHex);

    bool ok = false;
    CHECK(parseIntText(QStringLiteral("0x40"), &ok) == 64 && ok);
    CHECK(parseIntText(QStringLiteral("64"), &ok) == 64 && ok);
    CHECK(parseIntText(QStringLiteral("0x"), &ok) == 0 && !ok);
    CHECK(parseIntText(QStringLiteral("abc"), &ok) == 0 && !ok);

    // The unsigned parser never goes through a double: a 64-bit serial keeps
    // every bit.
    CHECK(parseUnsignedText(QStringLiteral("1001"), &ok) == 1001u && ok);
    CHECK(parseUnsignedText(QStringLiteral("0x3E9"), &ok) == 1001u && ok);
    CHECK(parseUnsignedText(QStringLiteral("0xFFFFFFFFFFFFFFFF"), &ok) == ~quint64(0) && ok);
    CHECK(parseUnsignedText(QStringLiteral("18446744073709551615"), &ok) == ~quint64(0) && ok);
    CHECK(parseUnsignedText(QStringLiteral("0x"), &ok) == 0 && !ok);
    CHECK(parseUnsignedText(QStringLiteral("-5"), &ok) == 0 && !ok);
}

void testIntBox()
{
    HexSpinBox box;
    box.setRange(0, 63);
    box.setSuffix(QStringLiteral(" bits"));

    // Keystroke by keystroke, the way the validator sees it: "0" is a value,
    // "0x" is allowed to exist while the rest is typed, "0x1" is a value again.
    int pos = 0;
    QString t = QStringLiteral("0 bits");
    CHECK(box.validate(t, pos) == QValidator::Acceptable);
    t = QStringLiteral("0x bits");
    CHECK(box.validate(t, pos) == QValidator::Intermediate);
    t = QStringLiteral("0x1 bits");
    CHECK(box.validate(t, pos) == QValidator::Acceptable);
    t = QStringLiteral("0xg bits");
    CHECK(box.validate(t, pos) == QValidator::Invalid); // the decimal rule refuses it

    typeAndLeave(&box, QStringLiteral("0x3F bits"));
    CHECK(box.value() == 63);
    CHECK(box.text() == QStringLiteral("63 bits")); // shown as decimal afterwards

    // Out of range lands on the limit, as a typed 999 would.
    typeAndLeave(&box, QStringLiteral("0x100 bits"));
    CHECK(box.value() == 63);

    // Decimal is untouched.
    typeAndLeave(&box, QStringLiteral("40 bits"));
    CHECK(box.value() == 40);
}

void testDoubleBoxes()
{
    HexDoubleSpinBox plain;
    plain.setRange(-1000, 1000);
    plain.setDecimals(2);
    typeAndLeave(&plain, QStringLiteral("0xFF"));
    CHECK(plain.value() == 255.0);
    CHECK(plain.text() == QStringLiteral("255.00")); // fixed decimals, as before
    typeAndLeave(&plain, QStringLiteral("-0x10"));
    CHECK(plain.value() == -16.0);

    TrimmedDoubleSpinBox trimmed;
    trimmed.setRange(-1e9, 1e9);
    trimmed.setDecimals(6);
    typeAndLeave(&trimmed, QStringLiteral("0x7F"));
    CHECK(trimmed.value() == 127.0);
    CHECK(trimmed.text() == QStringLiteral("127")); // and still trims
    typeAndLeave(&trimmed, QStringLiteral("0.5"));
    CHECK(trimmed.value() == 0.5);
    CHECK(trimmed.text() == QStringLiteral("0.5"));

    // Keyboard tracking: a complete hex spelling changes the value as it is
    // typed, without waiting for focus to leave.
    int changes = 0;
    QObject::connect(&trimmed, &QDoubleSpinBox::valueChanged, [&changes](double) { ++changes; });
    trimmed.findChild<QLineEdit *>()->setText(QStringLiteral("0x10"));
    CHECK(changes >= 1);
    CHECK(trimmed.value() == 16.0);
}

} // namespace

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    testParseHelpers();
    testIntBox();
    testDoubleBoxes();
    if (fails == 0)
        std::printf("test_hex_input: all checks passed\n");
    return fails == 0 ? 0 : 1;
}
