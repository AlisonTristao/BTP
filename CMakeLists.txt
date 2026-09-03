cmake_minimum_required(VERSION 3.16)

# The version number lives in one place -- include/btp/version.hpp -- so that
# the header the code sees, the number PlatformIO reads and project(VERSION)
# below can never drift apart. Parse it here rather than repeat it.
set(_btp_version_hpp "${CMAKE_CURRENT_SOURCE_DIR}/include/btp/version.hpp")
file(STRINGS "${_btp_version_hpp}" _btp_version_lines
     REGEX "kLibraryVersion(Major|Minor|Patch)[ \t]*=[ \t]*[0-9]+")
string(REGEX MATCH "kLibraryVersionMajor[ \t]*=[ \t]*([0-9]+)"
       _ "${_btp_version_lines}")
set(_btp_major "${CMAKE_MATCH_1}")
string(REGEX MATCH "kLibraryVersionMinor[ \t]*=[ \t]*([0-9]+)"
       _ "${_btp_version_lines}")
set(_btp_minor "${CMAKE_MATCH_1}")
string(REGEX MATCH "kLibraryVersionPatch[ \t]*=[ \t]*([0-9]+)"
       _ "${_btp_version_lines}")
set(_btp_patch "${CMAKE_MATCH_1}")
if(NOT _btp_major MATCHES "^[0-9]+$" OR NOT _btp_minor MATCHES "^[0-9]+$"
   OR NOT _btp_patch MATCHES "^[0-9]+$")
    message(FATAL_ERROR "cannot read the version from ${_btp_version_hpp}")
endif()

project(btp VERSION ${_btp_major}.${_btp_minor}.${_btp_patch} LANGUAGES CXX)

# library.json carries a copy of the version for PlatformIO, which never runs
# this file. Refuse to configure if the two disagree -- `python tools/version.py`
# is the one command that changes both, and CI runs it with --check.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/library.json" _btp_json_version_line
     REGEX "\"version\"[ \t]*:[ \t]*\"[0-9]+\\.[0-9]+\\.[0-9]+\"")
string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+"
       _btp_json_version "${_btp_json_version_line}")
if(NOT _btp_json_version STREQUAL PROJECT_VERSION)
    message(FATAL_ERROR
        "version mismatch: include/btp/version.hpp is ${PROJECT_VERSION}, "
        "library.json is ${_btp_json_version}. "
        "Run: python tools/version.py ${PROJECT_VERSION}")
endif()

# Tests are for developing BTP, not for consuming it. A parent project that
# pulls this in with add_subdirectory() or FetchContent should not inherit the
# test targets, and above all should not inherit the hard find_package(Python3)
# they require. CMake 3.21 has PROJECT_IS_TOP_LEVEL; this works on 3.16.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(btp_is_top_level ON)
else()
    set(btp_is_top_level OFF)
endif()

option(BTP_BUILD_TESTS "Build the BTP test suites" ${btp_is_top_level})
option(BTP_ENABLE_AEAD "Build the btp::aead backend (needs mbedtls)" ON)
option(BTP_INSTALL "Generate install rules for BTP" ${btp_is_top_level})
option(BTP_USE_SYSTEM_MBEDTLS
       "Use an mbedtls found on the system instead of fetching one" OFF)

# The same warning configuration was repeated once per target. One function
# means a flag added here reaches every target, which is the point of having
# -Werror at all.
function(btp_strict_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
endfunction()

# ---------------------------------------------------------------------------
# btp::codec -- the envelope, fragmentation and COBS. Zero dependencies.
# ---------------------------------------------------------------------------

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
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_compile_features(btp_codec PUBLIC cxx_std_11)
btp_strict_warnings(btp_codec)

# Without this the export namespace yields btp::btp_codec while the in-tree
# alias above is btp::codec, so a consumer's find_package(btp) succeeds and
# then fails to find the target it was told to link.
set_target_properties(btp_codec PROPERTIES EXPORT_NAME codec)

# ---------------------------------------------------------------------------
# btp::messages -- struct <-> bytes for every COMMAND / CONTROL payload
# (docs/commands.md, docs/session-and-terminal.md). Same guarantees as
# btp::codec: no allocation, noexcept, caller-owned buffers. Links btp::codec
# for ByteView and the shared little-endian helpers; NO mbedtls, so it is
# always built, like btp::codec and unlike btp::aead.
# ---------------------------------------------------------------------------

add_library(btp_messages src/messages.cpp)
add_library(btp::messages ALIAS btp_messages)

target_include_directories(
    btp_messages
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_compile_features(btp_messages PUBLIC cxx_std_11)
target_link_libraries(btp_messages PUBLIC btp::codec)
btp_strict_warnings(btp_messages)
set_target_properties(btp_messages PROPERTIES EXPORT_NAME messages)

# ---------------------------------------------------------------------------
# btp::telemetry -- the TELEMETRY body codec (PACKED_LE / TLV_LE), the nullable
# bitmap and the engineering conversion, against a schema the caller holds
# (docs/telemetry.md). btp::messages decodes the manifest that DESCRIBES the
# schema; this decodes a sample AGAINST it. Same guarantees, NO mbedtls, always
# built.
# ---------------------------------------------------------------------------

add_library(btp_telemetry src/telemetry.cpp)
add_library(btp::telemetry ALIAS btp_telemetry)

target_include_directories(
    btp_telemetry
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_compile_features(btp_telemetry PUBLIC cxx_std_11)
target_link_libraries(btp_telemetry PUBLIC btp::messages)
btp_strict_warnings(btp_telemetry)
set_target_properties(btp_telemetry PROPERTIES EXPORT_NAME telemetry)

# ---------------------------------------------------------------------------
# btp::session -- the stateful half of a BTP session that docs/library.md
# chapter 11 keeps above the payload layer, starting with btp::DedupCache: the
# command-deduplication cache of docs/commands.md 2.5-2.6 (bounded, boot-scoped,
# replay the stored COMMAND_RESULT instead of executing an action twice). Same
# guarantees as the rest of the library, NO mbedtls, always built. Links
# btp::messages for MessageError and the shared enums.
# ---------------------------------------------------------------------------

add_library(btp_session src/session.cpp)
add_library(btp::session ALIAS btp_session)

target_include_directories(
    btp_session
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_compile_features(btp_session PUBLIC cxx_std_11)
target_link_libraries(btp_session PUBLIC btp::messages)
btp_strict_warnings(btp_session)
set_target_properties(btp_session PROPERTIES EXPORT_NAME session)

# ---------------------------------------------------------------------------
# btp::endpoint -- the transmit side of a BTP endpoint (docs/library.md chapter
# 14): the local identity, the outgoing sequence counter and the
# seal -> fragment -> encode pipeline every producer runs. Same guarantees as
# btp::codec: no allocation, noexcept, caller-owned buffers. Links btp::codec
# for the frame codec and fragmentation; NO mbedtls (sealing is a caller
# callback), so it is always built.
# ---------------------------------------------------------------------------

add_library(btp_endpoint src/endpoint.cpp)
add_library(btp::endpoint ALIAS btp_endpoint)

target_include_directories(
    btp_endpoint
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_compile_features(btp_endpoint PUBLIC cxx_std_11)
target_link_libraries(btp_endpoint PUBLIC btp::codec)
btp_strict_warnings(btp_endpoint)
set_target_properties(btp_endpoint PROPERTIES EXPORT_NAME endpoint)

# ---------------------------------------------------------------------------
# btp::receiver -- the receive side of a BTP endpoint (docs/library.md chapter
# 15): btp::decode + CRC + btp::Reassembler wired into "one datagram in, one
# whole logical message out or nothing yet", with the timeout sweep and the
# STATUS counters. The mirror of btp::endpoint. Same guarantees as btp::codec;
# NO mbedtls (opening an encrypted payload is above this), so it is always built.
# ---------------------------------------------------------------------------

add_library(btp_receiver src/receiver.cpp)
add_library(btp::receiver ALIAS btp_receiver)

target_include_directories(
    btp_receiver
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_compile_features(btp_receiver PUBLIC cxx_std_11)
target_link_libraries(btp_receiver PUBLIC btp::codec)
btp_strict_warnings(btp_receiver)
set_target_properties(btp_receiver PROPERTIES EXPORT_NAME receiver)

# ---------------------------------------------------------------------------
# btp::catalog -- the schema catalogue btp::Node keeps current: the TELEMETRY
# topics a producer exposes, or the ones a consumer learned from a
# MANIFEST_DATA (docs/commands.md section 3, docs/library.md section 11.2). The
# ManifestReader walk and the FieldSpec cache, with the storage still the
# caller's. Links btp::telemetry (FieldSpec, field_spec) which brings
# btp::messages; NO mbedtls, always built.
# ---------------------------------------------------------------------------

add_library(btp_catalog src/catalog.cpp)
add_library(btp::catalog ALIAS btp_catalog)

target_include_directories(
    btp_catalog
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_compile_features(btp_catalog PUBLIC cxx_std_11)
target_link_libraries(btp_catalog PUBLIC btp::telemetry)
btp_strict_warnings(btp_catalog)
set_target_properties(btp_catalog PROPERTIES EXPORT_NAME catalog)

# ---------------------------------------------------------------------------
# btp::subscription -- docs/commands.md section 4 (SUBSCRIBE / SUBSCRIBE_RESULT
# / UNSUBSCRIBE) as state above the wire: btp::SubscriptionTable (the
# responder, granting subscriptions against a btp::Catalog) and
# btp::SubscriptionClient (the initiator, holding and renewing them). Links
# btp::catalog (topic validation, max_rate_millihz); NO mbedtls, always built.
# ---------------------------------------------------------------------------

add_library(btp_subscription src/subscription.cpp)
add_library(btp::subscription ALIAS btp_subscription)

target_include_directories(
    btp_subscription
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_compile_features(btp_subscription PUBLIC cxx_std_11)
target_link_libraries(btp_subscription PUBLIC btp::catalog)
btp_strict_warnings(btp_subscription)
set_target_properties(btp_subscription PROPERTIES EXPORT_NAME subscription)

# ---------------------------------------------------------------------------
# btp::node -- the friendly facade (docs/library.md chapter 16): btp::Endpoint,
# btp::Receiver and (opt-in) btp::Session in one object, sharing one identity,
# one clock notion and one set of caller-owned buffers, with every external
# dependency a C function pointer. Adds NO wire field. Links the layers it
# wires; NO mbedtls (seal / open are caller callbacks), so it is always built.
# ---------------------------------------------------------------------------

add_library(btp_node src/node.cpp)
add_library(btp::node ALIAS btp_node)

target_include_directories(
    btp_node
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_compile_features(btp_node PUBLIC cxx_std_11)
target_link_libraries(btp_node PUBLIC btp::endpoint btp::receiver btp::session
                                      btp::catalog btp::subscription)
btp_strict_warnings(btp_node)
set_target_properties(btp_node PROPERTIES EXPORT_NAME node)

# ---------------------------------------------------------------------------
# btp::aead -- optional wire v2 encryption, on top of mbedtls.
# ---------------------------------------------------------------------------

if(BTP_ENABLE_AEAD)
    # Three ways to get mbedtls, in order of preference. Fetching it is the
    # fallback, not the rule: it makes configuring require network access, and
    # it silently duplicates a copy a parent project may already have.
    if(TARGET mbedcrypto)
        set(btp_mbedtls_source "target provided by the parent project")
        set(btp_mbedtls_is_external ON)
    elseif(BTP_USE_SYSTEM_MBEDTLS)
        find_package(MbedTLS REQUIRED)
        set(btp_mbedtls_source "system package")
        set(btp_mbedtls_is_external ON)
    else()
        include(FetchContent)
        # EXCLUDE_FROM_ALL keeps mbedtls's own install rules from running.
        # Without it, `cmake --install` on this project deposits an entire TLS
        # stack -- every mbedtls, psa and everest header, plus five static
        # libraries -- into the consumer's prefix next to four BTP headers. We
        # fetched it as a private implementation detail; it should not show up
        # in anyone's install tree. The keyword landed in CMake 3.28, and the
        # options below are the fallback for older ones.
        if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.28)
            FetchContent_Declare(
                mbedtls
                GIT_REPOSITORY https://github.com/Mbed-TLS/mbedtls.git
                GIT_TAG v3.6.7 # newest 3.6.x LTS at time of pinning; a tag, never a branch
                EXCLUDE_FROM_ALL
            )
        else()
            FetchContent_Declare(
                mbedtls
                GIT_REPOSITORY https://github.com/Mbed-TLS/mbedtls.git
                GIT_TAG v3.6.7
            )
        endif()
        # mbedtls defaults to building its own test suite and sample programs,
        # and to treating its own warnings as errors. None of that should gate
        # or slow our build, so force it off before mbedtls configures.
        set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
        set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
        set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
        set(DISABLE_PACKAGE_CONFIG_AND_INSTALL ON CACHE BOOL "" FORCE)
        set(INSTALL_MBEDTLS_HEADERS OFF CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(mbedtls)
        set(btp_mbedtls_source "fetched at configure time")
        set(btp_mbedtls_is_external OFF)
    endif()
    message(STATUS "BTP: mbedtls -- ${btp_mbedtls_source}")

    add_library(btp_aead src/aead.cpp)
    add_library(btp::aead ALIAS btp_aead)

    target_include_directories(
        btp_aead
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    )
    target_compile_features(btp_aead PUBLIC cxx_std_11)
    target_link_libraries(btp_aead PUBLIC btp::codec PRIVATE mbedcrypto)
    btp_strict_warnings(btp_aead)
    set_target_properties(btp_aead PROPERTIES EXPORT_NAME aead)
    # aead.cpp compiles to nothing when it cannot find mbedtls, so that a
    # PlatformIO build on a board without it still succeeds. Here we did ask
    # for it, so a missing header must be an error rather than an empty target
    # that fails at link time with a confusing message.
    target_compile_definitions(btp_aead PRIVATE BTP_AEAD_REQUIRE_MBEDTLS=1)
endif()

# ---------------------------------------------------------------------------
# Install and export
# ---------------------------------------------------------------------------

if(BTP_INSTALL)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)

    set(btp_export_targets btp_codec btp_messages btp_telemetry btp_session
                           btp_endpoint btp_receiver btp_catalog
                           btp_subscription btp_node)

    # btp_aead joins the export set only when mbedtls came from outside this
    # build. A fetched mbedcrypto is never installed, so exporting a target
    # that links it would hand the consumer a btpTargets.cmake referencing a
    # target that does not exist on their machine -- a install that configures
    # and then fails at link time is worse than one that never claimed to work.
    if(BTP_ENABLE_AEAD AND btp_mbedtls_is_external)
        list(APPEND btp_export_targets btp_aead)
    elseif(BTP_ENABLE_AEAD)
        message(STATUS
            "BTP: btp::aead is excluded from the install set because mbedtls "
            "was fetched; configure with -DBTP_USE_SYSTEM_MBEDTLS=ON to "
            "install it")
    endif()

    install(
        TARGETS ${btp_export_targets}
        EXPORT btpTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
    install(
        DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/btp
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )
    install(
        EXPORT btpTargets
        FILE btpTargets.cmake
        NAMESPACE btp::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/btp
    )

    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/btpConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
    )
    configure_package_config_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/btpConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/btpConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/btp
    )
    install(
        FILES
            ${CMAKE_CURRENT_BINARY_DIR}/btpConfig.cmake
            ${CMAKE_CURRENT_BINARY_DIR}/btpConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/btp
    )
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

if(BTP_BUILD_TESTS)
    enable_testing()
    find_package(Python3 COMPONENTS Interpreter REQUIRED)

    # name -> source, all linking btp::codec with the same options.
    foreach(suite codec transport conformance conformance_v2)
        add_executable(btp_${suite}_tests tests/test_${suite}.cpp)
        target_link_libraries(btp_${suite}_tests PRIVATE btp::codec)
        target_compile_features(btp_${suite}_tests PRIVATE cxx_std_11)
        btp_strict_warnings(btp_${suite}_tests)
        add_test(NAME btp_${suite}_tests COMMAND btp_${suite}_tests)
    endforeach()

    # btp::messages has its own suite: it links btp::messages (not just
    # btp::codec) and white-box-includes src/messages_detail.hpp for the cursor
    # tests, then checks every fixed payload against test-vectors/v2/messages/.
    add_executable(btp_messages_tests tests/test_messages.cpp)
    target_link_libraries(btp_messages_tests PRIVATE btp::messages)
    target_compile_features(btp_messages_tests PRIVATE cxx_std_11)
    btp_strict_warnings(btp_messages_tests)
    target_compile_definitions(
        btp_messages_tests
        PRIVATE BTP_VECTOR_ROOT_V2="${CMAKE_CURRENT_SOURCE_DIR}/test-vectors/v2"
    )
    add_test(NAME btp_messages_tests COMMAND btp_messages_tests)

    # btp::telemetry: links btp::telemetry, white-box-includes the shared cursor
    # for its own detail tests, checks the sample codec against
    # test-vectors/v2/telemetry/.
    add_executable(btp_telemetry_tests tests/test_telemetry.cpp)
    target_link_libraries(btp_telemetry_tests PRIVATE btp::telemetry)
    target_compile_features(btp_telemetry_tests PRIVATE cxx_std_11)
    btp_strict_warnings(btp_telemetry_tests)
    target_compile_definitions(
        btp_telemetry_tests
        PRIVATE BTP_VECTOR_ROOT_V2="${CMAKE_CURRENT_SOURCE_DIR}/test-vectors/v2"
    )
    add_test(NAME btp_telemetry_tests COMMAND btp_telemetry_tests)

    # btp::session: the deduplication cache is behaviour with state, not wire
    # layout, so it has no vector tree -- the golden sequences of
    # docs/commands.md 2.5-2.6 live in this suite instead.
    add_executable(btp_session_tests tests/test_session.cpp)
    target_link_libraries(btp_session_tests PRIVATE btp::session)
    target_compile_features(btp_session_tests PRIVATE cxx_std_11)
    btp_strict_warnings(btp_session_tests)
    add_test(NAME btp_session_tests COMMAND btp_session_tests)

    # btp::endpoint: the transmit pipeline is orchestration over the codec, not
    # a wire layout, so it has no vector tree -- the suite drives the endpoint
    # and decodes what it produced with the public btp::decode / btp::Reassembler.
    add_executable(btp_endpoint_tests tests/test_endpoint.cpp)
    target_link_libraries(btp_endpoint_tests PRIVATE btp::endpoint)
    target_compile_features(btp_endpoint_tests PRIVATE cxx_std_11)
    btp_strict_warnings(btp_endpoint_tests)
    add_test(NAME btp_endpoint_tests COMMAND btp_endpoint_tests)

    # btp::receiver: decode + reassembly wiring, driven by feeding it frames
    # (built with btp::endpoint, so the RX/TX pair is exercised together) and
    # checking the outcome and the reconstructed payload. No vector tree.
    add_executable(btp_receiver_tests tests/test_receiver.cpp)
    target_link_libraries(btp_receiver_tests PRIVATE btp::receiver btp::endpoint)
    target_compile_features(btp_receiver_tests PRIVATE cxx_std_11)
    btp_strict_warnings(btp_receiver_tests)
    add_test(NAME btp_receiver_tests COMMAND btp_receiver_tests)

    # btp::catalog: the schema catalogue is state above btp::messages, not a
    # wire layout -- the suite fills a Catalog, round-trips it through a real
    # MANIFEST_DATA (ManifestWriter -> ingest), and checks the topics come back.
    add_executable(btp_catalog_tests tests/test_catalog.cpp)
    target_link_libraries(btp_catalog_tests PRIVATE btp::catalog)
    target_compile_features(btp_catalog_tests PRIVATE cxx_std_11)
    btp_strict_warnings(btp_catalog_tests)
    add_test(NAME btp_catalog_tests COMMAND btp_catalog_tests)

    # btp::subscription: SUBSCRIBE / SUBSCRIBE_RESULT / UNSUBSCRIBE lifecycle
    # is behaviour with state, not wire layout -- no vector tree, the golden
    # sequences of docs/commands.md section 4 live in this suite instead.
    add_executable(btp_subscription_tests tests/test_subscription.cpp)
    target_link_libraries(btp_subscription_tests PRIVATE btp::subscription)
    target_compile_features(btp_subscription_tests PRIVATE cxx_std_11)
    btp_strict_warnings(btp_subscription_tests)
    add_test(NAME btp_subscription_tests COMMAND btp_subscription_tests)

    # btp::node: the facade is wiring over endpoint + receiver + session, not a
    # wire layout, so it has no vector tree -- the suite drives a Node (often a
    # pair) and checks the frames, the reassembled payloads and the session
    # replies with the public btp:: APIs.
    add_executable(btp_node_tests tests/test_node.cpp)
    target_link_libraries(btp_node_tests PRIVATE btp::node)
    target_compile_features(btp_node_tests PRIVATE cxx_std_11)
    btp_strict_warnings(btp_node_tests)
    add_test(NAME btp_node_tests COMMAND btp_node_tests)

    target_compile_definitions(
        btp_conformance_tests
        PRIVATE BTP_VECTOR_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/test-vectors/v1"
    )
    target_compile_definitions(
        btp_conformance_v2_tests
        PRIVATE BTP_VECTOR_ROOT_V2="${CMAKE_CURRENT_SOURCE_DIR}/test-vectors/v2"
    )

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
    add_test(
        NAME btp_message_vectors
        COMMAND
            ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_messages.py
            --root ${CMAKE_CURRENT_SOURCE_DIR}/test-vectors/v2/messages
            --check
    )
    add_test(
        NAME btp_telemetry_vectors
        COMMAND
            ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_telemetry.py
            --root ${CMAKE_CURRENT_SOURCE_DIR}/test-vectors/v2/telemetry
            --check
    )

    if(BTP_ENABLE_AEAD)
        foreach(suite aead aead_conformance)
            add_executable(btp_${suite}_tests tests/test_${suite}.cpp)
            target_link_libraries(btp_${suite}_tests PRIVATE btp::aead)
            target_compile_features(btp_${suite}_tests PRIVATE cxx_std_11)
            btp_strict_warnings(btp_${suite}_tests)
            add_test(NAME btp_${suite}_tests COMMAND btp_${suite}_tests)
        endforeach()

        target_compile_definitions(
            btp_aead_conformance_tests
            PRIVATE BTP_VECTOR_ROOT_V2="${CMAKE_CURRENT_SOURCE_DIR}/test-vectors/v2"
        )
    endif()
endif()
