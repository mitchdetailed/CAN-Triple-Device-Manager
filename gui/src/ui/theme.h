// Tools → Theme: the colours the whole program is drawn in.
//
// "Windows Default" is the program as it has always looked: the native style
// and whatever palette Windows gives it, light or dark as the desktop is set.
// The others are Qt's Fusion style under a palette of their own, the same on
// every Windows version and whatever the desktop is set to: Light, Dark,
// Midnight Blue and Carbon Red. A theme applies at once, to every window open
// and every one opened after, and the choice is kept for this Windows account
// (QSettings, like the recent-files list).
//
// Colours that MEAN something — an error, a warning, a success, a note set
// back from the text — are not in a palette, so every window asks for them
// here with the palette it is drawn in (errorColor and the rest), and gets one
// that can be read on it: 4.5:1 or better against Base and Window in every
// theme, which test_theme holds them to. Those are inline, so the dialogs that
// use them (and the test programs built from those dialogs) need nothing more
// than this header; switching and keeping a theme is theme.cpp, in the program.
#pragma once

#include <QColor>
#include <QList>
#include <QPalette>
#include <QString>

#include <algorithm>
#include <cmath>

namespace ct {

enum class Theme {
    WindowsDefault,
    Light,
    Dark,
    MidnightBlue,
    CarbonRed,
};

struct ThemeInfo {
    Theme theme;
    QString key;  // what QSettings keeps: never shown, never translated
    QString name; // what the menu shows
};

// In the order the menu lists them, Windows Default first.
QList<ThemeInfo> availableThemes();
QString themeKey(Theme theme);
QString themeName(Theme theme);
// An unknown or empty key is Windows Default: a setting from a later version
// that named a theme this one does not have falls back to the plain program.
Theme themeFromKey(const QString &key);

// The palette a theme draws with. Windows Default has none of its own: an
// empty QPalette, which leaves the platform's in place.
QPalette themePalette(Theme theme);

// Style and palette together, at once, for every window. The first call notes
// the native style so Windows Default can go back to it.
void applyTheme(Theme theme);
Theme currentTheme();

// The theme kept for this account, and keeping one.
Theme savedTheme();
void saveTheme(Theme theme);

// ---- colours with a meaning, for text on the palette's Base or Window ----

// Window lightness under 128: the test the dialogs have always used.
inline bool isDarkPalette(const QPalette &pal)
{
    return pal.color(QPalette::Window).lightness() < 128;
}

// Refused, failed, wrong.
inline QColor errorColor(const QPalette &pal)
{
    return isDarkPalette(pal) ? QColor(0xFF, 0x8A, 0x80) : QColor(0xB0, 0x00, 0x20);
}

// Allowed, but look at this.
inline QColor warningColor(const QPalette &pal)
{
    return isDarkPalette(pal) ? QColor(0xFF, 0xA1, 0x78) : QColor(0xC0, 0x30, 0x00);
}

// Done, fits, good.
inline QColor okColor(const QPalette &pal)
{
    // Darker than the access dialog's #1a7f37, which is 4.46:1 on the
    // Windows light window (#f0f0f0).
    return isDarkPalette(pal) ? QColor(0x7E, 0xE7, 0x87) : QColor(0x15, 0x72, 0x2F);
}

// A note set back from the text.
inline QColor mutedColor(const QPalette &pal)
{
    return isDarkPalette(pal) ? QColor(0x9A, 0xA0, 0xA6) : QColor(0x60, 0x64, 0x68);
}

// A row's background meaning "this one is different" in a good way (the
// channels a device script writes): a tint the palette's Text reads on.
inline QColor okFill(const QPalette &pal)
{
    return isDarkPalette(pal) ? QColor(0x1F, 0x3A, 0x26) : QColor(0xE8, 0xF5, 0xE9);
}

// "color: #rrggbb;" for a label's style sheet.
inline QString colorRule(const QColor &colour)
{
    return QStringLiteral("color: %1;").arg(colour.name());
}

// Rich text that spells its errors in #c0392b and its warnings in #b9770e
// (the light theme's red and amber, which the dialogs' strings have always
// used), with those swapped for the ones this palette reads. The strings keep
// one spelling; every theme gets a legible one.
inline QString themedHtml(QString html, const QPalette &pal)
{
    html.replace(QLatin1String("#c0392b"), errorColor(pal).name());
    html.replace(QLatin1String("#b9770e"), warningColor(pal).name());
    return html;
}

// The WCAG 2 contrast ratio of two colours, from 1 (the same) to 21 (black on
// white). 4.5 is the floor for text, 3 for large text and for the parts of a
// control that have to be seen.
inline double contrastRatio(const QColor &a, const QColor &b)
{
    const auto channel = [](int c8) {
        const double c = c8 / 255.0;
        return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
    };
    const auto luminance = [&channel](const QColor &c) {
        return 0.2126 * channel(c.red()) + 0.7152 * channel(c.green())
               + 0.0722 * channel(c.blue());
    };
    const double la = luminance(a), lb = luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

// The colour `t` of the way from `from` to `to` (0: from, 1: to).
inline QColor blendColor(const QColor &from, const QColor &to, double t)
{
    const auto mix = [t](int a, int b) { return int(a + (b - a) * t + 0.5); };
    return QColor(mix(from.red(), to.red()), mix(from.green(), to.green()),
                  mix(from.blue(), to.blue()));
}

// `colour` as it is when it reads at `floor` on `background`, and otherwise
// moved toward white (on a dark background) or black (on a light one) until
// it does. For colours that come from a palette nobody here chose: Windows'
// own dark palette gives links #0063b1 on #2d2d2d, 2.2:1.
inline QColor readableOn(QColor colour, const QColor &background, double floor)
{
    const QColor toward = background.lightness() < 128 ? QColor(0xFF, 0xFF, 0xFF)
                                                       : QColor(0x00, 0x00, 0x00);
    for (int step = 0; step < 40 && contrastRatio(colour, background) < floor; ++step)
        colour = blendColor(colour, toward, 0.1);
    return colour;
}

} // namespace ct
