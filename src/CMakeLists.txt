add_executable(beebcrunch
    bitreader.c
    bitwriter.c
    coding.c
    frontier.c
    main.c
    matches.c
    tables.c
    v0.c
    v1.c
    v2.c
    v3.c
    v4.c
    v5.c
    v6.c
    v7.c
    bitreader.h
    bitutils.h
    bitwriter.h
    coding.h
    frontier.h
    matches.h
    tables.h
    v0.h
    v1.h
    v2.h
    v3.h
    v4.h
    v5.h
    v6.h
    v7.h
)

target_link_libraries(beebcrunch PRIVATE richc)

if(MSVC)
    target_compile_options(beebcrunch PRIVATE /W3)
else()
    target_compile_options(beebcrunch PRIVATE -Wall -Wextra)
endif()

# ---- unit tests ----

# Test blocks in the sources compile only when the ENABLE_TESTS cache
# variable is set; the post-build step then runs them all (--test with an
# empty prefix), and a failed test fails the build.
if(ENABLE_TESTS)
    target_compile_definitions(beebcrunch PRIVATE ENABLE_TESTS)
    add_custom_command(
        TARGET beebcrunch POST_BUILD
        COMMAND $<TARGET_FILE:beebcrunch> --test ""
        COMMENT "Running beebcrunch tests"
        VERBATIM
    )
endif()
