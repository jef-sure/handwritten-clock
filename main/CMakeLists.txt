idf_component_register(SRCS "main.c" "display.c" "clock_face.c" "city_face.c" "net.c" "settings.c" "tzones.c"
                         "weather.c" "weather_face.c" "status_face.c" "fonts/WeatherIconsRegular24.c"
                         "http_get.c" "location.c" "setup_screen.c" "webserver.c"
                    INCLUDE_DIRS ".")

# the table of time zones is made from the tz database of the machine that builds,
# the page of settings is cut into pieces at its %FIELDS%
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    set(WEB_TEMPLATE_HTML ${CMAKE_CURRENT_LIST_DIR}/../misc/index.html)
    set(WEB_TEMPLATE_SCRIPT ${CMAKE_CURRENT_LIST_DIR}/../misc/split_to_template.py)
    set(WEB_TEMPLATE_HEADER ${CMAKE_CURRENT_BINARY_DIR}/webserver_template.h)
    set(TZONES_SCRIPT ${CMAKE_CURRENT_LIST_DIR}/../misc/generate_tzones.py)
    set(TZONES_INCLUDE ${CMAKE_CURRENT_BINARY_DIR}/tzones_data.inc)

    if(DEFINED PYTHON AND NOT "${PYTHON}" STREQUAL "")
        set(BUILD_PYTHON ${PYTHON})
    else()
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        set(BUILD_PYTHON ${Python3_EXECUTABLE})
    endif()

    add_custom_command(
        OUTPUT ${TZONES_INCLUDE}
        COMMAND ${BUILD_PYTHON} ${TZONES_SCRIPT} build ${TZONES_INCLUDE}
        DEPENDS ${TZONES_SCRIPT}
        VERBATIM
    )
    add_custom_command(
        OUTPUT ${WEB_TEMPLATE_HEADER}
        COMMAND ${BUILD_PYTHON} ${WEB_TEMPLATE_SCRIPT} ${WEB_TEMPLATE_HTML} ${WEB_TEMPLATE_HEADER}
        DEPENDS ${WEB_TEMPLATE_HTML} ${WEB_TEMPLATE_SCRIPT}
        VERBATIM
    )
    add_custom_target(tzones_include DEPENDS ${TZONES_INCLUDE})
    add_custom_target(webserver_template DEPENDS ${WEB_TEMPLATE_HEADER})
    add_dependencies(${COMPONENT_LIB} tzones_include webserver_template)
    set_source_files_properties(tzones.c PROPERTIES OBJECT_DEPENDS ${TZONES_INCLUDE})
    set_source_files_properties(webserver.c PROPERTIES OBJECT_DEPENDS ${WEB_TEMPLATE_HEADER})
    target_include_directories(${COMPONENT_LIB} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
endif()
