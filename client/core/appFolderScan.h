#pragma once

#include <QDirIterator>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QSet>
#include "core/defs.h"

namespace AppFolderScan {
enum class Error { None, MissingFolder, LimitExceeded };
struct Result { QVector<amnezia::InstalledAppInfo> apps; Error error = Error::None; };

inline Result scan(const QString &folder, int limit = 4096, int timeoutMs = 10000)
{
    Result result;
    const QString root = QFileInfo(folder).canonicalFilePath();
    if (root.isEmpty() || !QFileInfo(root).isDir()) { result.error = Error::MissingFolder; return result; }
    const QString prefix = root.endsWith('/') ? root : root + '/';
    QElapsedTimer timer;
    timer.start();
    QSet<QString> seen;
    // Visit ordinary files too, so a directory full of non-EXE files still
    // yields regularly to our time budget instead of hiding that work in hasNext().
    QDirIterator iterator(root, QDir::Files | QDir::NoSymLinks, QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        if (timer.elapsed() >= timeoutMs) {
            result.apps.clear(); result.error = Error::LimitExceeded; return result;
        }
        const QFileInfo file(iterator.next());
        if (file.suffix().compare("exe", Qt::CaseInsensitive) != 0) continue;
        const QString path = file.canonicalFilePath();
        if (!path.startsWith(prefix, Qt::CaseInsensitive) || seen.contains(path.toLower())) continue;
        if (result.apps.size() >= limit) {
            result.apps.clear(); result.error = Error::LimitExceeded; return result;
        }
        seen.insert(path.toLower());
        result.apps.append({file.fileName(), "", path, root});
    }
    return result;
}
}
