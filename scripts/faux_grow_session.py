#!/usr/bin/env python3
"""Grow a faux-control pi-cli session by N scripted turns (testing helper)."""
import json, socket, sys


class FauxClient:
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.connect(path)
        self.sock.settimeout(20)
        self.buf = b""

    def send(self, obj):
        self.sock.sendall((json.dumps(obj) + "\n").encode())

    def next_msg(self, timeout=20):
        """Return next complete JSON message, or None on EOF/timeout."""
        self.sock.settimeout(timeout)
        while True:
            if b"\n" in self.buf:
                line, self.buf = self.buf.split(b"\n", 1)
                if line.strip():
                    try:
                        return json.loads(line)
                    except Exception:
                        continue
            try:
                chunk = self.sock.recv(65536)
            except socket.timeout:
                return None
            if not chunk:
                return None
            self.buf += chunk


def grow_session(path, n_turns, q_bytes=400, a_bytes=580):
    c = FauxClient(path)
    for t in range(n_turns):
        c.send({"type": "round", "id": f"c{t}", "stop_reason": "end_turn",
                "content": [{"type": "text",
                             "text": f"answer {t}: " + "y" * a_bytes}]})
        ack = c.next_msg()
        if not ack or not ack.get("success"):
            raise RuntimeError(f"round {t} failed: {ack}")
        c.send({"type": "turn", "id": f"t{t}",
                "prompt": {"text": f"question {t}: " + "q" * q_bytes,
                           "source": "ordinary"}})
        done = False
        while not done:
            e = c.next_msg(timeout=25)
            if e is None:
                raise RuntimeError(f"turn {t} timed out")
            if e.get("type") == "turn.completed":
                done = True
            elif e.get("type") == "turn.failed":
                raise RuntimeError(f"turn {t} failed: {e}")
    print(f"grew session by {n_turns} turns")


if __name__ == "__main__":
    grow_session(sys.argv[1], int(sys.argv[2]))
