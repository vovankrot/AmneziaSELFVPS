#pragma once
#include <QCryptographicHash>
#include <QFile>
#include <atomic>
#include <memory>

namespace InstallerFileCheck {
enum class Result { Valid, Missing, Changed, Canceled };
inline Result run(const QString &path, qint64 expectedSize, const QByteArray &expectedHash,
                  const std::shared_ptr<std::atomic_bool> &canceled)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return Result::Missing;
    if (file.size() != expectedSize) return Result::Changed;
    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 received = 0;
    while (!file.atEnd()) {
        if (canceled->load()) return Result::Canceled;
        const auto bytes = file.read(1024 * 1024);
        if (bytes.isEmpty()) return Result::Changed;
        received += bytes.size();
        if (received > expectedSize) return Result::Changed;
        hash.addData(bytes);
    }
    if (canceled->load()) return Result::Canceled;
    return received == expectedSize && file.size() == expectedSize
        && hash.result().toHex() == expectedHash ? Result::Valid : Result::Changed;
}
}
