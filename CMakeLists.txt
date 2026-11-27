cmake_minimum_required(VERSION 3.24)
project(CLab VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CTest)

option(CLAB_BUILD_SUBSTRATES "Build the tiny reference substrate and mock runner" ON)
set(CLAB_JSON_HEADER "${CMAKE_CURRENT_BINARY_DIR}/deps/nlohmann/json.hpp"
    CACHE FILEPATH "Pinned nlohmann/json 3.11.3 single header")
set(json_sha 9bea4c8066ef4a1c206b2be5a36302f8926f7fdc6087af5d20b417d0cf103ea6)
if(NOT EXISTS "${CLAB_JSON_HEADER}")
    file(DOWNLOAD
        https://raw.githubusercontent.com/nlohmann/json/v3.11.3/single_include/nlohmann/json.hpp
        "${CLAB_JSON_HEADER}.download" EXPECTED_HASH SHA256=${json_sha} TLS_VERIFY ON
        STATUS download_status)
    list(GET download_status 0 download_code)
    if(NOT download_code EQUAL 0)
        message(FATAL_ERROR "Unable to obtain pinned JSON header: ${download_status}")
    endif()
    file(RENAME "${CLAB_JSON_HEADER}.download" "${CLAB_JSON_HEADER}")
endif()
file(SHA256 "${CLAB_JSON_HEADER}" actual_json_sha)
if(NOT actual_json_sha STREQUAL json_sha)
    message(FATAL_ERROR "Unexpected SHA-256 for ${CLAB_JSON_HEADER}")
endif()
configure_file("${CLAB_JSON_HEADER}" "${CMAKE_CURRENT_BINARY_DIR}/private/nlohmann/json.hpp" COPYONLY)

function(clab_library name)
    add_library(${name} STATIC ${ARGN})
    add_library(clab::${name} ALIAS ${name})
    target_compile_features(${name} PUBLIC cxx_std_20)
    target_include_directories(${name} PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${name} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    install(TARGETS ${name} EXPORT CLabTargets)
endfunction()

clab_library(foundation src/foundation.cpp)
clab_library(observation src/observation.cpp)
target_link_libraries(observation PUBLIC foundation)
clab_library(trace src/trace.cpp)
target_link_libraries(trace PUBLIC observation)
target_include_directories(trace SYSTEM PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/private")
clab_library(evaluator src/evaluator.cpp src/dependencies.cpp src/histogram_cache.cpp)
target_link_libraries(evaluator PUBLIC observation)

add_executable(clab-eval apps/eval.cpp)
target_link_libraries(clab-eval PRIVATE evaluator trace)
add_executable(clab-inspect apps/inspect.cpp apps/checkpoint.cpp)
target_link_libraries(clab-inspect PRIVATE trace)
if(NOT APPLE)
    find_package(OpenSSL REQUIRED COMPONENTS Crypto)
    target_compile_definitions(clab-inspect PRIVATE CLAB_USE_OPENSSL)
    target_link_libraries(clab-inspect PRIVATE OpenSSL::Crypto)
endif()
install(TARGETS clab-eval clab-inspect)

if(CLAB_BUILD_SUBSTRATES)
    clab_library(substrate_api src/substrate.cpp)
    target_link_libraries(substrate_api PUBLIC observation)
    add_executable(clab-fixture apps/fixture.cpp)
    target_link_libraries(clab-fixture PRIVATE substrate_api trace)
    install(TARGETS clab-fixture)
    install(FILES include/clab/substrate.hpp DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/clab")
endif()

install(DIRECTORY include/clab DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    PATTERN substrate.hpp EXCLUDE)
install(FILES docs/cltr-v1.md docs/G0-closeout.md DESTINATION "${CMAKE_INSTALL_DOCDIR}")
install(EXPORT CLabTargets NAMESPACE clab:: DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/CLab")
configure_package_config_file(cmake/CLabConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/CLabConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/CLab")
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/CLabConfigVersion.cmake"
    COMPATIBILITY SameMinorVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/CLabConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/CLabConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/CLab")

if(BUILD_TESTING)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_executable(clab-tests tests/core.cpp)
    target_link_libraries(clab-tests PRIVATE trace evaluator)
    if(CLAB_BUILD_SUBSTRATES)
        target_link_libraries(clab-tests PRIVATE substrate_api)
        target_compile_definitions(clab-tests PRIVATE CLAB_TEST_SUBSTRATES)
        set(fixture_arg --fixture $<TARGET_FILE:clab-fixture>)
    endif()
    add_test(NAME core COMMAND clab-tests)
    add_test(NAME independent-fixtures COMMAND "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/tests/independent.py"
        --eval $<TARGET_FILE:clab-eval> --inspect $<TARGET_FILE:clab-inspect>
        ${fixture_arg})
endif()
