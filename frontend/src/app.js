import van from "vanjs-core";

const { div, span, button, a, table, thead, tbody, tr, th, td, pre, b, select, option, input } = van.tags;

// Dynamic Hardware State
const safetyState = van.state("ARMED");
const toastMsg = van.state("");

// 4 Scenarios in exact specified order: UART -> I2C -> Wi-Fi -> Zigbee
const activeScenario = van.state("UART");

// Wireshark Filter State (at Live Bus Event Stream)
const filterProtocol = van.state("ALL");
const filterSearchText = van.state("");

const uartPkts = van.state(0);
const i2cPkts = van.state(0);
const wifiPkts = van.state(0);
const zigbeePkts = van.state(0);
const irisTx = van.state(0);
const berPct = van.state("0.000%");
const corruptedCount = van.state(0);
const pcapSize = van.state(5004);

const accelX = van.state(0);
const accelY = van.state(0);
const accelZ = van.state(980);
const magStr = van.state("X:-350 Y:-210 Z:135");

const selectedPacket = van.state(null);
const packets = van.state([]);

// Wireshark-style Canonical Hex + ASCII Formatter with 100% Column Alignment
function formatHex(dataStr) {
  if (!dataStr) return "Select a packet in the table above to inspect raw bytes.";
  let bytes = [];
  if (typeof dataStr === "string") {
    for (let i = 0; i < dataStr.length; i++) {
      bytes.push(dataStr.charCodeAt(i));
    }
  } else if (Array.isArray(dataStr)) {
    bytes = dataStr;
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
      span({ class: "brand-sub" }, "Security Diagnostic Bench // µT-Kernel 3.0")
    ),
    div({ class: "header-status-group" },
      // Dedicated Baseline Reset in header
      button({
        class: "btn-baseline-nav",
        onclick: () => {
          triggerCmd("R", "Reset to clean baseline. Probes armed.");
          safetyState.val = "ARMED";
          filterProtocol.val = "ALL";
          filterSearchText.val = "";
        }
      }, "Reset to Baseline"),
      // Dedicated E-Stop / Re-Arm button in header
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
          filterProtocol.val = s.id; // Automatically sync Wireshark filter dropdown
          showToast(`Switched to ${s.label} scenario`);
        }
      }, s.label))
    ),
    () => {
      let sc = activeScenario.val;
      if (sc === "UART") {
        return div({ style: "display:flex;flex-direction:column;gap:8px" },
          div({ class: "scenario-desc-text" }, "Eavesdropping Hestia GP0 TX on Row 10 (115200 8N1). Series 220 Ω resistor protects line on Row 12."),
          button({
            class: "btn-scenario-action",
            onclick: () => triggerCmd("U", "Injected 10 µs UART bit-flip -> Daphne detected checksum error")
          },
            span("⚡ Inject 10 µs Bit-Flip"),
            span({ class: "tag" }, "Probe 0 // Row 12")
          )
        );
      } else if (sc === "I2C") {
        return div({ style: "display:flex;flex-direction:column;gap:8px" },
          div({ class: "scenario-desc-text" }, "Monitoring Daphne master transactions on GY-511 sensor at 400 kHz Fast-Mode."),
          button({
            class: "btn-scenario-action",
            onclick: () => triggerCmd("A", "Injected 50 µs ACK-kill -> Daphne triggered 9-clock recovery")
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
        return div({ style: "display:flex;flex-direction:column;gap:8px" },
          div({ class: "scenario-desc-text" }, "Promiscuous Wi-Fi 6 capture across Channels 1–13. Targeting Iris SoftAP (02:00:00:00:00:03)."),
          button({
            class: "btn-scenario-action",
            onclick: () => triggerCmd("W", "Hopped to capture window on Channel 6")
          },
            span("📡 Capture Channel 6 Window"),
            span({ class: "tag" }, "Iris BSSID")
          )
        );
      } else {
        return div({ style: "display:flex;flex-direction:column;gap:8px" },
          div({ class: "scenario-desc-text" }, "IEEE 802.15.4 frame receiver active on Channel 15. Targets Iris beacon node."),
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
    div({ class: "card-title" }, "Traffic & Bus Health"),
    div({ class: "stat-box" },
      () => div({ class: "stat-num" }, berPct.val),
      () => div({ class: "stat-sub" }, `${corruptedCount.val} corrupted frames recorded`)
    ),
    table({ class: "data-table" },
      tr(
        td({ class: "label" }, "UART"),
        td({ class: "val" }, () => uartPkts.val)
      ),
      tr(
        td({ class: "label" }, "I2C"),
        td({ class: "val" }, () => i2cPkts.val)
      ),
      tr(
        td({ class: "label" }, "Wi-Fi"),
        td({ class: "val" }, () => wifiPkts.val)
      ),
      tr(
        td({ class: "label" }, "Zigbee"),
        td({ class: "val" }, () => `${zigbeePkts.val} (Iris: ${irisTx.val})`)
      ),
      tr(
        td({ class: "label" }, "PCAP File"),
        td({ class: "val" }, () => `${pcapSize.val} B`)
      )
    )
  );
}

// Daphne Motion Sensor Card
function SensorCard() {
  return div({ class: "card" },
    div({ class: "card-title" }, "Daphne Motion Sensor"),
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

// Wireshark-Style Packet Stream Panel with Display Filter Toolbar
function StreamPanel() {
  return div({ class: "card", style: "display:flex;flex-direction:column;min-width:0" },
    div({ class: "card-title" },
      span("Live Packet Analyzer"),
      () => span({ style: "color:var(--text-dim);font-family:var(--font-mono);font-size:10px" },
        `${packets.val.length} CAPTURED`
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
        option({ value: "WIFI" }, "Wi-Fi 6 (802.11)"),
        option({ value: "ZIGBEE" }, "Zigbee (802.15.4)"),
        option({ value: "FAULTS" }, "Faults & Corruptions")
      ),
      input({
        class: "ws-filter-input",
        type: "text",
        placeholder: "Apply display filter... (e.g. daphne, iris, corrupt, pkt)",
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
    // Stream Table
    div({ class: "stream-box" },
      table({ class: "stream-table" },
        thead(
          tr(
            th("TIME"),
            th("BUS"),
            th("SOURCE"),
            th("BYTES"),
            th("DATA / SUMMARY")
          )
        ),
        () => {
          let protoFilter = filterProtocol.val;
          let search = filterSearchText.val.toLowerCase().trim();

          let list = packets.val.filter(p => {
            // Protocol match
            if (protoFilter !== "ALL") {
              if (protoFilter === "UART" && p.proto !== "UART") return false;
              if (protoFilter === "I2C" && p.proto !== "I2C") return false;
              if (protoFilter === "WIFI" && p.proto !== "WIFI") return false;
              if (protoFilter === "ZIGBEE" && p.proto !== "ZIGBEE") return false;
              if (protoFilter === "FAULTS" && p.proto !== "FAULT") return false;
            }
            // Text search match
            if (search) {
              let text = (p.time + " " + p.proto + " " + p.chan + " " + p.summary).toLowerCase();
              if (!text.includes(search)) return false;
            }
            return true;
          });

          return tbody(list.slice(0, 100).map(p => tr({
            class: () => (selectedPacket.val === p ? "selected" : ""),
            onclick: () => selectedPacket.val = p
          },
            td(p.time),
            td(span({ class: `tag-badge ${p.tagClass}` }, p.proto)),
            td(p.chan),
            td(p.len ? `${p.len}` : "-"),
            td({ class: "summary-col" }, p.summary)
          )));
        }
      )
    ),
    // Wireshark-Style Packet Bytes & Inspection
    div({ class: "inspector-card" },
      div({ class: "card-title", style: "margin-bottom:6px" }, "Packet Bytes (Hex & ASCII)"),
      () => {
        let p = selectedPacket.val;
        if (!p) {
          return div({ class: "inspector-box" }, "Select a packet in the table above to view raw bytes.");
        }
        return div(
          div({ class: "inspector-meta" },
            span("Time: ", b(p.time)),
            span("Protocol: ", b(p.proto)),
            span("Source/Node: ", b(p.chan)),
            span("Length: ", b(p.len ? `${p.len} bytes` : "-"))
          ),
          pre({ class: "inspector-box" }, formatHex(p.raw || p.summary))
        );
      }
    )
  );
}

// Ingestion processor
function pushRecord(time, proto, chan, len, summary, raw, tagClass) {
  let rec = { time, proto, chan, len, summary, raw, tagClass };
  packets.val = [rec, ...packets.val.slice(0, 150)];
  if (!selectedPacket.val) selectedPacket.val = rec;
}

// Data Stream Connection
function connectDataStream() {
  let wsProtocol = window.location.protocol === "https:" ? "wss:" : "ws:";
  let wsUrl = `${wsProtocol}//${window.location.host}/ws`;
  
  let ws = null;
  try {
    ws = new WebSocket(wsUrl);
  } catch(e) {}

  if (ws) {
    ws.onmessage = (evt) => {
      try {
        let d = JSON.parse(evt.data);
        if (d.type === "telemetry") {
          updateFromTelemetry(d);
        } else if (d.type === "alert") {
          showToast(d.msg || "ALERT");
          pushRecord(d.time || "NOW", "FAULT", "SYS", 0, d.msg, d.msg, "fault");
        }
      } catch(e) {}
    };
    ws.onerror = () => wsFallbackPoll();
    ws.onclose = () => wsFallbackPoll();
  } else {
    wsFallbackPoll();
  }
}

let fallbackActive = false;
function wsFallbackPoll() {
  if (fallbackActive) return;
  fallbackActive = true;
  
  setInterval(() => {
    fetch("/telemetry")
      .then(r => r.json())
      .then(updateFromTelemetry)
      .catch(() => {});
  }, 400);

  setInterval(() => {
    fetch("/logs")
      .then(r => r.json())
      .then(logs => {
        if (Array.isArray(logs) && logs.length > 0) {
          let latest = logs[logs.length - 1];
          let proto = "LOG";
          let tagClass = "uart";
          if (latest.src === "WTF") { proto = "SYS"; tagClass = "wifi"; }
          if (latest.src === "DAPHNE") { proto = "I2C"; tagClass = "i2c"; }
          if (latest.src === "HESTIA") { proto = "UART"; tagClass = "uart"; }
          if (latest.src === "IRIS" || latest.text.includes("802.15.4")) { proto = "ZIGBEE"; tagClass = "zigbee"; }
          if (latest.text.includes("802.11")) { proto = "WIFI"; tagClass = "wifi"; }
          if (latest.text.includes("INJECT") || latest.text.includes("FAULT") || latest.text.includes("CORRUPT")) {
            proto = "FAULT";
            tagClass = "fault";
          }
          
          if (!packets.val.some(p => p.summary === latest.text)) {
            pushRecord(latest.time, proto, latest.src, 0, latest.text, latest.text, tagClass);
          }
        }
      })
      .catch(() => {});
  }, 400);
}

function updateFromTelemetry(d) {
  if (d.safety) safetyState.val = d.safety;
  if (d.uart_pkts != null) uartPkts.val = d.uart_pkts;
  if (d.i2c_pkts != null) i2cPkts.val = d.i2c_pkts;
  if (d.wifi_pkts != null) wifiPkts.val = d.wifi_pkts;
  if (d.ieee154_pkts != null) zigbeePkts.val = d.ieee154_pkts;
  if (d.iris_tx != null) irisTx.val = d.iris_tx;
  if (d.corrupted_frames != null) corruptedCount.val = d.corrupted_frames;
  if (d.pcap_bytes) pcapSize.val = d.pcap_bytes;
  if (d.ber_percent) berPct.val = d.ber_percent;
  
  if (d.accel) {
    accelX.val = d.accel.x || 0;
    accelY.val = d.accel.y || 0;
    accelZ.val = d.accel.z || 980;
  }
  if (d.mag) {
    magStr.val = `X:${d.mag.x} Y:${d.mag.y} Z:${d.mag.z}`;
  }
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
