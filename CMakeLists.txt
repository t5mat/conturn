cmake_minimum_required(VERSION 3.20)

project(conturn CXX)

set_property(
    SOURCE
        "${CMAKE_CURRENT_LIST_DIR}/conturn.rc"
    APPEND PROPERTY OBJECT_DEPENDS
        "${CMAKE_CURRENT_LIST_DIR}/conturn.ico"
)

add_executable(conturn
    "${CMAKE_CURRENT_LIST_DIR}/conturn.rc"
    "${CMAKE_CURRENT_LIST_DIR}/conturn.cpp"
)

target_compile_options(conturn PRIVATE -std=c++2b -Wall -Werror -Wno-switch -Wno-string-plus-int)

target_link_libraries(conturn PRIVATE
    -static
    -Wl,--subsystem,windows
    -Wl,/manifestuac:"level='requireAdministrator' uiAccess='false'"
    -Wl,/manifestdependency:"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'"
    -Wl,/manifest:embed
    -lversion -lpathcch -lshlwapi
)

set_target_properties(conturn PROPERTIES LINK_FLAGS_RELEASE -s)
