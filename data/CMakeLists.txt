# Desktop integration for the viewer: launcher entry, icon, "Open with" for PDFs.
# All of it is named by LEHT_APP_ID so the three stay in step.
configure_file(page.leht.Leht.desktop.in "${LEHT_APP_ID}.desktop" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/${LEHT_APP_ID}.desktop"
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES icons/leht.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps
        RENAME "${LEHT_APP_ID}.svg")
