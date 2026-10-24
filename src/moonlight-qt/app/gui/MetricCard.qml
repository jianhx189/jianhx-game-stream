import QtQuick
import QtQuick.Layouts

Rectangle {
    property string title: ""
    property string value: "—"
    property string detail: ""
    property color tone: theme.primary
    property bool compact: false

    Theme { id: theme }
    radius: theme.radiusCard
    color: theme.surface
    border.width: 1
    border.color: theme.borderSoft
    implicitHeight: compact ? 76 : 108
    Layout.fillWidth: true
    Layout.minimumWidth: 0

    Rectangle {
        width: 3; height: parent.height - 28; radius: 2
        anchors.left: parent.left; anchors.leftMargin: 14
        anchors.verticalCenter: parent.verticalCenter
        color: tone
    }
    ColumnLayout {
        anchors.fill: parent
        anchors.leftMargin: 28; anchors.rightMargin: 14
        anchors.topMargin: compact ? 11 : 15; anchors.bottomMargin: compact ? 10 : 14
        spacing: compact ? 3 : 5
        Text { text: title; color: theme.textTertiary; font.pixelSize: 10; font.letterSpacing: 0.8; elide: Text.ElideRight; Layout.fillWidth: true }
        Text { text: value; color: theme.text; font.family: "Segoe UI"; font.pixelSize: compact ? 20 : 26; font.weight: Font.DemiBold; elide: Text.ElideRight; Layout.fillWidth: true }
        Text { visible: !compact && detail.length > 0; text: detail; color: theme.textTertiary; font.pixelSize: 10; elide: Text.ElideRight; Layout.fillWidth: true }
    }
}
