cmake_minimum_required(VERSION 3.21)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The parts of the mod that don't need Geode or Windows, tested on their own
project(mod-profiler-tests CXX)

enable_testing()

find_package(Threads REQUIRED)

add_executable(ring-tests RingTests.cpp)
target_link_libraries(ring-tests Threads::Threads)
add_test(NAME ring COMMAND ring-tests)

add_executable(modulemap-tests ModuleMapTests.cpp ../src/ModuleMap.cpp)
add_test(NAME modulemap COMMAND modulemap-tests)

add_executable(code-tests CodeTests.cpp ../src/Code.cpp)
add_test(NAME code COMMAND code-tests)

add_executable(profile-tests ProfileTests.cpp ../src/Profile.cpp ../src/ModuleMap.cpp)
add_test(NAME profile COMMAND profile-tests)

add_executable(unwind-tests UnwindTests.cpp ../src/Unwind.cpp)
add_test(NAME unwind COMMAND unwind-tests)
