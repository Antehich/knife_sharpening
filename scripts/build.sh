#!/usr/bin/env bash
# Сборка прошивок и копирование .hex/.bin в prebuilt/.
# Запускать из west-воркспейса (см. README, раздел «Сборка»):
#   ./knife_sharpening/scripts/build.sh              # все приложения (diag, knife_angle)
#   ./knife_sharpening/scripts/build.sh diag       # только диагностика
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
BOARD=holyiot_21011
APPS=("$@")
[[ ${#APPS[@]} -eq 0 ]] && APPS=(diag knife_angle)
EXTRA=()
# DC/DC включается так: DCDC=1 ./scripts/build.sh ...
if [[ "${DCDC:-0}" == "1" ]]; then
	EXTRA+=(-DEXTRA_DTC_OVERLAY_FILE="$REPO/boards/dcdc.overlay")
fi

for app in "${APPS[@]}"; do
	build_dir="$REPO/build/$app"
	west build -p always -b "$BOARD" -d "$build_dir" "$REPO/firmware/$app" -- "${EXTRA[@]}"
	out="$REPO/prebuilt/$app"
	name="$app"
	[[ "$name" == knife_* ]] || name="knife_$name"
	mkdir -p "$out"
	cp "$build_dir/zephyr/zephyr.hex" "$out/$name.hex"
	cp "$build_dir/zephyr/zephyr.bin" "$out/$name.bin"
	(cd "$out" && sha256sum "$name.hex" "$name.bin" > SHA256SUMS)
	echo "==> $out"
done
