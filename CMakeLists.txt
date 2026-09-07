cmake_minimum_required(VERSION 3.24)

project(three_d_tiles_clip_worker VERSION 0.1.9 LANGUAGES CXX)

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
find_package(PNG REQUIRED)
find_package(JPEG REQUIRED)
find_package(libjpeg-turbo CONFIG REQUIRED)
find_package(Ktx CONFIG REQUIRED)
find_package(draco CONFIG REQUIRED)
find_package(meshoptimizer CONFIG REQUIRED)
find_package(zstd CONFIG REQUIRED)
find_package(libzip CONFIG REQUIRED)
find_package(uriparser CONFIG REQUIRED)
find_package(unofficial-utf8proc CONFIG REQUIRED)
find_path(MAPBOX_EARCUT_INCLUDE_DIR mapbox/earcut.hpp REQUIRED)

add_library(clip_worker_core
    src/authorization/authorization_clip_proof.cpp
    src/authorization/clipper_v2_contract.cpp
    src/authorization/clipper_v2_executor.cpp
    src/authorization/clipper_v2_runtime.cpp
    src/clip/authorization_content_classifier.cpp
    src/clip/b3dm_clipper.cpp
    src/clip/canonical_mesh_clip_strategy.cpp
    src/clip/canonical_point_clip_strategy.cpp
    src/clip/canonical_instance_clip_strategy.cpp
    src/clip/instance_bounds_classifier.cpp
    src/clip/mesh_scene_clipper.cpp
    src/clip/point_scene_clipper.cpp
    src/clip/texture_masker.cpp
    src/client/curl_runtime.cpp
    src/client/clipper_v2_api_client.cpp
    src/client/inspection_api_client.cpp
    src/client/normalization_api_client.cpp
    src/client/normalization_v2_api_client.cpp
    src/client/worker_api_client.cpp
    src/client/object_transfer.cpp
    src/formats/b3dm/b3dm_parser.cpp
    src/formats/cmpt/cmpt_parser.cpp
    src/formats/b3dm/b3dm_mesh_adapter.cpp
    src/formats/i3dm/i3dm_parser.cpp
    src/formats/pnts/pnts_parser.cpp
    src/formats/gltf/draco_decoder.cpp
    src/formats/gltf/glb_parser.cpp
    src/formats/gltf/gltf_mesh_reader.cpp
    src/formats/gltf/meshopt_decoder.cpp
    src/formats/image/texture_codec.cpp
    src/geometry/triangle_clipper.cpp
    src/geometry/authorization_scope.cpp
    src/geometry/authorization_triangle_index.cpp
    src/geometry/matrix4.cpp
    src/inspection/inspection_contract.cpp
    src/inspection/package/package_enumerator.cpp
    src/inspection/pipeline/inspection_pipeline.cpp
    src/inspection/pipeline/expansion_uploader.cpp
    src/inspection/inspection_runtime.cpp
    src/inspection/inspection_scheduling.cpp
    src/logging/logger.cpp
    src/instance/instance_scene.cpp
    src/metadata/legacy_property_table.cpp
    src/metadata/feature_metadata.cpp
    src/metadata/metadata_capability_matrix.cpp
    src/metadata/legacy_feature_metadata.cpp
    src/metadata/property_lookup.cpp
    src/metadata/structural_metadata_reader.cpp
    src/mesh/mesh_scene.cpp
    src/normalization/canonical_artifact.cpp
    src/normalization/canonical_input_reader.cpp
    src/normalization/broad_resource_profile.cpp
    src/normalization/broad_normalization_executor.cpp
    src/normalization/composite_normalizer.cpp
    src/normalization/mesh_canonical_writer.cpp
    src/normalization/metadata_canonical_writer.cpp
    src/normalization/mesh_normalizer.cpp
    src/normalization/mesh_normalization_executor.cpp
    src/normalization/mesh_resource_profile.cpp
    src/normalization/instance_canonical_writer.cpp
    src/normalization/instance_normalizer.cpp
    src/normalization/point_canonical_writer.cpp
    src/normalization/point_normalizer.cpp
    src/normalization/normalization_contract.cpp
    src/normalization/normalization_runtime.cpp
    src/normalization/normalization_v2_contract.cpp
    src/normalization/normalization_v2_runtime.cpp
    src/task/task_contract.cpp
    src/task/worker_runtime.cpp
    src/point/point_scene.cpp
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
        PNG::PNG
        JPEG::JPEG
        $<IF:$<TARGET_EXISTS:libjpeg-turbo::turbojpeg>,libjpeg-turbo::turbojpeg,libjpeg-turbo::turbojpeg-static>
        KTX::ktx
        draco::draco
        meshoptimizer::meshoptimizer
        libzip::zip
        uriparser::uriparser
        utf8proc
)

if(MSVC)
    target_compile_options(clip_worker_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(clip_worker_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
endif()

add_executable(3d-tiles-clip-worker src/app/main.cpp)
target_compile_definitions(3d-tiles-clip-worker PRIVATE
    CLIP_WORKER_VERSION="${PROJECT_VERSION}"
)
target_link_libraries(3d-tiles-clip-worker PRIVATE clip_worker_core)

install(TARGETS 3d-tiles-clip-worker
    RUNTIME_DEPENDENCIES
        PRE_EXCLUDE_REGEXES "api-ms-.*" "ext-ms-.*"
        POST_EXCLUDE_REGEXES "^/lib/.*" "^/usr/lib/.*" "^/usr/lib64/.*"
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
)

install(FILES
    config/clipper-protocol-v2.schema.json
    config/normalizer-protocol-v1.schema.json
    config/normalizer-protocol-v2.schema.json
    config/normalizer-protocol-v3.schema.json
    config/metadata-capability-matrix-v1.json
    config/resource-limit-profiles-v1.json
    config/resource-limit-profiles-v2.json
    config/resource-limit-profiles-v3.json
    config/resource-limit-profiles-v4.json
    DESTINATION share/clip-worker
)

if(CLIP_WORKER_BUILD_TESTS)
    include(CTest)
    enable_testing()
    find_package(GTest CONFIG REQUIRED)

    add_executable(clip_worker_tests
        tests/unit/b3dm_clipper_test.cpp
        tests/unit/b3dm_mesh_adapter_test.cpp
        tests/unit/b3dm_parser_test.cpp
        tests/unit/broad_official_corpus_test.cpp
        tests/unit/broad_resource_profile_test.cpp
        tests/unit/canonical_mesh_clip_strategy_test.cpp
        tests/unit/canonical_instance_clip_strategy_test.cpp
        tests/unit/canonical_input_reader_test.cpp
        tests/unit/clipper_v2_contract_test.cpp
        tests/unit/clipper_v2_runtime_test.cpp
        tests/unit/cmpt_parser_test.cpp
        tests/unit/composite_normalizer_test.cpp
        tests/unit/authorization_scope_test.cpp
        tests/unit/authorization_content_classifier_test.cpp
        tests/unit/authorization_triangle_index_test.cpp
        tests/unit/glb_parser_test.cpp
        tests/unit/i3dm_parser_test.cpp
        tests/unit/gltf_mesh_reader_test.cpp
        tests/unit/inspection_contract_test.cpp
        tests/unit/expansion_uploader_test.cpp
        tests/unit/inspection_pipeline_test.cpp
        tests/unit/package_enumerator_test.cpp
        tests/unit/pnts_parser_test.cpp
        tests/unit/point_normalizer_test.cpp
        tests/unit/point_scene_clipper_test.cpp
        tests/unit/inspection_runtime_test.cpp
        tests/unit/instance_normalizer_test.cpp
        tests/unit/logger_test.cpp
        tests/unit/matrix4_test.cpp
        tests/unit/feature_metadata_test.cpp
        tests/unit/legacy_feature_metadata_test.cpp
        tests/unit/structural_metadata_reader_test.cpp
        tests/unit/metadata_capability_matrix_test.cpp
        tests/unit/metadata_canonical_writer_test.cpp
        tests/unit/metadata_semantic_hash_test.cpp
        tests/unit/mesh_scene_clipper_test.cpp
        tests/unit/mesh_scene_test.cpp
        tests/unit/mesh_resource_profile_test.cpp
        tests/unit/mesh_normalizer_test.cpp
        tests/unit/mesh_normalization_executor_test.cpp
        tests/unit/meshopt_decoder_test.cpp
        tests/unit/normalization_contract_test.cpp
        tests/unit/normalization_runtime_test.cpp
        tests/unit/normalization_v2_contract_test.cpp
        tests/unit/normalization_v2_runtime_test.cpp
        tests/unit/object_transfer_test.cpp
        tests/unit/task_contract_test.cpp
        tests/unit/texture_codec_test.cpp
        tests/unit/texture_masker_test.cpp
        tests/unit/triangle_clipper_test.cpp
        tests/support/synthetic_tile.cpp
        tests/support/synthetic_pnts.cpp
        tests/support/synthetic_i3dm.cpp
        tests/support/synthetic_cmpt.cpp
    )
    target_include_directories(clip_worker_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/tests
    )
    target_compile_definitions(clip_worker_tests PRIVATE
        CLIP_WORKER_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(clip_worker_tests PRIVATE clip_worker_core GTest::gtest_main)

    include(GoogleTest)
    gtest_discover_tests(clip_worker_tests)

    add_executable(metadata_privacy_corpus_runner
        tests/corpus/metadata_privacy_corpus_runner.cpp
    )
    target_compile_definitions(metadata_privacy_corpus_runner PRIVATE
        CLIP_WORKER_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(metadata_privacy_corpus_runner PRIVATE clip_worker_core)
    add_test(NAME MetadataPrivacyOfficialCorpus
        COMMAND metadata_privacy_corpus_runner
    )
    set_tests_properties(MetadataPrivacyOfficialCorpus PROPERTIES
        SKIP_RETURN_CODE 77
    )
endif()
