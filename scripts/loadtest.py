#!/usr/bin/env python3
"""loadtest.py — estimate how many targets and viewers one HoustonKVM server
can carry on this machine.

Builds an optimised server with synthetic capture sources (cmake
-DHOUSTONKVM_LOADTEST=ON, into build-loadtest/), starts it on a free port
against a throwaway database, and ramps up, one step at a time:

  mjpeg     viewers of one target's MJPEG stream (/api/targets/<id>/stream)
  webrtc    WebRTC (low-latency) viewers of one target
  targets   targets, each with one MJPEG and one WebRTC viewer
  idle      targets, each with one MJPEG viewer only: what a target costs
            while nobody watches it over WebRTC

At every step it records the frame rate each viewer actually received, the
server's CPU and memory, and how quickly the API still answers, both while
viewers connect and once they are watching. A ramp stops
at the first step where a viewer falls below 90% of the capture frame rate.
The report says where each ramp stopped and what each target and viewer
cost, which is what to scale from.

It never touches real hardware: every target it creates uses a synthetic
source and no keyboard/mouse device, and it runs its own server, separate
from any installed service. Viewers run on this same machine, so their own
CPU is reported too.

Usage:
  scripts/loadtest.py                      # full run, ~10 minutes
  scripts/loadtest.py --quick              # smoke test: tiny steps, fails if no video flows
  scripts/loadtest.py --pattern motion --fps 60 --report report.md
  scripts/loadtest.py --only mjpeg --https    # what TLS costs MJPEG viewers
"""
import argparse
import json
import os
import platform
import re
import statistics
import subprocess
import sys
import threading
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT / "tests" / "integration"))
from server_harness import Client, HoustonKVMServer  # noqa: E402

OWNER = ("loadtest", "loadtest-password")
# A step "holds" when every viewer gets at least this share of the capture rate.
HOLD_RATIO = 0.9
CLK_TCK = os.sysconf("SC_CLK_TCK")


def ints(text):
    return [int(x) for x in text.split(",") if x.strip()]


def parse_args():
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--build-dir", default=str(REPO_ROOT / "build-loadtest"))
    p.add_argument("--no-build", action="store_true", help="use the existing build as is")
    p.add_argument("--quick", action="store_true",
                   help="tiny steps and short windows: checks the pipeline works, measures nothing")
    p.add_argument("--pattern", choices=("desktop", "motion"), default="desktop",
                   help="desktop: mostly still screen (typical); motion: every pixel changes (worst case)")
    p.add_argument("--width", type=int, default=1280)
    p.add_argument("--height", type=int, default=720)
    p.add_argument("--fps", type=int, default=30)
    p.add_argument("--bitrate", type=int, default=4000, help="WebRTC bitrate in kbps")
    p.add_argument("--mjpeg-steps", type=ints, default=[1, 5, 10, 20, 40, 80])
    p.add_argument("--webrtc-steps", type=ints, default=[1, 5, 10, 20, 40])
    p.add_argument("--target-steps", type=ints, default=[1, 2, 3, 4, 6, 8])
    p.add_argument("--idle-steps", type=ints, default=[1, 2, 4, 8, 12, 16])
    p.add_argument("--only", choices=("mjpeg", "webrtc", "targets", "idle"), action="append",
                   help="run only this ramp (repeatable)")
    p.add_argument("--https", action="store_true",
                   help="serve and view over HTTPS (a throwaway certificate), to measure what TLS costs")
    p.add_argument("--warmup", type=float, default=5)
    p.add_argument("--seconds", type=float, default=15)
    p.add_argument("--report", help="also write the Markdown report here")
    p.add_argument("--json", help="also write the raw results here")
    a = p.parse_args()
    if a.quick:
        a.mjpeg_steps, a.webrtc_steps, a.target_steps, a.idle_steps = [2], [2], [2], [2]
        a.warmup, a.seconds = 3, 4
    return a


def build(build_dir):
    subprocess.run(["cmake", "-S", str(REPO_ROOT), "-B", build_dir, "-DCMAKE_BUILD_TYPE=Release",
                    "-DHOUSTONKVM_LOADTEST=ON"], check=True, stdout=subprocess.DEVNULL)
    subprocess.run(["cmake", "--build", build_dir, "-j", str(os.cpu_count() or 1)], check=True)


# ── Measuring the server ────────────────────────────────────────────────

def cpu_seconds(pid):
    """utime + stime of a process, in seconds."""
    with open(f"/proc/{pid}/stat") as f:
        fields = f.read().rsplit(")", 1)[1].split()
    return (int(fields[11]) + int(fields[12])) / CLK_TCK


def proc_status(pid, key):
    with open(f"/proc/{pid}/status") as f:
        for line in f:
            if line.startswith(key + ":"):
                return int(line.split()[1])
    return 0


class ApiProbe(threading.Thread):
    """Times GET /api/status in a loop: is the server still responsive while loaded?"""

    def __init__(self, port):
        super().__init__(daemon=True)
        self.client = Client(port)
        self.samples = []
        self.errors = 0
        self.stop = threading.Event()

    def run(self):
        while not self.stop.is_set():
            t0 = time.perf_counter()
            try:
                status, _, _ = self.client.get("/api/status")
                if status == 200:
                    self.samples.append((time.perf_counter() - t0) * 1000)
                else:
                    self.errors += 1
            except OSError:
                self.errors += 1
            self.stop.wait(0.2)

    def summary(self, since=0):
        s = sorted(self.samples[since:])
        if not s:
            return {"p50_ms": None, "p95_ms": None, "max_ms": None, "errors": self.errors}
        return {"p50_ms": s[len(s) // 2], "p95_ms": s[int(len(s) * 0.95)], "max_ms": s[-1],
                "errors": self.errors}


ENCODE_RX = re.compile(r"encode\+send avg=([\d.]+)ms max=([\d.]+)ms")


# ── The run ─────────────────────────────────────────────────────────────

class LoadTest:
    def __init__(self, args):
        self.args = args
        self.binary = Path(args.build_dir) / "HoustonKVM"
        self.loadgen = Path(args.build_dir) / "HoustonKVM-loadgen"
        self.server = None
        self.owner = None
        self.token = None
        self.targets = []      # ids of synthetic targets, in creation order
        self.jpeg_kb = None    # size of one synthetic frame, for the report

    def start(self):
        # The server's stdout is block-buffered into a pipe, which would hold
        # back the once-a-second encoder timings this reads; stdbuf makes it
        # line-buffered. Both exec, so the pid is still the server's own.
        wrapper = Path(self.args.build_dir) / "HoustonKVM-linebuffered"
        wrapper.write_text(f'#!/bin/sh\nexec stdbuf -oL "{self.binary.resolve()}" "$@"\n')
        wrapper.chmod(0o755)
        self.server = HoustonKVMServer(binary=wrapper, https=self.args.https)
        self.server.start()
        self.owner = Client(self.server.port)
        status, raw, _ = self.server.setup_owner(self.owner, *OWNER)
        assert status == 200, raw
        status, raw, _ = self.owner.post("/api/login", {"username": OWNER[0], "password": OWNER[1]})
        assert status == 200, raw
        status, raw, _ = self.owner.post("/api/tokens", {"label": "loadtest"})
        assert status in (200, 201), raw
        self.token = json.loads(raw)["token"]
        # A fresh database's placeholder target points at /dev/video0;
        # switch it off so only synthetic sources run.
        for t in self.list_targets():
            self.owner.put(f"/api/targets/{t['id']}", {"enabled": False})

    def stop(self):
        if self.server:
            self.server.stop()

    def list_targets(self):
        status, raw, _ = self.owner.get("/api/targets")
        assert status == 200, raw
        return json.loads(raw)["targets"]

    def ensure_targets(self, count):
        a = self.args
        while len(self.targets) < count:
            n = len(self.targets) + 1
            status, raw, _ = self.owner.post("/api/targets", {
                "name": f"loadtest-{n}", "enabled": True,
                "v4l2_device": f"synthetic:{a.pattern}/{n}",
                "capture_width": a.width, "capture_height": a.height, "capture_fps": a.fps,
                "webrtc_bitrate_kbps": a.bitrate,
                # Every target needs its own input backend; a QEMU socket
                # that doesn't exist is one that never touches hardware.
                "qmp_socket": os.path.join(self.server.tmpdir.name, f"no-qemu-{n}.sock"),
            })
            assert status == 201, raw
            self.targets.append(json.loads(raw)["id"])
        self.wait_for_video(self.targets[:count])

    def wait_for_video(self, ids, timeout=30):
        """Until every target serves a snapshot, i.e. its source is running."""
        deadline = time.time() + timeout
        viewer = Client(self.server.port)
        viewer.bearer = self.token
        for tid in ids:
            while (snap := viewer.get(f"/api/targets/{tid}/snapshot"))[0] != 200:
                if time.time() > deadline:
                    raise RuntimeError(f"target {tid} never produced video:\n"
                                       + "".join(self.server.output[-30:]))
                time.sleep(0.2)
            self.jpeg_kb = len(snap[1]) / 1024
        # The WebRTC publisher connects to the SFU in the background after
        # the source starts; give it a moment so the first step isn't
        # measuring its setup.
        time.sleep(2)

    def step(self, per_target):
        """One measured step. per_target: [(target_id, mjpeg_viewers, webrtc_viewers)]."""
        a = self.args
        # Probing from before the viewers start: connecting them must not
        # stall the server either.
        probe = ApiProbe(self.server.port)
        probe.start()
        procs = []
        for tid, m, w in per_target:
            procs.append(subprocess.Popen(
                [str(self.loadgen), f"--port={self.server.port}", f"--token={self.token}",
                 f"--target={tid}", f"--mjpeg={m}", f"--webrtc={w}",
                 f"--warmup={a.warmup}", f"--seconds={a.seconds}", "--sync", *(["--tls"] if a.https else [])],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True))
        for p in procs:
            line = p.stdout.readline()
            if '"ready"' not in line:
                # Stop them all first: one that did start waits for "go" on
                # stdin and would never close its stdout for read() below.
                for q in procs:
                    q.kill()
                probe.stop.set()
                raise RuntimeError(f"loadgen failed to start: {line}{p.stdout.read()}")

        pid = self.server.proc.pid
        out_mark = len(self.server.output)
        connecting = probe.summary()
        watching_from = len(probe.samples)
        cpu0, gen0, wall0 = cpu_seconds(pid), sum(cpu_seconds(p.pid) for p in procs), time.time()
        for p in procs:
            p.stdin.write("go\n")
            p.stdin.flush()
        results = [json.loads(p.stdout.readline()) for p in procs]
        wall = time.time() - wall0
        cpu1, gen1 = cpu_seconds(pid), sum(cpu_seconds(p.pid) for p in procs)
        probe.stop.set()
        probe.join()
        rss_mb = proc_status(pid, "VmRSS") / 1024
        threads = proc_status(pid, "Threads")
        for p in procs:
            p.wait(timeout=30)

        encode = [(float(m.group(1)), float(m.group(2)))
                  for line in self.server.output[out_mark:] if (m := ENCODE_RX.search(line))]
        viewers = [v | {"kind": kind} for r in results for kind in ("mjpeg", "webrtc") for v in r[kind]]
        fps = [v["fps"] for v in viewers]
        return {
            "targets": len(per_target),
            "mjpeg_viewers": sum(m for _, m, _ in per_target),
            "webrtc_viewers": sum(w for _, _, w in per_target),
            "server_cpu_pct": 100 * (cpu1 - cpu0) / wall,
            "loadgen_cpu_pct": 100 * (gen1 - gen0) / wall,
            "server_rss_mb": rss_mb,
            "server_threads": threads,
            "viewer_fps_min": min(fps) if fps else None,
            "viewer_fps_median": statistics.median(fps) if fps else None,
            "mbps_total": sum(v["mbps"] for v in viewers),
            "errors": sorted({v["error"] for v in viewers if "error" in v}),
            "webrtc_unconnected": sum(1 for v in viewers if v["kind"] == "webrtc" and not v.get("connected")),
            "webrtc_setup_s": max(r["webrtc_setup_ms"] for r in results) / 1000,
            "encode_avg_ms": statistics.mean(e[0] for e in encode) if encode else None,
            "encode_max_ms": max(e[1] for e in encode) if encode else None,
            "api": probe.summary(since=watching_from),
            "api_connecting": connecting,
            "webrtc_first_frame_ms": max((v["first_frame_ms"] for v in viewers
                                          if v["kind"] == "webrtc" and v["first_frame_ms"] >= 0),
                                         default=None),
        }

    def holds(self, s):
        return (s["viewer_fps_min"] is not None and not s["errors"]
                and s["viewer_fps_min"] >= HOLD_RATIO * self.args.fps)

    def ramp(self, name, steps, make):
        print(f"\n== {name} ramp: {steps}", flush=True)
        results = []
        for n in steps:
            s = make(n)
            s["holds"] = self.holds(s)
            results.append(s)
            print(f"   {name}={n:<3} server {s['server_cpu_pct']:6.1f}% CPU  "
                  f"{s['server_rss_mb']:6.1f} MB  viewer fps min {s['viewer_fps_min'] or 0:5.1f}  "
                  f"API p95 {s['api']['p95_ms'] or 0:6.1f} ms  "
                  f"max while connecting {s['api_connecting']['max_ms'] or 0:6.1f} ms  "
                  f"{'ok' if s['holds'] else 'FALLS BEHIND'}"
                  + (f"  errors: {'; '.join(s['errors'])}" if s["errors"] else ""), flush=True)
            if not s["holds"]:
                break
        return results

    def run(self):
        a = self.args
        only = set(a.only or ("mjpeg", "webrtc", "targets"))
        out = {}
        if "mjpeg" in only:
            self.ensure_targets(1)
            out["mjpeg"] = self.ramp("mjpeg", a.mjpeg_steps,
                                     lambda n: self.step([(self.targets[0], n, 0)]))
        if "webrtc" in only:
            self.ensure_targets(1)
            out["webrtc"] = self.ramp("webrtc", a.webrtc_steps,
                                      lambda n: self.step([(self.targets[0], 0, n)]))
        if "targets" in only:
            def targets_step(n):
                self.ensure_targets(n)
                # Targets beyond n from an earlier step keep running: they
                # cost CPU whether watched or not, so switch them off.
                self.set_enabled(n)
                return self.step([(tid, 1, 1) for tid in self.targets[:n]])
            out["targets"] = self.ramp("targets", a.target_steps, targets_step)
        if "idle" in only:
            def idle_step(n):
                self.ensure_targets(n)
                self.set_enabled(n)
                return self.step([(tid, 1, 0) for tid in self.targets[:n]])
            out["idle"] = self.ramp("idle", a.idle_steps, idle_step)
        return out

    def set_enabled(self, n):
        for i, tid in enumerate(self.targets):
            self.owner.put(f"/api/targets/{tid}", {"enabled": i < n})
        self.wait_for_video(self.targets[:n])


# ── Report ──────────────────────────────────────────────────────────────

def machine():
    model = ""
    try:
        with open("/proc/cpuinfo") as f:
            model = next((l.split(":", 1)[1].strip() for l in f if l.startswith("model name")), "")
    except OSError:
        pass
    mem_gb = os.sysconf("SC_PAGE_SIZE") * os.sysconf("SC_PHYS_PAGES") / 2**30
    others = subprocess.run(["pgrep", "-c", "-x", "HoustonKVM"], capture_output=True, text=True).stdout.strip()
    return {"cpu": model, "cores": len(os.sched_getaffinity(0)), "mem_gb": round(mem_gb, 1),
            "os": platform.platform(), "loadavg": os.getloadavg()[0],
            "other_houstonkvm": max(0, int(others or 0) - 1)}


def fmt(v, spec=".1f"):
    return "–" if v is None else format(v, spec)


def per_unit(results, key):
    """CPU per unit from the last step that held, less the idle baseline."""
    held = [r for r in results if r["holds"]]
    if len(held) < 2:
        return None
    first, last = held[0], held[-1]
    d = last[key] - first[key]
    return (last["server_cpu_pct"] - first["server_cpu_pct"]) / d if d else None


def report(args, m, res, jpeg_kb):
    cores = m["cores"]
    L = [f"# HoustonKVM load test", "",
         f"{time.strftime('%Y-%m-%d %H:%M %Z')} · {m['cpu']} · {cores} cores · {m['mem_gb']} GB · {m['os']}",
         "",
         f"Synthetic `{args.pattern}` source at {args.width}×{args.height}, {args.fps} fps "
         f"(MJPEG frames of about {fmt(jpeg_kb, '.0f')} KB), "
         f"WebRTC {args.bitrate} kbps, over {'HTTPS' if args.https else 'plain HTTP'}. Each step: {args.warmup:g} s warm-up, {args.seconds:g} s measured. "
         f"A step holds when every viewer gets ≥ {HOLD_RATIO:.0%} of {args.fps} fps. "
         f"CPU is % of one core ({cores * 100}% = whole machine).", ""]
    if m["other_houstonkvm"] or m["loadavg"] > 0.5:
        L += [f"> **Shared machine:** load average was {m['loadavg']:.2f} at the start"
              + (f" and {m['other_houstonkvm']} other HoustonKVM process(es) were running" if m["other_houstonkvm"] else "")
              + ". The numbers include their noise.", ""]

    headers = {
        "mjpeg": ("MJPEG viewers on one target", "mjpeg_viewers"),
        "webrtc": ("WebRTC viewers on one target", "webrtc_viewers"),
        "targets": ("Targets, each with 1 MJPEG + 1 WebRTC viewer", "targets"),
        "idle": ("Targets, each with 1 MJPEG viewer and no WebRTC viewer", "targets"),
    }
    summary = []
    for key, (title, unit) in headers.items():
        if key not in res:
            continue
        rows = res[key]
        L += [f"## {title}", "",
              "| Count | Server CPU | Loadgen CPU | RSS MB | Threads | Viewer fps min / median | Mbit/s | H.264 encode avg / max ms | API max while connecting ms | API p95 / max ms | WebRTC first frame ms | Result |",
              "|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|"]
        for r in rows:
            result = "holds" if r["holds"] else "**falls behind**"
            if r["errors"]:
                result += " — " + "; ".join(r["errors"])
            if r["webrtc_unconnected"]:
                result += f" — {r['webrtc_unconnected']} WebRTC viewer(s) never connected"
            L.append(f"| {r[unit]} | {fmt(r['server_cpu_pct'])}% | {fmt(r['loadgen_cpu_pct'])}% | "
                     f"{fmt(r['server_rss_mb'], '.0f')} | {r['server_threads']} | "
                     f"{fmt(r['viewer_fps_min'])} / {fmt(r['viewer_fps_median'])} | {fmt(r['mbps_total'])} | "
                     f"{fmt(r['encode_avg_ms'])} / {fmt(r['encode_max_ms'])} | "
                     f"{fmt(r['api_connecting']['max_ms'])} | "
                     f"{fmt(r['api']['p95_ms'])} / {fmt(r['api']['max_ms'])} | "
                     f"{fmt(r['webrtc_first_frame_ms'], '.0f')} | {result} |")
        held = [r for r in rows if r["holds"]]
        best = held[-1][unit] if held else 0
        stopped = not rows[-1]["holds"]
        cost = per_unit(rows, unit)
        line = (f"**{best}** held" + (f", {rows[-1][unit]} did not" if stopped else " (the largest step tried)"))
        if cost is not None:
            line += f"; each extra one cost about {cost:.1f}% of a core"
        L += ["", line + ".", ""]
        summary.append(f"- {title}: " + line)

    L += ["## Summary", ""] + summary + ["",
          "Viewers here run on the server machine itself and share its CPU (the Loadgen column), "
          "so a server whose viewers are elsewhere goes further. WebRTC connections also need a "
          "free UDP port in 50000–50100 each (one per target plus one per WebRTC viewer, shared "
          "with any other HoustonKVM on the machine), which caps WebRTC at about 100 whatever the CPU.", ""]
    return "\n".join(L)


def main():
    args = parse_args()
    if not args.no_build:
        build(args.build_dir)
    lt = LoadTest(args)
    m = machine()
    try:
        lt.start()
        res = lt.run()
    finally:
        lt.stop()
    text = report(args, m, res, lt.jpeg_kb)
    print("\n" + text)
    if args.report:
        Path(args.report).write_text(text)
    if args.json:
        Path(args.json).write_text(json.dumps({"machine": m, "args": vars(args), "results": res}, indent=2))
    if args.quick:
        # The smoke test's only claim: video reaches both kinds of viewer.
        bad = [r for rows in res.values() for r in rows if not r["viewer_fps_min"] or r["errors"]]
        if bad:
            sys.exit("loadtest --quick: a viewer received no video")


if __name__ == "__main__":
    main()
