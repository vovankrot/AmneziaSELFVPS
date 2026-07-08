#ifndef DIAGNOSTICSCONTROLLER_H
#define DIAGNOSTICSCONTROLLER_H

#include <QObject>
#include <QVariantList>
#include <QVariantMap>
#include <QQueue>
#include <memory>

class Settings;

/**
 * @brief Controller managing server diagnostic issues queue.
 * 
 * Issues are displayed one at a time. After resolving an issue, 
 * the next one is shown automatically.
 */
class DiagnosticsController : public QObject
{
    Q_OBJECT

    // Current issue to display (empty map if no issues)
    Q_PROPERTY(QVariantMap currentIssue READ currentIssue NOTIFY currentIssueChanged)
    
    // Whether there are any issues pending
    Q_PROPERTY(bool hasIssues READ hasIssues NOTIFY hasIssuesChanged)
    
    // Is currently processing/resolving an issue
    Q_PROPERTY(bool isResolving READ isResolving NOTIFY isResolvingChanged)
    
    // Total count of pending issues
    Q_PROPERTY(int issueCount READ issueCount NOTIFY issueCountChanged)

public:
    enum Severity {
        Info = 0,
        Warning = 1,
        Error = 2,
        Critical = 3
    };
    Q_ENUM(Severity)

    explicit DiagnosticsController(std::shared_ptr<Settings> settings, QObject *parent = nullptr);
    ~DiagnosticsController() override;

    QVariantMap currentIssue() const;
    bool hasIssues() const;
    bool isResolving() const;
    int issueCount() const;

    /**
     * @brief Add a diagnostic issue to the queue
     * @param issueId Unique identifier (used to prevent duplicates)
     * @param message Human-readable description
      * @param details Optional detailed explanation shown in the banner body
     * @param actionText Text for the action button
     * @param severity Issue severity level
     * @param actionType Action identifier for resolveCurrentIssue
     */
    Q_INVOKABLE void addIssue(const QString &issueId, 
                               const QString &message,
                             const QString &details,
                               const QString &actionText,
                               int severity = Warning,
                               const QString &actionType = QString());

    /**
     * @brief Remove an issue by ID (e.g., when condition resolved externally)
     */
    Q_INVOKABLE void removeIssue(const QString &issueId);

    /**
     * @brief Start resolving the current issue
     * Emits resolveRequested with the actionType
     */
    Q_INVOKABLE void resolveCurrentIssue();

    /**
     * @brief Mark current issue as resolved and move to next
     * @param success Whether resolution was successful
     * @param resultMessage Optional message to display
     */
    Q_INVOKABLE void markResolved(bool success, const QString &resultMessage = QString());

    /**
     * @brief Skip current issue (move to next without resolving)
     */
    Q_INVOKABLE void skipCurrentIssue();

    /**
     * @brief Clear all issues
     */
    Q_INVOKABLE void clearAll();

    /**
     * @brief Check if issue with given ID exists
     */
    Q_INVOKABLE bool hasIssue(const QString &issueId) const;

signals:
    void currentIssueChanged();
    void hasIssuesChanged();
    void isResolvingChanged();
    void issueCountChanged();
    
    /**
     * @brief Emitted when user requests to resolve an issue
     * @param actionType Type of action to perform
     * @param issueId Issue being resolved
     */
    void resolveRequested(const QString &actionType, const QString &issueId);
    
    /**
     * @brief Emitted when an issue is resolved
     * @param issueId ID of resolved issue
     * @param success Whether resolution was successful
     * @param message Result message
     */
    void issueResolved(const QString &issueId, bool success, const QString &message);

private:
    struct Issue {
        QString id;
        QString message;
        QString details;
        QString actionText;
        Severity severity;
        QString actionType;
        
        QVariantMap toVariantMap() const;
    };

    std::shared_ptr<Settings> m_settings;
    QQueue<Issue> m_issues;
    QSet<QString> m_issueIds; // Track IDs for duplicate prevention
    bool m_isResolving = false;
    
    void emitAllChanges();
};

#endif // DIAGNOSTICSCONTROLLER_H
