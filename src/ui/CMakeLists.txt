cmake_minimum_required(VERSION 3.5)

project(SmartBookshelf LANGUAGES C)

set(UI_SOURCES
    archive/stringlist.c
    archive/string_s.c
    archive/window.c
    archive/font_songti_24.c
    ui_model.c
    main.c
    ../common/sb_ipc.c
    ../db/db.c
    ../db/sqlite3.c
)

add_executable(${PROJECT_NAME} ${UI_SOURCES})

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/archive
    ${CMAKE_CURRENT_SOURCE_DIR}/../db
    ${CMAKE_CURRENT_SOURCE_DIR}/../fsm
    ${CMAKE_CURRENT_SOURCE_DIR}/../common
)

set(COMMON_LIBS -Wl,--start-group lvgl lvgl_demos -Wl,--end-group)

if(CMAKE_CROSSCOMPILING)
    target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_SYSROOT}/usr/include)
    target_link_libraries(${PROJECT_NAME} ${COMMON_LIBS} pthread freetype evdev drm m dl)
    message("ARM build: SmartBookshelf rewritten UI")
else()
    add_compile_definitions(LVGL_SIMULATION)
    target_link_libraries(${PROJECT_NAME} ${COMMON_LIBS} pthread freetype X11 m)
    message("Desktop build: SmartBookshelf rewritten UI simulation")
endif()

install(TARGETS SmartBookshelf RUNTIME DESTINATION ${DESTDIR}/usr/bin/lvgl)
