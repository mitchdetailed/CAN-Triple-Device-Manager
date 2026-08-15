// Tools > Lua Console… — write, run and save Lua scripts against the open
// document. A thin shell over ScriptRunner: the editor and output panes are
// UI, but everything that matters (the sandbox, the ct.* bindings, the
// all-or-nothing rollback) lives in src/scripting/ and is what test_lua
// exercises headlessly.
//
// Non-modal singleton, like Monitor Channels: the point of a console is to
// alternate between tweaking something in a dialog and re-running a script,
// which a modal window would forbid.
#pragma once

#include <QDialog>
#include <QSyntaxHighlighter>

class QLabel;
class QPlainTextEdit;
class QPushButton;

namespace ct {

class Configuration;

// Keyword/string/comment/number highlighting. Deliberately regex-simple: it
// colours 95% of real scripts correctly and is 60 lines, where a correct Lua
// lexer (long strings, nested comments) is a project. Multi-line strings
// render as code; nobody debugs by colour.
class LuaHighlighter : public QSyntaxHighlighter
{
    Q_OBJECT
public:
    explicit LuaHighlighter(QTextDocument *parent);

protected:
    void highlightBlock(const QString &text) override;
};

class LuaConsoleDialog : public QDialog
{
    Q_OBJECT
public:
    explicit LuaConsoleDialog(Configuration &config, QWidget *parent = nullptr);

signals:
    // The last run changed the document. MainWindow refreshes the same things
    // it refreshes after a Get — title bar, protection state, monitor list.
    void configurationChanged();
    void helpRequested(const QString &pageFileName);

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

private:
    void buildUi();
    void onRun();
    void onOpen();
    bool onSave();   // false if the user cancelled the Save As
    bool onSaveAs();
    void appendOutput(const QString &line, bool isError = false);
    bool maybeDiscardEdits(); // prompt when the editor holds unsaved changes
    QString scriptsDirectory() const;
    void updateTitle();

    Configuration &m_config;
    QString m_scriptPath;      // empty = unsaved buffer
    QPlainTextEdit *m_editor = nullptr;
    QPlainTextEdit *m_output = nullptr;
    QPushButton *m_runButton = nullptr;
    QLabel *m_status = nullptr;
    bool m_running = false;    // a script is executing; re-entrant Run is ignored
};

} // namespace ct
