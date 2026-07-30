cmake_minimum_required(VERSION 2.8)

project(yolo26)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_SYSTEM_PROCESSOR arm)



if(EXTERN_DEFINE_TARGET)
    message (STATUS "USE EXTERN DEFINE TARGET")
else()
    #set(TARGET_NAME T527)
    #set(TARGET_NAME T536)
endif()

# ------------------------------- FLAGS -----------------------------
set(MODEL_ZOO_HOME_DIR ${CMAKE_SOURCE_DIR}/../..)

if(USE_EXTERN_TOOLCHAIN)
    message (STATUS "USE EXTERN TOOLCHAIN BUILD")
else()
    #set(CMAKE_C_COMPILER ${MODEL_ZOO_HOME_DIR}/0-toolchains/arm-openwrt-linux-muslgnueabi/bin/arm-openwrt-linux-muslgnueabi-gcc)
    #set(CMAKE_CXX_COMPILER ${MODEL_ZOO_HOME_DIR}/0-toolchains/arm-openwrt-linux-muslgnueabi/bin/arm-openwrt-linux-muslgnueabi-g++)
    #set(CMAKE_C_COMPILER ${MODEL_ZOO_HOME_DIR}/0-toolchains/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu-gcc)
    #set(CMAKE_CXX_COMPILER ${MODEL_ZOO_HOME_DIR}/0-toolchains/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu-g++)
endif()


message (STATUS "CMAKE_C_COMPILER = ${CMAKE_C_COMPILER}")
message (STATUS "CMAKE_SYSTEM_NAME = ${CMAKE_SYSTEM_NAME}")
message (STATUS "TARGET_NAME = ${TARGET_NAME}")

if (CMAKE_SYSTEM_NAME STREQUAL "Android")
    set(SYS_ARCH android)
elseif(CMAKE_C_COMPILER MATCHES "aarch64")
    set(SYS_ARCH linux_aarch64)
    add_definitions(-D_GLIBCXX_USE_CXX11_ABI=0)
else()
    set(SYS_ARCH linux_armhf)
    add_definitions(-DLIBVIP_VERSION_85X)
endif()

if(SYS_ARCH STREQUAL "linux_armhf")
    FIND_PACKAGE(OpenMP REQUIRED)
elseif(SYS_ARCH STREQUAL "android")
    FIND_PACKAGE(OpenMP REQUIRED)
endif()
if(OPENMP_FOUND)
    message("OPENMP FOUND")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${OpenMP_C_FLAGS}")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${OpenMP_CXX_FLAGS}")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${OpenMP_EXE_LINKER_FLAGS}")
endif()

if(SYS_ARCH STREQUAL "linux_armhf")
    set(OpenCV_DIR ${MODEL_ZOO_HOME_DIR}/3rdparty/opencv/opencv-3.4.16-gnueabihf-linux/share/OpenCV)
elseif(SYS_ARCH STREQUAL "linux_aarch64")
    set(OpenCV_DIR ${MODEL_ZOO_HOME_DIR}/3rdparty/opencv/opencv-4.9.0-aarch64-linux-sunxi-glibc/lib/cmake/opencv4)
elseif(SYS_ARCH STREQUAL "android")
    set(OpenCV_DIR ${MODEL_ZOO_HOME_DIR}/3rdparty/opencv/opencv-4.9.0-android/sdk/native/jni/abi-${ANDROID_ABI})
else()
    message(FATAL_ERROR "SYS_ARCH is not linux_armhf, linux_aarch64 or android.")
endif()

find_package(OpenCV REQUIRED)


set(NPU_RT_DIR ${MODEL_ZOO_HOME_DIR}/common/npuruntime)
if(SYS_ARCH STREQUAL "android")
    FIle(GLOB NPU_RT_LIBS "${NPU_RT_DIR}/lib_${SYS_ARCH}/${TARGET_NAME}/${ANDROID_ABI}/*.so*")
else()
    FIle(GLOB NPU_RT_LIBS "${NPU_RT_DIR}/lib_${SYS_ARCH}/${TARGET_NAME}/*.so*")
endif()


# Add the source in project root directory
aux_source_directory(. DIRSRCS)
aux_source_directory(${NPU_RT_DIR} NPULIB_DIRSRCS)

# Add header file include directories
if(SYS_ARCH STREQUAL "linux_armhf")
    include_directories(./ ${NPU_RT_DIR} ${NPU_RT_DIR}/include_85x)
else()
    include_directories(./ ${NPU_RT_DIR} ${NPU_RT_DIR}/include)
endif()

# Add block directories


string(TOLOWER "${TARGET_NAME}" TARGET_LOWER)
string(TOLOWER "${CMAKE_SYSTEM_NAME}" SYSTEM_LOWER)
# Target
add_executable(${PROJECT_NAME}_${TARGET_LOWER} ${DIRSRCS} ${NPULIB_DIRSRCS})
target_link_libraries(${PROJECT_NAME}_${TARGET_LOWER} ${OpenCV_LIBS} ${NPU_RT_LIBS} -lm -ldl -pthread)


# install
set(CMAKE_INSTALL_PREFIX ${CMAKE_SOURCE_DIR}/install/${PROJECT_NAME}_${SYSTEM_LOWER}_${TARGET_LOWER})
message (STATUS "CMAKE_INSTALL_PREFIX = ${CMAKE_INSTALL_PREFIX}")
install(TARGETS ${PROJECT_NAME}_${TARGET_LOWER} DESTINATION ./)

if(SYS_ARCH STREQUAL "android")
    install(FILES ${NPU_RT_LIBS} DESTINATION ./lib/)
    install(FILES ${NPU_RT_DIR}/lib_${SYS_ARCH}/libomp.so DESTINATION ./lib/)
endif()

if(SYS_ARCH STREQUAL "android" AND ANDROID_STL STREQUAL "c++_shared")
    get_filename_component(STL_LIB_PATH ${CMAKE_CXX_COMPILER} DIRECTORY)
    set(STL_SHARED_LIB "${STL_LIB_PATH}/../sysroot/usr/lib/aarch64-linux-android/libc++_shared.so")

    if(EXISTS ${STL_SHARED_LIB})
        install(FILES ${STL_SHARED_LIB} DESTINATION lib)
    else()
        message(WARNING "libc++_shared.so not found at: ${STL_SHARED_LIB}")
    endif()
endif()


file(GLOB MODEL_FILE "model/*_${TARGET_LOWER}.nb")
file(GLOB TEST_IMAGE_FILES "model/*.jpg")
install(FILES ${MODEL_FILE} DESTINATION ./model/)
install(FILES ${TEST_IMAGE_FILES} DESTINATION ./model/)
