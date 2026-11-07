#pragma once
// 1.8.5 3D tab overview (Ctrl+Shift+Space): open tabs as cards on a curved 3D carousel.
// 2.0.6 redesign ("3D tab switcher" design): dark backdrop, "N tabs open", Carousel / Stack / Grid
// (setting "switcher_style"), cards with a title bar, arrows, position dots and keyboard hints.
// Cards are cached, pre-scaled snapshots painted with a perspective QTransform, so a frame costs a
// handful of small pixmap draws (no web view is rendered or resized while it runs).
#include <QIcon>
#include <QPixmap>
#include <QPolygonF>
#include <QVariantAnimation>
#include <QWidget>

class TabOverview : public QWidget {
    Q_OBJECT
public:
    struct Card { QPixmap shot; QString title; QIcon icon; QString url; bool asleep = false; };
    TabOverview(const QList<Card>& cards, int current, QWidget* parent);
    void open();
signals:
    void chosen(int index);    // -1 = closed without choosing
protected:
    void paintEvent(QPaintEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
    bool focusNextPrevChild(bool) override { return false; }   // Tab / Shift+Tab browse cards, not move focus away
private:
    void select(int i, bool animate = true);
    void finish(int index);
    QSize cardSize() const;
    void prepareCards();
    void setMode(const QString& m);
    struct Place { QTransform tr; qreal opacity = 1, dim = 0; qreal depth = 0; };
    Place place(int i, const QSizeF& cs, const QPointF& centre) const;
    QRectF stageRect() const;
    int m_active = 0;           // the tab that was current when the overview opened
    QString m_mode = QStringLiteral("carousel");
    QList<QPair<QRectF, QString>> m_buttons;   // top bar / arrows / dots, for clicks
    QList<Card> m_cards;
    QList<QPixmap> m_scaled;    // per card, sized for the current window
    QList<QPolygonF> m_hit;     // last painted outline per card (for clicks)
    int m_sel = 0;
    qreal m_pos = 0.0;          // animated carousel position (card index at the centre)
    qreal m_show = 0.0;         // 0..1 open/close progress
    int m_result = -1;
    bool m_closing = false;
    QVariantAnimation m_posAnim, m_showAnim;
    qreal m_wheelAcc = 0.0;
};
