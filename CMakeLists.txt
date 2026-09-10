cmake_minimum_required(VERSION 3.20)

project(InvoiceDrop
    VERSION 0.1.0
    DESCRIPTION "Analyse invoices with a local AI model"
    HOMEPAGE_URL "https://github.com/example/invoicedrop"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Runs moc over headers and sources with Q_OBJECT, which the QTest classes need.
set(CMAKE_AUTOMOC ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

# Lets clangd, cpptools and the editor tooling resolve the exact compile flags.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(INVOICEDROP_WERROR "Treat compiler warnings as errors" OFF)
option(BUILD_TESTING "Build the test suite" ON)

include(GNUInstallDirs)
include(CTest)

# ---------------------------------------------------------------------------
# Qt. Network arrives with the Ollama client. Sql and DBus follow in later
# phases.
# ---------------------------------------------------------------------------
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Network)

# ---------------------------------------------------------------------------
# Document libraries. MuPDF renders and reads PDF text, Leptonica decodes
# raster images and encodes JPEG, Tesseract does the OCR pre-pass.
# ---------------------------------------------------------------------------
find_package(PkgConfig REQUIRED)

pkg_check_modules(MUPDF REQUIRED IMPORTED_TARGET mupdf)
pkg_check_modules(LEPTONICA REQUIRED IMPORTED_TARGET lept)
pkg_check_modules(TESSERACT REQUIRED IMPORTED_TARGET tesseract)

message(STATUS "MuPDF      ${MUPDF_VERSION}")
message(STATUS "Leptonica  ${LEPTONICA_VERSION}")
message(STATUS "Tesseract  ${TESSERACT_VERSION}")

add_subdirectory(src)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
