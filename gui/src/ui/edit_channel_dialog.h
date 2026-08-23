// "Edit Custom Channel" — name, channel type (quantity), data type
// (boolean/u8..s32/float), decimal places, display units. Base resolution and
// the physical range are derived from the data type + decimal places.
//
// A channel carried by a PROTECTED message opens here READ-ONLY: every value is
// still on show, and none of them can be changed. At every tier, Read Only
// included — the channel's data type and resolution are the message's decode,
// so they are locked by the same rule that locks the message, and that rule is
// not lifted by any password. It is lifted by unticking the message's marking.
// See the note the constructor puts at the top of the dialog for why the two
// halves — everything visible, nothing writable — belong together.
#pragma once

#include <QDialog>

#include "../model/channel.h"
#include "../model/configuration.h"

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QSpinBox;

namespace ct {

class EditChannelDialog : public QDialog
{
    Q_OBJECT
public:
    // isNew: the dialog enforces a unique name and adds nothing itself; the
    // caller stores channel() into the catalog on accept.
    EditChannelDialog(Configuration *config, const Channel &initial, bool isNew,
                      QWidget *parent = nullptr);

    Channel channel() const;

    // Shows the dialog; on accept stores the channel in the catalog and
    // returns its name. Empty string if cancelled.
    static QString createOrEdit(Configuration *config, const Channel &initial, bool isNew,
                                QWidget *parent);

private:
    void onQuantityChanged();
    void onDataTypeChanged();
    void updateDerived(); // resolution + range from data type and decimals
    bool validate();

    Configuration *m_config;
    bool m_isNew;
    // Latched at construction from Configuration::isChannelEditLocked — the
    // "may this be CHANGED" half of the predicate that split in 2.3.0, not the
    // "may this be SEEN" half. It cannot change while a modal dialog is up —
    // lowering a message's tier goes through Communications Setup — so re-asking
    // per keystroke would only invite the two answers to disagree halfway
    // through an edit.
    bool m_readOnly;
    QString m_originalName;
    Channel m_initial; // to preserve a range wider than the type's display span
    QLineEdit *m_nameEdit;
    QComboBox *m_quantityCombo;
    QComboBox *m_dataTypeCombo;
    QSpinBox *m_decimalsSpin;
    QDoubleSpinBox *m_resolutionSpin;
    QComboBox *m_unitsCombo;
    QDoubleSpinBox *m_minSpin;
    QDoubleSpinBox *m_maxSpin;
};

} // namespace ct
