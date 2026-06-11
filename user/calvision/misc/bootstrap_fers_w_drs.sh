#!/usr/bin/env bash

set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BUILD_DIR="$REPO_DIR/build"
MODULE_DIR="$REPO_DIR/modules_calvision"

INSTALL_SYSTEM_DEPS=0
CHECK_ONLY=0
CLEAN_BUILD=0
JOBS=""
ROOT_HOME_ARG="${ROOT_HOME:-}"
ROOT_HOME_EXPLICIT=0
CAEN_PREFIX=""

usage() {
  cat <<'EOF'
Usage: bootstrap_fers_w_drs.sh [options]

Prepare a fresh clone to run user/calvision/misc/run_local_fers_w_drs.sh.

Options:
  --install-system-deps  Install compiler, CMake, Qt5, libusb, and runtime tools.
  --root PATH            ROOT installation prefix containing bin/thisroot.sh.
  --caen-prefix PATH     CAEN SDK prefix containing include/ and lib/ or lib64/.
  --jobs N               Number of parallel build jobs.
  --clean                Remove the existing build directory before configuring.
  --check-only           Check dependencies and an existing installation; do not build.
  -h, --help             Show this help.

ROOT and the CAEN Digitizer, CAENComm, and CAENVME SDKs are vendor dependencies
and must be installed separately before running this script.
EOF
}

die() {
  echo "ERROR: $*" >&2
  exit 1
}

run_privileged() {
  if (( EUID == 0 )); then
    "$@"
  elif command -v sudo >/dev/null 2>&1; then
    sudo "$@"
  else
    die "This action needs root privileges, but sudo is not available."
  fi
}

install_system_dependencies() {
  if command -v apt-get >/dev/null 2>&1; then
    run_privileged apt-get update
    run_privileged env DEBIAN_FRONTEND=noninteractive apt-get install -y \
      git build-essential cmake qtbase5-dev libusb-1.0-0-dev zlib1g-dev \
      procps iproute2
  elif command -v dnf >/dev/null 2>&1; then
    run_privileged dnf install -y \
      git gcc gcc-c++ make cmake qt5-qtbase-devel libusb1-devel zlib-devel \
      procps-ng iproute
  elif command -v yum >/dev/null 2>&1; then
    run_privileged yum install -y \
      git gcc gcc-c++ make cmake qt5-qtbase-devel libusb1-devel zlib-devel \
      procps-ng iproute
  else
    die "Unsupported package manager. Install CMake, a C++ compiler, Qt5 development files, libusb-1.0 development files, zlib development files, procps, and iproute manually."
  fi
}

check_required_commands() {
  local missing=()
  local cmd
  for cmd in cmake c++ make awk ldd pgrep pkill ss; do
    if ! command -v "$cmd" >/dev/null 2>&1; then
      missing+=("$cmd")
    fi
  done

  if (( ${#missing[@]} > 0 )); then
    die "Missing required commands: ${missing[*]}. Run this script with --install-system-deps."
  fi
}

resolve_root_home() {
  local candidates=()
  local candidate
  local root_config

  if [[ -n "$ROOT_HOME_ARG" ]]; then
    candidates+=("$ROOT_HOME_ARG")
  fi
  if [[ -n "${ROOTSYS:-}" ]]; then
    candidates+=("$ROOTSYS")
  fi
  if command -v root-config >/dev/null 2>&1; then
    root_config="$(command -v root-config)"
    candidates+=("$(cd "$(dirname "$root_config")/.." && pwd)")
  fi
  candidates+=("/home/softwares/root")

  for candidate in "${candidates[@]}"; do
    if [[ -x "$candidate/bin/root-config" && -f "$candidate/bin/thisroot.sh" ]]; then
      cd "$candidate"
      pwd
      return 0
    fi
  done

  return 1
}

load_root_environment() {
  local root_home="$1"
  set +u
  # ROOT supplies this environment script as part of its installation.
  source "$root_home/bin/thisroot.sh"
  set -u
  export ROOTSYS="$root_home"
}

find_header() {
  local header="$1"
  shift
  local directory
  for directory in "$@"; do
    if [[ -f "$directory/$header" ]]; then
      printf '%s\n' "$directory"
      return 0
    fi
  done
  return 1
}

find_library() {
  local library="$1"
  shift
  local directory
  local candidate
  for directory in "$@"; do
    for candidate in "$directory/lib${library}.so" "$directory/lib${library}.a"; do
      if [[ -f "$candidate" ]]; then
        printf '%s\n' "$candidate"
        return 0
      fi
    done
    candidate="$(find "$directory" -maxdepth 1 -name "lib${library}.so.*" -print -quit 2>/dev/null || true)"
    if [[ -n "$candidate" ]]; then
      printf '%s\n' "$candidate"
      return 0
    fi
  done
  return 1
}

resolve_caen_dependencies() {
  local include_dirs=()
  local library_dirs=()

  if [[ -n "$CAEN_PREFIX" ]]; then
    include_dirs+=("$CAEN_PREFIX/include")
    library_dirs+=("$CAEN_PREFIX/lib" "$CAEN_PREFIX/lib64")
  fi

  include_dirs+=("$HOME/local_install/include" /usr/include /usr/local/include)
  library_dirs+=(
    "$HOME/local_install/lib"
    /usr/lib
    /usr/lib64
    /usr/lib/x86_64-linux-gnu
    /usr/local/lib
  )

  CAEN_INCLUDE_DIR_FOUND="$(find_header CAENDigitizer.h "${include_dirs[@]}" || true)"
  CAEN_DIGI_LIBRARY_FOUND="$(find_library CAENDigitizer "${library_dirs[@]}" || true)"
  CAEN_COMM_LIBRARY_FOUND="$(find_library CAENComm "${library_dirs[@]}" || true)"
  CAEN_VME_LIBRARY_FOUND="$(find_library CAENVME "${library_dirs[@]}" || true)"

  local missing=()
  [[ -n "$CAEN_INCLUDE_DIR_FOUND" ]] || missing+=("CAENDigitizer.h")
  [[ -n "$CAEN_DIGI_LIBRARY_FOUND" ]] || missing+=("libCAENDigitizer")
  [[ -n "$CAEN_COMM_LIBRARY_FOUND" ]] || missing+=("libCAENComm")
  [[ -n "$CAEN_VME_LIBRARY_FOUND" ]] || missing+=("libCAENVME")

  if (( ${#missing[@]} > 0 )); then
    cat >&2 <<EOF
ERROR: Missing CAEN vendor dependencies: ${missing[*]}

Install the CAEN Digitizer, CAENComm, and CAENVME SDKs. Place them under
\$HOME/local_install/include and \$HOME/local_install/lib, install them in a
system path, or rerun with:

  $0 --caen-prefix /path/to/caen/prefix
EOF
    exit 1
  fi

  CAEN_RUNTIME_LIB_DIRS="$(dirname "$CAEN_DIGI_LIBRARY_FOUND")"
  local directory
  for directory in "$(dirname "$CAEN_COMM_LIBRARY_FOUND")" "$(dirname "$CAEN_VME_LIBRARY_FOUND")"; do
    if [[ ":$CAEN_RUNTIME_LIB_DIRS:" != *":$directory:"* ]]; then
      CAEN_RUNTIME_LIB_DIRS="$CAEN_RUNTIME_LIB_DIRS:$directory"
    fi
  done
}

verify_installation() {
  local missing=0
  local path
  local unresolved
  local required_paths=(
    "$REPO_DIR/bin/euRun"
    "$REPO_DIR/bin/euLog"
    "$REPO_DIR/bin/euCliMonitor"
    "$REPO_DIR/bin/euCliCollector"
    "$REPO_DIR/bin/euCliProducer"
    "$MODULE_DIR/libeudaq_module_calvision.so"
    "$REPO_DIR/user/calvision/misc/fers_w_drs.ini"
    "$REPO_DIR/user/calvision/misc/fers_w_drs.conf"
  )

  for path in "${required_paths[@]}"; do
    if [[ ! -e "$path" ]]; then
      echo "Missing installed file: $path" >&2
      missing=1
    fi
  done

  local runtime_paths=(
    "$REPO_DIR/bin/euRun"
    "$REPO_DIR/bin/euLog"
    "$REPO_DIR/bin/euCliMonitor"
    "$REPO_DIR/bin/euCliCollector"
    "$REPO_DIR/bin/euCliProducer"
    "$MODULE_DIR/libeudaq_module_calvision.so"
  )

  for path in "${runtime_paths[@]}"; do
    if [[ -f "$path" ]]; then
      unresolved="$(LD_LIBRARY_PATH="$ROOT_HOME_FOUND/lib:$CAEN_RUNTIME_LIB_DIRS:$REPO_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
        ldd "$path" | awk '/not found/{print}')"
      if [[ -n "$unresolved" ]]; then
        echo "Unresolved libraries for $path:" >&2
        echo "$unresolved" >&2
        missing=1
      fi
    fi
  done

  (( missing == 0 )) || die "The FERS+DRS installation check failed."

  if [[ -z "${DISPLAY:-}" ]]; then
    echo "WARNING: DISPLAY is not set. The euRun and euLog GUIs need a local or forwarded X display." >&2
  fi
}

while (( $# > 0 )); do
  case "$1" in
    --install-system-deps)
      INSTALL_SYSTEM_DEPS=1
      shift
      ;;
    --root)
      [[ $# -ge 2 ]] || die "--root requires a path."
      ROOT_HOME_ARG="$2"
      ROOT_HOME_EXPLICIT=1
      shift 2
      ;;
    --caen-prefix)
      [[ $# -ge 2 ]] || die "--caen-prefix requires a path."
      CAEN_PREFIX="$2"
      shift 2
      ;;
    --jobs)
      [[ $# -ge 2 ]] || die "--jobs requires a positive integer."
      [[ "$2" =~ ^[1-9][0-9]*$ ]] || die "--jobs requires a positive integer."
      JOBS="$2"
      shift 2
      ;;
    --clean)
      CLEAN_BUILD=1
      shift
      ;;
    --check-only)
      CHECK_ONLY=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      die "Unknown option: $1"
      ;;
  esac
done

if (( ROOT_HOME_EXPLICIT )); then
  [[ -x "$ROOT_HOME_ARG/bin/root-config" && -f "$ROOT_HOME_ARG/bin/thisroot.sh" ]] ||
    die "--root must point to a ROOT installation containing bin/root-config and bin/thisroot.sh."
fi

if [[ -n "$CAEN_PREFIX" ]]; then
  [[ -d "$CAEN_PREFIX" ]] || die "--caen-prefix does not exist: $CAEN_PREFIX"
  CAEN_PREFIX="$(cd "$CAEN_PREFIX" && pwd)"
fi

if (( INSTALL_SYSTEM_DEPS )); then
  install_system_dependencies
fi

check_required_commands

ROOT_HOME_FOUND="$(resolve_root_home || true)"
if [[ -z "$ROOT_HOME_FOUND" ]]; then
  die "ROOT was not found. Install ROOT with GUI support, then rerun with --root /path/to/root."
fi
load_root_environment "$ROOT_HOME_FOUND"

resolve_caen_dependencies

echo "ROOT:              $ROOT_HOME_FOUND"
echo "CAEN headers:      $CAEN_INCLUDE_DIR_FOUND"
echo "CAEN Digitizer:    $CAEN_DIGI_LIBRARY_FOUND"
echo "CAENComm:          $CAEN_COMM_LIBRARY_FOUND"
echo "CAENVME:           $CAEN_VME_LIBRARY_FOUND"

if (( CHECK_ONLY )); then
  verify_installation
  echo "FERS+DRS dependencies and installed files look ready."
  exit 0
fi

if (( CLEAN_BUILD )); then
  [[ "$BUILD_DIR" == "$REPO_DIR/build" ]] || die "Refusing to clean unexpected build path: $BUILD_DIR"
  rm -rf "$BUILD_DIR"
fi

cmake_args=(
  -S "$REPO_DIR"
  -B "$BUILD_DIR"
  "-DEUDAQ_INSTALL_PREFIX=$REPO_DIR"
  -DUSER_CALVISION_BUILD=ON
  -DUSER_CALVISION_BUILD_VISA=OFF
  -DEUDAQ_BUILD_GUI=ON
  -DEUDAQ_BUILD_ONLINE_ROOT_MONITOR=ON
  -DEUDAQ_LIBRARY_BUILD_TTREE=ON
  -DUSER_EXAMPLE_BUILD=OFF
  -DUSER_EUDET_BUILD=OFF
  "-DCAEN_INCLUDE_DIR=$CAEN_INCLUDE_DIR_FOUND"
  "-DCAEN_DIGI_LIBRARY=$CAEN_DIGI_LIBRARY_FOUND"
  "-DCAEN_COMM_LIBRARY=$CAEN_COMM_LIBRARY_FOUND"
  "-DCAEN_VME_LIBRARY=$CAEN_VME_LIBRARY_FOUND"
)

if [[ -d "$ROOT_HOME_FOUND/cmake" ]]; then
  cmake_args+=("-DROOT_DIR=$ROOT_HOME_FOUND/cmake")
fi

cmake "${cmake_args[@]}"

if [[ -n "$JOBS" ]]; then
  cmake --build "$BUILD_DIR" --parallel "$JOBS"
else
  cmake --build "$BUILD_DIR" --parallel
fi

cmake --install "$BUILD_DIR"
mkdir -p "$MODULE_DIR"
install -m 755 "$REPO_DIR/lib/libeudaq_module_calvision.so" "$MODULE_DIR/libeudaq_module_calvision.so"

verify_installation

echo
echo "FERS+DRS build and installation completed."
echo "Launch it with:"
printf '  ROOT_HOME=%q CAEN_LIB_DIR=%q %q\n' \
  "$ROOT_HOME_FOUND" \
  "$CAEN_RUNTIME_LIB_DIRS" \
  "$REPO_DIR/user/calvision/misc/run_local_fers_w_drs.sh"
echo
echo "In euRun, load:"
echo "  $REPO_DIR/user/calvision/misc/fers_w_drs.ini"
echo "  $REPO_DIR/user/calvision/misc/fers_w_drs.conf"
