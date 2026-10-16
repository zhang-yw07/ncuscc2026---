cmake_minimum_required(VERSION 3.16)
project(hpc_linux_metrics VERSION 0.1.0 LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "stage1 workload and collector only support Linux (WSL 2 is supported)")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(linux_procfs STATIC src/linux_procfs.cpp)
target_include_directories(linux_procfs PUBLIC include)
target_compile_options(linux_procfs PRIVATE -Wall -Wextra -Wpedantic)

add_executable(workload src/workload.cpp)
target_include_directories(workload PRIVATE include)
target_compile_options(workload PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(workload PRIVATE pthread)

add_executable(collector src/collector.cpp)
target_include_directories(collector PRIVATE include)
target_compile_definitions(collector PRIVATE COLLECTOR_VERSION="${PROJECT_VERSION}")
target_compile_options(collector PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(collector PRIVATE linux_procfs)

include(CTest)
if(BUILD_TESTING)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  add_test(
    NAME stage1_integration
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration_test.py
            --workload $<TARGET_FILE:workload>
            --collector $<TARGET_FILE:collector>
  )
  add_test(
    NAME stage1_lifecycle
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/lifecycle_test.py
            --workload $<TARGET_FILE:workload>
            --collector $<TARGET_FILE:collector>
  )
endif()
