#pragma once
#include <QWidget>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QKeyEvent>

class QWebEngineView;

class FindBar : public QWidget {
    Q_OBJECT
public:
    explicit FindBar(QWidget* parent = nullptr);
    void setBrowserView(QWebEngineView* view) { m_view = view; }
    void showFind();
    void hideFind();

signals:
    void closed();

private slots:
    void onTextChanged(const QString& text);
    void findNext();
    void findPrev();
    void closeBar();

protected:
    void keyPressEvent(QKeyEvent* e) override;
private:
    void doFind(bool forward);
    void refreshIcons();

    QWebEngineView* m_view = nullptr;
    QLineEdit* m_input = nullptr;
    QLabel* m_result = nullptr;
    QPushButton* m_prev = nullptr;
    QPushButton* m_next = nullptr;
    QPushButton* m_close = nullptr;
};
