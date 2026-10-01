// The help in the theme (src/ui/help_style.h, HelpBrowser in help_window.cpp).
//
// First the stylesheet itself, the one the help pages all link: on a light
// palette it is served byte for byte as shipped; on every dark one each of its
// colours is swapped and none of the light ones is left, the text reads on
// every surface it is drawn on (the page, code, table headers, notes,
// warnings) at 7:1 and the links and the footer at 4.5:1, the borders can
// still be seen, and nothing but the colours changed.
//
// Then the real help window, on the compiled manual built next to this test:
// a page opens in the colours of the theme, a theme chosen while it is open
// repaints it where the reader was, and Windows Default puts back the page as
// it has always been.

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSet>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTextFrame>

#include <cstdio>

#include "../src/ui/help_style.h"
#include "../src/ui/help_window.h"
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

// Every colour in the stylesheet, in order, as written.
QStringList coloursIn(const QByteArray &css)
{
    QStringList out;
    static const QRegularExpression kHex(QStringLiteral("#[0-9a-fA-F]{6}\\b"));
    auto it = kHex.globalMatch(QString::fromUtf8(css));
    while (it.hasNext())
        out.append(it.next().captured().toLower());
    return out;
}

QByteArray withoutColours(const QByteArray &css)
{
    QString s = QString::fromUtf8(css);
    s.replace(QRegularExpression(QStringLiteral("#[0-9a-fA-F]{6}\\b")), QStringLiteral("#"));
    return s.toUtf8();
}

void atLeast(const char *palette, const char *what, const QColor &fg, const QColor &bg,
             double floor)
{
    const double ratio = contrastRatio(fg, bg);
    if (ratio < floor) {
        std::printf("FAIL %s: %s %s on %s is %.2f:1, under %.1f\n", palette, what,
                    qPrintable(fg.name()), qPrintable(bg.name()), ratio, floor);
        ++fails;
    }
}

QPalette plain(const QColor &window, const QColor &base, const QColor &text, const QColor &link)
{
    QPalette p;
    p.setColor(QPalette::Window, window);
    p.setColor(QPalette::Base, base);
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Link, link);
    return p;
}

void testStyleSheet(const QByteArray &shipped)
{
    std::printf("the help's stylesheet: as shipped when light, every colour the theme's when "
                "dark\n");
    const QStringList light = coloursIn(shipped);
    CHECK(QSet<QString>(light.begin(), light.end()).size() >= 10);

    // Light: exactly as it has always been.
    CHECK(themedHelpStyleSheet(shipped, themePalette(Theme::Light)) == shipped);
    CHECK(themedHelpStyleSheet(shipped, plain(QColor(0xF0, 0xF0, 0xF0), Qt::white, Qt::black,
                                              QColor(0x00, 0x66, 0xCC)))
          == shipped);

    QList<QPair<QByteArray, QPalette>> dark;
    for (Theme t : {Theme::Dark, Theme::MidnightBlue, Theme::CarbonRed})
        dark.append({themeName(t).toUtf8(), themePalette(t)});
    // Windows Default on a dark desktop: the palette Windows 11 hands Qt 6.7,
    // read off the real platform (links #0063b1: 2.2:1 on its own Base).
    dark.append({"Windows' dark palette", plain(QColor(0x1E, 0x1E, 0x1E), QColor(0x2D, 0x2D, 0x2D),
                                                Qt::white, QColor(0x00, 0x63, 0xB1))});
    for (const auto &entry : dark) {
        const QPalette &p = entry.second;
        const char *n = entry.first.constData();
        // Every colour the stylesheet uses is one the map knows...
        QSet<QString> known;
        QHash<QString, QColor> to;
        for (const auto &swap : helpColorMap(p)) {
            known.insert(swap.first);
            to.insert(swap.first, swap.second);
        }
        for (const QString &c : light)
            if (!known.contains(c)) {
                std::printf("FAIL help.css uses %s, which helpColorMap does not name\n",
                            qPrintable(c));
                ++fails;
            }
        // ...and each one, wherever it is written, became the map's.
        const QByteArray themed = themedHelpStyleSheet(shipped, p);
        const QStringList after = coloursIn(themed);
        CHECK(after.size() == light.size());
        for (int i = 0; i < light.size() && i < after.size(); ++i)
            CHECK(after[i] == to.value(light[i]).name());
        CHECK(withoutColours(themed) == withoutColours(shipped)); // only colours changed

        const QColor text = to.value(QStringLiteral("#24292f"));
        const QColor page = to.value(QStringLiteral("#ffffff"));
        const QColor link = to.value(QStringLiteral("#1a5a96"));
        const QColor note = to.value(QStringLiteral("#e8f1f8"));
        const QColor warn = to.value(QStringLiteral("#fcf3d7"));
        CHECK(text == p.color(QPalette::Text) && page == p.color(QPalette::Base));
        atLeast(n, "text on the page", text, page, 7.0);
        atLeast(n, "text in code", text, to.value(QStringLiteral("#f2f3f5")), 7.0);
        atLeast(n, "text in a table header", text, to.value(QStringLiteral("#e9ecef")), 7.0);
        atLeast(n, "text in a note", text, note, 7.0);
        atLeast(n, "text in a warning", text, warn, 7.0);
        atLeast(n, "a link", link, page, 4.5);
        atLeast(n, "a link in a note", link, note, 4.5);
        atLeast(n, "a link in a warning", link, warn, 4.5);
        atLeast(n, "the footer", to.value(QStringLiteral("#6e7781")), page, 4.5);
        atLeast(n, "a table's borders", to.value(QStringLiteral("#b9c0c7")), page, 1.4);
        atLeast(n, "a note's border", to.value(QStringLiteral("#c6dcee")), note, 1.4);
        atLeast(n, "a warning's border", to.value(QStringLiteral("#ecd9a0")), warn, 1.4);
        atLeast(n, "the rules", to.value(QStringLiteral("#d8dee4")), page, 1.3);
        // A note and a warning still look like two different things.
        CHECK(note != warn && note != page && warn != page);
    }
}

// QTextBrowser imports a linked stylesheet only when the link says it is one:
// type="text/css". Without it the rule is dropped in silence, and until
// 2026-09-28 every page's was, so the manual showed without its stylesheet
// (no table borders, no note or warning tints) in every theme.
void testPagesLinkTheStyleSheet(const QString &pagesDir)
{
    std::printf("every help page links the stylesheet the way the help browser reads it\n");
    const QStringList pages = QDir(pagesDir).entryList({QStringLiteral("*.html")}, QDir::Files);
    CHECK(pages.size() >= 20);
    static const QRegularExpression kLink(QStringLiteral("<link\\b[^>]*>"),
                                          QRegularExpression::CaseInsensitiveOption);
    for (const QString &name : pages) {
        QFile f(QDir(pagesDir).filePath(name));
        CHECK(f.open(QIODevice::ReadOnly));
        const QString html = QString::fromUtf8(f.readAll());
        bool linked = false;
        auto it = kLink.globalMatch(html);
        while (it.hasNext()) {
            const QString tag = it.next().captured();
            if (tag.contains(QLatin1String("help.css")))
                linked = tag.contains(QLatin1String("type=\"text/css\""));
        }
        if (!linked) {
            std::printf("FAIL %s does not link help.css with type=\"text/css\"\n",
                        qPrintable(name));
            ++fails;
        }
    }
}

// The page's background and the colour of its first words, as the browser
// has them.
QColor pageColour(QTextBrowser *b)
{
    return b->document()->rootFrame()->frameFormat().background().color();
}

QColor textColour(QTextBrowser *b)
{
    for (QTextBlock block = b->document()->begin(); block.isValid(); block = block.next()) {
        if (block.text().trimmed().isEmpty())
            continue;
        const QTextBlock::iterator it = block.begin();
        if (!it.atEnd())
            return it.fragment().charFormat().foreground().color();
    }
    return QColor();
}

void settle()
{
    for (int i = 0; i < 5; ++i)
        QCoreApplication::processEvents();
}

void testHelpWindow()
{
    std::printf("the help window: in the theme when opened, repainted where it was when the "
                "theme changes\n");
    HelpWindow window;
    window.resize(900, 650);
    window.show();
    window.showPage(QStringLiteral("communications.html"));
    settle();
    QTextBrowser *browser = window.findChild<QTextBrowser *>();
    CHECK(browser != nullptr);
    if (!browser)
        return;
    CHECK(!browser->source().isEmpty());
    if (browser->source().isEmpty()) {
        std::printf("  (no compiled help beside the test: nothing more to check)\n");
        return;
    }

    // As it has always been.
    CHECK(pageColour(browser) == QColor(0xFF, 0xFF, 0xFF));
    CHECK(textColour(browser) == QColor(0x24, 0x29, 0x2F));

    // Down the page, then a theme: the page in its colours, the reader where
    // they were.
    browser->verticalScrollBar()->setValue(browser->verticalScrollBar()->maximum() / 2);
    const int at = browser->verticalScrollBar()->value();
    CHECK(at > 0);
    for (Theme t : {Theme::Dark, Theme::MidnightBlue, Theme::CarbonRed}) {
        applyTheme(t);
        settle();
        const QPalette p = themePalette(t);
        CHECK(pageColour(browser) == p.color(QPalette::Base));
        CHECK(textColour(browser) == p.color(QPalette::Text));
        CHECK(browser->verticalScrollBar()->value() == at);
    }
    applyTheme(Theme::Light);
    settle();
    CHECK(pageColour(browser) == QColor(0xFF, 0xFF, 0xFF));
    CHECK(textColour(browser) == QColor(0x24, 0x29, 0x2F));

    // Another page, opened in a dark theme, comes up dark.
    applyTheme(Theme::CarbonRed);
    settle();
    window.showPage(QStringLiteral("index.html"));
    settle();
    CHECK(pageColour(browser) == themePalette(Theme::CarbonRed).color(QPalette::Base));

    applyTheme(Theme::WindowsDefault);
    settle();
    CHECK(pageColour(browser) == QColor(0xFF, 0xFF, 0xFF));
    CHECK(textColour(browser) == QColor(0x24, 0x29, 0x2F));
}

} // namespace

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    // The help window copies the manual into AppData under these names: the
    // test's own, not the program's.
    QCoreApplication::setOrganizationName(QStringLiteral("CAN Triple test"));
    QCoreApplication::setApplicationName(QStringLiteral("test_help_theme"));
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    QFile css(QStringLiteral(CT_HELP_CSS));
    CHECK(css.open(QIODevice::ReadOnly));
    const QByteArray shipped = css.readAll();
    CHECK(!shipped.isEmpty());

    testStyleSheet(shipped);
    testPagesLinkTheStyleSheet(QFileInfo(css.fileName()).absolutePath());
    testHelpWindow();

    if (fails == 0)
        std::printf("test_help_theme: all checks passed\n");
    else
        std::printf("test_help_theme: %d FAILURES\n", fails);
    return fails == 0 ? 0 : 1;
}
