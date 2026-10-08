// ─── WebRTC (low-latency) viewing mode ─────────────────────────────────────
//
// Wait for our own ICE gathering to complete before sending the answer, so
// it carries all our candidates. The server's candidates come back in the
// answer's response (?candidates=on-answer), not in its offer: with them
// earlier, the browser starts connecting before the server has the answer,
// and the server can then reject the browser's certificate for good. See
// /api/targets/:id/webrtc/* in docs/API.md.
let webrtcPc = null;
let webrtcStatsTimer = null;
// Per-phase timing from the most recent startWebrtc() call, shown alongside
// the live diagnostics below — non-trickle ICE means connecting is a chain
// of serial round trips (subscribe, full ICE gathering including a STUN
// round trip, the answer post), and "switching modes feels slow" needs
// this breakdown to tell which of those is actually the slow one, rather
// than guessing.
let lastConnectTimings = '';

// On-page WebRTC diagnostics — reusing the #video-status banner instead of
// asking a viewer to open dev tools. A "no video in Low-Latency mode" report
// is otherwise a dead end: the server logs look healthy, and browser console
// output often can't be had at all (e.g. a phone/tablet Safari with no
// reachable inspector). This makes the signal visible on the page: connection/ICE
// state, plus per-second RTP stats, so "healthy transport, zero frames
// decoded" (codec problem) is distinguishable at a glance from "zero bytes
// received" (network/ICE problem) or "frames decoded fine" (a paint/CSS
// problem, which would point somewhere else entirely).
function startWebrtcStats(pc) {
  const statusEl = document.getElementById('video-status');
  statusEl.classList.add('video-status--diag');
  const update = async () => {
    // Real video playing → same as the MJPEG health banner, stay out of the
    // way and say nothing. Only surface the detailed diagnostics (connection
    // state, RTP stats, decode counters) when there's actually a problem to
    // look at.
    const healthy = kvmVideo.readyState >= 3 && !kvmVideo.paused && kvmVideo.videoWidth > 0;
    if (healthy) { setShown(statusEl, false); return; }
    setShown(statusEl, true);

    let line = `conn: ${pc.connectionState} / ice: ${pc.iceConnectionState}`;
    try {
      const stats = await pc.getStats();
      let sawVideo = false, sawAudio = false;
      stats.forEach(r => {
        if (r.type === 'inbound-rtp' && r.kind === 'video') {
          sawVideo = true;
          line += `\nvideo: ${r.bytesReceived}B ${r.packetsReceived}pkt `
                + `${r.framesReceived || 0} recv ${r.framesDecoded || 0} decoded `
                + `${r.framesDropped || 0} dropped`;
          if (r.frameWidth) line += ` ${r.frameWidth}x${r.frameHeight}`;
        }
        if (r.type === 'inbound-rtp' && r.kind === 'audio') {
          sawAudio = true;
          line += `\naudio: ${r.bytesReceived}B ${r.packetsReceived}pkt`;
        }
      });
      if (!sawVideo) line += '\nvideo: no inbound-rtp stats yet';
      if (!sawAudio) line += '\naudio: no inbound-rtp stats yet';
    } catch (e) {
      line += `\ngetStats failed: ${e.message}`;
    }
    line += `\nvideoEl: readyState=${kvmVideo.readyState} paused=${kvmVideo.paused} `
          + `${kvmVideo.videoWidth}x${kvmVideo.videoHeight}`;
    if (lastConnectTimings) line = lastConnectTimings + '\n' + line;
    statusEl.textContent = line;
  };
  update();
  webrtcStatsTimer = setInterval(update, 1500);
}

function stopWebrtcStats() {
  if (webrtcStatsTimer) clearInterval(webrtcStatsTimer);
  webrtcStatsTimer = null;
}

function setWebrtcButton(active) {
  const btn = document.getElementById('webrtc-btn');
  if (!btn) return;
  // The label names the mode you'd switch *to*.
  btn.textContent = active ? 'Standard video' : 'Low-latency video';
  btn.classList.toggle('btn-outline', !active);
  updateSoundButton();
}

// Sound rides the low-latency stream only, and starts muted: browsers pause a
// video that unmutes itself without a click, so only the button unmutes it,
// and it isn't remembered between visits. Shown while there's sound to hear.
// Hiding it never re-mutes: a moment without sound (the sound card
// reconnecting, the target's settings being applied) shouldn't undo the
// viewer's choice. Leaving low-latency video does (stopWebrtc).
function updateSoundButton() {
  const btn = document.getElementById('sound-btn');
  const state = currentTarget && currentTarget.status ? currentTarget.status.audio_state : 'off';
  const on = !!webrtcPc && (state === 'idle' || state === 'good');
  setShown(btn, on);
  btn.textContent = kvmVideo.muted ? 'Unmute' : 'Mute';
  btn.classList.toggle('btn-outline', kvmVideo.muted);
}

document.getElementById('sound-btn').addEventListener('click', () => {
  kvmVideo.muted = !kvmVideo.muted;
  updateSoundButton();
});

// The server sends H.264 only (it re-encodes the capture once and fans it
// out). A browser that can't play H.264 — a Firefox whose OpenH264 plugin
// isn't installed or is disabled, some locked-down builds — rejects the video
// section, and with everything bundled onto it ICE never even starts. So
// check first and say so, instead of leaving a button that silently does
// nothing.
function browserCanPlayH264() {
  try {
    return RTCRtpReceiver.getCapabilities('video').codecs
      .some(c => c.mimeType.toLowerCase() === 'video/h264');
  } catch (e) {
    return true; // can't tell (older browser): try, and let the timeouts below catch a failure
  }
}

// Bounds on the parts that can otherwise wait forever. Gathering normally
// finishes in well under a second on a LAN; if it hasn't by ICE_GATHER_MAX_MS
// (an unreachable STUN server, say) the candidates found so far are enough
// for a server on the same network. And a connection that hasn't come up
// WEBRTC_CONNECT_MAX_MS after the answer is sent isn't going to.
const ICE_GATHER_MAX_MS = 4000;
const WEBRTC_CONNECT_MAX_MS = 12000;

async function startWebrtc() {
  if (!browserCanPlayH264()) {
    setCtrlHint('Low-latency video needs H.264, which this browser can’t play — staying on standard video.', true);
    return;
  }
  lastConnectTimings = '';
  const t0 = performance.now();
  const marks = [];
  const mark = label => marks.push(`${label}=${(performance.now() - t0).toFixed(0)}ms`);
  try {
    const id = currentTargetId;
    const subRes = await fetch(targetApi('/webrtc/subscribe') + '?candidates=on-answer',
                               {method:'POST', credentials:'include'});
    if (!subRes.ok) throw new Error((await subRes.text()) || 'subscribe failed');
    const {subscriberId, sdp, iceServers = []} = await subRes.json();
    mark('subscribe');

    // The STUN/TURN servers the Owner set (Admin → Network), the same ones
    // the server uses; none by default. None are needed on a LAN: the
    // browser reaches the server's own addresses, and the server learns
    // the browser's from those very checks (a peer-reflexive candidate),
    // even when Chrome hides its local addresses behind mDNS ".local" names.
    const pc = new RTCPeerConnection({iceServers});
    // Don't rely on e.streams[0]: the server's video and audio tracks
    // aren't guaranteed to share an msid, so the browser can hand each
    // track its own synthesized single-track stream in a separate ontrack
    // event. Trusting e.streams[0] means whichever track's event fires
    // last silently overwrites srcObject — if that's the audio track, the
    // element ends up bound to an audio-only stream: connection healthy,
    // both tracks genuinely open, nothing to paint. Building our own
    // stream and adding every track to it sidesteps that regardless of
    // arrival order or server-side SDP grouping.
    const inboundStream = new MediaStream();
    kvmVideo.srcObject = inboundStream;
    pc.ontrack = e => {
      inboundStream.addTrack(e.track);
      // Ask for the smallest video jitter buffer the browser will allow:
      // for a KVM, a frame shown late is worse than an occasional stutter.
      // jitterBufferTarget is the standard (ms); playoutDelayHint is the
      // older Chrome-only spelling (s). Audio keeps its default, since a
      // starved audio buffer crackles.
      if (e.track.kind === 'video') {
        if ('jitterBufferTarget' in e.receiver) e.receiver.jitterBufferTarget = 0;
        else if ('playoutDelayHint' in e.receiver) e.receiver.playoutDelayHint = 0;
      }
    };

    await pc.setRemoteDescription({type: 'offer', sdp});
    mark('setRemoteDescription');
    await pc.setLocalDescription(await pc.createAnswer());
    mark('createAnswer');
    if (pc.iceGatheringState !== 'complete') {
      await new Promise(resolve => {
        const timer = setTimeout(resolve, ICE_GATHER_MAX_MS);
        pc.addEventListener('icegatheringstatechange', function check() {
          if (pc.iceGatheringState === 'complete') {
            pc.removeEventListener('icegatheringstatechange', check);
            clearTimeout(timer);
            resolve();
          }
        });
      });
    }
    mark(pc.iceGatheringState === 'complete' ? 'iceGatheringComplete' : 'iceGatheringTimedOut');

    if (currentTargetId !== id) throw new Error('target changed while connecting');
    const ansRes = await post(targetApi('/webrtc/answer'),
      {subscriberId, sdp: pc.localDescription.sdp});
    if (!ansRes.ok) throw new Error((await ansRes.text()) || 'answer failed');
    mark('answerPosted');
    const {candidates = []} = await ansRes.json();
    for (const c of candidates) await pc.addIceCandidate({candidate: c.candidate, sdpMid: c.mid});
    mark('serverCandidatesAdded');

    pc.addEventListener('connectionstatechange', () => {
      if (pc.connectionState === 'connected') {
        mark('pcConnected');
        lastConnectTimings = 'connect: ' + marks.join(' ');
        console.log('HoustonKVM: WebRTC connect timings —', lastConnectTimings);
      }
    });

    webrtcPc = pc;
    activeVideoEl = kvmVideo;
    setShown(kvmImg, false);
    setShown(kvmVideo, true);
    // checkVideoHealth() only watches the MJPEG element and no-ops while
    // WebRTC is active — the banner it uses is repurposed below for live
    // WebRTC diagnostics instead of being hidden.
    startWebrtcStats(pc);
    setWebrtcButton(true);

    // If it hasn't connected by now it won't: go back to the picture that
    // works and say why. Does nothing if the viewer already switched back or
    // moved on (webrtcPc then isn't this connection any more).
    setTimeout(() => {
      if (webrtcPc !== pc || pc.connectionState === 'connected') return;
      console.warn('HoustonKVM: WebRTC did not connect in time', pc.connectionState, pc.iceConnectionState);
      stopWebrtc();
      setCtrlHint('Low-latency video didn’t connect — showing standard video.', true);
    }, WEBRTC_CONNECT_MAX_MS);
  } catch (e) {
    console.error('HoustonKVM: failed to start WebRTC', e);
    stopWebrtc();
    setCtrlHint('Could not start low-latency video: ' + e.message, true);
  }
}

async function stopWebrtc() {
  stopWebrtcStats();
  if (webrtcPc) { webrtcPc.close(); webrtcPc = null; }
  kvmVideo.srcObject = null;
  kvmVideo.muted = true;
  setShown(kvmVideo, false);
  setShown(kvmImg, true);
  activeVideoEl = kvmImg;
  setWebrtcButton(false);
  // Force a fresh health read now that the MJPEG element is active again,
  // rather than showing whatever (possibly stale) state was last known
  // from before WebRTC mode was entered.
  videoAvailable = null;
  document.getElementById('video-status').classList.remove('video-status--diag');
  if (currentTargetId !== null) checkVideoHealth();
}

document.getElementById('webrtc-btn').addEventListener('click', () => {
  if (webrtcPc) stopWebrtc(); else startWebrtc();
});
