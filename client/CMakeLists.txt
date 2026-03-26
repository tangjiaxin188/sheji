cmake_minimum_required(VERSION 3.20)
project(mongoose_demo)

set(CMAKE_C_STANDARD 11)

include_directories(${CMAKE_SOURCE_DIR}/include)

add_executable(mongoose_demo
    src/mongoose.c
    src/monocypher.c
    src/monocypher-ed25519.c
    src/main.c
)
