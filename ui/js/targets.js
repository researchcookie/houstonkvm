// ─── Targets: the directory page ───────────────────────────────────────────
// A "target" is one machine you can view and control. This file owns finding
// them: the home page (search, group/tag filters, cards).

const targetFilters = {q: '', group: '', tag: ''};
let targetsPollTimer = null;
let renderedSignature = '';        // static fields of what's on screen (see renderTargets)
let renderedCards = new Map();     // id -> {card, status, img, thumb, lastThumb}
const TARGETS_POLL_MS = 10000;

// ── Status vocabulary ────────────────────────────────────────────────────
// What a person needs to know before clicking: is there a picture, and can
// they type? Returned as tags so cards and the KVM header
// both say the same thing in the same words.
function statusTags(t) {
  if (!t.enabled) return [{text: 'Disabled', kind: 'off'}];
  if (t.state === 'starting') return [{text: 'Starting…', kind: 'off'}];
  const tags = [];
  tags.push(videoTag(t));
  if (t.status.controlled) {
    const driver = t.status.driver;
    const who = !driver ? '' : driver === currentUsername ? 'you' : driver;
    tags.push({text: who ? `In use by ${who}` : 'In use', kind: 'busy'});
  } else if (t.status.input_health === 'down') tags.push({text: 'Input down', kind: 'fault'});
  else if (!t.status.input_ready) tags.push({text: 'View only', kind: 'off'});
  // Working, but it failed recently: worth a look before it gets worse.
  if (t.status.input_health === 'degraded') tags.push({text: 'Input flaky', kind: 'warn'});
  return tags;
}

// The server reopens a capture device that drops out by itself, so
// "reconnecting" and "no capture device" fix themselves once it's back.
const VIDEO_TAGS = {
  starting: {text: 'Starting…', kind: 'off'},
  good: {text: 'Live', kind: 'live'},
  no_signal: {text: 'No signal', kind: 'nosignal'},
  reconnecting: {text: 'Video reconnecting…', kind: 'warn'},
  absent: {text: 'No capture device', kind: 'fault'},
  off: {text: 'No video', kind: 'off'},
};

function videoTag(t) {
  // video_state is newer than capture_active; fall back for an older server.
  return VIDEO_TAGS[t.status.video_state] ||
    (t.status.capture_active ? VIDEO_TAGS.good : VIDEO_TAGS.no_signal);
}

function statusTagNode(tag) {
  return el('span', {class: 'status-tag status-tag--' + tag.kind}, tag.text);
}

// ── Loading ──────────────────────────────────────────────────────────────
async function fetchTargets(query) {
  const params = new URLSearchParams();
  if (query && query.q) params.set('q', query.q);
  if (query && query.group) params.set('group', query.group);
  if (query && query.tag) params.set('tag', query.tag);
  const qs = params.toString();
  const d = await getJson('/api/targets' + (qs ? '?' + qs : ''));
  return d.targets;
}

// Where a bare load lands. With exactly one target there is nothing to choose,
// so go straight to it — a single-machine install then looks just like it
// always has. With more (even if most are disabled), show the directory so
// nobody misses that the others exist.
function hashIsEmpty() { return !location.hash || location.hash === '#' || location.hash === '#/'; }

async function landAfterSignIn() {
  try {
    const all = await fetchTargets();
    // The app is already showing while this loads. If the person (or a link)
    // has gone somewhere in the meantime, that wins — a late redirect must not
    // yank them back.
    if (!hashIsEmpty()) return;
    if (all.length === 1) {
      goTo(targetHref(all[0].id));
      return;
    }
  } catch (e) { /* fall through to the directory */ }
  goTo('#/targets');
}

// ── The directory page ───────────────────────────────────────────────────
async function openTargetsPage() {
  const search = document.getElementById('targets-search');
  search.value = targetFilters.q;
  setAlert('targets-error', 'targets-error-text', '');
  await Promise.all([loadFacets(), loadTargetsPage(true)]);
  startTargetsPolling();
}

async function loadFacets() {
  let facets = {groups: [], tags: []};
  try { facets = await getJson('/api/targets/facets'); } catch (e) { /* filters just stay hidden */ }

  const groupSel = document.getElementById('targets-group');
  clearChildren(groupSel);
  groupSel.append(el('option', {value: ''}, 'All groups'));
  for (const g of facets.groups)
    groupSel.append(el('option', {value: g.name}, `${g.name} (${g.count})`));
  groupSel.value = facets.groups.some(g => g.name === targetFilters.group) ? targetFilters.group : '';
  targetFilters.group = groupSel.value;

  const tagRow = document.getElementById('targets-tags');
  clearChildren(tagRow);
  for (const t of facets.tags.slice(0, 16)) {
    const on = targetFilters.tag === t.name;
    tagRow.append(el('button', {
      type: 'button', class: 'chip' + (on ? ' chip--on' : ''), 'aria-pressed': String(on),
      onclick: () => { targetFilters.tag = on ? '' : t.name; loadFacets(); loadTargetsPage(true); }
    }, t.name, el('span', {class: 'chip-count'}, String(t.count))));
  }
  setShown(document.getElementById('targets-tags-wrap'), facets.tags.length > 0);
  setShown(document.getElementById('targets-filters'), facets.groups.length > 0 || facets.tags.length > 0);
}

async function loadTargetsPage(force) {
  let list;
  try {
    list = await fetchTargets(targetFilters);
  } catch (e) {
    setAlert('targets-error', 'targets-error-text', 'Could not load targets: ' + e.message);
    return;
  }
  setAlert('targets-error', 'targets-error-text', '');
  renderTargets(list, force);
}

function filtersActive() {
  return !!(targetFilters.q || targetFilters.group || targetFilters.tag);
}

function renderTargets(list, force) {
  const results = document.getElementById('targets-results');

  // Empty states first: no targets at all vs. nothing matches the filters.
  const empty = document.getElementById('targets-empty');
  const actions = document.getElementById('targets-empty-actions');
  clearChildren(actions);
  if (list.length === 0) {
    clearChildren(results);
    renderedSignature = '';
    renderedCards = new Map();
    if (filtersActive()) {
      document.getElementById('targets-empty-title').textContent = 'No matching targets';
      document.getElementById('targets-empty-text').textContent =
        'Nothing matches your search. Try fewer words, or clear the filters.';
      actions.append(el('button', {type: 'button', class: 'btn', onclick: clearTargetFilters}, 'Clear search'));
    } else if (currentRole === 'owner') {
      document.getElementById('targets-empty-title').textContent = 'No targets yet';
      document.getElementById('targets-empty-text').textContent =
        'Add your first target to start viewing and controlling a machine.';
      actions.append(el('a', {class: 'btn', href: '#/admin',
                             onclick: () => { adminWantsNewTarget = true; }}, 'Add a target'));
    } else {
      document.getElementById('targets-empty-title').textContent = 'No targets yet';
      document.getElementById('targets-empty-text').textContent =
        'Nothing has been set up here yet. Ask an administrator to add a target.';
    }
    setShown(empty, true);
    document.getElementById('targets-count').textContent = '';
    return;
  }
  setShown(empty, false);
  document.getElementById('targets-count').textContent =
    list.length === 1 ? '1 target' : `${list.length} targets`;

  // A periodic refresh must not rebuild the page under the user (it would
  // drop hover and keyboard focus). Only the things that change while you
  // watch — status tags and thumbnails — are updated in place; a full
  // re-render happens only when the set/order/text of targets changed.
  const signature = JSON.stringify(list.map(t => [t.id, t.name, t.description, t.group, t.tags]));
  if (!force && signature === renderedSignature) {
    for (const t of list) updateCard(t);
    return;
  }
  renderedSignature = signature;
  renderedCards = new Map();
  clearChildren(results);

  // The server returns targets ordered by group (ungrouped last), so groups
  // are just runs of the same value.
  const anyGrouped = list.some(x => x.group);
  let run = null, grid = null;
  for (const t of list) {
    if (run === null || t.group !== run) {
      run = t.group;
      if (anyGrouped) {
        const count = list.filter(x => x.group === run).length;
        results.append(el('h2', {class: 'group-heading'}, run || 'Ungrouped',
                          el('span', {class: 'group-count'}, String(count))));
      }
      grid = el('ul', {class: 'target-grid'});
      results.append(grid);
    }
    grid.append(buildCard(t));
  }
}

function buildCard(t) {
  const img = el('img', {alt: '', loading: 'lazy'});
  img.addEventListener('error', () => { img.hidden = true; });
  img.addEventListener('load', () => { img.hidden = false; });
  const thumb = el('div', {class: 'target-thumb'}, img,
                   el('span', {class: 'thumb-note'}));
  const status = el('div', {class: 'card-status'});
  const tags = el('ul', {class: 'tag-list', 'aria-label': 'Tags'},
                  t.tags.map(name => el('li', {}, el('button', {
                    type: 'button', class: 'tag-btn',
                    title: `Show only "${name}" targets`,
                    onclick: ev => { ev.stopPropagation(); targetFilters.tag = name; loadFacets(); loadTargetsPage(true); }
                  }, name))));
  const card = el('li', {class: 'target-card'},
    el('div', {class: 'target-card__frame'},
      el('div', {class: 'target-card__media'}, thumb),
      el('div', {class: 'target-card__header'},
        el('h3', {class: 'target-card__heading'},
          el('a', {class: 'target-link', href: targetHref(t.id)}, t.name))),
      el('div', {class: 'target-card__body'},
        t.description ? el('p', {class: 'target-desc'}, t.description) : null,
        t.tags.length ? tags : null),
      el('div', {class: 'target-card__footer'}, status)));
  renderedCards.set(t.id, {card, status, img, thumb, lastThumb: 0});
  updateCard(t);
  return card;
}

function updateCard(t) {
  const c = renderedCards.get(t.id);
  if (!c) return;
  clearChildren(c.status);
  for (const tag of statusTags(t)) c.status.append(statusTagNode(tag));
  c.card.dataset.state = t.enabled ? (t.status.capture_active ? 'live' : 'nosignal') : 'off';

  const note = c.thumb.querySelector('.thumb-note');
  if (t.enabled && t.status.capture_active) {
    note.textContent = '';
    // The snapshot is the capture's latest frame — cheap — but there's no
    // point refetching faster than the page itself polls.
    const now = Date.now();
    if (now - c.lastThumb > TARGETS_POLL_MS - 500) {
      c.lastThumb = now;
      c.img.src = `/api/targets/${t.id}/snapshot?t=${now}`;
    }
  } else {
    c.img.hidden = true;
    note.textContent = !t.enabled ? 'Disabled' : t.state === 'starting' ? 'Starting…' : videoTag(t).text;
  }
}

function startTargetsPolling() {
  stopTargetsPolling();
  targetsPollTimer = setInterval(() => {
    if (!currentRoute || currentRoute.name !== 'targets' || document.hidden) return;
    loadTargetsPage(false);
  }, TARGETS_POLL_MS);
}

function stopTargetsPolling() {
  if (targetsPollTimer) clearInterval(targetsPollTimer);
  targetsPollTimer = null;
}

function clearTargetFilters() {
  targetFilters.q = ''; targetFilters.group = ''; targetFilters.tag = '';
  document.getElementById('targets-search').value = '';
  loadFacets();
  loadTargetsPage(true);
}

const searchTargetsSoon = debounce(() => loadTargetsPage(true), 150);
document.getElementById('targets-search').addEventListener('input', e => {
  targetFilters.q = e.target.value.trim();
  searchTargetsSoon();
});
document.getElementById('targets-search-form').addEventListener('submit', e => {
  e.preventDefault();
  targetFilters.q = document.getElementById('targets-search').value.trim();
  loadTargetsPage(true);
});
document.getElementById('targets-group').addEventListener('change', e => {
  targetFilters.group = e.target.value;
  loadTargetsPage(true);
});

// "/" jumps to the directory's search box — unless it's a real character for
// someone: a text field is focused, or the user is driving a target, where
// every key belongs to that machine (see input.js).
document.addEventListener('keydown', e => {
  if (e.key !== '/' || e.ctrlKey || e.metaKey || e.altKey) return;
  if (!isSignedIn() || !currentRoute || currentRoute.name !== 'targets') return;
  const t = e.target;
  if (t && (t.tagName === 'INPUT' || t.tagName === 'TEXTAREA' || t.tagName === 'SELECT' || t.isContentEditable)) return;
  e.preventDefault();
  document.getElementById('targets-search').focus();
});
