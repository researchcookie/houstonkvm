// ─── Owner-only Admin page: targets, users, network and audit ──────────────
// The whole "Admin" nav item is hidden for non-owners (see auth.js's
// showApp()) and router.js bounces them off #/admin; the server independently
// enforces Role::Owner on every route these call, so hiding is UX only.

const adminTabs = initTabs('admin-tabs', ['targets', 'users', 'network', 'audit'], 'atab-', 'apanel-',
                          name => {
                            if (name === 'audit') openAuditPanel();
                            if (name === 'network') { loadHttps(); loadIceServers(); }
                          });

// Set by the "Add a target" button on an empty Targets page so the editor is
// already open when the Admin page appears.
let adminWantsNewTarget = false;

function openAdminPage() {
  adminTabs.show('targets');
  showTargetEditor(false);
  setMsg('admin-targets-msg', '');
  loadAdminTargets().then(() => {
    if (adminWantsNewTarget) { adminWantsNewTarget = false; openTargetEditor(null); }
  });
  loadUsers();
  loadHttps();   // for the banner, whichever tab is showing
}

// ── Targets: the list ────────────────────────────────────────────────────
let adminTargets = [];   // last loaded list, with each target's config
let editingId = null;    // id being edited, or null when adding

async function loadAdminTargets() {
  setAlert('admin-targets-err-box', 'admin-targets-err', '');
  let list;
  try {
    list = await fetchTargets();
  } catch (e) {
    setAlert('admin-targets-err-box', 'admin-targets-err', 'Could not load targets: ' + e.message);
    return;
  }
  adminTargets = list;
  const body = document.getElementById('admin-targets-body');
  clearChildren(body);
  setShown(document.getElementById('admin-targets-empty'), list.length === 0);
  setShown(document.querySelector('#apanel-targets .table-wrap'), list.length > 0);
  for (const t of list) body.append(adminRow(t));
}

function rowButton(label, target, onclick, danger) {
  return el('button', {
    type: 'button',
    class: 'link-btn' + (danger ? ' link-danger' : ''),
    'aria-label': `${label} ${target.name}`,
    onclick
  }, label);
}

function adminRow(t) {
  const states = statusTags(t);
  if (t.default) states.unshift({text: 'Default', kind: 'default'});
  return el('tr', {},
    el('th', {scope: 'row'},
      el('a', {href: targetHref(t.id)}, t.name),
      t.description ? el('div', {class: 'row-meta'}, t.description) : null),
    el('td', {}, t.group || '—'),
    el('td', {}, t.tags.length
      ? el('ul', {class: 'tag-list'}, t.tags.map(n => el('li', {}, el('span', {class: 'tag'}, n))))
      : '—'),
    el('td', {}, el('div', {class: 'card-status'}, states.map(statusTagNode))),
    el('td', {}, el('div', {class: 'row-actions'},
      rowButton('Edit', t, () => openTargetEditor(t)),
      rowButton(t.enabled ? 'Disable' : 'Enable', t, () => updateTarget(t, {enabled: !t.enabled},
                t.enabled ? `${t.name} disabled.` : `${t.name} enabled.`)),
      t.default ? null : rowButton('Make default', t, () => makeDefault(t)),
      rowButton('Delete', t, () => deleteTarget(t), true))));
}

async function updateTarget(t, fields, okMessage) {
  setAlert('admin-targets-err-box', 'admin-targets-err', '');
  const r = await send('PUT', '/api/targets/' + t.id, fields);
  if (!r.ok) {
    setAlert('admin-targets-err-box', 'admin-targets-err', r.text || 'Could not update target');
    return false;
  }
  setMsg('admin-targets-msg', okMessage, 'success');
  await loadAdminTargets();
  return true;
}

function makeDefault(t) {
  if (!confirm(`Make "${t.name}" the default target?\n\n` +
               'Automated clients that don’t name a target will start viewing and controlling it.')) return;
  updateTarget(t, {default: true}, `${t.name} is now the default target.`);
}

async function deleteTarget(t) {
  const warn = t.default
    ? '\n\nThis is the default target. Until you choose another, automated clients that don’t name a target will fail.'
    : '';
  if (!confirm(`Delete "${t.name}"?\n\nIt stops capturing and its settings are removed. This can’t be undone.${warn}`)) return;
  setAlert('admin-targets-err-box', 'admin-targets-err', '');
  const r = await send('DELETE', '/api/targets/' + t.id);
  if (!r.ok) {
    setAlert('admin-targets-err-box', 'admin-targets-err', r.text || 'Could not delete target');
    return;
  }
  setMsg('admin-targets-msg', `${t.name} deleted.`, 'success');
  await loadAdminTargets();
}

document.getElementById('target-add-btn').addEventListener('click', () => openTargetEditor(null));

// ── Targets: the editor ──────────────────────────────────────────────────
// Every device picker only ever offers what /api/admin/devices reports is
// actually plugged in, with an "Other…" fallback for the rare case where the
// right one isn't detected — the point of a KVM is to be easier to set up
// than fighting device paths. A device already wired to another target is
// shown but not selectable, and says which target has it.
const STANDARD_BAUDS = [1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600];
let editorFacets = {groups: [], tags: []};

const targetModePicker = createModePicker({
  res: 't-resolution', fpsSel: 't-fps-select', fpsField: 't-fps-field',
  customRow: 't-custom-row', width: 't-width', height: 't-height',
  fps: 't-fps', capsErr: 't-caps-err'
});

function showTargetEditor(on) {
  setShown(document.getElementById('admin-targets-list'), !on);
  setShown(document.getElementById('form-target'), on);
}

// Fills a <select> from detected devices (+ "Other…"), returns whether the
// wanted value was one of them.
function fillDeviceSelect(sel, devices, wanted, describe) {
  clearChildren(sel);
  for (const d of devices) {
    const other = d.in_use_by && d.in_use_by.id !== editingId;
    sel.append(el('option', {value: d.path, disabled: other},
      describe(d) + (other ? ` — in use by ${d.in_use_by.name}` : '')));
  }
  sel.append(el('option', {value: CUSTOM_MODE}, 'Other…'));
  const known = devices.some(d => d.path === wanted);
  sel.value = known ? wanted : CUSTOM_MODE;
  return known;
}

// The value the owner actually means: the select, or the typed path when the
// select says "Other…".
function deviceValue(selectId, customId) {
  const sel = document.getElementById(selectId);
  return sel.value === CUSTOM_MODE ? document.getElementById(customId).value.trim() : sel.value;
}

function wireCustomDevice(selectId, rowId, inputId, onChange) {
  const sel = document.getElementById(selectId);
  sel.addEventListener('change', () => {
    setShown(document.getElementById(rowId), sel.value === CUSTOM_MODE);
    if (onChange) onChange();
  });
  document.getElementById(inputId).addEventListener('change', () => { if (onChange) onChange(); });
}

function reloadTargetModes() {
  const path = deviceValue('t-video', 't-video-custom');
  if (!path) return;
  const cur = targetModePicker.read();
  targetModePicker.load('/api/video/capabilities?device=' + encodeURIComponent(path),
                        cur.width || 1280, cur.height || 720, cur.fps || 30);
}
wireCustomDevice('t-video', 't-video-custom-row', 't-video-custom', reloadTargetModes);
wireCustomDevice('t-serial', 't-serial-custom-row', 't-serial-custom');

// Sound: "auto" (the capture dongle's own sound card, the default), none,
// or a particular sound card. A setting naming a card that isn't plugged in
// right now is kept, not silently swapped for another.
function fillAudioSelect(devices, wanted) {
  const sel = document.getElementById('t-audio');
  clearChildren(sel);
  sel.append(el('option', {value: 'auto'}, 'The capture device’s own sound (automatic)'));
  sel.append(el('option', {value: ''}, 'No sound'));
  for (const d of devices.audio_devices || []) {
    const other = d.in_use_by && d.in_use_by.id !== editingId;
    sel.append(el('option', {value: d.device, disabled: other},
      d.name + (other ? ` — in use by ${d.in_use_by.name}` : '')));
  }
  if (![...sel.options].some(o => o.value === wanted))
    sel.append(el('option', {value: wanted}, `${wanted} (not plugged in)`));
  sel.value = wanted;
  const hint = document.getElementById('t-audio-hint');
  if (devices.audio_supported === false)
    hint.textContent = 'This server was built without sound support, so nothing will be heard.';
}

function selectedInputMethod() {
  return document.querySelector('input[name="t-input"]:checked').value;
}

function syncInputMethodFields() {
  const m = selectedInputMethod();
  setShown(document.getElementById('t-serial-fields'), m === 'serial');
  setShown(document.getElementById('t-qemu-fields'), m === 'qemu');
  setShown(document.getElementById('t-gadget-note'), m === 'gadget');
}
for (const id of ['t-in-serial', 't-in-qemu', 't-in-gadget'])
  document.getElementById(id).addEventListener('change', syncInputMethodFields);

// Group: snap to an existing group's spelling as you leave the field, the
// same rule the server applies — so "rack 3" never becomes a second group.
// Also re-applied when the group list finishes loading, for someone who typed
// faster than it arrived.
function snapGroupSpelling() {
  const input = document.getElementById('t-group');
  const typed = input.value.trim().toLowerCase();
  const match = editorFacets.groups.find(g => g.name.toLowerCase() === typed);
  if (match) input.value = match.name;
}
document.getElementById('t-group').addEventListener('change', snapGroupSpelling);

function parseTagInput(text) {
  return [...new Set(text.split(/[\s,]+/).map(s => s.trim().toLowerCase()).filter(Boolean))];
}

function renderTagSuggestions() {
  const box = document.getElementById('t-tags-suggest');
  clearChildren(box);
  const have = new Set(parseTagInput(document.getElementById('t-tags').value));
  const unused = editorFacets.tags.filter(t => !have.has(t.name)).slice(0, 12);
  if (unused.length) box.append(el('span', {class: 'suggest-label'}, 'Existing tags:'));
  for (const t of unused) {
    box.append(el('button', {
      type: 'button', class: 'chip',
      onclick: () => {
        const input = document.getElementById('t-tags');
        input.value = [...parseTagInput(input.value), t.name].join(', ');
        renderTagSuggestions();
        input.focus();
      }
    }, '+ ' + t.name, el('span', {class: 'chip-count'}, String(t.count))));
  }
}
document.getElementById('t-tags').addEventListener('input', renderTagSuggestions);

// t: an existing target (edit) or null (add).
//
// Everything that doesn't need the network is filled in *before* the first
// await. The device scan can take a while on a box with a capture card and
// serial adapters attached, and the form is already on screen: filling the
// text fields only after it returns would wipe whatever the owner had
// started typing in the meantime. Only the pickers that depend on what's
// plugged in wait for it, and Save stays disabled until they're ready.
async function openTargetEditor(t) {
  editingId = t ? t.id : null;
  const cfg = t ? t.config : null;
  document.getElementById('target-form-title').textContent = t ? `Edit ${t.name}` : 'Add target';
  setAlert('t-err-box', 't-err', '');
  document.getElementById('t-save').disabled = true;
  showTargetEditor(true);

  document.getElementById('t-name').value = t ? t.name : '';
  document.getElementById('t-desc').value = t ? t.description : '';
  document.getElementById('t-group').value = t ? t.group : '';
  document.getElementById('t-tags').value = t ? t.tags.join(', ') : '';
  document.getElementById('t-qmp').value = cfg ? cfg.qmp_socket : '';
  document.getElementById('t-bitrate').value = cfg ? cfg.webrtc_bitrate_kbps : 4000;
  document.getElementById('t-enabled').checked = t ? t.enabled : true;
  const def = document.getElementById('t-default');
  // There is no "un-default" — designate a different one instead — so the
  // current default's box is checked and locked.
  def.checked = !!(t && t.default);
  def.disabled = !!(t && t.default);

  // Input method. Existing targets keep theirs; new ones default to a
  // CH9329 adapter, the common case. The local gadget port can only serve one
  // target, so it's offered only if nobody else has it.
  const gadgetTaken = adminTargets.some(o => o.id !== editingId && o.config &&
                                             !o.config.serial_device && !o.config.qmp_socket);
  const method = !cfg ? 'serial' : cfg.qmp_socket ? 'qemu' : cfg.serial_device ? 'serial' : 'gadget';
  document.getElementById('t-in-gadget').disabled = gadgetTaken && method !== 'gadget';
  document.getElementById('t-in-' + method).checked = true;
  syncInputMethodFields();

  const baud = cfg ? cfg.serial_baud : 9600;
  const baudSel = document.getElementById('t-baud');
  clearChildren(baudSel);
  for (const b of (STANDARD_BAUDS.includes(baud) ? STANDARD_BAUDS : [...STANDARD_BAUDS, baud].sort((a, b) => a - b)))
    baudSel.append(el('option', {value: b}, String(b)));
  baudSel.value = String(baud);

  document.getElementById('t-name').focus();

  // ── Now the parts that depend on what's plugged in ──
  const [facets, devices] = await Promise.all([
    getJson('/api/targets/facets').catch(() => ({groups: [], tags: []})),
    getJson('/api/admin/devices').catch(() => ({video_devices: [], serial_devices: [], audio_devices: []}))
  ]);
  if (editingId !== (t ? t.id : null)) return; // closed or reopened for another target meanwhile
  editorFacets = facets;

  const dl = document.getElementById('dl-groups');
  clearChildren(dl);
  for (const g of facets.groups) dl.append(el('option', {value: g.name}));
  renderTagSuggestions();
  snapGroupSpelling();

  // Video device: keep this target's own device; for a new target, offer the
  // first one nobody else is using.
  const freeVideo = devices.video_devices.find(d => !d.in_use_by);
  const wantedVideo = cfg ? cfg.v4l2_device : (freeVideo ? freeVideo.path : '');
  const knownVideo = fillDeviceSelect(document.getElementById('t-video'), devices.video_devices, wantedVideo,
    d => `${d.name} (${d.path})` + (d.mjpeg ? '' : ' — no MJPEG modes detected'));
  document.getElementById('t-video-custom').value = knownVideo ? '' : wantedVideo;
  setShown(document.getElementById('t-video-custom-row'), !knownVideo);

  const freeSerial = devices.serial_devices.find(d => !d.in_use_by);
  const wantedSerial = cfg && cfg.serial_device ? cfg.serial_device : (freeSerial ? freeSerial.path : '');
  const knownSerial = fillDeviceSelect(document.getElementById('t-serial'), devices.serial_devices, wantedSerial,
    d => `${d.label} (${d.path})`);
  document.getElementById('t-serial-custom').value = knownSerial ? '' : wantedSerial;
  setShown(document.getElementById('t-serial-custom-row'), !knownSerial);

  fillAudioSelect(devices, cfg ? cfg.audio_device : 'auto');

  const mode = cfg ? [cfg.capture_width, cfg.capture_height, cfg.capture_fps] : [1280, 720, 30];
  const path = deviceValue('t-video', 't-video-custom');
  const capsUrl = path ? '/api/video/capabilities?device=' + encodeURIComponent(path) : '/api/video/capabilities';
  document.getElementById('t-save').disabled = false;   // the pickers are ready; the mode list can finish in its own time
  targetModePicker.load(capsUrl, ...mode);
}

document.getElementById('t-cancel').addEventListener('click', () => showTargetEditor(false));

document.getElementById('form-target').addEventListener('submit', async e => {
  e.preventDefault();
  setAlert('t-err-box', 't-err', '');

  const method = selectedInputMethod();
  const video = deviceValue('t-video', 't-video-custom');
  const serial = method === 'serial' ? deviceValue('t-serial', 't-serial-custom') : '';
  const qmp = method === 'qemu' ? document.getElementById('t-qmp').value.trim() : '';
  if (!video) { setAlert('t-err-box', 't-err', 'Choose a capture device.'); return; }
  if (method === 'serial' && !serial) { setAlert('t-err-box', 't-err', 'Choose the serial device the CH9329 is on.'); return; }
  if (method === 'qemu' && !qmp) { setAlert('t-err-box', 't-err', 'Enter the QEMU QMP socket path.'); return; }

  const mode = targetModePicker.read();
  const body = {
    name: document.getElementById('t-name').value.trim(),
    description: document.getElementById('t-desc').value.trim(),
    group: document.getElementById('t-group').value.trim(),
    tags: parseTagInput(document.getElementById('t-tags').value),
    enabled: document.getElementById('t-enabled').checked,
    v4l2_device: video,
    capture_width: mode.width, capture_height: mode.height, capture_fps: mode.fps,
    serial_device: serial,
    serial_baud: parseInt(document.getElementById('t-baud').value, 10) || 9600,
    qmp_socket: qmp,
    webrtc_bitrate_kbps: parseInt(document.getElementById('t-bitrate').value, 10),
    audio_device: document.getElementById('t-audio').value
  };
  const def = document.getElementById('t-default');
  if (def.checked && !def.disabled) body.default = true;

  const saving = document.getElementById('t-save');
  saving.disabled = true;
  const r = editingId === null
    ? await send('POST', '/api/targets', body)
    : await send('PUT', '/api/targets/' + editingId, body);
  saving.disabled = false;
  if (!r.ok) {
    setAlert('t-err-box', 't-err', r.text || 'Could not save target');
    return;
  }
  showTargetEditor(false);
  setMsg('admin-targets-msg', `${r.data.name} saved.` + (r.data.enabled ? ' It is starting up…' : ''), 'success');
  await loadAdminTargets();
});

// ── Users ────────────────────────────────────────────────────────────────
const ROLES = ['viewer', 'operator', 'owner'];

async function loadUsers() {
  const list = document.getElementById('user-list');
  try {
    const users = await getJson('/api/users');
    clearChildren(list);
    for (const u of users) {
      const meta = u.last_login_at
        ? `created ${u.created_at} · last sign-in ${u.last_login_at}`
        : `created ${u.created_at} · never signed in`;
      const roleSelect = el('select', {class: 'role-select', 'aria-label': `Role for ${u.username}`},
        ROLES.map(role => el('option', {value: role, selected: role === u.role}, capitalize(role))));
      roleSelect.addEventListener('change', () => changeUserRole(u.id, roleSelect.value));
      list.append(el('li', {},
        el('span', {class: 'row-main'},
          el('strong', {}, u.username),
          el('span', {class: 'row-meta'}, meta)),
        el('span', {class: 'row-actions'},
          roleSelect,
          el('button', {type: 'button', class: 'btn btn-accent btn-sm',
                        'aria-label': `Delete user ${u.username}`,
                        onclick: () => deleteUser(u.id, u.username)}, 'Delete'))));
    }
  } catch (e) {
    setMsg('user-msg', 'Could not load users', 'error');
  }
}

async function changeUserRole(id, role) {
  setMsg('user-msg', '');
  const r = await send('PUT', '/api/users/' + id + '/role', {role});
  if (!r.ok) setMsg('user-msg', r.text || 'Could not change role', 'error');
  loadUsers();
}

async function deleteUser(id, username) {
  if (!confirm(`Delete user "${username}"? This cannot be undone.`)) return;
  setMsg('user-msg', '');
  const r = await send('DELETE', '/api/users/' + id);
  if (!r.ok) {
    setMsg('user-msg', r.text || 'Could not delete user', 'error');
    return;
  }
  loadUsers();
}

document.getElementById('form-user').addEventListener('submit', async e => {
  e.preventDefault();
  setMsg('user-msg', '');
  const r = await post('/api/users', {
    username: document.getElementById('user-new-name').value,
    password: document.getElementById('user-new-pass').value,
    role: document.getElementById('user-new-role').value
  });
  if (!r.ok) {
    setMsg('user-msg', (await r.text()) || 'Could not create user', 'error');
    return;
  }
  document.getElementById('form-user').reset();
  loadUsers();
});

// ── Network: STUN/TURN servers ───────────────────────────────────────────
// The whole list is saved on every change (PUT replaces it). When the
// server's configuration fixes the list (--ice-server), it's shown
// read-only and the server refuses changes anyway.
let iceServers = [];

const isTurn = url => /^turns?:/i.test(url.trim());

async function loadIceServers() {
  setMsg('ice-msg', '');
  let state;
  try {
    state = await getJson('/api/admin/ice-servers');
  } catch (e) {
    setMsg('ice-msg', 'Could not load the STUN and TURN servers', 'error');
    return;
  }
  setShown(document.getElementById('ice-managed'), state.managed_by_config);
  setShown(document.getElementById('form-ice'), !state.managed_by_config);
  renderIceServers(state.ice_servers, state.managed_by_config);
}

function renderIceServers(list, managed) {
  iceServers = list;
  const ul = document.getElementById('ice-list');
  clearChildren(ul);
  for (const s of list) {
    ul.append(el('li', {},
      el('span', {class: 'row-main'},
        el('strong', {}, s.url),
        el('span', {class: 'row-meta'}, s.username ? `TURN · username ${s.username}` : 'STUN')),
      managed ? null : el('span', {class: 'row-actions'},
        el('button', {type: 'button', class: 'btn btn-accent btn-sm', 'aria-label': `Remove ${s.url}`,
                      onclick: () => saveIceServers(iceServers.filter(x => x !== s), `Removed ${s.url}.`)},
           'Remove'))));
  }
  setShown(document.getElementById('ice-empty'), list.length === 0);
}

async function saveIceServers(list, okMessage) {
  setMsg('ice-msg', '');
  const r = await send('PUT', '/api/admin/ice-servers', {ice_servers: list});
  if (!r.ok) {
    setMsg('ice-msg', r.text || 'Could not save the STUN and TURN servers', 'error');
    return false;
  }
  renderIceServers(r.data.ice_servers, false);
  setMsg('ice-msg', okMessage + ' Viewers who connect from now on use the new list.', 'success');
  return true;
}

document.getElementById('ice-url').addEventListener('input', e =>
  setShown(document.getElementById('ice-login'), isTurn(e.target.value)));

document.getElementById('form-ice').addEventListener('submit', async e => {
  e.preventDefault();
  const url = document.getElementById('ice-url').value.trim();
  const turn = isTurn(url);
  const server = {
    url,
    username: turn ? document.getElementById('ice-username').value : '',
    credential: turn ? document.getElementById('ice-credential').value : '',
  };
  if (await saveIceServers([...iceServers, server], `Added ${url}.`)) {
    e.target.reset();
    setShown(document.getElementById('ice-login'), false);
  }
});
