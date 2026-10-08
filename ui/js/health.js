// ─── Input health panel: is the keyboard/mouse link working, and if not, ───
// which cable to look at. Everyone who can see the target can read it; only
// an Owner can run the test, which the server refuses while someone drives.

const healthBtn   = document.getElementById('health-btn');
const healthPanel = document.getElementById('health-panel');
let healthTimer = null;

const HEALTH_TAGS = {
  good:     {text: 'Good',            kind: 'live'},
  degraded: {text: 'Flaky',           kind: 'warn'},
  down:     {text: 'Down',            kind: 'fault'},
  unknown:  {text: 'Not checked yet', kind: 'off'},
};
const FASTEST_BAUD = 115200;   // the CH9329's fastest
const INPUT_KINDS = {
  'ch9329': 'CH9329 serial adapter', 'usb-gadget': 'USB gadget port', 'qemu': 'QEMU virtual machine',
};

function updateHealthAvailability() {
  const on = !!currentTarget && currentTarget.enabled;
  setShown(healthBtn, on);
  if (!on) closeHealthPanel();
}

function closeHealthPanel() {
  clearTimeout(healthTimer);
  healthTimer = null;
  setShown(healthPanel, false);
  healthBtn.setAttribute('aria-expanded', 'false');
}

function verdictLine(box, state, text) {
  clearChildren(box);
  box.append(statusTagNode(HEALTH_TAGS[state] || HEALTH_TAGS.unknown), el('span', {}, text));
}

function renderHealthFacts(h) {
  const dl = document.getElementById('health-facts');
  clearChildren(dl);
  const fact = (term, value) => {
    if (value === null || value === undefined) return;
    dl.append(el('dt', {}, term), el('dd', {}, String(value)));
  };
  fact('Connection', INPUT_KINDS[h.kind] || h.kind || 'none');
  if (h.kind !== 'ch9329') return;
  const yesNo = v => v === null ? null : v ? 'yes' : 'no';
  fact('Target recognises it', yesNo(h.target_enumerated));
  fact('Firmware', h.firmware);
  let baud = `${h.configured_baud}`;
  if (h.chip_baud && h.chip_baud !== h.configured_baud) baud += ` (chip stores ${h.chip_baud})`;
  if (h.answers_at_baud) baud += ` (chip answers at ${h.answers_at_baud})`;
  fact('Baud rate', baud);
  const w = h.last_hour;
  fact('Checks, last hour', `${w.checks_ok} OK, ${w.checks_failed} failed`);
  if (w.checks_ok) fact('Reply time', `${w.reply_ms_avg.toFixed(1)} ms average, ${w.reply_ms_max.toFixed(1)} ms worst`);
  fact('Reconnects, last hour', w.reconnects);
  if (w.dropped_commands) fact('Input lost while down', `${w.dropped_commands} commands`);
}

function renderSelfTest(t) {
  const area = document.getElementById('health-test-area');
  setShown(area, !!t && currentRole === 'owner');
  if (!t) return;
  const btn = document.getElementById('health-test-btn');
  const result = document.getElementById('health-test-result');
  btn.disabled = t.state === 'running';
  if (t.state === 'running') { verdictLine(result, 'unknown', 'Testing…'); return; }
  if (t.state !== 'done') { clearChildren(result); return; }
  verdictLine(result, t.verdict_state, t.verdict);
  const details = [`${t.ok} of ${t.probes} replies`];
  if (t.ok) details.push(`${t.reply_ms_avg.toFixed(1)} ms average`);
  if (t.chip_baud) details.push(`chip stores ${t.chip_baud} baud`);
  details.push(`tested ${new Date(t.finished_at * 1000).toLocaleTimeString()}`);
  result.append(el('span', {class: 'health-test-detail'}, details.join(' · ')));
}

// Offered while the adapter answers at a slower rate than its fastest, or to
// show how the last change went.
function renderBaudChange(h) {
  const area = document.getElementById('health-baud-area');
  const c = h.baud_change;
  const slow = h.state === 'good' && h.configured_baud < FASTEST_BAUD;
  const show = !!c && currentRole === 'owner' && (slow || c.state !== 'idle');
  setShown(area, show);
  if (!show) return;
  const btn = document.getElementById('health-baud-btn');
  const result = document.getElementById('health-baud-result');
  setShown(btn, slow || c.state === 'running');
  setShown(document.getElementById('health-baud-hint'), !btn.hidden);
  btn.disabled = c.state === 'running';
  if (c.state === 'running') { verdictLine(result, 'unknown', `Switching to ${c.to_baud} baud…`); return; }
  if (c.state !== 'done') { clearChildren(result); return; }
  verdictLine(result, c.ok ? 'good' : 'degraded', c.message);
}

async function loadHealth() {
  clearTimeout(healthTimer);
  const id = currentTargetId;
  if (id === null || healthPanel.hidden) return;
  const r = await send('GET', `/api/targets/${id}/health`);
  if (currentTargetId !== id || healthPanel.hidden) return;
  if (!r.ok) {
    setMsg('health-msg', 'Could not load input health: ' + r.text, 'error');
  } else {
    setMsg('health-msg', '');
    verdictLine(document.getElementById('health-summary'), r.data.state, r.data.summary);
    renderHealthFacts(r.data);
    renderSelfTest(r.data.test);
    renderBaudChange(r.data);
  }
  const testing = r.ok && ((r.data.test && r.data.test.state === 'running') ||
                           (r.data.baud_change && r.data.baud_change.state === 'running'));
  healthTimer = setTimeout(loadHealth, testing ? 1000 : 5000);
}

healthBtn.addEventListener('click', () => {
  const open = healthPanel.hidden;
  setShown(healthPanel, open);
  healthBtn.setAttribute('aria-expanded', String(open));
  if (open) loadHealth(); else closeHealthPanel();
});

document.getElementById('health-test-btn').addEventListener('click', async () => {
  const id = currentTargetId;
  if (id === null) return;
  setMsg('health-msg', '');
  const r = await send('POST', `/api/targets/${id}/health/test`);
  if (currentTargetId !== id) return;
  if (!r.ok) setMsg('health-msg', r.text, 'error');
  loadHealth();
});

document.getElementById('health-baud-btn').addEventListener('click', async () => {
  const id = currentTargetId;
  if (id === null) return;
  setMsg('health-msg', '');
  const r = await send('POST', `/api/targets/${id}/health/baud`, {baud: FASTEST_BAUD});
  if (currentTargetId !== id) return;
  if (!r.ok) setMsg('health-msg', r.text, 'error');
  loadHealth();
});
