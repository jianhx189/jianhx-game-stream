import QtQuick

// =============================================================================
//  Theme.qml —— MiniStream 深色主题令牌
//  所有界面配色都从这里取，避免各处散落硬编码颜色。
// =============================================================================
QtObject {
    // ---- 底色 ----
    readonly property color canvas:        "#111c18"   // 窗口最底层
    readonly property color toolbar:       "#15231d"   // 工具栏
    readonly property color surface:       "#1c2c24"   // 卡片 / 弹窗
    readonly property color surfaceAlt:    "#263a30"   // 输入框 / 次级块
    readonly property color surfaceHover:  "#2b4236"   // 悬停态

    // ---- 边框 ----
    readonly property color border:        "#344b3e"
    readonly property color borderSoft:    "#263a30"
    readonly property color borderStrong:  "#53705d"

    // ---- 文字 ----
    readonly property color text:          "#e7efe8"
    readonly property color textSecondary: "#acbdb0"
    readonly property color textTertiary:  "#829788"

    // ---- 强调 / 语义色 ----
    readonly property color primary:       "#74be91"
    readonly property color success:       "#35d39a"
    readonly property color warning:       "#f0b429"
    readonly property color danger:        "#f87171"
    readonly property color dangerSoft:    "#3a2226"

    // ---- 圆角 ----
    readonly property int radiusCard:      10
    readonly property int radiusControl:   8

    // ---- 字体 ----
    readonly property string fontFamily:   "Microsoft YaHei UI"
}
