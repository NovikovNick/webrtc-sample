// Set this to your public signaling endpoint.
// GitHub Pages is HTTPS, therefore use wss:// (not ws://) in production.
window.WEBRTC_CONFIG = {
  signalingUrl: "ws://localhost:8080/ws",
  iceServers: [
    { urls: "stun:stun.l.google.com:19302" }
  ]
};
