cmake_minimum_required(VERSION 3.14)
project(strata LANGUAGES CXX)

include(FetchContent)

# Building blocks (Kronuz libraries). Point FETCHCONTENT_SOURCE_DIR_<NAME> at local
# checkouts to develop them together.
set(TASK_ASIO OFF CACHE BOOL "" FORCE)
foreach(lib docset bitpack fst task hashes residency)
  FetchContent_Declare(${lib} GIT_REPOSITORY https://github.com/Kronuz/${lib}.git GIT_TAG main)
endforeach()
FetchContent_MakeAvailable(docset bitpack fst task hashes residency)

add_library(strata INTERFACE)
target_include_directories(strata INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(strata INTERFACE cxx_std_20)
target_link_libraries(strata INTERFACE docset::docset bitpack::bitpack fst::fst task::task hashes::hashes residency::residency)
add_library(strata::strata ALIAS strata)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  enable_testing()
  add_executable(concepts_test test/concepts_test.cc)
  target_link_libraries(concepts_test PRIVATE strata)
  add_test(NAME concepts COMMAND concepts_test)
endif()
