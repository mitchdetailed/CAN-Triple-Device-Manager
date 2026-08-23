#include "numeric_grid.h"

#include <QAbstractItemModel>
#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QDoubleSpinBox>
#include <QKeyEvent>
#include <QModelIndex>
#include <QPalette>
#include <QStyleOptionViewItem>
#include <QTableWidget>
#include <QTableWidgetItem>

#include <algorithm>
#include <limits>

#include "trimmed_spin_box.h"

namespace ct {

double cellValue(QTableWidget *t, int r, int c)
{
    QTableWidgetItem *item = t->item(r, c);
    return item ? item->text().toDouble() : 0.0;
}

bool cellBlank(QTableWidget *t, int r, int c)
{
    QTableWidgetItem *item = t->item(r, c);
    return !item || item->text().trimmed().isEmpty();
}

QTableWidgetItem *numItem(double v, const CellSpec &s)
{
    return new QTableWidgetItem(trimmedNumber(qBound(s.lo, v, s.hi), s.decimals));
}

QTableWidgetItem *blankItem()
{
    return new QTableWidgetItem(QString());
}

QColor gridBandColor(const QPalette &pal)
{
    const QColor base = pal.color(QPalette::Base);
    const QColor text = pal.color(QPalette::Text);
    // 12%: far enough from Base to read as its own band at a glance, and still
    // far enough from Text that the values sitting on it keep their contrast.
    constexpr double kToward = 0.12;
    const auto mix = [](double a, double b) { return a + (b - a) * kToward; };
    return QColor::fromRgbF(mix(base.redF(), text.redF()), mix(base.greenF(), text.greenF()),
                            mix(base.blueF(), text.blueF()));
}

// ---------------------------------------------------------------- GridDelegate
GridDelegate::GridDelegate(SpecFn fn, QObject *parent)
    : QStyledItemDelegate(parent), m_fn(std::move(fn))
{
}

void GridDelegate::setHeaderBand(BandFn fn)
{
    m_band = std::move(fn);
}

// Setting backgroundBrush rather than painting the fill ourselves keeps the
// style in charge: QCommonStyle paints this brush only when the cell is NOT
// selected, so the selection highlight still wins and the band never hides which
// cell is current.
void GridDelegate::initStyleOption(QStyleOptionViewItem *option, const QModelIndex &idx) const
{
    QStyledItemDelegate::initStyleOption(option, idx);
    if (m_band && m_band(idx.row(), idx.column()))
        option->backgroundBrush = gridBandColor(option->palette);
}

QWidget *GridDelegate::createEditor(QWidget *parent, const QStyleOptionViewItem &,
                                    const QModelIndex &idx) const
{
    const CellSpec s = m_fn(idx.row(), idx.column());
    if (!s.editable)
        return nullptr;
    auto *sb = new TrimmedDoubleSpinBox(parent);
    sb->setDecimals(s.decimals);
    sb->setRange(s.lo, s.hi);
    sb->setButtonSymbols(QAbstractSpinBox::NoButtons); // no up/down arrows
    return sb;
}

void GridDelegate::setEditorData(QWidget *editor, const QModelIndex &idx) const
{
    if (auto *sb = qobject_cast<QDoubleSpinBox *>(editor))
        sb->setValue(idx.data(Qt::DisplayRole).toDouble());
}

void GridDelegate::setModelData(QWidget *editor, QAbstractItemModel *model,
                                const QModelIndex &idx) const
{
    auto *sb = qobject_cast<QDoubleSpinBox *>(editor);
    if (!sb)
        return;
    sb->interpretText();
    const CellSpec s = m_fn(idx.row(), idx.column());
    model->setData(idx, trimmedNumber(sb->value(), s.decimals), Qt::DisplayRole);
}

// --------------------------------------------------------------- GridClipboard
GridClipboard::GridClipboard(QTableWidget *grid, SpecFn spec, ChangedFn afterPaste)
    : m_grid(grid), m_spec(std::move(spec)), m_afterPaste(std::move(afterPaste))
{
}

bool GridClipboard::handleKeyPress(QKeyEvent *ev)
{
    if (ev->matches(QKeySequence::Copy)) {
        copy(false);
        return true;
    }
    if (ev->matches(QKeySequence::Cut)) {
        copy(true);
        return true;
    }
    if (ev->matches(QKeySequence::Paste)) {
        paste();
        return true;
    }
    if (ev->key() == Qt::Key_Delete || ev->key() == Qt::Key_Backspace) {
        clearSelection();
        return true;
    }
    return false;
}

// The selection's bounding rectangle, TSV. A cell inside the rectangle but not
// itself selected contributes a blank field, so an L-shaped selection copies as
// a rectangle with holes — the same thing a spreadsheet does.
void GridClipboard::copy(bool cut)
{
    const QList<QTableWidgetSelectionRange> ranges = m_grid->selectedRanges();
    if (ranges.isEmpty())
        return;
    int top = std::numeric_limits<int>::max(), left = std::numeric_limits<int>::max();
    int bottom = -1, right = -1;
    for (const QTableWidgetSelectionRange &r : ranges) {
        top = std::min(top, r.topRow());
        left = std::min(left, r.leftColumn());
        bottom = std::max(bottom, r.bottomRow());
        right = std::max(right, r.rightColumn());
    }
    QString out;
    for (int row = top; row <= bottom; ++row) {
        for (int col = left; col <= right; ++col) {
            QTableWidgetItem *it = m_grid->item(row, col);
            if (it && it->isSelected())
                out += it->text();
            if (col < right)
                out += QLatin1Char('\t');
        }
        if (row < bottom)
            out += QLatin1Char('\n');
    }
    QApplication::clipboard()->setText(out);
    if (cut)
        clearSelection();
}

void GridClipboard::clearSelection()
{
    for (QTableWidgetItem *it : m_grid->selectedItems())
        if (m_spec(it->row(), it->column()).editable)
            it->setText(QString());
}

void GridClipboard::paste()
{
    QString text = QApplication::clipboard()->text();
    if (text.isEmpty())
        return;
    text.replace(QLatin1String("\r\n"), QLatin1String("\n"));
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    // A trailing newline (Excel appends one) would otherwise read as an extra
    // blank row and clear a row of cells below the paste.
    while (text.endsWith(QLatin1Char('\n')))
        text.chop(1);

    QList<QStringList> cells;
    int width = 0;
    for (const QString &line : text.split(QLatin1Char('\n'))) {
        const QStringList cols = line.split(QLatin1Char('\t'));
        width = std::max(width, int(cols.size()));
        cells.append(cols);
    }
    if (cells.isEmpty())
        return;

    // Anchor at the selection's top-left, or the current cell if nothing is
    // selected.
    int anchorRow = std::max(0, m_grid->currentRow());
    int anchorCol = std::max(0, m_grid->currentColumn());
    const QList<QTableWidgetSelectionRange> ranges = m_grid->selectedRanges();
    if (!ranges.isEmpty()) {
        anchorRow = ranges.first().topRow();
        anchorCol = ranges.first().leftColumn();
        for (const QTableWidgetSelectionRange &r : ranges) {
            anchorRow = std::min(anchorRow, r.topRow());
            anchorCol = std::min(anchorCol, r.leftColumn());
        }
    }

    // Writing the pasted block with signals blocked so per-cell itemChanged
    // handlers (axis re-sort) do not fire mid-paste; the owner re-sorts once
    // afterward via m_afterPaste.
    const QSignalBlocker block(m_grid);

    const bool singleSource = cells.size() == 1 && width == 1;
    int selectedCount = 0;
    for (const QTableWidgetSelectionRange &r : ranges)
        selectedCount += (r.bottomRow() - r.topRow() + 1) * (r.rightColumn() - r.leftColumn() + 1);

    const auto writeCell = [&](int r, int c, const QString &token) {
        if (r < 0 || r >= m_grid->rowCount() || c < 0 || c >= m_grid->columnCount())
            return;
        const CellSpec s = m_spec(r, c);
        if (!s.editable)
            return;
        QTableWidgetItem *it = m_grid->item(r, c);
        if (!it) {
            it = new QTableWidgetItem;
            m_grid->setItem(r, c, it);
        }
        const QString t = token.trimmed();
        if (t.isEmpty()) {
            it->setText(QString());
            return;
        }
        bool ok = false;
        const double v = t.toDouble(&ok);
        if (!ok)
            return; // leave non-numeric tokens alone rather than zeroing the cell
        it->setText(trimmedNumber(qBound(s.lo, v, s.hi), s.decimals));
    };

    if (singleSource && selectedCount > 1) {
        // One value dropped onto a block fills the block.
        const QString token = cells.first().value(0);
        for (QTableWidgetItem *it : m_grid->selectedItems())
            writeCell(it->row(), it->column(), token);
    } else {
        for (int i = 0; i < cells.size(); ++i)
            for (int j = 0; j < cells.at(i).size(); ++j)
                writeCell(anchorRow + i, anchorCol + j, cells.at(i).at(j));
    }

    if (m_afterPaste)
        m_afterPaste();
}

} // namespace ct
