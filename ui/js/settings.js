// ─── Settings page (account / preferences / API tokens) ────────────────────
// What belongs to the signed-in person, not to a machine. Per-target things
// live with the target: wiring on the owner-only Admin page (admin.js), and
// resolution / refresh rate in each target's Display panel (display.js).

const settingsTabs = initTabs('settings-tabs', ['account', 'preferences', 'bots'],
                              'stab-', 'spanel-');

// Reloads on every visit, so its data is fresh every time rather than only
// once at sign-in.
function openSettingsPage() {
  settingsTabs.show('account');
  for (const id of ['pw-msg', 'pref-msg', 'token-msg']) setMsg(id, '');
  loadPreferences();
  loadBots();
}

// ── Account ──────────────────────────────────────────────────────────────
document.getElementById('form-password').addEventListener('submit', async e => {
  e.preventDefault();
  setMsg('pw-msg', '');
  const r = await post('/api/account/password', {
    current_password: document.getElementById('pw-current').value,
    new_password: document.getElementById('pw-new').value
  });
  if (r.ok) {
    const n = (await r.json().catch(() => ({}))).signed_out_sessions || 0;
    setMsg('pw-msg', n ? `Password changed. Signed out ${n} other session${n === 1 ? '' : 's'}.`
                       : 'Password changed.', 'success');
    e.target.reset();
  } else {
    setMsg('pw-msg', (await r.text()) || 'Could not change password', 'error');
  }
});

// ── Preferences ──────────────────────────────────────────────────────────
async function loadPreferences() {
  try {
    const d = await getJson('/api/settings');
    document.getElementById('pref-stream-mode').value = d.default_stream_mode;
    document.getElementById('pref-bitrate').value = d.webrtc_bitrate_kbps ?? '';
  } catch (e) {
    setMsg('pref-msg', 'Could not load preferences', 'error');
  }
}

document.getElementById('form-preferences').addEventListener('submit', async e => {
  e.preventDefault();
  setMsg('pref-msg', '');
  const bitrateRaw = document.getElementById('pref-bitrate').value;
  const body = {
    default_stream_mode: document.getElementById('pref-stream-mode').value,
    webrtc_bitrate_kbps: bitrateRaw === '' ? null : parseInt(bitrateRaw, 10)
  };
  const r = await send('PUT', '/api/settings', body);
  if (r.ok) {
    userPrefs = body;
    setMsg('pref-msg', 'Preferences saved.', 'success');
  } else {
    setMsg('pref-msg', r.text || 'Could not save preferences', 'error');
  }
});

// ── API tokens ───────────────────────────────────────────────────────────
const TOKEN_SCOPES = {full: 'full access', read: 'read only', metrics: 'metrics only'};

async function loadBots() {
  const list = document.getElementById('token-list');
  // Only an Owner may make a metrics token (the server checks too).
  const metricsOption = document.getElementById('token-scope-metrics');
  metricsOption.hidden = metricsOption.disabled = currentRole !== 'owner';
  try {
    const tokens = await getJson('/api/tokens');
    clearChildren(list);
    if (!tokens.length) list.append(el('li', {class: 'row-empty'}, 'No API tokens yet.'));
    for (const t of tokens) {
      const meta = [
        TOKEN_SCOPES[t.scope] || t.scope,
        `created ${t.created_at}`,
        t.last_used_at ? `last used ${t.last_used_at}` : 'never used',
        t.expired ? 'EXPIRED' : t.expires_at ? `expires ${t.expires_at}` : 'never expires',
      ].join(' · ');
      list.append(el('li', {},
        el('span', {class: 'row-main'},
          el('strong', {}, t.label || '(unlabeled)'),
          el('span', {class: 'row-meta'}, meta)),
        el('button', {type: 'button', class: 'btn btn-accent btn-sm',
                      'aria-label': `Revoke token ${t.label || ''}`.trim(),
                      onclick: () => revokeBot(t.id)}, 'Revoke')));
    }
  } catch (e) {
    setMsg('token-msg', 'Could not load API tokens', 'error');
  }
}

async function revokeBot(id) {
  await fetch('/api/tokens/' + id, {method: 'DELETE', credentials: 'include'});
  loadBots();
}

document.getElementById('form-token').addEventListener('submit', async e => {
  e.preventDefault();
  setMsg('token-msg', '');
  const label = document.getElementById('token-label').value;
  const scope = document.getElementById('token-scope').value;
  const expiry = document.getElementById('token-expiry').value;
  const r = await post('/api/tokens', {
    label, scope, expires_in_days: expiry === 'never' ? null : Number(expiry)});
  if (!r.ok) {
    setMsg('token-msg', (await r.text()) || 'Could not create token', 'error');
    return;
  }
  const info = await r.json();
  document.getElementById('token-label').value = '';
  const wrap = document.getElementById('token-reveal-wrap');
  clearChildren(wrap);
  wrap.append(el('div', {class: 'alert alert-warning', role: 'status'},
    el('h2', {class: 'alert-heading'}, 'Copy this token now'),
    el('p', {class: 'alert-text'}, 'It won’t be shown again.'),
    el('code', {class: 'token-reveal'}, info.token)));
  loadBots();
});
