#include "FindBar.h"
#include "Motion.h"
#include <QWebEngineView>
#include <QWebEnginePage>
#include <QWebEngineFindTextResult>
#include <QHBoxLayout>
#include "ThemeManager.h"

FindBar::FindBar(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(0); setMaximumHeight(48); setProperty("_pmMax", 48); hide();   // height animated by Motion::slideVisible
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(12,6,12,6); layout->setSpacing(8);

    m_result = new QLabel(this); m_result->setMinimumWidth(80);
    m_input = new QLineEdit(this); m_input->setPlaceholderText(QStringLiteral("Find in page..."));
    m_input->setMinimumWidth(200); m_input->setMaximumWidth(400);
    m_prev = new QPushButton(this); m_prev->setFixedSize(32,32);
    m_next = new QPushButton(this); m_next->setFixedSize(32,32);
    m_close = new QPushButton(this); m_close->setFixedSize(32,32);
    // 2.0.8: the buttons drew nothing in some themes (a bare "<", ">", "x" on a dark button): icons and tips now.
    m_prev->setToolTip(QStringLiteral("Previous match (Shift+Enter)")); m_next->setToolTip(QStringLiteral("Next match (Enter)")); m_close->setToolTip(QStringLiteral("Close (Esc)"));
    m_prev->setAccessibleName(QStringLiteral("Previous match")); m_next->setAccessibleName(QStringLiteral("Next match")); m_close->setAccessibleName(QStringLiteral("Close find bar"));
    refreshIcons();

    layout->addWidget(m_result); layout->addWidget(m_input);
    layout->addWidget(m_prev); layout->addWidget(m_next);
    layout->addStretch(); layout->addWidget(m_close);

    connect(m_input, &QLineEdit::textChanged, this, &FindBar::onTextChanged);
    connect(m_input, &QLineEdit::returnPressed, this, &FindBar::findNext);
    connect(m_prev, &QPushButton::clicked, this, &FindBar::findPrev);
    connect(m_next, &QPushButton::clicked, this, &FindBar::findNext);
    connect(m_close, &QPushButton::clicked, this, &FindBar::closeBar);
}

void FindBar::showFind() {
    m_input->clear();
    m_result->clear();
    refreshIcons();
    Motion::slideVisible(this, true, 48, Motion::Base);   // slides down 160 ms OutCubic
    m_input->setFocus();
}
void FindBar::hideFind() { Motion::slideVisible(this, false, 48, Motion::Fast); }

void FindBar::onTextChanged(const QString& t) {
    if (!m_view || t.isEmpty()) { m_result->setText({}); return; }
    doFind(true);
}
void FindBar::findNext() { doFind(true); }
void FindBar::findPrev() { doFind(false); }

void FindBar::doFind(bool forward) {
    if (!m_view) return;
    QString text = m_input->text();
    if (text.isEmpty()) { m_result->setText({}); return; }
    auto flags = QWebEnginePage::FindFlags();
    if (!forward) flags |= QWebEnginePage::FindBackward;
    m_view->findText(text, flags, [this](const QWebEngineFindTextResult &result){
        const int n = result.numberOfMatches();
        m_result->setText(n > 0 ? QStringLiteral("%1 of %2").arg(result.activeMatch()).arg(n) : QStringLiteral("Not found"));   // 2.0.8: the count
    });
}

void FindBar::refreshIcons() {
    const QString c = ThemeManager::current().text;
    m_prev->setIcon(ThemeManager::icon(QStringLiteral("chevron-up"), c, 16));
    m_next->setIcon(ThemeManager::icon(QStringLiteral("chevron-down"), c, 16));
    m_close->setIcon(ThemeManager::icon(QStringLiteral("close"), c, 16));
    for (auto* b : {m_prev, m_next, m_close}) { b->setIconSize(QSize(16, 16)); if (b->icon().isNull()) b->setText(b == m_prev ? QStringLiteral("\u2191") : b == m_next ? QStringLiteral("\u2193") : QStringLiteral("\u00D7")); }
}

void FindBar::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_Escape) { closeBar(); return; }
    if ((e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) && (e->modifiers() & Qt::ShiftModifier)) { findPrev(); return; }
    QWidget::keyPressEvent(e);
}

void FindBar::closeBar() {
    if (m_view) m_view->findText({});
    hideFind(); emit closed();
}
