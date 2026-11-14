(() => {
  "use strict";

  const config = window.WEBRTC_CONFIG || {};
  const $ = id => document.getElementById(id);

  const statusEl = $("status");
  const logEl = $("log");
  const sessionEl = $("session");
  const connectBtn = $("connectBtn");
  const disconnectBtn = $("disconnectBtn");
  const sendBtn = $("sendBtn");
  const sendInput = $("messageInput");
  const signalingUrl = $("signalingUrl");

  let ws = null;
  let pc = null;
  let dc = null;
  let myId = null;
  let peerId = null;
  let role = null;
  let remoteReady = false;
  let pendingCandidates = [];
  let closedManually = false;

  function log(text) {
    logEl.textContent +=
      `[${new Date().toLocaleTimeString()}] ${text}\n`;
    logEl.scrollTop = logEl.scrollHeight;
  }

  function status(text) {
    statusEl.textContent = text;
  }

  function updateUI() {
    const open = dc?.readyState === "open";
    sendBtn.disabled = !open
    connectBtn.disabled = !!ws && ws.readyState < WebSocket.CLOSING;
    disconnectBtn.disabled = !ws;
  }

  function signal(message) {
    if (!ws || ws.readyState !== WebSocket.OPEN) {
      throw new Error("Signaling WebSocket is not open");
    }
    ws.send(JSON.stringify(message));
  }

  function sendToPeer(message) {
    if (!peerId) {
      log("Peer ID is not assigned yet");
      return;
    }
    signal({ ...message, to: peerId });
  }

  async function flushCandidates() {
    while (pendingCandidates.length && remoteReady) {
      const candidate = pendingCandidates.shift();
      await pc.addIceCandidate(candidate);
    }
  }

  function attachDataChannel(channel) {
    dc = channel;
    dc.binaryType = "arraybuffer";

    dc.onopen = () => {
      log("P2P DataChannel opened");
      status("P2P connected");
      updateUI();
    };

    dc.onclose = () => {
      log("DataChannel closed");
      updateUI();
    };

    dc.onerror = e => log(`DataChannel error: ${e.message || "unknown"}`);

    dc.onmessage = event => {
      if (typeof event.data === "string") {
        log(`Received from peer: ${event.data}`);
      } else {
        const size = event.data.byteLength ?? event.data.size ?? 0;
        log(`Received binary data: ${size} bytes`);
      }
    };

    updateUI();
  }

  async function createPeerConnection() {
    if (pc) return;

    pc = new RTCPeerConnection({
      iceServers: config.iceServers || []
    });

    pc.onicecandidate = event => {
      if (!event.candidate || !peerId) return;

      try {
        sendToPeer({
          type: "candidate",
          candidate: event.candidate.candidate,
          sdpMid: event.candidate.sdpMid,
          sdpMLineIndex: event.candidate.sdpMLineIndex
        });
      } catch (e) {
        log(`ICE send failed: ${e.message}`);
      }
    };

    pc.onconnectionstatechange = () => {
      log(`PeerConnection: ${pc.connectionState}`);
      status(pc.connectionState);
      if (pc.connectionState === "failed") {
        log("P2P failed. A TURN server may be required.");
      }
    };

    pc.oniceconnectionstatechange = () => {
      log(`ICE: ${pc.iceConnectionState}`);
    };

    pc.ondatachannel = event => {
      log(`Incoming DataChannel: ${event.channel.label}`);
      attachDataChannel(event.channel);
    };

    if (role === "initiator") {
      attachDataChannel(pc.createDataChannel("data", {
        ordered: true
      }));
    }
  }

  async function makeOffer() {
    await createPeerConnection();

    const offer = await pc.createOffer();
    await pc.setLocalDescription(offer);

    sendToPeer({
      type: "offer",
      sdp: pc.localDescription.sdp
    });

    log(`Offer sent to ${peerId}`);
  }

  async function handleSignal(message) {

    log(`WS: ${JSON.stringify(message)}`);

    switch (message.type) {

      case "joined":
        myId = message.id;
        role = message.role;

        sessionEl.textContent =
          `ID: ${myId} | Room: room1 | Role: ${role}`;

        log(`Joined as ${myId}; role=${role}`);

        if (role === "responder") {
          peerId = message.peerId;
          await createPeerConnection();
        }
        break;

      case "peer-joined":
        peerId = message.peerId;
        log(`Peer joined: ${message.peerId}`);

        if (role === "initiator") {
          await makeOffer();
        }
        break;

      case "offer": {
        if (!pc) await createPeerConnection();

        await pc.setRemoteDescription({
          type: "offer",
          sdp: message.sdp
        });

        remoteReady = true;
        await flushCandidates();

        const answer = await pc.createAnswer();
        await pc.setLocalDescription(answer);

        sendToPeer({
          type: "answer",
          sdp: pc.localDescription.sdp
        });

        log("Answer sent");
        break;
      }

      case "answer":
        if (!pc) throw new Error("Answer received without PeerConnection");

        await pc.setRemoteDescription({
          type: "answer",
          sdp: message.sdp
        });

        remoteReady = true;
        await flushCandidates();

        log("Remote answer applied");
        break;

      case "candidate": {
        if (!pc) {
          log("ICE candidate received before PeerConnection");
          break;
        }

        const candidate = new RTCIceCandidate({
          candidate: message.candidate,
          sdpMid: message.sdpMid ?? null,
          sdpMLineIndex: message.sdpMLineIndex ?? null
        });

        if (remoteReady) {
          log("Remote is ready");
          await pc.addIceCandidate(candidate);
        } else {
          log("Remote is NOT  ready");
          pendingCandidates.push(candidate);
        }
        break;
      }

      case "peer-left":
        log("Peer disconnected");
        status("Peer disconnected");
        cleanupPeer();
        break;

      case "error":
        log(`Server error: ${message.message}`);
        break;

      default:
        log(`Unknown signaling message: ${message.type}`);
    }
  }

  function connect() {
    const host = signalingUrl.value;

    const url = `wss://${host}:443/ws`;
    config.iceServers = [
      { urls: `stun:${host}:3478` }
    ];
    log(`Trying to connect to ${host}`);


    if (!url || !url.startsWith("ws")) {
      log("Set signalingUrl in web/config.js");
      return;
    }

    closedManually = false;
    status("Connecting signaling...");
    ws = new WebSocket(url);

    ws.onopen = () => {
      log("Signaling connected");
      signal({ type: "join" });
    };

    ws.onmessage = async event => {
      try {
        await handleSignal(JSON.parse(event.data));
      } catch (e) {
        log(`Signaling error: ${e.message}`);
      }
    };

    ws.onerror = () => log("WebSocket error");

    ws.onclose = () => {
      log("Signaling disconnected");
      if (!closedManually) status("Signaling disconnected");
      cleanupPeer();
      ws = null;
      updateUI();
    };

    updateUI();
  }

  function cleanupPeer() {
    if (dc) {
      try { dc.close(); } catch { }
    }
    if (pc) {
      try { pc.close(); } catch { }
    }

    dc = null;
    pc = null;
    peerId = null;
    remoteReady = false;
    pendingCandidates = [];
    updateUI();
  }

  function disconnect() {
    closedManually = true;
    cleanupPeer();

    if (ws && ws.readyState < WebSocket.CLOSING) {
      ws.close(1000, "client disconnect");
    }

    ws = null;
    myId = null;
    role = null;
    sessionEl.textContent = "Session: —";
    status("Disconnected");
    updateUI();
  }

  $("sendForm").onsubmit = event => {
    event.preventDefault();
    const input = $("messageInput");

    log(`Sent to peer: ${input.value}`);

    if (dc && dc.readyState === "open") {
      dc.send(input.value);
    }

    input.value = "";
  };

  connectBtn.addEventListener("click", connect);
  disconnectBtn.addEventListener("click", disconnect);

  updateUI();
})();