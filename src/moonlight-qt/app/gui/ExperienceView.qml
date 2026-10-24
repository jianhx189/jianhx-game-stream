import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import MslinkTunnel 1.0

Flickable {
    id: page
    objectName: "体验监测"
    property var live: MslinkTunnel.telemetry
    property var media: live.media || ({})
    property var history: MslinkTunnel.history
    property var events: MslinkTunnel.events
    property int windowSeconds: 60
    property string exportResult: ""
    property bool hasMedia: media.received_fps > 0 && media.fps > 0
    Theme { id: theme }
    contentWidth: width
    contentHeight: content.height + 32
    boundsBehavior: Flickable.StopAtBounds
    clip: true
    ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

    function metric(value, suffix, digits) {
        return typeof value === "number" && isFinite(value) ? value.toFixed(digits) + suffix : "—"
    }
    function mediaMetric(value, suffix, digits) {
        return hasMedia ? metric(value, suffix, digits) : "—"
    }
    function transport() {
        if (live.transport === "udp") return "UDP · 低延迟通道"
        if (live.transport === "tcp_fallback") return "TCP · 兼容通道"
        return live.tunnel_up ? "正在探测 UDP" : "等待隧道连接"
    }
    function advice() {
        if (!live.tunnel_up) return "尚未建立认证会话。先在连接配置中确认地址、端口和口令。"
        if (live.transport === "tcp_fallback") return "UDP 探测尚未确认，实时数据正在走 TCP 兼容通道。检查 UDP 转发与回包源地址。"
        if (live.queued_bytes > 1048576) return "发送队列已偏高，可能存在带宽不足或突发拥塞。观察吞吐和重连变化。"
        if (hasMedia && media.network_frame_drop_percent > 2) return "媒体网络丢帧偏高。优先检查链路质量，不等同于物理 UDP 丢包率。"
        if (hasMedia && media.queue_ms > 30) return "解码不是主要瓶颈，帧排队耗时偏高。检查主控端呈现节奏与显示刷新率。"
        if (hasMedia && media.decode_ms > 16) return "解码耗时偏高。检查硬件解码器是否真正启用，以及主控端 GPU 负载。"
        return "当前采样未触发提示阈值。指标仅用于定位问题，不代表完整体验评分。"
    }

    ColumnLayout {
        id: content
        width: page.width - 40
        x: 20; y: 20
        spacing: 16

        RowLayout {
            Layout.fillWidth: true
            ColumnLayout {
                spacing: 5
                Text { text: "EXPERIENCE / 体验监测"; color: theme.primary; font.pixelSize: 10; font.letterSpacing: 1.7 }
                Text { text: "连接质量，逐层看清。"; color: theme.text; font.pixelSize: 26; font.weight: Font.DemiBold }
                Text { text: "真实隧道与媒体采样 · 1 秒刷新 · 最近 300 点仅保存在内存"; color: theme.textTertiary; font.pixelSize: 11 }
            }
            Item { Layout.fillWidth: true }
            SignalBadge { text: page.transport(); tone: live.transport === "udp" ? theme.success : live.tunnel_up ? theme.warning : theme.textTertiary }
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: insightRow.implicitHeight + 28
            radius: theme.radiusCard
            color: Qt.rgba(theme.primary.r, theme.primary.g, theme.primary.b, 0.06)
            border.color: Qt.rgba(theme.primary.r, theme.primary.g, theme.primary.b, 0.25)
            RowLayout {
                id: insightRow
                anchors.fill: parent; anchors.margins: 14; spacing: 12
                Text { text: "i"; color: theme.primary; font.pixelSize: 20; font.family: "Georgia"; Layout.alignment: Qt.AlignTop }
                Text { Layout.fillWidth: true; text: page.advice(); color: theme.textSecondary; font.pixelSize: 11; wrapMode: Text.Wrap }
            }
        }

        GridLayout {
            Layout.fillWidth: true
            columns: page.width < 720 ? 2 : 4
            columnSpacing: 12; rowSpacing: 12
            MetricCard { title: "隧道 RTT / 抖动"; value: page.metric(live.rtt_ms, " ms", 0); detail: "抖动 " + page.metric(live.rtt_jitter_ms, " ms", 0) + " · 非端到端操作延迟"; tone: theme.primary; compact: true }
            MetricCard { title: "实际渲染帧率"; value: page.mediaMetric(media.fps, " FPS", 1); detail: "接收 " + page.mediaMetric(media.received_fps, " FPS", 1) + " · 媒体过期显示 —"; tone: theme.accent; compact: true }
            MetricCard { title: "视频码率"; value: page.mediaMetric(media.video_mbps, " Mbps", 1); detail: "Sunshine 编码媒体，不是重压缩"; tone: theme.success; compact: true }
            MetricCard { title: "发送队列"; value: live.queued_bytes === undefined ? "—" : MslinkTunnel.formatBytes(live.queued_bytes); detail: "上限 4 MiB · 重连 " + (live.reconnects || 0) + " 次"; tone: live.queued_bytes > 1048576 ? theme.warning : theme.primary; compact: true }
        }

        RowLayout {
            Layout.fillWidth: true
            Text { text: "SIGNALS / 实时趋势"; color: theme.textTertiary; font.pixelSize: 10; font.letterSpacing: 1.2 }
            Item { Layout.fillWidth: true }
            BrandButton { label: "60 秒"; accent: page.windowSeconds === 60; onClicked: page.windowSeconds = 60 }
            BrandButton { label: "300 秒"; accent: page.windowSeconds === 300; onClicked: page.windowSeconds = 300 }
        }
        GridLayout {
            Layout.fillWidth: true; columns: page.width < 960 ? 1 : 3; columnSpacing: 12
            TrendChart { title: "隧道 RTT"; unit: "ms"; history: page.history; valueKey: "rtt_ms"; windowSeconds: page.windowSeconds; lineColor: theme.primary }
            TrendChart { title: "下行吞吐"; unit: "Mbps"; history: page.history; valueKey: "udp_bytes_down"; cumulative: true; rate: true; windowSeconds: page.windowSeconds; lineColor: theme.success; emptyText: "真实 UDP 媒体到达后显示" }
            TrendChart { title: "发送队列"; unit: "KiB"; history: page.history; valueKey: "queued_kib"; windowSeconds: page.windowSeconds; lineColor: theme.accent }
        }

        GridLayout {
            Layout.fillWidth: true; columns: page.width < 900 ? 1 : 2; columnSpacing: 14; rowSpacing: 14
            Rectangle {
                Layout.fillWidth: true; implicitHeight: videoGrid.implicitHeight + 38; radius: theme.radiusCard; color: theme.surface; border.color: theme.borderSoft
                ColumnLayout {
                    id: videoGrid
                    anchors.fill: parent; anchors.margins: 18; spacing: 12
                    Text { text: "视频体验 / 来自主控端实际媒体窗口"; color: theme.text; font.pixelSize: 13; font.weight: Font.DemiBold }
                    GridLayout {
                        Layout.fillWidth: true; columns: 2; columnSpacing: 18; rowSpacing: 11
                        Repeater {
                            model: [
                                {name:"网络丢帧", value:page.mediaMetric(media.network_frame_drop_percent," %",2)},
                                {name:"呈现丢帧", value:page.mediaMetric(media.pacing_frame_drop_percent," %",2)},
                                {name:"分辨率", value:hasMedia && media.width ? media.width + " × " + media.height : "—"},
                                {name:"媒体流 RTT", value:page.mediaMetric(media.stream_rtt_ms," ms",0)}
                            ]
                            delegate: ColumnLayout {
                                Layout.fillWidth: true
                                Text { text: modelData.name; color: theme.textTertiary; font.pixelSize: 10 }
                                Text { text: modelData.value; color: theme.text; font.pixelSize: 19; font.family: "Segoe UI" }
                            }
                        }
                    }
                }
            }
            Rectangle {
                Layout.fillWidth: true; implicitHeight: timingGrid.implicitHeight + 38; radius: theme.radiusCard; color: theme.surface; border.color: theme.borderSoft
                ColumnLayout {
                    id: timingGrid
                    anchors.fill: parent; anchors.margins: 18; spacing: 12
                    Text { text: "客户端耗时 / 分环节定位"; color: theme.text; font.pixelSize: 13; font.weight: Font.DemiBold }
                    GridLayout {
                        Layout.fillWidth: true; columns: 2; columnSpacing: 18; rowSpacing: 11
                        Repeater {
                            model: [
                                {name:"解码", value:page.mediaMetric(media.decode_ms," ms",2)},
                                {name:"帧排队", value:page.mediaMetric(media.queue_ms," ms",2)},
                                {name:"渲染", value:page.mediaMetric(media.render_ms," ms",2)},
                                {name:"主机处理", value:page.mediaMetric(media.host_processing_ms," ms",2)}
                            ]
                            delegate: ColumnLayout {
                                Layout.fillWidth: true
                                Text { text: modelData.name; color: theme.textTertiary; font.pixelSize: 10 }
                                Text { text: modelData.value; color: theme.text; font.pixelSize: 19; font.family: "Segoe UI" }
                            }
                        }
                    }
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true; implicitHeight: networkContent.implicitHeight + 36; radius: theme.radiusCard; color: theme.surface; border.color: theme.borderSoft
            GridLayout {
                id: networkContent
                anchors.fill: parent; anchors.margins: 18; columns: page.width < 720 ? 2 : 4; columnSpacing: 16; rowSpacing: 13
                Repeater {
                    model: [
                        {name:"活动 TCP / UDP 流", value:(live.active_tcp_streams || 0) + " / " + (live.active_udp_flows || 0)},
                        {name:"本地丢弃", value:live.udp_local_dropped === undefined ? "—" : String(live.udp_local_dropped)},
                        {name:"UDP 探测 / 应答", value:(live.udp_probe_sent || 0) + " / " + (live.udp_probe_ack || 0)},
                        {name:"认证拒绝 / 回包错误", value:(live.udp_auth_rejected || 0) + " / " + (live.udp_reply_errors || 0)}
                    ]
                    delegate: ColumnLayout {
                        Layout.fillWidth: true
                        Text { text: modelData.name; color: theme.textTertiary; font.pixelSize: 10 }
                        Text { text: modelData.value; color: theme.text; font.pixelSize: 19; font.family: "Segoe UI" }
                    }
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true; implicitHeight: eventContent.implicitHeight + 34; radius: theme.radiusCard; color: theme.surface; border.color: theme.borderSoft
            ColumnLayout {
                id: eventContent
                anchors.fill: parent; anchors.margins: 17; spacing: 10
                Text { text: "EVENTS / 连接事件"; color: theme.textSecondary; font.pixelSize: 11; font.letterSpacing: 1 }
                Text { visible: page.events.length === 0; text: "等待真实连接事件。事件只保存在内存，最多 100 条。"; color: theme.textTertiary; font.pixelSize: 11 }
                Repeater {
                    model: page.events.slice(Math.max(0, page.events.length - 6)).reverse()
                    delegate: RowLayout {
                        Layout.fillWidth: true; spacing: 12
                        Rectangle { width: 6; height: 6; radius: 3; color: modelData.level === "warning" ? theme.warning : modelData.level === "error" ? theme.danger : theme.primary; Layout.alignment: Qt.AlignTop; Layout.topMargin: 5 }
                        Text { text: Qt.formatDateTime(new Date(modelData.time_unix_ms), "HH:mm:ss"); color: theme.textTertiary; font.pixelSize: 10; font.family: "Cascadia Mono" }
                        Text { text: modelData.message; color: theme.textSecondary; font.pixelSize: 11; wrapMode: Text.Wrap; Layout.fillWidth: true }
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Text { Layout.fillWidth: true; text: "RTT 不是端到端操作延迟；媒体丢帧不是 IP 丢包率。媒体采样停止 5 秒后失效。"; color: theme.textTertiary; font.pixelSize: 10; wrapMode: Text.Wrap }
            BrandButton { label: "导出脱敏诊断"; accent: true; onClicked: page.exportResult = MslinkTunnel.exportDiagnostics() }
        }
        Text { visible: page.exportResult.length > 0; text: page.exportResult; color: theme.textTertiary; font.pixelSize: 10; wrapMode: Text.WrapAnywhere; Layout.fillWidth: true }
    }
}
