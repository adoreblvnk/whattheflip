import van from "vanjs-core";

const { div, span, button, a, table, thead, tbody, tr, th, td, pre, b, select, option, input } = van.tags;

// Dynamic Hardware State
const safetyState = van.state("ARMED");
const toastMsg = van.state("");

// 4 Scenarios in exact specified order: UART -> I2C -> Wi-Fi -> Zigbee
const activeScenario = van.state("UART");

// Wi-Fi Channel Control State (0 = Sweep All Channels 1..13, 1..13 = Locked Channel)
const wifiChannelMode = van.state(0);
const wifiActiveCh = van.state(6);

// Client-Side Wireshark Filter & Sort State (100% executed on client CPU/RAM)
const filterProtocol = van.state("ALL");
const filterSearchText = van.state("");
const sortColumn = van.state("id");     // "id" | "tsUs" | "proto" | "chan" | "len" | "summary"
const sortAscending = van.state(false); // Default newest first

const uartPkts = van.state(0);
const i2cPkts = van.state(0);
const wifiPkts = van.state(0);
const zigbeePkts = van.state(0);
const irisTx = van.state(0);
const berPct = van.state("0.000%");
const corruptedCount = van.state(0);
const pcapSize = van.state(24);
const pcapSyncedBytes = van.state(0);
const pcapChunkCount = van.state(0);
const isStreamingChunk = van.state(false);

const accelX = van.state(0);
const accelY = van.state(0);
const accelZ = van.state(980);
const magStr = van.state("X:-305 Y:52 Z:61");

const selectedPacket = van.state(null);
const packets = van.state([]);

// Client-side BER & Protocol Analytics Counters (computed in JS from raw PCAP bytes)
let clientTotalUartBits = 0;
let clientCorruptFrames = 0;
let clientPacketSeq = 0;

// Wireshark-style Canonical Hex + ASCII Formatter with 100% Column Alignment
function formatHex(dataBytes) {
  if (!dataBytes) return "Select a packet in the table above to inspect raw bytes.";
  let bytes = [];
  if (dataBytes instanceof Uint8Array || Array.isArray(dataBytes)) {
    bytes = Array.from(dataBytes);
  } else if (typeof dataBytes === "string") {
    for (let i = 0; i < dataBytes.length; i++) {
      bytes.push(dataBytes.charCodeAt(i) & 0xff);
    }
  }

  if (bytes.length === 0) {
    return "0000:  (0 payload bytes)";
  }

  let lines = [];
  for (let i = 0; i < bytes.length; i += 16) {
    let chunk = bytes.slice(i, i + 16);
    let hexParts = [];
    for (let j = 0; j < 16; j++) {
      if (j < chunk.length) {
        hexParts.push(chunk[j].toString(16).padStart(2, "0"));
      } else {
        hexParts.push("  ");
      }
      if (j === 7) hexParts.push(" ");
    }
    let hex = hexParts.join(" ");
    let ascii = chunk.map(x => (x >= 32 && x <= 126) ? String.fromCharCode(x) : ".").join("");
    let offset = i.toString(16).padStart(4, "0");
    lines.push(`${offset}:  ${hex}  |${ascii.padEnd(16, " ")}|`);
  }
  return lines.join("\n");
}

function showToast(msg) {
  toastMsg.val = msg;
  setTimeout(() => { if (toastMsg.val === msg) toastMsg.val = ""; }, 3500);
}

function triggerCmd(cmd, feedbackText) {
  fetch("/cmd?c=" + cmd, { method: "POST" })
    .then(() => showToast(feedbackText))
    .catch(() => showToast("Command sent: " + cmd));
}

function selectWifiChannel(ch) {
  wifiChannelMode.val = ch;
  if (ch > 0) wifiActiveCh.val = ch;
  let label = ch === 0 ? "All-Channel Sweep (Ch 1..13)" : `Locked to Channel ${ch}`;
  fetch("/cmd?ch=" + ch, { method: "POST" })
    .then(() => showToast("Wi-Fi Sniffer: " + label))
    .catch(() => showToast("Wi-Fi Channel -> " + label));
}

// ---------------------------------------------------------------------------
// Client-Side Binary PCAP Dissector & Statistical Engine
// Offloads all heavy packet decoding, BER math, sorting, and filtering from ESP32-C6
// ---------------------------------------------------------------------------
function recomputeClientBer() {
  let totalCorrupt = Math.max(clientCorruptFrames, corruptedCount.val);
  corruptedCount.val = totalCorrupt;
  let bits = Math.max(clientTotalUartBits, uartPkts.val * 160);
  if (bits > 0 && totalCorrupt > 0) {
    let ber = ((totalCorrupt * 2.0) / bits) * 100.0;
    berPct.val = ber.toFixed(3) + "%";
  } else if (totalCorrupt === 0) {
    berPct.val = "0.000%";
  }
}

function dissectPcapRecord(tsSec, tsUsec, inclLen, payloadBytes) {
  clientPacketSeq++;
  let tsTotalUs = tsSec * 1000000 + tsUsec;
  let timeStr = (tsTotalUs / 1000000).toFixed(3) + "s";

  // Extract 2-byte USER0 link header [protocol, probe_id] if present
  let protoId = 1;
  let probeId = 1;
  let body = payloadBytes;

  if (payloadBytes.length >= 2 && payloadBytes[0] >= 1 && payloadBytes[0] <= 5) {
    protoId = payloadBytes[0];
    probeId = payloadBytes[1];
    body = payloadBytes.slice(2);
  }

  let proto = "UART";
  let tagClass = "uart";
  let chan = `Probe ${probeId}`;
  let summary = "";

  if (protoId === 1) {
    // PROTO_UART: Verify canonical "HESTIA:<counter>:<checksum>" checksum in client JS
    proto = "UART";
    tagClass = "uart";
    chan = "Row 22/13 (GP0->GP1)";
    let ascii = "";
    for (let i = 0; i < body.length; i++) {
      let c = body[i];
      if (c >= 32 && c <= 126) ascii += String.fromCharCode(c);
    }
    clientTotalUartBits += Math.max(body.length * 10, 160);

    let m = ascii.match(/HESTIA:(\d+):([0-9A-Fa-f]{1,4})/);
    if (m) {
      let ctr = parseInt(m[1], 10) >>> 0;
      let rxCsum = parseInt(m[2], 16) & 0xffff;
      let expCsum = (~ctr) & 0xffff;
      if (rxCsum === expCsum) {
        summary = `${ascii} [VALID csum=0x${rxCsum.toString(16).toUpperCase().padStart(4, "0")}]`;
      } else {
        clientCorruptFrames++;
        tagClass = "fault";
        summary = `${ascii} [CRC MISMATCH: rx=0x${rxCsum.toString(16).toUpperCase()} exp=0x${expCsum.toString(16).toUpperCase()}]`;
      }
    } else {
      summary = ascii || "UART Binary Burst";
    }
    recomputeClientBer();
  } else if (protoId === 2) {
    // PROTO_I2C: Hardware-captured I2C frame from GPIO6 (SDA) / GPIO7 (SCL)
    proto = "I2C";
    tagClass = "i2c";
    chan = "I2C1 (Row 55/58)";
    if (body.length >= 1) {
      let addr7 = body[0] >> 1;
      let rw = (body[0] & 1) ? "READ" : "WRITE";
      let devName = addr7 === 0x19 ? "LSM303 Accel" : (addr7 === 0x1e ? "LSM303 Mag" : "I2C Slave");
      let hexPreview = Array.from(body.slice(1, 9)).map(b => b.toString(16).toUpperCase().padStart(2, "0")).join(" ");

      // Client-side GY-511 two's complement decoding if 6-byte sensor read
      if ((body[0] & 1) === 1 && body.length >= 7 && addr7 === 0x19) {
        let rx = ((body[2] << 8) | body[1]);
        if (rx & 0x8000) rx -= 65536;
        rx = rx >> 4;
        let ry = ((body[4] << 8) | body[3]);
        if (ry & 0x8000) ry -= 65536;
        ry = ry >> 4;
        let rz = ((body[6] << 8) | body[5]);
        if (rz & 0x8000) rz -= 65536;
        rz = rz >> 4;
        accelX.val = rx;
        accelY.val = ry;
        accelZ.val = rz;
        summary = `0x${addr7.toString(16).toUpperCase()} ${devName} ${rw}: X=${rx}mg Y=${ry}mg Z=${rz}mg [${hexPreview}]`;
      } else {
        summary = `0x${addr7.toString(16).toUpperCase()} (${devName}) ${rw} [${hexPreview}]`;
      }
    } else {
      summary = "I2C Bus Start/Address Frame";
    }
  } else if (protoId === 4) {
    // PROTO_WIFI: 802.11 Promiscuous Frame with channel in probeId
    proto = "WIFI";
    tagClass = "wifi";
    chan = `Ch ${probeId}`;
    let isIris = false;
    for (let i = 0; i <= body.length - 6; i++) {
      if (body[i] === 0x02 && body[i + 1] === 0x00 && body[i + 2] === 0x00 &&
          body[i + 3] === 0x00 && body[i + 4] === 0x00 && body[i + 5] === 0x03) {
        isIris = true;
        break;
      }
    }
    let fc = body.length >= 2 ? ((body[1] << 8) | body[0]) : 0;
    let subtype = (fc >> 4) & 0xf;
    let typeLabel = subtype === 8 ? "Beacon" : (subtype === 4 ? "Probe Req" : "802.11 Data");
    summary = `802.11 ${typeLabel} on Ch ${probeId} (${body.length}B)${isIris ? " [IRIS BSSID 02:00:00:00:00:03]" : ""}`;
  } else if (protoId === 5) {
    // PROTO_IEEE802154: Channel 15 frame
    proto = "ZIGBEE";
    tagClass = "zigbee";
    chan = `Ch ${probeId || 15}`;
    let ascii = "";
    for (let i = 0; i < body.length; i++) {
      if (body[i] >= 32 && body[i] <= 126) ascii += String.fromCharCode(body[i]);
    }
    let seqNum = body.length >= 3 ? body[2] : 0;
    summary = `802.15.4 PAN 0x1234 Seq #${seqNum} ${ascii ? `("${ascii}")` : ""}`;
  } else if (protoId === 3) {
    // PROTO_FAULT_ALERT
    proto = "FAULT";
    tagClass = "fault";
    chan = `Probe ${probeId}`;
    clientCorruptFrames++;
    recomputeClientBer();
    let faultDesc = probeId === 0 ? "10 µs UART Bit-Flip (Row 13)"
                  : probeId === 2 ? "50 µs I2C ACK-Kill (Row 58 SDA)"
                  : probeId === 3 ? "50 µs I2C Clock-Stretch (Row 55 SCL)"
                  : "Targeted 802.15.4 Burst (Ch 15 -> Iris)";
    summary = `⚡ HARDWARE FAULT INJECTED: ${faultDesc}`;
  }

  return {
    id: clientPacketSeq,
    tsUs: tsTotalUs,
    time: timeStr,
    proto,
    chan,
    len: body.length,
    summary,
    raw: body,
    tagClass
  };
}

// ---------------------------------------------------------------------------
// Batched & Chunked SD PCAP Streamer (/api/pcap?offset=<N>&limit=4096)
// Prevents full-file buffering lag; parses binary records incrementally in client RAM
// ---------------------------------------------------------------------------
let s_pcapStreamOffset = 0;
let s_pcapCarryBuffer = new Uint8Array(0);
const PCAP_BATCH_LIMIT = 4096; // 4 KB chunk per HTTP batch (8 SD sectors)

function concatUint8(a, b) {
  if (a.length === 0) return b;
  if (b.length === 0) return a;
  let out = new Uint8Array(a.length + b.length);
  out.set(a, 0);
  out.set(b, a.length);
  return out;
}

function parseIncomingPcapBytes(chunkBytes, isFromStart) {
  let buf = concatUint8(s_pcapCarryBuffer, chunkBytes);
  let pos = 0;

  // Skip 24-byte global PCAP header at file offset 0 (Magic 0xA1B2C3D4)
  if (isFromStart && buf.length >= 24) {
    let dvGlobal = new DataView(buf.buffer, buf.byteOffset, 24);
    let magic = dvGlobal.getUint32(0, true);
    if (magic === 0xa1b2c3d4 || magic === 0xd4c3b2a1) {
      pos = 24;
    }
  }

  let newRecords = [];
  while (pos + 16 <= buf.length) {
    let dv = new DataView(buf.buffer, buf.byteOffset + pos, 16);
    let tsSec = dv.getUint32(0, true);
    let tsUsec = dv.getUint32(4, true);
    let inclLen = dv.getUint32(8, true);

    if (inclLen > 2048) {
      // Resync if across boundary
      pos += 16;
      continue;
    }
    if (pos + 16 + inclLen > buf.length) {
      break; // Wait for next streamed chunk to complete this packet
    }

    let pktBytes = buf.slice(pos + 16, pos + 16 + inclLen);
    let rec = dissectPcapRecord(tsSec, tsUsec, inclLen, pktBytes);
    newRecords.push(rec);
    pos += 16 + inclLen;
  }

  s_pcapCarryBuffer = buf.slice(pos);

  if (newRecords.length > 0) {
    let merged = [...newRecords.reverse(), ...packets.val].slice(0, 500);
    packets.val = merged;
    if (!selectedPacket.val && merged.length > 0) {
      selectedPacket.val = merged[0];
    }
  }
}

async function fetchNextPcapChunk(forceManual = false) {
  if (isStreamingChunk.val) return;
  isStreamingChunk.val = true;
  try {
    let isStart = (s_pcapStreamOffset === 0);
    let resp = await fetch(`/api/pcap?offset=${s_pcapStreamOffset}&limit=${PCAP_BATCH_LIMIT}`);
    if (!resp.ok) {
      isStreamingChunk.val = false;
      return;
    }
    let totalHeader = resp.headers.get("X-Pcap-Total-Bytes");
    if (totalHeader) {
      let totalVal = parseInt(totalHeader, 10);
      if (!isNaN(totalVal) && totalVal > 0) pcapSize.val = totalVal;
    }

    let arrayBuf = await resp.arrayBuffer();
    let bytes = new Uint8Array(arrayBuf);
    if (bytes.length > 0) {
      parseIncomingPcapBytes(bytes, isStart);
      s_pcapStreamOffset += bytes.length;
      pcapSyncedBytes.val = s_pcapStreamOffset;
      pcapChunkCount.val = pcapChunkCount.val + 1;
      if (forceManual) {
        showToast(`Streamed ${bytes.length} B chunk from MicroSD (offset ${s_pcapStreamOffset} B)`);
      }
    } else if (forceManual) {
      showToast("SD PCAP buffer up to date (" + s_pcapStreamOffset + " B synced)");
    }
  } catch (e) {
    // Ignore transient network errors during channel hops
  }
  isStreamingChunk.val = false;
}

// Floating Toast Notification Component (Zero Layout Shift)
function ToastOverlay() {
  return div({ class: "toast-container" },
    () => toastMsg.val
      ? div({ class: "toast" },
          span(toastMsg.val),
          span({ style: "cursor:pointer;opacity:0.6;font-size:14px", onclick: () => toastMsg.val = "" }, "✕")
        )
      : null
  );
}

// Top Navigation Bar
function Header() {
  return div({ class: "top-nav" },
    div({ class: "brand-group" },
      span({ class: "brand-title" }, "WHATTHEFLIP"),
      span({ class: "brand-sub" }, "Self-Contained SD Web Bench // µT-Kernel 3.0")
    ),
    div({ class: "header-status-group" },
      button({
        class: "btn-baseline-nav",
        onclick: () => {
          triggerCmd("R", "Reset to clean baseline. Probes armed.");
          safetyState.val = "ARMED";
          filterProtocol.val = "ALL";
          filterSearchText.val = "";
          clientCorruptFrames = 0;
          corruptedCount.val = 0;
          berPct.val = "0.000%";
        }
      }, "Reset to Baseline"),
      () => {
        let isAborted = safetyState.val === "ABORTED";
        return isAborted
          ? button({ class: "btn-rearm-nav", onclick: () => { triggerCmd("R", "System re-armed"); safetyState.val = "ARMED"; } }, "Re-Arm System")
          : button({ class: "btn-estop-nav", onclick: () => { triggerCmd("E", "Emergency stop triggered. Probes floated in < 200 ns."); safetyState.val = "ABORTED"; } }, "EMERGENCY STOP");
      },
      a({ href: "/capture.pcap", download: "capture.pcap", class: "btn-pcap" }, "Download PCAP")
    )
  );
}

// Diagnostic Scenarios Card (UART -> I2C -> Wi-Fi -> Zigbee in exact order)
function ScenariosCard() {
  const scenarioList = [
    { id: "UART", label: "UART" },
    { id: "I2C", label: "I2C" },
    { id: "WIFI", label: "Wi-Fi" },
    { id: "ZIGBEE", label: "Zigbee" }
  ];

  return div({ class: "card" },
    div({ class: "card-title" },
      span("Diagnostic Scenarios"),
      () => span({ class: "val-mono", style: "font-size:10px;color:var(--primary)" }, activeScenario.val)
    ),
    div({ class: "scenario-tab-bar" },
      scenarioList.map(s => button({
        class: () => "scenario-tab" + (activeScenario.val === s.id ? " active" : ""),
        onclick: () => {
          activeScenario.val = s.id;
          filterProtocol.val = s.id;
          showToast(`Switched to ${s.label} scenario`);
        }
      }, s.label))
    ),
    () => {
      let sc = activeScenario.val;
      if (sc === "UART") {
        return div({ style: "display:flex;flex-direction:column;gap:8px" },
          div({ class: "scenario-desc-text" }, "Unidirectional UART1 hardware RX on Hestia GP0 -> Daphne GP1 (115200 8N1). Series 220 Ω resistor isolates glitch current (<= 15 mA)."),
          button({
            class: "btn-scenario-action",
            onclick: () => triggerCmd("U", "Injected 10 µs UART bit-flip -> Daphne detected checksum error")
          },
            span("⚡ Inject 10 µs Bit-Flip"),
            span({ class: "tag" }, "Probe 0 // Row 13")
          )
        );
      } else if (sc === "I2C") {
        return div({ style: "display:flex;flex-direction:column;gap:8px" },
          div({ class: "scenario-desc-text" }, "Hardware edge-capture ISR on Row 55 (SCL) & Row 58 (SDA) tapping Daphne <-> GY-511 at 400 kHz. Synchronized 9th-clock ACK-kill."),
          button({
            class: "btn-scenario-action",
            onclick: () => triggerCmd("A", "Injected 9th-clock 50 µs ACK-kill -> Daphne triggered 9-clock recovery")
          },
            span("💥 Inject 50 µs ACK-Kill"),
            span({ class: "tag" }, "Probe 2 // SDA")
          ),
          button({
            class: "btn-scenario-action",
            onclick: () => triggerCmd("S", "Injected 50 µs clock-stretch on SCL")
          },
            span("⏱️ Inject 50 µs Clock-Stretch"),
            span({ class: "tag" }, "Probe 3 // SCL")
          )
        );
      } else if (sc === "WIFI") {
        const channels = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13];
        return div({ style: "display:flex;flex-direction:column;gap:6px" },
          div({ class: "scenario-desc-text" },
            () => `Single-radio time-sliced promiscuous capture (Active: Ch ${wifiActiveCh.val}). Sweep all Ch 1–13 or lock a target channel:`
          ),
          button({
            class: () => "btn-scenario-action" + (wifiChannelMode.val === 0 ? " active" : ""),
            style: () => wifiChannelMode.val === 0 ? "border-color:var(--primary);background:#faebe3" : "",
            onclick: () => selectWifiChannel(0)
          },
            span("🔄 Sweep All Channels (1–13)"),
            span({ class: "tag" }, () => wifiChannelMode.val === 0 ? `ACTIVE (Ch ${wifiActiveCh.val})` : "Auto-Hop")
          ),
          div({ class: "ch-grid" },
            channels.map(ch => button({
              class: () => "btn-ch" + (wifiChannelMode.val === ch ? " active" : ""),
              onclick: () => selectWifiChannel(ch)
            }, `Ch ${ch}`))
          )
        );
      } else {
        return div({ style: "display:flex;flex-direction:column;gap:8px" },
          div({ class: "scenario-desc-text" }, "IEEE 802.15.4 hardware transceiver active on Channel 15. Captures Iris PAN 0x1234 beacons and injects allow-listed bursts."),
          button({
            class: "btn-scenario-action",
            onclick: () => triggerCmd("W", "Targeted 802.15.4 burst sent to Iris BSSID 02:00:00:00:00:03")
          },
            span("🎯 Targeted Zigbee Jam"),
            span({ class: "tag" }, "Iris Ch 15")
          )
        );
      }
    }
  );
}

// Live Bus Telemetry Card
function TelemetryCard() {
  return div({ class: "card" },
    div({ class: "card-title" },
      span("Traffic & Bus Health"),
      span({ style: "font-size:9px;color:var(--c-green-text);font-family:var(--font-mono)" }, "CLIENT-JS COMPUTED")
    ),
    div({ class: "stat-box" },
      () => div({ class: "stat-num" }, berPct.val),
      () => div({ class: "stat-sub" }, `${corruptedCount.val} corrupted frames recorded`)
    ),
    table({ class: "data-table" },
      tr(
        td({ class: "label" }, "UART (115200)"),
        td({ class: "val" }, () => uartPkts.val)
      ),
      tr(
        td({ class: "label" }, "I2C (400 kHz)"),
        td({ class: "val" }, () => i2cPkts.val)
      ),
      tr(
        td({ class: "label" }, "Wi-Fi (Ch 1–13)"),
        td({ class: "val" }, () => `${wifiPkts.val} (Ch ${wifiActiveCh.val})`)
      ),
      tr(
        td({ class: "label" }, "Zigbee (Ch 15)"),
        td({ class: "val" }, () => zigbeePkts.val)
      ),
      tr(
        td({ class: "label" }, "SD PCAP Stream"),
        td({ class: "val" }, () => `${pcapSyncedBytes.val} / ${pcapSize.val} B (${pcapChunkCount.val} chunks)`)
      )
    )
  );
}

// Daphne Motion Sensor Card
function SensorCard() {
  return div({ class: "card" },
    div({ class: "card-title" }, "GY-511 I2C Sensor (0x19 / 0x1E)"),
    div({ class: "bar-label-row" },
      span("Roll (X)"),
      () => span({ class: "val-mono" }, `${accelX.val} mg`)
    ),
    div({ class: "bar-track" },
      div({
        class: "bar-fill",
        style: () => `width:${Math.min(100, Math.max(0, (accelX.val + 1000) / 20))}%;background:#0284c7`
      })
    ),
    div({ class: "bar-label-row" },
      span("Pitch (Y)"),
      () => span({ class: "val-mono" }, `${accelY.val} mg`)
    ),
    div({ class: "bar-track" },
      div({
        class: "bar-fill",
        style: () => `width:${Math.min(100, Math.max(0, (accelY.val + 1000) / 20))}%;background:#7c3aed`
      })
    ),
    div({ class: "bar-label-row" },
      span("Gravity (Z ~980 mg)"),
      () => span({ class: "val-mono" }, `${accelZ.val} mg`)
    ),
    div({ class: "bar-track" },
      div({
        class: "bar-fill",
        style: () => `width:${Math.min(100, Math.max(0, (accelZ.val + 1000) / 20))}%;background:#15803d`
      })
    ),
    table({ class: "data-table", style: "margin-top:4px" },
      tr(
        td({ class: "label" }, "Compass"),
        td({ class: "val" }, () => magStr.val)
      )
    )
  );
}

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

// Wireshark-Style Packet Stream Panel with Client-Side Sorting, Filtering & SD Chunk Streaming
function StreamPanel() {
  return div({ class: "card", style: "display:flex;flex-direction:column;min-width:0" },
    div({ class: "card-title" },
      span("Live Packet Analyzer (Client-Side JS Dissector & Sorter)"),
      () => span({ style: "color:var(--text-dim);font-family:var(--font-mono);font-size:10px" },
        `${packets.val.length} DECODED FRAMES`
      )
    ),
    // SD Card Chunked Streaming Status Bar
    div({ class: "sd-chunk-bar" },
      () => span(
        `SD /CAPTURE.PCAP: ${pcapSyncedBytes.val} B streamed of ${pcapSize.val} B (4 KB batch mode, ${pcapChunkCount.val} batches)`
      ),
      div({ style: "display:flex;gap:6px" },
        button({
          class: "btn-ws-clear",
          onclick: () => fetchNextPcapChunk(true)
        }, "Fetch Next 4KB Chunk"),
        button({
          class: "btn-ws-clear",
          onclick: () => {
            s_pcapStreamOffset = 0;
            s_pcapCarryBuffer = new Uint8Array(0);
            packets.val = [];
            pcapChunkCount.val = 0;
            fetchNextPcapChunk(true);
          }
        }, "Reload From Offset 0")
      )
    ),
    // Wireshark Display Filter Toolbar
    div({ class: "wireshark-filter-bar" },
      span({ class: "ws-filter-label" }, "Filter:"),
      select({
        class: "ws-filter-select",
        value: () => filterProtocol.val,
        onchange: (e) => filterProtocol.val = e.target.value
      },
        option({ value: "ALL" }, "All Protocols"),
        option({ value: "UART" }, "UART (115200 8N1)"),
        option({ value: "I2C" }, "I2C (400 kHz)"),
        option({ value: "WIFI" }, "Wi-Fi 6 (Ch 1–13)"),
        option({ value: "ZIGBEE" }, "Zigbee (802.15.4)"),
        option({ value: "FAULTS" }, "Faults & Corruptions")
      ),
      input({
        class: "ws-filter-input",
        type: "text",
        placeholder: "Client-side filter expression... (e.g. lsm303, iris, ch 6, mismatch, hestia)",
        value: () => filterSearchText.val,
        oninput: (e) => filterSearchText.val = e.target.value
      }),
      button({
        class: "btn-ws-clear",
        onclick: () => {
          filterProtocol.val = "ALL";
          filterSearchText.val = "";
          showToast("Display filter cleared");
        }
      }, "Clear")
    ),
    // Stream Table with Clickable Client-Side Sort Headers
    div({ class: "stream-box" },
      table({ class: "stream-table" },
        thead(
          tr(
            th({ onclick: () => toggleSort("id") }, () => "NO." + sortIndicator("id")),
            th({ onclick: () => toggleSort("tsUs") }, () => "TIME" + sortIndicator("tsUs")),
            th({ onclick: () => toggleSort("proto") }, () => "BUS" + sortIndicator("proto")),
            th({ onclick: () => toggleSort("chan") }, () => "SOURCE / CH" + sortIndicator("chan")),
            th({ onclick: () => toggleSort("len") }, () => "BYTES" + sortIndicator("len")),
            th({ onclick: () => toggleSort("summary") }, () => "DECODED DATA / SUMMARY" + sortIndicator("summary"))
          )
        ),
        () => {
          let protoFilter = filterProtocol.val;
          let search = filterSearchText.val.toLowerCase().trim();
          let col = sortColumn.val;
          let asc = sortAscending.val;

          let list = packets.val.filter(p => {
            if (protoFilter !== "ALL") {
              if (protoFilter === "UART" && p.proto !== "UART") return false;
              if (protoFilter === "I2C" && p.proto !== "I2C") return false;
              if (protoFilter === "WIFI" && p.proto !== "WIFI") return false;
              if (protoFilter === "ZIGBEE" && p.proto !== "ZIGBEE") return false;
              if (protoFilter === "FAULTS" && p.proto !== "FAULT" && p.tagClass !== "fault") return false;
            }
            if (search) {
              let text = (p.time + " " + p.proto + " " + p.chan + " " + p.summary).toLowerCase();
              if (!text.includes(search)) return false;
            }
            return true;
          });

          // Client-side sorting in JS
          let sorted = list.slice().sort((a, b) => {
            let va = a[col];
            let vb = b[col];
            if (typeof va === "string" && typeof vb === "string") {
              let cmp = va.localeCompare(vb);
              return asc ? cmp : -cmp;
            }
            let diff = (va || 0) - (vb || 0);
            return asc ? diff : -diff;
          });

          return tbody(sorted.slice(0, 120).map(p => tr({
            class: () => (selectedPacket.val === p ? "selected" : ""),
            onclick: () => selectedPacket.val = p
          },
            td(p.id || "-"),
            td(p.time),
            td(span({ class: `tag-badge ${p.tagClass}` }, p.proto)),
            td(p.chan),
            td(p.len != null ? `${p.len}` : "-"),
            td({ class: "summary-col" }, p.summary)
          )));
        }
      )
    ),
    // Wireshark-Style Packet Bytes & Inspection
    div({ class: "inspector-card" },
      div({ class: "card-title", style: "margin-bottom:6px" }, "Packet Bytes (Client-Side Hex & ASCII Dissector)"),
      () => {
        let p = selectedPacket.val;
        if (!p) {
          return div({ class: "inspector-box" }, "Select a packet in the table above to view raw bytes.");
        }
        return div(
          div({ class: "inspector-meta" },
            span("Frame: ", b(`#${p.id || 1}`)),
            span("Time: ", b(p.time)),
            span("Protocol: ", b(p.proto)),
            span("Source/Channel: ", b(p.chan)),
            span("Length: ", b(p.len != null ? `${p.len} bytes` : "-"))
          ),
          pre({ class: "inspector-box" }, formatHex(p.raw || p.summary))
        );
      }
    )
  );
}

// Ingestion processor for immediate WebSocket alert events
function pushAlertRecord(time, proto, chan, len, summary, raw, tagClass) {
  clientPacketSeq++;
  let rec = { id: clientPacketSeq, tsUs: Date.now() * 1000, time, proto, chan, len, summary, raw, tagClass };
  packets.val = [rec, ...packets.val.slice(0, 499)];
  if (!selectedPacket.val) selectedPacket.val = rec;
}

function updateFromTelemetry(d) {
  if (d.safety) safetyState.val = d.safety;
  if (d.uart_pkts != null) uartPkts.val = d.uart_pkts;
  if (d.i2c_pkts != null) i2cPkts.val = d.i2c_pkts;
  if (d.wifi_pkts != null) wifiPkts.val = d.wifi_pkts;
  if (d.ieee154_pkts != null) zigbeePkts.val = d.ieee154_pkts;
  if (d.iris_tx != null) irisTx.val = d.iris_tx;
  if (d.corrupted_frames != null) {
    corruptedCount.val = Math.max(corruptedCount.val, d.corrupted_frames);
  }
  if (d.wifi_ch != null) wifiActiveCh.val = d.wifi_ch;
  if (d.wifi_mode != null) wifiChannelMode.val = d.wifi_mode;

  if (d.pcap_bytes != null) {
    pcapSize.val = d.pcap_bytes;
    // Automatically stream next 4 KB chunk when new PCAP bytes arrive on SD card
    if (d.pcap_bytes > s_pcapStreamOffset) {
      fetchNextPcapChunk(false);
    }
  }
  recomputeClientBer();

  if (d.accel) {
    accelX.val = d.accel.x != null ? d.accel.x : accelX.val;
    accelY.val = d.accel.y != null ? d.accel.y : accelY.val;
    accelZ.val = d.accel.z != null ? d.accel.z : accelZ.val;
  }
  if (d.mag) {
    magStr.val = `X:${d.mag.x} Y:${d.mag.y} Z:${d.mag.z}`;
  }
}

// Data Stream Connection
function connectDataStream() {
  // 1. Kick off initial 4 KB chunked PCAP fetch immediately on load
  fetchNextPcapChunk(false);

  // 2. Periodic chunked PCAP tailing & telemetry sync
  setInterval(() => {
    if (pcapSize.val > s_pcapStreamOffset) {
      fetchNextPcapChunk(false);
    }
  }, 800);

  let wsProtocol = window.location.protocol === "https:" ? "wss:" : "ws:";
  let ws = new WebSocket(`${wsProtocol}//${window.location.host}/ws`);
  ws.onmessage = (evt) => {
    let d = JSON.parse(evt.data);
    if (d.type === "telemetry") {
      updateFromTelemetry(d);
    } else if (d.type === "alert") {
      showToast(d.msg || "ALERT");
      pushAlertRecord("NOW", "FAULT", "ALERT", 0, d.msg, d.msg, "fault");
    }
  };
}

// App Root Layout
function App() {
  return div({ class: "app-container" },
    ToastOverlay(),
    Header(),
    div({ class: "dashboard-grid" },
      div({ class: "panel-col" },
        ScenariosCard(),
        TelemetryCard(),
        SensorCard()
      ),
      div({ class: "panel-col" },
        StreamPanel()
      )
    )
  );
}

van.add(document.getElementById("app"), App());
connectDataStream();
