find_package(Qt6 6.5 QUIET COMPONENTS Test)

if(NOT Qt6Test_FOUND)
    message(STATUS "Qt6 Test not found, skipping the test suite")
    return()
endif()

qt_add_executable(tst_documentreader tst_documentreader.cpp)

target_link_libraries(tst_documentreader
    PRIVATE
        invoicedrop-extract
        Qt6::Test
)

add_test(NAME documentreader COMMAND tst_documentreader)

qt_add_executable(tst_invoice tst_invoice.cpp)

target_link_libraries(tst_invoice
    PRIVATE
        invoicedrop-ai
        Qt6::Test
)

add_test(NAME invoice COMMAND tst_invoice)

# Run from the project root so tests/testdata is reachable by relative path as
# well as through QFINDTESTDATA.
set_tests_properties(documentreader PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
