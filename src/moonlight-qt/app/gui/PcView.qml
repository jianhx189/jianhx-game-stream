import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import ComputerModel 1.0
import ComputerManager 1.0
import MslinkTunnel 1.0

CenteredGridView {
    id: pcGrid
    property ComputerModel computerModel: createModel()
    objectName: "我的设备"
    focus: true
    activeFocusOnTab: true
    topMargin: 14
    bottomMargin: 5
    cellWidth: Math.max(240, Math.floor(width / Math.max(1, Math.floor(width / 320))))
    cellHeight: 224
    model: computerModel
    Theme { id: theme }

    Component.onCompleted: currentIndex = -1
    StackView.onActivated: ComputerManager.computerAddCompleted.connect(addComplete)
    StackView.onDeactivating: ComputerManager.computerAddCompleted.disconnect(addComplete)

    function createModel() {
        const created = Qt.createQmlObject('import ComputerModel 1.0; ComputerModel {}', pcGrid)
        created.initialize(ComputerManager)
        created.pairingCompleted.connect(function(error) {
            pairing.close()
            if (error) showError(error)
        })
        return created
    }

    function addComplete(success, blocked) {
        if (!success) showError(blocked ? "无法连接此地址。请检查网络、防火墙与 Sunshine 服务。"
                                        : "无法连接此地址。请确认主机在线且地址正确。")
    }

    function showError(message) {
        deviceError.text = message
        deviceError.open()
    }

    function openDevice(id, action) {
        const device = computerModel.deviceInfo(id)
        if (device.index === undefined) return
        if (MslinkTunnel.hasDeviceRoute(device.endpoint) &&
                (device.endpoint !== MslinkTunnel.localServiceEndpoint || !MslinkTunnel.tunnelUp)) {
            window.openSavedDevice(device.endpoint, device.name, action)
            return
        }
        if (!device.online || !device.supported) {
            showError(device.online ? "这台设备的串流服务版本无法连接。" : "设备离线。请确认主机和 Sunshine 服务已启动。")
            return
        }
        if (!device.paired) {
            const pin = computerModel.generatePinString()
            pairing.text = "在「" + device.name + "」的 Sunshine 管理页面输入 PIN " + pin + "。"
            pairing.open()
            computerModel.pairComputer(device.index, pin)
            return
        }
        if (action === "test") {
            if (device.busy) { showError("被控端正在使用。请先结束远程应用，再检测连通性。"); return }
            testConfirm.deviceId = id
            testConfirm.deviceName = device.name
            testConfirm.open()
            return
        }
        const component = Qt.createComponent("AppView.qml")
        stackView.push(component.createObject(stackView, {"computerIndex": device.index, "objectName": device.name}))
    }

    function startTest(id, name) {
        const device = computerModel.deviceInfo(id)
        if (device.index === undefined) return
        const session = computerModel.createDiagnosticSession(device.index)
        if (!session) {
            showError("无法启动检测。请确认设备在线、空闲，且被控端应用列表中有 Desktop 或桌面。刚配对时请等待应用列表刷新。")
            return
        }
        const component = Qt.createComponent("StreamSegue.qml")
        stackView.push(component.createObject(stackView, {"session": session, "appName": name,
                                                            "diagnostic": true, "stageText": "正在启动连通性检测…"}))
    }

    Column {
        anchors.centerIn: parent
        spacing: 12
        visible: pcGrid.count === 0
        Label { anchors.horizontalCenter: parent.horizontalCenter; text: "还没有设备"; color: theme.text; font.pixelSize: 21 }
        BrandButton { anchors.horizontalCenter: parent.horizontalCenter; accent: true; label: "添加设备"; onClicked: window.importLink() }
    }

    delegate: NavigableItemDelegate {
        id: card
        width: pcGrid.cellWidth - 16
        height: 204
        grid: pcGrid
        padding: 16
        readonly property bool saved: MslinkTunnel.hasDeviceRoute(model.endpoint)
        readonly property bool selectedRoute: saved && MslinkTunnel.localServiceEndpoint === model.endpoint && MslinkTunnel.tunnelUp
        readonly property bool available: saved ? selectedRoute && model.online : model.online
        Accessible.name: model.name + "，" + stateLabel.text
        background: Rectangle {
            radius: 14
            color: card.hovered ? theme.surfaceHover : theme.surface
            border.color: card.activeFocus ? theme.primary : theme.border
        }

        Image { x: 18; y: 16; width: 42; height: 42; source: "qrc:/res/brand.svg" }
        Text {
            id: stateLabel
            anchors.right: parent.right
            anchors.rightMargin: 16
            y: 26
            color: card.available ? theme.success : theme.textSecondary
            text: card.available ? (model.paired ? (card.selectedRoute ? "已连接" : "在线") : "待配对")
                                : card.saved ? "未连接" : model.statusUnknown ? "检查中" : "离线"
            font.pixelSize: 12
        }
        Text {
            x: 18; y: 76; width: parent.width - 36
            text: model.name || "未命名设备"
            textFormat: Text.PlainText
            color: theme.text
            font.pixelSize: 23
            font.bold: true
            elide: Text.ElideRight
        }
        Text {
            x: 18; y: 112; width: parent.width - 36
            text: card.available && model.paired ? "桌面与应用" : card.saved ? "已保存连接配置" : "局域网设备"
            color: theme.textSecondary
            font.pixelSize: 12
        }
        RowLayout {
            anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
            anchors.margins: 16
            BrandButton {
                label: "连接"
                accent: card.available
                enabled: card.saved || model.online
                onClicked: pcGrid.openDevice(model.deviceId, "connect")
            }
            Item { Layout.fillWidth: true }
            BrandButton {
                label: "更多  ···"
                Accessible.name: "设备操作"
                onClicked: deviceMenu.open()
            }
        }
        NavigableMenu {
            id: deviceMenu
            initiator: card
            x: Math.max(0, card.width - width - 16)
            y: card.height - 48
            width: 172
            palette.text: theme.text
            palette.buttonText: theme.text
            palette.highlight: theme.surfaceHover
            palette.highlightedText: theme.text
            background: Rectangle { radius: 10; color: theme.surfaceAlt; border.color: theme.border }
            NavigableMenuItem { text: "连接"; enabled: card.saved || model.online; onTriggered: pcGrid.openDevice(model.deviceId, "connect") }
            NavigableMenuItem {
                text: "修改名称"
                onTriggered: {
                    renameDevice.deviceId = model.deviceId
                    nameField.text = model.name
                    renameDevice.open()
                }
            }
            NavigableMenuItem { text: "检测连通性"; enabled: card.saved || model.online; onTriggered: pcGrid.openDevice(model.deviceId, "test") }
            NavigableMenuItem { text: "唤醒设备"; visible: !card.saved && !model.online && model.wakeable; onTriggered: computerModel.wakeComputer(index) }
            MenuSeparator {}
            NavigableMenuItem {
                text: "移除设备"
                onTriggered: {
                    removeDevice.deviceId = model.deviceId
                    removeDevice.text = "移除「" + model.name + "」及其在本机保存的连接配置？"
                    removeDevice.open()
                }
            }
        }
        onClicked: if (saved || model.online) pcGrid.openDevice(model.deviceId, "connect"); else deviceMenu.open()
        onPressAndHold: deviceMenu.open()
        Keys.onMenuPressed: deviceMenu.open()

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.RightButton
            onClicked: deviceMenu.popup()
        }
    }

    NavigableMessageDialog {
        id: deviceError
        title: "设备连接"
        standardButtons: Dialog.Ok
    }
    NavigableMessageDialog {
        id: pairing
        title: "设备配对"
        standardButtons: Dialog.Ok
    }
    NavigableMessageDialog {
        id: removeDevice
        title: "移除设备"
        property string deviceId
        standardButtons: Dialog.Yes | Dialog.No
        onAccepted: computerModel.removeDevice(deviceId)
    }
    NavigableDialog {
        id: renameDevice
        title: "修改名称"
        property string deviceId
        standardButtons: Dialog.Ok | Dialog.Cancel
        onOpened: { nameField.forceActiveFocus(); nameField.selectAll() }
        onAccepted: computerModel.renameDevice(deviceId, nameField.text)
        TextField {
            id: nameField
            width: 260
            maximumLength: 64
            Accessible.name: "设备名称"
            onAccepted: renameDevice.accept()
        }
    }
    NavigableMessageDialog {
        id: testConfirm
        title: "检测连通性"
        property string deviceId
        property string deviceName
        text: "将短暂启动「" + deviceName + "」的桌面串流，测量约 12 秒的实际画面与音频，结束后生成远程游戏评分和报告。"
        standardButtons: Dialog.Ok | Dialog.Cancel
        onAccepted: pcGrid.startTest(deviceId, deviceName)
    }

    ScrollBar.vertical: ScrollBar {}
}
