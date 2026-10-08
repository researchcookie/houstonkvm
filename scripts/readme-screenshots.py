#!/usr/bin/env python3
"""The README's screenshots (docs/images/), from a throwaway server.

Every target is a synthetic capture source with a fake CH9329 on a
pseudo-terminal, and the browser is shown console pictures instead of the
synthetic desktop, so nothing touches real hardware. Needs the load-test
build (it has the synthetic sources) and Playwright, as the UI tests do:

    cmake -B build-loadtest -S . -DHOUSTONKVM_LOADTEST=ON && cmake --build build-loadtest
    scripts/readme-screenshots.py [OUTPUT_DIR]      # default docs/images
"""
import os, re, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tests" / "ui"))
sys.path.insert(0, str(ROOT / "tests" / "integration"))
os.environ.setdefault("HOUSTONKVM_BINARY", str(ROOT / "build-loadtest" / "HoustonKVM"))
from ui_harness import BrowserPage, OWNER, PASSWORD, wait_until
from server_harness import Client, HoustonKVMServer
from test_control import FakeChip
from playwright.sync_api import sync_playwright

OUT = sys.argv[1] if len(sys.argv) > 1 else str(ROOT / "docs" / "images")
os.makedirs(OUT, exist_ok=True)
W, H = 1440, 900

server = HoustonKVMServer(); server.start()
base = f"http://127.0.0.1:{server.port}"
owner = Client(server.port)
assert server.setup_owner(owner, *OWNER)[0] == 200
assert owner.post("/api/login", {"username": OWNER[0], "password": OWNER[1]})[0] == 200
for t in owner.as_json(owner.get("/api/targets")[1])["targets"]:
    owner.delete(f"/api/targets/{t['id']}")
assert owner.post("/api/users", {"username": "alex", "password": PASSWORD, "role": "operator"})[0] == 200
assert owner.post("/api/users", {"username": "sam", "password": PASSWORD, "role": "viewer"})[0] == 200

chips, ids = [], {}
specs = [
    ("web-01", "Data center", ["linux", "prod"], "Front door nginx box"),
    ("web-02", "Data center", ["linux", "prod"], "Second nginx box"),
    ("db-primary", "Data center", ["linux", "prod", "postgres"], "PostgreSQL primary"),
    ("build-01", "Lab", ["linux", "ci"], "CI runner"),
    ("win-desk", "Lab", ["windows"], "Windows test desktop"),
    ("nas", "Storage closet", ["storage"], "Home NAS"),
]
for n, (name, group, tags, desc) in enumerate(specs, 1):
    chip = FakeChip(); chips.append(chip)
    st, raw, _ = owner.post("/api/targets", {
        "name": name, "group": group, "tags": tags, "description": desc, "enabled": True,
        "v4l2_device": f"synthetic:desktop/{n}", "capture_width": 1280, "capture_height": 720,
        "capture_fps": 30, "serial_device": chip.path})
    assert st == 201, raw
    ids[name] = owner.as_json(raw)["id"]

def ready():
    ts = owner.as_json(owner.get("/api/targets")[1])["targets"]
    return all(t["status"].get("input_ready") for t in ts)
wait_until(ready, 60, "inputs ready")
time.sleep(2)

pw = sync_playwright().start()
browser = pw.chromium.launch()

CONSOLE = {
"web-01": """CentOS Stream 9
Kernel 5.14.0-729.el9.x86_64 on x86_64

web-01 login: admin
Password:
Last login: Sun Oct  4 09:12:44 on tty1
[admin@web-01 ~]$ systemctl status nginx
<g>●</g> nginx.service - The nginx HTTP and reverse proxy server
     Loaded: loaded (/usr/lib/systemd/system/nginx.service; enabled; preset: disabled)
     Active: <g>active (running)</g> since Sun 2026-10-04 08:58:02 CDT; 2h 14min ago
   Main PID: 1042 (nginx)
      Tasks: 5 (limit: 48812)
     Memory: 7.9M
        CPU: 1.204s
     CGroup: /system.slice/nginx.service
             ├─1042 "nginx: master process /usr/sbin/nginx"
             ├─1043 "nginx: worker process"
             ├─1044 "nginx: worker process"
             ├─1045 "nginx: worker process"
             └─1046 "nginx: worker process"

[admin@web-01 ~]$ uptime
 11:12:31 up 2:14,  1 user,  load average: 0.08, 0.05, 0.01
[admin@web-01 ~]$ <c> </c>""",
"web-02": """CentOS Stream 9
Kernel 5.14.0-729.el9.x86_64 on x86_64

web-02 login: <c> </c>""",
"db-primary": """CentOS Stream 9
Kernel 5.14.0-729.el9.x86_64 on x86_64

db-primary login: <c> </c>""",
"build-01": """[  OK  ] Started <b>Network Manager Wait Online</b>.
[  OK  ] Reached target <b>Network is Online</b>.
         Starting <b>CI runner</b>...
[  OK  ] Started <b>CI runner</b>.
[  OK  ] Started <b>OpenSSH server daemon</b>.
[  OK  ] Reached target <b>Multi-User System</b>.

CentOS Stream 9
Kernel 5.14.0-729.el9.x86_64 on x86_64

build-01 login: <c> </c>""",
"nas": """CentOS Stream 9
Kernel 5.14.0-729.el9.x86_64 on x86_64

nas login: <c> </c>""",
}

def render_frames(browser):
    pg = browser.new_page(viewport={"width": 1280, "height": 720})
    out = {}
    pg.set_content("""<style>body{margin:0;height:720px;font-family:'Segoe UI',sans-serif;color:#fff;
      background:linear-gradient(135deg,#0b3d6b,#2a7ab0 60%,#6fb3d9)}
      .t{position:absolute;left:60px;bottom:120px;font-size:150px;font-weight:200;line-height:1}
      .d{position:absolute;left:66px;bottom:80px;font-size:38px;font-weight:300}</style>
      <div class=t>11:12</div><div class=d>Sunday, October 4</div>""")
    out["win-desk"] = pg.screenshot(type="jpeg", quality=90)
    for name, text in CONSOLE.items():
        text = text.replace("[  OK  ]", "[  <g>OK</g>  ]")
        pg.set_content("""<style>body{margin:0;background:#000;color:#c8c8c8;
          font:17px/1.25 'DejaVu Sans Mono','Liberation Mono',monospace;padding:14px 18px;
          white-space:pre;height:720px;box-sizing:border-box;overflow:hidden}
          g{color:#3fd13f}b{color:#fff;font-weight:normal}c{background:#c8c8c8}</style>""" + text)
        out[name] = pg.screenshot(type="jpeg", quality=90)
    pg.close()
    return out

def fake_video(bp, frames):
    by_id = {ids[n]: f for n, f in frames.items()}
    def pick(route):
        tid = int(re.search(r"/api/targets/(\d+)/", route.request.url).group(1))
        return by_id.get(tid)
    def snap(route):
        f = pick(route)
        route.fulfill(status=200, body=f, content_type="image/jpeg") if f else route.continue_()
    def stream(route):
        f = pick(route)
        if not f: return route.continue_()
        body = b"--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n\r\n" % len(f) + f + b"\r\n"
        route.fulfill(status=200, body=body, content_type="multipart/x-mixed-replace;boundary=frame")
    bp.page.route(re.compile(r"/api/targets/\d+/snapshot"), snap)
    bp.page.route(re.compile(r"/api/targets/\d+/stream"), stream)

FRAMES = render_frames(browser)

def shot(bp, name, settle=1.0):
    time.sleep(settle)
    bp.page.screenshot(path=os.path.join(OUT, name))
    print("wrote", name)

# Operator: directory, then a target in control
bp = BrowserPage(browser, base, W, 1250)
fake_video(bp, FRAMES)
bp.sign_in("alex")
shot(bp, "directory.png", 3)
bp.page.set_viewport_size({"width": W, "height": H})
bp.goto(f"#/targets/{ids['web-01']}")
bp.page.wait_for_selector("#ctrl-btn:not([hidden])", timeout=20000)
bp.page.click("#ctrl-btn")
shot(bp, "target.png", 4)
bp.page.click("#ctrl-btn")   # release control
bp.context.close()

# Owner: admin tabs (a few audit records exist from the above)
bp = BrowserPage(browser, base, W, H)
bp.sign_in(*OWNER)
for tab in ("targets", "network", "audit"):
    bp.goto("#/admin")
    bp.page.click(f"#atab-{tab}")
    shot(bp, f"admin-{tab}.png", 1.5)
bp.context.close()

browser.close(); pw.stop()
for c in chips: c.close()
server.stop()
