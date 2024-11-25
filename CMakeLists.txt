cmake_minimum_required (VERSION 3.8)
set(PROJ_NAME "engine")
project (PROJ_NAME)
# set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/bin")
# set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/bin")
# add_compile_options("-O3")
set(CMAKE_CXX_STANDARD 20)
set(LIBRARY_PATH ${CMAKE_CURRENT_SOURCE_DIR}/libs)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_BUILD_TYPE RELEASE)
#add_compile_options("/DEBUG")

#sources
file(GLOB_RECURSE SRCS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
file(GLOB_RECURSE HDRS "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp")

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/include/path_config.hpp.in ${CMAKE_CURRENT_SOURCE_DIR}/include/path_config.hpp)
add_executable(${PROJ_NAME} WIN32 ${HDRS} ${SRCS})
# set_target_properties(${PROJ_NAME} PROPERTIES DEBUG_POSTFIX _d)

# add_definitions(${COMPILER_FLAGS})
target_include_directories(${PROJ_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(${PROJ_NAME} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/libs/vulkan")
target_include_directories(${PROJ_NAME} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/libs/glfw")
target_include_directories(${PROJ_NAME} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/libs/fastgltf/include")
# target_include_directories(${PROJ_NAME} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/libs/fmt/include")

# libraries
find_package(Vulkan REQUIRED)
add_subdirectory(libs)
add_subdirectory(shared)

target_include_directories(${PROJ_NAME} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(${PROJ_NAME} vkguide_shared)
target_link_libraries(${PROJ_NAME} Vulkan::Vulkan sdl2)
target_link_libraries(${PROJ_NAME} vkbootstrap vma glm imgui stb_image fastgltf::fastgltf)
# target_link_libraries(${PROJ_NAME} Vulkan::Vulkan)
if(NOT WIN32)
    find_library(GLFW_LIBRARY glfw)
elseif(WIN32)
    find_library(GLFW_LIBRARY glfw3 HINTS ${LIBRARY_PATH}/glfw)
endif(NOT WIN32)

target_link_libraries(${PROJ_NAME} ${GLFW_LIBRARY})

# shaders
find_program(GLSL_VALIDATOR glslangValidator HINTS /usr/bin /usr/local/bin $ENV{VULKAN_SDK}/Bin/ $ENV{VULKAN_SDK}/Bin32/)
file(GLOB_RECURSE GLSL_SOURCE_FILES
    "${PROJECT_SOURCE_DIR}/shaders/raster/*.frag"
    "${PROJECT_SOURCE_DIR}/shaders/raster/*.vert"
    "${PROJECT_SOURCE_DIR}/shaders/raster/*.geom"
    "${PROJECT_SOURCE_DIR}/shaders/comp/*.comp"
)

message(STATUS "BUILDING SHADERS")
file(MAKE_DIRECTORY "${PROJECT_SOURCE_DIR}/shaders/bin")
foreach(GLSL ${GLSL_SOURCE_FILES})
  get_filename_component(FILE_NAME ${GLSL} NAME)
  get_filename_component(FILE_EXT ${GLSL} EXT)
  set(SPIRV "${PROJECT_SOURCE_DIR}/shaders/bin/${FILE_NAME}.spv")

  # Add the -G flag for .geom files
  if(FILE_EXT STREQUAL ".geom")
    set(GLSL_FLAGS "-V")
  else()
    set(GLSL_FLAGS "-V")
  endif()

  message(STATUS ${GLSL})
  message(STATUS ${GLSL_VALIDATOR} " " ${GLSL_FLAGS} " " ${GLSL} " " -o " " ${SPIRV} " " -g "\n")
  add_custom_command(
    OUTPUT ${SPIRV}
    COMMAND ${GLSL_VALIDATOR} ${GLSL_FLAGS} ${GLSL} -o ${SPIRV}
    DEPENDS ${GLSL})
  list(APPEND SPIRV_BINARY_FILES ${SPIRV})
endforeach(GLSL)

add_custom_target(
    shaders 
    DEPENDS ${SPIRV_BINARY_FILES}
)
