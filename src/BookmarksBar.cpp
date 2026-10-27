#include "BookmarksBar.h"
#include "BookmarkManager.h"
#include "ThemeManager.h"
#include "Motion.h"
#include "Widgets.h"
#include <QSet>
#include <QPropertyAnimation>
#include <QFontMetrics>
#include <QPushButton>
#include <QMenu>
#include <QScrollArea>
#include <QAction>
#include <QApplication>
#include <QContextMenuEvent>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>

namespace {
const char* kMime = "application/x-pacitum-bookmark";

// A bookmark on the bar: click opens it, middle-click opens a new tab, drag it onto a folder.
class BookmarkChip : public QPushButton {
public:
    BookmarkChip(const QString& url, QWidget* parent) : QPushButton(parent), m_url(url) {}
    std::function<void()> onMiddle;
protected:
    void mousePressEvent(QMouseEvent* e) override {
        if (e->button() == Qt::LeftButton) m_press = e->position().toPoint();
        if (e->button() == Qt::MiddleButton && onMiddle) { onMiddle(); return; }
        QPushButton::mousePressEvent(e);
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        if ((e->buttons() & Qt::LeftButton) && (e->position().toPoint() - m_press).manhattanLength() > QApplication::startDragDistance()) {
            setDown(false);
            auto* drag = new QDrag(this);
            auto* md = new QMimeData;
            md->setData(kMime, m_url.toUtf8());
            md->setUrls({QUrl(m_url)});   // dropping on a page / another app opens the address
            md->setText(m_url);
            drag->setMimeData(md);
            drag->setPixmap(grab());
            drag->setHotSpot(e->position().toPoint());
            drag->exec(Qt::MoveAction | Qt::CopyAction);
            return;
        }
        QPushButton::mouseMoveEvent(e);
    }
private:
    QString m_url; QPoint m_press;
};

// A folder on the bar: lights up while a bookmark is dragged over it, takes the drop.
class FolderChip : public QPushButton {
public:
    FolderChip(const QString& folder, QWidget* parent) : QPushButton(parent), m_folder(folder) { setAcceptDrops(true); }
    std::function<void(const QString& url)> onDrop;
protected:
    void dragEnterEvent(QDragEnterEvent* e) override {
        if (!e->mimeData()->hasFormat(kMime)) return;
        e->acceptProposedAction(); setDown(true);
    }
    void dragLeaveEvent(QDragLeaveEvent*) override { setDown(false); }
    void dropEvent(QDropEvent* e) override {
        setDown(false);
        const QString url = QString::fromUtf8(e->mimeData()->data(kMime));
        if (!url.isEmpty() && onDrop) { e->acceptProposedAction(); onDrop(url); }
    }
private:
    QString m_folder;
};
}

BookmarksBar::BookmarksBar(BookmarkManager* mgr, QWidget* parent) : QWidget(parent), m_mgr(mgr) {
    setFixedHeight(35);   // 34 + 1 px bottom inset
    setAttribute(Qt::WA_StyledBackground, true);
    setAcceptDrops(true);   // drop a bookmark on the bar = take it out of its folder
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(10,0,10,1); layout->setSpacing(2);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_inner = new QWidget(scroll);
    m_innerLayout = new QHBoxLayout(m_inner);
    m_innerLayout->setContentsMargins(0,0,0,0); m_innerLayout->setSpacing(2);
    m_innerLayout->addStretch();
    m_inner->setLayout(m_innerLayout);
    scroll->setWidget(m_inner);
    scroll->setFrameShape(QFrame::NoFrame);
    layout->addWidget(scroll, 1);
    m_other = new QPushButton(QStringLiteral("All bookmarks"), this);
    m_other->setFixedHeight(26); m_other->setIconSize(QSize(14,14));
    connect(m_other, &QPushButton::clicked, this, [this]{
        Pacitum::PopupMenu m(this);
        for (const QString& f : m_mgr->folders()) {
            QMenu* sub = Pacitum::addSubMenu(&m, QString(f).replace(QLatin1Char('&'), QStringLiteral("&&")), m_theme ? m_theme->secondaryIcon(QStringLiteral("folder"), 14) : QIcon());
            addFolderMenu(sub, f);
        }
        if (!m_mgr->folders().isEmpty()) m.addSeparator();
        addFolderMenu(&m, QString());
        m.exec(m_other->mapToGlobal(QPoint(0, m_other->height() + 4)));
    });
    layout->addWidget(m_other);
    refresh();
}

// Bookmarks of `folder` into menu m (plus "Open all" when there are several).
void BookmarksBar::addFolderMenu(QMenu* m, const QString& folder) {
    const auto list = m_mgr->bookmarksIn(folder);
    for (const auto& bm : list) {
        const QString u = bm.url;
        auto* a = m->addAction(m_theme ? m_theme->secondaryIcon(QStringLiteral("globe"), 14) : QIcon(),
                               QFontMetrics(m->font()).elidedText(bm.title.isEmpty() ? bm.url : bm.title, Qt::ElideRight, 320).replace(QLatin1Char('&'), QStringLiteral("&&")));
        a->setToolTip(u);
        connect(a, &QAction::triggered, this, [this, u]{ emit urlActivated(u); });
    }
    if (list.isEmpty()) m->addAction(folder.isEmpty() ? QStringLiteral("No bookmarks yet") : QStringLiteral("Empty folder"))->setEnabled(false);
    if (list.size() > 1 && !folder.isEmpty()) {
        m->addSeparator();
        connect(m->addAction(QStringLiteral("Open all (%1)").arg(list.size())), &QAction::triggered, this, [this, list]{
            if (list.size() > 15 && QMessageBox::question(this, QStringLiteral("Open all"), QStringLiteral("Open %1 tabs?").arg(list.size())) != QMessageBox::Yes) return;
            for (const auto& b : list) emit urlActivatedInNewTab(b.url);
        });
    }
}

void BookmarksBar::refresh() {
    QSet<QString> before = m_shownUrls; m_shownUrls.clear();
    // remove all except stretch
    while (m_innerLayout->count() > 1) {
        auto* item = m_innerLayout->takeAt(0);
        if (auto* w = item->widget()) { w->hide(); w->deleteLater(); }   // hidden now: no stale chip drawn over the new ones
        delete item;
    }
    auto growIn = [this, &before](QPushButton* btn, const QString& key){
        if (m_primed && !before.contains(key) && isVisible()) {   // kit #18: new chip grows in (maxWidth 0→hint, OutBack) + fades
            Motion::fadeIn(btn, Motion::Medium);
            if (Motion::enabled()) {
                const int w = btn->sizeHint().width();
                btn->setMaximumWidth(0);
                auto* a = new QPropertyAnimation(btn, "maximumWidth", btn);
                a->setDuration(Motion::Medium); a->setStartValue(0); a->setEndValue(w);
                QEasingCurve back(QEasingCurve::OutBack); back.setOvershoot(1.2); a->setEasingCurve(back);
                connect(a, &QPropertyAnimation::finished, btn, [btn]{ btn->setMaximumWidth(QWIDGETSIZE_MAX); });
                a->start(QAbstractAnimation::DeleteWhenStopped);
            }
        }
        m_shownUrls.insert(key);
    };
    // folders first (like Chrome's bar), then the loose bookmarks
    for (const QString& folder : m_mgr->folders()) {
        auto* btn = new FolderChip(folder, m_inner);
        btn->setText(QFontMetrics(btn->font()).elidedText(folder, Qt::ElideRight, 130));
        btn->setFixedHeight(26); btn->setIconSize(QSize(14,14)); btn->setFlat(true);
        if (m_theme) btn->setIcon(m_theme->secondaryIcon(QStringLiteral("folder"), 14));
        btn->setToolTip(QStringLiteral("%1 · %2 bookmarks").arg(folder).arg(m_mgr->bookmarksIn(folder).size()));
        connect(btn, &QPushButton::clicked, this, [this, folder, btn]{ showFolderMenu(folder, btn); });
        btn->onDrop = [this, folder](const QString& url){ m_mgr->moveToFolder(url, folder); changed(); };
        btn->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(btn, &QWidget::customContextMenuRequested, this, [this, folder, btn](const QPoint& p){ showFolderContextMenu(p, folder, btn); });
        m_innerLayout->insertWidget(m_innerLayout->count()-1, btn);
        growIn(btn, QStringLiteral("folder:") + folder);
    }
    for (auto &bm : m_mgr->bookmarksIn(QString())) {
        if (m_innerLayout->count() > 41) break;
        const QString label = bm.title.isEmpty() ? bm.url : bm.title;
        const QString url = bm.url;
        auto* btn = new BookmarkChip(url, m_inner);
        btn->setText(QFontMetrics(btn->font()).elidedText(label, Qt::ElideRight, 130));
        btn->setFixedHeight(26); btn->setIconSize(QSize(14,14));
        if (m_theme) btn->setIcon(m_theme->secondaryIcon(QStringLiteral("globe"), 14));
        btn->setToolTip(bm.url); btn->setFlat(true);
        connect(btn, &QPushButton::clicked, this, [this, url]{ emit urlActivated(url); });
        btn->onMiddle = [this, url]{ emit urlActivatedInNewTab(url); };
        btn->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(btn, &QWidget::customContextMenuRequested, this, [this, url, btn](const QPoint& p){ showContextMenu(p, url, btn); });
        m_innerLayout->insertWidget(m_innerLayout->count()-1, btn);
        growIn(btn, url);
    }
    m_primed = true;
}

void BookmarksBar::showFolderMenu(const QString& folder, QWidget* btn) {
    Pacitum::PopupMenu m(this);
    m.setToolTipsVisible(true);
    addFolderMenu(&m, folder);
    // a bookmark's own right-click menu works inside the drop-down too
    m.setContextMenuPolicy(Qt::CustomContextMenu);
    connect(&m, &QWidget::customContextMenuRequested, this, [this, &m](const QPoint& p){
        if (auto* a = m.actionAt(p); a && !a->toolTip().isEmpty()) { m.close(); showContextMenu(m.mapToGlobal(p) - mapToGlobal(QPoint()), a->toolTip(), this); }
    });
    m.exec(btn->mapToGlobal(QPoint(0, btn->height() + 4)));
}

void BookmarksBar::showContextMenu(const QPoint& pos, const QString& url, QWidget* btn) {
    Pacitum::PopupMenu m(this);
    m.addAction(QStringLiteral("Open"), this, [this, url]{ emit urlActivated(url); });
    m.addAction(QStringLiteral("Open in new tab"), this, [this, url]{ emit urlActivatedInNewTab(url); });
    m.addSeparator();
    m.addAction(QStringLiteral("Edit…"), this, [this, url]{ editBookmark(url); });
    QMenu* move = Pacitum::addSubMenu(&m, QStringLiteral("Move to folder"));
    QString current;
    for (const auto& b : m_mgr->allBookmarks()) if (b.url == url) current = b.folder;
    if (!current.isEmpty()) move->addAction(QStringLiteral("Bookmarks bar"), this, [this, url]{ m_mgr->moveToFolder(url, QString()); changed(); });
    for (const QString& f : m_mgr->folders()) {
        if (f == current) continue;
        move->addAction(m_theme ? m_theme->secondaryIcon(QStringLiteral("folder"), 14) : QIcon(), QString(f).replace(QLatin1Char('&'), QStringLiteral("&&")),
                        this, [this, url, f]{ m_mgr->moveToFolder(url, f); changed(); });
    }
    move->addSeparator();
    move->addAction(QStringLiteral("New folder…"), this, [this, url]{ newFolder(url); });
    m.addSeparator();
    m.addAction(QStringLiteral("Remove"), this, [this, url]{ m_mgr->removeBookmark(url); changed(); });
    m.exec(btn->mapToGlobal(pos));
}

void BookmarksBar::showFolderContextMenu(const QPoint& pos, const QString& folder, QWidget* btn) {
    Pacitum::PopupMenu m(this);
    const auto list = m_mgr->bookmarksIn(folder);
    if (!list.isEmpty()) m.addAction(QStringLiteral("Open all (%1)").arg(list.size()), this, [this, list]{ for (const auto& b : list) emit urlActivatedInNewTab(b.url); });
    m.addAction(QStringLiteral("Rename…"), this, [this, folder]{
        bool ok = false;
        const QString to = QInputDialog::getText(this, QStringLiteral("Rename folder"), QStringLiteral("Folder name"), QLineEdit::Normal, folder, &ok);
        if (ok && m_mgr->renameFolder(folder, to)) changed();
    });
    const auto folders = m_mgr->folders();
    const int at = folders.indexOf(folder);
    if (at > 0) m.addAction(QStringLiteral("Move left"), this, [this, folders, at]{ auto f = folders; f.swapItemsAt(at, at - 1); m_mgr->setFolderOrder(f); changed(); });
    if (at >= 0 && at < folders.size() - 1) m.addAction(QStringLiteral("Move right"), this, [this, folders, at]{ auto f = folders; f.swapItemsAt(at, at + 1); m_mgr->setFolderOrder(f); changed(); });
    m.addSeparator();
    m.addAction(QStringLiteral("Delete folder…"), this, [this, folder, list]{
        if (list.isEmpty()) { m_mgr->removeFolder(folder, true); changed(); return; }
        QMessageBox box(QMessageBox::Question, QStringLiteral("Delete folder"),
                        QStringLiteral("Delete \"%1\"? It has %2 bookmarks.").arg(folder).arg(list.size()), QMessageBox::NoButton, this);
        auto* keep = box.addButton(QStringLiteral("Keep the bookmarks"), QMessageBox::AcceptRole);
        auto* all = box.addButton(QStringLiteral("Delete them too"), QMessageBox::DestructiveRole);
        box.addButton(QMessageBox::Cancel);
        box.exec();
        if (box.clickedButton() == keep) { m_mgr->removeFolder(folder, true); changed(); }
        else if (box.clickedButton() == all) { m_mgr->removeFolder(folder, false); changed(); }
    });
    m.exec(btn->mapToGlobal(pos));
}

void BookmarksBar::contextMenuEvent(QContextMenuEvent* e) {
    Pacitum::PopupMenu m(this);
    m.addAction(m_theme ? m_theme->secondaryIcon(QStringLiteral("folder-add"), 16) : QIcon(), QStringLiteral("New folder…"), this, [this]{ newFolder(); });
    m.exec(e->globalPos());
}

void BookmarksBar::newFolder(const QString& moveUrl) {
    bool ok = false;
    const QString name = QInputDialog::getText(this, QStringLiteral("New folder"), QStringLiteral("Folder name"), QLineEdit::Normal, QString(), &ok).trimmed();
    if (!ok || name.isEmpty()) return;
    m_mgr->addFolder(name);
    if (!moveUrl.isEmpty()) m_mgr->moveToFolder(moveUrl, name);
    changed();
}

void BookmarksBar::editBookmark(const QString& url) {
    QString title;
    for (const auto& b : m_mgr->allBookmarks()) if (b.url == url) title = b.title;
    bool ok = false;
    const QString t = QInputDialog::getText(this, QStringLiteral("Edit bookmark"), QStringLiteral("Name"), QLineEdit::Normal, title, &ok);
    if (!ok) return;
    m_mgr->setTitle(url, t.trimmed().isEmpty() ? url : t.trimmed());
    changed();
}

void BookmarksBar::changed() { refresh(); emit bookmarksChanged(); }

void BookmarksBar::dragEnterEvent(QDragEnterEvent* e) { if (e->mimeData()->hasFormat(kMime)) e->acceptProposedAction(); }

void BookmarksBar::dropEvent(QDropEvent* e) {
    const QString url = QString::fromUtf8(e->mimeData()->data(kMime));
    if (url.isEmpty()) return;
    e->acceptProposedAction();
    m_mgr->moveToFolder(url, QString());   // dropped on the bar itself: out of its folder
    changed();
}

void BookmarksBar::applyTheme(ThemeManager* tm) {
    m_theme = tm;
    if (m_other) m_other->setIcon(tm->secondaryIcon(QStringLiteral("bookmarks"), 14));
    refresh();
}
