# ---------------------------------------------------------------------------
# Document extraction: MuPDF, Leptonica, Tesseract. No Qt beyond Core, so it
# stays independent of the network layer.
# ---------------------------------------------------------------------------

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
# Invoice model and the Ollama client.
# ---------------------------------------------------------------------------

add_library(invoicedrop-ai STATIC
    analysis.cpp
    hash.cpp
    invoice.cpp
    daemon.cpp
    inboxwatcher.cpp
    notifier.cpp
    ollama.cpp
    paths.cpp
    json.cpp
    store.cpp
    totals.cpp
)

target_link_libraries(invoicedrop-ai
    PUBLIC
        invoicedrop-extract
        Qt6::Network
        Qt6::Sql
        Qt6::DBus
)

target_include_directories(invoicedrop-ai
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

if(INVOICEDROP_WERROR)
    target_compile_options(invoicedrop-ai PRIVATE -Wall -Wextra -Werror)
else()
    target_compile_options(invoicedrop-ai PRIVATE -Wall -Wextra)
endif()

# ---------------------------------------------------------------------------

configure_file(version.h.in version.h @ONLY)

add_executable(invoicedrop
    main.cpp
    cli.cpp
)

target_link_libraries(invoicedrop PRIVATE invoicedrop-ai)

target_include_directories(invoicedrop PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

install(TARGETS invoicedrop
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# ---------------------------------------------------------------------------
# The daemon reaches the session bus two ways. The systemd user unit is the
# normal one; the D-Bus service file lets the first CLI call start it on demand,
# so nothing has to be enabled by hand for the tool to work after a login.
# ---------------------------------------------------------------------------

configure_file(${CMAKE_SOURCE_DIR}/data/invoicedrop.service.in
               ${CMAKE_BINARY_DIR}/data/invoicedrop.service @ONLY)
configure_file(${CMAKE_SOURCE_DIR}/data/org.kde.invoicedrop.service.in
               ${CMAKE_BINARY_DIR}/data/org.kde.invoicedrop.service @ONLY)

# Where the user unit has to go depends on the prefix, because systemd does not
# look in one fixed place. `systemd-analyze --user unit-paths` lists
# ~/.local/share/systemd/user but not ~/.local/lib/systemd/user, so a prefix
# under $HOME needs the unit in <prefix>/share/systemd/user. A system prefix
# needs <prefix>/lib/systemd/user. GNUInstallDirs answers "lib" for both, which
# is right for a package and invisible-but-broken for a local install: the file
# is written, systemd never reads it, and `systemctl --user start` reports a
# unit that cannot be found.
if(CMAKE_INSTALL_PREFIX MATCHES "^/(usr|usr/local)/?$")
    set(INVOICEDROP_USERUNIT_DIR "${CMAKE_INSTALL_LIBDIR}/systemd/user")
else()
    set(INVOICEDROP_USERUNIT_DIR "share/systemd/user")
endif()

install(FILES ${CMAKE_BINARY_DIR}/data/invoicedrop.service
        DESTINATION ${INVOICEDROP_USERUNIT_DIR})

# The session bus does not have that problem: <standard_session_servicedirs/>
# covers $XDG_DATA_HOME/dbus-1/services, so ~/.local/share/dbus-1/services is
# found without a special case.
install(FILES ${CMAKE_BINARY_DIR}/data/org.kde.invoicedrop.service
        DESTINATION ${CMAKE_INSTALL_DATADIR}/dbus-1/services)

# The same mark the plasmoid draws, installed under its own name so the icon
# theme -- and with it the desktop entry and the notifications -- can find it.
# No theme ships anything called invoice, which is why the package brings one.
install(FILES ${CMAKE_SOURCE_DIR}/plasmoid/com.github.invoicedrop/contents/icons/invoice.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps
        RENAME com.github.invoicedrop.svg)

install(FILES ${CMAKE_SOURCE_DIR}/data/com.github.invoicedrop.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)

install(FILES ${CMAKE_SOURCE_DIR}/LICENSE
        DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/invoicedrop
        OPTIONAL)

# Convenience alias so both spellings work on a case sensitive filesystem.
install(CODE "
    execute_process(
        COMMAND \${CMAKE_COMMAND} -E create_symlink
                invoicedrop \$ENV{DESTDIR}${CMAKE_INSTALL_FULL_BINDIR}/InvoiceDrop
    )
")
