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

add_executable(modulemap-tests ModuleMapTests.cpp ../src/ModuleMap.cpp ../src/Unwind.cpp)
add_test(NAME modulemap COMMAND modulemap-tests)

add_executable(code-tests CodeTests.cpp ../src/Code.cpp)
add_test(NAME code COMMAND code-tests)

add_executable(profile-tests ProfileTests.cpp ../src/Profile.cpp ../src/ModuleMap.cpp ../src/Attribution.cpp ../src/Unwind.cpp)
add_test(NAME profile COMMAND profile-tests)

add_executable(unwind-tests UnwindTests.cpp ../src/Unwind.cpp)
add_test(NAME unwind COMMAND unwind-tests)

add_executable(attribution-tests AttributionTests.cpp ../src/Attribution.cpp ../src/ModuleMap.cpp ../src/Unwind.cpp)
add_test(NAME attribution COMMAND attribution-tests)

add_executable(spikes-tests SpikesTests.cpp ../src/Spikes.cpp ../src/Attribution.cpp ../src/ModuleMap.cpp ../src/Unwind.cpp)
add_test(NAME spikes COMMAND spikes-tests)

add_executable(speedscope-tests SpeedscopeTests.cpp ../src/Speedscope.cpp)
add_test(NAME speedscope COMMAND speedscope-tests)

add_executable(compare-tests CompareTests.cpp ../src/Compare.cpp ../src/Profile.cpp ../src/ModuleMap.cpp ../src/Attribution.cpp ../src/Unwind.cpp)
add_test(NAME compare COMMAND compare-tests)

# The sampler itself on Windows: a thread in a known chain of functions, suspended and walked
if (WIN32)
    add_executable(sampler-tests SamplerTests.cpp ../src/Sampler.cpp ../src/Modules.cpp ../src/ModuleMap.cpp ../src/Zones.cpp
        ../src/Unwind.cpp ../src/Code.cpp ../src/Profile.cpp ../src/Attribution.cpp)
    target_link_libraries(sampler-tests psapi)
    add_test(NAME sampler COMMAND sampler-tests)
endif()

add_executable(zones-tests ZonesTests.cpp ../src/Zones.cpp)
add_test(NAME zones COMMAND zones-tests)
