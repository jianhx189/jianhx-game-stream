import QtQuick
import QtQuick.Layouts

Rectangle {
    property string text: "等待连接"
    property color tone: theme.textTertiary
    Theme { id: theme }
    implicitWidth: row.implicitWidth + 20
    implicitHeight: 28
    radius: 14
    color: Qt.rgba(tone.r, tone.g, tone.b, 0.10)
    border.color: Qt.rgba(tone.r, tone.g, tone.b, 0.35)
    Row {
        id: row
        anchors.centerIn: parent
        spacing: 7
        Rectangle { width: 6; height: 6; radius: 3; color: tone; anchors.verticalCenter: parent.verticalCenter }
        Text { text: parent.parent.text; color: tone; font.pixelSize: 11; anchors.verticalCenter: parent.verticalCenter }
    }
}
