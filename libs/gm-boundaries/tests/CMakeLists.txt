add_executable(gm-boundaries-tests
  mahalanobis_test.cpp
  kde_test.cpp
  marching_tetrahedra_test.cpp
)

target_link_libraries(gm-boundaries-tests
  PRIVATE
    gm::boundaries
    Catch2::Catch2WithMain
)

catch_discover_tests(gm-boundaries-tests)
