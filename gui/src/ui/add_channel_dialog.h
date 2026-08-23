// "Add Comms Channel" — one signal row inside a message, defined DBC-style:
// channel + Select…, Default Value (on timeout), Start Bit, Bit Length,
// DBC Type (Unsigned / Signed / IEEE754), Bit Resolution and Offset
// (physical = raw × Factor + Offset), and on a transmit row the clamp/roll-over
// choice. Live-validated against the section's alignment and message length.
#pragma once

#include <QDialog>

#include "../model/comms_types.h"
#include "../model/configuration.h"

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QSpinBox;

namespace ct {

class TrimmedDoubleSpinBox;

class AddChannelDialog : public QDialog
{
    Q_OBJECT
public:
    // transmit decides which side of the channel this row sits on: a transmit
    // row READS a channel (any channel, in any number of messages — sending a
    // value does not change it), a receive row WRITES one (so naming a channel
    // another source already writes is flagged: they share one device slot).
    // livePatch: re-states the enclosing Communications Setup / section editor's
    // sections, which are not written back to the document until OK — see
    // ConfigPatch. Both the picker and this dialog's own warning judge against
    // that view rather than the stale document.
    // sectionProtection: the enclosing section's RAW TIER. It used to be the
    // already-decided bool CommsSection::isEditLocked(), and that lost the one
    // fact the window title needs: which tier locked it. "Read Only" is now a
    // TIER NAME, so titling every locked row "Comms Channel — Read Only" told a
    // viewer of a Hidden or Protect Communication message the wrong tier — and
    // the wrong tier implies the wrong way to unlock it (this section's own
    // password vs. Protected Comms proved against a device). The lock
    // itself is still just `!= None`: an edit lock is NOT lifted by the Edit
    // Protected Comms password, so there is nothing for this dialog to fold in
    // beyond naming what it is looking at. Both section-editor call sites pass
    // m_section.protection; the default is None for the offline and test
    // callers that construct a row outside any section.
    AddChannelDialog(Configuration *config, const CommsChannelRow &initial,
                     SectionAlignment alignment, int messageLengthBytes, bool transmit,
                     const ConfigPatch &livePatch = {}, QWidget *parent = nullptr,
                     CommsProtection sectionProtection = CommsProtection::None);

    CommsChannelRow row() const;

private:
    void onSelectChannel();
    void onDbcTypeChanged(); // IEEE754 forces a 32-bit length
    void revalidate();       // preview + OK enabled state

    Configuration *m_config;
    SectionAlignment m_alignment;
    int m_messageLength;
    bool m_transmit;
    CommsProtection m_protection; // the enclosing section's tier, for the title
    bool m_readOnly; // the section is edit-locked: show every field, write none
    ConfigPatch m_livePatch;
    Configuration m_live; // m_config + m_livePatch, re-derived after each pick
    CommsChannelRow m_row;
    // A new row's Bit Length was still blank when IEEE754 auto-filled 32, so
    // leaving IEEE754 blanks it again rather than keeping a number the user
    // never entered. See onDbcTypeChanged().
    bool m_lengthWasUnchosen = false;

    QLineEdit *m_channelEdit;
    // Concrete type, not QDoubleSpinBox: this one switches between trimmed and
    // fixed-width decimals as the selected channel's precision changes.
    TrimmedDoubleSpinBox *m_defaultSpin;
    QLabel *m_defaultUnitLabel;
    QSpinBox *m_startBitSpin;
    QSpinBox *m_bitLengthSpin;
    QComboBox *m_dbcTypeCombo;
    QDoubleSpinBox *m_factorSpin;
    QDoubleSpinBox *m_dbcOffsetSpin;
    // Transmit rows only — null on a receive row, where there is nothing to
    // clamp but the channel's own range and the device always does that.
    QCheckBox *m_clampCheck = nullptr;
    QLabel *m_previewLabel;
    QLabel *m_warnLabel; // dimmed note, or the two-writer warning; never blocks OK
    QLabel *m_errorLabel;
    QDialogButtonBox *m_buttons;
};

} // namespace ct
