add_library(KeyboardTestModule STATIC
    KeyboardLayout.cpp KeyboardLayout.h KeyboardLogic.cpp KeyboardLogic.h
    KeyboardUi.cpp KeyboardUi.h KeyboardTestModule.cpp KeyboardTestModule.h)
target_include_directories(KeyboardTestModule PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_compile_definitions(KeyboardTestModule PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00 WINVER=0x0A00)
target_compile_options(KeyboardTestModule PRIVATE /W4 /WX /EHsc /permissive- /utf-8 /guard:cf)
target_link_libraries(KeyboardTestModule PRIVATE
    fluent::FluentUI ModuleConfig ModuleCommunication d2d1 dwrite dwmapi gdi32 user32)

if(BUILD_TESTING)
    add_executable(KeyboardTestTests tests/KeyboardTestTests.cpp)
    target_include_directories(KeyboardTestTests PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_compile_definitions(KeyboardTestTests PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00 WINVER=0x0A00)
    target_compile_options(KeyboardTestTests PRIVATE /W4 /WX /EHsc /permissive- /utf-8 /guard:cf)
    target_link_libraries(KeyboardTestTests PRIVATE KeyboardTestModule)
    add_test(NAME KeyboardTestTests COMMAND KeyboardTestTests)
    set_tests_properties(KeyboardTestTests PROPERTIES TIMEOUT 30)
endif()
