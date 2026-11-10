#include "Tools.h"
#include "ThemeManager.h"
#include "Motion.h"
#include "qrcodegen.hpp"
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScreen>
#include <QSizeGrip>
#include <QStandardPaths>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVariantAnimation>
#include <QPointer>
#include <QSaveFile>
#include <optional>

// ---------------------------------------------------------------------------- QR code
QImage QrDialog::render(const QString& text, int modulePx, int quiet) {
    // 2.0.8: an address too long for a QR code made qrcodegen throw, and the exception closed Pacitum.
    std::optional<qrcodegen::QrCode> made;
    try { made.emplace(qrcodegen::QrCode::encodeText(text.toUtf8().constData(), qrcodegen::QrCode::Ecc::MEDIUM)); }
    catch (...) { return {}; }
    const auto& qr = *made;
    const int n = qr.getSize();
    QImage img((n + 2 * quiet) * modulePx, (n + 2 * quiet) * modulePx, QImage::Format_RGB32);
    img.fill(Qt::white);
    QPainter p(&img);
    p.setPen(Qt::NoPen); p.setBrush(QColor(0x16, 0x19, 0x1E));
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x)
            if (qr.getModule(x, y)) p.drawRect((x + quiet) * modulePx, (y + quiet) * modulePx, modulePx, modulePx);
    return img;
}

QrDialog::QrDialog(const QString& url, QWidget* parent) : QDialog(parent) {
    setWindowTitle(QStringLiteral("QR code for this page"));
    setAttribute(Qt::WA_DeleteOnClose);
    auto* l = new QVBoxLayout(this); l->setContentsMargins(24, 22, 24, 20); l->setSpacing(12);
    auto* title = new QLabel(QStringLiteral("Scan to open on your phone"), this);
    QFont tf = title->font(); tf.setPixelSize(16); tf.setWeight(QFont::DemiBold); title->setFont(tf);
    l->addWidget(title);
    m_img = render(url, 8, 4);
    auto* pic = new QLabel(this);
    if (m_img.isNull()) {   // 2.0.8
        title->setText(QStringLiteral("This address is too long for a QR code"));
        pic->setText(QStringLiteral("Share a shorter link to this page instead."));
        pic->setObjectName(QStringLiteral("RowSub"));
    }
    const int side = 248;
    if (!m_img.isNull()) {
        QPixmap pm = QPixmap::fromImage(m_img.scaled(side * devicePixelRatioF(), side * devicePixelRatioF(), Qt::KeepAspectRatio, Qt::FastTransformation));
        pm.setDevicePixelRatio(devicePixelRatioF());
        pic->setPixmap(pm);
    }
    pic->setAlignment(Qt::AlignCenter); pic->setAccessibleName(QStringLiteral("QR code for %1").arg(url));
    l->addWidget(pic, 0, Qt::AlignHCenter);
    auto* addr = new QLabel(fontMetrics().elidedText(url, Qt::ElideMiddle, 300), this);
    addr->setObjectName(QStringLiteral("RowSub")); addr->setAlignment(Qt::AlignHCenter); addr->setToolTip(url);
    l->addWidget(addr);
    auto* row = new QHBoxLayout(); row->setSpacing(8);
    auto* copy = new QPushButton(QStringLiteral("Copy image"), this);
    auto* save = new QPushButton(QStringLiteral("Save…"), this);
    auto* done = new QPushButton(QStringLiteral("Done"), this); done->setObjectName(QStringLiteral("PrimaryButton")); done->setDefault(true);
    row->addWidget(copy); row->addWidget(save); row->addStretch(1); row->addWidget(done);
    copy->setEnabled(!m_img.isNull()); save->setEnabled(!m_img.isNull());
    l->addLayout(row);
    connect(copy, &QPushButton::clicked, this, [this, copy]{ QApplication::clipboard()->setImage(m_img); copy->setText(QStringLiteral("Copied")); });
    connect(save, &QPushButton::clicked, this, [this]{
        const QString f = QFileDialog::getSaveFileName(this, QStringLiteral("Save QR code"),
            QStandardPaths::writableLocation(QStandardPaths::PicturesLocation) + QStringLiteral("/page-qr.png"), QStringLiteral("PNG image (*.png)"));
        if (!f.isEmpty()) m_img.save(f, "PNG");
    });
    connect(done, &QPushButton::clicked, this, &QDialog::accept);
}

// ---------------------------------------------------------------------------- Screenshot
ScreenshotOverlay::ScreenshotOverlay(const QPixmap& shot, QWidget* parent) : QWidget(parent), m_shot(shot) {
    setAttribute(Qt::WA_DeleteOnClose);
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::CrossCursor);
    m_bar = new QWidget(this); m_bar->setObjectName(QStringLiteral("ShotBar")); m_bar->setAttribute(Qt::WA_StyledBackground, true);
    m_bar->setCursor(Qt::ArrowCursor);
    auto* bl = new QHBoxLayout(m_bar); bl->setContentsMargins(14, 6, 6, 6); bl->setSpacing(6);
    m_hint = new QLabel(QStringLiteral("Drag to select an area, or copy the whole page"), m_bar);
    auto* copy = new QPushButton(QStringLiteral("Copy"), m_bar); copy->setObjectName(QStringLiteral("PrimaryButton"));
    auto* save = new QPushButton(QStringLiteral("Save…"), m_bar);
    auto* cancel = new QPushButton(QStringLiteral("Cancel"), m_bar);
    bl->addWidget(m_hint); bl->addSpacing(8); bl->addWidget(copy); bl->addWidget(save); bl->addWidget(cancel);
    connect(copy, &QPushButton::clicked, this, &ScreenshotOverlay::copy);
    connect(save, &QPushButton::clicked, this, &ScreenshotOverlay::save);
    connect(cancel, &QPushButton::clicked, this, [this]{ emit finished(QString()); close(); });
    // capture "flash": a white veil that fades out (shows the shot was taken)
    if (const int ms = Motion::fadeMs(Motion::Base); ms > 0) {
        auto* a = new QVariantAnimation(this);
        a->setDuration(ms); a->setStartValue(1.0); a->setEndValue(0.0); a->setEasingCurve(QEasingCurve::OutCubic);
        connect(a, &QVariantAnimation::valueChanged, this, [this](const QVariant& v){ m_flash = v.toReal(); update(); });
        a->start(QAbstractAnimation::DeleteWhenStopped);
    } else m_flash = 0.0;
}

void ScreenshotOverlay::resizeEvent(QResizeEvent*) { layoutBar(); }
void ScreenshotOverlay::layoutBar() {
    m_bar->adjustSize();
    m_bar->move((width() - m_bar->width()) / 2, height() - m_bar->height() - 20);
    m_bar->raise();
}

QPixmap ScreenshotOverlay::selected() const {
    if (m_sel.width() < 4 || m_sel.height() < 4) return m_shot;
    const qreal sx = m_shot.width() / qreal(qMax(1, width())), sy = m_shot.height() / qreal(qMax(1, height()));
    const QRect src(qRound(m_sel.x() * sx), qRound(m_sel.y() * sy), qRound(m_sel.width() * sx), qRound(m_sel.height() * sy));
    return m_shot.copy(src);
}

void ScreenshotOverlay::copy() {
    QApplication::clipboard()->setPixmap(selected());
    emit finished(QStringLiteral("Screenshot copied"));
    close();
}

void ScreenshotOverlay::save() {
    const QString name = QStringLiteral("/Pacitum %1.png").arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH.mm.ss")));
    const QString f = QFileDialog::getSaveFileName(this, QStringLiteral("Save screenshot"),
        QStandardPaths::writableLocation(QStandardPaths::PicturesLocation) + name, QStringLiteral("PNG image (*.png)"));
    if (f.isEmpty()) return;
    const bool ok = selected().save(f, "PNG");
    emit finished(ok ? QStringLiteral("Screenshot saved") : QStringLiteral("Couldn't save the screenshot"));
    close();
}

void ScreenshotOverlay::paintEvent(QPaintEvent*) {
    const auto& pal = ThemeManager::current();
    QPainter p(this);
    p.drawPixmap(rect(), m_shot);
    // dim everything outside the selection
    QPainterPath dim; dim.addRect(QRectF(rect()));
    if (!m_sel.isEmpty()) { QPainterPath hole; hole.addRect(QRectF(m_sel)); dim = dim.subtracted(hole); }
    p.fillPath(dim, QColor(0, 0, 0, m_sel.isEmpty() ? 60 : 110));
    if (!m_sel.isEmpty()) {
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(QColor(pal.accent), 2)); p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(m_sel).adjusted(-1, -1, 1, 1));
        const QString size = QStringLiteral("%1 × %2").arg(selected().width()).arg(selected().height());
        QFont f = font(); f.setPixelSize(12); f.setWeight(QFont::DemiBold); p.setFont(f);
        const QRect tag(m_sel.left(), qMax(0, m_sel.top() - 26), QFontMetrics(f).horizontalAdvance(size) + 16, 22);
        p.setPen(Qt::NoPen); p.setBrush(QColor(pal.accent)); p.drawRoundedRect(tag, 6, 6);
        p.setPen(QColor(pal.onAccent)); p.drawText(tag, Qt::AlignCenter, size);
    }
    if (m_flash > 0.0) p.fillRect(rect(), QColor(255, 255, 255, int(150 * m_flash)));
}

void ScreenshotOverlay::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return;
    m_dragging = true; m_origin = e->position().toPoint(); m_sel = QRect(); update();
}
void ScreenshotOverlay::mouseMoveEvent(QMouseEvent* e) {
    if (!m_dragging) return;
    m_sel = QRect(m_origin, e->position().toPoint()).normalized().intersected(rect());
    update();
}
void ScreenshotOverlay::mouseReleaseEvent(QMouseEvent*) {
    m_dragging = false;
    m_hint->setText(m_sel.width() < 4 || m_sel.height() < 4 ? QStringLiteral("Drag to select an area, or copy the whole page")
                                                           : QStringLiteral("Area selected · drag again to change"));
    layoutBar();
}
void ScreenshotOverlay::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_Escape) { emit finished(QString()); close(); return; }
    if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter || e->matches(QKeySequence::Copy)) { copy(); return; }
    QWidget::keyPressEvent(e);
}

// ---------------------------------------------------------------------------- Notes
static QString notesPath() { return QDir::current().absoluteFilePath(QStringLiteral("notes.txt")); }
// 2.0.8: every window has its own Notes panel on the same file; after one saves, the others read it again
// (a panel with stale text used to overwrite what another window had just written).
static QList<QPointer<NotesPanel>>& notesPanels() { static QList<QPointer<NotesPanel>> l; return l; }

NotesPanel::NotesPanel(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("NotesPanel"));
    setAttribute(Qt::WA_StyledBackground, true);
    setFixedWidth(320);
    auto* l = new QVBoxLayout(this); l->setContentsMargins(16, 12, 10, 12); l->setSpacing(8);
    auto* head = new QHBoxLayout(); head->setSpacing(6);
    auto* title = new QLabel(QStringLiteral("Notes"), this); title->setObjectName(QStringLiteral("SettingsH2"));
    auto* close = new QToolButton(this); close->setAutoRaise(true); close->setFixedSize(30, 30);
    close->setIcon(ThemeManager::icon(QStringLiteral("close"), ThemeManager::current().textSecondary, 16));
    close->setToolTip(QStringLiteral("Close notes (Ctrl+Shift+Y)")); close->setAccessibleName(QStringLiteral("Close notes"));
    head->addWidget(title); head->addStretch(1); head->addWidget(close);
    l->addLayout(head);
    m_edit = new QPlainTextEdit(this); m_edit->setObjectName(QStringLiteral("NotesEdit"));
    m_edit->setPlaceholderText(QStringLiteral("Write anything. Notes are saved automatically for this profile."));
    l->addWidget(m_edit, 1);
    m_status = new QLabel(this); m_status->setObjectName(QStringLiteral("RowSub"));
    l->addWidget(m_status);
    QFile f(notesPath());
    if (f.open(QIODevice::ReadOnly)) m_edit->setPlainText(QString::fromUtf8(f.readAll()));
    m_save.setSingleShot(true); m_save.setInterval(600);
    connect(&m_save, &QTimer::timeout, this, &NotesPanel::flush);
    connect(m_edit, &QPlainTextEdit::textChanged, this, [this]{ m_status->setText(QStringLiteral("Editing…")); m_save.start(); });
    connect(close, &QToolButton::clicked, this, &NotesPanel::closeRequested);
    notesPanels().removeAll(nullptr);
    notesPanels() << this;
}
void NotesPanel::reload() {
    QFile f(notesPath());
    const QString text = f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
    if (text == m_edit->toPlainText()) return;
    const QSignalBlocker block(m_edit);   // not an edit: don't schedule a save
    m_edit->setPlainText(text);
    m_status->setText(QStringLiteral("Updated from sync"));
}
void NotesPanel::focusEditor() { m_edit->setFocus(); m_edit->moveCursor(QTextCursor::End); }
void NotesPanel::flush() {
    m_save.stop();
    QSaveFile f(notesPath());   // 2.0.8: atomic, never a half-written notes file
    if (f.open(QIODevice::WriteOnly) && f.write(m_edit->toPlainText().toUtf8()) >= 0 && f.commit()) {
        m_status->setText(QStringLiteral("Saved"));
        for (const auto& other : notesPanels()) if (other && other != this && !other->m_save.isActive()) other->reload();
    } else m_status->setText(QStringLiteral("Couldn't save notes"));
}

// ---------------------------------------------------------------------------- Picture-in-picture
PipWindow::PipWindow(QWidget* view, const QString& title, QWidget* parentWindow)
    : QWidget(nullptr, Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool) {
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(title);
    setObjectName(QStringLiteral("PipWindow"));
    setStyleSheet(QStringLiteral("#PipWindow{background:#000;} #PipBar{background:#111318;} #PipBar QLabel{color:#E8EAEE;font-size:12px;}"
                                 "#PipBar QToolButton{border:none;border-radius:6px;color:#E8EAEE;padding:0 8px;} #PipBar QToolButton:hover{background:#2D323A;}"));
    auto* l = new QVBoxLayout(this); l->setContentsMargins(0, 0, 0, 0); l->setSpacing(0);
    m_titleBar = new QWidget(this); m_titleBar->setObjectName(QStringLiteral("PipBar")); m_titleBar->setAttribute(Qt::WA_StyledBackground, true);
    m_titleBar->setFixedHeight(30); m_titleBar->setCursor(Qt::SizeAllCursor); m_titleBar->installEventFilter(this);
    auto* tl = new QHBoxLayout(m_titleBar); tl->setContentsMargins(10, 0, 4, 0); tl->setSpacing(2);
    auto* name = new QLabel(QFontMetrics(font()).elidedText(title, Qt::ElideRight, 260), m_titleBar);
    auto* back = new QToolButton(m_titleBar); back->setText(QStringLiteral("Back to tab")); back->setFixedHeight(24); back->setCursor(Qt::ArrowCursor);
    auto* close = new QToolButton(m_titleBar); close->setFixedSize(24, 24); close->setCursor(Qt::ArrowCursor);
    close->setIcon(ThemeManager::icon(QStringLiteral("close"), QStringLiteral("#E8EAEE"), 14)); close->setToolTip(QStringLiteral("Close"));
    tl->addWidget(name, 1); tl->addWidget(back); tl->addWidget(close);
    l->addWidget(m_titleBar);
    view->setParent(this);
    l->addWidget(view, 1);
    view->show();
    auto* grip = new QSizeGrip(this); grip->setFixedSize(14, 14);
    auto* gl = new QHBoxLayout(); gl->setContentsMargins(0, 0, 0, 0); gl->addStretch(1); gl->addWidget(grip);
    l->addLayout(gl);
    connect(back, &QToolButton::clicked, this, [this]{ m_closingByReturn = true; emit returnRequested(); });
    connect(close, &QToolButton::clicked, this, [this]{ m_closingByReturn = true; emit returnRequested(); });
    resize(480, 300);
    QScreen* scr = parentWindow && parentWindow->screen() ? parentWindow->screen() : QGuiApplication::primaryScreen();
    if (scr) { const QRect a = scr->availableGeometry(); move(a.right() - width() - 24, a.bottom() - height() - 24); }
}
void PipWindow::closeEvent(QCloseEvent* e) {
    if (!m_closingByReturn) { e->ignore(); m_closingByReturn = true; emit returnRequested(); return; }   // always give the page back first
    QWidget::closeEvent(e);
}
bool PipWindow::eventFilter(QObject* o, QEvent* e) {
    if (o == m_titleBar) {
        if (e->type() == QEvent::MouseButtonPress) { m_dragOffset = static_cast<QMouseEvent*>(e)->globalPosition().toPoint() - frameGeometry().topLeft(); return true; }
        if (e->type() == QEvent::MouseMove && (static_cast<QMouseEvent*>(e)->buttons() & Qt::LeftButton)) {
            move(static_cast<QMouseEvent*>(e)->globalPosition().toPoint() - m_dragOffset); return true; }
    }
    return QWidget::eventFilter(o, e);
}
