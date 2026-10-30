#!/usr/bin/env bash
#
# 生成 Android 签名 keystore，并写一份可直接 source 的环境文件。
#
# 为什么要单独一个脚本：打 release APK 必须有签名，没签名就是
# "INSTALL_PARSE_FAILED_NO_CERTIFICATES"，装不上真机；而 keytool 的参数
# （DN、有效期、storetype）每次手写都容易出错，打包脚本里又不好做交互提示。
#
# 用法：
#   bash tools/android_keystore.sh
#   bash tools/android_keystore.sh <keystore 文件> [别名] [store 密码] [key 密码]
#
# 生成后按提示 export 那几行，再跑 tools/package_android.sh 即可出签名包。
set -euo pipefail

# shellcheck source=tools/common.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

KEYSTORE="${1:-$HOME/.qdownload/qdownload.keystore}"
ALIAS="${2:-qdownload}"
STORE_PASS="${3:-}"
KEY_PASS="${4:-${STORE_PASS}}"

if [ -z "$STORE_PASS" ]; then
    # 不搞交互提示（脚本要能进 CI），给个随机默认密码并明确告知
    STORE_PASS="$(head -c 12 /dev/urandom | od -An -tx1 | tr -d ' \n')"
    KEY_PASS="$STORE_PASS"
    echo "== 未指定密码，已生成随机密码：$STORE_PASS"
    echo "   请务必记下来（已写入下面的 env 文件）；丢了就没法再更新这个 App。"
fi

KEYTOOL=""
for c in "${JAVA_HOME:-}/bin/keytool" "$QT_PREFIX/bin/keytool" "$(command -v keytool 2>/dev/null || true)"; do
    [ -n "$c" ] && [ -x "$c" ] && {
        KEYTOOL="$c"
        break
    }
done
[ -n "$KEYTOOL" ] || qd_die "找不到 keytool（装 JDK，或 export JAVA_HOME=<jdk 目录>）"

mkdir -p "$(dirname "$KEYSTORE")"

if [ -f "$KEYSTORE" ]; then
    qd_die "keystore 已存在，不覆盖：$KEYSTORE（要重建请先手动删掉）"
fi

qd_info "生成 keystore"
"$KEYTOOL" -genkeypair -v \
    -keystore "$KEYSTORE" \
    -storetype JKS \
    -alias "$ALIAS" \
    -keyalg RSA -keysize 2048 -validity 10950 \
    -storepass "$STORE_PASS" -keypass "$KEY_PASS" \
    -dname "CN=qdownload, OU=Dev, O=qdownload, L=Local, ST=Local, C=CN"

ENV_FILE="$(dirname "$KEYSTORE")/signing.env"
cat >"$ENV_FILE" <<EOF
# 由 tools/android_keystore.sh 生成；打包前 source 本文件即可
export QT_ANDROID_KEYSTORE_PATH="$KEYSTORE"
export QT_ANDROID_KEYSTORE_ALIAS="$ALIAS"
export QT_ANDROID_KEYSTORE_STORE_PASS='$STORE_PASS'
export QT_ANDROID_KEYSTORE_KEY_PASS='$KEY_PASS'
EOF
chmod 0600 "$ENV_FILE"

qd_info "完成"
echo "   keystore：$KEYSTORE"
echo "   环境文件：$ENV_FILE"
echo
echo "   打包前执行：source $ENV_FILE"
echo "   然后：bash tools/package_android.sh"
