// "0x" in a decimal field.
//
// Every number the editors take is typed into a decimal box — a start bit, a
// bit length, a constant, a timeout, a table cell — and a CAN engineer holding
// a DBC or a capture has most of those numbers in hex. Retyping 0x7F as 127 is
// the same transcription nobody gets right first time that the Copy buttons
// exist to avoid, so the boxes accept the hex spelling as well: anything that
// starts with 0x (or -0x) reads as hexadecimal and is stored, displayed and
// stepped as the ordinary decimal value it always was. The field is not a hex
// field afterwards; it merely understood a hex spelling on the way in.
//
// Two things are deliberately NOT here. Fields that are hex BY DEFINITION — a
// CAN identifier, an identifier mask, the CRC parameters — keep their own hex
// validators and are not routed through this: "0x" there would be a spelling
// of a spelling. And the Compound Message Identifier window is left exactly as
// it was, on request.
//
// The mixin is a template over the two Qt spin boxes because the three
// overrides are the same code twice otherwise, differing only in the value
// type valueFromText() returns. No Q_OBJECT: nothing is added that moc would
// need to see, and qobject_cast<QDoubleSpinBox *>() on an instance keeps
// working because the meta-object it finds is the Qt base's.
#pragma once

#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QString>
#include <QValidator>
#include <QtGlobal>

#include <climits>

namespace ct {

enum class HexParse {
    NotHex,     // no 0x prefix after the optional sign, or a non-hex digit after one
    Incomplete, // "0x" / "-0x" with nothing after it yet: legal to keep typing
    Value       // parsed; *out holds it
};

// Recognise "0x1F", "-0x1f", "+0X10". Surrounding whitespace is ignored. A
// prefix followed by a digit that is not hex reports NotHex rather than
// Incomplete, so the caller's ordinary decimal rule gets to refuse it — "0xg"
// must not be a state a field can sit in.
inline HexParse parseHexPrefixed(QString text, double *out)
{
    text = text.trimmed();
    bool negative = false;
    if (text.startsWith(QLatin1Char('-'))) {
        negative = true;
        text.remove(0, 1);
    } else if (text.startsWith(QLatin1Char('+'))) {
        text.remove(0, 1);
    }
    if (text.size() < 2 || text.at(0) != QLatin1Char('0')
        || (text.at(1) != QLatin1Char('x') && text.at(1) != QLatin1Char('X')))
        return HexParse::NotHex;
    const QString digits = text.mid(2);
    if (digits.isEmpty())
        return HexParse::Incomplete;
    bool ok = false;
    const qulonglong v = digits.toULongLong(&ok, 16);
    if (!ok)
        return HexParse::NotHex;
    if (out)
        *out = negative ? -double(v) : double(v);
    return HexParse::Value;
}

// Decimal or 0x text to an int, for the QLineEdit fields that hold a count
// rather than a spin box (the message length). `ok` follows QString::toInt().
inline int parseIntText(const QString &text, bool *ok)
{
    double v = 0;
    switch (parseHexPrefixed(text, &v)) {
    case HexParse::Value:
        if (ok)
            *ok = v >= double(INT_MIN) && v <= double(INT_MAX);
        return (v >= double(INT_MIN) && v <= double(INT_MAX)) ? int(v) : 0;
    case HexParse::Incomplete:
        if (ok)
            *ok = false;
        return 0;
    case HexParse::NotHex:
        break;
    }
    return text.trimmed().toInt(ok);
}

// Decimal or 0x text to an unsigned 64-bit value, parsed WITHOUT passing
// through a double so a serial number above 2^53 survives intact. No sign.
inline quint64 parseUnsignedText(QString text, bool *ok)
{
    text = text.trimmed();
    if (text.size() >= 2 && text.at(0) == QLatin1Char('0')
        && (text.at(1) == QLatin1Char('x') || text.at(1) == QLatin1Char('X')))
        return text.mid(2).toULongLong(ok, 16); // "" -> ok = false, like toULongLong
    return text.toULongLong(ok, 10);
}

// The mixin. `Base` is QSpinBox or QDoubleSpinBox; `ValueT` is what its
// valueFromText() returns.
template <class Base, class ValueT>
class HexAware : public Base
{
public:
    using Base::Base;

    // Called on every keystroke with the WHOLE text, prefix and suffix
    // included — " Hz" and the like are stripped before the number is looked
    // at, the same way Qt's own validator strips them. A hex value outside the
    // range is Intermediate rather than Invalid so the digits can still be
    // typed; fixup() below is what lands it in range when editing ends.
    QValidator::State validate(QString &input, int &pos) const override
    {
        double v = 0;
        switch (parseHexPrefixed(core(input), &v)) {
        case HexParse::Incomplete:
            return QValidator::Intermediate;
        case HexParse::Value:
            return (v >= double(this->minimum()) && v <= double(this->maximum()))
                       ? QValidator::Acceptable
                       : QValidator::Intermediate;
        case HexParse::NotHex:
            break;
        }
        return Base::validate(input, pos);
    }

    // Editing ended on a hex spelling that is out of range: rewrite it as the
    // nearest value the field can hold, in the decimal the field will show
    // anyway. Qt then re-validates and accepts it instead of reverting to the
    // previous value, which is what a clamp-on-exit field does for decimal.
    void fixup(QString &input) const override
    {
        double v = 0;
        if (parseHexPrefixed(core(input), &v) == HexParse::Value) {
            input = this->prefix() + this->textFromValue(clamp(v)) + this->suffix();
            return;
        }
        Base::fixup(input);
    }

    ValueT valueFromText(const QString &text) const override
    {
        double v = 0;
        if (parseHexPrefixed(core(text), &v) == HexParse::Value)
            return clamp(v);
        return Base::valueFromText(text);
    }

private:
    QString core(QString text) const
    {
        const QString p = this->prefix();
        const QString s = this->suffix();
        if (!p.isEmpty() && text.startsWith(p))
            text.remove(0, p.size());
        if (!s.isEmpty() && text.endsWith(s))
            text.chop(s.size());
        return text;
    }
    ValueT clamp(double v) const
    {
        return ValueT(qBound(double(this->minimum()), v, double(this->maximum())));
    }
};

// Drop-in replacements for the two Qt boxes wherever a field reads decimal.
using HexSpinBox = HexAware<QSpinBox, int>;
using HexDoubleSpinBox = HexAware<QDoubleSpinBox, double>;

} // namespace ct
