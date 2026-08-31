add_executable(gm-signals-tests
  peer_basket_test.cpp
  ou_fit_test.cpp
  excursion_test.cpp
  earnings_test.cpp
)

target_link_libraries(gm-signals-tests
  PRIVATE
    gm::signals
    Catch2::Catch2WithMain
)

catch_discover_tests(gm-signals-tests)
