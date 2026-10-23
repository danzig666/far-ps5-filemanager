cmake_minimum_required(VERSION 3.20)
project(far-ps5-filemanager LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static CRT on MSVC so the plugin DLL has no runtime dependencies.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

option(PS5FM_BUILD_TESTS "Build the core unit and integration tests" ON)
if(WIN32)
  option(PS5FM_BUILD_PLUGIN "Build the Far Manager plugin DLL" ON)
endif()

if(MSVC)
  add_compile_options(/W4 /utf-8 /permissive- /EHsc)
else()
  add_compile_options(-Wall -Wextra)
endif()

# Platform-neutral core: JSON, path codec, payload API client.
add_library(ps5core STATIC
  src/core/Json.cpp
  src/core/Operations.cpp
  src/core/PathCodec.cpp
  src/core/Ps5Client.cpp
  src/core/Text.cpp
)
target_include_directories(ps5core PUBLIC src/core)

if(PS5FM_BUILD_TESTS)
  enable_testing()
  add_executable(unit_tests tests/unit_tests.cpp)
  target_link_libraries(unit_tests PRIVATE ps5core)
  add_test(NAME unit_tests COMMAND unit_tests)

  if(NOT WIN32)
    # Runs against a live payload: integration_tests <port> <scratch-dir>
    add_executable(integration_tests tests/integration_tests.cpp tests/PosixHttp.cpp)
    target_link_libraries(integration_tests PRIVATE ps5core)
  endif()
endif()

if(WIN32 AND PS5FM_BUILD_PLUGIN)
  add_library(PS5FM SHARED
    src/far/Config.cpp
    src/far/FarApi.cpp
    src/far/Panel.cpp
    src/far/Plugin.cpp
    src/far/Progress.cpp
    src/far/Updater.cpp
    src/far/WinHttpTransport.cpp
    src/far/PS5FM.def
    src/far/PS5FM.rc
  )
  target_include_directories(PS5FM PRIVATE far-sdk src/far)
  target_compile_definitions(PS5FM PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
  target_link_libraries(PS5FM PRIVATE ps5core winhttp bcrypt psapi)
  set_target_properties(PS5FM PROPERTIES PREFIX "" OUTPUT_NAME "PS5FM")
endif()
