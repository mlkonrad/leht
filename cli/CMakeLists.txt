add_executable(leht_cli src/main.cpp)
target_link_libraries(leht_cli PRIVATE leht::core leht::warnings)
# The binary is `leht`; the target is leht_cli to avoid clashing with the
# project name.
set_target_properties(leht_cli PROPERTIES OUTPUT_NAME leht)
