async function post(url, body) {
  return fetch(url, {
    method:'POST', credentials:'include',
    headers:{'Content-Type':'application/json'},
    body: JSON.stringify(body)
  });
}

async function getJson(url) {
  const r = await fetch(url, {credentials:'include'});
  if (!r.ok) throw new Error((await r.text()) || String(r.status));
  return r.json();
}

// Any request with a JSON body. Resolves (never rejects on an HTTP error) to
// {ok, status, text, data}, where `data` is the parsed JSON body or null —
// callers show `text` to the user when !ok, since the server's error bodies
// are short human-readable messages.
async function send(method, url, body) {
  const init = {method, credentials:'include'};
  if (body !== undefined) {
    init.headers = {'Content-Type':'application/json'};
    init.body = JSON.stringify(body);
  }
  const r = await fetch(url, init);
  const text = await r.text();
  let data = null;
  try { data = text ? JSON.parse(text) : null; } catch (e) { /* not JSON */ }
  return {ok: r.ok, status: r.status, text, data};
}

function escapeHtml(s) {
  const d = document.createElement('div');
  d.textContent = s;
  return d.innerHTML;
}

// Builds a DOM node. Target names, tags, descriptions and usernames are all
// admin-supplied text, so everything user-visible goes through textContent
// (via text children here) rather than being interpolated into innerHTML.
//   el('button', {class:'btn', onclick: fn, 'aria-label':'x'}, 'Label', childNode)
function el(tag, props, ...children) {
  const node = document.createElement(tag);
  for (const [k, v] of Object.entries(props || {})) {
    if (v === undefined || v === null || v === false) continue;
    if (k === 'class') node.className = v;
    else if (k.startsWith('on') && typeof v === 'function') node.addEventListener(k.slice(2), v);
    else if (k === 'text') node.textContent = v;
    else node.setAttribute(k, v === true ? '' : v);
  }
  for (const c of children.flat()) {
    if (c === undefined || c === null || c === false) continue;
    node.append(c.nodeType ? c : document.createTextNode(String(c)));
  }
  return node;
}

function clearChildren(node) {
  while (node.firstChild) node.removeChild(node.firstChild);
}

function debounce(fn, ms) {
  let t = null;
  return (...args) => { clearTimeout(t); t = setTimeout(() => fn(...args), ms); };
}

// Shows/hides via the `hidden` attribute (css/base.css makes [hidden] win over
// component display rules, which the attribute alone doesn't guarantee).
function setShown(node, shown) {
  if (node) node.hidden = !shown;
}

// One-line status under a form: kind is 'error', 'success' or '' (clear).
function setMsg(id, text, kind) {
  const n = document.getElementById(id);
  if (!n) return;
  n.textContent = text || '';
  n.className = 'form-msg' + (text && kind ? ' form-msg--' + kind : '');
}

// Error alert boxes (a hidden .alert wrapping a message element).
function setAlert(boxId, textId, text) {
  document.getElementById(textId).textContent = text || '';
  setShown(document.getElementById(boxId), !!text);
}
