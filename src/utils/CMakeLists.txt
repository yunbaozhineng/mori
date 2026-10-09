add_library(mori_logging mori_log.cpp)
target_include_directories(mori_logging PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(mori_logging PUBLIC spdlog::spdlog)
