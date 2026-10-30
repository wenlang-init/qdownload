"""独立实现的上传权限探测：验证 UPLOAD_REQ 的两道服务端护栏。

不依赖 qdownload 客户端代码，直接说协议：
  1. 未鉴权就发 UPLOAD_REQ  -> 401
  2. 以只读设备（perm=ro）发 UPLOAD_REQ -> 403

用法：python probe_upload.py <port> <peerId 前缀> <配对码>
前置：主机以 --share --write --pair-code <码> --auto-approve（**不带** --auto-approve-write）启动
"""
import hashlib
import hmac
import json
import socket
import struct
import sys

HELLO, HELLO_ACK = 1, 2
PAIR_REQ, PAIR_CHALLENGE, PAIR_RESP, PAIR_OK, PAIR_FAIL = 5, 6, 7, 8, 9
UPLOAD_REQ, UPLOAD_ACCEPT, ERROR = 19, 20, 15
NAMES = {
    1: "HELLO", 2: "HELLO_ACK", 5: "PAIR_REQ", 6: "PAIR_CHALLENGE", 7: "PAIR_RESP",
    8: "PAIR_OK", 9: "PAIR_FAIL", 19: "UPLOAD_REQ", 20: "UPLOAD_ACCEPT", 15: "ERROR",
}


def encode(msg_type, payload):
    body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
    return b"QDL1" + bytes([msg_type]) + struct.pack(">I", len(body)) + body


def read_frame(sock):
    header = b""
    while len(header) < 9:
        chunk = sock.recv(9 - len(header))
        if not chunk:
            return None
        header += chunk
    assert header[:4] == b"QDL1", header
    msg_type = header[4]
    length = struct.unpack(">I", header[5:9])[0]
    body = b""
    while len(body) < length:
        chunk = sock.recv(length - len(body))
        if not chunk:
            return None
        body += chunk
    return msg_type, json.loads(body.decode("utf-8"))


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 45455
    peer_id = (sys.argv[2] if len(sys.argv) > 2 else "probe") + "-up"
    code = sys.argv[3] if len(sys.argv) > 3 else "123456"

    ok_all = True

    # ---- 1. 未鉴权 → 401 ----
    sock = socket.create_connection(("127.0.0.1", port), timeout=5)
    sock.sendall(encode(HELLO, {"peerId": peer_id + "-anon", "name": "探针", "platform": "python",
                                "protoVer": 1}))
    read_frame(sock)
    sock.sendall(encode(UPLOAD_REQ, {"reqId": "u-0", "name": "x.bin", "size": 1, "offset": 0}))
    msg_type, payload = read_frame(sock)
    ok = msg_type == ERROR and payload.get("code") == 401
    ok_all &= ok
    print("UNAUTH_UPLOAD", NAMES.get(msg_type), payload.get("code"), payload.get("message"),
          "->", "PASS" if ok else "FAIL")
    sock.close()

    # ---- 2. 以只读身份配对 → 403 ----
    sock = socket.create_connection(("127.0.0.1", port), timeout=5)
    sock.sendall(encode(HELLO, {"peerId": peer_id, "name": "只读探针", "platform": "python",
                                "protoVer": 1}))
    read_frame(sock)

    sock.sendall(encode(PAIR_REQ, {"peerId": peer_id, "name": "只读探针", "platform": "python"}))
    msg_type, payload = read_frame(sock)
    if msg_type != PAIR_CHALLENGE:
        print("PAIR_CHALLENGE_FAIL", NAMES.get(msg_type), payload, "-> FAIL")
        return 1

    nonce = payload["nonce"]
    mac = hmac.new(code.encode(), (nonce + peer_id).encode(), hashlib.sha256).hexdigest()
    sock.sendall(encode(PAIR_RESP, {"nonce": nonce, "mac": mac}))
    msg_type, payload = read_frame(sock)
    if msg_type != PAIR_OK:
        print("PAIR_FAIL", NAMES.get(msg_type), payload, "-> FAIL")
        return 1
    print("PAIRED perm=%s canUpload=%s" % (payload.get("perm"), payload.get("canUpload")))

    sock.sendall(encode(UPLOAD_REQ, {"reqId": "u-1", "name": "ro.bin", "size": 1, "offset": 0}))
    msg_type, payload = read_frame(sock)
    ok = msg_type == ERROR and payload.get("code") == 403
    ok_all &= ok
    print("RO_UPLOAD", NAMES.get(msg_type), payload.get("code"), payload.get("message"),
          "->", "PASS" if ok else "FAIL")

    sock.close()
    print("PROBE_UPLOAD_RESULT", "PASS" if ok_all else "FAIL")
    return 0 if ok_all else 1


if __name__ == "__main__":
    sys.exit(main())
