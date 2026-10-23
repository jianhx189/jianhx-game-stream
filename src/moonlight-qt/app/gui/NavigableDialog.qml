import QtQuick 2.0
import QtQuick.Controls.Basic

Dialog {
    modal: true
    anchors.centerIn: Overlay.overlay

    Theme { id: dialogTheme }

    // 深色主题：ApplicationWindow 的 palette 不会传进 Dialog 的弹层窗口，
    // 这里显式覆盖，否则原生风格弹窗是黑底（Basic 暗色猜错）或浅灰。
    background: Rectangle {
        color: dialogTheme.surface
        radius: dialogTheme.radiusCard
        border.width: 1
        border.color: dialogTheme.border
    }
    palette.window: dialogTheme.surface
    palette.windowText: dialogTheme.text
    palette.base: dialogTheme.surfaceAlt
    palette.text: dialogTheme.text
    palette.button: dialogTheme.surfaceAlt
    palette.buttonText: dialogTheme.text
    palette.highlight: dialogTheme.primary
    palette.highlightedText: "#ffffff"

    onClosed: {
        // We must force focus back to the last item. If we don't,
        // gamepad and keyboard navigation will break after a
        // dialog appears.
        stackView.forceActiveFocus()
    }
}
