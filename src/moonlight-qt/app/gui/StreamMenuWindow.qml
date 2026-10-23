// 注意：这里必须用「不带版本号」的 import。
// 带版本号（QtQuick 2.9 / QtQuick.Window 2.2）会把模块的 revision 钉死在老版本，
// Window.transientParent 这种后加的属性会被判定为「不可用」，整个组件加载失败。
import QtQuick
import QtQuick.Window
import QtQuick.Controls

import StreamMenu 1.0

// =============================================================================
//  StreamMenuWindow.qml —— MiniStream 串流中的浮层菜单
//
//  由 main.qml 常驻实例化，visible 绑定 StreamMenu.menuOpen：
//  串流中按 F12 或 Ctrl+Alt+Shift+X 呼出，菜单里所有操作都可以点击。
//  配色与 Theme.qml / SettingsView.qml 的深色卡片保持一致。
// =============================================================================
Window {
    id: menuWindow

    width: 320
    height: 366

    visible: StreamMenu.menuOpen
    color: "transparent"

    flags: Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.Tool
    title: "MiniStream 串流菜单"

    // 串流期间主窗口是隐藏的（StreamSegue 里 window.visible = false）。
    // 如果不显式清掉 transientParent，Qt 会把本窗口当成主窗口的附属窗口，
    // 主窗口一隐藏它也会被一起隐藏 —— 表现就是「menuOpen 已经是 true，
    // 但窗口永远弹不出来」。
    transientParent: null

    Theme { id: theme }

    // ---- 菜单项（文案跟随实时状态变化）----
    property var menuItems: [
        {
            key: "quit",
            title: "断开串流",
            detail: "Ctrl + Alt + Shift + Q",
            accent: true
        },
        {
            key: "stats",
            title: "性能统计：" + (StreamMenu.statsOverlayEnabled ? "关" : "开"),
            detail: "显示帧率 / 码率 / 延迟",
            accent: false
        },
        {
            key: "mouse",
            title: "鼠标模式：" + (StreamMenu.absoluteMouseMode ? "绝对坐标" : "相对坐标"),
            detail: "点击切换为" + (StreamMenu.absoluteMouseMode ? "相对坐标" : "绝对坐标"),
            accent: false
        },
        {
            key: "fullscreen",
            title: "全屏 / 窗口",
            detail: "切换串流显示模式",
            accent: false
        },
        {
            key: "minimize",
            title: "最小化",
            detail: "把串流窗口缩到任务栏",
            accent: false
        },
        {
            key: "close",
            title: "关闭菜单",
            detail: "也可以按 F12 / Esc",
            accent: false
        }
    ]

    function activate(key) {
        if (key === "quit") {
            StreamMenu.quitStream()
        }
        else if (key === "stats") {
            StreamMenu.toggleStatsOverlay()
        }
        else if (key === "mouse") {
            StreamMenu.toggleMouseMode()
        }
        else if (key === "fullscreen") {
            StreamMenu.toggleFullScreen()
        }
        else if (key === "minimize") {
            StreamMenu.minimizeWindow()
        }
        else {
            StreamMenu.closeMenu()
        }
    }

    function recenter() {
        if (menuWindow.screen) {
            x = Math.round(menuWindow.screen.virtualX +
                           (menuWindow.screen.width - menuWindow.width) / 2)
            y = Math.round(menuWindow.screen.virtualY +
                           (menuWindow.screen.height - menuWindow.height) / 2)
        }
    }

    onVisibleChanged: {
        console.log("MiniStream: StreamMenuWindow visible =", visible,
                    "x =", x, "y =", y, "screen =", menuWindow.screen)
        if (visible) {
            recenter()
            raise()
            requestActivate()
        }
    }

    // 菜单窗口自己拿到键盘焦点后，F12 / Esc 也能关闭
    Shortcut {
        sequence: "F12"
        onActivated: StreamMenu.closeMenu()
    }

    Shortcut {
        sequence: "Escape"
        onActivated: StreamMenu.closeMenu()
    }

    // ---- 半透明深色卡片 ----
    Rectangle {
        id: card
        anchors.fill: parent
        radius: theme.radiusCard
        color: theme.surface
        opacity: 0.97
        border.width: 1
        border.color: theme.borderStrong

        // 顶部品牌色细条
        Rectangle {
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.leftMargin: theme.radiusCard
            anchors.rightMargin: theme.radiusCard
            height: 2
            radius: 1
            color: theme.primary
            opacity: 0.85
        }

        // ---- 标题栏 ----
        Item {
            id: header
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.topMargin: 10
            height: 38

            Column {
                anchors.left: parent.left
                anchors.leftMargin: 16
                anchors.verticalCenter: parent.verticalCenter
                spacing: 1

                Text {
                    text: "串流菜单"
                    color: theme.text
                    font.pixelSize: 14
                    font.bold: true
                    font.family: theme.fontFamily
                }

                Text {
                    text: "F12 或 Ctrl + Alt + Shift + X 呼出"
                    color: theme.textTertiary
                    font.pixelSize: 10
                    font.family: theme.fontFamily
                }
            }
        }

        // ---- 菜单项 ----
        Column {
            id: list
            anchors.top: header.bottom
            anchors.topMargin: 4
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.leftMargin: 10
            anchors.rightMargin: 10
            spacing: 6

            Repeater {
                model: menuItems

                delegate: Rectangle {
                    id: row
                    width: list.width
                    height: 42
                    radius: theme.radiusControl
                    color: rowMouse.containsMouse
                           ? theme.surfaceHover
                           : (modelData.accent ? theme.dangerSoft : theme.surfaceAlt)
                    border.width: 1
                    border.color: rowMouse.containsMouse ? theme.borderStrong
                                                         : theme.borderSoft

                    Text {
                        anchors.left: parent.left
                        anchors.leftMargin: 12
                        anchors.verticalCenter: parent.verticalCenter
                        text: modelData.title
                        color: modelData.accent ? theme.danger : theme.text
                        font.pixelSize: 13
                        font.bold: modelData.accent
                        font.family: theme.fontFamily
                    }

                    Text {
                        anchors.right: parent.right
                        anchors.rightMargin: 12
                        anchors.verticalCenter: parent.verticalCenter
                        text: modelData.detail
                        color: theme.textTertiary
                        font.pixelSize: 10
                        font.family: theme.fontFamily
                    }

                    MouseArea {
                        id: rowMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: menuWindow.activate(modelData.key)
                    }
                }
            }
        }

        // ---- 底部提示 ----
        Text {
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 8
            anchors.horizontalCenter: parent.horizontalCenter
            text: "点击任意一项即可执行"
            color: theme.textTertiary
            font.pixelSize: 10
            font.family: theme.fontFamily
        }
    }
}
