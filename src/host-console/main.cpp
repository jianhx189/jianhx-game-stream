#include <QtWidgets>
#include <QtNetwork>
#include <windows.h>
#include <iphlpapi.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <cmath>
#include "ui_components.h"
#include "host_setup.h"

class ConnectionPulse final : public QWidget {
    QVariantAnimation animation;
    qreal progress = 0;
    QColor tone = Ui::Warning;

public:
    ConnectionPulse()
    {
        setFixedSize(104, 74);
        setStyleSheet("background: transparent");
        animation.setStartValue(0.0);
        animation.setEndValue(1.0);
        animation.setDuration(2300);
        animation.setLoopCount(-1);
        connect(&animation, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
            progress = value.toReal();
            update();
        });
    }

    void setStatus(bool ready, bool connected)
    {
        const auto next = connected ? Ui::Success : ready ? Ui::Primary : Ui::Warning;
        if (tone == next) return;
        tone = next;
        update();
    }

protected:
    void showEvent(QShowEvent *event) override
    {
        QWidget::showEvent(event);
        if (HostSetup::animationsEnabled()) animation.start();
    }

    void hideEvent(QHideEvent *event) override
    {
        animation.stop();
        QWidget::hideEvent(event);
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QPointF center(width() / 2.0, height() / 2.0);
        QRadialGradient glow(center, 38);
        auto color = tone;
        color.setAlpha(62);
        glow.setColorAt(0, color);
        color.setAlpha(0);
        glow.setColorAt(1, color);
        painter.setPen(Qt::NoPen);
        painter.setBrush(glow);
        painter.drawEllipse(center, 38, 38);
        painter.setBrush(Qt::NoBrush);
        for (int radius : {16, 25, 34}) {
            color.setAlpha(31);
            painter.setPen(QPen(color));
            painter.drawEllipse(center, radius, radius);
        }
        color.setAlpha(qRound(180 * (1 - progress)));
        painter.setPen(QPen(color, 1.6));
        const auto radius = 10 + 24 * progress;
        painter.drawEllipse(center, radius, radius);
        painter.setPen(Qt::NoPen);
        painter.setBrush(tone);
        painter.drawEllipse(center, 4, 4);
    }
};



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
    QTimer timer, clipboardTimer, checkDeadline;
    QProcess selfCheck;
    QSystemTrayIcon *tray = nullptr;
    QAction *trayStatus = nullptr;
    bool exiting = false, trayHintShown = false;
    DWORD clipboardSequence = 0;
    ConnectionPulse *pulse = nullptr;
    QPushButton *setupNav = nullptr, *serviceNav = nullptr, *checkButton = nullptr;
    QLabel *checkResult = nullptr, *stepHint = nullptr, *routeStatus = nullptr;
    QLabel *summary = nullptr, *setupNotice = nullptr, *mappingStatus = nullptr;
    QWidget *lanFields = nullptr, *frpFields = nullptr;
    QTabBar *templateMode = nullptr;
    QCheckBox *advancedMapping = nullptr;
    QWidget *mappingFields = nullptr;
    QComboBox *lanAddress = nullptr;
    QLineEdit *publicAddress = nullptr;
    QSpinBox *publicTcp = nullptr, *publicUdp = nullptr, *privateTcp = nullptr, *privateUdp = nullptr;
    QAction *copyEndpoint = nullptr;
    QPushButton *copyConnection = nullptr, *frpCopy = nullptr;
    QFrame *setupFooter = nullptr;
    QPushButton *applyMapping = nullptr;
    QString connectionAddress, frpTemplate;
    int connectionTcp = 0, connectionUdp = 0;
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
        QString result = fallback;
        for (auto line : QString::fromUtf8(f.readAll()).split('\n')) {
            line = line.trimmed();
            if (line.startsWith('#') || line.startsWith(';')) continue;
            const int index = line.indexOf('=');
            if (index <= 0 || line.left(index).trimmed().compare(key, Qt::CaseInsensitive) != 0) continue;
            auto value = line.mid(index + 1);
            const auto comment = value.indexOf('#');
            if (comment >= 0) value.truncate(comment);
            result = value.trimmed();
        }
        return result;
    }

    bool listening(int port, const QString &expected) {
        DWORD size = 0;
        GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0);
        QByteArray buffer(int(size), 0);
        if (GetExtendedTcpTable(buffer.data(), &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0) != NO_ERROR) return false;
        auto table = reinterpret_cast<PMIB_TCPTABLE_OWNER_PID>(buffer.data());
        for (DWORD i = 0; i < table->dwNumEntries; ++i)
            if (ntohs((u_short)table->table[i].dwLocalPort) == port &&
                processMatches(table->table[i].dwOwningPid, expected)) return true;
        return false;
    }

    bool processMatches(qint64 pid, const QString &expected = "mslink.exe") {
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
                if (entry.th32ProcessID == DWORD(pid) && QString::fromWCharArray(entry.szExeFile).compare(QFileInfo(expected).fileName(), Qt::CaseInsensitive) == 0) {
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
        return ok && QDir::cleanPath(QString::fromWCharArray(path, length)).compare(QDir::cleanPath(root + "/" + expected), Qt::CaseInsensitive) == 0;
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
        if (verb == "save" && tcp->value() == udp->value()) {
            notice->setText("TCP 和 UDP 端口必须不同。");
            return;
        }
        QString prompt;
        if (verb == "save") prompt = "保存后请重启相关服务。端口有变动时，请更新主控端、FRP 映射及防火墙规则。是否保存？";
        if (verb == "restart" || verb == "stop") prompt = "此操作会中断当前远程连接。是否继续？";
        if (verb == "firewall") prompt = "将为 mslink 更新当前配置端口对应的 TCP 与 UDP 入站放行规则。是否继续？";
        if (!prompt.isEmpty() && QMessageBox::question(this, "MiniStream", prompt) != QMessageBox::Yes) return;
        const auto owner = reinterpret_cast<HWND>(winId());
        if (verb == "password") {
            actionHandle = HostSetup::elevate(QCoreApplication::applicationFilePath(),
                "--change-password --root " + HostSetup::quotePath(root), owner, SW_SHOWNORMAL);
        } else {
            const auto args = "-NoProfile -ExecutionPolicy Bypass -File " + HostSetup::quotePath(root + "/_internal/gui-action.ps1")
                + " -Root " + HostSetup::quotePath(root) + " -Action " + verb + " -Component " + component
                + " -TcpPort " + QString::number(tcp->value()) + " -UdpPort " + QString::number(udp->value())
                + " -TimeoutMs " + QString::number(timeout->value());
            actionHandle = HostSetup::elevate(HostSetup::powerShell(), args, owner, SW_HIDE);
        }
        if (!actionHandle) {
            const auto message = GetLastError() == ERROR_CANCELLED ? "已取消管理员授权。" : "无法启动管理操作，请检查 Windows 权限。";
            notice->setText(message);
            if (verb == "password") setupNotice->setText(message);
            return;
        }
        pending = verb;
        save->setEnabled(false);
        notice->setText("管理操作正在进行。");
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

    QWidget *makePage(const QString &description) {
        auto *page = new QWidget;
        page->setAttribute(Qt::WA_StyledBackground, false);
        auto *layout = new QVBoxLayout(page);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(16);
        if (!description.isEmpty()) {
            auto *detail = Ui::text(description, "muted");
            detail->setWordWrap(true);
            layout->addWidget(detail);
        }
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
        pulse = new ConnectionPulse;
        layout->addWidget(pulse);
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
        auto *page = makePage("服务状态、当前认证会话与待处理事项。监听端口不等于已认证连接。 ");
        page->layout()->addWidget(hero("正在检查主机", "状态会自动刷新"));
        auto *start = new QHBoxLayout;
        start->addWidget(button("局域网连接", [this] { templateMode->setCurrentIndex(0); setupNav->click(); }, "primary"));
        start->addWidget(button("FRP 穿透连接", [this] { templateMode->setCurrentIndex(1); setupNav->click(); }));
        start->addWidget(button("运行本机自检", [this] { serviceNav->click(); runSelfCheck(); }));
        start->addStretch();
        qobject_cast<QVBoxLayout *>(page->layout())->addLayout(start);
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
        auto *page = makePage("从 RTT、吞吐、队列和 UDP 探测逐层定位链路问题。缺失或过期数据不会补成演示曲线。 ");
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
        auto *page = makePage("这些指标由主控端解码器经认证隧道回传。未开始串流或超过 5 秒无新报告时显示 —。 ");
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

    void copyText(const QString &text, bool credential, const QString &label = QStringLiteral("内容"))
    {
        if (text.isEmpty()) { setupNotice->setText("请先填写或选择有效的连接地址。"); return; }
        const auto owner = reinterpret_cast<HWND>(winId());
        if (!credential) {
            QApplication::clipboard()->setText(text);
            setupNotice->setText(QStringLiteral("已复制%1。").arg(label));
            return;
        }
        if (clipboardSequence) HostSetup::clearCredential(owner, clipboardSequence);
        clipboardSequence = HostSetup::copyCredential(owner, text);
        if (!clipboardSequence) { setupNotice->setText("剪贴板被占用，请稍后重试。"); return; }
        setupNotice->setText(QStringLiteral("已复制%1 · 60 秒后清除剪贴板。").arg(label));
        clipboardTimer.start(60000);
    }

    QString settingsKey(const QString &name) const
    {
        return "host/" + QString::fromLatin1(QCryptographicHash::hash(root.toCaseFolded().toUtf8(),
            QCryptographicHash::Sha256).toHex()) + '/' + name;
    }

    void refreshSetup()
    {
        const bool frp = templateMode->currentIndex() == 1;
        const bool custom = frp && advancedMapping->isChecked();
        lanFields->setVisible(!frp);
        frpFields->setVisible(frp);
        connectionAddress = frp ? publicAddress->text().trimmed() : lanAddress->currentData().toString();
        const QHostAddress parsed(connectionAddress);
        if (parsed.protocol() != QAbstractSocket::IPv4Protocol || parsed == QHostAddress::AnyIPv4 ||
            parsed == QHostAddress::Broadcast || parsed.isMulticast()) connectionAddress.clear();
        else connectionAddress = parsed.toString();
        const int localTcp = cfg("mslink-host.ini", "tcp_port", "14738").toInt();
        const int localUdp = cfg("mslink-host.ini", "udp_port", "14739").toInt();
        const bool localPortsValid = localTcp > 0 && localTcp <= 65535 && localUdp > 0 && localUdp <= 65535 && localTcp != localUdp;
        const auto syncPort = [this](QSpinBox *field, const QString &key, int port) {
            if (!QSettings().contains(settingsKey(key))) {
                const QSignalBlocker blocker(field);
                field->setValue(port);
            }
        };
        syncPort(publicTcp, "frp/tcp", localTcp);
        syncPort(publicUdp, "frp/udp", localUdp);
        syncPort(privateTcp, "frp/privateTcp", localTcp);
        syncPort(privateUdp, "frp/privateUdp", localUdp);
        const int targetTcp = custom ? privateTcp->value() : localTcp;
        const int targetUdp = custom ? privateUdp->value() : localUdp;
        connectionTcp = custom ? publicTcp->value() : localTcp;
        connectionUdp = custom ? publicUdp->value() : localUdp;
        const bool distinctTargets = targetTcp != targetUdp;
        const bool targetsMatch = targetTcp == localTcp && targetUdp == localUdp;
        const bool endpointReady = localPortsValid && targetsMatch && !connectionAddress.isEmpty();
        stepHint->setText(frp ? "使用自己的 FRP 服务，将两条代理加入现有 frpc 配置。"
            : "主控端和本机使用同一个网络时，分享此地址。");
        mappingStatus->setText(!distinctTargets ? "MiniStream 的 TCP 和 UDP 内网监听端口必须不同。"
            : !targetsMatch ? QString("当前主机监听配置为 TCP %1 / UDP %2。请应用内网端口后再生成邀请。").arg(localTcp).arg(localUdp)
            : QString());
        mappingStatus->setVisible(!distinctTargets || !targetsMatch);
        applyMapping->setVisible(custom && distinctTargets && !targetsMatch);
        routeStatus->setText(endpointReady ? frp ? "请在主控端测试公网 TCP 和 UDP。" : "局域网地址已就绪。"
            : connectionAddress.isEmpty() ? "请先确认连接地址。" : "请先核对本机隧道端口。");
        summary->setText(QString("%1  ·  TCP %2  ·  UDP %3")
            .arg(connectionAddress.isEmpty() ? "待填写" : connectionAddress)
            .arg(connectionTcp).arg(connectionUdp));
        copyEndpoint->setEnabled(!connectionAddress.isEmpty());
        copyConnection->setEnabled(endpointReady);
        mappingFields->setVisible(custom);
        frpCopy->setVisible(frp);
        const auto bind = cfg("mslink-host.ini", "bind", "0.0.0.0");
        const auto localAddress = bind == "0.0.0.0" ? QString("127.0.0.1") : bind;
        const bool localAddressValid = QHostAddress(localAddress).protocol() == QAbstractSocket::IPv4Protocol;
        frpCopy->setEnabled(frp && localAddressValid && distinctTargets &&
                            targetTcp > 0 && targetTcp <= 65535 && targetUdp > 0 && targetUdp <= 65535);
        frpTemplate = !frpCopy->isEnabled() ? QString() :
            QString("[[proxies]]\nname = \"ministream-tcp\"\ntype = \"tcp\"\nlocalIP = \"%1\"\n"
                    "localPort = %2\nremotePort = %3\n\n"
                    "[[proxies]]\nname = \"ministream-udp\"\ntype = \"udp\"\nlocalIP = \"%1\"\n"
                    "localPort = %4\nremotePort = %5\n")
                .arg(localAddress).arg(targetTcp).arg(connectionTcp).arg(targetUdp).arg(connectionUdp);
    }

    void shareInvitation()
    {
        refreshSetup();
        if (!copyConnection->isEnabled()) return;
        const auto password = cfg("mslink-host.ini", "password", "");
        if (!HostSetup::validPassword(password, 1)) { setupNotice->setText("请先设置有效的隧道口令。"); return; }
        const bool frp = templateMode->currentIndex() == 1;
        const auto ip = connectionAddress;
        const int tcpPort = connectionTcp, udpPort = connectionUdp;
        const QJsonObject invite{{"v",1},{"mode",frp ? "frp" : "lan"},{"ip",ip},{"tcp",tcpPort},{"udp",udpPort},{"key",password}};
        const auto payload = QJsonDocument(invite).toJson(QJsonDocument::Compact).toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
        copyText("https://ministream.jianhx.com/connect.html#" + QString::fromLatin1(payload), true, QStringLiteral("邀请链接"));
        if (clipboardSequence) setupNotice->setText("邀请链接已复制。链接含隧道口令，仅私发给信任的人。");
    }

    void copyTunnelPassword()
    {
        const auto password = cfg("mslink-host.ini", "password", "");
        if (!HostSetup::validPassword(password, 1)) {
            setupNotice->setText("无法读取有效口令。请确认是完整安装目录，或点击「更换隧道口令」。");
            return;
        }
        copyText(password, true, QStringLiteral("隧道口令"));
    }

    QWidget *setupPage()
    {
        auto *page = makePage({});
        page->setObjectName("connectionPage");
        auto *card = new QFrame;
        card->setObjectName("connectionPanel");
        auto *form = new QVBoxLayout(card);
        form->setContentsMargins(22, 16, 22, 16);
        form->setSpacing(10);
        templateMode = new QTabBar;
        templateMode->setObjectName("connectionTabs");
        templateMode->setExpanding(false);
        templateMode->setDrawBase(false);
        templateMode->addTab("局域网直连");
        templateMode->addTab("FRP 穿透");
        templateMode->setAccessibleName("网络模板");
        page->layout()->addWidget(templateMode);
        auto *lan = new QWidget;
        lanFields = lan;
        lan->setObjectName("connectionFields");
        auto *lanForm = new QFormLayout(lan);
        lanForm->setContentsMargins(0, 0, 0, 0);
        lanAddress = new QComboBox;
        lanAddress->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        lanAddress->setAccessibleName("本机网卡地址");
        HostSetup::populateLanAddresses(lanAddress, cfg("mslink-host.ini", "bind", "0.0.0.0"));
        const auto lanIndex = lanAddress->findData(QSettings().value(settingsKey("lan")));
        if (lanIndex >= 0) lanAddress->setCurrentIndex(lanIndex);
        auto *lanRow = new QHBoxLayout;
        lanRow->addWidget(lanAddress, 1);
        lanRow->addWidget(button("刷新网卡", [this] { HostSetup::populateLanAddresses(lanAddress, cfg("mslink-host.ini", "bind", "0.0.0.0")); refreshSetup(); }));
        lanForm->addRow("本机 IPv4", lanRow);
        form->addWidget(lan);
        auto *frp = new QWidget;
        frpFields = frp;
        frp->setObjectName("connectionFields");
        auto *frpForm = new QVBoxLayout(frp);
        frpForm->setContentsMargins(0, 0, 0, 0);
        frpForm->setSpacing(10);
        auto *addressLabel = Ui::text("公网 IPv4", "connectionLabel");
        publicAddress = new QLineEdit(QSettings().value(settingsKey("frp/address")).toString());
        publicAddress->setPlaceholderText("输入穿透后的公网 IPv4");
        publicAddress->setObjectName("publicAddress");
        publicAddress->setMaxLength(15);
        publicAddress->setAccessibleName(addressLabel->text());
        addressLabel->setBuddy(publicAddress);
        frpForm->addWidget(addressLabel);
        frpForm->addWidget(publicAddress);
        advancedMapping = new QCheckBox("自定义公网与内网端口映射");
        const QSettings settings;
        const bool hasOldMapping = settings.contains(settingsKey("frp/tcp")) || settings.contains(settingsKey("frp/udp"));
        advancedMapping->setChecked(settings.value(settingsKey("frp/customMapping"), hasOldMapping).toBool());
        frpForm->addWidget(advancedMapping);
        connect(advancedMapping, &QCheckBox::toggled, this, [this](bool enabled) {
            QSettings().setValue(settingsKey("frp/customMapping"), enabled);
            refreshSetup();
        });

        auto *mappingCard = new QWidget;
        mappingCard->setObjectName("mappingTable");
        mappingFields = mappingCard;
        auto *mapping = new QGridLayout(mappingCard);
        mapping->setContentsMargins(12, 8, 12, 9);
        mapping->setHorizontalSpacing(10);
        mapping->setVerticalSpacing(6);
        mapping->setColumnStretch(1, 1);
        mapping->setColumnStretch(3, 1);
        mapping->addWidget(Ui::text("公网端口", "muted"), 1, 1);
        mapping->addWidget(Ui::text("本机隧道端口", "muted"), 1, 3);
        auto port = [this, mapping](const QString &name, const QString &key, int row, int column) {
            auto *field = new QSpinBox;
            field->setObjectName("mappingPort");
            field->setAccessibleName(name);
            field->setRange(1, 65535);
            field->setValue(QSettings().value(settingsKey(key), 1).toInt());
            field->setKeyboardTracking(false);
            field->setButtonSymbols(QAbstractSpinBox::NoButtons);
            connect(field, &QSpinBox::valueChanged, this, [this, key](int value) {
                QSettings().setValue(settingsKey(key), value);
                refreshSetup();
            });
            mapping->addWidget(field, row, column);
            return field;
        };
        mapping->addWidget(Ui::text("TCP", "sectionTitle"), 2, 0);
        mapping->addWidget(Ui::text("→", "muted"), 2, 2);
        mapping->addWidget(Ui::text("UDP", "sectionTitle"), 3, 0);
        mapping->addWidget(Ui::text("→", "muted"), 3, 2);
        publicTcp = port("公网 TCP 端口", "frp/tcp", 2, 1);
        privateTcp = port("内网 TCP 端口", "frp/privateTcp", 2, 3);
        publicUdp = port("公网 UDP 端口", "frp/udp", 3, 1);
        privateUdp = port("内网 UDP 端口", "frp/privateUdp", 3, 3);
        mappingStatus = Ui::text("", "muted");
        mappingStatus->setWordWrap(true);
        mapping->addWidget(mappingStatus, 4, 0, 1, 4);
        applyMapping = button("在服务设置中应用内网端口", [this] {
            tcp->setValue(privateTcp->value());
            udp->setValue(privateUdp->value());
            serviceNav->click();
            save->setFocus();
        });
        mapping->addWidget(applyMapping, 5, 0, 1, 4, Qt::AlignLeft);

        form->addWidget(frp);
        stepHint = Ui::text("", "muted");
        stepHint->setWordWrap(true);
        form->addWidget(stepHint);
        form->addWidget(mappingCard);
        card->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
        page->layout()->addWidget(card);

        setupFooter = new QFrame;
        setupFooter->setObjectName("connectionFooter");
        auto *details = new QVBoxLayout(setupFooter);
        details->setContentsMargins(16, 10, 16, 10);
        details->setSpacing(5);
        auto *copyRow = new QHBoxLayout;
        copyRow->setSpacing(8);
        routeStatus = Ui::text("", "connectionHelp");
        routeStatus->setWordWrap(true);
        summary = Ui::text("", "connectionSummary");
        summary->setTextFormat(Qt::PlainText);
        summary->setWordWrap(true);
        details->addWidget(summary);
        copyConnection = button("生成并复制邀请链接", [this] { shareInvitation(); }, "inviteAction");
        copyRow->addStretch();
        frpCopy = button("复制 frpc 代理段", [this] {
            copyText(frpTemplate, false, QStringLiteral("FRP 配置"));
            setupNotice->setText("已复制 TCP 和 UDP 代理段。请合并到现有 frpc 配置。");
        });
        copyRow->addWidget(frpCopy);
        auto *credentials = new QToolButton;
        credentials->setText("更多");
        credentials->setPopupMode(QToolButton::InstantPopup);
        auto *menu = new QMenu(credentials);
        copyEndpoint = menu->addAction("复制地址", this, [this] { copyText(connectionAddress, false, QStringLiteral("连接地址")); });
        menu->addAction("复制隧道口令", this, [this] { copyTunnelPassword(); });
        menu->addAction("更换隧道口令", this, [this] { action("password", "tunnel"); });
        credentials->setMenu(menu);
        copyRow->addWidget(credentials);
        copyRow->addWidget(copyConnection);
        setupNotice = Ui::text("", "connectionHelp");
        setupNotice->setWordWrap(true);
        details->addLayout(copyRow);
        details->addWidget(routeStatus);
        details->addWidget(setupNotice);
        qobject_cast<QVBoxLayout *>(page->layout())->addStretch();
        connect(templateMode, &QTabBar::currentChanged, this, [this](int index) {
            QSettings().setValue(settingsKey("template"), index);
            refreshSetup();
        });
        connect(lanAddress, &QComboBox::currentIndexChanged, this, [this] {
            const auto value = lanAddress->currentData();
            if (value.isValid()) QSettings().setValue(settingsKey("lan"), value);
            refreshSetup();
        });
        connect(publicAddress, &QLineEdit::textChanged, this, [this] { refreshSetup(); });
        connect(publicAddress, &QLineEdit::editingFinished, this, [this] {
            QSettings().setValue(settingsKey("frp/address"), publicAddress->text().trimmed());
        });
        templateMode->setCurrentIndex(QSettings().value(settingsKey("template"), 0).toInt() == 1 ? 1 : 0);
        refreshSetup();
        return page;
    }

    void runSelfCheck()
    {
        if (selfCheck.state() != QProcess::NotRunning) return;
        checkButton->setEnabled(false);
        checkResult->setText("正在检查 Sunshine、隧道端口与 Windows 防火墙…");
        checkDeadline.start(22000);
        selfCheck.start(HostSetup::powerShell(), {"-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
            root + "/_internal/host-health.ps1", "-Root", root});
    }

    void showCheckResults(const QJsonObject &result)
    {
        if (result.isEmpty()) {
            checkResult->setText("未取得自检结果。请确认安装完整，或稍后重试。");
            return;
        }
        const auto listener = [](const QString &value) -> QString {
            if (value == "ready") return "通过 · 本安装目录的进程已监听";
            if (value == "limited") return "已监听 · 核对了进程，Windows 限制路径核验";
            if (value == "missing") return "待处理 · 未监听，可尝试启动对应服务";
            if (value == "occupied") return "待处理 · 端口被其他程序占用";
            return "无法核验 · 请检查权限与进程";
        };
        const auto rule = [](const QString &value) -> QString {
            if (value == "ready") return "通过 · 已找到匹配的启用入站规则";
            if (value == "missing") return "待处理 · 未匹配 MiniStream 规则，可点「修复防火墙规则」";
            return "无法核验 · 请检查 Windows 防火墙服务与权限";
        };
        const auto v = [&](const char *key) { return result.value(key).toString(); };
        QStringList lines;
        if (v("config") != "ready") lines << "隧道配置 · 无法读取有效的端口或口令，请核对安装与连接设置。";
        lines << "Sunshine 47989 · " + listener(v("sunshine"));
        if (v("sunshine") == "ready" || v("sunshine") == "limited") lines << QString("Sunshine 本地响应 · ") +
            (v("http") == "ready" ? "通过 · 服务已回应" : "待处理 · 未收到有效 serverinfo，请查看 Sunshine 日志");
        if (v("config") == "ready") {
            lines << QString("隧道 TCP %1 · %2").arg(result.value("tcpPort").toInt()).arg(listener(v("tcp")));
            lines << QString("隧道 UDP %1 · %2").arg(result.value("udpPort").toInt()).arg(listener(v("udp")));
            lines << "防火墙 TCP · " + rule(v("firewallTcp"));
            lines << "防火墙 UDP · " + rule(v("firewallUdp"));
        }
        const auto policy = v("policy");
        lines << QString("防火墙活动配置 · ") + (policy == "ready" ? "已启用并允许入站规则"
            : policy == "disabled" ? "至少一个活动配置已关闭"
            : policy == "restricted" ? "系统策略可能阻止本地入站规则" : "无法核验");
        const auto startup = v("startup");
        lines << QString("登录自启 · ") + (startup == "ready" ? "两项后台任务已启用"
            : startup == "missing" ? "任务缺失、已禁用或指向其他目录" : "无法核验");
        lines << "验证外网 TCP 和 UDP 回程：请用另一台电脑的连接向导测试。";
        checkResult->setText(lines.join('\n'));
    }

    QWidget *servicePage() {
        auto *page = makePage("先运行自检，再按结果启动服务、修复防火墙或查看日志。");
        auto *check = Ui::card();
        auto *checkLayout = new QVBoxLayout(check);
        checkLayout->setContentsMargins(20, 16, 20, 16);
        auto *checkActions = new QHBoxLayout;
        checkButton = button("运行自检", [this] { runSelfCheck(); }, "primary");
        checkActions->addWidget(checkButton);
        checkActions->addWidget(button("修复防火墙规则", [this] { action("firewall", "tunnel"); }));
        checkActions->addStretch();
        checkLayout->addLayout(checkActions);
        checkResult = Ui::text("点击「运行自检」，核对 Sunshine 响应、TCP/UDP 监听、防火墙规则及登录自启任务。", "muted");
        checkResult->setWordWrap(true);
        checkLayout->addWidget(checkResult);
        page->layout()->addWidget(check);
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
        auto *hint = Ui::text("保存后请重启相关服务以应用设置。改端口后，请修复防火墙规则并更新主控端及 FRP 映射。", "muted");
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
        const auto tcpPort = cfg("mslink-host.ini", "tcp_port", "14738").toInt();
        const bool sun = listening(47989, "sunshine/sunshine.exe");
        const bool tun = listening(tcpPort, "mslink.exe");
        serviceState->setText(QString("Sunshine HTTP：%1    /    mslink TCP：%2").arg(sun ? "已监听" : "未验证", tun ? "已监听" : "未验证"));
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
        pulse->setStatus(sun && tun, up);
        if (trayStatus) {
            const QString state = up ? "设备已连接" : (sun && tun ? "本机服务运行中" : "需要检查本机服务");
            trayStatus->setText(state);
            tray->setToolTip("MiniStream 被控端 · " + state);
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
            if (!GetExitCodeProcess(actionHandle, &result)) result = 1;
            if (result != STILL_ACTIVE) {
                CloseHandle(actionHandle);
                actionHandle = nullptr;
                save->setEnabled(true);
                if (pending == "password") {
                    const QString message = result == 0 ? "口令设置已保存。请复制当前口令，在主控端更新后重新连接。"
                        : result == 5 ? "已关闭口令设置。当前口令以服务配置为准。"
                        : "口令设置未确认完成，请运行自检并查看管理日志。";
                    if (result == 0) HostSetup::clearCredential(reinterpret_cast<HWND>(winId()), clipboardSequence);
                    setupNotice->setText(message);
                    notice->setText(message);
                } else if (result != 0) {
                    notice->setText("操作失败，退出码 " + QString::number(result) + "。请查看管理日志。");
                } else if (pending == "save") {
                    notice->setText("配置已保存。请重启相关服务，更新防火墙及主控端端口。");
                    refreshSetup();
                } else {
                    notice->setText("操作已完成，正在重新核对状态。");
                    runSelfCheck();
                }
            }
        }
    }

public:
    explicit Console(const QString &folder) : root(QDir(folder).absolutePath()) {
        setWindowTitle("MiniStream 被控端 · Powered by jianhx2026");
        setWindowIcon(QIcon(QCoreApplication::applicationDirPath() + "/ministream.ico"));
        const auto screen = QGuiApplication::primaryScreen()->availableGeometry();
        resize(qMin(1360, screen.width() - 32), qMin(920, screen.height() - 48));
        setMinimumSize(760, 560);
        setStyleSheet(R"(
            QWidget{font-family:'Microsoft YaHei UI';font-size:12px;color:#f0f5ff;background:#0b1020}
            QMainWindow{background:#0b1020}QFrame#sidebar{background:#10182b;border-right:1px solid #20304b}
            QFrame#card{background:#131e33;border:1px solid #20304b;border-radius:12px}
            QLabel#brand{color:#f0f5ff;font-size:19px;font-weight:700;background:transparent}
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
            QPushButton#primary:hover{background:#28667f} QPushButton:pressed{background:#121e33;border-color:#66c9f2}QPushButton#danger{color:#ff8296}
            QPushButton#nav{background:transparent;color:#7f95b7;border:0;text-align:left;padding:13px 18px;font-size:12px;border-radius:7px}
            QPushButton#nav:checked{background:#1d3550;color:#b8edff;border-left:3px solid #66c9f2;font-weight:700}
            QPushButton#nav:hover{background:#1b2942;color:#f0f5ff}QPushButton:disabled{color:#526580}
            QPushButton:focus,QLineEdit:focus,QComboBox:focus,QSpinBox:focus{border-color:#66c9f2}
            QLineEdit,QSpinBox,QComboBox{background:#1b2942;border:1px solid #2c3c5a;border-radius:6px;padding:9px;min-width:160px;color:#f0f5ff}
            QTabBar#connectionTabs::tab{background:transparent;color:#8d9bb1;border:none;border-bottom:2px solid #243247;margin-right:8px;padding:8px 18px;font-size:13px}
            QTabBar#connectionTabs::tab:selected{color:#bdf6e5;border-bottom-color:#69dfbd;font-weight:600}
            QTabBar#connectionTabs::tab:hover{color:#f0f5ff}
            QFrame#connectionPanel,QFrame#connectionFooter{background:#141e2d;border:1px solid #2a394b;border-radius:12px}
            QWidget#connectionFields{background:transparent}
            QWidget#mappingTable{background:#101926;border:1px solid #27384b;border-radius:8px}
            QLabel#connectionLabel,QLabel#connectionSummary{background:transparent;color:#e7f1f6;font-size:14px;font-weight:600}
            QLabel#connectionHelp{background:transparent;color:#9cafc0;font-size:11px}
            QLineEdit#publicAddress,QSpinBox#mappingPort{background:#0e1725;border:1px solid #36475b;color:#edf8fe;border-radius:7px;padding:6px 10px}
            QLineEdit#publicAddress:focus,QSpinBox#mappingPort:focus{border-color:#69dfbd}
            QPushButton#inviteAction{background:#79e4c5;color:#102523;border:1px solid #79e4c5;border-radius:8px;font-weight:700;padding:8px 13px}
            QPushButton#inviteAction:hover{background:#a5f2db}
            QPushButton#inviteAction:disabled{background:#263646;border-color:#334658;color:#8496ab}
            QCheckBox{background:transparent;spacing:9px;padding:3px 0;color:#d3e2f6}
            QSpinBox#mappingPort{min-width:68px}
            QToolButton{background:#1b2942;border:1px solid #2c3c5a;border-radius:7px;padding:7px 12px;color:#b1c2dd}
            QToolButton:hover{background:#243653;color:#f0f5ff}
            QAbstractItemView{background:#131e33;border:1px solid #20304b;color:#b1c2dd;selection-background-color:#243653}
            QHeaderView::section{background:#18243a;color:#7f95b7;border:0;padding:9px;text-align:left}
            QPlainTextEdit{background:#0d1628;color:#b1c2dd;border:1px solid #20304b;border-radius:8px;font-family:Consolas;font-size:11px;padding:14px}
            QScrollArea,QWidget#qt_scrollarea_viewport{background:transparent;border:0}QScrollArea>QWidget>QWidget{background:transparent}
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
        sidebar->setFixedWidth(184);
        auto *side = new QVBoxLayout(sidebar);
        side->setContentsMargins(14, 23, 14, 17);
        side->setSpacing(8);
        auto *brandRow = new QHBoxLayout;
        auto *logo = Ui::text("");
        logo->setPixmap(QIcon(QCoreApplication::applicationDirPath() + "/ministream.ico").pixmap(32, 32));
        brandRow->addWidget(logo);
        brandRow->addWidget(Ui::text("MiniStream", "brand"));
        brandRow->addStretch();
        side->addLayout(brandRow);
        side->addWidget(Ui::text("被控端控制台", "sidebarSmall"));
        side->addSpacing(16);
        pages = new QStackedWidget;
        auto *group = new QButtonGroup(this);
        const QStringList names{"主机概览", "连接质量", "媒体会话", "服务与诊断", "连接设置"};
        const QStringList subtitles{"查看后台服务和当前连接。", "查看链路质量与发送队列。", "查看主控端上报的画面与处理耗时。", "核对本机服务、防火墙和登录自启。", "配置主控端访问地址和端口。"};
        for (int i = 0; i < names.size(); ++i) {
            auto *nav = button(names[i], [this, i, names, subtitles] {
                const bool changed = pages->currentIndex() != i;
                pages->setCurrentIndex(i);
                if (changed && QApplication::isEffectEnabled(Qt::UI_AnimateMenu) &&
                    pages->currentWidget()->graphicsEffect() == nullptr) {
                    auto *page = pages->currentWidget();
                    auto *fade = new QGraphicsOpacityEffect(page);
                    page->setGraphicsEffect(fade);
                    auto *transition = new QPropertyAnimation(fade, "opacity", page);
                    transition->setDuration(165);
                    transition->setStartValue(0.72);
                    transition->setEndValue(1.0);
                    transition->setEasingCurve(QEasingCurve::OutCubic);
                    connect(transition, &QPropertyAnimation::finished, page, [page] { page->setGraphicsEffect(nullptr); });
                    transition->start(QAbstractAnimation::DeleteWhenStopped);
                }
                setupFooter->setVisible(i == 4);
                notice->setVisible(i != 4);
                if (i == 4) refreshSetup();
                static_cast<QScrollArea *>(pages->currentWidget())->verticalScrollBar()->setValue(0);
                headline->setText(names[i]);
                subtitle->setText(subtitles[i]);
                refresh();
            }, "nav");
            nav->setCheckable(true);
            group->addButton(nav);
            if (i == 0) nav->setChecked(true);
            if (i == 3) serviceNav = nav;
            if (i == 4) setupNav = nav;
            side->addWidget(nav);
        }
        side->addStretch();
        auto link = [&](const QString &text, const QUrl &url) { side->addWidget(button(text, [url] { QDesktopServices::openUrl(url); }, "nav")); };
        link("Sunshine 管理  ↗", QUrl("https://localhost:47990"));
        link("使用教程  ↗", QUrl("https://ministream.jianhx.com/tutorial.html"));
        link("安装目录  ↗", QUrl::fromLocalFile(root));
        side->addSpacing(12);
        side->addWidget(Ui::text("MiniStream " + qApp->applicationVersion(), "sidebarSmall"));
        side->addWidget(Ui::text("Powered by jianhx2026", "sidebarSmall"));
        outer->addWidget(sidebar);
        auto *content = new QWidget;
        auto *body = new QVBoxLayout(content);
        body->setContentsMargins(24, 20, 24, 16);
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
        const auto addPage = [this](QWidget *page) {
            auto *scroll = new QScrollArea;
            scroll->setWidgetResizable(true);
            scroll->setWidget(page);
            pages->addWidget(scroll);
        };
        addPage(overviewPage());
        addPage(qualityPage());
        addPage(mediaPage());
        addPage(servicePage());
        addPage(setupPage());
        body->addWidget(pages, 1);
        body->addWidget(setupFooter);
        setupFooter->hide();
        notice = Ui::text("关闭窗口后，Sunshine 和隧道继续运行。", "small");
        notice->setWordWrap(true);
        body->addWidget(notice);
        outer->addWidget(content, 1);
        connect(&timer, &QTimer::timeout, this, [this] { refresh(); });
        timer.start(1000);
        clipboardTimer.setSingleShot(true);
        connect(&clipboardTimer, &QTimer::timeout, this, [this] {
            if (!HostSetup::clearCredential(reinterpret_cast<HWND>(winId()), clipboardSequence)) {
                clipboardTimer.start(250);
                return;
            }
            clipboardSequence = 0;
        });
        selfCheck.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) { args->flags |= CREATE_NO_WINDOW; });
        checkDeadline.setSingleShot(true);
        connect(&checkDeadline, &QTimer::timeout, &selfCheck, &QProcess::kill);
        connect(&selfCheck, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
            checkDeadline.stop();
            checkButton->setEnabled(true);
            if (code != 0 || status != QProcess::NormalExit) {
                checkResult->setText("未能完成自检。请检查安装文件、Windows 防火墙和任务计划程序，再重试。");
                return;
            }
            showCheckResults(QJsonDocument::fromJson(selfCheck.readAllStandardOutput()).object());
        });
        connect(&selfCheck, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
            if (error != QProcess::FailedToStart) return;
            checkDeadline.stop();
            checkButton->setEnabled(true);
            checkResult->setText("无法启动自检工具，请检查 PowerShell 与安装目录。");
        });
        if (QSystemTrayIcon::isSystemTrayAvailable()) {
            tray = new QSystemTrayIcon(windowIcon(), this);
            auto *menu = new QMenu(this);
            trayStatus = menu->addAction("正在检查主机");
            trayStatus->setEnabled(false);
            menu->addSeparator();
            menu->addAction("打开控制台", this, &Console::openConsole);
            menu->addAction("运行自检", this, [this] { openConsole(); serviceNav->click(); runSelfCheck(); });
            menu->addAction("退出控制台（保留服务）", this, [this] {
                exiting = true;
                tray->hide();
                close();
                qApp->quit();
            });
            tray->setContextMenu(menu);
            connect(tray, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
                if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) openConsole();
            });
            tray->show();
        }
        refresh();
    }

    void openConsole() {
        showNormal();
        raise();
        activateWindow();
        refresh();
    }

    ~Console() {
        if (actionHandle) CloseHandle(actionHandle);
        if (clipboardSequence) HostSetup::clearCredential(reinterpret_cast<HWND>(winId()), clipboardSequence);
        if (selfCheck.state() != QProcess::NotRunning) {
            selfCheck.disconnect(this);
            selfCheck.kill();
            selfCheck.waitForFinished(1500);
        }
    }

protected:
    void closeEvent(QCloseEvent *event) override {
        if (exiting || !tray || qApp->isSavingSession()) { event->accept(); return; }
        hide();
        event->ignore();
        if (!trayHintShown && QSystemTrayIcon::supportsMessages()) {
            trayHintShown = true;
            tray->showMessage("MiniStream 已最小化到托盘", "Sunshine 和隧道继续在后台运行。点击托盘图标可重新打开。");
        }
    }
};

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setOrganizationName("MiniStream");
    app.setApplicationName("MiniStreamHost");
    app.setApplicationVersion("2.3.3");
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption rootOption("root", "MiniStream Host installation directory.", "path");
    const QCommandLineOption passwordOption("change-password", "Open the tunnel password settings dialog.");
    parser.addOptions({rootOption, passwordOption});
    parser.process(app);
    QString root = QCoreApplication::applicationDirPath();
    if (!QFile::exists(root + "/mslink.exe") && QFile::exists(root + "/../mslink.exe")) root = QDir(root + "/..").absolutePath();
    if (parser.isSet(rootOption)) root = QDir(parser.value(rootOption)).absolutePath();
    const auto canonicalRoot = QDir(root).canonicalPath();
    if (!canonicalRoot.isEmpty()) root = canonicalRoot;
    if (parser.isSet(passwordOption)) return HostSetup::changePassword(root);

    const auto directory = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (!QDir().mkpath(directory)) return 1;
    const auto key = "MiniStreamHost-" + QString::fromLatin1(QCryptographicHash::hash(
        (directory + "/" + root).toCaseFolded().toUtf8(), QCryptographicHash::Sha256).toHex());
    QLockFile instanceLock(directory + "/" + key + ".lock");
    instanceLock.setStaleLockTime(0);
    if (!instanceLock.tryLock()) {
        if (instanceLock.error() != QLockFile::LockFailedError) return 1;
        QElapsedTimer timeout;
        timeout.start();
        do {
            QLocalSocket existing;
            existing.connectToServer(key);
            if (existing.waitForConnected(120)) return 0;
            QThread::msleep(35);
        } while (timeout.elapsed() < 1500);
        QMessageBox::information(nullptr, "MiniStream", "控制台已经在运行，请通过系统托盘打开。");
        return 0;
    }

    Console console(root);
    QLocalServer instance;
    instance.setSocketOptions(QLocalServer::UserAccessOption);
    QObject::connect(&instance, &QLocalServer::newConnection, &console, [&] {
        while (auto *client = instance.nextPendingConnection()) client->deleteLater();
        console.openConsole();
    });
    instance.listen(key);
    console.show();
    return app.exec();
}
