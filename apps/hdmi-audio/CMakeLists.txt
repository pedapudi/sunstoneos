cmake_minimum_required(VERSION 3.24)
project(a733_hdmi_audio_application LANGUAGES C ASM)
set(SUNSTONE_HDMI_AUDIO ON CACHE BOOL "Finite internal I2S3 HDMI playback" FORCE)
include("${CMAKE_CURRENT_SOURCE_DIR}/../hdmi/CMakeLists.txt")
