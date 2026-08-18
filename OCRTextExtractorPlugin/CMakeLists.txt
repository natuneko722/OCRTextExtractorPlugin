cmake_minimum_required(VERSION 3.21)
project(OCRTextExtractorPlugin VERSION 1.4.0 LANGUAGES CXX)

set(AVIUTL2_SDK_DIR "D:/創作物全般/codex君/aviutl2_sdk" CACHE PATH "Path to the AviUtl2 Plugin SDK")

add_library(OCRTextExtractorPlugin SHARED
    main.cpp
    app/app.cpp
    ocr/ocr_manager.cpp
    ocr/ocr_formatter.cpp
    history/history_manager.cpp
    capture/capture_object.cpp
    capture/image_saver.cpp
    window/ocr_window.cpp
    config/config_io.cpp)

target_include_directories(OCRTextExtractorPlugin PRIVATE ${AVIUTL2_SDK_DIR} .)
target_compile_features(OCRTextExtractorPlugin PRIVATE cxx_std_20)
target_compile_definitions(OCRTextExtractorPlugin PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(OCRTextExtractorPlugin PRIVATE /utf-8 /wd4828)
target_link_libraries(OCRTextExtractorPlugin PRIVATE windowsapp d3d11 windowscodecs ole32 shell32)
set_target_properties(OCRTextExtractorPlugin PROPERTIES PREFIX "" SUFFIX ".aux2")
