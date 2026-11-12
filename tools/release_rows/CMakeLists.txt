# stem_parity (release-rows A4): snowball at the pin Xapiand and prospect share.
cmake_minimum_required(VERSION 3.18)
project(release_rows LANGUAGES C CXX)
include(FetchContent)
FetchContent_Declare(snowball GIT_REPOSITORY https://github.com/Kronuz/snowball.git GIT_TAG 3efcd0faa97997acf361cf8edff97cb083e14bc8)
FetchContent_MakeAvailable(snowball)
add_executable(stem_parity stem_parity.cc)
target_link_libraries(stem_parity PRIVATE snowball::snowball)
