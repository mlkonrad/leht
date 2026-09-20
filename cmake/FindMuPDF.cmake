# FindMuPDF.cmake - locate MuPDF via pkg-config (mupdf.pc), with a manual fallback.
# Defines the imported target MuPDF::MuPDF.

if(TARGET MuPDF::MuPDF)
    return()
endif()

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(PC_MUPDF QUIET mupdf)
endif()

find_path(MuPDF_INCLUDE_DIR
    NAMES mupdf/fitz.h
    HINTS ${PC_MUPDF_INCLUDE_DIRS})

find_library(MuPDF_LIBRARY
    NAMES mupdf
    HINTS ${PC_MUPDF_LIBRARY_DIRS})

# Fedora unbundles MuPDF's vendored dependencies, so mupdf.pc carries them in
# Requires/Libs. When pkg-config is unavailable we cannot reconstruct that list
# reliably, so treat its absence as fatal rather than guessing.
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(MuPDF
    REQUIRED_VARS MuPDF_INCLUDE_DIR MuPDF_LIBRARY
    VERSION_VAR PC_MUPDF_VERSION)

if(MuPDF_FOUND)
    add_library(MuPDF::MuPDF UNKNOWN IMPORTED)
    set_target_properties(MuPDF::MuPDF PROPERTIES
        IMPORTED_LOCATION "${MuPDF_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${MuPDF_INCLUDE_DIR}")
    if(PC_MUPDF_FOUND)
        target_link_libraries(MuPDF::MuPDF INTERFACE ${PC_MUPDF_LIBRARIES})
        target_link_directories(MuPDF::MuPDF INTERFACE ${PC_MUPDF_LIBRARY_DIRS})
    endif()
endif()

mark_as_advanced(MuPDF_INCLUDE_DIR MuPDF_LIBRARY)

# Known-bad versions. MuPDF 1.28.2 and earlier abort with an invalid free while
# repairing a malformed xref -- five bytes are enough (tests/crashes/README.md).
# Fixed in 1.28.4.
#
# A warning rather than a hard requirement: Fedora 44 ships 1.28.2, and refusing
# to build on the project's own development platform would cost more than it
# buys. Leht works fine on 1.28.2 for files you trust.
set(LEHT_MUPDF_MIN_SAFE_VERSION "1.28.4")
# Imported targets are directory-scoped, so find_package runs once per
# subdirectory. Warn only the first time.
if(MuPDF_FOUND AND PC_MUPDF_VERSION
   AND PC_MUPDF_VERSION VERSION_LESS LEHT_MUPDF_MIN_SAFE_VERSION
   AND NOT LEHT_MUPDF_VERSION_WARNED)
    set(LEHT_MUPDF_VERSION_WARNED TRUE CACHE INTERNAL "")
    message(WARNING
        "MuPDF ${PC_MUPDF_VERSION} has a known heap-corruption bug in PDF xref "
        "repair: a five-byte malformed file aborts the process with an invalid "
        "free. Fixed in ${LEHT_MUPDF_MIN_SAFE_VERSION}. Leht will build and "
        "work, but do NOT open untrusted PDFs with this version. "
        "See tests/crashes/README.md.")
endif()
