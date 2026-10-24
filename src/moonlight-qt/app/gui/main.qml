import QtQuick 2.9
import QtQuick.Controls.Basic
import QtQuick.Layouts 1.3
import QtQuick.Window 2.2
import QtQuick.Controls.Material 2.2
import Qt.labs.platform 1.1 as LabPlatform

import ComputerManager 1.0
import StreamingPreferences 1.0
import SystemProperties 1.0
import SdlGamepadKeyNavigation 1.0
import MslinkTunnel 1.0
import StreamMenu 1.0

// =============================================================================
//  main.qml —— MiniStream 客户端外壳
//
//  深色自绘工具栏：品牌标 / 隧道状态 / 重新连接 / 添加主机 / 设置。
//  页面切换仍走 StackView；与 backend 的调用关系保持原样。
// =============================================================================
ApplicationWindow {
    property bool pollingActive: false

    // MiniStream：点关闭时默认最小化到托盘，而不是退出。
    // forceQuit 由 C++ 托盘菜单「退出」置位后才真正关闭。
    property bool forceQuit: false

    // Set by SettingsView to force the back operation to pop all
    // pages except the initial view. This is required when doing
    // a retranslate() because AppView breaks for some reason.
    property bool clearOnBack: false

    // 重新连接隧道的临时提示
    property string tunnelActionMessage: ""

    id: window
    title: "MiniStream · Workspace | Powered by jianhx2026"
    width: Math.min(1280, Screen.desktopAvailableWidth - 32)
    height: Math.min(800, Screen.desktopAvailableHeight - 48)
    minimumWidth: 640
    minimumHeight: 480
    color: theme.canvas

    Theme { id: theme }

    // ApplicationWindow 自己会画一层 style 的 background（浅色），
    // 单设 color 不生效，必须显式覆盖 background，否则深色主题下窗口底色是浅灰。
    background: Rectangle { color: theme.canvas }
    palette.window: theme.canvas
    palette.windowText: theme.text
    palette.base: theme.surface
    palette.text: theme.text
    palette.button: theme.surfaceAlt
    palette.buttonText: theme.text
    palette.highlight: theme.primary
    palette.highlightedText: "#ffffff"

    // MiniStream：点 X 不退出，只收进托盘（真正退出走托盘右键菜单）。
    onClosing: function(close) {
        if (trayIcon.available && !window.forceQuit) {
            close.accepted = false
            window.hide()
        }
    }

    // ---- MiniStream：系统托盘 ----
    // 关闭主窗口后进程继续在托盘里跑，隧道和主机列表都保持；
    // 右键托盘才有「退出 MiniStream」。
    LabPlatform.SystemTrayIcon {
        id: trayIcon
        visible: true
        icon.source: "qrc:/res/brand.svg"
        tooltip: "MiniStream 主控端"

        menu: LabPlatform.Menu {
            LabPlatform.MenuItem {
                text: "显示主界面"
                onTriggered: {
                    window.show()
                    window.raise()
                    window.requestActivate()
                }
            }
            LabPlatform.MenuSeparator {}
            LabPlatform.MenuItem {
                text: "退出 MiniStream"
                onTriggered: {
                    window.forceQuit = true
                    Qt.quit()
                }
            }
        }

        onActivated: function(reason) {
            // Trigger = 左键单击；DoubleClick = 双击
            if (reason === LabPlatform.SystemTrayIcon.Trigger ||
                    reason === LabPlatform.SystemTrayIcon.DoubleClick) {
                if (window.visible) {
                    window.hide()
                } else {
                    window.show()
                    window.raise()
                    window.requestActivate()
                }
            }
        }
    }

    // ---- 隧道状态（工具栏用） ----
    readonly property bool tunnelRunning: MslinkTunnel.running
    readonly property bool tunnelConnected: MslinkTunnel.tunnelUp
    readonly property bool tunnelConnecting: MslinkTunnel.connecting
    readonly property color tunnelColor: tunnelConnected ? theme.success
                                        : (tunnelRunning || tunnelConnecting) ? theme.warning
                                        : theme.danger
    readonly property string tunnelLabel: {
        if (tunnelConnected) {
            return "隧道已连接"
        }
        if (tunnelConnecting) {
            return "隧道连接中…"
        }
        if (tunnelRunning) {
            return "隧道已断开"
        }
        return "隧道未连接"
    }

    // This function runs prior to creation of the initial StackView item
    function doEarlyInit() {
        // 深色底：与 Theme.qml 的 canvas 保持一致
        // MiniStream 深色主题：不再依赖 Material 3 检测结果，
        // 固定深色底/浅色字（之前在某些系统上检测为 false，窗口底色会变成浅灰）。
        Material.background = theme.canvas
        Material.foreground = theme.text

        SdlGamepadKeyNavigation.enable()
    }

    Component.onCompleted: {
        // Show the window according to the user's preferences
        if (SystemProperties.hasDesktopEnvironment) {
            if (StreamingPreferences.uiDisplayMode == StreamingPreferences.UI_MAXIMIZED) {
                window.showMaximized()
            }
            else if (StreamingPreferences.uiDisplayMode == StreamingPreferences.UI_FULLSCREEN) {
                window.showFullScreen()
            }
            else {
                window.show()
            }
        } else {
            window.showFullScreen()
        }

        // Display any modal dialogs for configuration warnings
        if (runConfigChecks) {
            if (SystemProperties.isWow64) {
                wow64Dialog.open()
            }

            // Hardware acceleration and unmapped gamepads are checked asynchronously
            SystemProperties.hasHardwareAccelerationChanged.connect(hasHardwareAccelerationChanged)
            SystemProperties.unmappedGamepadsChanged.connect(hasUnmappedGamepadsChanged)
            SystemProperties.startAsyncLoad()
        }
    }

    function hasHardwareAccelerationChanged() {
        if (!SystemProperties.hasHardwareAcceleration && StreamingPreferences.videoDecoderSelection !== StreamingPreferences.VDS_FORCE_SOFTWARE) {
            if (SystemProperties.isRunningXWayland) {
                xWaylandDialog.open()
            }
            else {
                noHwDecoderDialog.open()
            }
        }
    }

    function hasUnmappedGamepadsChanged() {
        if (SystemProperties.unmappedGamepads) {
            unmappedGamepadDialog.unmappedGamepads = SystemProperties.unmappedGamepads
            unmappedGamepadDialog.open()
        }
    }

    function goBack() {
        if (clearOnBack) {
            // Pop all items except the first one
            stackView.pop(null)
            clearOnBack = false
        }
        else {
            stackView.pop()
        }
    }

    function showAddPcDialog() {
        addPcDialog.open()
    }

    // 重新连接 mslink 隧道（保持主机列表不变）
    function reconnectTunnel() {
        if (!MslinkTunnel.configured) {
            tunnelActionMessage = "尚未配置被控端，请先填写地址和口令"
            tunnelMessageTimer.restart()
            mslinkSetupDialog.open()
            return
        }

        var err = MslinkTunnel.connectToServer(MslinkTunnel.server,
                                                MslinkTunnel.tcpPort,
                                                MslinkTunnel.udpPort,
                                                MslinkTunnel.password)
        if (err !== "") {
            tunnelActionMessage = err
            mslinkSetupDialog.open()
        }
        else {
            tunnelActionMessage = "正在重新连接隧道…"
        }
        tunnelMessageTimer.restart()
    }

    Timer {
        id: tunnelMessageTimer
        interval: 4000
        onTriggered: window.tunnelActionMessage = ""
    }

    readonly property bool compact: width < 980
    readonly property bool shortWindow: height < 620
    readonly property var liveMetrics: MslinkTunnel.telemetry
    readonly property var mediaMetrics: liveMetrics.media || ({})
    property string workspacePage: "devices"
    function metric(value, suffix, digits) { return typeof value === "number" && isFinite(value) ? value.toFixed(digits) + suffix : "—" }
    function showWorkspacePage(page) {
        if (page === "experience" && stackView.depth > 1) stackView.pop(null)
        workspacePage = page
    }

    Rectangle {
        id: workspaceNav
        anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom
        width: window.compact ? 68 : 220; color: theme.toolbar
        Rectangle { anchors.right: parent.right; width: 1; height: parent.height; color: theme.borderSoft }
        ColumnLayout {
            anchors.fill: parent; anchors.margins: window.compact ? 10 : 20; spacing: 10
            Text { visible: !window.compact; text: "CONTROL / 主控工作台"; color: theme.primary; font.pixelSize: 9; font.letterSpacing: 1.6; Layout.topMargin: 16; Layout.bottomMargin: 16 }
            BrandButton { label: window.compact ? "" : "我的设备"; iconSource: "qrc:/res/devices.svg"; ToolTip.text: "我的设备"; ToolTip.visible: hovered && window.compact; Layout.fillWidth: true; accent: window.workspacePage === "devices"; onClicked: window.showWorkspacePage("devices") }
            BrandButton { label: window.compact ? "" : "体验监测"; iconSource: "qrc:/res/activity.svg"; ToolTip.text: "体验监测"; ToolTip.visible: hovered && window.compact; Layout.fillWidth: true; accent: window.workspacePage === "experience"; onClicked: window.showWorkspacePage("experience") }
            BrandButton { label: window.compact ? "" : "连接配置"; iconSource: "qrc:/res/network.svg"; ToolTip.text: "连接配置"; ToolTip.visible: hovered && window.compact; Layout.fillWidth: true; onClicked: mslinkSetupDialog.open() }
            BrandButton { label: window.compact ? "" : "串流偏好"; iconSource: "qrc:/res/sliders.svg"; ToolTip.text: "串流偏好"; ToolTip.visible: hovered && window.compact; Layout.fillWidth: true; onClicked: { window.showWorkspacePage("devices"); settingsButton.clicked() } }
            Item { Layout.fillHeight: true }
            Rectangle { visible: !window.compact; Layout.fillWidth: true; height: 66; radius: 10; color: theme.surface; border.color: theme.borderSoft
                Column { anchors.fill: parent; anchors.margins: 11; spacing: 4
                    Text { text: "连接方式"; color: theme.textTertiary; font.pixelSize: 9 }
                    Text { text: window.liveMetrics.transport === "udp" ? "UDP 实时通道" : window.liveMetrics.transport === "tcp_fallback" ? "TCP 兼容通道" : "等待连接"; color: window.tunnelConnected ? theme.success : theme.textSecondary; font.pixelSize: 11 }
                    Text { text: "1 TCP + 1 UDP"; color: theme.textTertiary; font.pixelSize: 9 }
                }
            }
            Rectangle { Layout.fillWidth: true; height: 1; color: theme.borderSoft }
            BrandButton { label: window.compact ? "" : "使用教程"; iconSource: "qrc:/res/help.svg"; Layout.fillWidth: true; onClicked: Qt.openUrlExternally("https://ministream.jianhx.com/tutorial.html") }
            Text { visible: !window.compact; text: "MiniStream 2.2 · Workspace"; color: theme.textTertiary; font.pixelSize: 10 }
            Text { visible: !window.compact; text: "Powered by jianhx2026"; color: theme.textSecondary; font.pixelSize: 10 }
            Text { visible: !window.compact; text: "Sunshine / Moonlight · GPLv3"; color: theme.textTertiary; font.pixelSize: 9 }
        }
    }

    ExperienceView {
        anchors.left: workspaceNav.right; anchors.right: parent.right; anchors.top: parent.top; anchors.bottom: parent.bottom
        visible: window.workspacePage === "experience"
        focus: visible
    }

    StackView {
        id: stackView
        visible: window.workspacePage === "devices"
        anchors.left: workspaceNav.right; anchors.right: parent.right; anchors.top: parent.top; anchors.bottom: parent.bottom
        focus: visible
        // StackView 是 Control，会自带一层 style 的浅色 background，
        // 不覆盖的话深色主题下窗口内容区会是一片浅灰。
        background: Rectangle { color: theme.canvas }
    palette.window: theme.canvas
    palette.windowText: theme.text
    palette.base: theme.surface
    palette.text: theme.text
    palette.button: theme.surfaceAlt
    palette.buttonText: theme.text
    palette.highlight: theme.primary
    palette.highlightedText: "#ffffff"

        Component.onCompleted: {
            // Perform our early initialization before constructing
            // the initial view and pushing it to the StackView
            doEarlyInit()
            push(initialView)
        }

        onCurrentItemChanged: {
            // Ensure focus travels to the next view when going back
            if (currentItem) {
                currentItem.forceActiveFocus()
            }
        }

        Keys.onEscapePressed: {
            if (depth > 1) {
                goBack()
            }
            else {
                quitConfirmationDialog.open()
            }
        }

        Keys.onBackPressed: {
            if (depth > 1) {
                goBack()
            }
            else {
                quitConfirmationDialog.open()
            }
        }

        Keys.onMenuPressed: {
            settingsButton.clicked()
        }

        // This is a keypress we've reserved for letting the
        // SdlGamepadKeyNavigation object tell us to show settings
        // when Menu is consumed by a focused control.
        Keys.onHangupPressed: {
            settingsButton.clicked()
        }
    }

    // This timer keeps us polling for 5 minutes of inactivity
    // to allow the user to work with MiniStream on a second display
    // while dealing with configuration issues. This will ensure
    // machines come online even if the input focus isn't on MiniStream.
    Timer {
        id: inactivityTimer
        interval: 5 * 60000
        onTriggered: {
            if (!active && pollingActive) {
                ComputerManager.stopPollingAsync()
                pollingActive = false
            }
        }
    }

    onVisibleChanged: {
        // When we become invisible while streaming is going on,
        // stop polling immediately.
        if (!visible) {
            inactivityTimer.stop()

            if (pollingActive) {
                ComputerManager.stopPollingAsync()
                pollingActive = false
            }
        }
        else if (active) {
            // When we become visible and active again, start polling
            inactivityTimer.stop()

            // Restart polling if it was stopped
            if (!pollingActive) {
                ComputerManager.startPolling()
                pollingActive = true
            }
        }

        // Poll for gamepad input only when the window is in focus
        SdlGamepadKeyNavigation.notifyWindowFocus(visible && active)
    }

    onActiveChanged: {
        if (active) {
            // Stop the inactivity timer
            inactivityTimer.stop()

            // Restart polling if it was stopped
            if (!pollingActive) {
                ComputerManager.startPolling()
                pollingActive = true
            }
        }
        else {
            // Start the inactivity timer to stop polling
            // if focus does not return within a few minutes.
            inactivityTimer.restart()
        }

        // Poll for gamepad input only when the window is in focus
        SdlGamepadKeyNavigation.notifyWindowFocus(visible && active)
    }

    function navigateTo(url, objectType)
    {
        var existingItem = stackView.find(function(item, index) {
            return item instanceof objectType
        })

        if (existingItem !== null) {
            // Pop to the existing item
            stackView.pop(existingItem)
        }
        else {
            // Create a new item
            stackView.push(url)
        }
    }

    // =========================================================================
    //  自绘深色工具栏
    // =========================================================================
    header: Rectangle {
        id: toolBar
        height: 76
        color: theme.toolbar

        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width
            height: 1
            color: theme.border
        }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 14
            anchors.rightMargin: 12
            spacing: 8

            BrandButton {
                id: backButton
                visible: stackView.depth > 1
                iconSource: "qrc:/res/arrow_left.svg"
                implicitWidth: 34
                onClicked: goBack()

                Keys.onDownPressed: {
                    stackView.currentItem.forceActiveFocus(Qt.TabFocus)
                }
            }

            // ---- 品牌标 ----
            Row {
                id: brandRow
                Layout.alignment: Qt.AlignVCenter
                spacing: 10

                Image {
                    anchors.verticalCenter: parent.verticalCenter
                    source: "qrc:/res/brand.svg"
                    width: 30
                    height: 30
                    sourceSize.width: 30
                    sourceSize.height: 30
                }

                Column {
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 0

                    Text {
                        visible: window.width >= 800
                        text: "MiniStream"
                        color: theme.text
                        font.pixelSize: 20
                        font.bold: true
                        font.family: theme.fontFamily
                    }

                    Text {
                        visible: window.width > 980
                        text: "主控端 · 连接自己的电脑"
                        color: theme.textTertiary
                        font.pixelSize: 10
                        font.family: theme.fontFamily
                    }
                }
            }

            // 当前页面名
            Text {
                id: pageTitleLabel
                Layout.leftMargin: 8
                visible: !window.compact && stackView.depth > 1 && stackView.currentItem !== null
                text: stackView.currentItem ? stackView.currentItem.objectName : ""
                color: theme.textSecondary
                font.pixelSize: 13
                font.family: theme.fontFamily
                elide: Text.ElideRight
                Layout.maximumWidth: window.compact ? 80 : 260
                Layout.alignment: Qt.AlignVCenter
            }

            Item { Layout.fillWidth: true }

            // ---- 重新连接提示 ----
            Text {
                visible: window.tunnelActionMessage !== ""
                text: window.tunnelActionMessage
                color: theme.textSecondary
                font.pixelSize: 11
                font.family: theme.fontFamily
                elide: Text.ElideRight
                Layout.maximumWidth: window.compact ? 80 : 260
                Layout.alignment: Qt.AlignVCenter
            }

            // ---- 隧道状态 ----
            Rectangle {
                id: tunnelChip
                Layout.alignment: Qt.AlignVCenter
                height: 32
                implicitWidth: tunnelChipRow.implicitWidth + 24
                radius: 16
                color: theme.surface
                border.width: 1
                border.color: tunnelChipMouse.containsMouse ? theme.borderStrong : theme.border

                Row {
                    id: tunnelChipRow
                    anchors.centerIn: parent
                    spacing: 7

                    Rectangle {
                        width: 8
                        height: 8
                        radius: 4
                        color: window.tunnelColor
                        anchors.verticalCenter: parent.verticalCenter
                    }

                    Text {
                        text: window.width < 800 ? (window.tunnelConnected ? "已连接" : "未连接") : window.tunnelLabel
                        color: theme.textSecondary
                        font.pixelSize: 12
                        font.family: theme.fontFamily
                        anchors.verticalCenter: parent.verticalCenter
                    }
                }

                MouseArea {
                    id: tunnelChipMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: mslinkSetupDialog.open()
                }

                ToolTip.visible: tunnelChipMouse.containsMouse
                ToolTip.delay: 500
                ToolTip.text: MslinkTunnel.running ? MslinkTunnel.statusText
                                                   : "隧道未运行。点击填写被控端地址和连接口令。"
            }

            BrandButton { label: "连接设置"; accent: !window.tunnelConnected; onClicked: mslinkSetupDialog.open() }
            BrandButton {
                id: reconnectButton
                visible: !window.compact
                label: "重新连接"
                iconSource: "qrc:/res/refresh.svg"
                fontSize: 12
                Layout.alignment: Qt.AlignVCenter
                onClicked: reconnectTunnel()
                ToolTip.delay: 800
                ToolTip.timeout: 4000
                ToolTip.visible: hovered
                ToolTip.text: "重新连接 mslink 隧道（主机列表保持不变）"
            }

            BrandButton {
                id: addPcButton
                visible: stackView.currentItem instanceof PcView
                label: window.width > 1080 ? "添加主机" : ""
                iconSource: "qrc:/res/plus.svg"
                fontSize: 12
                Layout.alignment: Qt.AlignVCenter
                onClicked: addPcDialog.open()

                Shortcut {
                    id: newPcShortcut
                    sequence: StandardKey.New
                    onActivated: addPcButton.clicked()
                }

                ToolTip.delay: 800
                ToolTip.timeout: 3000
                ToolTip.visible: hovered
                ToolTip.text: "手动添加主机" + (newPcShortcut.nativeText ? (" (" + newPcShortcut.nativeText + ")") : "")

                Keys.onDownPressed: {
                    stackView.currentItem.forceActiveFocus(Qt.TabFocus)
                }
            }

            BrandButton {
                id: settingsButton
                label: window.compact ? "" : "设置"
                iconSource: "qrc:/res/settings.svg"
                fontSize: 12
                Layout.alignment: Qt.AlignVCenter
                onClicked: navigateTo("qrc:/gui/SettingsView.qml", SettingsView)

                Keys.onDownPressed: {
                    stackView.currentItem.forceActiveFocus(Qt.TabFocus)
                }

                Shortcut {
                    id: settingsShortcut
                    sequence: StandardKey.Preferences
                    onActivated: settingsButton.clicked()
                }

                ToolTip.delay: 800
                ToolTip.timeout: 3000
                ToolTip.visible: hovered
                ToolTip.text: "设置" + (settingsShortcut.nativeText ? (" (" + settingsShortcut.nativeText + ")") : "")
            }
        }
    }

    // =========================================================================
    //  对话框
    // =========================================================================
    ErrorMessageDialog {
        id: noHwDecoderDialog
        text: "没有检测到可用的硬件视频解码器。在这种配置下，串流性能可能会明显下降。"
    }

    ErrorMessageDialog {
        id: xWaylandDialog
        text: "硬件加速在 XWayland 下不可用，继续使用可能导致串流性能不佳。"
    }

    NavigableMessageDialog {
        id: wow64Dialog
        standardButtons: Dialog.Ok
        text: "当前版本不是为你的 PC 架构优化的。为获得最佳串流性能，请下载 %1 版本。".arg(SystemProperties.friendlyNativeArchName)
    }

    ErrorMessageDialog {
        id: unmappedGamepadDialog
        property string unmappedGamepads : ""
        text: "检测到未映射的手柄：" + "\n" + unmappedGamepads
        helpTextSeparator: "\n\n"
    }

    // This dialog appears when quitting via keyboard or gamepad button
    NavigableMessageDialog {
        id: quitConfirmationDialog
        standardButtons: Dialog.Yes | Dialog.No
        text: "确定要退出 MiniStream 吗？"
        // For keyboard/gamepad navigation
        onAccepted: Qt.quit()
    }

    // -------------------------------------------------------------------------
    //  MiniStream：串流中的浮层菜单（F12 / Ctrl+Alt+Shift+X 呼出）
    //  常驻实例化，自己监听 StreamMenu.menuOpen 控制显隐；
    //  StreamSegue 负责在串流开始/结束时 attach / close，所以只有串流时能打开。
    // -------------------------------------------------------------------------
    StreamMenuWindow {
        id: streamMenuWindow
    }

    // HACK: This belongs in StreamSegue but keeping a dialog around after the parent
    // dies can trigger bugs in Qt 5.12 that cause the app to crash. For now, we will
    // host this dialog in a QML component that is never destroyed.
    //
    // To repro: Start a stream, cut the network connection to trigger the "Connection
    // terminated" dialog, wait until the app grid times out back to the PC grid, then
    // try to dismiss the dialog.
    ErrorMessageDialog {
        id: streamSegueErrorDialog

        property bool quitAfter: false

        onClosed: {
            if (quitAfter) {
                Qt.quit()
            }

            // StreamSegue assumes its dialog will be re-created each time we
            // start streaming, so fake it by wiping out the text each time.
            text = ""
        }
    }

    NavigableDialog {
        id: addPcDialog
        property string label: "输入被控端 IP 地址（走隧道时填 127.0.0.1）："

        standardButtons: Dialog.Ok | Dialog.Cancel

        onOpened: {
            // Force keyboard focus on the textbox so keyboard navigation works
            editText.forceActiveFocus()
        }

        onClosed: {
            editText.clear()
        }

        onAccepted: {
            if (editText.text) {
                ComputerManager.addNewHostManually(editText.text.trim())
            }
        }

        ColumnLayout {
            Label {
                text: addPcDialog.label
                font.bold: true
            }

            TextField {
                id: editText
                Layout.fillWidth: true
                focus: true

                Keys.onReturnPressed: {
                    addPcDialog.accept()
                }

                Keys.onEnterPressed: {
                    addPcDialog.accept()
                }
            }
        }
    }

    // ---- MiniStream：2 端口隧道连接设置 ----
    // 启动时若已有 mslink.ini 就静默自动连接；配置缺失或连接失败才弹出来。
    // 隧道已在本机 127.0.0.1 监听全部标准端口，连接成功后自动把
    // 127.0.0.1 加进主机列表，用户只需要填被控端地址和口令。
    MslinkSetupDialog {
        id: mslinkSetupDialog
        autoConnect: true
    }
}
