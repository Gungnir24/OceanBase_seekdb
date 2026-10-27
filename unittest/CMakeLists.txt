# Focused AI regression tests; use the same engine implementation as seekdb.
add_executable(sql_tests EXCLUDE_FROM_ALL
  all_tests_main.cpp
  sql/engine/expr/ob_expr_ai_prompt_test.cpp
  sql/engine/expr/test_ob_openai_utils.cpp)
target_link_libraries(sql_tests PRIVATE ob_base oceanbase "${DEP_DIR}/lib/libgtest.a")
set_target_properties(sql_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/sql")
add_custom_target(pretest DEPENDS sql_tests)
add_test(NAME sql_tests_shard_0 COMMAND sql_tests)
set(SEEKDB_PRETEST_MODULES sql PARENT_SCOPE)
set(SEEKDB_PRETEST_SQL_TARGET sql_tests PARENT_SCOPE)
configure_file(run_tests.sh.in run_tests.sh @ONLY)
file(CHMOD "${CMAKE_CURRENT_BINARY_DIR}/run_tests.sh"
  PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
