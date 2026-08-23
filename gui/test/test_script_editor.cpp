// The Device Script dialog, driven the way a person drives it.
//
// The logic underneath is covered elsewhere — test_script_compiler for the
// compiler, test_script_sim for the VM and the send path. What is left, and
// what this covers, is the WIRING: that the dialog builds against a real
// configuration, that its buttons reach the right code, that a compile error
// reaches the user instead of being swallowed, and — the one that would be
// silent and expensive — that OK writes the script back into the document and
// Cancel does not.
//
// Runs on the offscreen platform, so it needs no display.

#include <QApplication>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QTreeWidget>

#include <cstdio>

#include "../src/model/channel.h"
#include "../src/model/configuration.h"
#include "../src/scripting/script_compiler.h"
#include "../src/scripting/script_disassembler.h"
#include "../src/ui/script_editor_dialog.h"

static int fails = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);              \
            ++fails;                                                                 \
        }                                                                            \
    } while (0)

namespace {

using namespace ct;

// The same shape test_script_sim uses: one transmit message carrying two
// channels a script can name.
void buildConfig(Configuration &config)
{
    config.clear();
    config.bus[0].enabled = true;
    config.bus[0].rateKbps = 500;

    CommsSection section;
    section.name = QStringLiteral("Script Out");
    section.device = SectionDevice::TransmitMessage;
    section.alignment = SectionAlignment::WordSwap;
    section.baseAddress = 0x400;
    section.transmitRateHz = 10;
    section.messageLengthBytes = 8;
    int bit = 0;
    for (const char *name : { "Engine RPM", "Fan Request" }) {
        Channel ch;
        ch.name = QString::fromUtf8(name);
        ch.dataType = QStringLiteral("u16");
        ch.baseResolution = 1.0;
        ch.decimalPlaces = 0;
        config.catalog().addOrUpdateUserChannel(ch);

        CommsChannelRow row;
        row.channelName = ch.name;
        row.startBit = bit;
        row.bitLength = 16;
        section.rows.append(row);
        bit += 16;
    }
    config.bus[0].sections.append(section);
}

// Widgets by the text on them, which is how a person finds them too.
QPushButton *button(ScriptEditorDialog &d, const QString &text)
{
    for (QPushButton *b : d.findChildren<QPushButton *>()) {
        if (b->text().remove(QLatin1Char('&')).startsWith(text)) {
            return b;
        }
    }
    return nullptr;
}

QTableWidget *channelTable(ScriptEditorDialog &d)
{
    // The first table is Channels, the second Persistent state (creation order).
    const auto tables = d.findChildren<QTableWidget *>();
    return tables.isEmpty() ? nullptr : tables.first();
}

QTableWidget *stateTable(ScriptEditorDialog &d)
{
    const auto tables = d.findChildren<QTableWidget *>();
    return tables.size() < 2 ? nullptr : tables.at(1);
}

int channelRow(QTableWidget *t, const QString &name)
{
    for (int i = 0; i < t->rowCount(); ++i) {
        if (t->item(i, 0)->text() == name) {
            return i;
        }
    }
    return -1;
}

// Click something that puts up a modal message box, and dismiss the box.
//
// QMessageBox::exec() spins its own event loop, so without this the test hangs
// forever waiting for a click nobody is there to make. Timers DO fire inside
// that nested loop, which is what makes this work: the timer is armed first,
// the click blocks, and the timer closes the box from inside.
void clickAndDismiss(QPushButton *b, bool *sawDialog = nullptr)
{
    if (sawDialog) {
        *sawDialog = false;
    }
    QTimer timer;
    timer.setInterval(10);
    QObject::connect(&timer, &QTimer::timeout, [&]() {
        if (QWidget *modal = QApplication::activeModalWidget()) {
            if (qobject_cast<QMessageBox *>(modal)) {
                if (sawDialog) {
                    *sawDialog = true;
                }
                modal->close();
            }
        }
    });
    timer.start();
    b->click();
    timer.stop();
}

// ---------------------------------------------------------------------------

void testOpensAndListsChannels()
{
    Configuration config;
    buildConfig(config);
    ScriptEditorDialog d(config);

    QTableWidget *chans = channelTable(d);
    CHECK(chans != nullptr);
    CHECK(channelRow(chans, QStringLiteral("Engine RPM")) >= 0);
    CHECK(channelRow(chans, QStringLiteral("Fan Request")) >= 0);

    // A document with no script opens on the starter template, not on nothing:
    // an empty box gives no clue what a script here is supposed to look like.
    auto *editor = d.findChild<QPlainTextEdit *>();
    CHECK(editor != nullptr);
    CHECK(editor->toPlainText().contains(QLatin1String("function on_tick()")));

    // Step and Run stay disabled until something compiles — there is nothing to
    // run, and a button that does nothing is worse than one that is greyed.
    CHECK(button(d, QStringLiteral("Step"))->isEnabled() == false);
    CHECK(button(d, QStringLiteral("Run"))->isEnabled() == false);
}

void testCompileAndStep()
{
    Configuration config;
    buildConfig(config);
    ScriptEditorDialog d(config);

    d.findChild<QPlainTextEdit *>()->setPlainText(QStringLiteral(
        "function on_tick()\n"
        "    setSig(\"Fan Request\", sig(\"Engine RPM\") * 2)\n"
        "end\n"));
    button(d, QStringLiteral("Compile"))->click();
    CHECK(button(d, QStringLiteral("Step"))->isEnabled());
    CHECK(button(d, QStringLiteral("Run"))->isEnabled());

    // Seed an input the way the user does — by typing into the table — then
    // step, and read the output back out of the table. That round trip is the
    // whole dialog: if the seeding or the reporting is wired to the wrong slot,
    // everything still "works" and every answer is wrong.
    QTableWidget *chans = channelTable(d);
    chans->item(channelRow(chans, QStringLiteral("Engine RPM")), 1)
        ->setText(QStringLiteral("1500"));
    button(d, QStringLiteral("Step"))->click();
    CHECK(chans->item(channelRow(chans, QStringLiteral("Fan Request")), 1)->text()
          == QLatin1String("3000"));
}

void testStateTableTracksDeclaredRegisters()
{
    Configuration config;
    buildConfig(config);
    ScriptEditorDialog d(config);

    d.findChild<QPlainTextEdit *>()->setPlainText(QStringLiteral(
        "local count = state(0)\n"
        "function on_tick()\n"
        "    count = count + 1\n"
        "end\n"));
    button(d, QStringLiteral("Compile"))->click();

    // Run the default 100 ticks, then check the state view. One row, because
    // the script declared one — showing all 64 would bury the one that matters.
    button(d, QStringLiteral("Run"))->click();
    QTableWidget *st = stateTable(d);
    CHECK(st != nullptr);
    CHECK(st->rowCount() == 1);
    CHECK(st->item(0, 1)->text() == QLatin1String("100"));

    // Reset puts it back to the initialiser.
    button(d, QStringLiteral("Reset"))->click();
    button(d, QStringLiteral("Step"))->click();
    CHECK(st->item(0, 1)->text() == QLatin1String("1"));
}

void testCompileErrorIsReported()
{
    Configuration config;
    buildConfig(config);
    ScriptEditorDialog d(config);

    d.findChild<QPlainTextEdit *>()->setPlainText(QStringLiteral(
        "function on_tick()\n"
        "    setSig(\"No Such Channel\", 1)\n"
        "end\n"));
    button(d, QStringLiteral("Compile"))->click();

    // The status line has to name the thing that is wrong. A message that says
    // only "compile failed" sends the user reading their whole script.
    bool reported = false;
    for (QLabel *l : d.findChildren<QLabel *>()) {
        reported = reported || l->text().contains(QLatin1String("No Such Channel"));
    }
    CHECK(reported);
    // And nothing became runnable.
    CHECK(button(d, QStringLiteral("Step"))->isEnabled() == false);
}

void testOkStoresAndCancelDoesNot()
{
    const QString script = QStringLiteral(
        "function on_tick()\n"
        "    setSig(\"Fan Request\", sig(\"Engine RPM\"))\n"
        "end\n");

    {
        Configuration config;
        buildConfig(config);
        config.setDirty(false);
        ScriptEditorDialog d(config);
        d.findChild<QPlainTextEdit *>()->setPlainText(script);
        // Cancel with unsaved edits now asks before discarding — the script
        // lives only in this editor until OK, so a stray Esc must not lose it.
        // Dismiss the prompt by accepting the default (Cancel = stay open):
        // the dialog must still be open and the document untouched.
        bool asked = false;
        clickAndDismiss(button(d, QStringLiteral("Cancel")), &asked);
        CHECK(asked);
        CHECK(d.result() != QDialog::Accepted);
        CHECK(config.scriptSource().isEmpty());
        CHECK(!config.isDirty());
    }
    {
        Configuration config;
        buildConfig(config);
        config.setDirty(false);
        ScriptEditorDialog d(config);
        d.findChild<QPlainTextEdit *>()->setPlainText(script);
        button(d, QStringLiteral("OK"))->click();
        CHECK(config.scriptSource() == script);
        CHECK(config.isDirty());
    }
}

void testOkRefusesAScriptThatWillNotCompile()
{
    Configuration config;
    buildConfig(config);
    config.setScriptSource(QStringLiteral("function on_tick()\nend\n"));
    config.setDirty(false);

    ScriptEditorDialog d(config);
    d.findChild<QPlainTextEdit *>()->setPlainText(QStringLiteral(
        "function on_tick()\n"
        "    setSig(\"No Such Channel\", 1)\n"
        "end\n"));
    bool warned = false;
    clickAndDismiss(button(d, QStringLiteral("OK")), &warned);

    // The refusal is stated, not silent: a user who pressed OK and watched
    // nothing happen would press it again.
    CHECK(warned);
    // Still open, and the old script untouched. Storing one that does not build
    // would move the failure to the next Send — a different day, a different
    // dialog, and no sign of what caused it.
    CHECK(d.result() != QDialog::Accepted);
    CHECK(config.scriptSource() == QLatin1String("function on_tick()\nend\n"));
}

void testClearingTheScriptRemovesIt()
{
    Configuration config;
    buildConfig(config);
    config.setScriptSource(QStringLiteral(
        "function on_tick()\n"
        "    setSig(\"Fan Request\", 1)\n"
        "end\n"));
    config.setDirty(false);

    ScriptEditorDialog d(config);
    d.findChild<QPlainTextEdit *>()->setPlainText(QString());
    button(d, QStringLiteral("OK"))->click();

    // Empty means NO SCRIPT, which is what a Send then uses to clear one off a
    // device. Storing an empty string instead would leave the document claiming
    // to carry a script that compiles to nothing.
    CHECK(config.scriptSource().isEmpty());
    CHECK(config.isDirty());
}

// ---------------------------------------------------------------------------
// A document whose script is a compiled image read back from a device
// ---------------------------------------------------------------------------
//
// There is no Lua behind such an image and none is possible, so this dialog
// cannot show the script as text. What it must never do is show NOTHING: an
// empty editor over a document that is carrying a working script is a
// decoration disagreeing with its content, and the accident that follows is the
// user typing into what looks like an empty buffer and pressing OK. These tests
// are that guarantee — the banner, the read-only editor, the listing, and the
// rule that only a deliberate Replace can drop the image.

// A compiled image for `config`, the way a Get would have left one in the
// document: real compiler output, so the disassembly below is of a real script.
QByteArray compiledImageFor(Configuration &config, const char *source)
{
    ScriptSymbols syms;
    QString error;
    if (!ScriptSymbols::fromConfiguration(config, &syms, &error)) {
        std::printf("FAIL  symbols: %s\n", error.toUtf8().constData());
        ++fails;
        return QByteArray();
    }
    const auto r = ScriptCompiler::compile(QString::fromUtf8(source), syms);
    if (!r.ok) {
        std::printf("FAIL  compile: %s\n", r.error.toUtf8().constData());
        ++fails;
        return QByteArray();
    }
    return r.image;
}

const char kRetainedSource[] =
    "function on_tick()\n"
    "    setSig(\"Fan Request\", sig(\"Engine RPM\") > 6000)\n"
    "end\n";

void buildRetainedConfig(Configuration &config, QByteArray *imageOut)
{
    buildConfig(config);
    *imageOut = compiledImageFor(config, kRetainedSource);
    config.setScriptBytecode(*imageOut);
    config.setDirty(false);
}

// Answer a modal message box by pressing the button whose text starts with
// `label`, and report whether a box appeared at all. The existing
// clickAndDismiss closes the box, which is the ESCAPE answer; several of the
// questions below have to be answered the other way, and answering them by
// their button text is how a person answers them.
void clickAndAnswer(QPushButton *b, const QString &label, bool *sawDialog = nullptr)
{
    if (sawDialog) {
        *sawDialog = false;
    }
    QTimer timer;
    timer.setInterval(10);
    QObject::connect(&timer, &QTimer::timeout, [&]() {
        auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        if (!box) {
            return;
        }
        if (sawDialog) {
            *sawDialog = true;
        }
        for (QAbstractButton *ab : box->buttons()) {
            if (ab->text().remove(QLatin1Char('&')).startsWith(label)) {
                ab->click();
                return;
            }
        }
        // The button asked for is not there. Close rather than hang, and let the
        // caller's own assertions report the difference.
        box->close();
    });
    timer.start();
    b->click();
    timer.stop();
}

QLabel *bannerLabel(ScriptEditorDialog &d)
{
    for (QLabel *l : d.findChildren<QLabel *>()) {
        if (l->text().contains(QLatin1String("compiled")) && l->isVisibleTo(&d)) {
            return l;
        }
    }
    return nullptr;
}

void testARetainedImageIsShownRatherThanHidden()
{
    Configuration config;
    QByteArray image;
    buildRetainedConfig(config, &image);
    ScriptEditorDialog d(config);

    // THE POINT OF ALL OF THIS: the dialog does not look empty. It says what the
    // document holds, and it opens on the listing of it.
    QLabel *banner = bannerLabel(d);
    CHECK(banner != nullptr);
    if (banner) {
        CHECK(banner->text().contains(QLatin1String("read back from a device")));
        CHECK(banner->text().contains(QLatin1String("no Lua source")));
        CHECK(banner->text().contains(QLatin1String("byte for byte")));
    }

    auto *tabs = d.findChild<QTabWidget *>();
    CHECK(tabs != nullptr);
    if (tabs) {
        CHECK(tabs->currentIndex() == 1); // the Disassembly tab, not the empty editor
    }

    // The listing is the script: one row per instruction, the entry marked, and
    // the count measured against the device's ceiling.
    auto *listing = d.findChild<QTreeWidget *>();
    CHECK(listing != nullptr);
    if (listing) {
        const ScriptListing expected = disassembleScriptImage(image);
        CHECK(expected.valid);
        CHECK(listing->topLevelItemCount() == expected.instructionCount);
        CHECK(listing->topLevelItemCount() > 0);
        if (listing->topLevelItemCount() > 0) {
            CHECK(listing->topLevelItem(0)->text(1) == expected.lines.first().mnemonic);
            CHECK(listing->topLevelItem(listing->topLevelItemCount() - 1)->text(1)
                  == QLatin1String("HALT"));
        }
        // READ-ONLY: no edit trigger reaches it. An editable listing would need a
        // validating assembler behind it, which is deliberately not built.
        CHECK(listing->editTriggers() == QAbstractItemView::NoEditTriggers);
    }
    bool summarised = false;
    for (QLabel *l : d.findChildren<QLabel *>()) {
        summarised = summarised || l->text().contains(QLatin1String("of 1024 instructions"));
    }
    CHECK(summarised);

    // And typing is not one of the things that can happen here. The editor is
    // read-only and the controls that would fill it are off, so the ONLY route
    // to replacing the image is the button that asks first.
    auto *editor = d.findChild<QPlainTextEdit *>();
    CHECK(editor != nullptr);
    if (editor) {
        CHECK(editor->isReadOnly());
        CHECK(editor->toPlainText().isEmpty());
    }
    CHECK(button(d, QStringLiteral("Compile"))->isEnabled() == false);
    CHECK(button(d, QStringLiteral("Load Script"))->isEnabled() == false);
    CHECK(button(d, QStringLiteral("Save Script"))->isEnabled() == false);
    CHECK(button(d, QStringLiteral("Replace")) != nullptr);
}

void testOkLeavesARetainedImageAlone()
{
    Configuration config;
    QByteArray image;
    buildRetainedConfig(config, &image);

    {
        ScriptEditorDialog d(config);
        button(d, QStringLiteral("OK"))->click();

        // Opening the dialog and pressing OK is the commonest thing a user does
        // after a Get. It must not cost them the script: the image is still
        // there, no source was invented for it, and the document was not even
        // dirtied.
        CHECK(config.scriptBytecode() == image);
        CHECK(config.scriptSource().isEmpty());
        CHECK(!config.isDirty());
        CHECK(d.result() == QDialog::Accepted);
    }
    {
        // And it holds because OK CHECKS, not because the editor happened to be
        // empty. Text is put into the buffer here the way no user can — the
        // editor is read-only and every control that could fill it is off — so
        // that what is being tested is the rule and not the lock. A guard that
        // only works while the box stays empty is one refactor from being gone,
        // and the thing it loses is the only copy of a script in existence.
        ScriptEditorDialog d(config);
        d.findChild<QPlainTextEdit *>()->setPlainText(QStringLiteral(
            "function on_tick()\n"
            "    setSig(\"Fan Request\", 1)\n"
            "end\n"));
        button(d, QStringLiteral("OK"))->click();
        CHECK(config.scriptBytecode() == image);
        CHECK(config.scriptSource().isEmpty());
        CHECK(!config.isDirty());
    }
}

void testReplacingIsDeliberate()
{
    Configuration config;
    QByteArray image;
    buildRetainedConfig(config, &image);
    ScriptEditorDialog d(config);

    // Saying no leaves everything exactly as it was — including the editor
    // being read-only, which is what makes "no" mean something.
    bool asked = false;
    clickAndAnswer(button(d, QStringLiteral("Replace")), QStringLiteral("Cancel"), &asked);
    CHECK(asked);
    CHECK(d.findChild<QPlainTextEdit *>()->isReadOnly());
    CHECK(button(d, QStringLiteral("Compile"))->isEnabled() == false);
    CHECK(config.scriptBytecode() == image);

    // Saying yes hands the editor over: writable, on the source tab, with the
    // starter template to write into. The DOCUMENT is still untouched — nothing
    // is lost until OK.
    clickAndAnswer(button(d, QStringLiteral("Replace")), QStringLiteral("Yes"));
    auto *editor = d.findChild<QPlainTextEdit *>();
    CHECK(!editor->isReadOnly());
    CHECK(editor->toPlainText().contains(QLatin1String("function on_tick()")));
    CHECK(d.findChild<QTabWidget *>()->currentIndex() == 0);
    CHECK(button(d, QStringLiteral("Compile"))->isEnabled());
    CHECK(config.scriptBytecode() == image);

    // Now the replacement is written and accepted, and the two halves of the
    // document's script swap over: a source appears and the image is GONE. A
    // document holding both would send whichever the next reader tested first.
    const QString replacement = QStringLiteral(
        "function on_tick()\n"
        "    setSig(\"Fan Request\", 1)\n"
        "end\n");
    editor->setPlainText(replacement);
    button(d, QStringLiteral("OK"))->click();
    CHECK(config.scriptSource() == replacement);
    CHECK(config.scriptBytecode().isEmpty());
    CHECK(config.isDirty());
}

void testReplacingAndThenWritingNothingAsksBeforeLosingTheImage()
{
    // Replace pressed, nothing written, OK. Two honest readings and no way to
    // guess, so it is asked — and the answer that loses the only copy of a
    // script has to be chosen, not defaulted into.
    {
        Configuration config;
        QByteArray image;
        buildRetainedConfig(config, &image);
        ScriptEditorDialog d(config);
        clickAndAnswer(button(d, QStringLiteral("Replace")), QStringLiteral("Yes"));

        bool asked = false;
        clickAndDismiss(button(d, QStringLiteral("OK")), &asked);
        CHECK(asked);                                   // it did ask
        CHECK(config.scriptBytecode() == image);        // and closing it kept the image
        CHECK(!config.isDirty());
    }
    // Choosing to remove it does remove it — the document then has no script at
    // all, which is what makes the next Send clear the device.
    {
        Configuration config;
        QByteArray image;
        buildRetainedConfig(config, &image);
        ScriptEditorDialog d(config);
        clickAndAnswer(button(d, QStringLiteral("Replace")), QStringLiteral("Yes"));
        clickAndAnswer(button(d, QStringLiteral("OK")), QStringLiteral("Remove"));
        CHECK(config.scriptBytecode().isEmpty());
        CHECK(config.scriptSource().isEmpty());
        CHECK(config.isDirty());
    }
}

void testAnOrdinaryDocumentIsUnaffected()
{
    // The banner and everything behind it must be invisible to the ninety-nine
    // documents in a hundred that hold Lua or nothing. This is the control: if
    // it fails, the retained-image case has leaked into the ordinary one.
    Configuration config;
    buildConfig(config);
    ScriptEditorDialog d(config);

    CHECK(bannerLabel(d) == nullptr);
    CHECK(button(d, QStringLiteral("Replace"))->isVisibleTo(&d) == false);
    CHECK(button(d, QStringLiteral("Compile"))->isEnabled());
    CHECK(d.findChild<QPlainTextEdit *>()->isReadOnly() == false);
    CHECK(d.findChild<QTabWidget *>()->currentIndex() == 0);

    // The listing follows what would be SENT, so a compile fills it in — the
    // same view of the same bytes the retained case shows, for a script the user
    // can also read as text.
    d.findChild<QPlainTextEdit *>()->setPlainText(QStringLiteral(
        "function on_tick()\n"
        "    setSig(\"Fan Request\", sig(\"Engine RPM\") * 2)\n"
        "end\n"));
    button(d, QStringLiteral("Compile"))->click();
    auto *listing = d.findChild<QTreeWidget *>();
    CHECK(listing != nullptr);
    if (listing) {
        // Guarded, not merely checked: a regression that left the listing empty
        // would otherwise index topLevelItem(-1), and a crash here takes every
        // other test's output down with it instead of reporting one failure.
        CHECK(listing->topLevelItemCount() > 0);
        if (listing->topLevelItemCount() > 0) {
            CHECK(listing->topLevelItem(listing->topLevelItemCount() - 1)->text(1)
                  == QLatin1String("HALT"));
        }
    }
}

// Load/Save are the only way a script leaves this dialog for a file, and the
// file dialogs they open cannot be driven headlessly — so what is pinned here is
// that the two buttons exist and are wired. Their absence is the failure that
// would otherwise reach a user as "the buttons are gone".
void testLoadAndSaveButtonsExist()
{
    Configuration config;
    buildConfig(config);
    ScriptEditorDialog d(config);

    QPushButton *load = button(d, QStringLiteral("Load Script"));
    QPushButton *save = button(d, QStringLiteral("Save Script"));
    CHECK(load != nullptr);
    CHECK(save != nullptr);
    if (load && save) {
        CHECK(load->isEnabled());
        CHECK(save->isEnabled());
        // Both explain themselves on hover; "Load Script…" beside a
        // document-backed editor is otherwise ambiguous about what it replaces.
        CHECK(!load->toolTip().isEmpty());
        CHECK(!save->toolTip().isEmpty());
    }
}

} // namespace

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    testOpensAndListsChannels();
    testCompileAndStep();
    testStateTableTracksDeclaredRegisters();
    testCompileErrorIsReported();
    testOkStoresAndCancelDoesNot();
    testOkRefusesAScriptThatWillNotCompile();
    testClearingTheScriptRemovesIt();
    testLoadAndSaveButtonsExist();
    testARetainedImageIsShownRatherThanHidden();
    testOkLeavesARetainedImageAlone();
    testReplacingIsDeliberate();
    testReplacingAndThenWritingNothingAsksBeforeLosingTheImage();
    testAnOrdinaryDocumentIsUnaffected();

    if (fails == 0) {
        std::printf("test_script_editor: all checks passed\n");
        return 0;
    }
    std::printf("test_script_editor: %d check(s) failed\n", fails);
    return 1;
}
