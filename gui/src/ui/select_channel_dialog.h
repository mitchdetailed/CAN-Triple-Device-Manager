// "Select Channel" — searchable list of the document's (user-created)
// channels with any-order substring matching, plus New…/Edit….
#pragma once

#include <QDialog>
#include <QHash>
#include <QSet>
#include <QStringList>

#include "../model/configuration.h"

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;

namespace ct {

// Which side of a channel the picker sits on. Every call site still has to say
// which, but the two sides differ in what they DO to the channel, and the role
// decides two things: which guard rail applies, and whether New… is there at
// all.
//
//   Output — the site WRITES the channel (a receive comms row, a math /
//            condition / counter / timer / integrator / table output). The
//            device has one slot per channel, so two writers overwrite each
//            other and whichever runs last wins — picking a channel something
//            else already writes asks for confirmation. This is the ONLY
//            channel conflict the app guards against. THE CREATING SIDE:
//            New… is offered here, because a channel defined at a site that
//            writes it is filled the moment that site is saved.
//
//   Input  — the site READS the channel (a transmit comms row, a math or
//            condition input, a counter/timer trigger, an integrator input, a
//            table axis). Reading a value never alters it, so every channel in
//            the catalogue is offered, unfiltered, however many other sites
//            already read it and whether or not anything writes it yet. A
//            channel with no writer is annotated — it reads its default value
//            until something generates it — but it is never hidden, never
//            flagged as a conflict, and never blocks OK.
//
//            NO New… ON THIS SIDE. What you read has to be produced somewhere
//            first — a receive message row, a calculation, a constant — and a
//            channel invented at a picker that only reads has nothing writing
//            it: a transmit row would send its default value for ever, and a
//            math input would read that same default. The catalogue is the
//            offer, and it is the whole offer.
//
//            Not a restriction on what can be built, only on where. Tools ->
//            Channel Editor creates a channel from anywhere, and every Output
//            picker still does — so the out-of-order path is "define it at the
//            thing that generates it, then read it here", which is the order
//            the device runs in anyway.
enum class ChannelRole { Input, Output };

class SelectChannelDialog : public QDialog
{
    Q_OBJECT
public:
    // livePatch: re-states the CALLING dialog's slice of the document, which it
    // has not written back yet — "my math rows are really these", "this bus's
    // sections are really these". The picker judges what generates a channel
    // against that patched view rather than the document, so a row added
    // seconds ago counts and a row just deleted stops counting. See ConfigPatch.
    SelectChannelDialog(Configuration *config, ChannelRole role,
                        const ConfigPatch &livePatch = {}, QWidget *parent = nullptr);

    void setSelectedChannel(const QString &name);
    QString selectedChannel() const;

    // Convenience wrappers: return the picked name, or an empty string if
    // cancelled. Named rather than a role argument so a call site cannot pick
    // the wrong side by passing a bare bool.
    static QString pickInput(Configuration *config, const QString &current, QWidget *parent,
                             const ConfigPatch &livePatch = {});
    static QString pickOutput(Configuration *config, const QString &current, QWidget *parent,
                              const ConfigPatch &livePatch = {});

private:
    // `preferred` names the channel to end up highlighted, and WINS over
    // whatever the list is showing now. Callers that have just created or
    // renamed a channel must pass it: without it the rebuild keeps the old
    // highlight, because the list has not been cleared yet when the preference
    // is read. Left empty (the search box's rebuild) the current highlight is
    // kept, so typing a filter does not move the selection.
    void rebuildList(const QString &preferred = QString());
    void onNewChannel();
    void onEditChannel();
    void updateButtons();
    void onAccept();
    QString currentHighlightedName() const;

    // Re-derives the live view and everything read off it. Run again after
    // New…/Edit…, which can rename a channel and rewrite every reference to it.
    void refreshLiveView();

    bool isGenerated(const QString &name) const;
    // Where a channel is already written ("Math 1", "CAN 1 · Receive 0x640"),
    // empty when nothing writes it. Used for the Output-side duplicate-writer
    // warning and the list annotation.
    QStringList generatorsOf(const QString &name) const;

    Configuration *m_config;
    ChannelRole m_role;
    ConfigPatch m_livePatch;
    Configuration m_live;                     // m_config + m_livePatch
    QSet<QString> m_generated;                // lower-cased, off m_live
    QHash<QString, QStringList> m_generators; // lower-cased name -> where, off m_live
    QStringList m_allocated;                  // off m_live
    QString m_openedWith;                     // value the picker was opened on

    QLineEdit *m_searchEdit;
    QListWidget *m_list;
    QLabel *m_noteLabel;
    QPushButton *m_newButton = nullptr;       // Output only — see ChannelRole
    QPushButton *m_editButton;
    QPushButton *m_okButton;
    QString m_selected;
};

} // namespace ct
