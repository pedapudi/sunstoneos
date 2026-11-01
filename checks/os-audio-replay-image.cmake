# Add an audio-specific application link check after the normal OS targets.
# Pass this file through CMAKE_PROJECT_TOP_LEVEL_INCLUDES; no shared build
# fragment or application registration changes are needed.
function(sunstone_audio_replay_check_image)
  add_sunstone_application(audio-replay
    SOURCES "${SUNSTONE_SOURCE_ROOT}/checks/os-audio-replay-application.c"
    MANIFEST "${SUNSTONE_SOURCE_ROOT}/checks/os-audio-replay-manifest.json")
  add_sunstone_image(sunstone-a733-os-audio-replay
    LIFETIME supervised APPLICATION audio-replay)
endfunction()
cmake_language(DEFER CALL sunstone_audio_replay_check_image)
