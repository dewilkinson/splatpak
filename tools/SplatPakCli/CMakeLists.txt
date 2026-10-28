# SplatPakCli: SplatPak's command line (docs/requirements/cli.md). A console program: the package commands run headless (no
# window, no GPU); the others start SplatPak.exe beside it. (Not splatpak.exe: Windows file names ignore case, and it would
# be SplatPak.exe.)
add_executable(SplatPakCli ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp)
target_link_libraries(SplatPakCli PRIVATE gstx)
set_target_properties(SplatPakCli PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    FOLDER "Tools"
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_HOME_DIRECTORY}"
    DEBUG_POSTFIX "d"
)
