#!/usr/bin/env bash
#
# Linux 出包（M11）：便携包（tar.gz，自带 Qt 运行时）+ 可选 .deb。
#
# 产出：
#   dist/qdownload-<版本>-linux-<架构>.tar.gz   解压即用，双击 run 或 AppRun
#   dist/qdownload_<版本>_<deb架构>.deb         装到 /usr，应用菜单里能搜到
#
# 用法：
#   bash tools/package_linux.sh                 # Release 构建 + 便携包 + deb
#   bash tools/package_linux.sh --debug
#   bash tools/package_linux.sh --skip-build    # 已有构建目录，只重新打包
#   bash tools/package_linux.sh --no-deb        # 只要便携包
#   bash tools/package_linux.sh --check         # 只做环境自检，不构建
#
# 环境变量：
#   QT_PREFIX=<Qt 安装前缀>      不设置时自动探测（qmake6 → $HOME/Qt/6.*/gcc_64 → /usr/lib/qt6）
#   QT_ROOT=<Qt 版本根目录>      自动探测时用（默认 $HOME/Qt）
#   BUILD_DIR=<构建目录>         默认 build/linux-<构建类型小写>
#   JOBS=<并行数>                默认 CPU 核数
#
# 设计取舍：
#   1. **只搬 Qt 自己的库**（libQt6* / libicu*）。libc、libX11、libGL 这类跟发行版
#      强绑定，一起搬反而更容易起不来——这是 linuxdeployqt 挨骂最多的地方。
#   2. **不做 patchelf**。改用 LD_LIBRARY_PATH + qt.conf 相对路径：少一个外部依赖，
#      在只有 CMake 和 Qt 的机器上也能跑完。
#   3. **先装到 staging 再换上**。打包中途失败时 dist 里上一个能用的包还是完整的。
set -euo pipefail

# shellcheck source=tools/common.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

BUILD_TYPE=Release
DO_BUILD=1
DO_DEB=1
DO_APPIMAGE=1
CHECK_ONLY=0
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"
BUILD_DIR="${BUILD_DIR:-}"

while [ $# -gt 0 ]; do
    case "$1" in
        --release) BUILD_TYPE=Release ;;
        --debug) BUILD_TYPE=Debug ;;
        --skip-build) DO_BUILD=0 ;;
        --no-deb) DO_DEB=0 ;;
        --no-appimage) DO_APPIMAGE=0 ;;
        --check) CHECK_ONLY=1 ;;
        -j | --jobs)
            shift
            JOBS="${1:-4}"
            ;;
        -h | --help)
            cat <<'USAGE'
用法：bash tools/package_linux.sh [选项]

  --release          Release 构建（默认）
  --debug            Debug 构建
  --skip-build       已有构建目录，只重新打包
  --no-deb           不出 .deb
  --no-appimage      不出 AppImage
  --check            只做环境自检，不构建
  -j, --jobs N       并行编译数（默认 CPU 核数）

环境变量：
  QT_PREFIX=<Qt 安装前缀>   含 bin/ 与 lib/ 的那一层
  QT_ROOT=<Qt 版本根目录>    自动探测用（默认 $HOME/Qt）
  BUILD_DIR=<构建目录>       默认 build/linux-<构建类型>
  JOBS=<并行数>
USAGE
            exit 0
            ;;
        *) qd_die "未知参数：$1（用 -h 看用法）" ;;
    esac
    shift
done

BUILD_LC="$(printf '%s' "$BUILD_TYPE" | tr '[:upper:]' '[:lower:]')"
[ -n "$BUILD_DIR" ] || BUILD_DIR="$QD_ROOT/build/linux-$BUILD_LC"
VERSION="$(qd_version)"
ARCH="$(qd_portable_arch)"
DEB_ARCH="$(qd_deb_arch)"

echo "qdownload Linux 出包"
echo "  版本：$VERSION   架构：$ARCH   构建类型：$BUILD_TYPE"
echo "  构建目录：$BUILD_DIR"
echo

# ---------------------------------------------------------------- 环境自检

echo "== 环境自检"

QT_PREFIX="$(qd_find_qt_prefix)"

# 优先用 Qt 自带的 qt-cmake：它已经把 CMAKE_PREFIX_PATH / 工具链设好了，
# 比手传 -DCMAKE_PREFIX_PATH 少一类"找到别处 Qt"的坑
CMAKE_BIN="cmake"
if [ -x "$QT_PREFIX/bin/qt-cmake" ]; then
    CMAKE_BIN="$QT_PREFIX/bin/qt-cmake"
elif ! command -v cmake >/dev/null 2>&1; then
    qd_die "找不到 cmake（Debian/Ubuntu: sudo apt install cmake）"
fi
qd_check_line yes cmake "$CMAKE_BIN"

GEN=""
GEN_NAME=""
if command -v ninja >/dev/null 2>&1; then
    GEN="Ninja"
    GEN_NAME="ninja"
elif command -v make >/dev/null 2>&1; then
    GEN="Unix Makefiles"
    GEN_NAME="make"
fi
[ -n "$GEN" ] || qd_die "找不到 ninja 或 make（Debian/Ubuntu: sudo apt install ninja-build）"
qd_check_line yes 生成器 "$GEN ($GEN_NAME)"

QT_VERSION="$("$QT_PREFIX/bin/qmake6" -query QT_VERSION 2>/dev/null ||
    "$QT_PREFIX/bin/qmake" -query QT_VERSION 2>/dev/null || echo 未知)"
qd_check_line yes Qt "$QT_PREFIX（$QT_VERSION）"

[ -x "$QT_PREFIX/bin/lupdate" ] || [ -x "$QT_PREFIX/bin/lrelease" ] ||
    qd_warn "Qt 前缀里没有 lupdate/lrelease，翻译相关的 CMake 步骤可能失败"

command -v ldd >/dev/null 2>&1 || qd_die "找不到 ldd（glibc 自带，不该缺）"
qd_check_line yes ldd "$(command -v ldd)"

if [ "$DO_DEB" = "1" ] && command -v dpkg-deb >/dev/null 2>&1; then
    qd_check_line yes dpkg-deb "$(command -v dpkg-deb)（会产出 .deb）"
else
    DO_DEB=0
    qd_check_line no dpkg-deb "没有就不出 .deb，只出便携包"
fi

APPIMAGETOOL=""
if [ "$DO_APPIMAGE" = "1" ]; then
    APPIMAGETOOL="$(command -v appimagetool 2>/dev/null ||
        command -v appimagetool-x86_64.AppImage 2>/dev/null || true)"
    if [ -n "$APPIMAGETOOL" ]; then
        qd_check_line yes appimagetool "$APPIMAGETOOL"
    else
        DO_APPIMAGE=0
        qd_check_line no appimagetool "没有就不出 AppImage，只出便携包与 deb"
    fi
fi

if [ "$CHECK_ONLY" = "1" ]; then
    echo
    qd_info "自检结束（--check，未构建）"
    exit 0
fi

# ---------------------------------------------------------------- 构建

APPDIR_NAME="qdownload-${VERSION}-linux-${ARCH}"
STAGE="$QD_ROOT/dist/.staging-$APPDIR_NAME"
APPDIR="$STAGE/$APPDIR_NAME"

if [ "$DO_BUILD" = "1" ]; then
    qd_info "配置（$BUILD_TYPE / $GEN）"
    qd_need "$GEN_NAME" "Debian/Ubuntu: sudo apt install ninja-build make"
    "$CMAKE_BIN" -S "$QD_ROOT" -B "$BUILD_DIR" -G "$GEN" \
        -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
        -DCMAKE_PREFIX_PATH="$QT_PREFIX"

    qd_info "编译（-$JOBS）"
    cmake --build "$BUILD_DIR" --parallel "$JOBS"
else
    [ -d "$BUILD_DIR" ] || qd_die "--skip-build 但构建目录不存在：$BUILD_DIR"
    qd_info "跳过构建（--skip-build）"
fi

EXE="$BUILD_DIR/qdownload"
[ -x "$EXE" ] || qd_die "构建产物不是可执行文件：$EXE"

# 打包前先跑一遍自检，QML 语法/类型错误在这里就暴露，别等到用户解压后。
# 用 offscreen 平台：CI / 容器里没有显示器，不能因为缺 X 就卡住打包。
qd_info "运行 --self-test"
QT_QPA_PLATFORM=offscreen "$EXE" --self-test >/dev/null 2>&1 ||
    qd_warn "self-test 非零退出，继续打包（详见构建目录日志）"

# ---------------------------------------------------------------- 组装 AppDir

rm -rf "$STAGE"
mkdir -p "$STAGE"

qd_info "安装到 staging"
# 直接 --prefix 到 <AppDir>/usr：AppDir 的标准布局，qt.conf 与 AppRun
# 里的相对路径都以此为准（比 DESTDIR 少一层心智负担）
cmake --install "$BUILD_DIR" --prefix "$APPDIR/usr"

# 装完必须落在 <AppDir>/usr/bin
[ -x "$APPDIR/usr/bin/qdownload" ] ||
    qd_die "install 后没找到 $APPDIR/usr/bin/qdownload（检查 CMakeLists 的 install 规则）"

# --- Qt 运行时：库 / 插件 / QML 模块 ---

declare -A QD_SEEN=()
QD_BUNDLED=0

# 递归补齐依赖：只搬落在 Qt 前缀下的 libQt6* 与 libicu*，系统库一律不碰
qd_bundle_deps() {
    local appdir="$1" qt_prefix="$2"
    shift 2
    local libdir="$appdir/usr/lib"
    mkdir -p "$libdir"

    local todo=("$@")
    local f line dep base
    while [ "${#todo[@]}" -gt 0 ]; do
        f="${todo[0]}"
        todo=("${todo[@]:1}")
        [ -n "${QD_SEEN[$f]:-}" ] && continue
        QD_SEEN[$f]=1

        # 只处理 ELF：bin 下混着脚本、qml 目录里有 qmldir 等文本文件
        [ "$(head -c 4 "$f" | od -An -tx1 | tr -d ' \n')" = "7f454c46" ] || continue

        while IFS= read -r line; do
            dep="$(qd_ldd_path "$line")"
            [ -n "$dep" ] || continue
            case "$dep" in
                "$qt_prefix"/*) ;;
                *) continue ;;
            esac
            case "$(basename "$dep")" in
                libQt6* | libicu*) ;;
                *) continue ;;
            esac
            base="$(basename "$dep")"
            if [ ! -e "$libdir/$base" ]; then
                cp -L "$dep" "$libdir/$base"
                chmod 0755 "$libdir/$base"
                todo+=("$libdir/$base")
                QD_BUNDLED=$((QD_BUNDLED + 1))
            fi
        done < <(ldd "$f" 2>/dev/null)
    done
}

# 从一行 ldd 输出里取被解析到的绝对路径；取不到（not found / vdso）就空
qd_ldd_path() {
    local line="$1" p
    case "$line" in
        *"=> not found"*)
            qd_warn "依赖缺失：$(printf '%s' "${line%%=>*}" | sed 's/[[:space:]]*$//')（目标机需自行安装）"
            return 0
            ;;
        *"=>"*) p="${line#*=> }" ;;
        /*) p="$line" ;;
        *) return 0 ;;
    esac
    p="${p%% (*}"
    p="$(printf '%s' "$p" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')"
    case "$p" in /*) printf '%s' "$p" ;; esac
}

qd_info "收集 Qt 运行库"
qd_bundle_deps "$APPDIR" "$QT_PREFIX" "$APPDIR/usr/bin/qdownload"

qd_info "收集 Qt 插件"
# 只搬这几类：Quick 渲染（含 xcb/Wayland）、图片与图标、TLS、SQLite。
# 全量搬会把 sensors/gamepad 之类连同它们的系统依赖一起拖进来。
QT_PLUGIN_DIRS="platforms platformthemes platforminputcontexts xcbglintegrations
wayland-decoration-client wayland-graphics-integration-client
wayland-graphics-integration-server wayland-shell-integration
imageformats iconengines tls sqldrivers networkinformation generic"
for d in $QT_PLUGIN_DIRS; do
    if [ -d "$QT_PREFIX/plugins/$d" ]; then
        mkdir -p "$APPDIR/usr/plugins/$d"
        cp -a "$QT_PREFIX/plugins/$d/." "$APPDIR/usr/plugins/$d/"
        qd_step "插件 $d"
    fi
done

qd_info "收集 QML 模块"
QML_MODULES="$(mktemp)"
if [ -x "$QT_PREFIX/bin/qmlimportscanner" ] || [ -x "$QT_PREFIX/libexec/qmlimportscanner" ]; then
    QML_SCANNER="$QT_PREFIX/bin/qmlimportscanner"
    [ -x "$QML_SCANNER" ] || QML_SCANNER="$QT_PREFIX/libexec/qmlimportscanner"
    "$QML_SCANNER" -rootPath "$QD_ROOT/qml" -importPath "$QT_PREFIX/qml" 2>/dev/null |
        sed -n 's/.*"relativePath"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' |
        sort -u >"$QML_MODULES" || true
fi

if [ -s "$QML_MODULES" ]; then
    while IFS= read -r rel; do
        # 自己的 QML 模块已经编进可执行文件，不用搬
        case "$rel" in Qdownload | Qdownload/*) continue ;; esac
        [ -d "$QT_PREFIX/qml/$rel" ] || continue
        mkdir -p "$APPDIR/usr/qml/$(dirname "$rel")"
        cp -a "$QT_PREFIX/qml/$rel" "$APPDIR/usr/qml/$rel"
        qd_step "QML $rel"
    done <"$QML_MODULES"
else
    qd_warn "qmlimportscanner 不可用，整棵 qml/ 一起搬（包会大一些）"
    cp -a "$QT_PREFIX/qml" "$APPDIR/usr/qml"
fi
rm -f "$QML_MODULES"

# 插件与 QML 里的 .so 也有依赖，补最后一轮
while IFS= read -r so; do
    qd_bundle_deps "$APPDIR" "$QT_PREFIX" "$so"
done < <(find "$APPDIR/usr/plugins" "$APPDIR/usr/qml" -name '*.so*' -type f 2>/dev/null)
qd_step "共 $QD_BUNDLED 个 Qt 库"

# --- 启动器与入口文件 ---

qd_info "写入启动器"
cat >"$APPDIR/usr/bin/qt.conf" <<'EOF'
[Paths]
# 相对 qt.conf 所在目录（usr/bin）解析：.. → usr
Prefix = ..
Plugins = plugins
Qml2Imports = qml
EOF

cat >"$APPDIR/AppRun" <<'EOF'
#!/bin/sh
# 便携包启动器：把自带 Qt 库排在最前面，再启动真正的可执行文件。
# 用 LD_LIBRARY_PATH 而不是 patchelf，是为了不依赖额外工具。
HERE="$(cd "$(dirname "$0")" && pwd)"
export LD_LIBRARY_PATH="$HERE/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export QT_QPA_PLATFORM_PLUGIN_PATH="$HERE/usr/plugins/platforms"
exec "$HERE/usr/bin/qdownload" "$@"
EOF
chmod 0755 "$APPDIR/AppRun"

cp "$QD_ROOT/assets/icons/qdownload.svg" "$APPDIR/qdownload.svg"
cp "$QD_ROOT/packaging/linux/qdownload.desktop" "$APPDIR/qdownload.desktop"

cat >"$APPDIR/install.sh" <<'EOF'
#!/bin/sh
# 把便携包装进系统（默认 /usr/local），装完可从应用菜单启动。
# 用法：sudo ./install.sh [前缀]     例：sudo ./install.sh /opt/qdownload
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
PREFIX="${1:-/usr/local}"
TARGET="$PREFIX/lib/qdownload"

[ -w "$PREFIX" ] || { echo "对 $PREFIX 没有写权限，请用 sudo 运行" >&2; exit 1; }

rm -rf "$TARGET"
mkdir -p "$TARGET" "$PREFIX/bin" "$PREFIX/share/applications" \
         "$PREFIX/share/icons/hicolor/scalable/apps"

cp -a "$HERE/usr/bin" "$HERE/usr/lib" "$TARGET/"
[ -d "$HERE/usr/plugins" ] && cp -a "$HERE/usr/plugins" "$TARGET/"
[ -d "$HERE/usr/qml" ] && cp -a "$HERE/usr/qml" "$TARGET/"

sed "s|^Exec=.*|Exec=$TARGET/bin/qdownload %u|" \
    "$HERE/qdownload.desktop" >"$PREFIX/share/applications/qdownload.desktop"
cp "$HERE/qdownload.svg" "$PREFIX/share/icons/hicolor/scalable/apps/qdownload.svg"

cat >"$PREFIX/bin/qdownload" <<EOS
#!/bin/sh
export LD_LIBRARY_PATH="$TARGET/lib\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}"
export QT_QPA_PLATFORM_PLUGIN_PATH="$TARGET/plugins/platforms"
exec "$TARGET/bin/qdownload" "\$@"
EOS
chmod 0755 "$PREFIX/bin/qdownload"

command -v update-desktop-database >/dev/null 2>&1 &&
    update-desktop-database -q "$PREFIX/share/applications" || true

echo "已安装到 $TARGET，启动器 $PREFIX/bin/qdownload"
EOF
chmod 0755 "$APPDIR/install.sh"

cat >"$APPDIR/使用说明.txt" <<EOF
qdownload — 局域网文件互传（Linux 便携版 $VERSION / $ARCH）
==========================================================

运行
----
解压后执行 ./AppRun（或 ./run.sh），无需事先安装 Qt。
想装进系统用：sudo ./install.sh         # 默认装到 /usr/local
                sudo ./install.sh /opt/qdownload

两台机器互传
------------
1. 两台机器接在同一个局域网；
2. 共享方打开顶部「共享」开关，并在「设置」里确认共享根目录；
3. 另一方在上方下拉框里选中这台机器 →「配对」→ 输入对方显示的 6 位数字；
4. 左栏本机接收目录，右栏对方共享目录，点文件右侧的「下载」/「上传」。

上传到对方机器时：
- 接收方必须同时打开「共享」和「写权限」；
- 文件落在对方共享根下的 inbox 目录；
- 支持断点续传，双方都会校验 SHA-256。

找不到对方
----------
- 组播地址 239.255.42.99:45454 被交换机/AP 屏蔽时会走黄条提示，
  此时只能靠同一网段的手动连接；
- 防火墙需要放行 TCP 45455（控制）与 45456 起（数据）。

配置与日志
----------
配置：~/.local/share/qdownload/qdownload/
日志：同目录下 logs/，按天与实例名分文件。
EOF

ln -sf AppRun "$APPDIR/run.sh" 2>/dev/null || cp "$APPDIR/AppRun" "$APPDIR/run.sh"

# ---------------------------------------------------------------- 便携包

qd_info "打包 tar.gz"
mkdir -p "$QD_ROOT/dist"
TARBALL="$QD_ROOT/dist/$APPDIR_NAME.tar.gz"
tar -czf "$TARBALL" -C "$STAGE" "$APPDIR_NAME"

# ---------------------------------------------------------------- deb

if [ "$DO_DEB" = "1" ]; then
    qd_info "打包 deb"
    DEB_ROOT="$STAGE/deb"
    DEB_NAME="qdownload_${VERSION}_${DEB_ARCH}"
    rm -rf "$DEB_ROOT"
    mkdir -p "$DEB_ROOT/DEBIAN"
    cp -a "$APPDIR/usr" "$DEB_ROOT/usr"
    # deb 里不需要便携启动器
    rm -f "$DEB_ROOT/usr/bin/qt.conf"

    INSTALLED_KB="$(du -sk "$DEB_ROOT/usr" | awk '{print $1}')"
    cat >"$DEB_ROOT/DEBIAN/control" <<EOF
Package: qdownload
Version: ${VERSION}
Section: net
Priority: optional
Architecture: ${DEB_ARCH}
Maintainer: qdownload <noreply@qdownload.local>
Installed-Size: ${INSTALLED_KB}
Depends: libc6, libstdc++6, libgcc-s1, libglib2.0-0t64 | libglib2.0-0, libgl1, libegl1, libxkbcommon0, libdbus-1-3, libfontconfig1, libfreetype6, zlib1g
Homepage: https://github.com/qdownload/qdownload
Description: 局域网文件互传工具
 跨平台（Windows / Linux / Android）的局域网文件互传：组播自动发现邻居、
 配对码鉴权、上传需显式授权、断点续传、并发与限速可调。
EOF

    cat >"$DEB_ROOT/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database -q /usr/share/applications || true
fi
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache -q -t -f /usr/share/icons/hicolor || true
fi
EOF
    chmod 0755 "$DEB_ROOT/DEBIAN/postinst"

    if dpkg-deb --build --root-owner-group "$DEB_ROOT" "$QD_ROOT/dist/$DEB_NAME.deb" >/dev/null; then
        qd_step "dist/$DEB_NAME.deb"
    else
        # 老版本 dpkg 不认 --root-owner-group，退回无参数形式
        dpkg-deb --build "$DEB_ROOT" "$QD_ROOT/dist/$DEB_NAME.deb"
    fi
fi

# ---------------------------------------------------------------- AppImage

if [ "$DO_APPIMAGE" = "1" ] && [ -n "$APPIMAGETOOL" ]; then
    qd_info "打包 AppImage"
    APPIMAGE_OUT="$QD_ROOT/dist/qdownload-${VERSION}-linux-${ARCH}.AppImage"
    if "$APPIMAGETOOL" "$APPDIR" "$APPIMAGE_OUT" >/dev/null 2>&1; then
        qd_step "dist/$(basename "$APPIMAGE_OUT")"
    else
        qd_warn "appimagetool 执行失败，跳过 AppImage（便携包与 deb 已产出）"
    fi
fi

# ---------------------------------------------------------------- 收尾

rm -rf "$STAGE"

qd_info "完成"
ls -lh "$QD_ROOT/dist" | sed 's/^/   /'
