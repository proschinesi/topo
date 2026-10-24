# SPDX-License-Identifier: Apache-2.0
# Modified for topo (fork of btop for Apple Silicon), 2026: see NOTICE.

include(FetchContent)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.17.0
  FIND_PACKAGE_ARGS NAMES GTest
)
# topo: googletest serve solo ai test, non va installato con `cmake --install`
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_library(libbtop_test)
target_include_directories(libbtop_test PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(libbtop_test libbtop GTest::gtest_main)

add_executable(btop_test cpu_names.cpp tools.cpp)
if(APPLE)
  # topo: parti pure di src/osx/apple_soc
  target_sources(btop_test PRIVATE apple_soc.cpp)
endif()
target_link_libraries(btop_test libbtop_test)

include(GoogleTest)
gtest_discover_tests(btop_test)
