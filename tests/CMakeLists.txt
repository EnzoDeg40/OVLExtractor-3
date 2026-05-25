include(FetchContent)

set(FETCHCONTENT_QUIET OFF)

FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.5.4
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(Catch2)

add_executable(ovl_tests
    test_endianness.cpp
    test_binary_reader.cpp
    test_header_parsing.cpp
)

target_link_libraries(ovl_tests
    PRIVATE
        ovlcore
        ovlextractors
        Catch2::Catch2WithMain
)

target_compile_features(ovl_tests PRIVATE cxx_std_17)
ovl_set_compile_warnings(ovl_tests)

target_compile_definitions(ovl_tests PRIVATE
    OVL_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
)

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)
catch_discover_tests(ovl_tests)
