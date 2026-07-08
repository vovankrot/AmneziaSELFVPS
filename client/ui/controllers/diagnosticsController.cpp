#include "diagnosticsController.h"
#include "settings.h"
#include "logger.h"

namespace {
Logger logger("DiagnosticsController");
}

DiagnosticsController::DiagnosticsController(std::shared_ptr<Settings> settings, QObject *parent)
    : QObject(parent)
    , m_settings(settings)
{
    logger.info() << "DiagnosticsController created";
}

DiagnosticsController::~DiagnosticsController()
{
}

QVariantMap DiagnosticsController::Issue::toVariantMap() const
{
    QVariantMap map;
    map["id"] = id;
    map["message"] = message;
    map["details"] = details;
    map["actionText"] = actionText;
    map["severity"] = static_cast<int>(severity);
    map["actionType"] = actionType;
    return map;
}

QVariantMap DiagnosticsController::currentIssue() const
{
    if (m_issues.isEmpty()) {
        return QVariantMap();
    }
    return m_issues.head().toVariantMap();
}

bool DiagnosticsController::hasIssues() const
{
    return !m_issues.isEmpty();
}

bool DiagnosticsController::isResolving() const
{
    return m_isResolving;
}

int DiagnosticsController::issueCount() const
{
    return m_issues.size();
}

void DiagnosticsController::addIssue(const QString &issueId,
                                      const QString &message,
                                      const QString &details,
                                      const QString &actionText,
                                      int severity,
                                      const QString &actionType)
{
    // Prevent duplicates
    if (m_issueIds.contains(issueId)) {
        logger.debug() << "Issue already exists:" << issueId;
        return;
    }

    Issue issue;
    issue.id = issueId;
    issue.message = message;
    issue.details = details;
    issue.actionText = actionText;
    issue.severity = static_cast<Severity>(severity);
    issue.actionType = actionType.isEmpty() ? issueId : actionType;

    m_issues.enqueue(issue);
    m_issueIds.insert(issueId);
    
    logger.info() << "Issue added:" << issueId << "| Severity:" << severity 
                  << "| Queue size:" << m_issues.size();

    emitAllChanges();
}

void DiagnosticsController::removeIssue(const QString &issueId)
{
    if (!m_issueIds.contains(issueId)) {
        return;
    }

    // Find and remove issue from queue
    QQueue<Issue> newQueue;
    for (const Issue &issue : m_issues) {
        if (issue.id != issueId) {
            newQueue.enqueue(issue);
        }
    }
    m_issues = newQueue;
    m_issueIds.remove(issueId);
    
    logger.info() << "Issue removed:" << issueId << "| Queue size:" << m_issues.size();

    emitAllChanges();
}

void DiagnosticsController::resolveCurrentIssue()
{
    if (m_issues.isEmpty() || m_isResolving) {
        return;
    }

    m_isResolving = true;
    emit isResolvingChanged();

    const Issue &current = m_issues.head();
    logger.info() << "Resolving issue:" << current.id << "| Action:" << current.actionType;

    emit resolveRequested(current.actionType, current.id);
}

void DiagnosticsController::markResolved(bool success, const QString &resultMessage)
{
    if (m_issues.isEmpty()) {
        m_isResolving = false;
        emit isResolvingChanged();
        return;
    }
    m_isResolving = false;

    if (success) {
        Issue resolved = m_issues.dequeue();
        m_issueIds.remove(resolved.id);

        logger.info() << "Issue resolved:" << resolved.id
                      << "| Success:" << success
                      << "| Remaining:" << m_issues.size();

        emit issueResolved(resolved.id, true, resultMessage);
        emitAllChanges();
        return;
    }

    Issue &current = m_issues.head();
    const QString trimmedResult = resultMessage.trimmed();
    current.message = trimmedResult.isEmpty()
            ? tr("Failed to apply fix.")
            : tr("Failed to apply fix. Review the error below.");
    current.details = trimmedResult;
    current.actionText = tr("Retry");
    if (current.severity < Error) {
        current.severity = Error;
    }

    logger.warning() << "Issue resolution failed:" << current.id
                     << "| Details:" << trimmedResult;

    emit issueResolved(current.id, false, trimmedResult);
    emitAllChanges();
}

void DiagnosticsController::skipCurrentIssue()
{
    if (m_issues.isEmpty()) {
        return;
    }

    // Move current issue to back of queue
    Issue skipped = m_issues.dequeue();
    m_issues.enqueue(skipped);
    
    m_isResolving = false;

    logger.info() << "Issue skipped:" << skipped.id << "| Moved to back of queue";

    emit isResolvingChanged();
    emit currentIssueChanged();
}

void DiagnosticsController::clearAll()
{
    m_issues.clear();
    m_issueIds.clear();
    m_isResolving = false;
    
    logger.info() << "All issues cleared";

    emitAllChanges();
}

bool DiagnosticsController::hasIssue(const QString &issueId) const
{
    return m_issueIds.contains(issueId);
}

void DiagnosticsController::emitAllChanges()
{
    emit currentIssueChanged();
    emit hasIssuesChanged();
    emit isResolvingChanged();
    emit issueCountChanged();
}
