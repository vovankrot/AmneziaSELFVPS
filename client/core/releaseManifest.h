#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QUrl>
#include <QVersionNumber>

namespace SelfVpsRelease {
struct Release {
    QString version, filename, sha256, notes;
    QUrl download, page;
    qint64 size = 0;
    bool valid() const { return !sha256.isEmpty(); }
};
inline QVersionNumber version(QString text)
{
    static const QRegularExpression pattern(QStringLiteral("^v?([0-9]+\\.[0-9]+\\.[0-9]+(?:\\.[0-9]+)?)(?:-selfvps)?$"));
    const auto match = pattern.match(text);
    return match.hasMatch() ? QVersionNumber::fromString(match.captured(1)) : QVersionNumber();
}
inline bool repositoryAllowed(const QString &repo)
{
    return repo == QStringLiteral("vovankrot/AmneziaSELFVPS") || repo == QStringLiteral("vovankrot/selfvps");
}
inline bool githubUrl(const QUrl &url, const QString &pathPrefix)
{
    return url.scheme() == QStringLiteral("https") && url.host() == QStringLiteral("github.com")
        && url.userInfo().isEmpty() && (url.port() == -1 || url.port() == 443)
        && url.path().startsWith(pathPrefix) && url.query().isEmpty() && url.fragment().isEmpty();
}
inline Release parse(const QJsonObject &json, const QString &repo, const QString &current)
{
    Release result;
    if (!repositoryAllowed(repo) || json.value("draft").toBool(true) || json.value("prerelease").toBool(true)) return result;
    const auto tag = json.value("tag_name").toString();
    const auto remote = version(tag), local = version(current);
    if (remote.isNull() || local.isNull() || QVersionNumber::compare(remote.normalized(), local.normalized()) <= 0) return result;
    result.version = remote.toString();
    result.filename = QStringLiteral("AmneziaVPN_%1_x64_setup.exe").arg(result.version);
    result.page = QUrl(json.value("html_url").toString());
    if (!githubUrl(result.page, "/" + repo + "/releases/tag/") || result.page.path() != "/" + repo + "/releases/tag/" + tag) return {};
    int count = 0;
    for (const auto &asset : json.value("assets").toArray()) {
        const auto item = asset.toObject();
        if (item.value("name").toString() != result.filename) continue;
        ++count;
        const auto digest = item.value("digest").toString();
        static const QRegularExpression hashPattern(QStringLiteral("^sha256:([a-fA-F0-9]{64})$"));
        const auto hash = hashPattern.match(digest);
        result.size = item.value("size").toInteger();
        result.download = QUrl(item.value("browser_download_url").toString());
        if (!hash.hasMatch() || result.size <= 0 || result.size > 512LL * 1024 * 1024
            || item.value("state").toString() != "uploaded"
            || !githubUrl(result.download, "/" + repo + "/releases/download/")
            || result.download.path() != "/" + repo + "/releases/download/" + tag + "/" + result.filename) return {};
        result.sha256 = hash.captured(1).toLower();
    }
    if (count != 1) return {};
    result.notes = json.value("body").toString().left(16000);
    return result;
}
}
