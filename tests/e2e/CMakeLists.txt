# Standalone, OBS-free build of the plugin's real ASR client for end-to-end
# tests against a live tea-asr-service (see tests/e2e/run_e2e.py). This is NOT
# part of the plugin build and does not need the OBS SDK; it only needs a
# desktop Qt6 (Core + Network), e.g. Homebrew's `qt`.
#
#   cmake -S tests/e2e -B <builddir> -DCMAKE_PREFIX_PATH=$(brew --prefix qt)
#   cmake --build <builddir>
cmake_minimum_required(VERSION 3.28)
project(tea_asr_client_e2e LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

get_filename_component(TEA_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
# Lets the baseline run compile an older asr-client.cpp without touching src/.
set(TEA_ASR_CLIENT_SOURCE "${TEA_REPO_ROOT}/src/asr-client.cpp" CACHE FILEPATH "asr-client.cpp under test")

find_package(Qt6 REQUIRED COMPONENTS Core Network)

add_executable(
  asr-client-e2e
  driver.cpp
  fake-audio-tap.c
  obs-log-stub.c
  "${TEA_ASR_CLIENT_SOURCE}"
  "${TEA_REPO_ROOT}/src/asr-client.hpp"
  "${TEA_REPO_ROOT}/src/caption-state.c"
)
target_include_directories(asr-client-e2e PRIVATE "${TEA_REPO_ROOT}/src" "${TEA_REPO_ROOT}/tests/stubs")
target_link_libraries(asr-client-e2e PRIVATE Qt6::Core Qt6::Network)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(asr-client-e2e PRIVATE -Wall -Wextra)
endif()
