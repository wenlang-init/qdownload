#include "Translation.h"

#include <QCoreApplication>
#include <QLocale>
#include <QLoggingCategory>
#include <QTranslator>

namespace {

QTranslator *g_translator = nullptr;
QString g_current;

bool isChinese(const QString &code)
{
    return code.startsWith(QLatin1String("zh"), Qt::CaseInsensitive);
}

} // namespace

namespace Qdl {
namespace Translation {

QString systemLanguage()
{
    return QLocale::system().name();   // 例：zh_CN / en_GB / ja_JP
}

QString effective(const QString &code)
{
    const QString trimmed = code.trimmed();
    if (trimmed.isEmpty())
        return QStringLiteral("zh_CN");   // 没设置过 = 中文（源码语言，零依赖）
    if (trimmed.compare(QLatin1String("system"), Qt::CaseInsensitive) == 0)
        return systemLanguage();
    return trimmed;
}

bool apply(const QString &code)
{
    const QString target = effective(code);

    if (g_translator) {
        QCoreApplication::removeTranslator(g_translator);
        delete g_translator;
        g_translator = nullptr;
    }

    // 源码文案是中文：中文环境什么都不用装
    if (isChinese(target)) {
        if (g_current != target) {
            g_current = target;
            qInfo().noquote() << QStringLiteral("界面语言：%1（源语言，无需翻译包）").arg(target);
        }
        return true;
    }

    auto *translator = new QTranslator;
    // 只维护一份英文翻译，其它非中文语言也先用英文兜着，
    // 总比满屏看不懂的中文强
    if (!translator->load(QStringLiteral(":/i18n/qdownload_en.qm"))) {
        qWarning().noquote() << "英文翻译包加载失败，界面将保持中文";
        delete translator;
        return false;
    }

    QCoreApplication::installTranslator(translator);
    g_translator = translator;
    g_current = target;
    qInfo().noquote() << QStringLiteral("界面语言：%1（已装英文翻译包）").arg(target);
    return true;
}

QString current()
{
    return g_current;
}

} // namespace Translation
} // namespace Qdl
