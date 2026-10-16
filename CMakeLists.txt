cmake_minimum_required(VERSION 3.21)

# ---- vcpkg (manifest mode: vcpkg.json next to this file) --------------------------------------------------
# VCPKG_ROOT from the environment, else the local checkout. The packages are installed into
# build/vcpkg_installed (VCPKG_INSTALLED_DIR in CMakePresets.json) on the first configure.
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
	if(DEFINED ENV{VCPKG_ROOT})
		set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
	elseif(EXISTS "D:/vcpkg/scripts/buildsystems/vcpkg.cmake")
		set(CMAKE_TOOLCHAIN_FILE "D:/vcpkg/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
	endif()
endif()
set(VCPKG_TARGET_TRIPLET "x64-windows-static-md" CACHE STRING "")
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL" CACHE STRING "")

project(RuntimeCombiner VERSION 0.5.0 LANGUAGES CXX)

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
	message(FATAL_ERROR "in-source builds are not allowed")
endif()

# MO2 mod folder the plugin (DLL, PDB, meta.ini; the INI only when the folder has none) is copied into after
# every build; empty disables it.
set(RC_INSTALL_DIR "D:/MO2/Fallout 4/mods/Runtime Combiner" CACHE PATH "MO2 mod folder to install the plugin into")

# ---- dependencies ----------------------------------------------------------------------------------------
set(Boost_USE_STATIC_LIBS ON)
add_subdirectory(external/CommonLibF4RD/CommonLibF4 CommonLibF4)
find_package(fmt CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)

# ---- version ---------------------------------------------------------------------------------------------
configure_file(cmake/Version.h.in "${CMAKE_CURRENT_BINARY_DIR}/include/Version.h" @ONLY)
configure_file(cmake/version.rc.in "${CMAKE_CURRENT_BINARY_DIR}/version.rc" @ONLY)

# ---- the plugin ------------------------------------------------------------------------------------------
file(GLOB_RECURSE RC_SOURCES CONFIGURE_DEPENDS src/*.cpp src/*.h)
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${RC_SOURCES})

add_library(RuntimeCombiner SHARED ${RC_SOURCES} "${CMAKE_CURRENT_BINARY_DIR}/version.rc")

target_compile_features(RuntimeCombiner PRIVATE cxx_std_23)
target_compile_definitions(RuntimeCombiner PRIVATE _UNICODE UNICODE)
target_include_directories(RuntimeCombiner PRIVATE src "${CMAKE_CURRENT_BINARY_DIR}/include")
target_link_libraries(RuntimeCombiner PRIVATE CommonLibF4::CommonLibF4 fmt::fmt spdlog::spdlog user32)
target_precompile_headers(RuntimeCombiner PRIVATE src/PCH.h)

if(MSVC)
	target_compile_options(
		RuntimeCombiner
		PRIVATE
			/utf-8
			/Zi
			/permissive-
			/Zc:preprocessor
			/EHsc
			/W4
			/arch:AVX2
			# No /fp:fast: the bake keeps NaN/infinity checks on engine data meaningful.
			"$<$<CONFIG:Release>:/O2;/GL;/Zc:inline;/JMC-;/Gw>"
	)
	target_link_options(
		RuntimeCombiner
		PRIVATE
			"$<$<CONFIG:Release>:/INCREMENTAL:NO;/LTCG;/OPT:REF;/OPT:ICF;/DEBUG:FULL>"
			"$<$<CONFIG:Debug>:/DEBUG:FULL>"
	)
endif()

# ---- install into the MO2 mod folder ----------------------------------------------------------------------
if(RC_INSTALL_DIR)
	add_custom_command(
		TARGET RuntimeCombiner
		POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E make_directory "${RC_INSTALL_DIR}/F4SE/Plugins"
		COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:RuntimeCombiner>" "${RC_INSTALL_DIR}/F4SE/Plugins/"
		COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_PDB_FILE:RuntimeCombiner>" "${RC_INSTALL_DIR}/F4SE/Plugins/"
		# The INI in the mod folder is the user's: it is copied only when there is none yet.
		COMMAND ${CMAKE_COMMAND} "-DSRC=${CMAKE_CURRENT_SOURCE_DIR}/res/RuntimeCombiner.ini" "-DDST=${RC_INSTALL_DIR}/F4SE/Plugins/RuntimeCombiner.ini" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CopyIfMissing.cmake"
		COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/res/meta.ini" "${RC_INSTALL_DIR}/"
		COMMENT "Installing RuntimeCombiner into ${RC_INSTALL_DIR}"
	)
endif()
