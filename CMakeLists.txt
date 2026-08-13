cmake_minimum_required(VERSION 3.16)

project(btp VERSION 1.0.2 LANGUAGES CXX)

option(BTP_BUILD_TESTS "Build the BTP codec unit tests" ON)

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
    if(MSVC)
        target_compile_options(btp_codec_tests PRIVATE /W4 /WX)
        target_compile_options(btp_transport_tests PRIVATE /W4 /WX)
        target_compile_options(btp_conformance_tests PRIVATE /W4 /WX)
    else()
        target_compile_options(btp_codec_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
        target_compile_options(btp_transport_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
        target_compile_options(btp_conformance_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME btp_codec_tests COMMAND btp_codec_tests)
    add_test(NAME btp_transport_tests COMMAND btp_transport_tests)
    add_test(NAME btp_conformance_tests COMMAND btp_conformance_tests)
    add_test(
        NAME btp_vector_descriptions
        COMMAND
            ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_vectors.py
            --root ${CMAKE_CURRENT_SOURCE_DIR}/test-vectors/v1
            --check
    )
endif()
