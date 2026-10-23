#pragma once

#include <QObject>
#include <QPointer>

class Session;

// =============================================================================
//  streammenu.h —— MiniStream 串流中浮层菜单的 C++ 胶水（QML 单例）
//
//  QML 里用法：
//      import StreamMenu 1.0
//      visible: StreamMenu.menuOpen ... 点击时 StreamMenu.xxx()
//
//  职责：
//    1) 把 Session::menuRequested（F12 / Ctrl+Alt+Shift+X）转成 menuOpen 状态；
//    2) 菜单开关时通知 Session 放开/恢复 SDL 输入捕获；
//    3) 把菜单按钮转发给 Session 上那几个既有能力的入口。
// =============================================================================
class StreamMenu : public QObject
{
    Q_OBJECT

    Q_PROPERTY(bool menuOpen READ menuOpen NOTIFY menuOpenChanged)

    // 菜单里“性能统计”开关当前是否打开（跟随 Session 里的实时状态）
    Q_PROPERTY(bool statsOverlayEnabled READ statsOverlayEnabled NOTIFY statsOverlayEnabledChanged)

    // 菜单里“鼠标模式”显示用：true = 绝对坐标，false = 相对坐标
    Q_PROPERTY(bool absoluteMouseMode READ absoluteMouseMode WRITE setAbsoluteMouseMode
               NOTIFY absoluteMouseModeChanged)

public:
    explicit StreamMenu(QObject* parent = nullptr);

    static StreamMenu* get();

    bool menuOpen() const { return m_MenuOpen; }
    bool statsOverlayEnabled() const;
    bool absoluteMouseMode() const;
    void setAbsoluteMouseMode(bool absolute);

    // ---- 菜单开关 ----
    Q_INVOKABLE void openMenu();
    Q_INVOKABLE void closeMenu();
    Q_INVOKABLE void toggleMenu();

    // 调试用：确认 QML 侧看到的单例就是 C++ 侧这个对象
    Q_INVOKABLE QString instanceId() const
    {
        return QStringLiteral("%1").arg(quintptr(this), 0, 16);
    }

    // ---- 菜单按钮 ----
    Q_INVOKABLE void quitStream();              // 断开串流（与 Ctrl+Alt+Shift+Q 同一条路径）
    Q_INVOKABLE void toggleStatsOverlay();      // 性能统计 开/关
    Q_INVOKABLE void toggleMouseMode();         // 绝对坐标 <-> 相对坐标
    Q_INVOKABLE void toggleFullScreen();        // 全屏 / 窗口
    Q_INVOKABLE void minimizeWindow();          // 最小化串流窗口

    // 由 StreamSegue.qml 在串流会话建立/结束时调用
    Q_INVOKABLE void attachSession(QObject* session);
    Q_INVOKABLE void detachSession();

signals:
    void menuOpenChanged();
    void statsOverlayEnabledChanged();
    void absoluteMouseModeChanged();

private slots:
    void onMenuRequested();

private:
    void setMenuOpen(bool open);
    Session* session() const;

    bool m_MenuOpen;
    QPointer<Session> m_Session;
};
