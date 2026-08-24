# Stage the firmware payload into deploy/Firmware, from whichever of the two
# tree shapes this checkout happens to be.
#
# Run at BUILD time by the deploy target:
#
#   cmake -D FIRMWARE_DIR=... -D DEPLOY_DIR=... -D FW_VERSION=...
#         -D PIO_PACKAGES=... -P cmake/stage_firmware.cmake
#
# BUILD time rather than configure time, and that is the reason this is a
# separate script instead of a few if() branches up in CMakeLists.txt. The
# private tree's .ctf is a PlatformIO output: it appears, and changes, long
# after CMake last ran. A source chosen at configure time would be remembered,
# so a developer who rebuilt the firmware and re-ran `deploy` could stage
# whatever happened to be there at the last reconfigure. This project has
# already shipped one release carrying an image built from a dirty tree; that
# was a different cause with the same shape, and the principle is the same —
# decide against the tree as it is at the moment of staging.
#
# THE TWO SHAPES
#
#   PRIVATE  firmware/ is the real project. The .ctf and the bootloader image
#            are PlatformIO outputs under .pio/, OpenOCD comes from the
#            PlatformIO package cache, and every one of them can be rebuilt.
#
#   PUBLIC   firmware/ is the contract subset the publish pipeline stages:
#            include/, VERSION, the prebuilt device-core library, and the
#            RELEASED BINARIES can-triple-<version>.ctf and bootloader.bin.
#            There is no .pio, no firmware source, and normally no PlatformIO
#            installed at all.
#
# So each artifact is looked for as a fresh build first and as a shipped
# binary second. Before this script the deploy target knew only the first
# location, and a public checkout died on a bare file-not-found against a
# .pio path that repository is never going to have.
#
# The log names which source won. That is not decoration: "which image is in
# this installer" is the question a release post-mortem asks first.

cmake_minimum_required(VERSION 3.21)

foreach(_required FIRMWARE_DIR DEPLOY_DIR FW_VERSION)
    if(NOT DEFINED ${_required})
        message(FATAL_ERROR "stage_firmware.cmake: -D ${_required}=... is required.")
    endif()
endforeach()

# One artifact: prefer the fresh build, fall back to the released binary, and
# if neither is there say what was looked for and what to do about it. The old
# failure named one path and no remedy.
function(stage_one what built prebuilt dest)
    if(EXISTS "${built}")
        set(_src "${built}")
        set(_origin "freshly built")
    elseif(EXISTS "${prebuilt}")
        set(_src "${prebuilt}")
        set(_origin "released binary")
    else()
        message(FATAL_ERROR
            "deploy: cannot stage ${what}.\n"
            "  Looked for a fresh build at: ${built}\n"
            "  and the released binary at:  ${prebuilt}\n"
            "Neither exists. In the private tree the firmware has not been "
            "built yet - run firmware/build.ps1 first (every release path does "
            "that before this target). In a public checkout this file ships "
            "with the repository, so a missing one means the checkout is "
            "incomplete rather than merely unbuilt.")
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_src}" "${dest}"
        RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "deploy: copying ${_src} -> ${dest} failed (${_rc}).")
    endif()
    message(STATUS "deploy: ${what} <- ${_origin}: ${_src}")
endfunction()

file(MAKE_DIRECTORY "${DEPLOY_DIR}/Firmware")

# The firmware this Manager release pairs with, named with its own version so
# the file in the install cannot claim a version the image inside it does not
# carry. A bench machine that never saw the repositories can restore a unit
# from it.
#
# Online -> Update Firmware browses here, and now genuinely does:
# FirmwareUpdateDialog::onBrowse() starts its file chooser at
# ct::firmwareImagesDirectory(), which is this folder. The comment claimed that
# for a while before the code did it - onBrowse() passed an empty start
# directory, so Select Firmware Image opened wherever the last file dialog in
# the process happened to be and the user navigated here by hand. A build no
# installer ever ran has no such folder, and falls back to no default.
stage_one("can-triple-${FW_VERSION}.ctf"
    "${FIRMWARE_DIR}/.pio/build/CAN_Triple/firmware.ctf"
    "${FIRMWARE_DIR}/can-triple-${FW_VERSION}.ctf"
    "${DEPLOY_DIR}/Firmware/can-triple-${FW_VERSION}.ctf")

# A stale image beside the current one is worse than clutter: the
# initial-programming tool takes the HIGHEST header version it finds beside
# itself, and the version-one renumber made every older image "higher" than the
# firmware this release actually pairs with. Every staging pass therefore
# removes what it did not stage, so deploy/Firmware holds exactly one .ctf.
file(GLOB _stale_ctf "${DEPLOY_DIR}/Firmware/can-triple-*.ctf")
list(REMOVE_ITEM _stale_ctf "${DEPLOY_DIR}/Firmware/can-triple-${FW_VERSION}.ctf")
if(_stale_ctf)
    file(REMOVE ${_stale_ctf})
    message(STATUS "deploy: removed stale firmware image(s): ${_stale_ctf}")
endif()

# The bootloader image the initial-programming tool writes to a blank part.
stage_one("bootloader.bin"
    "${FIRMWARE_DIR}/bootloader/.pio/build/bootloader/firmware.bin"
    "${FIRMWARE_DIR}/bootloader.bin"
    "${DEPLOY_DIR}/Firmware/bootloader.bin")

# THE OPENOCD KIT - the one part with no published substitute. It is binaries
# from the PlatformIO package cache plus the exact three scripts the flashing
# invocation touches and this project's 512K-bank target config (stock OpenOCD
# trusts the part's FLASH_SIZE register, which lies - see firmware/openocd/).
# The folder is deliberately self-contained: copy it to a USB stick and it
# still works.
#
# Neither the package cache nor the .cfg travels in the public tree, and they
# cannot be reconstructed from it, so this is skipped rather than fatal there.
# What that costs is stated out loud, because the resulting deploy/ is a
# runnable program but NOT a valid installer payload - the .iss requires
# openocd.exe and stm32g4x_512k.cfg by name, and its own error would send a
# public user back to re-run this target, which would never help.
set(_ocd "${PIO_PACKAGES}/tool-openocd")
set(_cfg "${FIRMWARE_DIR}/openocd/stm32g4x_512k.cfg")
if(EXISTS "${_ocd}/bin" AND EXISTS "${_cfg}")
    file(MAKE_DIRECTORY
        "${DEPLOY_DIR}/Firmware/openocd/scripts/interface"
        "${DEPLOY_DIR}/Firmware/openocd/scripts/target")
    execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${_ocd}/bin" "${DEPLOY_DIR}/Firmware/openocd/bin" RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "deploy: staging the OpenOCD binaries failed (${_rc}).")
    endif()
    foreach(_pair
            "openocd/scripts/interface/stlink.cfg|openocd/scripts/interface"
            "openocd/scripts/target/swj-dp.tcl|openocd/scripts/target"
            "openocd/scripts/mem_helper.tcl|openocd/scripts")
        string(REPLACE "|" ";" _parts "${_pair}")
        list(GET _parts 0 _from)
        list(GET _parts 1 _to)
        execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${_ocd}/${_from}" "${DEPLOY_DIR}/Firmware/${_to}" RESULT_VARIABLE _rc)
        if(NOT _rc EQUAL 0)
            message(FATAL_ERROR "deploy: staging ${_from} failed (${_rc}).")
        endif()
    endforeach()
    execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${_cfg}" "${DEPLOY_DIR}/Firmware/openocd/scripts" RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "deploy: staging stm32g4x_512k.cfg failed (${_rc}).")
    endif()
    message(STATUS "deploy: OpenOCD initial-programming kit staged")
else()
    message(NOTICE
        "deploy: the OpenOCD initial-programming kit was NOT staged.\n"
        "  Wanted the PlatformIO package at: ${_ocd}\n"
        "  and this project's target config:  ${_cfg}\n"
        "deploy/ is a complete, runnable program without them; what it cannot "
        "do is bring a BLANK OR BRICKED board to life, and it is not a valid "
        "installer payload - the `installer` target is a private-tree release "
        "step and will refuse this folder by design.")
endif()
