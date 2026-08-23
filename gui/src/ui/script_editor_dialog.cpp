#include "script_editor_dialog.h"

#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QSplitter>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextBlock>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "../model/configuration.h"
#include "../model/device_mapper.h"          // scriptVerifyText — the one table of verdicts
#include "../scripting/script_disassembler.h"
#include "lua_console_dialog.h"   // LuaHighlighter — the same Lua, the same colours

extern "C" {
#include "script_vm.h"
}

namespace ct {
namespace {

const char kStarterScript[] =
    "-- Device script: on_tick() runs 100 times a second on the unit.\n"
    "-- Channels are addressed by name; press F1 for the full reference.\n"
    "\n"
    "-- Persistent state, declared out here, survives across ticks:\n"
    "-- local count = state(0)\n"
    "\n"
    "function on_tick()\n"
    "    -- local rpm = sig(\"Engine RPM\")\n"
    "    -- setSig(\"Fan Request\", rpm > 6000)\n"
    "end\n";

// A verifier verdict in words used to be a second switch here. It is
// scriptVerifyText() in device_mapper.h now: the Get note, the Send refusal,
// the disassembler's reason for not listing an image and the report below all
// describe the same fifteen refusals, and two tables were two chances to
// describe one fault in two ways.

} // namespace

ScriptEditorDialog::ScriptEditorDialog(Configuration &config, QWidget *parent)
    : QDialog(parent), m_config(config)
{
    setWindowTitle(tr("Device Script"));
    resize(940, 700);
    // WINDOW-modal, not application-modal, and that distinction is the whole
    // reason F1 works from here. exec() makes a dialog application-modal by
    // default, which blocks input to EVERY other window in the program — the
    // help window included, so the manual would open behind this dialog and be
    // inert when it got there. Window-modal blocks this dialog's own window
    // hierarchy and leaves the parentless help window (see
    // MainWindow::onHelpContents) alone. Everything a user must not touch
    // while this is open — the document, the other grids — is inside that
    // hierarchy and is still blocked.
    setWindowModality(Qt::WindowModal);
    buildUi();

    QString symbolError;
    m_symbolsValid = ScriptSymbols::fromConfiguration(m_config, &m_symbols, &symbolError);
    if (!m_symbolsValid) {
        // The configuration itself will not map, so channel names cannot be
        // resolved. Say so plainly rather than reporting every sig() as an
        // unknown channel, which would send the user hunting for typos.
        say(tr("This configuration cannot be mapped to the device, so channel names "
               "cannot be resolved yet:\n%1")
                .arg(symbolError),
            /*isError=*/true);
    }

    m_savedSource = m_config.scriptSource();
    m_retainedImage = m_config.scriptBytecode();
    if (m_retainedImage.isEmpty()) {
        m_editor->setPlainText(m_savedSource.isEmpty() ? QString::fromUtf8(kStarterScript)
                                                       : m_savedSource);
    } else {
        // The document holds a compiled image and no source (Configuration
        // keeps those two mutually exclusive), so there is nothing to put in
        // the editor and the starter template would be a lie: it would look
        // like the document's script, and typing over it would look like
        // editing one. Open on the LISTING instead, with the editor read-only,
        // so the first thing on screen is what the document actually carries.
        m_editor->setReadOnly(true);
        m_editor->setPlaceholderText(
            tr("This document has no Lua source. Its script is the compiled image listed "
               "on the Disassembly tab."));
        m_sourceTabs->setCurrentIndex(1);
    }
    showListing(m_retainedImage);
    updateRetainedBanner();
    rebuildChannelTable();
}

void ScriptEditorDialog::buildUi()
{
    auto *root = new QVBoxLayout(this);
    auto *split = new QSplitter(Qt::Horizontal, this);

    // ---- left: the editor -------------------------------------------------
    auto *left = new QWidget(split);
    auto *leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 0, 0);

    // The banner, above everything, and hidden in every ordinary document. It
    // exists for the one case where this dialog's contents and the document's
    // contents would otherwise disagree — a compiled image the editor cannot
    // show — and it is placed first because that disagreement has to be read
    // before anything else on this screen makes sense.
    m_retainedBanner = new QLabel(left);
    m_retainedBanner->setWordWrap(true);
    m_retainedBanner->setTextFormat(Qt::RichText);
    m_retainedBanner->setVisible(false);
    leftLayout->addWidget(m_retainedBanner);

    m_replaceButton = new QPushButton(tr("Re&place With a Lua Script…"), left);
    m_replaceButton->setToolTip(
        tr("Take over from the compiled script this document read back from a device, and "
           "write Lua in its place. Asks first — the image may be the only copy."));
    m_replaceButton->setVisible(false);
    auto *replaceRow = new QHBoxLayout();
    replaceRow->addStretch(1);
    replaceRow->addWidget(m_replaceButton);
    leftLayout->addLayout(replaceRow);

    // Source and disassembly as TABS rather than a split: they are two views of
    // the same one script, never both meaningful at once. A document with Lua
    // has a listing only after a Compile; a document with a retained image has
    // a listing and no Lua at all.
    m_sourceTabs = new QTabWidget(left);

    m_editor = new QPlainTextEdit(m_sourceTabs);
    const QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    m_editor->setFont(mono);
    m_editor->setTabStopDistance(4 * QFontMetricsF(mono).horizontalAdvance(u' '));
    new LuaHighlighter(m_editor->document());
    m_sourceTabs->addTab(m_editor, tr("Lua &Source"));

    auto *listingPage = new QWidget(m_sourceTabs);
    auto *listingLayout = new QVBoxLayout(listingPage);
    listingLayout->setContentsMargins(0, 0, 0, 0);
    m_listing = new QTreeWidget(listingPage);
    // READ-ONLY, and that is a decision rather than an omission. An editable
    // listing would need a validating assembler and the device's cost model on
    // this side to be safe, and the rule the whole feature rests on is that
    // writing or compiling LUA is what changes what gets sent. This is a view.
    m_listing->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_listing->setRootIsDecorated(false);
    m_listing->setUniformRowHeights(true);
    m_listing->setAlternatingRowColors(true);
    m_listing->setHeaderLabels({ tr("#"), tr("Instruction"), tr("Operands"), tr("Cost") });
    m_listing->setFont(mono);
    listingLayout->addWidget(m_listing, 1);
    m_listingSummary = new QLabel(listingPage);
    m_listingSummary->setWordWrap(true);
    listingLayout->addWidget(m_listingSummary);
    m_sourceTabs->addTab(listingPage, tr("&Disassembly"));

    leftLayout->addWidget(m_sourceTabs, 1);

    m_status = new QLabel(left);
    m_status->setWordWrap(true);
    m_status->setTextFormat(Qt::RichText);
    leftLayout->addWidget(m_status);
    split->addWidget(left);

    // ---- right: the simulator --------------------------------------------
    auto *right = new QWidget(split);
    auto *rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(0, 0, 0, 0);

    auto *chanBox = new QGroupBox(tr("Channels"), right);
    auto *chanLayout = new QVBoxLayout(chanBox);
    auto *hint = new QLabel(
        tr("Type a value to feed the script; channels it writes are highlighted."),
        chanBox);
    hint->setWordWrap(true);
    chanLayout->addWidget(hint);
    m_channels = new QTableWidget(0, 2, chanBox);
    m_channels->setHorizontalHeaderLabels({ tr("Channel"), tr("Value") });
    m_channels->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_channels->verticalHeader()->setVisible(false);
    chanLayout->addWidget(m_channels);
    rightLayout->addWidget(chanBox, 3);

    auto *stateBox = new QGroupBox(tr("Persistent state"), right);
    auto *stateLayout = new QVBoxLayout(stateBox);
    m_state = new QTableWidget(0, 2, stateBox);
    m_state->setHorizontalHeaderLabels({ tr("Register"), tr("Value") });
    m_state->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_state->verticalHeader()->setVisible(false);
    m_state->setEditTriggers(QAbstractItemView::NoEditTriggers);
    stateLayout->addWidget(m_state);
    rightLayout->addWidget(stateBox, 2);

    m_costLabel = new QLabel(right);
    m_costLabel->setWordWrap(true);
    rightLayout->addWidget(m_costLabel);
    split->addWidget(right);

    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);
    root->addWidget(split, 1);

    // ---- controls ---------------------------------------------------------
    auto *controls = new QHBoxLayout();
    auto *compileButton = new QPushButton(tr("&Compile"), this);
    m_compileButton = compileButton;
    // On the BUTTON, not in keyPressEvent: the editor has focus while you type,
    // and QPlainTextEdit swallows Return before the dialog ever sees it. A
    // button shortcut is a window-level one, so it fires from inside the editor
    // — which is the only place it is any use.
    compileButton->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return));
    compileButton->setToolTip(tr("Compile the script (Ctrl+Enter)"));
    m_stepButton = new QPushButton(tr("&Step"), this);
    m_runButton = new QPushButton(tr("&Run"), this);
    m_tickCount = new QSpinBox(this);
    m_tickCount->setRange(1, 100000);
    m_tickCount->setValue(100);
    m_tickCount->setSuffix(tr(" ticks"));
    // No up/down arrows: the useful values span 1 to 100,000, so stepping one
    // at a time is pointless — this field is typed, not nudged.
    m_tickCount->setButtonSymbols(QAbstractSpinBox::NoButtons);
    // 100 ticks is one second of device time (the engine runs on_tick 100 times
    // a second); say so on hover, since "ticks" alone does not.
    m_tickCount->setToolTip(tr("How many engine ticks to simulate. The device runs "
                               "on_tick 100 times a second, so 100 ticks is one second "
                               "of device time."));
    auto *resetButton = new QPushButton(tr("Re&set"), this);
    // Load/Save move a script between this editor and a .lua file. The script
    // itself lives in the .ct3 document, so these are for the things a document
    // cannot do: keeping a library of scripts, sharing one, or starting from a
    // shipped example. Grouped at the right, away from the compile/run controls,
    // because they act on the FILE rather than on the simulation.
    auto *loadButton = new QPushButton(tr("&Load Script…"), this);
    auto *saveButton = new QPushButton(tr("Save Scr&ipt…"), this);
    m_loadButton = loadButton;
    m_saveButton = saveButton;
    loadButton->setToolTip(tr("Replace the script in this editor with one from a .lua file."));
    saveButton->setToolTip(tr("Write the script in this editor out to a .lua file."));
    controls->addWidget(compileButton);
    controls->addSpacing(12);
    controls->addWidget(m_stepButton);
    controls->addWidget(m_runButton);
    controls->addWidget(m_tickCount);
    controls->addWidget(resetButton);
    controls->addStretch(1);
    controls->addWidget(loadButton);
    controls->addWidget(saveButton);
    root->addLayout(controls);

    auto *buttons = new QDialogButtonBox(this);
    auto *okButton = buttons->addButton(QDialogButtonBox::Ok);
    buttons->addButton(QDialogButtonBox::Cancel);
    auto *helpButton = buttons->addButton(QDialogButtonBox::Help);
    root->addWidget(buttons);

    connect(m_replaceButton, &QPushButton::clicked, this,
            &ScriptEditorDialog::onReplaceRetainedImage);
    connect(compileButton, &QPushButton::clicked, this, &ScriptEditorDialog::onCompile);
    connect(m_stepButton, &QPushButton::clicked, this, &ScriptEditorDialog::onStep);
    connect(m_runButton, &QPushButton::clicked, this, &ScriptEditorDialog::onRunTicks);
    connect(resetButton, &QPushButton::clicked, this, &ScriptEditorDialog::onResetSim);
    connect(loadButton, &QPushButton::clicked, this, &ScriptEditorDialog::onLoadScript);
    connect(saveButton, &QPushButton::clicked, this, &ScriptEditorDialog::onSaveScript);
    connect(okButton, &QPushButton::clicked, this, &ScriptEditorDialog::applyAndAccept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(helpButton, &QPushButton::clicked, this,
            [this]() { emit helpRequested(QStringLiteral("device-scripts.html")); });

    m_stepButton->setEnabled(false);
    m_runButton->setEnabled(false);
}

// The read-only listing, from whatever image the document would send right now:
// the retained one on open, the compiler's output after a Compile. Rebuilt
// rather than patched, because a listing that kept a line from a previous image
// would be the same disagreement between decoration and content this dialog's
// banner exists to prevent.
void ScriptEditorDialog::showListing(const QByteArray &image)
{
    m_listing->clear();

    // Slot -> channel name, so LOADSIG reads as something. m_symbols maps the
    // other way and is what the rest of the dialog uses; the inversion is built
    // here, per call, because a Compile can change the map underneath it.
    QHash<quint16, QString> names;
    names.reserve(m_symbols.signalIndex.size());
    for (auto it = m_symbols.signalIndex.constBegin(); it != m_symbols.signalIndex.constEnd();
         ++it) {
        names.insert(it.value(), it.key());
    }

    const ScriptListing listing = disassembleScriptImage(image, names);
    m_listingSummary->setText(scriptListingSummary(listing));
    for (const ScriptListingLine &line : listing.lines) {
        auto *item = new QTreeWidgetItem(m_listing);
        // The entry point is marked in the index column rather than by colour
        // alone: "where does this start" is the first question anyone reading a
        // listing asks, and a tint does not survive a screenshot in a bug report.
        item->setText(0, line.isEntry ? tr("→ %1").arg(line.index)
                                      : QString::number(line.index));
        item->setText(1, line.mnemonic);
        item->setText(2, line.operands);
        item->setText(3, QString::number(line.cost));
        item->setTextAlignment(0, Qt::AlignRight | Qt::AlignVCenter);
        item->setTextAlignment(3, Qt::AlignRight | Qt::AlignVCenter);
        if (line.isEntry) {
            item->setToolTip(0, tr("on_tick starts here"));
        }
    }
    for (int c = 0; c < 4; ++c) {
        m_listing->resizeColumnToContents(c);
    }
}

// One writer for the banner, the Replace button and everything the retained
// image makes inert. Split across the call sites it would eventually let the
// banner say the image is still there while the editor was already writable.
void ScriptEditorDialog::updateRetainedBanner()
{
    const bool held = !m_retainedImage.isEmpty();
    m_retainedBanner->setVisible(held);
    m_replaceButton->setVisible(held && !m_replacingRetained);

    // Inert = the document's script is an image and the user has not taken over.
    // Compile would compile the empty read-only editor and report an error about
    // a script nobody wrote; Load would fill a buffer OK is not permitted to
    // store; Save would write an empty file. All three would look like actions.
    const bool inert = held && !m_replacingRetained;
    m_compileButton->setEnabled(!inert);
    m_loadButton->setEnabled(!inert);
    m_saveButton->setEnabled(!inert);
    if (inert) {
        m_stepButton->setEnabled(false);
        m_runButton->setEnabled(false);
    }

    if (!held) {
        return;
    }
    // Both colours are set explicitly, and both are chosen against the theme in
    // use. A banner that set only its background would be dark text on a pale
    // panel under a dark palette — unreadable exactly where it matters most,
    // since this is the notice that stops a script being thrown away. Same
    // lightness test the other dialogs use (add_channel_dialog, access
    // passwords).
    const bool darkUi = palette().color(QPalette::Window).lightness() < 128;
    m_retainedBanner->setStyleSheet(
        darkUi ? QStringLiteral("QLabel { background: #4A3B10; color: #F5E6BC; "
                                "border: 1px solid #7A6420; padding: 6px; }")
               : QStringLiteral("QLabel { background: #FFF4CE; color: #3B2F00; "
                                "border: 1px solid #D9B441; padding: 6px; }"));

    if (m_replacingRetained) {
        m_retainedBanner->setText(
            tr("<b>The compiled script will be replaced.</b> What you write here is what "
               "this document sends; the image it read back from the device is dropped "
               "when you press OK. Cancel still leaves it alone, and the Disassembly tab "
               "keeps listing it until you compile something else."));
        return;
    }
    m_retainedBanner->setText(
        tr("<b>This document's script is a compiled image read back from a device.</b> "
           "There is no Lua source for it: the device stores only bytecode, and bytecode "
           "does not turn back into source. Sending this configuration puts the same "
           "script back on a unit byte for byte. The <b>Disassembly</b> tab lists it — the "
           "editor is read-only because there is nothing to edit. To write Lua instead, "
           "press <b>Replace With a Lua Script</b>; that, and nothing else here, drops the "
           "image."));
}

// Take over from a retained image. Everything about this is deliberate: a
// button that is not the OK button, a question that spells out what is lost,
// and a default that answers no. The image may be the only copy of that script
// in existence — there is no source for it anywhere — so it must not be
// possible to lose it by typing in a box.
void ScriptEditorDialog::onReplaceRetainedImage()
{
    const auto answer = QMessageBox::warning(
        this, tr("Device Script"),
        tr("This document's script is a compiled image read back from a device. There is "
           "no Lua source for it anywhere, and it cannot be turned back into source.\n\n"
           "If you write a script here and press OK, this document drops that image and "
           "sends yours instead — and the next Send overwrites the copy on the device "
           "too.\n\n"
           "Write a new script?"),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer != QMessageBox::Yes) {
        return;
    }
    m_replacingRetained = true;
    m_editor->setReadOnly(false);
    m_editor->setPlainText(QString::fromUtf8(kStarterScript));
    m_sourceTabs->setCurrentIndex(0);
    m_editor->setFocus();
    updateRetainedBanner();
}

void ScriptEditorDialog::rebuildChannelTable()
{
    // Every channel the configuration maps, so a script can be fed anything it
    // might read. Sorted by name; the table is the only place a simulated input
    // comes from, since there is no device attached.
    QStringList names = m_symbols.signalIndex.keys();
    names.sort(Qt::CaseInsensitive);

    m_channels->setRowCount(names.size());
    m_rowByName.clear();
    m_rowByName.reserve(names.size());
    for (int i = 0; i < names.size(); ++i) {
        auto *nameItem = new QTableWidgetItem(names[i]);
        nameItem->setFlags(nameItem->flags() & ~Qt::ItemIsEditable);
        nameItem->setData(Qt::UserRole, m_symbols.signalIndex.value(names[i]));
        m_channels->setItem(i, 0, nameItem);
        m_channels->setItem(i, 1, new QTableWidgetItem(QStringLiteral("0")));
        // A name -> row index, so showTick is a lookup per channel rather than a
        // scan. A configuration can map a thousand channels, and a tick reports
        // on every one of them; the scan version was a million string compares
        // for one Step.
        m_rowByName.insert(names[i], i);
    }
}

void ScriptEditorDialog::say(const QString &text, bool isError)
{
    m_status->setText(isError
                          ? QStringLiteral("<span style='color:#c0392b'>%1</span>")
                                .arg(text.toHtmlEscaped().replace(QStringLiteral("\n"),
                                                                  QStringLiteral("<br>")))
                          : text.toHtmlEscaped().replace(QStringLiteral("\n"),
                                                         QStringLiteral("<br>")));
}

bool ScriptEditorDialog::compileCurrent()
{
    const QString source = m_editor->toPlainText();
    m_lastResult = ScriptCompiler::compile(source, m_symbols);
    if (!m_lastResult.ok) {
        m_image.clear();
        m_stepButton->setEnabled(false);
        m_runButton->setEnabled(false);
        say(m_lastResult.errorLine > 0
                ? tr("Line %1: %2").arg(m_lastResult.errorLine).arg(m_lastResult.error)
                : m_lastResult.error,
            /*isError=*/true);
        // Put the caret on the offending line so the eye goes to the right place.
        if (m_lastResult.errorLine > 0) {
            QTextCursor c(m_editor->document()->findBlockByNumber(
                m_lastResult.errorLine - 1));
            m_editor->setTextCursor(c);
            m_editor->centerCursor();
        }
        return false;
    }
    m_image = m_lastResult.image;
    return true;
}

void ScriptEditorDialog::onCompile()
{
    if (!compileCurrent()) {
        return;
    }
    const quint8 rc = m_sim.load(m_image);
    if (rc != SCRIPT_OK) {
        // Cannot happen — the compiler verifies its own output — so if it does,
        // it is a compiler bug and should say so rather than blame the script.
        say(tr("Internal error: the device verifier rejected the generated "
               "bytecode (%1). Please report this script.")
                .arg(scriptVerifyText(rc)),
            /*isError=*/true);
        return;
    }
    m_stepButton->setEnabled(true);
    m_runButton->setEnabled(true);
    // The listing follows what would be SENT. After a successful compile that is
    // this image, retained or not, so the Disassembly tab stops describing the
    // image the document arrived with the moment it is no longer the answer.
    showListing(m_image);

    QString msg = tr("Compiled: %1 instructions, %2 state register(s), %3 register(s), "
                     "%4 bytes.")
                      .arg(m_lastResult.instructionCount)
                      .arg(m_lastResult.stateDeclared)
                      .arg(m_lastResult.registersUsed)
                      .arg(m_image.size());
    if (m_lastResult.loopsPresent) {
        msg += tr("\nThe script contains loops, so its cost depends on how many times "
                  "they run — use Run to see the real figure.");
    }
    say(msg);
    m_costLabel->setText(tr("Straight-line cost %1 of %2 budget units.")
                             .arg(m_lastResult.straightLineCost)
                             .arg(m_sim.budget()));
}

void ScriptEditorDialog::onResetSim()
{
    m_sim.reset();
    m_costLabel->setText(tr("Simulator reset."));
    for (int i = 0; i < m_state->rowCount(); ++i) {
        m_state->item(i, 1)->setText(QStringLiteral("0"));
    }
}

// Read the seed values out of the channel table into the simulator. Locale
// aware: a user in a comma-decimal locale types "1,5" and means 1.5, and
// QString::toFloat (C locale only) would silently read that as 0 — the script
// would then be validated against inputs the user never entered. QLocale
// parses it correctly, and a C-locale fallback keeps "1.5" working for a user
// who typed a dot regardless of their locale.
void ScriptEditorDialog::seedSignals()
{
    const QLocale loc;
    for (int i = 0; i < m_channels->rowCount(); ++i) {
        const quint16 idx = quint16(m_channels->item(i, 0)->data(Qt::UserRole).toUInt());
        const QString text = m_channels->item(i, 1)->text().trimmed();
        bool ok = false;
        float v = loc.toFloat(text, &ok);
        if (!ok)
            v = text.toFloat(&ok); // dot-decimal, whatever the locale
        m_sim.setSignal(idx, ok ? v : 0.0f);
    }
}

void ScriptEditorDialog::onStep()
{
    seedSignals();
    const ScriptSimulator::TickResult r = m_sim.step(m_symbols.signalIndex);
    showTick(r);
    reportResult(r, r.cost, 1);
}

void ScriptEditorDialog::onRunTicks()
{
    const int n = m_tickCount->value();
    seedSignals();

    // Only the LAST tick's channel values are shown, so only that tick asks the
    // simulator to collect and sort the (up to a thousand) watched channels.
    // The intermediate ticks run with an empty watched set — the cost and fault
    // are computed regardless — which is what turns a 100,000-tick run from tens
    // of seconds of per-tick sorting into a fraction of a second.
    static const QHash<QString, quint16> kNoWatch;
    ScriptSimulator::TickResult last;
    quint32 peak = 0;
    int ran = 0;
    for (int i = 0; i < n; ++i) {
        const bool wantChannels = (i == n - 1);
        last = m_sim.step(wantChannels ? m_symbols.signalIndex : kNoWatch);
        peak = qMax(peak, last.cost);
        ++ran;
        if (last.fault != SCRIPT_FAULT_NONE) {
            // Capture the channel view for the faulted tick too, so the table is
            // not left showing a tick that never happened.
            if (!wantChannels)
                last.channels = m_sim.step(m_symbols.signalIndex).channels;
            break;
        }
    }
    showTick(last);
    reportResult(last, peak, ran);
}

// One place decides how a tick's outcome is worded, so Step and Run cannot say
// different things about the same fault — and so a fault is NEVER shown as a
// cost figure, which used to read "Peak cost 0 (0.0%)" and make a suspended VM
// look like a script that did nothing.
void ScriptEditorDialog::reportResult(const ScriptSimulator::TickResult &r, quint32 peak,
                                      int ranTicks)
{
    if (r.fault != SCRIPT_FAULT_NONE) {
        const QString why =
            r.fault == SCRIPT_FAULT_OVERRUN
                ? tr("it ran too long in real time — this is what "
                     "<code>mod</code>, <code>wrap</code> or <code>clamp</code> over a very "
                     "wide range of magnitudes does")
                : tr("it used the whole %1-unit budget in a single tick — look for a loop "
                     "that does not finish").arg(m_sim.budget());
        m_costLabel->setText(
            tr("<span style='color:#c0392b'><b>Faulted:</b> %1.</span> On the device the "
               "script would be suspended and would not run again until the configuration "
               "is reloaded. Press <b>Reset</b> to try again.")
                .arg(why));
        return;
    }
    m_costLabel->setText(tr("Ran %1 tick(s). Peak cost %2 of %3 budget units (%4%).")
                             .arg(ranTicks)
                             .arg(peak)
                             .arg(m_sim.budget())
                             .arg(100.0 * peak / m_sim.budget(), 0, 'f', 1));
}

void ScriptEditorDialog::showTick(const ScriptSimulator::TickResult &r)
{
    // Channels the script WROTE are tinted, so cause and effect are visible at a
    // glance — the whole point of simulating rather than reasoning.
    for (const auto &cv : r.channels) {
        const int row = m_rowByName.value(cv.name, -1);
        if (row < 0) {
            continue;
        }
        auto *item = m_channels->item(row, 1);
        item->setText(QString::number(double(cv.value), 'g', 7));
        item->setBackground(cv.writtenByScript ? QColor(0xE8, 0xF5, 0xE9) : QBrush());
    }

    // Only the state registers the script DECLARED: showing all 64 would bury
    // the two that matter, and showing stateUsed would add the compiler's
    // once-only initialiser flag — a register the user never wrote and cannot
    // account for.
    const int used = m_lastResult.stateDeclared;
    m_state->setRowCount(used);
    for (int i = 0; i < used && i < r.state.size(); ++i) {
        if (!m_state->item(i, 0)) {
            m_state->setItem(i, 0, new QTableWidgetItem(tr("state %1").arg(i)));
            m_state->setItem(i, 1, new QTableWidgetItem());
        }
        m_state->item(i, 1)->setText(QString::number(double(r.state[i]), 'g', 7));
    }
}

void ScriptEditorDialog::applyAndAccept()
{
    const QString source = m_editor->toPlainText();

    // A document whose script is a RETAINED DEVICE IMAGE leaves here untouched
    // unless the user pressed Replace and answered its question. This is an
    // explicit guard and not a consequence of the editor happening to be empty:
    // OK on such a document must not be able to throw away what may be the only
    // copy of a script in existence, and "it works because nothing was typed"
    // is not a property anyone can maintain.
    if (!m_retainedImage.isEmpty() && !m_replacingRetained) {
        accept();
        return;
    }

    // An empty script (or the untouched starter) means "no script": store
    // nothing rather than a stub that compiles to an empty on_tick, so a
    // configuration is not silently carrying 40 bytes of nothing.
    const bool blank = source.trimmed().isEmpty()
                       || source == QString::fromUtf8(kStarterScript);
    if (blank) {
        // Replace was pressed and then nothing was written. That has two honest
        // readings — keep the compiled script, or remove the script from the
        // document — and no way to guess which was meant, so it is asked. The
        // default is the one that loses nothing, and removal is spelled out as
        // what it is: the thing that REMOVES the script from the device at the
        // next Send.
        if (m_replacingRetained) {
            QMessageBox box(this);
            box.setIcon(QMessageBox::Warning);
            box.setWindowTitle(tr("Device Script"));
            box.setText(tr("No script has been written, so there is nothing to replace the "
                           "compiled script with."));
            box.setInformativeText(
                tr("Keep the compiled script this document read back from the device, or "
                   "remove the script from the document altogether? Removing it means the "
                   "next Send REMOVES the script from the device."));
            QPushButton *keep =
                box.addButton(tr("&Keep the Compiled Script"), QMessageBox::AcceptRole);
            QPushButton *remove =
                box.addButton(tr("&Remove the Script"), QMessageBox::DestructiveRole);
            box.setDefaultButton(keep);
            box.setEscapeButton(keep);
            box.exec();
            if (box.clickedButton() != remove) {
                accept(); // nothing was written; the document still holds the image
                return;
            }
            // setScriptSource(QString()) IS the removal: an empty source through
            // the one setter drops the retained image with it, which is the only
            // way this document can come to hold no script at all.
            m_config.setScriptSource(QString());
            m_config.setDirty(true);
            accept();
            return;
        }
        if (!m_config.scriptSource().isEmpty()) {
            m_config.setScriptSource(QString());
            m_config.setDirty(true);
        }
        accept();
        return;
    }

    // Refuse to store a script that will not compile — a document that carried
    // one would fail at Send, far from here with the reason long gone — BUT
    // only when the configuration maps and a compile can actually succeed.
    // When it does not map, every sig() fails "no channel named…", which is the
    // very misleading error the constructor took pains to suppress, and there is
    // nothing the user can do about it from this dialog. In that case store the
    // source as typed: Send re-compiles against the real configuration and
    // blocks on its own if the script is wrong, so nothing unchecked reaches a
    // device.
    if (m_symbolsValid && !compileCurrent()) {
        QMessageBox::warning(
            this, tr("Device Script"),
            tr("The script does not compile, so it has not been saved:\n\n%1")
                .arg(m_lastResult.errorLine > 0
                         ? tr("Line %1: %2").arg(m_lastResult.errorLine)
                               .arg(m_lastResult.error)
                         : m_lastResult.error));
        return;
    }
    // Through the setter, which is what drops any bytecode this document
    // retained from a device: writing a script here REPLACES the compiled image,
    // and the document must not come away holding both.
    if (source != m_config.scriptSource()) {
        m_config.setScriptSource(source);
        m_config.setDirty(true);
    }
    accept();
}

// The directory the last script was loaded from or saved to, so a user keeping
// a library of scripts does not re-navigate to it every time. Remembered across
// sessions in QSettings, like the recent-files list.
static const char kScriptDirKey[] = "script/lastDir";
static const char kScriptFilter[] = QT_TRANSLATE_NOOP("ct::ScriptEditorDialog",
                                                      "Device scripts (*.lua);;All files (*)");

// Replace the editor's contents from a file. This DISCARDS what is in the
// editor, so it asks first when that would lose work — the same question
// Cancel asks, and for the same reason: the script lives only here until OK.
void ScriptEditorDialog::onLoadScript()
{
    if (!confirmDiscard(tr("Loading a script replaces the one in this editor. "
                           "Discard the current changes?")))
        return;

    QSettings settings;
    const QString start = settings.value(QLatin1String(kScriptDirKey)).toString();
    const QString path =
        QFileDialog::getOpenFileName(this, tr("Load Device Script"), start, tr(kScriptFilter));
    if (path.isEmpty())
        return;

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QMessageBox::warning(this, tr("Load Device Script"),
                             tr("Could not read %1:\n%2").arg(QDir::toNativeSeparators(path),
                                                              f.errorString()));
        return;
    }
    // UTF-8 explicitly rather than the locale codec: scripts are source text and
    // a comment with a degree sign must survive a trip through another machine.
    const QString source = QString::fromUtf8(f.readAll());
    f.close();
    settings.setValue(QLatin1String(kScriptDirKey), QFileInfo(path).absolutePath());

    m_editor->setPlainText(source);
    // Compile it straight away. A script that has just arrived from elsewhere is
    // exactly the one whose errors the user wants to see now, not at Send.
    onCompile();
    say(tr("Loaded %1.").arg(QDir::toNativeSeparators(path)));
}

// Write the editor's contents to a .lua file.
void ScriptEditorDialog::onSaveScript()
{
    QSettings settings;
    const QString start = settings.value(QLatin1String(kScriptDirKey)).toString();
    QString path =
        QFileDialog::getSaveFileName(this, tr("Save Device Script"), start, tr(kScriptFilter));
    if (path.isEmpty())
        return;
    if (QFileInfo(path).suffix().isEmpty())
        path += QLatin1String(".lua");

    // QFile + explicit flush, not QSaveFile: the atomic-rename path fails on a
    // target another program is holding open, which on this platform includes
    // OneDrive-synced folders and some editors. Same choice, same reason, as
    // Configuration::saveToFile.
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::warning(this, tr("Save Device Script"),
                             tr("Could not write %1:\n%2").arg(QDir::toNativeSeparators(path),
                                                               f.errorString()));
        return;
    }
    const QByteArray data = m_editor->toPlainText().toUtf8();
    if (f.write(data) != data.size() || !f.flush()) {
        const QString why = f.errorString();
        f.close();
        QMessageBox::warning(this, tr("Save Device Script"),
                             tr("Could not write %1:\n%2").arg(QDir::toNativeSeparators(path), why));
        return;
    }
    f.close();
    settings.setValue(QLatin1String(kScriptDirKey), QFileInfo(path).absolutePath());
    say(tr("Saved to %1.").arg(QDir::toNativeSeparators(path)));
}

// True if it is safe to throw away what is in the editor: either nothing would
// be lost, or the user said to discard it. Shared by Cancel and Load so the two
// cannot drift apart on what counts as "changed".
bool ScriptEditorDialog::confirmDiscard(const QString &question)
{
    const QString source = m_editor->toPlainText();
    const bool blank = source.trimmed().isEmpty()
                       || source == QString::fromUtf8(kStarterScript);
    const QString effective = blank ? QString() : source;
    if (effective == m_savedSource)
        return true;
    return QMessageBox::question(this, tr("Device Script"), question,
                                 QMessageBox::Discard | QMessageBox::Cancel,
                                 QMessageBox::Cancel)
           == QMessageBox::Discard;
}

// Cancel, Esc and the title-bar X all land here. Guard against silently
// throwing away a script the user typed or pasted: it lives only in this
// editor until OK writes it to the document, so a stray Esc after pasting a
// long script would lose it with nothing to recover from.
void ScriptEditorDialog::reject()
{
    if (!confirmDiscard(tr("The script has unsaved changes. Discard them?")))
        return; // stay open
    QDialog::reject();
}

void ScriptEditorDialog::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_F1) {
        emit helpRequested(QStringLiteral("device-scripts.html"));
        event->accept();
        return;
    }
    QDialog::keyPressEvent(event);
}

} // namespace ct
