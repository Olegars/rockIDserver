#!/usr/bin/env python3
"""Small client for the rockIDserver skeleton.

    python3 tools/client.py smoke --port 7410
    python3 tools/client.py repl --port 7410
"""

import argparse
import json
import socket
import struct
import sys
import time
import uuid


class Client:
    def __init__(self, host, port):
        self.sock = socket.create_connection((host, port), timeout=5)
        self.buf = b""
        self.inbox = []
        self.next_id = 0

    def close(self):
        self.sock.close()

    def call(self, method, params=None, timeout=5):
        self.next_id += 1
        req_id = self.next_id
        self._send({"id": req_id, "method": method, "params": params or {}})
        deadline = time.time() + timeout
        while time.time() < deadline:
            for i, msg in enumerate(self.inbox):
                if msg.get("id") == req_id:
                    return self.inbox.pop(i)
            self._pump(min(0.2, max(0.01, deadline - time.time())))
        raise TimeoutError(method)

    def updates(self, wait=0.3):
        self._pump(wait)
        found = [msg for msg in self.inbox if "update" in msg]
        self.inbox = [msg for msg in self.inbox if "update" not in msg]
        return found

    def _send(self, obj):
        payload = json.dumps(obj, ensure_ascii=False).encode()
        self.sock.sendall(struct.pack(">I", len(payload)) + payload)

    def _pump(self, timeout):
        self.sock.settimeout(timeout)
        try:
            chunk = self.sock.recv(65536)
        except socket.timeout:
            return
        if not chunk:
            raise ConnectionError("server closed the connection")
        self.buf += chunk
        while len(self.buf) >= 4:
            size = struct.unpack(">I", self.buf[:4])[0]
            if len(self.buf) < 4 + size:
                break
            raw = self.buf[4 : 4 + size]
            self.buf = self.buf[4 + size :]
            self.inbox.append(json.loads(raw.decode()))


def expect_ok(resp, what):
    if not resp.get("ok"):
        raise SystemExit(f"{what} failed: {resp}")
    return resp["result"]


def smoke(host, port):
    suffix = uuid.uuid4().hex[:8]
    alice = Client(host, port)
    bob = Client(host, port)
    try:
        ping = expect_ok(alice.call("ping"), "ping")
        if ping.get("pong") is not True:
            raise SystemExit(f"unexpected ping: {ping}")

        a = expect_ok(
            alice.call(
                "auth.signUp",
                {"username": f"alice_{suffix}", "password": "password12", "display_name": "Alice"},
            ),
            "signup alice",
        )
        b = expect_ok(
            bob.call(
                "auth.signUp",
                {"username": f"bob_{suffix}", "password": "password12", "display_name": "Bob"},
            ),
            "signup bob",
        )
        bad = alice.call("auth.signIn", {"username": f"alice_{suffix}", "password": "wrong-pass"})
        if bad.get("ok") or bad.get("error", {}).get("message") != "INVALID_CREDENTIALS":
            raise SystemExit(f"bad password was accepted: {bad}")

        dialog = expect_ok(
            alice.call("dialogs.createPrivate", {"user_id": b["user"]["id"]}),
            "create private",
        )
        again = expect_ok(
            bob.call("dialogs.createPrivate", {"user_id": a["user"]["id"]}),
            "create private again",
        )
        if again["id"] != dialog["id"]:
            raise SystemExit("private dialog is not idempotent")

        bob_updates = bob.updates(0.4)
        if not any(item.get("update") == "dialog.new" for item in bob_updates):
            raise SystemExit(f"bob missed dialog.new: {bob_updates}")

        expect_ok(alice.call("contacts.add", {"username": f"bob_{suffix}"}), "contact")
        sent = expect_ok(
            alice.call("messages.send", {"dialog_id": dialog["id"], "text": "привет"}),
            "send",
        )
        incoming = bob.updates(0.5)
        texts = [item.get("data", {}).get("text") for item in incoming if item.get("update") == "message.new"]
        if texts != ["привет"]:
            raise SystemExit(f"bob missed message: {incoming}")

        history = expect_ok(
            bob.call("messages.getHistory", {"dialog_id": dialog["id"]}),
            "history",
        )
        if [m["text"] for m in history["messages"]] != ["привет"]:
            raise SystemExit(f"history mismatch: {history}")

        expect_ok(
            bob.call("messages.read", {"dialog_id": dialog["id"], "max_id": sent["id"]}),
            "read",
        )
        dialogs = expect_ok(bob.call("dialogs.get"), "dialogs")
        mine = [d for d in dialogs["dialogs"] if d["id"] == dialog["id"]]
        if not mine or mine[0]["unread"] != 0:
            raise SystemExit(f"unread was not cleared: {mine}")

        group = expect_ok(
            alice.call(
                "dialogs.createGroup",
                {"title": "Weekend", "member_ids": [b["user"]["id"]]},
            ),
            "group",
        )
        if group["type"] != "group" or group["member_count"] != 2:
            raise SystemExit(f"bad group: {group}")
        expect_ok(
            alice.call("messages.send", {"dialog_id": group["id"], "text": "all hands"}),
            "group send",
        )
        group_updates = bob.updates(0.5)
        if not any(
            item.get("update") == "message.new" and item.get("data", {}).get("text") == "all hands"
            for item in group_updates
        ):
            raise SystemExit(f"group fanout failed: {group_updates}")

        expect_ok(alice.call("auth.signOut"), "signout")
        denied = alice.call("users.getMe")
        if denied.get("ok") or denied.get("error", {}).get("code") != 401:
            raise SystemExit(f"signed-out session still works: {denied}")

        bound = Client(host, port)
        try:
            me = expect_ok(bound.call("auth.bind", {"token": b["token"]}), "bind")
            if me["user"]["id"] != b["user"]["id"]:
                raise SystemExit(f"bind returned the wrong user: {me}")
        finally:
            bound.close()
    finally:
        alice.close()
        bob.close()
    print("smoke ok")


def repl(host, port):
    client = Client(host, port)
    print("methods: ping, signup, signin, me, search, dialogs, private, send, history, quit")
    print("example: signup alice password12 Alice")
    while True:
        try:
            line = input("> ").strip()
        except EOFError:
            print()
            break
        if not line:
            continue
        if line in ("quit", "exit"):
            break
        parts = line.split(" ", 3)
        cmd = parts[0]
        try:
            if cmd == "ping":
                print(client.call("ping"))
            elif cmd == "signup" and len(parts) == 4:
                print(client.call("auth.signUp", {
                    "username": parts[1], "password": parts[2], "display_name": parts[3]
                }))
            elif cmd == "signin" and len(parts) == 3:
                print(client.call("auth.signIn", {"username": parts[1], "password": parts[2]}))
            elif cmd == "me":
                print(client.call("users.getMe"))
            elif cmd == "search" and len(parts) >= 2:
                print(client.call("users.search", {"query": parts[1]}))
            elif cmd == "dialogs":
                print(client.call("dialogs.get"))
            elif cmd == "private" and len(parts) == 2:
                print(client.call("dialogs.createPrivate", {"user_id": int(parts[1])}))
            elif cmd == "send" and len(parts) == 3:
                print(client.call("messages.send", {"dialog_id": int(parts[1]), "text": parts[2]}))
            elif cmd == "history" and len(parts) == 2:
                print(client.call("messages.getHistory", {"dialog_id": int(parts[1])}))
            else:
                print("unrecognized command")
            for update in client.updates(0.05):
                print("update", update)
        except Exception as exc:
            print("error:", exc)
    client.close()


def main():
    parser = argparse.ArgumentParser(description="rockIDserver client")
    parser.add_argument("mode", choices=("smoke", "repl"))
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=7410)
    args = parser.parse_args()
    if args.mode == "smoke":
        smoke(args.host, args.port)
    else:
        repl(args.host, args.port)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(130)
