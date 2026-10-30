#include "AndroidService.h"

#include <QDebug>

#ifdef Q_OS_ANDROID
#include <QJniObject>
// 见 SystemNotifier.cpp：QNativeInterface 属于 QtCore 的平台头，不是 QtGui。
#include <QtCore/qcoreapplication_platform.h>
#endif

namespace Qdl {
namespace AndroidService {

#ifdef Q_OS_ANDROID

namespace {

constexpr const char *kServiceClass = "org.qtproject.example.qdownload.TransferService";

QJniObject makeIntent(const QJniObject &context)
{
    // 类名必须是 const char*：QJniObject 没有 QString 版本的构造函数，
    // 传 QStringLiteral 在桌面端编译不到（这段在 #ifdef Q_OS_ANDROID 里）不会暴露，
    // 只在 Android 编译期才炸。
    QJniObject intent("android/content/Intent");
    if (!intent.isValid())
        return {};

    intent.callObjectMethod(
        "setClassName",
        "(Landroid/content/Context;Ljava/lang/String;)Landroid/content/Intent;",
        context.object(),
        QJniObject::fromString(QString::fromLatin1(kServiceClass)).object<jstring>());
    return intent;
}

} // namespace

void start(const QString &text)
{
    QJniObject context = QNativeInterface::QAndroidApplication::context();
    if (!context.isValid()) {
        qWarning() << "拿不到 Android Context，无法启动前台服务";
        return;
    }

    QJniObject intent = makeIntent(context);
    if (!intent.isValid()) {
        qWarning() << "构造前台服务 Intent 失败";
        return;
    }

    intent.callObjectMethod("putExtra",
                            "(Ljava/lang/String;Ljava/lang/String;)Landroid/content/Intent;",
                            QJniObject::fromString(QStringLiteral("text")).object<jstring>(),
                            QJniObject::fromString(text).object<jstring>());

    const jint sdk = QJniObject::getStaticField<jint>("android/os/Build$VERSION", "SDK_INT");
    if (sdk >= 26) {
        context.callObjectMethod(
            "startForegroundService",
            "(Landroid/content/Intent;)Landroid/content/ComponentName;",
            intent.object());
    } else {
        context.callObjectMethod("startService",
                                 "(Landroid/content/Intent;)Landroid/content/ComponentName;",
                                 intent.object());
    }
    qInfo().noquote() << QStringLiteral("前台服务已启动：%1").arg(text);
}

void stop()
{
    QJniObject context = QNativeInterface::QAndroidApplication::context();
    if (!context.isValid())
        return;

    QJniObject intent = makeIntent(context);
    if (!intent.isValid())
        return;

    context.callMethod<jboolean>("stopService",
                                 "(Landroid/content/Intent;)Z", intent.object());
    qInfo() << "前台服务已停止";
}

#else   // 桌面平台：没有这个概念，全部空实现

void start(const QString &text)
{
    Q_UNUSED(text)
}

void stop() {}

#endif

} // namespace AndroidService
} // namespace Qdl
