#pragma once
#include <QDebug>
class Logger {
public:
    explicit Logger(const char *) {}
    QDebug error() const { return qWarning(); }
    QDebug info() const { return qInfo(); }
};
