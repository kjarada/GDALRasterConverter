GDALRasterConverter
GDALRasterConverter is a tool developed in C++ that simplifies the conversion of raster data between different formats using the GDAL library. It leverages the power of C++ for efficient processing and CMake for streamlined builds across platforms.

Features
Converts raster files between various formats (GeoTIFF, PNG, JPEG, etc.)
Efficient memory management and processing with C++
Supports custom conversion parameters
Built using CMake for easy project setup and management
Prerequisites
Before building and using GDALRasterConverter, ensure you have the following installed:

GDAL
CMake (version 3.10 or later)
A C++23 compatible compiler (e.g., GCC, Clang, MSVC)
Installation
Clone the repository

bash
Copy code
git clone https://github.com/kjarada/GDALRasterConverter.git
cd GDALRasterConverter
Install GDAL and its development libraries



Contribution
We welcome contributions to improve GDALRasterConverter. Feel free to submit issues or pull requests on GitHub.

How to Contribute
Fork the repository.
Create a new feature branch (git checkout -b feature/new-feature).
Commit your changes (git commit -am 'Add new feature').
Push to the branch (git push origin feature/new-feature).
Create a pull request.
License
This project is licensed under the MIT License.

Credits
GDAL for providing the underlying raster processing capabilities.
CMake for the build configuration.
This version reflects the use of C++ and CMake, including instructions for building the project, using the CLI, and enabling CUDA support. Let me know if you'd like any further changes!
