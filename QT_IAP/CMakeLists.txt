cmake_minimum_required(VERSION 3.19)
project(PC_IAP LANGUAGES CXX)

find_package(Qt6 6.5 REQUIRED COMPONENTS
    Core
    Widgets
    SerialPort
    Network
)

qt_standard_project_setup()

qt_add_executable(PC_IAP
    WIN32 MACOSX_BUNDLE
    main.cpp
    mainwindow.cpp
    mainwindow.h
    mainwindow.ui
    bootprotocol.cpp
    bootprotocol.h
)

target_link_libraries(PC_IAP
    PRIVATE
        Qt::Core
        Qt::Widgets
        Qt::SerialPort
        Qt::Network
)

install(TARGETS PC_IAP
    BUNDLE  DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

qt_generate_deploy_app_script(
    TARGET PC_IAP
    OUTPUT_SCRIPT deploy_script
    NO_UNSUPPORTED_PLATFORM_ERROR
)
install(SCRIPT ${deploy_script})
