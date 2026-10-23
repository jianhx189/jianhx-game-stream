import QtQuick 2.9
import QtQuick.Controls.Basic
import QtQuick.Layouts 1.3
import MslinkTunnel 1.0
import ComputerManager 1.0

// =============================================================================
//  MslinkSetupDialog.qml —— 首次使用 / 更换被控端时的分步连接向导
//
//  第 1 步：选择连接方式（局域网直连 / FRP 内网穿透 / 帮我选）
//  第 2 步：按选择显示表单（局域网只填 IP；FRP 填公网地址 + TCP/UDP 端口；
//           共同填 隧道口令 + Sunshine 管理密码）
//  第 3 步：测试连接 —— 起隧道 → 探测 Sunshine → 未配对则自动配对 → 显示结果
//  连接成功后自动写 mslink.ini，下次启动自动连接。
// =============================================================================
Dialog {
    id: setupDialog

    property bool autoConnect: false
    property bool hostAddedOnce: false
    property bool everConnected: false
    property string failMessage: ""
    property int currentStep: 0        // 0=选择方式 1=填表单 2=测试连接
    property int connMode: -1          // 0=局域网 1=FRP（-1=未选）

    modal: true
    closePolicy: Popup.NoAutoClose
    title: qsTr("连接到被控端 · 第 %1/3 步").arg(currentStep + 1)
    width: Math.min(560, parent.width - 40)
    x: Math.max(20, (parent.width - width) / 2)
    y: Math.max(20, (parent.height - height) / 2)
    padding: 22

    Theme { id: theme }

    // Dialog 的弹层窗口不继承 ApplicationWindow 的 palette，
    // 必须显式覆盖，否则控件按系统浅灰主题渲染。
    background: Rectangle {
        color: theme.surface
        radius: theme.radiusCard
        border.width: 1
        border.color: theme.border
    }
    palette.window: theme.surface
    palette.windowText: theme.text
    palette.base: theme.surfaceAlt
    palette.text: theme.text
    palette.button: theme.surfaceAlt
    palette.buttonText: theme.text
    palette.highlight: theme.primary
    palette.highlightedText: "#ffffff"

    function openWithError(msg) {
        failMessage = msg ? msg : ""
        open()
    }

    // P1-1: 隧道模式下，Moonlight 应连接本地监听地址（127.0.0.1），
    // 而不是用户输入的公网地址。否则会绕过隧道直接探测公网。
    // 直连模式（无隧道）才使用远端地址。
    function addHostIfNeeded() {
        if (hostAddedOnce) { return }
        hostAddedOnce = true
        everConnected = true
        // 隧道模式：添加本地入口地址
        var hostToAdd = MslinkTunnel.localServiceEndpoint
        console.log("[MiniStream] 添加主机到列表: " + hostToAdd + "（隧道本地入口）")
        ComputerManager.addNewHostManually(hostToAdd)
    }

    // 尝试连接。注意：返回空串只代表「本机监听已就绪」，
    // 远端是否真的通了由 tunnelUp 属性在后续 1 秒内判定。
    function doConnect() {
        failMessage = ""
        // 先把自动配对账号存下来（空密码=不用自动配对，退回手动输 PIN）
        MslinkTunnel.setSunshineCredentials(sunUserField.text, sunPassField.text)
        var err = MslinkTunnel.connectToServer(serverField.text, parseInt(tcpField.text),
                                               parseInt(udpField.text), passwordField.text)
        if (err !== "") {
            failMessage = err
            return
        }
        currentStep = 2     // 进入测试连接页，等 tunnelUp / lastError
    }

    // 隧道状态变化：只有 tunnelUp 才是真的连上；失败/超时/掉线都要留在向导里
    Connections {
        target: MslinkTunnel
        function onStateChanged() {
            if (MslinkTunnel.tunnelUp) {
                failMessage = ""
                setupDialog.addHostIfNeeded()
                if (setupDialog.opened && setupDialog.currentStep === 2) {
                    testResultText.text = qsTr("连接成功！被控端 Sunshine 在线。\n关闭本窗口后，双击主机列表里的电脑即可开始串流。") +
                        (MslinkTunnel.autoPairReady ? qsTr("\n已填 Sunshine 管理密码：若主机显示「未配对」，点一下主机会自动完成配对。")
                                                    : qsTr("\n未填 Sunshine 管理密码：首次连接会显示 4 位 PIN，需到被控端 https://localhost:47990 输入。"))
                    testResultText.color = "#35d39a"
                    finishBtn.visible = true
                } else {
                    setupDialog.close()
                }
                return
            }
            if (MslinkTunnel.connecting) {
                return          // 还在连，界面上显示"正在连接"
            }
            if (MslinkTunnel.lastError !== "") {
                failMessage = MslinkTunnel.lastError
                if (setupDialog.currentStep === 2) {
                    testResultText.text = qsTr("连接失败：") + MslinkTunnel.lastError +
                        qsTr("\n\n排查：\n· 网络不通——被控端是否开机、FRP 转发是否生效\n· 口令不一致——核对被控端安装时显示的隧道口令\n· 端口不对——局域网填被控端 IP 和 14738/14739；FRP 填公网地址和公网端口")
                    testResultText.color = "#f87171"
                    finishBtn.visible = false
                } else {
                    setupDialog.open()
                }
            }
            else if (everConnected) {
                failMessage = qsTr("隧道已断开，请检查被控端是否在线、口令是否一致")
                setupDialog.open()
            }
        }
    }

    // 手动重新打开设置：Ctrl+T
    Shortcut {
        sequence: "Ctrl+T"
        onActivated: setupDialog.open()
    }

    Component.onCompleted: {
        serverField.text = MslinkTunnel.server
        tcpField.text = MslinkTunnel.tcpPort > 0 ? String(MslinkTunnel.tcpPort) : "14738"
        udpField.text = MslinkTunnel.udpPort > 0 ? String(MslinkTunnel.udpPort) : "14739"
        passwordField.text = MslinkTunnel.password
        sunUserField.text = MslinkTunnel.sunshineUser
        sunPassField.text = MslinkTunnel.sunshinePassword

        // 已配置过就静默自动连接，用户完全不用看见这个框
        if (autoConnect && MslinkTunnel.configured) {
            if (MslinkTunnel.tunnelUp) {
                // 关键：预设 mslink.ini 时隧道在 QML 加载之前就已经连上了，
                // 这时候不会再触发 onStateChanged，必须在这里直接补主机，
                // 否则用户看到一个空的主机列表，以为没连上。
                addHostIfNeeded()
            }
            else if (!MslinkTunnel.running) {
                doConnect()
                close()         // 静默重连，不弹向导
            }
            // 已经在连接中：等 Connections 里的 onStateChanged 处理
        }
        // 全新安装（没有 mslink.ini）才把向导亮出来
        else if (autoConnect && !MslinkTunnel.configured) {
            open()
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 12

        // 进度指示
        RowLayout {
            Layout.fillWidth: true
            spacing: 6
            Repeater {
                model: [qsTr("选择方式"), qsTr("填写信息"), qsTr("测试连接")]
                Label {
                    text: (index + 1) + ". " + modelData
                    font.pixelSize: 11
                    color: index === setupDialog.currentStep ? "#7cc4ff"
                         : index < setupDialog.currentStep ? "#35d39a" : "#5a6577"
                }
            }
            Item { Layout.fillWidth: true }
        }

        StackLayout {
            id: pages
            Layout.fillWidth: true
            currentIndex: setupDialog.currentStep

            // ================= 第 1 步：选择连接方式 =================
            ColumnLayout {
                spacing: 10
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: "#9aa7ba"
                    font.pixelSize: 12
                    text: qsTr("两台电脑的网络关系是哪种？")
                }
                Button {
                    Layout.fillWidth: true
                    text: qsTr("局域网直连 —— 两台电脑接同一个路由器（最简单）")
                    onClicked: { setupDialog.connMode = 0; setupDialog.currentStep = 1 }
                }
                Button {
                    Layout.fillWidth: true
                    text: qsTr("FRP 内网穿透 —— 不同网络，我有 FRP 服务器")
                    onClicked: { setupDialog.connMode = 1; setupDialog.currentStep = 1 }
                }
                Button {
                    Layout.fillWidth: true
                    text: qsTr("我不知道 —— 帮我选")
                    onClicked: {
                        unsureBox.visible = true
                    }
                }
                Label {
                    id: unsureBox
                    Layout.fillWidth: true
                    visible: false
                    wrapMode: Text.WordWrap
                    color: "#f0b429"
                    font.pixelSize: 12
                    text: qsTr("判断方法：\n· 两台电脑插着同一个路由器 / 连同一个 Wi-Fi → 选「局域网直连」\n· 一台在家、一台在公司或学校 → 需要 FRP，选「FRP 内网穿透」\n\n被控端安装完成时显示的「配置指南」里有详细说明（含 FRP 配置示例）。")
                }
            }

            // ================= 第 2 步：填表单 =================
            ColumnLayout {
                spacing: 10

                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: "#9aa7ba"
                    font.pixelSize: 12
                    text: setupDialog.connMode === 0
                        ? qsTr("局域网直连：地址填被控端安装完成时显示的「本机局域网 IP」（如 192.168.1.x）。")
                        : qsTr("FRP 内网穿透：填 FRP 服务端的公网地址，以及转发到被控端的公网 TCP/UDP 端口。")
                }

                Label { text: setupDialog.connMode === 0 ? qsTr("被控端局域网 IP") : qsTr("FRP 公网地址"); font.pixelSize: 12; color: "#c3cfe0" }
                TextField {
                    id: serverField
                    Layout.fillWidth: true
                    placeholderText: setupDialog.connMode === 0 ? qsTr("例如 192.168.1.100") : qsTr("FRP 域名或 IP，例如 192.0.2.10")
                    selectByMouse: true
                    onAccepted: passwordField.forceActiveFocus()
                }

                // FRP 模式常显端口；局域网模式藏进高级
                RowLayout {
                    Layout.fillWidth: true
                    visible: setupDialog.connMode === 1 || advCheck.checked
                    spacing: 10
                    Label { text: qsTr("TCP"); font.pixelSize: 12 }
                    TextField {
                        id: tcpField
                        Layout.preferredWidth: 90
                        inputMethodHints: Qt.ImhDigitsOnly
                        validator: IntValidator { bottom: 1; top: 65535 }
                        selectByMouse: true
                    }
                    Label { text: qsTr("UDP"); font.pixelSize: 12 }
                    TextField {
                        id: udpField
                        Layout.preferredWidth: 90
                        inputMethodHints: Qt.ImhDigitsOnly
                        validator: IntValidator { bottom: 1; top: 65535 }
                        selectByMouse: true
                    }
                    Item { Layout.fillWidth: true }
                }

                RowLayout {
                    Layout.fillWidth: true
                    visible: setupDialog.connMode === 0
                    spacing: 8
                    CheckBox {
                        id: advCheck
                        text: qsTr("高级（改端口）")
                        font.pixelSize: 12
                    }
                    Item { Layout.fillWidth: true }
                }

                Label { text: qsTr("隧道口令"); font.pixelSize: 12; color: "#c3cfe0" }
                TextField {
                    id: passwordField
                    Layout.fillWidth: true
                    echoMode: TextInput.Password
                    placeholderText: qsTr("被控端安装完成时显示的「隧道口令」")
                    selectByMouse: true
                    onAccepted: sunPassField.forceActiveFocus()
                }

                // ---------- 自动配对：被控端 Sunshine 管理账号 ----------
                // 常显：填一次之后，主控端配对时自己 approve，用户不用再去被控端输 4 位 PIN。
                // 留空则退回原来「手动输 PIN」的流程。
                Label { text: qsTr("Sunshine 管理密码（自动配对，可留空）"); font.pixelSize: 12; color: "#c3cfe0" }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    TextField {
                        id: sunUserField
                        Layout.preferredWidth: 96
                        placeholderText: qsTr("账号 admin")
                        selectByMouse: true
                    }
                    TextField {
                        id: sunPassField
                        Layout.fillWidth: true
                        echoMode: TextInput.Password
                        placeholderText: qsTr("被控端安装时显示的那个密码")
                        selectByMouse: true
                        onAccepted: setupDialog.doConnect()
                    }
                }

                Label {
                    Layout.fillWidth: true
                    visible: failMessage !== ""
                    text: failMessage
                    color: "#f87171"
                    font.pixelSize: 12
                    wrapMode: Text.WordWrap
                }
            }

            // ================= 第 3 步：测试连接 =================
            ColumnLayout {
                spacing: 12
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: "#9aa7ba"
                    font.pixelSize: 12
                    text: qsTr("测试内容：① 启动 mslink 隧道 ② 探测被控端 Sunshine 是否在线 ③ 在线且未配对时，用第 2 步填的管理密码自动配对。")
                }
                Label {
                    Layout.fillWidth: true
                    visible: MslinkTunnel.connecting
                    text: qsTr("正在连接被控端…（最多等 12 秒）")
                    color: "#f0b429"
                    font.pixelSize: 13
                }
                Label {
                    id: testResultText
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    font.pixelSize: 13
                    color: "#9aa7ba"
                    text: MslinkTunnel.connecting ? "" : qsTr("点「测试连接」开始。")
                }

                // P2-6: 传输模式详细状态
                ColumnLayout {
                    Layout.fillWidth: true
                    visible: MslinkTunnel.tunnelUp
                    spacing: 6

                    Label {
                        Layout.fillWidth: true
                        font.pixelSize: 12
                        color: "#9aa7ba"
                        text: qsTr("连接状态详情：")
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        Label {
                            font.pixelSize: 12
                            color: "#35d39a"
                            text: qsTr("✓ TCP 隧道认证成功")
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        Label {
                            font.pixelSize: 12
                            color: MslinkTunnel.udpDirectOk ? "#35d39a" : "#f0b429"
                            text: MslinkTunnel.udpDirectOk
                                  ? qsTr("✓ UDP 双向可用（低延迟模式）")
                                  : MslinkTunnel.udpViaTcp
                                    ? qsTr("未收到有效 UDP 探测应答，暂用 TCP；尚不能确定是映射、过滤还是版本问题")
                                    : qsTr("… UDP 探测中")
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        visible: MslinkTunnel.udpViaTcp
                        Label {
                            font.pixelSize: 11
                            color: "#f0b429"
                            wrapMode: Text.WordWrap
                            text: qsTr("注意：TCP 兼容模式功能可用，但视频/控制/音频都走 TCP，弱网下可能有队头阻塞。")
                        }
                    }
                }

                Button {
                    id: finishBtn
                    visible: false
                    Layout.fillWidth: true
                    highlighted: true
                    text: qsTr("完成，去主机列表")
                    onClicked: setupDialog.close()
                }
            }
        }

        // ---------- 底部按钮 ----------
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Button {
                text: qsTr("上一步")
                font.pixelSize: 12
                visible: setupDialog.currentStep > 0 && !MslinkTunnel.connecting
                onClicked: setupDialog.currentStep = setupDialog.currentStep - 1
            }
            Item { Layout.fillWidth: true }
            Button {
                text: qsTr("稍后设置")
                font.pixelSize: 12
                onClicked: setupDialog.close()
            }
            Button {
                text: setupDialog.currentStep === 2 ? qsTr("重新测试") : qsTr("下一步：测试连接")
                font.pixelSize: 12
                highlighted: true
                visible: setupDialog.currentStep >= 1
                enabled: !MslinkTunnel.connecting && !MslinkTunnel.tunnelUp
                         && serverField.text.trim() !== ""
                onClicked: setupDialog.doConnect()
            }
        }
    }
}
