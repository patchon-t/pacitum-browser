#include "AddressBar.h"
#include "Widgets.h"
#include "Motion.h"
#include <QPainter>
#include <QPainterPath>
#include "HistoryManager.h"
#include "ThemeManager.h"
#include <QHBoxLayout>
#include <QEvent>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QInputMethodEvent>
#include <QStringList>
#include <QStyle>
#include <QAbstractItemView>
#include <QUrl>
#include <QTimer>
#include <QVariantAnimation>
#include <QRegularExpression>
#include <QStandardItemModel>
#include <QStyledItemDelegate>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QJsonDocument>
#include <QJsonArray>
#include <QUrlQuery>
#include <QLocale>
#include <QApplication>

static QColor cssColor(const QString& s) {
    static const QRegularExpression re(QStringLiteral("rgba?\\(\\s*(\\d+)\\s*,\\s*(\\d+)\\s*,\\s*(\\d+)\\s*(?:,\\s*([\\d.]+))?\\s*\\)"));
    auto m = re.match(s);
    if (!m.hasMatch()) return QColor(s);
    QColor c(m.captured(1).toInt(), m.captured(2).toInt(), m.captured(3).toInt());
    if (!m.captured(4).isEmpty()) c.setAlphaF(m.captured(4).toDouble());
    return c;
}

// Forms of a URL the user may be typing the start of: "https://www.a.com/x" is also typed as
// "www.a.com/x" or "a.com/x". Same idea as Chrome, so "gith" can hint "github.com/".
// 1.8.10: the full "https://…" form only counts once the user types a scheme ("h" used to fill in
// "ttps://chatgpt.com/").
static QStringList inlineForms(const QString& url, const QString& query) {
    QStringList f;
    QString s = url;
    const int p = s.indexOf(QStringLiteral("://"));
    if (query.contains(QStringLiteral("://"))) f << url;
    if (p > 0 && p < 12) s = s.mid(p + 3);
    if (s.startsWith(QStringLiteral("www."), Qt::CaseInsensitive)) { if (query.startsWith(QStringLiteral("www."), Qt::CaseInsensitive)) f << s; f << s.mid(4); }
    else f << s;
    return f;
}

// Popup rows. Roles: UserRole = text that goes into the field / is opened, +1 = kind,
// +2 = second line (URL of a history row), +3 = the typed query (bolding).
enum { RoleValue = Qt::UserRole, RoleKind, RoleSub, RoleQuery };
enum { KindSearch = 0, KindHistory = 1, KindUrl = 2 };

// Google-style rows: icon, suggestion with the typed part regular and the rest bold;
// history rows add the page address in a quieter colour.
class OmniDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QColor text, sub, hover, selected; QIcon search, history, globe;
    QSize sizeHint(const QStyleOptionViewItem& o, const QModelIndex&) const override { return {o.rect.width(), 36}; }
    void paint(QPainter* p, const QStyleOptionViewItem& o, const QModelIndex& i) const override {
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        const QRectF r = QRectF(o.rect).adjusted(2, 1, -2, -1);
        if (o.state & QStyle::State_Selected) { p->setPen(Qt::NoPen); p->setBrush(selected); p->drawRoundedRect(r, 10, 10); }
        else if (o.state & QStyle::State_MouseOver) { p->setPen(Qt::NoPen); p->setBrush(hover); p->drawRoundedRect(r, 10, 10); }
        const int kind = i.data(RoleKind).toInt();
        const QIcon& ic = kind == KindSearch ? search : kind == KindHistory ? history : globe;
        ic.paint(p, QRect(int(r.left()) + 12, int(r.center().y()) - 8, 16, 16));
        QRectF tr = r.adjusted(40, 0, -12, 0);
        const QString main = i.data(Qt::DisplayRole).toString(), q = i.data(RoleQuery).toString();
        QFont f = o.font; QFont b = f; b.setWeight(QFont::DemiBold);
        p->setPen(text);
        auto draw = [&](const QString& s, const QFont& font){
            if (s.isEmpty() || tr.width() <= 0) return;
            p->setFont(font);
            const QFontMetricsF fm(font);
            const QString e = fm.elidedText(s, Qt::ElideRight, tr.width());
            p->drawText(tr, Qt::AlignVCenter | Qt::AlignLeft, e);
            tr.setLeft(tr.left() + fm.horizontalAdvance(e));
        };
        if (kind == KindSearch && !q.isEmpty() && main.startsWith(q, Qt::CaseInsensitive)) {
            draw(main.left(q.size()), f); draw(main.mid(q.size()), b);   // "google" + **search console**
        } else draw(main, f);
        const QString s2 = i.data(RoleSub).toString();
        if (!s2.isEmpty() && tr.width() > 60) {
            p->setPen(sub); draw(QStringLiteral("  —  "), f); draw(s2, f);
        }
        p->restore();
    }
};

// Suggest endpoints; all answer ["query", ["s1", "s2", …]].
static QUrl suggestUrl(const QString& engine, const QString& q) {
    QUrl u; QUrlQuery qq;
    if (const QString t = qEnvironmentVariable("PACITUM_SUGGEST_URL"); !t.isEmpty()) {   // tests: any server answering the same JSON
        u = QUrl(t); qq.addQueryItem(QStringLiteral("q"), q); u.setQuery(qq); return u;
    }
    const QString hl = QLocale::system().name().section(QLatin1Char('_'), 0, 0);
    if (engine == QLatin1String("Bing")) { u = QUrl(QStringLiteral("https://api.bing.com/osjson.aspx")); qq.addQueryItem(QStringLiteral("query"), q); }
    else if (engine == QLatin1String("DuckDuckGo")) { u = QUrl(QStringLiteral("https://duckduckgo.com/ac/")); qq.addQueryItem(QStringLiteral("q"), q); qq.addQueryItem(QStringLiteral("type"), QStringLiteral("list")); }
    else if (engine == QLatin1String("Brave")) { u = QUrl(QStringLiteral("https://search.brave.com/api/suggest")); qq.addQueryItem(QStringLiteral("q"), q); }
    else if (engine == QLatin1String("Yahoo")) { u = QUrl(QStringLiteral("https://ff.search.yahoo.com/gossip")); qq.addQueryItem(QStringLiteral("output"), QStringLiteral("fxjson")); qq.addQueryItem(QStringLiteral("command"), q); }
    else {
        u = QUrl(QStringLiteral("https://suggestqueries.google.com/complete/search"));
        qq.addQueryItem(QStringLiteral("client"), QStringLiteral("firefox"));
        qq.addQueryItem(QStringLiteral("ie"), QStringLiteral("utf-8")); qq.addQueryItem(QStringLiteral("oe"), QStringLiteral("utf-8"));
        if (!hl.isEmpty()) qq.addQueryItem(QStringLiteral("hl"), hl);
        qq.addQueryItem(QStringLiteral("q"), q);
    }
    u.setQuery(qq);
    return u;
}

// Typed text that is an address, not words to search for.
static bool looksLikeUrl(const QString& t) {
    if (t.contains(QLatin1Char(' '))) return false;
    if (t.contains(QStringLiteral("://")) || t.startsWith(QStringLiteral("localhost")) || t.startsWith(QStringLiteral("pacitum:"))) return true;
    static const QRegularExpression host(QStringLiteral("^[\\w-]+(\\.[\\w-]+)*\\.[a-zA-Z]{2,}(:\\d+)?([/?#].*)?$"));
    return host.match(t).hasMatch();
}

static constexpr qreal kRadius = 12.0;   // omnibox corner radius (1.9 refresh: rounded rectangle, not a pill)

static bool isInternalNewTab(const QString& url) {
    return url.isEmpty() || url.startsWith(QStringLiteral("pacitum://newtab")) || url == QStringLiteral("about:blank");
}

AddressBar::AddressBar(HistoryManager* hm, QWidget* parent) : QFrame(parent), m_history(hm) {
    setObjectName(QStringLiteral("Omnibox"));
    setAttribute(Qt::WA_StyledBackground, true);
    setFixedHeight(36);
    setMinimumWidth(240);   // spec §4.4: omnibox shrinks to min 240
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(5,0,4,0);
    layout->setSpacing(6);

    // Leading security / site chip (spec §5.2): h28 r14
    m_chip = new QToolButton(this);
    m_chip->setObjectName(QStringLiteral("OmniChip"));
    m_chip->setFixedHeight(28); m_chip->setMinimumWidth(28);
    m_chip->setIconSize(QSize(16,16));
    m_chip->setCursor(Qt::PointingHandCursor);
    m_chip->setToolButtonStyle(Qt::ToolButtonIconOnly);
    m_chip->setFocusPolicy(Qt::NoFocus);
    m_chip->installEventFilter(this);
    connect(m_chip, &QToolButton::clicked, this, [this]{ if (!isInternalNewTab(m_currentUrl)) emit securityIconClicked(); });
    layout->addWidget(m_chip);
    m_securityIcon = nullptr;
    m_permLeft = new QHBoxLayout(); m_permLeft->setContentsMargins(0, 0, 0, 0); m_permLeft->setSpacing(4);
    layout->addLayout(m_permLeft);

    m_input = new QLineEdit(this);
    m_input->setPlaceholderText(QStringLiteral("Search or enter address"));
    m_input->setFrame(false);
    m_input->installEventFilter(this);
    layout->addWidget(m_input, 1);
    // Blurred display: host in text colour, path in text-tertiary, scheme + query hidden.
    m_display = new QLabel(m_input);
    m_display->setObjectName(QStringLiteral("OmniDisplay"));
    m_display->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_display->setTextFormat(Qt::RichText);
    m_display->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_display->hide();

    m_star = new StarButton(this);
    m_star->setToolTip(QStringLiteral("Bookmark this tab (Ctrl+D)"));
    connect(m_star, &QToolButton::clicked, this, &AddressBar::starClicked);
    m_permRight = new QHBoxLayout(); m_permRight->setContentsMargins(0, 0, 0, 0); m_permRight->setSpacing(4);
    layout->addLayout(m_permRight);
    layout->addWidget(m_star);

    m_completer = new QCompleter(this);
    m_completer->popup()->setProperty("_pmNoSlide", true);   // re-positioned while typing: fade only
    m_completer->popup()->setProperty("_pmNoZoom", true);    // (and no zoom on every keystroke)
    m_completer->setCaseSensitivity(Qt::CaseInsensitive);
    // 1.8.10: rows are chosen here (search-engine keywords + history), so the completer shows them
    // all instead of filtering ("goggle" still lists "google search").
    m_completer->setCompletionMode(QCompleter::UnfilteredPopupCompletion);
    m_completer->setCompletionRole(RoleValue);
    m_model = new QStandardItemModel(m_completer);
    m_completer->setModel(m_model);
    m_completer->setMaxVisibleItems(12);
    m_delegate = new OmniDelegate(m_completer->popup());
    m_completer->popup()->setItemDelegate(m_delegate);
    m_completer->popup()->setMouseTracking(true);
    m_completer->popup()->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);   // at most 12 rows, all visible
    m_net = new QNetworkAccessManager(this);
    // 1.8.10: the list is attached to the field but not as its QLineEdit completer: that one also
    // inserted row text by itself ("goggle searc" became "goggle searcgoogle.com/search").
    m_completer->setWidget(m_input);
    // Arrow keys through the list put the row's text in the field (like Chrome); typing goes on from there.
    connect(m_completer, QOverload<const QString&>::of(&QCompleter::highlighted), this, [this](const QString& v){
        if (v.isEmpty()) return;
        m_navigating = true;
        m_input->setText(v); m_input->end(false);
    });

    connect(m_input, &QLineEdit::returnPressed, this, &AddressBar::onReturn);
    connect(m_input, &QLineEdit::textEdited, this, &AddressBar::onTextChanged);
    connect(m_completer, QOverload<const QString&>::of(&QCompleter::activated), this, &AddressBar::onSuggestionActivated);

    initProgress();
    m_timer.setSingleShot(true);
    m_timer.setInterval(90);   // history is local; the engine request is cancelled when you keep typing
    connect(&m_timer, &QTimer::timeout, this, &AddressBar::updateSuggestions);
}

void AddressBar::applyTheme(ThemeManager* tm) {
    m_theme = tm;
    setStyleSheet(tm->addressBarStyle());
    const auto& p = tm->palette();
    m_completer->popup()->setStyleSheet(QStringLiteral(
        "QAbstractItemView{background:%1;color:%2;border:1px solid %3;border-radius:14px;padding:0px;outline:0;}"
        "QAbstractItemView::item{padding:0px;border-radius:10px;}"
        "QAbstractItemView::item:hover{background:%4;color:%2;}"
        "QAbstractItemView::item:selected{background:%5;color:%2;}").arg(p.toolbar, p.text, p.borderStrong, p.surface, p.accentSoft));
    m_delegate->text = QColor(p.text); m_delegate->sub = QColor(p.textTertiary.isEmpty() ? p.textSecondary : p.textTertiary);
    m_delegate->hover = cssColor(p.surface); m_delegate->selected = cssColor(p.accentSoft);
    m_delegate->search = ThemeManager::icon(QStringLiteral("search"), p.textSecondary, 16);
    m_delegate->history = ThemeManager::icon(QStringLiteral("history"), p.textSecondary, 16);
    m_delegate->globe = ThemeManager::icon(QStringLiteral("globe"), p.textSecondary, 16);
    updateSecurityIcon(m_currentUrl);
    updateStar();
    if (!m_input->hasFocus()) showBlurred();
}

void AddressBar::setUrl(const QString& url) {
    m_currentUrl = url;
    // Pages like Google change their own address while you type; never overwrite typed text.
    if (!m_input->hasFocus() && !(m_editing && m_input->isModified())) showBlurred();
    updateSecurityIcon(url);
}

void AddressBar::showTabUrl(const QString& url) {
    m_currentUrl = url;
    if (m_input->hasFocus()) {
        setDisplayMode(false);
        m_input->setText(isInternalNewTab(url) ? QString() : url);
        m_input->setModified(false);
        m_input->selectAll();
    } else {
        m_input->setModified(false);
        showBlurred();
    }
    updateSecurityIcon(url);
}

void AddressBar::showBlurred() {
    const QString full = isInternalNewTab(m_currentUrl) ? QString() : m_currentUrl;
    if (full.isEmpty() || !m_theme) { setDisplayMode(false); m_input->setText(full); m_input->setCursorPosition(0); return; }
    const QUrl u(full);
    QString host, rest;
    if (u.scheme() == QLatin1String("pacitum")) {
        host = u.host(); rest = u.path();                          // pacitum://settings/appearance -> settings/appearance
        if (rest == QLatin1String("/")) rest.clear();              // pacitum://welcome/ -> welcome
    } else if (u.scheme() == QLatin1String("http") || u.scheme() == QLatin1String("https")) {
        host = u.host(QUrl::FullyDecoded);
        if (host.startsWith(QLatin1String("www."))) host = host.mid(4);
        if (u.port() > 0) host += QLatin1Char(':') + QString::number(u.port());
        rest = u.path(QUrl::FullyDecoded);
        if (rest == QLatin1String("/")) rest.clear();
    } else {
        host = full; if (host.size() > 200) host = host.left(200) + QStringLiteral("…");
    }
    const auto& p = m_theme->palette();
    m_display->setText(QStringLiteral("<span style=\"color:%1\">%2</span><span style=\"color:%3\">%4</span>")
        .arg(p.text, host.toHtmlEscaped(), p.textTertiary, rest.toHtmlEscaped()));
    m_input->setText(host + rest);    // same text, kept invisible; the label paints the two tones
    m_input->setCursorPosition(0);
    setDisplayMode(true);
}

void AddressBar::setDisplayMode(bool on) {
    m_input->setProperty("urlview", on);
    m_input->style()->unpolish(m_input); m_input->style()->polish(m_input);
    if (on) { m_display->setGeometry(2, 0, m_input->width() - 2, m_input->height()); m_display->show(); }
    else m_display->hide();
}

QString AddressBar::url() const { return m_input->text(); }

void AddressBar::focusUrl() { m_input->setFocus(); m_input->selectAll(); }

void AddressBar::setBookmarked(bool b, bool animate) { m_bookmarked = b; updateStar(); m_star->setStarred(b, animate); }

void AddressBar::updateStar() {
    if (!m_theme) return;
    const auto& p = m_theme->palette();
    m_star->setIcons(ThemeManager::icon(QStringLiteral("star"), p.textSecondary, 18),
                     ThemeManager::icon(QStringLiteral("star-filled"), p.accent, 18));
}

bool AddressBar::eventFilter(QObject* o, QEvent* e) {
    if (o == m_input) {
        if (e->type() == QEvent::Resize && !m_display->isHidden()) {
            m_display->setGeometry(2, 0, m_input->width() - 2, m_input->height());
        } else if (e->type() == QEvent::FocusIn) {
            // 1.8.10: the suggestion list opening/closing (Popup) or switching back to the window
            // (ActiveWindow) also sends FocusIn. That used to put the page URL back and select it,
            // so the next key wiped what you were typing.
            // Only while already editing (the short display text is not up): a window that opens
            // with the address bar focused still gets the full URL, selected.
            const auto reason = static_cast<QFocusEvent*>(e)->reason();
            if ((reason == Qt::PopupFocusReason || reason == Qt::ActiveWindowFocusReason) && m_editing) return QFrame::eventFilter(o, e);
            m_editing = true;
            m_noInline = false; m_composing = false;
            setDisplayMode(false);
            if (!isInternalNewTab(m_currentUrl)) m_input->setText(m_currentUrl);   // full URL on focus
            setProperty("focused", true); style()->unpolish(this); style()->polish(this);
            animateGlow(1.0);
            m_selectOnClick = true;
            QTimer::singleShot(0, m_input, [this]{ m_input->selectAll(); });
        } else if (e->type() == QEvent::FocusOut) {
            const auto reason = static_cast<QFocusEvent*>(e)->reason();
            if (reason == Qt::PopupFocusReason || reason == Qt::ActiveWindowFocusReason) return QFrame::eventFilter(o, e);   // keep the typed text
            m_editing = false;
            if (m_suggestReply) m_suggestReply->abort();
            setProperty("focused", false); style()->unpolish(this); style()->polish(this);
            animateGlow(0.0);
            if (!m_completer->popup()->isVisible()) showBlurred();
        } else if (e->type() == QEvent::MouseButtonRelease && m_selectOnClick) {
            m_selectOnClick = false; m_input->selectAll(); return true;
        } else if (e->type() == QEvent::InputMethod) {
            m_composing = !static_cast<QInputMethodEvent*>(e)->preeditString().isEmpty();
        } else if (e->type() == QEvent::KeyPress) {
            // The inline hint is a selected suffix. Backspace/Delete removes only the hint
            // (Chrome) and the hint must not come back until the user types again.
            auto* ke = static_cast<QKeyEvent*>(e);
            const bool plain = !(ke->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier));
            if ((ke->key() == Qt::Key_Backspace || ke->key() == Qt::Key_Delete) && plain) {
                m_noInline = true;
                if (inlineHintActive()) { dropInlineHint(); onTextChanged(QString()); return true; }
            } else if (ke->key() == Qt::Key_Escape && !m_completer->popup()->isVisible() && inlineHintActive()) {
                m_noInline = true; dropInlineHint(); return true;
            } else if (ke->matches(QKeySequence::Cut)) {
                m_noInline = true;
            } else if (plain && !ke->text().isEmpty() && ke->text().at(0).isPrint()) {
                m_noInline = false;
            } else if (ke->matches(QKeySequence::Paste)) {
                m_noInline = false;
            }
        }
    } else if (o == m_chip && m_secureHttps) {
        // hover swaps the lock for the site-controls glyph (click opens site controls / info)
        if (e->type() == QEvent::Enter) m_chip->setIcon(ThemeManager::icon(QStringLiteral("site-controls"), m_theme->palette().text, 16));
        else if (e->type() == QEvent::Leave) updateSecurityIcon(m_currentUrl);
    }
    return QFrame::eventFilter(o, e);
}

void AddressBar::onReturn() {
    m_timer.stop();
    if (m_suggestReply) m_suggestReply->abort();
    m_completer->popup()->hide();   // a pending refresh must not rewrite the field / reopen the popup after Enter
    emit returnPressed(m_input->text().trimmed());
    m_input->clearFocus();
}

void AddressBar::onTextChanged(const QString&) { m_navigating = false; m_timer.start(); }

bool AddressBar::inlineHintActive() const {
    if (!m_input->hasSelectedText()) return false;
    const int start = m_input->selectionStart();
    return start > 0 && m_input->selectionEnd() == m_input->text().size()
        && m_input->cursorPosition() == m_input->text().size();
}

void AddressBar::dropInlineHint() {
    if (!inlineHintActive()) return;
    const int keep = m_input->selectionStart();
    m_input->setText(m_input->text().left(keep));
    m_input->setCursorPosition(keep);
}

void AddressBar::updateSuggestions() {
    // Late timer (after Enter / focus lost) or IME preedit: leave the field alone.
    if (m_composing || !m_input->hasFocus()) return;

    const QString raw = m_input->text();
    const bool hint = inlineHintActive();
    // `typed` is exactly what the user typed (a selected inline hint is not part of it, and
    // spaces are kept: the old code trimmed and re-set the text, which ate a typed space).
    const QString typed = hint ? raw.left(m_input->selectionStart()) : raw;
    const QString query = typed.trimmed();
    if (query.isEmpty() || !m_suggest) {
        if (m_suggestReply) m_suggestReply->abort();
        m_query.clear(); m_hintUrl.clear(); m_engineRows.clear(); m_historyRows.clear();
        m_model->clear();
        m_completer->popup()->hide();
        if (hint) dropInlineHint();
        return;
    }

    // History: scan more rows for the hint than the list shows, so a prefix match is not
    // crowded out by newer substring matches.
    QVector<HistoryEntry> results;
    if (m_history) results = m_history->search(query, 40);
    QSet<QString> seen;
    QString bestHint;
    // Hint only for addresses being typed: cursor at the end, no spaces.
    const bool canHint = !m_noInline && typed == query && !query.contains(QLatin1Char(' '))
                         && (hint || m_input->cursorPosition() == raw.size());
    m_historyRows.clear();
    for (const auto& r : results) {
        if (seen.contains(r.url) || r.url == m_currentUrl || isInternalNewTab(r.url)) continue;
        seen.insert(r.url);
        if (m_historyRows.size() < 4) m_historyRows.append({r.url, r.title});
        if (!canHint) continue;
        for (const QString& form : inlineForms(r.url, query)) {
            if (form.size() > query.size() && form.startsWith(query, Qt::CaseInsensitive)) {
                QString rest = form.mid(query.size());
                // Complete the site only ("yout" -> "youtube.com/"), not a long deep link.
                if (const int slash = rest.indexOf(QLatin1Char('/')); slash >= 0 && !query.contains(QLatin1Char('/'))) rest = rest.left(slash + 1);
                // Shortest completion wins (domain before deep link); ties keep the most recent.
                if (bestHint.isEmpty() || rest.size() < bestHint.size()) bestHint = rest;
                break;
            }
        }
    }

    if (query != m_query) {
        // Keep the previous keywords while the new ones load when they still fit (no flicker).
        QStringList keep;
        for (const QString& k : m_engineRows) if (k.startsWith(query, Qt::CaseInsensitive)) keep << k;
        m_engineRows = keep;
        m_query = query;
        fetchSearchSuggestions(query);
    }
    m_hintUrl = bestHint.isEmpty() ? QString() : typed + bestHint;
    rebuildPopup();

    if (!bestHint.isEmpty()) {
        const QString filled = typed + bestHint;   // user's own casing is kept
        if (!(hint && raw == filled)) m_input->setText(filled);
        m_input->setSelection(typed.size(), bestHint.size());
    } else if (hint) {
        dropInlineHint();   // the old hint no longer matches
    }
}

void AddressBar::rebuildPopup() {
    const QString q = m_query;
    m_model->clear();
    auto add = [this, &q](int kind, const QString& text, const QString& value, const QString& sub){
        auto* it = new QStandardItem(text);
        it->setData(value, RoleValue); it->setData(kind, RoleKind); it->setData(sub, RoleSub); it->setData(q, RoleQuery);
        it->setToolTip(sub.isEmpty() ? value : sub);
        m_model->appendRow(it);
    };
    if (q.isEmpty()) { m_completer->popup()->hide(); return; }
    QSet<QString> used{q.toLower()};
    if (!m_hintUrl.isEmpty()) { add(KindUrl, m_hintUrl, m_hintUrl, {}); used.insert(m_hintUrl.toLower()); }   // what Enter opens
    if (looksLikeUrl(q)) { if (m_hintUrl.isEmpty()) add(KindUrl, q, q, {}); }
    else add(KindSearch, q, q, {});                // what you typed, as a search (like Google)
    int n = 0;
    for (const QString& k : m_engineRows) {
        if (n >= 7) break;
        if (used.contains(k.toLower())) continue;
        used.insert(k.toLower()); add(KindSearch, k, k, {}); ++n;
    }
    for (const auto& h : m_historyRows) {
        QString shown = h.first;
        if (const int p = shown.indexOf(QStringLiteral("://")); p > 0 && p < 12) shown = shown.mid(p + 3);
        if (shown.startsWith(QStringLiteral("www."))) shown = shown.mid(4);
        add(KindHistory, h.second.trimmed().isEmpty() ? shown : h.second.trimmed(), h.first, h.second.trimmed().isEmpty() ? QString() : shown);
    }
    m_completer->setCompletionPrefix(QString());
    m_completer->complete();
}

void AddressBar::fetchSearchSuggestions(const QString& query) {
    if (m_suggestReply) m_suggestReply->abort();
    // Only words go to the search engine: not addresses, not long pastes.
    if (!m_online || query.size() > 200 || query.contains(QStringLiteral("://")) || query.startsWith(QStringLiteral("pacitum:"))
        || query.startsWith(QStringLiteral("file:")) || query.startsWith(QStringLiteral("about:"))) return;
    QNetworkRequest r(suggestUrl(m_engine, query));
    r.setRawHeader("User-Agent", QByteArray("Mozilla/5.0 PacitumBrowser/") + QCoreApplication::applicationVersion().toLatin1());
    r.setTransferTimeout(3000);
    auto* reply = m_net->get(r);
    m_suggestReply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply, query]{
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError || query != m_query || !m_input->hasFocus() || m_composing) return;
        const QJsonArray a = QJsonDocument::fromJson(reply->readAll()).array();
        QStringList rows;
        for (const auto& v : a.at(1).toArray()) { const QString t = v.toString().trimmed(); if (!t.isEmpty()) rows << t; }
        m_engineRows = rows;
        // Don't reshuffle the list under someone moving through it with the arrow keys.
        if (m_navigating && m_completer->popup()->isVisible()) return;
        rebuildPopup();
    });
}

void AddressBar::onSuggestionActivated(const QString& v) { m_input->setText(v); emit returnPressed(v); }

void AddressBar::updateSecurityIcon(const QString& url) {
    if (!m_theme) return;
    const auto& p = m_theme->palette();
    m_secureHttps = false;
    QString text;
    QIcon ic;
    int isz = 16;
    if (url.startsWith(QStringLiteral("https://"))) {
        ic = ThemeManager::icon(QStringLiteral("lock"), p.secure, 14); isz = 14; m_secureHttps = true;
        m_chip->setToolTip(QStringLiteral("Connection is secure · Site controls"));
    } else if (url.startsWith(QStringLiteral("http://"))) {
        ic = ThemeManager::icon(QStringLiteral("not-secure"), p.danger, 14); isz = 14; text = QStringLiteral("Not secure");
        m_chip->setToolTip(QStringLiteral("Your connection to this site is not secure"));
    } else if (url.startsWith(QStringLiteral("pacitum:")) && !isInternalNewTab(url)) {
        ic = ThemeManager::icon(QStringLiteral("site-info"), p.accent, 16); text = QStringLiteral("Pacitum");
        m_chip->setToolTip(QStringLiteral("Pacitum page"));
    } else {
        ic = ThemeManager::icon(QStringLiteral("search"), p.textTertiary, 16);
        m_chip->setToolTip(QString());
    }
    m_chip->setIconSize(QSize(isz, isz));
    m_chip->setIcon(ic);
    m_chip->setText(text);
    m_chip->setToolButtonStyle(text.isEmpty() ? Qt::ToolButtonIconOnly : Qt::ToolButtonTextBesideIcon);
    const QVariant oldKind = m_chip->property("kind");
    m_chip->setProperty("kind", text.isEmpty() ? "icon" : (m_secureHttps ? "secure" : url.startsWith(QStringLiteral("http://")) ? "danger" : "internal"));
    m_chip->style()->unpolish(m_chip); m_chip->style()->polish(m_chip);
    if (oldKind.isValid() && oldKind != m_chip->property("kind") && m_chip->isVisible()) Motion::fadeIn(m_chip, Motion::Base);   // security badge change
    if (text.isEmpty()) m_chip->setFixedWidth(28); else { m_chip->setMinimumWidth(28); m_chip->setMaximumWidth(200); }
}

// ---------- address-bar progress (animations-spec §7) ----------
static qreal trickleAt(qreal t) {   // keyframes 0→30 % (300 ms OutCubic) →70 % (1500 OutQuad) →82 % (1900 OutQuad)
    QEasingCurve oc(QEasingCurve::OutCubic), oq(QEasingCurve::OutQuad);
    if (t < 300) return 0.30 * oc.valueForProgress(t / 300.0);
    if (t < 1500) return 0.30 + 0.40 * oq.valueForProgress((t - 300) / 1200.0);
    if (t < 1900) return 0.70 + 0.12 * oq.valueForProgress((t - 1500) / 400.0);
    return qMin(0.90, 0.82 + (t - 1900) / 60000.0);   // slow trickle, max 90 % until real progress
}
void AddressBar::initProgress() {
    connect(&m_glowAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant& v){ m_glow = v.toReal(); update(); });
    m_trickle.setDuration(60000); m_trickle.setStartValue(0.0); m_trickle.setEndValue(60000.0);
    connect(&m_trickle, &QVariantAnimation::valueChanged, this, [this](const QVariant& v){
        const qreal tv = trickleAt(v.toReal());
        if (tv > m_prog) { m_prog = tv; repaintProgress(); }
    });
    m_progAnim.setEasingCurve(QEasingCurve::OutCubic);
    connect(&m_progAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant& v){ m_prog = qMax(m_prog, v.toReal()); repaintProgress(); });
    m_fadeAnim.setEasingCurve(QEasingCurve::OutCubic);
    connect(&m_fadeAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant& v){ m_progOpacity = v.toReal(); repaintProgress(); });
    connect(&m_fadeAnim, &QVariantAnimation::finished, this, [this]{ m_prog = 0; m_progOpacity = 0; m_progError = false; repaintProgress(); });
    m_fadeDelay.setSingleShot(true); m_fadeDelay.setInterval(100);
    connect(&m_fadeDelay, &QTimer::timeout, this, [this]{ m_fadeAnim.stop(); m_fadeAnim.setDuration(250); m_fadeAnim.setStartValue(m_progOpacity); m_fadeAnim.setEndValue(0.0); m_fadeAnim.start(); });
}
void AddressBar::animateGlow(qreal to) {
    m_glowAnim.stop();
    const int ms = Motion::fadeMs(Motion::Base);   // kit #13: ring alpha 160 ms (fade kept when reduced)
    if (ms <= 0) { m_glow = to; update(); return; }
    m_glowAnim.setDuration(ms); m_glowAnim.setEasingCurve(QEasingCurve::OutCubic);
    m_glowAnim.setStartValue(m_glow); m_glowAnim.setEndValue(to); m_glowAnim.start();
}
void AddressBar::repaintProgress() { update(QRect(0, height() - 4, width(), 4)); }
void AddressBar::progressStarted() {
    m_fadeDelay.stop(); m_fadeAnim.stop(); m_progAnim.stop();
    m_prog = 0; m_progOpacity = 1.0; m_progError = false; m_progLoading = true;   // new navigation: snap opacity 1, width 0
    if (!Pacitum::reducedMotion()) m_trickle.start(); else m_trickle.stop();
    repaintProgress();
}
void AddressBar::progressChanged(int value) {
    if (!m_progLoading) return;
    const qreal target = qBound(0, value, 100) / 100.0;
    if (target <= m_prog) return;   // never backwards
    if (Pacitum::reducedMotion()) { m_prog = target; repaintProgress(); return; }
    m_progAnim.stop(); m_progAnim.setDuration(200); m_progAnim.setStartValue(m_prog); m_progAnim.setEndValue(target); m_progAnim.start();
}
void AddressBar::progressFinished(bool ok) {
    if (!m_progLoading) return;
    m_progLoading = false; m_trickle.stop();
    if (Pacitum::reducedMotion()) { m_progAnim.stop(); m_prog = 0; m_progOpacity = 0; m_progError = false; repaintProgress(); return; }
    m_progError = !ok;
    if (ok) { m_progAnim.stop(); m_progAnim.setDuration(200); m_progAnim.setStartValue(m_prog); m_progAnim.setEndValue(1.0); m_progAnim.start(); m_fadeDelay.start(); }
    else { m_fadeDelay.stop(); m_fadeAnim.stop(); m_fadeAnim.setDuration(250); m_fadeAnim.setStartValue(m_progOpacity); m_fadeAnim.setEndValue(0.0); m_fadeAnim.start(); }   // error: danger bar fades
}
void AddressBar::syncProgress(bool loading, int value) {
    m_trickle.stop(); m_progAnim.stop(); m_fadeAnim.stop(); m_fadeDelay.stop();
    m_progLoading = loading; m_progError = false;
    m_prog = loading ? qBound(0, value, 100) / 100.0 : 0.0; m_progOpacity = loading ? 1.0 : 0.0;
    repaintProgress();
}
void AddressBar::paintEvent(QPaintEvent* e) {
    QFrame::paintEvent(e);
    if (m_glow > 0.0) {   // focus glow: a soft accent halo just inside the 2 px focus border
        QPainter g(this); g.setRenderHint(QPainter::Antialiasing);
        QColor c(ThemeManager::current().accent);
        for (int i = 0; i < 3; ++i) {
            c.setAlphaF(0.16 * m_glow * (3 - i) / 3.0);
            g.setPen(QPen(c, 1.0)); g.setBrush(Qt::NoBrush);
            const qreal in = 2.5 + i;
            const QRectF r = QRectF(rect()).adjusted(in, in, -in, -in);
            g.drawRoundedRect(r, qMax<qreal>(0, kRadius - in), qMax<qreal>(0, kRadius - in));
        }
    }
    if (m_progOpacity <= 0.0 || m_prog <= 0.0) return;
    QPainter p(this); p.setRenderHint(QPainter::Antialiasing);
    QPainterPath clip; clip.addRoundedRect(QRectF(rect()), kRadius, kRadius);
    p.setClipPath(clip);
    QColor c(m_progError ? ThemeManager::current().danger : ThemeManager::current().accent);
    c.setAlphaF(c.alphaF() * m_progOpacity);
    p.fillRect(QRectF(0, height() - 2, width() * m_prog, 2), c);
}


// ---------------------------------------------------------------- permission chips
namespace {
class PermChip : public QToolButton {
public:
    explicit PermChip(QWidget* parent) : QToolButton(parent) {
        setFixedHeight(28); setCursor(Qt::PointingHandCursor); setFocusPolicy(Qt::TabFocus);
        connect(&m_anim, &QVariantAnimation::valueChanged, this, [this](const QVariant& v){ m_t = v.toReal(); updateGeometry(); update(); });
        connect(&m_pulse, &QVariantAnimation::valueChanged, this, [this](const QVariant& v){ m_p = v.toReal(); update(); });
        connect(&m_collapse, &QVariantAnimation::valueChanged, this, [this](const QVariant& v){ m_labelT = v.toReal(); updateGeometry(); update(); });
    }
    void setChip(const OmniChip& c, bool fresh) {
        const bool attention = fresh || (c.id == m_chip.id && c.badge != m_chip.badge);
        m_chip = c;
        setAccessibleName(accessible());
        setToolTip(accessible());
        if (fresh && Motion::enabled()) {   // §1 chip appear: width 28→auto, icon .8→1, 180 ms OutBack
            m_anim.stop(); m_anim.setDuration(Motion::Exit); m_anim.setEasingCurve(QEasingCurve(QEasingCurve::OutBack));
            m_anim.setStartValue(0.0); m_anim.setEndValue(1.0); m_anim.start();
        } else m_t = 1.0;
        if (c.id == QLatin1String("request") && attention && Motion::enabled()) {   // attention pulse (≤2×)
            m_pulse.stop(); m_pulse.setDuration(600); m_pulse.setEasingCurve(QEasingCurve::OutCubic);
            m_pulse.setStartValue(0.0); m_pulse.setEndValue(1.0); m_pulse.setLoopCount(2); m_pulse.start();
        }
        if (c.id == QLatin1String("quiet") && fresh) {   // §4: label collapses after 4 s (200 ms OutCubic)
            m_labelT = 1.0;
            QTimer::singleShot(4000, this, [this]{
                if (!Motion::enabled()) { m_labelT = 0; updateGeometry(); return; }
                m_collapse.stop(); m_collapse.setDuration(Motion::Medium); m_collapse.setEasingCurve(QEasingCurve::OutCubic);
                m_collapse.setStartValue(1.0); m_collapse.setEndValue(0.0); m_collapse.start();
            });
        } else if (c.id != QLatin1String("quiet")) m_labelT = 0;
        updateGeometry(); update();
    }
    QString accessible() const {
        if (m_chip.id == QLatin1String("request")) return QStringLiteral("Permission request pending%1 (F6)").arg(m_chip.badge ? QStringLiteral(", +%1 more").arg(m_chip.badge) : QString());
        if (m_chip.id == QLatin1String("quiet")) return QStringLiteral("Notifications blocked");
        if (m_chip.id == QLatin1String("inuse")) return m_chip.dot ? QStringLiteral("Camera and microphone in use") : QStringLiteral("In use: ") + m_chip.glyphs.join(QStringLiteral(", "));
        if (m_chip.id == QLatin1String("popups")) return QStringLiteral("Pop-ups blocked");
        return QStringLiteral("Blocked on this page");
    }
    QSize sizeHint() const override {
        const int glyphs = qMax(1, int(m_chip.glyphs.size()));
        int w = 10 + glyphs * 15 + (glyphs - 1) * 10 + 10;
        if (m_chip.badge > 0) w += 20;
        if (m_labelT > 0 && !m_chip.text.isEmpty()) { QFont f = font(); f.setPixelSize(12); w += int((QFontMetrics(f).horizontalAdvance(m_chip.text) + 6) * m_labelT); }
        return QSize(28 + int((w - 28) * qBound(0.0, m_t, 1.2)), 28);
    }
protected:
    void paintEvent(QPaintEvent*) override {
        const auto& pal = ThemeManager::current();
        QPainter p(this); p.setRenderHint(QPainter::Antialiasing);
        const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        if (m_pulse.state() == QAbstractAnimation::Running) {
            QColor ring(pal.accent); ring.setAlphaF(0.6 * (1.0 - m_p));
            p.setPen(QPen(ring, 2)); p.setBrush(Qt::NoBrush);
            const qreal g = 6 * m_p; p.drawRoundedRect(r.adjusted(-g / 2, -g / 2, g / 2, g / 2), 14 + g / 2, 14 + g / 2);
        }
        p.setPen(Qt::NoPen);
        p.setBrush(m_chip.soft ? cssColor(pal.accentSoft) : QColor(underMouse() ? pal.surfaceHover : pal.surface));
        if (false) p.setBrush(QColor(m_chip.soft ? pal.accentSoft : underMouse() ? pal.surfaceHover : pal.surface));
        p.drawRoundedRect(r, 14, 14);
        const qreal s = 0.8 + 0.2 * qMin(1.0, m_t);
        qreal x = 10;
        const QStringList gl = m_chip.glyphs.isEmpty() ? QStringList{m_chip.glyph} : m_chip.glyphs;
        for (const QString& g : gl) {
            const QPixmap pm = ThemeManager::icon(g, m_chip.color, 15).pixmap(QSize(15, 15), devicePixelRatioF());
            p.save(); p.translate(x + 7.5, 14); p.scale(s, s); p.drawPixmap(QPointF(-7.5, -7.5), pm); p.restore();
            if (m_chip.dot && (g == QLatin1String("camera") || g == QLatin1String("microphone"))) {   // 6 px danger dot, 1.5 px omnibox ring
                p.setBrush(QColor(pal.danger)); p.setPen(QPen(QColor(pal.omnibox), 1.5)); p.drawEllipse(QPointF(x + 14, 8), 3.5, 3.5); p.setPen(Qt::NoPen);
            }
            x += 25;
        }
        x -= 10;
        QFont f = font(); f.setPixelSize(12); f.setWeight(QFont::Medium); p.setFont(f);
        if (m_labelT > 0.01 && !m_chip.text.isEmpty()) {
            p.setOpacity(m_labelT); p.setPen(QColor(pal.textSecondary));
            p.drawText(QRectF(x + 6, 0, width() - x - 6, height()), Qt::AlignVCenter | Qt::AlignLeft, m_chip.text); p.setOpacity(1);
        }
        if (m_chip.badge > 0) { p.setPen(QColor(pal.accent)); p.drawText(QRectF(width() - 26, 0, 22, height()), Qt::AlignCenter, QStringLiteral("+%1").arg(m_chip.badge)); }
    }
private:
    OmniChip m_chip; qreal m_t = 1.0, m_p = 0, m_labelT = 0;
    QVariantAnimation m_anim, m_pulse, m_collapse;
};
}

void AddressBar::setPermissionChips(const QList<OmniChip>& chips) {
    QSet<QString> keep;
    int li = 0, ri = 0;
    for (const auto& c : chips) {
        keep.insert(c.id);
        auto* w = static_cast<PermChip*>(m_permChips.value(c.id));
        const bool fresh = !w;
        if (!w) {
            w = new PermChip(this);
            const QString id = c.id;
            connect(w, &QToolButton::clicked, this, [this, id]{ emit permissionChipClicked(id); });
            m_permChips.insert(c.id, w);
        }
        // in-use / request / blocked chips follow the lock; the quiet chip sits before the star (§4)
        if (c.id == QLatin1String("quiet")) m_permRight->insertWidget(ri++, w); else m_permLeft->insertWidget(li++, w);
        w->setChip(c, fresh);
        w->show();
    }
    for (auto it = m_permChips.begin(); it != m_permChips.end();) {
        if (!keep.contains(it.key())) { it.value()->hide(); it.value()->deleteLater(); it = m_permChips.erase(it); } else ++it;
    }
}

QPoint AddressBar::bubbleAnchor() const {
    const QWidget* tb = parentWidget() ? parentWidget() : this;
    const int bottom = tb->mapToGlobal(QPoint(0, tb->height())).y();
    return QPoint(mapToGlobal(QPoint(0, 0)).x() - 2, bottom + 6);
}
