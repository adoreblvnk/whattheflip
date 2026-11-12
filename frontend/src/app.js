import van from "vanjs-core";

const { div, span, button, a, table, thead, tbody, tr, th, td, pre, strong, input } = van.tags;
const svgNs = van.tags("http://www.w3.org/2000/svg");
const { svg, g, rect, line, path, text, polyline } = svgNs;

// ===========================================================================
// 1. Hardware, Telemetry & Sensor State
// ===========================================================================
const safetyState        = van.state("ARMED");
const toastMsg           = van.state("");
const isPaused           = van.state(false);
const activeGlitchBus    = van.state(""); // "" | "UART" | "I2C_ACK" | "I2C_STRETCH" | "RF"
const activeInspTab      = van.state("WAVE"); // "WAVE" | "I2C_TOK" | "RF_RADAR" | "RTOS"

// Wi-Fi Single-Radio Channel Control
const wifiChannelMode    = van.state(6);
const wifiActiveCh       = van.state(6);
// Tool Engine & Capture Mode Override (Flipper Zero Style)
const toolMode            = van.state("AUTO");
const discoveryLocked     = van.state(false);
const zigbeeChannel       = van.state(15);
const scopeFilter         = van.state("ALL"); // "ALL" | "UART" | "I2C"
const liveProbeState      = van.state({ p0: 1, p1: 1, p2: 1, p3: 1 });
const probeShift          = van.state(0);




// Live Telemetry Counters
const uartPkts           = van.state(0);
const i2cPkts            = van.state(0);
const wifiPkts           = van.state(0);
const zigbeePkts         = van.state(0);
const berPct             = van.state("0.000%");
const corruptedCount     = van.state(0);
const pcapSize           = van.state(24);
const pcapSyncedBytes    = van.state(0);

// Iris Wireless Victim Proximity & OTA State
const irisRssi           = van.state(-38);
const irisVerified       = van.state(true);
const irisRate           = van.state("2 Hz");
// GY-511 IMU Sensor & Animation
const accelX             = van.state(null);
const accelY             = van.state(null);
const accelZ             = van.state(null);
const accelAvgX          = van.state(0);
const rawRegHex          = van.state("AWAITING I2C");
const magStr             = van.state("");
const animPhase          = van.state(0);
const uptimeS            = van.state(0);

// Live Hardware Bus Activity & Value-Change Tracking
const uartActiveUntil    = van.state(0);
const i2cActiveUntil     = van.state(0);
const rfActiveUntil      = van.state(0);
const uartValueChangedUntil = van.state(0);
const i2cValueChangedUntil  = van.state(0);
const rfValueChangedUntil   = van.state(0);
const latestUartLabel    = van.state("HESTIA");
const latestI2cByte      = van.state("0x19");
const highSpeedLog       = van.state([]);

// Wireshark Filter, Search & Sort
const filterProtocol     = van.state("ALL");
const filterSearchText   = van.state("");
const sortColumn         = van.state("id");
const sortAscending      = van.state(false);
const hurryModeEnabled   = van.state(false);

// PCAP Audit Counters
const pcapAuditTotal     = van.state(0);
const pcapAuditNominal   = van.state(0);
const pcapAuditFaults    = van.state(0);
const pcapAuditRecovered = van.state(0);
const selectedPacket     = van.state(null);
const packets            = van.state([]);

let clientTotalUartBits  = 0;
let clientCorruptFrames  = 0;
let clientPacketSeq      = 0;
let xFilterRing          = [];
let prevUartPkts         = -1;
let prevI2cPkts          = -1;
let prevRfPkts           = -1;
// ===========================================================================
// 2. Canonical Hex Dump & Command Relay
// ===========================================================================
function formatHex(dataBytes) {
  if (!dataBytes) return "Select any packet in the table below or click a PDU on the breadboard.";
  let bytes = [];
  if (dataBytes instanceof Uint8Array || Array.isArray(dataBytes)) {
    bytes = Array.from(dataBytes);
  } else if (typeof dataBytes === "string") {
    for (let i = 0; i < dataBytes.length; i++) {
      bytes.push(dataBytes.charCodeAt(i) & 0xff);
    }
  }
  if (bytes.length === 0) return "0000:  (0 payload bytes)";

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

function showToast(msg) {
  toastMsg.val = msg;
  setTimeout(() => { if (toastMsg.val === msg) toastMsg.val = ""; }, 3200);
}

function triggerHardwareCommand(cmd, feedbackText, glitchBus = "") {
  if (glitchBus) {
    activeGlitchBus.val = glitchBus;
    setTimeout(() => { if (activeGlitchBus.val === glitchBus) activeGlitchBus.val = ""; }, 2200);
  }
  showToast(`${feedbackText} — relaying to ESP32...`);
  fetch("/cmd?c=" + cmd, { method: "POST" })
    .then(() => {
      showToast(`${feedbackText} — ✅ sent to hardware`);
      fetchNextPcapChunk(false);
      setTimeout(() => fetchNextPcapChunk(false), 180);
    })
    .catch(() => showToast(`${feedbackText} — ❌ FAILED: ESP32 not reachable`));
}

function triggerIrisCmd(cmd, feedbackText) {
  showToast(`${feedbackText} — relaying via ESP32 to Iris...`);
  fetch("/cmd?iris_cmd=" + cmd, { method: "POST" })
    .then(() => showToast(`${feedbackText} — ✅ sent (Iris OTA ack appears in packet log)`))
    .catch(() => showToast(`${feedbackText} — ❌ FAILED: ESP32 not reachable`));
}

function selectWifiChannel(ch) {
  wifiChannelMode.val = ch;
  let label = ch === 0 ? "All-Channel Sweep (Ch 1..13)" : `Locked to Channel ${ch}`;
  fetch("/cmd?ch=" + ch, { method: "POST" })
    .then(() => showToast("Wi-Fi Radio: " + label))
    .catch(() => showToast("Wi-Fi Radio: " + label));
}
function selectToolMode(mode) {
  toolMode.val = mode;
  if (mode !== "AUTO") {
    discoveryLocked.val = true;
  }
  let label = mode === "AUTO" ? "Auto-Discovery Engine" : `${mode} Tool Mode Override`;
  fetch("/cmd?mode=" + mode.toLowerCase(), { method: "POST" })
    .then(() => showToast(`Capture Engine: ${label}`))
    .catch(() => showToast(`Capture Engine: ${label}`));

  if (mode === "UART") activeInspTab.val = "WAVE";
  else if (mode === "I2C") activeInspTab.val = "I2C_TOK";
  else if (mode === "WIFI" || mode === "ZIGBEE") activeInspTab.val = "RF_RADAR";
  else if (mode === "EXPLOIT") activeInspTab.val = "WAVE";
}

function toggleDiscoveryLock() {
  let next = !discoveryLocked.val;
  discoveryLocked.val = next;
  let label = next ? "LOCKED (Manual Override Engaged)" : "UNLOCKED (Auto-Discovery Active)";
  fetch("/cmd?lock_disc=" + (next ? "1" : "0"), { method: "POST" })
    .then(() => showToast(`Auto-Discovery Gate: ${label}`))
    .catch(() => showToast(`Auto-Discovery Gate: ${label}`));
}

function selectZigbeeChannel(ch) {
  zigbeeChannel.val = ch;
  let freq = 2405 + 5 * (ch - 11);
  fetch("/cmd?zch=" + ch, { method: "POST" })
    .then(() => showToast(`802.15.4 Radio: Locked to Ch ${ch} (${freq} MHz)`))
    .catch(() => showToast(`802.15.4 Radio: Locked to Ch ${ch} (${freq} MHz)`));
}


function pushHighSpeedLog(proto, msg, isFault = false) {
  let now = new Date();
  let ts = now.toTimeString().split(" ")[0] + "." + String(now.getMilliseconds()).padStart(3, "0");
  highSpeedLog.val = [{ ts, proto, msg, isFault }, ...highSpeedLog.val].slice(0, 120);
}

function updateAccelHistory(rx, ry, rz) {
  let changed = (accelX.val !== null) && (Math.abs(rx - accelX.val) > 8 || Math.abs(ry - accelY.val) > 8 || Math.abs(rz - accelZ.val) > 8);
  accelX.val = rx;
  accelY.val = ry;
  accelZ.val = rz;
  if (changed) {
    i2cValueChangedUntil.val = Date.now() + 1400;
    latestI2cByte.val = `0x19 Δ(${rx},${ry},${rz})`;
  }
  xFilterRing.push(rx);
  if (xFilterRing.length > 8) xFilterRing.shift();
  let sumX = xFilterRing.reduce((a, bVal) => a + bVal, 0);
  accelAvgX.val = Math.round(sumX / xFilterRing.length);
}

function recomputeClientBer() {
  let totalCorrupt = Math.max(clientCorruptFrames, corruptedCount.val);
  corruptedCount.val = totalCorrupt;
  let bits = Math.max(clientTotalUartBits, uartPkts.val * 160);
  if (bits > 0 && totalCorrupt > 0) {
    berPct.val = (((totalCorrupt * 2.0) / bits) * 100.0).toFixed(3) + "%";
  } else if (totalCorrupt === 0) {
    berPct.val = "0.000%";
  }
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

  let proto         = "UART";
  let tagClass      = "uart";
  let chan          = `Probe ${probeId}`;
  let summary       = "";
  let inboundAscii  = "HESTIA:128:FF7F";
  let outboundAscii = "HESTIA:128:FF7F";
  let isFault       = false;

  if (protoId === 1) {
    proto    = "UART";
    tagClass = "uart";
    chan     = "Row 22->13 (GP0->GP1)";
    let ascii = "";
    for (let i = 0; i < body.length; i++) {
      if (body[i] >= 32 && body[i] <= 126) ascii += String.fromCharCode(body[i]);
    }
    clientTotalUartBits += Math.max(body.length * 10, 160);
    inboundAscii  = ascii || "HESTIA:128:FF7F";
    outboundAscii = inboundAscii;

    let m = ascii.match(/HESTIA:(\d+):([0-9A-Fa-f]{1,4})/);
    if (m) {
      let ctr     = parseInt(m[1], 10) >>> 0;
      let rxCsum  = parseInt(m[2], 16) & 0xffff;
      let expCsum = (~ctr) & 0xffff;
      let expStr  = `HESTIA:${ctr}:${expCsum.toString(16).toUpperCase().padStart(4, "0")}`;
      inboundAscii = expStr;
      if (rxCsum === expCsum) {
        summary = `${ascii} [VALID ~ctr=0x${expCsum.toString(16).toUpperCase().padStart(4, "0")}]`;
      } else {
        clientCorruptFrames++;
        tagClass = "fault";
        isFault = true;
        outboundAscii = ascii;
        summary = `${ascii} [GLITCHED / CRC FAIL: rx=0x${rxCsum.toString(16).toUpperCase()} != exp=0x${expCsum.toString(16).toUpperCase()}]`;
      }
    } else {
      summary = ascii || "UART1 Frame";
    }
    recomputeClientBer();
  } else if (protoId === 2) {
    proto    = "I2C";
    tagClass = "i2c";
    chan     = "I2C1 Row 55/58 (400kHz)";
    if (body.length >= 1) {
      let addr7   = body[0] >> 1;
      let rw      = (body[0] & 1) ? "READ" : "WRITE";
      let devName = addr7 === 0x19 ? "LSM303 Accel" : (addr7 === 0x1e ? "LSM303 Mag" : "I2C Slave");
      let hexPrev = Array.from(body.slice(1, 7)).map(bVal => bVal.toString(16).toUpperCase().padStart(2, "0")).join(" ");

      if ((body[0] & 1) === 1 && body.length >= 7 && addr7 === 0x19) {
        let rx = (body[2] << 8) | body[1]; if (rx & 0x8000) rx -= 65536; rx >>= 4;
        let ry = (body[4] << 8) | body[3]; if (ry & 0x8000) ry -= 65536; ry >>= 4;
        let rz = (body[6] << 8) | body[5]; if (rz & 0x8000) rz -= 65536; rz >>= 4;
        rawRegHex.val = hexPrev;
        updateAccelHistory(rx, ry, rz);
        summary = `0x${addr7.toString(16).toUpperCase()} ${devName} ${rw} [0xA8: ${hexPrev}] -> X=${rx}mg Y=${ry}mg Z=${rz}mg`;
      } else {
        summary = `0x${addr7.toString(16).toUpperCase()} (${devName}) ${rw} [${hexPrev}]`;
      }
    } else {
      summary = "I2C0 Bus Transaction";
    }
  } else if (protoId === 4) {
    proto    = "WIFI";
    tagClass = "wifi";
    chan     = `2.4GHz Ch ${probeId}`;
    let isIris = false;
    for (let i = 0; i <= body.length - 6; i++) {
      if (body[i] === 0x02 && body[i + 1] === 0x00 && body[i + 2] === 0x00 &&
          body[i + 3] === 0x00 && body[i + 4] === 0x00 && body[i + 5] === 0x03) {
        isIris = true;
        break;
      }
    }
    summary = `802.11 Frame on Ch ${probeId} (${body.length}B)${isIris ? " [IRIS BSSID 02:00:00:00:00:03]" : ""}`;
  } else if (protoId === 5) {
    proto    = "ZIGBEE";
    tagClass = "zigbee";
    chan     = `Zigbee Ch ${probeId}`;
    let ascii = "";
    for (let i = 0; i < body.length; i++) {
      if (body[i] >= 32 && body[i] <= 126) ascii += String.fromCharCode(body[i]);
    }
    if (ascii.includes("20H") || ascii.includes("20HZ")) {
      irisRate.val = "20 Hz";
    } else if (ascii.includes("2H") || ascii.includes("2HZ")) {
      irisRate.val = "2 Hz";
    }
    let seqNum = body.length >= 3 ? body[2] : 0;
    summary = `Zigbee PAN 0x1234 Seq #${seqNum} ${ascii ? `("${ascii}")` : ""}`;
  } else if (protoId === 3) {
    proto    = "FAULT";
    tagClass = "fault";
    chan     = `Probe ${probeId}`;
    isFault  = true;
    clientCorruptFrames++;
    recomputeClientBer();
    let ascii = "";
    for (let i = 0; i < body.length; i++) {
      if (body[i] >= 32 && body[i] <= 126) ascii += String.fromCharCode(body[i]);
    }
    let faultDesc = probeId === 0 ? "10 µs UART1 TX Bit-Flip on Row 13 (15.0 mA across R1 220 Ω)"
                  : probeId === 2 ? "50 µs I2C0 9th-Clock ACK-Kill on Row 58 SDA -> Daphne 9-Clock Recovery"
                  : probeId === 3 ? "50 µs I2C0 Clock-Stretch on Row 55 SCL"
                  : "Targeted Zigbee Jam Burst on Ch 15 -> Iris 02:00:00:00:00:03";
    summary = `⚡ FAULT INJECTED: ${ascii || faultDesc}`;
  }
  pcapAuditTotal.val++;
  if (isFault) {
    pcapAuditFaults.val++;
    if (protoId === 3 && (probeId === 2 || probeId === 3)) pcapAuditRecovered.val++;
  } else {
    pcapAuditNominal.val++;
  }

  let nowMs = Date.now();
  if (proto === "UART" || (proto === "FAULT" && probeId === 0)) {
    uartActiveUntil.val = nowMs + 1200;
    if (outboundAscii !== latestUartLabel.val) {
      latestUartLabel.val = outboundAscii.slice(0, 14);
      uartValueChangedUntil.val = nowMs + 1200;
    }
  } else if (proto === "I2C" || (proto === "FAULT" && (probeId === 2 || probeId === 3))) {
    i2cActiveUntil.val = nowMs + 1200;
  } else if (proto === "WIFI" || proto === "ZIGBEE" || (proto === "FAULT" && probeId === 15)) {
    rfActiveUntil.val = nowMs + 1200;
    rfValueChangedUntil.val = nowMs + 1200;
  }
  pushHighSpeedLog(proto, `${chan} | ${summary}`, isFault);
  return {
    id: clientPacketSeq,
    tsUs: tsTotalUs,
    time: timeStr,
    proto,
    chan,
    len: body.length,
    summary,
    raw: body,
    tagClass,
    inboundAscii,
    outboundAscii,
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

    if (inclLen > 2048) {
      pos += 16;
      continue;
    }
    if (pos + 16 + inclLen > buf.length) break;

    let pktBytes = buf.slice(pos + 16, pos + 16 + inclLen);
    let rec      = dissectPcapRecord(tsSec, tsUsec, inclLen, pktBytes);
    newRecords.push(rec);
    pos += 16 + inclLen;
  }

  s_pcapCarryBuffer = buf.slice(pos);

  if (newRecords.length > 0 && !isPaused.val) {
    let merged = [...newRecords.reverse(), ...packets.val].slice(0, 500);
    packets.val = merged;
    let latestFault = newRecords.find(r => r.isFault || r.tagClass === "fault");
    if (latestFault) {
      selectedPacket.val = latestFault;
    } else if (!selectedPacket.val && merged.length > 0) {
      selectedPacket.val = merged[0];
    }
  }
}

let s_fetchingPcap = false;
async function fetchNextPcapChunk(forceManual = false) {
  if (s_fetchingPcap) return;
  s_fetchingPcap = true;
  try {
    let loops = 0;
    while (loops < 4) {
      loops++;
      let isStart = (s_pcapStreamOffset === 0);
      let resp    = await fetch(`/api/pcap?offset=${s_pcapStreamOffset}&limit=${PCAP_BATCH_LIMIT}`);
      if (!resp.ok) break;

      let totalHeader = resp.headers.get("X-Pcap-Total-Bytes");
      let totalVal = 0;
      if (totalHeader) {
        totalVal = parseInt(totalHeader, 10);
        if (!isNaN(totalVal) && totalVal > 0) pcapSize.val = totalVal;
      }

      // Jump to tail if initial backlog is huge (> 32 KB) so live view starts immediately
      if (isStart && totalVal > 32768) {
        let hdrBuf = await resp.arrayBuffer();
        let hdrBytes = new Uint8Array(hdrBuf).slice(0, 24);
        parseIncomingPcapBytes(hdrBytes, true);
        s_pcapStreamOffset = 24;
        // Fetch the full stream in 16KB chunks up to current tail
        while (s_pcapStreamOffset < totalVal) {
          let r2 = await fetch(`/api/pcap?offset=${s_pcapStreamOffset}&limit=16384`);
          if (!r2.ok) break;
          let b2 = new Uint8Array(await r2.arrayBuffer());
          if (b2.length === 0) break;
          parseIncomingPcapBytes(b2, false);
          s_pcapStreamOffset += b2.length;
          pcapSyncedBytes.val = s_pcapStreamOffset;
        }
        break;
      }

      let arrayBuf = await resp.arrayBuffer();
      let bytes    = new Uint8Array(arrayBuf);
      if (bytes.length > 0) {
        parseIncomingPcapBytes(bytes, isStart);
        s_pcapStreamOffset += bytes.length;
        pcapSyncedBytes.val = s_pcapStreamOffset;
        if (forceManual) showToast(`Synced ${bytes.length} B`);
        if (totalVal > s_pcapStreamOffset && bytes.length >= PCAP_BATCH_LIMIT) {
          continue;
        }
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
    isPaused.val = true;
    packets.val = [];
    clientPacketSeq = 0;
    pcapAuditTotal.val = 0;
    pcapAuditNominal.val = 0;
    pcapAuditFaults.val = 0;
    pcapAuditRecovered.val = 0;
    s_pcapCarryBuffer = new Uint8Array(0);
    parseIncomingPcapBytes(bytes, true);
    showToast(`Loaded PCAP: ${file.name} (${bytes.length} bytes, ${packets.val.length} frames)`);
  };
  reader.readAsArrayBuffer(file);
}

// ===========================================================================
// 3. UI: Top Command & Action Bar
// ===========================================================================
function TopHeaderBar() {
  return div({ class: "top-cmd-bar" },
    div({ class: "brand-cluster" },
      span({ class: "brand-logo" }, "WHATTHEFLIP"),
      span({ class: "mono", style: "font-size:11px;color:var(--proto-uart);font-weight:700" }, () => `BER: ${berPct.val}`),
      span({ class: "mono", style: () => `font-size:10px;font-weight:700;color:${uptimeS.val > 0 ? 'var(--success)' : 'var(--error)'}'` },
        () => uptimeS.val > 0 ? `● HW LIVE (${uptimeS.val}s)` : `● HW OFFLINE`)
    ),
    div({ class: "injector-cluster" },
      button({
        class: "cmd-btn glitch",
        onclick: () => { activeInspTab.val = "WAVE"; triggerHardwareCommand("U", "Injected 10 µs UART bit-flip into Row 13 across R1 220 Ω", "UART"); }
      }, "⚡ 10µs UART Glitch (Row 13)"),
      button({
        class: "cmd-btn glitch",
        onclick: () => { activeInspTab.val = "I2C_TOK"; triggerHardwareCommand("A", "Injected 50 µs I2C 9th-clock ACK-kill on Row 58 -> Daphne 9-clk recovery", "I2C_ACK"); }
      }, "💥 50µs I2C ACK-Kill (Row 58)"),
      button({
        class: "cmd-btn warn",
        onclick: () => { activeInspTab.val = "I2C_TOK"; triggerHardwareCommand("S", "Injected 50 µs SCL clock-stretch on Row 55", "I2C_STRETCH"); }
      }, "⏱ 50µs I2C Stretch (Row 55)"),
      button({
        class: "cmd-btn rf-btn",
        onclick: () => { activeInspTab.val = "RF_RADAR"; triggerHardwareCommand("W", "Targeted 802.15.4 burst strictly to Iris BSSID 02:00:00:00:00:03 on Ch 15", "RF"); }
      }, "📡 802.15.4 Jam (Ch 15)")
    ),
    div({ class: "safety-cluster" },
      button({
        class: "cmd-btn",
        onclick: () => {
          triggerHardwareCommand("R", "Reset to clean 0.000% BER baseline.");
          safetyState.val = "ARMED";
          corruptedCount.val = 0;
          berPct.val = "0.000%";
        }
      }, "Reset Baseline"),
      () => safetyState.val === "ABORTED"
        ? button({
            class: "cmd-btn rearm",
            onclick: () => { triggerHardwareCommand("R", "Probes re-armed"); safetyState.val = "ARMED"; }
          }, "RE-ARM PROBES")
        : button({
            class: "cmd-btn estop",
            onclick: () => { triggerHardwareCommand("E", "GPIO9 E-STOP: All probes floated in < 200 ns (FR-7)"); safetyState.val = "ABORTED"; }
          }, "🛑 E-STOP (<200ns)"),
      button({
        class: "cmd-btn",
        onclick: () => document.getElementById("pcap-file-input").click(),
        title: "Load and replay offline .pcap file"
      }, "📂 Replay .pcap"),
      input({
        type: "file",
        id: "pcap-file-input",
        accept: ".pcap",
        style: "display:none",
        onchange: handlePcapFileUpload
      }),
      a({ href: "/capture.pcap", download: "capture.pcap", class: "cmd-btn pcap-dl" }, "⬇ .pcap")
    )
  );
}
function ToolBeltBar() {
  const tools = [
    { id: "AUTO", label: "🔍 Auto-Discovery", cls: "tool-auto" },
    { id: "UART", label: "⚡ UART Glitcher", cls: "tool-uart" },
    { id: "I2C", label: "🎛️ I2C Hijacker", cls: "tool-i2c" },
    { id: "WIFI", label: "📡 Wi-Fi 6 Sniffer", cls: "tool-wifi" },
    { id: "ZIGBEE", label: "🐝 Zigbee 802.15.4", cls: "tool-zigbee" },
    { id: "EXPLOIT", label: "🎯 Exploit Suite", cls: "tool-exploit" }
  ];

  return div({ class: "tool-belt-bar" },
    div({ class: "tool-cluster" },
      span({ class: "mono", style: "font-size:11px;font-weight:800;color:var(--fg-muted);margin-right:4px" }, "TOOL ENGINE:"),
      tools.map(t => button({
        class: () => "tool-btn " + t.cls + (toolMode.val === t.id ? " active" : ""),
        onclick: () => selectToolMode(t.id)
      }, t.label))
    ),
    div({ class: "tool-cluster" },
      span({ class: "mono", style: "font-size:11px;color:var(--fg-muted);margin-right:2px" }, "AUTO-DETECT GATE:"),
      button({
        class: () => "lock-gate-btn " + (discoveryLocked.val ? "locked" : "unlocked"),
        onclick: toggleDiscoveryLock,
        title: "Click to toggle manual capture override lock"
      },
        () => discoveryLocked.val ? "🔒 OVERRIDE LOCKED" : "🔓 AUTO-DETECT ACTIVE"
      )
    )
  );
}


// ===========================================================================
// 4. UI: Upper Left - The HERO Breadboard Signal Path Canvas
// ===========================================================================
function BreadboardCanvas() {
  return div({ class: "topology-pane" },
    div({ class: "pane-header" },
      span("TOPOLOGY"),
      span({ class: "sampled-badge", title: "Hardware buses run at 115.2 kbps (UART) and 400 kHz (I²C); canvas motion is rate-sampled at 2 Hz from live silicon telemetry." },
        "2 Hz SAMPLED STREAM (115.2k / 400kHz HW)"),
      () => span({ style: "color:var(--proto-uart)" },
        `UART: ${uartPkts.val} | I2C: ${i2cPkts.val} | Wi-Fi: ${wifiPkts.val} | Zigbee: ${zigbeePkts.val}`)
    ),
    div({ class: "topology-svg-wrap" },
      () => {
        let ph        = animPhase.val;
        let estop     = safetyState.val === "ABORTED";
        let glitchBus = activeGlitchBus.val;
        let probeDash = estop ? "4 4" : "none";
        let nowMs     = Date.now();

        let uartLive  = !estop && (uartActiveUntil.val > nowMs || glitchBus === "UART");
        let i2cLive   = !estop && (i2cActiveUntil.val > nowMs || glitchBus === "I2C_ACK" || glitchBus === "I2C_STRETCH");
        let rfLive    = !estop && (rfActiveUntil.val > nowMs || glitchBus === "RF");

        let uartDelta = uartValueChangedUntil.val > nowMs || glitchBus === "UART";
        let i2cDelta  = i2cValueChangedUntil.val > nowMs || glitchBus === "I2C_ACK" || glitchBus === "I2C_STRETCH";
        let rfDelta   = rfValueChangedUntil.val > nowMs || glitchBus === "RF";

        let t1 = (ph % 100) / 100;
        let t2 = ((ph + 50) % 100) / 100;
        let pduUartCleanX = 158 + t1 * 72;
        let pduUartOutX   = 322 + t1 * 72;
        let pduI2cX       = 555 - t2 * 60;
        let pduRfX        = 450 - t2 * 220;
        let tiltSlot      = Math.max(0, Math.min(7, Math.floor((accelAvgX.val + 1000) / 250)));

        return svg({ id: "topology-svg", class: "topology-svg", viewBox: "0 0 680 270" },
          // 1. Simplex UART TX Line: Hestia GP0 (Row 22) -> R1 (220 Ω) -> Daphne GP1 (Row 13)
          line({
            class: "topo-wire", x1: 150, y1: 56, x2: 245, y2: 56,
            stroke: uartLive ? "var(--proto-uart)" : "#cbd5e1", "stroke-width": 2.5,
            onclick: () => { activeInspTab.val = "WAVE"; }
          }),
          path({ d: "M 237 52 L 244 56 L 237 60", fill: "none", stroke: uartLive ? "var(--proto-uart)" : "#cbd5e1", "stroke-width": 2 }),
          text({ x: 197, y: 34, fill: "var(--proto-uart)", "font-size": 10, "font-family": "monospace", "font-weight": "bold", "text-anchor": "middle" }, "Row 22 TX"),

          line({
            class: "topo-wire", x1: 315, y1: 56, x2: 410, y2: 56,
            stroke: glitchBus === "UART" ? "var(--state-glitch)" : (uartLive ? "var(--proto-uart)" : "#cbd5e1"),
            "stroke-width": glitchBus === "UART" ? 3.5 : 2.5,
            "stroke-dasharray": glitchBus === "UART" ? "5 3" : "none",
            onclick: () => { activeInspTab.val = "WAVE"; }
          }),
          path({ d: "M 402 52 L 409 56 L 402 60", fill: "none", stroke: glitchBus === "UART" ? "var(--state-glitch)" : (uartLive ? "var(--proto-uart)" : "#cbd5e1"), "stroke-width": 2 }),
          text({ x: 362, y: 34, fill: glitchBus === "UART" ? "var(--state-glitch)" : "var(--fg-muted)", "font-size": 10, "font-family": "monospace", "font-weight": "bold", "text-anchor": "middle" },
            glitchBus === "UART" ? "Row 13 ⚡GLITCH" : "Row 13 RX"),

          // 2. Probe 1 Tap (Row 22 -> WhatTheFlip GPIO5)
          path({
            class: "topo-wire", d: "M 197 56 L 197 126 L 105 126 L 105 168",
            stroke: estop ? "#94a3b8" : "var(--proto-uart)", "stroke-width": 1.6, "stroke-dasharray": probeDash, fill: "none",
            onclick: () => { activeInspTab.val = "WAVE"; }
          }),
          text({ x: 145, y: 120, fill: "var(--proto-uart)", "font-size": 9.5, "font-family": "monospace", "font-weight": "bold", "text-anchor": "middle" }, "P1 Sniff"),

          // 3. Probe 0 Glitch Tap (WhatTheFlip GPIO4 -> Row 13)
          path({
            class: "topo-wire", d: "M 155 168 L 155 144 L 362 144 L 362 56",
            stroke: estop ? "#94a3b8" : (glitchBus === "UART" ? "var(--state-glitch)" : "var(--state-warn)"),
            "stroke-width": glitchBus === "UART" ? 2.5 : 1.6, "stroke-dasharray": probeDash, fill: "none",
            onclick: () => { activeInspTab.val = "WAVE"; }
          }),
          text({ x: 265, y: 138, fill: glitchBus === "UART" ? "var(--state-glitch)" : "var(--state-warn)", "font-size": 9.5, "font-family": "monospace", "font-weight": "bold", "text-anchor": "middle" },
            "P0 Glitch"),

          // 4. I2C Bus between Daphne & GY-511 (Row 55 SCL / Row 58 SDA)
          path({
            class: "topo-wire", d: "M 480 92 L 480 118 L 620 118 L 620 138",
            stroke: (glitchBus === "I2C_ACK" || glitchBus === "I2C_STRETCH") ? "var(--state-glitch)" : (i2cLive ? "var(--proto-i2c)" : "#cbd5e1"),
            "stroke-width": (glitchBus === "I2C_ACK" || glitchBus === "I2C_STRETCH") ? 3.5 : 2.5, fill: "none",
            onclick: () => { activeInspTab.val = "I2C_TOK"; }
          }),
          text({ x: 552, y: 110, fill: (glitchBus === "I2C_ACK" || glitchBus === "I2C_STRETCH") ? "var(--state-glitch)" : "var(--proto-i2c)", "font-size": 9.5, "font-family": "monospace", "font-weight": "bold", "text-anchor": "middle" },
            (glitchBus === "I2C_ACK" || glitchBus === "I2C_STRETCH") ? "⚡I²C FAULT" : "I²C Row 55/58"),

          // 5. Probes 2 & 3 Tap (WhatTheFlip GPIO6/7 -> I2C Bus)
          path({
            class: "topo-wire", d: "M 195 196 L 480 196 L 480 118",
            stroke: estop ? "#94a3b8" : "var(--proto-i2c)", "stroke-width": 1.6, "stroke-dasharray": probeDash, fill: "none",
            onclick: () => { activeInspTab.val = "I2C_TOK"; }
          }),
          text({ x: 338, y: 190, fill: "var(--proto-i2c)", "font-size": 9.5, "font-family": "monospace", "font-weight": "bold", "text-anchor": "middle" },
            "P2/P3 (SDA/SCL)"),

          // 6. 2.4 GHz RF Link (Iris <-> WhatTheFlip)
          line({
            class: "topo-wire", x1: 195, y1: 238, x2: 475, y2: 238,
            stroke: glitchBus === "RF" ? "var(--state-glitch)" : (rfLive ? "var(--proto-rf)" : "#cbd5e1"),
            "stroke-width": 2, "stroke-dasharray": "5 4",
            onclick: () => { activeInspTab.val = "RF_RADAR"; }
          }),
          text({ x: 335, y: 229, fill: glitchBus === "RF" ? "var(--state-glitch)" : "var(--proto-rf)", "font-size": 9.5, "font-family": "monospace", "font-weight": "bold", "text-anchor": "middle" },
            `Zigbee Ch ${zigbeeChannel.val} / Wi-Fi Ch ${wifiActiveCh.val}`),

          // Hardware Nodes (Crisp, non-overlapping, concise)
          // Node 1: Hestia
          g({ class: "topo-node", onclick: () => { activeInspTab.val = "WAVE"; } },
            rect({ class: "node-box", x: 15, y: 22, width: 135, height: 66, rx: 4, fill: "#ffffff", stroke: "rgba(15,23,42,0.18)", "stroke-width": 1.2 }),
            text({ x: 25, y: 42, fill: "var(--fg-primary)", "font-size": 11.5, "font-weight": "bold", "font-family": "monospace" }, "HESTIA"),
            text({ x: 25, y: 58, fill: "var(--proto-uart)", "font-size": 10, "font-family": "monospace", "font-weight": "600" }, "GP0 Simplex TX"),
            text({ x: 25, y: 74, fill: "var(--fg-muted)", "font-size": 9.5, "font-family": "monospace" }, "115.2 kbps 8N1")
          ),

          // Node 2: Resistor R1 (220 Ohm)
          g({ class: "topo-node", onclick: () => { activeInspTab.val = "WAVE"; } },
            rect({ class: "node-box", x: 245, y: 40, width: 70, height: 32, rx: 3, fill: "#ffffff", stroke: "rgba(15,23,42,0.18)", "stroke-width": 1.2 }),
            text({ x: 280, y: 60, fill: "var(--fg-primary)", "font-size": 10.5, "font-weight": "bold", "font-family": "monospace", "text-anchor": "middle" }, "R1 220Ω")
          ),

          // Node 3: Daphne
          g({ class: "topo-node", onclick: () => { activeInspTab.val = "I2C_TOK"; } },
            rect({ class: "node-box", x: 410, y: 22, width: 145, height: 70, rx: 4, fill: "#ffffff", stroke: "rgba(15,23,42,0.18)", "stroke-width": 1.2 }),
            text({ x: 420, y: 42, fill: "var(--fg-primary)", "font-size": 11.5, "font-weight": "bold", "font-family": "monospace" }, "DAPHNE"),
            text({ x: 420, y: 58, fill: "var(--proto-uart)", "font-size": 10, "font-family": "monospace", "font-weight": "600" }, "GP1 RX • BER Check"),
            text({ x: 420, y: 74, fill: "var(--proto-i2c)", "font-size": 10, "font-family": "monospace", "font-weight": "600" }, `I²C Master • GP${8 + tiltSlot}`)
          ),

          // Node 4: GY-511 IMU (No faux fallback values; highlights when values change)
          g({ class: "topo-node", onclick: () => { activeInspTab.val = "I2C_TOK"; } },
            rect({
              class: "node-box", x: 565, y: 138, width: 105, height: 72, rx: 4,
              fill: i2cDelta ? "var(--proto-i2c-bg)" : "#ffffff",
              stroke: i2cDelta ? "var(--proto-i2c)" : "rgba(2,132,199,0.28)",
              "stroke-width": i2cDelta ? 2 : 1.2
            }),
            text({ x: 575, y: 157, fill: "var(--proto-i2c)", "font-size": 11, "font-weight": "bold", "font-family": "monospace" }, "GY-511 IMU"),
            accelX.val !== null
              ? g(
                  text({ x: 575, y: 174, fill: "var(--fg-primary)", "font-size": 9.5, "font-weight": "bold", "font-family": "monospace" }, `X:${accelX.val} Y:${accelY.val}`),
                  text({ x: 575, y: 190, fill: "var(--fg-primary)", "font-size": 9.5, "font-weight": "bold", "font-family": "monospace" }, `Z:${accelZ.val} mg`),
                  i2cDelta ? text({ x: 575, y: 204, fill: "var(--proto-i2c)", "font-size": 8.5, "font-weight": "bold", "font-family": "monospace" }, "Δ 0x19 UPDATED") : null
                )
              : text({ x: 575, y: 180, fill: "var(--fg-dim)", "font-size": 9.5, "font-family": "monospace" }, "NO SENSOR DATA")
          ),

          // Node 5: WhatTheFlip Controller
          g({ class: "topo-node", onclick: () => { activeInspTab.val = "RTOS"; } },
            rect({ class: "node-box", x: 15, y: 168, width: 180, height: 84, rx: 4, fill: "#ffffff", stroke: "rgba(15,23,42,0.18)", "stroke-width": 1.2 }),
            text({ x: 25, y: 188, fill: "var(--fg-primary)", "font-size": 11.5, "font-weight": "bold", "font-family": "monospace" }, "WHATTHEFLIP"),
            text({ x: 25, y: 205, fill: "var(--proto-uart)", "font-size": 9.5, "font-family": "monospace" }, "P0/P1: UART Glitch/Sniff"),
            text({ x: 25, y: 221, fill: "var(--proto-i2c)", "font-size": 9.5, "font-family": "monospace" }, "P2/P3: I²C ACK/Stretch"),
            text({ x: 25, y: 238, fill: estop ? "var(--state-glitch)" : "var(--proto-rf)", "font-size": 9.5, "font-family": "monospace", "font-weight": estop ? "bold" : "normal" },
              estop ? "GPIO9 E-STOP: HIGH-Z" : "2.4 GHz Zigbee / Wi-Fi")
          ),

          // Node 6: Iris (Wireless Victim)
          g({ class: "topo-node", onclick: () => { activeInspTab.val = "RF_RADAR"; } },
            rect({ class: "node-box", x: 475, y: 214, width: 195, height: 48, rx: 4, fill: "#ffffff", stroke: "rgba(124,58,237,0.28)", "stroke-width": 1.2 }),
            text({ x: 485, y: 233, fill: "var(--proto-rf)", "font-size": 11.5, "font-weight": "bold", "font-family": "monospace" }, "IRIS (WIRELESS)"),
            text({ x: 485, y: 250, fill: () => irisVerified.val ? "var(--proto-uart)" : "var(--fg-dim)", "font-size": 9.5, "font-family": "monospace", "font-weight": "bold" },
              () => irisVerified.val ? `Ch ${zigbeeChannel.val} (${irisRate.val} • ${irisRssi.val} dBm)` : `OFFLINE`)
          ),

          // Animated PDU Pills ONLY rendered when live hardware packets are actively flowing,
          // and dynamic hex/payload labels ONLY shown when values change
          uartLive ? g({
            class: "pdu-pill", transform: `translate(${pduUartCleanX}, 48)`,
            onclick: () => { isPaused.val = true; activeInspTab.val = "WAVE"; }
          },
            rect({ x: 0, y: 0, width: 32, height: 16, rx: 3, fill: "var(--proto-uart)" }),
            text({ x: 16, y: 11, fill: "#ffffff", "font-size": 8.5, "font-family": "monospace", "text-anchor": "middle", "font-weight": "bold" }, "TX")
          ) : null,

          uartLive ? g({
            class: "pdu-pill", transform: `translate(${pduUartOutX}, 48)`,
            onclick: () => { isPaused.val = true; activeInspTab.val = "WAVE"; }
          },
            rect({
              x: 0, y: 0, width: uartDelta ? 42 : 32, height: 16, rx: 3,
              fill: glitchBus === "UART" ? "var(--state-glitch)" : "var(--proto-uart)"
            }),
            text({
              x: uartDelta ? 21 : 16, y: 11, fill: "#ffffff",
              "font-size": 8.5, "font-family": "monospace", "text-anchor": "middle", "font-weight": "bold"
            }, glitchBus === "UART" ? "⚡ERR" : (uartDelta ? "ΔCRC" : "RX"))
          ) : null,

          // Dynamic hex byte pill (0x19) ONLY displayed when sensor values change or glitch is active
          (i2cLive && i2cDelta) ? g({
            class: "pdu-pill", transform: `translate(${pduI2cX}, 110)`,
            onclick: () => { activeInspTab.val = "I2C_TOK"; }
          },
            rect({ x: 0, y: 0, width: 38, height: 16, rx: 3, fill: (glitchBus === "I2C_ACK" || glitchBus === "I2C_STRETCH") ? "var(--state-glitch)" : "var(--proto-i2c)" }),
            text({ x: 19, y: 11, fill: "#ffffff", "font-size": 8.5, "font-family": "monospace", "text-anchor": "middle", "font-weight": "bold" },
              (glitchBus === "I2C_ACK" || glitchBus === "I2C_STRETCH") ? "NACK" : "0x19")
          ) : null,

          (rfLive && rfDelta) ? g({
            class: "pdu-pill", transform: `translate(${pduRfX}, 230)`,
            onclick: () => { activeInspTab.val = "RF_RADAR"; }
          },
            rect({ x: 0, y: 0, width: 38, height: 16, rx: 3, fill: glitchBus === "RF" ? "var(--state-glitch)" : "var(--proto-rf)" }),
            text({ x: 19, y: 11, fill: "#ffffff", "font-size": 8.5, "font-family": "monospace", "text-anchor": "middle", "font-weight": "bold" },
              glitchBus === "RF" ? "JAM" : `Ch${zigbeeChannel.val}`)
          ) : null
        );
      }
    )
  );
}

// ===========================================================================
// 5. UI: Upper Right - Contextual Inspector (Waveforms, Tokens, RF, Tasks)
// ===========================================================================
function charToBits8N1(chCode) {
  let bits = [0];
  for (let bVal = 0; bVal < 8; bVal++) bits.push((chCode >> bVal) & 1);
  bits.push(1);
  return bits;
}

function generateDiscreteSquarePath(bits, startX, bitW, yHigh, yLow) {
  let pts = [];
  let curY = bits[0] ? yHigh : yLow;
  pts.push(`${startX},${curY}`);
  for (let i = 0; i < bits.length; i++) {
    let targetY = bits[i] ? yHigh : yLow;
    let xL = startX + i * bitW;
    let xR = xL + bitW;
    if (targetY !== curY) {
      pts.push(`${xL},${curY}`);
      pts.push(`${xL},${targetY}`);
      curY = targetY;
    }
    pts.push(`${xR},${curY}`);
  }
  return pts.join(" ");
}

function WaveformScopeTab() {
  let p = selectedPacket.val;
  let inStr  = (p && p.inboundAscii) ? p.inboundAscii : "HESTIA:128:FF7F";
  let outStr = (p && p.outboundAscii) ? p.outboundAscii : (corruptedCount.val > 0 ? "HESTIA:128:F77F" : inStr);
  let sampleByteIn  = inStr.charCodeAt(Math.min(11, inStr.length - 1)) || 0x46; // e.g. 'F' = 0x46 = 01000110b
  let isCorrupt     = (p && p.tagClass === "fault") || activeGlitchBus.val === "UART";
  let sampleByteOut = isCorrupt ? (sampleByteIn & ~0x08) : sampleByteIn; // Bit D3 flipped from 1 to 0

  let bitsIn  = charToBits8N1(sampleByteIn);
  let bitsOut = charToBits8N1(sampleByteOut);

  // 12-bit sequence: [IDLE(1), START(0), D0..D7, STOP(1), IDLE(1)]
  let uartP1CleanBits = [1, ...bitsIn, 1];
  let uartDaphneBits  = [1, ...bitsOut, 1];

  // Probe 0 Glitch Injector: HIGH-Z (3.3V) except during D3 active glitch pulse (0.0V for 10µs)
  let p0GlitchBits = [1, 1];
  for (let b = 0; b < 8; b++) {
    p0GlitchBits.push(b === 3 && isCorrupt ? 0 : 1);
  }
  p0GlitchBits.push(1, 1);

  // Probe 2 (SDA) and Probe 3 (SCL) bits for LSM303DLHC (Addr 0x19 W -> 0x32, Reg 0xA8)
  let isI2cGlitch = activeGlitchBus.val === "I2C_ACK" || activeGlitchBus.val === "I2C_STRETCH";
  let p2SdaBits = [1, 0, 0, 0, 1, 1, 0, 0, 1, 0, isI2cGlitch ? 1 : 0, 1]; // START, 0x19W, ACK(0)/NACK(1), STOP
  let p3SclBits = [1, 1, 0, 1, 0, 1, 0, 1, 0, 1, isI2cGlitch ? 0 : 1, 1]; // 400 kHz SCL clock / stretch

  let startX = 100;
  let bitW   = 33;
  let totalBits = 12;

  function getShiftedPath(baseBits, yHigh, yLow) {
    let s = probeShift.val;
    let len = baseBits.length;
    let rotated = baseBits.map((_, i) => baseBits[(i + s) % len]);
    return generateDiscreteSquarePath(rotated, startX, bitW, yHigh, yLow);
  }

  let bitLabels = [
    { label: "IDLE", sub: "3.3V", isStart: false, isGlitch: false, isStop: false },
    { label: "START", sub: "0.0V", isStart: true, isGlitch: false, isStop: false }
  ];
  for (let b = 0; b < 8; b++) {
    let bitVal = (sampleByteIn >> b) & 1;
    let flipped = b === 3 && isCorrupt;
    bitLabels.push({
      label: flipped ? "⚡D3:0" : `D${b}:${bitVal}`,
      sub: flipped ? "0.0V" : (bitVal ? "3.3V" : "0.0V"),
      isStart: false,
      isGlitch: flipped,
      isStop: false
    });
  }
  bitLabels.push({ label: isCorrupt ? "CRC ERR" : "STOP", sub: "3.3V", isStart: false, isGlitch: isCorrupt, isStop: true });
  bitLabels.push({ label: "IDLE", sub: "3.3V", isStart: false, isGlitch: false, isStop: false });

  return div({ style: "display:flex;flex-direction:column;gap:8px" },
    div({ class: "insp-card" },
      div({ class: "insp-card-title" },
        span("DIGITAL LOGIC SCOPE // LIVE HARDWARE GPIO PROBING"),
        span({ class: "mono", style: isCorrupt ? "color:var(--state-glitch);font-weight:800" : "color:var(--proto-uart);font-weight:700" },
          isCorrupt ? "⚡ 10.0 µs GLITCH ACTIVE (ROW 13)" : "● ALL 4 PROBES LIVE (3.3V CMOS)")
      ),

      div({ class: "scope-header-bar" },
        div({ style: "display:flex;gap:5px" },
          button({
            class: () => "cmd-btn" + (scopeFilter.val === "ALL" ? " active" : ""),
            style: "padding:2px 8px;font-size:11px",
            onclick: () => scopeFilter.val = "ALL"
          }, "All Probes (P0–P3)"),
          button({
            class: () => "cmd-btn" + (scopeFilter.val === "UART" ? " active" : ""),
            style: "padding:2px 8px;font-size:11px",
            onclick: () => scopeFilter.val = "UART"
          }, "UART (P1 / P0)"),
          button({
            class: () => "cmd-btn" + (scopeFilter.val === "I2C" ? " active" : ""),
            style: "padding:2px 8px;font-size:11px",
            onclick: () => scopeFilter.val = "I2C"
          }, "I²C (P2 / P3)")
        ),
        div({ style: "display:flex;gap:6px" },
          span({ class: "scope-chip" },
            span({ class: () => "scope-led " + (liveProbeState.val.p1 ? "high" : "low") }),
            "P1 ", strong({ style: "display:inline-block;width:32px;text-align:center" }, () => liveProbeState.val.p1 ? "3.3V" : "0.0V")
          ),
          span({ class: "scope-chip" },
            span({ class: () => "scope-led " + (liveProbeState.val.p0 === 0 ? "pulse" : "high") }),
            "P0 ", strong({ style: "display:inline-block;width:32px;text-align:center" }, () => liveProbeState.val.p0 === 0 ? "PULS" : "IDLE")
          ),
          span({ class: "scope-chip" },
            span({ class: () => "scope-led " + (liveProbeState.val.p2 ? "high" : "low") }),
            "P2 ", strong({ style: "display:inline-block;width:32px;text-align:center" }, () => liveProbeState.val.p2 ? "3.3V" : "0.0V")
          ),
          span({ class: "scope-chip" },
            span({ class: () => "scope-led " + (liveProbeState.val.p3 ? "high" : "low") }),
            "P3 ", strong({ style: "display:inline-block;width:32px;text-align:center" }, () => liveProbeState.val.p3 ? "3.3V" : "0.0V")
          )
        )
      ),

      // 4-Channel Live Digital Logic Scope SVG
      svg({ viewBox: "0 0 510 216", style: "width:100%;height:216px;background:#f8fafc;border:1px solid var(--border-strong);border-radius:3px" },
        // Timing grid & bit boundary lines
        // Timing grid internal bit boundary dividers
        Array.from({ length: totalBits - 1 }).map((_, i) => {
          let gx = startX + (i + 1) * bitW;
          return line({ x1: gx, y1: 14, x2: gx, y2: 178, stroke: "#e2e8f0", "stroke-width": 1, "stroke-dasharray": "2 2" });
        }),
        // Measurement Cursor (Δt = 8.68 µs / bit-time)
        line({ x1: startX + bitW, y1: 10, x2: startX + 2 * bitW, y2: 10, stroke: "#64748b", "stroke-width": 1.2 }),
        line({ x1: startX + bitW, y1: 6, x2: startX + bitW, y2: 14, stroke: "#64748b", "stroke-width": 1.2 }),
        line({ x1: startX + 2 * bitW, y1: 6, x2: startX + 2 * bitW, y2: 14, stroke: "#64748b", "stroke-width": 1.2 }),
        text({ x: startX + 1.5 * bitW, y: 8, fill: "#64748b", "font-size": 8, "font-family": "monospace", "text-anchor": "middle" }, "Δt = 8.68 µs (115200 8N1)"),

        // Glitch pulse cursor across D3 bit column
        isCorrupt ? rect({ x: startX + 5 * bitW, y: 14, width: bitW, height: 164, fill: "rgba(225,29,72,0.08)", stroke: "var(--state-glitch)", "stroke-width": 1, "stroke-dasharray": "3 3" }) : null,
        isCorrupt ? text({ x: startX + 5.5 * bitW, y: 8, fill: "var(--state-glitch)", "font-size": 8, "font-family": "monospace", "text-anchor": "middle", "font-weight": "bold" }, "⚡ 10.0 µs GLITCH") : null,

        // Channel 1: Probe 1 (GPIO5 IN) - UART Sniffer (Clean Row 22)
        text({ x: 6, y: 31, fill: "var(--proto-uart)", "font-size": 9.5, "font-family": "monospace", "font-weight": "bold" }, "P1: GPIO5 [IN]"),
        text({ x: 6, y: 43, fill: "#94a3b8", "font-size": 8, "font-family": "monospace" }, "UART1 Clean RX"),
        text({ x: 80, y: 25, fill: "#94a3b8", "font-size": 7.5, "font-family": "monospace" }, "3.3V"),
        text({ x: 80, y: 47, fill: "#94a3b8", "font-size": 7.5, "font-family": "monospace" }, "0.0V"),
        line({ x1: startX - 4, y1: 22, x2: startX + totalBits * bitW, y2: 22, stroke: "#e2e8f0", "stroke-dasharray": "2 2" }),
        line({ x1: startX - 4, y1: 44, x2: startX + totalBits * bitW, y2: 44, stroke: "#cbd5e1", "stroke-width": 1 }),
        polyline({ points: () => getShiftedPath(uartP1CleanBits, 22, 44), fill: "none", stroke: "var(--proto-uart)", "stroke-width": 2 }),

        // Channel 2: Probe 0 (GPIO4 OUT) - UART Glitcher (Row 13)
        text({ x: 6, y: 73, fill: isCorrupt ? "var(--state-glitch)" : "var(--state-warn)", "font-size": 9.5, "font-family": "monospace", "font-weight": "bold" }, "P0: GPIO4 [OUT]"),
        text({ x: 6, y: 85, fill: "#94a3b8", "font-size": 8, "font-family": "monospace" }, isCorrupt ? "10µs Active LOW" : "High-Z Pull-Up"),
        text({ x: 80, y: 67, fill: "#94a3b8", "font-size": 7.5, "font-family": "monospace" }, "3.3V"),
        text({ x: 80, y: 89, fill: "#94a3b8", "font-size": 7.5, "font-family": "monospace" }, "0.0V"),
        line({ x1: startX - 4, y1: 64, x2: startX + totalBits * bitW, y2: 64, stroke: "#e2e8f0", "stroke-dasharray": "2 2" }),
        line({ x1: startX - 4, y1: 86, x2: startX + totalBits * bitW, y2: 86, stroke: "#cbd5e1", "stroke-width": 1 }),
        polyline({ points: () => getShiftedPath(p0GlitchBits, 64, 86), fill: "none", stroke: isCorrupt ? "var(--state-glitch)" : "var(--state-warn)", "stroke-width": isCorrupt ? 2.5 : 1.8 }),

        // Channel 3: Probe 2 (GPIO6 IN/OUT) - I2C SDA (Row 43)
        text({ x: 6, y: 115, fill: "var(--proto-i2c)", "font-size": 9.5, "font-family": "monospace", "font-weight": "bold" }, "P2: GPIO6 [SDA]"),
        text({ x: 6, y: 127, fill: "#94a3b8", "font-size": 8, "font-family": "monospace" }, "I2C0 Data / ACK"),
        text({ x: 80, y: 109, fill: "#94a3b8", "font-size": 7.5, "font-family": "monospace" }, "3.3V"),
        text({ x: 80, y: 131, fill: "#94a3b8", "font-size": 7.5, "font-family": "monospace" }, "0.0V"),
        line({ x1: startX - 4, y1: 106, x2: startX + totalBits * bitW, y2: 106, stroke: "#e2e8f0", "stroke-dasharray": "2 2" }),
        line({ x1: startX - 4, y1: 128, x2: startX + totalBits * bitW, y2: 128, stroke: "#cbd5e1", "stroke-width": 1 }),
        polyline({ points: () => getShiftedPath(p2SdaBits, 106, 128), fill: "none", stroke: "var(--proto-i2c)", "stroke-width": 2 }),

        // Channel 4: Probe 3 (GPIO7 IN/OUT) - I2C SCL (Row 42)
        text({ x: 6, y: 157, fill: "#0284c7", "font-size": 9.5, "font-family": "monospace", "font-weight": "bold" }, "P3: GPIO7 [SCL]"),
        text({ x: 6, y: 169, fill: "#94a3b8", "font-size": 8, "font-family": "monospace" }, "400 kHz Clock"),
        text({ x: 80, y: 151, fill: "#94a3b8", "font-size": 7.5, "font-family": "monospace" }, "3.3V"),
        text({ x: 80, y: 173, fill: "#94a3b8", "font-size": 7.5, "font-family": "monospace" }, "0.0V"),
        line({ x1: startX - 4, y1: 148, x2: startX + totalBits * bitW, y2: 148, stroke: "#e2e8f0", "stroke-dasharray": "2 2" }),
        line({ x1: startX - 4, y1: 170, x2: startX + totalBits * bitW, y2: 170, stroke: "#cbd5e1", "stroke-width": 1 }),
        polyline({ points: () => getShiftedPath(p3SclBits, 148, 170), fill: "none", stroke: "#0284c7", "stroke-width": 2 }),

        // Decoded Protocol Annotation Blocks
        bitLabels.map((item, idx) => {
          let bx = startX + idx * bitW + 1;
          let bw = bitW - 2;
          let isG = item.isGlitch;
          let bg = isG ? "var(--state-glitch-bg)"
                 : item.isStart ? "var(--state-warn-bg)"
                 : item.isStop ? (isCorrupt ? "var(--state-glitch-bg)" : "var(--proto-uart-bg)")
                 : "var(--proto-i2c-bg)";
          let bBorder = isG ? "var(--state-glitch)"
                      : item.isStart ? "var(--state-warn-border)"
                      : item.isStop ? (isCorrupt ? "var(--state-glitch)" : "var(--proto-uart-border)")
                      : "var(--proto-i2c-border)";
          let fColor = isG ? "var(--state-glitch)"
                     : item.isStart ? "var(--state-warn)"
                     : item.isStop ? (isCorrupt ? "var(--state-glitch)" : "var(--proto-uart)")
                     : "var(--proto-i2c)";
          return g(
            rect({ x: bx, y: 182, width: bw, height: 26, rx: 2, fill: bg, stroke: bBorder, "stroke-width": isG ? 1.5 : 1 }),
            text({ x: bx + bw / 2, y: 194, fill: fColor, "font-size": 8, "font-family": "monospace", "text-anchor": "middle", "font-weight": (isG || item.isStart || item.isStop) ? "bold" : "normal" }, item.label),
            text({ x: bx + bw / 2, y: 204, fill: "var(--fg-muted)", "font-size": 7, "font-family": "monospace", "text-anchor": "middle" }, item.sub)
          );
        })
      )
    ),

    // Card 2: PDU Bit-Diff & Inversion Analysis
    div({ class: "insp-card" },
      div({ class: "insp-card-title" },
        span("PDU BIT-DIFF & HARDWARE FAULT INJECTION MECHANISM"),
        span({ style: isCorrupt ? "color:var(--state-glitch);font-weight:700" : "color:var(--proto-uart);font-weight:700" },
          isCorrupt ? "⚡ BIT D3 FLIPPED (3.3V -> 0.0V via 15 mA sink)" : "✓ 0 BIT INVERSIONS")
      ),
      div({ class: "diff-grid" },
        div({ class: "diff-box" },
          div({ style: "color:var(--proto-uart);font-weight:700" }, "P1 (GPIO5 Clean Tap / Row 22)"),
          div({ style: "margin-top:2px" }, `${inStr} (Char: '${String.fromCharCode(sampleByteIn)}' = 0x${sampleByteIn.toString(16).toUpperCase()})`),
          div({ class: "bit-row" },
            bitsIn.map((bVal, idx) => span({ class: "bit-cell" },
              idx === 0 ? "S:0" : (idx === 9 ? "P:1" : `D${idx - 1}:${bVal}`)
            ))
          )
        ),
        div({ class: "diff-box" },
          div({ style: isCorrupt ? "color:var(--state-glitch);font-weight:700" : "color:var(--proto-uart);font-weight:700" },
            "P0 (GPIO4 Glitched Output / Row 13)"),
          div({ style: "margin-top:2px" }, `${outStr} (Char: '${String.fromCharCode(sampleByteOut)}' = 0x${sampleByteOut.toString(16).toUpperCase()})`),
          div({ class: "bit-row" },
            bitsOut.map((bVal, idx) => {
              let diff = bVal !== bitsIn[idx];
              return span({ class: "bit-cell" + (diff ? " flipped" : "") },
                idx === 0 ? "S:0" : (idx === 9 ? "P:1" : `D${idx - 1}:${bVal}`)
              );
            })
          )
        )
      ),
    )
  );
}

function I2cTokensTab() {
  let isGlitch = activeGlitchBus.val === "I2C_ACK" || activeGlitchBus.val === "I2C_STRETCH";

  return div({ style: "display:flex;flex-direction:column;gap:8px" },
    div({ class: "insp-card" },
      div({ class: "insp-card-title" },
        span("I²C BUS TRANSACTION TOKEN STREAM"),
        span({ style: isGlitch ? "color:var(--state-glitch);font-weight:800" : "color:var(--proto-i2c);font-weight:700" },
          isGlitch ? "⚡ 9th-CLK NACK DETECTED" : "✓ VALID ACK STREAM")
      ),
      div({ class: "token-stream" },
        span({ class: "tok start" }, "s (START)"),
        span({ class: "tok addr" }, "32 (0x19 W)"),
        isGlitch
          ? span({ class: "tok nack" }, "n (NACK ⚡ 50µs ACK-KILL)")
          : span({ class: "tok ack" }, "a (ACK)"),
        isGlitch
          ? span({ class: "tok rec" }, "[DAPHNE 9-CLK SCL RECOVERY]")
          : span({ class: "tok data" }, "A8 (OUT_X_L_A | AUTO_INC)"),
        isGlitch ? null : span({ class: "tok ack" }, "a (ACK)"),
        isGlitch ? null : span({ class: "tok start" }, "s (REP START)"),
        isGlitch ? null : span({ class: "tok addr" }, "33 (0x19 R)"),
        isGlitch ? null : span({ class: "tok data" }, `[${rawRegHex.val}]`),
        span({ class: "tok stop" }, "p (STOP)")
      ),
      div({ style: "margin-top:6px;font-size:11.5px;color:var(--fg-muted)" },
        "Token Key: ", strong("s"), "=START, ", strong("32"), "=Addr 0x19 W, ", strong("a"), "=ACK (0), ", strong("n"), "=NACK (1), ", strong("p"), "=STOP"
      )
    ),
    div({ class: "insp-card" },
      div({ class: "insp-card-title" },
        span("I²C LOGIC WAVEFORM & 9-CLOCK SCL RECOVERY"),
        span({ class: "mono", style: "color:var(--proto-i2c);font-size:12px;font-weight:700" }, "400 kHz Fast-Mode")
      ),
      svg({ viewBox: "0 0 480 114", style: "width:100%;height:114px;background:#f8fafc;border:1px solid var(--border-strong);border-radius:3px" },
        text({ x: 6, y: 24, fill: "var(--proto-i2c)", "font-size": 11, "font-family": "monospace", "font-weight": "bold" }, "SCL (GP7)"),
        text({ x: 6, y: 56, fill: isGlitch ? "var(--state-glitch)" : "var(--proto-i2c)", "font-size": 11, "font-family": "monospace", "font-weight": "bold" }, "SDA (GP6)"),
        text({ x: 6, y: 92, fill: "var(--proto-rf)", "font-size": 11, "font-family": "monospace", "font-weight": "bold" }, "I²C:Dec"),

        polyline({ points: "75,24 82,24 82,14 90,14 90,24 98,24 98,14 106,14 106,24 114,24 114,14 122,14 122,24 130,24 130,14 138,14 138,24 146,24 146,14 154,14 154,24 162,24 162,14 170,14 170,24 178,24 178,14 186,14 186,24 194,24 194,14 202,14 202,24 234,24 234,14 244,14 244,24 254,24 254,14 264,14 264,24 274,24 274,14 284,14 284,24 294,24 294,14 304,14 304,24 314,24 314,14 324,14 324,24 334,24 334,14 344,14 344,24 354,24 354,14 364,14 364,24 374,24 374,14 384,14 384,24 394,24 394,14 404,14 404,24 414,24 414,14 424,14 424,24 460,24", fill: "none", stroke: "var(--proto-i2c)", "stroke-width": 2 }),

        polyline({ points: isGlitch ? "75,56 80,56 80,45 150,45 150,56 195,56 195,45 245,45 245,56 460,56" : "75,56 80,56 80,45 150,45 150,56 195,56 195,56 208,56 215,45 460,45", fill: "none", stroke: isGlitch ? "var(--state-glitch)" : "var(--proto-i2c)", "stroke-width": isGlitch ? 3.5 : 2 }),

        rect({ x: 75, y: 80, width: 34, height: 20, rx: 3, fill: "var(--state-warn-bg)", stroke: "var(--state-warn-border)", "stroke-width": 1 }),
        text({ x: 92, y: 94, fill: "var(--state-warn)", "font-size": 10.5, "font-family": "monospace", "text-anchor": "middle", "font-weight": "bold" }, "START"),

        rect({ x: 114, y: 80, width: 102, height: 20, rx: 3, fill: "var(--proto-i2c-bg)", stroke: "var(--proto-i2c-border)", "stroke-width": 1 }),
        text({ x: 165, y: 94, fill: "var(--proto-i2c)", "font-size": 10.5, "font-family": "monospace", "text-anchor": "middle" }, "ADDR: 0x19 W"),

        rect({ x: 220, y: 80, width: isGlitch ? 92 : 36, height: 20, rx: 3, fill: isGlitch ? "var(--state-glitch-bg)" : "var(--proto-uart-bg)", stroke: isGlitch ? "var(--state-glitch)" : "var(--proto-uart-border)", "stroke-width": 1.5 }),
        text({ x: isGlitch ? 266 : 238, y: 94, fill: isGlitch ? "var(--state-glitch)" : "var(--proto-uart)", "font-size": 10.5, "font-family": "monospace", "text-anchor": "middle", "font-weight": "bold" },
          isGlitch ? "⚡ NACK (50µs)" : "ACK"),

        isGlitch ? rect({ x: 316, y: 80, width: 118, height: 20, rx: 3, fill: "var(--proto-i2c-bg)", stroke: "var(--proto-i2c)", "stroke-dasharray": "2 2" }) : null,
        isGlitch ? text({ x: 375, y: 94, fill: "var(--proto-i2c)", "font-size": 10, "font-family": "monospace", "text-anchor": "middle", "font-weight": "bold" }, "9-CLK RECOVERY") : null,

        rect({ x: 438, y: 80, width: 34, height: 20, rx: 3, fill: "var(--state-warn-bg)", stroke: "var(--state-warn-border)", "stroke-width": 1 }),
        text({ x: 455, y: 94, fill: "var(--state-warn)", "font-size": 10.5, "font-family": "monospace", "text-anchor": "middle", "font-weight": "bold" }, "STOP")
      )
    )
  );
}
function RfRadarTab() {
  const wifiChannels = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13];
  const zigbeeChannels = [11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26];
  return div({ style: "display:flex;flex-direction:column;gap:8px" },
    div({ class: "insp-card" },
      div({ class: "insp-card-title" },
        span("2.4 GHz WI-FI SPECTRUM OCCUPANCY (802.11ax/b/g/n)"),
        span({ style: "color:var(--proto-rf);font-size:12px" },
          wifiChannelMode.val === 0 ? `SWEEPING (Active Ch ${wifiActiveCh.val})` : `LOCKED CH ${wifiActiveCh.val}`)
      ),
      div({ style: "display:flex;gap:8px;align-items:center;margin-bottom:5px" },
        button({
          class: () => "cmd-btn" + (wifiChannelMode.val === 0 ? " active" : ""),
          onclick: () => selectWifiChannel(0)
        }, "🔄 Sweep All Channels (1–13)"),
        span({ class: "mono", style: "font-size:11.5px;color:var(--fg-muted)" }, "50ms off-channel probe / 250ms HTTP Ch6 hold")
      ),
      div({ class: "ch-grid" },
        wifiChannels.map(ch => button({
          class: () => "btn-ch" + (wifiChannelMode.val === ch ? " active" : ""),
          onclick: () => selectWifiChannel(ch)
        }, `Ch ${ch}`))
      )
    ),
    div({ class: "insp-card" },
      div({ class: "insp-card-title" },
        span("IEEE 802.15.4 / ZIGBEE CHANNEL SELECTOR (CH 11–26)"),
        span({ style: "color:#d97706;font-size:12px" },
          () => `LOCKED CH ${zigbeeChannel.val} (${2405 + 5 * (zigbeeChannel.val - 11)} MHz)`)
      ),
      div({ style: "display:flex;gap:8px;align-items:center;margin-bottom:5px" },
        button({
          class: () => "cmd-btn" + (zigbeeChannel.val === 15 ? " active" : ""),
          onclick: () => selectZigbeeChannel(15)
        }, "⭐ Iris Default (Ch 15 / 2425 MHz)"),
        button({
          class: () => "cmd-btn" + (zigbeeChannel.val === 11 ? " active" : ""),
          onclick: () => selectZigbeeChannel(11)
        }, "Ch 11 (Iris Alt / 2405 MHz)"),
        button({
          class: () => "cmd-btn" + (zigbeeChannel.val === 26 ? " active" : ""),
          onclick: () => selectZigbeeChannel(26)
        }, "Ch 26 (Out-of-band / 2480 MHz)")
      ),
      div({ class: "ch-grid-zigbee" },
        zigbeeChannels.map(ch => button({
          class: () => "btn-ch btn-ch-zigbee" + (zigbeeChannel.val === ch ? " active" : ""),
          onclick: () => selectZigbeeChannel(ch)
        }, `Ch ${ch}`))
      )
    ),
    div({ class: "insp-card" },
      div({ class: "insp-card-title" },
        span("TARGETED IRIS ALLOW-LIST FILTER (FR-6, TC-3.3)"),
        span({ style: "color:var(--proto-uart)" }, "0 BROADCAST DROPS")
      ),
      div({ style: "font-size:12px;color:var(--fg-muted);display:flex;flex-direction:column;gap:4px" },
        div("• Target BSSID: ", strong({ style: "color:#d8b4fe" }, "02:00:00:00:00:03"), " (Short: 0x0003, PAN: 0x1234)"),
        div("• 802.15.4 Frequency: ", strong({ style: "color:var(--proto-i2c)" }, "Channel 15 (2425 MHz)"), " • Wi-Fi: ", strong("Ch 6 (2437 MHz)")),
        div("• Allow-List Check: ", strong({ style: "color:var(--proto-uart)" }, "Strict MAC Match Verified"), " — broadcast stress strictly prohibited.")
      )
    ),
    div({ class: "insp-card" },
      div({ class: "insp-card-title" },
        span("IRIS LOCATION & OVER-THE-AIR (OTA) CONTROL PLANE"),
        () => span({ style: irisVerified.val ? "color:var(--proto-uart);font-weight:700" : "color:var(--fg-dim)" },
          irisVerified.val ? `✓ VERIFIED BENCH ZONE (${irisRssi.val} dBm)` : `⏳ UNVERIFIED DISTANT (${irisRssi.val} dBm)`)
      ),
      div({ style: "display:flex;flex-direction:column;gap:6px;margin-top:4px" },
        div({ style: "font-size:12px;color:var(--fg-muted)" },
          () => `Current State: ${irisRate.val} Workload • SoftAP Channel ${wifiActiveCh.val} • Ranging: ${irisVerified.val ? "≤ 30 cm Verified" : "> 1 m Distant"}`
        ),
        div({ style: "display:flex;gap:6px;flex-wrap:wrap;margin-top:2px" },
          button({
            class: () => "cmd-btn" + (irisVerified.val && irisRate.val === "20 Hz" ? " active" : ""),
            disabled: () => !irisVerified.val,
            onclick: () => triggerIrisCmd("20hz", "OTA: Commanded Iris to 20 Hz High-Density Stress Burst (50 ms)")
          }, "⚡ 20 Hz Stress Burst"),
          button({
            class: () => "cmd-btn" + (irisVerified.val && irisRate.val === "2 Hz" ? " active" : ""),
            disabled: () => !irisVerified.val,
            onclick: () => triggerIrisCmd("2hz", "OTA: Restored Iris to 2 Hz Nominal Beacon Rate (500 ms)")
          }, "✓ 2 Hz Nominal"),
          button({
            class: "cmd-btn rf-btn",
            disabled: () => !irisVerified.val,
            onclick: () => triggerIrisCmd("ch11", "OTA: Commanded Iris to hop SoftAP to Wi-Fi Channel 11")
          }, "🔄 Hop to Ch 11"),
          button({
            class: "cmd-btn",
            disabled: () => !irisVerified.val,
            onclick: () => triggerIrisCmd("ch6", "OTA: Restored Iris SoftAP to Wi-Fi Channel 6")
          }, "🔄 Restore to Ch 6")
        )
      )
    )
  );
}

function RtosTasksTab() {
  return div({ style: "display:flex;flex-direction:column;gap:8px" },
    div({ class: "insp-card" },
      div({ class: "insp-card-title" },
        span("µT-KERNEL 3.0 CONCURRENCY & TASK TABLE"),
        span({ style: "color:var(--proto-i2c)" }, "ZERO MALLOC | BARR-C")
      ),
      table({ class: "ws-table" },
        thead(tr(th("TASK / CORE"), th("PRIO"), th("PERIPHERAL / REGISTER"), th("STATE"))),
        tbody(
          tr(td("TSK_SAFETY"), td("4"), td("GPIO9 ISR -> GPIO.enable_w1tc (<200ns)"), td({ style: "color:var(--proto-uart)" }, safetyState.val)),
          tr(td("TSK_DASHBOARD"), td("6"), td("SoftAP Ch6 + SD /WWW/ + 4KB /api/pcap"), td("2.0 Hz")),
          tr(td("TSK_DISCOVERY"), td("7"), td("UART1.lowpulse + I2C0.sr (<10ms remap)"), td("LOCKED")),
          tr(td("TSK_SNIFFER"), td("8"), td("U1RXD_IN (GPIO5) + I2CEXT0 (GPIO6/7)"), td("ACTIVE")),
          tr(td("TSK_FUZZER"), td("9"), td("U1TXD_OUT (10µs) + I2C0.ctr (50µs)"), td("ARMED")),
          tr(td("TSK_STORAGE"), td("10"), td("GPIO18..21 Bitbang SPI -> FAT32 PCAP"), td("READY"))
        )
      )
    ),
    div({ class: "insp-card" },
      div({ class: "insp-card-title" },
        span("HIGH-SPEED BUS ACTIVITY LOG (UNTHROTTLED)"),
        span({ class: "mono", style: "color:var(--proto-uart)" }, () => `${highSpeedLog.val.length} events`)
      ),
      div({ class: "high-speed-log-box" },
        () => highSpeedLog.val.length === 0
          ? div({ style: "color:var(--fg-dim);padding:4px" }, "Waiting for live hardware bus transitions...")
          : highSpeedLog.val.slice(0, 40).map(item =>
              div({ class: "log-line" + (item.isFault ? " log-fault" : "") },
                span({ class: "log-ts" }, item.ts),
                span({ class: `badge ${item.isFault ? "fault" : item.proto.toLowerCase()}` }, item.proto),
                span({ class: "log-msg" }, item.msg)
              )
            )
      )
    )
  );
}

function ContextualInspectorPane() {
  return div({ class: "inspector-pane" },
    div({ class: "inspector-tabs" },
      button({
        class: () => "insp-tab" + (activeInspTab.val === "WAVE" ? " active" : ""),
        onclick: () => activeInspTab.val = "WAVE"
      }, "1. LOGIC SCOPE & DIFF"),
      button({
        class: () => "insp-tab" + (activeInspTab.val === "I2C_TOK" ? " active" : ""),
        onclick: () => activeInspTab.val = "I2C_TOK"
      }, "2. I²C TOKENS & RECOVERY"),
      button({
        class: () => "insp-tab" + (activeInspTab.val === "RF_RADAR" ? " active" : ""),
        onclick: () => activeInspTab.val = "RF_RADAR"
      }, "3. 2.4GHz RF RADAR"),
      button({
        class: () => "insp-tab" + (activeInspTab.val === "RTOS" ? " active" : ""),
        onclick: () => activeInspTab.val = "RTOS"
      }, "4. TASKS & BUS LOG")
    ),
    () => {
      let tab = activeInspTab.val;
      if (tab === "WAVE") return div({ class: "inspector-body" }, WaveformScopeTab());
      if (tab === "I2C_TOK") return div({ class: "inspector-body" }, I2cTokensTab());
      if (tab === "RF_RADAR") return div({ class: "inspector-body" }, RfRadarTab());
      return div({ class: "inspector-body" }, RtosTasksTab());
    }
  );
}

// ===========================================================================
// 6. UI: Bottom Wireshark Suite & Dissector Pane
// ===========================================================================
function toggleSort(col) {
  if (sortColumn.val === col) {
    sortAscending.val = !sortAscending.val;
  } else {
    sortColumn.val = col;
    sortAscending.val = (col === "proto" || col === "chan" || col === "summary");
  }
}

function sortIndicator(col) {
  if (sortColumn.val !== col) return " ↕";
  return sortAscending.val ? " ▲" : " ▼";
}

function BottomWiresharkPane() {
  const quickFilters = [
    { id: "ALL", label: "All Traffic" },
    { id: "UART", label: "UART (115200)" },
    { id: "I2C", label: "I2C (400 kHz)" },
    { id: "WIFI", label: "Wi-Fi 6 (Ch 1–13)" },
    { id: "ZIGBEE", label: "802.15.4 (Ch 15)" },
    { id: "FAULTS", label: "⚡ Faults Only" }
  ];

  return div({ class: "bottom-wireshark" },
    div({ class: "ws-toolbar" },
      span({ class: "mono", style: "font-size:11.5px;font-weight:700;color:var(--proto-i2c)" }, "WIRESHARK:"),
      quickFilters.map(qf => button({
        class: () => "cmd-btn" + (filterProtocol.val === qf.id ? " active" : ""),
        onclick: () => filterProtocol.val = qf.id
      }, qf.label)),
      button({
        class: () => "hurry-toggle" + (hurryModeEnabled.val ? "" : " off"),
        onclick: () => {
          hurryModeEnabled.val = !hurryModeEnabled.val;
          showToast("Hurry Mode (Collapse Repetitive Polling): " + (hurryModeEnabled.val ? "ON" : "OFF"));
        }
      }, () => hurryModeEnabled.val ? "⚡ Hurry Mode: ON" : "Hurry Mode: OFF"),
      input({
        class: "ws-input",
        type: "text",
        placeholder: "Client expression (e.g. lsm303, 0xa8, iris, fail, hestia)...",
        value: () => filterSearchText.val,
        oninput: (e) => filterSearchText.val = e.target.value
      }),
      button({
        class: "cmd-btn",
        onclick: () => { filterProtocol.val = "ALL"; filterSearchText.val = ""; }
      }, "Clear"),
      () => span({ class: "mono", style: "font-size:11px;color:var(--fg-muted)" },
        `${pcapAuditTotal.val} total (${packets.val.length} buffered)`)
    ),
    div({ class: "ws-split" },
      // Left 62%: Sortable Packet Table
      div({ class: "ws-table-wrap" },
        table({ class: "ws-table" },
          thead(
            tr(
              th({ onclick: () => toggleSort("id") }, () => "NO." + sortIndicator("id")),
              th({ onclick: () => toggleSort("tsUs") }, () => "TIME" + sortIndicator("tsUs")),
              th({ onclick: () => toggleSort("proto") }, () => "PROTO" + sortIndicator("proto")),
              th({ onclick: () => toggleSort("chan") }, () => "BUS / CHANNEL" + sortIndicator("chan")),
              th({ onclick: () => toggleSort("len") }, () => "LEN" + sortIndicator("len")),
              th({ onclick: () => toggleSort("summary") }, () => "PROTOCOL SUMMARY & CHECKSUM" + sortIndicator("summary"))
            )
          ),
          () => {
            let protoFilter = filterProtocol.val;
            let search      = filterSearchText.val.toLowerCase().trim();
            let col         = sortColumn.val;
            let asc         = sortAscending.val;
            let hurry       = hurryModeEnabled.val;

            let list = packets.val.filter(p => {
              if (protoFilter !== "ALL") {
                if (protoFilter === "UART" && p.proto !== "UART") return false;
                if (protoFilter === "I2C" && p.proto !== "I2C") return false;
                if (protoFilter === "WIFI" && p.proto !== "WIFI") return false;
                if (protoFilter === "ZIGBEE" && p.proto !== "ZIGBEE") return false;
                if (protoFilter === "FAULTS" && p.proto !== "FAULT" && p.tagClass !== "fault") return false;
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
            let consecutiveNominal = 0;

            for (let i = 0; i < sorted.length && renderedRows.length < 120; i++) {
              let p = sorted[i];
              if (hurry && !p.isFault && p.tagClass !== "fault") {
                consecutiveNominal++;
                if (consecutiveNominal > 2) continue;
              } else {
                if (consecutiveNominal > 2) {
                  renderedRows.push(tr({ class: "row-hurry" },
                    td("-"), td("-"), td(span({ class: "badge hurry" }, "HURRY")), td("-"), td("-"),
                    td(`[... ${consecutiveNominal - 2} nominal polling frames collapsed by Hurry Mode (BER: 0.000%) ...]`)
                  ));
                }
                consecutiveNominal = 0;
              }

              renderedRows.push(tr({
                class: () => (selectedPacket.val === p ? "selected " : "") + (p.tagClass === "fault" ? "row-fault" : ""),
                onclick: () => {
                  selectedPacket.val = p;
                  if (p.proto === "UART" || p.proto === "FAULT") activeInspTab.val = "WAVE";
                  else if (p.proto === "I2C") activeInspTab.val = "I2C_TOK";
                  else if (p.proto === "WIFI" || p.proto === "ZIGBEE") activeInspTab.val = "RF_RADAR";
                }
              },
                td(p.id || "-"),
                td(p.time),
                td(span({ class: `badge ${p.tagClass}` }, p.proto)),
                td(p.chan),
                td(p.len != null ? `${p.len}B` : "-"),
                td({ class: "col-summary" }, p.summary)
              ));
            }

            return tbody(renderedRows);
          }
        )
      ),
      // Right 38%: Hex Dump & Dissector + PCAP Audit
      div({ class: "ws-hex-pane" },
        () => {
          let p = selectedPacket.val;
          let tot = pcapAuditTotal.val;
          let nom = pcapAuditNominal.val;
          let flt = pcapAuditFaults.val;
          let rec = pcapAuditRecovered.val;
          return div(
            div({ class: "pcap-audit-box" },
              div({ style: "font-weight:800;color:var(--proto-i2c);margin-bottom:2px" },
                "SESSION AUDIT"),
              div({ style: "display:grid;grid-template-columns:1fr 1fr;gap:4px;font-size:11px" },
                div("Total Frames: ", strong(`${tot}`)),
                div("Nominal Polls: ", strong({ style: "color:var(--proto-uart)" }, `${nom}`)),
                div("Faults Injected: ", strong({ style: "color:var(--state-glitch)" }, `${flt}`)),
                div("I2C Recovered: ", strong({ style: "color:var(--proto-i2c)" }, `${rec}`))
              )
            ),
            div({ style: "display:flex;justify-content:space-between;color:var(--fg-muted);border-bottom:1px solid var(--border-subtle);padding-bottom:2px;margin-top:4px" },
              span("FRAME ", strong({ style: "color:var(--fg-primary)" }, p ? `#${p.id}` : "-"), p ? ` (${p.proto})` : ""),
              span("LEN: ", strong({ style: "color:var(--proto-i2c)" }, p ? `${p.len} B` : "-"), p ? ` @ ${p.time}` : "")
            ),
            pre({ class: "hex-dump-pre" }, formatHex(p ? (p.raw || p.summary) : null))
          );
        }
      )
    )
  );
}

// ===========================================================================
// 7. Toast & Data Streaming Hooks
// ===========================================================================
function ToastOverlay() {
  return div({ class: "toast-container" },
    () => toastMsg.val
      ? div({ class: "toast" },
          span(toastMsg.val),
          span({ style: "cursor:pointer;opacity:0.7", onclick: () => toastMsg.val = "" }, "✕")
        )
      : span()
  );
}

function updateFromTelemetry(d) {
  let nowMs = Date.now();
  if (d.safety) safetyState.val = d.safety;
  if (d.uart_pkts != null) {
    if (prevUartPkts >= 0 && d.uart_pkts > prevUartPkts) {
      let delta = d.uart_pkts - prevUartPkts;
      uartActiveUntil.val = nowMs + 1200;
      uartValueChangedUntil.val = nowMs + 1000;
      pushHighSpeedLog("UART", `+${delta} frames on Row 22 -> Row 13 (total=${d.uart_pkts})`, false);
    }
    prevUartPkts = d.uart_pkts;
    uartPkts.val = d.uart_pkts;
  }
  if (d.i2c_pkts != null) {
    if (prevI2cPkts >= 0 && d.i2c_pkts > prevI2cPkts) {
      let delta = d.i2c_pkts - prevI2cPkts;
      i2cActiveUntil.val = nowMs + 1200;
      pushHighSpeedLog("I2C", `+${delta} 400kHz transactions on Row 55/58 (total=${d.i2c_pkts})`, false);
    }
    prevI2cPkts = d.i2c_pkts;
    i2cPkts.val = d.i2c_pkts;
  }
  if (d.wifi_pkts != null) wifiPkts.val = d.wifi_pkts;
  if (d.ieee154_pkts != null) {
    let rfSum = (d.wifi_pkts || 0) + d.ieee154_pkts;
    if (prevRfPkts >= 0 && rfSum > prevRfPkts) {
      rfActiveUntil.val = nowMs + 1200;
      rfValueChangedUntil.val = nowMs + 1000;
    }
    prevRfPkts = rfSum;
    zigbeePkts.val = d.ieee154_pkts;
  }
  if (d.corrupted_frames != null) corruptedCount.val = d.corrupted_frames;
  if (d.wifi_ch != null) wifiActiveCh.val = d.wifi_ch;
  if (d.wifi_mode != null) wifiChannelMode.val = d.wifi_mode;
  if (d.pcap_bytes != null) pcapSize.val = d.pcap_bytes;
  if (d.uptime_s != null) uptimeS.val = d.uptime_s;
  if (d.ber != null) berPct.val = d.ber;
  if (d.iris_rssi != null) irisRssi.val = d.iris_rssi;
  if (d.iris_verified != null) irisVerified.val = d.iris_verified;
  if (d.tool_mode != null) toolMode.val = d.tool_mode;
  if (d.disc_locked != null) discoveryLocked.val = d.disc_locked;
  if (d.zigbee_ch != null) zigbeeChannel.val = d.zigbee_ch;

  if (d.accel && d.accel.x != null && d.accel.y != null && d.accel.z != null) {
    updateAccelHistory(d.accel.x, d.accel.y, d.accel.z);
  }
  if (d.mag && d.mag.x != null) {
    magStr.val = `X:${d.mag.x} Y:${d.mag.y} Z:${d.mag.z}`;
  }
}

function connectDataStream() {
  fetchNextPcapChunk(false);

  // Animate packet envelopes smoothly along breadboard wires when running
  setInterval(() => {
    if (!isPaused.val && safetyState.val !== "ABORTED") {
      animPhase.val = (animPhase.val + 2) % 100;
    }
  }, 60);

  // Human-readable 2 Hz probe logic toggle (500 ms)
  setInterval(() => {
    if (!isPaused.val && safetyState.val !== "ABORTED") {
      probeShift.val = (probeShift.val + 1) % 12;
      let isGlitched = activeGlitchBus.val === "UART";
      liveProbeState.val = {
        p1: liveProbeState.val.p1 ? 0 : 1,
        p0: (isGlitched && liveProbeState.val.p1) ? 0 : 1,
        p2: liveProbeState.val.p2 ? 0 : 1,
        p3: liveProbeState.val.p3 ? 0 : 1
      };
    }
  }, 500);

  setInterval(() => {
    fetchNextPcapChunk(false);
  }, 400);

  let wsProtocol = window.location.protocol === "https:" ? "wss:" : "ws:";
  let wsHost = window.location.port === "8888" ? "localhost:8889" : window.location.host;
  let ws = new WebSocket(`${wsProtocol}//${wsHost}/ws`);
  ws.onmessage = (evt) => {
    let d = JSON.parse(evt.data);
    if (d.type === "telemetry") {
      updateFromTelemetry(d);
    } else if (d.type === "alert") {
      showToast(d.msg || "ALERT");
    }
  };
}

// ===========================================================================
// 8. Main App Mount
// ===========================================================================
function App() {
  return div({ class: "workbench-root" },
    ToastOverlay(),
    div({ class: "workbench-header" },
      TopHeaderBar(),
      ToolBeltBar()
    ),
    div({ class: "upper-workbench" },
      BreadboardCanvas(),
      ContextualInspectorPane()
    ),
    BottomWiresharkPane()
  );
}

van.add(document.getElementById("app"), App());
connectDataStream();
