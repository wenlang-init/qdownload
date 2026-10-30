"""独立实现的协议探测（二）：完整配对 → 列目录 → 关闭共享后再列目录。

用来验证两件事，都不依赖 qdownload 客户端代码：
  1. 鉴权后 LIST_REQ 能拿到正确的分页数据
  2. 主机把共享关掉之后，**已建立的连接**上下一次 LIST_REQ 立刻被 403 拦住
     （连接本身不该被踢掉，否则用户会以为断网了）

用法：python probe_list.py <port> <pair-code> [share-off-at-seconds]
"""
import hashlib
import hmac
import json
import socket
import struct
import sys
import time

HELLO, HELLO_ACK = 1, 2
PAIR_REQ, PAIR_CHALLENGE, PAIR_RESP, PAIR_OK, PAIR_FAIL = 5, 6, 7, 8, 9
LIST_REQ, LIST_RESP, ERROR = 11, 12, 15
NAMES = {2: "HELLO_ACK", 6: "PAIR_CHALLENGE", 8: "PAIR_OK", 9: "PAIR_FAIL",
         12: "LIST_RESP", 15: "ERROR"}


def encode(msg_type, payload):
    body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
    return b"QDL1" + bytes([msg_type]) + struct.pack(">I", len(body)) + body


def read_frame(sock):
    header = b""
    while len(header) < 9:
        chunk = sock.recv(9 - len(header))
        if not chunk:
            raise RuntimeError("连接被关闭")
        header += chunk
    assert header[:4] == b"QDL1", header
    msg_type = header[4]
    length = struct.unpack(">I", header[5:9])[0]
    body = b""
    while len(body) < length:
        chunk = sock.recv(length - len(body))
        if not chunk:
            raise RuntimeError("连接被关闭")
        body += chunk
    return msg_type, json.loads(body.decode("utf-8"))


def mac(code, message):
    return hmac.new(code.encode("utf-8"), message.encode("utf-8"), hashlib.sha256).hexdigest()


def main():
    port = int(sys.argv[1])
    code = sys.argv[2]
    share_off_at = float(sys.argv[3]) if len(sys.argv) > 3 else None
    started = time.time()

    sock = socket.create_connection(("127.0.0.1", port), timeout=10)
    peer_id = "probe-list"

    sock.sendall(encode(HELLO, {"peerId": peer_id, "name": "探针B",
                                "platform": "python", "protoVer": 1}))
    msg_type, payload = read_frame(sock)
    print("HELLO_ACK", NAMES.get(msg_type), "needPair=", payload.get("needPair"))

    sock.sendall(encode(PAIR_REQ, {"peerId": peer_id, "name": "探针B", "platform": "python"}))
    msg_type, payload = read_frame(sock)
    nonce = payload.get("nonce", "")
    print("PAIR_CHALLENGE", NAMES.get(msg_type), "nonce=", nonce[:12])

    sock.sendall(encode(PAIR_RESP, {"nonce": nonce, "mac": mac(code, nonce + peer_id)}))
    msg_type, payload = read_frame(sock)
    paired = msg_type == PAIR_OK
    print("PAIR_RESULT", NAMES.get(msg_type), "perm=", payload.get("perm"),
          "->", "PASS" if paired else "FAIL")
    if not paired:
        return 1

    def list_root(tag):
        sock.sendall(encode(LIST_REQ, {"reqId": "r-1", "path": "/", "offset": 0, "limit": 200}))
        # 主机会在关共享时先推一条 PERM_UPDATE，等响应要跳过这类无关帧
        while True:
            msg_type, payload = read_frame(sock)
            if msg_type in (LIST_RESP, ERROR):
                break
            print(f"{tag} SKIP_FRAME type={msg_type}")
        if msg_type == LIST_RESP:
            entries = payload.get("entries", [])
            dirs = [e["name"] for e in entries if e["isDir"]]
            print(f"{tag} LIST_RESP total={payload.get('total')} "
                  f"returned={len(entries)} dirs={dirs}")
            return True
        print(f"{tag} {NAMES.get(msg_type)} code={payload.get('code')} "
              f"message={payload.get('message')}")
        return False

    list_root("BEFORE")

    if share_off_at is not None:
        wait = share_off_at - (time.time() - started)
        if wait > 0:
            time.sleep(wait)
        ok = list_root("AFTER_SHARE_OFF")
        print("SHARE_OFF_403", "PASS" if not ok else "FAIL")
        sock.close()
        return 0 if not ok else 1

    sock.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
