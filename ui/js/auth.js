const authView = document.getElementById('auth-view');
const appRoot  = document.getElementById('app');

// Populated from /api/me after every sign-in/setup/boot — 'viewer',
// 'operator', or 'owner'. Server-side routes are the real enforcement;
// this only drives which controls the UI shows (see kvm.js/admin.js).
let currentRole = null;
// This signed-in user's own username, so "someone else is driving" tags
// (see targets.js's statusTags) can say "you" instead of echoing your own name.
let currentUsername = null;

// Per-user preferences from /api/settings, loaded at sign-in. The default
// stream mode is applied each time a target is opened (see kvm.js).
let userPrefs = {default_stream_mode: 'mjpeg'};

function isSignedIn() { return !appRoot.hidden; }

function capitalize(s) { return s ? s.charAt(0).toUpperCase() + s.slice(1) : ''; }

// ─── The Owner session looks different ───────────────────────────────────
// Owners administer the server; they can't drive a target (see kvm.js).
// Their session gets its own header colour, a standing banner, a filled role
// badge, an "Owner ·" prefix on every tab title and its own tab icon, so an
// Owner window can't be mistaken for an ordinary one at a glance or in a tab
// strip. The stylesheet keys off body[data-session].
const favicon = document.getElementById('favicon');
const PLAIN_ICON = favicon.getAttribute('href');
const OWNER_ICON = PLAIN_ICON.replace("fill='%233a302a'", "fill='%23cca586'")
                             .replace("stroke='%23f7efe2'", "stroke='%233a302a'")
                             .replace("stroke='%23cca586'", "stroke='%233a302a'");

function applySessionLook(role) {
  const owner = role === 'owner';
  if (owner) document.body.dataset.session = 'owner';
  else delete document.body.dataset.session;
  setShown(document.getElementById('owner-rail'), owner);
  favicon.href = owner ? OWNER_ICON : PLAIN_ICON;
}

// Every page title goes through here, so the Owner prefix is never lost.
function setPageTitle(page) {
  const base = page ? page + ' — HoustonKVM' : 'HoustonKVM';
  document.title = currentRole === 'owner' ? 'Owner · ' + base : base;
}

// ─── Sign in / first-time setup ───────────────────────────────────────────
function showAuthForm(which) {
  document.getElementById('auth-heading').textContent = which === 'setup' ? 'First-time setup' : 'Sign in';
  for (const n of ['login', 'setup']) setShown(document.getElementById('form-' + n), n === which);
  // The skip link jumps to whichever <main> is showing.
  document.querySelector('.skip-link').setAttribute('href', '#auth-view');
}

async function boot() {
  await confirmHttpsIfAsked();
  try {
    const r = await fetch('/api/status', {credentials: 'include'});
    const d = await r.json();
    if (d.needs_setup) {
      showAuthForm('setup');
    } else if (d.authenticated) {
      await showApp();
      return;
    }
  } catch (e) { /* stay on the sign-in screen */ }
  setShown(authView, true);
  document.getElementById(document.getElementById('form-setup').hidden ? 'l-user' : 's-code').focus();
}

async function loadPrefs() {
  try {
    const d = await getJson('/api/settings');
    userPrefs = d;
  } catch (e) { /* defaults are fine */ }
}

async function showApp() {
  try {
    const me = await getJson('/api/me');
    currentRole = me.role;
    currentUsername = me.username;
  } catch (e) {
    currentRole = null;
    currentUsername = null;
  }
  document.getElementById('role-badge').textContent = capitalize(currentRole);
  applySessionLook(currentRole);
  setShown(document.getElementById('nav-admin-item'), currentRole === 'owner');
  await loadPrefs();

  setShown(authView, false);
  setShown(appRoot, true);
  document.querySelector('.skip-link').setAttribute('href', '#main-content');

  // A deep link (#/targets/3) is honoured as-is; a bare load lands sensibly:
  // straight into the only target if there's just one, else the directory.
  if (hashIsEmpty()) await landAfterSignIn();
  else await route();
}

function showLogin() {
  currentRoute = null;
  currentRole = null;
  currentUsername = null;
  applySessionLook(null);
  setPageTitle('');
  setShown(appRoot, false);
  setShown(authView, true);
  history.replaceState(null, '', location.pathname);
  showAuthForm('login');
  document.getElementById('l-pass').value = '';
  document.getElementById('l-user').focus();
}

document.getElementById('form-login').addEventListener('submit', async e => {
  e.preventDefault();
  setAlert('l-err-box', 'l-err', '');
  const r = await post('/api/login', {
    username: document.getElementById('l-user').value,
    password: document.getElementById('l-pass').value
  });
  if (r.ok) await showApp();
  else setAlert('l-err-box', 'l-err', (await r.text()) || 'Sign-in failed');
});

document.getElementById('form-setup').addEventListener('submit', async e => {
  e.preventDefault();
  setAlert('s-err-box', 's-err', '');
  const r = await post('/api/setup', {
    setup_code: document.getElementById('s-code').value,
    username: document.getElementById('s-user').value,
    password: document.getElementById('s-pass').value
  });
  if (r.ok) await showApp();
  else setAlert('s-err-box', 's-err', (await r.text()) || 'Setup failed');
});

document.getElementById('logout-btn').addEventListener('click', async () => {
  await closeTargetView(); // releases control and stops streams first
  await fetch('/api/logout', {method: 'POST', credentials: 'include'});
  showLogin();
});
