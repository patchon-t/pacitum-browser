#pragma once
// 2.0.10 Quick switcher (Ctrl+K): one box that finds open tabs, Pacitum's actions, bookmarks and history,
// or searches the web. It is a window overlay like the tab overview: a dimmed window, a card near the top
// with a search field and a list of results grouped by kind, driven from the keyboard
// (Up / Down / Page Up / Page Down, Enter, Esc) or the mouse.
#include <QIcon>
#include <QVariantAnimation>
#include <QWidget>
#include <functional>

class QLineEdit;

class QuickSwitcher : public QWidget {
    Q_OBJECT
public:
    struct Item {
        enum Kind { Go, Tab, Action, Bookmark, History, Web };   // the list shows the kinds in this order (Go: a typed address)
        Kind kind = Action;
        QString title, sub, hint;   // hint: right-hand text (a shortcut, "Current tab")
        QIcon icon;
        std::function<void()> run;
    };
    using Provider = std::function<QList<Item>(const QString& query)>;

    QuickSwitcher(Provider provider, QWidget* parent);
    void open();
    void dismiss() { finish({}); }

    // How well `words` (the query split on spaces, lower case) match an item: -1 = not every word is
    // found; higher is better (a title that starts with the query beats a word inside the address).
    static int score(const QStringList& words, const QString& title, const QString& sub, const QString& keywords = QString());
    static QStringList words(const QString& query);

    // For tests: the current results and choosing one.
    QList<Item> items() const { return m_items; }
    QString query() const;
    void setQuery(const QString& q);
    void activate(int index);
    int selected() const { return m_sel; }

signals:
    void closed();

protected:
    void paintEvent(QPaintEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
    bool eventFilter(QObject* o, QEvent* e) override;

private:
    struct Row { int item = -1; QString header; int y = 0, h = 0; };   // item < 0: a group header
    void refresh();
    void layoutRows();
    void select(int item);
    void ensureVisible();
    void finish(std::function<void()> then);
    QRect cardRect() const;
    QRect listRect() const;
    int rowAt(const QPoint& p) const;
    static QString kindLabel(Item::Kind k);

    Provider m_provider;
    QLineEdit* m_field = nullptr;
    QList<Item> m_items;
    QList<Row> m_rows;
    int m_sel = -1;          // index into m_items
    int m_scroll = 0;        // pixels scrolled in the list
    int m_contentH = 0;
    qreal m_show = 0.0;      // 0..1 open / close
    bool m_closing = false;
    std::function<void()> m_then;
    QVariantAnimation m_anim;
};
