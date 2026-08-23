#include "lua_console_dialog.h"

#include <QCloseEvent>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSplitter>
#include <QStandardPaths>
#include <QTextCharFormat>
#include <QTimer>
#include <QVBoxLayout>

#include "../model/configuration.h"
#include "../scripting/script_runner.h"
#include "../model/user_paths.h"

namespace ct {

// ---------------------------------------------------------------- highlighter

LuaHighlighter::LuaHighlighter(QTextDocument *parent)
    : QSyntaxHighlighter(parent)
{
}

void LuaHighlighter::highlightBlock(const QString &text)
{
    // Order matters: keywords first, strings over them, comments over
    // everything — so "-- local x" is all comment.
    static const QRegularExpression kKeywords(QStringLiteral(
        "\\b(and|break|do|else|elseif|end|false|for|function|goto|if|in|local|"
        "nil|not|or|repeat|return|then|true|until|while)\\b"));
    static const QRegularExpression kNumbers(
        QStringLiteral("\\b(0[xX][0-9a-fA-F]+|\\d+\\.?\\d*([eE][+-]?\\d+)?)\\b"));
    static const QRegularExpression kStrings(
        QStringLiteral("\"[^\"]*\"|'[^']*'"));
    static const QRegularExpression kComment(QStringLiteral("--[^\n]*"));

    QTextCharFormat keyword;
    keyword.setForeground(QColor(0x56, 0x6d, 0xd6));
    keyword.setFontWeight(QFont::Bold);
    QTextCharFormat number;
    number.setForeground(QColor(0xb0, 0x5c, 0x1a));
    QTextCharFormat str;
    str.setForeground(QColor(0x2e, 0x7d, 0x32));
    QTextCharFormat comment;
    comment.setForeground(QColor(0x8a, 0x8a, 0x8a));
    comment.setFontItalic(true);

    auto apply = [&](const QRegularExpression &re, const QTextCharFormat &fmt) {
        auto it = re.globalMatch(text);
        while (it.hasNext()) {
            const auto m = it.next();
            setFormat(int(m.capturedStart()), int(m.capturedLength()), fmt);
        }
    };
    apply(kKeywords, keyword);
    apply(kNumbers, number);
    apply(kStrings, str);
    apply(kComment, comment);
}

// ---------------------------------------------------------------- dialog

namespace {

const char kStarterScript[] =
    "-- Lua Console: scripts run against the OPEN document.\n"
    "-- If a script fails, everything it changed is rolled back.\n"
    "-- Press F1 for the manual page with the full ct.* reference.\n"
    "\n"
    "print(('%d channels, title %q'):format(ct.channelCount(), ct.title()))\n"
    "\n"
    "-- Example: generate a block of channels.\n"
    "-- for i = 1, 16 do\n"
    "--     ct.addChannel{ name = ('Cell Volt %02d'):format(i),\n"
    "--                    quantity = 'Voltage', unit = 'V',\n"
    "--                    dataType = 'u16', baseResolution = 0.001 }\n"
    "-- end\n";

} // namespace

LuaConsoleDialog::LuaConsoleDialog(Configuration &config, QWidget *parent)
    : QDialog(parent), m_config(config)
{
    setWindowFlag(Qt::WindowMinMaxButtonsHint, true);
    resize(780, 620);
    buildUi();
    updateTitle();
}

void LuaConsoleDialog::buildUi()
{
    auto *root = new QVBoxLayout(this);

    auto *splitter = new QSplitter(Qt::Vertical, this);

    m_editor = new QPlainTextEdit(splitter);
    m_editor->setPlainText(QString::fromUtf8(kStarterScript));
    const QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    m_editor->setFont(mono);
    m_editor->setTabStopDistance(4 * QFontMetricsF(mono).horizontalAdvance(u' '));
    new LuaHighlighter(m_editor->document());

    m_output = new QPlainTextEdit(splitter);
    m_output->setReadOnly(true);
    m_output->setFont(mono);
    m_output->setPlaceholderText(tr("print() output and errors appear here."));
    // Cap the scrollback. A script that prints in a tight loop can emit hundreds
    // of thousands of lines in its ten seconds, and an uncapped QTextDocument
    // keeps every one — plus an undo record per line, since read-only does not
    // disable undo — reaching hundreds of megabytes that persist until Clear
    // Output. A block cap discards the oldest lines and (per Qt) also turns the
    // undo stack off, which is the memory that actually accumulates here.
    m_output->setMaximumBlockCount(10000);

    splitter->addWidget(m_editor);
    splitter->addWidget(m_output);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 1);
    root->addWidget(splitter, 1);

    m_status = new QLabel(this);
    root->addWidget(m_status);

    auto *buttons = new QHBoxLayout();
    m_runButton = new QPushButton(tr("&Run  (Ctrl+Return)"), this);
    m_runButton->setDefault(true);
    auto *openButton = new QPushButton(tr("&Open…"), this);
    auto *saveButton = new QPushButton(tr("&Save"), this);
    auto *saveAsButton = new QPushButton(tr("Save &As…"), this);
    auto *clearButton = new QPushButton(tr("&Clear Output"), this);
    auto *helpButton = new QPushButton(tr("Help"), this);
    auto *closeButton = new QPushButton(tr("Close"), this);
    buttons->addWidget(m_runButton);
    buttons->addWidget(openButton);
    buttons->addWidget(saveButton);
    buttons->addWidget(saveAsButton);
    buttons->addWidget(clearButton);
    buttons->addStretch(1);
    buttons->addWidget(helpButton);
    buttons->addWidget(closeButton);
    root->addLayout(buttons);

    connect(m_runButton, &QPushButton::clicked, this, &LuaConsoleDialog::onRun);
    connect(openButton, &QPushButton::clicked, this, &LuaConsoleDialog::onOpen);
    connect(saveButton, &QPushButton::clicked, this, [this]() { onSave(); });
    connect(saveAsButton, &QPushButton::clicked, this, [this]() { onSaveAs(); });
    connect(clearButton, &QPushButton::clicked, m_output, &QPlainTextEdit::clear);
    connect(helpButton, &QPushButton::clicked, this,
            [this]() { emit helpRequested(QStringLiteral("scripting.html")); });
    connect(closeButton, &QPushButton::clicked, this, &QDialog::close);
    connect(m_editor, &QPlainTextEdit::modificationChanged, this,
            [this](bool) { updateTitle(); });
}

void LuaConsoleDialog::updateTitle()
{
    const QString base = m_scriptPath.isEmpty()
                             ? tr("Lua Console")
                             : tr("Lua Console — %1").arg(QFileInfo(m_scriptPath).fileName());
    setWindowTitle(m_editor && m_editor->document()->isModified()
                       ? base + QStringLiteral(" *")
                       : base);
}

void LuaConsoleDialog::appendOutput(const QString &line, bool isError)
{
    if (isError) {
        m_output->appendHtml(QStringLiteral("<span style='color:#c0392b'>%1</span>")
                                 .arg(line.toHtmlEscaped().replace(
                                     QStringLiteral("\n"), QStringLiteral("<br>"))));
    } else {
        m_output->appendPlainText(line);
    }
}

void LuaConsoleDialog::onRun()
{
    // Re-entrancy guard. The run freezes the UI for up to ten seconds, and a
    // user who clicks Run several times into that freeze would otherwise have
    // every click replayed the instant it ends — the script running again and
    // again. The button is disabled below, but a press QUEUED during the freeze
    // is still delivered afterwards, so the disable alone is not enough.
    if (m_running)
        return;
    m_running = true;

    m_runButton->setEnabled(false);
    m_status->setText(tr("Running… (limit 10 s)"));
    // The run is synchronous on this thread — deliberately. The bindings
    // mutate the live document, and no repaint or user event may interleave
    // with that. The sandbox's time limit is what bounds the freeze. (This is
    // also why there is no live Stop button: a Stop would need events pumped
    // mid-run, and a pumped event could open another dialog that mutates the
    // same half-changed document. A real Stop needs the sandbox on a worker
    // thread, which is a larger change than this one.)
    repaint();

    ScriptRunner runner(m_config);
    runner.setOutputHandler([this](const QString &line) { appendOutput(line); });
    const QString chunkName = m_scriptPath.isEmpty()
                                  ? QStringLiteral("script")
                                  : QFileInfo(m_scriptPath).fileName();
    const ScriptResult result = runner.run(m_editor->toPlainText(), chunkName);

    if (result.ok) {
        m_status->setText(result.mutated
                              ? tr("OK in %1 ms — the document was modified.")
                                    .arg(result.elapsedMs)
                              : tr("OK in %1 ms — no changes made.").arg(result.elapsedMs));
        if (result.mutated) {
            emit configurationChanged();
        }
    } else {
        appendOutput(result.error, /*isError=*/true);
        m_status->setText(result.rolledBack
                              ? tr("Failed — every change the script made was rolled back.")
                              : tr("Failed — nothing was changed."));
    }

    // Discard any Run clicks that piled up during the freeze BEFORE re-enabling,
    // so they land on the still-disabled button and are ignored. The 0 ms timer
    // fires after those queued events have been processed; only then does Run
    // come back. This is what stops one impatient user from queuing ten runs.
    m_running = false;
    QCoreApplication::removePostedEvents(m_runButton, QEvent::MouseButtonPress);
    QTimer::singleShot(0, m_runButton, [this]() { m_runButton->setEnabled(true); });
}

QString LuaConsoleDialog::scriptsDirectory() const
{
    // The path is user_paths.h's; creating it on demand is this dialog's, since
    // it is the only caller that wants the folder to exist merely for browsing.
    //
    // Empty when it could not be created, because mkpath's answer — discarded
    // until now — is the only thing here that knows.
    //
    // Nothing the user sees turns on it, and claiming otherwise would be the
    // easy mistake: Qt resolves a starting directory that does not exist to
    // the SAME fallback it gives an empty string, so returning the path anyway
    // would not have opened that folder either. Measured against 6.7 rather
    // than assumed — QFileDialog::directory() lands in the identical place for
    // both. What changes is that this stops handing a caller a path it has
    // already been told is not there, and stops offering the dialog a default
    // that gets silently dropped.
    //
    // No message goes with it. Nothing is at stake until a save, and onSave()
    // reports that with the real error.
    const QString dir = deviceScriptsDirectory();
    if (!QDir().mkpath(dir)) {
        return QString();
    }
    return dir;
}

bool LuaConsoleDialog::maybeDiscardEdits()
{
    if (!m_editor->document()->isModified()) {
        return true;
    }
    const auto answer = QMessageBox::question(
        this, tr("Lua Console"),
        tr("The script has unsaved changes. Save them first?"),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
        QMessageBox::Save);
    if (answer == QMessageBox::Cancel) {
        return false;
    }
    if (answer == QMessageBox::Save) {
        return onSave();
    }
    return true;
}

void LuaConsoleDialog::onOpen()
{
    if (!maybeDiscardEdits()) {
        return;
    }
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Open Script"), scriptsDirectory(),
        tr("Lua scripts (*.lua);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QMessageBox::warning(this, tr("Lua Console"),
                             tr("Could not open %1: %2").arg(path, file.errorString()));
        return;
    }
    m_editor->setPlainText(QString::fromUtf8(file.readAll()));
    m_editor->document()->setModified(false);
    m_scriptPath = path;
    updateTitle();
}

bool LuaConsoleDialog::onSave()
{
    if (m_scriptPath.isEmpty()) {
        return onSaveAs();
    }
    QFile file(m_scriptPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(
            this, tr("Lua Console"),
            tr("Could not save %1: %2").arg(m_scriptPath, file.errorString()));
        return false;
    }
    file.write(m_editor->toPlainText().toUtf8());
    m_editor->document()->setModified(false);
    updateTitle();
    return true;
}

bool LuaConsoleDialog::onSaveAs()
{
    // A bare file name when there is no folder to offer, rather than the
    // "/script.lua" that concatenating onto an empty directory would produce.
    const QString dir = scriptsDirectory();
    const QString suggestion =
        dir.isEmpty() ? QStringLiteral("script.lua") : dir + QStringLiteral("/script.lua");

    const QString path = QFileDialog::getSaveFileName(
        this, tr("Save Script"), suggestion,
        tr("Lua scripts (*.lua)"));
    if (path.isEmpty()) {
        return false;
    }
    m_scriptPath = path;
    return onSave();
}

void LuaConsoleDialog::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_F1) {
        emit helpRequested(QStringLiteral("scripting.html"));
        event->accept();
        return;
    }
    if ((event->modifiers() & Qt::ControlModifier)
        && (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)) {
        onRun();
        event->accept();
        return;
    }
    // Deliberately NOT forwarding to QDialog for Escape-with-edits: closing a
    // buffer of unsaved script over a stray Escape is the classic console
    // annoyance. closeEvent() below is the single gate.
    if (event->key() == Qt::Key_Escape) {
        close();
        event->accept();
        return;
    }
    QDialog::keyPressEvent(event);
}

void LuaConsoleDialog::closeEvent(QCloseEvent *event)
{
    if (maybeDiscardEdits()) {
        event->accept();
    } else {
        event->ignore();
    }
}

} // namespace ct
