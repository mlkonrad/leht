# Shared warning + sanitizer configuration, applied via target_link_libraries.
add_library(leht_warnings INTERFACE)
add_library(leht::warnings ALIAS leht_warnings)

target_compile_options(leht_warnings INTERFACE
    -Wall -Wextra -Wpedantic
    -Wshadow -Wnon-virtual-dtor -Wold-style-cast -Wcast-align
    -Wunused -Woverloaded-virtual -Wconversion -Wsign-conversion
    -Wnull-dereference -Wdouble-promotion -Wformat=2
    $<$<CONFIG:Debug>:-Werror>)

if(LEHT_SANITIZE)
    target_compile_options(leht_warnings INTERFACE
        -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(leht_warnings INTERFACE
        -fsanitize=address,undefined)
endif()
