if(NOT MAQUE_ANDROID)
    return()
endif()

add_library(main SHARED main.cpp)
target_link_libraries(main PRIVATE maque::runtime)
if(TARGET SDL2::SDL2)
    target_link_libraries(main PRIVATE SDL2::SDL2)
elseif(TARGET SDL2)
    target_link_libraries(main PRIVATE SDL2)
else()
    message(FATAL_ERROR "maque Android host requires SDL2")
endif()
set_target_properties(main PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    OUTPUT_NAME "main")
