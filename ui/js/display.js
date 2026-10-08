// ─── Display panel: one target's resolution and refresh rate ───────────────
// Lives on the target's own page, next to the picture it changes, so there's
// no "which target?" question to answer. Operators and Owners only — the
// server enforces that independently on /video/settings, and a Viewer would
// just get a 403, so the button isn't shown to them.
//
// Changing the mode rebuilds that target's capture pipeline (up to ~11 s) for
// every viewer, hence an explicit Apply rather than applying on selection.

const displayBtn   = document.getElementById('display-btn');
const displayPanel = document.getElementById('display-panel');

const videoModePicker = createModePicker({
  res: 'vid-resolution', fpsSel: 'vid-fps-select', fpsField: 'vid-fps-field',
  customRow: 'vid-custom-row', width: 'vid-width', height: 'vid-height',
  fps: 'vid-fps', capsErr: 'vid-caps-err'
});
let displayReloadTimer = null;

function canTuneVideo() { return currentRole === 'operator' || currentRole === 'owner'; }

// Called whenever the target's state is (re)loaded: the button is there only
// while there's a running capture to tune.
function updateDisplayAvailability() {
  const on = !!currentTarget && currentTarget.enabled && canTuneVideo();
  setShown(displayBtn, on);
  if (!on) closeDisplayPanel();
}

function closeDisplayPanel() {
  clearTimeout(displayReloadTimer);
  setShown(displayPanel, false);
  displayBtn.setAttribute('aria-expanded', 'false');
}

function renderActiveLine(active) {
  document.getElementById('vid-active').textContent = active
    ? `Currently streaming ${active.width} × ${active.height} at ${active.fps} Hz.`
    : 'Not streaming right now — the capture device may be unplugged or have no signal.';
}

// Apply stays disabled until the current settings and the device's modes have
// arrived, so it can never send a half-loaded form.
async function loadVideoSettings() {
  const id = currentTargetId;
  if (id === null) return;
  document.getElementById('vid-apply').disabled = true;
  const r = await send('GET', `/api/targets/${id}/video/settings`);
  if (currentTargetId !== id) return; // navigated away while loading
  if (!r.ok) { setMsg('vid-msg', 'Could not load display settings: ' + r.text, 'error'); return; }
  const s = r.data;
  document.getElementById('vid-bitrate').value = s.webrtc_bitrate_kbps;
  renderActiveLine(s.active);
  await videoModePicker.load(`/api/targets/${id}/video/capabilities`,
                             s.capture_width, s.capture_height, s.capture_fps);
  if (currentTargetId === id) document.getElementById('vid-apply').disabled = false;
}

displayBtn.addEventListener('click', () => {
  const open = displayPanel.hidden;
  setShown(displayPanel, open);
  displayBtn.setAttribute('aria-expanded', String(open));
  if (open) { setMsg('vid-msg', ''); loadVideoSettings(); }
});

document.getElementById('form-video').addEventListener('submit', async e => {
  e.preventDefault();
  const id = currentTargetId;
  if (id === null) return;
  setMsg('vid-msg', '');
  const mode = videoModePicker.read();
  const r = await send('PUT', `/api/targets/${id}/video/settings`, {
    capture_width: mode.width, capture_height: mode.height, capture_fps: mode.fps,
    webrtc_bitrate_kbps: parseInt(document.getElementById('vid-bitrate').value, 10)
  });
  if (currentTargetId !== id) return;
  if (r.ok) {
    setMsg('vid-msg', 'Applied. The picture will pause for a few seconds while it restarts…', 'success');
    // The rebuild runs on the target's worker thread and can take up to ~11s —
    // refresh once it should be done so "Currently streaming" reflects what
    // actually got negotiated, not the still-tearing-down previous mode.
    clearTimeout(displayReloadTimer);
    displayReloadTimer = setTimeout(() => {
      if (currentTargetId === id && !displayPanel.hidden) loadVideoSettings();
    }, 12000);
  } else {
    setMsg('vid-msg', r.text || 'Could not change the display settings', 'error');
  }
});
