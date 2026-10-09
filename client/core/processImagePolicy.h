#pragma once
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

namespace ProcessImagePolicy {
inline QString expectedPath(const QString &program)
{
    if (program.isEmpty()) return {};
    const QFileInfo requested(program);
    const QString path = requested.isAbsolute() ? program
        : QDir(QCoreApplication::applicationDirPath()).filePath(program);
    return QFileInfo(path).canonicalFilePath();
}
inline bool matches(const QString &expected, const QString &image)
{
    if (expected.isEmpty() || image.isEmpty()) return false;
    const QString actual = QFileInfo(image).canonicalFilePath();
    return !actual.isEmpty() && expected.compare(actual, Qt::CaseInsensitive) == 0;
}
}
