cmake_minimum_required(VERSION 3.10)
project(srvman CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(FREEIPMI REQUIRED libfreeipmi)
pkg_check_modules(IPMIMONITORING REQUIRED libipmimonitoring)
pkg_check_modules(GCRYPT REQUIRED libgcrypt)
find_package(Threads REQUIRED)

add_executable(srvman
  src/main.cpp
  src/config.cpp
  src/ipmi_power.cpp
  src/ipmi_monitor.cpp
  src/api.cpp
  src/ikvm_session.cpp
  src/ikvm_decoder.cpp
  src/ikvm_bridge.cpp
)

target_include_directories(srvman PRIVATE
  third_party
  ${FREEIPMI_INCLUDE_DIRS}
  ${IPMIMONITORING_INCLUDE_DIRS}
  ${GCRYPT_INCLUDE_DIRS}
)

target_link_libraries(srvman PRIVATE
  ${FREEIPMI_LIBRARIES}
  ${IPMIMONITORING_LIBRARIES}
  ${GCRYPT_LIBRARIES}
  Threads::Threads
)

target_compile_options(srvman PRIVATE
  ${FREEIPMI_CFLAGS_OTHER}
  ${IPMIMONITORING_CFLAGS_OTHER}
  ${GCRYPT_CFLAGS_OTHER}
)

# cpp-httplib's default (300s) is how long a KVM console WebSocket can sit on
# a dead browser (closed laptop lid, network drop with no close handshake)
# before the server notices and tears down its BMC session. 60s still gives
# the 30s ping interval (CPPHTTPLIB_WEBSOCKET_PING_INTERVAL_SECOND, left at
# its default) a full missed-pong cycle of slack.
target_compile_definitions(srvman PRIVATE
  CPPHTTPLIB_WEBSOCKET_SERVER_READ_TIMEOUT_SECOND=60
)

add_custom_target(copy_web ALL
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/web ${CMAKE_BINARY_DIR}/web
)
