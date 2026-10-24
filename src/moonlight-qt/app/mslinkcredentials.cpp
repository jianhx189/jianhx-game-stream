#include "mslinkcredentials.h"

#ifdef Q_OS_WIN
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <dpapi.h>
#endif

namespace MslinkCredentials {

namespace {

// "dpapi:v1:" 的长度（不含结尾 NUL）
constexpr int kPrefixLen = int(sizeof(kDpapiPrefix) - 1);

#ifdef Q_OS_WIN
QString win32Error(DWORD code) {
    wchar_t* buf = nullptr;
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                   FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, code,
                                   MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                   reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    QString msg;
    if (n && buf)
        msg = QString::fromWCharArray(buf, int(n)).trimmed();
    if (buf)
        LocalFree(buf);
    if (msg.isEmpty())
        return QStringLiteral("Windows 错误码 %1").arg(qulonglong(code));
    return QStringLiteral("%1（错误码 %2）").arg(msg, QString::number(qulonglong(code)));
}
#endif

} // namespace

void wipe(QByteArray& buf) {
    if (!buf.isEmpty()) {
        volatile char* p = buf.data();
        const int n = int(buf.size());
        for (int i = 0; i < n; ++i)
            p[i] = 0;
    }
    buf.clear();
}

bool encodeSecret(const QString& plain, QString* encoded, QString* err) {
    if (encoded) encoded->clear();
#ifdef Q_OS_WIN
    QByteArray plainBytes = plain.toUtf8();

    DATA_BLOB in;
    in.pbData = reinterpret_cast<BYTE*>(plainBytes.data());
    in.cbData = DWORD(plainBytes.size());

    static const wchar_t kDescription[] =
            L"MiniStream mslink.ini credential (current Windows user)";

    DATA_BLOB out;
    ZeroMemory(&out, sizeof(out));
    const BOOL ok = CryptProtectData(&in, kDescription, nullptr, nullptr, nullptr,
                                     CRYPTPROTECT_UI_FORBIDDEN, &out);
    const DWORD lastErr = ok ? ERROR_SUCCESS : GetLastError();
    wipe(plainBytes);

    if (!ok) {
        if (err) *err = QStringLiteral("CryptProtectData 失败：%1").arg(win32Error(lastErr));
        return false;
    }

    QByteArray blob(reinterpret_cast<const char*>(out.pbData), int(out.cbData));
    if (out.pbData) {
        SecureZeroMemory(out.pbData, out.cbData);
        LocalFree(out.pbData);
        out.pbData = nullptr;
    }
    if (encoded)
        *encoded = QString::fromLatin1(kDpapiPrefix) + QString::fromLatin1(blob.toBase64());
    wipe(blob);
    return true;
#else
    Q_UNUSED(plain);
    if (err) *err = QStringLiteral("当前平台不支持 Windows DPAPI（仅 Windows 可用）");
    return false;
#endif
}

bool decodeSecret(const QString& encoded, QString* plain, QString* err) {
    if (plain) plain->clear();
    if (!encoded.startsWith(QLatin1String(kDpapiPrefix))) {
        if (err) *err = QStringLiteral("不是 dpapi:v1 密文");
        return false;
    }
#ifdef Q_OS_WIN
    QByteArray blob = QByteArray::fromBase64(encoded.mid(kPrefixLen).toLatin1());
    if (blob.isEmpty()) {
        if (err) *err = QStringLiteral("密文格式不合法（base64 为空或无法解析）");
        return false;
    }

    DATA_BLOB in;
    in.pbData = reinterpret_cast<BYTE*>(blob.data());
    in.cbData = DWORD(blob.size());

    DATA_BLOB out;
    ZeroMemory(&out, sizeof(out));
    const BOOL ok = CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr,
                                       CRYPTPROTECT_UI_FORBIDDEN, &out);
    const DWORD lastErr = ok ? ERROR_SUCCESS : GetLastError();
    wipe(blob);

    if (!ok) {
        if (err) *err = QStringLiteral("CryptUnprotectData 失败：%1").arg(win32Error(lastErr));
        return false;
    }

    QByteArray plainBytes(reinterpret_cast<const char*>(out.pbData), int(out.cbData));
    if (out.pbData) {
        SecureZeroMemory(out.pbData, out.cbData);
        LocalFree(out.pbData);
        out.pbData = nullptr;
    }
    if (plain) *plain = QString::fromUtf8(plainBytes);
    wipe(plainBytes);
    return true;
#else
    Q_UNUSED(encoded);
    if (err) *err = QStringLiteral("当前平台不支持 Windows DPAPI（仅 Windows 可用）");
    return false;
#endif
}

bool decodeStored(const QString& stored, QString* plain, bool* wasPlaintext, QString* err) {
    if (plain) plain->clear();
    if (wasPlaintext) *wasPlaintext = false;
    if (stored.isEmpty())
        return true;   // 没配置，不是错误
    if (!stored.startsWith(QLatin1String(kDpapiPrefix))) {
        if (plain) *plain = stored;
        if (wasPlaintext) *wasPlaintext = true;
        return true;
    }
    return decodeSecret(stored, plain, err);
}

} // namespace MslinkCredentials
