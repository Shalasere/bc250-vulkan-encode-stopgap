# bc250-vcn-driver v0.2.0 - https://github.com/Kai/bc250-vcn-driver
cmake_minimum_required(VERSION 3.16)

# All four test binaries below validate exclusively via assert() - there is
# no other pass/fail signal (a failed assert() aborts with a nonzero exit
# code; ctest and manual invocation both rely on that). The top-level
# CMakeLists.txt's documented build (`-DCMAKE_BUILD_TYPE=Release`) makes
# CMake append `-DNDEBUG` to CMAKE_C_FLAGS, which compiles every assert()
# in these files into a no-op per the C standard - so a Release build of
# this test suite "passes" unconditionally, even with the exact bugs these
# tests exist to catch reintroduced (confirmed directly: temporarily
# reverting the run_before ordering fix in src/cavlc.c and rebuilding
# test_cavlc via this project's own documented Release command still
# printed "ALL CAVLC CONFORMANCE UNIT TESTS PASSED!"). -UNDEBUG below
# unconditionally re-enables assert() for these test targets only (the
# main bc250_drv_video library keeps NDEBUG/-O3 as configured) regardless
# of CMAKE_BUILD_TYPE.
add_compile_options(-UNDEBUG)

add_executable(test_bitstream test_bitstream.c ../src/bitstream.c)
target_include_directories(test_bitstream PRIVATE ../src)

add_executable(test_va_api test_va_api.c)
target_include_directories(test_va_api PRIVATE ../src ${Vulkan_INCLUDE_DIRS} ${LIBVA_INCLUDE_DIRS} ${DRM_INCLUDE_DIRS})
find_package(Threads REQUIRED)
target_link_libraries(test_va_api PRIVATE bc250_drv_video ${Vulkan_LIBRARIES} ${LIBVA_LIBRARIES} ${DRM_LIBRARIES} ${CMAKE_THREAD_LIBS_INIT} m)

add_executable(test_cavlc test_cavlc.c ../src/cavlc.c ../src/bitstream.c)
target_include_directories(test_cavlc PRIVATE ../src)

add_executable(test_encode test_encode.c)
target_include_directories(test_encode PRIVATE ../src ${Vulkan_INCLUDE_DIRS} ${LIBVA_INCLUDE_DIRS} ${DRM_INCLUDE_DIRS})
target_link_libraries(test_encode PRIVATE bc250_drv_video ${Vulkan_LIBRARIES} ${LIBVA_LIBRARIES} ${DRM_LIBRARIES} ${CMAKE_THREAD_LIBS_INIT} m)

enable_testing()
add_test(NAME BitstreamTest COMMAND test_bitstream)
add_test(NAME CavlcUnitTest COMMAND test_cavlc)
add_test(NAME VaApiDriverTest COMMAND test_va_api)
add_test(NAME EncodeBitstreamTest COMMAND test_encode WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
