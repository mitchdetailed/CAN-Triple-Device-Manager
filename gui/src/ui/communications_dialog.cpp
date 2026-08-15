#include "communications_dialog.h"

#include <array>

#include <QBrush>
#include <QColor>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPalette>
#include <QPushButton>
#include <QStandardItemModel>
#include <QTabWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>

#include "../model/channel_catalog.h"
#include "../model/dbc_import.h"
#include "../protocol/wire_structs.h"
#include "color_item_delegate.h"
#include "import_dbc_dialog.h"
#include "section_editor_dialog.h"

namespace ct {

namespace {

// The Section column: normally the direction, a padlock for a message the
// viewer may not read. MoTeC's own list does exactly this, and the substitution
// is deliberate — the row's job for a concealed message is to say "locked",
// which is also why opening it asks for a password. The direction is not itself
// the secret (the Config Summary report still names it, see
// CommsSection::displayDetail); it is simply displaced by the thing the user
// needs to know first.
//
// Read Only is the tier that changed. It conceals NOTHING, so it keeps its
// direction and gains a lock-with-pen marker instead: the message is fully
// legible and simply cannot be edited. Hidden and Protected share the plain
// padlock — which of the two it is, and therefore which password opens it, is
// said in the Name column by sectionSummary, where there is room to say it in
// words rather than by asking the reader to tell two glyphs apart.
QString sectionKind(const CommsSection &s, bool revealed)
{
    if (s.isConcealed(revealed))
        return QStringLiteral("🔒");
    QString kind;
    switch (s.device) {
    case SectionDevice::ReceiveMessage: kind = QObject::tr("CAN Rx"); break;
    case SectionDevice::TransmitMessage: kind = QObject::tr("CAN Tx"); break;
    case SectionDevice::TransmitCrc8: kind = QObject::tr("CAN Tx CRC8"); break;
    case SectionDevice::MessageRelay: kind = QObject::tr("Relay"); break;
    case SectionDevice::Off: kind = QObject::tr("Off"); break;
    }
    if (s.isEditLocked())
        kind += QStringLiteral("  🔏");
    return kind;
}

QString sectionSummary(const CommsSection &s, bool revealed)
{
    const QString address = QStringLiteral("0x") + QString::number(s.baseAddress, 16).toUpper();
    // A concealed message shows its name and nothing else — no ID, no extended
    // marker, no compound tag. Note the name itself is the user's to choose:
    // the default "Receive 0x640" would give the ID away, so the section editor
    // warns about that when the box is ticked.
    //
    // Hidden and Protected are named apart rather than both reading
    // "(protected)". They are not the same promise: Hidden is opened by this
    // section's own password, held in this document, while Protected wants that
    // password AND a live device confirming Edit Protected Comms. A viewer
    // deciding whether it is worth going to find a password has to be told which
    // of the two they are looking at.
    //
    // A section with NO Message Password is named apart AGAIN, for the same
    // reason one step further on: there is no password anywhere for it, so a row
    // reading "(hidden)" would send its reader looking for something that does
    // not exist — through their notes, their colleagues and the original .ct3 —
    // and every one of those searches would fail without ever explaining why.
    // This is the ordinary state of a configuration read back off a device: the
    // wire carries no key, so a Get returns every section keyless.
    if (s.isConcealed(revealed)) {
        if (s.messageKey == kNoAccessKey) {
            return s.protection == CommsProtection::Protected
                       ? QObject::tr("%1  (protected — no password)").arg(s.name)
                       : QObject::tr("%1  (hidden — no password)").arg(s.name);
        }
        return s.protection == CommsProtection::Protected
                   ? QObject::tr("%1  (protected)").arg(s.name)
                   : QObject::tr("%1  (hidden)").arg(s.name);
    }
    QString text = s.name;
    if (!s.name.contains(address, Qt::CaseInsensitive))
        text += QStringLiteral("  —  ") + address;
    if (s.extended)
        text += QStringLiteral(" x");
    if (s.compound && !s.isRelay())
        text += QObject::tr("  (compound)");
    if (s.isRelay()) {
        QStringList buses;
        for (int i = 0; i < 3; ++i)
            if (s.routeBusMask & (1 << i))
                buses << QStringLiteral("CAN %1").arg(i + 1);
        text += QObject::tr("  → %1").arg(buses.isEmpty() ? QObject::tr("(no target)")
                                                          : buses.join(QStringLiteral(", ")));
    }
    return text;
}

// Mode colours per palette. Picked numerically, not by eye: in every state the
// delegate can paint (resting / hover / selected) the fill clears the WCAG AA
// 4.5:1 threshold against its own text, and stays distinguishable from both the
// popup background and the neighbouring combos. The dark set is the one that
// matters in practice — this app runs under the Windows dark palette — but the
// light set keeps it correct if that ever changes.
struct ModeColors
{
    QColor canFill, canText, offFill, offText;
};

ModeColors modeColorsFor(const QPalette &pal)
{
    if (pal.color(QPalette::Window).lightness() < 128)
        return {QColor(0x2A, 0x55, 0x33), QColor(0xF1, 0xFA, 0xF1),
                QColor(0x7E, 0x2C, 0x2F), QColor(0xFF, 0xEB, 0xEE)};
    return {QColor(0xC8, 0xE6, 0xC9), QColor(0x14, 0x47, 0x18),
            QColor(0xFF, 0xCD, 0xD2), QColor(0x8C, 0x15, 0x15)};
}

} // namespace

CommunicationsDialog::CommunicationsDialog(Configuration *config, QWidget *parent,
                                           ProtectedCommsProver prover)
    : QDialog(parent)
    , m_config(config)
    , m_prover(std::move(prover))
{
    setWindowTitle(tr("Communications Setup"));
    resize(720, 480);

    for (int i = 0; i < 3; ++i)
        m_buses[i] = config->bus[i];

    auto *layout = new QVBoxLayout(this);
    m_tabs = new QTabWidget;
    for (int i = 0; i < 3; ++i)
        m_tabs->addTab(buildBusTab(i), tr("CAN %1").arg(i + 1));
    layout->addWidget(m_tabs);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &CommunicationsDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    // A rename can arrive from deep inside a section editor's picker while
    // these working copies are open — see Configuration::channelRenamed. It
    // also keeps accept()'s per-bus toJson comparison honest: a bus whose only
    // difference was the rename stays identical to the document and is
    // skipped, rather than writing the stale names back through
    // applyBusSections.
    connect(m_config, &Configuration::channelRenamed, this,
            [this](const QString &oldName, const QString &newName) {
                for (int i = 0; i < 3; ++i)
                    if (renameChannelRefs(m_buses[i].sections, oldName, newName) > 0)
                        rebuildSections(i);
            });

    for (int i = 0; i < 3; ++i) {
        rebuildSections(i);
        updateButtons(i);
    }
}

QWidget *CommunicationsDialog::buildBusTab(int busIndex)
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    BusTab &tab = m_busTabs[busIndex];

    // Options row
    auto *optionsGroup = new QGroupBox(tr("Options"));
    auto *optionsRow = new QHBoxLayout(optionsGroup);
    optionsRow->addWidget(new QLabel(tr("Mode :")));
    tab.modeCombo = new QComboBox;
    tab.modeCombo->addItem(tr("CAN"), true);
    tab.modeCombo->addItem(tr("Off"), false);
    // Colour the mode so a disabled bus is obvious at a glance: green = running,
    // red = off. Applied to the dropdown entries AND to the closed combo, since
    // otherwise the state is only visible while the list is open.
    const ModeColors mode = modeColorsFor(palette());
    tab.modeCombo->setItemData(0, QBrush(mode.canFill), Qt::BackgroundRole);
    tab.modeCombo->setItemData(0, QBrush(mode.canText), Qt::ForegroundRole);
    tab.modeCombo->setItemData(1, QBrush(mode.offFill), Qt::BackgroundRole);
    tab.modeCombo->setItemData(1, QBrush(mode.offText), Qt::ForegroundRole);
    // Without this the popup drops back to the plain highlight under the cursor,
    // losing the colour exactly when the user is pointing at the row.
    tab.modeCombo->setItemDelegate(new ColorItemDelegate(tab.modeCombo));
    tab.modeCombo->setCurrentIndex(m_buses[busIndex].enabled ? 0 : 1);
    const auto paintMode = [combo = tab.modeCombo, mode] {
        const bool on = combo->currentData().toBool();
        const QColor fill = on ? mode.canFill : mode.offFill;
        const QColor text = on ? mode.canText : mode.offText;
        // A background-color rule stops QStyleSheetStyle delegating this combo to
        // the platform style, so the frame the native style would have drawn has
        // to be supplied here — otherwise it reads as a bare colour block next to
        // its unstyled siblings in the same row.
        // The 3px horizontal padding is not cosmetic: without it the styled combo
        // measures 42px against its native siblings' 48px and sits visibly narrow
        // in the row. Vertical padding only steps in 2s, so 1px short is the
        // closest match available and is invisible on a vertically-centred combo.
        combo->setStyleSheet(
            QStringLiteral("QComboBox { background-color: %1; color: %2; "
                           "border: 1px solid %3; border-radius: 3px; padding: 0px 3px; }")
                .arg(fill.name(), text.name(),
                     ColorItemDelegate::shade(fill, ColorItemDelegate::kSelectedShift).name()));
    };
    connect(tab.modeCombo, &QComboBox::currentIndexChanged, this,
            [paintMode](int) { paintMode(); });
    paintMode();
    optionsRow->addWidget(tab.modeCombo);
    optionsRow->addSpacing(16);
    optionsRow->addWidget(new QLabel(tr("Rate :")));
    tab.rateCombo = new QComboBox;
    tab.rateCombo->addItem(tr("1M"), 1000);
    tab.rateCombo->addItem(tr("800k"), 800);
    tab.rateCombo->addItem(tr("500k"), 500);
    tab.rateCombo->addItem(tr("250k"), 250);
    tab.rateCombo->addItem(tr("200k"), 200);
    tab.rateCombo->addItem(tr("125k"), 125);
    tab.rateCombo->addItem(tr("100k"), 100);
    // Stored as 83, sent as 83,333 Hz: GMLAN's low-speed rate is 1 Mbit / 12,
    // and 83,000 would be 0.4% off the bus. See ct::busRateHz.
    tab.rateCombo->addItem(tr("83.3k"), 83);
    tab.rateCombo->addItem(tr("50k"), 50);
    tab.rateCombo->setCurrentIndex(qMax(0, tab.rateCombo->findData(m_buses[busIndex].rateKbps)));
    optionsRow->addWidget(tab.rateCombo);
    optionsRow->addSpacing(16);
    optionsRow->addWidget(new QLabel(tr("FD Data :")));
    tab.fdRateCombo = new QComboBox;
    tab.fdRateCombo->addItem(tr("Off (classic)"), 0);
    tab.fdRateCombo->addItem(tr("1M"), 1000);
    tab.fdRateCombo->addItem(tr("2M"), 2000);
    // 4M and 5M left this menu before anything shipped with them. A file that
    // still says one means "the fastest FD this build offers", not "classic" —
    // falling to index 0 would silently strip FD from every section using it.
    int fdIdx = tab.fdRateCombo->findData(m_buses[busIndex].dataRateKbps);
    if (fdIdx < 0 && m_buses[busIndex].dataRateKbps > 0)
        fdIdx = tab.fdRateCombo->findData(2000);
    tab.fdRateCombo->setCurrentIndex(qMax(0, fdIdx));
    // The device runs FD only when the data rate EXCEEDS the base rate — a
    // data choice at or below it is not a slower FD, it is classic wearing an
    // FD label. Those entries go dark as the base rate moves, and a selection
    // stranded by the move slides to 2M, the one rate faster than every base
    // this menu offers.
    {
        QComboBox *rateCombo = tab.rateCombo;
        QComboBox *fdCombo = tab.fdRateCombo;
        const auto updateFdChoices = [rateCombo, fdCombo]() {
            const int base = rateCombo->currentData().toInt();
            auto *model = qobject_cast<QStandardItemModel *>(fdCombo->model());
            for (int i = 0; model && i < fdCombo->count(); ++i) {
                const int fd = fdCombo->itemData(i).toInt();
                model->item(i)->setEnabled(fd == 0 || fd > base);
            }
            const int cur = fdCombo->currentData().toInt();
            if (cur > 0 && cur <= base)
                fdCombo->setCurrentIndex(qMax(0, fdCombo->findData(2000)));
        };
        connect(tab.rateCombo, &QComboBox::currentIndexChanged, this, updateFdChoices);
        updateFdChoices();
    }
    optionsRow->addWidget(tab.fdRateCombo);
    optionsRow->addSpacing(16);
    optionsRow->addWidget(new QLabel(tr("Termination Resistor :")));
    tab.terminationCombo = new QComboBox;
    tab.terminationCombo->addItem(tr("Off"), false);
    tab.terminationCombo->addItem(tr("On"), true);
    tab.terminationCombo->setCurrentIndex(m_buses[busIndex].termination ? 1 : 0);
    tab.terminationCombo->setToolTip(tr("Enable this bus's 120Ω termination resistor "
                                        "(applied on Send Configuration — firmware v9)"));
    optionsRow->addWidget(tab.terminationCombo);
    auto *rateNote = new QLabel(tr("(applied on Send Configuration — firmware v2)"));
    rateNote->setStyleSheet(QStringLiteral("color: gray;"));
    optionsRow->addWidget(rateNote);
    optionsRow->addStretch();
    layout->addWidget(optionsGroup);

    // Sections + buttons + channels
    auto *middle = new QHBoxLayout;

    auto *sectionsColumn = new QVBoxLayout;
    auto *sectionsLabel = new QLabel(tr("Sections :  (list order = transmit order)"));
    sectionsLabel->setToolTip(tr("Transmit messages are pushed to the bus in this order, "
                                 "top first. Use Move Up / Move Down to reorder."));
    sectionsColumn->addWidget(sectionsLabel);
    tab.sectionTree = new QTreeWidget;
    tab.sectionTree->setHeaderLabels({tr("Section"), tr("Name")});
    tab.sectionTree->setRootIsDecorated(false);
    tab.sectionTree->setColumnWidth(0, 90);
    // Shift-click for a run, ctrl-click to add or drop one. Reordering and
    // deleting a group of messages is the ordinary case once a DBC import has
    // dropped thirty of them in at once, and doing it one row at a time is the
    // kind of tedium that gets a configuration wrong.
    tab.sectionTree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    tab.sectionTree->setSelectionBehavior(QAbstractItemView::SelectRows);
    connect(tab.sectionTree, &QTreeWidget::currentItemChanged, this, [this, busIndex]() {
        updateChannelPane(busIndex);
        updateButtons(busIndex);
    });
    // Selection can change without the CURRENT row changing — ctrl-clicking a
    // second row, or shift-extending downward — and the buttons have to follow
    // the selection, not the cursor.
    connect(tab.sectionTree, &QTreeWidget::itemSelectionChanged, this,
            [this, busIndex]() { updateButtons(busIndex); });
    connect(tab.sectionTree, &QTreeWidget::itemDoubleClicked, this,
            [this, busIndex]() { onEditSection(busIndex); });
    sectionsColumn->addWidget(tab.sectionTree, 1);
    tab.availableLabel = new QLabel;
    sectionsColumn->addWidget(tab.availableLabel);
    middle->addLayout(sectionsColumn, 3);

    auto *buttonColumn = new QVBoxLayout;
    auto *selectButton = new QPushButton(tr("Select…"));
    selectButton->setEnabled(false);
    selectButton->setToolTip(tr("Predefined device templates — planned"));
    buttonColumn->addWidget(selectButton);
    auto *importButton = new QPushButton(tr("Import DBC…"));
    importButton->setToolTip(tr("Import messages and signals from a .dbc file"));
    connect(importButton, &QPushButton::clicked, this, [this, busIndex]() { onImportDbc(busIndex); });
    buttonColumn->addWidget(importButton);
    tab.newButton = new QPushButton(tr("New…"));
    connect(tab.newButton, &QPushButton::clicked, this, [this, busIndex]() { onNewSection(busIndex); });
    buttonColumn->addWidget(tab.newButton);
    tab.editButton = new QPushButton(tr("Edit…"));
    connect(tab.editButton, &QPushButton::clicked, this, [this, busIndex]() { onEditSection(busIndex); });
    buttonColumn->addWidget(tab.editButton);
    tab.removeButton = new QPushButton(tr("Remove"));
    connect(tab.removeButton, &QPushButton::clicked, this,
            [this, busIndex]() { onRemoveSection(busIndex); });
    buttonColumn->addWidget(tab.removeButton);
    // Section order is the transmit order: on each tick the firmware composes
    // and enqueues its transmit messages in list order (top first), so moving a
    // message up sends it earlier.
    const QString orderTip = tr("Messages are transmitted in list order — the top "
                                "section is pushed to the bus first each cycle.");
    tab.upButton = new QPushButton(tr("↑ Move Up"));
    tab.upButton->setToolTip(orderTip);
    connect(tab.upButton, &QPushButton::clicked, this,
            [this, busIndex]() { onMoveSection(busIndex, -1); });
    buttonColumn->addWidget(tab.upButton);
    tab.downButton = new QPushButton(tr("↓ Move Down"));
    tab.downButton->setToolTip(orderTip);
    connect(tab.downButton, &QPushButton::clicked, this,
            [this, busIndex]() { onMoveSection(busIndex, +1); });
    buttonColumn->addWidget(tab.downButton);
    tab.removeAllButton = new QPushButton(tr("Remove All"));
    connect(tab.removeAllButton, &QPushButton::clicked, this,
            [this, busIndex]() { onRemoveAll(busIndex); });
    buttonColumn->addWidget(tab.removeAllButton);
    buttonColumn->addStretch();
    middle->addLayout(buttonColumn, 0);

    auto *channelsColumn = new QVBoxLayout;
    channelsColumn->addWidget(new QLabel(tr("Channels :")));
    tab.channelList = new QListWidget;
    tab.channelList->setSelectionMode(QAbstractItemView::NoSelection);
    channelsColumn->addWidget(tab.channelList, 1);
    middle->addLayout(channelsColumn, 2);

    layout->addLayout(middle, 1);
    return page;
}

// The document's verdict with rule 3's pending re-conceals over the top. Every
// display decision in this dialog asks THIS rather than the document directly, so
// a section cannot come back padlocked in the list and still list its channels in
// the pane beside it. See m_pendingRevoke for why the grant is still live on the
// document while this already answers false.
// The queue key, and it carries the BUS for the same reason the document's grants
// do: two buses may hold a section of the same name, and a pending re-conceal for
// one of them must not padlock the other's row.
static QString pendingKey(int busIndex, const QString &name)
{
    return QStringLiteral("%1/%2").arg(busIndex).arg(name.toLower());
}

bool CommunicationsDialog::sectionRevealed(int busIndex, const CommsSection &section) const
{
    if (m_pendingRevoke.contains(pendingKey(busIndex, section.name)))
        return false;
    // The bus goes to the document too. This dialog always knows which bus a row
    // is on, so a grant taken for a same-named section on another bus must not
    // unlock this row's channel pane.
    return m_config->isSectionRevealed(section, busIndex);
}

void CommunicationsDialog::rebuildSections(int busIndex)
{
    BusTab &tab = m_busTabs[busIndex];
    const int previousRow = tab.sectionTree->indexOfTopLevelItem(tab.sectionTree->currentItem());
    tab.sectionTree->clear();
    // Asked per SECTION, not once for the document: a section unlocked with its
    // own password this session is revealed while its neighbours stay shut.
    for (const CommsSection &s : m_buses[busIndex].sections) {
        const bool revealed = sectionRevealed(busIndex, s);
        auto *item = new QTreeWidgetItem(tab.sectionTree);
        item->setText(0, sectionKind(s, revealed));
        item->setText(1, sectionSummary(s, revealed));
    }
    if (tab.sectionTree->topLevelItemCount() > 0) {
        const int row = qBound(0, previousRow, tab.sectionTree->topLevelItemCount() - 1);
        tab.sectionTree->setCurrentItem(tab.sectionTree->topLevelItem(row));
    }

    // Count what the label CLAIMS to count: sections that consume a slot in
    // the device's 500-entry message table. The mapper never appends an Off
    // section (it configures nothing) or a Relay (relays live in their own
    // 32-rule table), so counting every section read high by exactly those
    // rows and contradicted the mapper this number is meant to summarise.
    int totalMessages = 0;
    for (const auto &bus : m_buses)
        for (const CommsSection &s : bus.sections)
            if (s.device != SectionDevice::Off && !s.isRelay())
                ++totalMessages;
    tab.availableLabel->setText(tr("%1 of %2 device messages used")
                                    .arg(totalMessages)
                                    .arg(MAX_MESSAGES));
    updateChannelPane(busIndex);
}

void CommunicationsDialog::updateChannelPane(int busIndex)
{
    BusTab &tab = m_busTabs[busIndex];
    tab.channelList->clear();
    const CommsSection *selected = selectedSection(busIndex);
    if (!selected)
        return;
    const CommsSection &s = *selected;
    if (s.isRelay()) {
        tab.channelList->addItem(tr("(relay — forwards whole frames, no channels)"));
        return;
    }
    // One line and nothing else, which is what MoTeC shows and what the flag is
    // for. Channel NAMES are not the secret and stay visible everywhere they are
    // USED — the Channel Editor, math and condition inputs, transmit rows — but
    // this pane is the one place they would be listed BY MESSAGE, and that
    // grouping is protocol detail in its own right: it says which signals share
    // a frame, and in compound mode which multiplexor value each belongs to.
    // Listing them here would hand back a good part of the frame's shape.
    //
    // Read Only lists its channels normally. It withholds nothing — that is the
    // entire difference between it and Hidden — and blanking this pane for it
    // would be the v21 behaviour this release exists to undo.
    if (s.isConcealed(sectionRevealed(busIndex, s))) {
        tab.channelList->addItem(tr("(Channel information locked)"));
        return;
    }
    // Names are shown with their unit. This pane is display only — the list is
    // NoSelection, it is cleared and rebuilt from the sections on every change,
    // and nothing ever reads an item's text back — so the decorated string has
    // nowhere to leak into. The row's `channelName` remains the identity.
    const ChannelCatalog &catalog = m_config->catalog();
    if (s.compound) {
        for (int i = 0; i < s.identifiers.size(); ++i) {
            const CompoundIdentifier &ident = s.identifiers[i];
            if (ident.rows.isEmpty())
                continue;
            tab.channelList->addItem(tr("— ID %1 (0x%2) —")
                                         .arg(i + 1)
                                         .arg(QString::number(ident.id, 16).toUpper()));
            for (const CommsChannelRow &r : ident.rows)
                tab.channelList->addItem(QStringLiteral("    ") + catalog.labelFor(r.channelName));
        }
    } else {
        for (const CommsChannelRow &r : s.rows)
            tab.channelList->addItem(catalog.labelFor(r.channelName));
    }
    if (tab.channelList->count() == 0)
        tab.channelList->addItem(tr("(no channels)"));
}

const CommsSection *CommunicationsDialog::selectedSection(int busIndex) const
{
    const BusTab &tab = m_busTabs[busIndex];
    const int row = tab.sectionTree->indexOfTopLevelItem(tab.sectionTree->currentItem());
    if (row < 0 || row >= m_buses[busIndex].sections.size())
        return nullptr;
    return &m_buses[busIndex].sections.at(row);
}

void CommunicationsDialog::updateButtons(int busIndex)
{
    BusTab &tab = m_busTabs[busIndex];
    const int count = m_buses[busIndex].sections.size();
    const QList<int> rows = selectedRows(busIndex);
    const CommsSection *selected = selectedSection(busIndex);
    const bool haveRow = selected != nullptr;
    const bool concealed =
        haveRow && selected->isConcealed(sectionRevealed(busIndex, *selected));

    // Edit stays LIVE for a concealed message now, and that is the change. It
    // used to be greyed out with a tooltip naming a menu item, which meant the
    // one control that could plausibly ask for the password was the one control
    // that refused to do anything. Pressing it now runs the challenge the
    // section's own tier demands (see unlockConcealedSection) and opens the
    // editor if it is met — the password prompt belongs on the thing being
    // unlocked.
    //
    // It still needs exactly ONE row: the editor opens a single section, and
    // there is no sensible section for it to open out of several.
    const bool single = rows.size() == 1;
    tab.editButton->setEnabled(single && haveRow);
    tab.editButton->setToolTip(
        !single && rows.size() > 1
            ? tr("Select a single message to edit it.")
            : (concealed
                   // Three states, not two. A concealed section with NO Message
                   // Password cannot be opened by anyone, so the tooltip says
                   // that instead of promising a prompt that would only be able
                   // to refuse — and names the things that DO work, since "Edit
                   // does nothing" with no reason given is the worst of the
                   // available answers.
                   //
                   // "and cannot be given one" is spelled out because the obvious
                   // guess is wrong in a way that costs time: giving a keyless
                   // section a first password IS the repair, and it IS free, but
                   // it happens in the editor — and the editor is the thing this
                   // tier will not open. That route exists only at Read Only,
                   // which is not this branch. Say so here rather than let
                   // someone hunt for it.
                   ? (selected->messageKey == kNoAccessKey
                          ? tr("\"%1\" is marked, and it arrived without a Message Password — "
                               "from a configuration read back off a device, or from a file "
                               "written before markings carried passwords. No password for it "
                               "exists, so it cannot be opened, and it cannot be given one "
                               "either — that is done in the editor, and the editor will not "
                               "open. It can be removed, and it can be reordered and sent.")
                                .arg(selected->name)
                          : selected->protection == CommsProtection::Protected
                          ? tr("\"%1\" is marked Protect Communication. Opening it asks for this "
                               "section's own Message Password AND for the Edit Protected Comms "
                               "password, which is checked against the connected device.")
                                .arg(selected->name)
                          : tr("\"%1\" is hidden. Opening it asks for this section's own "
                               "password.")
                                .arg(selected->name))
                   // Read Only, and it gets TWO states for the same reason the
                   // concealing branch above does. A keyless Read Only section
                   // opens — it conceals nothing, so there is nothing to unlock —
                   // but the untick inside it is refused by
                   // Configuration::maySectionLower, which fails closed when there
                   // is no password. "Untick Read Only inside to change it" is
                   // then an instruction that cannot be carried out. Unlike the
                   // concealing tiers this one HAS a repair, because the editor
                   // does open, so the tooltip gives it.
                   : (haveRow && selected->isEditLocked()
                          ? (selected->messageKey == kNoAccessKey
                                 ? tr("\"%1\" is Read Only and arrived without a Message "
                                      "Password, so there is nothing that could authorise "
                                      "unticking it. Open it, type a Message Password and save; "
                                      "unticking Read Only then asks for that password on the "
                                      "next visit.")
                                       .arg(selected->name)
                                 : tr("\"%1\" is Read Only: it opens with every field shown and "
                                      "none of them editable. Untick Read Only inside to change "
                                      "it.")
                                       .arg(selected->name))
                          : QString())));
    // Remove deliberately stays live AT EVERY TIER, and that is now the spec
    // rather than this file's own judgement — Read Only, Hidden and Protected
    // all permit removal, and nothing in the host or the firmware refuses it.
    // Protecting a message protects its protocol, not its place in the
    // customer's configuration: someone who cannot read a supplier's message may
    // still have every reason to delete it, and refusing would leave a locked
    // message impossible to be rid of short of editing the .ct3 by hand.
    // Reordering is the same argument, so Move Up / Move Down are untouched too.
    //
    // The consequence is stated honestly in the tooltips and the help rather
    // than papered over: for Read Only, remove-and-retype reproduces the message
    // without the password, which is exactly why Read Only is described as
    // accident prevention and never as security. For Hidden and Protected the
    // same sequence DESTROYS rather than reveals, because the operator cannot
    // see what to retype — which is the substantive difference between the
    // tiers.
    //
    // All three act on the whole selection. The two Move buttons go dead when
    // the selection already touches the end it would travel towards, because the
    // group moves as a unit — see onMoveSection.
    tab.removeButton->setEnabled(!rows.isEmpty());
    tab.upButton->setEnabled(!rows.isEmpty() && rows.first() > 0);
    tab.downButton->setEnabled(!rows.isEmpty() && rows.last() < count - 1);
    tab.removeAllButton->setEnabled(count > 0);
}

ConfigPatch CommunicationsDialog::liveView() const
{
    // By value: the section editor holds the patch for as long as it is open.
    std::array<BusConfig, 3> buses{m_buses[0], m_buses[1], m_buses[2]};
    return [buses](Configuration &c) {
        for (int b = 0; b < 3; ++b)
            c.bus[b] = buses[b];
    };
}

void CommunicationsDialog::onNewSection(int busIndex)
{
    CommsSection section;
    section.device = SectionDevice::ReceiveMessage;
    SectionEditorDialog dialog(m_config, section, busIndex, liveView(), /*sectionIndex=*/-1,
                               this, m_prover,
                               m_busTabs[busIndex].fdRateCombo->currentData().toInt());
    if (dialog.exec() != QDialog::Accepted)
        return;
    m_buses[busIndex].sections.append(dialog.section());
    rebuildSections(busIndex);
    m_busTabs[busIndex].sectionTree->setCurrentItem(m_busTabs[busIndex].sectionTree->topLevelItem(
        m_buses[busIndex].sections.size() - 1));
    updateButtons(busIndex);
}

// EVERY challenge a CONCEALED section's tier demands, before its editor will
// open. Which ones apply is Configuration::proofsRequiredFor()'s answer and
// nothing this function decides for itself:
//
//   Hidden      this section's own password, checked here against the document.
//               No device, by design — Hidden is the tier that never names one.
//   Protected   that password AND the Edit Protected Comms password PROVED
//               AGAINST A CONNECTED DEVICE. The round trip is the only thing that
//               makes Protected stronger than Hidden, so there is no offline
//               fallback and there must never be one; the section password is the
//               half added in 2.3.1, when Protected stopped being the one marking
//               with no per-section secret behind it.
//
// The local password is asked FIRST and the device second, deliberately: it costs
// nothing, so a wrong answer or a change of mind never spends a serial round trip
// (which blocks the UI) to find that out.
//
// Meeting them records a grant on the DOCUMENT rather than a flag on this dialog,
// because the grant has to outlive both: it is what reveals the section
// everywhere else in the app for the rest of the session, and it is what
// Configuration::applyBusSections consults when the untick is finally committed.
// It is recorded ONCE, after the last proof — a grant stands for the whole of a
// tier's challenge, so recording it between the two halves would hand out the
// device half for free.
bool CommunicationsDialog::unlockConcealedSection(int busIndex, const CommsSection &section)
{
    const Configuration::SectionProofs need =
        Configuration::proofsRequiredFor(section.protection);

    // A CONCEALED SECTION WITH NO PASSWORD CANNOT BE OPENED, and saying so here
    // is what keeps this dialog's decoration and its behaviour telling the same
    // story. It used to skip the challenge instead — "nobody set a password, so
    // asking for one would be theatre" — which produced the row the user
    // reported: a padlock and the word "hidden" in the list, and an Edit button
    // that opened the whole message on a double-click with nothing asked for.
    // Configuration::isSectionRevealed() now conceals such a section, so this
    // has to refuse it; the two must ask the same question or the disagreement
    // simply comes back.
    //
    // Refused BEFORE the device proof below, deliberately. A keyless Protected
    // section would otherwise spend a serial round trip (which blocks the UI) to
    // earn a grant that Configuration::grantSectionAccess declines to record —
    // proving something true and being told nothing at all had changed.
    //
    // This is the ordinary state of a configuration read back off a unit: the
    // wire carries `reserved[4]` and no key, so every section a Get produces
    // arrives keyless, as does every section in a file written before markings
    // carried passwords.
    if (need.sectionPassword && section.messageKey == kNoAccessKey) {
        QMessageBox::information(
            this, windowTitle(),
            tr("\"%1\" is marked %2, and it arrived without a Message Password — from a "
               "configuration read back off a device, or from a file written before markings "
               "carried passwords.\n\n"
               "No password for it exists anywhere, so there is nothing that could open it and "
               "nothing to go and look for. It cannot be given one either: a Message Password is "
               "typed into this message's editor, and that is the window this refusal is "
               "standing in the way of. (A Read Only message has that way out, because Read "
               "Only conceals nothing and always opens. This one is not Read Only.)\n\n"
               "The message can still be REMOVED, and it can be reordered and sent as it is. Its "
               "channels can be used everywhere else in the configuration.\n\n"
               "The original configuration file the message was built in still holds its "
               "password and still opens it.")
                .arg(section.name,
                     section.protection == CommsProtection::Protected
                         ? tr("Protect Communication")
                         : tr("Hidden")));
        return false;
    }

    if (need.sectionPassword) {
        QString prompt =
            section.protection == CommsProtection::Protected
                ? tr("\"%1\" is marked Protect Communication. Enter this section's own password "
                     "to open it — the device is asked for the Edit Protected Comms password "
                     "next.")
                      .arg(section.name)
                : tr("\"%1\" is hidden. Enter this section's own password to open it.")
                      .arg(section.name);
        for (;;) {
            bool ok = false;
            const QString typed =
                QInputDialog::getText(this, windowTitle(), prompt + tr("\n\nMessage Password :"),
                                      QLineEdit::Password, QString(), &ok);
            if (!ok)
                return false;
            if (!typed.isEmpty() && deriveAccessKey(typed) == section.messageKey)
                break;
            // Nothing about how wrong it was: no "close", no length hint.
            prompt = tr("That password is not correct.");
        }
    }

    if (need.deviceProof) {
        if (!m_prover) {
            QMessageBox::information(
                this, windowTitle(),
                tr("\"%1\" is marked Protect Communication by whoever built this "
                   "configuration. Opening it needs the Edit Protected Comms password checked "
                   "by a connected CAN Triple, and this window has no device to ask.\n\n"
                   "The channels it produces can still be used anywhere else, and the section "
                   "can still be removed, reordered and sent — none of that needs a device.")
                    .arg(section.name));
            return false;
        }
        // The prover owns every fact about the hardware and reports its own
        // failures: nothing connected, the password wrong, or the unit holding
        // no such password at all.
        if (!m_prover())
            return false;
    }

    // The BUS goes in with it. A grant used to be a bare name, which is a value
    // the person being kept out picks: add a section on another bus under the
    // same name, unlock that one with a password of your own, and the real one
    // opened. `section` also carries the messageKey the challenge above was
    // answered against, which is what retires the grant if that key is ever
    // replaced.
    m_config->grantSectionAccess(busIndex, section);
    return true;
}

void CommunicationsDialog::onEditSection(int busIndex)
{
    BusTab &tab = m_busTabs[busIndex];
    const int row = tab.sectionTree->indexOfTopLevelItem(tab.sectionTree->currentItem());
    if (row < 0 || row >= m_buses[busIndex].sections.size())
        return;
    // A concealed message is opened by ASKING, not by refusing and pointing at a
    // menu. The old refusal named File > Reveal Protected Comms, which is the
    // document-wide password and is not what a Hidden section is guarded by at
    // all. The editor still opens with every protocol field disabled once it is
    // in — revealing buys viewing and the right to untick, not editing.
    //
    // A Read Only section falls straight through: it conceals nothing, so there
    // is nothing to unlock before looking at it.
    const CommsSection &target = m_buses[busIndex].sections[row];
    if (target.isConcealed(sectionRevealed(busIndex, target))
        && !unlockConcealedSection(busIndex, target))
        return;
    // By value, before the editor can write over the slot: `target` is a reference
    // INTO the list and the write-back below replaces what it points at, so
    // reading a "before" name through it afterwards would silently return the
    // "after" one.
    const QString priorName = target.name;
    SectionEditorDialog dialog(m_config, m_buses[busIndex].sections[row], busIndex, liveView(),
                               row, this, m_prover,
                               m_busTabs[busIndex].fdRateCombo->currentData().toInt());
    const bool accepted = dialog.exec() == QDialog::Accepted;
    if (accepted) {
        // The editor is the only place a tier can be MOVED, and it refuses to move
        // one without running every challenge that tier demands first. Recording
        // the grant here, on the document, is what lets
        // Configuration::applyBusSections accept the change when this dialog
        // finally commits on OK — the dialog proves, the model enforces, and
        // neither is asked to do the other's job.
        //
        // Recorded from the section AS IT STILL STANDS — before the line below
        // overwrites it — because the grant has to answer for the key
        // applyBusSections will ask about, which is the PRIOR one. Record it
        // from dialog.section() instead and a password change would grant
        // against the new key and authorise itself.
        if (dialog.protectionUnlocked())
            m_config->grantSectionAccess(busIndex, m_buses[busIndex].sections[row]);
        m_buses[busIndex].sections[row] = dialog.section();
    }

    // RULE 3. The editor has closed — OK or Cancel, it makes no difference — and
    // if what it leaves behind STILL CONCEALS, this dialog conceals it again right
    // now. A password given to open a message once is not a standing licence to
    // leave it open on screen while the box is still ticked; the user's words are
    // "do not show the channels available in the Main Communications Setup
    // afterwards".
    //
    // Protected is included as well as Hidden. The user wrote "hidden", but
    // Protected conceals identically — same padlock, same "(Channel information
    // locked)" pane — and leaving one revealed while the other re-conceals would
    // be an inconsistency with no reason behind it, on the STRONGER of the two.
    //
    // Queued rather than revoked outright, because the grant is also what
    // authorises the lowering that applyBusSections has not seen yet. Protect
    // Communication down to Hidden is precisely that case: lowered, still
    // concealing, and refused at OK if the grant went first. See m_pendingRevoke.
    //
    // Both names, so a rename inside the editor cannot leave the old grant
    // standing: applyBusSections matches by name, and the grant set does too.
    // Both on THIS bus, which is the only one this editor could have touched.
    //
    // Asked through CommsSection::isConcealed() rather than by comparing the
    // tier here, and that is the point rather than a tidy-up: this queue and
    // unlockConcealedSection's challenge must be driven by ONE predicate or they
    // drift, and the drift has a shape — this site queued the re-conceal on the
    // tier while that one skipped the password whenever the section was keyless,
    // so a row came back padlocked and reading "(hidden)" with an Edit button
    // that opened it for free. Both now ask "does this section conceal?".
    // `revealed=false` is the right argument: the question is whether the
    // section conceals from a viewer who has proved nothing, since re-concealing
    // is exactly what turns this viewer back into one.
    const CommsSection &after = m_buses[busIndex].sections[row];
    if (after.isConcealed(/*revealed=*/false)) {
        m_pendingRevoke.insert(pendingKey(busIndex, priorName));
        m_pendingRevoke.insert(pendingKey(busIndex, after.name));
    }

    // Repaint either way. Even a cancelled editor may have unlocked the section on
    // the way in, and that grant is real — the password was given — so the row has
    // to stop showing a padlock the viewer has already opened, unless the line
    // above has just shut it again.
    rebuildSections(busIndex);
    updateButtons(busIndex);
}

QList<int> CommunicationsDialog::selectedRows(int busIndex) const
{
    const BusTab &tab = m_busTabs[busIndex];
    QList<int> rows;
    for (QTreeWidgetItem *item : tab.sectionTree->selectedItems()) {
        const int r = tab.sectionTree->indexOfTopLevelItem(item);
        if (r >= 0 && r < m_buses[busIndex].sections.size())
            rows.append(r);
    }
    // selectedItems() is in no documented order — with shift-select it commonly
    // comes back in click order, so a downward drag and an upward one would
    // otherwise remove and move different things.
    std::sort(rows.begin(), rows.end());
    return rows;
}

void CommunicationsDialog::onRemoveSection(int busIndex)
{
    const QList<int> rows = selectedRows(busIndex);
    if (rows.isEmpty())
        return;

    // One row goes without asking, as it always has. Several is a different
    // amount of lost work — a shift-select can span thirty imported messages —
    // and it is the one case where a mis-aimed click is expensive, so it is
    // confirmed the way Remove All is.
    if (rows.size() > 1) {
        const auto answer = QMessageBox::question(
            this, windowTitle(),
            tr("Remove the %1 selected sections from CAN %2?").arg(rows.size()).arg(busIndex + 1),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes)
            return;
    }

    // Descending, so each removal cannot shift the rows still to be removed.
    for (int i = rows.size() - 1; i >= 0; --i)
        m_buses[busIndex].sections.removeAt(rows.at(i));
    rebuildSections(busIndex);
    updateButtons(busIndex);
}

void CommunicationsDialog::onMoveSection(int busIndex, int delta)
{
    BusTab &tab = m_busTabs[busIndex];
    const QList<int> rows = selectedRows(busIndex);
    const int count = m_buses[busIndex].sections.size();
    if (rows.isEmpty() || delta == 0)
        return;
    // The whole selection moves one place, so it is blocked as a unit: an edge
    // row means the group has nowhere to go. Moving the rest and leaving that
    // one behind would silently change the selection's internal order, and the
    // order IS the transmit order.
    if (delta < 0 && rows.first() == 0)
        return;
    if (delta > 0 && rows.last() == count - 1)
        return;

    // Swap each selected row with its neighbour, walking in the direction of
    // travel: ascending for a move up, descending for a move down. Adjacent
    // selected rows then carry each other rather than colliding, so a
    // contiguous block slides intact and a scattered selection has every one of
    // its rows step one place. Unselected rows in the gaps get pushed the other
    // way, which is exactly what moving past them means.
    if (delta < 0) {
        for (int r : rows)
            m_buses[busIndex].sections.swapItemsAt(r - 1, r);
    } else {
        for (int i = rows.size() - 1; i >= 0; --i) {
            const int r = rows.at(i);
            m_buses[busIndex].sections.swapItemsAt(r, r + 1);
        }
    }

    rebuildSections(busIndex);
    // Follow the rows that moved, so the same messages stay selected and the
    // button can be pressed again to keep going.
    tab.sectionTree->setCurrentItem(tab.sectionTree->topLevelItem(rows.first() + delta));
    for (int r : rows) {
        if (QTreeWidgetItem *item = tab.sectionTree->topLevelItem(r + delta))
            item->setSelected(true);
    }
    updateButtons(busIndex);
}

void CommunicationsDialog::onRemoveAll(int busIndex)
{
    if (m_buses[busIndex].sections.isEmpty())
        return;
    const auto answer = QMessageBox::question(
        this, windowTitle(),
        tr("Remove all %1 sections from CAN %2?")
            .arg(m_buses[busIndex].sections.size())
            .arg(busIndex + 1),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes)
        return;
    m_buses[busIndex].sections.clear();
    rebuildSections(busIndex);
    updateButtons(busIndex);
}

void CommunicationsDialog::onImportDbc(int busIndex)
{
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Import DBC"), QString(), tr("DBC files (*.dbc);;All files (*.*)"));
    if (path.isEmpty())
        return;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, tr("Import DBC"),
                             tr("Could not open %1:\n%2").arg(path, file.errorString()));
        return;
    }
    const QByteArray bytes = file.readAll();
    // DBC files are usually UTF-8 or Latin-1; decode as UTF-8 and fall back to
    // Latin-1 if that produced replacement characters (e.g. a Latin-1 ° in a unit).
    QString text = QString::fromUtf8(bytes);
    if (text.contains(QChar(QChar::ReplacementCharacter)))
        text = QString::fromLatin1(bytes);

    QStringList warnings;
    const DbcFile dbc = parseDbc(text, &warnings);
    if (dbc.messages.isEmpty()) {
        QMessageBox::information(this, tr("Import DBC"),
                                 tr("No CAN messages (BO_) were found in %1.")
                                     .arg(QFileInfo(path).fileName()));
        return;
    }

    ImportDbcDialog dlg(m_config, dbc, QFileInfo(path).fileName(), busIndex, warnings, this);
    if (dlg.exec() != QDialog::Accepted)
        return;

    const int target = dlg.targetBusIndex();
    for (const CommsSection &s : dlg.importedSections())
        m_buses[target].sections.append(s);

    // Rebuild every tab: the "N of M device messages used" count spans all buses.
    for (int i = 0; i < 3; ++i) {
        rebuildSections(i);
        updateButtons(i);
    }
    m_tabs->setCurrentIndex(target);
    if (!m_buses[target].sections.isEmpty())
        m_busTabs[target].sectionTree->setCurrentItem(
            m_busTabs[target].sectionTree->topLevelItem(m_buses[target].sections.size() - 1));
    updateButtons(target);
}

void CommunicationsDialog::accept()
{
    // THE last chokepoint before the document. Every path through this dialog —
    // New, Edit, Remove, Remove All, Move, DBC import — edits the working copies
    // and lands here, so this one loop is where the untick rule is finally
    // enforced, by Configuration::applyBusSections rather than by anything on
    // screen. A guard on the section editor's checkboxes alone would be a guard
    // on one of the six.
    //
    // A refusal does NOT close the dialog. Everything the user has done is still
    // in the working copies, so they can undo the one change that was refused
    // and press OK again; discarding an evening's editing because the last click
    // was not permitted would be a worse answer than the refusal itself.
    QString refusal;
    bool changed = false;
    for (int i = 0; i < 3; ++i) {
        m_buses[i].enabled = m_busTabs[i].modeCombo->currentData().toBool();
        m_buses[i].rateKbps = m_busTabs[i].rateCombo->currentData().toInt();
        m_buses[i].dataRateKbps = m_busTabs[i].fdRateCombo->currentData().toInt();
        m_buses[i].termination = m_busTabs[i].terminationCombo->currentData().toBool();
        if (m_buses[i].toJson() == m_config->bus[i].toJson())
            continue;
        // The bus's own settings (mode, rate, FD, termination) are not sections
        // and applyBusSections does not carry them, so they are assigned around
        // it. They are copied FIRST but only once the sections are known to be
        // acceptable, so a refusal leaves the bus wholly untouched rather than
        // half-applied.
        if (!m_config->applyBusSections(i, m_buses[i].sections, &refusal)) {
            m_tabs->setCurrentIndex(i);
            QMessageBox::warning(this, windowTitle(), refusal);
            return;
        }
        m_config->bus[i].enabled = m_buses[i].enabled;
        m_config->bus[i].rateKbps = m_buses[i].rateKbps;
        m_config->bus[i].dataRateKbps = m_buses[i].dataRateKbps;
        m_config->bus[i].termination = m_buses[i].termination;
        changed = true;
    }
    // applyBusSections has already called setDirty() for every bus it accepted;
    // this catches a bus whose sections were identical and only its rate moved.
    if (changed)
        m_config->setDirty();
    // AFTER the loop, never before it. Rule 3's revokes drop the grants that
    // authorise a lowering, and every lowering this dialog is carrying has just
    // been written above. Moving this one line up is the mistake that makes a
    // Protect-Communication-down-to-Hidden edit refuse itself at the last step.
    flushPendingRevokes();
    QDialog::accept();
}

void CommunicationsDialog::reject()
{
    // Cancel throws the working copies away, so nothing was written and there is
    // no ordering to respect — but the grants are on the DOCUMENT and outlive this
    // dialog either way, so rule 3 applies to a cancelled session exactly as it
    // does to an accepted one.
    flushPendingRevokes();
    QDialog::reject();
}

void CommunicationsDialog::flushPendingRevokes()
{
    // Split back into the (bus, name) pendingKey() joined. At the FIRST separator
    // only: the bus index is one digit, and a section name may contain a slash of
    // its own — "Engine/Trans" is a name a user can type, and taking the last
    // separator would revoke a grant for a section called "Trans" on a bus called
    // "0/Engine".
    for (const QString &key : m_pendingRevoke) {
        const int slash = key.indexOf(QLatin1Char('/'));
        if (slash < 0)
            continue;
        m_config->revokeSectionAccess(key.left(slash).toInt(), key.mid(slash + 1));
    }
    m_pendingRevoke.clear();
}

} // namespace ct
