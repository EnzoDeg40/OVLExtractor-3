add_library(ovlextractors STATIC
    src/AtlasExtractor.cpp
    src/DataExtractor.cpp
    src/DataExtractor_Anims.cpp
    src/DataExtractor_Items.cpp
    src/DataExtractor_Paths.cpp
    src/DataExtractor_People.cpp
    src/DataExtractor_Scenery.cpp
    src/DumpExtractor.cpp
    src/Layout.cpp
    src/FontExtractor.cpp
    src/GltfWriter.cpp
    src/ModelExtractor.cpp
    src/SceneryExtractor.cpp
    src/ShapeReader.cpp
    src/SoundExtractor.cpp
    src/TerrainExtractor.cpp
    src/PeopleExtractor.cpp
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
