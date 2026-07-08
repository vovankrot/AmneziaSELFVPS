#ifndef SERVERTERMINALCONTROLLER_H
#define SERVERTERMINALCONTROLLER_H

#include <QObject>
#include <QMutex>
#include <QString>
#include <QSharedPointer>

#include "settings.h"
#include "ui/models/servers_model.h"

class ServerTerminalController : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QString outputText READ outputText NOTIFY outputChanged)
    Q_PROPERTY(bool isBusy READ isBusy NOTIFY isBusyChanged)

public:
    explicit ServerTerminalController(const QSharedPointer<ServersModel> &serversModel,
                                      const std::shared_ptr<Settings> &settings,
                                      QObject *parent = nullptr);
    ~ServerTerminalController() override;

    QString outputText() const;
    bool isBusy() const;

public slots:
    void executeCommand(const QString &command);
    void fetchXrayLogs();
    void fetchDockerLogs();
    void fetchSystemErrors();
    void fetchSshAuthLog();
    void clearOutput();

signals:
    void outputChanged();
    void isBusyChanged();
    void commandFinished(bool success);

private:
    void runRemoteCommand(const QString &cmd);
    void appendOutput(const QString &text);
    bool isDangerousCommand(const QString &cmd) const;

    QSharedPointer<ServersModel> m_serversModel;
    std::shared_ptr<Settings> m_settings;

    QString m_output;
    bool m_isBusy = false;
    std::shared_ptr<std::atomic<bool>> m_alive;

    static constexpr int MaxOutputSize = 512 * 1024; // 512 KB
};

#endif // SERVERTERMINALCONTROLLER_H
