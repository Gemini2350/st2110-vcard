cmake_minimum_required(VERSION 3.16)
project(st2110-vcard VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(ENABLE_NMOS "Build the embedded NMOS IS-04/IS-05 node (needs nmos-cpp)" ON)

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)

# Media Transport Library exports a pkg-config file "mtl"
pkg_check_modules(MTL REQUIRED IMPORTED_TARGET mtl)

# swr_alloc_set_opts2() and the AVChannelLayout API landed in FFmpeg 5.1
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
  libavformat>=59.27 libavcodec>=59.37 libavutil>=57.28
  libswscale>=6.7 libswresample>=4.7)

include(FetchContent)
FetchContent_Declare(json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3)
FetchContent_Declare(httplib
  GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
  GIT_TAG v0.18.3)
FetchContent_MakeAvailable(json httplib)

add_executable(st2110-vcard
  src/main.cpp
  src/config.cpp
  src/core/log.cpp
  src/core/formats.cpp
  src/core/mtl-device.cpp
  src/media/decoder.cpp
  src/media/encoder.cpp
  src/player/player.cpp
  src/recorder/recorder.cpp
  src/monitor/preview.cpp
  src/monitor/meters.cpp
  src/api/http-server.cpp
  src/engine.cpp
)

target_include_directories(st2110-vcard PRIVATE src)
target_link_libraries(st2110-vcard PRIVATE
  PkgConfig::MTL
  PkgConfig::FFMPEG
  nlohmann_json::nlohmann_json
  httplib::httplib
  Threads::Threads
)

if(ENABLE_NMOS)
  find_package(nmos-cpp CONFIG QUIET)
  if(nmos-cpp_FOUND)
    target_sources(st2110-vcard PRIVATE src/nmos/nmos-node.cpp)
    target_link_libraries(st2110-vcard PRIVATE nmos-cpp::nmos-cpp)
    target_compile_definitions(st2110-vcard PRIVATE HAVE_NMOS)
    message(STATUS "NMOS node: enabled")
  else()
    message(WARNING "nmos-cpp not found - building without the NMOS node")
  endif()
endif()

install(TARGETS st2110-vcard RUNTIME DESTINATION bin)
install(DIRECTORY web/ DESTINATION share/st2110-vcard/web)
install(FILES config/vcard.example.json DESTINATION share/st2110-vcard)
