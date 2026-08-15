// Shared building blocks for the numeric QTableWidget grids the app edits:
// lookup-table cells (Calculations > Tables) and axis-breakpoint rows (Axis
// Setup). Both want the same three things — a per-cell (min, max, decimals)
// editor, blank/value helpers, and spreadsheet copy/paste — so they live here
// rather than being written twice.
#pragma once

#include <QStyledItemDelegate>

#include <functional>

class QTableWidget;
class QTableWidgetItem;
class QKeyEvent;
class QColor;
class QPalette;

namespace ct {

// Fill for a grid's "header band" — the cells holding axis breakpoints rather
// than table data. Derived from the palette rather than fixed, so it follows the
// desktop's light or dark theme.
//
// It blends Base a little toward Text. That direction is self-adapting (a dark
// theme's Text is light, so the band lightens; a light theme's darkens) with no
// theme flag to get wrong, and unlike QColor::lighter() it still moves on a pure
// black Base — lighter() scales an HSV value of zero and returns black, which
// would leave the band invisible on a fully black theme.
QColor gridBandColor(const QPalette &pal);

// What a numeric grid cell accepts. editable=false marks a fixed label (a
// header or corner): the delegate refuses to open an editor on it and the
// clipboard skips it on paste.
struct CellSpec {
    double lo = -1e9;
    double hi = 1e9;
    int decimals = 3;
    bool editable = true;
};

// Read a cell as a number (0 if blank/missing) and test whether it is blank —
// partial tables leave unused sites empty rather than zero, so "blank" and
// "zero" are different states the callers must tell apart.
double cellValue(QTableWidget *t, int r, int c);
bool cellBlank(QTableWidget *t, int r, int c);

// A cell showing v, formatted and clamped to the spec; and an empty cell.
QTableWidgetItem *numItem(double v, const CellSpec &s);
QTableWidgetItem *blankItem();

// Constrains a grid cell to its (min, max, decimals) through a no-arrows double
// spin box. The spec function is supplied by the owner and reads whatever the
// current axis/output constraints are, so it re-derives per edit.
class GridDelegate : public QStyledItemDelegate
{
public:
    using SpecFn = std::function<CellSpec(int row, int col)>;
    // Which cells belong to the header band (axis breakpoints, corner label) and
    // so take gridBandColor(). Optional — without it every cell paints normally.
    using BandFn = std::function<bool(int row, int col)>;

    GridDelegate(SpecFn fn, QObject *parent);

    // A POSITIONAL rule, deliberately: the editors replace grid items outright
    // when an axis is applied or resized, so a background set on the items
    // themselves would silently vanish. A rule the delegate applies at paint
    // time cannot go stale that way.
    void setHeaderBand(BandFn fn);

    QWidget *createEditor(QWidget *parent, const QStyleOptionViewItem &,
                          const QModelIndex &idx) const override;
    void setEditorData(QWidget *editor, const QModelIndex &idx) const override;
    void setModelData(QWidget *editor, QAbstractItemModel *model,
                      const QModelIndex &idx) const override;

protected:
    void initStyleOption(QStyleOptionViewItem *option, const QModelIndex &idx) const override;

private:
    SpecFn m_fn;
    BandFn m_band;
};

// Spreadsheet-style clipboard for a numeric QTableWidget. Handles Ctrl+C /
// Ctrl+X / Ctrl+V and Delete/Backspace over the current selection, exchanging
// tab-between-columns, newline-between-rows text — the same shape Excel and
// Sheets use, so a block round-trips to and from a spreadsheet. Pasted values
// clamp to each destination cell's spec; non-editable cells are skipped, never
// overwritten. A single copied value fills a multi-cell selection.
//
// Not a QObject: the owning dialog already filters the grid's key events (for
// its own reasons) and forwards them here, so this needs no event machinery of
// its own — just handleKeyPress.
class GridClipboard
{
public:
    using SpecFn = std::function<CellSpec(int row, int col)>;
    // Called once after a paste (never per cell) so the owner can re-sort axes
    // and reformat — the writes happen with the grid's signals blocked, so its
    // itemChanged handlers do not fire mid-paste. Optional.
    using ChangedFn = std::function<void()>;

    GridClipboard(QTableWidget *grid, SpecFn spec, ChangedFn afterPaste = {});

    // True if ev was a clipboard/delete command and was handled (the caller
    // should then consume it); false to let the grid see the key.
    bool handleKeyPress(QKeyEvent *ev);

private:
    void copy(bool cut);
    void clearSelection();
    void paste();

    QTableWidget *m_grid;
    SpecFn m_spec;
    ChangedFn m_afterPaste;
};

} // namespace ct
