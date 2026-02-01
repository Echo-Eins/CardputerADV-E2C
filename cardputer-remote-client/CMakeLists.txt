# Cardputer Remote Client - ESP-IDF Project
cmake_minimum_required(VERSION 3.16)

# Set target to ESP32-S3 (M5Stack Cardputer)
set(IDF_TARGET esp32s3)

# Include IDF build system
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(cardputer_remote_client)
