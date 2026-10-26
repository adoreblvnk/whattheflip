# writing rules for this repo

for anything here that writes: docs, comments, commit messages. wording follows the root rules; this file holds only what is specific to whattheflip.

## names

use only `whattheflip`, `hestia`, `daphne`, `iris`. a bracket names a chip only when the chip is the point. hestia transmits uart only; daphne receives it and reads the GY-511; iris is the wireless board.

## layout

- folders are `whattheflip/`, `victim-a/`, `victim-b/`, `wireless-victim/`, `shared/`.
- puml sources in `reports/diagrams`, rendered with `plantuml`; one diagram, one idea.
- engineering numbers stay exact.
- tables only where rows repeat.


## hardware pinouts (Maker Pi Base socket)

ESP32-C6 pins do not map 1-to-1 to Maker Pi Base labels.
When Waveshare ESP32-C6-Pico is seated in the Cytron Maker Pi Base socket:
- microSD SCK: Maker Pi Base GP10 (socket pin 14) -> ESP32-C6 `GPIO18`
- microSD MOSI: Maker Pi Base GP11 (socket pin 15) -> ESP32-C6 `GPIO19`
- microSD MISO: Maker Pi Base GP12 (socket pin 16) -> ESP32-C6 `GPIO20`
- microSD CS: Maker Pi Base GP15 (socket pin 20) -> `EXIO7` (TCA9554 I2C expander at 0x20 on `GPIO22` SDA / `GPIO23` SCL), NOT a native GPIO.
- Maker Pi buttons 1 & 2 (Pico GP20/GP21, socket pins 26/27) tie to `GPIO22`/`GPIO23`; pressing them shorts the TCA9554 bus.
## tone

short and direct. if a sentence needs a second read, cut it.
