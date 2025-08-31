cmake_minimum_required(VERSION 3.15)

# Find dependencies
find_package(Protobuf REQUIRED)
find_package(gRPC REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(Threads REQUIRED)
find_package(spdlog REQUIRED)
find_package(Boost REQUIRED COMPONENTS lockfree)
find_package(asio REQUIRED)

# Net sources including legacy files
set(netSources
    Protocol/BaseNetProtocol.cpp
    Protocol/NetProtocol.cpp
    System/Net/DCFConnection.cpp
    System/Net/DCFUtils.cpp
    System/Net/LegacyUDPConnection.cpp  # New legacy wrapper
    GameParticipant.cpp  # Untouched legacy
)

# Build net library
add_library(net STATIC ${netSources})
target_link_libraries(net
    PRIVATE
        dcf_sdk
        protobuf::libprotobuf
        gRPC::grpc++
        nlohmann_json::nlohmann_json
        spdlog::spdlog
        Boost::lockfree
        Threads::Threads
        asio::asio
)

target_compile_features(net PRIVATE cxx_std_17)
target_compile_options(net PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-Wall -Wextra -Werror -O3 -march=native -flto>
    $<$<CXX_COMPILER_ID:Clang>:-Wall -Wextra -Werror -O3 -march=native -flto>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX /O2 /GL>
)

target_include_directories(net PRIVATE
    ${CMAKE_SOURCE_DIR}/rts/Protocol  # For BaseNetProtocol, NetMessageTypes, NetProtocol
    ${CMAKE_SOURCE_DIR}/rts/GameParticipant  # For GameParticipant
)

# Build transport plugin
add_library(bumpstock_transport MODULE System/Net/BumpStockTransport.cpp)
target_link_libraries(bumpstock_transport
    PRIVATE
        dcf_sdk
        asio::asio
        Threads::Threads
        Boost::lockfree
)

target_compile_features(bumpstock_transport PRIVATE cxx_std_17)
target_compile_options(bumpstock_transport PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-Wall -Wextra -Werror -fPIC -O3 -march=native -flto>
    $<$<CXX_COMPILER_ID:Clang>:-Wall -Wextra -Werror -fPIC -O3 -march=native -flto>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX /O2 /GL>
)

# Install
install(TARGETS net bumpstock_transport
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
)
