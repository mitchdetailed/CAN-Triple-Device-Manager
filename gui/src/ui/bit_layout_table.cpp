#include "bit_layout_table.h"

#include <QHeaderView>
#include <QPalette>
#include <QStringList>
#include <QVariant>

#include "../model/device_mapper.h"

namespace ct {

namespace {

bool darkUi(const QPalette &pal)
{
    return pal.color(QPalette::Base).lightness() < 128;
}

// Channel name -> "Coolant Temp °C". The map is handed CommsChannelRows, which
// carry a channel's NAME and nothing else, and it has no route to the document's
// catalogue — so whoever owns the document leaves the labels here as a dynamic
// property (SectionEditorDialog::refreshBitTable does, before every setFrame).
// The key must match kChannelLabelsProperty in section_editor_dialog.cpp.
const char *const kChannelLabelsProperty = "ct_channel_labels";

// The name to SHOW for a channel: its label when one was supplied, the bare name
// otherwise, so a caller that supplies nothing gets what this map always drew.
// Only ever used to build a tooltip or the caption — never to look a channel up,
// and never stored: m_rows keeps the identity.
QString displayLabel(const QObject *table, const QString &name)
{
    const QVariantMap labels = table->property(kChannelLabelsProperty).toMap();
    const auto it = labels.constFind(name);
    if (it == labels.constEnd())
        return name;
    const QString label = it->toString();
    return label.isEmpty() ? name : label;
}

} // namespace

// Twelve hues spread evenly over 30°..316° and then ORDERED so that
// consecutive signals land far apart on the wheel — signals that touch in the
// frame are usually adjacent in the list, and two neighbouring blocks in
// similar colours is exactly the confusion this map exists to remove.
//
// Red is deliberately absent. Red means "two signals claim this bit" here, and
// a signal that happened to be drawn red would look like a fault; that is not a
// hypothetical, it is what the first cut of this palette did.
QColor BitLayoutTable::signalColour(int index, const QPalette &pal, bool selected)
{
    static const int kHues[] = {212, 82, 316, 134, 30, 264, 186, 56, 238, 108, 290, 160};
    const int hue = kHues[qAbs(index) % 12];
    const bool dark = darkUi(pal);
    if (selected)
        // The picked signal: its own hue at full strength. "A different colour"
        // that still says WHICH signal it is.
        return QColor::fromHsl(hue, 255, dark ? 118 : 130);
    return dark ? QColor::fromHsl(hue, 110, 58) : QColor::fromHsl(hue, 165, 208);
}

QColor BitLayoutTable::signalTextColour(const QColor &fill)
{
    return fill.lightness() < 140 ? QColor(0xF5, 0xF7, 0xFA) : QColor(0x10, 0x14, 0x18);
}

BitLayoutTable::BitLayoutTable(QWidget *parent)
    : QTableWidget(parent)
{
    setColumnCount(8);
    QStringList heads;
    for (int b = 7; b >= 0; --b)
        heads << QString::number(b);
    setHorizontalHeaderLabels(heads);
    horizontalHeader()->setToolTip(
        tr("Bit within the byte: 7 is the most significant, 0 the least. The number in each "
           "cell is the global bit index — the value a channel's Start Bit field takes."));
    horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    verticalHeader()->setDefaultSectionSize(20);

    // The highlight is painted from the signal colours, so the view's own
    // selection would only fight it — and there is nothing here to select.
    setSelectionMode(QAbstractItemView::NoSelection);
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    setFocusPolicy(Qt::NoFocus);
    setShowGrid(true);
    setWordWrap(false);
    // Scrolls rather than demanding room for every byte: a 64-row CAN FD frame
    // must not set the dialog's minimum height.
    setSizeAdjustPolicy(QAbstractScrollArea::AdjustIgnored);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    connect(this, &QTableWidget::cellClicked, this, [this](int row, int column) {
        const QList<int> owners = m_owners.value(bitAt(row, column));
        if (!owners.isEmpty())
            emit rowClicked(owners.first());
    });
}

void BitLayoutTable::setFrame(const QList<CommsChannelRow> &rows, SectionAlignment alignment,
                              int byteCount, int usableBytes)
{
    m_rows = rows;
    m_alignment = alignment;
    m_byteCount = qBound(1, byteCount, 64);
    m_usableBytes = qBound(0, usableBytes, m_byteCount);
    if (m_selected >= m_rows.size())
        m_selected = -1;
    rebuild();
}

void BitLayoutTable::setSelectedRow(int index)
{
    const int wanted = (index >= 0 && index < m_rows.size()) ? index : -1;
    if (wanted == m_selected)
        return;
    m_selected = wanted;
    applyColours();
}

void BitLayoutTable::setCrcByte(int byteIndex)
{
    // Clamped against the 64-byte ceiling rather than m_byteCount: the CRC byte
    // usually arrives BEFORE setFrame() on a refresh (the caller sets both from
    // the same widgets), and clamping against the old frame would throw away a
    // location the very next call is about to make room for.
    const int wanted = (byteIndex >= 0 && byteIndex < 64) ? byteIndex : -1;
    if (wanted == m_crcByte)
        return;
    m_crcByte = wanted;
    applyColours();
}

void BitLayoutTable::rebuild()
{
    m_owners.clear();
    for (int i = 0; i < m_rows.size(); ++i)
        for (int pos : rowBitPositions(m_rows[i], m_alignment))
            m_owners[pos].append(i);

    setRowCount(m_byteCount);
    QStringList byteLabels;
    byteLabels.reserve(m_byteCount);
    for (int b = 0; b < m_byteCount; ++b)
        byteLabels << tr("Byte %1").arg(b);
    setVerticalHeaderLabels(byteLabels);

    for (int b = 0; b < m_byteCount; ++b) {
        for (int c = 0; c < 8; ++c) {
            QTableWidgetItem *cell = item(b, c);
            if (!cell) {
                cell = new QTableWidgetItem;
                cell->setTextAlignment(Qt::AlignCenter);
                setItem(b, c, cell);
            }
            cell->setText(QString::number(bitAt(b, c)));
        }
    }
    applyColours();
}

void BitLayoutTable::applyColours()
{
    const bool dark = darkUi(palette());
    const QColor freeFill = palette().color(QPalette::Base);
    const QColor freeText = dark ? QColor(0x70, 0x76, 0x7C) : QColor(0xA6, 0xAB, 0xB0);
    // Bytes past the message length: drawn so the map is the length the frame
    // kind implies, but pushed AWAY from the base colour rather than towards
    // it — a fill lighter than the surface reads as emphasis, which is the
    // opposite of "the device never looks here".
    const QColor outsideFill = dark ? freeFill.darker(150) : freeFill.darker(108);
    const QColor outsideText = dark ? QColor(0x45, 0x49, 0x4D) : QColor(0xC4, 0xC8, 0xCC);
    // Two signals claiming one bit — the same red the rest of the app uses for
    // a real conflict, and the thing this map exists to make obvious.
    const QColor clashFill = dark ? QColor(0x7E, 0x2C, 0x2F) : QColor(0xFF, 0xCD, 0xD2);
    // The Transmit CRC8 checksum byte. A neutral slate, deliberately outside the
    // twelve signal hues AND not the clash red: it is neither a channel nor a
    // fault, it is "spoken for — place your channels elsewhere". Stronger than
    // outsideFill, which recedes; this byte must assert itself, because the one
    // job of marking it is to be seen before a channel is dropped on it.
    const QColor crcFill = dark ? QColor(0x3A, 0x40, 0x4A) : QColor(0xDC, 0xE0, 0xE8);
    const QColor crcText = dark ? QColor(0x9A, 0xA2, 0xAE) : QColor(0x6E, 0x76, 0x84);

    QFont plain = font();
    QFont bold = font();
    bold.setBold(true);

    for (int b = 0; b < rowCount(); ++b) {
        const bool outside = b >= m_usableBytes;
        const bool crcByte = b == m_crcByte;
        for (int c = 0; c < 8; ++c) {
            QTableWidgetItem *cell = item(b, c);
            if (!cell)
                continue;
            const int bit = bitAt(b, c);
            const QList<int> owners = m_owners.value(bit);

            QColor fill;
            QColor text;
            QString tip;
            bool picked = false;
            if (owners.isEmpty()) {
                // The CRC marking outranks the outside grey on a free cell: a
                // location past the message length is a mistake the editor's OK
                // refuses, and drawing it as mere dead space would hide the very
                // thing the refusal is about to name.
                if (crcByte) {
                    fill = crcFill;
                    text = crcText;
                    tip = tr("bit %1 — CRC8: reserved for the stamped checksum byte").arg(bit);
                    if (outside)
                        tip += tr("  ⚠ outside the %1-byte message").arg(m_usableBytes);
                } else {
                    fill = outside ? outsideFill : freeFill;
                    text = outside ? outsideText : freeText;
                    tip = outside ? tr("bit %1 — outside the %2-byte message")
                                        .arg(bit).arg(m_usableBytes)
                                  : tr("bit %1 — free").arg(bit);
                }
            } else {
                QStringList names;
                names.reserve(owners.size());
                for (int idx : owners)
                    names << displayLabel(this, m_rows[idx].channelName);
                // Precedence: the picked signal always shows its full extent,
                // even where it overlaps something — otherwise clicking the very
                // signal in a clash would hide the half you are looking for.
                if (owners.contains(m_selected)) {
                    fill = signalColour(m_selected, palette(), true);
                    picked = true;
                } else if (owners.size() > 1) {
                    fill = clashFill;
                } else {
                    fill = signalColour(owners.first(), palette());
                }
                text = signalTextColour(fill);
                tip = owners.size() > 1
                          ? tr("bit %1 — %2  ⚠ these overlap")
                                .arg(bit)
                                .arg(names.join(QStringLiteral(" + ")))
                          : tr("bit %1 — %2").arg(bit).arg(names.first());
                if (outside)
                    tip += tr("  ⚠ outside the %1-byte message").arg(m_usableBytes);
                // A channel sitting on the CRC byte keeps its own colour — the
                // fill is how the user identifies WHICH channel is in the way —
                // and the tooltip carries the checksum's claim. The model's
                // validation is what refuses the overlap; this is the map's job
                // of making the finding unsurprising.
                if (crcByte)
                    tip += tr("  ⚠ the CRC8 checksum is stamped over this byte");
            }
            cell->setBackground(fill);
            cell->setForeground(text);
            cell->setFont(picked ? bold : plain);
            cell->setToolTip(tip);
        }
    }
}

QString BitLayoutTable::selectionSummary() const
{
    if (m_selected < 0 || m_selected >= m_rows.size())
        return {};
    const CommsChannelRow &row = m_rows[m_selected];
    const int bitLen = row.dbcType == int(DbcType::IEEE754) ? 32 : row.bitLength;
    const QList<int> bits = rowBitPositions(row, m_alignment);
    if (bits.isEmpty())
        return tr("\"%1\" — start bit %2, %3 bits: not a position this frame has.")
            .arg(displayLabel(this, row.channelName)).arg(row.startBit).arg(bitLen);

    int lo = bits.first();
    int hi = bits.first();
    for (int pos : bits) {
        lo = qMin(lo, pos);
        hi = qMax(hi, pos);
    }
    QString text = tr("\"%1\" — start bit %2, %3 bits: bits %4–%5, byte %6 to byte %7.")
                       .arg(displayLabel(this, row.channelName))
                       .arg(row.startBit)
                       .arg(bitLen)
                       .arg(lo)
                       .arg(hi)
                       .arg(lo / 8)
                       .arg(hi / 8);
    if (bits.size() < bitLen)
        text += tr("  ⚠ %1 bit(s) run off the end of the frame.").arg(bitLen - bits.size());
    else if (hi / 8 >= m_usableBytes)
        text += tr("  ⚠ reaches past the %1-byte message, so it is not extracted.")
                    .arg(m_usableBytes);
    // Named here as well as in the cell tooltips, because the caption is what a
    // user reads after CLICKING the channel — the moment they are deciding
    // whether its placement is right. Checked against the actual bit positions
    // rather than the lo..hi byte span: a Motorola field's span can cross a byte
    // none of its bits actually land in.
    if (m_crcByte >= 0) {
        for (int pos : bits) {
            if (pos / 8 == m_crcByte) {
                text += tr("  ⚠ overlaps byte %1, which is reserved for the CRC8 checksum.")
                            .arg(m_crcByte);
                break;
            }
        }
    }
    return text;
}

} // namespace ct
