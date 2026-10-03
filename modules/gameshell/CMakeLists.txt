engine_add_module(gameshell)

# PUBLIC: UIContext (gameui) appears in include/gameshell/. Nothing else: the
# screens are built on gameui's public API alone, as a game's would be.
target_link_libraries(engine_gameshell PUBLIC mir::gameui)

target_sources(engine_gameshell PRIVATE
    src/gameshell/GameHud.cpp
    src/gameshell/GameShell.cpp
    src/gameshell/PauseMenu.cpp
)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
