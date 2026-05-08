#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  run-qemu-plugins.sh [options] -- <guest_program> [guest_args...]

Options:
  -q, --qemu <path>      Path to qemu-riscv64
                         Default: ./build-riscv64-linux-user-plugin/qemu-riscv64
  -p, --plugin <spec>    Plugin spec, can be repeated
                         Examples:
                           ./build-riscv64-linux-user-plugin/contrib/plugins/libhotblocks.so
                           ./build-riscv64-linux-user-plugin/contrib/plugins/libhowvec.so,inline=on,count=hint
  -d, --debug-plugin     Add -d plugin for plugin logging
  -h, --help             Show help

Example:
  ./myScript/run-qemu-plugins.sh \
    -p ./build-riscv64-linux-user-plugin/contrib/plugins/libhotblocks.so \
    -p ./build-riscv64-linux-user-plugin/contrib/plugins/libexeclog.so \
    -- /nfs/home/huxuan/specdir/spec06_gcc15.2.0_rv64gcbv_base_maxk30_NEMU_archgroup_2026-03-24-23-34/elf/libquantum 143 8
EOF
}

QEMU="./build-riscv64-linux-user-plugin/qemu-riscv64"
DEBUG_PLUGIN=0
PLUGINS=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    -q|--qemu)
      [[ $# -ge 2 ]] || { echo "Missing argument for $1" >&2; exit 2; }
      QEMU="$2"
      shift 2
      ;;
    -p|--plugin)
      [[ $# -ge 2 ]] || { echo "Missing argument for $1" >&2; exit 2; }
      PLUGINS+=("$2")
      shift 2
      ;;
    -d|--debug-plugin)
      DEBUG_PLUGIN=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    --)
      shift
      break
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage
      exit 2
      ;;
  esac
done

[[ -x "$QEMU" ]] || { echo "QEMU binary not found or not executable: $QEMU" >&2; exit 1; }
[[ $# -ge 1 ]] || { echo "Missing guest_program" >&2; usage; exit 2; }

GUEST_PROG="$1"
shift
[[ -f "$GUEST_PROG" ]] || { echo "Guest program not found: $GUEST_PROG" >&2; exit 1; }

CMD=("$QEMU")
if [[ $DEBUG_PLUGIN -eq 1 ]]; then
  CMD+=("-d" "plugin")
fi

for spec in "${PLUGINS[@]}"; do
  plugin_path="${spec%%,*}"
  plugin_path="${plugin_path#file=}"
  [[ -f "$plugin_path" ]] || { echo "Plugin file not found: $plugin_path" >&2; exit 1; }
  CMD+=("-plugin" "$spec")
done

CMD+=("$GUEST_PROG" "$@")

echo "Running command:"
printf ' %q' "${CMD[@]}"
echo
exec "${CMD[@]}"
