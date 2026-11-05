import van from "vanjs-core";

const { div, span, button, a, table, thead, tbody, tr, th, td, pre, b, select, option, input } = van.tags;
const svgNs = van.tags("http://www.w3.org/2000/svg");
const { svg, g, rect, line, path, circle, text, polyline } = svgNs;

// ---------------------------------------------------------------------------
// Hardware & Workbench State
// ---------------------------------------------------------------------------
const safetyState     = van.state("ARMED");
const toastMsg        = van.state("");
const isPaused        = van.state(false);
const autoFreezeGlitch = van.state(true);
const activeGlitchBus = van.state(""); // "" | "UART" | "I2C_ACK" | "I2C_STRETCH" | "RF"

// Selected Topology Entity & Inspector Tab
const selectedNode    = van.state("whattheflip"); // "hestia" | "r1" | "daphne" | "gy511" | "whattheflip" | "iris"
const selectedBus     = van.state("ALL");         // "ALL" | "UART" | "I2C" | "WIFI" | "ZIGBEE" | "FAULTS"
const activeInspTab   = van.state("PDU");         // "PDU" | "RTOS" | "SCOPE"

// Wi-Fi Channel Control State (0 = Sweep Ch 1..13, 1..13 = Locked Channel)
const wifiChannelMode = van.state(0);
const wifiActiveCh    = van.state(6);

// Client-Side Wireshark Filter & Sort State (100% executed in Client JS)
const filterProtocol   = van.state("ALL");
const filterSearchText = van.state("");
const sortColumn       = van.state("id"); // "id" | "tsUs" | "proto" | "chan" | "len" | "summary"
const sortAscending    = van.state(false);

// Live Telemetry Counters & GY-511 Sensor State
const uartPkts         = van.state(0);
const i2cPkts          = van.state(0);
const wifiPkts         = van.state(0);
const zigbeePkts       = van.state(0);
const berPct           = van.state("0.000%");
const corruptedCount   = van.state(0);
const pcapSize         = van.state(24);
const pcapSyncedBytes  = van.state(0);
const pcapChunkCount   = van.state(0);
const isStreamingChunk = van.state(false);

const accelX     = van.state(-28);
const accelY     = van.state(54);
const accelZ     = van.state(981);
const accelAvgX  = van.state(-28);
const accelAvgY  = van.state(54);
const accelAvgZ  = van.state(981);
const rawRegHex  = van.state("E0 FF 60 03 D0 3D");
const magStr     = van.state("X:-305 Y:52 Z:61");
const animPhase  = van.state(0);

// Rolling History for 60 FPS Raw vs. 8-Sample Moving Average Scope
const scopeRawXHist = van.state([-28, -26, -30, -25, -29, -28, -27, -28]);
const scopeAvgXHist = van.state([-28, -27, -28, -27, -28, -28, -28, -28]);

const selectedPacket = van.state(null);
const packets        = van.state([]);

let clientTotalUartBits = 0;
let clientCorruptFrames = 0;
let clientPacketSeq     = 0;
let xFilterRing         = [];

// ---------------------------------------------------------------------------
// Canonical 16-Byte Aligned Hex + ASCII Formatter
// ---------------------------------------------------------------------------
function formatHex(dataBytes) {
  if (!dataBytes) return "Select a frame in the Wireshark table or click a PDU envelope on the canvas.";
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
  setTimeout(() => { if (toastMsg.val === msg) toastMsg.val = ""; }, 3500);
}

function triggerCmd(cmd, feedbackText, glitchType = "") {
  if (glitchType) {
    activeGlitchBus.val = glitchType;
    if (autoFreezeGlitch.val) {
      isPaused.val = true;
      activeInspTab.val = "PDU";
    }
    setTimeout(() => { if (activeGlitchBus.val === glitchType) activeGlitchBus.val = ""; }, 1800);
  }
  fetch("/cmd?c=" + cmd, { method: "POST" })
    .then(() => showToast(feedbackText))
    .catch(() => showToast(feedbackText));
}

function selectWifiChannel(ch) {
  wifiChannelMode.val = ch;
  if (ch > 0) wifiActiveCh.val = ch;
  let label = ch === 0 ? "All-Channel Sweep (Ch 1..13)" : `Locked to Channel ${ch}`;
  fetch("/cmd?ch=" + ch, { method: "POST" })
    .then(() => showToast("Wi-Fi Radio: " + label))
    .catch(() => showToast("Wi-Fi Radio: " + label));
}

// ---------------------------------------------------------------------------
// Client-Side Binary PCAP Dissector, Moving-Average Filter & BER Engine
// ---------------------------------------------------------------------------
function updateAccelHistory(rx, ry, rz) {
  accelX.val = rx;
  accelY.val = ry;
  accelZ.val = rz;
  xFilterRing.push(rx);
  if (xFilterRing.length > 8) xFilterRing.shift();
  let sumX = xFilterRing.reduce((a, b) => a + b, 0);
  let avgX = Math.round(sumX / xFilterRing.length);
  accelAvgX.val = avgX;
  accelAvgY.val = ry;
  accelAvgZ.val = rz;

  scopeRawXHist.val = [...scopeRawXHist.val.slice(-31), rx];
  scopeAvgXHist.val = [...scopeAvgXHist.val.slice(-31), avgX];
}

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
  let timeStr   = (tsTotalUs / 1000000).toFixed(3) + "s";

  let protoId = 1;
  let probeId = 1;
  let body    = payloadBytes;

  if (payloadBytes.length >= 2 && payloadBytes[0] >= 1 && payloadBytes[0] <= 5) {
    protoId = payloadBytes[0];
    probeId = payloadBytes[1];
    body    = payloadBytes.slice(2);
  }

  let proto        = "UART";
  let tagClass     = "uart";
  let chan         = `Probe ${probeId}`;
  let summary      = "";
  let inboundAscii = "HESTIA:128:FF7F";
  let outboundAscii = "HESTIA:128:FF7F";
  let flippedBitIdx = -1;

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
        flippedBitIdx = 3;
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
      let hexPrev = Array.from(body.slice(1, 7)).map(b => b.toString(16).toUpperCase().padStart(2, "0")).join(" ");

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
    chan     = `802.15.4 Ch ${probeId || 15}`;
    let ascii = "";
    for (let i = 0; i < body.length; i++) {
      if (body[i] >= 32 && body[i] <= 126) ascii += String.fromCharCode(body[i]);
    }
    let seqNum = body.length >= 3 ? body[2] : 0;
    summary = `802.15.4 PAN 0x1234 Seq #${seqNum} ${ascii ? `("${ascii}")` : ""}`;
  } else if (protoId === 3) {
    proto    = "FAULT";
    tagClass = "fault";
    chan     = `Probe ${probeId}`;
    clientCorruptFrames++;
    recomputeClientBer();
    flippedBitIdx = 3;
    inboundAscii  = "HESTIA:128:FF7F";
    outboundAscii = "HESTIA:128:F77F";
    let faultDesc = probeId === 0 ? "10 µs UART1 TX Bit-Flip on Row 13 (15.0 mA across R1 220 Ω)"
                  : probeId === 2 ? "50 µs I2C0 9th-Clock ACK-Kill on Row 58 SDA -> Daphne 9-Clock Recovery"
                  : probeId === 3 ? "50 µs I2C0 Clock-Stretch on Row 55 SCL"
                  : "Targeted 802.15.4 Jam Burst on Ch 15 -> Iris 02:00:00:00:00:03";
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
    tagClass,
    inboundAscii,
    outboundAscii,
    flippedBitIdx
  };
}

// ---------------------------------------------------------------------------
// Batched 4 KB SD Card PCAP Streamer (/api/pcap?offset=<N>&limit=4096)
// ---------------------------------------------------------------------------
let s_pcapStreamOffset = 0;
let s_pcapCarryBuffer  = new Uint8Array(0);
const PCAP_BATCH_LIMIT = 4096;

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

  if (isFromStart && buf.length >= 24) {
    let dvGlobal = new DataView(buf.buffer, buf.byteOffset, 24);
    let magic = dvGlobal.getUint32(0, true);
    if (magic === 0xa1b2c3d4 || magic === 0xd4c3b2a1) {
      pos = 24;
    }
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
    let resp    = await fetch(`/api/pcap?offset=${s_pcapStreamOffset}&limit=${PCAP_BATCH_LIMIT}`);
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
    let bytes    = new Uint8Array(arrayBuf);
    if (bytes.length > 0) {
      parseIncomingPcapBytes(bytes, isStart);
      s_pcapStreamOffset   += bytes.length;
      pcapSyncedBytes.val   = s_pcapStreamOffset;
      pcapChunkCount.val    = pcapChunkCount.val + 1;
      if (forceManual) {
        showToast(`Streamed ${bytes.length} B chunk from SD (/CAPTURE.PCAP @ ${s_pcapStreamOffset} B)`);
      }
    }
  } catch (e) {}
  isStreamingChunk.val = false;
}

// ---------------------------------------------------------------------------
// Top Command & Transport Bar (46px height)
// ---------------------------------------------------------------------------
function TopCommandBar() {
  return div({ class: "top-cmd-bar" },
    div({ class: "brand-cluster" },
      span({ class: "brand-logo" }, "WHATTHEFLIP // WORKBENCH"),
      () => span({
        class: "safety-pill" + (safetyState.val === "ABORTED" ? " aborted" : "")
      }, safetyState.val === "ABORTED" ? "🛑 E-STOP HIGH-Z (<200ns)" : "● ARMED (µT-Kernel 3.0)")
    ),
    // Packet Tracer Transport Controls
    div({ class: "transport-cluster" },
      button({
        class: () => "cmd-btn" + (!isPaused.val ? " active" : ""),
        onclick: () => { isPaused.val = false; showToast("Real-Time Packet Flow Resumed"); }
      }, "▶ Live"),
      button({
        class: () => "cmd-btn" + (isPaused.val ? " active" : ""),
        onclick: () => { isPaused.val = true; showToast("Bullet-Time Freeze Active: Click any PDU envelope"); }
      }, "⏸ Freeze"),
      button({
        class: () => "cmd-btn" + (autoFreezeGlitch.val ? " active" : ""),
        onclick: () => {
          autoFreezeGlitch.val = !autoFreezeGlitch.val;
          showToast("Auto-Freeze on Glitch: " + (autoFreezeGlitch.val ? "ON" : "OFF"));
        }
      }, () => `⚡ Auto-Freeze: ${autoFreezeGlitch.val ? "ON" : "OFF"}`),
      button({
        class: "cmd-btn",
        onclick: () => fetchNextPcapChunk(true)
      }, () => `SD Chunk (${pcapSyncedBytes.val}/${pcapSize.val}B)`)
    ),
    // Hardware Fault Injectors (UART1 TX, I2C0 ACK/Stretch, 802.15.4 Jam)
    div({ class: "injector-cluster" },
      button({
        class: "cmd-btn glitch",
        onclick: () => triggerCmd("U", "UART1 TX injected 10 µs bit-flip on Row 13 (15 mA across 220 Ω)", "UART")
      }, "⚡ 10µs UART Flip (Row 13)"),
      button({
        class: "cmd-btn glitch",
        onclick: () => triggerCmd("A", "I2C0 injected 9th-clock 50 µs ACK-kill on Row 58 -> Daphne 9-clk recovery", "I2C_ACK")
      }, "💥 50µs I2C ACK-Kill (Row 58)"),
      button({
        class: "cmd-btn warn",
        onclick: () => triggerCmd("S", "I2C0 stretched SCL low for 50 µs on Row 55", "I2C_STRETCH")
      }, "⏱ 50µs I2C Stretch (Row 55)"),
      button({
        class: "cmd-btn warn",
        onclick: () => triggerCmd("W", "Targeted 802.15.4 burst to Iris BSSID 02:00:00:00:00:03 on Ch 15", "RF")
      }, "📡 802.15.4 Jam (Ch 15)")
    ),
    // Safety & Export Controls
    div({ class: "safety-cluster" },
      button({
        class: "cmd-btn",
        onclick: () => {
          triggerCmd("R", "Reset to clean 0.000% BER baseline.");
          safetyState.val = "ARMED";
          isPaused.val = false;
          filterProtocol.val = "ALL";
          filterSearchText.val = "";
          clientCorruptFrames = 0;
          corruptedCount.val = 0;
          berPct.val = "0.000%";
        }
      }, "Reset Baseline"),
      () => safetyState.val === "ABORTED"
        ? button({
            class: "cmd-btn rearm",
            onclick: () => { triggerCmd("R", "Probes re-armed"); safetyState.val = "ARMED"; }
          }, "RE-ARM PROBES")
        : button({
            class: "cmd-btn estop",
            onclick: () => { triggerCmd("E", "GPIO9 E-STOP: Probes floated in ~6.25 ns (< 200 ns)"); safetyState.val = "ABORTED"; }
          }, "🛑 E-STOP"),
      a({ href: "/capture.pcap", download: "capture.pcap", class: "cmd-btn pcap-dl" }, "⬇ .pcap")
    )
  );
}

// ---------------------------------------------------------------------------
// Upper Left: Interactive Packet Tracer Hardware Topology Canvas (SVG)
// ---------------------------------------------------------------------------
function selectTopoNode(nodeId, protoFilter) {
  selectedNode.val = nodeId;
  activeInspTab.val = "RTOS";
  if (protoFilter) filterProtocol.val = protoFilter;
}

function selectTopoBus(busProto, tab = "PDU") {
  selectedBus.val = busProto;
  filterProtocol.val = busProto;
  activeInspTab.val = tab;
}

function TopologyCanvas() {
  return div({ class: "topology-pane" },
    div({ class: "pane-header" },
      span("INTERACTIVE HARDWARE TOPOLOGY // CLICK NODE, BREADBOARD WIRE, OR PDU ENVELOPE"),
      () => span({ style: "color:var(--proto-uart)" },
        `BER: ${berPct.val} | UART:${uartPkts.val} I2C:${i2cPkts.val} WiFi:${wifiPkts.val}(Ch${wifiActiveCh.val}) 15.4:${zigbeePkts.val}`
      )
    ),
    div({ class: "topology-svg-wrap" },
      () => {
        let ph        = animPhase.val;
        let estop     = safetyState.val === "ABORTED";
        let glitchBus = activeGlitchBus.val;
        let selNode   = selectedNode.val;
        let probeDash = estop ? "4 4" : "none";

        // Envelope positions along wires (0.0 -> 1.0)
        let t1 = (ph % 100) / 100;
        let t2 = ((ph + 50) % 100) / 100;

        // Wire 1: Hestia GP0 (155,70) -> R1 (240,70)
        let pduUartCleanX = 155 + t1 * 75;
        // Wire 2: R1 (315,70) -> Daphne GP1 (420,70)
        let pduUartOutX   = 315 + t1 * 95;
        // Wire 3: GY-511 (560,155) -> Daphne I2C1 (490,115)
        let pduI2cX       = 555 - t2 * 60;
        // Wire 4: Iris RF (490,260) -> whattheflip RF (265,245)
        let pduRfX        = 480 - t2 * 200;

        return svg({ class: "topology-svg", viewBox: "0 0 680 310" },
          // --- Breadboard Wires & RF Links ---
          // Row 22 Clean UART (Hestia GP0 -> R1)
          line({
            class: "topo-wire",
            x1: 155, y1: 70, x2: 240, y2: 70,
            stroke: "#10b981", "stroke-width": selectedBus.val === "UART" ? 3.5 : 2.2,
            onclick: () => selectTopoBus("UART", "PDU")
          }),
          text({ x: 197, y: 58, fill: "#10b981", "font-size": 8, "font-family": "monospace", "text-anchor": "middle" }, "Row 22 Clean"),

          // Row 13 Glitched UART (R1 -> Daphne GP1)
          line({
            class: "topo-wire",
            x1: 315, y1: 70, x2: 420, y2: 70,
            stroke: glitchBus === "UART" ? "#f43f5e" : "#10b981",
            "stroke-width": (selectedBus.val === "UART" || glitchBus === "UART") ? 3.5 : 2.2,
            onclick: () => selectTopoBus("UART", "PDU")
          }),
          text({ x: 368, y: 58, fill: glitchBus === "UART" ? "#f43f5e" : "#94a3b8", "font-size": 8, "font-family": "monospace", "text-anchor": "middle" }, "Row 13 RX"),

          // Probe 1 Clean Tap (Row 22 -> whattheflip GPIO5)
          path({
            class: "topo-wire",
            d: "M 195 70 L 195 180",
            stroke: estop ? "#64748b" : "#10b981",
            "stroke-width": 1.8, "stroke-dasharray": probeDash, fill: "none",
            onclick: () => selectTopoBus("UART", "PDU")
          }),
          text({ x: 162, y: 142, fill: "#10b981", "font-size": 8, "font-family": "monospace" }, "GPIO5 (U1RXD)"),

          // Probe 0 Glitch Tap (whattheflip GPIO4 -> Row 13)
          path({
            class: "topo-wire",
            d: "M 235 180 L 235 125 L 355 125 L 355 70",
            stroke: estop ? "#64748b" : (glitchBus === "UART" ? "#f43f5e" : "#f59e0b"),
            "stroke-width": glitchBus === "UART" ? 3.2 : 1.8,
            "stroke-dasharray": probeDash, fill: "none",
            onclick: () => selectTopoBus("FAULTS", "PDU")
          }),
          text({ x: 295, y: 119, fill: "#f59e0b", "font-size": 8, "font-family": "monospace", "text-anchor": "middle" }, "GPIO4 (U1TXD 10µs)"),

          // I2C Bus Row 55 (SCL) & Row 58 (SDA) between Daphne & GY-511
          path({
            class: "topo-wire",
            d: "M 535 100 L 605 100 L 605 145",
            stroke: (glitchBus === "I2C_ACK" || glitchBus === "I2C_STRETCH") ? "#f43f5e" : "#06b6d4",
            "stroke-width": selectedBus.val === "I2C" ? 3.5 : 2.4, fill: "none",
            onclick: () => selectTopoBus("I2C", "SCOPE")
          }),
          text({ x: 602, y: 92, fill: "#06b6d4", "font-size": 8, "font-family": "monospace", "text-anchor": "end" }, "Row 55 SCL / Row 58 SDA (400kHz)"),

          // Probes 2 & 3 Tap (whattheflip GPIO6/7 -> I2C Bus)
          path({
            class: "topo-wire",
            d: "M 265 205 L 455 205 L 455 165 L 555 165",
            stroke: estop ? "#64748b" : "#06b6d4",
            "stroke-width": 1.8, "stroke-dasharray": probeDash, fill: "none",
            onclick: () => selectTopoBus("I2C", "SCOPE")
          }),
          text({ x: 365, y: 198, fill: "#06b6d4", "font-size": 8, "font-family": "monospace", "text-anchor": "middle" }, "GPIO6/7 (I2CEXT0 Sniff + 9th-Clk ACK)"),

          // 2.4 GHz RF Link (Iris <-> whattheflip)
          line({
            class: "topo-wire",
            x1: 265, y1: 255, x2: 485, y2: 255,
            stroke: "#a855f7", "stroke-width": 1.8, "stroke-dasharray": "5 4",
            onclick: () => selectTopoBus("WIFI", "RTOS")
          }),
          text({ x: 375, y: 247, fill: "#a855f7", "font-size": 8, "font-family": "monospace", "text-anchor": "middle" },
            `2.4GHz Wi-Fi Ch ${wifiActiveCh.val} + 802.15.4 Ch 15`),

          // --- Clickable Hardware Nodes ---
          // 1. Hestia Node
          g({ class: "topo-node", onclick: () => selectTopoNode("hestia", "UART") },
            rect({
              class: "node-box", x: 20, y: 35, width: 135, height: 72, rx: 3,
              fill: "#161d2b", stroke: selNode === "hestia" ? "#10b981" : "#334155", "stroke-width": 1.8
            }),
            text({ x: 30, y: 52, fill: "#f1f5f9", "font-size": 10, "font-weight": "bold", "font-family": "monospace" }, "HESTIA (RP2040)"),
            text({ x: 30, y: 66, fill: "#10b981", "font-size": 8.5, "font-family": "monospace" }, "Core 0: UART0 TX GP0"),
            text({ x: 30, y: 79, fill: "#94a3b8", "font-size": 8, "font-family": "monospace" }, "115200 8N1 | ~counter"),
            text({ x: 30, y: 93, fill: "#38bdf8", "font-size": 8, "font-family": "monospace" }, "LEDs GP2..GP9 Sweep")
          ),

          // 2. Resistor R1 (220 Ohm) Node
          g({ class: "topo-node", onclick: () => selectTopoBus("UART", "PDU") },
            rect({
              class: "node-box", x: 240, y: 52, width: 75, height: 36, rx: 3,
              fill: "#161d2b", stroke: "#f59e0b", "stroke-width": 1.6
            }),
            text({ x: 277, y: 68, fill: "#f59e0b", "font-size": 9.5, "font-weight": "bold", "font-family": "monospace", "text-anchor": "middle" }, "R1: 220 Ω"),
            text({ x: 277, y: 80, fill: "#94a3b8", "font-size": 7.5, "font-family": "monospace", "text-anchor": "middle" }, "B22-B13 <=15mA")
          ),

          // 3. Daphne Node
          g({ class: "topo-node", onclick: () => selectTopoNode("daphne", "I2C") },
            rect({
              class: "node-box", x: 420, y: 35, width: 145, height: 82, rx: 3,
              fill: "#161d2b", stroke: selNode === "daphne" ? "#06b6d4" : "#334155", "stroke-width": 1.8
            }),
            text({ x: 430, y: 52, fill: "#f1f5f9", "font-size": 10, "font-weight": "bold", "font-family": "monospace" }, "DAPHNE (RP2040 SMP)"),
            text({ x: 430, y: 66, fill: "#10b981", "font-size": 8, "font-family": "monospace" }, "Core 0: GP1 RX + BER"),
            text({ x: 430, y: 79, fill: "#06b6d4", "font-size": 8, "font-family": "monospace" }, "Core 1: I2C1 400kHz"),
            text({ x: 430, y: 92, fill: "#f59e0b", "font-size": 8, "font-family": "monospace" }, "9-Clk Bus Recovery"),
            text({ x: 430, y: 105, fill: "#38bdf8", "font-size": 8, "font-family": "monospace" }, "GP8..15 Tilt Spirit LED")
          ),

          // 4. GY-511 IMU Sensor Node
          g({ class: "topo-node", onclick: () => selectTopoBus("I2C", "SCOPE") },
            rect({
              class: "node-box", x: 555, y: 145, width: 112, height: 68, rx: 3,
              fill: "#161d2b", stroke: selNode === "gy511" ? "#06b6d4" : "#334155", "stroke-width": 1.8
            }),
            text({ x: 563, y: 161, fill: "#06b6d4", "font-size": 9.5, "font-weight": "bold", "font-family": "monospace" }, "GY-511 LSM303"),
            text({ x: 563, y: 175, fill: "#f1f5f9", "font-size": 8, "font-family": "monospace" }, `X:${accelX.val} Y:${accelY.val} mg`),
            text({ x: 563, y: 188, fill: "#10b981", "font-size": 8, "font-family": "monospace" }, `Z:${accelZ.val} mg (0x19)`),
            text({ x: 563, y: 201, fill: "#94a3b8", "font-size": 7.5, "font-family": "monospace" }, "Mag 0x1E | I55/I58")
          ),

          // 5. whattheflip Diagnostic Controller Node
          g({ class: "topo-node", onclick: () => selectTopoNode("whattheflip", "ALL") },
            rect({
              class: "node-box", x: 85, y: 180, width: 180, height: 102, rx: 3,
              fill: "#161d2b", stroke: selNode === "whattheflip" ? "#f1f5f9" : "#38bdf8", "stroke-width": 2
            }),
            text({ x: 95, y: 198, fill: "#f1f5f9", "font-size": 10.5, "font-weight": "bold", "font-family": "monospace" }, "WHATTHEFLIP (ESP32-C6)"),
            text({ x: 95, y: 213, fill: "#10b981", "font-size": 8, "font-family": "monospace" }, "UART1 Autobaud + TX/RX FIFO"),
            text({ x: 95, y: 226, fill: "#06b6d4", "font-size": 8, "font-family": "monospace" }, "I2CEXT0 Sniff + 9th-Clk Fuzz"),
            text({ x: 95, y: 239, fill: "#a855f7", "font-size": 8, "font-family": "monospace" }, `Wi-Fi Ch 1..13 + 15.4 Ch 15`),
            text({ x: 95, y: 252, fill: "#f59e0b", "font-size": 8, "font-family": "monospace" }, "SD FAT32 /WWW/ + 4KB PCAP"),
            text({ x: 95, y: 266, fill: estop ? "#f43f5e" : "#94a3b8", "font-size": 8, "font-family": "monospace" },
              estop ? "GPIO9 E-STOP: HIGH-Z ACTIVE" : "GPIO9 E-Stop Gate < 200 ns")
          ),

          // 6. Iris Wireless Victim Node
          g({ class: "topo-node", onclick: () => selectTopoNode("iris", "WIFI") },
            rect({
              class: "node-box", x: 485, y: 228, width: 175, height: 62, rx: 3,
              fill: "#161d2b", stroke: selNode === "iris" ? "#a855f7" : "#334155", "stroke-width": 1.8
            }),
            text({ x: 495, y: 245, fill: "#a855f7", "font-size": 10, "font-weight": "bold", "font-family": "monospace" }, "IRIS (ESP32-C6 RF)"),
            text({ x: 495, y: 259, fill: "#f1f5f9", "font-size": 8, "font-family": "monospace" }, "BSSID 02:00:00:00:00:03"),
            text({ x: 495, y: 272, fill: "#94a3b8", "font-size": 8, "font-family": "monospace" }, "SoftAP Ch 6 + 802.15.4 Ch 15")
          ),

          // --- Animated Clickable PDU Packet Envelopes ---
          // Clean UART PDU (Row 22)
          g({
            class: "pdu-pill",
            transform: `translate(${pduUartCleanX}, 61)`,
            onclick: () => { isPaused.val = true; activeInspTab.val = "PDU"; showToast("Inspected Clean Inbound UART PDU (Row 22)"); }
          },
            rect({ x: 0, y: 0, width: 28, height: 16, rx: 2, fill: "#052e16", stroke: "#10b981", "stroke-width": 1.2 }),
            text({ x: 14, y: 11, fill: "#10b981", "font-size": 7.5, "font-family": "monospace", "text-anchor": "middle" }, "PDU")
          ),

          // Post-Resistor Outbound UART PDU (Row 13)
          g({
            class: "pdu-pill",
            transform: `translate(${pduUartOutX}, 61)`,
            onclick: () => { isPaused.val = true; activeInspTab.val = "PDU"; showToast("Inspected Post-Resistor Outbound UART PDU (Row 13)"); }
          },
            rect({
              x: 0, y: 0, width: 32, height: 16, rx: 2,
              fill: glitchBus === "UART" ? "#4c0519" : "#052e16",
              stroke: glitchBus === "UART" ? "#f43f5e" : "#10b981", "stroke-width": 1.4
            }),
            text({
              x: 16, y: 11,
              fill: glitchBus === "UART" ? "#fda4af" : "#10b981",
              "font-size": 7.5, "font-family": "monospace", "text-anchor": "middle"
            }, glitchBus === "UART" ? "⚡ERR" : "8N1")
          ),

          // I2C Sensor PDU (Row 55/58)
          g({
            class: "pdu-pill",
            transform: `translate(${pduI2cX}, 92)`,
            onclick: () => { activeInspTab.val = "SCOPE"; showToast("Inspected GY-511 0xA8 I2C Burst"); }
          },
            rect({ x: 0, y: 0, width: 30, height: 15, rx: 2, fill: "#083344", stroke: "#06b6d4", "stroke-width": 1.2 }),
            text({ x: 15, y: 10.5, fill: "#06b6d4", "font-size": 7.5, "font-family": "monospace", "text-anchor": "middle" }, "0x19")
          ),

          // RF 802.11 / 802.15.4 PDU
          g({
            class: "pdu-pill",
            transform: `translate(${pduRfX}, 248)`,
            onclick: () => { selectTopoNode("iris", "WIFI"); }
          },
            rect({ x: 0, y: 0, width: 30, height: 15, rx: 2, fill: "#2e1065", stroke: "#a855f7", "stroke-width": 1.2 }),
            text({ x: 15, y: 10.5, fill: "#d8b4fe", "font-size": 7.5, "font-family": "monospace", "text-anchor": "middle" }, `Ch${wifiActiveCh.val}`)
          )
        );
      }
    )
  );
}

// ---------------------------------------------------------------------------
// Upper Right: Contextual 3-Tab Inspector
//   Tab 1 ("PDU"): Inbound (Row 22) vs Outbound (Row 13) Bit-Diff + WaveDrom Logic Scope
//   Tab 2 ("RTOS"): Selected Board µT-Kernel 3.0 Internals + Wi-Fi 1..13 Selector
//   Tab 3 ("SCOPE"): GY-511 0xA8 Register Dissector + Raw vs 8-Sample Avg Scope + 8-LED Tilt
// ---------------------------------------------------------------------------
function charToBits8N1(chCode) {
  let bits = [0]; // START bit (LOW)
  for (let b = 0; b < 8; b++) {
    bits.push((chCode >> b) & 1);
  }
  bits.push(1); // STOP bit (HIGH)
  return bits;
}

function PduBitDiffTab() {
  return () => {
    let p = selectedPacket.val;
    let inStr  = (p && p.inboundAscii)  ? p.inboundAscii  : "HESTIA:128:FF7F";
    let outStr = (p && p.outboundAscii) ? p.outboundAscii : (corruptedCount.val > 0 ? "HESTIA:128:F77F" : inStr);
    let sampleByteIn  = inStr.charCodeAt(Math.min(11, inStr.length - 1)) || 0x46;
    let isCorrupt     = (p && p.tagClass === "fault") || activeGlitchBus.val === "UART";
    let sampleByteOut = isCorrupt ? (sampleByteIn & ~0x08) : sampleByteIn;

    let bitsIn  = charToBits8N1(sampleByteIn);
    let bitsOut = charToBits8N1(sampleByteOut);

    // Build SVG points for 8.68 µs UART 8N1 bit-cell timing diagram
    let ptsClean = [];
    let ptsOut   = [];
    for (let i = 0; i < 10; i++) {
      let x0 = 35 + i * 32;
      let x1 = x0 + 32;
      let yIn  = bitsIn[i]  ? 14 : 34;
      let yOut = bitsOut[i] ? 52 : 72;
      ptsClean.push(`${x0},${yIn}`, `${x1},${yIn}`);
      ptsOut.push(`${x0},${yOut}`, `${x1},${yOut}`);
    }

    return div({ style: "display:flex;flex-direction:column;gap:7px" },
      div({ class: "insp-card" },
        div({ class: "insp-card-title" },
          span("PACKET TRACER PDU BIT-DIFF (BEFORE vs. AFTER R1 220 Ω)"),
          span({ style: isCorrupt ? "color:var(--state-glitch)" : "color:var(--proto-uart)" },
            isCorrupt ? "⚡ BIT FLIPPED (15.0 mA)" : "✓ 0 BIT ERRORS")
        ),
        div({ class: "diff-grid" },
          div({ class: "diff-box" },
            div({ style: "color:var(--proto-uart);font-weight:700" }, "INBOUND (Row 22 / Probe 1)"),
            div({ style: "margin-top:2px" }, inStr),
            div({ class: "bit-row" },
              bitsIn.map((bVal, idx) => span({ class: "bit-cell" },
                idx === 0 ? "S:0" : (idx === 9 ? "P:1" : `D${idx - 1}:${bVal}`)
              ))
            )
          ),
          div({ class: "diff-box" },
            div({ style: isCorrupt ? "color:var(--state-glitch);font-weight:700" : "color:var(--proto-uart);font-weight:700" },
              "OUTBOUND (Row 13 / Daphne GP1)"),
            div({ style: "margin-top:2px" }, outStr),
            div({ class: "bit-row" },
              bitsOut.map((bVal, idx) => {
                let diff = bVal !== bitsIn[idx];
                return span({ class: "bit-cell" + (diff ? " flipped" : "") },
                  idx === 0 ? "S:0" : (idx === 9 ? "P:1" : `D${idx - 1}:${bVal}`)
                );
              })
            )
          )
        )
      ),
      // Saleae / WaveDrom Style Digital Logic Bit-Cell Waveform
      div({ class: "insp-card" },
        div({ class: "insp-card-title" },
          span("DIGITAL LOGIC WAVEFORM // 115200 8N1 (8.68 µs/bit) + UART1 TX 10 µs GLITCH"),
          span({ class: "mono", style: "color:var(--state-warn)" }, "3.3V / 220Ω = 15.0 mA")
        ),
        svg({ viewBox: "0 0 370 84", style: "width:100%;height:84px;background:#090d14;border:1px solid #1e293b" },
          text({ x: 4, y: 26, fill: "#10b981", "font-size": 8, "font-family": "monospace" }, "Row22"),
          text({ x: 4, y: 64, fill: isCorrupt ? "#f43f5e" : "#06b6d4", "font-size": 8, "font-family": "monospace" }, "Row13"),
          // Glitch window highlight on bit D3
          isCorrupt ? rect({ x: 35 + 4 * 32, y: 6, width: 32, height: 72, fill: "rgba(244,63,94,0.22)", stroke: "#f43f5e", "stroke-dasharray": "2 2" }) : null,
          polyline({ points: ptsClean.join(" "), fill: "none", stroke: "#10b981", "stroke-width": 1.8 }),
          polyline({ points: ptsOut.join(" "), fill: "none", stroke: isCorrupt ? "#f43f5e" : "#06b6d4", "stroke-width": 1.8 }),
          text({ x: 51, y: 81, fill: "#64748b", "font-size": 7, "font-family": "monospace" }, "START(8.68µs)   D0    D1    D2   [D3:10µs GLITCH]   D4    D5    D6    D7   STOP")
        )
      )
    );
  };
}

function DeviceRtosTab() {
  const channels = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13];
  return () => {
    let node = selectedNode.val;
    return div({ style: "display:flex;flex-direction:column;gap:7px" },
      div({ class: "insp-card" },
        div({ class: "insp-card-title" },
          span(`DEVICE & µT-KERNEL 3.0 INTERNALS // ${node.toUpperCase()}`),
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
          span("SINGLE-RADIO WI-FI 6 CHANNEL HOPPER (CH 1..13) & LOCK SELECTOR"),
          span({ style: "color:var(--proto-rf)" },
            wifiChannelMode.val === 0 ? `SWEEPING (Active Ch ${wifiActiveCh.val})` : `LOCKED CH ${wifiActiveCh.val}`)
        ),
        div({ style: "display:flex;gap:6px;align-items:center;margin-bottom:4px" },
          button({
            class: () => "cmd-btn" + (wifiChannelMode.val === 0 ? " active" : ""),
            onclick: () => selectWifiChannel(0)
          }, "🔄 Sweep All Channels (1–13)"),
          span({ class: "mono", style: "font-size:9px;color:var(--fg-muted)" }, "50ms off-channel probe / 250ms HTTP Ch6 hold")
        ),
        div({ class: "ch-grid" },
          channels.map(ch => button({
            class: () => "btn-ch" + (wifiChannelMode.val === ch ? " active" : ""),
            onclick: () => selectWifiChannel(ch)
          }, `Ch ${ch}`))
        )
      )
    );
  };
}

function SensorScopeTab() {
  return () => {
    let rawHist = scopeRawXHist.val;
    let avgHist = scopeAvgXHist.val;

    let rawPts = rawHist.map((v, i) => {
      let x = 10 + (i / Math.max(1, rawHist.length - 1)) * 345;
      let y = 42 - Math.max(-36, Math.min(36, v / 25));
      return `${x.toFixed(1)},${y.toFixed(1)}`;
    }).join(" ");

    let avgPts = avgHist.map((v, i) => {
      let x = 10 + (i / Math.max(1, avgHist.length - 1)) * 345;
      let y = 42 - Math.max(-36, Math.min(36, v / 25));
      return `${x.toFixed(1)},${y.toFixed(1)}`;
    }).join(" ");

    let tiltSlot = Math.max(0, Math.min(7, Math.floor((accelAvgX.val + 1000) / 250)));
    let ledPins  = [8, 9, 10, 11, 12, 13, 14, 15];

    return div({ style: "display:flex;flex-direction:column;gap:7px" },
      div({ class: "insp-card" },
        div({ class: "insp-card-title" },
          span("GY-511 (0x19 ACCEL / 0x1E MAG) // RAW I2C vs. 8-SAMPLE MOVING AVERAGE"),
          span({ class: "mono", style: "color:var(--proto-i2c)" }, `0xA8: [${rawRegHex.val}]`)
        ),
        svg({ viewBox: "0 0 365 84", style: "width:100%;height:84px;background:#090d14;border:1px solid #1e293b" },
          line({ x1: 10, y1: 42, x2: 355, y2: 42, stroke: "#1e293b", "stroke-dasharray": "3 3" }),
          polyline({ points: rawPts, fill: "none", stroke: "#06b6d4", "stroke-width": 1.4 }),
          polyline({ points: avgPts, fill: "none", stroke: "#10b981", "stroke-width": 2.2 }),
          text({ x: 14, y: 14, fill: "#06b6d4", "font-size": 8, "font-family": "monospace" },
            `Raw X: ${accelX.val} mg | Y: ${accelY.val} mg | Z: ${accelZ.val} mg`),
          text({ x: 14, y: 76, fill: "#10b981", "font-size": 8, "font-family": "monospace" },
            `8-Sample Avg X: ${accelAvgX.val} mg | Mag: ${magStr.val}`)
        )
      ),
      div({ class: "insp-card" },
        div({ class: "insp-card-title" },
          span("DAPHNE PHYSICAL MAKER PI BASE LED SPIRIT LEVEL (GP8..GP15)"),
          span({ class: "mono", style: "color:#38bdf8" }, `Active LED: GP${8 + tiltSlot}`)
        ),
        div({ class: "led-strip" },
          ledPins.map((pin, idx) => div({
            class: "led-dot" + (idx === tiltSlot ? " lit-blue" : "")
          }, `GP${pin}`))
        )
      )
    );
  };
}

function ContextualInspectorPane() {
  return div({ class: "inspector-pane" },
    div({ class: "inspector-tabs" },
      button({
        class: () => "insp-tab" + (activeInspTab.val === "PDU" ? " active" : ""),
        onclick: () => activeInspTab.val = "PDU"
      }, "1. PDU BIT-DIFF & LOGIC SCOPE"),
      button({
        class: () => "insp-tab" + (activeInspTab.val === "RTOS" ? " active" : ""),
        onclick: () => activeInspTab.val = "RTOS"
      }, "2. DEVICE & RF CHANNELS"),
      button({
        class: () => "insp-tab" + (activeInspTab.val === "SCOPE" ? " active" : ""),
        onclick: () => activeInspTab.val = "SCOPE"
      }, "3. GY-511 I2C & DSP SCOPE")
    ),
    div({ class: "inspector-body" },
      () => {
        if (activeInspTab.val === "PDU") return PduBitDiffTab();
        if (activeInspTab.val === "RTOS") return DeviceRtosTab();
        return SensorScopeTab();
      }
    )
  );
}

// ---------------------------------------------------------------------------
// Bottom Pane: Client-Side Wireshark Dissector, Sorter & Hex Inspector
// ---------------------------------------------------------------------------
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
    { id: "FAULTS", label: "⚡ Glitches / Faults" }
  ];

  return div({ class: "bottom-wireshark" },
    div({ class: "ws-toolbar" },
      span({ class: "mono", style: "font-size:10px;font-weight:700;color:var(--proto-i2c)" }, "WIRESHARK FILTER:"),
      quickFilters.map(qf => button({
        class: () => "cmd-btn" + (filterProtocol.val === qf.id ? " active" : ""),
        onclick: () => filterProtocol.val = qf.id
      }, qf.label)),
      input({
        class: "ws-input",
        type: "text",
        placeholder: "Client-side expression (e.g. lsm303, 0xa8, iris, ch 6, fail, hestia)...",
        value: () => filterSearchText.val,
        oninput: (e) => filterSearchText.val = e.target.value
      }),
      button({
        class: "cmd-btn",
        onclick: () => { filterProtocol.val = "ALL"; filterSearchText.val = ""; }
      }, "Clear"),
      () => span({ class: "mono", style: "font-size:10px;color:var(--fg-muted)" },
        `${packets.val.length} frames (${pcapChunkCount.val} SD chunks)`)
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
              th({ onclick: () => toggleSort("summary") }, () => "CLIENT-JS DECODED SUMMARY & CHECKSUM" + sortIndicator("summary"))
            )
          ),
          () => {
            let protoFilter = filterProtocol.val;
            let search      = filterSearchText.val.toLowerCase().trim();
            let col         = sortColumn.val;
            let asc         = sortAscending.val;

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
              class: () => (selectedPacket.val === p ? "selected " : "") + (p.tagClass === "fault" ? "row-fault" : ""),
              onclick: () => {
                selectedPacket.val = p;
                if (p.proto === "UART" || p.proto === "FAULT") activeInspTab.val = "PDU";
                else if (p.proto === "I2C") activeInspTab.val = "SCOPE";
              }
            },
              td(p.id || "-"),
              td(p.time),
              td(span({ class: `badge ${p.tagClass}` }, p.proto)),
              td(p.chan),
              td(p.len != null ? `${p.len}B` : "-"),
              td({ class: "col-summary" }, p.summary)
            )));
          }
        )
      ),
      // Right 38%: Canonical 16-Byte Aligned Hex + ASCII Dissector
      div({ class: "ws-hex-pane" },
        () => {
          let p = selectedPacket.val;
          if (!p) {
            return div({ style: "color:var(--fg-muted)" }, "Select any packet row on the left or click a PDU on the topology canvas.");
          }
          return div(
            div({ style: "display:flex;justify-content:space-between;color:var(--fg-muted);border-bottom:1px solid var(--border-subtle);padding-bottom:3px" },
              span("FRAME ", b({ style: "color:var(--fg-primary)" }, `#${p.id}`), ` (${p.proto} // ${p.chan})`),
              span("LEN: ", b({ style: "color:var(--proto-i2c)" }, `${p.len} B`), " @ ", b(p.time))
            ),
            pre({ class: "hex-dump-pre" }, formatHex(p.raw || p.summary))
          );
        }
      )
    )
  );
}

function ToastOverlay() {
  return div({ class: "toast-container" },
    () => toastMsg.val
      ? div({ class: "toast" },
          span(toastMsg.val),
          span({ style: "cursor:pointer;opacity:0.7", onclick: () => toastMsg.val = "" }, "✕")
        )
      : null
  );
}

// ---------------------------------------------------------------------------
// Live WebSocket + Chunked SD PCAP Streamer
// ---------------------------------------------------------------------------
function pushAlertRecord(time, proto, chan, len, summary, raw, tagClass) {
  clientPacketSeq++;
  let rec = {
    id: clientPacketSeq,
    tsUs: Date.now() * 1000,
    time, proto, chan, len, summary, raw, tagClass,
    inboundAscii: "HESTIA:128:FF7F",
    outboundAscii: "HESTIA:128:F77F",
    flippedBitIdx: 3
  };
  packets.val = [rec, ...packets.val.slice(0, 499)];
  selectedPacket.val = rec;
}

function updateFromTelemetry(d) {
  if (d.safety) safetyState.val = d.safety;
  if (d.uart_pkts != null) uartPkts.val = d.uart_pkts;
  if (d.i2c_pkts != null) i2cPkts.val = d.i2c_pkts;
  if (d.wifi_pkts != null) wifiPkts.val = d.wifi_pkts;
  if (d.ieee154_pkts != null) zigbeePkts.val = d.ieee154_pkts;
  if (d.corrupted_frames != null) {
    corruptedCount.val = Math.max(corruptedCount.val, d.corrupted_frames);
  }
  if (d.wifi_ch != null) wifiActiveCh.val = d.wifi_ch;
  if (d.wifi_mode != null) wifiChannelMode.val = d.wifi_mode;

  if (d.pcap_bytes != null) {
    pcapSize.val = d.pcap_bytes;
    if (d.pcap_bytes > s_pcapStreamOffset) {
      fetchNextPcapChunk(false);
    }
  }
  recomputeClientBer();

  if (d.accel) {
    updateAccelHistory(
      d.accel.x != null ? d.accel.x : accelX.val,
      d.accel.y != null ? d.accel.y : accelY.val,
      d.accel.z != null ? d.accel.z : accelZ.val
    );
  }
  if (d.mag) {
    magStr.val = `X:${d.mag.x} Y:${d.mag.y} Z:${d.mag.z}`;
  }
}

function connectDataStream() {
  fetchNextPcapChunk(false);

  // Animate packet envelopes smoothly along breadboard wires when not frozen
  setInterval(() => {
    if (!isPaused.val && safetyState.val !== "ABORTED") {
      animPhase.val = (animPhase.val + 4) % 100;
    }
  }, 60);

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

function App() {
  return div({ class: "workbench-root" },
    ToastOverlay(),
    TopCommandBar(),
    div({ class: "upper-workbench" },
      TopologyCanvas(),
      ContextualInspectorPane()
    ),
    BottomWiresharkPane()
  );
}

van.add(document.getElementById("app"), App());
connectDataStream();
