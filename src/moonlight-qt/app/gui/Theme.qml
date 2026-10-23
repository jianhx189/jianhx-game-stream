import QtQuick

// =============================================================================
//  Theme.qml —— MiniStream 深色主题令牌
//  所有界面配色都从这里取，避免各处散落硬编码颜色。
// =============================================================================
QtObject {
    // ---- 底色 ----
    readonly property color canvas:        "#101827"   // 窗口最底层
    readonly property color toolbar:       "#131f32"   // 工具栏
    readonly property color surface:       "#19273c"   // 卡片 / 弹窗
    readonly property color surfaceAlt:    "#22324b"   // 输入框 / 次级块
    readonly property color surfaceHover:  "#2a3e5d"   // 悬停态

    // ---- 边框 ----
    readonly property color border:        "#344863"
    readonly property color borderSoft:    "#22324b"
    readonly property color borderStrong:  "#587599"

    // ---- 文字 ----
    readonly property color text:          "#edf2fb"
    readonly property color textSecondary: "#afbed4"
    readonly property color textTertiary:  "#7e92af"

    // ---- 强调 / 语义色 ----
    readonly property color primary:       "#88a6ff"
    readonly property color success:       "#54d8b4"
    readonly property color warning:       "#f0b429"
    readonly property color danger:        "#f87171"
    readonly property color dangerSoft:    "#3a2226"

    // ---- 圆角 ----
    readonly property int radiusCard:      10
    readonly property int radiusControl:   8

    // ---- 字体 ----
    readonly property string fontFamily:   "Microsoft YaHei UI"
}
