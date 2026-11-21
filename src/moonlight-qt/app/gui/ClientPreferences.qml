import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic
import StreamingPreferences 1.0

ScrollView {
    id: page
    objectName: "串流偏好"
    contentWidth: availableWidth
    clip: true
    property bool advanced: false
    contentHeight: options.implicitHeight + 32
    Theme { id: theme }
    readonly property var prefs: StreamingPreferences
    function setPref(key, value) { prefs[key] = value; prefs.save() }
    component Section: Label {
        Layout.topMargin: 12
        color: theme.primary; font.bold: true; font.pixelSize: 14
    }
    component SettingRow: RowLayout {
        Layout.fillWidth: true
        property string title
        Label { text: parent.title; Layout.fillWidth: true; color: theme.text; font.pixelSize: 14 }
    }
    component Option: ComboBox {
        id: choice
        Layout.preferredWidth: 208
        palette.button: theme.surfaceAlt
        background: Rectangle {
            radius: theme.radiusControl
            color: choice.hovered ? theme.surfaceHover : theme.surfaceAlt
            border.color: choice.activeFocus ? theme.primary : theme.border
        }
        indicator: Text {
            x: choice.width - width - 12
            y: (choice.height - height) / 2
            text: "⌄"
            color: theme.textSecondary
            font.pixelSize: 17
        }
    }
    component PreferenceSwitch: Switch {
        id: toggle
        indicator: Rectangle {
            implicitWidth: 38; implicitHeight: 21
            x: toggle.leftPadding; y: (toggle.height - height) / 2
            radius: 11
            color: toggle.checked ? theme.primary : theme.surfaceAlt
            border.color: toggle.activeFocus ? theme.text : theme.border
            Rectangle {
                width: 15; height: 15; radius: 8
                y: 3; x: toggle.checked ? 20 : 3
                color: toggle.checked ? theme.canvas : theme.textSecondary
                Behavior on x { NumberAnimation { duration: 145; easing.type: Easing.OutCubic } }
            }
        }
    }
    ColumnLayout {
        id: options
        x: 24; y: 16; width: Math.min(720, page.availableWidth - 48); spacing: 8
        Label { text: "串流偏好"; font.pixelSize: 26; font.bold: true; color: theme.text }
        Section { text: "画面与声音" }
        SettingRow {
            title: "分辨率"
            Option {
                readonly property string selected: page.prefs.width + " × " + page.prefs.height
                readonly property var presets: ["1280 × 720", "1920 × 1080", "2560 × 1440", "3840 × 2160"]
                model: presets.includes(selected) ? presets : presets.concat([selected])
                currentIndex: model.indexOf(selected)
                onActivated: {
                    const size = currentText.split(" × ")
                    page.prefs.width = Number(size[0]); page.prefs.height = Number(size[1]); page.prefs.save()
                }
            }
        }
        SettingRow {
            title: "帧率"
            Option {
                readonly property var presets: [30, 60, 90, 120, 144]
                model: presets.includes(page.prefs.fps) ? presets : presets.concat([page.prefs.fps])
                currentIndex: model.indexOf(page.prefs.fps)
                onActivated: page.setPref("fps", Number(currentText))
            }
        }
        SettingRow {
            title: "视频码率（Kbps）"
            SpinBox { editable: true; from: 500; to: Math.max(500000, page.prefs.bitrateKbps); stepSize: 500; value: page.prefs.bitrateKbps; onValueModified: page.setPref("bitrateKbps", value) }
        }
        SettingRow {
            title: "声道"
            Option { model: ["立体声", "5.1 环绕声", "7.1 环绕声"]; currentIndex: page.prefs.audioConfig; onActivated: page.setPref("audioConfig", currentIndex) }
        }
        SettingRow {
            title: "显示模式"
            Option { model: ["独占全屏", "无边框全屏", "窗口"]; currentIndex: page.prefs.windowMode; onActivated: page.setPref("windowMode", currentIndex) }
        }
        Section { text: "菜单与输入" }
        SettingRow {
            title: "串流菜单"
            PreferenceSwitch { checked: page.prefs.streamingMenuEnabled; onToggled: page.setPref("streamingMenuEnabled", checked) }
        }
        SettingRow {
            enabled: page.prefs.streamingMenuEnabled
            title: "显示侧边菜单按钮"
            PreferenceSwitch { checked: page.prefs.showMenuTab; onToggled: page.setPref("showMenuTab", checked) }
        }
        SettingRow {
            enabled: page.prefs.streamingMenuEnabled
            title: "菜单快捷键"
            ComboBox {
                id: modifier
                model: ["无修饰键", "Ctrl + Shift", "Ctrl + Alt"]
                readonly property var prefixes: ["", "Ctrl+Shift+", "Ctrl+Alt+"]
                currentIndex: Math.max(0, prefixes.indexOf(page.prefs.menuShortcut.replace(/F\d+$/, "")))
                onActivated: page.setPref("menuShortcut", prefixes[currentIndex] + functionKey.currentText)
            }
            ComboBox {
                id: functionKey
                model: ["F1","F2","F3","F4","F5","F6","F7","F8","F9","F10","F11","F12"]
                currentIndex: Math.max(0, model.indexOf(page.prefs.menuShortcut.split("+").pop()))
                onActivated: page.setPref("menuShortcut", modifier.prefixes[modifier.currentIndex] + currentText)
            }
        }
        SettingRow {
            title: "鼠标输入"
            Option { model: ["桌面 · 绝对坐标", "游戏 · 相对位移"]; currentIndex: page.prefs.absoluteMouseMode ? 0 : 1; onActivated: page.setPref("absoluteMouseMode", currentIndex === 0) }
        }
        SettingRow {
            title: "Win 键与系统快捷键"
            Option {
                model: ["保留在本机", "全屏时发给被控端", "串流聚焦时发给被控端"]
                currentIndex: page.prefs.captureSysKeysMode
                onActivated: page.setPref("captureSysKeysMode", currentIndex)
            }
        }
        Label { Layout.fillWidth: true; wrapMode: Text.Wrap; color: theme.textSecondary; text: "指针锁定时可按 " + page.prefs.menuShortcut + " 打开菜单。画面设置在下次串流时生效。" }
        BrandButton { label: page.advanced ? "收起高级选项" : "展开高级选项"; onClicked: page.advanced = !page.advanced }
        ColumnLayout {
            visible: page.advanced
            Layout.fillWidth: true
            SettingRow {
                title: "视频编码"
                Option {
                    model: ["自动", "H.264", "HEVC", "AV1"]
                    readonly property var codecs: [0, 1, 2, 4]
                    currentIndex: page.prefs.videoCodecConfig === 3 ? 2 : codecs.indexOf(page.prefs.videoCodecConfig)
                    onActivated: page.setPref("videoCodecConfig", codecs[currentIndex])
                }
            }
            SettingRow {
                title: "解码器"
                Option { model: ["自动", "硬件", "软件"]; currentIndex: page.prefs.videoDecoderSelection; onActivated: page.setPref("videoDecoderSelection", currentIndex) }
            }
            Repeater {
                model: [
                    ["enableVsync", "垂直同步"], ["framePacing", "帧节奏控制"], ["enableHdr", "HDR"],
                    ["enableYUV444", "YUV 4:4:4"], ["playAudioOnHost", "同时在被控端播放声音"],
                    ["muteOnFocusLoss", "后台静音"], ["showPerformanceOverlay", "默认显示性能统计"],
                    ["enableMdns", "发现局域网电脑"], ["reverseScrollDirection", "反向滚动"],
                    ["swapMouseButtons", "交换鼠标左右键"], ["backgroundGamepad", "后台接收手柄输入"]
                ]
                SettingRow {
                    required property var modelData
                    id: advancedRow
                    title: modelData[1]
                    PreferenceSwitch { checked: page.prefs[advancedRow.modelData[0]]; onToggled: page.setPref(advancedRow.modelData[0], checked) }
                }
            }
            BrandButton { label: "其他设备与平台选项"; onClicked: navigateTo("qrc:/gui/SettingsView.qml", SettingsView) }
        }
        Item { implicitHeight: 28 }
    }
}
