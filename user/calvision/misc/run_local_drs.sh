#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BIN_DIR="$ROOT_DIR/bin"
MODULE_DIR="$ROOT_DIR/modules_calvision"
ROOT_HOME="/home/softwares/root"

stop_existing() {
  pkill -x euCliProducer 2>/dev/null || true
  pkill -x euCliCollector 2>/dev/null || true
  pkill -x euCliMonitor 2>/dev/null || true
  pkill -x euLog 2>/dev/null || true
  pkill -x euRun 2>/dev/null || true
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
export ROOTSYS="$ROOT_HOME"
export PATH="$ROOTSYS/bin${PATH:+:$PATH}"
export LD_LIBRARY_PATH="$ROOTSYS/lib:$HOME/local_install/lib:$ROOT_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

stop_existing

cd "$ROOT_DIR"

"$BIN_DIR/euRun" &
sleep 1

"$BIN_DIR/euLog" &
sleep 1

"$BIN_DIR/euCliMonitor" -n FERSROOTMonitor -t my_mon0 &
sleep 1

"$BIN_DIR/euCliCollector" -n FERSDataCollector -t my_dc0 &
sleep 1

"$BIN_DIR/euCliProducer" -n DRSProducer -t my_drs0 &

echo "Calvision DRS-only components launched from $ROOT_DIR"
echo "Using EUDAQ_MODULE_DIR=$EUDAQ_MODULE_DIR"
echo "In euRun, load user/calvision/misc/drs_only.ini + user/calvision/misc/drs_only.conf"
