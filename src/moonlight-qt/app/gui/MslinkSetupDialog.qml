import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import MslinkTunnel 1.0
import ComputerManager 1.0

Dialog {
    id: dialog
    property bool autoConnect: true
    property var candidate: null
    property bool added: false
    property string problem: ""
    property string step: "import"
    modal: true
    title: step === "switch" ? "切换被控端" : step === "status" ? "连接状态" : "导入连接链接"
    width: Math.min(480, parent.width - 32)
    anchors.centerIn: parent
    padding: 20
    Theme { id: theme }
    background: Rectangle { radius: 14; color: theme.surface; border.color: theme.border }
    palette.windowText: theme.text
    palette.text: theme.text
    palette.buttonText: theme.text
    palette.button: theme.surfaceAlt
    palette.base: theme.canvas

    function beginImport() {
        step = "import"
        problem = ""
        candidate = null
        link.clear()
        open()
        link.forceActiveFocus()
    }
    function openInvitation(data) {
        candidate = data
        link.clear()
        problem = ""
        const same = MslinkTunnel.server === data.ip && MslinkTunnel.tcpPort === data.tcp &&
                     MslinkTunnel.udpPort === data.udp && MslinkTunnel.password === data.key
        open()
        if (same && (MslinkTunnel.tunnelUp || MslinkTunnel.connecting)) {
            step = "status"
        } else if (!same && MslinkTunnel.running) {
            step = "switch"
        } else {
            connectCandidate()
        }
    }
    function submit() {
        const parsed = MslinkTunnel.parseInvitation(link.text)
        if (!parsed.ip) {
            problem = "链接无效。请在被控端复制完整连接链接，再粘贴到这里。"
            return
        }
        openInvitation(parsed)
    }
    function connectCandidate() {
        step = "status"
        problem = ""
        if (!candidate) {
            window.reconnectTunnel()
            return
        }
        const data = candidate
        if (MslinkTunnel.server !== data.ip || MslinkTunnel.password !== data.key ||
            MslinkTunnel.tcpPort !== data.tcp || MslinkTunnel.udpPort !== data.udp) {
            MslinkTunnel.setSunshineCredentials("", "")
        }
        added = false
        problem = MslinkTunnel.connectToServer(data.ip, data.tcp, data.udp, data.key)
    }
    function discoverHost() {
        if (MslinkTunnel.tunnelUp && !added) {
            added = true
            ComputerManager.addNewHostManually(MslinkTunnel.localServiceEndpoint)
        }
    }
    Connections {
        target: MslinkTunnel
        function onStateChanged() { dialog.discoverHost() }
    }
    Component.onCompleted: {
        if (!autoConnect) return
        if (!MslinkTunnel.configured) beginImport()
        else if (MslinkTunnel.tunnelUp) discoverHost()
        else if (!MslinkTunnel.running) window.reconnectTunnel()
    }
    enter: Transition {
        NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 155; easing.type: Easing.OutCubic }
        NumberAnimation { property: "scale"; from: 0.985; to: 1; duration: 180; easing.type: Easing.OutCubic }
    }
    exit: Transition { NumberAnimation { property: "opacity"; to: 0; duration: 115 } }
    onClosed: { link.clear(); candidate = null; problem = "" }
    contentItem: ColumnLayout {
        spacing: 14
        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: dialog.step === "import" ? "在被控端的「连接设置」复制链接，然后粘贴到这里。" :
                  dialog.step === "switch" ? "已有隧道正在运行。切换到新链接将断开当前隧道。" :
                  MslinkTunnel.tunnelUp ? "隧道已认证。在设备列表中选择电脑，首次配对时按提示输入 PIN。" :
                  MslinkTunnel.connecting ? "正在连接被控端…" : "尚未连接，请确认被控端在线。"
        }
        TextField {
            id: link
            Layout.fillWidth: true
            visible: dialog.step === "import"
            placeholderText: "粘贴完整连接链接"
            placeholderTextColor: theme.textTertiary
            color: theme.text
            Accessible.name: "被控端邀请链接"
            background: Rectangle {
                radius: theme.radiusControl
                color: theme.canvas
                border.color: link.activeFocus ? theme.primary : theme.border
            }
            echoMode: TextInput.Password
            selectByMouse: true
            onTextEdited: dialog.problem = ""
            onAccepted: if (text.trim()) dialog.submit()
        }
        BusyIndicator { visible: dialog.step === "status" && MslinkTunnel.connecting; running: visible; Layout.alignment: Qt.AlignHCenter }
        Label {
            Layout.fillWidth: true; wrapMode: Text.Wrap; color: theme.danger
            text: dialog.problem || (dialog.step === "status" ? MslinkTunnel.lastError : "")
            visible: text.length > 0
        }
        RowLayout {
            Layout.alignment: Qt.AlignRight
            BrandButton { label: dialog.step === "status" && MslinkTunnel.tunnelUp ? "查看设备" : "关闭"; onClicked: dialog.close() }
            BrandButton {
                visible: dialog.step !== "status" || (!MslinkTunnel.connecting && !MslinkTunnel.tunnelUp)
                label: dialog.step === "import" ? "导入并连接" : dialog.step === "switch" ? "确认切换" : "重新连接"
                enabled: dialog.step !== "import" || link.text.trim().length > 0
                accent: true
                onClicked: dialog.step === "import" ? dialog.submit() : dialog.connectCandidate()
            }
        }
    }
}
