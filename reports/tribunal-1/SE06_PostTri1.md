# Post-Tribunal 1 (version C). Ground truth reconciliation

Audit of version A (`SE06_PreTri1.md`) and version B (`SE06_ImmTri1.md`) against the actual SDKs in `third_party/`: `esp-idf` v5.3, `mtkernel_3`, `mtk3smp-rp2040`, `esp32c6-mtk3`, `esp32c6-sniffer`, `pico-sdk`, `pico-examples`. Every verdict cites file and line. The frozen version A and B documents are records of what was submitted; this file supersedes them.

## Errors already known before this audit

- Daphne as I2C slave (version A) is infeasible under the port: `i2c_rp2040.c` programs master mode exclusively (slave registers exist as dead defines). Version B already fixed this by moving the GY-511 to daphne as master.
- Probe pin remap "GPIO6, GPIO7, GPIO14, GPIO15" contradicts the board's own annotated pinout (`reports/diagrams/ESP32_pinout.png`): header GP4 to GP7 is GPIO4 to GPIO7. Corrected in `whattheflip.md`.
- The microSD SPI pin row (GPIO10/11/12/15) has no source on any cloned board and GPIO10-13 are not on the Pico header. Rejected with this audit (below).

## Errors found in version B by this audit

| # | Claim in version B | Ground truth | Impact |
|---|---|---|---|
| B-1 | `tk_tget_mpf(..., TMO_POL)` polls the packet pool | The syscall is `tk_get_mpf(ID, void**, TMO)` (`mtkernel_3/include/tk/syscall.h:788`); `tk_tget_mpf` exists nowhere. `TMO_POL` itself is real (`typedef.h:125`) | Firmware uses `tk_get_mpf(..., TMO_POL)`. Fixed in `whattheflip.md` |
| B-2 | "ESP32-C6 RMT peripheral has 4 RX channels" | C6: 1 RMT group, 4 channels, only 2 RX-capable (`esp-idf/components/hal/esp32c6/include/hal/rmt_ll.h:32-35`; `esp32c6-sniffer/sdkconfig:188-190`) | The 4-probe simultaneous RMT RX premise fails. Tracked as gap A8 in `whattheflip.md` |
| B-3 | NimBLE eats ~45 KB SRAM (BLE cut rationale) | No such figure anywhere in IDF; documented numbers are ~1 KB DRAM per connection (`components/bt/host/nimble/Kconfig.in:384`) | BLE cut still stands on its confirmed grounds: no promiscuous API (zero "promiscuous" hits under `components/bt`), hopping data channels not interceptable. The 45 KB figure is retracted |
| B-4 | microSD on GPIO10/11/12/15 | C6 has no SDMMC host (SDIO slave only, `soc_caps.h:68`); IDF SDSPI defaults for this class are MOSI 4, MISO 6, CLK 5, CS 1 (`examples/storage/sd_card/sdspi/Kconfig.projbuild:23-49`); the tribunal's pin set matches no cloned evidence | Board truth is the annotated pinout: GPIO18/19/20 + EXIO7 (pending one bench check) |
| B-5 | Filter task calls `knl_kernel_lock_suspend()/resume()` to escape the BKL | Both functions exist (`port/kernel/tkernel/smp_lock.c:227,252`) but are declared **internal** (`kernel/knlinc/smp_lock.h:16`); the kernel itself only detaches its cyclic handlers (time_calls.c:281-286) | Do not call internal-API functions from app tasks. Either keep the filter inside the BKL, or find a supported pattern; the port-bug hunt Prof. Fauzi rewards lives here |
| B-6 | A6 "esp_wifi_80211_tx may filter our deauth" | An understatement: the documented supported TX set is "beacon/probe request/probe response/action and non-QoS data frame"; encrypted or QoS frames are refused (`components/esp_wifi/include/esp_wifi.h:1252-1267`, `docs/.../wifi-vendor-features.rst:17-28`). Deauth is not in the supported list | Escalate A6: deauth TX is officially unsupported; treat driver filtering as the expected case and the 802.15.4 fallback as the plan |
| B-7 | Fuzzer pulses via "hardware ETM / RMT TX" | ETM exists and can reach both GPIO set/clear/toggle tasks and `RMT_TASK_TX_START` (`soc_etm_source.h:173-196, 270-272`), but `esp_driver_rmt` ships no ETM helper; wiring is manual via `esp_etm` | Feasible, at the cost of raw ETM registration in the fuzzer path |
| B-8 | `SMP=1` at `pico_rp2040.mk` line 8 | It is at line 13 (comment doc); the operative switch is `SMP ?= 0` at line 19 | Citation fix only |

## Verified correct in versions A and B

- BKL as recursive, masking local interrupts, maximum hold 85 µs, maximum wait 36 µs, local dispatch 43 µs (`PORT_RP2040.md:45,54,62`), `TK_SUPPORT_SMP 1` at `sysdef.h:80` exact, and `PORT_ESP32C6.md` line 128 "makes no attempt at SMP" exact. The quotes the tribunals built on are verbatim from the ports.
- `TA_ASSPRC` with `assprc = TP_PRC1` / `TP_PRC2` is real and is a port extension (`port/include/tk/syscall.h:51-55`, `task_manage.c:91-96`).
- Drivers as single-owner resources is a verbatim Safety Rule (`PORT_RP2040.md:149`); no FAT32/SD driver exists in the RP2040 port, supporting the decision to keep storage on the C6.
- `GPIO.enable_w1tc` one-write clear of output-enable is the real silicon register (`esp-idf/components/soc/esp32c6/register/soc/gpio_struct.h:148-159`).
- RMT channel RAM of 48 symbols; the cut table's "4.8 µs at 10 MHz" overflow arithmetic is exact (`soc_caps.h:290`, `sdkconfig:191`).
- Pico wiring: UART0 default TX=GP0/RX=GP1 (`pico.h:23-31`); I2C0 default SDA=GP4/SCL=GP5 (`pico.h:40-48`); the same pair carries the MMA8451 example, which is the GY-511; I2C slave mode exists in Pico SDK with an example on GP4/GP5, confirming the victim were able to run slave-era designs had the port supported it.
- The FreeRTOS translation shim is real and validated (97 checks pass, 0 fail, `PORT_ESP32C6.md:69`); it wraps `malloc`/`calloc`/`free`, which coexists with the no-malloc invariant. Its history includes a fixed stack over-allocation bug that wasted ~76 KB (`shim_task.c:88-91`), material for the port-bug bonus.
- RF: single front-end, time-division via the coexistence module; sniffer RX plus Zigbee scan is "supported but unstable" (C1), SoftAP vs 802.15.4 router is not supported (`docs/en/api-guides/coexist.rst:76-98,129-140`). The 1-2 s capture window design is consistent with this.
- Coexistence: ch 6 SoftAP, ch 15 = 2425 MHz, all consistent with the official docs.

## What this changes going forward

1. All code uses `tk_get_mpf` with `TMO_POL`; grep for `tk_tget_mpf` in future reviews must return nothing.
2. Gap A8 blocks discovery firmware until Tan picks the capture path for probes 3 and 4 or the time-share duty cycle. FR-1 stays a "what" requirement; the "how" is now a design decision, decided by week 10.
3. A6 is escalated as recorded in `whattheflip.md`.
4. The buzzer EXIO2-or-EXIO3 bench check remains the one open hardware check.

## Scorekeeping note

Version C's contribution is that every surviving claim is now traceable to a file and line in a cloned SDK, and every retraction has its replacement written down. That is the standard the next tribunal document gets held to.
