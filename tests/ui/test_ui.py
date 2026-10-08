"""The web UI in a real browser: finding a target (directory, search, filters),
driving one, tuning its display, and the owner's target management.

Targets are wired to fake CH9329s (see ui_harness), so "typing reaches the
machine" is checked against the actual HID frames rather than the DOM.
"""
import re
import subprocess
import sys
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from ui_harness import (KEY_A, KEY_B, KEY_C, KEY_7, OWNER, PASSWORD, TestCA, UITestCase, expect, free_port,
                        wait_until)


def key_frames(chip):
    return chip.frames(0x02)


def typed_text(chip, since=0):
    """Lower-case letters, spaces and Enters a chip was sent since frame
    `since`, read from newly pressed keys; anything else shows as '?'."""
    out, prev = [], set()
    for f in key_frames(chip)[since:]:
        keys = {k for k in f[2:8] if k}
        for k in sorted(keys - prev):
            out.append(chr(ord("a") + k - 0x04) if 0x04 <= k <= 0x1D
                       else {0x2C: " ", 0x28: "\n"}.get(k, "?"))
        prev = keys
    return "".join(out)


class TestDirectory(UITestCase):
    @classmethod
    def seed(cls):
        cls.add_user("op", "operator")
        cls.add_user("viewer1", "viewer")
        cls.web = cls.add_target("Web Server 1", group="Rack 3", description="Front door nginx box",
                                 tags=["prod", "linux"])
        cls.db = cls.add_target("DB Server", group="Rack 3", description="Primary PostgreSQL",
                                tags=["prod", "windows"])
        cls.lab = cls.add_target("Lab Box", group="Lab", description="Test bench", tags=["dev"])
        cls.spare = cls.add_target("Spare PC", chip=False, enabled=False, serial_device="/dev/ttyUSB77")

    def test_directory_groups_targets_and_shows_their_status(self):
        bp = self.open("op")
        p = bp.page
        cards = p.locator(".target-card")
        expect(cards).to_have_count(5)  # four above + the migrated "Default target"
        headings = p.locator(".group-heading")
        expect(headings.nth(0)).to_contain_text("Lab")
        expect(headings.nth(1)).to_contain_text("Rack 3")
        expect(headings.nth(2)).to_contain_text("Ungrouped")

        # No capture hardware here: a running target says so; a disabled one says that.
        web = p.locator(".target-card", has_text="Web Server 1")
        expect(web.locator(".status-tag--fault", has_text="No capture device")).to_be_visible()
        spare = p.locator(".target-card", has_text="Spare PC")
        expect(spare.locator(".status-tag--off")).to_contain_text("Disabled")
        expect(p.locator("#targets-count")).to_have_text("5 targets")
        self.assert_no_unexpected_errors(bp)

    def test_search_narrows_then_clears(self):
        bp = self.open("op")
        p = bp.page
        cards = p.locator(".target-card")
        expect(cards).to_have_count(5)

        p.fill("#targets-search", "windows")            # matches a tag
        expect(cards).to_have_count(1)
        expect(cards.first).to_contain_text("DB Server")
        expect(p.locator("#targets-count")).to_have_text("1 target")

        p.fill("#targets-search", "web rack")           # terms may match different fields
        expect(cards).to_have_count(1)
        expect(cards.first).to_contain_text("Web Server 1")

        p.fill("#targets-search", "zzzz-nothing")
        expect(cards).to_have_count(0)
        expect(p.locator("#targets-empty-title")).to_have_text("No matching targets")
        p.click("#targets-empty-actions button")        # "Clear search"
        expect(cards).to_have_count(5)
        expect(p.locator("#targets-search")).to_have_value("")

    def test_tag_chips_and_group_filter(self):
        bp = self.open("op")
        p = bp.page
        cards = p.locator(".target-card")
        expect(cards).to_have_count(5)

        chip = p.locator("#targets-tags .chip", has_text="prod")
        chip.click()
        expect(cards).to_have_count(2)
        expect(p.locator("#targets-tags .chip--on")).to_have_count(1)
        p.locator("#targets-tags .chip--on").click()     # toggles off
        expect(cards).to_have_count(5)

        p.select_option("#targets-group", "Lab")
        expect(cards).to_have_count(1)
        expect(cards.first).to_contain_text("Lab Box")

    def test_clicking_a_tag_on_a_card_filters_by_it(self):
        bp = self.open("op")
        p = bp.page
        p.locator(".target-card", has_text="Lab Box").locator(".tag-btn", has_text="dev").click()
        expect(p.locator(".target-card")).to_have_count(1)

    def test_clicking_a_card_opens_that_target(self):
        bp = self.open("op")
        p = bp.page
        p.locator(".target-card", has_text="Web Server 1").locator("a.target-link").click()
        expect(p.locator("#page-kvm")).to_be_visible()
        expect(p.locator("#kvm-title")).to_have_text("Web Server 1")
        self.assertEqual(p.evaluate("location.hash"), f"#/targets/{self.web['id']}")
        expect(p.locator(".breadcrumb")).to_contain_text("Rack 3")
        self.assertEqual(p.title(), "Web Server 1 — HoustonKVM")
        p.go_back()                                       # browser Back returns to the directory
        expect(p.locator("#page-targets")).to_be_visible()

    def test_slash_jumps_to_the_directory_search(self):
        bp = self.open("op")
        p = bp.page
        p.locator(".target-card").first.wait_for()
        p.locator("#page-targets h1").click()             # focus somewhere neutral
        p.keyboard.press("/")
        expect(p.locator("#targets-search")).to_be_focused()
        p.keyboard.type("lab/x")                          # a "/" typed into the box is just a character
        expect(p.locator("#targets-search")).to_have_value("lab/x")
        p.fill("#targets-search", "lab")
        p.keyboard.press("Enter")
        expect(p.locator(".target-card")).to_have_count(1)
        expect(p.locator(".target-card")).to_contain_text("Lab Box")

    def test_there_is_one_place_to_search_and_the_nav_always_leads_back_to_it(self):
        # No second search box in the header: the Targets tab is how you get
        # back to the directory, from a target or from anywhere else.
        bp = self.open("op")
        p = bp.page
        expect(p.locator("#jump-input")).to_have_count(0)
        expect(p.locator("#app-header input")).to_have_count(0)
        p.locator(".target-card", has_text="DB Server").locator("a.target-link").click()
        expect(p.locator("#kvm-title")).to_have_text("DB Server")
        p.click("#nav-targets")
        expect(p.locator("#page-targets")).to_be_visible()
        expect(p.locator(".target-card").first).to_be_visible()
        p.goto(f"{self.base}/#/settings")
        p.click("#nav-targets")
        expect(p.locator("#page-targets")).to_be_visible()

    def test_slash_does_nothing_off_the_directory_page(self):
        bp = self.open("op")
        p = bp.page
        p.goto(f"{self.base}/#/settings")
        expect(p.locator("#page-settings")).to_be_visible()
        p.locator("#settings-title").click()
        p.keyboard.press("/")
        expect(p.locator("#targets-search")).not_to_be_focused()

    def test_a_late_landing_redirect_does_not_override_where_you_already_went(self):
        # After sign-in the app shows immediately and then decides where to
        # land once the target list arrives. If you navigate before it does,
        # that late decision must not yank you back.
        bp = self.open()
        p = bp.page
        held = []
        p.route(re.compile(r"/api/targets(\?.*)?$"), lambda route: held.append(route))
        p.goto(self.base + "/")
        p.fill("#l-user", "op")
        p.fill("#l-pass", PASSWORD)
        p.click("#form-login button[type=submit]")
        p.wait_for_selector("#app:not([hidden])")
        wait_until(lambda: held, what="the landing list request")
        p.evaluate("location.hash = '#/settings'")          # go somewhere while it's still loading
        expect(p.locator("#page-settings")).to_be_visible()
        for route in held:                                  # now let the landing decision arrive
            route.continue_()
        time.sleep(1)
        self.assertEqual(p.evaluate("location.hash"), "#/settings")
        expect(p.locator("#page-settings")).to_be_visible()

    def test_a_viewer_can_look_but_not_touch(self):
        bp = self.open("viewer1")
        p = bp.page
        expect(p.locator("#nav-admin-item")).to_be_hidden()
        p.locator(".target-card", has_text="Web Server 1").locator("a.target-link").click()
        expect(p.locator("#kvm-title")).to_have_text("Web Server 1")
        expect(p.locator("#ctrl-btn")).to_be_hidden()
        expect(p.locator("#ctrl-hint")).to_contain_text("Operator")
        p.goto(f"{self.base}/#/admin")                    # and can't wander into Admin
        expect(p.locator("#page-targets")).to_be_visible()

    def test_a_target_that_does_not_exist_says_so(self):
        bp = self.open("op")
        p = bp.page
        p.goto(f"{self.base}/#/targets/99999")
        expect(p.locator("#kvm-title")).to_have_text("Target not found")
        expect(p.locator("#video-status")).to_contain_text("doesn")

    def test_target_names_are_text_not_markup(self):
        # Names, descriptions and tags are admin-supplied; they must never be
        # interpreted as HTML anywhere the UI shows them.
        evil = self.add_target("<img src=x onerror=window.__pwned=1>", chip=False, enabled=False,
                               description="<b>bold?</b>", tags=["safe"], serial_device="/dev/ttyUSB78")
        self.addCleanup(self.owner.delete, f"/api/targets/{evil['id']}")
        bp = self.open("op")
        p = bp.page
        card = p.locator(".target-card", has_text="<img src=x")
        expect(card).to_have_count(1)
        expect(card.locator(".target-desc")).to_have_text("<b>bold?</b>")
        self.assertIsNone(p.evaluate("window.__pwned"))
        self.assertEqual(p.locator(".target-card img[src='x']").count(), 0)
        card.locator("a.target-link").click()                 # ...and on the target's own page
        expect(p.locator("#kvm-title")).to_have_text("<img src=x onerror=window.__pwned=1>")
        expect(p.locator(".breadcrumb")).to_contain_text("<img src=x")
        self.assertIsNone(p.evaluate("window.__pwned"))
        self.assertEqual(p.locator("#page-kvm img[src='x'], .breadcrumb img").count(), 0)


class TestControl(UITestCase):
    @classmethod
    def seed(cls):
        cls.add_user("op", "operator")
        cls.add_user("op2", "operator")
        cls.web = cls.add_target("Web Server 1", group="Rack 3")
        cls.db = cls.add_target("DB Server", group="Rack 3")
        cls.wait_input_ready(cls.web["id"])
        cls.wait_input_ready(cls.db["id"])

    def controlled(self, target):
        return self.owner.as_json(self.owner.get(f"/api/targets/{target['id']}")[1])["status"]["controlled"]

    def take_control(self, bp, target):
        p = bp.page
        p.goto(f"{self.base}/#/targets/{target['id']}")
        expect(p.locator("#ctrl-btn")).to_have_text("Take control")
        # Closing a browser context doesn't release control (a dropped tab
        # keeps its seat), so make sure a test can't leave one held.
        self.addCleanup(self.owner.post, f"/api/targets/{target['id']}/control/release")
        p.click("#ctrl-btn")
        expect(p.locator("#ctrl-btn")).to_have_text("Release control")
        expect(p.locator("#ctrl-indicator")).to_be_visible()
        expect(p.locator("#kvm-viewport")).to_be_focused()

    def test_typing_reaches_the_machine_but_typing_in_a_form_field_does_not(self):
        bp = self.open("op")
        p = bp.page
        self.take_control(bp, self.web)
        chip = self.web["chip"]

        p.keyboard.press("a")
        wait_until(lambda: any(f[2] == KEY_A for f in key_frames(chip)), what="A to reach the target")

        before = len(key_frames(chip))
        p.click("#display-btn")                           # the Display panel has a text field
        expect(p.locator("#vid-bitrate")).to_have_value("4000")      # the panel has loaded
        p.fill("#vid-bitrate", "")
        p.keyboard.type("77")                             # this is a setting, not input for the machine
        expect(p.locator("#vid-bitrate")).to_have_value("77")
        time.sleep(0.6)
        leaked = [f for f in key_frames(chip)[before:] if any(k == KEY_7 for k in f[2:8])]
        self.assertEqual(leaked, [], "keystrokes typed into a form field reached the target")
        self.assert_no_unexpected_errors(bp)

    def test_switching_to_another_target_releases_control_of_the_first(self):
        bp = self.open("op")
        p = bp.page
        self.take_control(bp, self.web)
        p.keyboard.down("a")                              # a key is physically held...
        wait_until(lambda: self.web["chip"].held() == {KEY_A}, what="A held on target")

        p.click("#nav-targets")                           # ...when the driver goes to pick another
        p.locator(".target-card", has_text="DB Server").locator("a.target-link").click()
        expect(p.locator("#kvm-title")).to_have_text("DB Server")
        wait_until(lambda: not self.controlled(self.web), what="Web Server 1 to be released")
        wait_until(lambda: self.web["chip"].held() == set(), what="the held key to be released")
        expect(p.locator("#ctrl-indicator")).to_be_hidden()
        expect(p.locator("#ctrl-btn")).to_have_text("Take control")   # and nothing carried over

    def test_going_back_to_the_directory_releases_control(self):
        bp = self.open("op")
        p = bp.page
        self.take_control(bp, self.db)
        p.keyboard.down("a")
        wait_until(lambda: self.db["chip"].held() == {KEY_A}, what="A held")
        p.click(".breadcrumb-link >> text=Targets")
        expect(p.locator("#page-targets")).to_be_visible()
        wait_until(lambda: not self.controlled(self.db), what="control to be released")
        wait_until(lambda: self.db["chip"].held() == set(), what="key to be released")

    def test_signing_out_releases_control(self):
        bp = self.open("op")
        p = bp.page
        self.take_control(bp, self.web)
        p.keyboard.down("a")
        wait_until(lambda: self.web["chip"].held() == {KEY_A}, what="A held")
        p.click("#logout-btn")
        expect(p.locator("#form-login")).to_be_visible()
        wait_until(lambda: not self.controlled(self.web), what="control to be released")
        wait_until(lambda: self.web["chip"].held() == set(), what="key to be released")

    def test_only_one_person_drives_a_target_at_a_time(self):
        first = self.open("op")
        self.take_control(first, self.db)
        second = self.open("op2")
        q = second.page
        q.goto(f"{self.base}/#/targets/{self.db['id']}")
        expect(q.locator("#kvm-status")).to_contain_text("In use")
        q.click("#ctrl-btn")
        expect(q.locator("#ctrl-hint")).to_contain_text("Someone else")
        expect(q.locator("#ctrl-btn")).to_have_text("Take control")
        # ...while a different target is free for them.
        self.take_control(second, self.web)

    def test_the_special_key_menu_sends_ctrl_alt_del(self):
        bp = self.open("op")
        p = bp.page
        self.take_control(bp, self.web)
        p.select_option("#special-key-select", "__ctrlaltdel")
        chip = self.web["chip"]
        # Ctrl (0x01) + Alt (0x04) held together with Delete (0x4c).
        wait_until(lambda: any(f[0] == 0x05 and 0x4C in f[2:8] for f in key_frames(chip)),
                   what="Ctrl+Alt+Del to reach the target")
        wait_until(lambda: chip.held() == set(), what="all keys released afterwards")

    def test_paste_text_is_typed_on_the_machine(self):
        bp = self.open("op")
        p = bp.page
        expect(p.locator("#paste-btn")).to_be_hidden()    # not before taking control
        self.take_control(bp, self.web)
        chip = self.web["chip"]
        since = len(key_frames(chip))
        p.click("#paste-btn")
        expect(p.locator("#paste-text")).to_be_focused()
        p.fill("#paste-text", "echo hello world\n")
        p.click("#paste-type")
        wait_until(lambda: typed_text(chip, since) == "echo hello world\n" and chip.held() == set(),
                   timeout=20, what="the pasted text to be typed")
        expect(p.locator("#paste-msg")).to_have_text("Done.", timeout=10000)
        expect(p.locator("#paste-stop")).to_be_hidden()
        self.assert_no_unexpected_errors(bp)

    def test_paste_names_a_character_it_cannot_type_and_sends_nothing(self):
        bp = self.open("op")
        p = bp.page
        self.take_control(bp, self.web)
        chip = self.web["chip"]
        since = len(key_frames(chip))
        p.click("#paste-btn")
        p.fill("#paste-text", "na\u00efve")
        p.click("#paste-type")
        expect(p.locator("#paste-msg")).to_contain_text("Character 3 (\u2018\u00ef\u2019 U+00EF)")
        time.sleep(0.5)
        self.assertEqual(typed_text(chip, since), "")

    def test_paste_can_be_stopped_and_releasing_control_closes_it(self):
        bp = self.open("op")
        p = bp.page
        self.take_control(bp, self.web)
        chip = self.web["chip"]
        since = len(key_frames(chip))
        p.click("#paste-btn")
        p.fill("#paste-text", "abc " * 100)
        p.click("#paste-type")
        expect(p.locator("#paste-msg")).to_contain_text("characters left")
        p.click("#paste-stop")
        expect(p.locator("#paste-msg")).to_have_text("Stopped.")
        wait_until(lambda: chip.held() == set(), what="nothing left held")
        stopped_at = typed_text(chip, since)
        self.assertLess(len(stopped_at), 400)
        time.sleep(0.5)
        self.assertEqual(typed_text(chip, since), stopped_at)

        p.click("#ctrl-btn")                               # release control
        expect(p.locator("#paste-panel")).to_be_hidden()
        expect(p.locator("#paste-btn")).to_be_hidden()


class TestVideoAndInputState(UITestCase):
    """How the UI presents a target that has (or lacks) a picture and input.

    This machine has no capture card, so a picture is faked at the HTTP
    boundary: the tests answer /snapshot and /stream with a real JPEG and
    flip the "capture_active" flag the server reports. That exercises
    everything the browser does with a live target — thumbnails, the stream
    element, the "no video" banner, the Live tag — without pretending to
    test V4L2 (the integration suite and real hardware cover that).
    """

    @classmethod
    def seed(cls):
        cls.add_user("op", "operator")
        cls.cam = cls.add_target("Camera Box", group="Bench")
        cls.wait_input_ready(cls.cam["id"])
        # Enabled, but with a serial device that isn't there: no way to type.
        cls.mute = cls.add_target("Screen Only", chip=False, serial_device="/dev/ttyUSB91")

    def _jpeg(self):
        page = self.browser.new_page(viewport={"width": 640, "height": 360})
        try:
            page.set_content("<body style='margin:0;background:#2a6fdb'><h1 style='color:#fff;font:48px sans-serif;"
                             "padding:120px'>fake screen</h1></body>")
            return page.screenshot(type="jpeg")
        finally:
            page.close()

    def _fake_picture(self, bp, ids):
        """Serve a picture for `ids` and report those targets as capturing."""
        jpeg = self._jpeg()
        frame = (b"--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n\r\n" % len(jpeg)
                 + jpeg + b"\r\n")

        def mark_live(route):
            resp = route.fetch()
            body = resp.json()
            for t in (body["targets"] if "targets" in body else [body]):
                if t["id"] in ids:
                    t["status"]["capture_active"] = True
                    t["status"]["video_state"] = "good"
            route.fulfill(response=resp, json=body)

        p = bp.page
        p.route(re.compile(r"/api/targets(\?.*)?$"), mark_live)
        p.route(re.compile(r"/api/targets/\d+$"), mark_live)
        p.route(re.compile(r"/api/targets/\d+/snapshot"),
                lambda r: r.fulfill(status=200, body=jpeg, content_type="image/jpeg"))
        p.route(re.compile(r"/api/targets/\d+/stream"),
                lambda r: r.fulfill(status=200, body=frame,
                                    content_type="multipart/x-mixed-replace;boundary=frame"))

    def test_a_live_target_shows_a_thumbnail_and_the_live_tag(self):
        bp = self.open()
        self._fake_picture(bp, {self.cam["id"]})
        bp.sign_in("op")
        p = bp.page
        card = p.locator(".target-card", has_text="Camera Box")
        expect(card.locator(".status-tag--live")).to_be_visible()
        expect(card.locator(".status-tag--nosignal")).to_have_count(0)
        p.wait_for_function("""() => { const i = document.querySelector('.target-card[data-state=live] img');
                                       return !!i && i.complete && i.naturalWidth > 0; }""")
        # The card for the target without a picture keeps its placeholder.
        expect(p.locator(".target-card", has_text="Screen Only").locator(".thumb-note")).to_have_text("No capture device")

    def test_a_live_target_streams_into_the_screen_and_drops_the_no_video_banner(self):
        bp = self.open()
        self._fake_picture(bp, {self.cam["id"]})
        bp.sign_in("op")
        p = bp.page
        p.goto(f"{self.base}/#/targets/{self.cam['id']}")
        expect(p.locator("#kvm-status .status-tag--live")).to_be_visible()
        # A function, not an expression: Playwright eval()s expressions, which the CSP forbids.
        p.wait_for_function("() => document.getElementById('kvm').naturalWidth > 0")
        expect(p.locator("#video-status")).to_be_hidden()

    def test_without_a_picture_the_screen_says_it_is_waiting(self):
        bp = self.open("op")
        p = bp.page
        p.goto(f"{self.base}/#/targets/{self.cam['id']}")
        expect(p.locator("#video-status")).to_contain_text("waiting for the capture device")
        expect(p.locator("#kvm-status .status-tag--fault", has_text="No capture device")).to_be_visible()

    def test_a_target_with_no_input_connection_says_so_before_you_click(self):
        bp = self.open("op")
        p = bp.page
        p.goto(f"{self.base}/#/targets/{self.mute['id']}")
        expect(p.locator("#kvm-status .status-tag--fault", has_text="Input")).to_have_text("Input down")
        expect(p.locator("#ctrl-btn")).to_be_disabled()
        expect(p.locator("#ctrl-hint")).to_contain_text("keyboard/mouse link is down")
        # Whereas one that can take input offers the button as usual.
        p.goto(f"{self.base}/#/targets/{self.cam['id']}")
        expect(p.locator("#ctrl-btn")).to_be_enabled()
        expect(p.locator("#ctrl-hint")).to_have_text("")

    H264 = "RTCRtpReceiver.getCapabilities('video').codecs.some(c => c.mimeType.toLowerCase() === 'video/h264')"

    def test_a_browser_that_cannot_play_h264_stays_on_standard_video_and_says_why(self):
        # The server sends H.264 only. A browser without it (Firefox with no
        # OpenH264 plugin, say) must say so rather than leave the button doing
        # nothing. Simulated here so it's covered whatever engine runs.
        bp = self.open()
        bp.page.add_init_script("""
            const real = RTCRtpReceiver.getCapabilities.bind(RTCRtpReceiver);
            RTCRtpReceiver.getCapabilities = kind => {
              const caps = real(kind);
              if (kind === 'video') caps.codecs = caps.codecs.filter(c => c.mimeType.toLowerCase() !== 'video/h264');
              return caps;
            };""")
        bp.sign_in("op")
        p = bp.page
        p.goto(f"{self.base}/#/targets/{self.cam['id']}")
        p.click("#webrtc-btn")
        expect(p.locator("#ctrl-hint")).to_contain_text("H.264")
        expect(p.locator("#webrtc-btn")).to_have_text("Low-latency video")   # still on standard video
        expect(p.locator("#kvm")).not_to_have_attribute("hidden", "")

    def test_a_low_latency_connection_that_never_comes_up_falls_back_to_standard_video(self):
        bp = self.open("op")
        p = bp.page
        if not p.evaluate(self.H264):
            self.skipTest("this browser engine can't play H.264, so it never gets as far as connecting")
        # The answer is swallowed, so the server never completes the handshake.
        p.route(re.compile(r"/webrtc/answer"), lambda r: r.fulfill(status=200, body='{"ok":true}',
                                                                     content_type="application/json"))
        p.goto(f"{self.base}/#/targets/{self.cam['id']}")
        p.click("#webrtc-btn")
        expect(p.locator("#webrtc-btn")).to_have_text("Standard video")     # optimistic flip...
        expect(p.locator("#ctrl-hint")).to_contain_text("didn’t connect", timeout=30000)   # ...then the watchdog
        expect(p.locator("#webrtc-btn")).to_have_text("Low-latency video")
        expect(p.locator("#kvm")).not_to_have_attribute("hidden", "")

    def test_the_low_latency_toggle_connects_and_switches_back(self):
        bp = self.open("op")
        p = bp.page
        if not p.evaluate(self.H264):
            self.skipTest("this browser engine can't play H.264 (covered by the fallback test above)")
        p.goto(f"{self.base}/#/targets/{self.cam['id']}")
        expect(p.locator("#webrtc-btn")).to_have_text("Low-latency video")
        p.click("#webrtc-btn")
        expect(p.locator("#webrtc-btn")).to_have_text("Standard video", timeout=15000)
        # Which element the UI *shows* is the [hidden] state; with no capture
        # there are no frames, so neither has any pixel size to be "visible".
        expect(p.locator("#kvm-webrtc")).not_to_have_attribute("hidden", "")
        expect(p.locator("#kvm")).to_have_attribute("hidden", "")
        # The diagnostics banner explains an absent picture instead of leaving a blank box.
        expect(p.locator("#video-status")).to_contain_text("ice: connected", timeout=15000)
        # Connected with no STUN server at all, even though Chrome hides its
        # own addresses behind mDNS names: none is needed on a LAN.
        self.assertEqual(p.evaluate("webrtcPc.getConfiguration().iceServers"), [])
        p.click("#webrtc-btn")
        expect(p.locator("#webrtc-btn")).to_have_text("Low-latency video")
        expect(p.locator("#kvm")).not_to_have_attribute("hidden", "")
        expect(p.locator("#kvm-webrtc")).to_have_attribute("hidden", "")

    def test_a_stun_server_that_never_answers_does_not_stop_low_latency_video(self):
        bp = self.open("op")
        p = bp.page
        if not p.evaluate(self.H264):
            self.skipTest("this browser engine can't play H.264 (covered by the fallback test above)")
        # TEST-NET-1: routed nowhere, so neither the server's gathering nor
        # the browser's ever hears back. Both go ahead with what they have.
        status, raw, _ = self.owner.put("/api/admin/ice-servers",
                                        {"ice_servers": [{"url": "stun:192.0.2.1:3478"}]})
        self.assertEqual(status, 200, raw)
        self.addCleanup(self.owner.put, "/api/admin/ice-servers", {"ice_servers": []})
        p.goto(f"{self.base}/#/targets/{self.cam['id']}")
        p.click("#webrtc-btn")
        expect(p.locator("#video-status")).to_contain_text("ice: connected", timeout=20000)
        expect(p.locator("#webrtc-btn")).to_have_text("Standard video")   # it stayed on low-latency
        p.click("#webrtc-btn")
        expect(p.locator("#webrtc-btn")).to_have_text("Low-latency video")

    def test_low_latency_video_uses_the_owners_stun_and_turn_servers(self):
        bp = self.open("op")
        p = bp.page
        if not p.evaluate(self.H264):
            self.skipTest("this browser engine can't play H.264 (covered by the fallback test above)")
        # What the page builds its connection with. (Nothing here answers
        # STUN, so connecting through these isn't the point.)
        p.add_init_script("""
            const Real = window.RTCPeerConnection;
            window.RTCPeerConnection = function (config) {
              window.lastIceServers = config.iceServers;
              return new Real(config);
            };
            window.RTCPeerConnection.prototype = Real.prototype;
        """)
        servers = [{"url": "stun:127.0.0.1:9"},
                   {"url": "turn:127.0.0.1:9", "username": "alice", "credential": "s3cret"}]
        status, raw, _ = self.owner.put("/api/admin/ice-servers", {"ice_servers": servers})
        self.assertEqual(status, 200, raw)
        self.addCleanup(self.owner.put, "/api/admin/ice-servers", {"ice_servers": []})
        p.goto(f"{self.base}/#/targets/{self.cam['id']}")
        p.reload()   # a new hash alone isn't a load, and the script above runs on loads
        p.click("#webrtc-btn")
        wait_until(lambda: p.evaluate("window.lastIceServers !== undefined"), what="the page to connect")
        self.assertEqual(p.evaluate("window.lastIceServers"), [
            {"urls": "stun:127.0.0.1:9"},
            {"urls": "turn:127.0.0.1:9", "username": "alice", "credential": "s3cret"},
        ])


class TestSound(UITestCase):
    """Sound plays with low-latency video, muted until the viewer unmutes."""

    @classmethod
    def seed(cls):
        cls.add_user("op", "operator")
        # A steady tone stands in for a sound card (load-test builds only).
        status, raw, _ = cls.owner.post("/api/targets", {
            "name": "Radio", "enabled": True, "v4l2_device": "/dev/video140",
            "qmp_socket": "/nonexistent/radio.sock", "audio_device": "synthetic:tone/ui"})
        cls.radio = cls.owner.as_json(raw) if status == 201 else None
        cls.quiet = cls.add_target("Quiet Box", chip=False, qmp_socket="/nonexistent/quiet.sock",
                                   audio_device="")
        status, raw, _ = cls.owner.post("/api/targets", {
            "name": "Jukebox", "enabled": True, "v4l2_device": "synthetic:desktop/jukebox",
            "capture_width": 640, "capture_height": 360, "capture_fps": 30,
            "qmp_socket": "/nonexistent/jukebox.sock", "audio_device": "synthetic:tone/jukebox"})
        cls.jukebox = cls.owner.as_json(raw) if status == 201 else None

    def low_latency(self, p, target):
        if not p.evaluate(TestVideoAndInputState.H264):
            self.skipTest("this browser engine can't play H.264")
        p.goto(f"{self.base}/#/targets/{target['id']}")
        expect(p.locator("#webrtc-btn")).to_have_text("Low-latency video")
        expect(p.locator("#sound-btn")).to_be_hidden()      # no sound with standard video
        p.click("#webrtc-btn")
        expect(p.locator("#video-status")).to_contain_text("ice: connected", timeout=15000)

    def audio_packets(self, p):
        return p.evaluate("""async () => {
            if (!webrtcPc) return -1;
            let n = 0;
            (await webrtcPc.getStats()).forEach(r => {
                if (r.type === 'inbound-rtp' && r.kind === 'audio') n += r.packetsReceived;
            });
            return n;
        }""")

    def test_sound_arrives_muted_and_the_viewer_can_unmute_it(self):
        if not self.radio:
            self.skipTest("this server has no synthetic sound (build with -DHOUSTONKVM_LOADTEST=ON)")
        bp = self.open("op")
        p = bp.page
        self.low_latency(p, self.radio)
        expect(p.locator("#sound-btn")).to_have_text("Unmute")
        self.assertTrue(p.evaluate("kvmVideo.muted"))
        wait_until(lambda: self.audio_packets(p) > 50, timeout=15, what="sound to arrive")

        p.click("#sound-btn")
        expect(p.locator("#sound-btn")).to_have_text("Mute")
        self.assertFalse(p.evaluate("kvmVideo.muted"))
        p.click("#sound-btn")
        expect(p.locator("#sound-btn")).to_have_text("Unmute")
        self.assertTrue(p.evaluate("kvmVideo.muted"))

        p.click("#sound-btn")                                  # unmuted, then back to standard video
        p.click("#webrtc-btn")
        expect(p.locator("#sound-btn")).to_be_hidden()
        self.assertTrue(p.evaluate("kvmVideo.muted"))
        self.assert_no_unexpected_errors(bp)

    def test_sound_does_not_hold_the_video_back(self):
        # Browsers lip-sync tracks that share a stream, delaying video to
        # match audio's jitter buffer: ~400 ms of cursor lag in Chrome.
        if not self.jukebox:
            self.skipTest("this server has no synthetic capture (build with -DHOUSTONKVM_LOADTEST=ON)")
        bp = self.open("op")
        p = bp.page
        if not p.evaluate(TestVideoAndInputState.H264):
            self.skipTest("this browser engine can't play H.264")
        p.goto(f"{self.base}/#/targets/{self.jukebox['id']}")
        expect(p.locator("#webrtc-btn")).to_have_text("Low-latency video")
        p.click("#webrtc-btn")
        wait_until(lambda: self.audio_packets(p) > 100, timeout=15, what="sound to arrive")
        video_buffer = """async () => {
            let v = null;
            (await webrtcPc.getStats()).forEach(r => {
                if (r.type === 'inbound-rtp' && r.kind === 'video')
                    v = [r.jitterBufferDelay, r.jitterBufferEmittedCount];
            });
            return v;
        }"""
        # Sync settles over a few seconds, and the stats are totals since the
        # start: look at a window once it has.
        time.sleep(5)
        (d0, n0) = p.evaluate(video_buffer)
        time.sleep(3)
        (d1, n1) = p.evaluate(video_buffer)
        self.assertGreater(n1 - n0, 30, "video stopped playing")
        held_ms = 1000 * (d1 - d0) / (n1 - n0)
        self.assertLess(held_ms, 150, f"video waits {held_ms:.0f} ms in the jitter buffer")

    def test_unmuted_sound_plays(self):
        # Counting packets isn't enough: the browser has to decode real sound
        # and the element has to be playing it, unmuted.
        if not self.jukebox:
            self.skipTest("this server has no synthetic capture (build with -DHOUSTONKVM_LOADTEST=ON)")
        bp = self.open("op")
        p = bp.page
        if not p.evaluate(TestVideoAndInputState.H264):
            self.skipTest("this browser engine can't play H.264")
        # Not low_latency(): its diagnostics banner hides once real video plays.
        p.goto(f"{self.base}/#/targets/{self.jukebox['id']}")
        expect(p.locator("#webrtc-btn")).to_have_text("Low-latency video")
        p.click("#webrtc-btn")
        wait_until(lambda: self.audio_packets(p) > 50, timeout=15, what="sound to arrive")
        p.click("#sound-btn")
        time.sleep(1)
        r = p.evaluate("""async () => {
            let level = 0;
            (await webrtcPc.getStats()).forEach(s => {
                if (s.type === 'inbound-rtp' && s.kind === 'audio') level = s.audioLevel;
            });
            const audio = kvmVideo.srcObject.getAudioTracks();
            return {level, muted: kvmVideo.muted, paused: kvmVideo.paused,
                    audio: audio.length === 1 && audio[0].readyState === 'live'};
        }""")
        self.assertTrue(r["audio"], "the element has no live audio track")
        self.assertFalse(r["muted"])
        self.assertFalse(r["paused"])
        self.assertGreater(r["level"], 0.01, "the browser decodes only silence")

    def test_a_target_without_sound_offers_no_sound_button(self):
        bp = self.open("op")
        p = bp.page
        self.low_latency(p, self.quiet)
        time.sleep(1)
        expect(p.locator("#sound-btn")).to_be_hidden()


class TestDisplayPanel(UITestCase):
    """Resolution and refresh rate live on the target's own page, as dropdowns."""

    @classmethod
    def seed(cls):
        cls.add_user("op", "operator")
        cls.add_user("viewer1", "viewer")
        cls.cam = cls.add_target("Camera Box", group="Bench")
        cls.other = cls.add_target("Other Box", group="Bench")
        cls.parked = cls.add_target("Parked", chip=False, enabled=False, serial_device="/dev/ttyUSB92")

    def saved(self, target=None):
        t = target or self.cam
        s = self.owner.as_json(self.owner.get(f"/api/targets/{t['id']}/video/settings")[1])
        return (s["capture_width"], s["capture_height"], s["capture_fps"])

    def open_display(self, bp, target=None):
        p = bp.page
        p.goto(f"{self.base}/#/targets/{(target or self.cam)['id']}")
        p.click("#display-btn")
        expect(p.locator("#display-panel")).to_be_visible()
        expect(p.locator("#display-btn")).to_have_attribute("aria-expanded", "true")
        expect(p.locator("#vid-apply")).to_be_enabled()                  # settings and modes have loaded
        return p

    def options(self, p, select):
        return p.eval_on_selector_all(f"{select} option", "os => os.map(o => o.textContent)")

    def test_settings_no_longer_has_a_video_tab(self):
        bp = self.open("op")
        p = bp.page
        p.goto(f"{self.base}/#/settings")
        expect(p.locator("#settings-tabs [role=tab]")).to_have_text(["Account", "Preferences", "API tokens"])
        expect(p.locator("#stab-video")).to_have_count(0)

    def test_only_people_who_can_change_video_get_the_button_and_only_on_a_running_target(self):
        viewer = self.open("viewer1")
        viewer.page.goto(f"{self.base}/#/targets/{self.cam['id']}")
        expect(viewer.page.locator("#kvm-title")).to_have_text("Camera Box")
        expect(viewer.page.locator("#display-btn")).to_be_hidden()

        owner = self.open()
        owner.sign_in(*OWNER)
        owner.page.goto(f"{self.base}/#/targets/{self.cam['id']}")
        expect(owner.page.locator("#display-btn")).to_be_visible()      # owners can tune video too
        owner.page.goto(f"{self.base}/#/targets/{self.parked['id']}")
        expect(owner.page.locator("#kvm-title")).to_have_text("Parked")
        expect(owner.page.locator("#display-btn")).to_be_hidden()       # nothing is capturing

    def test_resolution_and_refresh_rate_are_dropdowns_with_nothing_to_type(self):
        bp = self.open("op")
        p = self.open_display(bp)
        for sel in ("#vid-resolution", "#vid-fps-select"):
            self.assertEqual(p.eval_on_selector(sel, "e => e.tagName"), "SELECT")
        expect(p.locator("#vid-resolution")).to_have_value("1280x720")   # what's configured is preselected
        expect(p.locator("#vid-fps-select")).to_have_value("30")
        expect(p.locator("#vid-custom-row")).to_be_hidden()
        res = self.options(p, "#vid-resolution")
        self.assertEqual(res[0], "1920 × 1080")                          # largest first, like a display picker
        self.assertIn("1280 × 720", res)
        self.assertEqual(self.options(p, "#vid-fps-select")[0], "60 Hz")

    def test_choosing_a_resolution_and_refresh_rate_applies_it(self):
        bp = self.open("op")
        p = self.open_display(bp)
        p.select_option("#vid-resolution", "1920x1080")
        p.select_option("#vid-fps-select", "60")
        p.click("#vid-apply")
        expect(p.locator("#vid-msg")).to_contain_text("Applied")
        wait_until(lambda: self.saved() == (1920, 1080, 60), what="the new mode to be saved")
        self.addCleanup(self.owner.put, f"/api/targets/{self.cam['id']}/video/settings",
                        {"capture_width": 1280, "capture_height": 720, "capture_fps": 30,
                         "webrtc_bitrate_kbps": 4000})
        p.reload()                                                       # and it's what you see next time
        p.click("#display-btn")
        expect(p.locator("#vid-resolution")).to_have_value("1920x1080")
        expect(p.locator("#vid-fps-select")).to_have_value("60")

    def test_a_configured_mode_that_is_not_in_the_list_is_kept_not_silently_replaced(self):
        odd = {"capture_width": 1234, "capture_height": 567, "capture_fps": 30, "webrtc_bitrate_kbps": 4000}
        self.owner.put(f"/api/targets/{self.other['id']}/video/settings", odd)
        self.addCleanup(self.owner.put, f"/api/targets/{self.other['id']}/video/settings",
                        {**odd, "capture_width": 1280, "capture_height": 720})
        bp = self.open("op")
        p = self.open_display(bp, self.other)
        expect(p.locator("#vid-resolution")).to_have_value("1234x567")
        p.click("#vid-apply")                                            # applying without touching it changes nothing
        expect(p.locator("#vid-msg")).to_contain_text("Applied")
        self.assertEqual(self.saved(self.other), (1234, 567, 30))

    def test_a_device_that_reports_its_modes_is_offered_exactly_those(self):
        bp = self.open("op")
        bp.page.route(re.compile(r"/api/targets/\d+/video/capabilities"), lambda r: r.fulfill(
            status=200, content_type="application/json",
            body='{"modes":[{"width":1280,"height":720,"fps":[30]},{"width":1920,"height":1080,"fps":[30,60]}],"error":""}'))
        p = self.open_display(bp)
        self.assertEqual(self.options(p, "#vid-resolution"), ["1920 × 1080", "1280 × 720"])   # no "Custom…" to invent one
        expect(p.locator("#vid-caps-err")).to_have_text("")
        p.select_option("#vid-resolution", "1920x1080")
        self.assertEqual(self.options(p, "#vid-fps-select"), ["60 Hz", "30 Hz"])
        p.select_option("#vid-resolution", "1280x720")
        self.assertEqual(self.options(p, "#vid-fps-select"), ["30 Hz"])

    def test_a_device_that_cannot_report_modes_offers_common_ones_and_says_so(self):
        bp = self.open("op")
        p = self.open_display(bp)
        expect(p.locator("#vid-caps-err")).to_contain_text("common ones are listed")
        self.assertEqual(self.options(p, "#vid-resolution")[-1], "Custom…")    # the last resort, at the bottom
        p.select_option("#vid-resolution", "custom")
        expect(p.locator("#vid-custom-row")).to_be_visible()
        expect(p.locator("#vid-fps-field")).to_be_hidden()

    def test_the_panel_closes_when_you_open_another_target(self):
        bp = self.open("op")
        p = self.open_display(bp)
        p.click("#nav-targets")
        p.locator(".target-card", has_text="Other Box").locator("a.target-link").click()
        expect(p.locator("#kvm-title")).to_have_text("Other Box")
        expect(p.locator("#display-panel")).to_be_hidden()
        expect(p.locator("#display-btn")).to_have_attribute("aria-expanded", "false")


class TestInputHealth(UITestCase):
    """The Input health panel: which cable to look at, and the Owner's test."""

    @classmethod
    def seed(cls):
        cls.add_user("op", "operator")
        cls.good = cls.add_target("Healthy Box", group="Bench")
        cls.flaky = cls.add_target("Flaky Box", group="Bench")
        cls.slow = cls.add_target("Slow Box", group="Bench")
        cls.wait_input_ready(cls.good["id"])
        cls.wait_input_ready(cls.flaky["id"])
        cls.wait_input_ready(cls.slow["id"])
        cls.flaky["chip"].fail_every = 2                 # a cable losing every other reply

    def open_panel(self, bp, target):
        p = bp.page
        p.goto(f"{self.base}/#/targets/{target['id']}")
        p.click("#health-btn")
        expect(p.locator("#health-panel")).to_be_visible()
        return p

    def test_a_healthy_link_and_the_owners_test(self):
        bp = self.open(OWNER[0], OWNER[1])
        p = self.open_panel(bp, self.good)
        expect(p.locator("#health-summary")).to_contain_text("Adapter and target link OK")
        expect(p.locator("#health-facts")).to_contain_text("CH9329 serial adapter")
        expect(p.locator("#health-facts")).to_contain_text("9600")
        p.click("#health-test-btn")
        expect(p.locator("#health-test-result")).to_contain_text("20 of 20 replies", timeout=20000)
        expect(p.locator("#health-test-result .status-tag--live")).to_be_visible()
        expect(p.locator("#health-test-btn")).to_be_enabled()
        self.assert_no_unexpected_errors(bp)

    def test_operators_read_it_but_cannot_run_the_test(self):
        bp = self.open("op")
        p = self.open_panel(bp, self.good)
        expect(p.locator("#health-summary")).to_contain_text("link OK")
        expect(p.locator("#health-test-area")).to_be_hidden()
        expect(p.locator("#health-baud-area")).to_be_hidden()

    def test_the_owner_switches_a_slow_adapter_to_115200(self):
        bp = self.open(OWNER[0], OWNER[1])
        p = self.open_panel(bp, self.slow)
        expect(p.locator("#health-baud-btn")).to_be_visible(timeout=15000)
        p.click("#health-baud-btn")
        expect(p.locator("#health-baud-result")).to_contain_text("now runs at 115200 baud", timeout=20000)
        expect(p.locator("#health-baud-result .status-tag--live")).to_be_visible()
        expect(p.locator("#health-baud-btn")).to_be_hidden()
        expect(p.locator("#health-baud-hint")).to_be_hidden()
        expect(p.locator("#health-facts")).to_contain_text("115200")
        self.assertEqual(self.slow["chip"].baud, 115200)
        self.assert_no_unexpected_errors(bp)

    def test_a_flaky_cable_stands_out_in_the_directory(self):
        bp = self.open("op")
        p = bp.page
        p.goto(f"{self.base}/#/targets")
        flaky = p.locator(".target-card", has_text="Flaky Box")
        expect(flaky.locator(".status-tag--warn")).to_have_text("Input flaky", timeout=45000)
        expect(p.locator(".target-card", has_text="Healthy Box").locator(".status-tag--warn")).to_have_count(0)
        p = self.open_panel(bp, self.flaky)
        expect(p.locator("#health-summary")).to_contain_text("loose or failing cable")


class TestAdmin(UITestCase):
    @classmethod
    def seed(cls):
        cls.add_user("op", "operator")
        cls.web = cls.add_target("Web Server 1", group="Rack 3", description="Front door", tags=["prod"])

    def admin(self):
        bp = self.open()
        bp.sign_in(*OWNER)
        bp.page.goto(f"{self.base}/#/admin")
        expect(bp.page.locator("#page-admin")).to_be_visible()
        return bp

    def fill_new_target(self, p, name, chip, **extra):
        p.click("#target-add-btn")
        expect(p.locator("#form-target")).to_be_visible()
        p.fill("#t-name", name)
        p.fill("#t-desc", extra.get("desc", ""))
        p.fill("#t-group", extra.get("group", ""))
        p.fill("#t-tags", extra.get("tags", ""))
        # Wire it explicitly through the "Other…" paths so the test doesn't
        # depend on which devices happen to be plugged into this machine.
        p.select_option("#t-video", "custom")
        p.fill("#t-video-custom", extra.get("video", f"/dev/video{400 + self._next_device()}"))
        p.check("#t-in-serial")
        p.select_option("#t-serial", "custom")
        p.fill("#t-serial-custom", chip.path)
        p.select_option("#t-resolution", "custom")
        p.fill("#t-width", "1280"); p.fill("#t-height", "720"); p.fill("#t-fps", "30")

    def test_owner_adds_a_target_through_the_form(self):
        chip = self.new_chip()
        bp = self.admin()
        p = bp.page
        self.fill_new_target(p, "Build Box", chip, desc="CI runner", group="Lab", tags="ci, Linux ,ci")
        p.click("#t-save")
        expect(p.locator("#admin-targets-msg")).to_contain_text("Build Box saved")
        row = p.locator("#admin-targets-body tr", has_text="Build Box")
        expect(row).to_have_count(1)
        expect(row).to_contain_text("Lab")
        expect(row.locator(".tag", has_text="ci")).to_have_count(1)     # tags de-duplicated,
        expect(row.locator(".tag", has_text="linux")).to_have_count(1)  # and lowercased

        made = next(t for t in self.owner.as_json(self.owner.get("/api/targets")[1])["targets"]
                    if t["name"] == "Build Box")
        self.assertEqual(made["config"]["serial_device"], chip.path)
        self.assertEqual(made["tags"], ["ci", "linux"])

        # ...and it's immediately findable by anyone.
        q = self.open("op").page
        q.fill("#targets-search", "ci runner")
        expect(q.locator(".target-card")).to_have_count(1)
        self.assert_no_unexpected_errors(bp)

    def test_a_duplicate_name_is_reported_and_nothing_is_lost(self):
        chip = self.new_chip()
        bp = self.admin()
        p = bp.page
        self.fill_new_target(p, "web server 1", chip)      # same as an existing name, differently cased
        p.click("#t-save")
        expect(p.locator("#t-err")).to_contain_text("already exists")
        expect(p.locator("#form-target")).to_be_visible()  # still open, so nothing typed is lost
        expect(p.locator("#t-name")).to_have_value("web server 1")

    def test_group_spelling_snaps_to_an_existing_group(self):
        bp = self.admin()
        p = bp.page
        p.click("#target-add-btn")
        p.fill("#t-group", "rack 3")
        p.press("#t-group", "Tab")
        expect(p.locator("#t-group")).to_have_value("Rack 3")

    def test_typing_while_the_device_scan_is_still_running_is_not_wiped(self):
        # The form appears at once but its device pickers wait on a scan of
        # what's plugged in, which can be slow. Text typed in the meantime
        # must survive the scan returning.
        bp = self.admin()
        p = bp.page
        held = []
        p.route(re.compile(r"/api/admin/devices"), lambda route: held.append(route))   # hold the scan open
        p.click("#target-add-btn")
        expect(p.locator("#form-target")).to_be_visible()
        p.fill("#t-name", "Typed early")
        p.fill("#t-group", "rack 3")
        p.fill("#t-tags", "early")
        expect(p.locator("#t-save")).to_be_disabled()      # can't save with the pickers still empty
        wait_until(lambda: held, what="the device scan request")
        held[0].continue_()                                # ...now let the scan finish
        expect(p.locator("#t-save")).to_be_enabled()
        expect(p.locator("#t-name")).to_have_value("Typed early")
        expect(p.locator("#t-tags")).to_have_value("early")
        expect(p.locator("#t-group")).to_have_value("Rack 3")   # and the late group list still snapped it

    def test_existing_tags_are_offered_as_one_click_suggestions(self):
        bp = self.admin()
        p = bp.page
        p.click("#target-add-btn")
        p.locator("#t-tags-suggest .chip", has_text="prod").click()
        expect(p.locator("#t-tags")).to_have_value("prod")

    def test_the_local_gadget_option_is_disabled_while_another_target_uses_it(self):
        # The migrated default target uses the local USB gadget, which only one target can.
        bp = self.admin()
        p = bp.page
        p.click("#target-add-btn")
        expect(p.locator("#t-in-gadget")).to_be_disabled()
        expect(p.locator("#t-in-serial")).to_be_checked()

    def test_enable_disable_make_default_and_delete(self):
        t = self.add_target("Throwaway", chip=False, enabled=True, serial_device="/dev/ttyUSB79")
        bp = self.admin()
        p = bp.page
        row = p.locator("#admin-targets-body tr", has_text="Throwaway")

        p.get_by_role("button", name="Disable Throwaway").click()
        expect(row).to_contain_text("Disabled")
        p.get_by_role("button", name="Enable Throwaway").click()
        expect(row).not_to_contain_text("Disabled")

        p.get_by_role("button", name="Make default Throwaway").click()   # confirm() is auto-accepted
        expect(row.locator(".status-tag--default")).to_be_visible()
        self.assertEqual(self.owner.as_json(self.owner.get(f"/api/targets/{t['id']}")[1])["default"], True)

        p.get_by_role("button", name="Delete Throwaway").click()
        expect(row).to_have_count(0)
        self.assertEqual(self.owner.get(f"/api/targets/{t['id']}")[0], 404)
        # Restore a default so later tests see the state they expect.
        first = self.owner.as_json(self.owner.get("/api/targets")[1])["targets"][0]
        self.owner.put(f"/api/targets/{first['id']}", {"default": True})

    def test_editing_a_target_keeps_what_you_did_not_change(self):
        t = self.add_target("Editable", chip=False, enabled=False, description="before", group="Rack 3",
                            tags=["keep"], serial_device="/dev/ttyUSB80")
        bp = self.admin()
        p = bp.page
        p.get_by_role("button", name="Edit Editable").click()
        expect(p.locator("#form-target")).to_be_visible()
        expect(p.locator("#t-name")).to_have_value("Editable")
        expect(p.locator("#t-tags")).to_have_value("keep")
        p.fill("#t-desc", "after")
        p.click("#t-save")
        expect(p.locator("#admin-targets-msg")).to_contain_text("Editable saved")
        after = self.owner.as_json(self.owner.get(f"/api/targets/{t['id']}")[1])
        self.assertEqual((after["description"], after["group"], after["tags"]), ("after", "Rack 3", ["keep"]))
        self.assertEqual(after["config"]["serial_device"], "/dev/ttyUSB80")
        self.owner.delete(f"/api/targets/{t['id']}")

    def test_the_editor_sets_where_sound_comes_from(self):
        t = self.add_target("Quiet", chip=False, enabled=False, serial_device="/dev/ttyUSB81")
        bp = self.admin()
        p = bp.page
        p.get_by_role("button", name="Edit Quiet").click()
        expect(p.locator("#t-audio")).to_have_value("auto")               # the default
        p.select_option("#t-audio", "")
        p.click("#t-save")
        expect(p.locator("#admin-targets-msg")).to_contain_text("Quiet saved")
        cfg = lambda: self.owner.as_json(self.owner.get(f"/api/targets/{t['id']}")[1])["config"]
        self.assertEqual(cfg()["audio_device"], "")

        # A sound card that isn't plugged in right now is kept, not swapped.
        self.owner.put(f"/api/targets/{t['id']}", {"audio_device": "plughw:CARD=Gone,DEV=0"})
        p.reload()                                       # the list holds what it loaded
        p.get_by_role("button", name="Edit Quiet").click()
        expect(p.locator("#t-audio")).to_have_value("plughw:CARD=Gone,DEV=0")
        expect(p.locator("#t-audio option:checked")).to_have_text("plughw:CARD=Gone,DEV=0 (not plugged in)")
        p.click("#t-save")
        expect(p.locator("#admin-targets-msg")).to_contain_text("Quiet saved")
        self.assertEqual(cfg()["audio_device"], "plughw:CARD=Gone,DEV=0")
        self.owner.delete(f"/api/targets/{t['id']}")

    def test_an_operator_cannot_reach_admin(self):
        bp = self.open("op")
        expect(bp.page.locator("#nav-admin-item")).to_be_hidden()
        bp.page.goto(f"{self.base}/#/admin")
        expect(bp.page.locator("#page-targets")).to_be_visible()


class TestNetworkAdmin(UITestCase):
    """The Owner decides which STUN/TURN servers low-latency video uses."""

    def test_adding_and_removing_servers(self):
        self.addCleanup(self.owner.put, "/api/admin/ice-servers", {"ice_servers": []})
        bp = self.open()
        p = bp.page
        bp.sign_in(*OWNER)
        p.goto(f"{self.base}/#/admin")
        p.click("#atab-network")
        expect(p.locator("#ice-empty")).to_be_visible()
        expect(p.locator("#ice-managed")).to_be_hidden()

        p.fill("#ice-url", "stun:stun.example.com:3478")
        expect(p.locator("#ice-login")).to_be_hidden()      # STUN takes no login
        p.click("#form-ice button[type=submit]")
        expect(p.locator("#ice-list")).to_contain_text("stun:stun.example.com:3478")
        expect(p.locator("#ice-empty")).to_be_hidden()
        expect(p.locator("#ice-url")).to_have_value("")

        p.fill("#ice-url", "turn:turn.example.com:3478")
        expect(p.locator("#ice-login")).to_be_visible()
        p.fill("#ice-username", "alice")
        p.fill("#ice-credential", "s3cret")
        p.click("#form-ice button[type=submit]")
        expect(p.locator("#ice-list li")).to_have_count(2)
        expect(p.locator("#ice-list")).to_contain_text("TURN · username alice")
        self.assertNotIn("s3cret", p.locator("#ice-list").inner_text())

        # The server's reason is shown, and nothing changes.
        p.fill("#ice-url", "stun.example.com")
        p.click("#form-ice button[type=submit]")
        expect(p.locator("#ice-msg")).to_contain_text("stun:, turn: or turns:")
        expect(p.locator("#ice-list li")).to_have_count(2)
        bp.errors.remove(f"HTTP 400 {self.base}/api/admin/ice-servers")   # that refusal, and only it

        p.click("button[aria-label='Remove stun:stun.example.com:3478']")
        expect(p.locator("#ice-list li")).to_have_count(1)
        status, raw, _ = self.owner.get("/api/admin/ice-servers")
        self.assertEqual([s["url"] for s in self.owner.as_json(raw)["ice_servers"]],
                         ["turn:turn.example.com:3478"])

        p.click("#atab-audit")
        expect(p.locator("#audit-events-body")).to_contain_text("Changed the STUN and TURN servers")
        self.assert_no_unexpected_errors(bp)


class TestNetworkAdminFixedByConfig(UITestCase):
    SERVER_ARGS = ("--ice-server=stun:stun.example.com",)

    def test_the_list_is_shown_read_only(self):
        bp = self.open()
        p = bp.page
        bp.sign_in(*OWNER)
        p.goto(f"{self.base}/#/admin")
        p.click("#atab-network")
        expect(p.locator("#ice-managed")).to_be_visible()
        expect(p.locator("#ice-list")).to_contain_text("stun:stun.example.com")
        expect(p.locator("#form-ice")).to_be_hidden()
        expect(p.locator("#ice-list button")).to_have_count(0)
        self.assert_no_unexpected_errors(bp)


HTTPS_PORT = free_port()


class TestHttpsAdmin(UITestCase):
    """HTTPS from Admin -> Network, step 1 (a certificate) then step 2 (turn
    it on): a certificate from HoustonKVM's own CA, turning HTTPS on (landing
    on the HTTPS page confirms it), the revert when no browser gets there, a
    CSR for the organization's CA, and turning it off."""
    SERVER_ARGS = (f"--https-port={HTTPS_PORT}", "--tls-confirm-seconds=4")

    def network_tab(self, p, base=None):
        p.goto(f"{base or self.base}/#/admin")
        p.click("#atab-network")
        expect(p.locator("#https-status")).not_to_be_empty()

    def mode(self):
        status, raw, _ = self.owner.get("/api/tls")
        return self.owner.as_json(raw)["mode"] if status == 200 else None

    def test_1_a_certificate_and_https_without_a_shell(self):
        bp = self.open(ignore_https_errors=True)
        p = bp.page
        bp.sign_in(*OWNER)
        p.goto(f"{self.base}/#/admin")
        expect(p.locator("#https-banner")).to_contain_text("plain HTTP")
        p.click("#https-banner-link")
        expect(p.locator("#https-status")).to_contain_text("Plain HTTP")
        expect(p.locator("#https-enable")).to_be_disabled()   # no certificate yet
        # The organization's CA is the first choice; HoustonKVM's own is one click away.
        expect(p.locator("#https-source-org")).to_be_checked()
        expect(p.locator("#form-https-csr")).to_be_visible()
        expect(p.locator("#https-route-own")).to_be_hidden()
        p.click("#https-upload summary")
        expect(p.locator("#https-upload-warning")).to_be_hidden()   # 127.0.0.1: the key wouldn't cross a network
        p.click("#https-source-own")
        expect(p.locator("#https-route-org")).to_be_hidden()
        expect(p.locator("#https-ca-download")).to_be_hidden()
        self.assertTrue(p.locator("#https-auto-renew-row").is_hidden())   # nothing of its own to renew yet

        p.click("#https-generate")
        expect(p.locator("#https-msg")).to_contain_text("Made a certificate")
        expect(p.locator("#https-status")).to_contain_text("HoustonKVM’s own certificate authority")
        expect(p.locator("#https-status")).to_contain_text("127.0.0.1")
        expect(p.locator("#https-ca-download")).to_be_visible()
        with p.expect_download() as download:
            p.click("#https-ca-download")
        self.assertIn("BEGIN CERTIFICATE", Path(download.value.path()).read_text())
        expect(p.locator("#https-generate")).to_have_text("Make a new certificate")
        expect(p.locator("#https-cert-tools")).to_have_class(re.compile(r"\bstep--done\b"))
        p.click("#https-advanced summary")
        expect(p.locator("#https-auto-renew-row")).to_be_visible()

        # On: the browser lands on HTTPS, which confirms it.
        p.click("#https-enable")
        p.wait_for_url(f"https://127.0.0.1:{HTTPS_PORT}/**")
        expect(p.locator("#auth-notice")).to_contain_text("HTTPS is on")
        self.assertEqual(p.evaluate("location.hash"), "")
        bp.sign_in(*OWNER, here=True)
        https = f"https://127.0.0.1:{HTTPS_PORT}"
        self.network_tab(p, https)
        expect(p.locator("#https-status")).to_contain_text(f"HTTPS on port {HTTPS_PORT}")
        expect(p.locator("#https-banner")).to_be_hidden()
        expect(p.locator("#https-hsts")).to_be_enabled()      # HTTPS on, certificate from a CA
        p.click("#atab-audit")
        expect(p.locator("#audit-events-body")).to_contain_text("Turned on HTTPS")
        expect(p.locator("#audit-events-body")).to_contain_text("Made HoustonKVM’s own certificate authority")

        # Off again: back to plain HTTP.
        p.click("#atab-network")
        p.click("#https-disable")
        p.wait_for_url(f"http://127.0.0.1:{self.server.http_port}/**")
        wait_until(lambda: self.mode() == "http", what="HTTPS off")
        self.assert_no_unexpected_errors(bp)

    def test_2_no_browser_reaches_https_so_it_reverts(self):
        bp = self.open()
        p = bp.page
        bp.sign_in(*OWNER)
        self.network_tab(p)
        if p.locator("#https-enable").is_disabled():
            p.click("#https-source-own")
            p.click("#https-generate")
            expect(p.locator("#https-enable")).to_be_enabled()
        p.route("https://**/*", lambda route: route.abort())   # as if the port were blocked
        p.click("#https-enable")
        p.wait_for_timeout(500)
        self.network_tab(p)
        expect(p.locator("#https-status")).to_contain_text("Waiting for a browser")
        expect(p.locator("#https-disable")).to_have_text("Cancel")
        expect(p.locator("#https-status")).to_contain_text("Plain HTTP", timeout=10000)
        expect(p.locator("#https-status")).to_contain_text("firewall-cmd --permanent --add-port=")
        p.click("#atab-audit")
        expect(p.locator("#audit-events-body")).to_contain_text("HTTPS went back off")
        self.assert_no_unexpected_errors(bp)

    def test_3_a_csr_for_the_companys_ca(self):
        bp = self.open()
        p = bp.page
        bp.sign_in(*OWNER)
        self.network_tab(p)
        p.click("#https-source-org")   # the certificate now is HoustonKVM's own, which shows that route
        expect(p.locator("#https-route-own")).to_be_hidden()
        p.fill("#https-csr-names", "kvm.example.com\n127.0.0.1")
        p.fill("#https-csr-org", "Example Co")
        p.fill("#https-csr-country", "us")
        p.click("#form-https-csr button[type=submit]")
        expect(p.locator("#https-csr-pending")).to_be_visible()
        csr = p.locator("#https-csr-text").input_value()
        self.assertIn("BEGIN CERTIFICATE REQUEST", csr)

        ca = TestCA(Path(self.server.tmpdir.name) / "corp-ca")
        (ca.dir / "req.csr").write_text(csr)
        subprocess.run(["openssl", "x509", "-req", "-in", ca.dir / "req.csr", "-CA", ca.crt, "-CAkey", ca.key,
                        "-CAcreateserial", "-days", "60", "-copy_extensions", "copyall",
                        "-out", ca.dir / "signed.crt"], check=True, capture_output=True)
        p.fill("#https-signed", (ca.dir / "signed.crt").read_text() + ca.pem)
        p.click("#form-https-signed button[type=submit]")
        expect(p.locator("#https-msg")).to_contain_text("Installed the certificate from your certificate authority")
        expect(p.locator("#https-status")).to_contain_text("CN=HoustonKVM Test CA")
        expect(p.locator("#https-status")).to_contain_text("kvm.example.com")
        expect(p.locator("#https-csr-pending")).to_be_hidden()
        self.assert_no_unexpected_errors(bp)


class TestHttpsFixedByConfig(UITestCase):
    SERVER_ARGS = ("--tls=off",)

    def test_turning_it_on_isnt_offered(self):
        bp = self.open()
        p = bp.page
        bp.sign_in(*OWNER)
        p.goto(f"{self.base}/#/admin")
        expect(p.locator("#https-banner")).to_be_hidden()   # the configuration chose plain HTTP
        p.click("#atab-network")
        expect(p.locator("#https-pinned")).to_contain_text("--tls=off")
        expect(p.locator("#https-enable")).to_be_hidden()
        expect(p.locator("#https-cert-tools")).to_be_visible()   # a certificate can still be got ready
        self.assert_no_unexpected_errors(bp)


class TestLanding(UITestCase):
    """With one target the app opens straight into it; with several, the directory."""

    def test_single_target_opens_directly_and_two_show_the_directory(self):
        bp = self.open()
        bp.sign_in(*OWNER)                                # only the migrated default target exists
        expect(bp.page.locator("#page-kvm")).to_be_visible()
        self.assertEqual(bp.page.evaluate("location.hash"), "#/targets/1")

        self.add_target("Second", chip=False, enabled=False, serial_device="/dev/ttyUSB81")
        bp.page.click("#logout-btn")
        expect(bp.page.locator("#form-login")).to_be_visible()
        bp.sign_in(*OWNER)
        expect(bp.page.locator("#page-targets")).to_be_visible()
        expect(bp.page.locator(".target-card")).to_have_count(2)



class TestFirstRun(UITestCase):
    """A fresh install asks for the setup code the server logged."""
    SETUP_OWNER = False

    def test_setup_needs_the_logged_code_then_signs_the_owner_in(self):
        bp = self.open()
        p = bp.page
        p.goto(self.base + "/")
        expect(p.locator("#form-setup")).to_be_visible()
        expect(p.locator("#s-code")).to_be_focused()
        p.fill("#s-code", "AAAA-AAAA-AAAA")
        p.fill("#s-user", OWNER[0])
        p.fill("#s-pass", OWNER[1])
        p.click("#form-setup button[type=submit]")
        expect(p.locator("#s-err")).to_contain_text("journalctl")

        p.fill("#s-code", self.server.setup_code.lower())
        p.click("#form-setup button[type=submit]")
        expect(p.locator("#app")).to_be_visible()
        self.assertEqual([e for e in bp.unexpected_errors() if "/api/setup" not in e], [])


class TestEmptySystem(UITestCase):
    def test_an_empty_system_invites_the_owner_to_add_a_target(self):
        for t in self.owner.as_json(self.owner.get("/api/targets")[1])["targets"]:
            self.owner.delete(f"/api/targets/{t['id']}")
        bp = self.open()
        bp.sign_in(*OWNER)
        expect(bp.page.locator("#targets-empty-title")).to_have_text("No targets yet")
        bp.page.click("#targets-empty-actions a")         # "Add a target" opens the editor directly
        expect(bp.page.locator("#form-target")).to_be_visible()


class TestLookAndPhone(UITestCase):
    @classmethod
    def seed(cls):
        cls.add_user("op", "operator")
        cls.add_target("Web Server 1", chip=False, enabled=False, serial_device="/dev/ttyUSB82")
        cls.add_target("DB Server", chip=False, enabled=False, serial_device="/dev/ttyUSB83")

    def test_one_look_with_our_own_fonts(self):
        bp = self.open("op")
        p = bp.page
        expect(p.locator(".target-card").first).to_be_visible()
        # There is one theme; nothing switches it.
        self.assertIsNone(p.get_attribute("body", "data-theme"))
        p.goto(f"{self.base}/#/settings")
        p.click("#stab-preferences")
        expect(p.locator("#pref-theme")).to_have_count(0)
        # Both fonts load from our own origin, past the CSP, and are in use.
        p.evaluate("document.fonts.ready")
        loaded = p.evaluate("""[...document.fonts].filter(f => f.status === 'loaded')
                                                  .map(f => f.family.replace(/"/g, ''))""")
        self.assertIn("Playfair Display", loaded)
        self.assertIn("Source Sans 3", loaded)
        self.assertIn("Playfair Display", p.evaluate("getComputedStyle(document.querySelector('.page-title')).fontFamily"))
        self.assertIn("Source Sans 3", p.evaluate("getComputedStyle(document.body).fontFamily"))
        self.assert_no_unexpected_errors(bp)

    def test_on_a_phone_the_menu_holds_navigation(self):
        bp = self.open("op", width=390, height=800)
        p = bp.page
        expect(p.locator(".target-card").first).to_be_visible()
        expect(p.locator("#nav-targets")).to_be_hidden()            # tucked into the menu
        p.click(".menu-btn")
        expect(p.locator("#nav-targets")).to_be_visible()
        expect(p.locator("#nav-settings")).to_be_visible()
        p.click("#nav-settings")
        expect(p.locator("#page-settings")).to_be_visible()
        expect(p.locator("#nav-targets")).to_be_hidden()            # and the menu closes itself
        p.locator(".menu-btn").click()
        p.click("#nav-targets")
        expect(p.locator("#page-targets")).to_be_visible()
        # No sideways scrolling on a phone.
        self.assertLessEqual(p.evaluate("document.documentElement.scrollWidth"), 390)


class TestOwnerSession(UITestCase):
    """An Owner's session is unmistakable; nobody else's looks like it."""

    @classmethod
    def seed(cls):
        cls.add_user("op", "operator")
        cls.add_user("viewer1", "viewer")
        cls.add_target("Web Server 1", chip=False, enabled=False, serial_device="/dev/ttyUSB84")

    def header_bg(self, p):
        return p.evaluate("getComputedStyle(document.getElementById('app-header')).backgroundColor")

    def test_the_owner_session_has_its_own_look_and_nobody_else_gets_it(self):
        bp = self.open()
        p = bp.page
        bp.sign_in(*OWNER)
        expect(p.locator("body")).to_have_attribute("data-session", "owner")
        expect(p.locator("#owner-rail")).to_be_visible()
        expect(p.locator("#owner-rail")).to_contain_text("Owner session")
        expect(p).to_have_title(re.compile(r"^Owner · Targets — HoustonKVM$"))
        p.goto(f"{self.base}/#/admin")
        expect(p).to_have_title(re.compile(r"^Owner · Admin — HoustonKVM$"))
        owner_bg = self.header_bg(p)
        owner_icon = p.get_attribute("#favicon", "href")

        # Signing out drops it at once, even before anyone else signs in.
        p.click("#logout-btn")
        expect(p.locator("#form-login")).to_be_visible()
        self.assertIsNone(p.get_attribute("body", "data-session"))
        self.assertNotIn("Owner", p.title())

        for user in ("op", "viewer1"):
            with self.subTest(user=user):
                bp.sign_in(user)
                expect(p.locator(".target-card").first).to_be_visible()
                self.assertIsNone(p.get_attribute("body", "data-session"))
                expect(p.locator("#owner-rail")).to_be_hidden()
                self.assertFalse(p.title().startswith("Owner"), p.title())
                self.assertNotEqual(self.header_bg(p), owner_bg)
                self.assertNotEqual(p.get_attribute("#favicon", "href"), owner_icon)
                p.click("#logout-btn")
                expect(p.locator("#form-login")).to_be_visible()
        self.assert_no_unexpected_errors(bp)

    def test_the_owner_look_holds_on_a_phone(self):
        bp = self.open(width=390, height=800)
        p = bp.page
        bp.sign_in(*OWNER)
        expect(p.locator("#owner-rail")).to_be_visible()
        self.assertLessEqual(p.evaluate("document.documentElement.scrollWidth"), 390)

    def test_changing_a_password_says_how_many_sessions_were_signed_out(self):
        self.add_user("pw_user", "viewer")
        elsewhere = self.open("pw_user")
        bp = self.open("pw_user")
        p = bp.page
        p.goto(f"{self.base}/#/settings")
        p.fill("#pw-current", PASSWORD)
        p.fill("#pw-new", "a brand new password")
        p.click("#form-password button[type=submit]")
        expect(p.locator("#pw-msg")).to_have_text("Password changed. Signed out 1 other session.")
        # The other browser is signed out: its next request comes back 401.
        status = elsewhere.page.evaluate("fetch('/api/me').then(r => r.status)")
        self.assertEqual(status, 401)


class TestApiTokens(UITestCase):
    """Tokens are made with a scope and an expiry, and the list says which."""

    @classmethod
    def seed(cls):
        cls.add_user("op", "operator")

    def test_a_token_is_made_with_a_scope_and_an_expiry(self):
        bp = self.open("op")
        p = bp.page
        p.goto(f"{self.base}/#/settings")
        p.click("#stab-bots")
        expect(p.locator("#token-expiry")).to_have_value("90")        # expiring is the default
        expect(p.locator("#token-scope-metrics")).to_be_disabled()     # only an Owner's to make
        p.fill("#token-label", "dashboard")
        p.select_option("#token-scope", "read")
        p.select_option("#token-expiry", "never")
        p.click("#form-token button[type=submit]")
        expect(p.locator(".token-reveal")).to_be_visible()
        row = p.locator("#token-list li", has_text="dashboard")
        expect(row).to_contain_text("read only")
        expect(row).to_contain_text("never expires")
        self.assert_no_unexpected_errors(bp)

    def test_an_owner_can_make_a_metrics_token(self):
        bp = self.open()
        p = bp.page
        bp.sign_in(*OWNER)
        p.goto(f"{self.base}/#/settings")
        p.click("#stab-bots")
        expect(p.locator("#token-scope-metrics")).to_be_enabled()
        p.fill("#token-label", "prometheus")
        p.select_option("#token-scope", "metrics")
        p.click("#form-token button[type=submit]")
        row = p.locator("#token-list li", has_text="prometheus")
        expect(row).to_contain_text("metrics only")
        expect(row).to_contain_text("expires ")
        self.assert_no_unexpected_errors(bp)


class TestAuditPage(UITestCase):
    """An Owner sees who drove what and how much, never what they typed."""

    @classmethod
    def seed(cls):
        cls.add_user("op", "operator")
        cls.box = cls.add_target("Jump Box", group="Lab")
        cls.wait_input_ready(cls.box["id"])

    def test_the_owner_sees_sessions_and_counts_but_not_the_typing(self):
        bp = self.open("op")
        p = bp.page
        p.goto(f"{self.base}/#/targets/{self.box['id']}")
        self.addCleanup(self.owner.post, f"/api/targets/{self.box['id']}/control/release")
        p.click("#ctrl-btn")
        expect(p.locator("#kvm-viewport")).to_be_focused()
        for key in ("Shift+KeyH", "KeyI", "Enter"):
            p.keyboard.press(key)
        chip = self.box["chip"]
        wait_until(lambda: len(key_frames(chip)) >= 6, what="the typing to reach the target")
        p.click("#ctrl-btn")
        expect(p.locator("#ctrl-btn")).to_have_text("Take control")

        owner = self.open()
        o = owner.page
        owner.sign_in(*OWNER)
        o.goto(f"{self.base}/#/admin")
        o.click("#atab-audit")
        events = o.locator("#audit-events-body")
        expect(events).to_contain_text("Took control")
        expect(events).to_contain_text("Stopped driving: released control")
        o.select_option("#audit-kind", "auth.")
        expect(events.locator("tr").first).to_contain_text("Signed in")
        expect(events).not_to_contain_text("Took control")

        row = o.locator("#audit-sessions-body tr", has_text="Jump Box")
        expect(row).to_contain_text("released control")
        cells = row.locator("td")
        expect(cells.nth(4)).to_have_text("4")          # Shift, H, I, Enter
        # No way to see what was typed: no button, no keys on the page.
        expect(row.locator("button")).to_have_count(0)
        self.assertNotIn("KeyH", o.content())
        self.assert_no_unexpected_errors(owner)

    def test_only_owners_have_the_audit_tab(self):
        bp = self.open("op")
        bp.page.goto(f"{self.base}/#/admin")
        expect(bp.page.locator("#page-admin")).to_be_hidden()
