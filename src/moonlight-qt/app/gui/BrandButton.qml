import QtQuick
import QtQuick.Controls.Basic

// =============================================================================
//  BrandButton.qml —— MiniStream 深色工具栏按钮
//  支持「图标 + 文字」两种呈现：只给 iconSource 就是纯图标按钮，
//  同时给 label 就是图标+文字；都没给则显示纯文字按钮。
// =============================================================================
Button {
    id: control

    property string label: ""
    property string iconSource: ""
    property int fontSize: 13
    // 主按钮（强调色填充）
    property bool accent: false

    Theme { id: theme }

    implicitHeight: 32
    implicitWidth: label.length > 0 ? (contentRow.implicitWidth + 20)
                                    : (iconSource.length > 0 ? 34 : 80)
    padding: 0
    hoverEnabled: true
    focusPolicy: Qt.TabFocus

    contentItem: Row {
        id: contentRow
        spacing: 6
        anchors.centerIn: parent

        Image {
            id: icon
            visible: control.iconSource.length > 0
            source: control.iconSource
            sourceSize.width: 16
            sourceSize.height: 16
            width: 16
            height: 16
            anchors.verticalCenter: parent.verticalCenter
            opacity: control.enabled ? 0.95 : 0.4
        }

        Text {
            id: labelText
            visible: control.label.length > 0
            text: control.label
            color: control.accent ? "#ffffff"
                                  : (control.enabled ? theme.text : theme.textTertiary)
            font.pixelSize: control.fontSize
            font.family: theme.fontFamily
            anchors.verticalCenter: parent.verticalCenter
        }
    }

    background: Rectangle {
        radius: theme.radiusControl
        color: {
            if (!control.enabled) return theme.surfaceAlt
            if (control.accent) {
                if (control.down) return Qt.darker(theme.primary, 1.25)
                if (control.hovered) return Qt.lighter(theme.primary, 1.1)
                return theme.primary
            }
            if (control.down) return theme.surfaceAlt
            if (control.hovered) return theme.surfaceHover
            return "transparent"
        }
        border.width: control.accent ? 0 : 1
        border.color: control.hovered ? theme.borderStrong : theme.border
    }

    ToolTip.visible: false
}
