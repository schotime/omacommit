#include "Portal.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusReply>
#include <QEventLoop>
#include <QRandomGenerator>
#include <QStringList>
#include <QUrl>
#include <QVariantMap>

namespace {

const QString Service = QStringLiteral("org.freedesktop.portal.Desktop");
const QString DesktopPath = QStringLiteral("/org/freedesktop/portal/desktop");
const QString RequestIface = QStringLiteral("org.freedesktop.portal.Request");

// Receives the Request's Response signal; QtDBus delivers signals to slots only.
class ResponseWaiter : public QObject {
    Q_OBJECT
public:
    uint response = 2;   // 0 picked, 1 cancelled, 2 anything else
    QStringList uris;
    QEventLoop loop;

public Q_SLOTS:
    void onResponse(uint code, const QVariantMap &results)
    {
        response = code;
        uris = results.value(QStringLiteral("uris")).toStringList();
        loop.quit();
    }
};

}

Portal::Result Portal::pickDirectory(const QString &title, const QString &startDir, QString *picked)
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected())
        return Result::Unavailable;

    // The Request object's path is predictable from our bus name and a token of
    // our choosing: subscribe to it before calling, or a fast answer is missed.
    const QString token = QStringLiteral("og%1").arg(QRandomGenerator::global()->generate());
    QString sender = bus.baseService().mid(1);
    sender.replace(QLatin1Char('.'), QLatin1Char('_'));
    const QString requestPath = DesktopPath + QStringLiteral("/request/") + sender + QLatin1Char('/') + token;

    ResponseWaiter waiter;
    bus.connect(Service, requestPath, RequestIface, QStringLiteral("Response"), &waiter,
                SLOT(onResponse(uint, QVariantMap)));

    QByteArray folder = startDir.toUtf8();
    folder.append('\0');   // current_folder is a NUL-terminated byte string
    const QVariantMap options{
        {QStringLiteral("handle_token"), token},
        {QStringLiteral("modal"), true},
        {QStringLiteral("directory"), true},
        {QStringLiteral("current_folder"), folder},
    };
    QDBusMessage call = QDBusMessage::createMethodCall(Service, DesktopPath,
                                                       QStringLiteral("org.freedesktop.portal.FileChooser"),
                                                       QStringLiteral("OpenFile"));
    call << QString() << title << options;
    const QDBusReply<QDBusObjectPath> reply = bus.call(call);
    if (!reply.isValid())
        return Result::Unavailable;

    // Older portals return their own path rather than the predicted one.
    const QString handle = reply.value().path();
    if (handle != requestPath) {
        bus.disconnect(Service, requestPath, RequestIface, QStringLiteral("Response"), &waiter,
                       SLOT(onResponse(uint, QVariantMap)));
        bus.connect(Service, handle, RequestIface, QStringLiteral("Response"), &waiter,
                    SLOT(onResponse(uint, QVariantMap)));
    }
    waiter.loop.exec();

    if (waiter.response == 0 && !waiter.uris.isEmpty()) {
        *picked = QUrl(waiter.uris.first()).toLocalFile();
        return picked->isEmpty() ? Result::Cancelled : Result::Picked;
    }
    // 1 is the user cancelling; 2 is the portal giving up (no FileChooser
    // backend could open, say) -- fall back to Qt's dialog for that.
    return waiter.response == 1 ? Result::Cancelled : Result::Unavailable;
}

#include "Portal.moc"
