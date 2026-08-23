// Calculations > Device Script… — write a script, compile it, and watch it run
// before it ever reaches a device.
//
// The simulator is the reason this dialog is worth building. A device script
// runs 100 times a second inside a gateway in a vehicle; the feedback loop for
// "did I get the hysteresis right?" would otherwise be edit, Send, drive,
// guess. Here it is edit, Run, read the numbers — against the REAL VM, because
// script_exec.c is compiled into the configurator (see ScriptSimulator).
//
// The script is part of the document, not a separate file: it saves in the
// .ct3 as source and is compiled to bytecode at Send time. That is why this
// dialog writes back through Configuration::setScriptSource() rather than
// owning a buffer of its own — and it must go through the SETTER, because a
// document read off a device may already hold a compiled image with no source
// behind it, and saving a script here REPLACES that image rather than sitting
// beside it.
//
// THE RETAINED-IMAGE CASE, which shapes half of this dialog. A document that
// came off a device by a Get carries Configuration::scriptBytecode(): a working
// compiled script with no Lua behind it and none possible. Opened naively, this
// dialog would then show an EMPTY editor over a document that is carrying a
// script — a decoration disagreeing with its content, which is the one failure
// this program has spent the most effort removing. So in that case the editor
// opens read-only on the DISASSEMBLY of what the document holds, says so in a
// banner, and replacing the image takes pressing Replace and answering a
// question. Typing cannot do it, because there is nothing to type into.
#pragma once

#include <QByteArray>
#include <QDialog>
#include <QHash>

#include "../scripting/script_compiler.h"
#include "../scripting/script_simulator.h"

class QLabel;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QTabWidget;
class QTreeWidget;

namespace ct {

class Configuration;

class ScriptEditorDialog : public QDialog
{
    Q_OBJECT
public:
    explicit ScriptEditorDialog(Configuration &config, QWidget *parent = nullptr);

signals:
    void helpRequested(const QString &pageFileName);

public:
    // Overridden to prompt before discarding unsaved script edits. Public
    // because QDialog::reject() is a public slot and callers (the Cancel button,
    // Esc) reach it as one; narrowing the access would change the base contract.
    void reject() override;

protected:
    void keyPressEvent(QKeyEvent *event) override;

private:
    void buildUi();
    void onCompile();
    void onRunTicks();
    void onStep();
    void onResetSim();
    // Move the script between this editor and a .lua file, for keeping a
    // library or starting from a shipped example. The script itself still
    // lives in the .ct3 document; these are not a second home for it.
    void onLoadScript();
    void onSaveScript();
    // True if the editor's contents may be thrown away — unchanged, or the user
    // chose to discard. Shared by Cancel and Load so they cannot disagree about
    // what counts as an unsaved change.
    bool confirmDiscard(const QString &question);
    void applyAndAccept();
    // Take over from the compiled image the document was read off a device
    // with. Asks first, in as many words, because the image may be the only
    // copy of that script in existence; only after a yes does the editor become
    // writable, and only then can OK replace it.
    void onReplaceRetainedImage();
    // Fill the read-only listing from `image`, or empty it when there is none.
    // Both the retained image (on open) and the compiler's output (after
    // Compile) go through here, so the pane always describes the bytes the
    // document would currently send.
    void showListing(const QByteArray &image);
    // Put the banner in step with what the document holds and what the user has
    // decided about it. One writer, so the banner cannot claim the image is
    // still there after Replace has unlocked the editor.
    void updateRetainedBanner();
    void rebuildChannelTable();
    void showTick(const ScriptSimulator::TickResult &r);
    void say(const QString &text, bool isError = false);
    // Compiles into m_image, reporting errors. Returns false on failure; also
    // used by the OK path, because a document should not carry a script that
    // will fail the next Send.
    bool compileCurrent();
    // Read the channel table's seed values into the simulator. Locale-aware, so
    // a comma decimal ("1,5") is not silently read as zero. Shared by Step and
    // Run so the two cannot drift.
    void seedSignals();
    // Describe a tick's outcome in the cost label — a fault says so, in red,
    // instead of the previous "Peak cost 0" that made a suspended VM look
    // healthy. `context` is "tick" or "run" for the wording.
    void reportResult(const ScriptSimulator::TickResult &r, quint32 peak, int ranTicks);

    Configuration &m_config;
    ScriptSimulator m_sim;
    ScriptSymbols m_symbols;
    // False when the configuration itself will not map (so channel names cannot
    // be resolved). The OK path must not then gate on a compile that can only
    // fail for a reason the user cannot fix from here — see applyAndAccept.
    bool m_symbolsValid = false;
    QHash<QString, int> m_rowByName;  // channel name -> row in m_channels
    QByteArray m_image;
    ScriptCompiler::Result m_lastResult;
    QString m_savedSource;            // scriptSource as it was on open, for the dirty check
    // The compiled image the document held on open, with no source behind it.
    // Empty in every ordinary document. Kept as a member rather than re-read
    // from the configuration because the question the OK path asks is "was this
    // dialog opened on a retained image", which stops being answerable from the
    // configuration the moment a replacement is written into it.
    QByteArray m_retainedImage;
    // Set only by onReplaceRetainedImage(), and the ONE thing that lets OK
    // overwrite m_retainedImage. Its default is what makes the destructive path
    // unreachable by accident.
    bool m_replacingRetained = false;

    QPlainTextEdit *m_editor = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_retainedBanner = nullptr;    // what the document holds, when it is an image
    QPushButton *m_replaceButton = nullptr;
    QTabWidget *m_sourceTabs = nullptr;    // Lua source | Disassembly
    QTreeWidget *m_listing = nullptr;      // the read-only disassembly
    QLabel *m_listingSummary = nullptr;
    QTableWidget *m_channels = nullptr;   // seedable inputs / observed outputs
    QTableWidget *m_state = nullptr;      // persistent registers
    QSpinBox *m_tickCount = nullptr;
    QLabel *m_costLabel = nullptr;
    QPushButton *m_runButton = nullptr;
    QPushButton *m_stepButton = nullptr;
    // Held so updateRetainedBanner() can switch them off while a retained image
    // stands. Compile would compile the empty read-only editor, and Load would
    // put a script into a buffer that OK is not allowed to store — both would
    // look like they had done something.
    QPushButton *m_compileButton = nullptr;
    QPushButton *m_loadButton = nullptr;
    QPushButton *m_saveButton = nullptr;
};

} // namespace ct
