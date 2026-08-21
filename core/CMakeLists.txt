add_library(ovlcore STATIC
    src/BinaryReader.cpp
    src/OvlChecksum.cpp
    src/OvlParser.cpp
)

target_include_directories(ovlcore
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(ovlcore PUBLIC cxx_std_17)
ovl_set_compile_warnings(ovlcore)

set_target_properties(ovlcore PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)
