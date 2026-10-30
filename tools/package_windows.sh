#!/usr/bin/env bash
#
# Windows 便携包（M8）。
#
# 产出一个自带 Qt 运行时的目录，整目录拷到别的 Windows 机器上双击即可运行，
# 不需要目标机装 Qt 或 MinGW。想做成单文件 exe 可以再套 enigma virtual box，
# 这里保持"目录 + 依赖"的形式，排查问题更直观。
#
# 用法：bash tools/package_windows.sh
set -euo pipefail

cd "$(dirname "$0")/.."

QT_BIN="${QT_BIN:-D:/Qt/6.12.0/mingw_64/bin}"
MINGW_BIN="${MINGW_BIN:-D:/Qt/Tools/mingw1310_64/bin}"
BUILD_DIR="${BUILD_DIR:-build}"

VERSION="$(sed -n 's/^project(qdownload VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)"
DIST="dist/qdownload-${VERSION}-win64"
# 先装到临时目录，成功后再整体换上去：windeployqt 中途失败时，
# 上一个能用的包还是完整的，不会出现"删了一半"的状态
STAGE="dist/.staging-qdownload-${VERSION}-win64"

if [ ! -f "$BUILD_DIR/qdownload.exe" ]; then
    echo "找不到 $BUILD_DIR/qdownload.exe，先构建：cmake --build $BUILD_DIR" >&2
    exit 1
fi

rm -rf "$STAGE"
mkdir -p "$STAGE"
cp "$BUILD_DIR/qdownload.exe" "$STAGE/"
DIST="$STAGE"

echo "== 收集 Qt 依赖（含 QML 模块与插件）"
"$QT_BIN/windeployqt.exe" \
    --release \
    --qmldir qml \
    --no-system-d3d-compiler \
    --no-opengl-sw \
    "$DIST/qdownload.exe"

# MinGW 运行时：windeployqt 在 MinGW 构建下有时漏掉这几份，少了会报
# "找不到 libgcc_s_seh-1.dll"；按需补齐，已存在就跳过
echo "== 补齐 MinGW 运行时"
for dll in libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll; do
    if [ -f "$MINGW_BIN/$dll" ] && [ ! -f "$DIST/$dll" ]; then
        cp "$MINGW_BIN/$dll" "$DIST/"
        echo "   + $dll"
    fi
done

cat > "$DIST/使用说明.txt" <<'EOF'
qdownload — 局域网文件互传（便携版）
=====================================

运行
----
双击 qdownload.exe 即可，无需安装 Qt 或其它运行时。

两台机器互传
------------
1. 两台机器接在同一个局域网（同一个路由器 / 同一个交换机组网内）；
2. 需要共享文件的一方打开顶部「共享」开关，并在「设置」里确认共享根目录；
3. 另一方在上方下拉框里选中这台机器，点击「配对」，输入对方窗口上显示的 6 位数字；
4. 左栏是本机接收目录，右栏是对方共享目录，点文件右侧的「下载」/「上传」。

需要上传到对方机器时：
- 接收方必须同时打开「共享」和「写权限」两个开关；
- 文件会落到对方共享根下的 inbox 目录；
- 支持断点续传：传输中断后重新发起会从断点继续，双方都会校验 SHA-256。

找不到对方
----------
- Windows 防火墙首次运行会弹窗，勾选「专用网络」并允许；
- 部分企业无线网络/交换机默认屏蔽组播（239.255.42.99:45454）；
- 组播被拦时页面顶部会有黄色提示条，此时只能靠同一网段的手动连接，功能不受影响。

配置与日志
----------
配置：%LOCALAPPDATA%\qdownload\qdownload
日志：同上目录下的 logs\ 里，按天与实例名分文件。
EOF

# 换包：旧目录只改名让位，不做整目录删除。产物有上千个文件，
# 脚本中途被打断时"删了半个包"比留着旧包更麻烦
FINAL="dist/qdownload-${VERSION}-win64"
if [ -d "$FINAL" ]; then
    OLD="dist/.old-qdownload-${VERSION}-$(date +%Y%m%d-%H%M%S)"
    mv "$FINAL" "$OLD"
    echo "== 换包"
    echo "   旧包 → $OLD（确认新包可用后自行删除）"
fi
mv "$STAGE" "$FINAL"

echo "== 完成"
echo "   目录：$FINAL"
du -sh "$FINAL" 2>/dev/null || true
