// ─── One target's screen ───────────────────────────────────────────────────
// Opening #/targets/<id> lands here. Everything the page talks to is scoped
// to that target (/api/targets/<id>/...); leaving the page — for another
// target, the directory, or sign-out — releases control and closes the
// streams, so you never sit holding a lock on a machine you're not looking at.

const kvmImg      = document.getElementById('kvm');
const kvmVideo    = document.getElementById('kvm-webrtc');
const kvmViewport = document.getElementById('kvm-viewport');
kvmViewport.tabIndex = -1; // focusable by script: keystrokes go to the screen, not a stray control

// Whichever element pointer input targets right now — swaps between kvmImg
// and kvmVideo when WebRTC (low-latency) mode is toggled.
let activeVideoEl = kvmImg;

let currentTarget = null;     // the target JSON from /api/targets/<id>
let currentTargetId = null;
let targetStatusTimer = null;
const TARGET_STATUS_MS = 5000;

// URL of an endpoint for the target being viewed, e.g. targetApi('/stream').
function targetApi(path) {
  return '/api/targets/' + currentTargetId + (path || '');
}

async function openTargetView(id) {
  // Same target again (e.g. the hash was re-set): nothing to rebuild.
  if (currentTargetId === id && currentTarget) return;

  currentTargetId = id;
  currentTarget = null;
  isDriver = false;
  updateControlUI();
  setKvmTitle('Loading…', null);
  setVideoMessage('');
  setShown(kvmImg, true);

  const r = await send('GET', '/api/targets/' + id);
  if (currentTargetId !== id) return; // navigated away while loading
  if (!r.ok) {
    setKvmTitle(r.status === 404 ? 'Target not found' : 'Could not load target', null);
    setVideoMessage(r.status === 404
      ? 'This target doesn’t exist any more. Pick another from the Targets page.'
      : 'Could not load this target: ' + r.text);
    kvmImg.removeAttribute('src');
    currentTargetId = null;
    return;
  }
  currentTarget = r.data;
  renderTargetHeader();

  updateControlAvailability();

  if (!currentTarget.enabled) {
    kvmImg.removeAttribute('src');
    setVideoMessage('This target is disabled. An administrator can enable it on the Admin page.');
  } else {
    kvmImg.src = targetApi('/stream');
    startVideoHealthCheck();
    if (userPrefs.default_stream_mode === 'webrtc') startWebrtc();
  }
  startTargetStatusPolling();
  document.getElementById('kvm-title').focus({preventScroll: true});
}

// Leaves the target view: releases control, stops WebRTC and the MJPEG
// connection, and stops all polling. Safe to call when nothing is open.
async function closeTargetView() {
  if (currentTargetId === null) return;
  if (isDriver) await releaseControl();
  stopWebrtc();
  stopVideoHealthCheck();
  stopTargetStatusPolling();
  kvmImg.removeAttribute('src'); // drops the long-lived /stream connection
  closeDisplayPanel();
  closeHealthPanel();
  closePastePanel();
  currentTarget = null;
  currentTargetId = null;
}

// Whether — and if not, why not — this person can take control right now.
// Exactly 'operator': an Owner does not auto-qualify as a driver (see
// http_common.h's requireExactRole()), so an Owner who wants to pilot a
// target needs a dedicated Operator account. Say so up front instead of
// hiding the button with no explanation, or letting someone click it only to
// learn the target has no keyboard/mouse connection at all.
function updateControlAvailability() {
  const t = currentTarget;
  if (!t) return;
  const isOperator = currentRole === 'operator';
  const btn = document.getElementById('ctrl-btn');
  setShown(btn, isOperator && t.enabled);
  const noInput = t.enabled && !t.status.input_ready && !isDriver;
  btn.disabled = isOperator && noInput;
  if (isDriver) return; // updateControlUI() owns the hint while driving
  if (!isOperator) setCtrlHint('View only — controlling a target needs an Operator account');
  else if (noInput) setCtrlHint('View only — the keyboard/mouse link is down. Input health says why.');
  else setCtrlHint('');
}

function setKvmTitle(name, groupName) {
  document.getElementById('kvm-title').textContent = name;
  document.getElementById('crumb-name').textContent = name;
  setPageTitle(name);
  const groupItem = document.getElementById('crumb-group-item');
  setShown(groupItem, !!groupName);
  document.getElementById('crumb-group').textContent = groupName || '';
}

// Header: name, group breadcrumb, and the same status tags the cards show.
function renderTargetHeader() {
  const t = currentTarget;
  setKvmTitle(t.name, t.group);
  const status = document.getElementById('kvm-status');
  clearChildren(status);
  const tags = statusTags(t);
  status.className = 'status-tags';
  for (const tag of tags) status.append(statusTagNode(tag));
  if (t.description) document.getElementById('kvm-title').title = t.description;
  updateDisplayAvailability();
  updateHealthAvailability();
  updateSoundButton();
}

document.getElementById('crumb-group').addEventListener('click', () => {
  // The group crumb leads to the directory already filtered to that group.
  if (currentTarget) { targetFilters.group = currentTarget.group; targetFilters.q = ''; targetFilters.tag = ''; }
});

function setVideoMessage(text) {
  const box = document.getElementById('video-status');
  box.textContent = text;
  setShown(box, !!text);
}

// Keeps the header's tags honest while you watch (someone else takes
// control, the signal drops, an admin disables the target).
function startTargetStatusPolling() {
  stopTargetStatusPolling();
  const id = currentTargetId;
  targetStatusTimer = setInterval(async () => {
    if (currentTargetId !== id || document.hidden) return;
    const r = await send('GET', '/api/targets/' + id);
    if (currentTargetId !== id || !r.ok) return;
    const wasEnabled = currentTarget && currentTarget.enabled;
    currentTarget = r.data;
    renderTargetHeader();
    updateControlAvailability();
    if (wasEnabled && !currentTarget.enabled) {
      // Disabled under us: the server already dropped our control.
      isDriver = false; updateControlUI(); closeInputWs();
      kvmImg.removeAttribute('src');
      setVideoMessage('This target was just disabled by an administrator.');
    }
  }, TARGET_STATUS_MS);
}

function stopTargetStatusPolling() {
  if (targetStatusTimer) clearInterval(targetStatusTimer);
  targetStatusTimer = null;
}

// ─── Video health ───────────────────────────────────────────────────────────
// /stream never errors or completes: with no capture it just sits open with
// no frames, so the <img> has no load or error event to report. Poll
// /snapshot instead (503 with no capture) to show the real state, and
// reconnect the stream the moment capture is back; some browsers never pick
// up frames on a stream opened while capture was down.
let videoHealthTimer = null;
let videoAvailable = null; // null = not checked yet

async function checkVideoHealth() {
  const id = currentTargetId;
  if (id === null || activeVideoEl !== kvmImg) return; // WebRTC mode has its own path
  let available = false;
  try {
    const r = await fetch(targetApi('/snapshot'), {credentials: 'include'});
    available = r.status === 200;
  } catch (e) { available = false; }
  if (currentTargetId !== id) return; // switched targets while waiting

  if (available && videoAvailable === false) {
    kvmImg.src = targetApi('/stream') + '?r=' + Date.now();
  }
  videoAvailable = available;
  setVideoMessage(available ? '' : 'No video — waiting for the capture device…');

  // Poll fast (5s) while down, so recovery is picked up quickly; back off
  // to 20s once healthy — this hits /snapshot (a real JPEG download), no
  // need to pay that cost often while the live /stream is already doing the
  // real work.
  videoHealthTimer = setTimeout(checkVideoHealth, available ? 20000 : 5000);
}

function startVideoHealthCheck() {
  videoAvailable = null;
  if (videoHealthTimer) clearTimeout(videoHealthTimer);
  checkVideoHealth();
}

function stopVideoHealthCheck() {
  if (videoHealthTimer) clearTimeout(videoHealthTimer);
  videoHealthTimer = null;
  setVideoMessage('');
}
