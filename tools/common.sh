#!/usr/bin/env bash
#
# 打包脚本公共函数（M11）。
#
# 被 tools/package_linux.sh 与 tools/package_android.sh source。
# 单独执行没有意义，只提供日志、环境探测、版本号与产物路径这几个公共动作。
#
# 约定：所有函数以 qd_ 开头，避免和调用方脚本的局部变量撞名。
# shellcheck shell=bash

# 项目根目录：source 本文件时 BASH_SOURCE[0] 就是本文件的路径
QD_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

qd_info() { printf '== %s\n' "$*"; }
qd_step() { printf '   %s\n' "$*"; }
qd_warn() { printf '   [警告] %s\n' "$*" >&2; }
qd_die() {
    printf '\n[失败] %s\n' "$*" >&2
    exit 1
}

# 从 CMakeLists.txt 的 project(qdownload VERSION x.y.z) 取版本号。
# 用 cmake 去问当然更准，但要 configure 一轮；打包脚本常在没有 configure
# 的情况下先跑 --check，直接 sed 更轻。
qd_version() {
    local v
    v="$(sed -n 's/^[[:space:]]*project(qdownload[[:space:]]\{1,\}VERSION[[:space:]]\{1,\}\([0-9][0-9A-Za-z.+-]*\).*/\1/p' \
        "$QD_ROOT/CMakeLists.txt" | head -n 1)"
    [ -n "$v" ] || qd_die "无法从 CMakeLists.txt 解析 project(qdownload VERSION ...)"
    printf '%s' "$v"
}

# 命令是否存在；不存在就带上"怎么装"的提示退出
qd_need() {
    local cmd="$1" hint="${2:-}"
    if command -v "$cmd" >/dev/null 2>&1; then
        return 0
    fi
    if [ -n "$hint" ]; then
        qd_die "缺少命令：$cmd（$hint）"
    fi
    qd_die "缺少命令：$cmd"
}

# 环境自检的一行输出（--check 用）
qd_check_line() {
    local ok="$1" name="$2" detail="${3:-}"
    if [ "$ok" = "yes" ]; then
        printf '   [可用] %-14s %s\n' "$name" "$detail"
    else
        printf '   [缺失] %-14s %s\n' "$name" "$detail"
    fi
}

# 本机 CPU 架构 → Debian 包名后缀
qd_deb_arch() {
    case "$(uname -m)" in
        x86_64 | amd64) printf 'amd64' ;;
        aarch64 | arm64) printf 'arm64' ;;
        armv7l) printf 'armhf' ;;
        *) printf '%s' "$(uname -m)" ;;
    esac
}

# 本机 CPU 架构 → 产物名后缀
qd_portable_arch() {
    case "$(uname -m)" in
        x86_64 | amd64) printf 'x86_64' ;;
        aarch64 | arm64) printf 'arm64' ;;
        armv7l) printf 'armv7' ;;
        *) printf '%s' "$(uname -m)" ;;
    esac
}

# 找桌面端 Qt 前缀。
# 优先级：QT_PREFIX > qmake6/qmake 自报 > $QT_ROOT 下最新版 > 发行版常见路径
qd_find_qt_prefix() {
    local prefix="${QT_PREFIX:-}"

    if [ -z "$prefix" ] && command -v qmake6 >/dev/null 2>&1; then
        prefix="$(qmake6 -query QT_INSTALL_PREFIX 2>/dev/null)"
    fi
    if [ -z "$prefix" ] && command -v qmake >/dev/null 2>&1; then
        prefix="$(qmake -query QT_INSTALL_PREFIX 2>/dev/null)"
    fi
    if [ -z "$prefix" ]; then
        local root="${QT_ROOT:-$HOME/Qt}"
        if [ -d "$root" ]; then
            prefix="$(ls -d "$root"/6.*/gcc_64 2>/dev/null | sort -V | tail -n 1)"
            [ -n "$prefix" ] || prefix="$(ls -d "$root"/6.*/*/ 2>/dev/null | grep -v android | sort -V | tail -n 1)"
        fi
    fi
    if [ -z "$prefix" ]; then
        local c
        for c in /usr/lib/qt6 /usr/local/Qt/6/gcc_64 /opt/Qt/6/gcc_64; do
            if [ -x "$c/bin/qmake6" ] || [ -x "$c/bin/qmake" ]; then
                prefix="$c"
                break
            fi
        done
    fi

    [ -n "$prefix" ] || qd_die "找不到 Qt 6，请设置 QT_PREFIX=<Qt 安装前缀>（含 bin/ 与 lib/ 的那一层）"
    [ -d "$prefix" ] || qd_die "QT_PREFIX 指向的目录不存在：$prefix"
    printf '%s' "$prefix"
}

# ABI → Qt 套件目录名。Gradle 叫 arm64-v8a / armeabi-v7a，
# Qt 的套件叫 android_arm64_v8a / android_armv7，两边对不上，集中在这里翻译。
qd_qt_android_dirname() {
    case "$1" in
        arm64-v8a) printf 'android_arm64_v8a' ;;
        armeabi-v7a) printf 'android_armv7' ;;
        x86) printf 'android_x86' ;;
        x86_64) printf 'android_x86_64' ;;
        *) printf '' ;;
    esac
}

# 找 Qt Android 套件：QT_ANDROID_PREFIX > $QT_ROOT/<最新 6.x>/android_<abi>
qd_find_qt_android_prefix() {
    local abi="$1"
    local prefix="${QT_ANDROID_PREFIX:-}"
    local dirname
    dirname="$(qd_qt_android_dirname "$abi")"
    [ -n "$dirname" ] || qd_die "不支持的 ABI：$abi（可选 arm64-v8a / armeabi-v7a / x86 / x86_64）"

    if [ -z "$prefix" ]; then
        local root="${QT_ROOT:-}"
        if [ -z "$root" ]; then
            case "$(uname -s)" in
                MINGW* | MSYS* | CYGWIN*) root="D:/Qt" ;;
                *) root="$HOME/Qt" ;;
            esac
        fi
        if [ -d "$root" ]; then
            # 所有 <版本>/android_<abi> 目录，sort -V 取版本号最大的那个
            prefix="$(find "$root" -maxdepth 2 -mindepth 2 -type d -name "$dirname" 2>/dev/null |
                sort -V | tail -n 1)"
        fi
    fi

    [ -n "$prefix" ] || qd_die "找不到 Qt Android 套件（${dirname}），请设置 QT_ANDROID_PREFIX=<.../Qt/6.x.y/${dirname}>"
    [ -d "$prefix" ] || qd_die "QT_ANDROID_PREFIX 指向的目录不存在：$prefix"
    printf '%s' "$prefix"
}

# 找 JDK：JAVA_HOME 若为主版本 >= 17 的 JDK 就直接用；否则扫描常见安装位置，
# 挑版本号最高的一个（仍 < 17 才报错）。Qt 6.12 模板固定 gradle-9.5.1，
# Gradle 9 起不再支持 JDK 11，所以门槛就是 17。
qd_find_jdk() {
    local jh="${JAVA_HOME:-}" v=""

    if [ -n "$jh" ] && [ -d "$jh" ]; then
        v="$(qd_jdk_major "$jh")"
        if [ "$v" -ge 17 ]; then
            printf '%s' "$jh"
            return 0
        fi
        qd_warn "JAVA_HOME=$jh 是 JDK $v，Gradle 9 需要 17+，改去找别的 JDK"
    fi

    # 用数组而不是字符串：Windows 上的 "Program Files" 带空格，
    # 走 for c in $pats 会被按空格切开
    local -a pats=()
    case "$(uname -s)" in
        MINGW* | MSYS* | CYGWIN*)
            pats+=("/c/Program Files/Android/openjdk"/*)
            pats+=("/c/Program Files/Eclipse Adoptium"/*)
            pats+=("/c/Program Files/Java"/*)
            pats+=("/c/Qt/Tools"/*jdk*)
            ;;
        Darwin)
            pats+=(/Library/Java/JavaVirtualMachines/*/Contents/Home)
            pats+=(/opt/homebrew/opt/openjdk*/libexec/openjdk.jdk/Contents/Home)
            ;;
        *)
            pats+=(/usr/lib/jvm/*)
            ;;
    esac

    local best="" bestver=0 c ver
    for c in ${pats[@]+"${pats[@]}"}; do
        [ -x "$c/bin/javac" ] || [ -x "$c/bin/javac.exe" ] || continue
        ver="$(qd_jdk_major "$c")"
        if [ "$ver" -gt "$bestver" ]; then
            bestver="$ver"
            best="$c"
        fi
    done

    [ -n "$best" ] && [ "$bestver" -ge 17 ] ||
        qd_die "找不到 JDK 17+（Gradle 9 要求）。装好后 export JAVA_HOME=<jdk 目录> 再跑"
    printf '%s' "$best"
}

# 读某个 JDK 的主版本号（21 / 17 / 11 ...）；读不到输出 0
qd_jdk_major() {
    local home="$1" out=""
    if [ -x "$home/bin/javac" ]; then
        out="$("$home/bin/javac" -version 2>&1 | head -n 1)"
    elif [ -x "$home/bin/javac.exe" ]; then
        out="$("$home/bin/javac.exe" -version 2>&1 | head -n 1)"
    elif [ -x "$home/bin/java" ]; then
        out="$("$home/bin/java" -version 2>&1 | head -n 1)"
    elif [ -x "$home/bin/java.exe" ]; then
        out="$("$home/bin/java.exe" -version 2>&1 | head -n 1)"
    else
        printf '0'
        return 0
    fi

    # "javac 21.0.8" → 21 ；"openjdk version \"17.0.9\"" → 17
    local v
    v="$(printf '%s' "$out" | sed -n 's/^[^0-9]*\([0-9]\{1,\}\).*/\1/p' | head -n 1)"
    printf '%s' "${v:-0}"
}

# 找 Android SDK：ANDROID_SDK_ROOT / ANDROID_HOME > 常见路径扫描
qd_find_android_sdk() {
    local sdk="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-}}"

    if [ -z "$sdk" ]; then
        local -a cands=()
        case "$(uname -s)" in
            MINGW* | MSYS* | CYGWIN*)
                # 注意用 $HOME/... 而不是 $LOCALAPPDATA：后者是 C:\... 反斜杠路径，
                # 混进 Unix 路径里（C:\Users\x\AppData\Local/Android/Sdk）很难看也很难用
                cands+=("$HOME/AppData/Local/Android/Sdk")
                cands+=("$HOME/Android/Sdk")
                cands+=(/d/software/Android/Qt/*/SDK)
                cands+=(/c/software/Android/Qt/*/SDK)
                ;;
            Darwin)
                cands+=("$HOME/Library/Android/sdk")
                ;;
            *)
                cands+=("$HOME/Android/Sdk")
                cands+=(/opt/android-sdk /usr/lib/android-sdk)
                ;;
        esac
        # 先把所有"像 SDK"的目录都列出来，再优先挑带 NDK 的那个：
        # 机器上常同时装着 Qt Creator 自带的一份（只有 platform-tools）和
        # 命令行工具装的一份（带 NDK/平台），先命中的那个多半是没用的
        local -a found=()
        local c
        for c in ${cands[@]+"${cands[@]}"}; do
            [ -d "$c/platforms" ] && [ -d "$c/build-tools" ] && found+=("$c")
        done

        for c in ${found[@]+"${found[@]}"}; do
            if [ -d "$c/ndk-bundle" ] ||
                { [ -d "$c/ndk" ] &&
                    [ -n "$(find "$c/ndk" -maxdepth 1 -mindepth 1 -type d -print -quit 2>/dev/null)" ]; }; then
                sdk="$c"
                break
            fi
        done
        [ -n "$sdk" ] || sdk="${found[0]:-}"
    fi

    [ -n "$sdk" ] || qd_die "找不到 Android SDK，请设置 ANDROID_SDK_ROOT=<.../SDK>"
    [ -d "$sdk" ] || qd_die "ANDROID_SDK_ROOT 指向的目录不存在：$sdk"
    printf '%s' "$sdk"
}

# 找 NDK：ANDROID_NDK_ROOT > $SDK/ndk/<最新版本> > $SDK/ndk-bundle
qd_find_android_ndk() {
    local sdk="$1"
    local ndk="${ANDROID_NDK_ROOT:-}"

    if [ -z "$ndk" ] && [ -d "$sdk/ndk" ]; then
        ndk="$(find "$sdk/ndk" -maxdepth 1 -mindepth 1 -type d 2>/dev/null | sort -V | tail -n 1)"
    fi
    if [ -z "$ndk" ] && [ -d "$sdk/ndk-bundle" ]; then
        ndk="$sdk/ndk-bundle"
    fi

    [ -n "$ndk" ] || qd_die "SDK 里没装 NDK：$sdk/ndk（Qt 6.12 需要 NDK 25/27/28 之一）"
    printf '%s' "$ndk"
}

# Windows 上把 jom 放进 PATH：Qt Android 构建的默认生成器会用 make，
# 而 Windows 没有 GNU make，Qt Creator 自带的就是 jom。
qd_prepare_windows_path() {
    case "$(uname -s)" in
        MINGW* | MSYS* | CYGWIN*) ;;
        *) return 0 ;;
    esac

    local c
    for c in "$HOME/Qt/Tools/QtCreator/bin/jom" \
             "/d/Qt/Tools/QtCreator/bin/jom" \
             "/c/Qt/Tools/QtCreator/bin/jom" \
             "/d/Qt/Tools/Ninja"; do
        if [[ ":$PATH:" != *":$c:"* ]]; then
            export PATH="$c:$PATH"
        fi
    done
}
