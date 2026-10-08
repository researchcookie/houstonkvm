// ─── Hash router ───────────────────────────────────────────────────────────
// Every screen has a URL, so a target can be bookmarked or shared and the
// browser's Back button does what people expect:
//   #/targets        the directory (home)
//   #/targets/<id>   one target's screen
//   #/settings       #/admin (owner only)
const ROUTE_PAGES = {targets: 'page-targets', target: 'page-kvm',
                     settings: 'page-settings', admin: 'page-admin'};
const ROUTE_NAV = {targets: 'nav-targets', target: 'nav-targets',
                   settings: 'nav-settings', admin: 'nav-admin'};

let currentRoute = null; // {name, id?}

function parseRoute(hash) {
  const parts = (hash || '').replace(/^#\/?/, '').split('/').filter(Boolean);
  if (parts[0] === 'targets' && parts[1] && /^\d+$/.test(parts[1]))
    return {name: 'target', id: Number(parts[1])};
  if (parts[0] === 'settings') return {name: 'settings'};
  if (parts[0] === 'admin') return {name: 'admin'};
  if (parts[0] === 'targets' || parts.length === 0) return {name: 'targets'};
  return {name: 'targets'};
}

function goTo(hash) {
  if (location.hash === hash) route(); else location.hash = hash;
}

function targetHref(id) { return '#/targets/' + id; }

// Runs on every hash change (and once after sign-in). Does nothing until the
// user is signed in, so a stray hashchange on the login screen can't render
// pages that need data.
async function route() {
  if (!isSignedIn()) return;
  const next = parseRoute(location.hash);

  // Owner-only page: bounce anyone else rather than show an empty shell. The
  // server enforces this too; this just keeps the UI honest.
  if (next.name === 'admin' && currentRole !== 'owner') {
    goTo('#/targets');
    return;
  }

  const prev = currentRoute;
  if (prev && prev.name === 'target' && !(next.name === 'target' && next.id === prev.id))
    await closeTargetView();
  currentRoute = next;

  for (const [name, pageId] of Object.entries(ROUTE_PAGES))
    setShown(document.getElementById(pageId), name === next.name);
  for (const id of new Set(Object.values(ROUTE_NAV))) {
    const link = document.getElementById(id);
    const active = id === ROUTE_NAV[next.name];
    if (active) link.setAttribute('aria-current', 'page'); else link.removeAttribute('aria-current');
  }
  closeMobileNav();

  if (next.name === 'targets') { setPageTitle('Targets'); openTargetsPage(); }
  else if (next.name === 'target') await openTargetView(next.id);
  else if (next.name === 'settings') { setPageTitle('Settings'); openSettingsPage(); }
  else if (next.name === 'admin') { setPageTitle('Admin'); openAdminPage(); }

  // Keep keyboard/screen-reader users oriented after a page change; the KVM
  // view manages its own focus (it wants keystrokes to reach the screen).
  if (next.name !== 'target') document.getElementById('main-content').focus({preventScroll: true});
}

// ─── Mobile nav ──────────────────────────────────────────────────────────
// Below the desktop breakpoint the header's menu button collapses/expands
// #primary-nav (see css/layout.css); this owns opening, closing, and every way
// of dismissing it — its own close button, Escape, a click outside, or
// (below) navigating anywhere.
const menuBtn = document.getElementById('menu-btn');
const primaryNav = document.getElementById('primary-nav');

function setMobileNavOpen(open) {
  primaryNav.classList.toggle('is-open', open);
  menuBtn.setAttribute('aria-expanded', String(open));
}
function closeMobileNav() { setMobileNavOpen(false); }

menuBtn.addEventListener('click', () => setMobileNavOpen(menuBtn.getAttribute('aria-expanded') !== 'true'));
document.getElementById('nav-close').addEventListener('click', closeMobileNav);
primaryNav.addEventListener('click', (e) => { if (e.target.closest('a')) closeMobileNav(); });
document.addEventListener('keydown', (e) => { if (e.key === 'Escape') closeMobileNav(); });
document.addEventListener('click', (e) => {
  if (primaryNav.classList.contains('is-open') && !primaryNav.contains(e.target) && !menuBtn.contains(e.target))
    closeMobileNav();
});

window.addEventListener('hashchange', route);
