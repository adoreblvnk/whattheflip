import van from "vanjs-core";

const { div, span, button, a, table, thead, tbody, tr, th, td, pre, strong, input } = van.tags;
const svgNs = van.tags("http://www.w3.org/2000/svg");
const { svg, g, rect, line, path, text, polyline } = svgNs;
// ===========================================================================
// 1. Hardware, Telemetry & CTF State (Zero Hardcoded Flags)
// ===========================================================================
const safetyState        = van.state("Active"); // "Active" | "Stopped"
const toastMsg           = van.state("");
const isPaused           = van.state(false);
const activeGlitchBus    = van.state(""); // "" | "UART" | "I2C_ACK" | "I2C_STRETCH" | "WIFI" | "ZIGBEE"
const lastUartGlitch     = van.state("");
const lastI2cFault       = van.state("");
const isReplayMode       = van.state(false);
const wifiScanningUi     = van.state(false);
const zigbeeScanningUi   = van.state(false);

const toolMode           = van.state("OFF"); // "OFF" | "AUTO" | "UART" | "I2C" | "WIFI" | "ZIGBEE"
const uartBaud           = van.state(115200);
const uartClear          = van.state(false);
const uartSample         = van.state("");

const wifiChannelMode    = van.state(0);
const wifiActiveCh       = van.state(6);
const wifiClear          = van.state(false);
const wifiSampleCh       = van.state(0);
const wifiSample         = van.state("");

const zigbeeChannel      = van.state(0);
const z154Clear          = van.state(false);
const z154Sample         = van.state("");
// Live Telemetry Counters
const uartPkts           = van.state(0);
const i2cPkts            = van.state(0);
const wifiPkts           = van.state(0);
const zigbeePkts         = van.state(0);
const corruptedCount     = van.state(0);
const pcapSize           = van.state(24);
const pcapSyncedBytes    = van.state(0);

// Hardware-Captured Messages (populated strictly from live telemetry / PCAP)
const ctfUart            = van.state("");
const ctfWifi            = van.state("");
const ctfZigbee          = van.state("");
const ctfBonus           = van.state("");
const ctfImask           = van.state(0);
const ctfEmask           = van.state(0);
// Iris Proximity & Sensor State
const irisRssi           = van.state(null);
const irisVerified       = van.state(false);
const accelX             = van.state(null);
const accelY             = van.state(null);
const accelZ             = van.state(null);
const accelAvgX          = van.state(0);
const magStr             = van.state("");
const animPhase          = van.state(0);

// Real-Traffic Wire Activity Windows (only light up when real packets flow)
const uartActiveUntil    = van.state(0);
const i2cActiveUntil     = van.state(0);
const wifiActiveUntil    = van.state(0);
const zigbeeActiveUntil  = van.state(0);
const i2cValueChangedUntil = van.state(0);
const latestUartSent     = van.state("");
const latestUartRecv     = van.state("");

// Bottom Packet Table State
const filterProtocol     = van.state("ALL");
const faultsOnly         = van.state(false);
const filterSearchText   = van.state("");
const sortColumn         = van.state("id");
const sortAscending      = van.state(false);
const hideRepeatingNormal = van.state(false);
const showRawHex         = van.state(false);
const selectedPacket     = van.state(null);
const packets            = van.state([]);

// Instant Floating Tooltip State
const hoverTip           = van.state(null); // { text, x, y }

let clientPacketSeq      = 0;
let xFilterRing          = [];
let prevUartPkts         = -1;
let prevI2cPkts          = -1;
let prevWifiPkts         = -1;
let prevZigbeePkts       = -1;

// ===========================================================================
// 2. Crisp Stroke SVG Icons (No Emojis)
// ===========================================================================
function iconSvg(dPath, cls = "ico") {
  return svg({ class: cls, viewBox: "0 0 24 24", fill: "none", stroke: "currentColor", "stroke-width": 1.85, "stroke-linecap": "round", "stroke-linejoin": "round" },
    path({ d: dPath })
  );
}

const ICONS = {
  power:   () => iconSvg("M18.36 6.64a9 9 0 1 1-12.73 0 M12 2v10"),
  radar:   () => iconSvg("M12 12m-3 0a3 3 0 1 0 6 0a3 3 0 1 0-6 0 M12 2a10 10 0 0 1 10 10 M12 6a6 6 0 0 1 6 6"),
  zap:     () => iconSvg("M13 2L3 14h9l-1 8 10-12h-9l1-8z"),
  cpu:     () => iconSvg("M4 4h16v16H4z M9 9h6v6H9z M9 1v3 M15 1v3 M9 20v3 M15 20v3 M1 9h3 M1 15h3 M20 9h3 M20 15h3"),
  wifi:    () => iconSvg("M5 12.55a11 11 0 0 1 14.08 0 M1.42 9a16 16 0 0 1 21.16 0 M8.53 16.11a6 6 0 0 1 6.95 0 M12 20h.01"),
  radio:   () => iconSvg("M4.9 19.1C1 15.2 1 8.8 4.9 4.9 M7.8 16.2c-2.3-2.3-2.3-6.1 0-8.5 M16.2 7.8c2.3 2.3 2.3 6.1 0 8.5 M19.1 4.9C23 8.8 23 15.2 19.1 19.1 M12 12h.01"),
  trash:   () => iconSvg("M3 6h18 M8 6V4h8v2 M19 6l-1 14H6L5 6"),
  stop:    () => iconSvg("M7.86 2h8.28L22 7.86v8.28L16.14 22H7.86L2 16.14V7.86z M9 9h6v6H9z"),
  play:    () => iconSvg("M5 3l14 9-14 9V3z"),
  upload:  () => iconSvg("M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4 M17 8l-5-5-5 5 M12 3v12"),
  download:() => iconSvg("M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4 M7 10l5 5 5-5 M12 15V3"),
  shield:  () => iconSvg("M12 22s8-4 8-10V5l-8-3-8 3v7c0 6 8 10 8 10z"),
  flag:    () => iconSvg("M4 15s1-1 4-1 5 2 8 2 4-1 4-1V3s-1 1-4 1-5-2-8-2-4 1-4 1z M4 22v-7"),
  code:    () => iconSvg("M16 18l6-6-6-6 M8 6l-6 6 6 6"),
  filter:  () => iconSvg("M22 3H2l8 9.46V19l4 2v-8.54L22 3z")
};

function tipHandlers(textStr) {
  return {
    onmouseenter: (e) => { hoverTip.val = { text: textStr, x: e.clientX + 12, y: e.clientY + 14 }; },
    onmousemove:  (e) => { hoverTip.val = { text: textStr, x: e.clientX + 12, y: e.clientY + 14 }; },
    onmouseleave: () => { hoverTip.val = null; },
    onpointerdown: () => { hoverTip.val = null; }
  };
}

// ===========================================================================
// 3. Formatting, Hex Dump & Command Relay
// ===========================================================================
function formatHex(dataBytes) {
  if (!dataBytes) return "Select a packet to inspect raw bytes.";
  let bytes = [];
  if (dataBytes instanceof Uint8Array || Array.isArray(dataBytes)) {
    bytes = Array.from(dataBytes);
  } else if (typeof dataBytes === "string") {
    for (let i = 0; i < dataBytes.length; i++) bytes.push(dataBytes.charCodeAt(i) & 0xff);
  }
  if (bytes.length === 0) return "0000:  0 payload bytes";
  let lines = [];
  for (let i = 0; i < bytes.length; i += 16) {
    let chunk = bytes.slice(i, i + 16);
    let hexParts = [];
    for (let j = 0; j < 16; j++) {
      hexParts.push(j < chunk.length ? chunk[j].toString(16).toUpperCase().padStart(2, "0") : "  ");
      if (j === 7) hexParts.push(" ");
    }
    let ascii = chunk.map(x => (x >= 32 && x <= 126) ? String.fromCharCode(x) : ".").join("");
    lines.push(`${i.toString(16).padStart(4, "0")}:  ${hexParts.join(" ")}  |${ascii.padEnd(16, " ")}|`);
  }
  return lines.join("\n");
}

function sanitizePrintable(bytes) {
  let out = "";
  for (let i = 0; i < bytes.length; i++) {
    let c = bytes[i];
    out += (c >= 32 && c <= 126) ? String.fromCharCode(c) : ".";
  }
  return out;
}

function showToast(msg) {
  toastMsg.val = msg;
  setTimeout(() => { if (toastMsg.val === msg) toastMsg.val = ""; }, 3200);
}

function triggerHardwareCommand(cmd, feedbackText, glitchBus = "") {
  if (glitchBus) {
    activeGlitchBus.val = glitchBus;
    setTimeout(() => { if (activeGlitchBus.val === glitchBus) activeGlitchBus.val = ""; }, 2200);
  }
  fetch("/cmd?c=" + cmd, { method: "POST" })
    .then(() => {
      showToast(feedbackText);
      fetchNextPcapChunk();
      setTimeout(() => fetchNextPcapChunk(), 180);
    })
    .catch(() => showToast(`${feedbackText} (hardware unreachable)`));
}

function clearAllStats() {
  isReplayMode.val = false;
  activeGlitchBus.val = "";
  uartPkts.val = 0;
  i2cPkts.val = 0;
  wifiPkts.val = 0;
  zigbeePkts.val = 0;
  corruptedCount.val = 0;
  packets.val = [];
  selectedPacket.val = null;
  s_pcapStreamOffset = 0;
  s_pcapCarryBuffer = new Uint8Array(0);
  latestUartSent.val = "";
  latestUartRecv.val = "";
  lastUartGlitch.val = "";
  lastI2cFault.val = "";
  uartClear.val = false;
  uartSample.val = "";
  wifiClear.val = false;
  wifiSample.val = "";
  z154Clear.val = false;
  z154Sample.val = "";
  ctfUart.val = "";
  ctfWifi.val = "";
  ctfZigbee.val = "";
  ctfBonus.val = "";
  ctfImask.val = 0;
  ctfEmask.val = 0;
  triggerHardwareCommand("C", "Stats and messages cleared");
}

let wifiAutoScanning = false;
let zigbeeAutoScanning = false;
let modeLockUntil = 0;
let wifiLockUntil = 0;
let zigbeeLockUntil = 0;

function selectToolMode(mode) {
  wifiAutoScanning = false;
  zigbeeAutoScanning = false;
  wifiScanningUi.val = false;
  zigbeeScanningUi.val = false;
  modeLockUntil = Date.now() + 1500;
  if (mode === "WIFI") wifiLockUntil = Date.now() + 1500;
  if (mode === "ZIGBEE") zigbeeLockUntil = Date.now() + 1500;
  toolMode.val = mode;
  if (mode === "OFF" || mode === "AUTO") {
    filterProtocol.val = "ALL";
  } else {
    filterProtocol.val = mode;
  }
  if (mode === "WIFI") {
    wifiChannelMode.val = 0;
    wifiSampleCh.val = 0;
    wifiClear.val = false;
    wifiSample.val = "";
  } else if (mode === "ZIGBEE") {
    zigbeeChannel.val = 0;
    z154Clear.val = false;
    z154Sample.val = "";
  }
  fetch("/cmd?mode=" + mode.toLowerCase(), { method: "POST" })
    .then(() => showToast(mode === "OFF" ? "Capture paused" : `Switched mode to ${mode}`))
    .catch(() => {});
}

function selectUartBaud(baud) {
  uartBaud.val = baud;
  uartClear.val = false;
  uartSample.val = "";
  lastUartGlitch.val = "";
  fetch("/cmd?baud=" + baud, { method: "POST" })
    .then(() => showToast(`UART sniffer set to ${baud} bps`))
    .catch(() => {});
}

function triggerUartAutodetect() {
  uartClear.val = false;
  uartSample.val = "";
  lastUartGlitch.val = "";
  showToast("Scanning 115200 / 57600 / 38400 / 9600 bps...");
  fetch("/cmd?uart_auto=1", { method: "POST" })
    .catch(() => {});
}

function selectWifiChannel(ch) {
  wifiAutoScanning = false;
  wifiScanningUi.val = false;
  wifiLockUntil = Date.now() + 1500;
  wifiChannelMode.val = ch;
  wifiActiveCh.val = ch;
  wifiSampleCh.val = ch;
  wifiClear.val = false;
  wifiSample.val = "";
  fetch("/cmd?ch=" + ch, { method: "POST" })
    .then(() => showToast(`Captured burst on Wi-Fi Ch ${ch}`))
    .catch(() => {});
}

function triggerWifiAutoScan() {
  wifiAutoScanning = true;
  wifiScanningUi.val = true;
  wifiLockUntil = 0;
  wifiClear.val = false;
  wifiSample.val = "";
  showToast("Auto-Scanning Wi-Fi Ch 1–13...");
  fetch("/cmd?wifi_auto=1", { method: "POST" })
    .catch(() => {});
  setTimeout(() => { wifiAutoScanning = false; wifiScanningUi.val = false; }, 9500);
}

function jamWifiChannel(ch) {
  if (ch < 1 || ch > 13) return;
  activeGlitchBus.val = "WIFI";
  setTimeout(() => { if (activeGlitchBus.val === "WIFI") activeGlitchBus.val = ""; }, 2200);
  wifiClear.val = false;
  wifiSample.val = "";
  fetch("/cmd?wifi_jam=" + ch, { method: "POST" })
    .then(() => showToast(`Jammed Wi-Fi Ch ${ch} — Iris hopping to a new channel`))
    .catch(() => {});
}

function selectZigbeeChannel(ch) {
  zigbeeAutoScanning = false;
  zigbeeScanningUi.val = false;
  zigbeeLockUntil = Date.now() + 1500;
  zigbeeChannel.val = ch;
  z154Clear.val = false;
  z154Sample.val = "";
  let freq = 2405 + 5 * (ch - 11);
  fetch("/cmd?zch=" + ch, { method: "POST" })
    .then(() => showToast(`Tuned Zigbee to Ch ${ch} · ${freq} MHz`))
    .catch(() => {});
}

function triggerZigbeeAutoScan() {
  zigbeeAutoScanning = true;
  zigbeeScanningUi.val = true;
  zigbeeLockUntil = 0;
  z154Clear.val = false;
  z154Sample.val = "";
  showToast("Auto-Scanning Zigbee Ch 11–26...");
  fetch("/cmd?z_auto=1", { method: "POST" })
    .catch(() => {});
  setTimeout(() => { zigbeeAutoScanning = false; zigbeeScanningUi.val = false; }, 9500);
}

function jamZigbeeChannel() {
  if (zigbeeChannel.val < 11 || zigbeeChannel.val > 26) return;
  triggerHardwareCommand("W", `Jammed Zigbee Ch ${zigbeeChannel.val} — Iris hopping to a new channel`, "ZIGBEE");
}
function updateAccelHistory(rx, ry, rz) {
  let changed = (accelX.val !== null) && (Math.abs(rx - accelX.val) > 8 || Math.abs(ry - accelY.val) > 8 || Math.abs(rz - accelZ.val) > 8);
  accelX.val = rx;
  accelY.val = ry;
  accelZ.val = rz;
  if (changed) i2cValueChangedUntil.val = Date.now() + 1400;
  xFilterRing.push(rx);
  if (xFilterRing.length > 8) xFilterRing.shift();
  let sumX = xFilterRing.reduce((a, bVal) => a + bVal, 0);
  accelAvgX.val = Math.round(sumX / xFilterRing.length);
}

// ===========================================================================
// 4. PCAP Dissector (Zero Hardcoded Flags; Extracts Real Captured Data)
// ===========================================================================
function extractCtfFlag(textStr) {
  let m = textStr.match(/WTF\{[A-Za-z0-9_]+\}/);
  return m ? m[0] : "";
}

function dissectPcapRecord(tsSec, tsUsec, inclLen, payloadBytes) {
  clientPacketSeq++;
  let tsTotalUs = tsSec * 1000000 + tsUsec;
  let timeStr   = (tsTotalUs / 1000000).toFixed(3) + "s";

  let protoId = 1;
  let probeId = 1;
  let body    = payloadBytes;

  if (payloadBytes.length >= 2 && payloadBytes[0] >= 1 && payloadBytes[0] <= 5) {
    protoId = payloadBytes[0];
    probeId = payloadBytes[1];
    body    = payloadBytes.slice(2);
  }

  let proto     = "UART";
  let tagClass  = "uart";
  let chan      = `Probe ${probeId}`;
  let summary   = "";
  let details   = {};
  let isFault   = false;
  let nowMs     = Date.now();

  if (protoId === 1) {
    proto    = "UART";
    tagClass = "uart";
    chan     = `${uartBaud.val} bps`;
    let ascii = sanitizePrintable(body);
    let m = ascii.match(/HESTIA:(\d+):([0-9A-Fa-f]{1,4})/);
    if (m) {
      let ctr     = parseInt(m[1], 10) >>> 0;
      let rxCsum  = parseInt(m[2], 16) & 0xffff;
      let expCsum = (~ctr) & 0xffff;
      let flag    = extractCtfFlag(ascii);
      let expStr  = `HESTIA:${ctr}:${expCsum.toString(16).toUpperCase().padStart(4, "0")}` + (flag ? `:${flag}` : "");
      latestUartSent.val = expStr;
      latestUartRecv.val = ascii;
      if (rxCsum === expCsum) {
        if (isReplayMode.val && flag) ctfUart.val = flag;
        summary = `Valid frame #${ctr}` + (flag ? ` · ${flag}` : "");
        details = {
          Source: "Hestia (GP0 UART0 TX)",
          Destination: "Daphne (GP1 UART0 RX)",
          BaudRate: `${uartBaud.val} bps`,
          Validation: `Checksum OK (0x${rxCsum.toString(16).toUpperCase().padStart(4, "0")})`,
          DecodedPayload: ascii
        };
      } else {
        tagClass = "fault";
        isFault = true;
        summary = `Glitched frame #${ctr}: checksum 0x${rxCsum.toString(16).toUpperCase()} != 0x${expCsum.toString(16).toUpperCase()}`;
        details = {
          Source: "Hestia (GP0 UART0 TX)",
          Destination: "Daphne (GP1 UART0 RX)",
          FaultEffect: "10 us bit-flip inverted payload checksum; Daphne dropped frame and sounded alarm",
          Expected: expStr,
          Received: ascii
        };
      }
    } else {
      latestUartRecv.val = ascii;
      summary = `Scrambled framing bytes at ${uartBaud.val} bps`;
      details = {
        Source: "Probe 1 (UART1 RX)",
        BaudRate: `${uartBaud.val} bps`,
        Status: "Framing mismatch — select matching baud rate or click Auto-Detect",
        DecodedPreview: ascii
      };
    }
    if (toolMode.val === "UART" || toolMode.val === "AUTO") uartActiveUntil.val = nowMs + 1200;
  } else if (protoId === 2) {
    proto    = "I2C";
    tagClass = "i2c";
    chan     = "400 kHz I²C";
    if (body.length >= 1) {
      let addr7 = body[0] >> 1;
      let rw    = (body[0] & 1) ? "Read" : "Write";
      if ((body[0] & 1) === 1 && body.length >= 7 && addr7 === 0x19) {
        let rx = (body[2] << 8) | body[1]; if (rx & 0x8000) rx -= 65536; rx >>= 4;
        let ry = (body[4] << 8) | body[3]; if (ry & 0x8000) ry -= 65536; ry >>= 4;
        let rz = (body[6] << 8) | body[5]; if (rz & 0x8000) rz -= 65536; rz >>= 4;
        updateAccelHistory(rx, ry, rz);
        summary = `GY-511 Accel Read: X=${rx} mg, Y=${ry} mg, Z=${rz} mg`;
        details = {
          Bus: "I²C1 · 400 kHz",
          TargetDevice: "LSM303DLHC Accelerometer 0x19",
          Readings: `X=${rx} mg, Y=${ry} mg, Z=${rz} mg`
        };
      } else {
        let devName = addr7 === 0x19 ? "LSM303DLHC Accel" : (addr7 === 0x1e ? "LSM303DLHC Mag" : `Slave 0x${addr7.toString(16).toUpperCase()}`);
        summary = `${devName} ${rw} · ${body.length} B`;
        details = {
          Bus: "I²C1 · 400 kHz",
          TargetDevice: `${devName} 0x${addr7.toString(16).toUpperCase()}`,
          Operation: rw,
          ByteCount: `${body.length} bytes`
        };
      }
    } else {
      summary = "I²C bus transaction";
      details = { Bus: "I²C1 (400 kHz)", Summary: "Bus activity captured on SDA/SCL probes" };
    }
    if (toolMode.val === "I2C" || toolMode.val === "AUTO") i2cActiveUntil.val = nowMs + 1200;
  } else if (protoId === 4) {
    proto    = "WIFI";
    tagClass = "wifi";
    chan     = `Ch ${probeId}`;
    let ascii = sanitizePrintable(body);
    let flag  = extractCtfFlag(ascii);
    let isIris = ascii.includes("IRIS:") || (body.length >= 16 && body.some((_, i) => i + 5 < body.length && body[i] === 0x02 && body[i + 5] === 0x03));
    if (isReplayMode.val && flag) ctfWifi.val = flag;
    if (isIris) {
      summary = flag ? `Iris Ch ${probeId} cleartext · ${flag}` : `Iris Ch ${probeId} scrambled bytes`;
    } else {
      summary = `802.11 frame on Ch ${probeId} · ${body.length} B`;
    }
    details = {
      Protocol: "IEEE 802.11 Wi-Fi",
      Channel: `Channel ${probeId}`,
      Source: isIris ? "Iris 02:00:00:00:00:03" : "Ambient 802.11 Station",
      DecodedPayload: flag || (ascii.includes("IRIS:SCRM:") ? "Scrambled payload" : ascii)
    };
    if (toolMode.val === "WIFI" || toolMode.val === "AUTO") wifiActiveUntil.val = nowMs + 1200;
  } else if (protoId === 5) {
    proto    = "ZIGBEE";
    tagClass = "zigbee";
    chan     = `Ch ${probeId}`;
    let ascii = sanitizePrintable(body);
    let flag  = extractCtfFlag(ascii);
    let seqNum = body.length >= 3 ? body[2] : 0;
    if (isReplayMode.val && flag) ctfZigbee.val = flag;
    summary = flag ? `Iris Ch ${probeId} #${seqNum} · ${flag}` : `Zigbee Ch ${probeId} #${seqNum} · scrambled`;
    details = {
      Protocol: "IEEE 802.15.4 / Zigbee",
      Channel: `Ch ${probeId} · ${2405 + 5 * (probeId - 11)} MHz`,
      PAN_ID: "0x1234",
      Sequence: `#${seqNum}`,
      DecodedPayload: flag || "Scrambled payload"
    };
    if (toolMode.val === "ZIGBEE" || toolMode.val === "AUTO") zigbeeActiveUntil.val = nowMs + 1200;
  } else if (protoId === 3) {
    proto    = "FAULT";
    tagClass = "fault";
    chan     = probeId === 0 ? "UART Glitch" : (probeId === 2 ? "I²C ACK-Kill" : (probeId === 3 ? "I²C Stretch" : (probeId === 4 ? "Wi-Fi Jam" : `Zigbee Jam Ch ${probeId}`)));
    isFault  = true;
    let desc = probeId === 0 ? "10 us UART bit-flip injected across R1 220 Ohm"
             : probeId === 2 ? "50 us I²C 9th-clock ACK-kill on SDA — Daphne ran 9-clock recovery"
             : probeId === 3 ? "50 us I²C SCL clock-stretch held bus low"
             : probeId === 4 ? "Targeted Wi-Fi jam sent to Iris — Iris hopped channels"
             : `Targeted 802.15.4 jam sent on Ch ${probeId} — Iris hopped channels`;
    if (probeId === 0) lastUartGlitch.val = "Bit-flip injected -> Daphne checksum mismatch & GP18 alarm";
    if (probeId === 2) lastI2cFault.val = "Kill ACK -> 50 us NACK on 9th clock -> Daphne 9-clock recovery restored GY-511";
    if (probeId === 3) lastI2cFault.val = "Stretch Clock -> 50 us SCL hold -> Daphne paused cleanly and resumed GY-511";
    summary = desc;
    details = {
      Action: chan,
      Result: desc,
      SafetyGuard: "Bounded hardware pulse; probes returned to High-Z"
    };
  }

  return {
    id: clientPacketSeq,
    tsUs: tsTotalUs,
    time: timeStr,
    proto,
    chan,
    len: body.length,
    summary,
    details,
    raw: body,
    tagClass,
    isFault
  };
}

let s_pcapStreamOffset = 0;
let s_pcapCarryBuffer  = new Uint8Array(0);
const PCAP_BATCH_LIMIT = 16384;

function concatUint8(a, bVal) {
  if (a.length === 0) return bVal;
  if (bVal.length === 0) return a;
  let out = new Uint8Array(a.length + bVal.length);
  out.set(a, 0);
  out.set(bVal, a.length);
  return out;
}

function parseIncomingPcapBytes(chunkBytes, isFromStart) {
  let buf = concatUint8(s_pcapCarryBuffer, chunkBytes);
  let pos = 0;
  if (isFromStart && buf.length >= 24) {
    let dvGlobal = new DataView(buf.buffer, buf.byteOffset, 24);
    let magic = dvGlobal.getUint32(0, true);
    if (magic === 0xa1b2c3d4 || magic === 0xd4c3b2a1) pos = 24;
  }

  let newRecords = [];
  while (pos + 16 <= buf.length) {
    let dv      = new DataView(buf.buffer, buf.byteOffset + pos, 16);
    let tsSec   = dv.getUint32(0, true);
    let tsUsec  = dv.getUint32(4, true);
    let inclLen = dv.getUint32(8, true);
    if (inclLen > 2048) { pos += 16; continue; }
    if (pos + 16 + inclLen > buf.length) break;
    let pktBytes = buf.slice(pos + 16, pos + 16 + inclLen);
    newRecords.push(dissectPcapRecord(tsSec, tsUsec, inclLen, pktBytes));
    pos += 16 + inclLen;
  }
  s_pcapCarryBuffer = buf.slice(pos);

  if (newRecords.length > 0 && !isPaused.val) {
    let merged = [...newRecords.reverse(), ...packets.val].slice(0, 500);
    packets.val = merged;
    let latestFault = newRecords.find(r => r.isFault || r.tagClass === "fault");
    if (latestFault) selectedPacket.val = latestFault;
    else if (!selectedPacket.val && merged.length > 0) selectedPacket.val = merged[0];
  }
}

let s_fetchingPcap = false;
async function fetchNextPcapChunk() {
  if (s_fetchingPcap) return;
  s_fetchingPcap = true;
  try {
    let loops = 0;
    while (loops < 4) {
      loops++;
      let isStart = (s_pcapStreamOffset === 0);
      let resp = await fetch(`/api/pcap?offset=${s_pcapStreamOffset}&limit=${PCAP_BATCH_LIMIT}`);
      if (!resp.ok) break;
      let totalHeader = resp.headers.get("X-Pcap-Total-Bytes");
      let totalVal = totalHeader ? parseInt(totalHeader, 10) : 0;
      if (!isNaN(totalVal) && totalVal > 0) {
        pcapSize.val = totalVal;
        if (totalVal < s_pcapStreamOffset) {
          s_pcapStreamOffset = 0;
          s_pcapCarryBuffer = new Uint8Array(0);
          isStart = true;
          let retry = await fetch(`/api/pcap?offset=0&limit=${PCAP_BATCH_LIMIT}`);
          if (retry.ok) resp = retry;
        }
      }
      let arrayBuf = await resp.arrayBuffer();
      let bytes = new Uint8Array(arrayBuf);
      if (bytes.length > 0) {
        parseIncomingPcapBytes(bytes, isStart);
        s_pcapStreamOffset += bytes.length;
        pcapSyncedBytes.val = s_pcapStreamOffset;
        if (totalVal > s_pcapStreamOffset && bytes.length >= PCAP_BATCH_LIMIT) continue;
      }
      break;
    }
  } catch (e) {
  } finally {
    s_fetchingPcap = false;
  }
}

function handlePcapFileUpload(e) {
  let file = e.target.files[0];
  if (!file) return;
  let reader = new FileReader();
  reader.onload = (evt) => {
    let bytes = new Uint8Array(evt.target.result);
    isReplayMode.val = true;
    isPaused.val = true;
    packets.val = [];
    s_pcapCarryBuffer = new Uint8Array(0);
    parseIncomingPcapBytes(bytes, true);
    showToast(`Loaded ${file.name} (${packets.val.length} frames)`);
  };
  reader.readAsArrayBuffer(file);
}

// ===========================================================================
// 5. Top Bar & Single Mode Selector Bar
// ===========================================================================
function TopHeaderBar() {
  return div({ class: "top-cmd-bar" },
    div({ class: "brand-cluster" },
      span({ class: "brand-logo" }, "WHATTHEFLIP")
    ),
    div({ class: "safety-cluster" },
      button({
        class: "cmd-btn",
        ...tipHandlers("Reset packet counters and stop active fault injection on whattheflip so Daphne clears its alarm"),
        onclick: clearAllStats
      }, ICONS.trash(), "Clear Stats"),
      () => safetyState.val === "Stopped"
        ? button({
            class: "cmd-btn rearm",
            ...tipHandlers("Resume whattheflip monitoring and restore probes"),
            onclick: () => { triggerHardwareCommand("R", "whattheflip Active"); safetyState.val = "Active"; }
          }, ICONS.play(), "Resume")
        : button({
            class: "cmd-btn estop",
            ...tipHandlers("Float probes P0-P3 to High-Z and turn off all whattheflip LEDs"),
            onclick: () => { triggerHardwareCommand("E", "E-Stop active: whattheflip stopped"); safetyState.val = "Stopped"; }
          }, ICONS.stop(), "E-Stop"),
      button({
        class: "cmd-btn",
        ...tipHandlers("Load and replay an offline .pcap capture file"),
        onclick: () => document.getElementById("pcap-file-input").click()
      }, ICONS.upload(), "Replay"),
      input({
        type: "file",
        id: "pcap-file-input",
        accept: ".pcap",
        style: "display:none",
        onchange: handlePcapFileUpload
      }),
      a({
        href: "/capture.pcap",
        download: "capture.pcap",
        class: "cmd-btn pcap-dl",
        ...tipHandlers("Download captured traffic as Wireshark PCAP")
      }, ICONS.download(), "Export")
    )
  );
}

function ToolBeltBar() {
  const modes = [
    { id: "OFF",    label: "Off",    cls: "tool-off",    icon: ICONS.power },
    { id: "AUTO",   label: "Auto",   cls: "tool-auto",   icon: ICONS.radar },
    { id: "UART",   label: "UART",   cls: "tool-uart",   icon: ICONS.zap },
    { id: "I2C",    label: "I²C",    cls: "tool-i2c",    icon: ICONS.cpu },
    { id: "WIFI",   label: "Wi-Fi",  cls: "tool-wifi",   icon: ICONS.wifi },
    { id: "ZIGBEE", label: "Zigbee", cls: "tool-zigbee", icon: ICONS.radio }
  ];

  return div({ class: "tool-belt-bar" },
    div({ class: "tool-cluster" },
      modes.map(m => button({
        class: () => "tool-btn " + m.cls + (toolMode.val === m.id ? " active" : ""),
        onclick: () => selectToolMode(m.id)
      }, m.icon(), m.label))
    ),
    span({ class: "mode-hint" }, () => {
      if (toolMode.val === "OFF") return "Select Auto or a protocol to begin";
      if (toolMode.val === "AUTO") return "Auto-discovery active across all 4 buses";
      return `Inspecting ${toolMode.val} traffic & panel`;
    })
  );
}

// ===========================================================================
// 6. Upper Left: Topology Pane
// ===========================================================================
function BreadboardCanvas() {
  return div({ class: "topology-pane" },
    div({ class: "pane-header" },
      div({ class: "topo-title-group" },
        span("TOPOLOGY"),
        button({
          type: "button",
          class: "info-icon-btn",
          "aria-label": "Animation sampling info",
          ...tipHandlers("The animation is slowed down to 2 Hz to see the packets moving.")
        }, "i")
      ),
      div({ class: "pkt-counter-strip" },
        span({
          class: "pkt-counter uart",
          ...tipHandlers("Total UART frames sniffed from Hestia to Daphne on Probe 1")
        }, () => `UART: ${uartPkts.val} pkts`),
        span({
          class: "pkt-counter i2c",
          ...tipHandlers("Total I²C transactions captured between Daphne and GY-511 on Probes 2 & 3")
        }, () => `I2C: ${i2cPkts.val} pkts`),
        span({
          class: "pkt-counter wifi",
          ...tipHandlers("Total 802.11 Wi-Fi frames captured during channel monitoring")
        }, () => `Wi-Fi: ${wifiPkts.val} pkts`),
        span({
          class: "pkt-counter zigbee",
          ...tipHandlers("Total IEEE 802.15.4 Zigbee frames captured from Iris")
        }, () => `Zigbee: ${zigbeePkts.val} pkts`)
      )
    ),
    div({ class: "topology-svg-wrap" },
      () => {
        let ph        = animPhase.val;
        let mode      = toolMode.val;
        let stopped   = safetyState.val === "Stopped";
        let isOff     = mode === "OFF" || stopped;
        let glitchBus = activeGlitchBus.val;
        let probeDash = stopped ? "4 4" : "none";
        let nowMs     = Date.now();

        let uartLive   = !isOff && (mode === "AUTO" || mode === "UART") && (uartActiveUntil.val > nowMs || glitchBus === "UART");
        let i2cLive    = !isOff && (mode === "AUTO" || mode === "I2C") && (i2cActiveUntil.val > nowMs || glitchBus === "I2C_ACK" || glitchBus === "I2C_STRETCH");
        let wifiLive   = !isOff && (mode === "AUTO" || mode === "WIFI") && (wifiActiveUntil.val > nowMs || glitchBus === "WIFI");
        let zigbeeLive = !isOff && (mode === "AUTO" || mode === "ZIGBEE") && (zigbeeActiveUntil.val > nowMs || glitchBus === "ZIGBEE");
        let rfLive     = wifiLive || zigbeeLive;

        let i2cDelta   = !isOff && i2cValueChangedUntil.val > nowMs;

        let t1 = (ph % 100) / 100;
        let t2 = ((ph + 50) % 100) / 100;
        let pduUartX = 160 + t1 * 235;
        let pduPastR1 = pduUartX > 280;
        let pduI2cX  = 555 - t2 * 60;
        let pduRfX   = 450 - t2 * 220;
        let tiltSlot = Math.max(0, Math.min(7, Math.floor((accelAvgX.val + 1000) / 250)));

        const idleWire = "#cbd5e1";

        return svg({ id: "topology-svg", class: "topology-svg", viewBox: "0 0 680 264" },
          // 1. Simplex UART Link: Hestia -> R1 -> Daphne
          g({ class: "topo-wire", onclick: () => selectToolMode("UART"), ...tipHandlers("UART TX line on Row 22: Hestia GP0 to 220 Ohm R1") },
            line({ x1: 150, y1: 54, x2: 245, y2: 54, stroke: uartLive ? "var(--proto-uart)" : idleWire, "stroke-width": 3 }),
            path({ d: "M 235 49 L 244 54 L 235 59 Z", fill: uartLive ? "var(--proto-uart)" : idleWire }),
            text({ x: 197, y: 31, fill: uartLive ? "var(--proto-uart)" : "var(--fg-dim)", "font-size": 10.5, "font-family": "monospace", "font-weight": "bold", "text-anchor": "middle" }, "UART TX →")
          ),
          g({ class: "topo-wire", onclick: () => selectToolMode("UART"), ...tipHandlers("UART RX line on Row 13: R1 to Daphne GP1") },
            line({
              x1: 315, y1: 54, x2: 410, y2: 54,
              stroke: glitchBus === "UART" ? "var(--state-glitch)" : (uartLive ? "var(--proto-uart)" : idleWire),
              "stroke-width": glitchBus === "UART" ? 4 : 3,
              "stroke-dasharray": glitchBus === "UART" ? "5 3" : "none"
            }),
            path({ d: "M 400 49 L 409 54 L 400 59 Z", fill: glitchBus === "UART" ? "var(--state-glitch)" : (uartLive ? "var(--proto-uart)" : idleWire) }),
            text({ x: 362, y: 31, fill: glitchBus === "UART" ? "var(--state-glitch)" : (uartLive ? "var(--proto-uart)" : "var(--fg-dim)"), "font-size": 10.5, "font-family": "monospace", "font-weight": "bold", "text-anchor": "middle" },
              glitchBus === "UART" ? "UART RX GLITCH" : "UART RX")
          ),

          // 2. Probe 1 Sniff Tap (Row 22 -> WhatTheFlip GPIO5)
          g({ class: "topo-wire", onclick: () => selectToolMode("UART"), ...tipHandlers("Probe 1 GPIO5 passive UART sniffer tap on Row 22") },
            path({ d: "M 197 54 L 197 124 L 105 124 L 105 166", stroke: uartLive ? "var(--proto-uart)" : idleWire, "stroke-width": 1.6, "stroke-dasharray": probeDash, fill: "none" }),
            text({ x: 145, y: 118, fill: uartLive ? "var(--proto-uart)" : "var(--fg-dim)", "font-size": 10, "font-family": "monospace", "font-weight": "bold", "text-anchor": "middle" }, "P1 Sniff")
          ),

          // 3. Probe 0 Glitch Tap (WhatTheFlip GPIO4 -> Row 13)
          g({ class: "topo-wire", onclick: () => selectToolMode("UART"), ...tipHandlers("Probe 0 GPIO4 10 us UART glitch injector tap on Row 13") },
            path({
              d: "M 155 166 L 155 142 L 362 142 L 362 54",
              stroke: glitchBus === "UART" ? "var(--state-glitch)" : (uartLive ? "var(--state-warn)" : idleWire),
              "stroke-width": glitchBus === "UART" ? 2.5 : 1.6, "stroke-dasharray": probeDash, fill: "none"
            }),
            text({ x: 265, y: 136, fill: glitchBus === "UART" ? "var(--state-glitch)" : (uartLive ? "var(--state-warn)" : "var(--fg-dim)"), "font-size": 10, "font-family": "monospace", "font-weight": "bold", "text-anchor": "middle" }, "P0 Glitch")
          ),

          // 4. I2C Bus between Daphne & GY-511 (Row 55 SCL / Row 58 SDA)
          g({ class: "topo-wire", onclick: () => selectToolMode("I2C"), ...tipHandlers("I²C1 bus on Row 55 SCL and Row 58 SDA") },
            path({
              d: "M 480 90 L 480 116 L 620 116 L 620 136",
              stroke: (glitchBus === "I2C_ACK" || glitchBus === "I2C_STRETCH") ? "var(--state-glitch)" : (i2cLive ? "var(--proto-i2c)" : idleWire),
              "stroke-width": (glitchBus === "I2C_ACK" || glitchBus === "I2C_STRETCH") ? 3.5 : 2.5, fill: "none"
            }),
            text({ x: 552, y: 108, fill: (glitchBus === "I2C_ACK" || glitchBus === "I2C_STRETCH") ? "var(--state-glitch)" : (i2cLive ? "var(--proto-i2c)" : "var(--fg-dim)"), "font-size": 10, "font-family": "monospace", "font-weight": "bold", "text-anchor": "middle" },
              (glitchBus === "I2C_ACK" || glitchBus === "I2C_STRETCH") ? "I²C FAULT" : "I²C 400 kHz")
          ),

          // 5. Probes 2 & 3 Tap (WhatTheFlip GPIO6/7 -> I2C Bus)
          g({ class: "topo-wire", onclick: () => selectToolMode("I2C"), ...tipHandlers("Probes 2 & 3 GPIO6 SDA / GPIO7 SCL on Row 58 / Row 55") },
            path({ d: "M 195 194 L 480 194 L 480 116", stroke: i2cLive ? "var(--proto-i2c)" : idleWire, "stroke-width": 1.6, "stroke-dasharray": probeDash, fill: "none" }),
            text({ x: 338, y: 188, fill: i2cLive ? "var(--proto-i2c)" : "var(--fg-dim)", "font-size": 10, "font-family": "monospace", "font-weight": "bold", "text-anchor": "middle" }, "P2/P3 SDA/SCL")
          ),

          // 6. 2.4 GHz RF Link (Iris <-> WhatTheFlip)
          g({ class: "topo-wire", onclick: () => selectToolMode(mode === "ZIGBEE" ? "ZIGBEE" : "WIFI"), ...tipHandlers("2.4 GHz link for Wi-Fi Ch 1–13 and Zigbee Ch 11–26") },
            line({
              x1: 195, y1: 236, x2: 475, y2: 236,
              stroke: (glitchBus === "WIFI" || glitchBus === "ZIGBEE") ? "var(--state-glitch)" : (zigbeeLive ? "var(--proto-zigbee)" : (wifiLive ? "var(--proto-wifi)" : idleWire)),
              "stroke-width": 2, "stroke-dasharray": "5 4"
            }),
            text({ x: 335, y: 227, fill: (glitchBus === "WIFI" || glitchBus === "ZIGBEE") ? "var(--state-glitch)" : (zigbeeLive ? "var(--proto-zigbee)" : (wifiLive ? "var(--proto-wifi)" : "var(--fg-dim)")), "font-size": 10, "font-family": "monospace", "font-weight": "bold", "text-anchor": "middle" },
              mode === "ZIGBEE" ? (zigbeeChannel.val >= 11 ? `Zigbee Ch ${zigbeeChannel.val}` : "Zigbee Select") : mode === "WIFI" ? (wifiChannelMode.val >= 1 ? `Wi-Fi Ch ${wifiChannelMode.val}` : "Wi-Fi Select") : "2.4 GHz RF")
          ),

          // Hardware Boxes
          g({ class: "topo-node", onclick: () => selectToolMode("UART"), ...tipHandlers("Hestia RP2040: Transmits UART counter frames and message at dynamic baud rate") },
            rect({ class: "node-box", x: 15, y: 20, width: 135, height: 66, rx: 5, fill: "#ffffff", stroke: "rgba(15,23,42,0.22)", "stroke-width": 1.2 }),
            text({ x: 25, y: 40, fill: "var(--fg-primary)", "font-size": 12, "font-weight": "bold", "font-family": "monospace" }, "HESTIA"),
            text({ x: 25, y: 56, fill: uartLive ? "var(--proto-uart)" : "var(--fg-muted)", "font-size": 10, "font-family": "monospace", "font-weight": "600" }, `UART TX ${uartBaud.val}`),
            text({ x: 25, y: 72, fill: "var(--fg-dim)", "font-size": 9.5, "font-family": "monospace" }, "RP2040 Victim A")
          ),

          g({ class: "topo-node", onclick: () => selectToolMode("UART"), ...tipHandlers("R1 220 Ohm current-limiting resistor isolating Hestia TX during 10 us glitches") },
            rect({ class: "node-box", x: 245, y: 38, width: 70, height: 32, rx: 4, fill: "#ffffff", stroke: "rgba(15,23,42,0.22)", "stroke-width": 1.2 }),
            text({ x: 280, y: 58, fill: "var(--fg-primary)", "font-size": 10.5, "font-weight": "bold", "font-family": "monospace", "text-anchor": "middle" }, "R1 220Ω")
          ),

          g({ class: "topo-node", onclick: () => selectToolMode("I2C"), ...tipHandlers("Daphne RP2040: Validates UART checksums, polls GY-511, and sounds GP18 alarm on faults") },
            rect({ class: "node-box", x: 410, y: 20, width: 145, height: 70, rx: 5, fill: "#ffffff", stroke: "rgba(15,23,42,0.22)", "stroke-width": 1.2 }),
            text({ x: 420, y: 40, fill: "var(--fg-primary)", "font-size": 12, "font-weight": "bold", "font-family": "monospace" }, "DAPHNE"),
            text({ x: 420, y: 56, fill: uartLive ? "var(--proto-uart)" : "var(--fg-muted)", "font-size": 10, "font-family": "monospace", "font-weight": "600" }, "UART RX + Alarm"),
            text({ x: 420, y: 72, fill: i2cLive ? "var(--proto-i2c)" : "var(--fg-muted)", "font-size": 10, "font-family": "monospace", "font-weight": "600" }, `I²C Master • GP${8 + tiltSlot}`)
          ),

          g({ class: "topo-node", onclick: () => selectToolMode("I2C"), ...tipHandlers("GY-511 LSM303DLHC 3-axis accelerometer and magnetometer polled at 400 kHz") },
            rect({
              class: "node-box", x: 565, y: 136, width: 105, height: 72, rx: 5,
              fill: i2cDelta ? "var(--proto-i2c-bg)" : "#ffffff",
              stroke: i2cLive ? "var(--proto-i2c)" : "rgba(15,23,42,0.22)",
              "stroke-width": i2cDelta ? 2 : 1.2
            }),
            text({ x: 575, y: 155, fill: i2cLive ? "var(--proto-i2c)" : "var(--fg-primary)", "font-size": 11, "font-weight": "bold", "font-family": "monospace" }, "GY-511"),
            accelX.val !== null
              ? g(
                  text({ x: 575, y: 173, fill: "var(--fg-primary)", "font-size": 10, "font-weight": "bold", "font-family": "monospace" }, `X:${accelX.val} Y:${accelY.val}`),
                  text({ x: 575, y: 189, fill: "var(--fg-primary)", "font-size": 10, "font-weight": "bold", "font-family": "monospace" }, `Z:${accelZ.val} mg`)
                )
              : text({ x: 575, y: 180, fill: "var(--fg-dim)", "font-size": 9.5, "font-family": "monospace" }, "Awaiting I²C")
          ),

          g({ class: "topo-node", onclick: () => selectToolMode("AUTO"), ...tipHandlers("WhatTheFlip ESP32-C6: Multi-protocol sniffer, fault injector, and SoftAP web dashboard") },
            rect({ class: "node-box", x: 15, y: 166, width: 180, height: 84, rx: 5, fill: "#ffffff", stroke: stopped ? "var(--state-glitch)" : "rgba(15,23,42,0.22)", "stroke-width": 1.4 }),
            text({ x: 25, y: 186, fill: "var(--fg-primary)", "font-size": 12, "font-weight": "bold", "font-family": "monospace" }, "WHATTHEFLIP"),
            text({ x: 25, y: 203, fill: uartLive ? "var(--proto-uart)" : "var(--fg-muted)", "font-size": 10, "font-family": "monospace" }, "P0/P1: UART Probes"),
            text({ x: 25, y: 219, fill: i2cLive ? "var(--proto-i2c)" : "var(--fg-muted)", "font-size": 10, "font-family": "monospace" }, "P2/P3: I²C Probes"),
            text({ x: 25, y: 236, fill: stopped ? "var(--state-glitch)" : (rfLive ? "var(--proto-wifi)" : "var(--fg-muted)"), "font-size": 10, "font-family": "monospace", "font-weight": stopped ? "bold" : "normal" },
              stopped ? "STOPPED" : `MODE: ${mode}`)
          ),

          g({ class: "topo-node", onclick: () => selectToolMode("ZIGBEE"), ...tipHandlers("Iris ESP32-C6: Wireless victim hopping Wi-Fi Ch 1–13 and Zigbee Ch 11–26") },
            rect({ class: "node-box", x: 475, y: 212, width: 195, height: 48, rx: 5, fill: "#ffffff", stroke: rfLive ? "var(--proto-wifi)" : "rgba(15,23,42,0.22)", "stroke-width": 1.2 }),
            text({ x: 485, y: 231, fill: rfLive ? "var(--proto-wifi)" : "var(--fg-primary)", "font-size": 12, "font-weight": "bold", "font-family": "monospace" }, "IRIS"),
            text({ x: 485, y: 248, fill: irisVerified.val ? "var(--proto-uart)" : "var(--fg-muted)", "font-size": 10, "font-family": "monospace", "font-weight": "bold" },
              irisRssi.val !== null && irisRssi.val > -70 ? `RSSI ${irisRssi.val} dBm` : "2.4 GHz Target")
          ),

          // Live Animated PDU Pills
          (uartLive && !(pduUartX > 225 && pduUartX < 318)) ? g({ transform: `translate(${pduUartX}, 46)` },
            rect({ x: 0, y: 0, width: 38, height: 16, rx: 3, fill: (pduPastR1 && glitchBus === "UART") ? "var(--state-glitch)" : "var(--proto-uart)" }),
            text({ x: 19, y: 11, fill: "#ffffff", "font-size": 8.5, "font-family": "monospace", "text-anchor": "middle", "font-weight": "bold" },
              (pduPastR1 && glitchBus === "UART") ? "ERR" : "TX->")
          ) : null,
          i2cLive ? g({ transform: `translate(${pduI2cX}, 108)` },
            rect({ x: 0, y: 0, width: 38, height: 16, rx: 3, fill: (glitchBus === "I2C_ACK" || glitchBus === "I2C_STRETCH") ? "var(--state-glitch)" : "var(--proto-i2c)" }),
            text({ x: 19, y: 11, fill: "#ffffff", "font-size": 8.5, "font-family": "monospace", "text-anchor": "middle", "font-weight": "bold" },
              (glitchBus === "I2C_ACK" || glitchBus === "I2C_STRETCH") ? "NACK" : "0x19")
          ) : null,

          rfLive ? g({ transform: `translate(${pduRfX}, 228)` },
            rect({ x: 0, y: 0, width: 42, height: 16, rx: 3, fill: (glitchBus === "WIFI" || glitchBus === "ZIGBEE") ? "var(--state-glitch)" : (zigbeeLive ? "var(--proto-zigbee)" : "var(--proto-wifi)") }),
            text({ x: 21, y: 11, fill: "#ffffff", "font-size": 8.5, "font-family": "monospace", "text-anchor": "middle", "font-weight": "bold" },
              (glitchBus === "WIFI" || glitchBus === "ZIGBEE") ? "JAM" : (zigbeeLive ? (zigbeeChannel.val >= 11 ? `Ch${zigbeeChannel.val}` : "15.4") : (wifiChannelMode.val >= 1 ? `Ch${wifiChannelMode.val}` : "WiFi")))
          ) : null
        );
      }
    )
  );
}

// ===========================================================================
// 7. Upper Right: Context-Sensitive Protocol Panel
// ===========================================================================
function CtfVaultSection() {
  return div({ class: "panel-section" },
    div({ class: "panel-hdr" },
      span({ class: "panel-title" }, "CAPTURED MESSAGES"),
      span({ class: "panel-meta" }, () => {
        let count = [ctfUart.val, ctfWifi.val, ctfZigbee.val].filter(Boolean).length;
        return `${count}/3 messages`;
      })
    ),
    div({ class: "ctf-grid" },
      div({ class: () => "ctf-item" + (ctfUart.val ? " found" : "") },
        span({ class: "ctf-item-name" }, "Hestia UART"),
        span({ class: "ctf-item-flag" }, () => ctfUart.val || "Hunt baud rate")
      ),
      div({ class: () => "ctf-item" + (((ctfImask.val & 2) && (ctfEmask.val & 2)) ? " found" : "") },
        span({ class: "ctf-item-name" }, "Daphne I²C"),
        span({ class: "ctf-item-flag" }, () => ((ctfImask.val & 2) && (ctfEmask.val & 2)) ? "GY-511 + 9-Clk Recovery" : (ctfImask.val & 2) ? "Run Kill ACK or Stretch" : "Sniff I²C bus")
      ),
      div({ class: () => "ctf-item" + (ctfWifi.val ? " found" : "") },
        span({ class: "ctf-item-name" }, "Iris Wi-Fi"),
        span({ class: "ctf-item-flag" }, () => ctfWifi.val || "Hunt Ch 1–13")
      ),
      div({ class: () => "ctf-item" + (ctfZigbee.val ? " found" : "") },
        span({ class: "ctf-item-name" }, "Iris Zigbee"),
        span({ class: "ctf-item-flag" }, () => ctfZigbee.val || "Hunt Ch 11–26")
      )
    ),
    () => {
      if (ctfBonus.val) {
        return div({ class: "flag-box bonus" },
          span({ class: "panel-meta", style: "color:var(--proto-wifi)" }, "BONUS SECRET"),
          span({ class: "flag-value" }, ctfBonus.val)
        );
      }
      let both = (ctfImask.val & ctfEmask.val);
      let stepLabel = (bit, name) => (both & (1 << bit)) ? `${name}: Done` : (ctfImask.val & (1 << bit)) ? `${name}: Glitch/Jam` : `${name}: Intercept`;
      return div({ class: "flag-box" },
        span({ class: "panel-meta" }, "Bonus Secret"),
        span({ style: "color:var(--fg-muted);font-size:12px" }, "Reveals automatically after all 4 protocol intercepts and faults:"),
        span({ class: "mono", style: "color:var(--fg-primary);font-size:11.5px;font-weight:700" },
          `${stepLabel(0, "UART")} · ${stepLabel(1, "I²C")} · ${stepLabel(2, "Wi-Fi")} · ${stepLabel(3, "Zigbee")}`)
      );
    }
  );
}

function OffPanel() {
  return div({ class: "inspector-body" },
    div({ class: "empty-guide" },
      strong({ style: "color:var(--fg-primary);font-family:var(--font-mono)" }, "CAPTURE STANDBY"),
      span("Probes and radios are idle. Select Auto or a protocol above.")
    ),
    CtfVaultSection()
  );
}

function AutoPanel() {
  return div({ class: "inspector-body" },
    div({ class: "panel-section" },
      div({ class: "panel-hdr" },
        span({ class: "panel-title" }, "AUTO-DISCOVERY OVERVIEW"),
        span({ class: "panel-meta", style: "color:var(--proto-uart)" }, "All Buses Active")
      ),
      div({ class: "sensor-readout-grid" },
        div({ class: "sensor-metric" },
          div({ class: "sensor-metric-label" }, "UART Baud"),
          div({ class: "sensor-metric-val", style: "color:var(--proto-uart)" }, () => `${uartBaud.val} bps`)
        ),
        div({ class: "sensor-metric" },
          div({ class: "sensor-metric-label" }, "Wi-Fi Radio"),
          div({ class: "sensor-metric-val", style: "color:var(--proto-wifi)" }, () => wifiChannelMode.val >= 1 ? `Ch ${wifiChannelMode.val}` : "Select")
        ),
        div({ class: "sensor-metric" },
          div({ class: "sensor-metric-label" }, "Zigbee Radio"),
          div({ class: "sensor-metric-val", style: "color:var(--proto-zigbee)" }, () => zigbeeChannel.val >= 11 ? `Ch ${zigbeeChannel.val}` : "Select")
        )
      )
    ),
    CtfVaultSection()
  );
}
function ProtocolObjectiveStrip(bit, interceptAction, faultAction) {
  let hasI = (ctfImask.val & (1 << bit)) !== 0;
  let hasE = (ctfEmask.val & (1 << bit)) !== 0;
  return div({ class: () => "flag-box " + (ctfBonus.val ? "bonus" : (hasI && hasE ? "captured" : "")) },
    span({ class: "panel-meta" }, () => ctfBonus.val ? "BONUS SECRET" : "Protocol Objective"),
    span({ class: "mono", style: "font-size:12px;font-weight:700" }, () => {
      if (ctfBonus.val) return ctfBonus.val;
      let s1 = hasI ? "Intercept: Done" : `Intercept: ${interceptAction}`;
      let s2 = hasE ? "Fault: Done" : `Fault: ${faultAction}`;
      return `${s1} · ${s2}`;
    })
  );
}

function UartPanel() {
  const bauds = [9600, 38400, 57600, 115200];
  return () => {
    let isCorrupt = Boolean(lastUartGlitch.val) || activeGlitchBus.val === "UART" || (selectedPacket.val && selectedPacket.val.proto === "UART" && selectedPacket.val.isFault);
    let sentStr = latestUartSent.val || (uartClear.val ? uartSample.val : "Waiting for matching baud rate...");
    let recvStr = lastUartGlitch.val
      ? `${lastUartGlitch.val}${latestUartRecv.val ? ` (Sniffer saw pre-R1 frame: ${latestUartRecv.val})` : ""}`
      : (latestUartRecv.val || uartSample.val || "No UART frames captured yet");

    return div({ class: "inspector-body" },
      div({ class: "panel-section" },
        div({ class: "panel-hdr" },
          span({ class: "panel-title" }, "UART BAUD RATE HUNT"),
          span({ class: "panel-meta", style: () => uartClear.val ? "color:var(--proto-uart)" : "color:var(--state-warn)" },
            () => uartClear.val ? `Decoded @ ${uartBaud.val} bps` : `${uartBaud.val} bps`)
        ),
        div({ style: "display:flex;gap:6px;flex-wrap:wrap" },
          bauds.map(b => button({
            class: () => "cmd-btn" + (uartBaud.val === b ? " active" : ""),
            onclick: () => selectUartBaud(b)
          }, String(b))),
          button({
            class: "cmd-btn",
            ...tipHandlers("Sweep 115200, 57600, 38400, and 9600 bps to lock onto Hestia automatically"),
            onclick: triggerUartAutodetect
          }, ICONS.radar(), "Auto-Detect")
        ),
        div({ class: () => "flag-box " + (uartClear.val || ctfUart.val ? "captured" : "scrambled") },
          span({ class: "panel-meta" }, () => (uartClear.val || ctfUart.val) ? "Cleartext Message" : "Wire Preview"),
          span({ class: "flag-value" }, () => ctfUart.val || uartSample.val || "Select a baud rate or click Auto-Detect")
        )
      ),
      div({ class: "panel-section" },
        div({ class: "panel-hdr" },
          span({ class: "panel-title" }, "BEFORE vs AFTER GLITCH"),
          button({
            class: "cmd-btn glitch",
            disabled: safetyState.val === "Stopped",
            ...tipHandlers("Inject a bounded 10 us LOW pulse on Probe 0 to corrupt Hestia's UART checksum"),
            onclick: () => {
              lastUartGlitch.val = "Bit-flip injected -> Daphne checksum mismatch & GP18 alarm";
              triggerHardwareCommand("U", "Injected 10 us UART bit-flip glitch", "UART");
            }
          }, ICONS.zap(), "UART Glitch")
        ),
        div({ class: "before-after-flow" },
          div({ class: "ba-card" },
            span({ class: "ba-label" }, "Sent by Hestia"),
            span({ class: "ba-val" }, sentStr)
          ),
          div({ class: "ba-mid" + (isCorrupt ? " glitch" : "") },
            span(isCorrupt ? "10 us Glitch" : "Clean Wire"),
            span("->")
          ),
          div({ class: "ba-card" },
            span({ class: "ba-label" + (isCorrupt ? " glitch" : "") }, "Received by Daphne"),
            span({ class: "ba-val" }, recvStr)
          )
        )
      ),
      ProtocolObjectiveStrip(0, "Run Auto-Detect", "Click UART Glitch")
    );
  };
}

function I2cPanel() {
  return () => {
    let isGlitch = activeGlitchBus.val === "I2C_ACK" || activeGlitchBus.val === "I2C_STRETCH";
    let hasI = (ctfImask.val & 2) !== 0;
    let hasE = (ctfEmask.val & 2) !== 0;
    let ax = accelX.val, ay = accelY.val, az = accelZ.val;
    return div({ class: "inspector-body" },
      div({ class: "panel-section" },
        div({ class: "panel-hdr" },
          span({ class: "panel-title" }, "GY-511 SENSOR & DAPHNE I²C"),
          span({ class: "panel-meta", style: isGlitch ? "color:var(--state-glitch)" : "color:var(--proto-i2c)" },
            isGlitch ? "Fault Active -> 9-Clk Recovery" : "400 kHz Live")
        ),
        div({ class: "sensor-readout-grid" },
          div({ class: "sensor-metric" },
            div({ class: "sensor-metric-label" }, "Accel X"),
            div({ class: "sensor-metric-val" }, ax !== null ? `${ax} mg` : "--")
          ),
          div({ class: "sensor-metric" },
            div({ class: "sensor-metric-label" }, "Accel Y"),
            div({ class: "sensor-metric-val" }, ay !== null ? `${ay} mg` : "--")
          ),
          div({ class: "sensor-metric" },
            div({ class: "sensor-metric-label" }, "Accel Z"),
            div({ class: "sensor-metric-val" }, az !== null ? `${az} mg` : "--")
          )
        ),
        div({ class: "token-stream" },
          span({ class: "tok start" }, "START"),
          span({ class: "tok addr" }, "0x19 W"),
          span({ class: isGlitch ? "tok nack" : "tok ack" }, isGlitch ? "NACK Killed" : "ACK"),
          span({ class: isGlitch ? "tok rec" : "tok" }, isGlitch ? "9-CLK RECOVERY" : "Reg 0xA8"),
          span({ class: "tok stop" }, "STOP")
        ),
        div({ style: "display:flex;gap:8px;flex-wrap:wrap" },
          button({
            class: "cmd-btn glitch",
            disabled: safetyState.val === "Stopped",
            ...tipHandlers("Pull SDA high during 9th ACK clock for 50 us to trigger Daphne 9-clock recovery"),
            onclick: () => {
              lastI2cFault.val = "Kill ACK -> 50 us NACK on 9th clock -> Daphne 9-clock recovery restored GY-511";
              triggerHardwareCommand("A", "Injected 50 us I²C ACK-Kill", "I2C_ACK");
            }
          }, ICONS.zap(), "Kill ACK"),
          button({
            class: "cmd-btn warn",
            disabled: safetyState.val === "Stopped",
            ...tipHandlers("Hold SCL low for 50 us to test Daphne clock-stretch tolerance"),
            onclick: () => {
              lastI2cFault.val = "Stretch Clock -> 50 us SCL hold -> Daphne paused cleanly and resumed GY-511";
              triggerHardwareCommand("S", "Injected 50 us I²C Clock Stretch", "I2C_STRETCH");
            }
          }, ICONS.cpu(), "Stretch Clock")
        ),
        div({ class: () => "flag-box " + ((hasI && hasE) ? "captured" : "") },
          span({ class: "panel-meta" }, "Bus Recovery & Status"),
          span({ class: "flag-value" }, () => lastI2cFault.val || (hasI ? "No secret message on I²C — GY-511 sniffed. Click Kill ACK or Stretch Clock to verify 9-clock recovery." : "No secret message on I²C — sniffing Probes 2 & 3 on 400 kHz bus..."))
        )
      ),
      ProtocolObjectiveStrip(1, "Sniff 3+ I²C frames", "Click Kill ACK or Stretch Clock")
    );
  };
}

function WifiPanel() {
  const channels = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13];
  return () => div({ class: "inspector-body" },
    div({ class: "panel-section" },
      div({ class: "panel-hdr" },
        span({ class: "panel-title" }, "WI-FI CHANNEL HUNT"),
        span({ class: "panel-meta", style: "color:var(--proto-wifi)" },
          wifiChannelMode.val >= 1 ? `Ch ${wifiChannelMode.val} · SoftAP Ch 6` : "SoftAP Ch 6")
      ),
      div({ class: "ch-grid" },
        channels.map(ch => button({
          class: () => "btn-ch" + (wifiChannelMode.val === ch ? " active" : ""),
          onclick: () => selectWifiChannel(ch)
        }, `Ch ${ch}`))
      ),
      div({ class: () => "flag-box " + (wifiClear.val ? "captured" : "scrambled") },
        span({ class: "panel-meta" }, () => wifiClear.val ? `Message on Ch ${wifiSampleCh.val}` : (wifiScanningUi.val ? "Auto-Scanning Ch 1–13..." : (wifiSampleCh.val >= 1 ? `Ch ${wifiSampleCh.val} Capture` : "Channel Capture"))),
        span({ class: "flag-value" }, () => (wifiClear.val ? (ctfWifi.val || wifiSample.val) : wifiSample.val) || (wifiScanningUi.val ? `Scanning Ch ${wifiChannelMode.val || 1}...` : "Select a channel 1–13 or click Auto-Scan"))
      ),
      div({ style: "display:flex;gap:8px;align-items:center;flex-wrap:wrap" },
        button({
          class: "cmd-btn",
          disabled: safetyState.val === "Stopped",
          ...tipHandlers("Sweep Ch 1–13 in hardware until Iris cleartext is captured"),
          onclick: triggerWifiAutoScan
        }, ICONS.radar(), "Auto-Scan"),
        button({
          class: "cmd-btn rf-btn",
          disabled: safetyState.val === "Stopped" || wifiChannelMode.val < 1,
          ...tipHandlers("Send targeted Wi-Fi jam burst to Iris on the selected channel; Iris hops if hit"),
          onclick: () => jamWifiChannel(wifiChannelMode.val)
        }, ICONS.wifi(), () => wifiChannelMode.val >= 1 ? `Jam Wi-Fi Ch ${wifiChannelMode.val}` : "Jam Wi-Fi")
      )
    ),
    ProtocolObjectiveStrip(2, "Find true Ch 1–13", "Click Jam Wi-Fi")
  );
}

function ZigbeePanel() {
  const channels = [11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26];
  return () => div({ class: "inspector-body" },
    div({ class: "panel-section" },
      div({ class: "panel-hdr" },
        span({ class: "panel-title" }, "ZIGBEE 802.15.4 HUNT"),
        span({ class: "panel-meta", style: "color:var(--proto-zigbee)" },
          zigbeeChannel.val >= 11 ? `Ch ${zigbeeChannel.val} · ${2405 + 5 * (zigbeeChannel.val - 11)} MHz` : "Select Ch 11–26")
      ),
      div({ class: "ch-grid-zigbee" },
        channels.map(ch => button({
          class: () => "btn-ch btn-ch-zigbee" + (zigbeeChannel.val === ch ? " active" : ""),
          onclick: () => selectZigbeeChannel(ch)
        }, `Ch ${ch}`))
      ),
      div({ class: () => "flag-box " + (z154Clear.val ? "captured" : "scrambled") },
        span({ class: "panel-meta" }, () => z154Clear.val ? `Message on Ch ${zigbeeChannel.val}` : (zigbeeScanningUi.val ? "Auto-Scanning Ch 11–26..." : (zigbeeChannel.val >= 11 ? `Ch ${zigbeeChannel.val} Capture` : "Channel Capture"))),
        span({ class: "flag-value" }, () => (z154Clear.val ? (ctfZigbee.val || z154Sample.val) : z154Sample.val) || (zigbeeScanningUi.val ? `Scanning Ch ${zigbeeChannel.val || 11}...` : "Select a channel 11–26 or click Auto-Scan"))
      ),
      div({ style: "display:flex;gap:8px;align-items:center;flex-wrap:wrap" },
        button({
          class: "cmd-btn",
          disabled: safetyState.val === "Stopped",
          ...tipHandlers("Sweep Ch 11–26 in hardware until Iris cleartext is captured"),
          onclick: triggerZigbeeAutoScan
        }, ICONS.radar(), "Auto-Scan"),
        button({
          class: "cmd-btn zigbee-btn",
          disabled: safetyState.val === "Stopped" || zigbeeChannel.val < 11,
          ...tipHandlers("Send targeted 802.15.4 jam frame to Iris on active channel; Iris hops if hit"),
          onclick: jamZigbeeChannel
        }, ICONS.radio(), () => zigbeeChannel.val >= 11 ? `Jam Zigbee Ch ${zigbeeChannel.val}` : "Jam Zigbee")
      )
    ),
    ProtocolObjectiveStrip(3, "Find true Ch 11–26", "Click Jam Zigbee")
  );
}
function ContextualInspectorPane() {
  return div({ class: "inspector-pane" },
    () => {
      let m = toolMode.val;
      if (m === "UART") return UartPanel()();
      if (m === "I2C") return I2cPanel()();
      if (m === "WIFI") return WifiPanel()();
      if (m === "ZIGBEE") return ZigbeePanel()();
      if (m === "AUTO") return AutoPanel();
      return OffPanel();
    }
  );
}

// ===========================================================================
// 8. Bottom Packet Pane (Mode-Filtered + Plain-English Summary Default)
// ===========================================================================
function toggleSort(col) {
  if (sortColumn.val === col) sortAscending.val = !sortAscending.val;
  else {
    sortColumn.val = col;
    sortAscending.val = (col === "proto" || col === "chan" || col === "summary");
  }
}

function sortIndicator(col) {
  if (sortColumn.val !== col) return " ↕";
  return sortAscending.val ? " ▲" : " ▼";
}

function BottomWiresharkPane() {
  return div({ class: "bottom-wireshark" },
    div({ class: "ws-toolbar" },
      button({
        class: () => "cmd-btn" + (faultsOnly.val ? " glitch" : ""),
        ...tipHandlers("Show only fault injection events and corrupted checksums"),
        onclick: () => { faultsOnly.val = !faultsOnly.val; }
      }, ICONS.filter(), "Faults Only"),
      button({
        class: () => "hurry-toggle" + (hideRepeatingNormal.val ? "" : " off"),
        ...tipHandlers("Collapses consecutive normal polling frames so faults and new protocol events stay visible."),
        onclick: () => { hideRepeatingNormal.val = !hideRepeatingNormal.val; }
      }, "Hide Repeating Normal"),
      input({
        class: "ws-input",
        type: "text",
        placeholder: "Search frames: hestia, 0x19, iris, glitch...",
        oninput: (e) => { filterSearchText.val = e.target.value; }
      }),
      button({
        class: "cmd-btn",
        onclick: () => { faultsOnly.val = false; filterSearchText.val = ""; }
      }, "Clear"),
      () => span({ class: "mono", style: "font-size:12px;color:var(--fg-muted)" },
        `${packets.val.length} frames`)
    ),
    div({ class: "ws-split" },
      div({ class: "ws-table-wrap" },
        table({ class: "ws-table" },
          thead(
            tr(
              th({ onclick: () => toggleSort("id") }, () => "NO." + sortIndicator("id")),
              th({ onclick: () => toggleSort("tsUs") }, () => "TIME" + sortIndicator("tsUs")),
              th({ onclick: () => toggleSort("proto") }, () => "PROTO" + sortIndicator("proto")),
              th({ onclick: () => toggleSort("chan") }, () => "CHANNEL / RATE" + sortIndicator("chan")),
              th({ onclick: () => toggleSort("len") }, () => "LEN" + sortIndicator("len")),
              th({ onclick: () => toggleSort("summary") }, () => "SUMMARY" + sortIndicator("summary"))
            )
          ),
          () => {
            let protoFilter = filterProtocol.val;
            let onlyFaults  = faultsOnly.val;
            let search      = filterSearchText.val.toLowerCase().trim();
            let col         = sortColumn.val;
            let asc         = sortAscending.val;
            let collapse    = hideRepeatingNormal.val;

            let list = packets.val.filter(p => {
              if (onlyFaults && !p.isFault && p.tagClass !== "fault") return false;
              if (protoFilter !== "ALL") {
                if (protoFilter === "UART" && p.proto !== "UART" && p.proto !== "FAULT") return false;
                if (protoFilter === "I2C" && p.proto !== "I2C" && p.proto !== "FAULT") return false;
                if (protoFilter === "WIFI" && p.proto !== "WIFI" && p.proto !== "FAULT") return false;
                if (protoFilter === "ZIGBEE" && p.proto !== "ZIGBEE" && p.proto !== "FAULT") return false;
              }
              if (search) {
                let txt = (p.time + " " + p.proto + " " + p.chan + " " + p.summary).toLowerCase();
                if (!txt.includes(search)) return false;
              }
              return true;
            });

            let sorted = list.slice().sort((a, bVal) => {
              let va = a[col];
              let vb = bVal[col];
              if (typeof va === "string" && typeof vb === "string") {
                let cmp = va.localeCompare(vb);
                return asc ? cmp : -cmp;
              }
              let diff = (va || 0) - (vb || 0);
              return asc ? diff : -diff;
            });

            let renderedRows = [];
            let consecutiveNormal = 0;

            for (let i = 0; i < sorted.length && renderedRows.length < 120; i++) {
              let p = sorted[i];
              if (collapse && !p.isFault && p.tagClass !== "fault") {
                consecutiveNormal++;
                if (consecutiveNormal > 2) continue;
              } else {
                if (consecutiveNormal > 2) {
                  renderedRows.push(tr({ class: "row-hurry" },
                    td("-"), td("-"), td(span({ class: "badge hurry" }, "HIDDEN")), td("-"), td("-"),
                    td(`${consecutiveNormal - 2} repeating normal frames hidden`)
                  ));
                }
                consecutiveNormal = 0;
              }

              renderedRows.push(tr({
                class: () => (selectedPacket.val === p ? "selected " : "") + (p.tagClass === "fault" ? "row-fault" : ""),
                onclick: () => { selectedPacket.val = p; }
              },
                td(p.id || "-"),
                td(p.time),
                td(span({ class: `badge ${p.tagClass}` }, p.proto)),
                td(p.chan),
                td(p.len != null ? `${p.len} B` : "-"),
                td({ class: "col-summary" }, p.summary)
              ));
            }
            if (collapse && consecutiveNormal > 2) {
              renderedRows.push(tr({ class: "row-hurry" },
                td("-"), td("-"), td(span({ class: "badge hurry" }, "HIDDEN")), td("-"), td("-"),
                td(`${consecutiveNormal - 2} repeating normal frames hidden`)
              ));
            }
            if (renderedRows.length === 0) {
              renderedRows.push(tr(
                td({ colspan: 6, style: "color:var(--fg-muted);padding:14px;text-align:center" },
                  toolMode.val === "OFF" ? "Capture is Off. Select Auto or a protocol mode above to inspect live packets." : "No matching packets yet.")
              ));
            }

            return tbody(renderedRows);
          }
        )
      ),
      div({ class: "ws-hex-pane" },
        () => {
          let p = selectedPacket.val;
          return div({ style: "display:flex;flex-direction:column;gap:8px" },
            div({ class: "pkt-detail-hdr" },
              span({ class: "mono", style: "font-weight:700" }, p ? `Packet #${p.id} · ${p.proto} · ${p.len} B` : "No packet selected"),
              button({
                class: "cmd-btn pkt-toggle-btn",
                disabled: !p,
                onclick: () => { showRawHex.val = !showRawHex.val; }
              }, ICONS.code(), showRawHex.val ? "Plain Summary" : "View Raw Hex")
            ),
            !p
              ? div({ class: "plain-summary-card" }, span({ style: "color:var(--fg-muted)" }, "Click any row in the packet table to read a plain-English summary or inspect raw bytes."))
              : showRawHex.val
                ? pre({ class: "hex-dump-pre" }, formatHex(p.raw || p.summary))
                : div({ class: "plain-summary-card" },
                    div({ class: "plain-summary-row" },
                      span({ class: "plain-summary-key" }, "Summary"),
                      span({ class: "plain-summary-val", style: "font-weight:700" }, p.summary)
                    ),
                    Object.entries(p.details || {}).map(([k, v]) =>
                      div({ class: "plain-summary-row" },
                        span({ class: "plain-summary-key" }, k),
                        span({ class: "plain-summary-val" }, String(v))
                      )
                    )
                  )
          );
        }
      )
    )
  );
}

// ===========================================================================
// 9. Telemetry, WebSocket & Boot
// ===========================================================================
function ToastOverlay() {
  return div({ class: "toast-container" },
    () => toastMsg.val
      ? div({ class: "toast" },
          span(toastMsg.val),
          span({ style: "cursor:pointer;opacity:0.7", onclick: () => { toastMsg.val = ""; } }, "x")
        )
      : span()
  );
}

function TooltipOverlay() {
  return () => hoverTip.val
    ? div({
        class: "floating-tip",
        style: `left:${Math.min(window.innerWidth - 290, hoverTip.val.x)}px;top:${Math.min(window.innerHeight - 70, hoverTip.val.y)}px`
      }, hoverTip.val.text)
    : span();
}

function updateFromTelemetry(d) {
  let nowMs = Date.now();
  if (d.safety) {
    safetyState.val = (d.safety === "ABORTED" || d.safety === "Stopped") ? "Stopped" : "Active";
  }
  if (d.tool_mode && nowMs >= modeLockUntil && d.tool_mode !== toolMode.val) {
    toolMode.val = d.tool_mode;
    filterProtocol.val = (d.tool_mode === "OFF" || d.tool_mode === "AUTO") ? "ALL" : d.tool_mode;
  }

  if (d.uart_pkts != null) {
    if (prevUartPkts >= 0 && d.uart_pkts > prevUartPkts) uartActiveUntil.val = nowMs + 1200;
    prevUartPkts = d.uart_pkts;
    uartPkts.val = d.uart_pkts;
  }
  if (d.i2c_pkts != null) {
    if (prevI2cPkts >= 0 && d.i2c_pkts > prevI2cPkts) i2cActiveUntil.val = nowMs + 1200;
    prevI2cPkts = d.i2c_pkts;
    i2cPkts.val = d.i2c_pkts;
  }
  if (d.wifi_pkts != null) {
    if (prevWifiPkts >= 0 && d.wifi_pkts > prevWifiPkts) wifiActiveUntil.val = nowMs + 1200;
    prevWifiPkts = d.wifi_pkts;
    wifiPkts.val = d.wifi_pkts;
  }
  if (d.ieee154_pkts != null) {
    if (prevZigbeePkts >= 0 && d.ieee154_pkts > prevZigbeePkts) zigbeeActiveUntil.val = nowMs + 1200;
    prevZigbeePkts = d.ieee154_pkts;
    zigbeePkts.val = d.ieee154_pkts;
  }

  if (d.corrupted_frames != null) corruptedCount.val = d.corrupted_frames;
  if (d.wifi_ch != null) wifiActiveCh.val = d.wifi_ch;
  if (d.wifi_mode != null && nowMs >= wifiLockUntil) wifiChannelMode.val = d.wifi_mode;
  if (d.zigbee_ch != null && nowMs >= zigbeeLockUntil) zigbeeChannel.val = d.zigbee_ch;
  if (d.uart_clear != null) uartClear.val = d.uart_clear;
  if (d.uart_sample != null) uartSample.val = d.uart_sample;
  if (d.wifi_clear != null) {
    if (wifiAutoScanning && d.wifi_clear) {
      wifiAutoScanning = false;
      wifiScanningUi.val = false;
      showToast(`Locked Iris Wi-Fi on Ch ${d.wifi_sample_ch || wifiChannelMode.val}`);
    }
    wifiClear.val = d.wifi_clear;
  }
  if (d.wifi_sample_ch != null) wifiSampleCh.val = d.wifi_sample_ch;
  if (d.wifi_sample != null) wifiSample.val = d.wifi_sample;
  if (d.z154_clear != null) {
    if (zigbeeAutoScanning && d.z154_clear) {
      zigbeeAutoScanning = false;
      zigbeeScanningUi.val = false;
      showToast(`Locked Iris Zigbee on Ch ${d.zigbee_ch || zigbeeChannel.val}`);
    }
    z154Clear.val = d.z154_clear;
  }
  if (d.z154_sample != null) z154Sample.val = d.z154_sample;
  if (d.iris_rssi != null) irisRssi.val = d.iris_rssi;
  if (d.iris_verified != null) irisVerified.val = d.iris_verified;

  if (d.ctf) {
    ctfUart.val = d.ctf.uart || "";
    ctfWifi.val = d.ctf.wifi || "";
    ctfZigbee.val = d.ctf.zigbee || "";
    ctfBonus.val = d.ctf.bonus || "";
    if (d.ctf.imask != null) ctfImask.val = d.ctf.imask;
    if (d.ctf.emask != null) ctfEmask.val = d.ctf.emask;
  }

  if (d.accel && d.accel.x != null && d.accel.y != null && d.accel.z != null) {
    updateAccelHistory(d.accel.x, d.accel.y, d.accel.z);
  }
  if (d.mag && d.mag.x != null) {
    magStr.val = `X:${d.mag.x} Y:${d.mag.y} Z:${d.mag.z}`;
  }
}

function connectDataStream() {
  fetchNextPcapChunk();
  const pollTelemetry = () => {
    fetch("/telemetry").then(r => r.json()).then(updateFromTelemetry).catch(() => {});
  };
  pollTelemetry();

  // 2 Hz animation step (500 ms) — matches ⓘ tooltip: "The animation is slowed down to 2 Hz to see the packets moving."
  setInterval(() => {
    if (!isPaused.val && toolMode.val !== "OFF" && safetyState.val !== "Stopped") {
      animPhase.val = (animPhase.val + 14) % 100;
    }
  }, 500);

  setInterval(() => {
    fetchNextPcapChunk();
  }, 450);

  // Periodic HTTP telemetry fallback so SoftAP bursts or bridge restarts never leave UI stale
  setInterval(pollTelemetry, 750);

  let wsProtocol = window.location.protocol === "https:" ? "wss:" : "ws:";
  let wsHost = window.location.port === "8888" ? "localhost:8889" : window.location.host;
  function openWs() {
    let ws = new WebSocket(`${wsProtocol}//${wsHost}/ws`);
    ws.onmessage = (evt) => {
      try {
        let d = JSON.parse(evt.data);
        if (d.type === "telemetry") updateFromTelemetry(d);
        else if (d.type === "alert") showToast(d.msg || "Alert");
      } catch (e) {}
    };
    ws.onclose = () => { setTimeout(openWs, 1200); };
  }
  openWs();
}
function App() {
  return div(
    ToastOverlay(),
    TooltipOverlay(),
    div({ class: "workbench-root" },
      div({ class: "workbench-header" },
        TopHeaderBar(),
        ToolBeltBar()
      ),
      div({ class: "upper-workbench" },
        BreadboardCanvas(),
        ContextualInspectorPane()
      ),
      BottomWiresharkPane()
    )
  );
}

van.add(document.getElementById("app"), App());
connectDataStream();
