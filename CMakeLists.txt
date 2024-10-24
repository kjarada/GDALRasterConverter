cmake_minimum_required(VERSION 3.14)

# Project Name and Language
project(GDALRasterConverter LANGUAGES CXX)

# Set C++ Standard
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Enable Automatic Processing of Qt-related Files
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC OFF)
set(CMAKE_AUTORCC ON)

# Find Qt Packages
find_package(Qt5 COMPONENTS Widgets REQUIRED)

# Find GDAL
find_package(GDAL REQUIRED)

# Include Directories
include_directories(
    ${GDAL_INCLUDE_DIRS}
)

# Source Files
set(SOURCES
    main.cpp
)

# Add Executable with WIN32 flag to hide console window on Windows
add_executable(${PROJECT_NAME} WIN32 ${SOURCES})

# Link Libraries
target_link_libraries(${PROJECT_NAME}
    Qt5::Widgets
    ${GDAL_LIBRARIES}
)

# Additional Definitions (if needed)
# For example, if GDAL requires specific definitions, add them here
# add_definitions(-DGDAL_USE_VSI)
