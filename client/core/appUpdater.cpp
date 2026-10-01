#include "appUpdater.h"
#include "version.h"
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QJsonDocument>
#include <QSettings>
#include <QStandardPaths>
#ifdef Q_OS_WIN
#include <windows.h>
#include <shellapi.h>
#endif

AppUpdater::AppUpdater(QObject *parent, QNetworkAccessManager *network) : QObject(parent),
    m_network(network ? network : new QNetworkAccessManager(this))
{
    m_periodic.setInterval(6 * 60 * 60 * 1000);
    connect(&m_periodic, &QTimer::timeout, this, [this] { check(false); });
    m_periodic.start();
    QTimer::singleShot(15000, this, [this] { check(false); });
}
bool AppUpdater::supported() const
{
#if defined(Q_OS_WIN) && defined(_WIN64)
    return true;
#else
    return false;
#endif
}
QString AppUpdater::repository() const
{
    const auto repo = QSettings().value("Updates/repository", "vovankrot/AmneziaSELFVPS").toString();
    return SelfVpsRelease::repositoryAllowed(repo) ? repo : QStringLiteral("vovankrot/AmneziaSELFVPS");
}
void AppUpdater::setRepository(const QString &repo)
{
    if (busy() || !SelfVpsRelease::repositoryAllowed(repo) || repo == repository()) return;
    QSettings().setValue("Updates/repository", repo);
    m_release = {}; m_readyPath.clear(); m_status.clear(); emit changed();
}
bool AppUpdater::automatic() const { return QSettings().value("Updates/automatic", true).toBool(); }
void AppUpdater::setAutomatic(bool value) { QSettings().setValue("Updates/automatic", value); emit changed(); }
QNetworkRequest AppUpdater::request(const QUrl &url) const
{
    QNetworkRequest req(url);
    req.setRawHeader("User-Agent", QByteArray("SELFVPS/") + APP_VERSION);
    req.setRawHeader("Accept", "application/vnd.github+json");
    req.setTransferTimeout(30000);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    return req;
}
void AppUpdater::fail(const QString &message)
{
    if (m_failed) return;
    m_failed = true; m_status = message;
    if (m_file) m_file->cancelWriting();
    if (m_reply && m_reply->isRunning()) m_reply->abort();
    emit changed();
}
void AppUpdater::check(bool manual)
{
    if (!supported() || busy() || (!manual && !automatic())) return;
    m_failed = false; m_metadata.clear();
    m_status = tr("Проверяем релизы GitHub…");
    const auto repo = repository();
    auto req = request(QUrl("https://api.github.com/repos/" + repo + "/releases/latest"));
    req.setRawHeader("X-GitHub-Api-Version", "2022-11-28");
    auto reply = m_network->get(req); m_reply = reply; reply->setReadBufferSize(256 * 1024);
    QTimer::singleShot(60000, reply, [reply] { if (reply->isRunning()) reply->abort(); });
    connect(reply, &QNetworkReply::readyRead, this, [this, reply] {
        m_metadata += reply->readAll();
        if (m_metadata.size() > 2 * 1024 * 1024) fail(tr("Ответ GitHub слишком большой."));
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, repo] {
        const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (!m_failed) {
            if (code == 404) m_status = tr("В этом репозитории пока нет опубликованного релиза.");
            else if (code == 403 || code == 429) m_status = tr("GitHub ограничил запросы. Повторите проверку позднее.");
            else if (reply->error() != QNetworkReply::NoError || code != 200) m_status = tr("Не удалось проверить обновления: %1").arg(reply->errorString());
            else {
                m_metadata += reply->readAll();
                QJsonParseError error;
                const auto json = QJsonDocument::fromJson(m_metadata, &error);
                if (m_metadata.size() > 2 * 1024 * 1024 || error.error != QJsonParseError::NoError || !json.isObject()) m_status = tr("GitHub вернул некорректные данные.");
                else {
                    const auto candidate = SelfVpsRelease::parse(json.object(), repo, APP_VERSION);
                    if (candidate.valid()) {
                        const bool newOffer = candidate.sha256 != m_release.sha256;
                        if (newOffer) { m_release = candidate; m_readyPath.clear(); }
                        m_status = tr("Доступна версия %1.").arg(candidate.version);
                        if (newOffer) emit updateAvailable();
                    } else {
                        const auto remote = SelfVpsRelease::version(json.object().value("tag_name").toString());
                        if (!remote.isNull() && QVersionNumber::compare(remote.normalized(), SelfVpsRelease::version(APP_VERSION).normalized()) <= 0) {
                            m_release = {}; m_readyPath.clear(); m_status = tr("Установлена актуальная версия.");
                        } else m_status = tr("Релиз не содержит подходящего x64 установщика с SHA-256. Автоустановка недоступна.");
                    }
                }
            }
        }
        m_reply.clear(); reply->deleteLater(); m_metadata.clear(); emit changed();
    });
    emit changed();
}
void AppUpdater::download()
{
    if (busy() || !supported() || !m_release.valid()) return;
    const auto folder = m_downloadRoot.isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/updates" : m_downloadRoot;
    if (!QDir().mkpath(folder)) { m_status = tr("Не удалось создать каталог обновлений."); emit changed(); return; }
    m_readyPath.clear(); m_failed = false; m_received = 0; m_progress = 0;
    m_file = std::make_unique<QSaveFile>(folder + "/" + m_release.sha256 + ".exe");
    if (!m_file->open(QIODevice::WriteOnly)) { m_status = m_file->errorString(); m_file.reset(); emit changed(); return; }
    m_file->setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    m_hash = std::make_unique<QCryptographicHash>(QCryptographicHash::Sha256);
    m_status = tr("Скачиваем %1…").arg(m_release.version);
    auto reply = m_network->get(request(m_release.download)); m_reply = reply; reply->setReadBufferSize(256 * 1024);
    QTimer::singleShot(20 * 60 * 1000, reply, [reply] { if (reply->isRunning()) reply->abort(); });
    connect(reply, &QNetworkReply::readyRead, this, [this, reply] {
        if (m_failed) return;
        const auto data = reply->readAll(); m_received += data.size();
        if (m_received > m_release.size || m_file->write(data) != data.size()) { fail(tr("Ошибка размера или записи установщика.")); return; }
        m_hash->addData(data); m_progress = int(m_received * 100 / m_release.size); emit changed();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        if (!m_failed) {
            const auto tail = reply->readAll(); m_received += tail.size();
            if (m_file->write(tail) != tail.size()) fail(tr("Ошибка записи установщика."));
            else m_hash->addData(tail);
        }
        if (!m_failed && (reply->error() != QNetworkReply::NoError
            || reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200
            || m_received != m_release.size || m_hash->result().toHex() != m_release.sha256.toLatin1())) {
            fail(tr("Загрузка не прошла проверку SHA-256/размера. Установка отменена."));
        }
        if (!m_failed) {
            const auto path = m_file->fileName();
            if (!m_file->commit()) fail(tr("Не удалось сохранить обновление."));
            else { m_readyPath = path; m_status = tr("Обновление проверено. Можно установить с сохранением настроек."); m_progress = 100; }
        }
        m_file.reset(); m_hash.reset(); m_reply.clear(); reply->deleteLater(); emit changed();
    });
    emit changed();
}
void AppUpdater::cancel() { if (busy()) fail(tr("Операция отменена.")); }
void AppUpdater::openRelease() { if (m_release.valid()) QDesktopServices::openUrl(m_release.page); }
void AppUpdater::install()
{
    if (!ready() || busy() || !supported()) return;
    QFile file(m_readyPath);
    if (!file.open(QIODevice::ReadOnly) || file.size() != m_release.size) { m_status = tr("Файл обновления недоступен."); emit changed(); return; }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file) || hash.result().toHex() != m_release.sha256.toLatin1()) { m_readyPath.clear(); m_status = tr("Установщик изменился. Скачайте обновление заново."); emit changed(); return; }
    file.close();
#ifdef Q_OS_WIN
    const auto path = QDir::toNativeSeparators(m_readyPath).toStdWString();
    SHELLEXECUTEINFOW info = {}; info.cbSize = sizeof(info); info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.lpVerb = L"runas"; info.lpFile = path.c_str(); info.lpParameters = L"/update"; info.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&info)) { m_status = tr("Установка не запущена: UAC отменён или произошла ошибка %1.").arg(GetLastError()); emit changed(); return; }
    if (info.hProcess) CloseHandle(info.hProcess);
    QCoreApplication::quit();
#endif
}
