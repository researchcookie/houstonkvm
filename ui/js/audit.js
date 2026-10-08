// ─── Admin → Audit: what happened, and who drove which target ─────────────
// Reads /api/audit/*, which only Owners may. Control sessions carry only how
// many keys were pressed and how much the mouse was used, never what was
// typed: the server doesn't keep it.

const AUDIT_PAGE = 50;
let auditEventsBefore = null;     // ?before= for "Show older", null at the end
let auditSessionsBefore = null;

function openAuditPanel() {
  loadAuditEvents(true);
  loadAuditSessions(true);
}

function auditError(text) {
  setAlert('audit-err-box', 'audit-err', text || '');
}

function auditTime(iso) {
  const d = new Date(iso);
  return isNaN(d) ? iso : d.toLocaleString();
}

function auditWho(e) {
  const name = e.user && e.user.username;
  if (!name) return e.ip || '—';
  return e.via === 'token' ? `${name} (API token)` : name;
}

// ── What each event says ────────────────────────────────────────────────
const END_REASONS = {
  released: 'released control',
  owner_override: 'an Owner took control away',
  signed_out: 'signed out',
  credentials_revoked: 'their access ended',
  target_stopped: 'the target stopped',
  server_stopped: 'the server stopped',
  interrupted: 'the server stopped unexpectedly',
};

const REFUSALS = {
  wrong_password: 'wrong password',
  unknown_user: 'no such user',
  locked_out: 'locked out after repeated failures',
  wrong_setup_code: 'wrong setup code',
  wrong_current_password: 'wrong current password',
};

function auditWhat(e) {
  const d = e.detail || {};
  const refused = e.outcome === 'busy' ? 'server busy' : (REFUSALS[d.reason] || d.reason || '');
  switch (e.type) {
    case 'auth.setup':           return e.outcome === 'ok' ? 'Created the first Owner account' : `Setup refused: ${refused}`;
    case 'auth.login':           return e.outcome === 'ok' ? 'Signed in' : `Sign-in refused: ${refused}`;
    case 'auth.logout':          return 'Signed out';
    case 'auth.password_change': return e.outcome === 'ok' ? 'Changed their password' : `Password change refused: ${refused}`;
    case 'token.create':         return 'Created an API token';
    case 'token.revoke':         return 'Revoked an API token';
    case 'user.create':          return `Added ${d.username} (${d.role})`;
    case 'user.role_change':     return `Made ${d.username} ${d.to} (was ${d.from})`;
    case 'user.delete':          return `Deleted ${d.username}`;
    case 'target.create':        return 'Added the target';
    case 'target.update':        return 'Changed the target';
    case 'target.delete':        return 'Deleted the target';
    case 'target.video_settings': return 'Changed video settings';
    case 'target.input_test':    return 'Ran an input test';
    case 'target.baud_change':   return `Changed the adapter's speed to ${d.baud}`;
    case 'server.ice_servers':   return 'Changed the STUN and TURN servers';
    case 'tls.ca_created':       return 'Made HoustonKVM’s own certificate authority';
    case 'tls.csr_created':      return `Made a certificate signing request for ${(d.names || [])[0] || ''}`;
    case 'tls.certificate_installed':
      if (e.outcome !== 'ok') return `Renewing the HTTPS certificate failed: ${d.error}`;
      return d.source === 'houstonkvm-ca' ? 'Issued an HTTPS certificate from HoustonKVM’s CA'
           : d.source === 'csr' ? 'Installed the certificate for the signing request'
           : 'Installed an HTTPS certificate';
    case 'tls.enabled':          return 'Turned on HTTPS';
    case 'tls.reverted':         return 'HTTPS went back off: not confirmed in time';
    case 'tls.disabled':         return 'Turned off HTTPS';
    case 'tls.settings':         return 'Changed HTTPS settings';
    case 'tls.reloaded':         return e.outcome === 'ok' ? 'Reloaded the HTTPS certificate'
                                                           : `HTTPS certificate reload failed: ${d.error}`;
    case 'control.acquire':      return 'Took control';
    case 'control.release':      return `Stopped driving: ${END_REASONS[d.reason] || d.reason}`;
    case 'control.force_release': return `Took control away from ${d.driver}`;
    default:                     return e.type;
  }
}

function auditDetails(e) {
  const d = e.detail || {};
  if (d.changes) {
    return Object.entries(d.changes)
      .map(([k, v]) => `${k}: ${JSON.stringify(v.from)} → ${JSON.stringify(v.to)}`).join('; ');
  }
  if (e.type === 'auth.password_change' && d.signed_out_sessions)
    return `Signed out ${d.signed_out_sessions} other session${d.signed_out_sessions === 1 ? '' : 's'}`;
  if (d.label) return `“${d.label}”`;
  if (d.fingerprint_sha256) return `Certificate ${d.fingerprint_sha256.slice(0, 23)}…`;
  if (d.control_session) return `Session #${d.control_session}`;
  return e.ip ? `from ${e.ip}` : '';
}

// ── Activity ────────────────────────────────────────────────────────────
async function loadAuditEvents(fresh) {
  if (fresh) auditEventsBefore = null;
  const kind = document.getElementById('audit-kind').value;
  const q = new URLSearchParams({limit: AUDIT_PAGE});
  if (kind) q.set('type', kind);
  if (auditEventsBefore) q.set('before', auditEventsBefore);
  let page;
  try {
    page = await getJson('/api/audit/events?' + q);
  } catch (e) {
    auditError('Could not load the audit log: ' + e.message);
    return;
  }
  auditError('');
  const body = document.getElementById('audit-events-body');
  if (fresh) clearChildren(body);
  for (const e of page.events) {
    body.append(el('tr', {},
      el('td', {}, auditTime(e.at)),
      el('td', {}, auditWho(e)),
      el('td', {class: e.outcome === 'ok' ? '' : 'audit-denied'}, auditWhat(e)),
      el('td', {}, e.target ? e.target.name : ''),
      el('td', {class: 'audit-detail'}, auditDetails(e))));
  }
  auditEventsBefore = page.next_before;
  setShown(document.getElementById('audit-events-more'), !!auditEventsBefore);
  setShown(document.getElementById('audit-events-empty'), !body.firstChild);
}

document.getElementById('audit-kind').addEventListener('change', () => loadAuditEvents(true));
document.getElementById('audit-events-more').addEventListener('click', () => loadAuditEvents(false));

// ── Control sessions ────────────────────────────────────────────────────
function auditLength(s) {
  if (!s.ended_at) return s.end_reason ? 'unknown' : 'still driving';
  const secs = Math.max(0, Math.round((new Date(s.ended_at) - new Date(s.started_at)) / 1000));
  if (secs < 60) return `${secs} s`;
  const mins = Math.floor(secs / 60);
  return mins < 60 ? `${mins} min ${secs % 60} s` : `${Math.floor(mins / 60)} h ${mins % 60} min`;
}

async function loadAuditSessions(fresh) {
  if (fresh) auditSessionsBefore = null;
  const q = new URLSearchParams({limit: AUDIT_PAGE});
  if (auditSessionsBefore) q.set('before', auditSessionsBefore);
  let page;
  try {
    page = await getJson('/api/audit/control-sessions?' + q);
  } catch (e) {
    auditError('Could not load control sessions: ' + e.message);
    return;
  }
  const body = document.getElementById('audit-sessions-body');
  if (fresh) clearChildren(body);
  for (const s of page.sessions) {
    body.append(el('tr', {},
      el('td', {}, auditTime(s.started_at)),
      el('td', {}, s.user.username + (s.via === 'token' ? ' (API token)' : '')),
      el('td', {}, s.target.name),
      el('td', {}, auditLength(s)),
      el('td', {}, String(s.key_count)),
      el('td', {}, String(s.mouse_count)),
      el('td', {}, s.end_reason ? (END_REASONS[s.end_reason] || s.end_reason) : '—')));
  }
  auditSessionsBefore = page.next_before;
  setShown(document.getElementById('audit-sessions-more'), !!auditSessionsBefore);
  setShown(document.getElementById('audit-sessions-empty'), !body.firstChild);
}

document.getElementById('audit-sessions-more').addEventListener('click', () => loadAuditSessions(false));
