add_executable(ovlextract src/main.cpp)

target_link_libraries(ovlextract
    PRIVATE
        ovlcore
        ovlextractors
)

target_include_directories(ovlextract
    PRIVATE
        ${CMAKE_SOURCE_DIR}/third_party/CLI11
)

target_compile_features(ovlextract PRIVATE cxx_std_17)
ovl_set_compile_warnings(ovlextract)
