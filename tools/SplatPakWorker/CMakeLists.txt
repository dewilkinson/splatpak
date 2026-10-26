# SplatPakWorker: SplatPak's headless compute worker (RDO jobs; docs/design/distributed-rdo.md). No window, no GPU: it runs
# on this machine, BLUESHELL, or a partner's machine through the fleet relay.
add_executable(SplatPakWorker ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp)
target_link_libraries(SplatPakWorker PRIVATE gstx)
set_target_properties(SplatPakWorker PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    FOLDER "Tools"
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_HOME_DIRECTORY}"
    DEBUG_POSTFIX "d"
)
