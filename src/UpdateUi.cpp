#include "UpdateUi.h"
#include "UpdateOverlay.h"
#include "AppSettings.h"
#include "ThemeManager.h"
#include <QApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMainWindow>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScreen>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariantAnimation>
#include <QWindow>
#include <QDateTime>
#include <QFileInfo>
#include <QStyle>
#include <QtMath>
#include <memory>

void pacitumLog(const QString& line);   // main.cpp (startup.log)

using UpdUi::Sheet;
using UpdUi::Hero;

namespace {
QString mb(qint64 bytes) {
    const double m = bytes / 1048576.0;
    return m >= 100 ? QString::number(qRound(m)) : QString::number(m, 'f', 1);
}
QString duration(double secs) {
    if (secs < 1) return QObject::tr("a moment");
    if (secs < 60) return QObject::tr("%1 s").arg(qCeil(secs));
    return QObject::tr("%1 min").arg(qCeil(secs / 60));
}
QColor mix(const QColor& a, const QColor& b, double t) {
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t, a.greenF() + (b.greenF() - a.greenF()) * t, a.blueF() + (b.blueF() - a.blueF()) * t);
}

// The browser window on its way out: a screenshot of it shrinking (100 % → 88 %) and fading.
// The real window is already closed underneath, so nothing can freeze on screen.
class Ghost : public QWidget {
public:
    Ghost(const QPixmap& pm, const QRect& g) : QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint | Qt::WindowTransparentForInput), m_pm(pm) {
        setAttribute(Qt::WA_TranslucentBackground); setAttribute(Qt::WA_TransparentForMouseEvents); setAttribute(Qt::WA_DeleteOnClose);
        setGeometry(g);
        auto* a = new QVariantAnimation(this);
        a->setStartValue(0.0); a->setEndValue(1.0); a->setDuration(Pacitum::reducedMotion() ? 200 : 600); a->setEasingCurve(QEasingCurve::InOutCubic);
        connect(a, &QVariantAnimation::valueChanged, this, [this](const QVariant& v){ m_t = v.toDouble(); update(); });
        connect(a, &QVariantAnimation::finished, this, &QWidget::close);
        a->start(QAbstractAnimation::DeleteWhenStopped);
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        const double s = Pacitum::reducedMotion() ? 1.0 : 1.0 - 0.12 * m_t;
        p.setOpacity(1.0 - m_t);
        p.translate(width() / 2.0, height() / 2.0); p.scale(s, s); p.translate(-width() / 2.0, -height() / 2.0);
        p.drawPixmap(rect(), m_pm);
    }
private:
    QPixmap m_pm; double m_t = 0;
};
}

// ============================================================================================ pill
UpdatePill::UpdatePill(QWidget* parent) : QAbstractButton(parent) {
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::TabFocus);
    m_anim = new QVariantAnimation(this);
    m_anim->setStartValue(0.0); m_anim->setEndValue(1.0); m_anim->setDuration(320); m_anim->setEasingCurve(QEasingCurve::OutBack);
    connect(m_anim, &QVariantAnimation::valueChanged, this, [this](const QVariant& v){ m_appear = v.toDouble(); update(); });
    hide();
}

QString UpdatePill::label() const {
    switch (m_mode) {
    case Available: return tr("Update");
    case Downloading: return QStringLiteral("%1%").arg(qRound(m_frac * 100));
    case Ready: return tr("Restart to update");
    default: return {};
    }
}

QSize UpdatePill::sizeHint() const {
    QFont f = font(); f.setPixelSize(12); f.setWeight(QFont::DemiBold);
    const int text = QFontMetrics(f).horizontalAdvance(m_mode == Downloading ? QStringLiteral("100%") : label());
    return {12 + 16 + 6 + text + 12, 28};
}

void UpdatePill::setMode(Mode m) {
    if (m == m_mode) return;
    const bool appearing = m_mode == Hidden && m != Hidden;
    m_mode = m;
    setVisible(m != Hidden);
    setToolTip(m == Available ? tr("A new version of Pacitum is ready to download")
             : m == Downloading ? tr("Downloading the update. Click to see it.")
             : m == Ready ? tr("The update is downloaded. Restart Pacitum to finish.") : QString());
    updateGeometry();
    if (appearing && !Pacitum::reducedMotion()) { m_appear = 0; m_anim->start(); }
    update();
}

void UpdatePill::paintEvent(QPaintEvent*) {
    if (m_mode == Hidden) return;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const auto& pal = ThemeManager::current();
    const QColor acc(pal.accent);
    const QRectF r = QRectF(rect()).adjusted(1, 2, -1, -2);
    p.translate(r.center()); p.scale(m_appear, m_appear); p.translate(-r.center());
    QColor bg = acc; bg.setAlphaF(m_hover ? 0.26 : 0.16);
    p.setPen(Qt::NoPen); p.setBrush(bg);
    p.drawRoundedRect(r, r.height() / 2, r.height() / 2);
    const QRectF icon(r.left() + 9, r.center().y() - 8, 16, 16);
    if (m_mode == Downloading) {
        QColor track = acc; track.setAlphaF(0.28);
        p.setPen(QPen(track, 2.4)); p.setBrush(Qt::NoBrush); p.drawEllipse(icon.adjusted(1.5, 1.5, -1.5, -1.5));
        p.setPen(QPen(acc, 2.4, Qt::SolidLine, Qt::RoundCap));
        p.drawArc(icon.adjusted(1.5, 1.5, -1.5, -1.5), 90 * 16, -int(m_frac * 360 * 16));
    } else {
        p.setPen(QPen(acc, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin)); p.setBrush(Qt::NoBrush);
        const QPointF c = icon.center();
        if (m_mode == Ready) {   // restart arrow
            p.drawArc(QRectF(c.x() - 6, c.y() - 6, 12, 12), 60 * 16, 290 * 16);
            p.drawPolyline(QPolygonF({QPointF(c.x() + 1.5, c.y() - 8.5), QPointF(c.x() + 4.5, c.y() - 5.5), QPointF(c.x() + 1, c.y() - 3)}));
        } else {
            p.drawLine(QPointF(c.x(), c.y() + 6), QPointF(c.x(), c.y() - 6));
            p.drawPolyline(QPolygonF({QPointF(c.x() - 5, c.y() - 1), QPointF(c.x(), c.y() - 6), QPointF(c.x() + 5, c.y() - 1)}));
        }
    }
    QFont f = font(); f.setPixelSize(12); f.setWeight(QFont::DemiBold);
    p.setFont(f); p.setPen(acc);
    p.drawText(QRectF(icon.right() + 6, r.top(), r.right() - icon.right() - 6 - 10, r.height()), Qt::AlignVCenter | Qt::AlignLeft, label());
}

// ============================================================================================ card
UpdateCard::UpdateCard(const UpdateInfo& info, QWidget* window, QWidget* anchor) : QWidget(window), m_anchor(anchor) {
    setObjectName(QStringLiteral("UpdCard"));
    setAttribute(Qt::WA_TranslucentBackground);
    const auto& pal = ThemeManager::current();
    setStyleSheet(QStringLiteral(
        "QLabel{color:%1;background:transparent;} QLabel#UpdSub{color:%2;}"
        "QPushButton{min-height:32px;max-height:32px;border-radius:16px;padding:0 16px;font-size:13px;}"
        "QPushButton#UpdPrimary{background:%3;color:%4;border:none;font-weight:600;}"
        "QPushButton#UpdGhost{background:transparent;color:%1;border:1px solid %5;}")
        .arg(pal.text, pal.textSecondary, pal.accent, pal.onAccent, pal.border));
    auto* l = new QVBoxLayout(this);
    l->setContentsMargins(12 + 16, 10 + 16, 12 + 16, 14 + 16); l->setSpacing(6);
    auto* hero = new Hero(this);
    UpdUi::Colors c; c.accent = QColor(pal.accent);
    hero->setColors(c); hero->setReducedMotion(Pacitum::reducedMotion());
    hero->setMode(Hero::Arrow);
    l->addWidget(hero, 0, Qt::AlignLeft);
    auto* title = new QLabel(tr("Pacitum %1 is available").arg(info.version), this);
    QFont tf = title->font(); tf.setPixelSize(14); tf.setWeight(QFont::DemiBold); title->setFont(tf);
    l->addSpacing(2); l->addWidget(title);
    const QStringList notes = Updater::noteLines(info.notes, 1);
    QString sub = notes.isEmpty() ? tr("A new version with fixes and improvements.") : notes.first();
    if (!sub.endsWith(QLatin1Char('.')) && !sub.endsWith(QChar(0x2026))) sub += QLatin1Char('.');
    sub += QLatin1Char(' ') + tr("About 2-3 min.");
    auto* text = new QLabel(sub, this); text->setObjectName(QStringLiteral("UpdSub")); text->setWordWrap(true); text->setTextFormat(Qt::PlainText);
    l->addWidget(text);
    auto* bl = new QHBoxLayout(); bl->setSpacing(8);
    auto* upd = new QPushButton(tr("Update"), this); upd->setObjectName(QStringLiteral("UpdPrimary"));
    auto* later = new QPushButton(tr("Later"), this); later->setObjectName(QStringLiteral("UpdGhost"));
    for (auto* b : {upd, later}) b->setCursor(Qt::PointingHandCursor);
    bl->addWidget(upd); bl->addWidget(later); bl->addStretch(1);
    l->addSpacing(6); l->addLayout(bl);
    connect(upd, &QPushButton::clicked, this, &UpdateCard::updateClicked);
    connect(later, &QPushButton::clicked, this, &UpdateCard::laterClicked);
    setFixedWidth(320 + 2 * 16);
    adjustSize();
    window->installEventFilter(this);
    place();
}

void UpdateCard::place() {
    auto* w = parentWidget();
    if (!w) return;
    int right = w->width() - 8, top = 56;
    if (m_anchor) {
        const QPoint br = w->mapFromGlobal(m_anchor->mapToGlobal(QPoint(m_anchor->width(), m_anchor->height())));
        right = qMin(br.x() + 12, w->width()) ; top = br.y() + 2;
    }
    move(right - width(), top - 16 + 6);
}

bool UpdateCard::eventFilter(QObject* o, QEvent* e) {
    if (o == parentWidget() && e->type() == QEvent::Resize) place();
    return QWidget::eventFilter(o, e);
}

void UpdateCard::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const auto& pal = ThemeManager::current();
    const QRectF card = QRectF(rect()).adjusted(16, 16, -16, -16);
    p.setPen(Qt::NoPen);
    for (int i = 8; i >= 1; --i) {
        const double g = i * 1.8;
        p.setBrush(QColor(0, 0, 0, 4 + (8 - i) * 2));
        p.drawRoundedRect(card.adjusted(-g, -g + 6, g, g + 6), 16 + g, 16 + g);
    }
    p.setBrush(QColor(pal.toolbar)); p.setPen(QPen(QColor(pal.border), 1));
    p.drawRoundedRect(card.adjusted(0.5, 0.5, -0.5, -0.5), 16, 16);
}

void UpdateCard::slideIn() {
    place(); show(); raise();
    if (Pacitum::reducedMotion()) return;
    auto* eff = new QGraphicsOpacityEffect(this); eff->setOpacity(0); setGraphicsEffect(eff);
    auto* a = new QVariantAnimation(this);
    m_anim = a;
    a->setStartValue(0.0); a->setEndValue(1.0); a->setDuration(360); a->setEasingCurve(QEasingCurve::OutCubic);
    const QPoint end = pos();
    QPointer<QGraphicsOpacityEffect> fx(eff);
    connect(a, &QVariantAnimation::valueChanged, this, [this, fx, end](const QVariant& v){
        const double t = v.toDouble(); if (fx) fx->setOpacity(t); move(end + QPoint(0, qRound(-12 * (1 - t))));
    });
    connect(a, &QVariantAnimation::finished, this, [this, fx]{ if (fx && graphicsEffect() == fx) setGraphicsEffect(nullptr); });
    a->start(QAbstractAnimation::DeleteWhenStopped);
}

void UpdateCard::slideOut() {
    // Once only: a second call replaced the opacity effect, which deletes the one the first fade
    // was still animating (1.8.9/1.8.10 crashed right after clicking Update).
    if (m_leaving) return;
    m_leaving = true;
    if (Pacitum::reducedMotion() || !isVisible()) { deleteLater(); return; }
    if (m_anim) m_anim->stop();   // the slide-in, if still running (deletes itself when stopped)
    auto* eff = new QGraphicsOpacityEffect(this); setGraphicsEffect(eff);
    auto* a = new QVariantAnimation(this);
    m_anim = a;
    a->setStartValue(1.0); a->setEndValue(0.0); a->setDuration(200); a->setEasingCurve(QEasingCurve::InCubic);
    const QPoint start = pos();
    QPointer<QGraphicsOpacityEffect> fx(eff);
    connect(a, &QVariantAnimation::valueChanged, this, [this, fx, start](const QVariant& v){
        const double t = v.toDouble(); if (fx) fx->setOpacity(t); move(start + QPoint(0, qRound(-8 * (1 - t))));
    });
    connect(a, &QVariantAnimation::finished, this, &QObject::deleteLater);
    a->start(QAbstractAnimation::DeleteWhenStopped);
}

// ============================================================================================ flow
UpdateFlow* UpdateFlow::instance() { static UpdateFlow* f = new UpdateFlow(qApp); return f; }

UpdateFlow::UpdateFlow(QObject* parent) : QObject(parent) {
    m_details = AppSettings::get(QStringLiteral("update_show_details")).toBool(false);
    auto* up = Updater::instance();
    connect(up, &Updater::downloadProgress, this, [this](qint64 r, qint64 t){
        if (m_state != Downloading) return;
        m_received = r; m_total = t;
        const qint64 now = m_clock.elapsed();
        if (now - m_lastMs >= 400) {
            const double inst = (r - m_lastBytes) * 1000.0 / qMax<qint64>(1, now - m_lastMs);
            m_speed = m_speed <= 0 ? inst : m_speed * 0.7 + inst * 0.3;
            m_lastBytes = r; m_lastMs = now;
        }
        refreshProgress();
    });
    connect(up, &Updater::updateAvailable, this, [this](const UpdateInfo& info, bool){
        if (!m_retryFresh) return;
        m_retryFresh = false;
        m_info = info;   // the current file, size and checksum
        startDownload(m_retryWindow ? m_retryWindow.data() : hostWindow());
    });
    auto retryFailed = [this](const QString& err){
        if (!m_retryFresh) return;
        m_retryFresh = false;
        m_state = Failed; m_error = err;
        if (m_sheet && m_sheet->isVisible()) fillFailed(err); else setPills(UpdatePill::Available);
    };
    connect(up, &Updater::checkFailed, this, [retryFailed](const QString& err, bool){ retryFailed(err); });
    connect(up, &Updater::upToDate, this, [this, retryFailed](bool){ retryFailed(tr("The update is no longer available. Check again later.")); });
    connect(up, &Updater::downloadFinished, this, [this](bool ok, const QString& err){
        if (m_state != Downloading) return;
        if (m_cancelled) {   // Cancel: back to "available"
            m_cancelled = false; m_state = Offered;
            hideSheet(); setPills(UpdatePill::Available);
            return;
        }
        if (!ok) {
            m_state = Failed; m_error = err;
            Updater::logLine(QStringLiteral("update: download failed: %1").arg(err));
            setPills(UpdatePill::Available);
            if (m_sheet && m_sheet->isVisible()) fillFailed(err);
            else showSheet(hostWindow());
            return;
        }
        m_state = Ready;
        setPills(UpdatePill::Hidden);
        if (m_sheet && m_sheet->isVisible()) m_sheet->swap([this]{ fillReady(m_sheet); });
        else showSheet(hostWindow());   // the download was hidden: ask to restart now
    });
}

UpdUi::Colors UpdateFlow::colors() const {
    const auto& p = ThemeManager::current();
    UpdUi::Colors c;
    c.bg = QColor(p.toolbar); c.border = QColor(p.border); c.text = QColor(p.text); c.sub = QColor(p.textSecondary);
    c.accent = QColor(p.accent); c.onAccent = QColor(p.onAccent); c.panel = QColor(p.page);
    c.track = mix(c.bg, c.text, 0.10);
    return c;
}

QWidget* UpdateFlow::hostWindow(QWidget* preferred) const {
    if (preferred && preferred->isVisible()) return preferred;
    if (auto* a = QApplication::activeWindow(); a && qobject_cast<QMainWindow*>(a)) return a;
    for (auto* w : QApplication::topLevelWidgets()) if (qobject_cast<QMainWindow*>(w) && w->isVisible()) return w;
    return nullptr;
}

void UpdateFlow::attach(QWidget* window, UpdatePill* pill, QWidget* anchor) {
    Q_UNUSED(window); Q_UNUSED(anchor);
    m_pills.removeAll(nullptr);
    m_pills << pill;
    connect(pill, &QAbstractButton::clicked, this, [this, pill]{
        QWidget* w = pill->window();
        switch (m_state) {
        case Offered: case Failed: startDownload(w); break;
        case Downloading: case Ready: showSheet(w); break;
        default: break;
        }
    });
    // a window opened later shows the same state
    pill->setMode(m_state == Downloading && (!m_sheet || !m_sheet->isVisible()) ? UpdatePill::Downloading
                : m_state == Ready && (!m_sheet || !m_sheet->isVisible()) ? UpdatePill::Ready
                : (m_state == Offered || m_state == Failed) && !m_card ? UpdatePill::Available : UpdatePill::Hidden);
    pill->setProgress(m_total > 0 ? double(m_received) / m_total : 0);
    pill->setProperty("updAnchor", QVariant::fromValue(static_cast<QObject*>(anchor)));
}

void UpdateFlow::setPills(UpdatePill::Mode m) {
    m_pills.removeAll(nullptr);
    for (auto& p : m_pills) p->setMode(m);
}

void UpdateFlow::offer(const UpdateInfo& info, QWidget* window, bool userInitiated) {
    if (m_state == Downloading || m_state == Ready || m_state == Installing) {
        if (userInitiated) showSheet(window);
        return;
    }
    m_info = info;
    m_state = Offered;
    if (m_card) return;
    QWidget* anchor = nullptr;
    for (auto& p : m_pills) if (p && p->window() == window) anchor = qobject_cast<QWidget*>(p->property("updAnchor").value<QObject*>());
    setPills(UpdatePill::Hidden);
    auto* card = new UpdateCard(info, window, anchor);
    m_card = card;
    connect(card, &UpdateCard::updateClicked, this, [this, card]{ startDownload(card->window()); });   // startDownload slides the card out
    connect(card, &UpdateCard::laterClicked, this, [this, card]{ card->slideOut(); setPills(UpdatePill::Available); });
    card->slideIn();
    Updater::logLine(QStringLiteral("update: offered %1 (%2)").arg(info.version, userInitiated ? QStringLiteral("user") : QStringLiteral("background")));
}

void UpdateFlow::startDownload(QWidget* window) {
    if (!m_info.valid()) return;
    m_state = Downloading; m_cancelled = false;
    m_received = 0; m_total = m_info.installerSize; m_speed = 0; m_lastBytes = 0; m_lastMs = 0;
    m_clock.restart();
    if (m_card) m_card->slideOut();
    setPills(UpdatePill::Hidden);
    showSheet(window);
    Updater::instance()->download(m_info);
}

void UpdateFlow::showSheet(QWidget* window) {
    window = hostWindow(window);
    if (!window) return;
    if (m_sheet && m_sheet->parentWidget() != window) { m_sheet->hide(); m_sheet->deleteLater(); }
    if (!m_sheet) {
        m_sheet = new Sheet(colors(), window);
        m_sheet->setReducedMotion(Pacitum::reducedMotion());
        m_sheet->setWindowTitle(tr("Update Pacitum"));
        connect(m_sheet->link(), &QLabel::linkActivated, this, [this]{
            m_details = !m_details;
            AppSettings::set(QStringLiteral("update_show_details"), m_details);
            m_sheet->swap([this]{ fillDownloading(); });
        });
    }
    if (m_state == Downloading) fillDownloading();
    else if (m_state == Ready) fillReady(m_sheet);
    else if (m_state == Failed) fillFailed(m_error);   // not visible yet: applied without the fade
    else return;
    if (m_state == Downloading || m_state == Ready) setPills(UpdatePill::Hidden);
    m_sheet->relayout();
    m_sheet->placeCenter(window->mapToGlobal(window->rect().center()));
    if (!m_sheet->isVisible()) m_sheet->popIn();
    m_sheet->raise(); m_sheet->activateWindow();
}

void UpdateFlow::hideSheet() {
    if (m_sheet) m_sheet->popOut();
}

void UpdateFlow::fillDownloading() {
    Sheet* s = m_sheet;
    if (!s) return;
    if (s->hero()->mode() != Hero::Ring) s->hero()->setMode(Hero::Ring);
    s->bigText()->hide(); s->notes()->hide();
    s->title()->setText(tr("Downloading Pacitum %1").arg(m_info.version));
    s->text()->hide();
    s->link()->setText(QStringLiteral("<a href=\"#\" style=\"color:%1;text-decoration:none;\">%2</a>")
                       .arg(colors().accent.name(), m_details ? tr("Hide details ▴") : tr("Show details ▾")));
    s->link()->show();
    s->detail()->setVisible(m_details);
    s->setButtons(tr("Hide"), tr("Cancel"));
    s->primary()->setObjectName(QStringLiteral("UpdGhost"));   // both quiet in this step (design)
    s->primary()->style()->unpolish(s->primary()); s->primary()->style()->polish(s->primary());
    connect(s->primary(), &QPushButton::clicked, this, [this]{   // Hide: progress moves to the toolbar
        hideSheet();
        setPills(UpdatePill::Downloading);
        refreshProgress();
    });
    connect(s->secondary(), &QPushButton::clicked, this, [this]{
        m_cancelled = true;
        Updater::instance()->cancelDownload();
    });
    refreshProgress();
}

void UpdateFlow::refreshProgress() {
    const double frac = m_total > 0 ? double(m_received) / m_total : -1.0;
    m_pills.removeAll(nullptr);
    for (auto& p : m_pills) p->setProgress(qMax(0.0, frac));
    if (!m_sheet || m_state != Downloading) return;
    m_sheet->hero()->setProgress(frac);
    if (m_details) {
        m_sheet->hero()->setCenterText(tr("%1 MB").arg(mb(m_received)), m_total > 0 ? tr("of %1 MB").arg(mb(m_total)) : QString());
        QStringList parts;
        if (m_speed > 0) parts << tr("%1 MB/s").arg(m_speed / 1048576.0, 0, 'f', 1);
        if (m_speed > 0 && m_total > 0) parts << tr("%1 left").arg(duration((m_total - m_received) / m_speed));
        parts << (m_info.shaUrl.isValid() ? tr("SHA-256 checked when done") : tr("size checked when done"));
        if (frac >= 0.999) parts.last() = m_info.shaUrl.isValid() ? tr("checking SHA-256…") : tr("checking…");
        m_sheet->detail()->setText(parts.join(QStringLiteral(" · ")));
    } else m_sheet->hero()->setCenterText(QString(), QString());
}

void UpdateFlow::fillReady(Sheet* s) {
    if (!s) return;
    s->hero()->setMode(Hero::Check);
    s->bigText()->hide(); s->notes()->hide(); s->link()->hide(); s->detail()->hide();
    s->title()->setText(tr("Update downloaded"));
    s->text()->setText(tr("Restart Pacitum to finish. Your tabs and windows come back exactly as they are."));
    s->text()->show();
    s->setButtons(tr("Restart now"), tr("Later"));
    s->primary()->setObjectName(QStringLiteral("UpdPrimary"));
    s->primary()->style()->unpolish(s->primary()); s->primary()->style()->polish(s->primary());
    connect(s->primary(), &QPushButton::clicked, this, &UpdateFlow::install);
    connect(s->secondary(), &QPushButton::clicked, this, [this]{ hideSheet(); setPills(UpdatePill::Ready); });
}

void UpdateFlow::fillFailed(const QString& error) {
    Sheet* s = m_sheet;
    if (!s) return;
    s->swap([this, s, error]{
        s->hero()->setMode(Hero::Warn);
        s->bigText()->hide(); s->notes()->hide(); s->link()->hide(); s->detail()->hide();
        s->title()->setText(tr("The update didn't download"));
        s->text()->setText(error.isEmpty() ? tr("Check your connection and try again.") : error);
        s->text()->show();
        s->setButtons(tr("Try again"), tr("Close"));
        s->primary()->setObjectName(QStringLiteral("UpdPrimary"));
        s->primary()->style()->unpolish(s->primary()); s->primary()->style()->polish(s->primary());
        connect(s->primary(), &QPushButton::clicked, this, [this, s]{ retryFresh(s->parentWidget()); });
        connect(s->secondary(), &QPushButton::clicked, this, [this]{ hideSheet(); });
    });
}

// 1.9.1: "Try again" asks GitHub again before downloading. The file can change after the check
// (an installer replaced on the release page): the old link then breaks and its size no longer
// matches, so retrying with the old details failed every time.
void UpdateFlow::retryFresh(QWidget* window) {
    m_retryFresh = true;
    m_retryWindow = window;
    m_state = Downloading;   // keeps the background offer from opening the card meanwhile
    m_received = 0; m_total = -1;
    if (m_sheet) m_sheet->swap([this]{ fillDownloading(); });
    Updater::logLine(QStringLiteral("update: retry, checking the release again"));
    Updater::instance()->check(false);
}

// Must read exactly like the helper's pop-up (UpdateOverlay.cpp), which takes over at the same spot.
void UpdateFlow::fillInstalling(Sheet* s) {
    s->hero()->setMode(Hero::Spinner);
    s->bigText()->hide(); s->notes()->hide(); s->link()->hide();
    s->title()->setText(QStringLiteral("Installing update…"));
    s->text()->setText(QStringLiteral("Pacitum closed to update itself. It opens again by itself in a moment. Don't turn off your computer."));
    s->text()->show();
    s->detail()->setText(QStringLiteral("Pacitum %1").arg(m_info.version)); s->detail()->show();
    s->setButtons({});
}

void UpdateFlow::install() {
    if (m_state != Ready || !m_sheet) return;
    m_state = Installing;
    const UpdUi::Colors c = colors();
    {   // what the new version shows in "Update done"
        QJsonObject o{{QStringLiteral("version"), m_info.version}, {QStringLiteral("from"), Updater::currentVersion()},
                      {QStringLiteral("page"), m_info.pageUrl}, {QStringLiteral("at"), double(QDateTime::currentSecsSinceEpoch())},
                      {QStringLiteral("notes"), QJsonArray::fromStringList(Updater::noteLines(m_info.notes, 4))}};
        QFile f(Updater::pendingFile());
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) f.write(QJsonDocument(o).toJson(QJsonDocument::Compact));
    }
    // The pop-up moves to a window of its own (on top, no parent) so it outlives the browser windows.
    auto* inst = new Sheet(c, nullptr, true);
    inst->setReducedMotion(Pacitum::reducedMotion());
    inst->setWindowTitle(tr("Updating Pacitum"));
    fillReady(inst);
    inst->primary()->setEnabled(false);
    inst->relayout();
    inst->placeCenter(m_sheet->cardCenter());
    inst->show();
    m_sheet->hide(); m_sheet->deleteLater();
    inst->swap([this, inst]{ fillInstalling(inst); });
    inst->lift();
    const QPoint center = inst->cardCenter() - QPoint(0, Pacitum::reducedMotion() ? 0 : 14);
    QStringList args{QStringLiteral("--cx"), QString::number(center.x()), QStringLiteral("--cy"), QString::number(center.y()),
                     QStringLiteral("--colors"), c.toArg()};
    if (Pacitum::reducedMotion()) args << QStringLiteral("--reduced");
    if (!Updater::instance()->startInstall(args)) {
        QFile::remove(Updater::pendingFile());
        m_state = Ready;
        const QString where = QDir::toNativeSeparators(Updater::instance()->downloadedPath());
        inst->swap([this, inst, where]{
            inst->hero()->setMode(Hero::Check);
            inst->detail()->hide();
#if defined(Q_OS_MACOS)
            inst->title()->setText(tr("Almost done"));
            inst->text()->setText(tr("The new version is open in Finder. Drag Pacitum Browser into Applications, then open it again."));
            inst->setButtons(tr("Quit Pacitum"), tr("Later"));
            connect(inst->primary(), &QPushButton::clicked, qApp, &QCoreApplication::quit);
#else
            inst->title()->setText(tr("Install it yourself"));
            inst->text()->setText(tr("Pacitum couldn't replace itself here. The new version was saved to %1").arg(where));
            inst->setButtons(tr("Show file"), tr("Close"));
            connect(inst->primary(), &QPushButton::clicked, this, [where]{ QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(where).absolutePath())); });
#endif
            connect(inst->secondary(), &QPushButton::clicked, inst, [inst]{ inst->popOut([inst]{ inst->deleteLater(); }); });
        });
        setPills(UpdatePill::Ready);
        return;
    }
    Updater::logLine(QStringLiteral("update: installing %1 from %2; closing windows").arg(m_info.version, Updater::instance()->downloadedPath()));
    // Windows shrink away: a screenshot of each plays the animation while the real window closes
    // at once (sessions are saved in closeEvent), so nothing can sit there "not responding".
    QApplication::setQuitOnLastWindowClosed(false);
    QTimer::singleShot(Pacitum::reducedMotion() ? 0 : 260, this, [this]{
        for (auto* w : QApplication::topLevelWidgets()) {
            auto* mw = qobject_cast<QMainWindow*>(w);
            if (!mw || !mw->isVisible()) continue;
            QPixmap pm;
            if (!mw->isMinimized()) if (auto* scr = mw->screen()) { const QRect g = mw->frameGeometry(); pm = scr->grabWindow(0, g.x() - scr->geometry().x(), g.y() - scr->geometry().y(), g.width(), g.height()); }
            if (!pm.isNull()) { auto* gh = new Ghost(pm, mw->frameGeometry()); gh->show(); }
            mw->close();
        }
    });
    // Quit once the helper's identical pop-up is on screen. Never quit with nothing running: when the
    // helper dies or doesn't show up (antivirus, a missing file), Setup opens with its own window.
    auto* wait = new QTimer(this); wait->setInterval(50);
    auto started = std::make_shared<QElapsedTimer>(); started->start();
    QPointer<Sheet> guard(inst);
    connect(wait, &QTimer::timeout, this, [this, wait, started, guard, where = Updater::instance()->downloadedPath()]{
        const qint64 t = started->elapsed();
        const bool ready = QFile::exists(Updater::readyFile());
        if (t < 900) return;   // let the windows finish shrinking
        bool fallback = false;
        if (!ready) {
#ifdef Q_OS_WIN
            const bool alive = Updater::instance()->helperRunning();
            if (alive && t < 20000) return;          // slow start (first run, antivirus scan): keep waiting
            if (!alive && t < 1500) return;
            fallback = true;
            Updater::logLine(QStringLiteral("update: helper %1 after %2 ms").arg(alive ? QStringLiteral("not on screen") : QStringLiteral("exited")).arg(t));
#else
            if (t < 6000) return;
#endif
        }
        wait->stop();
        if (fallback && !Updater::instance()->runInstallerYourself()) {
            // Nothing could start: say where the file is instead of vanishing.
            if (!guard) { QTimer::singleShot(30, qApp, &QCoreApplication::quit); return; }
            QFile::remove(Updater::pendingFile());
            guard->swap([this, inst = guard.data(), where]{
                inst->hero()->setMode(Hero::Warn);
                inst->detail()->hide();
                inst->title()->setText(tr("Install it yourself"));
                inst->text()->setText(tr("The update couldn't start. The new version was saved to %1").arg(QDir::toNativeSeparators(where)));
                inst->setButtons(tr("Show file"), tr("Close"));
                connect(inst->primary(), &QPushButton::clicked, this, [where]{ QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(where).absolutePath())); });
                connect(inst->secondary(), &QPushButton::clicked, inst, [inst]{ inst->popOut([]{ QCoreApplication::quit(); }); });
            });
            return;
        }
        Updater::logLine(QStringLiteral("update: helper %1; quitting").arg(ready ? QStringLiteral("ready") : QStringLiteral("not confirmed")));
        if (guard) guard->hide();
        QTimer::singleShot(30, qApp, &QCoreApplication::quit);
    });
    wait->start();
}

void UpdateFlow::runSelfTest(QWidget* window) {
    UpdateInfo info;
    info.version = QStringLiteral("99.0.0"); info.tag = QStringLiteral("v99.0.0"); info.name = QStringLiteral("Pacitum test update");
    info.notes = QStringLiteral("- self test");
    info.installerName = QStringLiteral("PacitumBrowser-Setup-99.0.0.exe");
    info.installerUrl = QUrl(QStringLiteral("https://github.com/namo2024mntik-ops/pacitum-browser/releases/download/v99.0.0/PacitumBrowser-Setup-99.0.0.exe"));
    info.installerSize = 1;
    offer(info, window, true);
    QPointer<UpdateCard> card = m_card;
    // Click "Update" mid slide-in (the pop-up opens before the download starts, so it is checked
    // right away; the first request can block a cold CI runner for seconds). Surviving the card's
    // fade-out afterwards is checked by the smoke test still reporting SMOKE OK.
    QTimer::singleShot(150, this, [this, card]{
        if (!card) { Updater::logLine(QStringLiteral("UPDATE UI FAIL no card")); return; }
        emit card->updateClicked();
        const bool ok = m_sheet && m_sheet->isVisible() && (m_state == Downloading || m_state == Failed);
        Updater::logLine(QStringLiteral("UPDATE UI %1 state=%2 sheet=%3").arg(ok ? QStringLiteral("OK") : QStringLiteral("FAIL"))
                         .arg(int(m_state)).arg(m_sheet && m_sheet->isVisible()));
        QTimer::singleShot(1200, this, [this]{
            if (m_state == Downloading) { m_cancelled = true; Updater::instance()->cancelDownload(); }
            if (m_sheet) { m_sheet->hide(); m_sheet->deleteLater(); }
            m_state = Idle; setPills(UpdatePill::Hidden);
        });
    });
}

void UpdateFlow::afterStartup(QWidget* window) {
    if (m_startupDone) return;
    m_startupDone = true;
    QFile f(Updater::pendingFile());
    if (!f.exists()) return;
    QJsonObject o;
    if (f.open(QIODevice::ReadOnly)) o = QJsonDocument::fromJson(f.readAll()).object();
    f.close(); f.remove();
    { QFile m(Updater::markerFile()); if (m.open(QIODevice::WriteOnly)) m.write("1"); }   // the helper's pop-up can go now
    const QString ver = o.value(QStringLiteral("version")).toString();
    const qint64 age = QDateTime::currentSecsSinceEpoch() - qint64(o.value(QStringLiteral("at")).toDouble());
    Updater::logLine(QStringLiteral("update: started after installing %1 (now %2, %3 s ago)").arg(ver, Updater::currentVersion()).arg(age));
    if (ver.isEmpty() || age > 3600 || Updater::compareVersions(Updater::currentVersion(), ver) < 0) return;
    QPointer<QWidget> w(window);
    QTimer::singleShot(650, this, [this, w, o]{ if (w) showDone(w, o); });   // after the helper's pop-up zooms out
}

void UpdateFlow::showDone(QWidget* window, const QJsonObject& pending) {
    auto* s = new Sheet(colors(), window);
    s->setAttribute(Qt::WA_DeleteOnClose);
    s->setReducedMotion(Pacitum::reducedMotion());
    s->setWindowTitle(tr("Pacitum updated"));
    s->hero()->setMode(Hero::Done);
    s->title()->setText(tr("Update done"));
    s->title()->setStyleSheet(QStringLiteral("color:%1;font-size:14px;").arg(colors().sub.name()));
    s->bigText()->setText(Updater::currentVersion());
    s->bigText()->show();
    s->text()->hide();
    QStringList notes;
    for (const auto& v : pending.value(QStringLiteral("notes")).toArray()) notes << v.toString();
    s->setNotes(notes, tr("What's new"));
    const QString page = pending.value(QStringLiteral("page")).toString();
    if (!page.isEmpty()) {
        s->link()->setText(QStringLiteral("<a href=\"%1\" style=\"color:%2;text-decoration:none;\">%3</a>")
                           .arg(page.toHtmlEscaped(), colors().accent.name(), tr("Full release notes ↗")));
        s->link()->show();
        connect(s->link(), &QLabel::linkActivated, s, [](const QString& u){ QDesktopServices::openUrl(QUrl(u)); });
    }
    s->setButtons(tr("Done"));
    connect(s->primary(), &QPushButton::clicked, s, [s]{ s->popOut([s]{ s->close(); }); });
    s->relayout();
    s->placeCenter(window->mapToGlobal(window->rect().center()));
    s->popIn();
    s->bigText()->play(Pacitum::reducedMotion() ? 0 : 380, Pacitum::reducedMotion());
}
