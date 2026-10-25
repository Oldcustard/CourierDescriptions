cmake_minimum_required(VERSION 3.21)
# ⚠️ Bump this on every release. It becomes the SKSEPluginInfo version and is
# the FIRST line of CDNBookText.log - which is how a bug report's log is matched
# to a build. 1.0.1 shipped for a day announcing itself as v1-0-0-0.
project(CDNBookText VERSION 1.3.0 LANGUAGES CXX)

# CommonLibSSE-NG (alandtse/CommonLibVR branch ng) as a source submodule.
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(lib/commonlibsse-ng commonlib)

# A plain source add_subdirectory does NOT define add_commonlibsse_plugin.
if(NOT COMMAND add_commonlibsse_plugin)
    include(lib/commonlibsse-ng/cmake/CommonLibSSE.cmake)
endif()

# The helper generates the SKSEPluginInfo (Version + Query); we supply only Load.
add_commonlibsse_plugin(${PROJECT_NAME}
    AUTHOR "Courier Descriptions"
    SOURCES src/main.cpp src/Papyrus.cpp)

# MinHook does the function-ENTRY detouring CommonLibSSE has no facility for.
# SKSE::Trampoline::write_branch only patches an existing call site: it reads the
# rel32 already at that address and returns the old target. Aimed at a function
# entry it invents a garbage "original" from the prologue bytes - which is how the
# first attempt at this hook crashed the game. MinHook decodes the prologue and
# builds a real trampoline, so it is the correct tool here.
find_package(minhook CONFIG REQUIRED)

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)
target_compile_options(${PROJECT_NAME} PRIVATE /Zc:preprocessor)   # required by CommonLib headers
target_precompile_headers(${PROJECT_NAME} PRIVATE src/pch.h)       # required (PCH-dependent headers)

target_link_libraries(${PROJECT_NAME} PRIVATE minhook::minhook)

# Drop the built DLL (+ pdb) into a Data-shaped mod folder on every build.
set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/CDNBookText/SKSE/Plugins")
