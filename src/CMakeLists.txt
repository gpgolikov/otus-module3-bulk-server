list(APPEND ${PROJECT_NAME}_SOURCES
    logger.cpp
    statement.cpp
    statement_factory.cpp
    reader.cpp
    interpreter.cpp
    main.cpp)

add_executable(${PROJECT_NAME} ${${PROJECT_NAME}_SOURCES})

target_link_libraries(${PROJECT_NAME} 
    ${CMAKE_THREAD_LIBS_INIT}
    CONAN_PKG::boost
    CONAN_PKG::range-v3)

set_target_properties(${PROJECT_NAME} PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    COMPILE_OPTIONS "-Wpedantic;-Wall;-Wextra"
)

install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION bin)
