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

# Checks what the model read, against the .expected-result.json files next to
# the documents. Needs a running Ollama and skips itself without one.
qt_add_executable(tst_bills tst_bills.cpp)

target_link_libraries(tst_bills
    PRIVATE
        invoicedrop-ai
        Qt6::Test
)

add_test(NAME bills COMMAND tst_bills)

set_tests_properties(bills PROPERTIES
    LABELS "integration"
    TIMEOUT 1800
)

qt_add_executable(tst_store tst_store.cpp)

target_link_libraries(tst_store
    PRIVATE
        invoicedrop-ai
        Qt6::Test
)

add_test(NAME store COMMAND tst_store)

qt_add_executable(tst_daemon tst_daemon.cpp)

target_link_libraries(tst_daemon
    PRIVATE
        invoicedrop-ai
        Qt6::Test
)

add_test(NAME daemon COMMAND tst_daemon)

# The plasmoid's decision logic lives in a JavaScript module so it can be run on
# its own. The drag itself cannot be automated; the command a drop builds and the
# reply it parses can be, and that is where the mistakes were.
find_program(QMLSCENE6_EXECUTABLE NAMES qmlscene6)

if(QMLSCENE6_EXECUTABLE)
    add_test(NAME plasmoid
             COMMAND ${QMLSCENE6_EXECUTABLE} -platform offscreen
                     ${CMAKE_CURRENT_SOURCE_DIR}/tst_plasmoid.qml)

    # qmlscene ignores Qt.exit(), so the verdict comes from the output.
    set_tests_properties(plasmoid PROPERTIES
        PASS_REGULAR_EXPRESSION "ALL PASSED"
        FAIL_REGULAR_EXPRESSION "CHECK\\(S\\) FAILED"
        TIMEOUT 120
    )
else()
    message(STATUS "qmlscene6 not found, skipping the plasmoid logic checks")
endif()

# Run from the project root so tests/testdata is reachable by relative path as
# well as through QFINDTESTDATA.
set_tests_properties(documentreader PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
