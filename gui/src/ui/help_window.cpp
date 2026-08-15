#include "help_window.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHelpContentWidget>
#include <QHelpEngine>
#include <QHelpIndexWidget>
#include <QHelpLink>
#include <QHelpSearchEngine>
#include <QHelpSearchQueryWidget>
#include <QHelpSearchResultWidget>
#include <QLineEdit>
#include <QSplitter>
#include <QStandardPaths>
#include <QStyle>
#include <QTabWidget>
#include <QTextBrowser>
#include <QToolBar>
#include <QUrl>
#include <QVBoxLayout>

namespace ct {

namespace {
// The manual's start page inside the compiled help. Namespace and virtual
// folder come from help/cantriple.qhp; "pages/" is there because the .qhp
// lists its files relative to itself and the pages live in a subdirectory.
const char *kHomeUrl = "qthelp://org.cantriple.devicemanager.10/doc/pages/index.html";
} // namespace

// QTextBrowser's stock loader only knows files and Qt resources, so every
// qthelp:// request — pages, the stylesheet, links between pages — is served
// out of the compiled .qch here. External links leave the manual entirely:
// QTextBrowser cannot render arbitrary web pages, and pretending otherwise
// shows the user a broken half-page, so those go to the system browser.
class HelpBrowser : public QTextBrowser
{
public:
    explicit HelpBrowser(QHelpEngine *engine, QWidget *parent = nullptr)
        : QTextBrowser(parent)
        , m_engine(engine)
    {
    }

    QVariant loadResource(int type, const QUrl &url) override
    {
        if (url.scheme() == QLatin1String("qthelp"))
            return m_engine->fileData(url);
        return QTextBrowser::loadResource(type, url);
    }

protected:
    void doSetSource(const QUrl &url, QTextDocument::ResourceType type) override
    {
        if (url.scheme() == QLatin1String("http") || url.scheme() == QLatin1String("https")) {
            QDesktopServices::openUrl(url);
            return;
        }
        QTextBrowser::doSetSource(url, type);
    }

private:
    QHelpEngine *m_engine;
};

QString HelpWindow::prepareCollection()
{
    // The .qhc/.qch pair is copied — not opened in place — because the help
    // engine treats the collection as its private database: it records
    // filters and the full-text index next to it. Program Files is not a
    // place to be writing databases. The pair must travel together: the .qhc
    // references the .qch by relative name, so a lone copy of either is
    // useless.
    const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + QLatin1String("/help");
    QDir().mkpath(dataDir);
    const QString appDir = QCoreApplication::applicationDirPath();

    const char *names[] = {"cantriple.qhc", "cantriple.qch"};
    for (const char *name : names) {
        const QString shipped = appDir + QLatin1Char('/') + QLatin1String(name);
        const QString local = dataDir + QLatin1Char('/') + QLatin1String(name);
        if (!QFileInfo::exists(shipped))
            continue; // nothing to ship; setupData() reports the absence
        if (QFileInfo::exists(local)) {
            // Refresh only when the shipped copy is newer — the local pair
            // otherwise carries a valid search index worth keeping. Windows
            // preserves the modification time across QFile::copy, which is
            // what makes this comparison meaningful.
            if (QFileInfo(shipped).lastModified() <= QFileInfo(local).lastModified())
                continue;
            if (!QFile::remove(local)) {
                // Locked — a second Device Manager instance holds the engine
                // open. A stale manual beats a broken one: keep the pair we
                // have.
                qWarning("help: cannot refresh %s (file in use?)", name);
                continue;
            }
        }
        if (!QFile::copy(shipped, local)) {
            // The remove above may already have taken the old copy with it;
            // setupData() will report the damage rather than hiding it.
            qWarning("help: failed to copy %s into %s", name, qUtf8Printable(dataDir));
        }
    }
    return dataDir + QLatin1String("/cantriple.qhc");
}

HelpWindow::HelpWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(tr("CAN Triple Device Manager Help"));
    resize(900, 650);

    m_engine = new QHelpEngine(prepareCollection(), this);
    const bool engineReady = m_engine->setupData();

    m_browser = new HelpBrowser(m_engine, this);

    // Navigation — the three buttons a manual needs and no more.
    QToolBar *nav = addToolBar(tr("Navigation"));
    nav->setMovable(false);
    QAction *backAction = nav->addAction(style()->standardIcon(QStyle::SP_ArrowBack), tr("Back"));
    backAction->setShortcut(QKeySequence::Back);
    backAction->setEnabled(false);
    QAction *forwardAction =
        nav->addAction(style()->standardIcon(QStyle::SP_ArrowForward), tr("Forward"));
    forwardAction->setShortcut(QKeySequence::Forward);
    forwardAction->setEnabled(false);
    QAction *homeAction = nav->addAction(style()->standardIcon(QStyle::SP_DirHomeIcon), tr("Home"));
    connect(backAction, &QAction::triggered, m_browser, &QTextBrowser::backward);
    connect(forwardAction, &QAction::triggered, m_browser, &QTextBrowser::forward);
    connect(homeAction, &QAction::triggered, this, [this]() {
        // Guarded like showPage(): with no collection, Home would swap the
        // missing-help explanation for a blank page.
        if (m_helpOk)
            m_browser->setSource(QUrl(QLatin1String(kHomeUrl)));
    });
    connect(m_browser, &QTextBrowser::backwardAvailable, backAction, &QAction::setEnabled);
    connect(m_browser, &QTextBrowser::forwardAvailable, forwardAction, &QAction::setEnabled);

    // Left side: Contents / Index / Search, the classic help-viewer triple.
    auto *tabs = new QTabWidget;
    tabs->setDocumentMode(true);

    QHelpContentWidget *contents = m_engine->contentWidget();
    tabs->addTab(contents, tr("Contents"));

    auto *indexTab = new QWidget;
    auto *indexLayout = new QVBoxLayout(indexTab);
    indexLayout->setContentsMargins(4, 4, 4, 4);
    auto *indexFilter = new QLineEdit;
    indexFilter->setPlaceholderText(tr("Look for:"));
    indexFilter->setClearButtonEnabled(true);
    QHelpIndexWidget *indexWidget = m_engine->indexWidget();
    indexLayout->addWidget(indexFilter);
    indexLayout->addWidget(indexWidget);
    tabs->addTab(indexTab, tr("Index"));

    QHelpSearchEngine *searchEngine = m_engine->searchEngine();
    auto *searchTab = new QWidget;
    auto *searchLayout = new QVBoxLayout(searchTab);
    searchLayout->setContentsMargins(4, 4, 4, 4);
    searchLayout->addWidget(searchEngine->queryWidget());
    searchLayout->addWidget(searchEngine->resultWidget(), 1);
    tabs->addTab(searchTab, tr("Search"));

    auto *splitter = new QSplitter(Qt::Horizontal);
    splitter->addWidget(tabs);
    splitter->addWidget(m_browser);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({260, 640});
    setCentralWidget(splitter);

    // Every pane funnels into the same place: show that page on the right.
    connect(contents, &QHelpContentWidget::linkActivated, this,
            [this](const QUrl &url) { m_browser->setSource(url); });
    connect(indexWidget, &QHelpIndexWidget::documentActivated, this,
            [this](const QHelpLink &document, const QString &) {
                m_browser->setSource(document.url);
            });
    // A keyword bound to several pages reports them all; opening the first
    // beats opening nothing (a picker dialog is not worth it at 20 pages).
    connect(indexWidget, &QHelpIndexWidget::documentsActivated, this,
            [this](const QList<QHelpLink> &documents, const QString &) {
                if (!documents.isEmpty())
                    m_browser->setSource(documents.first().url);
            });
    connect(indexFilter, &QLineEdit::textChanged, indexWidget,
            [indexWidget](const QString &text) { indexWidget->filterIndices(text); });
    connect(indexFilter, &QLineEdit::returnPressed, indexWidget,
            &QHelpIndexWidget::activateCurrentItem);
    connect(searchEngine->queryWidget(), &QHelpSearchQueryWidget::search, this,
            [searchEngine]() { searchEngine->search(searchEngine->queryWidget()->searchInput()); });
    connect(searchEngine->resultWidget(), &QHelpSearchResultWidget::requestShowLink, this,
            [this](const QUrl &url) { m_browser->setSource(url); });

    if (engineReady && !m_engine->fileData(QUrl(QLatin1String(kHomeUrl))).isEmpty()) {
        m_helpOk = true;
        m_browser->setSource(QUrl(QLatin1String(kHomeUrl)));
    } else {
        // No compiled help next to the executable (or a broken collection).
        // Say what is missing and where it was expected instead of showing a
        // silent empty pane.
        m_browser->setPlainText(
            tr("The help files could not be loaded.\n\n"
               "cantriple.qhc and cantriple.qch are expected next to the program\n"
               "(%1) and are built by the 'help_docs' CMake target.")
                .arg(QCoreApplication::applicationDirPath()));
    }
}

void HelpWindow::showPage(const QString &pageFileName)
{
    if (!m_browser) {
        return;
    }
    // With no loaded collection every qthelp:// load yields an empty document,
    // and setSource would replace the constructor's explanation of what is
    // missing with a silently blank pane — F1 from a dialog is precisely the
    // moment that explanation is being asked for. Stay on it.
    if (!m_helpOk) {
        return;
    }
    // Built from kHomeUrl rather than from a second copy of the namespace and
    // virtual folder, so a page URL cannot drift from the home URL the way two
    // hand-written strings eventually do.
    // Braces, not parentheses: QUrl url(QLatin1String(kHomeUrl)) is the most
    // vexing parse and declares a function.
    QUrl url{QLatin1String(kHomeUrl)};
    url.setPath(QStringLiteral("/doc/pages/") + pageFileName);
    m_browser->setSource(url);
}

void HelpWindow::showEvent(QShowEvent *event)
{
    QMainWindow::showEvent(event);
    // Full-text search needs its index built once per shipped documentation.
    // Reindexing 20 small pages is near-instant, so doing it on first show —
    // rather than tracking documentation timestamps — buys correctness (a
    // freshly copied .qch always gets a matching index) for no visible cost.
    if (!m_searchIndexed) {
        m_searchIndexed = true;
        m_engine->searchEngine()->reindexDocumentation();
    }
}

} // namespace ct
