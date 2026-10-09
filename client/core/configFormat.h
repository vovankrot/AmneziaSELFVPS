#pragma once
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <cmath>
namespace ConfigFormat {
constexpr int current = 0; // Same schema revision as upstream; application version is separate.
// Only Windows x64 ships the audited AWG 3.1 library. Other backends retain
// their compatibility gate until their native libraries are upgraded too.
#if defined(Q_OS_WIN) && defined(Q_PROCESSOR_X86_64)
constexpr bool nativeAwg31 = true;
#else
constexpr bool nativeAwg31 = false;
#endif
inline bool backendCompatible(const QJsonValue &value, bool awg31 = nativeAwg31)
{
    if (value.isObject()) {
        const auto object = value.toObject();
        if (!awg31 && (object.contains("RandomTrailers") || object.contains("DisableCookies"))) return false;
        for (const auto &child : object) if (!backendCompatible(child, awg31)) return false;
    } else if (value.isArray()) {
        for (const auto &child : value.toArray()) if (!backendCompatible(child, awg31)) return false;
    } else if (value.isString()) {
        const QString text = value.toString();
        if (!awg31 && QRegularExpression("(?m)^[ \\t]*(RandomTrailers|DisableCookies)[ \\t]*=").match(text).hasMatch()) return false;
        // Native protocol JSON can be serialized inside last_config.
        if (text.trimmed().startsWith('{')) {
            const auto document = QJsonDocument::fromJson(text.toUtf8());
            if (document.isObject() && !backendCompatible(document.object(), awg31)) return false;
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
