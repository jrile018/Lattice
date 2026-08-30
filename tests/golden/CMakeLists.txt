# See tests/fixtures/golden_run.toml.in - @GM_REPO_ROOT@ becomes an
# absolute path so the fixture works regardless of ctest's working
# directory.
set(GM_REPO_ROOT ${CMAKE_SOURCE_DIR})
configure_file(
  ${CMAKE_SOURCE_DIR}/tests/fixtures/golden_run.toml.in
  ${CMAKE_CURRENT_BINARY_DIR}/golden_run.toml
  @ONLY
)

add_executable(gm-golden-tests
  m0_pipeline_test.cpp
)

target_link_libraries(gm-golden-tests
  PRIVATE
    gm::core
    Catch2::Catch2WithMain
)

# gm-golden-tests must run after gm-run (and every stage it invokes)
# exists as a built binary - this is a real dependency, not an ordering
# hint, since the test shells out to the gm-run executable by path.
add_dependencies(gm-golden-tests
  gm-run gm-universe gm-ingest gm-features gm-geometry gm-boundaries
  gm-signals gm-backtest gm-report
)

target_compile_definitions(gm-golden-tests PRIVATE
  GM_RUN_EXECUTABLE="$<TARGET_FILE:gm-run>"
  GM_GOLDEN_FIXTURE_CONFIG="${CMAKE_CURRENT_BINARY_DIR}/golden_run.toml"
)

catch_discover_tests(gm-golden-tests)
