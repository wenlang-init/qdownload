#!/usr/bin/env bash
#
# Android 出包（M11）：配置 Qt Android 套件 → 编 .so → 出 APK / AAB。
#
# 产出：dist/qdownload-<版本>-android-<abi>.apk（或 .aab）
#
# 用法：
#   bash tools/package_android.sh                    # arm64-v8a Release APK
#   bash tools/package_android.sh -a x86_64          # 换 ABI（同步换 Qt 套件）
#   bash tools/package_android.sh --aab              # 出 .aab（上架用）
#   bash tools/package_android.sh --debug
#   bash tools/package_android.sh --skip-build       # 已 configure 过，只出包
#   bash tools/package_android.sh --install          # 出包后 adb install -r
#   bash tools/package_android.sh --check            # 只自检环境
#   bash tools/package_android.sh -j 8
#
# 环境变量（不设就自动探测）：
#   QT_ANDROID_PREFIX=<.../Qt/6.x.y/android_arm64_v8a>   Qt Android 套件目录
#   QT_ROOT=<.../Qt>                                     Qt 版本根目录
#   JAVA_HOME=<jdk 17+ 目录>                             Gradle 9 要求 JDK 17+
#   ANDROID_SDK_ROOT / ANDROID_HOME=<.../SDK>
#   ANDROID_NDK_ROOT=<.../SDK/ndk/27.2.12479018>    （也可用 --ndk <路径> 指定）
#
# 签名（可选；不设就出**未签名**的 release 包，装不上真机）：
#   export QT_ANDROID_KEYSTORE_PATH=~/.qdownload/qdownload.keystore
#   export QT_ANDROID_KEYSTORE_ALIAS=qdownload
#   export QT_ANDROID_KEYSTORE_STORE_PASS='******'
#   export QT_ANDROID_KEYSTORE_KEY_PASS='******'
#   没有 keystore 就先跑：bash tools/android_keystore.sh
#
# 坑位备忘（详见 README「Android 构建」）：
#   - manifest 里不许写 <uses-sdk>，版本由 CMake 的 QT_ANDROID_MIN/TARGET_SDK_VERSION 下发
#   - minSdk 必须 >= 28（Qt 6.12 硬要求）
#   - Gradle 固定 9.5.1，要 JDK 17+
set -euo pipefail

# shellcheck source=tools/common.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

ABI="arm64-v8a"
BUILD_TYPE=Release
BUNDLE="apk"
DO_BUILD=1
DO_INSTALL=0
CHECK_ONLY=0
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"
BUILD_DIR="${BUILD_DIR:-}"

while [ $# -gt 0 ]; do
    case "$1" in
        -a | --abi)
            shift
            ABI="${1:-}"
            ;;
        --ndk)
            shift
            export ANDROID_NDK_ROOT="${1:-}"
            ;;
        --aab) BUNDLE="aab" ;;
        --apk) BUNDLE="apk" ;;
        --release) BUILD_TYPE=Release ;;
        --debug) BUILD_TYPE=Debug ;;
        --skip-build) DO_BUILD=0 ;;
        --install) DO_INSTALL=1 ;;
        --check) CHECK_ONLY=1 ;;
        -j | --jobs)
            shift
            JOBS="${1:-4}"
            ;;
        -h | --help)
            cat <<'USAGE'
用法：bash tools/package_android.sh [选项]

  -a, --abi <abi>    arm64-v8a（默认）| armeabi-v7a | x86 | x86_64
                     会同步切换到对应的 Qt Android 套件目录
  --apk / --aab      出 APK（默认）还是 AAB（上架用）
  --release/--debug  构建类型
  --skip-build       跳过 configure 与编译，只跑出包步骤
  --install          出包后 adb install -r 到已连接设备
  --check            只做环境自检，不构建
  -j, --jobs N       并行编译数

未签名包装不上真机；配 keystore 见脚本头部注释与 README。
USAGE
            exit 0
            ;;
        *) qd_die "未知参数：$1（用 -h 看用法）" ;;
    esac
    shift
done

# ABI 先做一次校验，顺便拿到 Qt 套件目录名（Qt 的命名跟 Gradle 的不一样）
QT_ABI_DIR="$(qd_qt_android_dirname "$ABI")"
[ -n "$QT_ABI_DIR" ] || qd_die "不支持的 ABI：$ABI（可选 arm64-v8a / armeabi-v7a / x86 / x86_64）"

BUILD_LC="$(printf '%s' "$BUILD_TYPE" | tr '[:upper:]' '[:lower:]')"
[ -n "$BUILD_DIR" ] || BUILD_DIR="$QD_ROOT/build/android-${ABI}-${BUILD_LC}"
VERSION="$(qd_version)"

echo "qdownload Android 出包"
echo "  版本：$VERSION   ABI：$ABI   构建类型：$BUILD_TYPE"
echo "  Qt 套件目录名：$QT_ABI_DIR"
echo "  构建目录：$BUILD_DIR"
echo

# ---------------------------------------------------------------- 环境自检

echo "== 环境自检"

QT_PREFIX="$(qd_find_qt_android_prefix "$ABI")"
qd_check_line yes "Qt Android" "$QT_PREFIX"

QT_CMAKE=""
if [ -x "$QT_PREFIX/bin/qt-cmake" ]; then
    QT_CMAKE="$QT_PREFIX/bin/qt-cmake"
elif [ -x "$QT_PREFIX/bin/qt-cmake.bat" ]; then
    QT_CMAKE="$QT_PREFIX/bin/qt-cmake.bat"
else
    qd_die "Qt 套件里没有 qt-cmake：$QT_PREFIX/bin/"
fi
qd_check_line yes qt-cmake "$QT_CMAKE"

JAVA_HOME="$(qd_find_jdk)"
export JAVA_HOME
qd_check_line yes JDK "$JAVA_HOME（$(qd_jdk_major "$JAVA_HOME")）"
export PATH="$JAVA_HOME/bin:$PATH"

SDK="$(qd_find_android_sdk)"
export ANDROID_SDK_ROOT="$SDK"
export ANDROID_HOME="$SDK"
qd_check_line yes "Android SDK" "$SDK"

NDK="$(qd_find_android_ndk "$SDK")"
export ANDROID_NDK_ROOT="$NDK"
export ANDROID_NDK_HOME="$NDK"
qd_check_line yes NDK "$NDK"

GEN=""
GEN_NAME=""
if command -v ninja >/dev/null 2>&1; then
    GEN="Ninja"
    GEN_NAME="ninja"
else
    # Windows 上 Qt Android 构建默认生成器用 make，Qt Creator 带的是 jom
    qd_prepare_windows_path
    if command -v jom >/dev/null 2>&1; then
        GEN="NMake Makefiles JOM"
        GEN_NAME="jom"
    elif command -v make >/dev/null 2>&1; then
        GEN="Unix Makefiles"
        GEN_NAME="make"
    fi
fi
[ -n "$GEN" ] || qd_die "找不到 ninja / jom / make（ninja 最简单：Qt 的 Tools/Ninja 或 apt install ninja-build）"
qd_check_line yes 生成器 "$GEN ($GEN_NAME)"

# 签名：四个变量齐全才开，缺一个就明确说清后果
SIGN_ARGS=()
SIGN_STATE="关（将产出**未签名**包，不能装真机）"
if [ -n "${QT_ANDROID_KEYSTORE_PATH:-}" ]; then
    missing=""
    [ -n "${QT_ANDROID_KEYSTORE_ALIAS:-}" ] || missing="$missing QT_ANDROID_KEYSTORE_ALIAS"
    [ -n "${QT_ANDROID_KEYSTORE_STORE_PASS:-}" ] || missing="$missing QT_ANDROID_KEYSTORE_STORE_PASS"
    [ -n "${QT_ANDROID_KEYSTORE_KEY_PASS:-}" ] || missing="$missing QT_ANDROID_KEYSTORE_KEY_PASS"
    [ -z "$missing" ] || qd_die "签名配置不完整，还缺：$missing"
    [ -f "$QT_ANDROID_KEYSTORE_PATH" ] || qd_die "keystore 不存在：$QT_ANDROID_KEYSTORE_PATH"
    SIGN_ARGS+=(
        "-DQT_ANDROID_KEYSTORE_PATH=$QT_ANDROID_KEYSTORE_PATH"
        "-DQT_ANDROID_KEYSTORE_ALIAS=$QT_ANDROID_KEYSTORE_ALIAS"
        "-DQT_ANDROID_KEYSTORE_STORE_PASS=$QT_ANDROID_KEYSTORE_STORE_PASS"
        "-DQT_ANDROID_KEYSTORE_KEY_PASS=$QT_ANDROID_KEYSTORE_KEY_PASS"
    )
    SIGN_STATE="开（$QT_ANDROID_KEYSTORE_PATH）"
fi
qd_check_line yes 签名 "$SIGN_STATE"

if [ "$DO_INSTALL" = "1" ]; then
    command -v adb >/dev/null 2>&1 || qd_die "--install 需要 adb（SDK 的 platform-tools，加进 PATH 即可）"
    qd_check_line yes adb "$(command -v adb)"
fi

if [ "$CHECK_ONLY" = "1" ]; then
    echo
    qd_info "自检结束（--check，未构建）"
    exit 0
fi

# ---------------------------------------------------------------- 配置与编译

if [ "$DO_BUILD" = "1" ]; then
    qd_info "配置（$BUILD_TYPE / $GEN / $ABI）"
    "$QT_CMAKE" -S "$QD_ROOT" -B "$BUILD_DIR" -G "$GEN" \
        -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
        -DQT_ANDROID_BUILD_ALL_ABIS=OFF \
        -DANDROID_ABI="$ABI" \
        ${SIGN_ARGS[@]+"${SIGN_ARGS[@]}"}

    qd_info "编译 .so（-$JOBS）"
    cmake --build "$BUILD_DIR" --target qdownload --parallel "$JOBS"
else
    [ -d "$BUILD_DIR" ] || qd_die "--skip-build 但构建目录不存在：$BUILD_DIR"
    qd_info "跳过构建（--skip-build）"
fi

# ---------------------------------------------------------------- 出包

qd_info "打 $BUNDLE（Gradle，首次会下依赖，慢是正常的）"
# 关掉 Gradle 守护进程：它会一直持有 stdout/stderr，
# 导致 `... | tail` 这类管道在构建结束后还等 EOF，CI 上表现为莫名卡住
export GRADLE_OPTS="${GRADLE_OPTS:-} -Dorg.gradle.daemon=false"
cmake --build "$BUILD_DIR" --target "$BUNDLE"

# 产物路径随 Gradle/AGP 版本变，按 mtime 取最新一份，不写死目录名
OUT_DIR="$BUILD_DIR/android-build"
[ -d "$OUT_DIR" ] || OUT_DIR="$(find "$BUILD_DIR" -maxdepth 1 -type d -name 'android-build*' |
    sort | tail -n 1)"
[ -n "$OUT_DIR" ] && [ -d "$OUT_DIR" ] || qd_die "没找到 android-build 目录，Gradle 可能失败了"

ARTIFACT="$(find "$OUT_DIR" -type f \( -name "*.${BUNDLE}" \) -printf '%T@ %p\n' 2>/dev/null |
    sort -n | tail -n 1 | cut -d' ' -f2-)"
[ -n "$ARTIFACT" ] && [ -f "$ARTIFACT" ] ||
    qd_die "没找到 .${BUNDLE} 产物：在 $OUT_DIR 下搜不到"

mkdir -p "$QD_ROOT/dist"
FINAL="$QD_ROOT/dist/qdownload-${VERSION}-android-${ABI}.${BUNDLE}"
cp -f "$ARTIFACT" "$FINAL"

qd_info "完成"
echo "   产物：$FINAL"
echo "   大小：$(du -h "$FINAL" | awk '{print $1}')"

case "$ARTIFACT" in
    *unsigned*) qd_warn "这是**未签名**包，真机装不上。配 keystore 再跑一次即可（见脚本头部注释）" ;;
esac

if [ "$DO_INSTALL" = "1" ]; then
    # 未签名包 adb install 必然失败（INSTALL_PARSE_FAILED_NO_CERTIFICATES），
    # 与其让它报一句看不懂的错，不如在这里直接说清原因
    case "$FINAL" in
        *unsigned*)
            qd_die "未签名包装不上设备，跳过 --install。先配 keystore：
   bash tools/android_keystore.sh
   source ~/.qdownload/signing.env
   bash tools/package_android.sh --install"
            ;;
    esac
    qd_info "安装到设备"
    adb install -r "$FINAL"
fi
