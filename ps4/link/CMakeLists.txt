# PCSX2 PS4 port (AI-assisted): links Makefile.ps4's objects into eboot.bin with CEMUPS4's PS4 runtime,
# CRT, link script, RADV (Mesa on GNM) and glslang. See ps4/build-ps4.sh. Needs proper testing on the console.
cmake_minimum_required(VERSION 3.13)
project(pcsx2_ps4 C CXX ASM)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif ()

set(PS4_PORT_DIR "$ENV{HOME}/cemups4" CACHE PATH "CEMUPS4 checkout (toolchain/ and port/)")
include(${PS4_PORT_DIR}/toolchain/ps4-love-style.cmake)

set(PCSX2_OBJECTS_FILE "" CACHE FILEPATH "objects.txt written by Makefile.ps4")
if (NOT EXISTS "${PCSX2_OBJECTS_FILE}")
    message(FATAL_ERROR "PCSX2_OBJECTS_FILE: run make -f Makefile.ps4 objects.txt first")
endif ()
file(STRINGS ${PCSX2_OBJECTS_FILE} pcsx2_objects)
set_source_files_properties(${pcsx2_objects} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)

add_executable(pcsx2 ${PS4_RUNTIME_SOURCES} ${pcsx2_objects})
set_target_properties(pcsx2 PROPERTIES LINKER_LANGUAGE CXX)

# The Vulkan window system on VideoOut (ps4/ps4_vk_wsi.cpp): PCSX2's and the frontend's calls to RADV's entry point go
# through it.
string(APPEND CMAKE_EXE_LINKER_FLAGS " --wrap=vk_icdGetInstanceProcAddr")
string(APPEND CMAKE_EXE_LINKER_FLAGS " --script ${CMAKE_CURRENT_SOURCE_DIR}/orbis-text.ld")
# PS5SX2's weak references to the PS5 driver (ps5vk), checked for null before use: null here, as in PS5SX2's own RADV
# eboot (create-fself refuses a weak name nothing defines).
foreach (sym orbis_ps5vk_war_shim ps5vk_debug_threads ps5vk_debug_timestamp_flush ps5vk_output_refresh_millihertz
         ps5vk_output_restore_default ps5vk_output_status
         ps4_jit_owner ps4_jit_dump_function) # CEMUPS4 port/ps4_sampler.cpp: Cemu's JIT lookups, weak
    string(APPEND CMAKE_EXE_LINKER_FLAGS " --defsym=${sym}=0")
endforeach ()
# CEMUPS4's resolver (port/ps4_net.cpp) falls back to the C library's lookups through __real_; ld.lld doesn't take a
# library member for a name reached only that way, so ask for them (create-fself: "missing module for gethostbyname").
string(APPEND CMAKE_EXE_LINKER_FLAGS " -u gethostbyname -u getaddrinfo")
# Every undefined name at once (the PS5-only calls), not the first 20.
string(APPEND CMAKE_EXE_LINKER_FLAGS " --error-limit=0")

ps4_link_mesa(pcsx2)  # RADV on GNM
# paraLLEl-GS and the Granite libraries under it (ps4/build-pgs.sh; Makefile.ps4 PGS=1), when they were built.
set(PGS_LIBS_FILE "$ENV{HOME}/pcsx2ps4-build/pgs/libs.txt" CACHE FILEPATH "libs.txt written by ps4/build-pgs.sh")
if (EXISTS "${PGS_LIBS_FILE}")
    file(STRINGS ${PGS_LIBS_FILE} pgs_libs)
    message(STATUS "paraLLEl-GS libraries: ${pgs_libs}")
    target_link_libraries(pcsx2 PRIVATE ${pgs_libs}) # ld.lld directly, inside the link's --start-group
endif ()
target_link_libraries(pcsx2 PRIVATE
                      ${PS4_SYSROOT}/lib/libglslang.a
                      ${PS4_SYSROOT}/lib/libglslang-default-resource-limits.a
                      z SceSysmodule SceSystemService SceUserService SceVideoOut SceAudioOut ScePad SceNet SceNetCtl
                      SceRandom SceSsl SceHttp SceImeDialog SceCommonDialog SceMouse SceKeyboard pthread)
ps4_add_eboot(pcsx2)
