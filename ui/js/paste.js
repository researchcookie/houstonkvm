// ─── Paste text: typed on the target by the server ──────────────────────────
// For getting text onto a machine that shares no clipboard with you: a BIOS
// screen, a login prompt, an installer. The server types it one key at a
// time, paced for the keyboard link, and keeps your own keyboard and mouse
// working in between (see InputQueue::pushText()). Only the driver sees this;
// losing control stops the typing on the server's side.

const pasteBtn   = document.getElementById('paste-btn');
const pastePanel = document.getElementById('paste-panel');
const pasteText  = document.getElementById('paste-text');
const pasteStop  = document.getElementById('paste-stop');
let pasteTimer = null;
let pasteTotal = 0;

// Mirrors the server's check (prepareTextForTyping()) so a stray curly quote
// is pointed out before anything is sent. The server checks again anyway.
function untypeableChar(text) {
  let pos = 0;
  for (const ch of text) {
    pos++;
    if (!/^[\x20-\x7E\t\n\r]$/.test(ch)) {
      const cp = 'U+' + ch.codePointAt(0).toString(16).toUpperCase().padStart(4, '0');
      const shown = ch.codePointAt(0) >= 0x20 && ch !== '\x7F' ? `‘${ch}’ ` : '';
      return `Character ${pos} (${shown}${cp}) can’t be typed: only US-keyboard characters ` +
             '(plain ASCII, newlines and tabs) can be.';
    }
  }
  return null;
}

function updatePasteAvailability() {
  setShown(pasteBtn, isDriver);
  if (!isDriver) closePastePanel();
}

function closePastePanel() {
  stopPasteProgress();
  setShown(pasteStop, false);
  setShown(pastePanel, false);
  pasteBtn.setAttribute('aria-expanded', 'false');
}

function stopPasteProgress() {
  if (pasteTimer) clearTimeout(pasteTimer);
  pasteTimer = null;
}

// Follows the server's count of characters still to type until it's done.
async function pollPasteProgress() {
  const id = currentTargetId;
  const r = await send('GET', targetApi('/control/status'));
  if (currentTargetId !== id || pastePanel.hidden) return;
  if (!r.ok || !r.data.is_driver) {
    setShown(pasteStop, false);
    setMsg('paste-msg', 'Stopped: you no longer have control of this machine.', 'error');
    return;
  }
  const left = r.data.typing_remaining;
  if (left > 0) {
    setMsg('paste-msg', `Typing… ${left} of ${pasteTotal} characters left.`);
    pasteTimer = setTimeout(pollPasteProgress, 500);
  } else {
    setShown(pasteStop, false);
    setMsg('paste-msg', 'Done.', 'success');
  }
}

pasteBtn.addEventListener('click', () => {
  const open = pastePanel.hidden;
  if (!open) { closePastePanel(); return; }
  setShown(pastePanel, true);
  pasteBtn.setAttribute('aria-expanded', 'true');
  setMsg('paste-msg', '');
  pasteText.focus();
});

document.getElementById('form-paste').addEventListener('submit', async e => {
  e.preventDefault();
  const text = pasteText.value;
  if (!text) { setMsg('paste-msg', 'Nothing to type.', 'error'); return; }
  const problem = untypeableChar(text);
  if (problem) { setMsg('paste-msg', problem, 'error'); return; }

  const id = currentTargetId;
  const r = await send('POST', targetApi('/input'), {type: 'text', text});
  if (currentTargetId !== id) return;
  if (!r.ok) {
    if (r.status === 401 || r.status === 403) { isDriver = false; updateControlUI(); }
    setMsg('paste-msg', r.text || 'Could not type the text.', 'error');
    return;
  }
  // A paste sent while another is still typing is queued after it.
  const length = text.replace(/\r\n?/g, '\n').length;
  pasteTotal = pasteStop.hidden ? length : pasteTotal + length;
  setShown(pasteStop, true);
  stopPasteProgress();
  pollPasteProgress();
});

pasteStop.addEventListener('click', async () => {
  const id = currentTargetId;
  stopPasteProgress();
  const r = await send('POST', targetApi('/input'), {type: 'text_cancel'});
  if (currentTargetId !== id) return;
  setShown(pasteStop, false);
  setMsg('paste-msg', r.ok ? 'Stopped.' : (r.text || 'Could not stop the typing.'), r.ok ? '' : 'error');
});
