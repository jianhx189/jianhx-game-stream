#pragma once

#include <QtWidgets>
#include <QtNetwork>
#include <windows.h>
#include <shellapi.h>

namespace HostSetup {

inline QString powerShell()
{
    wchar_t directory[MAX_PATH];
    const auto length = GetSystemDirectoryW(directory, MAX_PATH);
    if (!length || length >= MAX_PATH) return {};
    return QString::fromWCharArray(directory, int(length)) + "\\WindowsPowerShell\\v1.0\\powershell.exe";
}

inline QString quotePath(const QString &path)
{
    return '"' + QDir::toNativeSeparators(path) + '"';
}

inline HANDLE elevate(const QString &file, const QString &args, HWND owner, int show)
{
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.hwnd = owner;
    info.lpVerb = L"runas";
    info.lpFile = reinterpret_cast<LPCWSTR>(file.utf16());
    info.lpParameters = reinterpret_cast<LPCWSTR>(args.utf16());
    info.nShow = show;
    return ShellExecuteExW(&info) ? info.hProcess : nullptr;
}

inline bool validPassword(const QString &password, int minimum = 12)
{
    static const QRegularExpression characters("\\A[A-Za-z0-9_.@-]+\\z");
    return password.size() >= minimum && password.size() <= 64 && characters.match(password).hasMatch();
}

inline bool validAddress(const QString &address)
{
    static const QRegularExpression hostname(
        "\\A[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?"
        "(?:\\.[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?)*\\.?\\z");
    static const QRegularExpression numeric("\\A[0-9.]+\\z");
    if (numeric.match(address).hasMatch()) {
        const QHostAddress ip(address);
        return ip.protocol() == QAbstractSocket::IPv4Protocol && ip != QHostAddress::AnyIPv4;
    }
    return address.size() <= 253 && hostname.match(address).hasMatch();
}

inline void populateLanAddresses(QComboBox *box, const QString &bind)
{
    const auto selected = box->currentData().toString();
    box->clear();
    QList<QPair<QString, QString>> physical, other;
    for (const auto &iface : QNetworkInterface::allInterfaces()) {
        const auto flags = iface.flags();
        if (!flags.testFlag(QNetworkInterface::IsUp) || flags.testFlag(QNetworkInterface::IsLoopBack)) continue;
        for (const auto &entry : iface.addressEntries()) {
            const auto ip = entry.ip();
            if (ip.protocol() != QAbstractSocket::IPv4Protocol || ip.isLinkLocal()) continue;
            const auto value = ip.toString();
            if (bind != "0.0.0.0" && bind != value) continue;
            auto &addresses = iface.type() == QNetworkInterface::Ethernet || iface.type() == QNetworkInterface::Wifi
                ? physical : other;
            addresses.append({value + "  ·  " + iface.humanReadableName(), value});
        }
    }
    for (const auto &entry : physical + other) box->addItem(entry.first, entry.second);
    const int index = box->findData(selected);
    if (index >= 0) box->setCurrentIndex(index);
    if (box->count() == 0) box->addItem("未找到符合监听地址的 IPv4 · 检查网卡");
}

inline bool animationsEnabled()
{
    BOOL enabled = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0);
    return enabled;
}

inline HGLOBAL clipboardData(const void *buffer, SIZE_T bytes)
{
    auto memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) return nullptr;
    auto *data = GlobalLock(memory);
    if (!data) {
        GlobalFree(memory);
        return nullptr;
    }
    std::memcpy(data, buffer, bytes);
    GlobalUnlock(memory);
    return memory;
}

inline DWORD copyCredential(HWND owner, const QString &value)
{
    const UINT excluded = RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing");
    const DWORD zero = 0;
    auto flag = clipboardData(&zero, sizeof(zero));
    auto text = clipboardData(value.utf16(), SIZE_T(value.size() + 1) * sizeof(ushort));
    DWORD sequence = 0;
    if (excluded && flag && text && OpenClipboard(owner)) {
        if (EmptyClipboard() && SetClipboardData(excluded, flag)) {
            flag = nullptr;
            if (SetClipboardData(CF_UNICODETEXT, text)) {
                text = nullptr;
                sequence = GetClipboardSequenceNumber();
            }
        }
        CloseClipboard();
    }
    if (flag) GlobalFree(flag);
    if (text) GlobalFree(text);
    return sequence;
}

inline bool clearCredential(HWND owner, DWORD sequence)
{
    if (!sequence || GetClipboardSequenceNumber() != sequence) return true;
    if (!OpenClipboard(owner)) return false;
    if (GetClipboardSequenceNumber() == sequence) EmptyClipboard();
    CloseClipboard();
    return true;
}

inline int changePassword(const QString &root)
{
    QDialog dialog;
    dialog.setWindowTitle("MiniStream · 更换隧道口令");
    dialog.setMinimumWidth(440);
    auto *form = new QFormLayout(&dialog);
    auto *hint = new QLabel("输入 12–64 个字母、数字或 _ . @ -。若隧道正在运行，应用新口令时会短暂断开当前会话。");
    hint->setWordWrap(true);
    form->addRow(hint);
    auto *password = new QLineEdit, *confirm = new QLineEdit;
    for (auto *edit : {password, confirm}) {
        edit->setMaxLength(64);
        edit->setEchoMode(QLineEdit::Password);
    }
    form->addRow("新口令", password);
    form->addRow("再输入一次", confirm);
    auto *actions = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    actions->button(QDialogButtonBox::Save)->setText("应用新口令");
    actions->button(QDialogButtonBox::Cancel)->setText("取消");
    auto *result = new QLabel;
    result->setWordWrap(true);
    form->addRow(result);
    form->addRow(actions);
    QProcess process;
    process.setStandardOutputFile(QProcess::nullDevice());
    process.setStandardErrorFile(QProcess::nullDevice());
    process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) { args->flags |= CREATE_NO_WINDOW; });
    QObject::connect(actions, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(actions, &QDialogButtonBox::accepted, &dialog, [&] {
        if (!validPassword(password->text()) || password->text() != confirm->text()) {
            result->setText("请核对格式与两次输入。");
            return;
        }
        actions->setEnabled(false);
        password->setEnabled(false);
        confirm->setEnabled(false);
        result->setText("正在应用并检查隧道，请稍候…");
        process.start(powerShell(), {"-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
            root + "/_internal/gui-action.ps1", "-Root", root, "-Action", "password", "-Component", "tunnel"});
    });
    QObject::connect(&process, &QProcess::started, &dialog, [&] {
        process.write(password->text().toLatin1() + '\n');
        process.closeWriteChannel();
        password->clear();
        confirm->clear();
    });
    auto failed = [&](const QString &message) {
        result->setText(message);
        actions->setEnabled(true);
        password->setEnabled(true);
        confirm->setEnabled(true);
    };
    QObject::connect(&process, &QProcess::errorOccurred, &dialog, [&](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) failed("无法启动配置工具。请检查安装文件。");
    });
    QObject::connect(&process, &QProcess::finished, &dialog, [&](int code, QProcess::ExitStatus status) {
        if (status == QProcess::NormalExit && code == 0) {
            dialog.accept();
        } else {
            failed(code == 3 ? "新口令未生效，已恢复原口令。请运行自检确认隧道状态。"
                : code == 4 ? "应用或恢复时出现错误。请检查管理日志和当前隧道状态。"
                : "未能更新口令，请检查管理日志。");
        }
    });
    // Keep the transaction alive if the user presses Esc or closes the elevated dialog.
    bool running = false;
    QObject::connect(&process, &QProcess::stateChanged, &dialog, [&](QProcess::ProcessState state) { running = state != QProcess::NotRunning; });
    class Guard final : public QObject {
        bool &busy;
    public:
        explicit Guard(bool &busy) : busy(busy) {}
        bool eventFilter(QObject *, QEvent *event) override {
            if (!busy) return false;
            if (event->type() == QEvent::Close) {
                static_cast<QCloseEvent *>(event)->ignore();
                return true;
            }
            return event->type() == QEvent::KeyPress && static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape;
        }
    } guard(running);
    dialog.installEventFilter(&guard);
    return dialog.exec() == QDialog::Accepted ? 0 : 5;
}

}
