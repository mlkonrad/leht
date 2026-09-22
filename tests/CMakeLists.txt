# Whole-project tests that belong to no single component.

# Install into a scratch DESTDIR and check the result: every file lands where
# the desktop expects it, the installed viewer finds the installed worker, and
# nothing installed still points into the build tree.
add_test(NAME install_tree
         COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/install_test.sh"
                 "${CMAKE_BINARY_DIR}" "${CMAKE_INSTALL_PREFIX}"
                 "${CMAKE_INSTALL_BINDIR}" "${CMAKE_INSTALL_LIBEXECDIR}"
                 "${CMAKE_INSTALL_DATADIR}" "${LEHT_APP_ID}"
                 "$<BOOL:${LEHT_BUILD_UI}>")
set_tests_properties(install_tree PROPERTIES SKIP_RETURN_CODE 77)
