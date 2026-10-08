// ─── Small shared widgets: tabs and the resolution/refresh-rate picker ─────

// An accessible tab list (WAI-ARIA tabs pattern): one tab is in the tab
// order, arrow keys/Home/End move between tabs, hidden tabs are skipped.
// Tabs are #<tabPrefix><name>, their panels #<panelPrefix><name>.
function initTabs(tablistId, names, tabPrefix, panelPrefix, onShow) {
  const tablist = document.getElementById(tablistId);
  const tab = n => document.getElementById(tabPrefix + n);
  const panel = n => document.getElementById(panelPrefix + n);
  let current = names[0];

  function show(name, focus) {
    current = name;
    for (const n of names) {
      const on = n === name;
      tab(n).setAttribute('aria-selected', String(on));
      tab(n).tabIndex = on ? 0 : -1;
      setShown(panel(n), on);
    }
    if (focus) tab(name).focus();
    if (onShow) onShow(name);
  }

  for (const n of names) tab(n).addEventListener('click', () => show(n));
  tablist.addEventListener('keydown', e => {
    const visible = names.filter(n => !tab(n).hidden);
    const i = visible.indexOf(current);
    let next = null;
    if (e.key === 'ArrowRight') next = visible[(i + 1) % visible.length];
    else if (e.key === 'ArrowLeft') next = visible[(i - 1 + visible.length) % visible.length];
    else if (e.key === 'Home') next = visible[0];
    else if (e.key === 'End') next = visible[visible.length - 1];
    if (next) { e.preventDefault(); show(next, true); }
  });
  return {show, current: () => current};
}

// Resolution / refresh-rate picker, laid out like a Mac's display settings:
// a Resolution dropdown and a Refresh rate dropdown, nothing to type. The
// choices are the modes the capture device itself reported
// (VIDIOC_ENUM_FRAMESIZES/FRAMEINTERVALS via /video/capabilities), so it only
// ever offers what the hardware will accept. Some devices don't report
// discrete modes; those get a list of common resolutions instead, and typed
// numbers survive only as a last-resort "Custom…" entry in that case.
//
// `ids` maps roles to element ids: res, fpsSel, fpsField, customRow, width,
// height, fps, capsErr.
const CUSTOM_MODE = 'custom';

// Offered when the device can't say what it supports. Largest first.
const COMMON_RATES = [60, 50, 30, 25, 24, 15];
const COMMON_MODES = [
  [1920, 1080], [1680, 1050], [1600, 900], [1440, 900], [1366, 768],
  [1280, 1024], [1280, 800], [1280, 720], [1024, 768], [800, 600], [640, 480]
].map(([width, height]) => ({width, height, fps: COMMON_RATES}));

function createModePicker(ids) {
  const $ = k => document.getElementById(ids[k]);
  const key = (w, h) => `${w}x${h}`;
  let modes = [];        // what the res dropdown offers: [{width, height, fps: [..]}]
  let reported = false;  // whether the device itself said so

  function fillFps(mode, wanted) {
    const sel = $('fpsSel');
    clearChildren(sel);
    const list = (mode && mode.fps.length) ? [...mode.fps].sort((a, b) => b - a) : [wanted];
    for (const f of list) sel.append(el('option', {value: f}, `${f} Hz`));
    // Prefer an exact match; otherwise the closest available rate, since the
    // saved fps may predate a device swap and no longer be offered.
    sel.value = list.includes(wanted)
      ? wanted
      : list.reduce((a, b) => Math.abs(b - wanted) < Math.abs(a - wanted) ? b : a, list[0]);
  }

  function setCustom(on) {
    setShown($('customRow'), on);
    setShown($('fpsField'), !on);
  }

  // capsUrl: where to ask what the device supports. w/h/fps: the saved mode
  // to preselect.
  async function load(capsUrl, w, h, fps) {
    $('capsErr').textContent = '';
    $('width').value = w; $('height').value = h; $('fps').value = fps;

    let caps = {modes: [], error: ''};
    try { caps = await getJson(capsUrl); } catch (e) { /* falls back to the common list */ }
    reported = !!(caps.modes && caps.modes.length);
    modes = [...(reported ? caps.modes : COMMON_MODES)]
      .sort((a, b) => (b.width - a.width) || (b.height - a.height));

    // The saved mode always appears, so opening this page never silently
    // changes what's configured — even if the device no longer offers it.
    const savedKey = key(w, h);
    let savedListed = modes.some(m => key(m.width, m.height) === savedKey);
    const sel = $('res');
    clearChildren(sel);
    for (const m of modes) sel.append(el('option', {value: key(m.width, m.height)}, `${m.width} × ${m.height}`));
    if (!savedListed && w > 0 && h > 0) {
      sel.append(el('option', {value: savedKey}, `${w} × ${h}` + (reported ? ' (not offered by this device)' : '')));
      modes.push({width: w, height: h, fps: []});
      savedListed = true;
    }
    if (!reported) {
      sel.append(el('option', {value: CUSTOM_MODE}, 'Custom…'));
      $('capsErr').textContent = 'This capture device didn’t report its supported modes, so common ones are listed.' +
        (caps.error ? ` (${caps.error})` : '');
    }

    setCustom(false);
    if (savedListed) {
      sel.value = savedKey;
      fillFps(modes.find(m => key(m.width, m.height) === savedKey), fps);
    } else {
      sel.value = key(modes[0].width, modes[0].height);
      fillFps(modes[0], fps);
    }
  }

  $('res').addEventListener('change', () => {
    const v = $('res').value;
    if (v === CUSTOM_MODE) { setCustom(true); return; }
    setCustom(false);
    const [w, h] = v.split('x').map(Number);
    fillFps(modes.find(m => key(m.width, m.height) === key(w, h)), Number($('fpsSel').value) || 0);
  });

  // The chosen mode: {width, height, fps}.
  function read() {
    const v = $('res').value;
    if (v === CUSTOM_MODE) {
      return {width: parseInt($('width').value, 10), height: parseInt($('height').value, 10),
              fps: parseInt($('fps').value, 10)};
    }
    const [w, h] = v.split('x').map(Number);
    return {width: w, height: h, fps: parseInt($('fpsSel').value, 10)};
  }

  return {load, read};
}
