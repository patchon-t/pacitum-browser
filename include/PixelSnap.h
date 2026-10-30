#pragma once
#include <QPainter>
#include <QPointF>
#include <QPixmap>
#include <QTransform>
#include <QtMath>

// 2.0.1 crisp icons: at a fractional display scale (125 %, 150 %) an icon drawn at a fractional position lands
// between device pixels and is resampled (soft edges). These helpers put an icon's top-left on a whole device pixel.
namespace PixelSnap {

// Top-left in logical units that maps to a whole device pixel of the painter's target.
inline QPointF point(const QPainter& p, QPointF topLeft) {
    const QTransform t = p.deviceTransform();
    if (t.isRotating()) return topLeft;
    const QPointF d = t.map(topLeft);
    const QPointF snapped(qRound(d.x()), qRound(d.y()));
    bool ok = false;
    const QTransform inv = t.inverted(&ok);
    return ok ? inv.map(snapped) : topLeft;
}

// Draw `pm` (already rendered at the device pixel ratio) centred in `box`, on whole device pixels.
inline void drawCentered(QPainter& p, const QRectF& box, const QPixmap& pm) {
    const qreal dpr = pm.devicePixelRatio() > 0 ? pm.devicePixelRatio() : 1.0;
    const QSizeF logical(pm.width() / dpr, pm.height() / dpr);
    const QPointF tl(box.center().x() - logical.width() / 2.0, box.center().y() - logical.height() / 2.0);
    p.drawPixmap(point(p, tl), pm);
}

}
