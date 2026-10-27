# Focused AI regression tests; use the same engine implementation as seekdb.
# The target is optional: gtest is not part of every dependency profile, so the
# tests are only registered when a gtest library and headers are available.
find_path(SEEKDB_GTEST_INCLUDE_DIR gtest/gtest.h
  HINTS "${DEP_DIR}/include" "${DEP_DIR}/include/3rd" "${OB_HEADER_DEP_DIR}/include")
find_library(SEEKDB_GTEST_LIBRARY
  NAMES gtest gtest.lib libgtest.a
  HINTS "${DEP_DIR}/lib" "${DEP_DIR}/lib/3rd" "${DEP_DIR}/lib64")

if(SEEKDB_GTEST_INCLUDE_DIR AND SEEKDB_GTEST_LIBRARY)
  add_executable(sql_tests EXCLUDE_FROM_ALL
    all_tests_main.cpp
    sql/engine/expr/ob_expr_ai_prompt_test.cpp
    sql/engine/expr/test_ob_openai_utils.cpp)
  target_include_directories(sql_tests PRIVATE "${SEEKDB_GTEST_INCLUDE_DIR}")
  target_link_libraries(sql_tests PRIVATE ob_base oceanbase "${SEEKDB_GTEST_LIBRARY}")
  set_target_properties(sql_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/sql")
  add_custom_target(pretest DEPENDS sql_tests)
  add_test(NAME sql_tests_shard_0 COMMAND sql_tests)
  set(SEEKDB_PRETEST_MODULES sql PARENT_SCOPE)
  set(SEEKDB_PRETEST_SQL_TARGET sql_tests PARENT_SCOPE)
  configure_file(run_tests.sh.in run_tests.sh @ONLY)
  file(CHMOD "${CMAKE_CURRENT_BINARY_DIR}/run_tests.sh"
    PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
else()
  message(STATUS "gtest not found in ${DEP_DIR}; skipping sql_tests target")
endif()
