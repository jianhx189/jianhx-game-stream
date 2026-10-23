import QtQuick

// =============================================================================
//  Theme.qml —— MiniStream 深色主题令牌
//  所有界面配色都从这里取，避免各处散落硬编码颜色。
// =============================================================================
QtObject {
    // ---- 底色 ----
    readonly property color canvas:        "#0f1218"   // 窗口最底层
    readonly property color toolbar:       "#151a23"   // 工具栏
    readonly property color surface:       "#1a1f2b"   // 卡片 / 弹窗
    readonly property color surfaceAlt:    "#232936"   // 输入框 / 次级块
    readonly property color surfaceHover:  "#2a3140"   // 悬停态

    // ---- 边框 ----
    readonly property color border:        "#2b3342"
    readonly property color borderSoft:    "#232a36"
    readonly property color borderStrong:  "#3d4757"

    // ---- 文字 ----
    readonly property color text:          "#dfe6f0"
    readonly property color textSecondary: "#9aa7ba"
    readonly property color textTertiary:  "#6b7789"

    // ---- 强调 / 语义色 ----
    readonly property color primary:       "#4a9eff"
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
