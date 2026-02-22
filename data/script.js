/*
  Minimal WebSocket client
  - Receives JSON from ESP32:
      {"type":"led","state":true|false}
      {"type":"time","min":<int>,"sec":<int>}
  - Sends JSON to ESP32 when button is pressed:
      {"type":"button","state":true|false,"browser":{"min":..,"sec":..}}
*/

const gateway = `ws://${window.location.hostname}/ws`;
let websocket = null;

// UI state (mirrors what the ESP broadcasts)
let ledIsOn = false;

window.addEventListener('load', () => {
  initWebSocket();
  initUi();
});

function initUi() {
  const btn = document.getElementById('toggleBtn');
  btn.addEventListener('click', onToggleClicked);
  refreshButtonLabel();
}

function initWebSocket() {
  console.log('[WS] Opening connection ...');
  websocket = new WebSocket(gateway);

  websocket.onopen = () => console.log('[WS] Connected');

  websocket.onclose = () => {
    console.log('[WS] Disconnected - retrying in 2s');
    setTimeout(initWebSocket, 2000);
  };

  websocket.onmessage = (event) => onMessage(event.data);
}

function onMessage(payload) {
  // Prefer JSON
  try {
    const msg = JSON.parse(payload);

    if (msg.type === 'led') {
      ledIsOn = !!msg.state;
      document.getElementById('state').textContent = ledIsOn ? 'ON' : 'OFF';
      refreshButtonLabel();
      return;
    }

    if (msg.type === 'time') {
      const mm = String(msg.min ?? 0).padStart(2, '0');
      const ss = String(msg.sec ?? 0).padStart(2, '0');
      document.getElementById('espTime').textContent = `${mm}:${ss}`;
      return;
    }

    // Unknown JSON message => ignore
    return;
  } catch (e) {
    // Fallback for legacy plain-text messages (if any)
    console.warn('[WS] Non-JSON message:', payload);
  }
}

function refreshButtonLabel() {
  const btn = document.getElementById('toggleBtn');
  btn.textContent = ledIsOn ? 'Turn OFF' : 'Turn ON';
}

function onToggleClicked() {
  if (!websocket || websocket.readyState !== WebSocket.OPEN) {
    console.warn('[WS] Not connected');
    return;
  }

  const desired = !ledIsOn;

  // Read browser time (local)
  const now = new Date();
  const browserMin = now.getMinutes();
  const browserSec = now.getSeconds();

  const msg = {
    type: 'button',
    state: desired,
    browser: {
      min: browserMin,
      sec: browserSec,
    },
  };

  websocket.send(JSON.stringify(msg));
}
