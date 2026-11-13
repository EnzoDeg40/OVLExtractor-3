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

add_executable(ovl_roundtrip src/ovl_roundtrip.cpp)
target_link_libraries(ovl_roundtrip PRIVATE ovlcore)
target_compile_features(ovl_roundtrip PRIVATE cxx_std_17)
ovl_set_compile_warnings(ovl_roundtrip)

add_executable(ovlwrite src/ovlwrite.cpp)
target_link_libraries(ovlwrite PRIVATE ovlcore)
target_compile_features(ovlwrite PRIVATE cxx_std_17)
ovl_set_compile_warnings(ovlwrite)

add_executable(ovl_scenery_check src/ovl_scenery_check.cpp)
target_link_libraries(ovl_scenery_check PRIVATE ovlcore)
target_compile_features(ovl_scenery_check PRIVATE cxx_std_17)
ovl_set_compile_warnings(ovl_scenery_check)

add_executable(ovl_survey src/ovl_survey.cpp)
target_link_libraries(ovl_survey PRIVATE ovlcore)
target_compile_features(ovl_survey PRIVATE cxx_std_17)
ovl_set_compile_warnings(ovl_survey)

add_executable(ovl_layout_check src/ovl_layout_check.cpp)
target_link_libraries(ovl_layout_check PRIVATE ovlextractors)
target_compile_features(ovl_layout_check PRIVATE cxx_std_17)
ovl_set_compile_warnings(ovl_layout_check)
