add_executable(leht_cli src/main.cpp)
# leht::crypto for sign/verify: the CLI is the trusted process, so it is where
# the private key lives.
target_link_libraries(leht_cli PRIVATE leht::core leht::crypto leht::warnings)
# The binary is `leht`; the target is leht_cli to avoid clashing with the
# project name.
set_target_properties(leht_cli PROPERTIES OUTPUT_NAME leht)

install(TARGETS leht_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
