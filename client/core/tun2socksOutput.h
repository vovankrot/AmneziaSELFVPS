#pragma once

#include <QByteArray>
#include <QStringList>
#include <QRegularExpression>
#include "../../common/logger/logRedaction.h"

// IPC reads need not end on a line boundary. Never retain or log an oversized
// fragment: cutting a proxy URL in half could also defeat credential redaction.
class Tun2SocksOutput
{
public:
    struct Result { bool ready = false; QStringList diagnostics; };
    Result feed(const QByteArray &bytes)
    {
        Result result;
        for (char c : bytes) {
            if (c == '\n') {
                if (!m_discarding) {
                    const QString line = QString::fromUtf8(m_tail).trimmed();
                    if (!m_ready && line.contains("[STACK] tun://") && line.contains("<-> socks5://")) {
                        m_ready = true;
                        result.ready = true;
                    }
                    static const QRegularExpression problem(
                        R"(\b(?:warn(?:ing)?|error|fatal|panic)\b)", QRegularExpression::CaseInsensitiveOption);
                    const bool flow = line.contains("[TCP]") || line.contains("[UDP]");
                    if (!line.isEmpty() && (!flow || line.contains(problem)) && result.diagnostics.size() < 32)
                        result.diagnostics.append(LogRedaction::hideProxyCredentials(line));
                }
                m_tail.clear();
                m_discarding = false;
            } else if (!m_discarding) {
                if (m_tail.size() == 16384) {
                    m_tail.clear();
                    m_discarding = true;
                } else {
                    m_tail.append(c);
                }
            }
        }
        return result;
    }
    qsizetype bufferedBytes() const { return m_tail.size(); }
private:
    QByteArray m_tail;
    bool m_discarding = false;
    bool m_ready = false;
};
