#include "streammenu.h"

#include "streaming/session.h"
#include "streaming/video/overlaymanager.h"
#include "settings/streamingpreferences.h"

#include <QDebug>

namespace {
StreamMenu* s_Instance = nullptr;
}

StreamMenu::StreamMenu(QObject* parent)
    : QObject(parent),
      m_MenuOpen(false)
{
    s_Instance = this;
}

StreamMenu* StreamMenu::get()
{
    // 正常情况下 main.cpp 的 qmlRegisterSingletonType 已经创建了实例。
    // 这里只是给 C++ 侧的兜底，保证任何时候都能拿到唯一的那个。
    if (s_Instance == nullptr) {
        s_Instance = new StreamMenu(nullptr);
    }
    return s_Instance;
}

Session* StreamMenu::session() const
{
    return m_Session.data();
}

bool StreamMenu::statsOverlayEnabled() const
{
    Session* s = session();
    if (s == nullptr) {
        // 不在串流时显示偏好的状态
        StreamingPreferences* prefs = StreamingPreferences::get();
        return prefs != nullptr && prefs->showPerformanceOverlay;
    }

    return s->getOverlayManager().isOverlayEnabled(Overlay::OverlayDebug);
}

bool StreamMenu::absoluteMouseMode() const
{
    StreamingPreferences* prefs = StreamingPreferences::get();
    return prefs != nullptr && prefs->absoluteMouseMode;
}

void StreamMenu::setAbsoluteMouseMode(bool absolute)
{
    StreamingPreferences* prefs = StreamingPreferences::get();
    if (prefs != nullptr) {
        prefs->absoluteMouseMode = absolute;
    }

    // 串流中要立刻作用到 SdlInputHandler，否则点了没反应
    Session* s = session();
    if (s != nullptr) {
        s->setAbsoluteMouseMode(absolute);
    }

    emit absoluteMouseModeChanged();
}

void StreamMenu::attachSession(QObject* sessionObject)
{
    Session* s = qobject_cast<Session*>(sessionObject);
    qInfo() << "MiniStream: StreamMenu::attachSession" << sessionObject << "->" << (void*)s;
    if (m_Session == s) {
        return;
    }

    if (m_Session != nullptr) {
        disconnect(m_Session, nullptr, this, nullptr);
    }

    m_Session = s;

    if (m_Session != nullptr) {
        // 串流中按热键 -> 菜单显隐
        connect(m_Session, &Session::menuRequested,
                this, &StreamMenu::onMenuRequested);

        // 串流结束时务必把菜单收起来
        connect(m_Session, &Session::quitStarting,
                this, &StreamMenu::closeMenu);
        connect(m_Session, &Session::sessionFinished,
                this, &StreamMenu::closeMenu);
    }

    closeMenu();
    emit statsOverlayEnabledChanged();
}

void StreamMenu::detachSession()
{
    attachSession(nullptr);
}

void StreamMenu::onMenuRequested()
{
    qInfo() << "MiniStream: StreamMenu::onMenuRequested, session =" << (void*)session();

    // Session 那边已经按 streamingMenuEnabled 过滤过一次，这里双保险：
    // 设置里关掉浮层菜单时，热键不应该弹出菜单。
    StreamingPreferences* prefs = StreamingPreferences::get();
    if (prefs != nullptr && !prefs->streamingMenuEnabled) {
        qInfo() << "MiniStream: stream menu disabled in preferences";
        return;
    }

    toggleMenu();
}

void StreamMenu::setMenuOpen(bool open)
{
    // 只在串流时允许打开
    if (open && session() == nullptr) {
        qInfo() << "MiniStream: setMenuOpen(true) rejected, no session";
        return;
    }

    if (m_MenuOpen == open) {
        return;
    }

    m_MenuOpen = open;
    qInfo() << "MiniStream: menuOpen ->" << open << "singleton=" << instanceId();

    if (session() != nullptr) {
        session()->setMenuOpen(open);
    }

    emit menuOpenChanged();
}

void StreamMenu::openMenu()
{
    setMenuOpen(true);
}

void StreamMenu::closeMenu()
{
    setMenuOpen(false);
}

void StreamMenu::toggleMenu()
{
    setMenuOpen(!m_MenuOpen);
}

void StreamMenu::quitStream()
{
    Session* s = session();
    if (s == nullptr) {
        return;
    }

    // 菜单反正要随着串流一起消失
    closeMenu();

    s->quitStream();
}

void StreamMenu::toggleStatsOverlay()
{
    Session* s = session();
    if (s == nullptr) {
        return;
    }

    s->toggleStatsOverlay();
    emit statsOverlayEnabledChanged();
}

void StreamMenu::toggleMouseMode()
{
    setAbsoluteMouseMode(!absoluteMouseMode());
}

void StreamMenu::toggleFullScreen()
{
    Session* s = session();
    if (s == nullptr) {
        return;
    }

    s->toggleFullScreenMode();
}

void StreamMenu::minimizeWindow()
{
    Session* s = session();
    if (s == nullptr) {
        return;
    }

    closeMenu();
    s->minimizeStreamWindow();
}
