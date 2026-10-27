#pragma once
#include <QImage>
#include <QWidget>
#include <QDialog>
#include <QFrame>
#include "ProfileManager.h"

class QLineEdit;
class QLabel;
class QCheckBox;
class QHBoxLayout;
class QVBoxLayout;
class QAbstractButton;
class QKeyEvent;
class QMouseEvent;
class QPaintEvent;

// 2.0.1 profile picker (design: "Profile picker"): ink panel with the wordmark on the left,
// a list of profiles on the right, "Show at startup" and "Open as <name>" at the bottom.
// Used at startup and for "Manage profiles".
class ProfilePicker : public QWidget {
    Q_OBJECT
public:
    enum Mode { Startup, Manage };
    explicit ProfilePicker(Mode mode, QWidget* parent = nullptr);
    // Startup: blocks until a profile is chosen. Returns the id, "guest", or empty if closed.
    static QString pickAtStartup();
signals:
    void chosen(const QString& id);   // id or "guest"
protected:
    void closeEvent(QCloseEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void paintEvent(QPaintEvent* e) override;
private:
    void rebuild();
    void select(const QString& id);
    void openSelected();
    Mode m_mode;
    QVBoxLayout* m_list = nullptr;
    QAbstractButton* m_open = nullptr;
    QAbstractButton* m_manage = nullptr;
    QString m_selected;
    bool m_editAll = false;
    QPoint m_dragFrom;
};

// §2.3 add / edit profile (live preview on the left).
class ProfileEditDialog : public QDialog {
    Q_OBJECT
public:
    explicit ProfileEditDialog(const ProfileInfo& p, bool create, QWidget* parent = nullptr);
    ProfileInfo result() const { return m_p; }
    static bool editProfile(const QString& id, QWidget* parent);         // true if saved/deleted
    static QString addProfile(QWidget* parent);                         // new id or empty
private:
    void refreshPreview();
    void accept() override;
    ProfileInfo m_p; bool m_create;
    QLineEdit* m_name = nullptr; QLabel* m_error = nullptr;
    QLabel* m_avatar = nullptr; QLabel* m_previewName = nullptr; QWidget* m_mini = nullptr;
    QList<QAbstractButton*> m_colourBtns, m_glyphBtns;
    QAbstractButton* m_photoBtn = nullptr;
    QImage m_photo;                       // chosen in this dialog, saved on accept
public:
    QImage photo() const { return m_photo; }
    ~ProfileEditDialog() override;
private:
};

// §2.2 toolbar profile switcher popup.
class ProfileSwitcher : public QWidget {
    Q_OBJECT
public:
    explicit ProfileSwitcher(QWidget* parent = nullptr);
    void showUnder(QWidget* anchor);
signals:
    void settingsRequested();
    void manageRequested();
private:
    void build();
    QFrame* m_card = nullptr;
};

// Toolbar avatar with the 1.5 px profile-colour ring (§2.2 trigger).
void paintProfileAvatarLabel(QLabel* label, int avatarSize, qreal dpr);
