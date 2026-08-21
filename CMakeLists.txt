cmake_minimum_required(VERSION 3.16)

project(btp VERSION 1.1.0 LANGUAGES CXX)

option(BTP_BUILD_TESTS "Build the BTP codec unit tests" ON)
option(BTP_ENABLE_AEAD "Build the btp::aead AEAD backend (requires mbedtls)" ON)

if(BTP_ENABLE_AEAD)
    include(FetchContent)
    FetchContent_Declare(
        mbedtls
        GIT_REPOSITORY https://github.com/Mbed-TLS/mbedtls.git
        GIT_TAG v3.6.7 # newest 3.6.x LTS tag at time of pinning; pin to a tag, never a branch
    )
    # mbedtls defaults to building its own test suite and sample programs, and
    # treating its own compiler warnings as errors. None of that should gate
    # or slow down our build, so force these off before mbedtls configures.
    set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(mbedtls)

    add_library(btp_aead src/aead.cpp)
    add_library(btp::aead ALIAS btp_aead)

    target_include_directories(
        btp_aead
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
    )
    target_compile_features(btp_aead PUBLIC cxx_std_11)
    target_link_libraries(btp_aead PUBLIC btp::codec PRIVATE mbedcrypto)

    if(MSVC)
        target_compile_options(btp_aead PRIVATE /W4 /WX)
    else()
        target_compile_options(btp_aead PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
endif()

add_library(
    btp_codec
    src/codec.cpp
    src/fragmentation.cpp
    src/stream.cpp
)
add_library(btp::codec ALIAS btp_codec)

target_include_directories(
    btp_codec
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_compile_features(btp_codec PUBLIC cxx_std_11)

if(MSVC)
    target_compile_options(btp_codec PRIVATE /W4 /WX)
else()
    target_compile_options(btp_codec PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

if(BTP_BUILD_TESTS)
    enable_testing()
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_executable(btp_codec_tests tests/test_codec.cpp)
    target_link_libraries(btp_codec_tests PRIVATE btp::codec)
    target_compile_features(btp_codec_tests PRIVATE cxx_std_11)
    add_executable(btp_transport_tests tests/test_transport.cpp)
    target_link_libraries(btp_transport_tests PRIVATE btp::codec)
    target_compile_features(btp_transport_tests PRIVATE cxx_std_11)
    add_executable(btp_conformance_tests tests/test_conformance.cpp)
    target_link_libraries(btp_conformance_tests PRIVATE btp::codec)
    target_compile_features(btp_conformance_tests PRIVATE cxx_std_11)
    target_compile_definitions(
        btp_conformance_tests
        PRIVATE BTP_VECTOR_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/test-vectors/v1"
    )
    add_executable(btp_conformance_v2_tests tests/test_conformance_v2.cpp)
    target_link_libraries(btp_conformance_v2_tests PRIVATE btp::codec)
    target_compile_features(btp_conformance_v2_tests PRIVATE cxx_std_11)
    target_compile_definitions(
        btp_conformance_v2_tests
        PRIVATE BTP_VECTOR_ROOT_V2="${CMAKE_CURRENT_SOURCE_DIR}/test-vectors/v2"
    )
    if(MSVC)
        target_compile_options(btp_codec_tests PRIVATE /W4 /WX)
        target_compile_options(btp_transport_tests PRIVATE /W4 /WX)
        target_compile_options(btp_conformance_tests PRIVATE /W4 /WX)
        target_compile_options(btp_conformance_v2_tests PRIVATE /W4 /WX)
    else()
        target_compile_options(btp_codec_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
        target_compile_options(btp_transport_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
        target_compile_options(btp_conformance_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
        target_compile_options(btp_conformance_v2_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME btp_codec_tests COMMAND btp_codec_tests)
    add_test(NAME btp_transport_tests COMMAND btp_transport_tests)
    add_test(NAME btp_conformance_tests COMMAND btp_conformance_tests)
    add_test(NAME btp_conformance_v2_tests COMMAND btp_conformance_v2_tests)
    add_test(
        NAME btp_vector_descriptions
        COMMAND
            ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_vectors.py
            --root ${CMAKE_CURRENT_SOURCE_DIR}/test-vectors/v1
            --check
    )
    add_test(
        NAME btp_vector_descriptions_v2
        COMMAND
            ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_vectors_v2.py
            --root ${CMAKE_CURRENT_SOURCE_DIR}/test-vectors/v2
            --check
    )
endif()

if(BTP_BUILD_TESTS AND BTP_ENABLE_AEAD)
    add_executable(btp_aead_tests tests/test_aead.cpp)
    target_link_libraries(btp_aead_tests PRIVATE btp::aead)
    target_compile_features(btp_aead_tests PRIVATE cxx_std_11)
    if(MSVC)
        target_compile_options(btp_aead_tests PRIVATE /W4 /WX)
    else()
        target_compile_options(btp_aead_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME btp_aead_tests COMMAND btp_aead_tests)

    add_executable(btp_aead_conformance_tests tests/test_aead_conformance.cpp)
    target_link_libraries(btp_aead_conformance_tests PRIVATE btp::aead)
    target_compile_features(btp_aead_conformance_tests PRIVATE cxx_std_11)
    target_compile_definitions(
        btp_aead_conformance_tests
        PRIVATE BTP_VECTOR_ROOT_V2="${CMAKE_CURRENT_SOURCE_DIR}/test-vectors/v2"
    )
    if(MSVC)
        target_compile_options(btp_aead_conformance_tests PRIVATE /W4 /WX)
    else()
        target_compile_options(btp_aead_conformance_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME btp_aead_conformance_tests COMMAND btp_aead_conformance_tests)
endif()
