# ============================================================================
# moonlightweb-clipboard-agent — the clipboard agent for streamed hosts.
#
# Runs on a Sunshine host, inside its user's desktop session, and shares that
# session's clipboard with a MoonlightWeb server on another machine
# (docs/design/clipboard-agent.md). Small on purpose: Qt Core, Gui, Network
# and WebSockets (+ D-Bus on Linux for Klipper), and none of the server's
# streaming stack, so it builds on its own on the host:
#
#   cmake -S backend/clipboard-agent -B build-agent -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-agent
#
# The server build adds it too (MW_BUILD_CLIPBOARD_AGENT, ON by default).
# ============================================================================
cmake_minimum_required(VERSION 3.21)
project(moonlightweb-clipboard-agent VERSION 1.0.0 LANGUAGES CXX)

if(NOT CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif()

find_package(Qt6 6.2 REQUIRED COMPONENTS Core Gui Network WebSockets)
# Klipper (KDE) is reached over D-Bus; without Qt D-Bus that backend is left out.
if(UNIX AND NOT APPLE)
    find_package(Qt6 QUIET COMPONENTS DBus)
endif()
set(CMAKE_AUTOMOC ON)

set(_mw_src ${CMAKE_CURRENT_SOURCE_DIR}/../src)

# The parts the server's unit tests exercise too (backend/tests).
add_library(mw-clipboard-agent-core STATIC
    src/AgentConfig.cpp
    src/AgentClient.cpp
    src/BackendSelect.cpp
    src/PinCheck.cpp
    src/ClipboardBackend.h
    # The wire protocol, shared with the server.
    ${_mw_src}/clipboard/ClipboardAgentProtocol.cpp
)
target_include_directories(mw-clipboard-agent-core PUBLIC src ${_mw_src})
target_link_libraries(mw-clipboard-agent-core PUBLIC Qt6::Core Qt6::Network Qt6::WebSockets)
target_compile_definitions(mw-clipboard-agent-core PUBLIC
    MW_AGENT_VERSION="${PROJECT_VERSION}")
if(TARGET Qt6::DBus)
    target_compile_definitions(mw-clipboard-agent-core PUBLIC MW_AGENT_HAVE_DBUS)
endif()

add_executable(moonlightweb-clipboard-agent
    src/main.cpp
    src/QtClipboardBackend.cpp
    src/WlClipboardBackend.cpp
)
target_link_libraries(moonlightweb-clipboard-agent PRIVATE mw-clipboard-agent-core Qt6::Gui)
if(TARGET Qt6::DBus)
    target_sources(moonlightweb-clipboard-agent PRIVATE src/KlipperBackend.cpp)
    target_link_libraries(moonlightweb-clipboard-agent PRIVATE Qt6::DBus)
endif()

if(PROJECT_IS_TOP_LEVEL)
    include(GNUInstallDirs)
    install(TARGETS moonlightweb-clipboard-agent RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    if(UNIX AND NOT APPLE)
        configure_file(packaging/moonlightweb-clipboard-agent.service.in
                       ${CMAKE_CURRENT_BINARY_DIR}/moonlightweb-clipboard-agent.service @ONLY)
        install(FILES ${CMAKE_CURRENT_BINARY_DIR}/moonlightweb-clipboard-agent.service
                DESTINATION lib/systemd/user)
    endif()
endif()
