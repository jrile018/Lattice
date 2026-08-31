find_package(Eigen3 CONFIG REQUIRED)
find_package(osqp CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_library(gm-signals-lib STATIC
  src/peer_basket.cpp
  src/ou_fit.cpp
  src/excursion.cpp
  src/earnings.cpp
)
add_library(gm::signals ALIAS gm-signals-lib)

target_include_directories(gm-signals-lib
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(gm-signals-lib
  PUBLIC
    gm::core
    gm::io
    Eigen3::Eigen
    nlohmann_json::nlohmann_json
  PRIVATE
    # osqp's vcpkg port only guarantees the shared-lib target
    # (osqp::osqp) OR the static one (osqp::osqpstatic) depending on
    # how the port was built, per vcpkg's own generated usage note -
    # this selects whichever actually exists rather than hardcoding one.
    $<IF:$<TARGET_EXISTS:osqp::osqp>,osqp::osqp,osqp::osqpstatic>
)

target_compile_features(gm-signals-lib PUBLIC cxx_std_20)

if (GM_BUILD_TESTS)
  add_subdirectory(tests)
endif()
