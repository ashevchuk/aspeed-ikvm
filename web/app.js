const serverListEl = document.getElementById('server-list');
const serverDetailEl = document.getElementById('server-detail');
const dialog = document.getElementById('server-form-dialog');
const form = document.getElementById('server-form');

let editingId = null;
let detailPollTimer = null;
let listPollTimer = null;
let currentServerIds = [];
let consoleWs = null;
let consoleModifiers = 0;
let consoleMouseButtons = 0;
let consoleFrameSeq = 0;

// KeyboardEvent.code -> USB HID Keyboard/Keypad usage ID (standard HID
// Usage Tables, page 0x07 - not AMI-specific). Matches what the BMC's USB
// HID keyboard report expects, see IkvmSession::send_key_report.
const USB_HID_KEYCODES = {
  KeyA: 4, KeyB: 5, KeyC: 6, KeyD: 7, KeyE: 8, KeyF: 9, KeyG: 10, KeyH: 11, KeyI: 12, KeyJ: 13,
  KeyK: 14, KeyL: 15, KeyM: 16, KeyN: 17, KeyO: 18, KeyP: 19, KeyQ: 20, KeyR: 21, KeyS: 22, KeyT: 23,
  KeyU: 24, KeyV: 25, KeyW: 26, KeyX: 27, KeyY: 28, KeyZ: 29,
  Digit1: 30, Digit2: 31, Digit3: 32, Digit4: 33, Digit5: 34, Digit6: 35, Digit7: 36, Digit8: 37, Digit9: 38, Digit0: 39,
  Enter: 40, Escape: 41, Backspace: 42, Tab: 43, Space: 44,
  Minus: 45, Equal: 46, BracketLeft: 47, BracketRight: 48, Backslash: 49,
  Semicolon: 51, Quote: 52, Backquote: 53, Comma: 54, Period: 55, Slash: 56,
  CapsLock: 57,
  F1: 58, F2: 59, F3: 60, F4: 61, F5: 62, F6: 63, F7: 64, F8: 65, F9: 66, F10: 67, F11: 68, F12: 69,
  PrintScreen: 70, ScrollLock: 71, Pause: 72,
  Insert: 73, Home: 74, PageUp: 75, Delete: 76, End: 77, PageDown: 78,
  ArrowRight: 79, ArrowLeft: 80, ArrowDown: 81, ArrowUp: 82,
  NumLock: 83,
  NumpadDivide: 84, NumpadMultiply: 85, NumpadSubtract: 86, NumpadAdd: 87, NumpadEnter: 88,
  Numpad1: 89, Numpad2: 90, Numpad3: 91, Numpad4: 92, Numpad5: 93, Numpad6: 94, Numpad7: 95, Numpad8: 96, Numpad9: 97, Numpad0: 98,
  NumpadDecimal: 99,
};

// Bit layout of the USB HID boot-keyboard report's modifier byte.
const USB_HID_MODIFIER_BITS = {
  ControlLeft: 0x01, ShiftLeft: 0x02, AltLeft: 0x04, MetaLeft: 0x08,
  ControlRight: 0x10, ShiftRight: 0x20, AltRight: 0x40, MetaRight: 0x80,
};

const POWER_ACTION_LABELS = {
  on: 'Power on',
  off: 'Power off',
  cycle: 'Power cycle',
  reset: 'Hard reset',
  'soft-shutdown': 'Soft shutdown',
};

async function api(path, options) {
  const res = await fetch(path, options);
  if (!res.ok) {
    let message = res.statusText;
    try { message = (await res.json()).error || message; } catch (_) {}
    throw new Error(message);
  }
  if (res.status === 204) return null;
  return res.json();
}

function tagsHtml(tags) {
  return (tags || []).map(t => `<span class="tag">${escapeHtml(t)}</span>`).join('');
}

function escapeHtml(s) {
  return String(s).replace(/[&<>"']/g, c => ({
    '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;'
  }[c]));
}

function powerButtonsHtml() {
  return Object.entries(POWER_ACTION_LABELS).map(([action, label]) =>
    `<button class="${action === 'on' ? '' : 'secondary'}" data-action="${action}">${label}</button>`
  ).join('');
}

async function loadServerList() {
  stopDetailPolling();
  stopListPolling();
  closeConsole();
  serverDetailEl.hidden = true;
  serverListEl.hidden = false;
  serverListEl.innerHTML = '<p>Loading…</p>';

  let servers;
  try {
    servers = await api('/api/servers');
  } catch (e) {
    serverListEl.innerHTML = `<p class="error">${escapeHtml(e.message)}</p>`;
    return;
  }

  if (servers.length === 0) {
    serverListEl.innerHTML = '<p>No servers yet. Add one to get started.</p>';
    return;
  }

  currentServerIds = servers.map(s => s.id);

  serverListEl.innerHTML = servers.map(s => `
    <div class="card server-card" data-id="${s.id}">
      <div class="server-row">
        <div>
          <strong class="server-open" style="cursor:pointer">${escapeHtml(s.name)}</strong>
          <div class="meta">${escapeHtml(s.host)} ${tagsHtml(s.tags)}</div>
        </div>
        <div class="actions">
          <span class="badge unknown" data-badge="${s.id}">…</span>
          <button class="secondary edit-btn">Edit</button>
          <button class="danger delete-btn">Delete</button>
        </div>
      </div>
      <div class="power-actions" data-id="${s.id}">
        ${powerButtonsHtml()}
      </div>
    </div>
  `).join('');

  serverListEl.querySelectorAll('.server-card').forEach(card => {
    const id = card.dataset.id;
    const server = servers.find(s => s.id === id);
    card.querySelector('.server-open').addEventListener('click', () => openServerDetail(server));
    card.querySelector('.edit-btn').addEventListener('click', () => openForm(server));
    card.querySelector('.delete-btn').addEventListener('click', async () => {
      if (!confirm(`Delete ${server.name}?`)) return;
      await api(`/api/servers/${id}`, { method: 'DELETE' });
      loadServerList();
    });
    card.querySelectorAll('[data-action]').forEach(btn => {
      btn.addEventListener('click', () => runPowerAction(id, btn.dataset.action));
    });
  });

  currentServerIds.forEach(id => refreshPowerStatus(id));
  startListPolling();
}

function startListPolling() {
  stopListPolling();
  listPollTimer = setInterval(() => currentServerIds.forEach(id => refreshPowerStatus(id)), 5000);
}

function stopListPolling() {
  if (listPollTimer) {
    clearInterval(listPollTimer);
    listPollTimer = null;
  }
}

function openForm(server) {
  editingId = server ? server.id : null;
  document.getElementById('server-form-title').textContent = server ? 'Edit server' : 'Add server';
  form.name.value = server ? server.name : '';
  form.host.value = server ? server.host : '';
  form.username.value = server ? server.username : '';
  form.password.value = '';
  form.tags.value = server ? (server.tags || []).join(', ') : '';
  dialog.showModal();
}

document.getElementById('add-server-btn').addEventListener('click', () => openForm(null));
document.getElementById('server-form-cancel').addEventListener('click', () => dialog.close());

form.addEventListener('submit', async (e) => {
  const body = {
    name: form.name.value.trim(),
    host: form.host.value.trim(),
    username: form.username.value.trim(),
    password: form.password.value,
    tags: form.tags.value.split(',').map(t => t.trim()).filter(Boolean),
  };

  const path = editingId ? `/api/servers/${editingId}` : '/api/servers';
  const method = editingId ? 'PUT' : 'POST';
  await api(path, { method, headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) });
  dialog.close();
  loadServerList();
});

async function openServerDetail(server) {
  stopListPolling();
  serverListEl.hidden = true;
  serverDetailEl.hidden = false;
  serverDetailEl.innerHTML = `
    <a class="back-link">&larr; Back to servers</a>
    <h2>${escapeHtml(server.name)} <span class="meta">${escapeHtml(server.host)}</span></h2>
    <div class="card">
      <div class="server-row">
        <div>
          Power status: <span class="badge unknown" data-badge="${server.id}">checking…</span>
          <div class="power-note" data-id="${server.id}"></div>
        </div>
        <div class="power-actions" data-id="${server.id}">
          ${powerButtonsHtml()}
        </div>
      </div>
    </div>
    <div class="card">
      <h3>Console</h3>
      <div class="console-actions">
        <button class="secondary" data-hold="ControlLeft">Left Ctrl</button>
        <button class="secondary" data-hold="ControlRight">Right Ctrl</button>
        <button class="secondary" data-hold="AltLeft">Left Alt</button>
        <button class="secondary" data-hold="AltRight">Right Alt</button>
        <button class="secondary" id="console-ctrl-alt-del">Ctrl+Alt+Del</button>
        <button class="secondary" id="console-win-key">Win key</button>
        <label style="margin-left:auto;flex-direction:row;align-items:center;gap:0.35rem;margin-bottom:0;font-size:0.8rem">
          <input type="checkbox" id="console-compress"> Compress
        </label>
        <input type="number" id="console-quality" value="75" min="1" max="100" disabled style="width:3.5rem">
        <button class="secondary" id="console-toggle">Open console</button>
      </div>
      <div class="console-wrap">
        <canvas id="console-canvas" width="640" height="480" tabindex="0"></canvas>
      </div>
      <p class="power-note" id="console-note">Click the console to send it keyboard input.</p>
    </div>
    <div class="card">
      <h3>Sensors</h3>
      <div id="sensors-area"><p>Loading…</p></div>
    </div>
    <div class="card">
      <h3>System Event Log</h3>
      <div id="sel-area"><p>Loading…</p></div>
    </div>
  `;

  serverDetailEl.querySelector('.back-link').addEventListener('click', loadServerList);
  serverDetailEl.querySelectorAll('[data-action]').forEach(btn => {
    btn.addEventListener('click', () => runPowerAction(server.id, btn.dataset.action));
  });
  serverDetailEl.querySelector('#console-toggle').addEventListener('click', () => toggleConsole(server.id));
  serverDetailEl.querySelector('#console-compress').addEventListener('change', (ev) => {
    serverDetailEl.querySelector('#console-quality').disabled = !ev.target.checked;
  });
  const consoleCanvas = serverDetailEl.querySelector('#console-canvas');
  consoleCanvas.addEventListener('keydown', onConsoleKeyEvent);
  consoleCanvas.addEventListener('keyup', onConsoleKeyEvent);
  consoleCanvas.addEventListener('mousedown', onConsoleMouseEvent);
  consoleCanvas.addEventListener('mouseup', onConsoleMouseEvent);
  consoleCanvas.addEventListener('mousemove', onConsoleMouseEvent);
  consoleCanvas.addEventListener('contextmenu', (ev) => ev.preventDefault());
  serverDetailEl.querySelector('#console-ctrl-alt-del').addEventListener('click', sendCtrlAltDel);
  serverDetailEl.querySelector('#console-win-key').addEventListener('click', sendWinKey);
  serverDetailEl.querySelectorAll('[data-hold]').forEach(btn => {
    btn.addEventListener('click', () => toggleHoldModifier(btn));
  });

  refreshPowerStatus(server.id);
  refreshSensors(server.id);
  refreshSel(server.id);
  startDetailPolling(server.id);
}

function startDetailPolling(id) {
  stopDetailPolling();
  detailPollTimer = setInterval(() => refreshPowerStatus(id), 5000);
}

function stopDetailPolling() {
  if (detailPollTimer) {
    clearInterval(detailPollTimer);
    detailPollTimer = null;
  }
}

function applyPowerBadge(id, text, cls, title) {
  document.querySelectorAll(`[data-badge="${id}"]`).forEach(badge => {
    badge.textContent = text;
    badge.className = 'badge ' + cls;
    if (title) badge.title = title;
    else badge.removeAttribute('title');
  });
}

async function refreshPowerStatus(id) {
  try {
    const status = await api(`/api/servers/${id}/power`);
    applyPowerBadge(id, status.power_on ? 'ON' : 'OFF', status.power_on ? 'on' : 'off');
  } catch (e) {
    applyPowerBadge(id, 'error', 'unknown', e.message);
  }
}

function setPowerButtonsBusy(id, busy, activeAction) {
  document.querySelectorAll(`.power-actions[data-id="${id}"] [data-action]`).forEach(btn => {
    btn.disabled = busy;
    const isActive = busy && btn.dataset.action === activeAction;
    btn.classList.toggle('busy', isActive);
    btn.textContent = isActive ? `${POWER_ACTION_LABELS[activeAction]}…` : POWER_ACTION_LABELS[btn.dataset.action];
  });
}

function setPowerNote(id, text, kind) {
  document.querySelectorAll(`.power-note[data-id="${id}"]`).forEach(note => {
    note.textContent = text;
    note.className = kind ? `power-note ${kind}` : 'power-note';
  });
}

async function runPowerAction(id, action) {
  setPowerButtonsBusy(id, true, action);
  setPowerNote(id, '', '');

  try {
    await api(`/api/servers/${id}/power`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ action }),
    });
    setPowerNote(id, `${POWER_ACTION_LABELS[action]} sent.`, 'ok');
    await new Promise(r => setTimeout(r, 1500));
    await refreshPowerStatus(id);
  } catch (e) {
    setPowerNote(id, `${POWER_ACTION_LABELS[action]} failed: ${e.message}`, 'error');
  } finally {
    setPowerButtonsBusy(id, false, null);
  }
}

function consoleNote(text, kind) {
  const note = document.getElementById('console-note');
  if (!note) return;
  note.textContent = text;
  note.className = kind ? `power-note ${kind}` : 'power-note';
}

function toggleConsole(id) {
  if (consoleWs) {
    closeConsole();
  } else {
    openConsole(id);
  }
}

const CONSOLE_MAX_RECONNECT_ATTEMPTS = 3;
let consoleReconnectTimer = null;
let consoleReconnectAttempts = 0;
let consoleClosedByUser = false;

function clearConsoleReconnect() {
  if (consoleReconnectTimer) {
    clearTimeout(consoleReconnectTimer);
    consoleReconnectTimer = null;
  }
}

// Wire format: format byte (0 = raw RGB24, 1 = JPEG), dest_width (u16 LE),
// dest_height (u16 LE), payload - see ikvm_bridge.hpp.
const FRAME_FORMAT_JPEG = 1;

function openConsole(id) {
  clearConsoleReconnect();
  consoleClosedByUser = false;

  const canvas = document.getElementById('console-canvas');
  const ctx = canvas.getContext('2d');
  const scheme = location.protocol === 'https:' ? 'wss:' : 'ws:';
  const compress = document.getElementById('console-compress').checked;
  const quality = document.getElementById('console-quality').value;
  const query = compress ? `?compress=1&quality=${quality}` : '';
  const ws = new WebSocket(`${scheme}//${location.host}/api/servers/${id}/console${query}`);
  ws.binaryType = 'arraybuffer';
  consoleWs = ws;
  consoleModifiers = 0;
  document.querySelectorAll('[data-hold]').forEach(b => b.classList.remove('active'));
  document.getElementById('console-toggle').textContent = 'Close console';
  consoleNote('Connecting…', '');

  ws.addEventListener('open', () => {
    consoleReconnectAttempts = 0;
    consoleNote('Connected.', 'ok');
    canvas.focus();
  });

  ws.addEventListener('message', (ev) => {
    const seq = ++consoleFrameSeq;
    const frame = new Uint8Array(ev.data);
    const format = frame[0];
    const width = frame[1] | (frame[2] << 8);
    const height = frame[3] | (frame[4] << 8);
    const payload = frame.subarray(5);

    const resizeCanvas = () => {
      if (canvas.width !== width || canvas.height !== height) {
        canvas.width = width;
        canvas.height = height;
      }
    };

    if (format === FRAME_FORMAT_JPEG) {
      // Decoding is async, so a slower-to-decode frame can finish after a
      // newer one already drew - consoleFrameSeq drops it instead of
      // flashing stale video back onto the canvas.
      createImageBitmap(new Blob([payload], { type: 'image/jpeg' })).then((bitmap) => {
        if (seq !== consoleFrameSeq) return;
        resizeCanvas();
        ctx.drawImage(bitmap, 0, 0);
        bitmap.close();
      });
      return;
    }

    resizeCanvas();
    const image = ctx.createImageData(width, height);
    for (let src = 0, dst = 0; src < payload.length; src += 3, dst += 4) {
      image.data[dst] = payload[src];
      image.data[dst + 1] = payload[src + 1];
      image.data[dst + 2] = payload[src + 2];
      image.data[dst + 3] = 255;
    }
    ctx.putImageData(image, 0, 0);
  });

  ws.addEventListener('close', (ev) => {
    consoleWs = null;
    const toggleBtn = document.getElementById('console-toggle');
    if (toggleBtn) toggleBtn.textContent = 'Open console';

    if (!consoleClosedByUser && consoleReconnectAttempts < CONSOLE_MAX_RECONNECT_ATTEMPTS) {
      consoleReconnectAttempts++;
      consoleNote(`Connection lost, reconnecting… (${consoleReconnectAttempts}/${CONSOLE_MAX_RECONNECT_ATTEMPTS})`,
                  'error');
      consoleReconnectTimer = setTimeout(() => openConsole(id), 2000);
      return;
    }
    consoleNote(ev.reason || 'Disconnected.', ev.wasClean ? '' : 'error');
  });

  ws.addEventListener('error', () => consoleNote('Connection error.', 'error'));
}

// Only the 'close' event handler clears consoleWs / resets the button - it's
// the single point of truth for "is the socket actually gone". Clearing it
// here too would race: a click right after close() started (before the
// close handshake finishes) would see consoleWs already null and reopen a
// second connection on top of the one still shutting down.
function closeConsole() {
  consoleClosedByUser = true;
  clearConsoleReconnect();
  if (consoleWs) consoleWs.close();
}

// Inbound message: type byte (1 - the only one defined so far), modifiers
// bitmask, keycode - see ikvm_bridge.hpp.
function sendKeyReport(modifiers, keycode) {
  if (!consoleWs || consoleWs.readyState !== WebSocket.OPEN) return;
  consoleWs.send(new Uint8Array([1, modifiers, keycode]));
}

// Mirrors the real JViewer client (USBKeyProcessor.convertKeyCode): a
// modifier key only ever updates the modifier bitmask and reports keycode 0;
// an ordinary key reports its code on keydown and 0 (nothing held) on keyup
// - never more than one ordinary key at a time.
function onConsoleKeyEvent(ev) {
  const modifierBit = USB_HID_MODIFIER_BITS[ev.code];
  if (modifierBit !== undefined) {
    ev.preventDefault();
    consoleModifiers = ev.type === 'keydown' ? (consoleModifiers | modifierBit) : (consoleModifiers & ~modifierBit);
    sendKeyReport(consoleModifiers, 0);
    return;
  }

  const keycode = USB_HID_KEYCODES[ev.code];
  if (keycode === undefined) return;
  ev.preventDefault();
  sendKeyReport(consoleModifiers, ev.type === 'keydown' ? keycode : 0);
}

const MOUSE_BUTTON_BITS = { 0: 0x01, 2: 0x02, 1: 0x04 };  // DOM button -> left/right/middle

// Inbound message: type byte (2), buttons bitmask, x (u16 LE), y (u16 LE) -
// both pre-scaled to the device's 0..32767 absolute range - see
// ikvm_bridge.hpp.
function sendMouseReport(buttons, x, y) {
  if (!consoleWs || consoleWs.readyState !== WebSocket.OPEN) return;
  const msg = new Uint8Array(6);
  msg[0] = 2;
  msg[1] = buttons;
  msg[2] = x & 0xFF;
  msg[3] = (x >> 8) & 0xFF;
  msg[4] = y & 0xFF;
  msg[5] = (y >> 8) & 0xFF;
  consoleWs.send(msg);
}

function onConsoleMouseEvent(ev) {
  ev.preventDefault();
  const canvas = ev.currentTarget;
  // preventDefault() above also suppresses the browser's own
  // click-to-focus behavior, so it has to be done explicitly here.
  if (ev.type === 'mousedown') canvas.focus();
  const rect = canvas.getBoundingClientRect();
  const x = (ev.clientX - rect.left) / rect.width;
  const y = (ev.clientY - rect.top) / rect.height;
  if (x < 0 || y < 0 || x > 1 || y > 1) return;

  const bit = MOUSE_BUTTON_BITS[ev.button];
  if (ev.type === 'mousedown' && bit !== undefined) consoleMouseButtons |= bit;
  else if (ev.type === 'mouseup' && bit !== undefined) consoleMouseButtons &= ~bit;

  sendMouseReport(consoleMouseButtons, Math.round(x * 32767), Math.round(y * 32767));
}

// Neither combination can be captured as an actual keypress: the browser/OS
// intercepts Ctrl+Alt+Del before any page sees it, and tapping the GUI/Win
// key alone never fires a 'keydown' with a companion code to catch it by.
// Both have to be synthesized directly as HID reports instead.
async function sendCtrlAltDel() {
  if (!consoleWs) return;
  const ctrlAlt = USB_HID_MODIFIER_BITS.ControlLeft | USB_HID_MODIFIER_BITS.AltLeft;
  sendKeyReport(ctrlAlt, 0);
  await new Promise(r => setTimeout(r, 60));
  sendKeyReport(ctrlAlt, USB_HID_KEYCODES.Delete);
  await new Promise(r => setTimeout(r, 60));
  sendKeyReport(ctrlAlt, 0);
  await new Promise(r => setTimeout(r, 60));
  sendKeyReport(0, 0);
}

async function sendWinKey() {
  if (!consoleWs) return;
  sendKeyReport(USB_HID_MODIFIER_BITS.MetaLeft, 0);
  await new Promise(r => setTimeout(r, 60));
  sendKeyReport(0, 0);
}

// Mirrors the real JViewer client's "Hold Left/Right Ctrl/Alt Key" menu
// items: a modifier that stays down across separate keypresses, for
// combos the BMC console needs but a real keyboard sends concurrently (and
// a mouse click doesn't). Click again to release it.
function toggleHoldModifier(btn) {
  const bit = USB_HID_MODIFIER_BITS[btn.dataset.hold];
  const held = (consoleModifiers & bit) !== 0;
  consoleModifiers = held ? (consoleModifiers & ~bit) : (consoleModifiers | bit);
  btn.classList.toggle('active', !held);
  sendKeyReport(consoleModifiers, 0);
}

const CATEGORY_LABELS = {
  temperature: 'Temperatures',
  voltage: 'Voltages',
  current: 'Current',
  fan: 'Fans',
  power_supply: 'Power Supplies',
  power_unit: 'Power',
  processor: 'Processors',
  memory: 'Memory',
  drive_slot: 'Drives',
  system_event: 'System Events',
  watchdog: 'Watchdog',
  other: 'Other',
};

const STATE_LABELS = { nominal: 'Nominal', warning: 'Warning', critical: 'Critical', unknown: 'Unknown' };

function formatSensorValue(s) {
  if (!s.value && s.value !== 0) return '—';
  const decimals = (s.units === 'RPM' || s.units === '%') ? 0 : 1;
  return s.value.toFixed(decimals);
}

// Meter range comes only from thresholds the BMC itself reports (see
// SdrThresholds in ipmi_monitor.cpp) - never a guessed/padded range. Sensors
// with no usable threshold pair get no meter, just the plain value + badge.
function meterPercent(s) {
  if (s.value === undefined) return null;

  let min, max;
  if (s.lower_critical !== undefined && s.upper_critical !== undefined) {
    min = s.lower_critical;
    max = s.upper_critical;
  } else if (s.upper_critical !== undefined) {
    min = 0;  // temperature/RPM sensors: 0 is a real physical floor, not a guess
    max = s.upper_critical;
  } else {
    return null;
  }

  if (max <= min) return null;
  return Math.max(0, Math.min(100, ((s.value - min) / (max - min)) * 100));
}

async function refreshSensors(id) {
  const area = document.getElementById('sensors-area');
  try {
    const sensors = await api(`/api/servers/${id}/sensors`);
    if (sensors.length === 0) {
      area.innerHTML = '<p>No sensors reported.</p>';
      return;
    }

    const byCategory = new Map();
    sensors.forEach(s => {
      if (!byCategory.has(s.category)) byCategory.set(s.category, []);
      byCategory.get(s.category).push(s);
    });

    area.innerHTML = Array.from(byCategory.entries()).map(([category, items]) => `
      <section class="sensor-group">
        <h4>${escapeHtml(CATEGORY_LABELS[category] || category)}</h4>
        <div class="stat-grid">
          ${items.map(s => {
            const pct = meterPercent(s);
            return `
            <div class="stat-tile" data-state="${s.state}">
              <div class="stat-label">${escapeHtml(s.name)}</div>
              <div class="stat-value">${formatSensorValue(s)}<span class="stat-unit">${escapeHtml(s.units)}</span></div>
              ${pct !== null ? `<div class="meter"><div class="meter-fill" style="width:${pct.toFixed(1)}%"></div></div>` : ''}
              <span class="badge ${s.state}">${STATE_LABELS[s.state] || s.state}</span>
            </div>
          `;
          }).join('')}
        </div>
      </section>
    `).join('');
  } catch (e) {
    area.innerHTML = `<p class="error">${escapeHtml(e.message)}</p>`;
  }
}

async function refreshSel(id) {
  const area = document.getElementById('sel-area');
  try {
    const entries = await api(`/api/servers/${id}/sel?limit=50`);
    if (entries.length === 0) {
      area.innerHTML = '<p>No SEL entries.</p>';
      return;
    }
    area.innerHTML = `
      <table>
        <thead><tr><th>Time</th><th>Type</th><th>Description</th></tr></thead>
        <tbody>
          ${entries.map(e => `
            <tr>
              <td>${e.timestamp ? new Date(e.timestamp * 1000).toLocaleString() : '—'}</td>
              <td>${escapeHtml(e.sensor_type)}</td>
              <td>${escapeHtml(e.description)}</td>
            </tr>
          `).join('')}
        </tbody>
      </table>
    `;
  } catch (e) {
    area.innerHTML = `<p class="error">${escapeHtml(e.message)}</p>`;
  }
}

loadServerList();
