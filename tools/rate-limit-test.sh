#!/bin/bash
# 传输联调（M10 限速 / M12 单文件多连接）：同机双实例，客户端 B 以指定速率与连接数下载/上传。
#
# 用法：rate-limit-test.sh <MB/s> [get|put] [segments] [额外参数…]
#   get（默认）：下载共享根里的 24MB big.bin → 接收目录
#   put       ：把 out/up.bin（12MB）上传到主机收件箱
#   segments  ：单文件连接数（1 = 单连接，默认 1）
#   额外参数  ：原样透传给客户端，例如 --pause-at 50 / --workers-at 40:6
#
# 前置：D:/tmp/qdtest/{share,big.bin, out/up.bin} 已就位（可用 python os.urandom 生成）。
set -u

LIMIT=${1:-0}
MODE=${2:-get}
SEGMENTS=${3:-1}
# 第 4 个参数起原样透传（--pause-at / --kill-at / --workers-at …）
shift $(( $# < 3 ? $# : 3 )) 2>/dev/null || true
EXTRA="$*"
cd "$(dirname "$0")/.." || exit 9

export PATH="/d/Qt/6.12.0/mingw_64/bin:/d/Qt/Tools/mingw1310_64/bin:$PATH"
# 不关这个，MSYS 会把 /big.bin 这类虚拟路径改写成 Git 安装目录下的真实路径
export MSYS_NO_PATHCONV=1
export MSYS2_ARG_CONV_EXCL="*"

mkdir -p build/tmp
rm -f build/tmp/a.log build/tmp/b.log
rm -rf /d/tmp/qdtest/recv/*
mkdir -p /d/tmp/qdtest/recv
rm -f /d/tmp/qdtest/share/inbox/up.bin*

./build/qdownload.exe --instance a --ctrl-port 45455 --share --write \
    --share-root D:/tmp/qdtest/share --pair-code 123456 \
    --auto-approve --auto-approve-write > build/tmp/a.log 2>&1 &
APID=$!

sleep 6

start=$(date +%s)
if [ "$MODE" = "put" ]; then
    ./build/qdownload.exe --instance b --ctrl-port 45555 --limit "$LIMIT" \
        --segments "$SEGMENTS" --receive-root D:/tmp/qdtest/recv \
        --pair-test dev-a 123456 --put D:/tmp/qdtest/out/up.bin $EXTRA > build/tmp/b.log 2>&1
    TAG="PUT_TEST"
else
    ./build/qdownload.exe --instance b --ctrl-port 45555 --limit "$LIMIT" \
        --segments "$SEGMENTS" --receive-root D:/tmp/qdtest/recv \
        --pair-test dev-a 123456 --get /big.bin $EXTRA > build/tmp/b.log 2>&1
    TAG="GET_TEST"
fi
code=$?
end=$(date +%s)

echo "=== ${MODE} 限速=${LIMIT} MB/s 连接=${SEGMENTS} ==="
echo "B 退出码=$code 耗时=$((end - start)) 秒"
grep -a "${TAG}_RESULT" build/tmp/b.log

taskkill //PID "$APID" //F >/dev/null 2>&1
exit 0
