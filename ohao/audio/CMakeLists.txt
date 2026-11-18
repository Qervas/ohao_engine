# The audio module (audio/module.hpp) and the two libraries below it in its directory: the bank (the .audio.json
# reader, plain data with no miniaudio: the schedule reads it) and the mixer (miniaudio: a window plays the schedule's
# events through it, runtime/audio_player.hpp).
add_library(ohao_audio_bank STATIC audio_bank.cpp audio_bank.hpp)
target_include_directories(ohao_audio_bank PUBLIC ${CMAKE_SOURCE_DIR}/ohao)
target_compile_features(ohao_audio_bank PUBLIC cxx_std_20)
target_link_libraries(ohao_audio_bank PUBLIC ohao_core PRIVATE ohao_simdjson)

add_library(ohao_audio_mixer STATIC audio_system.cpp audio_system.hpp reverb_node.cpp reverb_node.hpp)
target_include_directories(ohao_audio_mixer PUBLIC ${CMAKE_SOURCE_DIR}/ohao)
target_compile_features(ohao_audio_mixer PUBLIC cxx_std_20)
target_link_libraries(ohao_audio_mixer PUBLIC ohao_core ohao_audio_bank miniaudio glm)
# miniaudio's platform backends.
find_package(Threads REQUIRED)
target_link_libraries(ohao_audio_mixer PUBLIC Threads::Threads dl m)

ohao_add_module(audio DEPENDS ecs visual animation physics LINKS ohao_audio_bank
    SOURCES audio_schedule.cpp audio_schedule.hpp components.hpp events.hpp module.cpp module.hpp storage.cpp types.cpp
            types_events.cpp)
