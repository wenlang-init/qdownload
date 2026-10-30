#include "AndroidStorage.h"

#ifdef Q_OS_ANDROID

#include <QCoreApplication>
#include <QJniEnvironment>
#include <QJniObject>
#include <QDir>
#include <QStandardPaths>
// QNativeInterface 属于 QtCore 的平台头，不在 QtGui 里，见 SystemNotifier.cpp 的说明
#include <QtCore/qcoreapplication_platform.h>

#endif // Q_OS_ANDROID

namespace Qdl {
namespace AndroidStorage {

#ifdef Q_OS_ANDROID

namespace {

// Android 11：MANAGE_EXTERNAL_STORAGE 从这里才开始存在，下面用它划代
constexpr int kSdkR = 30;
constexpr jint kFlagActivityNewTask = 0x10000000;   // Intent.FLAG_ACTIVITY_NEW_TASK
constexpr jint kRequestPermissionsCode = 4271;      // 旧版权限申请的回调码，没人收，随便给
constexpr jint kPermissionGranted = 0;              // PackageManager.PERMISSION_GRANTED

QJniObject context()
{
    return QNativeInterface::QAndroidApplication::context();
}

int sdkInt()
{
    static const int value =
        QJniObject::getStaticField<jint>("android/os/Build$VERSION", "SDK_INT");
    return value;
}

// 把 Java 侧的 File 对象转成路径字符串
QString fileToPath(const QJniObject &file)
{
    if (!file.isValid())
        return {};
    return file.callObjectMethod("getAbsolutePath", "()Ljava/lang/String;").toString();
}

// Context.getExternalFilesDir(null)：应用专属的外部存储目录。
// 卸载应用会被系统删掉，所以不要拿它当长期存档——但作为共享/接收目录正合适
QString appSpecificExternalDir()
{
    const QJniObject ctx = context();
    if (!ctx.isValid())
        return {};

    // 参数是允许为空的 String（表示主目录），nullptr 就是 Java 侧的 null
    const QJniObject file = ctx.callObjectMethod(
        "getExternalFilesDir", "(Ljava/lang/String;)Ljava/io/File;", nullptr);
    return fileToPath(file);
}

QString internalFilesDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
}

// 获得全盘权限后才有意义的外部存储根（/storage/emulated/0）。
// getExternalStorageDirectory 在 API 29 弃用但仍可用；拿不到就退回 QDir 探测
QString publicRoot()
{
    const QJniObject dir = QJniObject::callStaticObjectMethod(
        "android/os/Environment", "getExternalStorageDirectory", "()Ljava/io/File;");
    const QString viaApi = fileToPath(dir);
    if (!viaApi.isEmpty() && QDir(viaApi).exists())
        return viaApi;

    const QString fallback = QStringLiteral("/storage/emulated/0");
    return QDir(fallback).exists() ? fallback : QString();
}

} // namespace

bool hasFullAccess()
{
    if (sdkInt() >= kSdkR) {
        return QJniObject::callStaticMethod<jboolean>("android/os/Environment",
                                                      "isExternalStorageManager", "()Z");
    }

    // Android 9/10：有没有被授予 WRITE_EXTERNAL_STORAGE
    const QJniObject ctx = context();
    if (!ctx.isValid())
        return false;

    const QJniObject perm = QJniObject::fromString(
        QStringLiteral("android.permission.WRITE_EXTERNAL_STORAGE"));
    const jint granted = ctx.callMethod<jint>("checkSelfPermission",
                                              "(Ljava/lang/String;)I",
                                              perm.object<jstring>());
    return granted == kPermissionGranted;
}

void requestFullAccess()
{
    if (hasFullAccess())
        return;

    const QJniObject ctx = context();
    if (!ctx.isValid()) {
        qWarning() << "拿不到 Android Context，无法跳转到存储授权页";
        return;
    }

    if (sdkInt() >= kSdkR) {
        // "所有文件访问权限"没有运行时对话框，只能把用户送到设置页自己开
        const QJniObject action = QJniObject::getStaticObjectField<jstring>(
            "android/provider/Settings",
            "ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION");
        if (!action.isValid()) {
            qWarning() << "取不到 ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION";
            return;
        }

        const QString pkg = ctx.callObjectMethod("getPackageName",
                                                 "()Ljava/lang/String;").toString();
        const QJniObject uri = QJniObject::callStaticObjectMethod(
            "android/net/Uri", "parse", "(Ljava/lang/String;)Landroid/net/Uri;",
            QJniObject::fromString(QStringLiteral("package:") + pkg).object<jstring>());

        QJniObject intent("android/content/Intent",
                          "(Ljava/lang/String;Landroid/net/Uri;)V",
                          action.object<jstring>(), uri.object());
        intent.callObjectMethod("addFlags", "(I)Landroid/content/Intent;",
                                kFlagActivityNewTask);
        ctx.callMethod<void>("startActivity", "(Landroid/content/Intent;)V",
                             intent.object());
        qInfo() << "已跳转到「所有文件访问权限」设置页";
        return;
    }

    // 旧系统走运行时权限请求
    QJniEnvironment env;
    if (!env.isValid())
        return;

    jclass stringClass = env->FindClass("java/lang/String");
    if (!stringClass)
        return;

    jobjectArray permissions = env->NewObjectArray(1, stringClass, nullptr);
    const QByteArray name = QStringLiteral("android.permission.WRITE_EXTERNAL_STORAGE")
                                .toUtf8();
    jstring permutation = env.jniEnv()->NewStringUTF(name.constData());
    env->SetObjectArrayElement(permissions, 0, permutation);
    ctx.callMethod<void>("requestPermissions", "([Ljava/lang/String;I)V",
                         permissions, kRequestPermissionsCode);

    env->DeleteLocalRef(permutation);
    env->DeleteLocalRef(permissions);
    qInfo() << "已发起 WRITE_EXTERNAL_STORAGE 运行时权限请求";
}

QList<Root> roots()
{
    QList<Root> out;

    const QString external = appSpecificExternalDir();
    if (!external.isEmpty()) {
        out.append({external, QCoreApplication::translate("AndroidStorage",
                                                          "应用专属目录")});
    }

    const QString internal = internalFilesDir();
    if (!internal.isEmpty() && internal != external) {
        out.append({internal, QCoreApplication::translate("AndroidStorage",
                                                          "内部存储")});
    }

    if (hasFullAccess()) {
        const QString shared = publicRoot();
        if (!shared.isEmpty() && shared != external && shared != internal) {
            out.append({shared, QCoreApplication::translate("AndroidStorage",
                                                            "共享存储空间")});
        }
    }

    return out;
}

QString preferredDir()
{
    const QString external = appSpecificExternalDir();
    if (!external.isEmpty())
        return external;
    return internalFilesDir();
}

#else   // 桌面平台：没有分区存储这套限制，用不着这些

bool hasFullAccess()
{
    return true;
}

void requestFullAccess() {}

QList<Root> roots()
{
    return {};
}

QString preferredDir()
{
    return {};
}

#endif

} // namespace AndroidStorage
} // namespace Qdl
