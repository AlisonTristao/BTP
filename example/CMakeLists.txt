# Standalone build for the examples. Run from this directory:
#
#   cmake -B build
#   cmake --build build
#   ./build/sender              # writes frame.bin  (btp::Node)
#   ./build/receiver            # reads it back
#   ./build/by_hand_sender      # the same exchange step by step at the wire
#   ./build/by_hand_receiver
#
# It pulls btp::codec / btp::messages / btp::telemetry / btp::endpoint /
# btp::receiver / btp::session / btp::node straight from the sources one level
# up. BTP's own CMakeLists switches its tests and install rules off when it is
# not the top-level project, so this stays lean.

cmake_minimum_required(VERSION 3.16)
project(btp_examples LANGUAGES CXX)

set(BTP_ENABLE_AEAD OFF CACHE BOOL "" FORCE)   # the examples use only the plaintext path
add_subdirectory(.. "${CMAKE_CURRENT_BINARY_DIR}/btp")

# The btp::Node pair -- the friendly facade.
foreach(example sender receiver)
    add_executable(${example} ${example}.cpp)
    target_link_libraries(${example} PRIVATE btp::node btp::telemetry)
    target_compile_features(${example} PRIVATE cxx_std_11)
endforeach()

# The wire-level walkthrough -- codec + telemetry only.
foreach(example by_hand_sender by_hand_receiver)
    add_executable(${example} ${example}.cpp)
    target_link_libraries(${example} PRIVATE btp::telemetry)
    target_compile_features(${example} PRIVATE cxx_std_11)
endforeach()
