add_executable(urlshort_tests
  test_main.cpp
  test_url_validator.cpp
  test_code_generator.cpp
  test_link_store.cpp
  test_shortener_service.cpp
  test_http_api.cpp
)

target_link_libraries(urlshort_tests PRIVATE urlshort_core doctest::doctest)

# doctest ships a CMake helper that registers each TEST_CASE with CTest, so a
# failure names the case rather than just "the test binary".
include(${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake OPTIONAL RESULT_VARIABLE _doctest_cmake)
if(_doctest_cmake)
  doctest_discover_tests(urlshort_tests)
else()
  add_test(NAME urlshort_tests COMMAND urlshort_tests)
endif()
