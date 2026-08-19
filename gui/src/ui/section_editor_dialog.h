// "CAN Communications Setup" — the section (message) editor with Parameters
// and Received/Transmitted Channels tabs, including compound identifiers, plus
// a CRC8 tab that only a Transmit CRC8 section enables.
#pragma once

#include <QDialog>

#include "../model/comms_types.h"
#include "../model/configuration.h"
#include "bit_layout_table.h"

class QCheckBox;
class QComboBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QRadioButton;
class QSpinBox;
class QStackedWidget;
class QTabWidget;
class QTreeWidget;

namespace ct {

class SectionEditorDialog : public QDialog
{
    Q_OBJECT
public:
    // livePatch: Communications Setup's own patch, re-stating the working
    // copies of all three buses (see ConfigPatch); an empty one means the
    // document is authoritative. sectionIndex is where this section sits in
    // those buses, or -1 while it is still being created — liveView() layers
    // this dialog's in-progress edits on top at that position, so a row
    // deleted here stops counting as a source immediately. Deliberately has no
    // default: passing -1 for a section that IS in the list would append a
    // phantom copy of it and make its every channel look doubly generated.
    // `prover` is needed for exactly one thing: CROSSING the Protect
    // Communication boundary — ticking that box or unticking it — which by spec
    // requires the Edit Protected Comms password to be proved against a CONNECTED
    // DEVICE. Nothing else in this dialog goes near hardware. An empty one is not
    // an error — it means there is no device path, and the move is simply refused
    // with a message saying why. Defaulted, so the offline callers (main.cpp's
    // widget smoke test, the GUI tests) are unaffected.
    // busDataRateKbps: the bus's FD Data rate AS CURRENTLY SHOWN in the
    // Communications dialog's working copy, which gates the CAN FD checkbox.
    // -1 falls back to the committed configuration — but the Communications
    // dialog must pass its live value: its bus tabs commit only on OK, so
    // reading the config from inside the same visit sees the FD rate the
    // session STARTED with, and a user who set FD Data and then opened a
    // section found the FD box inexplicably locked until they OK'd the whole
    // dialog and came back in.
    SectionEditorDialog(Configuration *config, const CommsSection &section, int busIndex,
                        const ConfigPatch &livePatch, int sectionIndex,
                        QWidget *parent = nullptr, ProtectedCommsProver prover = {},
                        int busDataRateKbps = -1);

    CommsSection section() const { return m_section; }
    // True once this session has met EVERY challenge the tier the section ARRIVED
    // at demands — Configuration::proofsRequiredFor(m_openedTier), all of it, not
    // whichever part happened to be asked for first. Communications Setup reads it
    // after OK and records the grant on the document, so
    // Configuration::applyBusSections will accept the untick that was just
    // authorised here — the dialog proves, the model enforces, and neither does
    // the other's job.
    //
    // Partial credit is the thing this must never give. A grant stands for the
    // whole of a tier's proof once it is recorded, so returning true after only
    // the section password on a Protected section would hand out the device half
    // for free — for that section, everywhere in the app, for the rest of the
    // session.
    bool protectionUnlocked() const;

private:
    ConfigPatch liveView() const;

    // Parameters tab
    void buildParametersTab(QWidget *page);
    void onDeviceChanged();
    // Which controls the MESSAGE TYPE allows, with the tier's verdict ANDed in.
    // Split out of onDeviceChanged so updateProtectionUi can re-run it: the lock
    // has to be liftable, and "enable everything again" would hand a relay a
    // Transmit Rate. One answer to "what would this control be if nothing were
    // protected?", so the two passes cannot disagree.
    void applyDeviceKindEnablement(bool editable);
    void updateTxModeControls(); // show Transmit Mode only for compound transmit
    // Show the Transmit Condition row only for a Triggered transmit message. A
    // separate pass from updateTxModeControls, which despite its name governs
    // the COMPOUND cadence combo and nothing to do with Cyclic/Triggered.
    void updateTriggerControls();
    void updateRelayControls();  // show relay group only for Message Relay
    void onAddressEdited();
    void reformatAddress(); // 0x%03X standard / 0x%08X extended
    void reformatBitmask(); // relay match mask, same width as the address
    void onBitmaskEdited();
    void syncParametersFromUi();

    // ---- the protection tier: three checkboxes, ONE ordered level ----
    // The boxes are a ladder, not three switches: level N means boxes 1..N are
    // ticked. Ticking box k sets the level to k; unticking box k drops it to
    // k-1, taking every stronger box with it. That is the whole invariant, and
    // it lives here rather than in three toggled() handlers that would each have
    // to re-derive the other two.
    void onTierBoxToggled(CommsProtection level, bool ticked);
    // Repaint the three boxes, the two advisory labels and every control the
    // tier locks, from m_tier. Signals are blocked while the boxes are set, so
    // this never re-enters onTierBoxToggled.
    void updateProtectionUi();

    // ---- rule 2: ONE path for every tier change, up or down ----
    // May the tier move from `from` to `to`? Runs whatever that move demands and
    // remembers the answers for the rest of this editor. Reports its own
    // failures; true means the move may proceed.
    //
    // RAISING is guarded now too, and that is the change. It used to be free —
    // only `wanted < m_openedTier` was challenged — so a Read Only section could
    // be walked up to Hidden or Protected by anyone holding the file, and the
    // password that was supposed to govern the marking was never asked for.
    // Raising and lowering are the same event (this section is leaving the tier
    // it is on) and they go down one path so they cannot drift apart again.
    //
    // A KEYLESS marking cannot be lowered here at all, which is step 0 of it:
    // Configuration::maySectionLower fails closed for one, and a dialog that let
    // the box be unticked anyway would only be arranging for the model to refuse
    // the whole commit later, in another window, with unrelated edits attached.
    bool authoriseTierChange(CommsProtection from, CommsProtection to);
    // Give up `from`: this section's own password, checked locally against
    // messageKey. Nothing to ask when the section carries no key — which by the
    // time this runs means only a RAISE or a first password, both free.
    // Deliberately does NOT accept the document's Edit Protected Comms password
    // as a substitute at any tier: that master-key substitution is what the spec
    // refuses.
    //
    // TWO callers, and the second is not a tier change. accept() runs this before
    // it will accept a NEW password typed over an existing key, because replacing
    // the lock hands the section over exactly as unticking the box does — and on
    // Read Only, which never conceals and so opens with no challenge, that was
    // the whole of the bypass. One path for both, so "prove the current password"
    // has one implementation.
    bool proveSectionPassword(CommsProtection from);
    // The Edit Protected Comms password, checked BY A CONNECTED DEVICE. Demanded
    // whenever a move crosses the Protected boundary in either direction, which
    // is the only thing that makes that tier stronger than Hidden. Refuses
    // outright with no device — an offline fallback would delete the tier.
    bool proveDeviceForProtected();
    // Rule 1's precondition, asked by accept(). The stored messageKey belongs to
    // m_openedTier — the tier it was chosen to guard. So a marked section is
    // satisfied by a freshly typed password, or by its stored key ONLY while the
    // tier has not moved. That is rule 2's "a new password for the new attribute"
    // and rule 1's "no marked tier without a password", stated once.
    bool sectionPasswordSatisfied() const;
    // Everything that describes the message's protocol is editable only at tier
    // None — the password buys viewing and the right to untick, and unticking is
    // what unlocks editing.
    bool protocolFieldsEnabled() const { return m_tier == CommsProtection::None; }
    // The Name and Message Password fields answer a different question: they are
    // locked for a section that ARRIVED protected and has not been unlocked.
    // Name, because renaming a still-locked section would make it look like a
    // removal-and-addition to the model's untick rule, which matches by name.
    // Message Password, because setting a new one on a section you cannot untick
    // would be the way past not knowing the old — the same rule
    // Configuration::setCommsPassword states for the document.
    bool identityFieldsEnabled() const;

    // CRC8 tab (device == TransmitCrc8 only). One slot per possible checksum
    // element; mirrors CRC8_MAX_ELEMENTS in protocol/wire_structs.h rather than
    // including the wire header here — buildCrcTab static_asserts the two equal,
    // so the mirror cannot drift silently.
    static constexpr int kCrcElementSlots = 15;
    void buildCrcTab(QWidget *page);
    void onSelectCrcChannel();
    // The CRC widgets' enabled state, tier and element count together. The tier
    // is a parameter (not re-derived) for the same reason as
    // applyDeviceKindEnablement's: one verdict, ANDed into every line, so a
    // locked field is never briefly live between two passes.
    void applyCrcEnablement(bool editable);

    // Channels tab
    void buildChannelsTab(QWidget *page);
    void onMessageTypeToggled();
    void rebuildChannelList();
    // Frame layout map. refresh re-lays the whole grid (rows, alignment, frame
    // size); updateBitSelection only moves the highlight, and is what the
    // channel list's selection drives.
    void refreshBitTable();
    void updateBitSelection();
    void rebuildIdentifierTable();
    QList<CommsChannelRow> *activeRowList(); // rows of single mode or selected identifier
    // Compound-mode identifier tree role: -1 = the always-present set, >= 0 = an
    // identifier index, -2 = nothing selected.
    int currentIdentRole() const;
    void onAddRow();
    void onChangeRow();
    void onRemoveRow();
    void onEditIdentifier();
    void onClearIdentifier();
    void updateButtons();

    void accept() override;

    Configuration *m_config;
    CommsSection m_section;
    int m_busIndex;
    int m_busDataRateKbps; // caller's live FD rate; -1 = read the config (see ctor)
    ConfigPatch m_livePatch;
    int m_sectionIndex;
    ProtectedCommsProver m_prover; // for the Protected untick only; may be empty
    bool m_fdAllowed = false; // whether the CAN FD frame box may be enabled

    // The working tier. m_section.protection is only written from it in
    // syncParametersFromUi, so a cancelled dialog leaves the section untouched.
    CommsProtection m_tier = CommsProtection::None;
    // The tier the section ARRIVED with, and the tier its stored messageKey was
    // chosen to guard. Any move OFF it — up or down — is the change rule 2
    // guards, and any marked tier other than it needs a password typed here. Once
    // the move has been authorised, wandering between tiers costs nothing more:
    // the proof is about leaving m_openedTier, and it has been given.
    CommsProtection m_openedTier = CommsProtection::None;
    // This section's OWN password has been proved this editor (or it had none to
    // prove). Set only by proveSectionPassword().
    bool m_ownPasswordProved = false;
    // A connected device has confirmed Edit Protected Comms this editor. Set only
    // by proveDeviceForProtected(). Kept apart from the flag above because
    // Protected needs BOTH and a single "unlocked" bool cannot say which half was
    // given — which is how a grant standing for two proofs gets recorded after
    // one.
    bool m_deviceProved = false;
    // The password advice is shown once per editor, not once per tick: marking a
    // message up before choosing the password that will guard it is a perfectly
    // normal order to work in, and it is accept() that finally insists.
    bool m_warnedNoPassword = false;

    QTabWidget *m_tabs;

    // Parameters widgets
    QLineEdit *m_nameEdit;
    QComboBox *m_deviceCombo;
    QComboBox *m_alignmentCombo;
    QSpinBox *m_timeoutSpin;
    QCheckBox *m_defaultTimeoutCheck;
    // The tier ladder, weakest first. Three boxes, one ordered level: see
    // onTierBoxToggled. Named for the tiers rather than for the old flags,
    // because 2.2.x's "Read-only" is this build's HIDDEN and reusing the label
    // for the new visible tier is exactly the confusion the wire encoding was
    // chosen to avoid.
    QCheckBox *m_readOnlyCheck = nullptr;  // Read Only : visible, not editable
    QCheckBox *m_hiddenCheck = nullptr;    // Hidden    : not visible either
    QCheckBox *m_protectCheck = nullptr;   // Protected : Hidden + a device proof
    QLineEdit *m_messagePasswordEdit = nullptr; // this section's own password
    QLabel *m_protectionNote = nullptr;    // what the current tier actually means
    QRadioButton *m_standardRadio;
    QRadioButton *m_extendedRadio;
    QCheckBox *m_fdCheck;
    QLineEdit *m_addressEdit;
    QLabel *m_addressDecLabel;
    QLabel *m_lengthLabel;
    QLineEdit *m_lengthEdit;
    QLabel *m_lengthUnitLabel;
    QRadioButton *m_cyclicRadio;
    QRadioButton *m_triggeredRadio;
    QComboBox *m_rateCombo;
    QLabel *m_txConditionLabel;        // Triggered transmit: the gating User Condition
    QComboBox *m_txConditionCombo;     // userData is the condition's output channel name
    QLabel *m_txModeLabel;   // compound transmit cadence (Batch / Sequential)
    QComboBox *m_txModeCombo;
    QGroupBox *m_routeGroup;
    QCheckBox *m_routeEnableCheck;
    QCheckBox *m_routeBusCheck[3];

    // Message Relay (v11) widgets
    QGroupBox *m_relayGroup;
    QLabel *m_bitmaskLabel;
    QLineEdit *m_bitmaskEdit;
    QLabel *m_bitmaskDecLabel;
    QCheckBox *m_relayInvertCheck;
    QCheckBox *m_relayBusCheck[3];

    // Channels widgets
    QRadioButton *m_singleRadio;
    QRadioButton *m_compoundRadio;
    QStackedWidget *m_typeStack;
    QListWidget *m_channelList;
    QLabel *m_channelListLabel;
    QTreeWidget *m_identifierTree;
    QPushButton *m_identChangeButton;
    QPushButton *m_identClearButton;
    QPushButton *m_addButton;
    QPushButton *m_changeButton;
    QPushButton *m_removeButton;

    // Frame layout map, under both panes. Null until buildChannelsTab has run —
    // onDeviceChanged fires from the Parameters tab, which is built first.
    QGroupBox *m_bitGroup = nullptr;
    BitLayoutTable *m_bitTable = nullptr;
    QLabel *m_bitCaption = nullptr;

    // CRC8 tab widgets. Null until buildCrcTab has run, and it runs LAST of the
    // three tabs — every enablement pass that can fire earlier must check.
    QLineEdit *m_crcChannelEdit = nullptr;   // read via ct::channelField(), never text()
    QPushButton *m_crcSelectButton = nullptr;
    QComboBox *m_crcByteCombo = nullptr;
    QLineEdit *m_crcPolyEdit = nullptr;
    QLineEdit *m_crcInitEdit = nullptr;
    QLineEdit *m_crcXorEdit = nullptr;
    QCheckBox *m_crcRefInCheck = nullptr;
    QCheckBox *m_crcRefOutCheck = nullptr;
    QSpinBox *m_crcCountSpin = nullptr;
    // The 15 element rows, pre-built once. A fixed grid with a per-row stack for
    // the value side, instead of add/remove churn: rows past Element Count are
    // merely disabled, so a count lowered by accident and raised again hands the
    // user their elements back instead of fifteen fresh defaults.
    QLabel *m_crcElemLabel[kCrcElementSlots] = {};
    QComboBox *m_crcElemType[kCrcElementSlots] = {};
    QStackedWidget *m_crcElemStack[kCrcElementSlots] = {};
    QComboBox *m_crcElemId[kCrcElementSlots] = {};
    QComboBox *m_crcElemData[kCrcElementSlots] = {};
    QLineEdit *m_crcElemRaw[kCrcElementSlots] = {};
};

} // namespace ct
