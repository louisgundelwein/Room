add_library(room_xschem SHARED room_xschem.cpp)
target_link_libraries(room_xschem PRIVATE room_utils)
target_include_directories(room_xschem PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(room_xschem PROPERTIES VERSION 0.1 SOVERSION 0)
if(ROOM_INSTALL)
    install(TARGETS room_xschem LIBRARY DESTINATION lib)
    install(FILES room_xschem.h DESTINATION include)
endif()
if(ROOM_BUILD_TESTS)
    set(ROOM_XSCHEM_ORACLE_INPUTS "" CACHE STRING "Additional public .sch oracle inputs")
    add_executable(xschem_native_test tests/native.cpp)
    target_link_libraries(xschem_native_test PRIVATE room_xschem room_utils)
    add_test(NAME xschem_native COMMAND xschem_native_test
        ${CMAKE_CURRENT_BINARY_DIR}/test-output
        ${ROOM_ROOT}/examples/xschem_to_room/data/test.sch
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/wires_and_labels.sch
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/orientation.sch
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/header_sections.sch
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/text_and_graphics.sch
        ${ROOM_XSCHEM_ORACLE_INPUTS})
    set_tests_properties(xschem_native PROPERTIES LABELS "xschem;native" TIMEOUT 60)
endif()
