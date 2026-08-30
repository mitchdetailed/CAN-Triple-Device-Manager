// Implementation of the "Edit Custom Channel" dialog.
#include "edit_channel_dialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include <cmath>

#include "../model/channel_catalog.h"
#include "../protocol/wire_structs.h"
#include "channel_field.h"
#include "name_limits.h"
#include "trimmed_spin_box.h"

namespace ct {

namespace {

// Raw value range and decimal-places cap for each storage data type. Float
// has no integer raw range; ±1e9 is the app's practical unbounded span.
struct DataTypeInfo {
    const char *name;
    double rawMin;
    double rawMax;
    int maxDecimals;
    bool scalesWithResolution; // physical range = raw range × base resolution
};

const DataTypeInfo kDataTypes[] = {
    {"boolean", 0.0, 1.0, 0, false},
    {"u8", 0.0, 255.0, 2, true},
    {"u16", 0.0, 65535.0, 4, true},
    {"u32", 0.0, 4294967295.0, 8, true},
    {"s8", -128.0, 127.0, 2, true},
    {"s16", -32768.0, 32767.0, 4, true},
    {"s32", -2147483648.0, 2147483647.0, 8, true},
    {"float", -1e9, 1e9, 8, false},
};

const DataTypeInfo *dataTypeInfo(const QString &name)
{
    for (const DataTypeInfo &t : kDataTypes)
        if (name == QLatin1String(t.name))
            return &t;
    return nullptr;
}

} // namespace

EditChannelDialog::EditChannelDialog(Configuration *config, const Channel &initial,
                                     bool isNew, QWidget *parent)
    : QDialog(parent),
      m_config(config),
      m_isNew(isNew),
      // A brand-new channel belongs to nobody yet, so nothing can be protecting
      // it; for an existing one the document is the only authority on this.
      //
      // isChannelEditLocked, NOT isChannelConcealed. This dialog is asking "may
      // this be CHANGED", and the two questions came apart in 2.3.0: a Read Only
      // message shows every field it has and still must not be edited, so the
      // set of channels whose controls are dead is no longer the set whose
      // values are withheld. Note it deliberately does not fold in
      // commsRevealed() — the edit lock is not lifted by the password, because
      // the password buys the right to UNTICK the tier and unticking is what
      // unlocks editing.
      m_readOnly(config && !isNew && config->isChannelEditLocked(initial.name)),
      m_originalName(initial.name),
      m_initial(initial)
{
    setWindowTitle(m_readOnly ? tr("Custom Channel — Read Only")
                              : tr("Edit Custom Channel"));

    // --- Channel Name group ---------------------------------------------
    auto *nameGroup = new QGroupBox(tr("Channel Name"), this);
    auto *nameForm = new QFormLayout(nameGroup);

    m_nameEdit = new QLineEdit(nameGroup);
    // The device label holds MAX_CHANNEL_NAME_BYTES characters; cap typing here
    // so the limit is felt rather than reported. validate() still checks the
    // UTF-8 byte count, which a non-ASCII name can exceed within this cap.
    // BYTES, not characters: setMaxLength counts QChars, and the two part
    // company the moment a name is not ASCII. See name_limits.h.
    ct::limitToUtf8Bytes(m_nameEdit, MAX_CHANNEL_NAME_BYTES);
    m_nameEdit->setToolTip(tr("Up to %1 characters — the device stores a %2-byte label.")
                               .arg(MAX_CHANNEL_NAME_BYTES).arg(SIGNAL_LABEL_LEN));
    nameForm->addRow(tr("Channel Name:"), m_nameEdit);

    // --- Channel Details group ------------------------------------------
    auto *detailsGroup = new QGroupBox(tr("Channel Details"), this);
    auto *detailsForm = new QFormLayout(detailsGroup);

    m_quantityCombo = new QComboBox(detailsGroup);
    m_quantityCombo->addItems(ChannelCatalog::quantities());

    m_dataTypeCombo = new QComboBox(detailsGroup);
    m_dataTypeCombo->addItem(QString()); // blank — force a deliberate choice
    for (const DataTypeInfo &t : kDataTypes)
        m_dataTypeCombo->addItem(QLatin1String(t.name));

    m_decimalsSpin = new QSpinBox(detailsGroup);
    m_decimalsSpin->setRange(0, 8);

    m_resolutionSpin = new TrimmedDoubleSpinBox(detailsGroup);
    m_unitsCombo = new QComboBox(detailsGroup);
    m_minSpin = new TrimmedDoubleSpinBox(detailsGroup);
    m_maxSpin = new TrimmedDoubleSpinBox(detailsGroup);
    m_resolutionSpin->setRange(-5e9, 5e9);
    m_resolutionSpin->setReadOnly(true);
    m_resolutionSpin->setButtonSymbols(QAbstractSpinBox::NoButtons);
    m_resolutionSpin->setFocusPolicy(Qt::NoFocus);
    // The RANGE is the user's to declare. It defaults to what the type spans
    // and re-derives when the type or the decimals change, but a channel's
    // plausibility range is its own fact - 0..8000 on an RPM channel - and the
    // device clamps every reading to it, so the two fields are editable. The
    // limits are float32's, because that is what CanSignalConfig carries; the
    // J1939 wide signals a DBC brings in live far outside the old +/-5e9.
    const QString rangeTip =
        tr("Defaults to what the data type spans at the chosen precision, and "
           "re-derives when either changes. Edit it to declare what this "
           "channel should read - the device clamps every reading to this "
           "range.");
    for (QDoubleSpinBox *rangeSpin : {m_minSpin, m_maxSpin}) {
        rangeSpin->setRange(-3.402823e38, 3.402823e38);
        rangeSpin->setButtonSymbols(QAbstractSpinBox::NoButtons);
        rangeSpin->setToolTip(rangeTip);
    }
    // Named so a test can address them without counting spin boxes in creation
    // order - the two fields whose refusals (min >= max) exist to keep a range
    // the device would pin every reading against out of a configuration.
    m_minSpin->setObjectName(QStringLiteral("rangeMinimum"));
    m_maxSpin->setObjectName(QStringLiteral("rangeMaximum"));
    m_nameEdit->setObjectName(QStringLiteral("channelName"));
    
    detailsForm->addRow(tr("Data Type:"), m_dataTypeCombo);
    detailsForm->addRow(tr("Decimal Places:"), m_decimalsSpin);
    detailsForm->addRow(tr("Base Resolution:"), m_resolutionSpin);
    detailsForm->addRow(tr("Units Type:"), m_quantityCombo);
    detailsForm->addRow(tr("Display Units:"), m_unitsCombo);
    detailsForm->addRow(tr("Range Minimum:"), m_minSpin);
    detailsForm->addRow(tr("Range Maximum:"), m_maxSpin);

    // --- Buttons ----------------------------------------------------------
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                         Qt::Horizontal, this);

    auto *mainLayout = new QVBoxLayout(this);
    if (m_readOnly) {
        // Said at the top, before the values, because otherwise the first thing
        // the user learns about this dialog is that OK does not respond.
        //
        // The values themselves stay on show, and that is deliberate: the point
        // of Protect Communication is to protect the PROTOCOL, not the outputs.
        // A customer has to know that this channel is degrees C to two decimals
        // in order to use it in their own math and transmit messages; what they
        // are not being told is which bits of which message it came out of.
        auto *notice = new QLabel(
            tr("🔒 \"%1\" is carried by a protected message, so its definition belongs to "
               "that message and is read-only here. Its data type, base resolution and "
               "decimal places are what makes that message decode to the right numbers — "
               "change them and it quietly starts decoding to different ones.\n\n"
               "Unticking the message's protection in Connections > Communications is what "
               "unlocks this; the password on its own does not, because a password given to "
               "look at a message is not a decision to start editing it.")
                .arg(initial.name),
            this);
        notice->setWordWrap(true);
        // A wrapped label has no natural width of its own, so without a cap it
        // asks the layout for one very long line and drags the dialog with it.
        notice->setMaximumWidth(440);
        const bool darkUi = palette().color(QPalette::Window).lightness() < 128;
        notice->setStyleSheet(darkUi ? QStringLiteral("color: #9aa0a6;")
                                     : QStringLiteral("color: #606468;"));
        mainLayout->addWidget(notice);
    }
    mainLayout->addWidget(nameGroup);
    mainLayout->addWidget(detailsGroup);
    mainLayout->addWidget(buttons);

    // --- Signal wiring ----------------------------------------------------
    connect(m_quantityCombo, &QComboBox::currentIndexChanged, this,
            [this](int) { onQuantityChanged(); });
    connect(m_dataTypeCombo, &QComboBox::currentIndexChanged, this,
            [this](int) { onDataTypeChanged(); });
    connect(m_decimalsSpin, &QSpinBox::valueChanged, this,
            [this](int) { updateDerived(); });
    connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
        if (validate())
            QDialog::accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    // --- Populate from `initial` -------------------------------------------
    int qtyIndex = m_quantityCombo->findText(initial.quantity);
    if (qtyIndex < 0)
        qtyIndex = 0;
    m_quantityCombo->setCurrentIndex(qtyIndex);
    onQuantityChanged(); // ensure units combo is populated even if index unchanged

    int unitIndex = m_unitsCombo->findText(initial.unit);
    if (unitIndex >= 0)
        m_unitsCombo->setCurrentIndex(unitIndex);

    // Blank for channels created before data types existed — the user must
    // pick one before the dialog will accept.
    m_dataTypeCombo->setCurrentIndex(qMax(0, m_dataTypeCombo->findText(initial.dataType)));
    m_decimalsSpin->setValue(initial.decimalPlaces);
    onDataTypeChanged(); // apply the decimals cap, then derive res/range
    // AFTER the derivation, the stored range goes back in: the fields hold the
    // channel's own declared range, and the type span is only where a range
    // STARTS. Skipped for an inverted or empty pair - the derived span is a
    // better offer than a range the device would pin every reading against.
    if (!m_isNew && initial.minValue < initial.maxValue) {
        m_minSpin->setValue(initial.minValue);
        m_maxSpin->setValue(initial.maxValue);
    }

    if (m_isNew) {
        const QString defaultName =
            initial.name.isEmpty() ? tr("New Channel") : initial.name;
        m_nameEdit->setText(defaultName);
        m_nameEdit->selectAll(); // overwrite typing
    } else {
        m_nameEdit->setText(initial.name);
    }

    // Last, so that the population above — which re-enables Decimal Places
    // through onDataTypeChanged — cannot undo it.
    if (m_readOnly) {
        // The name goes too. createOrEdit treats a changed name as a rename and
        // rewrites every reference to the channel, so leaving that one field
        // live would be a way to alter the protected message's output by the
        // back door while looking like a typo fix.
        const QList<QWidget *> locked{m_nameEdit,      m_quantityCombo,  m_unitsCombo,
                                      m_dataTypeCombo, m_decimalsSpin,   m_resolutionSpin,
                                      m_minSpin,       m_maxSpin};
        for (QWidget *w : locked)
            w->setEnabled(false);
        // With OK disabled the dialog has exactly one exit, so channel() is
        // never read back and validate() never runs.
        buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
        buttons->button(QDialogButtonBox::Cancel)->setFocus();
    } else {
        m_nameEdit->setFocus();
    }
}

void EditChannelDialog::onQuantityChanged()
{
    const QString previousUnit = m_unitsCombo->currentText();
    const QString quantity = m_quantityCombo->currentText();
    m_unitsCombo->clear();
    m_unitsCombo->addItems(ChannelCatalog::unitsForQuantity(quantity));
    // Keep the previously chosen unit if the new quantity still offers it;
    // otherwise land on the quantity's default unit.
    int idx = m_unitsCombo->findText(previousUnit);
    if (idx < 0)
        idx = m_unitsCombo->findText(ChannelCatalog::defaultUnitForQuantity(quantity));
    m_unitsCombo->setCurrentIndex(idx >= 0 ? idx : 0);
}

void EditChannelDialog::onDataTypeChanged()
{
    const DataTypeInfo *type = dataTypeInfo(m_dataTypeCombo->currentText());
    // No decimals until a type is chosen; boolean locks them at 0. While the
    // type is blank keep the full range so a legacy channel's loaded decimals
    // survive until the user picks one (a 0 cap would clamp them away).
    m_decimalsSpin->setEnabled(!m_readOnly && type && type->maxDecimals > 0);
    m_decimalsSpin->setRange(0, type ? type->maxDecimals : 8);
    updateDerived();
}

void EditChannelDialog::updateDerived()
{
    const DataTypeInfo *type = dataTypeInfo(m_dataTypeCombo->currentText());
    const int decimals = m_decimalsSpin->value();
    const double resolution = std::pow(10.0, -decimals);
    for (QDoubleSpinBox *derived : {m_resolutionSpin, m_minSpin, m_maxSpin})
        derived->setDecimals(decimals);
    if (!type) {
        m_resolutionSpin->setValue(0);
        m_minSpin->setValue(0);
        m_maxSpin->setValue(0);
        return;
    }
    const double scale = type->scalesWithResolution ? resolution : 1.0;
    m_resolutionSpin->setValue(resolution);
    m_minSpin->setValue(type->rawMin * scale);
    m_maxSpin->setValue(type->rawMax * scale);
}

bool EditChannelDialog::validate()
{
    const QString name = m_nameEdit->text().trimmed();
    if (name.isEmpty()) {
        QMessageBox::warning(this, tr("Edit Custom Channel"),
                             tr("The channel name must not be empty."));
        return false;
    }
    if (name.toUtf8().size() > MAX_CHANNEL_NAME_BYTES) {
        QMessageBox::warning(this, tr("Edit Custom Channel"),
                             tr("Channel names are limited to %1 bytes on the device.")
                                 .arg(MAX_CHANNEL_NAME_BYTES));
        return false;
    }
    if (m_dataTypeCombo->currentText().isEmpty()) {
        QMessageBox::warning(this, tr("Edit Custom Channel"),
                             tr("Choose a data type for the channel."));
        return false;
    }
    // The device applies the min clamp first and the max clamp second, with no
    // inverted-pair guard, so min >= max pins every reading to the maximum.
    if (m_minSpin->value() >= m_maxSpin->value()) {
        QMessageBox::warning(this, tr("Edit Custom Channel"),
                             tr("Range Minimum must be below Range Maximum — the device "
                                "clamps every reading to this range."));
        return false;
    }

    const bool sameAsOriginal =
        !m_isNew && name.compare(m_originalName, Qt::CaseInsensitive) == 0;
    if (m_config && !sameAsOriginal) {
        for (const Channel &user : m_config->catalog().userChannels()) {
            if (user.name.compare(name, Qt::CaseInsensitive) == 0) {
                QMessageBox::warning(this, tr("Edit Custom Channel"),
                                     tr("A user channel named \"%1\" already exists.").arg(name));
                return false;
            }
        }
    }
    return true;
}

Channel EditChannelDialog::channel() const
{
    Channel c;
    c.name = m_nameEdit->text().trimmed();
    c.quantity = m_quantityCombo->currentText();
    c.unit = m_unitsCombo->currentText();
    c.dataType = m_dataTypeCombo->currentText();
    c.baseResolution = m_resolutionSpin->value();
    c.decimalPlaces = m_decimalsSpin->value();
    // At their word: the fields are populated from the channel's stored range
    // and only re-derived by a type or decimals change, so what they show is
    // what the user chose to leave there. The old preserve-the-wider-range
    // guard existed because they used to show the derived span instead.
    c.minValue = m_minSpin->value();
    c.maxValue = m_maxSpin->value();
    c.category = QStringLiteral("User Channels");
    c.userDefined = true;
    return c;
}

QString EditChannelDialog::createOrEdit(Configuration *config, const Channel &initial,
                                        bool isNew, QWidget *parent)
{
    EditChannelDialog dlg(config, initial, isNew, parent);
    if (dlg.exec() != QDialog::Accepted)
        return QString();
    // A protected channel's dialog has no enabled OK button, so accepting it
    // takes some route that does not exist today. Checking anyway keeps the
    // write-back behind the same condition as the button rather than behind the
    // button's enabled state, which is a UI detail and could change.
    if (dlg.m_readOnly)
        return QString();

    const Channel c = dlg.channel();
    const bool renamed =
        !isNew && c.name.compare(dlg.m_originalName, Qt::CaseInsensitive) != 0;
    if (renamed)
        config->catalog().removeUserChannel(dlg.m_originalName);
    config->catalog().addOrUpdateUserChannel(c);
    if (renamed) {
        // Keep every reference pointing at the new name: the document's stored
        // rows, the working copies of the grid dialogs this dialog is open
        // under (they listen to Configuration::channelRenamed), and the
        // channel fields of the open row editors above them
        // (renameOpenChannelFields). Deliberately AFTER the catalog update, so
        // a listener re-labelling its list finds the new name's unit.
        config->renameChannelReferences(dlg.m_originalName, c.name);
        renameOpenChannelFields(dlg.m_originalName, c.name, config->catalog());
    }
    config->setDirty();
    return c.name;
}

} // namespace ct
