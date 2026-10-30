#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QGuiApplication>
#include <QHash>
#include <QImage>
#include <QAbstractItemModel>
#include <QNetworkProxy>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSet>
#include <QSettings>
#include <QTimer>

#include <cstdio>
#include <cstdlib>
#include <memory>

#include "app/AppController.h"
#include "app/DataPaths.h"
#include "app/InstanceGuard.h"
#include "app/LaunchOptions.h"
#include "app/Logger.h"
#include "app/Translation.h"
#include "fs/FileListModel.h"
#include "net/PeerListModel.h"
#include "transfer/TransferModel.h"

namespace {
/// 在 --instance 预扫描阶段读取参数值（此时还没解析命令行，无法用 QCommandLineParser）
QString prescanInstanceTag(int argc, char *argv[])
{
    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);

        if ((arg == QStringLiteral("--instance")) &&
            (i + 1 < argc)) return QString::fromLocal8Bit(argv[i + 1]);

        if (arg.startsWith(QStringLiteral("--instance="))) return arg.mid(int(
                                                                              qstrlen(
                                                                                  "--instance=")));
    }
    return {};
}

/// 无界面探测模式：跑 N 秒组播，把发现到的主机按行打印出来，供联调脚本校验。
int runProbe(int seconds)
{
    AppController controller;
    auto *model = controller.peers();

    QTimer::singleShot(seconds * 1000, qApp, [model]() {
            printf("PROBE_PEERS %d\n", model->rowCount());

            for (int row = 0; row < model->rowCount(); ++row) {
                const QModelIndex index = model->index(row, 0);
                const QString line = QStringLiteral("%1|%2|%3|%4|%5|%6")
                                     .arg(model->data(index,
                                                      PeerListModel::PeerIdRole).
                                          toString(),
                                          model->data(index,
                                                      PeerListModel::NameRole).
                                          toString(),
                                          model->data(index,
                                                      PeerListModel::AddressRole).
                                          toString(),
                                          model->data(index,
                                                      PeerListModel::
                                                      StatusBadgeRole).toString(),
                                          model->data(index,
                                                      PeerListModel::OnlineRole).
                                          toBool()
                                          ? QStringLiteral("online")
                                          : QStringLiteral("offline"),
                                          model->data(index,
                                                      PeerListModel::SubtitleRole)
                                          .toString());
                printf("PEER %s\n", line.toUtf8().constData());
            }
            fflush(stdout);
            QCoreApplication::exit(0);
        });

    return qApp->exec();
}

/// 无界面配对测试：发现目标主机 → 连接 → 配对 → 打印结果后退出。
/// 用于自动化验证配对链路，不依赖人工读屏输码（主机侧要配合 --pair-code 与 --auto-approve）。
int runPairTest(AppController& controller, const QString& targetPeerId,
                const QString& code, int timeoutSeconds, int watchAfterReadyMs,
                const QString& listPath, const QStringList& getPaths,
                int pauseAtPercent, int cancelAtPercent, int killAtPercent,
                bool resumeMode, const QStringList& putPaths,
                int putPauseAt, int putCancelAt, int disconnectAfterMs,
                int workersAtPercent, int workersAtCount,
                const QString& deletePath)
{
    auto *model = controller.peers();

    // watch 模式下把连接状态流水打出来：主机掉线再回来时有没有自动重连，
    // 以及 --disconnect-after 断完之后有没有被心跳拉回来，全靠这条线判断
    if (watchAfterReadyMs > 0) {
        QObject::connect(&controller, &AppController::connChanged, &controller,
                         [&controller]() {
                printf("PAIR_TEST_CONN %s\n",
                       controller.connStateCode().toUtf8().constData());
                fflush(stdout);
            });
    }

    if (disconnectAfterMs > 0) {
        QTimer::singleShot(disconnectAfterMs, &controller, [&controller]() {
                printf("PAIR_TEST_DISCONNECTING\n");
                fflush(stdout);
                controller.disconnectPeer();      // 与界面「断开」按钮同一个入口
            });
    }

    QTimer poll;

    poll.setInterval(300);

    // 只主动发起一次配对：否则失败后每 300ms 就重发，会把主机刷爆
    auto requested = std::make_shared<bool>(false);
    auto readyPrinted = std::make_shared<bool>(false);
    auto lastCanUpload = std::make_shared<int>(-1);
    auto listStarted = std::make_shared<bool>(false);
    auto listRounds = std::make_shared<int>(0);
    auto getStarted = std::make_shared<bool>(false);
    auto lastPercent = std::make_shared<int>(-1);
    auto pausePhase = std::make_shared<int>(0); // 0 未暂停 / 1 已暂停 / 2 已继续
    auto cancelDone = std::make_shared<bool>(false);
    auto putStarted = std::make_shared<bool>(false);
    auto putCancelDone = std::make_shared<bool>(false);
    auto putPausePhase = std::make_shared<int>(0);
    auto workersDone = std::make_shared<bool>(false);   // 运行中改连接数只改一次
    auto rmPhase = std::make_shared<int>(0);            // --rm 的状态机

    // 多文件上传：按 transferId 记进度与已汇报，才能"逐条打结果、等全部结束"
    auto putLastPercent = std::make_shared<QHash<QString, int> >();
    auto putReported = std::make_shared<QSet<QString> >();

    QObject::connect(&poll, &QTimer::timeout, &controller,
                     [&controller, model, targetPeerId, requested, readyPrinted,
                      lastCanUpload,
                      watchAfterReadyMs, listPath, getPaths, listStarted,
                      listRounds,
                      getStarted, lastPercent, pauseAtPercent, cancelAtPercent,
                      pausePhase, cancelDone, killAtPercent, resumeMode,
                      putPaths, putPauseAt, putCancelAt, putStarted,
                      putCancelDone,
                      putPausePhase, putLastPercent, putReported,
                      workersAtPercent, workersAtCount, workersDone,
                      deletePath, rmPhase]() {
            if (controller.selectedPeerId() != targetPeerId) {
                for (int row = 0; row < model->rowCount(); ++row) {
                    const QModelIndex index = model->index(row, 0);
                    const bool online =
                        model->data(index, PeerListModel::OnlineRole).toBool();

                    if ((model->data(index,
                                     PeerListModel::PeerIdRole).toString() ==
                         targetPeerId) &&
                        online) {
                        printf("PAIR_TEST_FOUND %s\n",
                               targetPeerId.toUtf8().constData());
                        fflush(stdout);
                        controller.setSelectedPeerId(targetPeerId);
                        break;
                    }
                }
            }

            // 连接建立且确认未配对 → 主动发起配对（只一次）
            if (!*requested &&
                (controller.connStateCode() == QLatin1String("unpaired"))) {
                *requested = true;
                controller.requestPairing();
            }

            // Ready 是唯一成功判据：首次配对走 PAIR_OK，之后重连走 AUTH_RESP
            if (controller.connStateCode() == QLatin1String("ready")) {
                const int canUpload = controller.peerCanUpload() ? 1 : 0;

                if (!*readyPrinted) {
                    *readyPrinted = true;
                    *lastCanUpload = canUpload;
                    printf("PAIR_TEST_OK peer=%s perm=%s canUpload=%d\n",
                           controller.selectedPeerId().toUtf8().constData(),
                           controller.peerPerm().toUtf8().constData(),
                           canUpload);
                    fflush(stdout);

                    if (listPath.isEmpty() && (watchAfterReadyMs <= 0) &&
                        getPaths.isEmpty() && deletePath.isEmpty()
                        && !resumeMode && putPaths.isEmpty()) {
                        QCoreApplication::exit(0);
                        return;
                    }
                } else if (listPath.isEmpty() && getPaths.isEmpty() &&
                           (watchAfterReadyMs > 0)
                           && (canUpload != *lastCanUpload)) {
// 观察期内的权限变更：验证 PERM_UPDATE 是否即时到达
                    *lastCanUpload = canUpload;
                    printf("PAIR_TEST_CAN_UPLOAD %d\n", canUpload);
                    fflush(stdout);
                }

                if (!putPaths.isEmpty()) {
                    if (!*putStarted) {
                        *putStarted = true;
                        const int queued = controller.uploadPaths(putPaths);
                        printf("PUT_TEST_START queued=%d\n", queued);
                        fflush(stdout);

                        if (queued == 0) {
                            QCoreApplication::exit(8);
                            return;
                        }
                        return;
                    }

                    auto *putTransfers =
                        qobject_cast<Qdl::TransferModel *>(
                            controller.transfers());

                    if (!putTransfers || (putTransfers->rowCount() == 0)) return;

/**
 * `--put` 可以一次给多个文件。这里逐行扫、任一任务到终态就单独打一条
 * `PUT_TEST_RESULT`，**全部到终态才退出**：只盯 row 0 会在并发上传时
 * 提前收工，把其他任务甩在半路，测出来的"成功"其实是没传完。
 */
                    int terminalRows = 0;
                    bool allOk = true;

                    for (int row = 0; row < putTransfers->rowCount(); ++row) {
                        const QModelIndex rowIdx = putTransfers->index(row, 0);
                        const QString rowId =
                            putTransfers->data(rowIdx,
                                               Qdl::TransferModel::IdRole).toString();
                        const QString rowState =
                            putTransfers->data(rowIdx,
                                               Qdl::TransferModel::StateRole).toString();
                        const int rowPercent =
                            int(putTransfers->data(rowIdx,
                                                   Qdl::TransferModel::
                                                   ProgressRole)
                                .toDouble() * 100.0);

// 暂停/取消只掐第一条：多文件同时掐掉所有任务没有观测价值
                        const bool primary = row == 0;

                        if (primary && (putCancelAt > 0) && !*putCancelDone &&
                            (rowPercent >= putCancelAt)
                            && (rowState == QLatin1String("transferring"))) {
                            *putCancelDone = true;
                            printf("PUT_TEST_CANCEL %d%%\n", rowPercent);
                            fflush(stdout);
                            controller.cancelTransfer(rowId);
                            continue;
                        }

                        if (primary && (putPauseAt > 0) &&
                            (*putPausePhase == 0) && (rowPercent >= putPauseAt)
                            && (rowState == QLatin1String("transferring"))) {
                            *putPausePhase = 1;
                            controller.pauseTransfer(rowId);
                            printf("PUT_TEST_PAUSED %d%%\n", rowPercent);
                            fflush(stdout);
                            QTimer::singleShot(1200, &controller,
                                               [&controller, rowId,
                                                putPausePhase]() {
                                *putPausePhase = 2;
                                printf("PUT_TEST_RESUMED\n");
                                fflush(stdout);
                                controller.resumeTransfer(rowId);
                            });
                            continue;
                        }

                        if ((*putLastPercent)[rowId] != rowPercent) {
                            (*putLastPercent)[rowId] = rowPercent;
                            printf("PUT_TEST_PROGRESS %s %d%% %s %s\n",
                                   rowId.toUtf8().constData(), rowPercent,
                                   putTransfers->data(rowIdx,
                                                      Qdl::TransferModel::
                                                      SpeedTextRole)
                                   .toString().toUtf8().constData(),
                                   rowState.toUtf8().constData());
                            fflush(stdout);
                        }

// 计划内的暂停不算结束：等 1.2 秒后的继续。只有"没想到会停"才算失败
                        const bool pausedTerminal =
                            rowState == QLatin1String(
                                "paused") && putPauseAt <= 0;
                        const bool rowTerminal =
                            rowState == QLatin1String("completed")
                            || rowState == QLatin1String("failed")
                            || rowState == QLatin1String(
                                "canceled") || pausedTerminal;

                        if (!rowTerminal) continue;

                        ++terminalRows;

                        if (!putReported->contains(rowId)) {
                            putReported->insert(rowId);
                            printf(
                                "PUT_TEST_RESULT id=%s state=%s size=%s local=%s sha256=%s error=%s\n",
                                rowId.toUtf8().constData(),
                                rowState.toUtf8().constData(),
                                putTransfers->data(rowIdx,
                                                   Qdl::TransferModel::
                                                   SizeTextRole)
                                .toString().toUtf8().constData(),
                                putTransfers->data(rowIdx,
                                                   Qdl::TransferModel::
                                                   LocalPathRole)
                                .toString().toUtf8().constData(),
                                controller.transferChecksum(rowId).toUtf8().
                                constData(),
                                putTransfers->data(rowIdx,
                                                   Qdl::TransferModel::ErrorRole)
                                .toString().toUtf8().constData());
                            fflush(stdout);
                        }

// 暂停后点「继续」会把原任务 cancel 掉再新建一条（断点在主机侧，
// 重新申请才能拿回 offset），那条 canceled 是被替代的，不算失败
                        const bool rowOk =
                            rowState == QLatin1String("completed")
                            || (rowState == QLatin1String("canceled")
                                && (putCancelAt > 0 || putPauseAt > 0));
                        allOk = allOk && rowOk;
                    }

                    if (terminalRows >= putTransfers->rowCount()) QCoreApplication::exit(
                            allOk ? 0 : 7);
                    return;
                }

                if (!getPaths.isEmpty() || resumeMode) {
                    if (!*getStarted) {
                        *getStarted = true;

                        if (resumeMode) {
// 断点已在启动时恢复进抽屉，这里只是替用户点一次「继续」
                            const int count = controller.resumeAllTransfers();
                            printf("RESUME_TEST_START count=%d\n", count);
                            fflush(stdout);

                            if (count == 0) {
                                printf("RESUME_TEST_EMPTY\n");
                                fflush(stdout);
                                QCoreApplication::exit(3);
                            }
                        } else {
                            for (const QString &p : getPaths) {
                                printf("GET_TEST_REQ %s\n",
                                       p.toUtf8().constData());
                                fflush(stdout);
                                controller.downloadPath(p, 0);
                            }
                        }
                        return;
                    }

                    auto *transfers =
                        qobject_cast<Qdl::TransferModel *>(
                            controller.transfers());

                    if (!transfers || (transfers->rowCount() == 0)) return;

                    const QModelIndex idx = transfers->index(0, 0);
                    const QString taskId =
                        transfers->data(idx,
                                        Qdl::TransferModel::IdRole).toString();
                    const QString state =
                        transfers->data(idx,
                                        Qdl::TransferModel::StateRole).toString();
                    const int percent =
                        int(transfers->data(idx,
                                            Qdl::TransferModel::ProgressRole).
                            toDouble() * 100.0);

// 联调：到点硬退出（不跑析构、不落库）——模拟"下到一半被杀进程"，
// 续传能力必须靠已经落盘的断点记录接住
                    if ((killAtPercent > 0) && (percent >= killAtPercent)
                        && (state == QLatin1String("transferring"))) {
                        printf("GET_TEST_KILL %d%%\n", percent);
                        fflush(stdout);
                        std::_Exit(9);
                    }

// 联调：到点暂停，1.2 秒后继续——验证 offset 续传与 qdpart 复用
                    if ((pauseAtPercent > 0) && (*pausePhase == 0) &&
                        (percent >= pauseAtPercent)
                        && (state == QLatin1String("transferring"))) {
                        *pausePhase = 1;
                        controller.pauseTransfer(taskId);
                        printf("GET_TEST_PAUSED %d%%\n", percent);
                        fflush(stdout);
                        QTimer::singleShot(1200, &controller,
                                           [&controller, taskId, pausePhase]() {
                            *pausePhase = 2;
                            printf("GET_TEST_RESUMED\n");
                            fflush(stdout);
                            controller.resumeTransfer(taskId);
                        });
                        return;
                    }

// 联调：传到一半改单文件连接数，验证 worker 池能即时增减（M12）
                    if ((workersAtPercent > 0) && !*workersDone &&
                        (percent >= workersAtPercent)
                        && (state == QLatin1String("transferring"))) {
                        *workersDone = true;
                        controller.setSegmentCount(workersAtCount);
                        printf("GET_TEST_WORKERS %d\n", workersAtCount);
                        fflush(stdout);
                        return;
                    }

// 联调：到点取消，验证 qdpart 被清掉
                    if ((cancelAtPercent > 0) && !*cancelDone &&
                        (percent >= cancelAtPercent)
                        && (state == QLatin1String("transferring"))) {
                        *cancelDone = true;
                        printf("GET_TEST_CANCEL %d%%\n", percent);
                        fflush(stdout);
                        controller.cancelTransfer(taskId);
                        return;
                    }

                    if (percent != *lastPercent) {
                        *lastPercent = percent;
                        printf("GET_TEST_PROGRESS %d%% %s %s\n",
                               percent,
                               transfers->data(idx,
                                               Qdl::TransferModel::SpeedTextRole)
                               .toString().toUtf8().constData(),
                               state.toUtf8().constData());
                        fflush(stdout);
                    }

                    if ((state == QLatin1String("completed")) ||
                        (state == QLatin1String("failed"))
                        || (state == QLatin1String("canceled"))) {
                        printf("%s state=%s size=%s local=%s sha256=%s error=%s\n",
                               resumeMode ? "RESUME_TEST_RESULT" :
                               "GET_TEST_RESULT",
                               state.toUtf8().constData(),
                               transfers->data(idx,
                                               Qdl::TransferModel::SizeTextRole)
                               .toString().toUtf8().constData(),
                               transfers->data(idx,
                                               Qdl::TransferModel::LocalPathRole)
                               .toString().toUtf8().constData(),
                               controller.transferChecksum(taskId).toUtf8().
                               constData(),
                               transfers->data(idx, Qdl::TransferModel::ErrorRole)
                               .toString().toUtf8().constData());
                        fflush(stdout);
                        const bool ok = state == QLatin1String("completed")
                                        || (state == QLatin1String("canceled") &&
                                            cancelAtPercent > 0);
                        QCoreApplication::exit(ok ? 0 : 7);
                    }
                    return;
                }

                // --rm：先列目录确认文件在 → 让主机删 → 再列一次确认真的没了（M14）
                if (!deletePath.isEmpty()) {
                    const int slash = deletePath.lastIndexOf(QLatin1Char('/'));
                    const QString rmDir = slash <= 0 ? QStringLiteral("/")
                                                     : deletePath.left(slash);
                    const QString rmName = deletePath.mid(slash + 1);

                    if (*rmPhase == 0) {
                        *rmPhase = 1;
                        printf("RM_TEST_REQ %s\n", deletePath.toUtf8().constData());
                        fflush(stdout);
                        controller.remoteGo(rmDir);
                        return;
                    }

                    if (*rmPhase == 1 || *rmPhase == 3) {
                        if (controller.remoteLoading())
                            return;

                        auto *files = qobject_cast<Qdl::FileListModel *>(
                            controller.remoteFiles());
                        bool present = false;
                        for (int row = 0; files && row < files->rowCount(); ++row) {
                            if (files->entryAt(row).name == rmName)
                                present = true;
                        }

                        if (*rmPhase == 1) {
                            printf("RM_TEST_PRESENT_BEFORE %d\n", present ? 1 : 0);
                            fflush(stdout);
                            if (!present) {
                                QCoreApplication::exit(9);
                                return;
                            }
                            *rmPhase = 2;
                            controller.deleteRemoteFile(rmName);
                            return;
                        }

                        printf("RM_TEST_RESULT path=%s present_after=%d\n",
                               deletePath.toUtf8().constData(), present ? 1 : 0);
                        fflush(stdout);
                        QCoreApplication::exit(present ? 10 : 0);
                        return;
                    }

                    if (*rmPhase == 2) {
                        // 等删除结果落到"传输提示"（成功/失败都会写），再重新拉一遍目录
                        if (controller.transferNotice().isEmpty())
                            return;
                        *rmPhase = 3;
                        controller.remoteGo(rmDir);
                        return;
                    }
                    return;
                }

                if (!listPath.isEmpty()) {
                    if (!*listStarted) {
                        *listStarted = true;
                        printf("LIST_TEST_REQ %s\n",
                               listPath.toUtf8().constData());
                        fflush(stdout);
                        controller.remoteGo(listPath);
                        return;
                    }

                    if (controller.remoteLoading()) return;

// 还有下一页就继续要，最多 3 轮——用来验证分页 append 没有错位
                    if ((*listRounds < 3) && controller.remoteCanMore()) {
                        ++*listRounds;
                        controller.remoteLoadMore();
                        return;
                    }

                    auto *files =
                        qobject_cast<Qdl::FileListModel *>(
                            controller.remoteFiles());
                    printf(
                        "LIST_TEST path=%s total=%d rows=%d pages=%d error=%s\n",
                        controller.remotePath().toUtf8().constData(),
                        controller.remoteTotal(),
                        files ? files->rowCount() : -1,
                        *listRounds + 1,
                        controller.remoteError().toUtf8().constData());

                    if (files) {
                        const int shown = qMin(files->rowCount(), 8);

                        for (int row = 0; row < shown; ++row) {
                            const Qdl::FileEntry entry = files->entryAt(row);
                            printf("LIST_ENTRY %s|%s|%lld|%s\n",
                                   entry.isDir ? "dir" : "file",
                                   entry.name.toUtf8().constData(),
                                   static_cast<long long>(entry.size),
                                   entry.mtimeText().toUtf8().constData());
                        }
                    }
                    fflush(stdout);
                    QCoreApplication::exit(controller.remoteError().isEmpty() ? 0
                                           : 6);
                    return;
                }
            }
        });
    poll.start();

    if (watchAfterReadyMs > 0) {
        QTimer::singleShot(watchAfterReadyMs, &controller, []() {
                printf("PAIR_TEST_WATCH_DONE\n");
                fflush(stdout);
                QCoreApplication::exit(0);
            });
    }

    QObject::connect(&controller, &AppController::pairCodePrompt, &controller,
                     [&controller, code]() {
            printf("PAIR_TEST_PROMPT 使用配对码 %s\n", code.toUtf8().constData());
            fflush(stdout);
            controller.submitPairCode(code);
        });

    QObject::connect(&controller,
                     &AppController::pairSucceeded,
                     &controller,
                     []() {
            printf("PAIR_TEST_PAIRED\n");
            fflush(stdout);
        });

    QObject::connect(&controller, &AppController::pairFailed, &controller,
                     [](const QString& message) {
            printf("PAIR_TEST_FAIL %s\n", message.toUtf8().constData());
            fflush(stdout);
            QCoreApplication::exit(1);
        });

    QTimer::singleShot(timeoutSeconds * 1000, &controller, []() {
            printf("PAIR_TEST_TIMEOUT\n");
            fflush(stdout);
            QCoreApplication::exit(4);
        });

    return qApp->exec();
}
} // namespace

#include "app/cxxlog.h"
#include <cstdio>

void ExitRoutine1(void) {
    INFO_LOG_CXX("while exit\n");
}

void ExitRoutine2(void) {
    INFO_LOG_CXX("exit\n");
}

void exitAT() {
    // 注册顺序和执行顺序相反
    // 对于ctrl+c之类的不起作用，只有exit()或关闭窗口时才会起作用
    atexit(ExitRoutine2);
    atexit(ExitRoutine1);

    // exit(0);
}

#if defined(Q_OS_WINDOWS)
# include <windows.h>
# include <stdio.h>
# include <iostream>

// 将标准输出重定向到控制台
// 检测到从控制台启动，或onlyShow=true则启动控制台输出，否则无控制台输出
// 在创建 QApplication 之前处理控制台
static void AttachConsoleAndRedirect(bool onlyShow = false) {
    // 尝试附加到父进程的控制台（如果是从cmd启动的，这会成功）
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        // 重定向 stdout/stderr 到刚刚附加的控制台
        FILE *fp;
        freopen_s(&fp, "CONOUT$", "w", stdout);
        freopen_s(&fp, "CONOUT$", "w", stderr);

        // 使 C++ 标准流与 C 标准流同步，确保 qDebug/cout 能输出
        std::ios::sync_with_stdio();

        qDebug() << "检测到命令行环境，已附加到现有控制台。";
    } else {
        // 如果没有父控制台（双击运行），AttachConsole 会失败
        // 此时我们可以选择什么都不做（静默运行），或者手动创建一个新控制台用于调试

        if (onlyShow) {
            if (AllocConsole()) {
                FILE *fp;
                freopen_s(&fp, "CONOUT$", "w", stdout);
                freopen_s(&fp, "CONOUT$", "w", stderr);
                std::ios::sync_with_stdio();
                system("color 0");
                qDebug() << "总是显示输出模式：已创建新控制台。";
            }
        } else {
            // # ifndef QT_NO_DEBUG

            // if (AllocConsole()) {
            //     FILE *fp;
            //     freopen_s(&fp, "CONOUT$", "w", stdout);
            //     freopen_s(&fp, "CONOUT$", "w", stderr);
            //     std::ios::sync_with_stdio();
            //     system("color 0");
            //     qDebug() << "调试模式：已创建新控制台。";
            // }
            // # endif // ifndef QT_NO_DEBUG
        }
    }
}

#endif // if defined(Q_OS_WINDOWS)

int main(int argc, char *argv[])
{
#if defined(Q_OS_WINDOWS)
    AttachConsoleAndRedirect();
#endif // if defined(Q_OS_WINDOWS)

    // 让所有 qDebug / qCInfo 等 Qt 日志都汇入 CxxLog，统一落盘
    REDIRECT_QTMESSAGE_LOG_CXX(nullptr);

    exitAT();

    QGuiApplication app(argc, argv);

    // 必须在任何 QSettings 使用之前设置，AppController 依赖它定位配置
    QCoreApplication::setOrganizationName(QStringLiteral("qdownload"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("qdownload.local"));
    QCoreApplication::setApplicationName(QStringLiteral("qdownload"));
    QCoreApplication::setApplicationVersion(QStringLiteral(QDOWNLOAD_VERSION));

    // 用实例名区分日志文件，否则同机双实例会抢同一个文件
    // Logger::install(prescanInstanceTag(argc, argv));
    // qInfo().noquote() << "=== qdownload" <<
    //     QCoreApplication::applicationVersion() << "启动 ===";
    // qInfo().noquote() << "日志文件：" << Logger::logFilePath();

    // 数据落在哪儿要在打开任何文件之前定下来：程序目录写得进去就跟程序走，
    // 写不进去（Program Files / Android）才退回系统目录
    Qdl::DataPaths::init();

    QString logdir = Qdl::DataPaths::logDirectory();
    qDebug() << logdir << logdir.toLocal8Bit().data();
    std::string logdirstring = logdir.toLocal8Bit().toStdString();
    CxxLog::getInstance().initLog(logdirstring,
                                  1000, 1024 * 1024 * 10);
    CxxLog::getInstance().setLogLevel(CxxLog::LOG_TYPE_DEBUG);
    CxxLog::getInstance().setFileLogLevel(CxxLog::LOG_TYPE_DEBUG);
    CxxLog::getInstance().setPrint(true);
    CxxLog::getInstance().setColorLog(false);
    INFO_LOG_CXX("main start game\n");

    // 同机多开：先抢一个本机实例编号，再建 AppController。
    // 没有这一步，两个进程会共用一份 settings.ini、拿到同一个 peerId，
    // 组播里互相把对方当成自己的回环包丢掉——看起来就是"本机开两个程序互相发现不了"。
    Qdl::InstanceGuard::acquire();

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("qdownload — 局域网文件互传"));
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption selfTest(QStringLiteral("self-test"),
                                QStringLiteral("加载 QML 后立即退出，用于构建自检"));
    parser.addOption(selfTest);

    QCommandLineOption screenshot(QStringLiteral("screenshot"),
                                  QStringLiteral("渲染一帧并保存 PNG 后退出"),
                                  QStringLiteral("file"));
    parser.addOption(screenshot);

    QCommandLineOption sizeOpt(QStringLiteral("size"),
                               QStringLiteral("配合 --screenshot 指定窗口尺寸，如 430x820"),
                               QStringLiteral("WxH"));
    parser.addOption(sizeOpt);

    QCommandLineOption delayOpt(QStringLiteral("screenshot-delay"),
                                QStringLiteral("截图前等待毫秒数（等对端连上来或弹窗出现）"),
                                QStringLiteral("ms"));
    parser.addOption(delayOpt);

    QCommandLineOption instanceOpt(QStringLiteral("instance"),
                                   QStringLiteral(
                                       "实例名，用于同机多开联调（会覆盖 peerId 与显示名）"),
                                   QStringLiteral("tag"));
    parser.addOption(instanceOpt);

    QCommandLineOption nameOpt(QStringLiteral("name"),
                               QStringLiteral("覆盖主机显示名"),
                               QStringLiteral("text"));
    parser.addOption(nameOpt);

    QCommandLineOption peerIdOpt(QStringLiteral("peer-id"),
                                 QStringLiteral("覆盖 peerId（不落盘）"),
                                 QStringLiteral("id"));
    parser.addOption(peerIdOpt);

    QCommandLineOption probeOpt(QStringLiteral("probe"),
                                QStringLiteral("无界面运行 N 秒，打印发现到的主机后退出"),
                                QStringLiteral("seconds"));
    parser.addOption(probeOpt);

    QCommandLineOption shareOpt(QStringLiteral("share"),
                                QStringLiteral("本次运行强制开启共享（不写入设置）"));
    parser.addOption(shareOpt);

    QCommandLineOption writeOpt(QStringLiteral("write"),
                                QStringLiteral("本次运行强制开启写权限（不写入设置）"));
    parser.addOption(writeOpt);

    QCommandLineOption ctrlPortOpt(QStringLiteral("ctrl-port"),
                                   QStringLiteral("覆盖控制通道监听端口（默认 45455）"),
                                   QStringLiteral("port"));
    parser.addOption(ctrlPortOpt);

    QCommandLineOption limitOpt(QStringLiteral("limit"),
                                QStringLiteral("本次运行的传输限速（MB/s，0 = 不限速），不写入设置"),
                                QStringLiteral("mbps"));
    parser.addOption(limitOpt);

    QCommandLineOption segmentsOpt(QStringLiteral("segments"),
                                   QStringLiteral("本次运行的单文件连接数（1-8），不写入设置"),
                                   QStringLiteral("count"));
    parser.addOption(segmentsOpt);

    QCommandLineOption shareRootOpt(QStringLiteral("share-root"),
                                    QStringLiteral("本次运行覆盖共享根目录（不写入设置）"),
                                    QStringLiteral("path"));
    parser.addOption(shareRootOpt);

    QCommandLineOption receiveRootOpt(QStringLiteral("receive-root"),
                                      QStringLiteral("本次运行覆盖接收目录（不写入设置）"),
                                      QStringLiteral("path"));
    parser.addOption(receiveRootOpt);

    QCommandLineOption pairCodeOpt(QStringLiteral("pair-code"),
                                   QStringLiteral("固定配对码（联调时省去读屏）"),
                                   QStringLiteral("code"));
    parser.addOption(pairCodeOpt);

    QCommandLineOption autoApproveOpt(QStringLiteral("auto-approve"),
                                      QStringLiteral("自动放行配对请求（仅联调）"));
    parser.addOption(autoApproveOpt);

    QCommandLineOption autoApproveWriteOpt(QStringLiteral("auto-approve-write"),
                                           QStringLiteral("自动放行时授予 rw（仅联调）"));
    parser.addOption(autoApproveWriteOpt);

    QCommandLineOption approveAfterOpt(
        QStringLiteral("approve-after"),
        QStringLiteral("码校验通过后延时 N 毫秒模拟点「允许配对」（仅联调，复现手动同意路径）"),
        QStringLiteral("ms"));
    parser.addOption(approveAfterOpt);

    QCommandLineOption pairTestOpt(QStringLiteral("pair-test"),
                                   QStringLiteral(
                                       "无界面配对测试：--pair-test <peerId> <code>"));
    parser.addOption(pairTestOpt);

    QCommandLineOption watchOpt(QStringLiteral("watch"),
                                QStringLiteral(
                                    "配合 --pair-test：Ready 后继续观察 N 毫秒，打印权限变化"),
                                QStringLiteral("ms"));
    parser.addOption(watchOpt);

    QCommandLineOption listOpt(QStringLiteral("list"),
                               QStringLiteral(
                                   "配合 --pair-test：连上后读取该虚拟路径并打印结果（自动翻页）"),
                               QStringLiteral("path"));
    parser.addOption(listOpt);

    QCommandLineOption rmOpt(QStringLiteral("rm"),
                             QStringLiteral(
                                 "配合 --pair-test：删掉远端该虚拟路径的文件，再复核列表里是否已消失"),
                             QStringLiteral("path"));
    parser.addOption(rmOpt);

    QCommandLineOption rmLocalOpt(QStringLiteral("rm-local"),
                                  QStringLiteral("删除本机接收目录里的某个文件（虚拟路径）并退出"),
                                  QStringLiteral("path"));
    parser.addOption(rmLocalOpt);

    QCommandLineOption getOpt(QStringLiteral("get"),
                              QStringLiteral("配合 --pair-test：连上后下载该虚拟路径（可重复），超出并发上限的自动排队"),
                              QStringLiteral("path"));
    parser.addOption(getOpt);

    QCommandLineOption putOpt(QStringLiteral("put"),
                              QStringLiteral(
                                  "配合 --pair-test：连上后上传该本机文件（可重复），落到主机 /inbox"),
                              QStringLiteral("path"));
    parser.addOption(putOpt);

    QCommandLineOption putPauseAtOpt(QStringLiteral("put-pause-at"),
                                     QStringLiteral(
                                         "配合 --put：传到该百分比时暂停，1.2 秒后继续"),
                                     QStringLiteral("percent"));
    parser.addOption(putPauseAtOpt);

    QCommandLineOption putCancelAtOpt(QStringLiteral("put-cancel-at"),
                                      QStringLiteral(
                                          "配合 --put：传到该百分比时取消（主机应删掉半成品）"),
                                      QStringLiteral("percent"));
    parser.addOption(putCancelAtOpt);

    QCommandLineOption pauseAtOpt(QStringLiteral("pause-at"),
                                  QStringLiteral("配合 --get：下到该百分比时暂停，1.2 秒后继续"),
                                  QStringLiteral("percent"));
    parser.addOption(pauseAtOpt);

    QCommandLineOption killAtOpt(QStringLiteral("kill-at"),
                                 QStringLiteral("配合 --get：下到该百分比时硬退出（模拟杀进程）"),
                                 QStringLiteral("pct"));
    parser.addOption(killAtOpt);

    QCommandLineOption workersAtOpt(QStringLiteral("workers-at"),
                                    QStringLiteral("配合 --get：传到该百分比时改单文件连接数（格式 pct:count），验证 worker 池动态增减"),
                                    QStringLiteral("pct:count"));
    parser.addOption(workersAtOpt);

    QCommandLineOption resumeOpt(QStringLiteral("resume"),
                                 QStringLiteral("配合 --pair-test：连上后继续上次没下完的任务"));
    parser.addOption(resumeOpt);

    QCommandLineOption cancelAtOpt(QStringLiteral("cancel-at"),
                                   QStringLiteral("配合 --get：下到该百分比时取消"),
                                   QStringLiteral("percent"));
    parser.addOption(cancelAtOpt);

    QCommandLineOption toggleWriteOpt(QStringLiteral("toggle-write-after"),
                                      QStringLiteral(
                                          "N 毫秒后翻转写权限开关（验证 PERM_UPDATE）"),
                                      QStringLiteral("ms"));
    parser.addOption(toggleWriteOpt);

    QCommandLineOption disconnectAfterOpt(QStringLiteral("disconnect-after"),
                                          QStringLiteral(
                                              "配对测试用：连上 N 毫秒后手动断开，用来验证不会自动重连"),
                                          QStringLiteral("ms"));
    parser.addOption(disconnectAfterOpt);

    QCommandLineOption setNameOpt(QStringLiteral("set-name"),
                                  QStringLiteral(
                                      "无界面设置本端名称并退出（空串=恢复默认）"),
                                  QStringLiteral("name"));
    parser.addOption(setNameOpt);

    QCommandLineOption toggleShareOpt(QStringLiteral("toggle-share-after"),
                                      QStringLiteral("N 毫秒后翻转共享开关（验证列目录 403）"),
                                      QStringLiteral("ms"));
    parser.addOption(toggleShareOpt);

    // 仅用于生成文档截图：启动即打开「已配对设备」面板
    QCommandLineOption demoToggled(QStringLiteral("demo-trusted"),
                                   QStringLiteral("启动即打开已配对设备面板（截图用）"));
    parser.addOption(demoToggled);

    // 仅用于生成文档截图：窄屏布局启动即停在远端页
    QCommandLineOption demoRemoteOpt(QStringLiteral("demo-remote"),
                                     QStringLiteral("启动即切到远端 Tab（截图用）"));
    parser.addOption(demoRemoteOpt);

    QCommandLineOption demoPathOpt(QStringLiteral("demo-path"),
                                   QStringLiteral("连上主机后直接进入该虚拟路径（截图/联调用）"),
                                   QStringLiteral("path"));
    parser.addOption(demoPathOpt);

    // 仅用于生成文档截图：启动即展开传输抽屉
    QCommandLineOption demoTransfersOpt(QStringLiteral("demo-transfers"),
                                        QStringLiteral("启动即展开传输抽屉（截图用）"));
    parser.addOption(demoTransfersOpt);

    // 仅用于生成文档截图：连上后自动下载该虚拟路径，抽屉里就有真实进度
    QCommandLineOption demoGetOpt(QStringLiteral("demo-get"),
                                  QStringLiteral("连上后自动下载该虚拟路径（截图用）"),
                                  QStringLiteral("path"));
    parser.addOption(demoGetOpt);

    // 仅用于生成文档截图：连上后自动上传该本机文件，抽屉里就有真实的上传进度
    QCommandLineOption demoPutOpt(QStringLiteral("demo-put"),
                                  QStringLiteral("连上后自动上传该本机文件（截图用）"),
                                  QStringLiteral("path"));
    parser.addOption(demoPutOpt);

    QCommandLineOption langOpt(QStringLiteral("lang"),
                               QStringLiteral("界面语言：system / zh_CN / en（默认跟随系统）"),
                               QStringLiteral("code"));
    parser.addOption(langOpt);

    // 仅用于验证 i18n 热切换：N 毫秒后切到另一种语言
    QCommandLineOption demoLangSwitchOpt(QStringLiteral("demo-lang-switch"),
                                         QStringLiteral("N 毫秒后把界面语言切到另一种（截图对比用）"),
                                         QStringLiteral("ms"));
    parser.addOption(demoLangSwitchOpt);

    QCommandLineOption demoDropOpt(QStringLiteral("demo-drop"),
                                   QStringLiteral("强制显示拖拽上传提示层（截图用）"));
    parser.addOption(demoDropOpt);

    QCommandLineOption demoSettingsOpt(QStringLiteral("demo-settings"),
                                       QStringLiteral("启动即打开设置面板（截图用）"));
    parser.addOption(demoSettingsOpt);

    QCommandLineOption demoDirBrowserOpt(QStringLiteral("demo-dir-browser"),
                                         QStringLiteral("桌面也用安卓那套内置目录浏览器（排障/截图用）"));
    parser.addOption(demoDirBrowserOpt);

    QCommandLineOption demoDirRoleOpt(QStringLiteral("demo-dir-role"),
                                      QStringLiteral("0=共享根 1=接收目录，配合 --demo-dir"),
                                      QStringLiteral("role"));
    parser.addOption(demoDirRoleOpt);

    QCommandLineOption demoDirOpt(QStringLiteral("demo-dir"),
                                  QStringLiteral("到点自动切换目录并把体检结果写进日志"
                                                 "（验证改目录的接受/拒绝分支）"),
                                  QStringLiteral("path"));
    parser.addOption(demoDirOpt);

    QCommandLineOption demoConfirmWriteOpt(QStringLiteral(
                                               "demo-confirm-write-at"),
                                           QStringLiteral(
                                               "N 毫秒后打开「关闭写权限」确认框（截图用）"),
                                           QStringLiteral("ms"));
    parser.addOption(demoConfirmWriteOpt);

    QCommandLineOption trayOnCloseOpt(QStringLiteral("tray-on-close"),
                                      QStringLiteral(
                                          "关闭窗口时收进托盘而不是退出（不写入设置）"));
    parser.addOption(trayOnCloseOpt);

    QCommandLineOption demoQuitOpt(QStringLiteral("demo-quit-at"),
                                   QStringLiteral("N 毫秒后自己触发一次「关闭窗口」"
                                                  "（有传输时会先弹退出确认框）"),
                                   QStringLiteral("ms"));
    parser.addOption(demoQuitOpt);

    parser.process(app);

    launchOptions().instanceTag = parser.value(instanceOpt);
    launchOptions().nameOverride = parser.value(nameOpt);
    launchOptions().peerIdOverride = parser.value(peerIdOpt);

    // 同机双实例必须错开控制端口：Windows 上 SO_REUSEADDR 会让第二个进程
    // 也"监听成功"同一个端口，随后它的连接会落到自己身上，表现为发现得到却连不上
    if (parser.isSet(ctrlPortOpt)) launchOptions().ctrlPort =
            quint16(qBound(1, parser.value(ctrlPortOpt).toInt(), 65535));

    if (parser.isSet(shareOpt)) launchOptions().shareOverride = 1;

    if (parser.isSet(writeOpt)) launchOptions().writeOverride = 1;
    launchOptions().speedLimitOverrideMbps = parser.isSet(limitOpt)
                                             ? qMax(0, parser.value(limitOpt).toInt())
                                             : -1;
    launchOptions().segmentOverride = parser.isSet(segmentsOpt)
                                          ? qBound(1, parser.value(segmentsOpt).toInt(), 8)
                                          : 0;
    launchOptions().shareRootOverride = parser.value(shareRootOpt);
    launchOptions().receiveRootOverride = parser.value(receiveRootOpt);
    launchOptions().demoTabRemote = parser.isSet(demoRemoteOpt);
    launchOptions().demoPath = parser.value(demoPathOpt);
    launchOptions().demoTransfers = parser.isSet(demoTransfersOpt);
    launchOptions().demoGetPath = parser.value(demoGetOpt);
    launchOptions().demoPutPath = parser.value(demoPutOpt);

    // --lang 优先于设置；都没给就用中文（源码语言），想跟随系统要显式给 system
    launchOptions().language = parser.isSet(langOpt)
                               ? parser.value(langOpt)
                               : QSettings().value(QStringLiteral("ui/language"),
                                                   QStringLiteral("zh_CN")).
                               toString();

    if (parser.isSet(demoLangSwitchOpt)) launchOptions().demoLangSwitchMs = qMax(
            0,
            parser.value(demoLangSwitchOpt).toInt());
    launchOptions().demoDrop = parser.isSet(demoDropOpt);
    launchOptions().demoSettings = parser.isSet(demoSettingsOpt);
    launchOptions().demoDirBrowser = parser.isSet(demoDirBrowserOpt);
    launchOptions().demoDirRole = parser.value(demoDirRoleOpt).trimmed()
                                  == QStringLiteral("1") ? 1 : 0;
    launchOptions().demoDirPath = parser.value(demoDirOpt);
    launchOptions().closeToTrayOverride = parser.isSet(trayOnCloseOpt) ? 1 : 0;

    if (parser.isSet(demoQuitOpt)) launchOptions().demoQuitMs =
            qMax(0, parser.value(demoQuitOpt).toInt());

    if (parser.isSet(demoConfirmWriteOpt)) launchOptions().demoConfirmWriteMs =
            qMax(0, parser.value(demoConfirmWriteOpt).toInt());

    if (parser.isSet(pairCodeOpt)) launchOptions().pairCode = parser.value(
            pairCodeOpt);
    launchOptions().autoApprove = parser.isSet(autoApproveOpt);
    launchOptions().autoApproveWrite = parser.isSet(autoApproveWriteOpt);

    if (parser.isSet(approveAfterOpt)) launchOptions().approvePendingAfterMs =
            qMax(0, parser.value(approveAfterOpt).toInt());

    if (parser.isSet(toggleWriteOpt)) launchOptions().toggleWriteAfterMs = qMax(0,
                                                                                parser.value(
                                                                                    toggleWriteOpt).toInt());

    if (parser.isSet(toggleShareOpt)) launchOptions().toggleShareAfterMs = qMax(0,
                                                                                parser.value(
                                                                                    toggleShareOpt).toInt());

    // 实例名派生 peerId / 显示名，必须早于任何 AppController 创建
    if (!launchOptions().instanceTag.isEmpty()) {
        if (launchOptions().peerIdOverride.isEmpty()) launchOptions().
            peerIdOverride = QStringLiteral("dev-") + launchOptions().instanceTag;

        if (launchOptions().nameOverride.isEmpty()) launchOptions().nameOverride =
                launchOptions().instanceTag;
    }

    // 改名字要落库并广播，走的是和界面设置里「保存」完全相同的 setter
    if (parser.isSet(setNameOpt)) {
        if (!launchOptions().nameOverride.isEmpty()) {
            fprintf(stderr,
                    "--set-name 被忽略：--name（%s）是硬覆盖，优先级高于设置\n",
                    launchOptions().nameOverride.toUtf8().constData());
            return 7;
        }

        auto *nameController = new AppController();
        nameController->setDeviceName(parser.value(setNameOpt));
        printf("SET_NAME_OK %s\n",
               nameController->deviceName().toUtf8().constData());
        fflush(stdout);
        delete nameController;
        return 0;
    }

    // 本机删除：不连主机也能验，走的是和界面「删除」完全相同的入口
    if (parser.isSet(rmLocalOpt)) {
        auto *rmController = new AppController();
        const QString path = parser.value(rmLocalOpt);
        const bool ok = rmController->deleteLocalPath(path);
        printf("RM_LOCAL_TEST path=%s ok=%d\n", path.toUtf8().constData(), ok ? 1 : 0);
        fflush(stdout);
        delete rmController;
        return ok ? 0 : 11;
    }

    if (parser.isSet(pairTestOpt)) {
        // 目标主机与配对码走位置参数：Qt 的多值选项在解析时不如位置参数省事
        const QStringList args = parser.positionalArguments();

        if (args.size() < 2) {
            fprintf(stderr, "--pair-test 需要两个位置参数：<peerId> <code>\n");
            return 5;
        }

        // 堆上创建并在退出前销毁：静态对象析构发生在 QCoreApplication 之后，
        // 那时 QSqlDatabase / socket 都会报警
        auto *pairController = new AppController();
        const int watch =
            parser.isSet(watchOpt) ? parser.value(watchOpt).toInt() : -1;
        const QString listPath = parser.value(listOpt);
        const QStringList getPaths = parser.values(getOpt);
        const int     pauseAt =
            parser.isSet(pauseAtOpt) ? parser.value(pauseAtOpt).toInt() : 0;
        const int cancelAt = parser.isSet(cancelAtOpt) ? parser.value(
            cancelAtOpt).toInt() : 0;
        const int killAt =
            parser.isSet(killAtOpt) ? parser.value(killAtOpt).toInt() : 0;

        // --workers-at 50:6 → 传到 50% 时把单文件连接数改成 6
        int workersAtPercent = 0;
        int workersAtCount = 0;
        if (parser.isSet(workersAtOpt)) {
            const QStringList parts = parser.value(workersAtOpt).split(QLatin1Char(':'));
            if (parts.size() == 2) {
                workersAtPercent = parts.at(0).toInt();
                workersAtCount = parts.at(1).toInt();
            }
        }
        const QStringList putPaths = parser.values(putOpt);
        const int putPauseAt = parser.isSet(putPauseAtOpt) ? parser.value(
            putPauseAtOpt).toInt() : 0;
        const int putCancelAt = parser.isSet(putCancelAtOpt)
                                ? parser.value(putCancelAtOpt).toInt()
                                : 0;
        const int code = runPairTest(*pairController,
                                     args.at(0),
                                     args.at(1),
                                     60,
                                     watch,
                                     listPath,
                                     getPaths,
                                     pauseAt,
                                     cancelAt,
                                     killAt,
                                     parser.isSet(resumeOpt),
                                     putPaths,
                                     putPauseAt,
                                     putCancelAt,
                                     parser.isSet(disconnectAfterOpt)
                                     ? qMax(0,
                                            parser.value(disconnectAfterOpt).
                                            toInt())
                                     : 0,
                                     workersAtPercent,
                                     workersAtCount,
                                     parser.value(rmOpt));
        delete pairController;
        return code;
    }

    if (parser.isSet(probeOpt)) return runProbe(qMax(1,
                                                     parser.value(
                                                         probeOpt).toInt()));

    // 局域网 P2P：系统代理（http_proxy / 企业代理策略）绝不能套到 TCP 上，
    // 否则连接会被送到代理端口并报 “The proxy type is invalid”
    QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy);

    // Basic 是唯一能被完整自定义的样式，Theme.qml 全部自绘，避免不同平台原生样式差异
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    // 语言必须在 QML 首次求值前装好，否则首帧会出现中英文错位
    Qdl::Translation::apply(launchOptions().language);

    QQmlApplicationEngine engine;

    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, &app,
        []() {
        qCritical() << "QML 根对象创建失败，见上面的 QML 报错行";
        QCoreApplication::exit(2);
    },
        Qt::QueuedConnection);

    engine.loadFromModule("Qdownload", "Main");

    if (engine.rootObjects().isEmpty()) {
        qCritical() << "没有加载到根对象，检查 qt_add_qml_module 的 URI 与 QML_FILES 路径";
        return 1;
    }

    qInfo() << "根对象已创建，数量 =" << engine.rootObjects().size();

    if (parser.isSet(selfTest)) {
        QTimer::singleShot(800, &app, [&engine]() {
            qInfo() << "SELF-TEST OK  rootObjects =" << engine.rootObjects().size()
                    << " 日志 =" << Logger::logFilePath();
            QCoreApplication::exit(0);
        });
    }

    if (parser.isSet(screenshot)) {
        const QString path = parser.value(screenshot);
        const QString sizeArg = parser.value(sizeOpt);
        const int     delay =
            parser.isSet(delayOpt) ? qMax(0,
                                          parser.value(delayOpt).toInt()) : 900;
        QTimer::singleShot(delay, &app, [&engine, path, sizeArg]() {
            auto *window =
                qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());

            if (!window) {
                qWarning() << "截图失败：根对象不是 QQuickWindow";
                QCoreApplication::exit(3);
                return;
            }

            const QStringList parts = sizeArg.split(QLatin1Char('x'));

            if (parts.size() == 2) {
                window->resize(parts.at(0).toInt(), parts.at(1).toInt());
                qInfo() << "窗口尺寸调整为" << window->size();
            }

            QTimer::singleShot(450, window, [window, path]() {
                const QImage image = window->grabWindow();

                if (image.isNull() || !image.save(path)) {
                    qWarning() << "截图失败：图像为空或无法写入" << path;
                    QCoreApplication::exit(3);
                    return;
                }
                qInfo().noquote() << "截图已保存：" << path << image.size();
                QCoreApplication::exit(0);
            });
        });
    }

    return app.exec();
}
