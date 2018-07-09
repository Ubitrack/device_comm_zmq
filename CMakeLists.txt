project(ubitrack_device_comm_zmq)

cmake_minimum_required(VERSION 3.4.0)

include(${CMAKE_BINARY_DIR}/conanbuildinfo.cmake)
conan_basic_setup(TARGETS)

include(GNUInstallDirs)
include(cmake/UbitrackConfig)

if(WITH_MSGPACK)
  set(UBITRACK_COMPILE_DEFINITIONS ${UBITRACK_COMPILE_DEFINITIONS} "HAVE_MSGPACK")
endif(WITH_MSGPACK)

add_subdirectory(src/utzmq)

ubitrack_install_utql("doc/utql/*.xml" "doc/utql/*/*.xml"  "doc/utql/*/*/*.xml")
ubitrack_install_doc("doc/utqlDoc/*" "doc/utqlDoc/*/*" "doc/utqlDoc/*/*/*")