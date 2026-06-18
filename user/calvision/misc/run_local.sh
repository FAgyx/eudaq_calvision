#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BIN_DIR="$ROOT_DIR/bin"
MODULE_DIR="${EUDAQ_MODULE_DIR:-$ROOT_DIR/lib}"
ROOT_HOME="${ROOT_HOME:-}"
CAEN_LIB_DIR="${CAEN_LIB_DIR:-}"
LOG_BIN="$BIN_DIR/euLog"
LAUNCH_LOG_DIR="${LAUNCH_LOG_DIR:-$ROOT_DIR/logs/launch_$(date +%Y%m%d_%H%M%S)}"
RUNCONTROL_PORT="${RUNCONTROL_PORT:-44000}"

if [[ -x "$ROOT_DIR/build/gui/euLog" ]]; then
  LOG_BIN="$ROOT_DIR/build/gui/euLog"
fi

die() {
  echo "$*" >&2
  exit 1
}

resolve_root_home() {
  local candidates=()
  local candidate
  local root_config

  [[ -n "$ROOT_HOME" ]] && candidates+=("$ROOT_HOME")
  [[ -n "${ROOTSYS:-}" ]] && candidates+=("$ROOTSYS")
  if command -v root-config >/dev/null 2>&1; then
    root_config="$(command -v root-config)"
    candidates+=("$(cd "$(dirname "$root_config")/.." && pwd)")
  fi
  candidates+=("/home/softwares/root")

  for candidate in "${candidates[@]}"; do
    if [[ -x "$candidate/bin/root-config" ]]; then
      cd "$candidate"
      pwd
      return 0
    fi
  done
  return 1
}

resolve_caen_lib_dir() {
  local library_dirs=(
    "$HOME/local_install/lib"
    /usr/lib
    /usr/lib64
    /usr/lib/x86_64-linux-gnu
    /usr/local/lib
  )
  local result=""
  local directory
  local library

  for directory in "${library_dirs[@]}"; do
    for library in CAENDigitizer CAENComm CAENVME; do
      if compgen -G "$directory/lib${library}.so*" >/dev/null ||
         [[ -f "$directory/lib${library}.a" ]]; then
        if [[ ":$result:" != *":$directory:"* ]]; then
          result="${result:+$result:}$directory"
        fi
        break
      fi
    done
  done

  printf '%s\n' "${result:-$HOME/local_install/lib}"
}

stop_existing() {
  local processes=(
    euLog
    euCliProducer
    euCliCollector
    euCliMonitor
    euRun
    euCliRun
  )
  local process

  for process in "${processes[@]}"; do
    pkill -x "$process" 2>/dev/null || true
  done
}

wait_for_process_exit() {
  local name="$1"
  local tries=0

  while (( tries < 20 )); do
    if ! pgrep -x "$name" >/dev/null 2>&1; then
      return 0
    fi
    sleep 1
    ((tries += 1))
  done
  return 1
}

wait_for_runcontrol() {
  local tries=0

  while (( tries < 30 )); do
    if ss -ltn 2>/dev/null |
       awk -v port=":$RUNCONTROL_PORT" '$4 ~ port "$" { found=1 } END { exit !found }'; then
      return 0
    fi
    sleep 1
    ((tries += 1))
  done
  return 1
}

start_eulog() {
  local tries=0

  while (( tries < 10 )); do
    "$LOG_BIN" >> "$LAUNCH_LOG_DIR/euLog.log" 2>&1 &
    EULOG_PID=$!
    sleep 1
    if kill -0 "$EULOG_PID" 2>/dev/null; then
      echo "Started euLog from $LOG_BIN"
      return 0
    fi
    ((tries += 1))
    echo "euLog exited immediately, retrying ($tries/10)"
    sleep 1
  done

  die "Failed to start euLog after 10 attempts: $LOG_BIN"
}

require_file() {
  local path="$1"
  local description="$2"

  [[ -f "$path" ]] || die "Missing $description: $path"
}

require_executable() {
  local path="$1"
  local description="$2"

  [[ -x "$path" ]] || die "Missing $description: $path"
}

require_executable "$BIN_DIR/euRun" "executable"
require_executable "$BIN_DIR/euCliMonitor" "executable"
require_executable "$BIN_DIR/euCliCollector" "executable"
require_executable "$BIN_DIR/euCliProducer" "executable"
require_file "$MODULE_DIR/libeudaq_module_calvision.so" "Calvision module"

ROOT_HOME="$(resolve_root_home || true)"
[[ -n "$ROOT_HOME" ]] ||
  die "Missing ROOT installation: no bin/root-config found. Set ROOT_HOME or load ROOT into PATH."

ROOT_LIB_DIR="$("$ROOT_HOME/bin/root-config" --libdir)"
CAEN_LIB_DIR="${CAEN_LIB_DIR:-$(resolve_caen_lib_dir)}"

export EUDAQ_MODULE_DIR="$MODULE_DIR"
export EUDAQ_MODULE_IGNORE_DEFALUT=1
export ROOTSYS="$ROOT_HOME"
export PATH="$ROOTSYS/bin${PATH:+:$PATH}"
export LD_LIBRARY_PATH="$ROOT_LIB_DIR:$CAEN_LIB_DIR:$ROOT_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

if [[ -n "${DISPLAY:-}" && -z "${QT_XCB_GL_INTEGRATION:-}" ]]; then
  export QT_XCB_GL_INTEGRATION=none
fi

stop_existing
for process in euLog euRun euCliRun euCliProducer euCliCollector euCliMonitor; do
  wait_for_process_exit "$process" || die "Timed out waiting for $process to exit"
done

cd "$ROOT_DIR"
mkdir -p "$LAUNCH_LOG_DIR"

"$BIN_DIR/euRun" >> "$LAUNCH_LOG_DIR/euRun.log" 2>&1 &
RUNCONTROL_PID=$!
sleep 1
kill -0 "$RUNCONTROL_PID" 2>/dev/null ||
  die "euRun exited immediately; see $LAUNCH_LOG_DIR/euRun.log"

wait_for_runcontrol ||
  die "euRun did not open tcp://localhost:$RUNCONTROL_PORT within 30 seconds"

kill -0 "$RUNCONTROL_PID" 2>/dev/null ||
  die "euRun exited before startup completed; see $LAUNCH_LOG_DIR/euRun.log"

start_eulog

"$BIN_DIR/euCliMonitor" -n FERSROOTMonitor -t my_mon0 >> "$LAUNCH_LOG_DIR/my_mon0.log" 2>&1 &
sleep 1

"$BIN_DIR/euCliCollector" -n FERSDataCollector -t my_dc0 >> "$LAUNCH_LOG_DIR/my_dc0.log" 2>&1 &
sleep 1

echo "Calvision FERS+DRS framework launched from $ROOT_DIR"
echo "Launch logs: $LAUNCH_LOG_DIR"
echo "ROOT_HOME: $ROOT_HOME"
echo "ROOT_LIB_DIR: $ROOT_LIB_DIR"
echo "CAEN_LIB_DIR: $CAEN_LIB_DIR"
echo "EUDAQ_MODULE_DIR: $EUDAQ_MODULE_DIR"
echo
echo "Producer startup is handled by euRun now:"
echo "  1. Open the Devices tab and select the connected DRS/FERS boards."
echo "  2. Open the FERS tab for HV_bias, RunCtrl, AcqMode, Discr, Spectroscopy."
echo "  3. Click Init; euRun generates the temporary ini/conf and starts producers."
