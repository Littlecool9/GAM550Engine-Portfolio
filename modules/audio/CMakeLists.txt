engine_add_module(audio)

find_package(Wwise REQUIRED)

target_sources(engine_audio PRIVATE 
    src/audio/Wwise.cpp 
    src/audio/AudioSystem.cpp
    "${WWISE_SDK_ROOT}/samples/SoundEngine/Win32/AkDefaultIOHookDeferred.cpp"
    "${WWISE_SDK_ROOT}/samples/SoundEngine/Common/AkMultipleFileLocation.cpp"
    "${WWISE_SDK_ROOT}/samples/SoundEngine/Common/AkGeneratedSoundBanksResolver.cpp")

target_link_libraries(engine_audio PRIVATE mir::core Wwise::Wwise)

target_compile_definitions(engine_audio PRIVATE UNICODE _UNICODE)

target_include_directories(engine_audio PRIVATE
    "${WWISE_SDK_ROOT}/samples/SoundEngine/Win32"
    "${WWISE_SDK_ROOT}/samples/SoundEngine/Common")