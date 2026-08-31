# camera.cpp is pure math (no GL/GLFW/ImGui headers) - compiled
# directly into the test binary rather than through the gm-view
# executable target, so these tests need no display, no GL context, and
# no windowing dependency at all. gl.cpp and data_loader.cpp are not
# covered here: the former needs a real GL context to exercise
# meaningfully (out of scope for this pass - see the M3 review), and
# the latter's Parquet-reading logic is exercised end-to-end by the
# golden pipeline test's artifacts already existing on disk.
add_executable(gm-view-tests
  camera_test.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../camera.cpp
)

target_include_directories(gm-view-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)

target_link_libraries(gm-view-tests
  PRIVATE
    Catch2::Catch2WithMain
)

catch_discover_tests(gm-view-tests)
