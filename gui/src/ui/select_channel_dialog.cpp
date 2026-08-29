// "Select Channel" — searchable list of the document's user-created channels.

#include "select_channel_dialog.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPalette>
#include <QPushButton>
#include <QVBoxLayout>

#include "../model/config_report.h"
#include "edit_channel_dialog.h"

namespace ct {

namespace {

// Warning text for a list item, per palette. The app runs under the Windows
// dark palette in practice, where the light theme's deep orange-red would sit
// well under the AA contrast threshold against the list background.
QColor warnTextFor(const QPalette &pal)
{
    return pal.color(QPalette::Base).lightness() < 128 ? QColor(0xFF, 0xA1, 0x78)
                                                       : QColor(0xC0, 0x30, 0x00);
}

// Neutral annotation text — dimmer than the list's own text, deliberately not
// the warning colour. "Nothing writes this yet" is a fact about the channel,
// not a problem with the pick, and colouring it like a warning is what made an
// ordinary reference look forbidden.
QColor hintTextFor(const QPalette &pal)
{
    return pal.color(QPalette::Base).lightness() < 128 ? QColor(0x9A, 0xA0, 0xA6)
                                                       : QColor(0x60, 0x64, 0x68);
}

// Named lc(), not lower() — QWidget::lower() would win the overload.
QString lc(const QString &s)
{
    return s.toLower();
}

} // namespace

SelectChannelDialog::SelectChannelDialog(Configuration *config, ChannelRole role,
                                         const ConfigPatch &livePatch, QWidget *parent)
    : QDialog(parent)
    , m_config(config)
    , m_role(role)
    , m_livePatch(livePatch)
{
    setWindowTitle(role == ChannelRole::Input ? tr("Select Input Channel")
                                              : tr("Select Output Channel"));
    resize(460, 520);

    refreshLiveView();

    auto *layout = new QVBoxLayout(this);

    // Says up front what this side does to the channel, so an input never reads
    // as a restricted list and an output's one confirmation never reads as a
    // blanket ban on reusing a channel.
    auto *roleLabel = new QLabel(
        role == ChannelRole::Input
            ? tr("This is an <b>input</b>. It <b>reads</b> the channel's value and never "
                 "changes it, so any channel can be used here, in as many places as you "
                 "like — but it cannot create one. What you read has to be produced "
                 "somewhere first: a receive message row, a calculation, or a constant.")
            : tr("This is an <b>output</b>. It <b>writes</b> the channel's value. The device "
                 "has one slot per channel, so if something else already writes it the two "
                 "overwrite each other."));
    roleLabel->setWordWrap(true);
    roleLabel->setTextFormat(Qt::RichText);
    layout->addWidget(roleLabel);

    layout->addWidget(new QLabel(tr("Search text :")));
    m_searchEdit = new QLineEdit;
    const QString searchHint =
        tr("Type any part of the name — \"set\" finds \"CruiseSetSpeed\". "
           "Several words all have to appear. Regular expressions work too: "
           "^Cruise, Speed$, set|limit");
    m_searchEdit->setToolTip(searchHint);
    m_searchEdit->setPlaceholderText(tr("Search — any part of the name, or a regular expression"));
    // Through a lambda, NOT connected straight to rebuildList: textChanged
    // carries a QString, which would bind to the `preferred` parameter and make
    // every keystroke ask for a channel named after the search text.
    connect(m_searchEdit, &QLineEdit::textChanged, this, [this]() { rebuildList(); });
    layout->addWidget(m_searchEdit);

    layout->addWidget(new QLabel(tr("Channels :")));
    m_list = new QListWidget;
    connect(m_list, &QListWidget::itemSelectionChanged, this,
            &SelectChannelDialog::updateButtons);
    connect(m_list, &QListWidget::itemDoubleClicked, this, [this]() { onAccept(); });
    layout->addWidget(m_list, 1);

    m_noteLabel = new QLabel;
    m_noteLabel->setWordWrap(true);
    layout->addWidget(m_noteLabel);

    auto *buttons = new QHBoxLayout;
    // THE WRITING SIDE ONLY, and not created at all on the other one: a
    // channel invented at a picker that merely READS it has nothing filling it,
    // so a transmit row would send its default value for ever and a math input
    // would read that same default. Gone rather than grayed, for the reason the
    // CRC8 tab is gone rather than grayed — a disabled New… advertises a way to
    // define a channel here and then refuses it, and there is nothing this
    // picker could offer that would un-gray it.
    //
    // Nothing is unbuildable as a result. Tools -> Channel Editor creates a
    // channel from anywhere, and every Output picker still does, so building
    // out of order means defining the channel at the thing that GENERATES it
    // and then reading it here — which is the order the device runs in.
    if (role == ChannelRole::Output) {
        m_newButton = new QPushButton(tr("New…"));
        connect(m_newButton, &QPushButton::clicked, this, &SelectChannelDialog::onNewChannel);
        buttons->addWidget(m_newButton);
    }
    m_editButton = new QPushButton(tr("Edit…"));
    connect(m_editButton, &QPushButton::clicked, this, &SelectChannelDialog::onEditChannel);
    buttons->addWidget(m_editButton);
    buttons->addStretch();
    m_okButton = new QPushButton(tr("OK"));
    m_okButton->setDefault(true);
    connect(m_okButton, &QPushButton::clicked, this, &SelectChannelDialog::onAccept);
    buttons->addWidget(m_okButton);
    auto *cancelButton = new QPushButton(tr("Cancel"));
    connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);
    buttons->addWidget(cancelButton);
    layout->addLayout(buttons);

    m_searchEdit->setFocus();
    rebuildList();
}

void SelectChannelDialog::refreshLiveView()
{
    m_config->buildLiveView(m_live, m_livePatch);

    // What the configuration actually writes, by the same rule validation and
    // the device mapper use: active receive rows and active calculation
    // outputs. Read off the live view, so a row the calling dialog added counts
    // and one it deleted does not.
    m_generated.clear();
    for (const QString &n : m_live.generatedChannelNames())
        m_generated.insert(lc(n));
    m_generators = analyzeChannelUsage(m_live).generators;
    m_allocated = m_live.allocatedChannelNames();
}

bool SelectChannelDialog::isGenerated(const QString &name) const
{
    return m_generated.contains(lc(name));
}

QStringList SelectChannelDialog::generatorsOf(const QString &name) const
{
    // De-duplicated, the way validation counts writers: a compound section may
    // legitimately define the same channel in several identifiers (one per
    // multiplexor value), and that is ONE writer, not four. Without this the
    // picker would flag a perfectly ordinary multiplexed message as a conflict
    // and name the same section over and over.
    QStringList where;
    for (const QString &w : m_generators.value(lc(name)))
        if (!where.contains(w))
            where.append(w);
    return where;
}

void SelectChannelDialog::rebuildList(const QString &preferred)
{
    QString previous = preferred;
    if (previous.isEmpty())
        previous = currentHighlightedName().isEmpty() ? m_selected : currentHighlightedName();
    m_list->clear();

    const bool inputRole = m_role == ChannelRole::Input;
    const QStringList allocated = inputRole ? QStringList() : m_allocated;
    const QBrush warn(warnTextFor(palette()));
    const QBrush hint(hintTextFor(palette()));

    // Every channel in the catalogue is listed, on both sides. Nothing is
    // filtered out: an input reads the channel, and reading it cannot clash
    // with anything, so there is no set of channels it would be wrong to offer.
    for (const Channel &c : m_config->catalog().search(m_searchEdit->text())) {
        // Name AND unit. Identity travels in Qt::UserRole below, which is what
        // currentHighlightedName() and everything downstream read — the visible
        // text is never parsed back into a name.
        QString text = channelLabel(c);
        bool conflict = false; // warning colour — two writers, the one real clash
        bool dim = false;      // neutral annotation
        if (inputRole) {
            if (!isGenerated(c.name)) {
                text += tr(" — no generator yet, reads its default value");
                dim = true;
            }
        } else {
            const QStringList by = generatorsOf(c.name);
            if (!by.isEmpty()) {
                text += tr(" — already written by %1").arg(by.join(QStringLiteral(", ")));
                conflict = true;
            } else if (allocated.contains(c.name, Qt::CaseInsensitive)) {
                text += tr(" (allocated)");
            }
        }

        auto *item = new QListWidgetItem(text, m_list);
        item->setData(Qt::UserRole, c.name);
        if (conflict)
            item->setForeground(warn);
        else if (dim)
            item->setForeground(hint);
        if (!previous.isEmpty() && c.name.compare(previous, Qt::CaseInsensitive) == 0)
            m_list->setCurrentItem(item);
    }
    if (!m_list->currentItem() && m_list->count() > 0)
        m_list->setCurrentRow(0);

    updateButtons();
}

QString SelectChannelDialog::currentHighlightedName() const
{
    auto *item = m_list->currentItem();
    return (item && item->isSelected()) ? item->data(Qt::UserRole).toString() : QString();
}

void SelectChannelDialog::setSelectedChannel(const QString &name)
{
    m_selected = name;
    m_openedWith = name;
    rebuildList(name);
}

QString SelectChannelDialog::selectedChannel() const
{
    return m_selected;
}

void SelectChannelDialog::onAccept()
{
    const QString name = currentHighlightedName();
    if (name.isEmpty())
        return;

    // The one confirmation the picker asks for, and only on the write side: two
    // writers share one device slot and overwrite each other, so the value
    // everything else reads depends on evaluation order. Re-picking the value
    // the site already had is not a new conflict — that writer is this very
    // site. An input never asks anything, however many places read the channel.
    if (m_role == ChannelRole::Output
        && name.compare(m_openedWith, Qt::CaseInsensitive) != 0) {
        const QStringList by = generatorsOf(name);
        if (!by.isEmpty()) {
            const auto answer = QMessageBox::warning(
                this, windowTitle(),
                tr("\"%1\" is already written by %2.\n\n"
                   "Both would write the same channel slot on the device and overwrite each "
                   "other, so whichever runs last wins. Check Channels reports this as a "
                   "warning.\n\n"
                   "(Reading \"%1\" somewhere else — transmitting it, or using it as a "
                   "calculation input — is not affected by this; only writing it twice is.)\n\n"
                   "Use it as this output anyway?")
                    .arg(name, by.join(QStringLiteral(", "))),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (answer != QMessageBox::Yes)
                return;
        }
    }

    m_selected = name;
    accept();
}

void SelectChannelDialog::onNewChannel()
{
    if (m_role != ChannelRole::Output)
        return; // no button on the read side; no other path may define one either
    const QString created = EditChannelDialog::createOrEdit(m_config, Channel{}, true, this);
    if (created.isEmpty())
        return;
    m_selected = created;
    refreshLiveView();
    rebuildList(created);
    // Defining a channel from inside the picker IS the choice — you opened the
    // picker to pick one, and there was none to pick, so you made it. Asking
    // for OK as well would be confirming a decision that was just made, on a
    // list where the answer is already highlighted. Through onAccept() rather
    // than accept() so there is one acceptance path: a brand-new channel cannot
    // have another writer today, but nothing here has to know that.
    onAccept();
}

void SelectChannelDialog::onEditChannel()
{
    const QString name = currentHighlightedName();
    if (name.isEmpty())
        return;
    const Channel channel = m_config->catalog().findByName(name);
    if (!channel.isValid())
        return;
    const QString edited = EditChannelDialog::createOrEdit(m_config, channel, false, this);
    if (edited.isEmpty())
        return;
    // A rename rewrote every reference in the document, so the live view (and
    // the name this picker opened on) must follow.
    if (!m_openedWith.isEmpty() && m_openedWith.compare(name, Qt::CaseInsensitive) == 0)
        m_openedWith = edited;
    m_selected = edited;
    refreshLiveView();
    // By name, so a RENAME keeps the row selected. The old name is what is still
    // highlighted and it no longer exists in the rebuilt list, so without this
    // the selection would fall back to the top of the list. Unlike New this does
    // not accept: editing a channel is not choosing it.
    rebuildList(edited);
}

void SelectChannelDialog::updateButtons()
{
    const QString name = currentHighlightedName();
    const bool haveChannel = !name.isEmpty();
    m_editButton->setEnabled(haveChannel);
    m_okButton->setEnabled(haveChannel);

    // Live explanation of whatever is highlighted, so the consequence of the
    // pick is visible before OK rather than at Check Channels time. Only the
    // two-writer case is a warning; the input-side note is plain information
    // and is styled to match.
    QString note;
    bool isWarning = false;
    if (!haveChannel) {
        note.clear();
    } else if (m_role == ChannelRole::Input) {
        if (!isGenerated(name))
            note = tr("\"%1\" has no generator yet, so it reads its default value until a "
                      "receive comms row or a calculation writes it. Reading it here is fine "
                      "and changes nothing.")
                       .arg(name);
    } else {
        const QStringList by = generatorsOf(name);
        if (!by.isEmpty() && name.compare(m_openedWith, Qt::CaseInsensitive) != 0) {
            note = tr("⚠ \"%1\" is already written by %2 — both would write the same slot "
                      "and overwrite each other.")
                       .arg(name, by.join(QStringLiteral(", ")));
            isWarning = true;
        }
    }
    m_noteLabel->setStyleSheet(
        note.isEmpty()
            ? QString()
            : QStringLiteral("color: %1;")
                  .arg(isWarning ? warnTextFor(palette()).name() : hintTextFor(palette()).name()));
    m_noteLabel->setText(note);
}

QString SelectChannelDialog::pickInput(Configuration *config, const QString &current,
                                       QWidget *parent, const ConfigPatch &livePatch)
{
    SelectChannelDialog dialog(config, ChannelRole::Input, livePatch, parent);
    dialog.setSelectedChannel(current);
    if (dialog.exec() != QDialog::Accepted)
        return {};
    return dialog.selectedChannel();
}

QString SelectChannelDialog::pickOutput(Configuration *config, const QString &current,
                                        QWidget *parent, const ConfigPatch &livePatch)
{
    SelectChannelDialog dialog(config, ChannelRole::Output, livePatch, parent);
    dialog.setSelectedChannel(current);
    if (dialog.exec() != QDialog::Accepted)
        return {};
    return dialog.selectedChannel();
}

} // namespace ct
