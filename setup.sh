#!/usr/bin/env sh
# setup.sh - clone external reference repos into third_party/ (gitignored).
#
# Usage:
#   ./setup.sh              # clone everything (shallow)
#   ./setup.sh --skip-esp-idf   # skip the heavy ESP-IDF clone (~2 GB)
#
# Re-running is safe: existing directories are skipped, missing ones are cloned.
set -eu

if [ "${1:-}" = "--skip-esp-idf" ]; then
    SKIP_ESP_IDF=1
elif [ -n "${1:-}" ]; then
    printf 'unknown option: %s\n' "$1"
    exit 1
fi

ROOT="$(cd "$(dirname "$0")" && pwd)"

REPOS="
mtkernel_3            | https://github.com/tron-forum/mtkernel_3.git
esp32c6-mtk3          | https://github.com/sirfonzie/esp32c6-mtk3.git
mtk3smp-rp2040        | https://github.com/sirfonzie/mtk3smp-rp2040.git
esp-idf               | https://github.com/espressif/esp-idf.git
esp-csi               | https://github.com/espressif/esp-csi.git
pico-sdk              | https://github.com/raspberrypi/pico-sdk.git
pico-examples         | https://github.com/raspberrypi/pico-examples.git
clang-format-barr-c   | https://github.com/petertorelli/clang-format-barr-c.git
esp32c6-sniffer       | https://github.com/dj1ch/esp32c6-sniffer.git
"

printf '==> Fetching third-party repos into %s/third_party\n' "$ROOT"
mkdir -p "$ROOT/third_party"

while IFS='|' read -r name url; do
    name="$(echo "$name" | tr -d '[:space:]')"
    url="$(echo "$url" | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')"
    [ -n "$name" ] || continue

    dir="$ROOT/third_party/$name"
    if [ -d "$dir/.git" ]; then
        printf '  %-20s already present, skipped\n' "$name"
    elif [ "$name" = "esp-idf" ] && [ -n "${SKIP_ESP_IDF:-}" ]; then
        printf '  %-20s skipped (--skip-esp-idf)\n' "$name"
    elif [ "$name" = "esp-idf" ] || [ "$name" = "pico-sdk" ]; then
        # these need submodules checked out to build
        git clone --depth 1 --recursive --shallow-submodules "$url" "$dir"
        printf '  %-20s cloned (with submodules)\n' "$name"
    else
        git clone --depth 1 "$url" "$dir"
        printf '  %-20s cloned\n' "$name"
    fi
done <<EOF
$REPOS
EOF

cat <<EOF

Done. Reference clones live in: $ROOT/third_party  (gitignored)

Notes:
  - tron-forum/mtkernel_3 is the upstream uT-Kernel 3.0 kernel to diff against the
    sirfonzie ports (esp32c6-mtk3, mtk3smp-rp2040) when hunting for port bugs.
  - clang-format-barr-c: copy its .clang-format into the repo root for Barr C compliance.
  - esp-idf: after cloning, run its install.sh, then source export.sh to use idf.py.
EOF
