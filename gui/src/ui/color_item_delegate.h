// Item delegate that lets a row keep its own colour under hover and selection.
// By default a view paints the palette highlight over the item background, so a
// colour-coded row (green = running, red = off) reverts to plain grey the moment
// the mouse crosses it — exactly when the user is looking at it. This shades the
// row's own Qt::BackgroundRole instead, so the meaning survives every state.
//
// Rows with no BackgroundRole fall through to the stock delegate untouched, so
// this is safe on a view where only some rows are colour-coded. A row that IS
// colour-coded is drawn text-only: no icon, check box, or focus rect. That suits
// the two text-only views using it — Communications Setup's bus Mode combo, and
// the section editor's channel list, whose rows carry the colour their bits get
// in the frame layout map. Give it icons/check boxes before reusing it on a
// richer view.
#pragma once

#include <QAbstractItemDelegate>
#include <QApplication>
#include <QBrush>
#include <QColor>
#include <QModelIndex>
#include <QPainter>
#include <QPalette>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>

namespace ct {

class ColorItemDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    // Percent shift applied to a row's fill for hover and for selection. The
    // direction depends on the fill, not on a theme flag (see shade()), so one
    // pair of numbers serves both palettes. Selected is pushed further than
    // hover so "the row I am pointing at" stays distinguishable from "the row
    // that is active"; both are bounded so the row's own text stays above the
    // WCAG AA 4.5:1 threshold in every state.
    static constexpr int kHoverShift = 22;
    static constexpr int kSelectedShift = 40;

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        const QVariant background = index.data(Qt::BackgroundRole);
        if (!background.canConvert<QBrush>()) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }

        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);

        QColor fill = background.value<QBrush>().color();
        if (opt.state & QStyle::State_Selected)
            fill = shade(fill, kSelectedShift);
        else if (opt.state & QStyle::State_MouseOver)
            fill = shade(fill, kHoverShift);

        QColor text = opt.palette.color(QPalette::Text);
        const QVariant foreground = index.data(Qt::ForegroundRole);
        if (foreground.canConvert<QBrush>())
            text = foreground.value<QBrush>().color();

        // Match the stock text position exactly: SE_ItemViewItemText is only half
        // of it -- QCommonStylePrivate::viewItemDrawText then insets that rect by
        // PM_FocusFrameHMargin + 1. Without this, coloured rows sit 3px left of
        // every other item view on Windows.
        const QWidget *widget = opt.widget;
        QStyle *style = widget ? widget->style() : QApplication::style();
        const int inset = style->pixelMetric(QStyle::PM_FocusFrameHMargin, &opt, widget) + 1;
        const QRect textRect =
            style->subElementRect(QStyle::SE_ItemViewItemText, &opt, widget)
                .adjusted(inset, 0, -inset, 0);

        painter->save();
        painter->fillRect(opt.rect, fill);
        painter->setPen(text);
        painter->drawText(textRect, QStyle::visualAlignment(opt.direction, opt.displayAlignment),
                          opt.fontMetrics.elidedText(opt.text, opt.textElideMode,
                                                     textRect.width()));
        painter->restore();
    }

    // Shift a fill away from its own background: a dark fill (dark theme) gets
    // lighter, a light fill (light theme) gets darker. Darkening an already-dark
    // fill would push it into the surrounding chrome and lose the row entirely.
    static QColor shade(const QColor &fill, int percent)
    {
        return fill.lightness() < 128 ? fill.lighter(100 + percent)
                                      : fill.darker(100 + percent / 2);
    }
};

} // namespace ct
