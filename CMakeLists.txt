set(the_description "The UbiTrack ZeroMQ Networking Component")
ut_add_component(device_comm_zmq DEPS utcore utdataflow utvision )

SET(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

SET(ZMQ_DIR ${EXTERNAL_LIBRARIES_DIR}/zmq)
FIND_PACKAGE(ZMQ)

IF(ZeroMQ_FOUND)
  ut_component_include_directories("src/utzmq" ${ZeroMQ_INCLUDE_DIR} ${UBITRACK_CORE_DEPS_INCLUDE_DIR} ${OPENCV_INCLUDE_DIR})
  ut_glob_component_sources(HEADERS "src/utzmq/*.h" SOURCES "src/utzmq/*.cpp")
  ut_create_single_component(${ZeroMQ_LIBRARIES})
ENDIF(ZeroMQ_FOUND)

ut_install_utql_patterns()
