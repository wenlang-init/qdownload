"""独立实现的协议探测：不依赖 qdownload 客户端代码，验证服务端的鉴权护栏。

用法：python probe_auth.py <port>
"""
import json
import socket
import struct
import sys

HELLO, HELLO_ACK, LIST_REQ, LIST_RESP, ERROR = 1, 2, 11, 12, 15
NAMES = {1: "HELLO", 2: "HELLO_ACK", 11: "LIST_REQ", 12: "LIST_RESP", 15: "ERROR"}


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
    sock = socket.create_connection(("127.0.0.1", port), timeout=5)

    sock.sendall(encode(HELLO, {"peerId": "probe-unauth", "name": "探针",
                                "platform": "python", "protoVer": 1}))
    msg_type, payload = read_frame(sock)
    print("HELLO_ACK", NAMES.get(msg_type), "needPair=", payload.get("needPair"))

    # 未做 AUTH_REQ 就直接列目录：应被 401 拦住
    sock.sendall(encode(LIST_REQ, {"reqId": "r-1", "path": "/", "offset": 0, "limit": 10}))
    msg_type, payload = read_frame(sock)
    ok = msg_type == ERROR and payload.get("code") == 401
    print("UNAUTH_LIST", NAMES.get(msg_type), payload.get("code"), payload.get("message"),
          "->", "PASS" if ok else "FAIL")

    # 协议层畸形路径：即使已鉴权也不该被接受，这里验证帧不会被误解析
    sock.sendall(encode(LIST_REQ, {"reqId": "r-2", "path": "/../../etc/passwd",
                                   "offset": 0, "limit": 10}))
    msg_type, payload = read_frame(sock)
    print("TRAVERSAL", NAMES.get(msg_type), payload.get("code"), payload.get("message"))

    sock.close()
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
