#!/usr/bin/env bash
set -euo pipefail

project_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
idf_path=${IDF_PATH:-"$HOME/esp/esp-idf/esp-idf-v5.4.1"}
tools_root=${IDF_TOOLS_PATH:-"$HOME/.espressif"}
python_env=${IDF_PYTHON_ENV_PATH:-"$tools_root/python_env/idf5.4_py3.12_env"}

# ESP-IDF 5.4.1 checks these versions exactly. Do not select the newest
# installed tools: newer ESP-IDF releases may have placed incompatible
# toolchains beside them.
compiler="$tools_root/tools/xtensa-esp-elf/esp-14.2.0_20241119"
gdb="$tools_root/tools/xtensa-esp-elf-gdb/16.2_20250324"
cmake="$tools_root/tools/cmake/3.30.2"
ninja="$tools_root/tools/ninja/1.12.1"
rom_elf="$tools_root/tools/esp-rom-elfs/20241011"

for required in "$compiler" "$gdb" "$cmake" "$ninja" "$rom_elf" "$python_env"; do
  if [[ ! -d "$required" ]]; then
    echo "Required ESP-IDF 5.4.1 tool is missing: $required" >&2
    exit 1
  fi
done

export IDF_PATH="$idf_path"
export IDF_TOOLS_PATH="$tools_root"
export IDF_PYTHON_ENV_PATH="$python_env"
export ESP_ROM_ELF_DIR="$rom_elf"
export PATH="$compiler/xtensa-esp-elf/bin:$gdb/xtensa-esp-elf-gdb/bin:$cmake/bin:$ninja:$python_env/bin:$PATH"

idf_py=("$python_env/bin/python" "$idf_path/tools/idf.py")
cd "$project_root"
if [[ -f build/CMakeCache.txt ]] &&
   ! grep -q "$compiler" build/CMakeCache.txt; then
  "${idf_py[@]}" fullclean
fi
if [[ ! -f sdkconfig ]]; then
  "${idf_py[@]}" set-target esp32s3
fi
"${idf_py[@]}" build
