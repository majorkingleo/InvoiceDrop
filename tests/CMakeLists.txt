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

# The per file sums. Pure arithmetic on bills, so it needs neither a model nor a
# file, which is why it can check the cases a real run only reaches by accident:
# a mixed currency file, a bill that failed inside a sum that still adds up, and
# a file cut in half by the page limit.
qt_add_executable(tst_totals tst_totals.cpp)

target_link_libraries(tst_totals
    PRIVATE
        invoicedrop-ai
        Qt6::Test
)

add_test(NAME totals COMMAND tst_totals)

# Paths, and the two mistakes that made this suite worth having: a relative path
# handed to a daemon that has a different working directory, and a data directory
# that was built from AppLocalDataLocation and came out doubled.
qt_add_executable(tst_paths tst_paths.cpp)

target_link_libraries(tst_paths
    PRIVATE
        invoicedrop-ai
        Qt6::Test
)

add_test(NAME paths COMMAND tst_paths)

# The plasmoid package's structure, which Plasma never complains about: a
# config.qml that is not a ConfigModel costs the entire settings dialog and logs
# nothing, and a cfg_ alias that does not match an entry in main.xml leaves an
# empty row behind and saves nothing. Checked by a script rather than by a test
# binary because there is no code to run, only files to read.
add_test(NAME plasmoid-config
         COMMAND ${CMAKE_COMMAND}
                 -DPACKAGE_DIR=${CMAKE_SOURCE_DIR}/plasmoid/com.github.invoicedrop
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/check_plasmoid_config.cmake)

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

# The log's own decisions: how much is on, whether a tag gets a colour, and how a
# block is laid out. The formatting is a pure function, which is the only reason
# any of it can be checked -- what the lines look like on a terminal is not
# something a test can see, so it was looked at by hand.
qt_add_executable(tst_log tst_log.cpp)

target_link_libraries(tst_log
    PRIVATE
        invoicedrop-ai
        Qt6::Test
)

add_test(NAME log COMMAND tst_log)

# Run from the project root so tests/testdata is reachable by relative path as
# well as through QFINDTESTDATA.
set_tests_properties(documentreader PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
