# Create your own CMakeLists.txt file here that will build your project, based 
# on the directory structure you choose. You can use the CMakeLists.txt files
# we provided in the src directory of Project 1 and Project 2 as a starting point.

# You will also have to add a CMakelists.txt file in the common/ directory. 

# Running the standard CMake build sequence should result in the following executables:
#   wSender
#   wReceiver
#   wSenderOpt
#   wReceiverOpt

cmake_policy(SET CMP0167 NEW)

cmake_minimum_required(VERSION 3.10)
project(WTP)

set(CMAKE_CXX_STANDARD 11)

add_executable(wSender wSender.cpp)
add_executable(wReceiver wReceiver.cpp)
add_executable(wSenderOpt wSenderOpt.cpp)
add_executable(wReceiverOpt wReceiverOpt.cpp)
