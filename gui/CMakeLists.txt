cmake_minimum_required(VERSION 3.21)
# RC is in the language list for resources/app.rc alone — the Windows resource
# script that gives the executable its icon and its VERSIONINFO block. Without
# it CMake never enables windres and a .rc in a target's sources is treated as
# an unknown file type and silently ignored, which is a quieter failure than it
# sounds: the build succeeds and the exe comes out with no icon, exactly as
# before. See resources/app.rc and the compile definitions below the executable.
#
# It does make CONFIGURING require a resource compiler, so a non-Windows host
# now fails here rather than further down. That is the honest position: the
# `if(WIN32)` guards further down only ever protected the packaging targets, and
# an application built around windeployqt, the MinGW runtime DLLs and an Inno
# Setup script has never been buildable anywhere else.
#
# VERSION is the SINGLE source of the application's version number. It becomes
# -DCT_APP_VERSION (src/main.cpp hands it straight to
# QCoreApplication::setApplicationVersion), CT_VER_MAJOR/MINOR/PATCH for the
# VERSIONINFO block in resources/app.rc, and /DMyAppVersion for the Inno Setup
# script — whose own "0.1.0" is a standalone-compile fallback the CMake target
# always overrides, not a second place to maintain. A release changes the number
# HERE and nowhere else.
#
# 1.0.0 — the first release. The numbers that came before it (0.1.0 through
# 2.4.0, tracking and sometimes diverging from the firmware's 2.x) were
# development-only: nothing was ever shipped under them, so the whole project
# was reset to 1.0.0 in one move, firmware and Manager together, rather than
# carrying a pre-release history no user ever saw into the numbering forever.
# Comments in the sources still narrate that history by its old numbers —
# they describe real internal transitions and rewriting them would falsify
# the record — but every number a USER can meet starts at 1.0.0.
#
# The two versions are allowed to diverge again after this point (a host-only
# fix should not send people re-flashing units for nothing); what may never
# happen is the app naming a version older than the work inside it.
#
# NOT part of this reset, deliberately: the internal format epochs. The
# config store, the .ct3 schema, the wire protocol and FW_BOOTLOADER_VERSION
# all kept their own numbering — that last one is a flash-map GENERATION,
# not a version (see fw_image.h), and winding it back would tell bootloaders
# this image uses the old memory map. Compatibility gates keep counting;
# only the product number starts over.
#
# Their values are deliberately NOT restated here. This block twice named
# epochs that had since moved on — it claimed store v7 and schema 15 long
# after they were 11 and 19 — and a stale number in a comment that reads as
# authoritative is worse than no number at all, because it gets believed
# instead of checked. Each counter is declared in exactly one place, and
# that place is the only answer worth trusting:
#
#   config store    FLASH_STORE_VERSION     firmware  flash_store.h
#                   EXPECTED_STORE_VERSION  gui       wire_structs.h
#   .ct3 schema     kConfigSchemaVersion    gui       configuration.cpp
#   wire protocol   PROTOCOL_VERSION        firmware  protocol.h
#   flash-map gen   FW_BOOTLOADER_VERSION   firmware  fw_image.h
#
# The two store constants are a firmware value and the GUI's mirror of it;
# they must agree, and test_firmware_link asserts exactly that.
project(CANTripleDeviceManager VERSION 1.1.0 LANGUAGES C CXX RC)

# ---------------------------------------------------------------------------
# windres cannot live in a path with a space in it
#
# GNU windres does not preprocess a .rc itself — it shells out to the gcc that
# sits beside it, and it builds that command from its OWN program path via a
# relative prefix. The command it composes is not quoted, so a windres found at
# "C:/Program Files/mingw64/bin/windres.exe" runs
#
#     C:\Program Files\mingw64\bin\gcc.exe -E -xc -DRC_INVOKED ...
#
# and the shell answers `'C:\Program' is not recognized`, after which windres
# reports "preprocessing failed" and the build dies on app.rc.obj. Verified
# with binutils 2.39; the toolchain in PATH on the usual build machine is
# exactly that one.
#
# Nothing on the CMake side can quote the inner command — windres assembles it
# long after our arguments are parsed. What does work is giving windres a path
# with no space in it, because the prefix it derives is then space-free too.
# Windows keeps 8.3 aliases for precisely this ("C:\PROGRA~1\..."), so ask cmd
# for one and use that as the RC compiler. Output is byte-identical.
#
# --preprocessor= is deliberately NOT the fix here. Since binutils 2.36 that
# option takes the program alone, so overriding it means re-supplying
# `-E -xc -DRC_INVOKED` through --preprocessor-arg as well — version-dependent,
# and a silent breakage the day those defaults change. Passing only
# --preprocessor=gcc makes windres invoke gcc with no -E at all, which tries to
# LINK the .rc; that is the "file format not recognized" dead end.
# ---------------------------------------------------------------------------
if(WIN32 AND CMAKE_RC_COMPILER MATCHES " ")
    file(TO_NATIVE_PATH "${CMAKE_RC_COMPILER}" _rc_native)
    execute_process(
        COMMAND cmd /c for %I in ("${_rc_native}") do @echo %~sI
        OUTPUT_VARIABLE _rc_short
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(_rc_short AND NOT _rc_short MATCHES " " AND EXISTS "${_rc_short}")
        message(STATUS "windres path contains a space; using 8.3 alias ${_rc_short}")
        set(CMAKE_RC_COMPILER "${_rc_short}")
    else()
        # 8.3 alias generation is off on this volume (fsutil 8dot3name), so
        # there is no space-free spelling of the same file to fall back on.
        message(WARNING
            "windres is at '${CMAKE_RC_COMPILER}', whose path contains a space, and "
            "no 8.3 alias is available. windres will fail to preprocess "
            "resources/app.rc. Either install the toolchain in a path with no "
            "spaces, or configure with -DCMAKE_RC_COMPILER=<space-free windres>.")
    endif()
endif()

# The same unquoted-command defect, second half: windres copies each -I it is
# given straight into the gcc command line without quoting it, so an include
# directory containing a space is torn into fragments and preprocessing fails
# with something like `cc1.exe: fatal error: Projects/CAN: No such file or
# directory` — the tail of the split path, read as a filename. CMake hands a
# target's INCLUDE_DIRECTORIES to the RC compiler by default, which here means
# the Qt headers and the AUTOMOC include dir — the latter inside this very
# source tree. That one bites whenever the checkout sits under a path with a
# SPACE in it (say "C:/Users/you/My Projects/CAN Triple GUI/…"), which this
# project cannot control: it is wherever the user cloned it.
#
# So drop <INCLUDES> from the RC rule. Nothing is lost: resources/app.rc is the
# only .rc in the project and its single #include is <windows.h>, which gcc
# finds on its own system include path, while the icon is resolved by windres
# relative to the .rc's own directory rather than through any -I. The defines
# still come through, which is what the version block actually needs.
#
# Must be set before the target is created — the rule is baked in at that point.
set(CMAKE_RC_COMPILE_OBJECT
    "<CMAKE_RC_COMPILER> <DEFINES> <FLAGS> -O coff <SOURCE> <OBJECT>")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# ---------------------------------------------------------------------------
# Lua 5.4.8, vendored verbatim in third_party/lua (see README-VENDORED.md there
# for provenance and the upgrade drill). MIT licensed; the notice ships in
# installer/THIRD-PARTY-NOTICES.txt.
#
# Only onelua.c is compiled: it is upstream's own single-translation-unit
# amalgamation and #includes the other .c files, which is why they are present
# but not listed. MAKE_LIB selects the library build (no standalone
# interpreter, no luac).
#
# Used twice: as the user-facing scripting engine (Tools > Lua Console) and,
# later, as the parser front end that compiles device scripts to bytecode —
# firmware repo, docs/SCRIPTING-PLAN.md.
# ---------------------------------------------------------------------------
add_library(ct_lua STATIC third_party/lua/onelua.c)
target_compile_definitions(ct_lua PRIVATE MAKE_LIB)
target_include_directories(ct_lua PUBLIC third_party/lua)

# Network is here for QPasswordDigestor (PBKDF2-HMAC-SHA256) alone — see
# src/model/config_lock.* (the sealed-payload primitives), src/model/access_keys.*
# (the 4-byte access keys and the file verifiers, both PBKDF2) and
# src/model/secure_file.* (the .ct3s key wrap, which builds on both). It saves
# vendoring a key-derivation function, which is not code worth hand-rolling.
# Nothing in the app opens a socket.
find_package(Qt6 REQUIRED COMPONENTS Widgets SerialPort PrintSupport Network Help)

# The application compiles a handful of C files straight out of the firmware
# tree — the device's own image validator (fw_image.c) and the script VM
# (script_vm.c / script_exec.c / script_cycles.c / engine_math.c) — so that the
# configurator runs the DEVICE's code rather than a re-implementation of it.
# The firmware is deliberately NOT vendored under gui/ (it is its own project);
# FIRMWARE_DIR resolves where it actually is, in this order:
#
#   1. ./firmware      — a junction/symlink or a direct checkout inside gui/,
#                        the historical split-repo layout
#   2. ../firmware     — the published monorepo layout, where gui/ and
#                        firmware/ sit side by side and a fresh clone of
#                        https://github.com/mitchdetailed/CAN-Triple-Device-Manager
#                        builds with no extra step at all
#
# Resolved ONCE, here, and failed early with instructions — the alternative was
# CMake aborting deep in qt_add_executable with a bare "Cannot find source
# file", telling a newcomer nothing about what to do.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/firmware/include/fw_image.h")
    set(FIRMWARE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/firmware")
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../firmware/include/fw_image.h")
    get_filename_component(FIRMWARE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../firmware" ABSOLUTE)
else()
    message(FATAL_ERROR
        "The firmware sources are missing. This application embeds the device's "
        "own image validator and script VM, so the firmware tree must be present "
        "as ./firmware or ../firmware before it can build.\n"
        "A full clone of https://github.com/mitchdetailed/CAN-Triple-Device-Manager has "
        "both directories side by side and builds as-is; if you are working from "
        "a partial checkout, put the firmware tree beside gui/ or link it in:\n"
        "  (Windows)  mklink /J firmware ..\\\\firmware\n"
        "  (Unix)     ln -s ../firmware firmware\n"
        "See README.md > Build.")
endif()
message(STATUS "Firmware sources: ${FIRMWARE_DIR}")

# Full tree or PUBLIC checkout? The public repository carries no firmware
# source at all — the files the app compiles into itself travel as the
# prebuilt ct_device_core library (see its target below), and include/ +
# VERSION are the interface. The engine, wire-protocol core, flash store and
# bootloader sources exist only in the private tree, so the two tests that
# compile them exist only where the full tree does. Everything a user needs —
# the app, its tools, and every other test — builds from a public checkout.
if(EXISTS "${FIRMWARE_DIR}/src/engine_core.c")
    set(FIRMWARE_FULL TRUE)
else()
    set(FIRMWARE_FULL FALSE)
    message(STATUS "Firmware tree is the contract subset — device-core tests skipped")
endif()

# The firmware's own version, for naming the .ctf the deploy target stages
# into Firmware/. Read from the same VERSION file the firmware build stamps
# its images from, so the file in the install cannot claim a version the
# image inside it does not carry. CONFIGURE_DEPENDS-style tracking: listing
# the file as a configure dependency makes editing it re-run CMake, without
# which a version bump would deploy under the old name until someone
# reconfigured by hand.
file(READ "${FIRMWARE_DIR}/VERSION" FW_VERSION)
string(STRIP "${FW_VERSION}" FW_VERSION)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${FIRMWARE_DIR}/VERSION")
message(STATUS "Firmware version: ${FW_VERSION}")

# THE DEVICE-CORE LIBRARY — the five firmware files the application compiles
# into itself (the image validator and the script VM family), as ONE target
# every consumer links instead of fifteen targets each listing the .c files.
# Two lives: in the private tree it builds from source right here; in the
# public tree the same target name is the PREBUILT libct_device_core.a the
# publish pipeline stages into firmware/lib — which is what lets these
# implementations stay closed while a public clone still builds as-is. The
# Manager's own source is MIT; these objects are proprietary firmware code
# distributed in binary form, and the split between the two is exactly this
# target's boundary.
if(EXISTS "${FIRMWARE_DIR}/src/fw_image.c")
    add_library(ct_device_core STATIC
        ${FIRMWARE_DIR}/src/fw_image.c
        ${FIRMWARE_DIR}/src/script_vm.c
        ${FIRMWARE_DIR}/src/script_exec.c
        ${FIRMWARE_DIR}/src/script_cycles.c
        ${FIRMWARE_DIR}/src/engine_math.c
    )
    target_include_directories(ct_device_core PUBLIC ${FIRMWARE_DIR}/include)
else()
    if(NOT EXISTS "${FIRMWARE_DIR}/lib/libct_device_core.a")
        message(FATAL_ERROR
            "Neither the device-core sources nor the prebuilt library are "
            "present. A public checkout needs firmware/lib/libct_device_core.a "
            "(shipped in the repository); the private tree needs the firmware "
            "sources beside gui/. See README.md > Build.")
    endif()
    add_library(ct_device_core STATIC IMPORTED GLOBAL)
    set_target_properties(ct_device_core PROPERTIES
        IMPORTED_LOCATION "${FIRMWARE_DIR}/lib/libct_device_core.a"
        INTERFACE_INCLUDE_DIRECTORIES "${FIRMWARE_DIR}/include")
endif()

qt_add_executable(CANTripleDeviceManager WIN32
    src/main.cpp
    src/protocol/asc_log.cpp
    src/protocol/asc_log.h
    src/protocol/cobs.cpp
    src/protocol/cobs.h
    src/protocol/config_transfer.cpp
    src/protocol/config_transfer.h
    src/protocol/crc16.h
    src/protocol/device_link.cpp
    src/protocol/device_link.h
    src/protocol/device_session.cpp
    src/protocol/device_session.h
    src/protocol/framer.cpp
    src/protocol/framer.h
    src/protocol/firmware_image.cpp
    src/protocol/firmware_image.h
    src/protocol/firmware_update.cpp
    src/protocol/firmware_update.h
    # The DEVICE's own image validator and script VM ride in via the
    # ct_device_core library (linked below). Not a convenience: it makes a
    # .ctf the update dialog accepts one the bootloader will also accept,
    # because it is the same function over the same CRC32 rather than a C++
    # reimplementation that agrees by inspection. See the note in
    # wire_structs.h explaining why FwImageHeader is NOT mirrored there the
    # way every other wire structure is.
    src/protocol/wire_structs.h
    src/scripting/lua_sandbox.cpp
    src/scripting/lua_sandbox.h
    src/scripting/config_bindings.cpp
    src/scripting/config_bindings.h
    src/scripting/script_runner.cpp
    src/scripting/script_runner.h
    src/scripting/script_compiler.cpp
    src/scripting/script_compiler.h
    src/scripting/script_disassembler.cpp
    src/scripting/script_disassembler.h
    src/scripting/script_simulator.cpp
    src/scripting/script_simulator.h
    # (The VM family — script_vm/script_exec/script_cycles/engine_math — also
    # comes in through ct_device_core: the editor's simulator has to be the
    # unit's VM, not a second implementation that agrees by inspection. Note
    # that engine_core.c and flash_store.c are deliberately NOT in that
    # library — the split in script_exec.c's includes is what keeps them out.)
    src/ui/lua_console_dialog.cpp
    src/ui/lua_console_dialog.h
    src/ui/script_editor_dialog.cpp
    src/ui/script_editor_dialog.h
    src/model/access_keys.cpp
    src/model/access_keys.h
    src/model/channel.h
    src/model/channel_catalog.cpp
    src/model/channel_catalog.h
    src/model/comms_template.cpp
    src/model/comms_template.h
    src/model/comms_types.h
    src/model/config_lock.cpp
    src/model/config_lock.h
    src/model/config_report.cpp
    src/model/config_report.h
    src/model/configuration.cpp
    src/model/configuration.h
    src/model/dbc_import.cpp
    src/model/dbc_import.h
    src/model/device_mapper.cpp
    src/model/device_mapper.h
    src/model/secure_file.cpp
    src/model/secure_file.h
    src/model/config_file.cpp
    src/model/user_paths.cpp
    src/model/config_file.h
    src/model/user_paths.h
    src/model/validation.cpp
    src/model/validation.h
    src/ui/access_passwords_dialog.cpp
    src/ui/access_passwords_dialog.h
    src/ui/add_channel_dialog.cpp
    src/ui/add_channel_dialog.h
    src/ui/bit_layout_table.cpp
    src/ui/bit_layout_table.h
    src/ui/can_viewer_dialog.cpp
    src/ui/can_viewer_dialog.h
    src/ui/channel_editor_dialog.cpp
    src/ui/channel_editor_dialog.h
    src/ui/check_channels_dialog.cpp
    src/ui/check_channels_dialog.h
    src/ui/communications_dialog.cpp
    src/ui/communications_dialog.h
    src/ui/conditions_dialog.cpp
    src/ui/conditions_dialog.h
    src/ui/config_summary_dialog.cpp
    src/ui/config_summary_dialog.h
    src/ui/constants_dialog.cpp
    src/ui/constants_dialog.h
    src/ui/tables_dialog.cpp
    src/ui/tables_dialog.h
    src/ui/axis_setup_dialog.cpp
    src/ui/axis_setup_dialog.h
    src/ui/numeric_grid.cpp
    src/ui/numeric_grid.h
    src/ui/color_item_delegate.h
    src/ui/trimmed_spin_box.h
    src/ui/wheel_guard.h
    src/ui/counters_dialog.cpp
    src/ui/counters_dialog.h
    src/ui/import_dbc_dialog.cpp
    src/ui/import_dbc_dialog.h
    src/ui/timers_dialog.cpp
    src/ui/timers_dialog.h
    src/ui/integrators_dialog.cpp
    src/ui/integrators_dialog.h
    src/ui/connection_settings_dialog.cpp
    src/ui/connection_settings_dialog.h
    src/ui/edit_channel_dialog.cpp
    src/ui/edit_channel_dialog.h
    src/ui/firmware_update_dialog.cpp
    src/ui/firmware_update_dialog.h
    src/ui/fleet_identity_dialog.cpp
    src/ui/fleet_identity_dialog.h
    src/ui/help_window.cpp
    src/ui/help_window.h
    src/ui/main_window.cpp
    src/ui/main_window.h
    src/ui/math_dialog.cpp
    src/ui/math_dialog.h
    src/ui/monitor_channels_dialog.cpp
    src/ui/monitor_channels_dialog.h
    src/ui/section_editor_dialog.cpp
    src/ui/section_editor_dialog.h
    src/ui/select_channel_dialog.cpp
    src/ui/select_channel_dialog.h
    src/ui/upload_dialog.cpp
    src/ui/upload_dialog.h
    # The Windows resource script: the icon Explorer, Task Manager and
    # Add/Remove Programs draw on the executable, plus its VS_VERSION_INFO
    # block. Compiled by windres, which only runs because RC is in the
    # project() language list at the top of this file. It is listed here rather
    # than configured into the build tree because the ICON line inside it names
    # ../Icons/icons/app.ico relative to the .rc's own directory, which is how
    # windres resolves it — a generated copy elsewhere would not find the file.
    resources/app.rc
)

# The application icon as a Qt resource. This is the RUNTIME icon and a wholly
# separate mechanism from the .rc above: src/main.cpp builds a QIcon from the
# PNGs in here and hands it to QApplication::setWindowIcon(), which is what the
# title bar, the taskbar button, Alt-Tab and every dialog draw. The .rc's icon
# is what the shell draws on the file. Same artwork, two consumers, neither able
# to stand in for the other.
#
# First argument a variable rather than the target on purpose: that is the
# signature of qt_add_resources that takes an existing .qrc file. The target
# form builds a resource out of a FILES list instead and would not read
# resources/app.qrc at all.
qt_add_resources(APP_RESOURCES resources/app.qrc)
target_sources(CANTripleDeviceManager PRIVATE ${APP_RESOURCES})

# windres reads the .ico named inside app.rc, but that dependency is invisible
# to the build system: to CMake the .rc is one source file with no inputs. Edit
# the artwork and nothing rebuilds, while the installer re-reads the same .ico
# on every run — so Setup.exe would ship one icon and the program it installs
# another. Naming the .ico explicitly keeps the two honest.
set_property(SOURCE resources/app.rc APPEND PROPERTY
    OBJECT_DEPENDS "${CMAKE_SOURCE_DIR}/Icons/icons/app.ico")

target_link_libraries(CANTripleDeviceManager PRIVATE
    Qt6::Widgets Qt6::SerialPort Qt6::PrintSupport Qt6::Network Qt6::Help
    ct_lua ct_device_core)

# For fw_image.h — the flash map, the image header and the validation rule the
# bootloader, the application and this program all share. firmware/ is a
# directory junction to the firmware tree, so this is the same header the
# device is built against, not a copy.
target_include_directories(CANTripleDeviceManager PRIVATE ${FIRMWARE_DIR}/include)

# The version the program reports about itself — src/main.cpp feeds this to
# QCoreApplication::setApplicationVersion(), so About, the window title and
# anything else reading applicationVersion() all trace back here. It is the same
# PROJECT_VERSION the `installer` target passes to the .iss as /DMyAppVersion,
# which is the whole point: the number is edited in the project() call at the
# top of this file and nowhere else, so the Setup, Apps & Features and the
# running program cannot disagree about which release this is.
#
# CT_VER_MAJOR/MINOR/PATCH are that same number a second time, split into bare
# integers, because resources/app.rc cannot use the string: a VS_VERSION_INFO
# block's FILEVERSION field is four comma-separated numbers, not text. CMake
# passes a target's compile definitions to windres as well as to the C++
# compiler, so one call feeds both and the .rc hardcodes nothing — which is the
# point, since a version resource that disagreed with the About box would be
# worse than no version resource at all. The .rc supplies its own 0.0.0 fallback
# for a build that somehow loses these, the same way src/main.cpp does.
target_compile_definitions(CANTripleDeviceManager PRIVATE
    CT_APP_VERSION="${PROJECT_VERSION}"
    CT_VER_MAJOR=${PROJECT_VERSION_MAJOR}
    CT_VER_MINOR=${PROJECT_VERSION_MINOR}
    CT_VER_PATCH=${PROJECT_VERSION_PATCH})

# ---------------------------------------------------------------------------
# Offline help (Help > Contents…, F1): help/cantriple.qhp lists the manual's
# HTML pages; qhelpgenerator compiles them into cantriple.qch and registers
# that into the cantriple.qhc collection the in-app viewer opens.
#
# The whole help/ tree is staged into the build directory first and
# qhelpgenerator runs on the staged copy, because it writes its outputs next
# to the project file it reads — run in the source tree it would strand
# generated binaries there. Staging also keeps the .qhcp's relative
# references (cantriple.qhp beside it, pages/ below it) valid without any
# path surgery.
# ---------------------------------------------------------------------------
get_filename_component(_qt_tools_bin "${Qt6_DIR}/../../../bin" ABSOLUTE)
find_program(QHELPGENERATOR_EXE qhelpgenerator HINTS "${_qt_tools_bin}")
if(NOT QHELPGENERATOR_EXE)
    message(FATAL_ERROR "qhelpgenerator not found (looked in ${_qt_tools_bin} and PATH). "
                        "It ships with Qt's Help module and is required to build the manual; "
                        "set QHELPGENERATOR_EXE if it lives elsewhere.")
endif()

set(HELP_SRC_DIR "${CMAKE_SOURCE_DIR}/help")
set(HELP_BIN_DIR "${CMAKE_BINARY_DIR}/help")

# Every page the .qhp lists, so touching any one of them rebuilds the help.
set(HELP_PAGES
    help.css
    index.html
    getting-started.html
    files.html
    troubleshooting.html
    communications.html
    channels.html
    dbc-import.html
    relays.html
    math-channels.html
    conditions.html
    constants.html
    tables.html
    counters.html
    timers.html
    integrators.html
    device-scripts.html
    engine.html
    monitor.html
    scripting.html
    datatypes.html
    online.html
    firmware-update.html
    fleet-identity.html
    validation-report.html
    license.html
)
set(HELP_PAGE_PATHS "")
foreach(_page ${HELP_PAGES})
    list(APPEND HELP_PAGE_PATHS "${HELP_SRC_DIR}/pages/${_page}")
endforeach()

# The copy-beside-the-exe step rides the generation command, NOT a POST_BUILD
# hook on the executable: HelpWindow reads the pair next to the binary, and a
# doc-only edit must refresh those copies even though the executable never
# relinks.
add_custom_command(
    OUTPUT "${HELP_BIN_DIR}/cantriple.qch" "${HELP_BIN_DIR}/cantriple.qhc"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${HELP_SRC_DIR}" "${HELP_BIN_DIR}"
    COMMAND "${QHELPGENERATOR_EXE}" "${HELP_BIN_DIR}/cantriple.qhcp"
            -o "${HELP_BIN_DIR}/cantriple.qhc"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${HELP_BIN_DIR}/cantriple.qch" "${HELP_BIN_DIR}/cantriple.qhc"
            "$<TARGET_FILE_DIR:CANTripleDeviceManager>"
    DEPENDS "${HELP_SRC_DIR}/cantriple.qhp" "${HELP_SRC_DIR}/cantriple.qhcp"
            ${HELP_PAGE_PATHS}
    COMMENT "Generating Qt Help (cantriple.qch / cantriple.qhc)"
    VERBATIM
)
add_custom_target(help_docs ALL
    DEPENDS "${HELP_BIN_DIR}/cantriple.qch" "${HELP_BIN_DIR}/cantriple.qhc")
add_dependencies(CANTripleDeviceManager help_docs)

# Console self-test: protocol framing, extraction mapping, JSON round-trips.
qt_add_executable(test_roundtrip
    test/test_roundtrip.cpp
    src/protocol/asc_log.cpp
    src/protocol/cobs.cpp
    src/protocol/framer.cpp
    src/protocol/device_link.cpp
    # For the plan-classification seam: the test builds REAL Send/Get plans and
    # asks the guard whether every read step is classified — the check that has
    # been missed by hand six times now runs in every test pass.
    src/protocol/config_transfer.cpp
    src/model/access_keys.cpp
    src/model/channel_catalog.cpp
    src/model/config_lock.cpp
    src/model/config_report.cpp
    src/model/configuration.cpp
    src/model/dbc_import.cpp
    src/model/device_mapper.cpp
    src/model/secure_file.cpp
    src/model/config_file.cpp
    src/model/user_paths.cpp
    src/model/validation.cpp
    # THE DEVICE'S SCRIPT VERIFIER, here because device_mapper.cpp needs it and
    # not because this test asks for it. A Get now KEEPS the compiled script
    # image a device returns, and mapFromDevice validates it with the firmware's
    # own script_verify() before storing anything — the same argument fw_image.c
    # carries in the update path: the answer to "would the unit accept these
    # bytes?" should be the UNIT'S answer, not a host reimplementation of the
    # format. So every target that links the mapper links ct_device_core (which
    # carries script_vm) too. It is self-contained C with no engine
    # dependencies, which is what makes that cheap rather than a creeping
    # dependency.
)
target_include_directories(test_roundtrip PRIVATE ${FIRMWARE_DIR}/include)
target_link_libraries(test_roundtrip PRIVATE Qt6::Core Qt6::SerialPort Qt6::Network
    ct_device_core)

# Integration test: GUI protocol stack against the REAL firmware core in
# firmware/ (engine/serial/flash/identity modules compiled for the host).
# Full-tree only: the engine and protocol core it compiles are not part of the
# public source release (see the FIRMWARE_FULL note at the resolver).
#
# fleet_identity.c is in the list because serial_proto.c answers CMD_READ_FLEET_ID
# out of it — without it the link fails on fleet_identity() rather than on
# anything the test is about. Note it is compiled here with NO -DCT_* flags, so
# the test always faces an UNPROVISIONED unit: the identity is a build input, and
# the honest way to exercise a provisioned one would be to rebuild the firmware,
# not to poke a value in over the wire (there is no command that can).
if(FIRMWARE_FULL)
qt_add_executable(test_firmware_link
    test/test_firmware_link.cpp
    src/protocol/cobs.cpp
    src/protocol/framer.cpp
    src/protocol/device_session.cpp
    src/protocol/device_link.cpp
    src/model/access_keys.cpp
    src/model/channel_catalog.cpp
    src/model/config_lock.cpp
    src/model/config_report.cpp
    src/model/configuration.cpp
    src/model/dbc_import.cpp
    src/model/device_mapper.cpp
    src/model/secure_file.cpp
    src/model/config_file.cpp
    src/model/user_paths.cpp
    src/model/validation.cpp
    ${FIRMWARE_DIR}/src/engine_core.c
    # The MathOp evaluator, split out of engine_core.c so the script VM can link
    # without the engine. engine_core.c calls into it too, so it travels with it.
    ${FIRMWARE_DIR}/src/engine_math.c
    ${FIRMWARE_DIR}/src/serial_proto.c
    ${FIRMWARE_DIR}/src/flash_store.c
    ${FIRMWARE_DIR}/src/fleet_identity.c
    ${FIRMWARE_DIR}/src/sha256.c
    ${FIRMWARE_DIR}/src/cobs.c
    # The firmware-update path, compiled for the host so its state machine is
    # exercised off-hardware. fw_host_stub.c supplies the RAM model those
    # modules read through FW_FLASH_PTR, a NOR-accurate erase/program driver,
    # and the g_fw_header symbol the status report reads.
    ${FIRMWARE_DIR}/src/fw_update.c
    ${FIRMWARE_DIR}/src/bcb.c
    ${FIRMWARE_DIR}/src/fw_image.c
    # The script VM: engine_core.c calls into it from engine_tick and
    # engine_load_config, so the firmware-link test needs it to link at all —
    # which also means this test now exercises the engine WITH scripting
    # present, exactly as the device runs.
    ${FIRMWARE_DIR}/src/script_exec.c
    ${FIRMWARE_DIR}/src/script_cycles.c
    ${FIRMWARE_DIR}/src/script_vm.c
    test/fw_host_stub.c
)
target_include_directories(test_firmware_link PRIVATE ${FIRMWARE_DIR}/include)
# SerialPort comes in with device_session -> device_link. The test never opens a
# port; it only needs the wire-format parsers, which live on the same side of
# the wall as the transport.
target_link_libraries(test_firmware_link PRIVATE Qt6::Core Qt6::Network Qt6::SerialPort)

# Unit test: the retained-value (PRESERVE) 2-page flash ring, exercised against
# RAM pages. Pure firmware logic — no Qt needed. Full-tree only, like
# test_firmware_link above: preserve_store.c is engine internals.
add_executable(test_preserve
    test/test_preserve.cpp
    ${FIRMWARE_DIR}/src/preserve_store.c
)
target_include_directories(test_preserve PRIVATE ${FIRMWARE_DIR}/include)
endif() # FIRMWARE_FULL

# Unit test: the script VM verifier and cost model, exhaustively. Pure firmware
# logic (script_vm.c has no engine deps) — no Qt. This guards the safety
# foundation the whole scripting feature rests on; every rejection reason has a
# case, so a verifier that stopped rejecting one would fail here.
add_executable(test_script_vm
    test/test_script_vm.cpp
)
target_include_directories(test_script_vm PRIVATE ${FIRMWARE_DIR}/include)
target_link_libraries(test_script_vm PRIVATE ct_device_core)

# Unit test: the script INTERPRETER, against a RAM signal table. Where
# test_script_vm covers memory safety (the verifier), this covers the other
# half of the contract — liveness — which the verifier deliberately does not
# provide: it accepts an infinite self-jump as legal, so only the interpreter's
# per-dispatch budget stops one. Each of INTERPRETER REQUIREMENTS R1-R5 has a
# test that fails if the requirement is dropped.
#
# engine_math.c comes along for engine_math_eval, the evaluator the VM shares
# with executeMath — which is exactly the point: the test exercises the real
# arithmetic, not a stand-in. It used to be engine_core.c + flash_store.c;
# extracting the evaluator into its own translation unit is what let the whole
# engine drop out of this link, here and in the configurator.
add_executable(test_script_exec
    test/test_script_exec.cpp
)
target_include_directories(test_script_exec PRIVATE ${FIRMWARE_DIR}/include)
target_link_libraries(test_script_exec PRIVATE ct_device_core)

# scriptc — the script compiler as a command-line tool, so the HARDWARE suite
# can get an image out of the real compiler as a file. Without it the only way
# to put GUI-compiled bytecode on a board would be to drive the dialog by hand,
# and the cross-language check (does the device's verifier accept what this
# compiler emits, and does the board compute what the desktop simulator said?)
# would not be automatable at all.
qt_add_executable(scriptc
    src/tools/scriptc_main.cpp
    src/scripting/script_compiler.cpp
    src/scripting/script_simulator.cpp
    src/model/access_keys.cpp
    src/model/channel_catalog.cpp
    src/model/config_lock.cpp
    src/model/config_report.cpp
    src/model/configuration.cpp
    src/model/dbc_import.cpp
    src/model/device_mapper.cpp
    src/model/secure_file.cpp
    src/model/config_file.cpp
    src/model/user_paths.cpp
    src/model/validation.cpp
)
target_include_directories(scriptc PRIVATE ${FIRMWARE_DIR}/include)
# No Widgets: a compiler has no business linking a GUI toolkit, and keeping it
# out is what lets this run on a build machine with no display.
target_link_libraries(scriptc PRIVATE ct_lua Qt6::Core Qt6::Network ct_device_core)

# The Lua-subset compiler, end to end: compile Lua -> the DEVICE's verifier ->
# the DEVICE's interpreter -> check the signal values. Linking the real VM
# rather than a stub is the whole point; bytecode is unreadable by inspection,
# so running it is the only way to know the compiler emitted what it meant to.
qt_add_executable(test_script_compiler
    test/test_script_compiler.cpp
    src/scripting/script_compiler.cpp
    src/model/access_keys.cpp
    src/model/channel_catalog.cpp
    src/model/config_lock.cpp
    src/model/config_report.cpp
    src/model/configuration.cpp
    src/model/dbc_import.cpp
    src/model/device_mapper.cpp
    src/model/secure_file.cpp
    src/model/config_file.cpp
    src/model/user_paths.cpp
    src/model/validation.cpp
)
target_include_directories(test_script_compiler PRIVATE ${FIRMWARE_DIR}/include)
target_link_libraries(test_script_compiler PRIVATE ct_lua Qt6::Core Qt6::Network
    ct_device_core)

# The desktop simulator and the script's path to a device. Where
# test_script_compiler proves the compiler emits what it meant to, this proves
# the two things that stand between a correct compiler and a working feature:
# that the simulator's answers are the device's, and that the bytecode actually
# reaches the unit through mapWithScript() and the chunk table.
qt_add_executable(test_script_sim
    test/test_script_sim.cpp
    src/scripting/script_compiler.cpp
    # The disassembler is here rather than only under the dialog because what it
    # decodes is the retained image this test builds and round-trips: its
    # listing is checked against the instructions the compiler is known to have
    # emitted, which needs no widgets.
    src/scripting/script_disassembler.cpp
    src/scripting/script_simulator.cpp
    src/model/access_keys.cpp
    src/model/channel_catalog.cpp
    src/model/config_lock.cpp
    src/model/config_report.cpp
    src/model/configuration.cpp
    src/model/dbc_import.cpp
    src/model/device_mapper.cpp
    src/model/secure_file.cpp
    src/model/config_file.cpp
    src/model/user_paths.cpp
    src/model/validation.cpp
)
target_include_directories(test_script_sim PRIVATE ${FIRMWARE_DIR}/include)
target_link_libraries(test_script_sim PRIVATE ct_lua Qt6::Core Qt6::Network
    ct_device_core)

# The Device Script DIALOG, driven through its own buttons on the offscreen
# platform. Everything below it is covered by the two tests above; this covers
# the wiring, and in particular the one failure that would be silent — OK not
# writing the script back into the document.
qt_add_executable(test_script_editor
    test/test_script_editor.cpp
    src/ui/script_editor_dialog.cpp
    src/ui/lua_console_dialog.cpp      # LuaHighlighter
    src/scripting/script_compiler.cpp
    src/scripting/script_disassembler.cpp
    src/scripting/script_simulator.cpp
    src/scripting/script_runner.cpp
    src/scripting/lua_sandbox.cpp
    src/scripting/config_bindings.cpp
    src/model/access_keys.cpp
    src/model/channel_catalog.cpp
    src/model/config_lock.cpp
    src/model/config_report.cpp
    src/model/configuration.cpp
    src/model/dbc_import.cpp
    src/model/device_mapper.cpp
    src/model/secure_file.cpp
    src/model/config_file.cpp
    src/model/user_paths.cpp
    src/model/validation.cpp
)
target_include_directories(test_script_editor PRIVATE ${FIRMWARE_DIR}/include)
target_link_libraries(test_script_editor PRIVATE ct_lua Qt6::Widgets Qt6::Core Qt6::Network
    ct_device_core)

# Can the manual be READ from where F1 offers it? Two of the dialogs that offer
# help are modal, and a modal dialog does not merely cover other windows - it
# blocks input to them. This pins the arrangement that keeps the help window
# reachable, and does it for every dialog that offers help rather than for the
# two that do today. Run it with --live on a machine with a desktop and it
# checks the blocking itself, via Win32 IsWindowEnabled, with a polarity
# control; that mode is deliberately not in the sweep since it needs a session.
qt_add_executable(test_help_access
    test/test_help_access.cpp
    src/ui/help_window.cpp
    src/ui/script_editor_dialog.cpp
    src/ui/firmware_update_dialog.cpp
    src/ui/lua_console_dialog.cpp
    src/protocol/device_link.cpp
    src/protocol/device_session.cpp
    src/protocol/framer.cpp
    src/protocol/cobs.cpp
    src/protocol/firmware_image.cpp
    src/protocol/firmware_update.cpp
    src/protocol/config_transfer.cpp
    src/protocol/asc_log.cpp
    src/scripting/script_compiler.cpp
    src/scripting/script_disassembler.cpp
    src/scripting/script_simulator.cpp
    src/scripting/script_runner.cpp
    src/scripting/lua_sandbox.cpp
    src/scripting/config_bindings.cpp
    src/model/access_keys.cpp
    src/model/channel_catalog.cpp
    src/model/config_lock.cpp
    src/model/config_report.cpp
    src/model/configuration.cpp
    src/model/dbc_import.cpp
    src/model/device_mapper.cpp
    src/model/secure_file.cpp
    src/model/config_file.cpp
    src/model/user_paths.cpp
    src/model/validation.cpp
)
target_include_directories(test_help_access PRIVATE ${FIRMWARE_DIR}/include)
target_link_libraries(test_help_access
    PRIVATE ct_lua Qt6::Widgets Qt6::Help Qt6::SerialPort Qt6::PrintSupport
            Qt6::Core Qt6::Network ct_device_core)

# Channel labels show the unit; channel IDENTITY stays the bare name. The
# dialogs' read-only "Input :" boxes used to BE the identity — decorating them
# without separating the two jobs rewrites the configuration to name a channel
# that does not exist, silently. This drives each calculation editor open and
# shut and checks the document is unchanged.
qt_add_executable(test_channel_labels
    test/test_channel_labels.cpp
    src/ui/math_dialog.cpp
    src/ui/conditions_dialog.cpp
    src/ui/counters_dialog.cpp
    src/ui/timers_dialog.cpp
    src/ui/integrators_dialog.cpp
    src/ui/tables_dialog.cpp
    src/ui/numeric_grid.cpp
    src/ui/axis_setup_dialog.cpp
    src/ui/select_channel_dialog.cpp
    src/ui/edit_channel_dialog.cpp
    src/model/access_keys.cpp
    src/model/channel_catalog.cpp
    src/model/config_lock.cpp
    src/model/config_report.cpp
    src/model/configuration.cpp
    src/model/dbc_import.cpp
    src/model/device_mapper.cpp
    src/model/secure_file.cpp
    src/model/config_file.cpp
    src/model/user_paths.cpp
    src/model/validation.cpp
)
target_include_directories(test_channel_labels PRIVATE ${FIRMWARE_DIR}/include)
target_link_libraries(test_channel_labels PRIVATE Qt6::Widgets Qt6::Core Qt6::Network
    ct_device_core) # device_mapper.cpp — see test_roundtrip

# ct3check - load a configuration, map it, and report what it costs, without a
# device or a GUI. Same three calls the Manager makes on a Send, so a file this
# accepts is one the Manager accepts. Not a test: it takes a path and is meant
# to be run by hand or from a script (exit 0 = clean).
qt_add_executable(ct3check
    tools/ct3check.cpp
    src/scripting/script_compiler.cpp   # mapWithScript — the Send's mapping call
    src/model/access_keys.cpp
    src/model/channel_catalog.cpp
    src/model/config_lock.cpp
    src/model/config_report.cpp
    src/model/configuration.cpp
    src/model/dbc_import.cpp
    src/model/device_mapper.cpp
    src/model/secure_file.cpp
    src/model/config_file.cpp
    src/model/user_paths.cpp
    src/model/validation.cpp
)
target_include_directories(ct3check PRIVATE ${FIRMWARE_DIR}/include)
target_link_libraries(ct3check PRIVATE ct_lua Qt6::Core Qt6::Gui Qt6::Network
    ct_device_core) # device_mapper.cpp - see test_roundtrip

# CANTripleInitialProgramming - program the bootloader and application over
# the built-in ST-LINK, via a bundled OpenOCD. First programming of a
# factory-fresh board and the unbrick path in one: it depends on no code
# running on the target, so it works on a blank, corrupt or reset-looping
# unit, and a failed attempt is always recoverable by running it again.
# Validates both images with the device's own code (fw_image.c) before any
# flash is touched — the same compile-the-firmware's-sources pattern as the
# tests and ct3check, so the tool and the bootloader cannot disagree about
# what a valid image is.
#
# Plain add_executable, no Qt, and statically linked: it ships in the
# Firmware/ subfolder of an install, away from the Qt and MinGW DLLs in the
# program directory, and the whole folder is meant to be copyable onto a USB
# stick as a self-contained programming kit. A DLL dependency would quietly
# break both properties.
add_executable(CANTripleInitialProgramming
    tools/initial_programming_tool.cpp
)
target_include_directories(CANTripleInitialProgramming PRIVATE ${FIRMWARE_DIR}/include)
target_link_libraries(CANTripleInitialProgramming PRIVATE ct_device_core) # fw_image
target_link_options(CANTripleInitialProgramming PRIVATE -static)

# Multiple selection in Communications Setup, driven through the real dialog.
# The list order is the TRANSMIT order, so a reorder that loses a message or
# changes the order within the moved group corrupts a configuration in a way
# nothing downstream would flag — every section stays individually valid.
qt_add_executable(test_comms_multiselect
    test/test_comms_multiselect.cpp
    src/ui/communications_dialog.cpp
    src/ui/section_editor_dialog.cpp
    src/ui/import_dbc_dialog.cpp
    src/ui/select_channel_dialog.cpp
    src/ui/edit_channel_dialog.cpp
    src/ui/add_channel_dialog.cpp
    src/ui/bit_layout_table.cpp
    src/ui/numeric_grid.cpp
    src/model/access_keys.cpp
    src/model/channel_catalog.cpp
    src/model/comms_template.cpp
    src/model/config_lock.cpp
    src/model/config_report.cpp
    src/model/configuration.cpp
    src/model/dbc_import.cpp
    src/model/device_mapper.cpp
    src/model/secure_file.cpp
    src/model/config_file.cpp
    src/model/user_paths.cpp
    src/model/validation.cpp
)
target_include_directories(test_comms_multiselect PRIVATE ${FIRMWARE_DIR}/include)
target_link_libraries(test_comms_multiselect PRIVATE Qt6::Widgets Qt6::Core Qt6::Network
    ct_device_core) # device_mapper.cpp — see test_roundtrip

# Communications templates: the .ct3t container, the channel merge, and the
# Save… / Load… buttons driven through the real Communications Setup dialog.
#
# Three properties this pins, and each of them is a way a template quietly
# corrupts a configuration rather than failing:
#   1. the file gives up NOTHING to a text search — that is the entire reason it
#      is not JSON, and "encrypted" is a claim worth testing rather than
#      asserting;
#   2. loading never rewrites a channel the target document already had, because
#      re-scaling somebody's existing channel changes what every other message
#      using it decodes to, silently;
#   3. a template round-trips a message BYTE FOR BYTE through its JSON — a
#      dropped field is a message that looks right in the list and is wrong on
#      the wire.
qt_add_executable(test_comms_template
    test/test_comms_template.cpp
    src/ui/communications_dialog.cpp
    src/ui/section_editor_dialog.cpp
    src/ui/import_dbc_dialog.cpp
    src/ui/select_channel_dialog.cpp
    src/ui/edit_channel_dialog.cpp
    src/ui/add_channel_dialog.cpp
    src/ui/bit_layout_table.cpp
    src/ui/numeric_grid.cpp
    src/model/access_keys.cpp
    src/model/channel_catalog.cpp
    src/model/comms_template.cpp
    src/model/config_lock.cpp
    src/model/config_report.cpp
    src/model/configuration.cpp
    src/model/dbc_import.cpp
    src/model/device_mapper.cpp
    src/model/secure_file.cpp
    src/model/config_file.cpp
    src/model/user_paths.cpp
    src/model/validation.cpp
)
target_include_directories(test_comms_template PRIVATE ${FIRMWARE_DIR}/include)
target_link_libraries(test_comms_template PRIVATE Qt6::Widgets Qt6::Core Qt6::Network
    ct_device_core) # device_mapper.cpp — see test_roundtrip

# The CAN Viewer's display filters and Overwrite Mode, driven through the real
# dialog. Two properties this pins: a filter is a view and never a loss (the
# capture buffer is what Save to File writes), and an Overwrite Mode row always
# holds its identifier's LATEST frame — a stale row in a view whose whole
# purpose is "the value right now" reads as live data.
qt_add_executable(test_can_viewer
    test/test_can_viewer.cpp
    src/ui/can_viewer_dialog.cpp
    src/protocol/asc_log.cpp
    src/protocol/cobs.cpp
    src/protocol/device_link.cpp
    src/protocol/framer.cpp
)
target_include_directories(test_can_viewer PRIVATE ${FIRMWARE_DIR}/include)
target_link_libraries(test_can_viewer PRIVATE Qt6::Widgets Qt6::Core Qt6::Network
    Qt6::SerialPort)

# Every shipped device-script example, compiled and run on the real VM. These
# files are installed with the program and offered by the editor's Load Script…,
# so one that does not compile — or that the engine kills for exceeding its tick
# budget — reads to a user as a broken product. Headless: no Qt GUI needed.
qt_add_executable(test_script_examples
    test/test_script_examples.cpp
    src/scripting/script_compiler.cpp
    src/scripting/script_simulator.cpp
    src/model/access_keys.cpp
    src/model/channel_catalog.cpp
    src/model/config_lock.cpp
    src/model/config_report.cpp
    src/model/configuration.cpp
    src/model/dbc_import.cpp
    src/model/device_mapper.cpp
    src/model/secure_file.cpp
    src/model/config_file.cpp
    src/model/user_paths.cpp
    src/model/validation.cpp
)
target_include_directories(test_script_examples PRIVATE ${FIRMWARE_DIR}/include)
target_compile_definitions(test_script_examples PRIVATE
    CT_EXAMPLES_DIR="${CMAKE_SOURCE_DIR}/examples/scripts")
target_link_libraries(test_script_examples PRIVATE ct_lua Qt6::Core Qt6::Network
    ct_device_core)

# The table editors' clipboard and the Axis Setup dialog, on the offscreen
# platform. Covers GridClipboard's TSV/clamp/skip mechanics directly, and drives
# the axis window through its own buttons — the one thing that would be silent is
# OK not returning what the grid shows.
qt_add_executable(test_tables
    test/test_tables.cpp
    src/ui/axis_setup_dialog.cpp
    src/ui/numeric_grid.cpp
    src/ui/select_channel_dialog.cpp
    src/ui/edit_channel_dialog.cpp
    src/model/access_keys.cpp
    src/model/channel_catalog.cpp
    src/model/config_lock.cpp
    src/model/config_report.cpp
    src/model/configuration.cpp
    src/model/dbc_import.cpp
    src/model/device_mapper.cpp
    src/model/secure_file.cpp
    src/model/config_file.cpp
    src/model/user_paths.cpp
    src/model/validation.cpp
)
target_include_directories(test_tables PRIVATE ${FIRMWARE_DIR}/include)
target_link_libraries(test_tables PRIVATE Qt6::Widgets Qt6::Core Qt6::Network
    ct_device_core) # device_mapper.cpp — see test_roundtrip

# The scripting layer, headless: sandbox fences, ct.* bindings against a real
# Configuration, and the runner's rollback contract. Drives the same
# ScriptRunner the Lua Console does — the dialog is a thin shell over it.
qt_add_executable(test_lua
    test/test_lua.cpp
    src/scripting/lua_sandbox.cpp
    src/scripting/config_bindings.cpp
    src/scripting/script_runner.cpp
    src/model/access_keys.cpp
    src/model/channel_catalog.cpp
    src/model/config_lock.cpp
    src/model/config_report.cpp
    src/model/configuration.cpp
    src/model/dbc_import.cpp
    src/model/device_mapper.cpp
    src/model/secure_file.cpp
    src/model/config_file.cpp
    src/model/user_paths.cpp
    src/model/validation.cpp
)
target_include_directories(test_lua PRIVATE ${FIRMWARE_DIR}/include)
target_link_libraries(test_lua PRIVATE ct_lua Qt6::Core Qt6::Network
    ct_device_core) # device_mapper.cpp — see test_roundtrip

# The per-message protection TIER: the ordered level, its JSON and wire
# encodings, the schema 13 -> 14 migration, the applyBusSections untick
# chokepoint, what a Get preserves, and isChannelConcealed vs
# isChannelEditLocked. Separate from test_roundtrip on purpose — as of 2.3.0 the
# device enforces nothing about message protection, so these host rules are the
# only ones left holding the feature up and they deserve a binary whose failure
# names the subject.
#
# It links the two DIALOGS as well as the model, and that is not scope creep.
# 2.3.1's rules — every marked tier carries a password of its own, changing a
# tier proves the old one and then sets a new one, and a still-concealing
# section re-locks when its editor closes — are enforced in
# SectionEditorDialog::accept() and in CommunicationsDialog, and Configuration
# cannot answer for any of them: it deliberately records only THAT a section's
# challenges were met, never which ones ran. Testing them without the widgets
# would be testing a paraphrase. Runs offscreen; the source sets QT_QPA_PLATFORM
# itself.
qt_add_executable(test_protection
    test/test_protection.cpp
    src/ui/communications_dialog.cpp
    src/ui/section_editor_dialog.cpp
    src/ui/import_dbc_dialog.cpp
    src/ui/select_channel_dialog.cpp
    src/ui/channel_editor_dialog.cpp
    src/ui/edit_channel_dialog.cpp
    src/ui/add_channel_dialog.cpp
    src/ui/bit_layout_table.cpp
    src/ui/numeric_grid.cpp
    src/model/access_keys.cpp
    src/model/channel_catalog.cpp
    src/model/comms_template.cpp
    src/model/config_lock.cpp
    src/model/config_report.cpp
    src/model/configuration.cpp
    src/model/dbc_import.cpp
    src/model/device_mapper.cpp
    src/model/secure_file.cpp
    src/model/config_file.cpp
    src/model/user_paths.cpp
    src/model/validation.cpp
)
target_include_directories(test_protection PRIVATE ${FIRMWARE_DIR}/include)
target_link_libraries(test_protection PRIVATE Qt6::Widgets Qt6::Core Qt6::Network
    ct_device_core) # device_mapper.cpp — see test_roundtrip

# ---------------------------------------------------------------------------
# Standalone deployment: `cmake --build build --target deploy`
#
# Produces deploy/ containing everything needed to run on a PC with no Qt and
# no MinGW installed. Two halves, and both are required:
#   1. windeployqt  -> Qt DLLs + the plugin subdirs (platforms/ especially;
#                      without qwindows.dll the app dies with "no Qt platform
#                      plugin could be initialized").
#   2. the MinGW C/C++ runtime. windeployqt --compiler-runtime is NOT used here
#      on purpose: it copies from whichever MinGW it finds, which is Qt's
#      bundled GCC 11.2.0. This project links with GCC 13.1.0, whose libstdc++
#      exports symbol versions (GLIBCXX_3.4.30+) the 11.2 DLL lacks. Shipping
#      the older one gives a "procedure entry point not found" box at launch.
#      So the runtime is copied from the actual CMAKE_CXX_COMPILER's own bin dir.
#
# The licences are staged here too, and this is the only place they are staged.
# deploy/ is the payload of BOTH distribution channels — `package` zips it and
# the Inno script installs it wholesale — so putting the notices in the payload
# means neither channel can ship binaries without them, and there is one copy to
# keep correct rather than one per channel.
# ---------------------------------------------------------------------------
if(WIN32)
    get_filename_component(_qt_bin "${Qt6_DIR}/../../../bin" ABSOLUTE)
    find_program(WINDEPLOYQT_EXE windeployqt HINTS "${_qt_bin}")

    # Runtime DLLs live beside the compiler that linked the exe.
    get_filename_component(_cxx_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set(MINGW_RUNTIME_DLLS "")
    foreach(_dll libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll)
        if(EXISTS "${_cxx_bin}/${_dll}")
            list(APPEND MINGW_RUNTIME_DLLS "${_cxx_bin}/${_dll}")
        else()
            message(WARNING "deploy: ${_dll} not found in ${_cxx_bin}")
        endif()
    endforeach()

    set(DEPLOY_DIR "${CMAKE_SOURCE_DIR}/deploy")

    add_custom_target(deploy
        COMMAND ${CMAKE_COMMAND} -E rm -rf "${DEPLOY_DIR}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${DEPLOY_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy
                $<TARGET_FILE:CANTripleDeviceManager> "${DEPLOY_DIR}"
        COMMAND "${WINDEPLOYQT_EXE}" --release --no-translations
                --dir "${DEPLOY_DIR}" "${DEPLOY_DIR}/CANTripleDeviceManager.exe"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${MINGW_RUNTIME_DLLS} "${DEPLOY_DIR}"
        # The manual travels with the program — HelpWindow expects the pair
        # next to the executable.
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${HELP_BIN_DIR}/cantriple.qch" "${HELP_BIN_DIR}/cantriple.qhc"
                "${DEPLOY_DIR}"
        # And so do the licences. The MIT notice must accompany every copy of
        # the application, and LGPLv3 section 4 requires Qt's texts to travel
        # with the *binaries* — and the zip used to satisfy neither: it was a
        # folder of executables with no licence in it at all. Staging into
        # deploy/ fixes both channels at once (see the header comment above).
        #
        # COPYING is renamed on the way in for one reason: double-clicking a
        # file with no extension in Explorer opens the "how do you want to open
        # this file?" dialog, and .txt opens Notepad. The bytes are the
        # repository's own COPYING, unchanged.
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_SOURCE_DIR}/COPYING" "${DEPLOY_DIR}/COPYING.txt"
        # The other four are maintained in installer/ because that is where the
        # rest of the distribution paperwork lives; nothing but this command
        # copies them anywhere. LICENSE.LGPLv3.txt and GCC-RUNTIME-EXCEPTION.txt
        # are the verbatim texts the bundled Qt 6 and the MinGW runtime are used
        # under; LICENSE.GPLv3.txt is the GPL text both of those layer on
        # (carried for THEM — the application itself is MIT, see COPYING);
        # THIRD-PARTY-NOTICES.txt is the inventory that says which is which and
        # where the corresponding sources are.
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_SOURCE_DIR}/installer/LICENSE.LGPLv3.txt"
                "${CMAKE_SOURCE_DIR}/installer/LICENSE.GPLv3.txt"
                "${CMAKE_SOURCE_DIR}/installer/GCC-RUNTIME-EXCEPTION.txt"
                "${CMAKE_SOURCE_DIR}/installer/THIRD-PARTY-NOTICES.txt"
                "${DEPLOY_DIR}"
        # The worked device-script examples, so the editor's "Load Script…"
        # opens onto something useful on a machine that has never had a script
        # on it. Every one of them compiles and runs inside the tick budget —
        # test_script_examples proves it against the real compiler and VM.
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_SOURCE_DIR}/examples/scripts" "${DEPLOY_DIR}/examples"
        # The device's USB drivers — ST's stsw-link009 v3 package, vendored
        # verbatim in installer/drivers (the device presents as an STLINK-V3
        # composite: the Manager talks to its Virtual COM Port interface).
        # Windows Update provides the same driver on a connected machine, but
        # this tool lives on offline bench laptops, where "plug it in and wait"
        # ends at an unknown-device icon. Staged into deploy/ like everything
        # else so both distribution channels carry them and the installer's
        # single [Files] entry stays the only path into {app}.
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_SOURCE_DIR}/installer/drivers" "${DEPLOY_DIR}/Drivers"
        # The initial-programming tool, built from this repository: statically
        # linked, no DLL needs, so the Firmware/ folder works from a USB stick.
        #
        # The make_directory is NOT redundant with the one inside
        # stage_firmware.cmake below. `cmake -E copy_if_different <file> <dest>`
        # treats a dest that does not exist as a FILE NAME, so on a clean tree
        # this copy would silently produce a 500 KB file called Firmware and the
        # staging script would then fail trying to make a directory over it.
        COMMAND ${CMAKE_COMMAND} -E make_directory "${DEPLOY_DIR}/Firmware"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:CANTripleInitialProgramming>" "${DEPLOY_DIR}/Firmware"
        # The rest of Firmware/ — the .ctf this Manager release pairs with, the
        # bootloader image, and the OpenOCD kit — is staged by a script rather
        # than by copies listed here, because WHERE those files live depends on
        # which tree this is. The private tree builds them under .pio/; the
        # public repository ships the released binaries instead and has no
        # PlatformIO at all, so the copies this replaced died on it with a bare
        # file-not-found. See cmake/stage_firmware.cmake for the full reasoning,
        # including why the choice is made here at build time and not at
        # configure time.
        COMMAND ${CMAKE_COMMAND}
                -D "FIRMWARE_DIR=${FIRMWARE_DIR}"
                -D "DEPLOY_DIR=${DEPLOY_DIR}"
                -D "FW_VERSION=${FW_VERSION}"
                -D "PIO_PACKAGES=$ENV{USERPROFILE}/.platformio/packages"
                -P "${CMAKE_SOURCE_DIR}/cmake/stage_firmware.cmake"
        DEPENDS CANTripleDeviceManager CANTripleInitialProgramming help_docs
        COMMENT "Deploying standalone build to deploy/"
        VERBATIM
    )

    # Keep the in-tree build runnable after a fresh checkout/clean, so running
    # straight out of build/ behaves the same as the deployed copy.
    add_custom_command(TARGET CANTripleDeviceManager POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${MINGW_RUNTIME_DLLS} "$<TARGET_FILE_DIR:CANTripleDeviceManager>"
        VERBATIM
    )

    # `cmake --build build --target package` -> the distributable zip.
    # Staged through a CANTripleDeviceManager/ directory so the archive expands
    # into one named folder instead of spraying 32 files into the user's
    # Downloads.
    #
    # It is a straight copy of deploy/ and nothing else, which is how the zip
    # gets its licences: COPYING.txt and the three notice files are already in
    # the payload by the time this target runs. Nothing licence-shaped is listed
    # here on purpose — a second mechanism is a second thing to forget.
    set(PACKAGE_STAGE "${CMAKE_BINARY_DIR}/package")
    set(PACKAGE_ZIP "${CMAKE_SOURCE_DIR}/CANTripleDeviceManager-windows-x64.zip")

    # WORKING_DIRECTORY is the binary dir (which always exists) rather than the
    # stage dir, because the first command deletes the stage — the shell cannot
    # cd into it before it has been recreated. The tar step gets the directory
    # it needs via `cmake -E chdir`, so archive paths stay relative.
    add_custom_target(package
        COMMAND ${CMAKE_COMMAND} -E rm -rf "${PACKAGE_STAGE}"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${DEPLOY_DIR}" "${PACKAGE_STAGE}/CANTripleDeviceManager"
        COMMAND ${CMAKE_COMMAND} -E rm -f "${PACKAGE_ZIP}"
        COMMAND ${CMAKE_COMMAND} -E chdir "${PACKAGE_STAGE}"
                ${CMAKE_COMMAND} -E tar cf "${PACKAGE_ZIP}" --format=zip
                CANTripleDeviceManager
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
        COMMENT "Packaging ${PACKAGE_ZIP}"
        VERBATIM
    )
    add_dependencies(package deploy)

    # -----------------------------------------------------------------------
    # `cmake --build build --target installer` -> the Setup executable.
    #
    # Same payload as `package`, wrapped by Inno Setup instead of zipped:
    # installer/CANTripleDeviceManager.iss compresses the whole of deploy/ into
    # dist/CANTripleDeviceManager-<version>-Setup.exe. It depends on deploy for
    # the obvious reason, and the script refuses to compile if that directory is
    # missing rather than emitting an empty installer.
    #
    # PROJECT_VERSION is pushed in with /D so the number lives in exactly one
    # place — the project() call at the top of this file, the same source the
    # application's own CT_APP_VERSION comes from. The .iss defaults the same
    # value behind an #ifndef purely so it still compiles standalone; a build
    # through this target always overrides it.
    #
    # Inno Setup is not a build dependency of the application, so a machine
    # without it must still configure and build normally. Hence the missing
    # compiler is reported when someone asks for an installer, not at configure
    # time.
    # -----------------------------------------------------------------------
    find_program(ISCC_EXE ISCC
        HINTS "C:/Program Files (x86)/Inno Setup 6"
              "C:/Program Files/Inno Setup 6")

    set(DIST_DIR "${CMAKE_SOURCE_DIR}/dist")
    set(INSTALLER_ISS "${CMAKE_SOURCE_DIR}/installer/CANTripleDeviceManager.iss")

    if(ISCC_EXE)
        # /O overrides the script's own OutputDir. Both point at dist/; naming
        # it here as well keeps the output path under CMake's control for
        # anyone who moves the script or builds out of a different source tree.
        add_custom_target(installer
            COMMAND ${CMAKE_COMMAND} -E make_directory "${DIST_DIR}"
            COMMAND "${ISCC_EXE}" "/DMyAppVersion=${PROJECT_VERSION}"
                    "/O${DIST_DIR}" "${INSTALLER_ISS}"
            COMMENT "Building ${DIST_DIR}/CANTripleDeviceManager-${PROJECT_VERSION}-Setup.exe"
            VERBATIM
        )
        add_dependencies(installer deploy)
    else()
        add_custom_target(installer
            COMMAND ${CMAKE_COMMAND} -E echo
                    "ISCC.exe not found. Install Inno Setup 6.3 or newer from"
            COMMAND ${CMAKE_COMMAND} -E echo
                    "https://jrsoftware.org/isinfo.php, or configure with"
            COMMAND ${CMAKE_COMMAND} -E echo
                    "-DISCC_EXE=<path-to>/ISCC.exe if it is installed elsewhere."
            COMMAND ${CMAKE_COMMAND} -E false
            VERBATIM
        )
    endif()
endif()
