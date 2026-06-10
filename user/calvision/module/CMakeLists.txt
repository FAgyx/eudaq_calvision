include_directories(
  include
  ${PROJECT_SOURCE_DIR}/include
  ${PROJECT_SOURCE_DIR}/monitors/rootmonitor/include
  ${ROOT_INCLUDE_DIRS}
  /home/softwares/root/include
  /usr/include/root
  /usr/include)
link_directories(/usr/lib)
link_directories(/usr/lib/x86_64-linux-gnu)
aux_source_directory(src MODULE_SRC)

find_path(VISA_INCLUDE_DIR visa.h
  PATHS
    /usr/include/ni-visa
    /usr/local/include/ni-visa
    /usr/include
    /usr/local/include)
find_library(VISA_LIBRARY visa
  PATHS
    /usr/lib
    /usr/lib64
    /usr/lib/x86_64-linux-gnu
    /usr/local/lib)

option(USER_CALVISION_BUILD_VISA "build Calvision VISA-based producers" ON)

if(USER_CALVISION_BUILD_VISA AND VISA_INCLUDE_DIR AND VISA_LIBRARY)
  include_directories(include ${VISA_INCLUDE_DIR})
  set(FERS_VISA_LIBRARIES ${VISA_LIBRARY})
  message(STATUS "user/calvision VISA producer is to be built (USER_CALVISION_BUILD_VISA=ON)")
else()
  list(REMOVE_ITEM MODULE_SRC src/TexMSO64Producer.cc)
  if(USER_CALVISION_BUILD_VISA)
    message(STATUS "user/calvision VISA producer is NOT to be built (missing VISA dependency)")
  else()
    message(STATUS "user/calvision VISA producer is NOT to be built (USER_CALVISION_BUILD_VISA=OFF)")
  endif()
endif()

if(NOT EUDAQ_TTREE_LIBRARY)
  list(REMOVE_ITEM MODULE_SRC src/QTPDProducerEvent2TTreeEventConverter.cc)
  list(REMOVE_ITEM MODULE_SRC src/DRSProducerEvent2TTreeEventConverter.cc)
  list(REMOVE_ITEM MODULE_SRC src/FERSProducerEvent2TTreeEventConverter.cc)
endif()


add_library(${EUDAQ_MODULE} SHARED ${MODULE_SRC})
target_link_libraries(${EUDAQ_MODULE} ${EUDAQ_CORE_LIBRARY} ${EUDAQ_TTREE_LIBRARY} ${EUDAQ_ROOT_MONITOR_LIBRARY} ${USER_HARDWARE_LIBRARY} ${CAEN_DIGI_LIBRARY} ${CAEN_COMM_LIBRARY} ${CAEN_VME_LIBRARY} ${FERS_VISA_LIBRARIES})

install(TARGETS
  ${EUDAQ_MODULE}
  RUNTIME DESTINATION bin
  LIBRARY DESTINATION lib
  ARCHIVE DESTINATION lib)
