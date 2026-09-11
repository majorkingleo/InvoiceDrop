cmake_minimum_required(VERSION 3.20)

# Structural checks on the plasmoid package, for the two mistakes Plasma does not
# report at all.
#
# A `contents/config/config.qml` that is a plain page instead of a ConfigModel
# loads without a single warning, and the settings dialog then has no page of its
# own: the user sees the keyboard shortcuts and the About tab and nothing else,
# with no error anywhere. A `cfg_` alias that does not match an entry name in
# `main.xml` is the same kind of silence from the other end — the dialog opens, an
# empty row sits there, and the value is never written.
#
# Neither shows up in a build, a lint or a log, so they are checked here.
#
# Called by tests/CMakeLists.txt with -DPACKAGE_DIR=<the plasmoid package>.

if(NOT DEFINED PACKAGE_DIR)
    message(FATAL_ERROR "PACKAGE_DIR is not set")
endif()

set(schema_file "${PACKAGE_DIR}/contents/config/main.xml")
set(model_file "${PACKAGE_DIR}/contents/config/config.qml")

if(NOT EXISTS "${schema_file}" OR NOT EXISTS "${model_file}")
    message(FATAL_ERROR "${PACKAGE_DIR} does not look like a plasmoid package")
endif()

file(READ "${schema_file}" schema)
file(READ "${model_file}" model)

# ---------------------------------------------------------------- the model

string(REGEX MATCHALL "\n[ \t]*ConfigModel[ \t]*\\{" model_root "${model}")

if(NOT model_root)
    message(FATAL_ERROR
        "contents/config/config.qml is not a ConfigModel. Plasma reads the dialog "
        "pages from a ConfigModel, shows nothing for anything else, and reports "
        "nothing.")
endif()

string(REGEX MATCHALL "source:[ \t]*\"[^\"]+\"" source_attributes "${model}")

if(NOT source_attributes)
    message(FATAL_ERROR "config.qml has no ConfigCategory, so the dialog has no page")
endif()

# A ConfigCategory's source is resolved against contents/ui/, not against the
# folder the model file lives in.
set(pages "")

foreach(attribute IN LISTS source_attributes)
    string(REGEX REPLACE ".*\"([^\"]+)\".*" "\\1" page "${attribute}")
    set(page_file "${PACKAGE_DIR}/contents/ui/${page}")

    if(NOT EXISTS "${page_file}")
        message(FATAL_ERROR
            "config.qml names ${page}, and contents/ui/${page} does not exist. "
            "A page that is not there costs the whole settings dialog.")
    endif()

    list(APPEND pages "${page_file}")
endforeach()

# --------------------------------------------------------------- the pages

set(aliases "")

foreach(page_file IN LISTS pages)
    file(READ "${page_file}" page)

    string(REGEX MATCHALL "property[ \t]+alias[ \t]+cfg_[A-Za-z0-9_]+" declarations "${page}")

    foreach(declaration IN LISTS declarations)
        string(REGEX REPLACE ".*cfg_" "" name "${declaration}")
        list(APPEND aliases "${name}")
    endforeach()
endforeach()

# -------------------------------------------------------------- the schema

string(REGEX MATCHALL "entry[ \t]+name=\"[^\"]+\"" entries "${schema}")
set(names "")

foreach(entry IN LISTS entries)
    string(REGEX REPLACE ".*name=\"([^\"]+)\"" "\\1" name "${entry}")
    list(APPEND names "${name}")
endforeach()

if(NOT names)
    message(FATAL_ERROR "main.xml declares no entry, so there is nothing to configure")
endif()

# Both directions. A missing alias is a setting the user cannot reach, an extra
# alias is a control that edits nothing.
foreach(name IN LISTS names)
    if(NOT name IN_LIST aliases)
        message(FATAL_ERROR
            "main.xml declares ${name}, and no page has a `property alias cfg_${name}`")
    endif()
endforeach()

foreach(name IN LISTS aliases)
    if(NOT name IN_LIST names)
        message(FATAL_ERROR
            "a page has `property alias cfg_${name}`, and main.xml has no entry ${name}")
    endif()
endforeach()

list(LENGTH names entry_count)
list(LENGTH pages page_count)

message(STATUS
    "config.qml is a ConfigModel: ${page_count} page(s), ${entry_count} config "
    "entr(ies), every one of them wired to a cfg_ alias")
