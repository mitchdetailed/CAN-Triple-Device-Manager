// Help > Contents… (F1) — the offline manual. A Qt Help collection
// (cantriple.qhc + cantriple.qch, compiled from help/ by qhelpgenerator at
// build time) shown CHM-style: Contents / Index / Search panes on the left,
// a browser on the right. Non-modal, single instance owned by MainWindow.
#pragma once

#include <QMainWindow>

class QHelpEngine;

namespace ct {

class HelpBrowser;

class HelpWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit HelpWindow(QWidget *parent = nullptr);

    // Jump to one page of the manual, named by its file within help/pages
    // (e.g. "firmware-update.html"). Lets a dialog's F1 land on the page about
    // that dialog rather than on the front cover, which for a manual of this
    // size is the difference between context help and a reading assignment.
    // Unknown names are the caller's bug and show the browser's own "cannot
    // open" page rather than failing silently. A no-op when the collection
    // failed to load, so the pane keeps the explanation of what is missing
    // instead of going blank (see m_helpOk).
    void showPage(const QString &pageFileName);

protected:
    void showEvent(QShowEvent *event) override;

private:
    // Returns the path of the collection file to open, first copying the
    // shipped .qhc/.qch pair from the application directory into AppData —
    // the help engine writes to its collection (filters, search index), so
    // it must not be pointed at the read-only install location.
    static QString prepareCollection();

    QHelpEngine *m_engine = nullptr;
    HelpBrowser *m_browser = nullptr;
    bool m_searchIndexed = false;
    // The collection loaded and the front page exists. False means the browser
    // is showing the constructor's what-is-missing explanation, and navigation
    // (showPage, Home) must not replace it with a blank qthelp:// load.
    bool m_helpOk = false;
};

} // namespace ct
