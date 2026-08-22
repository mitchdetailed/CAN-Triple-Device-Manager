// "Communications Setup" — per-bus tabs with the sections list, mirroring
// Connections > Communications in Dash Manager.
#pragma once

#include <QDialog>
#include <QSet>
#include <QString>

#include "../model/configuration.h"

class QComboBox;
class QLabel;
class QListWidget;
class QPushButton;
class QTabWidget;
class QTreeWidget;

namespace ct {

class CommunicationsDialog : public QDialog
{
    Q_OBJECT
public:
    // `prover` is used here for opening a Protected section, and passed on to
    // the section editor for unticking one. Both are the same question — may
    // this session lower a Protect Communication marking — and by spec only a
    // connected device can answer it. Empty is a valid state (the app runs
    // offline by design) and means those two actions are refused with a message.
    explicit CommunicationsDialog(Configuration *config, QWidget *parent = nullptr,
                                  ProtectedCommsProver prover = {});

private:
    struct BusTab {
        QComboBox *modeCombo = nullptr;
        QComboBox *rateCombo = nullptr;
        QComboBox *fdRateCombo = nullptr;
        QComboBox *terminationCombo = nullptr;
        QTreeWidget *sectionTree = nullptr;
        QListWidget *channelList = nullptr;
        QLabel *availableLabel = nullptr;
        QPushButton *newButton = nullptr;
        QPushButton *editButton = nullptr;
        QPushButton *saveButton = nullptr;
        QPushButton *loadButton = nullptr;
        QPushButton *removeButton = nullptr;
        QPushButton *upButton = nullptr;
        QPushButton *downButton = nullptr;
        QPushButton *removeAllButton = nullptr;
    };

    // The bus's settings AS THE USER IS LOOKING AT THEM. The four combos are
    // only copied into m_buses in accept(), so a template saved mid-session
    // would otherwise record the rate the tab was opened with rather than the
    // one on screen — and a template exists to say what bus its device needs.
    BusConfig currentBusSettings(int busIndex) const;

    QWidget *buildBusTab(int busIndex);
    // The working-copy buses as a patch over the document — this dialog does
    // not write them back until OK, so anything downstream that has to know
    // what generates a channel needs this rather than the document.
    ConfigPatch liveView() const;
    void rebuildSections(int busIndex);
    void updateChannelPane(int busIndex);
    void updateButtons(int busIndex);
    // Is `section` revealed TO THIS DIALOG? Configuration::isSectionRevealed(),
    // with the pending re-conceals of rule 3 laid over it. THE one place this
    // dialog asks the question — the sections list, the channel pane, the button
    // states and the Edit path all route through it, so a section cannot be
    // padlocked in the list and still listing its channels beside it.
    bool sectionRevealed(int busIndex, const CommsSection &section) const;
    // The CURRENT section, or nullptr when the list has no selection. Still the
    // single row for the channel pane and the Edit path; use selectedRows() for
    // anything that acts on the whole selection.
    const CommsSection *selectedSection(int busIndex) const;
    // Every selected section's row, ASCENDING. The list takes shift- and
    // ctrl-click, so Remove and both Move buttons act on a set rather than on
    // the one current row, and the ascending order is what makes the move
    // algorithm correct.
    QList<int> selectedRows(int busIndex) const;
    void onNewSection(int busIndex);
    void onEditSection(int busIndex);
    // Run EVERY challenge a CONCEALED section's tier demands so its editor can be
    // opened — Configuration::proofsRequiredFor(): this section's own password
    // for Hidden, and for Protected that password AND an Edit Protected Comms
    // proof against the connected device. On success the grant is recorded on the
    // document (Configuration::grantSectionAccess), which both reveals the
    // section for the session and permits its tier to be lowered — so it must not
    // be recorded until all of them have passed. Reports its own failures; false
    // means leave the section shut.
    bool unlockConcealedSection(int busIndex, const CommsSection &section);
    // Save the SELECTED sections as a .ct3t. Refuses while the selection holds
    // a concealed message this session cannot read, for the reason
    // Configuration::saveToFile refuses the same thing: writing a message out is
    // exactly what a viewer without its password may not do, and a template is a
    // file meant to be handed on.
    void onSaveTemplate(int busIndex);
    // Append a .ct3t's messages to this bus, creating the channels they name.
    void onLoadTemplate(int busIndex);
    void onRemoveSection(int busIndex);
    void onMoveSection(int busIndex, int delta);
    void onRemoveAll(int busIndex);
    void onImportDbc(int busIndex);
    // Drop the grants rule 3 queued, now that the working copies have reached the
    // document (or been discarded). Called from both accept() and reject(),
    // because a grant that outlives this dialog is exactly what rule 3 exists to
    // prevent and a Cancel is not an exception to it.
    void flushPendingRevokes();
    void accept() override;
    void reject() override;

    Configuration *m_config;
    ProtectedCommsProver m_prover; // Protected open/untick only; may be empty
    BusConfig m_buses[3]; // working copies, committed on OK
    QTabWidget *m_tabs;
    BusTab m_busTabs[3];
    // RULE 3, and the reason it is a SET here rather than an immediate
    // Configuration::revokeSectionAccess() call.
    //
    // When a section editor closes on a section that still conceals, that section
    // must go straight back to padlocked in this dialog — that is the user's rule,
    // in their words: "do not show the channels available in the Main
    // Communications Setup afterwards". But this dialog does not write to the
    // document until OK: every edit lands in m_buses and the whole bus goes
    // through Configuration::applyBusSections in accept(). The grant is ALSO what
    // authorises a lowering there, so revoking it at editor-close time would make
    // applyBusSections refuse the very change the user was just authorised to
    // make — Protect Communication down to Hidden is exactly that shape: lowered,
    // and still concealing.
    //
    // So the name is queued: concealed on screen immediately (sectionRevealed()
    // reads this), and the document grant dropped in flushPendingRevokes() once
    // the write has happened or been abandoned. Nothing else can observe the gap
    // — this dialog is modal, so no other view is on screen to see a section it
    // still counts as revealed.
    //
    // Keys built by pendingKey(): the bus index and the lower-cased name, joined,
    // matching what Configuration's own grants are keyed on. The BUS is part of
    // it because two buses may hold a section of the same name, and a queued
    // re-conceal for one of them must not padlock the other's row.
    QSet<QString> m_pendingRevoke;
};

} // namespace ct
