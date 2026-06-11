#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BIN_DIR="$ROOT_DIR/bin"
MODULE_DIR="$ROOT_DIR/modules_calvision"
ROOT_HOME="${ROOT_HOME:-/home/softwares/root}"
CAEN_LIB_DIR="${CAEN_LIB_DIR:-$HOME/local_install/lib}"
LOG_BIN="$BIN_DIR/euLog"

if [[ -x "$ROOT_DIR/build/gui/euLog" ]]; then
  LOG_BIN="$ROOT_DIR/build/gui/euLog"
fi

stop_existing() {
  pkill -x euLog 2>/dev/null || true
  pkill -x euCliProducer 2>/dev/null || true
  pkill -x euCliCollector 2>/dev/null || true
  pkill -x euCliMonitor 2>/dev/null || true
  pkill -x euRun 2>/dev/null || true
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
    if ss -ltn 2>/dev/null | awk '$4 ~ /:44000$/ { found=1 } END { exit !found }'; then
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
    "$LOG_BIN" &
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
  echo "Failed to start euLog after 10 attempts: $LOG_BIN"
  return 1
}

if [[ ! -x "$BIN_DIR/euRun" ]]; then
  echo "Missing executable: $BIN_DIR/euRun"
  echo "Build EUDAQ first, then rerun this script."
  exit 1
fi

if [[ ! -f "$MODULE_DIR/libeudaq_module_calvision.so" ]]; then
  echo "Missing module: $MODULE_DIR/libeudaq_module_calvision.so"
  echo "Build and install user/calvision first, then rerun this script."
  exit 1
fi

if [[ ! -x "$ROOT_HOME/bin/root-config" ]]; then
  echo "Missing ROOT installation: $ROOT_HOME/bin/root-config"
  echo "Update ROOT_HOME in this launcher or install ROOT there."
  exit 1
fi

export EUDAQ_MODULE_DIR="$MODULE_DIR"
export EUDAQ_MODULE_IGNORE_DEFALUT=1
export ROOTSYS="$ROOT_HOME"
export PATH="$ROOTSYS/bin${PATH:+:$PATH}"
export LD_LIBRARY_PATH="$ROOTSYS/lib:$CAEN_LIB_DIR:$ROOT_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

stop_existing

if ! wait_for_process_exit euLog; then
  echo "Timed out waiting for euLog to exit"
  exit 1
fi

if ! wait_for_process_exit euRun; then
  echo "Timed out waiting for euRun to exit"
  exit 1
fi

cd "$ROOT_DIR"

"$BIN_DIR/euRun" &

if ! wait_for_runcontrol; then
  echo "euRun did not open tcp://localhost:44000 within 30 seconds"
  exit 1
fi

start_eulog

"$BIN_DIR/euCliMonitor" -n FERSROOTMonitor -t my_mon0 &
sleep 1

"$BIN_DIR/euCliCollector" -n FERSDataCollector -t my_dc0 &
sleep 1

"$BIN_DIR/euCliProducer" -n DRSProducer -t my_drs0 &
sleep 1

"$BIN_DIR/euCliProducer" -n FERSProducer -t my_fers0 &

echo "Calvision FERS+DRS components launched from $ROOT_DIR"
echo "Using euLog binary=$LOG_BIN"
echo "Using EUDAQ_MODULE_DIR=$EUDAQ_MODULE_DIR"
echo "Using EUDAQ_MODULE_IGNORE_DEFALUT=$EUDAQ_MODULE_IGNORE_DEFALUT"
echo "In euRun, load user/calvision/misc/fers_w_drs.ini + user/calvision/misc/fers_w_drs.conf"
