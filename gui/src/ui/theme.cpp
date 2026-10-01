#include "theme.h"

#include <QApplication>
#include <QCoreApplication>
#include <QSettings>
#include <QStyle>
#include <QStyleFactory>

namespace ct {

namespace {

const char kSettingsKey[] = "appearance/theme";

// Noted by the first applyTheme, before it changes anything: the style
// Windows default goes back to.
QString g_nativeStyle;
Theme g_current = Theme::WindowsDefault;

struct Colors {
    QColor window, windowText, base, alternateBase, text, button, buttonText;
    QColor highlight, highlightedText, link, linkVisited, placeholder;
    QColor light, midlight, mid, dark, shadow;
    QColor disabledText, disabledButton, disabledHighlight;
};

QPalette build(const Colors &c)
{
    QPalette p;
    for (QPalette::ColorGroup g : {QPalette::Active, QPalette::Inactive, QPalette::Disabled}) {
        p.setColor(g, QPalette::Window, c.window);
        p.setColor(g, QPalette::WindowText, c.windowText);
        p.setColor(g, QPalette::Base, c.base);
        p.setColor(g, QPalette::AlternateBase, c.alternateBase);
        p.setColor(g, QPalette::Text, c.text);
        p.setColor(g, QPalette::Button, c.button);
        p.setColor(g, QPalette::ButtonText, c.buttonText);
        p.setColor(g, QPalette::BrightText, QColor(0xFF, 0xFF, 0xFF));
        p.setColor(g, QPalette::Highlight, c.highlight);
        p.setColor(g, QPalette::HighlightedText, c.highlightedText);
        p.setColor(g, QPalette::Link, c.link);
        p.setColor(g, QPalette::LinkVisited, c.linkVisited);
        p.setColor(g, QPalette::PlaceholderText, c.placeholder);
        // A tooltip is the same surface as a field: Base and Text.
        p.setColor(g, QPalette::ToolTipBase, c.base);
        p.setColor(g, QPalette::ToolTipText, c.text);
        p.setColor(g, QPalette::Light, c.light);
        p.setColor(g, QPalette::Midlight, c.midlight);
        p.setColor(g, QPalette::Mid, c.mid);
        p.setColor(g, QPalette::Dark, c.dark);
        p.setColor(g, QPalette::Shadow, c.shadow);
        p.setColor(g, QPalette::Accent, c.highlight);
    }
    // Disabled: the text set back, the surfaces as they are, so a greyed-out
    // control still sits where it did.
    for (QPalette::ColorRole r : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText,
                                  QPalette::PlaceholderText, QPalette::HighlightedText})
        p.setColor(QPalette::Disabled, r, c.disabledText);
    p.setColor(QPalette::Disabled, QPalette::Button, c.disabledButton);
    p.setColor(QPalette::Disabled, QPalette::Highlight, c.disabledHighlight);
    p.setColor(QPalette::Disabled, QPalette::Accent, c.disabledHighlight);
    return p;
}

// Neutral light, blue accent: the program in daylight, the same on every
// Windows version.
const Colors kLight = {
    QColor(0xF3, 0xF4, 0xF6), QColor(0x1F, 0x23, 0x28), // window, windowText
    QColor(0xFF, 0xFF, 0xFF), QColor(0xF6, 0xF8, 0xFA), // base, alternateBase
    QColor(0x1F, 0x23, 0x28),                           // text
    QColor(0xF9, 0xFA, 0xFB), QColor(0x1F, 0x23, 0x28), // button, buttonText
    QColor(0x25, 0x63, 0xEB), QColor(0xFF, 0xFF, 0xFF), // highlight, highlightedText
    QColor(0x0B, 0x5C, 0xAD), QColor(0x6F, 0x42, 0xC1), // link, linkVisited
    QColor(0x6E, 0x77, 0x81),                           // placeholder
    QColor(0xFF, 0xFF, 0xFF), QColor(0xEA, 0xEC, 0xEF), // light, midlight
    QColor(0xC9, 0xCE, 0xD6), QColor(0x9A, 0xA1, 0xAB), // mid, dark
    QColor(0x6B, 0x72, 0x80),                           // shadow
    QColor(0x8C, 0x95, 0x9F), QColor(0xEF, 0xF1, 0xF3), // disabled text, button
    QColor(0xC9, 0xCE, 0xD6),                           // disabled highlight
};

// Graphite, blue accent.
const Colors kDark = {
    QColor(0x2B, 0x2D, 0x31), QColor(0xE3, 0xE5, 0xE8),
    QColor(0x1E, 0x1F, 0x22), QColor(0x26, 0x28, 0x2C),
    QColor(0xE3, 0xE5, 0xE8),
    QColor(0x38, 0x3A, 0x40), QColor(0xE3, 0xE5, 0xE8),
    QColor(0x25, 0x63, 0xEB), QColor(0xFF, 0xFF, 0xFF),
    QColor(0x7A, 0xB8, 0xFF), QColor(0xC4, 0xA3, 0xFF),
    QColor(0x8B, 0x90, 0x97),
    QColor(0x50, 0x53, 0x59), QColor(0x43, 0x46, 0x4C),
    QColor(0x20, 0x22, 0x25), QColor(0x15, 0x16, 0x18),
    QColor(0x0B, 0x0B, 0x0C),
    QColor(0x6D, 0x71, 0x78), QColor(0x30, 0x32, 0x37),
    QColor(0x3A, 0x3D, 0x42),
};

// Deep navy, cyan accent.
const Colors kMidnightBlue = {
    QColor(0x17, 0x20, 0x33), QColor(0xDC, 0xE6, 0xF2),
    QColor(0x0F, 0x17, 0x26), QColor(0x16, 0x21, 0x3A),
    QColor(0xDC, 0xE6, 0xF2),
    QColor(0x22, 0x30, 0x4C), QColor(0xDC, 0xE6, 0xF2),
    QColor(0x0E, 0x74, 0x90), QColor(0xFF, 0xFF, 0xFF),
    QColor(0x5C, 0xC8, 0xF0), QColor(0xB8, 0xA6, 0xF0),
    QColor(0x83, 0x95, 0xB0),
    QColor(0x33, 0x45, 0x6A), QColor(0x2A, 0x3A, 0x5A),
    QColor(0x12, 0x1B, 0x2E), QColor(0x0A, 0x10, 0x1C),
    QColor(0x04, 0x07, 0x0D),
    QColor(0x5D, 0x6B, 0x85), QColor(0x1B, 0x26, 0x3D),
    QColor(0x24, 0x35, 0x53),
};

// Carbon black, racing-red accent.
const Colors kCarbonRed = {
    QColor(0x1C, 0x1C, 0x1E), QColor(0xEC, 0xEC, 0xEC),
    QColor(0x12, 0x12, 0x13), QColor(0x1A, 0x1A, 0x1C),
    QColor(0xEC, 0xEC, 0xEC),
    QColor(0x2A, 0x2A, 0x2D), QColor(0xEC, 0xEC, 0xEC),
    QColor(0xA5, 0x1D, 0x2D), QColor(0xFF, 0xFF, 0xFF),
    QColor(0xFF, 0x7B, 0x7B), QColor(0xE6, 0xA8, 0xA8),
    QColor(0x8E, 0x8E, 0x93),
    QColor(0x41, 0x41, 0x45), QColor(0x35, 0x35, 0x39),
    QColor(0x16, 0x16, 0x18), QColor(0x0B, 0x0B, 0x0C),
    QColor(0x00, 0x00, 0x00),
    QColor(0x69, 0x69, 0x6D), QColor(0x23, 0x23, 0x25),
    QColor(0x4A, 0x2A, 0x2E),
};

} // namespace

QList<ThemeInfo> availableThemes()
{
    return {
        {Theme::WindowsDefault, QStringLiteral("windows"),
         QCoreApplication::translate("ct::Theme", "Windows Default")},
        {Theme::Light, QStringLiteral("light"), QCoreApplication::translate("ct::Theme", "Light")},
        {Theme::Dark, QStringLiteral("dark"), QCoreApplication::translate("ct::Theme", "Dark")},
        {Theme::MidnightBlue, QStringLiteral("midnight-blue"),
         QCoreApplication::translate("ct::Theme", "Midnight Blue")},
        {Theme::CarbonRed, QStringLiteral("carbon-red"),
         QCoreApplication::translate("ct::Theme", "Carbon Red")},
    };
}

QString themeKey(Theme theme)
{
    for (const ThemeInfo &t : availableThemes())
        if (t.theme == theme)
            return t.key;
    return QStringLiteral("windows");
}

QString themeName(Theme theme)
{
    for (const ThemeInfo &t : availableThemes())
        if (t.theme == theme)
            return t.name;
    return QString();
}

Theme themeFromKey(const QString &key)
{
    for (const ThemeInfo &t : availableThemes())
        if (t.key == key)
            return t.theme;
    return Theme::WindowsDefault;
}

QPalette themePalette(Theme theme)
{
    switch (theme) {
    case Theme::Light:
        return build(kLight);
    case Theme::Dark:
        return build(kDark);
    case Theme::MidnightBlue:
        return build(kMidnightBlue);
    case Theme::CarbonRed:
        return build(kCarbonRed);
    case Theme::WindowsDefault:
        break;
    }
    return QPalette();
}

void applyTheme(Theme theme)
{
    if (g_nativeStyle.isEmpty() && QApplication::style())
        g_nativeStyle = QApplication::style()->name();
    // The style before the palette: setting a style puts the palette back to
    // the style's own, so the other order would undo the theme at once.
    const QString wanted =
        theme == Theme::WindowsDefault ? g_nativeStyle : QStringLiteral("fusion");
    if (!wanted.isEmpty() && QApplication::style()
        && QApplication::style()->name().compare(wanted, Qt::CaseInsensitive) != 0) {
        if (QStyle *style = QStyleFactory::create(wanted))
            QApplication::setStyle(style);
    }
    // An empty palette sets no colour of its own, which is how Windows
    // default goes back to the platform's palette, light or dark.
    QApplication::setPalette(themePalette(theme));
    g_current = theme;
}

Theme currentTheme()
{
    return g_current;
}

Theme savedTheme()
{
    return themeFromKey(QSettings().value(QLatin1String(kSettingsKey)).toString());
}

void saveTheme(Theme theme)
{
    QSettings().setValue(QLatin1String(kSettingsKey), themeKey(theme));
}

} // namespace ct
