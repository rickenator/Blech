#!/usr/bin/env bash
set -euo pipefail

project_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
source_root=${1:-"$project_root/../esp32-ai"}

model="$source_root/firmware/model/model.bin"
vocab="$source_root/firmware/esp32_llm/vocab.h"
runtime="$source_root/firmware/common/llm.h"

for asset in "$model" "$vocab" "$runtime"; do
  if [[ ! -f "$asset" ]]; then
    echo "Missing required asset: $asset" >&2
    echo "Train/export the source project and run src/gen_assets.py first." >&2
    exit 1
  fi
done

mkdir -p "$project_root/model"
cp "$model" "$project_root/model/model.bin"
cp "$vocab" "$project_root/main/vocab.h"
cp "$runtime" "$project_root/main/llm.h"

echo "Installed model, vocabulary, and portable inference runtime."
