find_package(Eigen3 CONFIG REQUIRED)

add_library(gm-geometry STATIC
  src/correlation.cpp
  src/shrinkage.cpp
  src/rmt.cpp
  src/distance.cpp
  src/mds.cpp
  src/procrustes.cpp
)
add_library(gm::geometry ALIAS gm-geometry)

target_include_directories(gm-geometry
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(gm-geometry
  PUBLIC
    gm::core
    Eigen3::Eigen
)

target_compile_features(gm-geometry PUBLIC cxx_std_20)

if (GM_BUILD_TESTS)
  add_subdirectory(tests)
endif()
