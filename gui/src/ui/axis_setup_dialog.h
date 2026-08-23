// A dedicated window for defining one lookup-table axis: its input channel, its
// behaviour (interpolated / discrete), and its ascending list of breakpoint
// values, with the bulk tools that make a real axis quick to build — Insert,
// Delete, Linearise (even spacing between the ends) and Generate (from / to /
// count). The 8x8 table editor opens one of these per axis; the grid itself
// stays the place you type the output values.
#pragma once

#include <QDialog>
#include <QList>
#include <QString>

#include "../model/configuration.h" // ConfigPatch
#include "numeric_grid.h"           // CellSpec, GridClipboard

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;

namespace ct {

class Configuration;

class AxisSetupDialog : public QDialog
{
    Q_OBJECT
public:
    // One axis, in and out. maxSites bounds the breakpoint count (8 for an 8x8
    // table axis, 16 for a 2x16). sites is ascending and 0..maxSites long.
    struct Axis {
        QString title;   // "X Axis" / "Y Axis" — shown in the window title
        QString channel; // input channel name (may be empty)
        bool interp = true;
        QList<double> sites;
        int maxSites = 8;
    };

    AxisSetupDialog(Configuration *config, const Axis &axis, const ConfigPatch &livePatch,
                    QWidget *parent = nullptr);
    ~AxisSetupDialog() override;

    Axis result() const { return m_axis; }

private:
    CellSpec cellSpec() const; // range/decimals from the current channel
    void render(const QList<double> &values); // write values into the row, blank the rest
    QList<double> collect() const;            // non-blank cells, ascending
    void reformat();                          // clamp cells to the current channel spec
    void refreshChannelUi();                  // channel text, units label, spec

    void onSelectChannel();
    void onInsert();
    void onDelete();
    void onLinearise();
    void onGenerate();
    void validateAndAccept();

    Configuration *m_config;
    Axis m_axis;
    ConfigPatch m_livePatch;

    QLineEdit *m_channelEdit = nullptr;
    QComboBox *m_behaviour = nullptr;
    QLabel *m_valuesLabel = nullptr;
    QTableWidget *m_grid = nullptr;
    QPushButton *m_deleteButton = nullptr;
    QPushButton *m_lineariseButton = nullptr;
    GridClipboard *m_clip = nullptr;

    bool eventFilter(QObject *obj, QEvent *event) override;
};

} // namespace ct
