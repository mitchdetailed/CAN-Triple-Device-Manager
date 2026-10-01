// The help pages in the theme (Tools → Theme).
//
// Every page links one stylesheet, help/pages/help.css, and nothing else in
// the pages sets a colour. The stylesheet is written for a white page. The
// help browser serves it through themedHelpStyleSheet: on a light palette
// (Windows Default on a light desktop, or Light) exactly as shipped, so the
// help looks as it always has; on a dark one (Dark, Midnight Blue, Carbon Red,
// or Windows Default on a dark desktop) with each of its colours swapped for
// one drawn from the palette, so the page is the theme's own surface and text,
// its links the theme's, and its notes and warnings tinted into it. Layout,
// fonts and spacing are untouched: only the colours change.
//
// helpColorMap names every colour help.css uses. A colour added to the
// stylesheet later must be added here too; test_help_theme fails until it is,
// rather than leave one light patch on a dark page.
#pragma once

#include <QByteArray>
#include <QColor>
#include <QList>
#include <QPair>
#include <QPalette>
#include <QRegularExpression>
#include <QString>

#include "theme.h"

namespace ct {

// help.css's colours, lower case, and what each becomes on a dark palette.
inline QList<QPair<QString, QColor>> helpColorMap(const QPalette &pal)
{
    const QColor page = pal.color(QPalette::Base);
    const QColor text = pal.color(QPalette::Text);
    // Notes and warnings keep their meaning in every theme: an information
    // blue and a caution amber, whatever the theme's own accent.
    const QColor info(0x3B, 0x82, 0xF6);
    const QColor caution(0xF5, 0x9E, 0x0B);
    const QColor note = blendColor(page, info, 0.16);
    const QColor warn = blendColor(page, caution, 0.14);
    // The theme's link colour, lifted where it would not read on the page, a
    // note or a warning (see readableOn): links sit in all three.
    const QColor link = readableOn(
        readableOn(readableOn(pal.color(QPalette::Link), page, 4.5), note, 4.5), warn, 4.5);
    return {
        {QStringLiteral("#24292f"), text},                          // body text
        {QStringLiteral("#ffffff"), page},                          // the page
        {QStringLiteral("#1a5a96"), link},                          // links
        {QStringLiteral("#f2f3f5"), blendColor(page, text, 0.07)},  // code and pre
        {QStringLiteral("#b9c0c7"), blendColor(page, text, 0.28)},  // table borders
        {QStringLiteral("#e9ecef"), blendColor(page, text, 0.11)},  // table header row
        {QStringLiteral("#d8dee4"), blendColor(page, text, 0.18)},  // rules: h2, footer
        {QStringLiteral("#e8f1f8"), note},                          // .note
        {QStringLiteral("#c6dcee"), blendColor(page, info, 0.42)},  // .note's border
        {QStringLiteral("#fcf3d7"), warn},                          // .warn
        {QStringLiteral("#ecd9a0"), blendColor(page, caution, 0.40)}, // .warn's border
        {QStringLiteral("#6e7781"), readableOn(mutedColor(pal), page, 4.5)}, // the footer
    };
}

// help.css as it is to be shown on `pal`. One pass over the colours, so no
// swapped-in colour can be swapped again; one the map does not name is left
// as it is.
inline QByteArray themedHelpStyleSheet(const QByteArray &shipped, const QPalette &pal)
{
    if (!isDarkPalette(pal))
        return shipped;
    const QList<QPair<QString, QColor>> map = helpColorMap(pal);
    const QString css = QString::fromUtf8(shipped);
    static const QRegularExpression kHex(QStringLiteral("#[0-9a-fA-F]{6}\\b"));
    QString out;
    out.reserve(css.size());
    qsizetype last = 0;
    auto it = kHex.globalMatch(css);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        out += QStringView(css).mid(last, m.capturedStart() - last);
        QString colour = m.captured();
        const QString key = colour.toLower();
        for (const auto &swap : map)
            if (swap.first == key) {
                colour = swap.second.name();
                break;
            }
        out += colour;
        last = m.capturedEnd();
    }
    out += QStringView(css).mid(last);
    return out.toUtf8();
}

} // namespace ct
