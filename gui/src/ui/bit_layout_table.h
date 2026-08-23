// "Frame Layout" — a DBC-style bit map of one CAN message: a row per byte,
// eight columns for the bits inside it, numbered 7 (most significant) on the
// left to 0 on the right, exactly as every DBC tool and the CAN standard draw a
// frame. Each cell is labelled with its GLOBAL bit index (byte × 8 + bit), so
// the number in a cell is precisely what goes in a row's Start Bit field.
//
// The point of it is that a signal's bits are NOT a contiguous run of cells:
// Motorola (Normal) fields walk backwards through the bytes, so "start bit 8,
// 16 bits" covers a shape you cannot work out by reading two numbers. Colour
// makes the shape visible, and a picked signal is drawn in its own hue at full
// strength so it stands out from its neighbours.
#pragma once

#include <QHash>
#include <QList>
#include <QTableWidget>

#include "../model/comms_types.h"

namespace ct {

class BitLayoutTable : public QTableWidget
{
    Q_OBJECT
public:
    explicit BitLayoutTable(QWidget *parent = nullptr);

    // rows        — the signals to lay out (a section's rows, or one compound
    //               identifier's rows); indices in every signal below are
    //               indices into THIS list.
    // byteCount   — how many byte rows to draw.
    // usableBytes — bytes 0..usableBytes-1 are inside the message length; the
    //               rest are drawn greyed, because a signal there is not
    //               extracted (computeExtraction rejects it). For a transmit
    //               message the two are the same number.
    void setFrame(const QList<CommsChannelRow> &rows, SectionAlignment alignment,
                  int byteCount, int usableBytes);

    // Which signal is highlighted; -1 for none. Cheap — recolours, never rebuilds.
    void setSelectedRow(int index);
    int selectedRow() const { return m_selected; }

    // The byte a Transmit CRC8 section stamps its checksum into; -1 for "no CRC"
    // (every other section kind). Shaded even though no channel owns it, because
    // that byte is spoken for BEFORE any channel is placed — the whole point of
    // telling the map is that a user laying out channels sees the reserved byte
    // and routes around it, instead of meeting the collision as a validation
    // finding after OK. Cheap like setSelectedRow — recolours, never rebuilds.
    void setCrcByte(int byteIndex);

    // One-line caption for what is highlighted: where it actually lands, and a
    // warning when that is somewhere the device will not read it. Empty when
    // nothing is selected.
    QString selectionSummary() const;

    // The fill a signal at `index` is drawn with, and the text colour that
    // stays legible on it. Exposed so the channel list beside the map can carry
    // the same colours — that shared colour IS the link between the two panes,
    // and it would be worth nothing if the two derived it separately.
    static QColor signalColour(int index, const QPalette &pal, bool selected = false);
    static QColor signalTextColour(const QColor &fill);

signals:
    // A cell belonging to a signal was clicked. Carries the signal's index, so
    // the channel list can follow the map as well as drive it.
    void rowClicked(int index);

private:
    void rebuild();
    void applyColours();
    // Column 0 is bit 7 of the byte (most significant on the left).
    static int bitAt(int byteRow, int column) { return byteRow * 8 + (7 - column); }

    QList<CommsChannelRow> m_rows;
    SectionAlignment m_alignment = SectionAlignment::Normal;
    int m_byteCount = 8;
    int m_usableBytes = 8;
    int m_selected = -1;
    int m_crcByte = -1; // byte reserved for a Transmit CRC8 checksum; -1 = none
    QHash<int, QList<int>> m_owners; // bit position -> indices into m_rows
};

} // namespace ct
