find_package(Arrow CONFIG REQUIRED)
find_package(Parquet CONFIG REQUIRED)
find_package(cpr CONFIG REQUIRED)

add_library(gm-io STATIC
  src/http_cache.cpp
  src/csv.cpp
  src/table.cpp
  src/mesh.cpp
  src/parquet.cpp
)
add_library(gm::io ALIAS gm-io)

target_include_directories(gm-io
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(gm-io
  PUBLIC
    gm::core
  PRIVATE
    Arrow::arrow_static
    Parquet::parquet_static
    cpr::cpr
)

target_compile_features(gm-io PUBLIC cxx_std_20)

if (GM_BUILD_TESTS)
  add_subdirectory(tests)
endif()
