"""
test_no_outbound.py — out of the box, the server sends nothing beyond the
machine: no STUN server, no DNS lookup, nothing a network review would have
to ask about.

The server runs in a network namespace of its own (`unshare -rn`, no root
needed) whose only way out is a dummy interface with a default route. Every
packet the server tries to send off the machine lands there, where a raw
socket sees it. A viewer subscribes to low-latency video, which is when the
server would contact a STUN server. Then a positive control: once an Owner
sets a STUN server, its packets do show up, so a silent capture means
silence, not a capture that can't see.

Skips where unprivileged user namespaces aren't available.
"""
import json
import os
import socket
import struct
import subprocess
import sys
import threading
import time
import unittest

sys.path.insert(0, os.path.dirname(__file__))
from server_harness import Client, HoustonKVMServer  # noqa: E402

OWNER = ("owner", "ownerpassword1")
# TEST-NET-3 (RFC 5737): never a real host.
STUN_HOST = "203.0.113.5"


def probe(extra_args):
    """Runs inside the namespace: returns the packets that tried to leave."""
    for cmd in ("ip link set lo up", "ip link add d0 type dummy", "ip addr add 198.51.100.1/24 dev d0"):
        subprocess.run(cmd.split(), check=True)
    try:   # IPv6 router solicitations on link-up would be noise
        with open("/proc/sys/net/ipv6/conf/d0/disable_ipv6", "w") as f:
            f.write("1")
    except OSError:
        pass
    for cmd in ("ip link set d0 up", "ip route add default via 198.51.100.254"):
        subprocess.run(cmd.split(), check=True)

    seen, stop = [], threading.Event()
    cap = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(0x0003))
    cap.bind(("d0", 0))
    cap.settimeout(0.2)

    def capture():
        while not stop.is_set():
            try:
                frame = cap.recv(65535)
            except socket.timeout:
                continue
            ethertype = struct.unpack("!H", frame[12:14])[0]
            if ethertype == 0x0800:
                ihl = (frame[14] & 0x0F) * 4
                proto = {6: "tcp", 17: "udp"}.get(frame[23], str(frame[23]))
                dst = socket.inet_ntoa(frame[30:34])
                port = struct.unpack("!H", frame[14 + ihl + 2:14 + ihl + 4])[0] if proto in ("tcp", "udp") else 0
                seen.append(f"{proto} {dst}:{port}")
            elif ethertype == 0x86DD and frame[20] != 58:   # IPv6, but not ICMPv6 neighbour discovery
                seen.append("ipv6 " + socket.inet_ntop(socket.AF_INET6, frame[38:54]))

    thread = threading.Thread(target=capture, daemon=True)
    thread.start()
    server = HoustonKVMServer(extra_args=extra_args)
    server.start()
    try:
        server.setup_owner(Client(server.port), *OWNER)
        owner = Client(server.port)
        owner.post("/api/login", {"username": OWNER[0], "password": OWNER[1]})
        _, raw, _ = owner.get("/api/targets")
        tid = next(t["id"] for t in Client.as_json(raw)["targets"] if t["default"])
        result = {"default": None, "owner_set": None}

        def watch():
            # A full subscription: the server gathers its candidates, the
            # moment it would ask a STUN server for its public address.
            start = len(seen)
            owner.post(f"/api/targets/{tid}/webrtc/subscribe")
            time.sleep(2)
            return seen[start:]

        result["default"] = watch()
        status, raw, _ = owner.put("/api/admin/ice-servers",
                                   {"ice_servers": [{"url": f"stun:{STUN_HOST}:3478"}]})
        assert status == 200, raw
        result["owner_set"] = watch()
        return result
    finally:
        server.stop()
        stop.set()
        thread.join()


class TestNoOutbound(unittest.TestCase):

    def test_nothing_leaves_the_machine_until_an_owner_asks(self):
        try:   # a namespace with a dummy interface, as probe() builds (not in mock's chroot, say)
            subprocess.run(["unshare", "-rn", "ip", "link", "add", "d0", "type", "dummy"],
                           check=True, capture_output=True, timeout=10)
        except (OSError, subprocess.CalledProcessError) as e:
            self.skipTest(f"no unprivileged network namespace with a dummy interface here ({e})")
        out = subprocess.run(["unshare", "-rn", sys.executable, __file__, "--probe"],
                             capture_output=True, text=True, timeout=120, env=os.environ)
        self.assertEqual(out.returncode, 0, out.stderr[-3000:])
        result = json.loads(out.stdout.strip().splitlines()[-1])
        self.assertEqual(result["default"], [], "the server reached out with no STUN server set")
        self.assertIn(f"udp {STUN_HOST}:3478", result["owner_set"],
                      "the capture saw nothing even with a STUN server set: it can't be trusted")


if __name__ == "__main__":
    if sys.argv[1:] == ["--probe"]:
        print(json.dumps(probe(())))
    else:
        unittest.main()
