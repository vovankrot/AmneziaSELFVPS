/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <QDebug>
#include "notificationhandler.h"
#include "utilities.h"

#if defined(Q_OS_IOS)
#  include "platforms/ios/iosnotificationhandler.h"
#elif defined(AMNEZIA_DESKTOP)
#  include "systemtray_notificationhandler.h"
#endif


// static
NotificationHandler* NotificationHandler::create(QObject* parent) {
#if defined(Q_OS_IOS)
    return new IOSNotificationHandler(parent);
#elif defined(AMNEZIA_DESKTOP)
    return new SystemTrayNotificationHandler(parent);
#else
    Q_UNUSED(parent);
    return nullptr;
#endif
}

namespace {
NotificationHandler* s_instance = nullptr;
}  // namespace

// static
NotificationHandler* NotificationHandler::instance() {
    Q_ASSERT(s_instance);
    return s_instance;
}

NotificationHandler::NotificationHandler(QObject* parent) : QObject(parent) {
    Q_ASSERT(!s_instance);
    s_instance = this;
}

NotificationHandler::~NotificationHandler() {
    Q_ASSERT(s_instance == this);
    s_instance = nullptr;
}

void NotificationHandler::setConnectionState(Vpn::ConnectionState state)
{
    if (state != Vpn::ConnectionState::Connected && state != Vpn::ConnectionState::Disconnected) {
        return;
    }

    QString title;
    QString message;

    switch (state) {
    case Vpn::ConnectionState::Connected:
        m_connected = true;

        title = tr("AmneziaVPN");
        message = tr("VPN Connected");
        break;

    case Vpn::ConnectionState::Disconnected:
        if (m_connected) {
            m_connected = false;
            title = tr("AmneziaVPN");
            message = tr("VPN Disconnected");
        }
        break;

    default:
        break;
    }

    Q_ASSERT(title.isEmpty() == message.isEmpty());

    if (!title.isEmpty()) {
        notifyInternal(VpnState, title, message, 2000);
    }
}

void NotificationHandler::onTranslationsUpdated()
{
}

void NotificationHandler::unsecuredNetworkNotification(const QString& networkName) {
    qDebug() << "Unsecured network notification shown";


    QString title = tr("AmneziaVPN notification");
    QString message = tr("Unsecured network detected: ") + networkName;

    notifyInternal(UnsecuredNetwork, title, message, 2000);
}

void NotificationHandler::systemProxyDetectedNotification(const QString& proxyServer) {
    qWarning() << "System proxy detected that may intercept VPN traffic:" << proxyServer;

    QString title = tr("AmneziaVPN");
    QString message = tr("A local system proxy (%1) is intercepting traffic and may slow the VPN down. "
                         "Click this notification to disable it.").arg(proxyServer);

    notifyInternal(SystemProxyDetected, title, message, 10000);
}

void NotificationHandler::systemProxyAutoDisabledNotification(const QString& proxyServer) {
    qInfo() << "System proxy auto-disabled to protect VPN traffic:" << proxyServer;

    QString title = tr("AmneziaVPN");
    QString message = tr("A local system proxy (%1) was intercepting traffic and was automatically disabled "
                         "so the VPN can reach foreign sites. If it reappears, close the third-party app "
                         "(e.g. Invisible Man) that keeps re-enabling it.").arg(proxyServer);

    notifyInternal(SystemProxyAutoDisabled, title, message, 8000);
}

void NotificationHandler::notifyInternal(Message type, const QString& title,
                                         const QString& message,
                                         int timerMsec) {
    m_lastMessage = type;

    emit notificationShown(title, message);
    notify(type, title, message, timerMsec);
}

void NotificationHandler::messageClickHandle() {
    qDebug() << "Message clicked";

    if (m_lastMessage == VpnState) {
        qCritical() << "Random message clicked received";
        return;
    }

    if (m_lastMessage == SystemProxyDetected) {
        if (Utils::disableSystemProxy()) {
            qInfo() << "System proxy disabled by user from notification";
        } else {
            qWarning() << "Failed to disable system proxy from notification";
        }
    }

    emit notificationClicked(m_lastMessage);
    m_lastMessage = VpnState;
}
