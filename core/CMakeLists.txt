cmake_minimum_required(VERSION 3.16)

project(konacode_app LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# sources
# ---------------------------------------------------------------------------
file(GLOB_RECURSE APP_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
)

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
    ${Boost_INCLUDE_DIRS}
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
)

if(UNIX AND NOT APPLE)
  set_target_properties(konacode PROPERTIES
    BUILD_RPATH "$ORIGIN"
    INSTALL_RPATH "$ORIGIN"
  )
endif()