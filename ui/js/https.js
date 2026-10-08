// ─── Admin → Network → HTTPS ───────────────────────────────────────────────
// The server starts on plain HTTP; an Owner gets a certificate here and turns
// HTTPS on (see /api/tls in docs/API.md). Turning it on sends this browser to
// the HTTPS address with a one-time code after the #; that page confirms it
// (confirmHttpsIfAsked, on load), which is what makes HTTPS stick. Not
// confirmed in 2 minutes, the server goes back to plain HTTP by itself.

let httpsState = null;
let httpsPoll = null;
let httpsSource = null;   // the certificate route the Owner picked: 'org' or 'own'

const isLocalHost = h => ['localhost', '127.0.0.1', '[::1]'].includes(h);

function namesFrom(text) {
  return text.split(/[\s,]+/).map(s => s.trim()).filter(Boolean);
}

function formatDate(iso) {
  return new Date(iso).toLocaleDateString(undefined, {year: 'numeric', month: 'short', day: 'numeric'});
}

function httpsModeText(st) {
  if (st.mode === 'https')
    return `HTTPS on port ${st.https_port}` + (st.http_redirect ? '; plain HTTP sends visitors to it' : '');
  if (st.mode === 'pending')
    return `Waiting for a browser to reach HTTPS on port ${st.https_port} ` +
           `(${st.pending.expires_in} s left, then back to plain HTTP)`;
  return 'Plain HTTP: passwords and the screen cross the network unencrypted';
}

async function loadHttps() {
  clearTimeout(httpsPoll);
  try {
    httpsState = await getJson('/api/tls');
  } catch (e) {
    setMsg('https-msg', 'Could not load the HTTPS settings: ' + e.message, 'error');
    return;
  }
  renderHttps(httpsState);
  if (httpsState.pending_csr) loadPendingCsr();
  // While a switch waits to be confirmed, follow it: it may revert.
  if (httpsState.mode === 'pending' && !document.getElementById('apanel-network').hidden)
    httpsPoll = setTimeout(loadHttps, 2000);
}

function renderHttps(st) {
  const dl = document.getElementById('https-status');
  clearChildren(dl);
  const fact = (term, value, cls) => dl.append(el('dt', {}, term), el('dd', {class: cls}, value));
  fact('Now', httpsModeText(st));
  const back = st.last_revert;
  if (st.mode === 'http' && back && back.seconds_ago < 3600) {
    const ago = back.seconds_ago < 60 ? 'just now' : `${Math.round(back.seconds_ago / 60)} min ago`;
    fact('Last try', `HTTPS went back off ${ago}: no browser confirmed it on port ${back.https_port} in time. ` +
         'Either the browser refused the certificate (trust its CA first, see step 1) or a firewall ' +
         `blocks the port (as root: ${firewallCommand(back.https_port)}).`);
  }
  const c = st.certificate;
  if (c) {
    fact('Certificate for', c.names.join(', '));
    fact('Issued by', c.issued_by_houstonkvm_ca ? 'HoustonKVM’s own certificate authority'
                     : c.self_signed ? 'Itself (self-signed)' : c.issuer);
    fact('Expires', `${formatDate(c.not_after)} (${c.expired ? 'expired' : c.days_left + ' days left'})`);
    fact('Fingerprint (SHA-256)', c.fingerprint_sha256, 'mono');
    if (!c.covers_this_host)
      fact('Note', `It doesn’t name ${location.hostname}, the address you’re using, so browsers will warn.`);
  } else {
    fact('Certificate', 'None yet');
  }
  if (st.certificate_error) fact('Problem', st.certificate_error);
  if (st.ca) {
    const name = (/(?:^|,)CN=([^,]+)/.exec(st.ca.subject) || [])[1] || st.ca.subject;
    fact('Certificate authority', name);
    fact('Its fingerprint (SHA-256)', st.ca.fingerprint_sha256, 'mono');
  }

  const pinned = [];
  if (st.pinned) pinned.push(`HTTPS is ${st.pinned} by the server’s configuration (--tls=${st.pinned}).`);
  if (st.certificate_managed_by_config)
    pinned.push('The certificate is set by the server’s configuration (/etc/houstonkvm/tls/ or --tls-cert).');
  if (st.https_port_managed_by_config) pinned.push('The HTTPS port is set by the server’s configuration (--https-port).');
  document.getElementById('https-pinned-text').textContent = pinned.join(' ');
  setShown(document.getElementById('https-pinned'), pinned.length > 0);

  const enable = document.getElementById('https-enable');
  setShown(enable, st.mode === 'http' && !st.pinned);
  enable.disabled = !c;
  enable.title = c ? '' : 'Make or install a certificate first';
  setShown(document.getElementById('https-disable'), st.mode !== 'http' && !st.pinned);
  document.getElementById('https-disable').textContent = st.mode === 'pending' ? 'Cancel' : 'Turn off HTTPS';
  setShown(document.getElementById('https-switch-hint'), st.mode === 'http' && !st.pinned);

  const certStep = document.getElementById('https-cert-tools');
  setShown(certStep, !st.certificate_managed_by_config);
  certStep.classList.toggle('step--done', !!c);
  document.getElementById('https-switch').classList.toggle('step--done', st.mode === 'https');
  showHttpsSource(httpsSource || (c && c.issued_by_houstonkvm_ca ? 'own' : 'org'));
  document.getElementById('https-generate').textContent =
    c && c.issued_by_houstonkvm_ca ? 'Make a new certificate' : 'Make a certificate';
  setShown(document.getElementById('https-ca-download'), !!st.ca);
  for (const id of ['https-gen-names', 'https-csr-names']) {
    const box = document.getElementById(id);
    if (!box.value.trim()) box.value = st.suggested_names.join('\n');
  }
  setShown(document.getElementById('https-upload-warning'),
           location.protocol === 'http:' && !isLocalHost(location.hostname));

  const port = document.getElementById('https-port');
  port.value = st.https_port;
  port.disabled = st.https_port_managed_by_config || st.mode === 'pending';
  showFirewallCommand();
  setShown(document.getElementById('https-port-save'), st.mode === 'https' && !st.https_port_managed_by_config);
  document.getElementById('https-redirect').checked = st.http_redirect;
  document.getElementById('https-auto-renew').checked = st.auto_renew;
  setShown(document.getElementById('https-auto-renew-row'), !!st.ca);   // only its own CA's certificates renew
  const hsts = document.getElementById('https-hsts');
  hsts.checked = st.hsts;
  hsts.disabled = !st.hsts && !(st.mode === 'https' && c && !c.self_signed);

  updateHttpsBanner(st);
}

function showHttpsSource(source) {
  document.getElementById('https-source-' + source).checked = true;
  setShown(document.getElementById('https-route-org'), source === 'org');
  setShown(document.getElementById('https-route-own'), source === 'own');
}

for (const source of ['org', 'own']) {
  document.getElementById('https-source-' + source).addEventListener('change', () => {
    httpsSource = source;
    showHttpsSource(source);
  });
}

// The packaged firewalld service opens 8080 and 8443; any other port by number.
function firewallCommand(port) {
  const open = Number(port) === 8443 ? '--add-service=houstonkvm' : `--add-port=${port}/tcp`;
  return `firewall-cmd --permanent ${open} && firewall-cmd --reload`;
}

function showFirewallCommand() {
  const port = document.getElementById('https-port').value;
  document.getElementById('https-port-cmd').textContent = firewallCommand(port || 8443);
}

document.getElementById('https-port').addEventListener('input', showFirewallCommand);

// The port in the box, saved if it changed; false (and says why) if refused.
async function saveHttpsPort() {
  const port = Number(document.getElementById('https-port').value);
  if (httpsState.https_port_managed_by_config || port === httpsState.https_port) return true;
  const r = await send('PUT', '/api/tls/settings', {https_port: port});
  if (!r.ok) {
    setMsg('https-msg', r.text || 'Could not change the HTTPS port', 'error');
    return false;
  }
  httpsState = r.data;
  return true;
}

// Across the top of Admin: plain HTTP, or a certificate about to lapse.
function updateHttpsBanner(st) {
  const c = st.certificate;
  let text = '';
  if (st.mode === 'http' && !st.pinned)
    text = 'This server is on plain HTTP: passwords and the screen cross the network unencrypted. Turn on HTTPS under';
  else if (st.mode === 'https' && c && c.days_left <= 14 && !(c.issued_by_houstonkvm_ca && st.auto_renew))
    text = c.expired ? 'The HTTPS certificate has expired. Replace it under'
                     : `The HTTPS certificate expires in ${c.days_left} days. Replace it under`;
  document.getElementById('https-banner-text').textContent = text;
  setShown(document.getElementById('https-banner'), !!text);
}

function reportInstalled(r, what) {
  if (!r.ok) {
    setMsg('https-msg', r.text || 'Could not install the certificate', 'error');
    return false;
  }
  const next = httpsState && httpsState.mode === 'https' ? 'New connections use it now.'
             : httpsState && httpsState.pinned === 'off' ? '' : 'Next: turn on HTTPS.';
  const warnings = (r.data.warnings || []).join(' ');
  setMsg('https-msg', [what, next, warnings].filter(Boolean).join(' '), warnings ? 'error' : 'success');
  return true;
}

document.getElementById('https-generate').addEventListener('click', async () => {
  setMsg('https-msg', '');
  const r = await send('POST', '/api/tls/generate', {
    names: namesFrom(document.getElementById('https-gen-names').value),
    key_type: document.getElementById('https-gen-key').value,
  });
  if (reportInstalled(r, 'Made a certificate from HoustonKVM’s certificate authority. To stop browsers warning ' +
                         'about it, download the CA certificate and trust it.')) await loadHttps();
});

document.getElementById('https-enable').addEventListener('click', async () => {
  setMsg('https-msg', '');
  if (!await saveHttpsPort()) return;
  const r = await send('POST', '/api/tls/enable');
  if (!r.ok) {
    setMsg('https-msg', r.text || 'Could not turn on HTTPS', 'error');
    return;
  }
  const warnings = r.data.warnings || [];
  if (warnings.length && !confirm(warnings.join('\n') + '\n\nGo to the HTTPS address anyway?')) {
    await send('POST', '/api/tls/disable');
    await loadHttps();
    return;
  }
  location.assign(r.data.https_url);
});

document.getElementById('https-disable').addEventListener('click', async () => {
  const pending = httpsState && httpsState.mode === 'pending';
  if (!pending && !confirm('Turn off HTTPS? Everyone moves to plain HTTP, unencrypted, and signs in again.'))
    return;
  setMsg('https-msg', '');
  const r = await send('POST', '/api/tls/disable');
  if (!r.ok) {
    setMsg('https-msg', r.text || 'Could not turn off HTTPS', 'error');
    return;
  }
  if (location.protocol === 'https:') location.assign(`http://${location.hostname}:${r.data.http_port}/#/admin`);
  else await loadHttps();
});

// While HTTPS is on, a new port takes effect at once (no confirm-or-revert),
// so this browser follows it there.
document.getElementById('https-port-save').addEventListener('click', async () => {
  const port = Number(document.getElementById('https-port').value);
  if (port === httpsState.https_port) return;
  if (!confirm(`Move HTTPS to port ${port}? New connections go there straight away: open it in the ` +
               'firewall first, or nobody gets back in until it is.')) return;
  setMsg('https-msg', '');
  if (!await saveHttpsPort()) return;
  if (location.protocol === 'https:') location.assign(`https://${location.hostname}:${port}/#/admin`);
  else renderHttps(httpsState);
});

async function loadPendingCsr() {
  const r = await send('GET', '/api/tls/csr');
  document.getElementById('https-csr-text').value = r.ok ? r.text : '';
  setShown(document.getElementById('https-csr-pending'), r.ok);
}

document.getElementById('form-https-csr').addEventListener('submit', async e => {
  e.preventDefault();
  setMsg('https-msg', '');
  const r = await send('POST', '/api/tls/csr', {
    names: namesFrom(document.getElementById('https-csr-names').value),
    key_type: document.getElementById('https-csr-key').value,
    organization: document.getElementById('https-csr-org').value.trim(),
    unit: document.getElementById('https-csr-unit').value.trim(),
    country: document.getElementById('https-csr-country').value.trim().toUpperCase(),
  });
  if (!r.ok) {
    setMsg('https-msg', r.text || 'Could not make the signing request', 'error');
    return;
  }
  document.getElementById('https-csr-text').value = r.data.csr;
  setShown(document.getElementById('https-csr-pending'), true);
  setMsg('https-msg', 'Made a signing request. Give it to your certificate authority, then paste the ' +
                      'certificate it sends back below.', 'success');
});

document.getElementById('https-csr-discard').addEventListener('click', async () => {
  await send('DELETE', '/api/tls/csr');
  document.getElementById('https-csr-text').value = '';
  setShown(document.getElementById('https-csr-pending'), false);
  setMsg('https-msg', 'Discarded the signing request.', 'success');
});

document.getElementById('form-https-signed').addEventListener('submit', async e => {
  e.preventDefault();
  setMsg('https-msg', '');
  const r = await send('POST', '/api/tls/certificate', {cert: document.getElementById('https-signed').value});
  if (reportInstalled(r, 'Installed the certificate from your certificate authority.')) {
    e.target.reset();
    setShown(document.getElementById('https-csr-pending'), false);
    await loadHttps();
  }
});

document.getElementById('form-https-upload').addEventListener('submit', async e => {
  e.preventDefault();
  setMsg('https-msg', '');
  const r = await send('POST', '/api/tls/certificate', {
    cert: document.getElementById('https-up-cert').value,
    key: document.getElementById('https-up-key').value,
  });
  if (reportInstalled(r, 'Installed the certificate.')) {
    e.target.reset();   // the key doesn't stay in the page
    await loadHttps();
  }
});

document.getElementById('form-https-settings').addEventListener('submit', async e => {
  e.preventDefault();
  setMsg('https-msg', '');
  const body = {
    http_redirect: document.getElementById('https-redirect').checked,
    hsts: document.getElementById('https-hsts').checked,
    auto_renew: document.getElementById('https-auto-renew').checked,
  };
  const r = await send('PUT', '/api/tls/settings', body);
  if (!r.ok) {
    setMsg('https-msg', r.text || 'Could not save the HTTPS settings', 'error');
    return;
  }
  httpsState = r.data;
  renderHttps(r.data);
  setMsg('https-msg', 'Saved.', 'success');
});

document.getElementById('https-banner-link').addEventListener('click', e => {
  e.preventDefault();
  adminTabs.show('network');
});

// On the HTTPS address an Owner was just sent to: the code after the # is
// the proof a browser got here, which makes HTTPS stick. Nobody is signed
// in yet: the plain-HTTP session doesn't carry over (see kSessionCookie).
async function confirmHttpsIfAsked() {
  const m = /^#tls-confirm=([0-9a-f]{32})$/.exec(location.hash);
  if (!m) return;
  history.replaceState(null, '', location.pathname);
  const r = await send('POST', '/api/tls/confirm', {code: m[1]});
  const notice = document.getElementById('auth-notice');
  notice.className = 'alert alert-slim ' + (r.ok ? 'alert-success' : 'alert-error');
  document.getElementById('auth-notice-text').textContent = r.ok
    ? 'HTTPS is on. Sign in again: your plain-HTTP session doesn’t carry over.'
    : `HTTPS wasn’t confirmed: ${r.text || 'no answer'}. Go back to the plain-HTTP address and turn it on again.`;
  setShown(notice, true);
}
