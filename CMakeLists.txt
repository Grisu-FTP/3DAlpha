cmake_minimum_required(VERSION 3.13)

# ---------------------------------------------------------------------------
# Version selection happens here, before anything is compiled.
# tools/configure.py turns versions/${MCVER}.json into build inputs.
# See docs/build-versions.md.
# ---------------------------------------------------------------------------
set(MCVER "a1.1.2" CACHE STRING "Minecraft version to build")

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(GEN_DIR "${CMAKE_BINARY_DIR}/gen")
execute_process(
	COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/configure.py" "${MCVER}" "${GEN_DIR}"
	RESULT_VARIABLE configure_result
	OUTPUT_VARIABLE configure_output
	ERROR_VARIABLE  configure_output
)
message(STATUS "${configure_output}")
if(NOT configure_result EQUAL 0)
	message(FATAL_ERROR "configure.py failed for version '${MCVER}'")
endif()

# Re-run CMake when a manifest changes, so slot/feature edits can't go stale.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
	"${CMAKE_SOURCE_DIR}/versions/${MCVER}.json"
	"${CMAKE_SOURCE_DIR}/tools/configure.py"
	"${CMAKE_SOURCE_DIR}/packaging/3dalpha.rsf.in"
)

include("${GEN_DIR}/version.cmake")

# The data directory is only known after version.cmake defines it, so the
# generated tables get their own dependency line here rather than above.
#
# **Every file configure.py reads, not just blocks.json.** Only that one was
# listed, so regenerating the placement table left build/gen/placement.hpp
# holding the old numbers and the test suite failing against a fixture that was
# right -- which reads as a broken table rather than a stale build.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
	"${CMAKE_SOURCE_DIR}/${MCVER_DATA_DIR}/blocks.json"
	"${CMAKE_SOURCE_DIR}/${MCVER_DATA_DIR}/drops.json"
	"${CMAKE_SOURCE_DIR}/${MCVER_DATA_DIR}/items.json"
	"${CMAKE_SOURCE_DIR}/${MCVER_DATA_DIR}/paintings.json"
	"${CMAKE_SOURCE_DIR}/${MCVER_DATA_DIR}/placement.json"
	"${CMAKE_SOURCE_DIR}/${MCVER_DATA_DIR}/selection.json"
)

project(3DAlpha VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Sources: core is version- and platform-independent; only the slot
# implementations this version selected are compiled at all.
# ---------------------------------------------------------------------------
file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DEPENDS "src/core/*.cpp")

set(SLOT_SOURCES "")
foreach(dir IN LISTS MCVER_SLOT_DIRS)
	file(GLOB_RECURSE found CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/${dir}/*.cpp")
	list(APPEND SLOT_SOURCES ${found})
endforeach()

if(NINTENDO_3DS)
	file(GLOB_RECURSE PLATFORM_SOURCES CONFIGURE_DEPENDS "src/platform/ctr/*.cpp")
else()
	file(GLOB_RECURSE PLATFORM_SOURCES CONFIGURE_DEPENDS "src/platform/host/*.cpp")
endif()

# zlib decodes chunk files (gzip) and Map Chunk payloads (zlib). On 3DS it comes
# from the portlibs package; the failure message is explicit because 3ds-dev does
# not pull it in.
if(NINTENDO_3DS)
	find_package(ZLIB)
	if(NOT ZLIB_FOUND)
		message(FATAL_ERROR
			"zlib not found for the 3DS target. Install it with:\n"
			"    sudo pacman -S 3ds-zlib")
	endif()
else()
	find_package(ZLIB REQUIRED)
endif()

# Ogg Vorbis, for the sounds and music the player supplies. Two libraries with
# one API: the 3DS links Tremor (fixed point, because an ARM11 has no business
# doing this in floating point), the host links the reference vorbisfile. See
# core/audio/vorbis_stream.hpp for the one place they differ.
#
# **Optional on purpose.** A missing decoder is not a build failure -- it is a
# build whose `Backend::available()` says no and whose game is silent, the same
# degradation as a missing DSP firmware or a missing resources folder. Audio is
# never allowed to stand between someone and a working binary.
set(MC_HAVE_VORBIS 0)
set(MC_VORBIS_TREMOR 0)
if(NINTENDO_3DS)
	# **A failed search must not be sticky.** find_library writes its answer to
	# the cache including NOTFOUND, and a cached NOTFOUND is never retried -- so a
	# tree configured before `3ds-libvorbisidec` was installed goes on producing a
	# silent binary afterwards, with no warning, because the second configure never
	# looks again. That is precisely the trap for an optional dependency somebody
	# installs *because* the first build told them to. Clearing it on the way in
	# costs one filesystem search per configure and makes the answer current.
	if(NOT VORBISIDEC_LIB)
		unset(VORBISIDEC_LIB CACHE)
	endif()
	if(NOT OGG_LIB)
		unset(OGG_LIB CACHE)
	endif()
	find_library(VORBISIDEC_LIB vorbisidec HINTS "$ENV{DEVKITPRO}/portlibs/3ds/lib")
	find_library(OGG_LIB ogg HINTS "$ENV{DEVKITPRO}/portlibs/3ds/lib")
	if(VORBISIDEC_LIB AND OGG_LIB)
		set(MC_HAVE_VORBIS 1)
		set(MC_VORBIS_TREMOR 1)
	else()
		# A warning and not an error: docs/assets.md's rule is that audio is
		# never a startup dependency, and a build that refuses to produce a
		# binary because the console might not have sound would be a harsher
		# version of the same mistake.
		message(WARNING
			"libvorbisidec not found -- this build will have no audio. Install it with:\n"
			"    sudo dkp-pacman -S 3ds-libvorbisidec\n"
			"or, where devkitPro came from the distribution's own packages:\n"
			"    sudo pacman -S 3ds-libvorbisidec")
	endif()
else()
	find_package(PkgConfig QUIET)
	if(PkgConfig_FOUND)
		# Same staleness as the 3DS branch above, same reason: pkg_check_modules
		# short-circuits on this internal marker, so a tree configured before
		# libvorbis was installed keeps reporting it missing. Clearing the marker
		# makes the check run again.
		if(NOT VORBISFILE_FOUND)
			unset(__pkg_config_checked_VORBISFILE CACHE)
		endif()
		pkg_check_modules(VORBISFILE QUIET vorbisfile)
		if(VORBISFILE_FOUND)
			set(MC_HAVE_VORBIS 1)
		endif()
	endif()
	if(NOT MC_HAVE_VORBIS)
		message(WARNING "libvorbisfile not found -- host audio and its tests will be skipped.")
	endif()
endif()

# Core is a library so the host test binary can link it without dragging in a
# platform layer or a main().
add_library(3dalpha_core STATIC ${CORE_SOURCES} ${SLOT_SOURCES})

target_include_directories(3dalpha_core PUBLIC "${GEN_DIR}" "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(3dalpha_core PUBLIC ZLIB::ZLIB)

# MC_APP_VERSION is the client's own version, which `project()` above is the
# one place that records. It is PUBLIC because the Info screen reads it through
# core and the tests check it through core, and it is a string rather than three
# numbers because nothing does arithmetic on it.
target_compile_definitions(3dalpha_core PUBLIC
	MC_APP_VERSION="${PROJECT_VERSION}"
	MC_HAVE_VORBIS=${MC_HAVE_VORBIS}
	MC_VORBIS_TREMOR=${MC_VORBIS_TREMOR})
if(MC_HAVE_VORBIS)
	if(NINTENDO_3DS)
		# Tremor's headers install under tremor/, and it needs libogg's framing.
		target_include_directories(3dalpha_core PUBLIC "$ENV{DEVKITPRO}/portlibs/3ds/include")
		target_link_libraries(3dalpha_core PUBLIC ${VORBISIDEC_LIB} ${OGG_LIB})
	else()
		target_include_directories(3dalpha_core PUBLIC ${VORBISFILE_INCLUDE_DIRS})
		target_link_libraries(3dalpha_core PUBLIC ${VORBISFILE_LINK_LIBRARIES})
	endif()
endif()

# A 3DSX gets a 32 KB main-thread stack and nothing in the binary can enlarge
# it, so a frame the host shrugs off is a crash before the function's first
# line runs. This is a console constraint, not a code-style one: the host
# harness and the tests put whole worlds on their 8 MB stack on purpose, which
# is precisely why the overflow was invisible until hardware. 8 KB is well
# above the largest real frame here (~2 KB) and well under the console's limit.
if(NINTENDO_3DS)
	set(STACK_FLAGS -Werror=stack-usage=8192)
	# -Wno-psabi silences GCC's "parameter passing for argument of type ...
	# changed in GCC 7.1" notes, which devkitARM emits from inside <algorithm>
	# whenever a container of non-trivial types is sorted. It is a warning about
	# linking objects built by a pre-7.1 compiler against ones built by this
	# one, which cannot happen here -- everything in the binary comes out of the
	# same toolchain -- and it arrives twenty lines at a time. Nothing else is
	# silenced: the flag is this one ABI note and no other diagnostic.
	set(ABI_FLAGS -Wno-psabi)
else()
	set(STACK_FLAGS "")
	set(ABI_FLAGS "")
endif()

# Seed-exact worldgen means our double arithmetic has to agree with a 2010 JVM's
# bit for bit, and -ffp-contract=fast -- GCC's default for C++ -- quietly breaks
# that. It lets the compiler fuse a*b+c into a single FMA with one rounding
# where the source says two. Java never does this: the JLS specifies every
# double operation as separately rounded, and Math.fma is the only way to ask
# for anything else. So a host with FMA3 and an ARM11 without would disagree
# with each other *and* with Java, in the low bits of a noise lattice, which
# surfaces as terrain that is subtly wrong a thousand blocks from spawn.
#
# Off everywhere rather than only in the generator: the flag is per translation
# unit, the arithmetic crosses headers, and nothing here is hot enough to want
# the fusion. -ffast-math and -Ofast are absent for the same reason -- keep them
# that way.
set(FP_FLAGS -ffp-contract=off)

set(COMMON_FLAGS
	-Wall -Wextra
	${STACK_FLAGS}
	${ABI_FLAGS}
	${FP_FLAGS}
	-fno-exceptions -fno-rtti -fno-threadsafe-statics
	-ffunction-sections -fdata-sections
	$<$<CONFIG:Release>:-O3 -fomit-frame-pointer>
)
target_compile_options(3dalpha_core PUBLIC ${COMMON_FLAGS})

add_executable(3dalpha ${PLATFORM_SOURCES})
target_link_libraries(3dalpha PRIVATE 3dalpha_core)
target_link_options(3dalpha PRIVATE -Wl,--gc-sections)

# ---------------------------------------------------------------------------
# Host-only: unit tests. Core is platform-independent by construction, so its
# tests never need a console -- see docs/architecture.md.
# ---------------------------------------------------------------------------
if(NOT NINTENDO_3DS)
	enable_testing()

	file(GLOB TEST_SOURCES CONFIGURE_DEPENDS "tests/*.cpp")
	add_executable(3dalpha_tests ${TEST_SOURCES})
	target_link_libraries(3dalpha_tests PRIVATE 3dalpha_core)
	target_include_directories(3dalpha_tests PRIVATE "${CMAKE_SOURCE_DIR}/tests")

	# Sanitizers are the point of the host target: a bounds bug in the NBT
	# parser is a crash on hardware and a diagnosed line here.
	option(SANITIZE "Build host tests with ASan/UBSan" ON)
	if(SANITIZE)
		# **PUBLIC on the core library, not PRIVATE on the tests.** For a long
		# time these flags were attached to the test executable alone, which
		# instruments the `tests/*.cpp` files and nothing else -- so the NBT
		# parser named in the comment above, the mesher, and the world
		# generator were all built unsanitised and the suite was checking the
		# test code rather than the code under test. Anything reached through
		# 3dalpha_core has to carry the flags itself, and PUBLIC is what pushes
		# them onto both dependents.
		#
		# float-cast-overflow is named separately because **GCC leaves it out
		# of the `undefined` group**. An out-of-range double-to-int conversion
		# is undefined behaviour that a plainly-sanitised build reports nothing
		# about, and this project has one on its hottest path by design -- the
		# Far Lands are that conversion overflowing, and it read INT_MIN on
		# x86 where Java reads INT_MAX. See core/util/java_cast.hpp.
		set(SANITIZERS -fsanitize=address,undefined,float-cast-overflow)
		target_compile_options(3dalpha_core PUBLIC ${SANITIZERS} -g)
		target_link_options(3dalpha_core PUBLIC ${SANITIZERS})
	endif()

	add_test(NAME unit COMMAND 3dalpha_tests)
endif()

# ---------------------------------------------------------------------------
# 3DS packaging
# ---------------------------------------------------------------------------
if(NINTENDO_3DS)
	# citro2d draws the main menu -- text through the 3DS system font, and
	# quads for everything else. It ships with devkitPro and was simply never
	# linked before there was a 2D screen to draw. It sits on top of citro3d, so
	# the order matters to the linker.
	target_link_libraries(3dalpha PRIVATE citro2d citro3d ctru)

	# picasso assembles the shaders; bin2s embeds the .shbin in the binary.
	# NOTE: ctr_add_shader_library only accepts sources positionally -- passing
	# them via SOURCES hits a bug in its argument handling and fails.
	ctr_add_shader_library(world_shader shaders/world.v.pica)
	ctr_add_shader_library(detail_shader shaders/detail.v.pica)
	ctr_add_shader_library(outline_shader shaders/outline.v.pica)
	# Two sources, one shbin: picasso emits a DVLE per file in the order given,
	# so DVLE[0] is the vertex half and DVLE[1] the geometry half. The order is
	# load-bearing -- buildQuadPipeline indexes them.
	ctr_add_shader_library(quad_shader shaders/quad.v.pica shaders/quad.g.pica)
	dkp_add_embedded_binary_library(shaders world_shader detail_shader outline_shader quad_shader)
	target_link_libraries(3dalpha PRIVATE shaders)

	set_target_properties(3dalpha PROPERTIES
		OUTPUT_NAME "3DAlpha-${MCVER}"
		SUFFIX ".elf"
	)

	ctr_generate_smdh(
		OUTPUT      "${CMAKE_BINARY_DIR}/3DAlpha-${MCVER}.smdh"
		NAME        "${MCVER_APP_TITLE}"
		DESCRIPTION "${MCVER_APP_DESCRIPTION}"
		AUTHOR      "3DAlpha"
	)

	ctr_create_3dsx(3dalpha
		SMDH   "${CMAKE_BINARY_DIR}/3DAlpha-${MCVER}.smdh"
		OUTPUT "${CMAKE_BINARY_DIR}/3DAlpha-${MCVER}.3dsx"
	)

	# CIA needs makerom, which devkitPro does not ship. Absence is not an error:
	# a fresh clone must still produce something runnable.
	find_program(MAKEROM_EXE NAMES makerom)
	if(MAKEROM_EXE)
		message(STATUS "makerom found - 'cia' target available (UniqueId ${MCVER_UNIQUE_ID})")
		add_custom_target(cia
			COMMAND "${MAKEROM_EXE}" -f cia
				-o "${CMAKE_BINARY_DIR}/3DAlpha-${MCVER}.cia"
				-rsf "${GEN_DIR}/${MCVER}.rsf"
				-target t -exefslogo
				-elf "$<TARGET_FILE:3dalpha>"
				-icon "${CMAKE_BINARY_DIR}/3DAlpha-${MCVER}.smdh"
			DEPENDS 3dalpha
			COMMENT "Building CIA for ${MCVER_DISPLAY}"
			VERBATIM
		)
	else()
		message(STATUS "makerom not found - 3DSX only. See docs/toolchain-setup.md")
	endif()
endif()
