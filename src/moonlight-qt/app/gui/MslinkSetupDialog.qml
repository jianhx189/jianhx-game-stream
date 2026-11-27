import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import MslinkTunnel 1.0
import ComputerManager 1.0

Dialog {
    id: dialog
    property bool autoConnect: true
    property var candidate: null
    property string savedEndpoint: ""
    property string oldName: ""
    property string newName: ""
    property string action: ""
    property string problem: ""
    property string step: "import"
    property string pairingId: ""
    property bool startupDiscovery: autoConnect
    readonly property bool busy: ["connecting", "discovering", "pairing"].includes(step)

    modal: true
    closePolicy: busy ? Popup.NoAutoClose : Popup.CloseOnEscape
    title: step === "import" ? "添加设备" : step === "switch" ? "切换设备" : "连接设备"
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
        if (busy) return
        resetSelection()
        step = "import"
        link.clear()
        open()
        link.forceActiveFocus()
    }
    function resetSelection() {
        candidate = null
        savedEndpoint = ""
        oldName = ""
        newName = ""
        action = ""
        problem = ""
    }
    function openInvitation(data, confirmed) {
        if (busy) return
        resetSelection()
        startupDiscovery = false
        candidate = data
        link.clear()
        chooseRoute(MslinkTunnel.matchesInvitation(data), confirmed)
    }
    function openSaved(endpoint, name, nextAction) {
        if (busy) return
        resetSelection()
        startupDiscovery = false
        savedEndpoint = endpoint
        newName = name
        action = nextAction
        chooseRoute(MslinkTunnel.localServiceEndpoint === endpoint, false)
    }
    function chooseRoute(same, confirmed) {
        open()
        if (same && MslinkTunnel.tunnelUp) {
            discover()
        } else if (!confirmed && !same && MslinkTunnel.tunnelUp) {
            oldName = window.connectedDeviceName() || MslinkTunnel.server
            step = "switch"
        } else {
            connectSelected()
        }
    }
    function submit() {
        const parsed = MslinkTunnel.parseInvitation(link.text)
        if (!parsed.ip) {
            problem = "链接无效。请从被控端复制完整邀请。"
            return
        }
        openInvitation(parsed, false)
    }
    function connectSelected() {
        problem = candidate ? MslinkTunnel.connectInvitation(candidate) : MslinkTunnel.connectDevice(savedEndpoint)
        if (problem) {
            step = "failed"
            return
        }
        step = "connecting"
        advance()
    }
    function discover() {
        step = "discovering"
        ComputerManager.addInvitedHost(MslinkTunnel.localServiceEndpoint, MslinkTunnel.expectedDevice())
    }
    function advance() {
        if (step !== "connecting") return
        if (MslinkTunnel.tunnelUp) discover()
        else if (!MslinkTunnel.connecting && MslinkTunnel.lastError) {
            problem = MslinkTunnel.lastError
            MslinkTunnel.disconnectTunnel()
            step = "failed"
        }
    }
    function finish(id) {
        const next = action
        step = "done"
        close()
        if (next) Qt.callLater(window.showConnectedDevice, id, next)
    }

    Connections {
        target: MslinkTunnel
        function onStateChanged() {
            dialog.advance()
            if (dialog.startupDiscovery && MslinkTunnel.tunnelUp && !dialog.visible) {
                dialog.startupDiscovery = false
                ComputerManager.addInvitedHost(MslinkTunnel.localServiceEndpoint, MslinkTunnel.expectedDevice())
            }
        }
    }
    Connections {
        target: ComputerManager
        function onInvitedHostReady(endpoint, id, paired, error) {
            if (endpoint !== MslinkTunnel.localServiceEndpoint) return
            if (dialog.step !== "discovering") {
                if (id && !MslinkTunnel.expectedDevice()) MslinkTunnel.rememberDevice(id)
                return
            }
            if (error) {
                MslinkTunnel.disconnectTunnel()
                dialog.problem = error
                dialog.step = "failed"
                return
            }
            const saveError = MslinkTunnel.rememberDevice(id)
            if (saveError) dialog.problem = saveError
            if (paired) {
                if (!saveError || dialog.action) dialog.finish(id)
                else dialog.step = "saved"
            } else if (dialog.candidate && dialog.candidate.pair) {
                dialog.step = "pairing"
                dialog.pairingId = id
                ComputerManager.pairInvitedHost(id, endpoint, dialog.candidate.pair, dialog.candidate.cert)
            } else if (dialog.action) {
                dialog.finish(id)
            } else {
                dialog.step = "manual"
            }
        }
        function onInvitationPairingFinished(id, error) {
            if (dialog.step !== "pairing" || id !== dialog.pairingId) return
            if (error) {
                dialog.problem = error
                dialog.step = "manual"
            } else if (dialog.problem) {
                dialog.step = "saved"
            } else {
                dialog.finish(id)
            }
        }
    }
    Component.onCompleted: {
        if (autoConnect && MslinkTunnel.tunnelUp) {
            startupDiscovery = false
            ComputerManager.addInvitedHost(MslinkTunnel.localServiceEndpoint, MslinkTunnel.expectedDevice())
        }
    }
    onClosed: {
        link.clear()
        resetSelection()
        step = "import"
    }

    contentItem: ColumnLayout {
        spacing: 14
        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
            text: {
                switch (dialog.step) {
                case "import": return "从被控端复制邀请链接，然后粘贴到这里。"
                case "switch": return "设备「" + dialog.oldName + "」当前已连接。要断开它并连接" +
                                      (dialog.newName ? "「" + dialog.newName + "」" : "邀请中的设备") + "吗？"
                case "connecting": return "正在连接被控端…"
                case "discovering": return "正在识别设备…"
                case "pairing": return "正在完成首次配对…"
                case "manual": return "设备已添加。打开设备菜单选择「连接」可完成 PIN 配对。"
                case "saved": return "设备已连接。"
                default: return "连接未完成。"
                }
            }
        }
        TextField {
            id: link
            Layout.fillWidth: true
            visible: dialog.step === "import"
            placeholderText: "粘贴完整邀请链接"
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
        BusyIndicator { visible: dialog.busy; running: visible; Layout.alignment: Qt.AlignHCenter }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
            color: theme.danger
            text: dialog.problem
            visible: text.length > 0
        }
        RowLayout {
            Layout.fillWidth: true
            visible: !dialog.busy
            BrandButton {
                visible: dialog.step === "import"
                label: "手动添加地址"
                onClicked: { dialog.close(); window.openDirectAddress() }
            }
            Item { Layout.fillWidth: true }
            BrandButton {
                label: dialog.step === "switch" ? "继续当前连接" : "关闭"
                onClicked: dialog.close()
            }
            BrandButton {
                visible: ["import", "switch", "failed"].includes(dialog.step)
                label: dialog.step === "import" ? "导入并连接" : dialog.step === "switch" ? "断开并连接" : "重试"
                enabled: dialog.step !== "import" || link.text.trim().length > 0
                accent: true
                onClicked: dialog.step === "import" ? dialog.submit() : dialog.connectSelected()
            }
        }
    }
}
