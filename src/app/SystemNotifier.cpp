#include "SystemNotifier.h"

#include <QDebug>

#ifdef Q_OS_ANDROID
# include <QJniObject>
// QNativeInterface::QAndroidApplication 不在 QtGui，也不存在 <QNativeInterface> 这个头文件：
// 它声明在 QtCore 的 qcoreapplication_platform.h（Qt 6.9/6.12 的 android 套件里都只有这一处）。
# include <QtCore/qcoreapplication_platform.h>
#endif // ifdef Q_OS_ANDROID

namespace Qdl {
namespace SystemNotifier {
#ifdef Q_OS_ANDROID

namespace {
/// 固定通知 id：同一条通知重复发会顶掉旧的，而不是在通知栏堆一列
constexpr jint kNotificationId = 0x51444C; // "QDL"
constexpr const char *kChannelId = "qdownload.transfers";

QJniObject javaString(const QString& value)
{
    return QJniObject::fromString(value);
}
} // namespace

bool notify(const QString& title, const QString& body)
{
    QJniObject context = QNativeInterface::QAndroidApplication::context();

    if (!context.isValid()) {
        qWarning() << "Android Context 不可用，无法发通知";
        return false;
    }

    const jint sdk = QJniObject::getStaticField<jint>("android/os/Build$VERSION",
                                                      "SDK_INT");

    // Android 13 起通知要用户授权，没授权时 notify() 会静默丢弃，这里先问一下
    if (sdk >= 33) {
        const jint granted = context.callMethod<jint>(
            "checkSelfPermission",
            "(Ljava/lang/String;)I",
            javaString(QStringLiteral(
                           "android.permission.POST_NOTIFICATIONS")).object<jstring>());

        if (granted != 0) { // PackageManager.PERMISSION_GRANTED == 0
            qInfo() << "未获得通知权限，跳过系统通知";
            return false;
        }
    }

    QJniObject service = context.callObjectMethod(
        "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;",
        javaString(QStringLiteral("notification")).object<jstring>());

    if (!service.isValid()) {
        qWarning() << "拿不到 NotificationManager";
        return false;
    }

    const QJniObject channelId = javaString(QString::fromLatin1(kChannelId));

    // API 26 起必须先建渠道，否则 Builder 出来的通知发不出去
    if (sdk >= 26) {
        QJniObject channel("android/app/NotificationChannel",
                           "(Ljava/lang/String;Ljava/lang/CharSequence;I)V",
                           channelId.object<jstring>(),
                           javaString(QStringLiteral("文件传输")).object<jstring>(),
                           jint(3)); // NotificationManager.IMPORTANCE_DEFAULT

        if (channel.isValid()) {
            service.callMethod<void>("createNotificationChannel",
                                     "(Landroid/app/NotificationChannel;)V",
                                     channel.object());
        }
    }

    QJniObject builder;

    if (sdk >= 26) {
        builder = QJniObject("android/app/Notification$Builder",
                             "(Landroid/content/Context;Ljava/lang/String;)V",
                             context.object(), channelId.object<jstring>());
    } else {
        builder = QJniObject("android/app/Notification$Builder",
                             "(Landroid/content/Context;)V",
                             context.object());
    }

    if (!builder.isValid()) {
        qWarning() << "Notification.Builder 构造失败";
        return false;
    }

    // 没有 small icon 的通知会被系统直接丢掉，用应用自己的图标兜底
    jint icon = 0;
    QJniObject appInfo = context.callObjectMethod(
        "getApplicationInfo", "()Landroid/content/pm/ApplicationInfo;");

    if (appInfo.isValid()) icon = appInfo.getField<jint>("icon");

    if (icon == 0) {
        icon = QJniObject::getStaticField<jint>("android/R$drawable",
                                                "stat_sys_download");
    }

    builder.callObjectMethod("setContentTitle",
                             "(Ljava/lang/CharSequence;)Landroid/app/Notification$Builder;",
                             javaString(title).object<jstring>());
    builder.callObjectMethod("setContentText",
                             "(Ljava/lang/CharSequence;)Landroid/app/Notification$Builder;",
                             javaString(body).object<jstring>());

    if (icon != 0) {
        builder.callObjectMethod("setSmallIcon",
                                 "(I)Landroid/app/Notification$Builder;",
                                 icon);
    }
    builder.callObjectMethod("setAutoCancel",
                             "(Z)Landroid/app/Notification$Builder;",
                             jboolean(true));

    QJniObject notification = builder.callObjectMethod("build",
                                                       "()Landroid/app/Notification;");

    if (!notification.isValid()) {
        qWarning() << "Notification.build() 失败";
        return false;
    }

    service.callMethod<void>("notify", "(ILandroid/app/Notification;)V",
                             kNotificationId, notification.object());
    return true;
}

#else // !Q_OS_ANDROID

bool notify(const QString& title, const QString& body)
{
    Q_UNUSED(title)
    Q_UNUSED(body)

    // 桌面端由 QML 的托盘气泡负责，这里不做重复实现
    return false;
}

#endif // ifdef Q_OS_ANDROID
} // namespace SystemNotifier
} // namespace Qdl
