cmake_minimum_required(VERSION 3.15)

# Find required packages
find_package(Protobuf REQUIRED)
find_package(gRPC REQUIRED)
find_package(nlohmann_json REQUIRED)

set(netSources
    Protocol/BaseNetProtocol.cpp
    Protocol/NetProtocol.cpp
    System/Net/DCFConnection.cpp
    System/Net/DCFUtils.cpp
)

add_library(net STATIC ${netSources})
target_link_libraries(net
    PRIVATE
        dcf_sdk
        ${PROTOBUF_LIBRARIES}
        ${GRPC_LIBRARIES}
        nlohmann_json::nlohmann_json
        Threads::Threads
)

target_compile_features(net PRIVATE cxx_std_17)
target_compile_options(net PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-Wall -Wextra -Werror>
    $<$<CXX_COMPILER_ID:Clang>:-Wall -Wextra -Werror>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
)

# Build DCF transport plugin
add_library(recoil_transport MODULE
    System/Net/RecoilTransport.cpp
)

target_link_libraries(recoil_transport
    PRIVATE
        dcf_sdk
        Threads::Threads
)

target_compile_features(recoil_transport PRIVATE cxx_std_17)
target_compile_options(recoil_transport PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-Wall -Wextra -Werror -fPIC>
    $<$<CXX_COMPILER_ID:Clang>:-Wall -Wextra -Werror -fPIC>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
)

# Install targets
install(TARGETS net recoil_transport
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
)
