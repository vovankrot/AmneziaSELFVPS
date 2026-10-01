#pragma once
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <cmath>
namespace ConfigFormat {
constexpr int current = 0; // Same schema revision as upstream; application version is separate.
inline bool backendCompatible(const QJsonValue &value)
{
    if (value.isObject()) {
        const auto object = value.toObject();
        if (object.contains("RandomTrailers") || object.contains("DisableCookies")) return false;
        for (const auto &child : object) if (!backendCompatible(child)) return false;
    } else if (value.isArray()) {
        for (const auto &child : value.toArray()) if (!backendCompatible(child)) return false;
    } else if (value.isString()) {
        const QString text = value.toString();
        if (QRegularExpression("(?m)^[ \\t]*(RandomTrailers|DisableCookies)[ \\t]*=").match(text).hasMatch()) return false;
        // Native protocol JSON can be serialized inside last_config.
        if (text.trimmed().startsWith('{')) {
            const auto document = QJsonDocument::fromJson(text.toUtf8());
            if (document.isObject() && !backendCompatible(document.object())) return false;
        }
    }
    return true;
}
inline bool supported(const QJsonObject &object)
{
    if (!backendCompatible(object)) return false;
    if (!object.contains("formatVersion")) return true;
    const auto value = object.value("formatVersion");
    return value.isDouble() && std::isfinite(value.toDouble())
        && value.toDouble() == current;
}
inline QJsonObject stamp(QJsonObject object) { object["formatVersion"] = current; return object; }
}
