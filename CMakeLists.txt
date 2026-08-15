cmake_minimum_required(VERSION 3.24)

project(three_d_tiles_clip_worker VERSION 0.1.7 LANGUAGES CXX)

option(CLIP_WORKER_BUILD_TESTS "Build unit tests" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(CURL CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
find_package(PROJ CONFIG REQUIRED)
find_package(WebP CONFIG REQUIRED)
find_package(draco CONFIG REQUIRED)
find_path(MAPBOX_EARCUT_INCLUDE_DIR mapbox/earcut.hpp REQUIRED)

add_library(clip_worker_core
    src/clip/b3dm_clipper.cpp
    src/client/curl_runtime.cpp
    src/client/worker_api_client.cpp
    src/client/object_transfer.cpp
    src/formats/b3dm/b3dm_parser.cpp
    src/formats/gltf/draco_decoder.cpp
    src/formats/gltf/glb_parser.cpp
    src/geometry/triangle_clipper.cpp
    src/geometry/authorization_scope.cpp
    src/geometry/authorization_triangle_index.cpp
    src/geometry/matrix4.cpp
    src/logging/logger.cpp
    src/task/task_contract.cpp
    src/task/worker_runtime.cpp
)

target_include_directories(clip_worker_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${MAPBOX_EARCUT_INCLUDE_DIR}
)

target_link_libraries(clip_worker_core
    PUBLIC
        CURL::libcurl
        nlohmann_json::nlohmann_json
        OpenSSL::Crypto
        Threads::Threads
        PROJ::proj
        WebP::webp
        WebP::sharpyuv
        draco::draco
)

if(MSVC)
    target_compile_options(clip_worker_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(clip_worker_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
endif()

add_executable(3d-tiles-clip-worker src/app/main.cpp)
target_link_libraries(3d-tiles-clip-worker PRIVATE clip_worker_core)

install(TARGETS 3d-tiles-clip-worker
    RUNTIME_DEPENDENCIES
        PRE_EXCLUDE_REGEXES "api-ms-.*" "ext-ms-.*"
        POST_EXCLUDE_REGEXES "^/lib/.*" "^/usr/lib/.*" "^/usr/lib64/.*"
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
)

if(CLIP_WORKER_BUILD_TESTS)
    include(CTest)
    enable_testing()
    find_package(GTest CONFIG REQUIRED)

    add_executable(clip_worker_tests
        tests/unit/b3dm_clipper_test.cpp
        tests/unit/b3dm_parser_test.cpp
        tests/unit/authorization_scope_test.cpp
        tests/unit/authorization_triangle_index_test.cpp
        tests/unit/glb_parser_test.cpp
        tests/unit/logger_test.cpp
        tests/unit/matrix4_test.cpp
        tests/unit/object_transfer_test.cpp
        tests/unit/task_contract_test.cpp
        tests/unit/triangle_clipper_test.cpp
        tests/support/synthetic_tile.cpp
    )
    target_include_directories(clip_worker_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/tests
    )
    target_link_libraries(clip_worker_tests PRIVATE clip_worker_core GTest::gtest_main)

    include(GoogleTest)
    gtest_discover_tests(clip_worker_tests)
endif()
