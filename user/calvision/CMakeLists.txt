##-- OPT: Compile Calvision?
option(USER_CALVISION_BUILD "build Calvision" ON)
if(NOT USER_CALVISION_BUILD)
    message(STATUS "user/calvision is NOT to be built (USER_CALVISION_BUILD=OFF)")
  return()
endif()
message(STATUS "user/calvision is to be built (USER_CALVISION_BUILD=ON)")

enable_language(C)

##-- Global Preparation:
#-- EUDAQ_MODULE needed for compilation at lower level CMakeLists
get_filename_component(EUDAQ_USERNAME ${CMAKE_CURRENT_LIST_DIR} NAME)
set(EUDAQ_MODULE eudaq_module_${EUDAQ_USERNAME})
set(USER_HARDWARE_LIBRARY eudaq_static_${EUDAQ_USERNAME})

set(CALVISION_LOCAL_INSTALL_PREFIX "$ENV{HOME}/local_install")

find_path(CAEN_INCLUDE_DIR CAENDigitizer.h
  PATHS
    ${CALVISION_LOCAL_INSTALL_PREFIX}/include
    /usr/include
    /usr/local/include)

find_library(CAEN_DIGI_LIBRARY
  NAMES CAENDigitizer
  PATHS
    ${CALVISION_LOCAL_INSTALL_PREFIX}/lib
    /usr/lib
    /usr/lib64
    /usr/lib/x86_64-linux-gnu
    /usr/local/lib)

find_library(CAEN_COMM_LIBRARY
  NAMES CAENComm
  PATHS
    ${CALVISION_LOCAL_INSTALL_PREFIX}/lib
    /usr/lib
    /usr/lib64
    /usr/lib/x86_64-linux-gnu
    /usr/local/lib)

find_library(CAEN_VME_LIBRARY
  NAMES CAENVME
  PATHS
    ${CALVISION_LOCAL_INSTALL_PREFIX}/lib
    /usr/lib
    /usr/lib64
    /usr/lib/x86_64-linux-gnu
    /usr/local/lib)

if(NOT CAEN_INCLUDE_DIR)
  message(FATAL_ERROR "CAEN headers not found. Checked ${CALVISION_LOCAL_INSTALL_PREFIX}/include and system include paths.")
endif()

if(NOT CAEN_DIGI_LIBRARY OR NOT CAEN_COMM_LIBRARY OR NOT CAEN_VME_LIBRARY)
  message(FATAL_ERROR "CAEN libraries not found. Checked ${CALVISION_LOCAL_INSTALL_PREFIX}/lib and system library paths.")
endif()

include_directories(${EUDAQ_INCLUDE_DIRS})
include_directories(${CAEN_INCLUDE_DIR})

set(CALVISION_HARDWARE_DIR ${CMAKE_CURRENT_LIST_DIR}/hardware)
set(CALVISION_FERSLIB_DIR ${CALVISION_HARDWARE_DIR}/caenferslib-1.3.0)
set(CALVISION_COMPAT_DIR ${CALVISION_HARDWARE_DIR}/compat)
set(CALVISION_WAVEDUMP_DIR ${CALVISION_HARDWARE_DIR}/wavedump-v3.12.0)

include_directories(${CALVISION_COMPAT_DIR}/include)
include_directories(${CALVISION_WAVEDUMP_DIR}/include)
include_directories(${CALVISION_FERSLIB_DIR}/include)

message(STATUS "user/calvision CAEN headers: ${CAEN_INCLUDE_DIR}")
message(STATUS "user/calvision CAEN Digitizer lib: ${CAEN_DIGI_LIBRARY}")
message(STATUS "user/calvision CAEN Comm lib: ${CAEN_COMM_LIBRARY}")
message(STATUS "user/calvision CAEN VME lib: ${CAEN_VME_LIBRARY}")

add_subdirectory(hardware)
##-- Main Producer + DC:
add_subdirectory(module)
