add_library(ovlextractors STATIC
    src/AtlasExtractor.cpp
    src/DumpExtractor.cpp
    src/FontExtractor.cpp
    src/GltfWriter.cpp
    src/ModelExtractor.cpp
    src/SceneryExtractor.cpp
    src/ShapeReader.cpp
    src/SoundExtractor.cpp
    src/TextExtractor.cpp
    src/TextureExtractor.cpp
    src/TextureIndex.cpp
    src/TrackExtractor.cpp
    src/TrackReader.cpp
)

target_include_directories(ovlextractors
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(ovlextractors PUBLIC ovlcore)
target_compile_features(ovlextractors PUBLIC cxx_std_17)
ovl_set_compile_warnings(ovlextractors)

set_target_properties(ovlextractors PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)
