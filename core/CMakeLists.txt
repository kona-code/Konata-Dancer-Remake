cmake_minimum_required(VERSION 3.16)

project(konacode LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# vulkan KTX-software
# ---------------------------------------------------------------------------
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

set(KTX_FEATURE_TOOLS OFF CACHE BOOL "" FORCE)
set(KTX_FEATURE_TESTS OFF CACHE BOOL "" FORCE)
set(KTX_FEATURE_KTX1 ON CACHE BOOL "" FORCE)
set(KTX_FEATURE_KTX2 OFF CACHE BOOL "" FORCE)
set(KTX_FEATURE_VK_UPLOAD ON CACHE BOOL "" FORCE)
set(KTX_FEATURE_ASTC OFF CACHE BOOL "" FORCE)
set(KTX_FEATURE_BASISU OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/src/third_party/libktx/lib ktx)


# ---------------------------------------------------------------------------
# sources
# ---------------------------------------------------------------------------
file(GLOB_RECURSE APP_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
)

# do not include KTX sources
list(FILTER APP_SOURCES EXCLUDE REGEX ".*/src/third_party/.*")

set(SHADER_ROOT "${CMAKE_CURRENT_LIST_DIR}/shaders/compiled")

add_executable(konacode ${APP_SOURCES})

target_compile_features(konacode PUBLIC cxx_std_20)

# ---------------------------------------------------------------------------
# dependencies
# ---------------------------------------------------------------------------
find_package(Vulkan REQUIRED)
find_package(glfw3 CONFIG REQUIRED)
find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# includes
# ---------------------------------------------------------------------------
target_include_directories(konacode
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# ---------------------------------------------------------------------------
# link libraries
# ---------------------------------------------------------------------------
target_link_libraries(konacode
  PRIVATE
    Vulkan::Vulkan
    glfw
    CURL::libcurl
    Threads::Threads
    ktx
)

target_compile_definitions(konacode PRIVATE KHRONOS_STATIC) # static link

if(UNIX AND NOT APPLE)
  set_target_properties(konacode PROPERTIES
    BUILD_RPATH "$ORIGIN"
    INSTALL_RPATH "$ORIGIN"
  )
endif()

# ---------------------------------------------------------------------------
# copy shaders
# ---------------------------------------------------------------------------
add_custom_command(TARGET konacode POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${SHADER_ROOT}" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders"

  VERBATIM
)