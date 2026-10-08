// ─── Input control ────────────────────────────────────────────────────────────
// Everything here acts on the target being viewed (see kvm.js's targetApi()).
// Mouse is absolute-only: the position sent to the target is always the
// cursor's normalized [0,1] location over the video element, computed from
// its bounding rect. There is no relative/pointer-lock mode to fall back to.
let isDriver = false;
let pendingX = null, pendingY = null, rafId = null;

// Keys currently held down, so we can force-release all of them if we ever
// stop reliably seeing their keyup (window loses focus mid-press).
const pressedKeys = new Set();
function releaseAllKeys() {
  for (const code of pressedKeys) sendInput({type:'key', code, pressed:false});
  pressedKeys.clear();
}

// Input rides a persistent WebSocket (opened once control is acquired)
// instead of one HTTP request per event — cuts the per-mousemove/key cost
// down to a single frame write on an already-open connection. sendInput()
// falls back to a per-event POST whenever the socket isn't open yet
// (the brief connect race right after acquiring control) or can't be
// opened at all (e.g. a reverse proxy that doesn't pass WS upgrades
// through), so input keeps working either way.
let inputWs = null;

function openInputWs() {
  if (inputWs && (inputWs.readyState === WebSocket.OPEN || inputWs.readyState === WebSocket.CONNECTING)) return;
  const proto = location.protocol === 'https:' ? 'wss://' : 'ws://';
  inputWs = new WebSocket(proto + location.host + targetApi('/input/ws'));
  inputWs.onmessage = ev => {
    let msg;
    try { msg = JSON.parse(ev.data); } catch { return; }
    if (msg.ok === false && msg.reason === 'not_driver') {
      isDriver = false;
      updateControlUI();
      closeInputWs();
    } else if (msg.ok === false) {
      // Any other rejection (queue full, backend unavailable, bad
      // payload) means a dropped click or key. Surface it so it's at
      // least visible in devtools.
      console.warn('HoustonKVM: input event rejected:', msg.reason);
    }
  };
  inputWs.onclose = inputWs.onerror = () => { inputWs = null; };
}

function closeInputWs() {
  if (inputWs) { inputWs.onclose = null; inputWs.close(); inputWs = null; }
}

function updateControlUI() {
  const btn = document.getElementById('ctrl-btn');
  btn.textContent = isDriver ? 'Release control' : 'Take control';
  btn.className   = 'btn toolbar-btn' + (isDriver ? ' btn-accent' : '');
  setShown(document.getElementById('ctrl-indicator'), isDriver);
  setShown(document.getElementById('special-key-select'), isDriver);
  updatePasteAvailability();
  kvmViewport.classList.toggle('kvm-viewport--driving', isDriver);
  if (isDriver) setCtrlHint('Keyboard and mouse go to this machine. Click the screen if keys don’t register.');
  else updateControlAvailability();
}

// One inline line beside the controls, rather than a blocking alert().
function setCtrlHint(text, isError) {
  const hint = document.getElementById('ctrl-hint');
  hint.textContent = text || '';
  hint.classList.toggle('hint-inline--error', !!isError);
}

function sendInput(data) {
  if (inputWs && inputWs.readyState === WebSocket.OPEN) {
    inputWs.send(JSON.stringify(data));
    return;
  }
  sendInputHttp(data);
}

function sendInputHttp(data) {
  fetch(targetApi('/input'), {
    method:'POST', credentials:'include',
    headers:{'Content-Type':'application/json'},
    body: JSON.stringify(data)
  }).then(r => {
    if (r.status === 403 || r.status === 401) { isDriver = false; updateControlUI(); }
    else if (r.status === 503) { console.warn('HoustonKVM: input injection unavailable'); }
  }).catch(() => {});
}

async function acquireControl() {
  const id = currentTargetId;
  const r = await fetch(targetApi('/control/acquire'), {method:'POST', credentials:'include'});
  if (currentTargetId !== id) { // navigated away mid-request: don't keep a lock we can't see
    if (r.ok) fetch('/api/targets/' + id + '/control/release', {method:'POST', credentials:'include'});
    return;
  }
  if (r.ok) {
    isDriver = true;
    updateControlUI();
    openInputWs();
    kvmViewport.focus({preventScroll: true});
  } else if (r.status === 409) {
    setCtrlHint('Someone else is controlling this target right now.', true);
  } else if (r.status === 503) {
    setCtrlHint('Keyboard and mouse aren’t available for this target — check its input connection.', true);
  } else {
    setCtrlHint('Could not take control: ' + ((await r.text()) || r.status), true);
  }
}

async function releaseControl() {
  const id = currentTargetId;
  isDriver = false;
  updateControlUI();
  closeInputWs();
  // The server releases every key and button we had down when control ends,
  // so there's nothing to send — just forget our local record of held keys.
  pressedKeys.clear();
  await fetch('/api/targets/' + id + '/control/release', {method:'POST', credentials:'include'});
}

document.getElementById('ctrl-btn').addEventListener('click', () => {
  if (isDriver) releaseControl(); else acquireControl();
});

// If the tab/window loses focus entirely (alt-tab, switching apps)
// mid-keypress, release everything so a key doesn't stay stuck on the
// target for the rest of the session.
window.addEventListener('blur', () => { if (isDriver) releaseAllKeys(); });

// activeVideoEl's intrinsic (source) pixel size — the video's videoWidth/
// videoHeight, or the MJPEG img's naturalWidth/naturalHeight.
function intrinsicSize(el) {
  return el === kvmVideo
    ? {w: el.videoWidth,  h: el.videoHeight}
    : {w: el.naturalWidth, h: el.naturalHeight};
}

// The element is styled with object-fit:contain (see css/pages.css), so
// whenever its rendered box's aspect ratio doesn't exactly match the
// stream's intrinsic aspect ratio, the picture is letter/pillarboxed
// *inside* that box rather than filling it — getBoundingClientRect() alone
// then overstates the clickable picture and every coordinate is off by the
// size of the bars. Compute the actual centered picture sub-rect instead.
function visiblePictureRect(el) {
  const rect = el.getBoundingClientRect();
  const {w: iw, h: ih} = intrinsicSize(el);
  if (!iw || !ih || rect.width <= 0 || rect.height <= 0) return rect;

  const rectRatio = rect.width / rect.height;
  const picRatio  = iw / ih;
  let {left, top, width, height} = rect;
  if (rectRatio > picRatio) {        // rect wider than picture -> pillarboxed
    width = rect.height * picRatio;
    left  = rect.left + (rect.width - width) / 2;
  } else if (rectRatio < picRatio) { // rect taller than picture -> letterboxed
    height = rect.width / picRatio;
    top    = rect.top + (rect.height - height) / 2;
  }
  return {left, top, width, height};
}

// Normalizes a client-space point to [0,1] over activeVideoEl's visible
// picture (not its full box — see visiblePictureRect()).
function normalizedPos(e) {
  const rect = visiblePictureRect(activeVideoEl);
  if (rect.width <= 0 || rect.height <= 0) return null;
  const x = (e.clientX - rect.left) / rect.width;
  const y = (e.clientY - rect.top) / rect.height;
  return {x: Math.min(1, Math.max(0, x)), y: Math.min(1, Math.max(0, y))};
}

document.addEventListener('mousemove', e => {
  if (!isDriver || e.target !== activeVideoEl) return;
  const pos = normalizedPos(e);
  if (!pos) return;
  pendingX = pos.x; pendingY = pos.y;
  if (!rafId) rafId = requestAnimationFrame(() => {
    if (pendingX !== null) sendInput({type:'mousemove', x:pendingX, y:pendingY});
    pendingX = null; pendingY = null; rafId = null;
  });
});

document.addEventListener('mousedown', e => {
  if (!isDriver || e.target !== activeVideoEl) return;
  e.preventDefault();
  sendInput({type:'mousebutton', button:e.button, pressed:true});
});

document.addEventListener('mouseup', e => {
  if (!isDriver || e.target !== activeVideoEl) return;
  sendInput({type:'mousebutton', button:e.button, pressed:false});
});

document.addEventListener('wheel', e => {
  if (!isDriver || e.target !== activeVideoEl) return;
  e.preventDefault();
  sendInput({type:'mousescroll', delta: e.deltaY > 0 ? -1 : 1});
}, {passive: false});

// Keys typed into a text field or dropdown are for that control, not for the
// target — the page has form controls on several screens, so capturing
// "everything" would type your search into the remote machine.
function isFormControl(node) {
  return !!node && (node.tagName === 'INPUT' || node.tagName === 'TEXTAREA' ||
                    node.tagName === 'SELECT' || node.isContentEditable);
}

document.addEventListener('keydown', e => {
  if (!isDriver || isFormControl(e.target)) return;
  e.preventDefault();
  pressedKeys.add(e.code);
  sendInput({type:'key', code:e.code, pressed:true});
});

document.addEventListener('keyup', e => {
  if (!isDriver || isFormControl(e.target)) return;
  e.preventDefault();
  pressedKeys.delete(e.code);
  sendInput({type:'key', code:e.code, pressed:false});
});

// Ctrl+Alt+Del is the same story as F11/F12: on Windows it's trapped by
// the OS itself, not just the browser, so it can never reach a target
// through a real browser keypress no matter what the page does — the only
// way to deliver it is to send the HID codes directly, same as the F-keys.
document.getElementById('special-key-select').addEventListener('change', e => {
  const val = e.target.value;
  e.target.value = '';
  kvmViewport.focus({preventScroll: true}); // so the next keystroke goes to the target
  if (!isDriver || !val) return;
  if (val === '__ctrlaltdel') {
    for (const code of ['ControlLeft', 'AltLeft', 'Delete']) sendInput({type:'key', code, pressed:true});
    for (const code of ['Delete', 'AltLeft', 'ControlLeft']) sendInput({type:'key', code, pressed:false});
    return;
  }
  sendInput({type:'key', code:val, pressed:true});
  sendInput({type:'key', code:val, pressed:false});
});

kvmImg.addEventListener('contextmenu', e => e.preventDefault());
kvmVideo.addEventListener('contextmenu', e => e.preventDefault());

// Sync driver state every 5 s in case another session steals or server
// restarts, and self-heal the input WebSocket the same way: if we're still
// the driver but the socket dropped (network blip, server restart) or
// never finished connecting, just reopen it — openInputWs() is a no-op if
// one's already open/connecting.
setInterval(async () => {
  if (!isSignedIn() || currentTargetId === null || document.hidden) return;
  const id = currentTargetId;
  try {
    const r = await fetch(targetApi('/control/status'), {credentials:'include'});
    if (!r.ok || currentTargetId !== id) return;
    const d = await r.json();
    if (isDriver && !d.is_driver) { isDriver = false; updateControlUI(); closeInputWs(); }
    else if (isDriver) { openInputWs(); }
  } catch(e) {}
}, 5000);
