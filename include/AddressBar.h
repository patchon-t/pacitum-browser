#pragma once
#include <QFrame>
#include <QLineEdit>
#include <QLabel>
#include <QToolButton>
#include <QCompleter>
#include <QTimer>
#include <QVariantAnimation>
#include <QPointer>
class StarButton;
class QNetworkAccessManager;
class QNetworkReply;
class QStandardItemModel;
class OmniDelegate;
#include "PermissionUi.h"
class QHBoxLayout;

class HistoryManager;
class ThemeManager;

// Pill-shaped omnibox (design-spec §4 "Omnibox"): leading security/search icon,
// URL field and trailing bookmark star.
class AddressBar : public QFrame {
    Q_OBJECT
public:
    explicit AddressBar(HistoryManager* hm = nullptr, QWidget* parent = nullptr);
    void applyTheme(ThemeManager* tm);
    void setUrl(const QString& url);
    // Tab switch: the text belongs to the old tab, so replace it even while the field has focus.
    void showTabUrl(const QString& url);
    QString url() const;
    QString currentUrl() const { return m_currentUrl; }   // the URL last set (not the display text)
    void focusUrl();
    void setBookmarked(bool b, bool animate = false);
    // Omnibox progress bar (animations-spec §7)
    void progressStarted();
    void progressChanged(int value);
    void progressFinished(bool ok);
    void syncProgress(bool loading, int value);   // tab switch: jump to that tab's state
    void setSuggestionsEnabled(bool e) { m_suggest = e; }
    void setInlineHints(bool on) { m_inlineHints = on; }   // 2.0.1 Settings › Typing hints
    // 1.8.10: keyword suggestions from the search engine ("Google", "Bing", "DuckDuckGo", "Yahoo", "Brave")
    void setSearchEngine(const QString& name) { m_engine = name; }
    void setOnlineSuggestions(bool on) { m_online = on; }
    // 1.7.5 permission chips (permissions-spec §1/§4/§5/§6/§9)
    void setPermissionChips(const QList<OmniChip>& chips);
    QPoint bubbleAnchor() const;   // global: omnibox left − 2, toolbar bottom + 6

signals:
    void returnPressed(const QString& url);
    void starClicked();
    void securityIconClicked();
    void permissionChipClicked(const QString& id);

protected:
    void paintEvent(QPaintEvent* e) override;
    bool eventFilter(QObject* o, QEvent* e) override;

private slots:
    void onReturn();
    void onTextChanged(const QString& text);
    void updateSuggestions();
    void onSuggestionActivated(const QString& value);
    void fetchSearchSuggestions(const QString& query);

private:
    void updateSecurityIcon(const QString& url);
    void updateStar();
    void showBlurred();
    void setDisplayMode(bool on);
    QToolButton* m_chip = nullptr;
    QHBoxLayout* m_permLeft = nullptr; QHBoxLayout* m_permRight = nullptr;
    QHash<QString, QWidget*> m_permChips;
    QLabel* m_display = nullptr;
    bool m_secureHttps = false;

    HistoryManager* m_history = nullptr;
    ThemeManager* m_theme = nullptr;
    QLabel* m_securityIcon = nullptr;
    QLineEdit* m_input = nullptr;
    StarButton* m_star = nullptr;
    void initProgress();
    void repaintProgress();
    QVariantAnimation m_trickle, m_progAnim, m_fadeAnim, m_glowAnim;
    qreal m_glow = 0;
    void animateGlow(qreal to);
    QTimer m_fadeDelay;
    qreal m_prog = 0, m_progOpacity = 0;
    bool m_progLoading = false, m_progError = false;
    QCompleter* m_completer = nullptr;
    QTimer m_timer;
    QString m_currentUrl;
    bool m_bookmarked = false;
    bool m_suggest = true;
    bool m_selectOnClick = false;
    bool inlineHintActive() const;
    void dropInlineHint();                 // remove the selected suffix, keep what was typed
    void rebuildPopup();                   // history rows + engine rows -> model, show/hide popup
    QStandardItemModel* m_model = nullptr; // reused for every refresh
    OmniDelegate* m_delegate = nullptr;
    QString m_query;                       // what the popup was built for
    QString m_hintUrl;                     // typed text + inline hint (first row, what Enter opens)
    QStringList m_engineRows;              // keyword suggestions for m_query
    QVector<QPair<QString, QString>> m_historyRows;   // (url, title)
    QNetworkAccessManager* m_net = nullptr;
    QPointer<QNetworkReply> m_suggestReply;
    QString m_engine = QStringLiteral("Google");
    bool m_online = true;
    bool m_navigating = false;             // moving through the list with the arrow keys
    bool m_editing = false;                // focused for typing (survives the list / window switches)
    bool m_noInline = false;
    bool m_inlineHints = true;               // true after Backspace/Delete: don't re-add a hint
    bool m_composing = false;              // IME preedit active: never rewrite the text
};
