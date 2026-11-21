#!/usr/bin/env python3
"""Host-side control for w98agent (see docs/STAND.md).

Examples:
  w98.py discover
  w98.py -H z505 ping
  w98.py -H z505 sh "dir c:\\windows\\system\\ati*.*"
  w98.py -H z505 shot desktop.png
  w98.py -H z505 snapshot
  w98.py -H z505 trial build/atim1.drv=C:\\WINDOWS\\SYSTEM\\atim1.drv
  w98.py -H z505 confirm

-H takes an IP address or a computer name seen in the agent's beacon.
The W98_HOST environment variable is used when -H is omitted.
"""
import argparse
import os
import socket
import struct
import sys
import time
import zlib

PORT = 9898


class AgentError(Exception):
    pass


# ---------- transport ----------

def discover(seconds=7.0):
    """Listen for agent beacons. Returns {name: ip}."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("", PORT))
    found = {}
    end = time.time() + seconds
    while time.time() < end:
        s.settimeout(max(0.05, end - time.time()))
        try:
            data, addr = s.recvfrom(256)
        except socket.timeout:
            break
        parts = data.decode("ascii", "replace").split()
        if len(parts) >= 3 and parts[0] == "W98AGENT":
            found[parts[2]] = addr[0]
    s.close()
    return found


def resolve(host):
    try:
        socket.inet_aton(host)
        return host
    except OSError:
        pass
    for name, ip in discover().items():
        if name.lower().startswith(host.lower()):
            return ip
    raise AgentError("no agent named %r answered; is the laptop on?" % host)


class Agent:
    def __init__(self, ip, timeout=30.0):
        self.ip = ip
        self.timeout = timeout

    def _request(self, line, payload=b"", timeout=None):
        s = socket.create_connection((self.ip, PORT), timeout=timeout or self.timeout)
        s.sendall(line.encode("cp1251") + b"\n" + payload)
        f = s.makefile("rb")
        status = f.readline().decode("cp1251", "replace").rstrip("\r\n")
        if not status.startswith("OK"):
            s.close()
            raise AgentError(status or "connection closed without reply")
        return s, f, status[2:].strip()

    def simple(self, line, payload=b"", timeout=None):
        s, f, status = self._request(line, payload, timeout)
        s.close()
        return status

    def with_body(self, line, timeout=None):
        """For replies whose last word is the byte count of a body that follows."""
        s, f, status = self._request(line, timeout=timeout)
        size = int(status.split()[-1])
        body = f.read(size)
        s.close()
        if len(body) != size:
            raise AgentError("short reply: %d of %d bytes" % (len(body), size))
        return status, body

    def ping(self):
        status = self.simple("PING", timeout=5)
        return dict(kv.split("=", 1) for kv in status.split() if "=" in kv)

    def put(self, data, remote):
        self.simple("PUT %d %s" % (len(data), remote), data)

    def get(self, remote):
        return self.with_body("GET " + remote)[1]

    def delete(self, remote):
        self.simple("DEL " + remote)

    def listdir(self, pattern):
        s, f, status = self._request("DIR " + pattern)
        rows = []
        if status == "1":
            for raw in f:
                kind, size, name = raw.decode("cp1251", "replace").rstrip("\r\n").split(" ", 2)
                rows.append((kind, int(size), name))
        s.close()
        return rows

    def exec(self, cmdline, timeout_ms=60000):
        """Run a Win32 program hidden. Returns (exit code or None on timeout, output)."""
        status, body = self.with_body("EXEC %d %s" % (timeout_ms, cmdline),
                                      timeout=timeout_ms / 1000 + 30)
        first = status.split()[0]
        code = None if first == "timeout" else int(first.split("=")[1])
        return code, body

    def shell(self, command, timeout_ms=60000):
        """Run a command through COMMAND.COM and return its output.

        COMMAND.COM is a DOS program, so its output is redirected by the shell
        itself into a file; the exit code of the command is not available.
        """
        out = self.ping()["dir"] + "\\SH.OUT"
        self.delete(out)
        code, _ = self.exec("command.com /c %s > %s" % (command, out), timeout_ms)
        data = self.get(out) if self.listdir(out) else b""
        return code is None, data

    def spawn(self, cmdline):
        return self.simple("SPAWN 0 " + cmdline)

    def shot(self):
        return self.with_body("SHOT", timeout=120)[1]

    def online(self):
        try:
            self.ping()
            return True
        except (OSError, AgentError):
            return False

    def wait_offline(self, seconds):
        end = time.time() + seconds
        while time.time() < end:
            if not self.online():
                return True
            time.sleep(1)
        return False

    def wait_online(self, seconds):
        end = time.time() + seconds
        while time.time() < end:
            if self.online():
                return True
            time.sleep(2)
        return False

    def reboot(self, wait=300, hard_after=0):
        """Reboot and wait until the agent answers again."""
        self.simple("REBOOT %d" % hard_after)
        if not self.wait_offline(90):
            raise AgentError("laptop did not go down after REBOOT")
        if not self.wait_online(wait):
            raise AgentError("laptop did not come back within %d s" % wait)


# ---------- screenshots ----------

def bmp_to_png(bmp):
    """Convert the agent's 24-bit bottom-up BMP to PNG."""
    off = struct.unpack_from("<I", bmp, 10)[0]
    w, h = struct.unpack_from("<ii", bmp, 18)
    stride = (w * 3 + 3) & ~3
    raw = bytearray()
    for y in range(h - 1, -1, -1):
        row = bmp[off + y * stride: off + y * stride + w * 3]
        rgb = bytearray(row)
        rgb[0::3], rgb[2::3] = row[2::3], row[0::3]
        raw += b"\0" + rgb

    def chunk(tag, data):
        body = tag + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body))

    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(raw), 6))
            + chunk(b"IEND", b""))


# ---------- driver trial workflow ----------

def flag(agent, name):
    return agent.ping()["dir"] + "\\" + name


def do_snapshot(agent, wait):
    """Save the current registry and ini files as the known-good state (needs a reboot)."""
    agent.put(b"snap\r\n", flag(agent, "SNAP.FLG"))
    agent.reboot(wait)
    info = agent.ping()
    if info["snapshot"] != "1" or agent.listdir(flag(agent, "SNAP.FLG")):
        raise AgentError("snapshot was not taken; is W98BOOT.BAT called from AUTOEXEC.BAT?")
    print("known-good configuration saved")


def do_trial(agent, pairs, wait):
    """Install files, reboot on trial. Unless confirmed, the next boot rolls back."""
    info = agent.ping()
    if info["snapshot"] != "1":
        raise AgentError("no known-good snapshot yet; run 'snapshot' first")
    if info["pending"] == "1":
        raise AgentError("a trial is already pending; run 'confirm' or reboot to roll back")
    agent.delete(flag(agent, "ROLLED.FLG"))
    for local, remote in pairs:
        with open(local, "rb") as f:
            agent.put(f.read(), remote)
        print("put %s -> %s" % (local, remote))
    agent.put(b"trial\r\n", flag(agent, "TRIAL.FLG"))
    agent.simple("REBOOT 0")
    if not agent.wait_offline(90):
        raise AgentError("laptop did not go down after REBOOT")
    print("rebooting on trial...")
    if not agent.wait_online(wait):
        print("NOT BACK after %d s: Windows hung or the network is down.\n"
              "Power-cycle the laptop; it will boot the known-good configuration." % wait)
        return 2
    info = agent.ping()
    if info["rolled"] == "1":
        print("ROLLED BACK: the trial boot failed and known-good was restored")
        return 1
    if info["pending"] != "1":
        print("WARNING: trial flag was not armed; is W98BOOT.BAT called from AUTOEXEC.BAT?")
        return 1
    print("trial boot is up (pending). Check it, then 'confirm'; any reboot rolls back.")
    return 0


# ---------- command line ----------

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-H", "--host", default=os.environ.get("W98_HOST"))
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("discover", help="list agents heard on the network")
    sub.add_parser("ping", help="show agent state")
    p = sub.add_parser("put", help="copy a local file to the laptop")
    p.add_argument("local")
    p.add_argument("remote")
    p = sub.add_parser("get", help="copy a file from the laptop")
    p.add_argument("remote")
    p.add_argument("local")
    p = sub.add_parser("del", help="delete a file on the laptop")
    p.add_argument("remote")
    p = sub.add_parser("dir", help="list files matching a pattern")
    p.add_argument("pattern")
    p = sub.add_parser("exec", help="run a Win32 program hidden, print its output")
    p.add_argument("cmdline")
    p.add_argument("-t", "--timeout", type=float, default=60, help="seconds")
    p = sub.add_parser("sh", help="run a command through COMMAND.COM")
    p.add_argument("command")
    p.add_argument("-t", "--timeout", type=float, default=60, help="seconds")
    p = sub.add_parser("spawn", help="start a program visibly and do not wait")
    p.add_argument("cmdline")
    p = sub.add_parser("shot", help="save a screenshot (.png or .bmp)")
    p.add_argument("local")
    p = sub.add_parser("reboot", help="reboot and wait for the agent")
    p.add_argument("-w", "--wait", type=int, default=300)
    sub.add_parser("reset", help="hard reset through chipset ports (no clean shutdown)")
    p = sub.add_parser("arm", help="reboot the laptop unless disarmed within N seconds")
    p.add_argument("seconds", type=int)
    sub.add_parser("disarm", help="cancel the dead-man timer")
    p = sub.add_parser("wait", help="wait until the agent answers")
    p.add_argument("-w", "--wait", type=int, default=300)
    p = sub.add_parser("snapshot", help="save current config as known-good (reboots)")
    p.add_argument("-w", "--wait", type=int, default=300)
    p = sub.add_parser("trial", help="install files and reboot on trial")
    p.add_argument("files", nargs="*", metavar="LOCAL=REMOTE")
    p.add_argument("-w", "--wait", type=int, default=300)
    sub.add_parser("confirm", help="keep the trial configuration")
    a = ap.parse_args()

    if a.cmd == "discover":
        found = discover()
        for name, ip in sorted(found.items()):
            print("%-16s %s" % (name, ip))
        return 0 if found else 1
    if not a.host:
        ap.error("no host: use -H or set W98_HOST")
    agent = Agent(resolve(a.host))
    out = sys.stdout.buffer

    if a.cmd == "ping":
        for k, v in agent.ping().items():
            print("%s=%s" % (k, v))
    elif a.cmd == "put":
        with open(a.local, "rb") as f:
            agent.put(f.read(), a.remote)
    elif a.cmd == "get":
        with open(a.local, "wb") as f:
            f.write(agent.get(a.remote))
    elif a.cmd == "del":
        agent.delete(a.remote)
    elif a.cmd == "dir":
        for kind, size, name in agent.listdir(a.pattern):
            print("%s %10d %s" % (kind, size, name))
    elif a.cmd == "exec":
        code, body = agent.exec(a.cmdline, int(a.timeout * 1000))
        out.write(body)
        out.flush()
        if code is None:
            print("TIMEOUT: process was killed", file=sys.stderr)
            return 124
        return code
    elif a.cmd == "sh":
        timed_out, body = agent.shell(a.command, int(a.timeout * 1000))
        out.write(body)
        out.flush()
        if timed_out:
            print("TIMEOUT: command was killed", file=sys.stderr)
            return 124
    elif a.cmd == "spawn":
        print(agent.spawn(a.cmdline))
    elif a.cmd == "shot":
        bmp = agent.shot()
        with open(a.local, "wb") as f:
            f.write(bmp if a.local.lower().endswith(".bmp") else bmp_to_png(bmp))
    elif a.cmd == "reboot":
        agent.reboot(a.wait)
    elif a.cmd == "reset":
        agent.simple("RESET")
    elif a.cmd == "arm":
        agent.simple("ARM %d" % a.seconds)
    elif a.cmd == "disarm":
        agent.simple("DISARM")
    elif a.cmd == "wait":
        return 0 if agent.wait_online(a.wait) else 1
    elif a.cmd == "snapshot":
        do_snapshot(agent, a.wait)
    elif a.cmd == "trial":
        pairs = []
        for item in a.files:
            if "=" not in item:
                ap.error("trial files must be LOCAL=REMOTE, got %r" % item)
            pairs.append(tuple(item.split("=", 1)))
        return do_trial(agent, pairs, a.wait)
    elif a.cmd == "confirm":
        agent.delete(flag(agent, "PENDING.FLG"))
        print("trial configuration kept")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (AgentError, OSError) as e:
        print("error: %s" % e, file=sys.stderr)
        sys.exit(1)
