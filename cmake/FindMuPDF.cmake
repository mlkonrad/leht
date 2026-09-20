# FindMuPDF.cmake - locate MuPDF via pkg-config (mupdf.pc), with a manual fallback.
# Defines the imported target MuPDF::MuPDF.

if(TARGET MuPDF::MuPDF)
    return()
endif()

# LEHT_MUPDF_ROOT points at a hand-built MuPDF source tree (one that has been
# built with `make libs`), bypassing pkg-config and the system library.
#
# This exists because of a lesson the fuzzer taught us: the version a
# distribution ships is not the version upstream supports, and for a library
# parsing hostile input that gap matters. Being able to build and test against
# current upstream without touching the system is worth the twelve lines.
#
#   cmake -S . -B build-next -DLEHT_MUPDF_ROOT=/path/to/mupdf-1.28.4-source
set(LEHT_MUPDF_ROOT "" CACHE PATH "Built MuPDF source tree to use instead of the system one")

if(LEHT_MUPDF_ROOT)
    find_path(MuPDF_INCLUDE_DIR NAMES mupdf/fitz.h
              PATHS "${LEHT_MUPDF_ROOT}/include" NO_DEFAULT_PATH)
    find_library(MuPDF_LIBRARY NAMES mupdf
                 PATHS "${LEHT_MUPDF_ROOT}/build/release" NO_DEFAULT_PATH)
    find_library(MuPDF_THIRD_LIBRARY NAMES mupdf-third
                 PATHS "${LEHT_MUPDF_ROOT}/build/release" NO_DEFAULT_PATH)

    if(NOT MuPDF_INCLUDE_DIR OR NOT MuPDF_LIBRARY)
        message(FATAL_ERROR
            "LEHT_MUPDF_ROOT=${LEHT_MUPDF_ROOT} does not look like a built "
            "MuPDF tree. Run: make -j HAVE_X11=no HAVE_GLUT=no build=release libs")
    endif()

    add_library(MuPDF::MuPDF UNKNOWN IMPORTED)
    set_target_properties(MuPDF::MuPDF PROPERTIES
        IMPORTED_LOCATION "${MuPDF_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${MuPDF_INCLUDE_DIR}")
    target_link_libraries(MuPDF::MuPDF INTERFACE
        "${MuPDF_THIRD_LIBRARY}" m pthread)

    set(MuPDF_FOUND TRUE)
    message(STATUS "Using hand-built MuPDF from ${LEHT_MUPDF_ROOT}")
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
