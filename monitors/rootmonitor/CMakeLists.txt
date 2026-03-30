if(DEFINED ENV{ROOTSYS} AND EXISTS "$ENV{ROOTSYS}/cmake")
  list(PREPEND CMAKE_PREFIX_PATH "$ENV{ROOTSYS}")
  set(ROOT_DIR "$ENV{ROOTSYS}/cmake" CACHE PATH "Path to the preferred ROOT CMake package" FORCE)
  find_package(ROOT QUIET CONFIG PATHS "$ENV{ROOTSYS}/cmake" NO_DEFAULT_PATH)
else()
  find_package(ROOT QUIET)
endif()

cmake_dependent_option(EUDAQ_BUILD_ONLINE_ROOT_MONITOR "monitor/ROOTMonitor executable (requires ROOT)" OFF
  "ROOT_FOUND" OFF)

if(NOT EUDAQ_BUILD_ONLINE_ROOT_MONITOR)
  message(STATUS "monitor/ROOTMonitor is NOT to be built (EUDAQ_BUILD_ONLINE_ROOT_MONITOR=OFF)")
  unset(EUDAQ_ROOT_MONITOR_LIBRARY CACHE)
  return()
endif()
message(STATUS "monitor/ROOTMonitor is to be built (EUDAQ_BUILD_ONLINE_ROOT_MONITOR=ON)")

if(WIN32)
  if(DEFINED ENV{ROOTSYS} AND EXISTS "$ENV{ROOTSYS}/cmake")
    find_package(ROOT REQUIRED CONFIG PATHS "$ENV{ROOTSYS}/cmake" NO_DEFAULT_PATH COMPONENTS libGui libCore libHist libRIO)
  else()
    find_package(ROOT REQUIRED COMPONENTS libGui libCore libHist libRIO)
  endif()
else()
  if(DEFINED ENV{ROOTSYS} AND EXISTS "$ENV{ROOTSYS}/cmake")
    find_package(ROOT REQUIRED CONFIG PATHS "$ENV{ROOTSYS}/cmake" NO_DEFAULT_PATH COMPONENTS Gui)
  else()
    find_package(ROOT REQUIRED COMPONENTS Gui)
  endif()
endif()

get_filename_component(EUDAQ_COMPONENT ${CMAKE_CURRENT_LIST_DIR} NAME)
set(EUDAQ_ROOT_MONITOR_LIBRARY ${EUDAQ_COMPONENT} CACHE INTERNAL "eudaq root monitoring library")

set(EUDAQ_INCLUDE_DIRS
    ${EUDAQ_INCLUDE_DIRS}
    ${CMAKE_CURRENT_LIST_DIR}/include
    ${ROOT_INCLUDE_DIRS}
    CACHE INTERNAL "directory of eudaq include")

include_directories(${EUDAQ_INCLUDE_DIRS})

if(ROOT_USE_FILE)
  include(${ROOT_USE_FILE})
endif()

include_directories(. include)
include_directories(${CMAKE_BINARY_DIR})

aux_source_directory(src EUDAQ_COMPONENT_SRC)

root_generate_dictionary(eudaq_rootmonitorDict eudaq/ROOTMonitor.hh eudaq/ROOTMonitorWindow.hh
  LINKDEF include/LinkDef.h
  MODULE eudaq_rootmonitor
)
list(APPEND EUDAQ_COMPONENT_SRC ${CMAKE_CURRENT_BINARY_DIR}/eudaq_rootmonitorDict.cxx)

add_library(${EUDAQ_COMPONENT} SHARED ${EUDAQ_COMPONENT_SRC})

target_link_libraries(${EUDAQ_COMPONENT} ${EUDAQ_CORE_LIBRARY} ${EUDAQ_THREADS_LIB} ${ROOT_LIBRARIES})

install(TARGETS ${EUDAQ_COMPONENT}
  EXPORT eudaqTargets
  RUNTIME DESTINATION bin
  LIBRARY DESTINATION lib
  ARCHIVE DESTINATION lib)

if(ROOT_VERSION_MAJOR GREATER 5)
  set(ROOTProducer_PCM ${CMAKE_CURRENT_BINARY_DIR}/libeudaq_rootmonitor_rdict.pcm)
  set(ROOTProducer_MAP ${CMAKE_CURRENT_BINARY_DIR}/libeudaq_rootmonitor.rootmap)
  install(FILES
    ${ROOTProducer_PCM}
    ${ROOTProducer_MAP}
    DESTINATION lib)
endif()

file(GLOB INC_FILES "${CMAKE_CURRENT_SOURCE_DIR}/include/eudaq/*.hh")
install(FILES ${INC_FILES} DESTINATION include/eudaq)
