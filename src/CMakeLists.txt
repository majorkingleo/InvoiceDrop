add_library(invoicedrop-extract STATIC
    extract/documentreader.cpp
    extract/imagereader.cpp
    extract/imageops.cpp
    extract/ocr.cpp
    extract/pdfreader.cpp
    extract/threadctx.cpp
)

target_include_directories(invoicedrop-extract
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(invoicedrop-extract
    PUBLIC
        Qt6::Core
        PkgConfig::MUPDF
        PkgConfig::LEPTONICA
        PkgConfig::TESSERACT
)

if(INVOICEDROP_WERROR)
    target_compile_options(invoicedrop-extract PRIVATE -Wall -Wextra -Werror)
else()
    target_compile_options(invoicedrop-extract PRIVATE -Wall -Wextra)
endif()

# ---------------------------------------------------------------------------

configure_file(version.h.in version.h @ONLY)

add_executable(invoicedrop
    main.cpp
    cli.cpp
)

target_link_libraries(invoicedrop PRIVATE invoicedrop-extract)

target_include_directories(invoicedrop PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

install(TARGETS invoicedrop
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# Convenience alias so both spellings work on a case sensitive filesystem.
install(CODE "
    execute_process(
        COMMAND \${CMAKE_COMMAND} -E create_symlink
                invoicedrop \$ENV{DESTDIR}${CMAKE_INSTALL_FULL_BINDIR}/InvoiceDrop
    )
")
