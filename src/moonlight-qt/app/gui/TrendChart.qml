import QtQuick
import QtQuick.Layouts

Rectangle {
    id: chart
    property string title: ""
    property string unit: ""
    property var samples: []
    property var history: []
    property string valueKey: "rtt_ms"
    property bool cumulative: false
    property bool rate: false
    property int windowSeconds: 60
    property color lineColor: theme.primary
    property string emptyText: "建立连接后开始记录真实采样"

    Theme { id: theme }
    implicitHeight: 190
    Layout.fillWidth: true
    radius: theme.radiusCard
    color: theme.surface
    border.width: 1
    border.color: theme.borderSoft

    function pointAt(i) {
        var rows = history.length ? history : samples
        if (!rows || !rows.length) return null
        var row = rows[i]
        if (!row || row.tunnel_up === false) return null
        if (valueKey === "queued_kib") return (row.queued_bytes || 0) / 1024
        if (cumulative && i > 0) {
            var previous = rows[i - 1]
            var elapsed = (row.time_unix_ms - previous.time_unix_ms) / 1000
            var delta = row[valueKey] - previous[valueKey]
            if (elapsed <= 0 || elapsed > 5 || delta < 0) return null
            return rate ? delta * 8 / elapsed / 1000000 : delta / elapsed
        }
        var value = row[valueKey]
        return typeof value === "number" && isFinite(value) ? value : null
    }
    function valueList() {
        var rows = history.length ? history : samples
        var result = []
        if (!rows) return result
        var start = Math.max(0, rows.length - windowSeconds)
        var newest = rows.length ? rows[rows.length - 1].time_unix_ms : 0
        for (var i = start; i < rows.length; ++i) {
            if (newest - rows[i].time_unix_ms > windowSeconds * 1000) continue
            var value = pointAt(i)
            if (value !== null) result.push(value)
        }
        return result
    }
    function maximum() {
        var values = valueList()
        var max = 1
        for (var i = 0; i < values.length; ++i) max = Math.max(max, values[i])
        return max * 1.12
    }
    function latest() {
        var values = valueList()
        if (!values.length) return "—"
        var value = values[values.length - 1]
        return (value >= 100 ? value.toFixed(0) : value.toFixed(value < 10 ? 1 : 0)) + " " + unit
    }

    ColumnLayout {
        anchors.fill: parent; anchors.margins: 16; spacing: 9
        RowLayout {
            Layout.fillWidth: true
            Text { text: title; color: theme.textSecondary; font.pixelSize: 11; font.weight: Font.DemiBold }
            Item { Layout.fillWidth: true }
            Text { text: latest(); color: lineColor; font.pixelSize: 12; font.family: "Segoe UI" }
        }
        Item {
            id: plot
            Layout.fillWidth: true; Layout.fillHeight: true; Layout.minimumHeight: 105
            Canvas {
                id: canvas
                anchors.fill: parent
                onPaint: {
                    var ctx = getContext("2d")
                    ctx.clearRect(0, 0, width, height)
                    ctx.strokeStyle = theme.borderSoft
                    ctx.lineWidth = 1
                    for (var row = 0; row < 4; ++row) {
                        var y = Math.round(row * height / 3) + 0.5
                        ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(width, y); ctx.stroke()
                    }
                    var rows = chart.history.length ? chart.history : chart.samples
                    if (!rows || !rows.length) return
                    var start = Math.max(0, rows.length - chart.windowSeconds)
                    var newest = rows[rows.length - 1].time_unix_ms
                    var max = chart.maximum()
                    var began = false
                    ctx.strokeStyle = chart.lineColor
                    ctx.lineWidth = 2
                    ctx.lineJoin = "round"
                    ctx.beginPath()
                    for (var i = start; i < rows.length; ++i) {
                        if (newest - rows[i].time_unix_ms > chart.windowSeconds * 1000) continue
                        var value = chart.pointAt(i)
                        if (value === null) { began = false; continue }
                        var x = (i - start) / Math.max(1, rows.length - start - 1) * width
                        var y = height - Math.min(1, value / max) * (height - 5) - 2
                        if (began) ctx.lineTo(x, y)
                        else { ctx.moveTo(x, y); began = true }
                    }
                    ctx.stroke()
                }
            }
            Text {
                visible: chart.valueList().length === 0
                anchors.centerIn: parent
                text: chart.emptyText
                color: theme.textTertiary
                font.pixelSize: 11
            }
            Text { anchors.left: parent.left; anchors.bottom: parent.bottom; text: "0"; color: theme.textTertiary; font.pixelSize: 9 }
            Text { anchors.right: parent.right; anchors.top: parent.top; text: chart.maximum().toFixed(chart.maximum() < 10 ? 1 : 0) + " " + unit; color: theme.textTertiary; font.pixelSize: 9 }
        }
        RowLayout {
            Layout.fillWidth: true
            Text { text: "最近 " + windowSeconds + " 秒 · 1 秒采样"; color: theme.textTertiary; font.pixelSize: 9 }
            Item { Layout.fillWidth: true }
            Text { text: "无数据时断线，不补演示曲线"; color: theme.textTertiary; font.pixelSize: 9 }
        }
    }
    onHistoryChanged: canvas.requestPaint()
    onSamplesChanged: canvas.requestPaint()
    onWindowSecondsChanged: canvas.requestPaint()
    onWidthChanged: canvas.requestPaint()
    onHeightChanged: canvas.requestPaint()
}
