# Standalone build for the examples. Run from this directory:
#
#   cmake -B build
#   cmake --build build
#   ./build/by_hand_sender      # writes frame.bin -- the runnable exchange
#   ./build/by_hand_receiver    # reads it back, step by step at the wire
#
# sender.cpp / receiver.cpp are the btp::Node version of the same flow, kept as
# readable logic (faked link, no file, no prints) -- they compile but do not
# transmit anything.
#
# It pulls btp::codec / btp::messages / btp::telemetry / btp::endpoint /
# btp::receiver / btp::session / btp::node / btp::aead straight from the
# sources one level up. BTP's own CMakeLists switches its tests and install
# rules off when it is not the top-level project, so this stays lean.

cmake_minimum_required(VERSION 3.16)
project(btp_examples LANGUAGES CXX)

# sender.cpp / receiver.cpp seal / open every frame (btp::aead, AES-128-GCM)
# -- BTP_ENABLE_AEAD ON pulls in mbedtls (fetched at configure time unless
# BTP_USE_SYSTEM_MBEDTLS or a parent project already provides mbedcrypto --
# see the top-level CMakeLists.txt).
set(BTP_ENABLE_AEAD ON CACHE BOOL "" FORCE)
add_subdirectory(.. "${CMAKE_CURRENT_BINARY_DIR}/btp")

# The btp::Node pair -- the friendly facade.
foreach(example sender receiver)
    add_executable(${example} ${example}.cpp)
    target_link_libraries(${example} PRIVATE btp::node btp::telemetry btp::aead)
    target_compile_features(${example} PRIVATE cxx_std_11)
endforeach()

# The wire-level walkthrough -- codec + telemetry only.
foreach(example by_hand_sender by_hand_receiver)
    add_executable(${example} ${example}.cpp)
    target_link_libraries(${example} PRIVATE btp::telemetry)
    target_compile_features(${example} PRIVATE cxx_std_11)
endforeach()
