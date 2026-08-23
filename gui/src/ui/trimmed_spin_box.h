// QDoubleSpinBox that hides unused decimal places: setDecimals() still sets
// the maximum precision accepted while editing, but the displayed text drops
// trailing zeros ("1.000000" → "1", "0.500000" → "0.5"). Drop-in replacement
// wherever a factor / offset / limit is shown.
//
// setTrimTrailingZeros(false) switches it back to fixed-width decimals, for the
// fields where the decimal count is meaningful rather than incidental — see the
// comment on the setter.
#pragma once

#include <QDoubleSpinBox>

namespace ct {

// Fixed-decimals text formatting with unused decimals dropped: the value is
// rounded to `decimals` places, then trailing zeros (and a bare decimal point)
// are removed — "1.000000" → "1", "0.500" → "0.5".
inline QString trimmedNumber(double value, int decimals)
{
    QString text = QString::number(value, 'f', decimals);
    if (text.contains(QLatin1Char('.'))) {
        while (text.endsWith(QLatin1Char('0')))
            text.chop(1);
        if (text.endsWith(QLatin1Char('.')))
            text.chop(1);
    }
    return text;
}

class TrimmedDoubleSpinBox : public QDoubleSpinBox
{
public:
    using QDoubleSpinBox::QDoubleSpinBox;

    // Trimming is right when setDecimals() only states the widest precision the
    // field will ACCEPT — a DBC factor allowed 8 places should read "1", not
    // "1.00000000". It is wrong when the decimal count is itself the
    // information: a channel defined with 2 decimal places wants "0.00", and
    // the padding is what says how precisely the value is held. Turn trimming
    // off for those, then call setDecimals(), which refreshes the displayed
    // text (setDecimals -> setRange -> setValue -> updateEdit).
    void setTrimTrailingZeros(bool trim)
    {
        if (m_trim == trim)
            return;
        m_trim = trim;
        setDecimals(decimals()); // re-render the current value under the new rule
    }
    bool trimsTrailingZeros() const { return m_trim; }

    QString textFromValue(double value) const override
    {
        QString text = QDoubleSpinBox::textFromValue(value);
        if (!m_trim)
            return text; // fixed width: 2 decimals always shows both
        const QString point = locale().decimalPoint();
        if (text.indexOf(point) < 0)
            return text;
        const QString zero = locale().zeroDigit();
        while (text.endsWith(zero))
            text.chop(zero.size());
        if (text.endsWith(point))
            text.chop(point.size());
        return text;
    }

private:
    bool m_trim = true;
};

} // namespace ct
