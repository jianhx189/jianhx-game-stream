#include <QtWidgets>
#include <QtNetwork>
#include <windows.h>
#include <iphlpapi.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <cmath>
#include "ui_components.h"

static QString number(QJsonValue value, int precision = 1, const QString &suffix = {}) {
    return value.isDouble() && std::isfinite(value.toDouble())
        ? QString::number(value.toDouble(), 'f', precision) + suffix : QStringLiteral("—");
}

class Console final : public QMainWindow {
    QString root;
    QStackedWidget *pages = nullptr;
    QLabel *headline = nullptr, *subtitle = nullptr, *heroTitle = nullptr, *heroDetail = nullptr;
    QLabel *sessionCount = nullptr, *lastUpdate = nullptr, *quality = nullptr, *notice = nullptr;
    QLabel *serviceState = nullptr, *runtimeState = nullptr, *deviceDetail = nullptr;
    Ui::Metric *rtt = nullptr, *fps = nullptr, *traffic = nullptr, *drop = nullptr;
    Ui::Metric *decode = nullptr, *queue = nullptr, *render = nullptr, *hostProcessing = nullptr;
    Ui::Metric *jitter = nullptr, *tcpFlows = nullptr, *udpFlows = nullptr, *reconnects = nullptr;
    Ui::Metric *queued = nullptr, *drops = nullptr, *probes = nullptr, *errors = nullptr;
    Ui::Metric *receivedFps = nullptr, *bitrate = nullptr, *resolution = nullptr;
    Ui::Trend *sendTrend = nullptr, *receiveTrend = nullptr, *rttTrend = nullptr, *queueTrend = nullptr;
    QTableWidget *devices = nullptr, *eventsTable = nullptr;
    QPlainTextEdit *logs = nullptr;
    QComboBox *logKind = nullptr, *eventLevel = nullptr;
    QSpinBox *tcp = nullptr, *udp = nullptr, *timeout = nullptr;
    QPushButton *save = nullptr;
    QTimer timer;
    HANDLE actionHandle = nullptr;
    QString pending;
    QJsonObject previous, current;
    double sendMbps = std::numeric_limits<double>::quiet_NaN();
    double receiveMbps = std::numeric_limits<double>::quiet_NaN();
    bool identityLimited = false;
    qint64 previousHistoryMs = 0;

    QString cfg(const QString &file, const QString &key, const QString &fallback) {
        QFile f(root + "/" + file);
        if (!f.open(QIODevice::ReadOnly)) return fallback;
        for (auto line : QString::fromUtf8(f.readAll()).split('\n')) {
            line = line.trimmed();
            if (line.startsWith('#') || line.startsWith(';')) continue;
            const int index = line.indexOf('=');
            if (index > 0 && line.left(index).trimmed() == key) return line.mid(index + 1).trimmed();
        }
        return fallback;
    }

    bool listening(int port) {
        DWORD size = 0;
        GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0);
        QByteArray buffer(int(size), 0);
        if (GetExtendedTcpTable(buffer.data(), &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0) != NO_ERROR) return false;
        auto table = reinterpret_cast<PMIB_TCPTABLE_OWNER_PID>(buffer.data());
        for (DWORD i = 0; i < table->dwNumEntries; ++i)
            if (ntohs((u_short)table->table[i].dwLocalPort) == port) return true;
        return false;
    }

    bool processMatches(qint64 pid) {
        identityLimited = false;
        if (pid <= 0 || pid > MAXDWORD) return false;
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(pid));
        if (!process) {
            if (GetLastError() != ERROR_ACCESS_DENIED) return false;
            HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            if (snapshot == INVALID_HANDLE_VALUE) return false;
            PROCESSENTRY32W entry{};
            entry.dwSize = sizeof(entry);
            bool alive = false;
            if (Process32FirstW(snapshot, &entry)) do {
                if (entry.th32ProcessID == DWORD(pid) && QString::fromWCharArray(entry.szExeFile).compare("mslink.exe", Qt::CaseInsensitive) == 0) {
                    alive = true;
                    break;
                }
            } while (Process32NextW(snapshot, &entry));
            CloseHandle(snapshot);
            identityLimited = alive;
            return alive;
        }
        wchar_t path[32768];
        DWORD length = 32768;
        const bool ok = QueryFullProcessImageNameW(process, 0, path, &length);
        CloseHandle(process);
        return ok && QDir::cleanPath(QString::fromWCharArray(path, length)).compare(QDir::cleanPath(root + "/mslink.exe"), Qt::CaseInsensitive) == 0;
    }

    QString redact(QString text) {
        text.replace(QRegularExpression("(?im)^.*(?:password|sunshine_pass|token|authorization|private.key).*?$"), "[凭据行已隐藏]");
        text.replace(QRegularExpression("\\b(?:\\d{1,3}\\.){3}\\d{1,3}\\b"), "[IP]");
        return text;
    }

    QString logText() {
        QFile file(root + "/" + logKind->currentData().toString());
        if (!file.open(QIODevice::ReadOnly)) return "暂无日志。服务启动后会生成对应记录。";
        if (file.size() > 96000) file.seek(file.size() - 96000);
        return redact(QString::fromUtf8(file.readAll()));
    }

    QPushButton *button(const QString &text, std::function<void()> callback, const char *kind = nullptr) {
        auto *result = Ui::button(text, kind);
        connect(result, &QPushButton::clicked, this, callback);
        return result;
    }

    void action(const QString &verb, const QString &component) {
        if (actionHandle) { notice->setText("上一项管理操作尚未完成。"); return; }
        if (!QFile::exists(root + "/mslink.exe") || !QFile::exists(root + "/_internal/gui-action.ps1")) {
            notice->setText("请从完整安装包安装被控端，再运行管理操作。");
            return;
        }
        if (verb != "start" && QMessageBox::question(this, "MiniStream", verb == "save"
            ? "保存后需重启服务，并同步客户端、FRP 与防火墙端口。是否保存？"
            : "停止或重启会中断当前远程连接。是否继续？") != QMessageBox::Yes) return;
        QString args = "-NoProfile -ExecutionPolicy Bypass -File \"" + root + "/_internal/gui-action.ps1\" -Root \"" + root
            + "\" -Action " + verb + " -Component " + component + " -TcpPort " + QString::number(tcp->value())
            + " -UdpPort " + QString::number(udp->value()) + " -TimeoutMs " + QString::number(timeout->value());
        SHELLEXECUTEINFOW info{};
        info.cbSize = sizeof(info);
        info.fMask = SEE_MASK_NOCLOSEPROCESS;
        info.lpVerb = L"runas";
        info.lpFile = L"powershell.exe";
        info.lpParameters = reinterpret_cast<LPCWSTR>(args.utf16());
        info.nShow = SW_HIDE;
        if (!ShellExecuteExW(&info)) {
            notice->setText(GetLastError() == ERROR_CANCELLED ? "已取消管理员授权，未执行管理操作。" : "无法启动管理操作，请查看 Windows 权限。");
            return;
        }
        actionHandle = info.hProcess;
        pending = verb;
        save->setEnabled(false);
        notice->setText("正在执行管理操作，请等待完成并核对服务状态。");
    }

    void exportReport() {
        const auto path = QFileDialog::getSaveFileName(this, "导出诊断报告", QDir::homePath() + "/MiniStream-diagnostics.txt", "Text (*.txt)");
        if (path.isEmpty()) return;
        QJsonObject safe = current;
        safe.remove("device_name");
        safe.remove("peer_report_json");
        QSaveFile file(path);
        if (file.open(QIODevice::WriteOnly)) {
            file.write(("MiniStream | Powered by jianhx2026\n" + QDateTime::currentDateTime().toString(Qt::ISODate) + "\n"
                + serviceState->text() + "\n" + QString::fromUtf8(QJsonDocument(safe).toJson()) + "\n" + logText()).toUtf8());
            notice->setText(file.commit() ? "报告已保存。分享前请人工检查域名、IPv6 和设备信息。" : "报告保存失败。");
        }
    }

    QWidget *makePage(const QString &title, const QString &description) {
        auto *page = new QWidget;
        page->setAttribute(Qt::WA_StyledBackground, false);
        auto *layout = new QVBoxLayout(page);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(16);
        auto *heading = Ui::text(title, "sectionTitle");
        auto *detail = Ui::text(description, "muted");
        detail->setWordWrap(true);
        layout->addWidget(heading);
        layout->addWidget(detail);
        return page;
    }

    QWidget *hero(const QString &title, const QString &detail) {
        auto *card = Ui::card();
        auto *layout = new QHBoxLayout(card);
        layout->setContentsMargins(22, 18, 22, 18);
        auto *labels = new QVBoxLayout;
        heroTitle = Ui::text(title, "heroTitle");
        heroDetail = Ui::text(detail, "muted");
        heroDetail->setWordWrap(true);
        labels->addWidget(heroTitle);
        labels->addWidget(heroDetail);
        layout->addLayout(labels, 1);
        quality = Ui::text("LIVE / 等待数据", "pill");
        layout->addWidget(quality, 0, Qt::AlignTop);
        return card;
    }

    QWidget *trendCard(const QString &title, Ui::Trend **trend, const QColor &color, const QString &unit) {
        auto *card = Ui::card();
        auto *layout = new QVBoxLayout(card);
        layout->setContentsMargins(18, 14, 18, 14);
        auto *row = new QHBoxLayout;
        row->addWidget(Ui::text(title, "sectionTitle"));
        row->addStretch();
        row->addWidget(Ui::text("真实采样 · 1 秒间隔", "small"));
        layout->addLayout(row);
        *trend = new Ui::Trend(color, unit);
        layout->addWidget(*trend);
        return card;
    }

    QWidget *overviewPage() {
        auto *page = makePage("主机概览", "服务状态、当前认证会话与待处理事项。监听端口不等于已认证连接。 ");
        page->layout()->addWidget(hero("正在检查主机", "状态会自动刷新"));
        auto *metricHost = new QWidget;
        metricHost->setStyleSheet("background:transparent");
        auto *grid = new QGridLayout(metricHost);
        grid->setContentsMargins(0, 0, 0, 0);
        grid->setSpacing(12);
        rtt = new Ui::Metric("隧道往返延迟", "TCP 心跳 RTT · 非端到端操作延迟", Ui::Primary);
        fps = new Ui::Metric("实际渲染帧率", "来自已连接主控端的媒体采样", Ui::Accent);
        traffic = new Ui::Metric("主机发送速率", "TCP + UDP 隧道有效字节差分", Ui::Success);
        drop = new Ui::Metric("网络丢帧率", "媒体丢帧，不等同于 IP 丢包率", Ui::Warning);
        decode = new Ui::Metric("解码耗时", "主控端每帧平均解码时间", Ui::Primary);
        queue = new Ui::Metric("帧排队耗时", "主控端每帧平均呈现等待", Ui::Accent);
        const QList<Ui::Metric *> metrics{rtt, fps, traffic, drop, decode, queue};
        for (int i = 0; i < metrics.size(); ++i) grid->addWidget(metrics[i], i / 3, i % 3);
        page->layout()->addWidget(metricHost);
        auto *list = Ui::card();
        auto *layout = new QVBoxLayout(list);
        layout->setContentsMargins(20, 16, 20, 16);
        auto *row = new QHBoxLayout;
        row->addWidget(Ui::text("已认证设备", "sectionTitle"));
        row->addStretch();
        sessionCount = Ui::text("—", "small");
        row->addWidget(sessionCount);
        layout->addLayout(row);
        devices = new QTableWidget(0, 4);
        devices->setHorizontalHeaderLabels({"设备 / 会话", "传输模式", "连接时长", "状态"});
        devices->verticalHeader()->hide();
        devices->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
        for (int i = 1; i < 4; ++i) devices->horizontalHeader()->setSectionResizeMode(i, QHeaderView::ResizeToContents);
        devices->setEditTriggers(QAbstractItemView::NoEditTriggers);
        devices->setSelectionBehavior(QAbstractItemView::SelectRows);
        devices->setFocusPolicy(Qt::NoFocus);
        devices->setShowGrid(false);
        devices->setMaximumHeight(92);
        layout->addWidget(devices);
        deviceDetail = Ui::text("等待新版隧道提供会话数据。监听状态不代表已经连接。", "muted");
        deviceDetail->setWordWrap(true);
        layout->addWidget(deviceDetail);
        page->layout()->addWidget(list);
        qobject_cast<QVBoxLayout *>(page->layout())->addStretch();
        return page;
    }

    QWidget *qualityPage() {
        auto *page = makePage("连接质量", "从 RTT、吞吐、队列和 UDP 探测逐层定位链路问题。缺失或过期数据不会补成演示曲线。 ");
        auto *metricHost = new QWidget;
        metricHost->setStyleSheet("background:transparent");
        auto *grid = new QGridLayout(metricHost);
        grid->setContentsMargins(0, 0, 0, 0);
        grid->setSpacing(12);
        jitter = new Ui::Metric("心跳抖动", "隧道控制面抖动，非完整网络测量", Ui::Primary);
        tcpFlows = new Ui::Metric("活动 TCP 流", "信令、配置与 Moonlight 服务", Ui::Accent);
        udpFlows = new Ui::Metric("活动 UDP 流", "实时媒体与控制数据流", Ui::Success);
        queued = new Ui::Metric("发送队列", "每个发送队列上限 4 MiB", Ui::Warning);
        reconnects = new Ui::Metric("隧道重连", "TCP 隧道累计重连次数", Ui::Accent);
        drops = new Ui::Metric("本地丢弃", "未就绪、超限或重放拒绝", Ui::Warning);
        probes = new Ui::Metric("UDP 探测 / 应答", "小包探测不等于持续媒体吞吐", Ui::Primary);
        errors = new Ui::Metric("认证拒绝 / 回包错误", "无效包不会更新重放窗口", Ui::Danger);
        const QList<Ui::Metric *> metrics{jitter, tcpFlows, udpFlows, queued, reconnects, drops, probes, errors};
        for (int i = 0; i < metrics.size(); ++i) grid->addWidget(metrics[i], i / 4, i % 4);
        page->layout()->addWidget(metricHost);
        auto *chartHost = new QWidget;
        chartHost->setStyleSheet("background:transparent");
        auto *charts = new QGridLayout(chartHost);
        charts->setContentsMargins(0, 0, 0, 0);
        charts->setSpacing(12);
        charts->addWidget(trendCard("发送吞吐", &sendTrend, Ui::Success, "Mbps"), 0, 0);
        charts->addWidget(trendCard("接收吞吐", &receiveTrend, Ui::Primary, "Mbps"), 0, 1);
        charts->addWidget(trendCard("隧道 RTT", &rttTrend, Ui::Accent, "ms"), 1, 0);
        charts->addWidget(trendCard("发送队列", &queueTrend, Ui::Warning, "KiB"), 1, 1);
        page->layout()->addWidget(chartHost);
        auto *events = Ui::card();
        auto *layout = new QVBoxLayout(events);
        layout->setContentsMargins(18, 14, 18, 14);
        auto *row = new QHBoxLayout;
        row->addWidget(Ui::text("连接事件", "sectionTitle"));
        row->addStretch();
        eventLevel = new QComboBox;
        eventLevel->addItems({"全部级别", "提示与警告", "仅警告"});
        connect(eventLevel, &QComboBox::currentTextChanged, this, [this] { populateEvents(); });
        row->addWidget(eventLevel);
        layout->addLayout(row);
        eventsTable = new QTableWidget(0, 3);
        eventsTable->setHorizontalHeaderLabels({"时间", "级别", "事件"});
        eventsTable->verticalHeader()->hide();
        eventsTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
        eventsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
        eventsTable->setSelectionMode(QAbstractItemView::NoSelection);
        eventsTable->setMaximumHeight(180);
        layout->addWidget(eventsTable);
        page->layout()->addWidget(events);
        qobject_cast<QVBoxLayout *>(page->layout())->addStretch();
        return page;
    }

    QWidget *mediaPage() {
        auto *page = makePage("媒体会话", "这些指标由主控端解码器经认证隧道回传。未开始串流或超过 5 秒无新报告时显示 —。 ");
        auto *metricHost = new QWidget;
        metricHost->setStyleSheet("background:transparent");
        auto *grid = new QGridLayout(metricHost);
        grid->setContentsMargins(0, 0, 0, 0);
        grid->setSpacing(12);
        receivedFps = new Ui::Metric("接收帧率", "到达主控端的视频帧速率", Ui::Primary);
        fps = new Ui::Metric("实际渲染帧率", "主控端实际呈现的视频帧", Ui::Accent);
        bitrate = new Ui::Metric("视频码率", "当前编码媒体平均码率", Ui::Success);
        resolution = new Ui::Metric("视频分辨率", "当前解码器实际输入尺寸", Ui::Primary);
        drop = new Ui::Metric("网络丢帧", "最终无法恢复的视频帧，不是 IP 丢包率", Ui::Warning);
        decode = new Ui::Metric("解码耗时", "主控端每帧平均解码时间", Ui::Primary);
        queue = new Ui::Metric("帧排队耗时", "呈现节奏等待，不等同于网络延迟", Ui::Accent);
        render = new Ui::Metric("渲染耗时", "主控端每帧平均渲染时间", Ui::Success);
        hostProcessing = new Ui::Metric("主机处理", "有上报时的主机处理耗时", Ui::Accent);
        const QList<Ui::Metric *> metrics{receivedFps, fps, bitrate, resolution, drop, decode, queue, render, hostProcessing};
        for (int i = 0; i < metrics.size(); ++i) grid->addWidget(metrics[i], i / 3, i % 3);
        page->layout()->addWidget(metricHost);
        auto *hint = Ui::card();
        auto *layout = new QVBoxLayout(hint);
        layout->setContentsMargins(20, 16, 20, 16);
        auto *text = Ui::text("如何使用：先看网络丢帧与 RTT，再看解码与排队。解码正常但排队升高，优先检查呈现节奏和显示刷新率；RTT 不是端到端操作延迟。", "muted");
        text->setWordWrap(true);
        layout->addWidget(text);
        page->layout()->addWidget(hint);
        qobject_cast<QVBoxLayout *>(page->layout())->addStretch();
        return page;
    }

    QWidget *servicePage() {
        auto *page = makePage("服务与诊断", "管理本机 Sunshine 与 mslink，检查事件和日志。危险操作需要确认，导出后仍需人工复核。 ");
        auto *services = Ui::card();
        auto *layout = new QVBoxLayout(services);
        layout->setContentsMargins(22, 18, 22, 18);
        layout->addWidget(Ui::text("服务与运行环境", "sectionTitle"));
        serviceState = Ui::text("正在检查本机服务…", "muted");
        runtimeState = Ui::text("", "muted");
        layout->addWidget(serviceState);
        layout->addWidget(runtimeState);
        for (auto pair : QList<QPair<QString, QString>>{{"Sunshine · 画面与输入", "sunshine"}, {"mslink · 连接隧道", "tunnel"}}) {
            auto *row = new QHBoxLayout;
            row->addWidget(Ui::text(pair.first), 1);
            row->addWidget(button("启动", [this, pair] { action("start", pair.second); }));
            row->addWidget(button("重新启动", [this, pair] { action("restart", pair.second); }));
            row->addWidget(button("停止", [this, pair] { action("stop", pair.second); }, "danger"));
            layout->addLayout(row);
        }
        page->layout()->addWidget(services);
        auto *config = Ui::card();
        auto *form = new QFormLayout(config);
        form->setContentsMargins(22, 18, 22, 18);
        form->setVerticalSpacing(14);
        form->addRow(Ui::text("基础连接设置", "sectionTitle"));
        tcp = new QSpinBox; tcp->setRange(1, 65535); tcp->setValue(cfg("mslink-host.ini", "tcp_port", "14738").toInt());
        udp = new QSpinBox; udp->setRange(1, 65535); udp->setValue(cfg("mslink-host.ini", "udp_port", "14739").toInt());
        timeout = new QSpinBox; timeout->setRange(1000, 120000); timeout->setSingleStep(1000); timeout->setSuffix(" ms"); timeout->setValue(cfg("sunshine/config/sunshine.conf", "ping_timeout", "10000").toInt());
        form->addRow("隧道 TCP 端口", tcp);
        form->addRow("隧道 UDP 端口", udp);
        form->addRow("Sunshine 连接超时", timeout);
        save = button("保存配置", [this] { action("save", "all"); }, "primary");
        form->addRow(save);
        auto *hint = Ui::text("保存会备份原配置，保留其他字段；修改不会立即中断连接。保存后请主动重启相关服务，并同步网络映射。高级画质设置可打开 Sunshine 管理。", "muted");
        hint->setWordWrap(true);
        form->addRow(hint);
        page->layout()->addWidget(config);
        auto *diagnostics = Ui::card();
        auto *logLayout = new QVBoxLayout(diagnostics);
        logLayout->setContentsMargins(20, 15, 20, 15);
        auto *row = new QHBoxLayout;
        logKind = new QComboBox;
        logKind->addItem("管理操作", "gui-action.log");
        logKind->addItem("隧道采样历史", "mslink-status.jsonl");
        logKind->addItem("Sunshine 启动", "run-sunshine.log");
        logKind->addItem("隧道启动错误", "run-tunnel-error.log");
        logKind->addItem("Sunshine 日志", "sunshine/config/sunshine.log");
        row->addWidget(logKind, 1);
        row->addWidget(button("导出诊断", [this] { exportReport(); }, "primary"));
        logLayout->addLayout(row);
        logs = new QPlainTextEdit;
        logs->setReadOnly(true);
        logs->setMinimumHeight(200);
        logLayout->addWidget(logs);
        page->layout()->addWidget(diagnostics);
        qobject_cast<QVBoxLayout *>(page->layout())->addStretch();
        return page;
    }

    void populateEvents() {
        if (!eventsTable || !eventLevel) return;
        const auto events = current.value("events").toArray();
        QList<QJsonObject> filtered;
        for (const auto &value : events) {
            const auto event = value.toObject();
            const bool warning = event.value("level").toString() != "info";
            if (eventLevel->currentIndex() == 0 || (eventLevel->currentIndex() == 1 ? true : warning)) filtered.append(event);
        }
        eventsTable->setRowCount(qMin(30, filtered.size()));
        for (int row = 0; row < eventsTable->rowCount(); ++row) {
            const auto event = filtered.at(filtered.size() - 1 - row);
            const auto time = QDateTime::fromMSecsSinceEpoch(qint64(event.value("time_unix_ms").toDouble())).toString("HH:mm:ss");
            const bool warning = event.value("level").toString() != "info";
            const QStringList values{time, warning ? "注意" : "信息", event.value("message").toString()};
            for (int column = 0; column < 3; ++column) {
                auto *item = new QTableWidgetItem(values[column]);
                if (warning) item->setForeground(Ui::Warning);
                eventsTable->setItem(row, column, item);
            }
        }
    }

    void refresh() {
        const bool sun = listening(47989), tun = listening(cfg("mslink-host.ini", "tcp_port", "14738").toInt());
        serviceState->setText(QString("Sunshine HTTP：%1    /    mslink TCP：%2").arg(sun ? "已监听" : "未监听", tun ? "已监听" : "未监听"));
        runtimeState->setText(QFile::exists(QCoreApplication::applicationDirPath() + "/msvcp140.dll")
            ? "运行环境：已随程序部署匹配的 VC143 x64 运行库" : "运行环境：使用系统 VC++ 运行库，请确认已安装配套版本");
        QJsonObject state;
        QFile file(root + "/mslink-status.json");
        if (file.size() < 65536 && file.open(QIODevice::ReadOnly)) state = QJsonDocument::fromJson(file.readAll()).object();
        const auto stamp = state.value("updated_unix_ms").toDouble();
        const auto age = QDateTime::currentMSecsSinceEpoch() - stamp;
        const bool fresh = state.value("schema").toInt() == 1 && state.value("role").toString() == "host" && age >= 0 && age < 5000 && processMatches(qint64(state.value("pid").toDouble()));
        const bool up = fresh && state.value("tunnel_up").toBool();
        current = fresh ? state : QJsonObject();
        QJsonObject peer;
        if (up && state.value("peer_report_age_ms").toDouble(1e20) < 5000) peer = QJsonDocument::fromJson(state.value("peer_report_json").toString().toUtf8()).object();
        QJsonObject media;
        if (peer.value("media_age_ms").toDouble(1e20) < 5000) media = peer.value("media").toObject();
        const bool hasMedia = up && media.value("received_fps").toDouble() > 0;
        const auto absent = QJsonValue(QJsonValue::Null);
        if (!previousHistoryMs && fresh) {
            QFile historyFile(root + "/mslink-status.jsonl");
            if (historyFile.open(QIODevice::ReadOnly)) {
                const auto lines = historyFile.readAll().split('\n');
                const int first = qMax(0, lines.size() - 300);
                qint64 previousStamp = 0;
                double previousDown = 0, previousUp = 0;
                qint64 previousPid = -1;
                for (int i = first; i < lines.size(); ++i) {
                    const auto sample = QJsonDocument::fromJson(lines[i].trimmed()).object();
                    const auto sampleStamp = qint64(sample.value("updated_unix_ms").toDouble());
                    if (sampleStamp <= 0) continue;
                    const double down = sample.value("tcp_bytes_down").toDouble() + sample.value("udp_bytes_down").toDouble();
                    const double upBytes = sample.value("tcp_bytes_up").toDouble() + sample.value("udp_bytes_up").toDouble();
                    const double dt = double(sampleStamp - previousStamp) / 1000;
                    const bool canDiff = previousPid == qint64(sample.value("pid").toDouble()) && dt > 0 && dt < 5 && down >= previousDown && upBytes >= previousUp && sample.value("tunnel_up").toBool();
                    sendTrend->append(sampleStamp, canDiff ? (down - previousDown) * 8 / dt / 1e6 : std::numeric_limits<double>::quiet_NaN());
                    receiveTrend->append(sampleStamp, canDiff ? (upBytes - previousUp) * 8 / dt / 1e6 : std::numeric_limits<double>::quiet_NaN());
                    rttTrend->append(sampleStamp, sample.value("tunnel_up").toBool() ? sample.value("rtt_ms").toDouble(std::numeric_limits<double>::quiet_NaN()) : std::numeric_limits<double>::quiet_NaN());
                    queueTrend->append(sampleStamp, sample.value("queued_bytes").toDouble() / 1024);
                    previousStamp = sampleStamp;
                    previousDown = down;
                    previousUp = upBytes;
                    previousPid = qint64(sample.value("pid").toDouble());
                }
                previousHistoryMs = previousStamp;
            }
        }
        if (fresh && stamp > previousHistoryMs) {
            sendMbps = receiveMbps = std::numeric_limits<double>::quiet_NaN();
            const double dt = (stamp - previous.value("updated_unix_ms").toDouble()) / 1000;
            const double down = state.value("tcp_bytes_down").toDouble() + state.value("udp_bytes_down").toDouble();
            const double oldDown = previous.value("tcp_bytes_down").toDouble() + previous.value("udp_bytes_down").toDouble();
            const double sentUp = state.value("tcp_bytes_up").toDouble() + state.value("udp_bytes_up").toDouble();
            const double oldUp = previous.value("tcp_bytes_up").toDouble() + previous.value("udp_bytes_up").toDouble();
            if (previous.value("pid") == state.value("pid") && dt > 0 && dt < 5 && down >= oldDown && sentUp >= oldUp && up) {
                sendMbps = (down - oldDown) * 8 / dt / 1e6;
                receiveMbps = (sentUp - oldUp) * 8 / dt / 1e6;
            }
            if (stamp > previousHistoryMs) {
                sendTrend->append(stamp, sendMbps);
                receiveTrend->append(stamp, receiveMbps);
                rttTrend->append(stamp, up ? state.value("rtt_ms").toDouble(std::numeric_limits<double>::quiet_NaN()) : std::numeric_limits<double>::quiet_NaN());
                queueTrend->append(stamp, state.value("queued_bytes").toDouble() / 1024);
                previousHistoryMs = qint64(stamp);
            }
            previous = state;
        } else if (!fresh) {
            previous = {};
            sendMbps = receiveMbps = std::numeric_limits<double>::quiet_NaN();
        }
        rtt->set(number(up ? state.value("rtt_ms") : absent, 0, " ms"), up ? "心跳抖动 " + number(state.value("rtt_jitter_ms"), 0, " ms") : "TCP 心跳 RTT · 非端到端操作延迟");
        fps->set(number(hasMedia ? media.value("fps") : absent, 1, " FPS"));
        traffic->set(std::isfinite(sendMbps) ? QString::number(sendMbps, 'f', 2) + " Mbps" : "—");
        drop->set(number(hasMedia ? media.value("network_frame_drop_percent") : absent, 2, " %"));
        decode->set(number(hasMedia ? media.value("decode_ms") : absent, 2, " ms"));
        queue->set(number(hasMedia ? media.value("queue_ms") : absent, 2, " ms"));
        jitter->set(number(up ? state.value("rtt_jitter_ms") : absent, 0, " ms"));
        tcpFlows->set(up ? QString::number(state.value("active_tcp_streams").toInt()) : "—");
        udpFlows->set(up ? QString::number(state.value("active_udp_flows").toInt()) : "—");
        queued->set(up ? Ui::bytes(qint64(state.value("queued_bytes").toDouble())) : "—", up ? QString::number(state.value("queued_bytes").toDouble() / 4194304 * 100, 'f', 1) + "% / 4 MiB" : "上限 4 MiB");
        reconnects->set(up ? QString::number(state.value("reconnects").toInt()) : "—");
        drops->set(up ? QString::number(state.value("udp_local_dropped").toDouble()) : "—");
        probes->set(up ? QString("%1 / %2").arg(state.value("udp_probe_sent").toDouble()).arg(state.value("udp_probe_ack").toDouble()) : "—");
        errors->set(up ? QString("%1 / %2").arg(state.value("udp_auth_rejected").toDouble()).arg(state.value("udp_reply_errors").toDouble()) : "—");
        receivedFps->set(number(hasMedia ? media.value("received_fps") : absent, 1, " FPS"));
        bitrate->set(number(hasMedia ? media.value("video_mbps") : absent, 1, " Mbps"));
        resolution->set(hasMedia && media.value("width").toInt() ? QString("%1 × %2").arg(media.value("width").toInt()).arg(media.value("height").toInt()) : "—");
        render->set(number(hasMedia ? media.value("render_ms") : absent, 2, " ms"));
        hostProcessing->set(number(hasMedia ? media.value("host_processing_ms") : absent, 2, " ms"));
        heroTitle->setText(up ? (hasMedia ? "正在串流 · 设备已连接" : "设备已连接 · 等待媒体会话") : (sun && tun ? "本机服务已就绪" : "主机服务需要检查"));
        heroDetail->setText(fresh ? (up ? "状态来自真实隧道与主控端上报，每秒更新。" : "当前没有已认证隧道会话。") : (tun ? "检测到监听，但当前目录没有可用遥测。若后台仍为旧版，需要在会话结束后升级并重新启动隧道。" : "启动服务后，连接状态与性能数据会出现在这里。"));
        QString health = "等待连接";
        if (up) {
            health = "已连接 · 等待媒体数据";
            if (hasMedia) health = state.value("rtt_ms").toDouble() > 100 || media.value("network_frame_drop_percent").toDouble() > 2 || media.value("queue_ms").toDouble() > 30 ? "连接需关注" : "采样指标正常";
        }
        quality->setText(health);
        quality->setToolTip("经验提示：隧道 RTT > 100 ms、媒体网络丢帧 > 2% 或帧排队 > 30 ms；并非完整体验评分。");
        devices->setRowCount(up ? 1 : 0);
        sessionCount->setText(fresh ? QString::number(up ? 1 : 0) + " 个已认证隧道会话" : "会话数量未知");
        if (up) {
            QString name = peer.value("device_name").toString("已认证客户端 · 旧版未上报设备名").left(128);
            QString mode = peer.value("transport").toString(state.value("transport").toString());
            mode = mode == "udp" ? "UDP" : mode == "tcp_fallback" ? "TCP 兼容通道" : "探测中";
            const auto seconds = qint64(state.value("connected_seconds").toDouble());
            const QString duration = QString("%1:%2:%3").arg(seconds / 3600, 2, 10, QChar('0')).arg(seconds / 60 % 60, 2, 10, QChar('0')).arg(seconds % 60, 2, 10, QChar('0'));
            const QStringList values{name, mode, duration, hasMedia ? "媒体数据更新中" : "已认证"};
            for (int i = 0; i < 4; ++i) devices->setItem(0, i, new QTableWidgetItem(values[i]));
            deviceDetail->setText(QString("活动 TCP %1 · UDP 流 %2 · 发送队列 %3 · 本地丢弃 %4 · 重连 %5\n%6")
                .arg(state.value("active_tcp_streams").toInt()).arg(state.value("active_udp_flows").toInt())
                .arg(Ui::bytes(qint64(state.value("queued_bytes").toDouble()))).arg(state.value("udp_local_dropped").toDouble()).arg(state.value("reconnects").toInt())
                .arg(hasMedia ? "媒体指标由主控端回传；网络丢帧包含最终无法恢复的视频帧，不等同于 UDP 丢包。" : "需新版主控端且开始串流，才有帧率、丢帧和解码数据。"));
        } else deviceDetail->setText("mslink 当前支持单个已认证隧道客户端；配对设备列表与正在连接的设备是不同概念。");
        lastUpdate->setToolTip(identityLimited ? "当前权限无法核验后台进程路径，已核对 PID 与进程名；采样来自所选安装目录。" : "进程路径与采样时间已校验。");
        lastUpdate->setText(fresh ? "● 实时数据 · " + QDateTime::fromMSecsSinceEpoch(qint64(stamp)).toString("HH:mm:ss") : "○ 等待遥测");
        populateEvents();
        if (pages->currentIndex() == 3) {
            const auto text = logText();
            if (logs->toPlainText() != text) {
                const auto position = logs->verticalScrollBar()->value();
                logs->setPlainText(text);
                logs->verticalScrollBar()->setValue(position);
            }
        }
        if (actionHandle) {
            DWORD result = STILL_ACTIVE;
            GetExitCodeProcess(actionHandle, &result);
            if (result != STILL_ACTIVE) {
                CloseHandle(actionHandle);
                actionHandle = nullptr;
                save->setEnabled(true);
                notice->setText(result == 0 ? (pending == "save" ? "配置已保存。请手动重启相关服务并同步客户端与网络设置。" : "管理命令已完成，请核对实际状态与日志。") : ("操作失败，退出码 " + QString::number(result) + "。请查看管理日志。"));
            }
        }
    }

public:
    explicit Console(const QString &folder) : root(QDir(folder).absolutePath()) {
        setWindowTitle("MiniStream · Host Console | Powered by jianhx2026");
        setWindowIcon(QIcon(QCoreApplication::applicationDirPath() + "/ministream.ico"));
        const auto screen = QGuiApplication::primaryScreen()->availableGeometry();
        resize(qMin(1360, screen.width() - 32), qMin(920, screen.height() - 48));
        setMinimumSize(760, 560);
        setStyleSheet(R"(
            QWidget{font-family:'Microsoft YaHei UI';font-size:12px;color:#f0f5ff;background:#0b1020}
            QMainWindow{background:#0b1020}QFrame#sidebar{background:#10182b;border-right:1px solid #20304b}
            QFrame#card{background:#131e33;border:1px solid #20304b;border-radius:12px}
            QLabel#brand{color:#f0f5ff;font-size:22px;font-weight:700;background:transparent}
            QLabel#sidebarSmall{color:#7f95b7;font-size:10px;background:transparent}
            QLabel#title{color:#f0f5ff;font-size:27px;font-weight:650;background:transparent}
            QLabel#eyebrow{color:#66c9f2;font-size:10px;letter-spacing:2px;background:transparent}
            QLabel#muted{color:#7f95b7;font-size:11px;background:transparent}
            QLabel#small{color:#7f95b7;font-size:10px;background:transparent}
            QLabel#metric{font-family:'Segoe UI';font-size:28px;font-weight:600;color:#f0f5ff;background:transparent}
            QLabel#sectionTitle{font-size:14px;font-weight:600;color:#f0f5ff;background:transparent}
            QLabel#heroTitle{font-size:20px;font-weight:600;color:#f0f5ff;background:transparent}
            QLabel#pill{color:#66c9f2;font-size:11px;background:#182b40;border:1px solid #31556e;border-radius:13px;padding:7px 12px}
            QPushButton{background:#1b2942;border:1px solid #2c3c5a;border-radius:7px;padding:7px 15px;color:#b1c2dd}
            QPushButton:hover{background:#243653;border-color:#4a719b;color:#f0f5ff}
            QPushButton#primary{background:#1d5167;color:#b7edff;border-color:#3786a8}
            QPushButton#primary:hover{background:#28667f}QPushButton#danger{color:#ff8296}
            QPushButton#nav{background:transparent;color:#7f95b7;border:0;text-align:left;padding:13px 18px;font-size:12px;border-radius:7px}
            QPushButton#nav:checked{background:#1d3550;color:#8cddfa;border-left:3px solid #66c9f2}
            QPushButton#nav:hover{background:#1b2942;color:#f0f5ff}QPushButton:disabled{color:#526580}
            QSpinBox,QComboBox{background:#1b2942;border:1px solid #2c3c5a;border-radius:6px;padding:9px;min-width:160px;color:#f0f5ff}
            QAbstractItemView{background:#131e33;border:1px solid #20304b;color:#b1c2dd;selection-background-color:#243653}
            QHeaderView::section{background:#18243a;color:#7f95b7;border:0;padding:9px;text-align:left}
            QPlainTextEdit{background:#0d1628;color:#b1c2dd;border:1px solid #20304b;border-radius:8px;font-family:Consolas;font-size:11px;padding:14px}
            QScrollArea{background:transparent;border:0}QScrollArea>QWidget>QWidget{background:transparent}
            QScrollBar:vertical{background:transparent;width:8px}QScrollBar::handle:vertical{background:#2c3c5a;border-radius:4px;min-height:30px}
            QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical{height:0}
            QToolTip{background:#1b2942;color:#f0f5ff;border:1px solid #2c3c5a;padding:6px}
        )");
        auto *base = new QWidget;
        setCentralWidget(base);
        auto *outer = new QHBoxLayout(base);
        outer->setContentsMargins(0, 0, 0, 0);
        outer->setSpacing(0);
        auto *sidebar = new QFrame;
        sidebar->setObjectName("sidebar");
        sidebar->setFixedWidth(230);
        auto *side = new QVBoxLayout(sidebar);
        side->setContentsMargins(22, 28, 18, 24);
        side->setSpacing(8);
        auto *brandRow = new QHBoxLayout;
        auto *logo = Ui::text("");
        logo->setPixmap(QIcon(QCoreApplication::applicationDirPath() + "/ministream.ico").pixmap(32, 32));
        brandRow->addWidget(logo);
        brandRow->addWidget(Ui::text("MiniStream", "brand"));
        brandRow->addStretch();
        side->addLayout(brandRow);
        side->addWidget(Ui::text("HOST / 主机健康工作台", "sidebarSmall"));
        side->addSpacing(28);
        side->addWidget(Ui::text("WORKSPACE", "sidebarSmall"));
        pages = new QStackedWidget;
        auto *group = new QButtonGroup(this);
        const QStringList names{"主机概览", "连接质量", "媒体会话", "服务与诊断"};
        const QStringList subtitles{"服务、会话与主机状态，一屏掌握。", "从链路到队列，定位每一次波动。", "从接收到呈现，拆解媒体耗时。", "服务管理、端口配置与可解释诊断。"};
        for (int i = 0; i < names.size(); ++i) {
            auto *nav = button(names[i], [this, i, names, subtitles] {
                pages->setCurrentIndex(i);
                headline->setText(names[i]);
                subtitle->setText(subtitles[i]);
                refresh();
            }, "nav");
            nav->setCheckable(true);
            group->addButton(nav);
            if (i == 0) nav->setChecked(true);
            side->addWidget(nav);
        }
        side->addStretch();
        auto *telemetry = Ui::card();
        auto *telemetryLayout = new QVBoxLayout(telemetry);
        telemetryLayout->setContentsMargins(14, 12, 14, 12);
        telemetryLayout->addWidget(Ui::text("真实遥测 · 1 秒采样", "small"));
        telemetryLayout->addWidget(Ui::text("无数据时显示 —\n不生成演示指标", "muted"));
        side->addWidget(telemetry);
        auto link = [&](const QString &text, const QUrl &url) { side->addWidget(button(text, [url] { QDesktopServices::openUrl(url); }, "nav")); };
        link("Sunshine 管理  ↗", QUrl("https://localhost:47990"));
        link("使用教程  ↗", QUrl("https://ministream.jianhx.com/tutorial.html"));
        link("安装目录  ↗", QUrl::fromLocalFile(root));
        side->addSpacing(12);
        side->addWidget(Ui::text("MiniStream 2.2 · Host", "sidebarSmall"));
        side->addWidget(Ui::text("Powered by jianhx2026", "sidebarSmall"));
        side->addWidget(Ui::text("Sunshine / Moonlight · GPLv3", "sidebarSmall"));
        outer->addWidget(sidebar);
        auto *content = new QWidget;
        auto *body = new QVBoxLayout(content);
        body->setContentsMargins(30, 25, 30, 20);
        body->setSpacing(17);
        auto *header = new QHBoxLayout;
        auto *titleColumn = new QVBoxLayout;
        titleColumn->setSpacing(5);
        titleColumn->addWidget(Ui::text("MINISTREAM / HOST CONSOLE", "eyebrow"));
        headline = Ui::text("主机概览", "title");
        titleColumn->addWidget(headline);
        subtitle = Ui::text(subtitles[0], "muted");
        titleColumn->addWidget(subtitle);
        header->addLayout(titleColumn, 1);
        lastUpdate = Ui::text("○ 等待遥测", "small");
        header->addWidget(lastUpdate, 0, Qt::AlignTop);
        header->addSpacing(12);
        header->addWidget(button("刷新", [this] { refresh(); }));
        body->addLayout(header);
        pages->addWidget(overviewPage());
        pages->addWidget(qualityPage());
        pages->addWidget(mediaPage());
        pages->addWidget(servicePage());
        auto *scroll = new QScrollArea;
        scroll->setWidgetResizable(true);
        scroll->setWidget(pages);
        body->addWidget(scroll, 1);
        notice = Ui::text("本机监听、隧道认证与媒体会话分别展示；管理操作需要 Windows 授权。", "small");
        notice->setWordWrap(true);
        body->addWidget(notice);
        outer->addWidget(content, 1);
        connect(&timer, &QTimer::timeout, this, [this] { refresh(); });
        timer.start(1000);
        refresh();
    }
    ~Console() { if (actionHandle) CloseHandle(actionHandle); }
};

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setOrganizationName("MiniStream");
    app.setApplicationName("MiniStreamHost");
    app.setApplicationVersion("2.2.0");
    QString root = QCoreApplication::applicationDirPath();
    if (!QFile::exists(root + "/mslink.exe") && QFile::exists(root + "/../mslink.exe")) root = QDir(root + "/..").absolutePath();
    for (int i = 1; i < argc - 1; ++i) if (QString::fromLocal8Bit(argv[i]) == "--root") root = QString::fromLocal8Bit(argv[i + 1]);
    Console console(root);
    console.show();
    return app.exec();
}
