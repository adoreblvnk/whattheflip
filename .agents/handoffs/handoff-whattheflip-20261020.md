# Handoff: whattheflip (INF2004 SE06) — 2026-10-20

Working dir: /Users/adoreblvnk/Documents/school/INF2004/whattheflip
Read first: /Users/adoreblvnk/Documents/school/INF2004/AGENTS.md (prof profiles + "How the human wants work done") and whattheflip/AGENTS.md (canon names, layout, tone). Follow them strictly.

## Where things stand
- `whattheflip.md` (living source of truth): fully reconciled against ground truth. Boards renamed: whattheflip (tool), hestia = old victim A (uart tx only), daphne = old victim B (uart rx + gy-511 i2c master 400 kHz, sensor wired to daphne exclusively), iris = wireless victim (softap ch 6, bssid 02:00:00:00:00:03, 802.15.4 ch 15). Requirement IDs normalized to FR-1..FR-10 (house style: hyphenated, matching frozen tribunal docs).
- Key late corrections in the doc: probes are GPIO4-7 (annotated pinout `reports/diagrams/ESP32_pinout.png` is the bench truth; older docs said GPIO6,7,14,15 = wrong); strapping pins among our GPIOs are GPIO4, GPIO5, GPIO9; SD card = GPIO18/19/20 + EXIO7 CS; named kernel objects (MPF_PACKETS, MBF_STORAGE 16, MBF_TELEMETRY 8, MTX_PROBE_ACCESS, FLG_SAFETY_ABORT, FLG_PROTO_LOCKED); promiscuous callback < 5 µs; victim core bindings under TA_ASSPRC; radio plan (both APs ch 6, sniffer windows 1-2 s).
- `reports/tribunal-1/SE06_PostTri1.md` = tribunal 1 version C: audit of Pre + ImmTri against third_party clones (esp-idf 5.3 present, mtkernel_3, mtk3smp-rp2040, esp32c6-mtk3, esp32c6-sniffer, pico-sdk). Top findings: B-2 C6 RMT = 4 channels but only 2 RX-capable (rmt_ll.h:32-35, sdkconfig:188-190) → 4-probe RMT premise invalid, gap A8 (resolve week 10); B-1 `tk_tget_mpf` does not exist → use `tk_get_mpf(..., TMO_POL)` (syscall.h:788); B-6 esp_wifi_80211_tx officially supports only beacon/probe/action/non-QoS data → deauth TX unsupported, A6 escalated; B-5 knl_kernel_lock_suspend/resume are internal-API; B-3 NimBLE "45 KB" retracted; B-4 SD GPIO10/11/12/15 row rejected. BKL numbers 85/36/43 µs, PORT_ESP32C6.md line 128, TK_SUPPORT_SMP line 80 all verified verbatim.
- Diagrams: reports/diagrams/*.puml (hardware_interconnect, main_flow, discovery_flow, fuzzer_flow, sniffer_flow) regenerated with new names, no GPIO numbers inside. Render with `plantuml f.puml` from that dir. setup.sh clones repos into third_party/ (gitignored); all 9 including esp-idf are present now.
- PDF: `whattheflip.pdf` rebuilt via `npx md-to-pdf --config-file ~/.agents/skills/md-to-pdf/references/config.js whattheflip.md`. Last build 23:35.

## Open items (small)
1. Bench check: buzzer is EXIO2 (per pinout image annotation) vs EXIO3 (docs) — verify, fix one line.
2. Bench check: confirm SD pin wiring 18/19/20 + EXIO7 on the physical board.
3. Git housekeeping: old diagram deletions staged, everything new untracked; commit today's baseline (user's call).
4. A8: pick capture path for probes beyond the 2 RMT RX channels (week 10 check-in).
5. Design-review PDF title says "SE06_Tri1C.md was renamed SE06_PostTri1.md" — no such reference exists inside PostTri1 md itself; nothing pending there.

## Next moves (user's stated plan)
- Tribunal 1 version C (PostTri1) is the scored deliverable, not code yet. Current tribunal grade sits at B+ ceiling; A+ requires the reconciliation + further rigor.
- Later: firmware skeletons per folder (whattheflip/src tasks, hestia tx burst first), then week 13 tribunal.
- If user asks to update frozen docs: they are frozen; changes go into new versioned files only.

## Suggested skills
- writing-for-agents (any doc writing/editing here)
- karpathy-guidelines (when code starts)
- md-to-pdf (rebuild whattheflip.pdf)
- handoff (this file regenerates on session end)
