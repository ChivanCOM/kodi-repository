cmake_minimum_required(VERSION 3.5)
project(visualization.ibroadcast.albumart)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Kodi dev-kit headers ──────────────────────────────────────────────────────
# Set with: cmake -DKODI_INCLUDE_DIR=/path/to/xbmc/addons/kodi-dev-kit/include
if(NOT DEFINED KODI_INCLUDE_DIR)
  set(KODI_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/kodi-dev-kit/include"
      CACHE PATH "Path to Kodi addon dev-kit include directory")
endif()
include_directories(${KODI_INCLUDE_DIR})

# ── Sources ───────────────────────────────────────────────────────────────────
include_directories(${CMAKE_SOURCE_DIR}/src)

set(SOURCES src/main.cpp)

# ── OpenGL ────────────────────────────────────────────────────────────────────
find_package(OpenGL REQUIRED)
include_directories(${OPENGL_INCLUDE_DIR})
add_definitions(-DHAS_GL)

if(APPLE)
  add_definitions(-DGL_SILENCE_DEPRECATION)
endif()

# ── Build shared library ──────────────────────────────────────────────────────
add_library(${PROJECT_NAME} MODULE ${SOURCES})

target_link_libraries(${PROJECT_NAME} ${OPENGL_LIBRARIES})

set_target_properties(${PROJECT_NAME} PROPERTIES
  PREFIX ""
  OUTPUT_NAME "${PROJECT_NAME}"
)

if(APPLE)
  set_target_properties(${PROJECT_NAME} PROPERTIES SUFFIX ".dylib")
elseif(UNIX)
  set_target_properties(${PROJECT_NAME} PROPERTIES SUFFIX ".so")
endif()

# ── Install into addon folder ─────────────────────────────────────────────────
install(TARGETS ${PROJECT_NAME}
  LIBRARY DESTINATION "${CMAKE_SOURCE_DIR}/.."
)
install(FILES "${CMAKE_SOURCE_DIR}/../addon.xml"
              "${CMAKE_SOURCE_DIR}/../icon.png"
  DESTINATION "${CMAKE_SOURCE_DIR}/.."
)
