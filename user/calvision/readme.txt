CalVision/FERS Fresh Clone Build Guide
======================================

Dependencies
------------

Install a compiler, CMake, Qt5, libusb, and zlib.

On Ubuntu/Debian:

  sudo apt install build-essential cmake qtbase5-dev libusb-1.0-0-dev zlib1g-dev

You must also install:

  - ROOT with GUI support
  - CAEN Digitizer library
  - CAENComm library
  - CAENVME library

CMake searches for the CAEN headers and libraries in:

  /usr/include
  /usr/lib
  $HOME/local_install/include
  $HOME/local_install/lib

Clone and Build
---------------

  git clone https://github.com/FAgyx/eudaq_calvision.git
  cd eudaq_calvision

Load the ROOT environment, replacing the path if necessary:

  source /path/to/root/bin/thisroot.sh

Configure and compile:

  cmake -S . -B build \
    -DUSER_CALVISION_BUILD=ON \
    -DUSER_CALVISION_BUILD_VISA=OFF \
    -DEUDAQ_BUILD_GUI=ON \
    -DEUDAQ_BUILD_ONLINE_ROOT_MONITOR=ON \
    -DEUDAQ_LIBRARY_BUILD_TTREE=ON

  cmake --build build --parallel
  cmake --install build

The install step places executables and libraries inside the cloned repository
under bin/ and lib/.

Prepare the isolated CalVision module directory expected by the launch scripts:

  mkdir -p modules_calvision
  cp lib/libeudaq_module_calvision.so modules_calvision/

Bootstrap Script
----------------

From a fresh clone, the bootstrap script can check dependencies, build, install,
prepare modules_calvision, and verify runtime library linkage:

  ./user/calvision/misc/bootstrap_fers_w_drs.sh

To also install supported Ubuntu/Debian, Fedora, or RHEL system packages:

  ./user/calvision/misc/bootstrap_fers_w_drs.sh --install-system-deps

ROOT and the CAEN Digitizer, CAENComm, and CAENVME SDKs must be installed
separately. Specify nonstandard installation locations with:

  ./user/calvision/misc/bootstrap_fers_w_drs.sh \
    --root /path/to/root \
    --caen-prefix /path/to/caen/prefix

To validate an existing installation without rebuilding:

  ./user/calvision/misc/bootstrap_fers_w_drs.sh --check-only

Run
---

Launch the FERS-only DAQ:

  ./user/calvision/misc/run_local_fers.sh

Other launch modes:

  ./user/calvision/misc/run_local_drs.sh
  ./user/calvision/misc/run_local_fers_w_drs.sh

The launch scripts default to ROOT at /home/softwares/root. The FERS+DRS
launcher accepts ROOT_HOME and CAEN_LIB_DIR overrides when dependencies are
installed elsewhere:

  ROOT_HOME=/path/to/root \
  CAEN_LIB_DIR=/path/to/caen/lib \
  ./user/calvision/misc/run_local_fers_w_drs.sh

In the euRun GUI for FERS-only operation:

  1. Init -> Load: user/calvision/misc/fers_only.ini
  2. Configure -> Load: user/calvision/misc/fers_only.conf
  3. Click Start

Generic EUDAQ Build
-------------------

To build generic EUDAQ without CalVision hardware support, configure with:

  -DUSER_CALVISION_BUILD=OFF

The CAEN libraries are not required when CalVision support is disabled.
