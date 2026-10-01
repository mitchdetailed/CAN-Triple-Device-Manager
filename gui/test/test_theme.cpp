// Tools → Theme (src/ui/theme.h). Every theme's palette is read the way a
// customer would read it: each text colour against each surface it is drawn
// on, held to WCAG's 4.5:1 for text; the colours with a meaning (error,
// warning, success, a note) against every surface of every theme and of a
// plain light and dark desktop; disabled text set back but still there. Then
// the switch itself: a theme is Fusion under its palette, applied to widgets
// that already exist; Windows Default puts back the style and palette the
// program started with; the choice is kept and read back, and a name this
// version does not know is Windows Default.

#include <QApplication>
#include <QDir>
#include <QLabel>
#include <QSettings>
#include <QStyle>
#include <QTemporaryDir>

#include <cstdio>

#include "../src/ui/theme.h"

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

const QList<Theme> kOwnPalettes = {Theme::Light, Theme::Dark, Theme::MidnightBlue,
                                   Theme::CarbonRed};

// One pair held to a floor, said when it falls short.
void atLeast(const char *theme, const char *what, const QColor &fg, const QColor &bg, double floor,
             double *lowest = nullptr)
{
    const double ratio = contrastRatio(fg, bg);
    if (lowest && ratio < *lowest)
        *lowest = ratio;
    if (ratio < floor) {
        std::printf("FAIL %s: %s %s on %s is %.2f:1, under %.1f\n", theme, what,
                    qPrintable(fg.name()), qPrintable(bg.name()), ratio, floor);
        ++fails;
    }
}

void testPalettes()
{
    std::printf("every theme's text reads on every surface it is drawn on\n");
    for (Theme t : kOwnPalettes) {
        const QPalette p = themePalette(t);
        const QByteArray name = themeName(t).toUtf8();
        const char *n = name.constData();
        double lowest = 21.0;
        const auto c = [&p](QPalette::ColorRole r) { return p.color(QPalette::Active, r); };
        atLeast(n, "text", c(QPalette::Text), c(QPalette::Base), 7.0, &lowest);
        atLeast(n, "text on alternate rows", c(QPalette::Text), c(QPalette::AlternateBase), 7.0,
                &lowest);
        atLeast(n, "window text", c(QPalette::WindowText), c(QPalette::Window), 7.0, &lowest);
        atLeast(n, "button text", c(QPalette::ButtonText), c(QPalette::Button), 7.0, &lowest);
        atLeast(n, "selected text", c(QPalette::HighlightedText), c(QPalette::Highlight), 4.5,
                &lowest);
        atLeast(n, "tooltip", c(QPalette::ToolTipText), c(QPalette::ToolTipBase), 7.0, &lowest);
        atLeast(n, "link", c(QPalette::Link), c(QPalette::Base), 4.5, &lowest);
        atLeast(n, "visited link", c(QPalette::LinkVisited), c(QPalette::Base), 4.5, &lowest);
        atLeast(n, "placeholder", c(QPalette::PlaceholderText), c(QPalette::Base), 4.5, &lowest);
        // The selection has to stand out from the list it is in.
        atLeast(n, "selection against the list", c(QPalette::Highlight), c(QPalette::Base), 1.5);
        // Disabled: set back, visibly, and still there.
        const QColor off = p.color(QPalette::Disabled, QPalette::Text);
        const double offRatio = contrastRatio(off, c(QPalette::Base));
        if (!(offRatio >= 2.0 && offRatio < contrastRatio(c(QPalette::Text), c(QPalette::Base)) - 2.0)) {
            std::printf("FAIL %s: disabled text %.2f:1 against %.2f:1 enabled\n", n, offRatio,
                        contrastRatio(c(QPalette::Text), c(QPalette::Base)));
            ++fails;
        }
        // Inactive windows look the same as active ones.
        CHECK(p.color(QPalette::Inactive, QPalette::Text) == c(QPalette::Text));
        CHECK(p.color(QPalette::Inactive, QPalette::Highlight) == c(QPalette::Highlight));
        std::printf("  %-14s lowest text contrast %.1f:1, disabled %.1f:1\n", n, lowest, offRatio);
    }
    // Four themes, four looks: no two share their window colour or accent.
    for (int i = 0; i < kOwnPalettes.size(); ++i)
        for (int j = i + 1; j < kOwnPalettes.size(); ++j) {
            const QPalette a = themePalette(kOwnPalettes[i]), b = themePalette(kOwnPalettes[j]);
            CHECK(a.color(QPalette::Window) != b.color(QPalette::Window));
            CHECK(a.color(QPalette::Highlight) != b.color(QPalette::Highlight)
                  || kOwnPalettes[i] == Theme::Light || kOwnPalettes[j] == Theme::Light);
        }
    CHECK(!isDarkPalette(themePalette(Theme::Light)));
    CHECK(isDarkPalette(themePalette(Theme::Dark)));
    CHECK(isDarkPalette(themePalette(Theme::MidnightBlue)));
    CHECK(isDarkPalette(themePalette(Theme::CarbonRed)));
    // Windows Default has no colours of its own.
    CHECK(themePalette(Theme::WindowsDefault).resolveMask() == 0);
}

QPalette plain(const QColor &window, const QColor &base, const QColor &text)
{
    QPalette p;
    p.setColor(QPalette::Window, window);
    p.setColor(QPalette::Base, base);
    p.setColor(QPalette::AlternateBase, base);
    p.setColor(QPalette::Text, text);
    return p;
}

void testMeaningfulColours()
{
    std::printf("error, warning, success and note colours read on every theme\n");
    QList<QPair<QByteArray, QPalette>> palettes;
    for (Theme t : kOwnPalettes)
        palettes.append({themeName(t).toUtf8(), themePalette(t)});
    // Windows Default on a light and on a dark desktop.
    palettes.append({"a light desktop", plain(QColor(0xF0, 0xF0, 0xF0), Qt::white, Qt::black)});
    palettes.append({"a dark desktop", plain(QColor(0x20, 0x20, 0x20), QColor(0x2D, 0x2D, 0x2D),
                                             Qt::white)});
    for (const auto &entry : palettes) {
        const QPalette &p = entry.second;
        const char *n = entry.first.constData();
        for (QPalette::ColorRole surface :
             {QPalette::Window, QPalette::Base, QPalette::AlternateBase}) {
            const QColor bg = p.color(surface);
            atLeast(n, "error", errorColor(p), bg, 4.5);
            atLeast(n, "warning", warningColor(p), bg, 4.5);
            atLeast(n, "success", okColor(p), bg, 4.5);
            atLeast(n, "note", mutedColor(p), bg, 4.5);
        }
        atLeast(n, "text on the success tint", p.color(QPalette::Text), okFill(p), 4.5);
        // And they mean different things, so they look different.
        CHECK(errorColor(p) != warningColor(p) && warningColor(p) != okColor(p));
    }

    // The dialogs' rich text: the light theme's red and amber, swapped.
    const QPalette dark = themePalette(Theme::Dark);
    const QString html = themedHtml(
        QStringLiteral("<span style='color:#c0392b'>x</span><div style='color:#b9770e'>y</div>"),
        dark);
    CHECK(!html.contains(QLatin1String("#c0392b")) && !html.contains(QLatin1String("#b9770e")));
    CHECK(html.contains(errorColor(dark).name()) && html.contains(warningColor(dark).name()));
    CHECK(colorRule(QColor(0x12, 0x34, 0x56)) == QLatin1String("color: #123456;"));
}

void testNames()
{
    std::printf("each theme has one key and one name; an unknown key is Windows Default\n");
    const QList<ThemeInfo> all = availableThemes();
    CHECK(all.size() == 5);
    CHECK(!all.isEmpty() && all.first().theme == Theme::WindowsDefault);
    QStringList keys, names;
    for (const ThemeInfo &t : all) {
        CHECK(themeFromKey(t.key) == t.theme);
        CHECK(themeKey(t.theme) == t.key);
        CHECK(themeName(t.theme) == t.name && !t.name.isEmpty());
        keys << t.key;
        names << t.name;
    }
    keys.removeDuplicates();
    names.removeDuplicates();
    CHECK(keys.size() == all.size() && names.size() == all.size());
    CHECK(themeFromKey(QString()) == Theme::WindowsDefault);
    CHECK(themeFromKey(QStringLiteral("solarized")) == Theme::WindowsDefault);
}

void testSwitching(const QString &startStyle, const QPalette &startPalette)
{
    std::printf("a theme applies at once, to windows already open; Windows Default puts "
                "everything back\n");
    QLabel open(QStringLiteral("already open"));
    for (Theme t : kOwnPalettes) {
        applyTheme(t);
        QCoreApplication::processEvents(); // the change reaches open windows as events
        CHECK(currentTheme() == t);
        CHECK(QApplication::style()->name().compare(QLatin1String("fusion"), Qt::CaseInsensitive)
              == 0);
        const QPalette want = themePalette(t);
        for (QPalette::ColorRole r : {QPalette::Window, QPalette::WindowText, QPalette::Base,
                                      QPalette::Text, QPalette::Button, QPalette::Highlight}) {
            CHECK(QApplication::palette().color(r) == want.color(r));
            CHECK(open.palette().color(r) == want.color(r)); // the label that was already there
        }
        QLabel fresh(QStringLiteral("opened after"));
        CHECK(fresh.palette().color(QPalette::Window) == want.color(QPalette::Window));
    }
    applyTheme(Theme::WindowsDefault);
    QCoreApplication::processEvents();
    CHECK(currentTheme() == Theme::WindowsDefault);
    CHECK(QApplication::style()->name().compare(startStyle, Qt::CaseInsensitive) == 0);
    for (QPalette::ColorRole r : {QPalette::Window, QPalette::WindowText, QPalette::Base,
                                  QPalette::Text, QPalette::Button, QPalette::Highlight}) {
        CHECK(QApplication::palette().color(r) == startPalette.color(r));
        CHECK(open.palette().color(r) == startPalette.color(r));
    }
}

void testKept()
{
    std::printf("the choice is kept for the account and read back\n");
    CHECK(savedTheme() == Theme::WindowsDefault); // nothing kept yet
    for (const ThemeInfo &t : availableThemes()) {
        saveTheme(t.theme);
        CHECK(savedTheme() == t.theme);
    }
    QSettings().setValue(QStringLiteral("appearance/theme"), QStringLiteral("a later theme"));
    CHECK(savedTheme() == Theme::WindowsDefault);
}

} // namespace

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    // The settings go to a directory of the test's own, not this account's.
    QTemporaryDir settingsDir;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("CAN Triple test"));
    QCoreApplication::setApplicationName(QStringLiteral("test_theme"));
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // What the program starts with, before any theme: Windows Default's target.
    const QString startStyle = QApplication::style()->name();
    const QPalette startPalette = QApplication::palette();

    testPalettes();
    testMeaningfulColours();
    testNames();
    testSwitching(startStyle, startPalette);
    testKept();

    if (fails == 0)
        std::printf("test_theme: all checks passed\n");
    else
        std::printf("test_theme: %d FAILURES\n", fails);
    return fails == 0 ? 0 : 1;
}
