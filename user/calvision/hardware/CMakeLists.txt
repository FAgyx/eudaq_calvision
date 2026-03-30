include_directories(${CALVISION_COMPAT_DIR}/include)
include_directories(${CALVISION_WAVEDUMP_DIR}/include)
include_directories(${CALVISION_FERSLIB_DIR}/include)

find_path(LIBUSB_INCLUDE_DIR libusb.h
  PATHS
    /usr/include/libusb-1.0
    /usr/local/include/libusb-1.0
    /opt/homebrew/include/libusb-1.0)
find_library(LIBUSB_LIBRARY
  NAMES usb-1.0 libusb-1.0
  PATHS
    /usr/lib
    /usr/lib64
    /lib64
    /usr/lib/x86_64-linux-gnu
    /usr/local/lib
    /opt/homebrew/lib)

if(LIBUSB_INCLUDE_DIR)
  include_directories(${LIBUSB_INCLUDE_DIR})
endif()

set(USER_HARDWARE_SRC
  ${CALVISION_FERSLIB_DIR}/src/FERS_LLeth.c
  ${CALVISION_FERSLIB_DIR}/src/FERS_LLtdl.c
  ${CALVISION_FERSLIB_DIR}/src/FERS_LLusb.cpp
  ${CALVISION_FERSLIB_DIR}/src/FERS_MultiPlatform.c
  ${CALVISION_FERSLIB_DIR}/src/FERS_adapters.c
  ${CALVISION_FERSLIB_DIR}/src/FERS_configure.c
  ${CALVISION_FERSLIB_DIR}/src/FERS_configure_5202.c
  ${CALVISION_FERSLIB_DIR}/src/FERS_configure_5203.c
  ${CALVISION_FERSLIB_DIR}/src/FERS_configure_5204.c
  ${CALVISION_FERSLIB_DIR}/src/FERS_paramparser.c
  ${CALVISION_FERSLIB_DIR}/src/FERS_readout.c
  ${CALVISION_FERSLIB_DIR}/src/FERSlib.c
  ${CALVISION_WAVEDUMP_DIR}/src/WDconfig.c
  ${CALVISION_WAVEDUMP_DIR}/src/WDplot.c
  ${CALVISION_WAVEDUMP_DIR}/src/WaveDump.c
  ${CALVISION_WAVEDUMP_DIR}/src/X742CorrectionRoutines.c
  ${CALVISION_WAVEDUMP_DIR}/src/fft.c
  ${CALVISION_WAVEDUMP_DIR}/src/flash.c
  ${CALVISION_WAVEDUMP_DIR}/src/keyb.c
  ${CALVISION_WAVEDUMP_DIR}/src/spi.c)

if(USER_CALVISION_BUILD)
  add_library(${USER_HARDWARE_LIBRARY} STATIC ${USER_HARDWARE_SRC})
  if(NOT LIBUSB_LIBRARY)
    message(FATAL_ERROR "libusb-1.0 library not found for user/calvision")
  endif()
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(${USER_HARDWARE_LIBRARY} PRIVATE LINUX)
  endif()
  target_link_libraries(${USER_HARDWARE_LIBRARY} ${EUDAQ_CORE_LIBRARY} ${USER_HARDWARE_DEP_LIB} ${LIBUSB_LIBRARY})
endif()

install(TARGETS ${USER_HARDWARE_LIBRARY}
  RUNTIME DESTINATION bin
  LIBRARY DESTINATION lib
  ARCHIVE DESTINATION lib)
