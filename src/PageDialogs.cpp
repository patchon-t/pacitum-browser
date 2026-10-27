#include "PageDialogs.h"
#include "ThemeManager.h"
#include <QEventLoop>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QCheckBox>
#include <QPainter>
#include <QPointer>
#include <QKeyEvent>
#include <QResizeEvent>

// 2.0.1 page dialogs (design: "Web pop-ups and page dialogs"): shown near the top of the tab,
// the page dimmed behind; they only block their own tab.
namespace {
class Overlay : public QWidget {
public:
    explicit Overlay(QWidget* tab) : QWidget(tab) {
        setAttribute(Qt::WA_NoSystemBackground);
        setGeometry(tab->rect());
        tab->installEventFilter(this);
    }
    QWidget* card = nullptr;
    std::function<void()> onEscape, onEnter;
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), QColor(10, 8, 9, 92));
        if (!card) return;
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF r = QRectF(card->geometry()).adjusted(-0.5, -0.5, 0.5, 0.5);
        for (int i = 12; i >= 1; --i) { p.setPen(Qt::NoPen); p.setBrush(QColor(0, 0, 0, 3 + (12 - i))); p.drawRoundedRect(r.adjusted(-i, -i + 6, i, i + 6), 20 + i, 20 + i); }
        const auto& pal = ThemeManager::current();
        p.setBrush(QColor(pal.toolbar)); p.setPen(QPen(QColor(pal.border), 1)); p.drawRoundedRect(r, 20, 20);
    }
    bool eventFilter(QObject* o, QEvent* e) override {
        if (o == parentWidget() && e->type() == QEvent::Resize) { setGeometry(parentWidget()->rect()); place(); }
        return false;
    }
    void resizeEvent(QResizeEvent*) override { place(); }
    void keyPressEvent(QKeyEvent* e) override {
        if (e->key() == Qt::Key_Escape && onEscape) { onEscape(); return; }
        if ((e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) && onEnter) { onEnter(); return; }
        QWidget::keyPressEvent(e);
    }
public:
    void place() {
        if (!card) return;
        const int w = qMin(420, width() - 32);
        card->setFixedWidth(w);
        card->adjustSize();
        card->move((width() - w) / 2, qMin(96, qMax(16, height() / 8)));
        update();
    }
};

QString btnStyle(bool primary, bool danger = false) {
    const auto& p = ThemeManager::current();
    if (danger) return QStringLiteral("QPushButton{background:%1;color:%2;border:0;border-radius:12px;min-height:40px;min-width:92px;padding:0 18px;font-weight:600;font-size:14px;}").arg(p.accent, p.onAccent);
    return primary
        ? QStringLiteral("QPushButton{background:%1;color:%2;border:0;border-radius:12px;min-height:40px;min-width:92px;padding:0 18px;font-weight:600;font-size:14px;}").arg(p.primary, p.onPrimary)
        : QStringLiteral("QPushButton{background:%1;color:%2;border:0;border-radius:12px;min-height:40px;min-width:92px;padding:0 18px;font-weight:600;font-size:14px;}QPushButton:hover{background:%3;}").arg(p.omnibox, p.text, p.surfaceHover);
}
}

namespace PageDialogs {
Result run(QWidget* tab, Kind kind, const QString& host, const QString& message, const QString& value, bool offerSuppress, bool insecure) {
    Result res;
    if (!tab || !tab->isVisible()) return res;
    const auto& p = ThemeManager::current();
    QPointer<Overlay> ov = new Overlay(tab);
    auto* card = new QWidget(ov); ov->card = card;
    card->setAttribute(Qt::WA_TranslucentBackground);
    auto* l = new QVBoxLayout(card); l->setContentsMargins(20, 18, 20, 20); l->setSpacing(12);
    const QString textCss = QStringLiteral("color:%1;font-size:15px;").arg(p.text), secCss = QStringLiteral("color:%1;font-size:13px;").arg(p.textSecondary);
    QLineEdit* in = nullptr; QLineEdit* user = nullptr; QLineEdit* pass = nullptr; QCheckBox* stop = nullptr;
    const QString fieldCss = QStringLiteral("QLineEdit{background:%1;color:%2;border:0;border-radius:12px;min-height:42px;padding:0 12px;font-size:15px;}"
                                            "QLineEdit:focus{border:2px solid %3;background:%4;}").arg(p.omnibox, p.text, p.primary, p.toolbar);
    if (kind == Leave || kind == Auth) {
        auto* head = new QHBoxLayout; head->setSpacing(14);
        auto* tile = new QLabel(card); tile->setFixedSize(40, 40); tile->setAlignment(Qt::AlignCenter);
        tile->setStyleSheet(QStringLiteral("background:%1;border-radius:12px;").arg(kind == Leave ? p.dangerSoft : p.omnibox));
        tile->setPixmap(ThemeManager::icon(kind == Leave ? QStringLiteral("sign-out") : QStringLiteral("lock"), kind == Leave ? p.accent : p.text, 20).pixmap(20, 20));
        auto* tc = new QVBoxLayout; tc->setSpacing(1);
        auto* t = new QLabel(kind == Leave ? QStringLiteral("Leave this site?") : QStringLiteral("Sign in"), card);
        t->setStyleSheet(QStringLiteral("color:%1;font-size:18px;font-weight:600;").arg(p.text));
        tc->addWidget(t);
        if (kind == Auth) { auto* s = new QLabel(QStringLiteral("%1 asks for a username").arg(host), card); s->setStyleSheet(secCss); tc->addWidget(s); }
        head->addWidget(tile); head->addLayout(tc, 1);
        l->addLayout(head);
        if (kind == Leave) {
            auto* m = new QLabel(QStringLiteral("Changes you made on <b>%1</b> may not be saved.").arg(host.toHtmlEscaped()), card);
            m->setWordWrap(true); m->setStyleSheet(textCss); l->addWidget(m);
        } else {
            if (!message.isEmpty()) { auto* m = new QLabel(message, card); m->setWordWrap(true); m->setStyleSheet(secCss); l->addWidget(m); }
            auto* ul = new QLabel(QStringLiteral("Username"), card); ul->setStyleSheet(QStringLiteral("color:%1;font-size:13px;font-weight:600;").arg(p.text));
            user = new QLineEdit(value, card); user->setStyleSheet(fieldCss);
            auto* pl = new QLabel(QStringLiteral("Password"), card); pl->setStyleSheet(ul->styleSheet());
            pass = new QLineEdit(card); pass->setEchoMode(QLineEdit::Password); pass->setStyleSheet(fieldCss);
            l->addWidget(ul); l->addWidget(user); l->addWidget(pl); l->addWidget(pass);
            if (insecure) {
                auto* w = new QLabel(QStringLiteral("⚠  This connection isn't private (HTTP)"), card);
                w->setStyleSheet(QStringLiteral("background:%1;color:%2;border-radius:10px;padding:8px 12px;font-size:12.5px;").arg(QColor(p.warning).lighter(185).name(), QColor(p.warning).darker(130).name()));
                l->addWidget(w);
            }
        }
    } else {
        auto* says = new QLabel(QStringLiteral("<b>%1</b> says").arg(host.toHtmlEscaped()), card);
        says->setStyleSheet(QStringLiteral("color:%1;font-size:13px;").arg(p.textSecondary));
        auto* m = new QLabel(message, card); m->setWordWrap(true); m->setTextFormat(Qt::PlainText); m->setStyleSheet(textCss);
        m->setTextInteractionFlags(Qt::TextSelectableByMouse);
        l->addWidget(says); l->addWidget(m);
        if (kind == Prompt) { in = new QLineEdit(value, card); in->setStyleSheet(fieldCss); in->selectAll(); l->addWidget(in); }
        if (offerSuppress) {
            stop = new QCheckBox(QStringLiteral("Stop this site from showing more dialogs"), card);
            stop->setStyleSheet(QStringLiteral("QCheckBox{color:%1;font-size:13.5px;spacing:10px;}").arg(p.text));
            l->addWidget(stop);
        }
    }
    auto* row = new QHBoxLayout; row->setSpacing(8); row->addStretch(1);
    QPushButton* cancel = nullptr;
    if (kind != Alert) {
        cancel = new QPushButton(kind == Leave ? QStringLiteral("Stay") : QStringLiteral("Cancel"), card);
        cancel->setStyleSheet(btnStyle(false)); cancel->setCursor(Qt::PointingHandCursor); row->addWidget(cancel);
    }
    auto* ok = new QPushButton(kind == Leave ? QStringLiteral("Leave") : kind == Auth ? QStringLiteral("Sign in") : QStringLiteral("OK"), card);
    ok->setStyleSheet(btnStyle(true, kind == Leave)); ok->setCursor(Qt::PointingHandCursor); ok->setDefault(true);
    row->addWidget(ok);
    l->addSpacing(4); l->addLayout(row);

    QEventLoop loop;
    auto finish = [&](bool accepted) {
        res.ok = accepted;
        if (in) res.text = in->text();
        if (user) res.user = user->text();
        if (pass) res.pass = pass->text();
        if (stop) res.suppress = stop->isChecked();
        loop.quit();
    };
    QObject::connect(ok, &QPushButton::clicked, &loop, [&]{ finish(true); });
    if (cancel) QObject::connect(cancel, &QPushButton::clicked, &loop, [&]{ finish(false); });
    ov->onEscape = [&]{ finish(kind == Alert); };
    ov->onEnter = [&]{ finish(true); };
    QObject::connect(ov.data(), &QObject::destroyed, &loop, &QEventLoop::quit);   // tab closed
    ov->place(); ov->show(); ov->raise();
    if (in) in->setFocus(); else if (user) (value.isEmpty() ? user : pass)->setFocus(); else ok->setFocus();
    ov->setFocusPolicy(Qt::StrongFocus);
    loop.exec();
    if (ov) ov->deleteLater();
    return res;
}
}
