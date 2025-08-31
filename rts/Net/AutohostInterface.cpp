/* This file is part of the BumpStockEngine (GPL v2 or later), see LICENSE.html */

#include "AutohostInterface.h"
#include "Net/Protocol/BaseNetProtocol.h"
#include <cstring>
#include <chrono>
#include <algorithm>  // For std::min

#define LOG_SECTION_AUTOHOST_INTERFACE "AutohostInterface"
LOG_REGISTER_SECTION_GLOBAL(LOG_SECTION_AUTOHOST_INTERFACE)

#ifdef LOG_SECTION_CURRENT
    #undef LOG_SECTION_CURRENT
#endif
#define LOG_SECTION_CURRENT LOG_SECTION_AUTOHOST_INTERFACE

AutohostInterface::AutohostInterface(const std::string& remoteIP, int remotePort,
                                     const std::string& localIP, int localPort)
    : ioContext(std::make_unique<asio::io_context>()) {
    try {
        // Primary: Try DCF integration
        dcfConnection = std::make_unique<DCFConnection>("config/dcf_network.json");
        if (dcfConnection->initialized) {
            dcf::DCF_LOG(dcf::DCFLogLevel::INFO, "Autohost using DCF successfully");
            initialized = true;
            running = true;
            ReceiveAsync();  // Start DCF-based receive if applicable
            return;
        } else {
            dcf::DCF_LOG(dcf::DCFLogLevel::WARNING, "DCF init failed for autohost, falling back to UDP");
            usingFallback = true;
        }
    } catch (const std::exception& e) {
        dcf::DCF_LOG(dcf::DCFLogLevel::ERROR, "DCF setup for autohost failed: " + std::string(e.what()) + ", falling back to UDP");
        usingFallback = true;
    }

    // Fallback: UDP setup
    if (usingFallback) {
        try {
            udpSocket = std::make_shared<asio::ip::udp::socket>(*ioContext);
            std::string error = TryBindSocket(*udpSocket, remoteIP, remotePort, localIP, localPort);
            if (!error.empty()) {
                dcf::DCF_LOG(dcf::DCFLogLevel::ERROR, "UDP socket bind failed: " + error);
                return;
            }
            running = true;
            // Start worker threads for fallback
            for (int i = 0; i < NUM_WORKER_THREADS; ++i) {
                workerThreads.emplace_back([this]() {
                    while (running) {
                        try {
                            ioContext->run_one();
                        } catch (const std::exception& e) {
                            dcf::DCF_LOG(dcf::DCFLogLevel::ERROR, std::string("Fallback IO worker error: ") + e.what());
                        }
                    }
                });
            }
            ReceiveAsync();  // Start async receive for fallback
            initialized = true;
            dcf::DCF_LOG(dcf::DCFLogLevel::INFO, "Autohost fallback to UDP initialized on port " + std::to_string(remotePort));
        } catch (const std::exception& e) {
            dcf::DCF_LOG(dcf::DCFLogLevel::FATAL, std::string("Fallback initialization failed: ") + e.what());
        }
    }
}

AutohostInterface::~AutohostInterface() {
    running = false;
    if (ioContext) ioContext->stop();
    for (auto& th : workerThreads) {
        if (th.joinable()) th.join();
    }
    if (udpSocket && udpSocket->is_open()) udpSocket->close();
    dcfConnection.reset();
    dcf::DCF_LOG(dcf::DCFLogLevel::INFO, "AutohostInterface destroyed");
}

std::string AutohostInterface::TryBindSocket(asio::ip::udp::socket& socket,
                                             const std::string& remoteIP, int remotePort,
                                             const std::string& localIP, int localPort) {
    try {
        asio::ip::udp::resolver resolver(socket.get_executor());
        remoteEndpoint = *resolver.resolve(remoteIP, std::to_string(remotePort)).begin();
        socket.open(asio::ip::udp::v4());
        if (!localIP.empty()) {
            socket.bind(asio::ip::udp::endpoint(asio::ip::address::from_string(localIP), localPort));
        } else {
            socket.bind(asio::ip::udp::endpoint(asio::ip::udp::v4(), localPort));
        }
        return "";
    } catch (const asio::system_error& e) {
        return e.what();
    }
}

void AutohostInterface::SendAsync(std::vector<std::uint8_t> buffer) {
    if (!initialized) {
        dcf::DCF_LOG(dcf::DCFLogLevel::WARNING, "Interface not initialized for send");
        return;
    }
    if (dcfConnection && !usingFallback) {
        // Use DCF for send
        auto rawPacket = std::make_shared<RawPacket>(buffer.data(), buffer.size());
        dcfConnection->SendData(rawPacket);
    } else if (udpSocket && udpSocket->is_open()) {
        // Fallback UDP async send
        TrySendWithRetry(buffer);
    } else {
        dcf::DCF_LOG(dcf::DCFLogLevel::WARNING, "No valid socket for send");
    }
}

void AutohostInterface::TrySendWithRetry(const std::vector<std::uint8_t>& buffer, int maxRetries) {
    std::lock_guard<std::mutex> lock(sendMutex);
    for (int attempt = 0; attempt < maxRetries; ++attempt) {
        try {
            udpSocket->async_send_to(asio::buffer(buffer), remoteEndpoint,
                [this, buffer](const asio::error_code& error, size_t) {
                    if (error) {
                        dcf::DCF_LOG(dcf::DCFLogLevel::WARNING, "Fallback async send failed: " + error.message());
                    }
                });
            return;
        } catch (const asio::system_error& e) {
            dcf::DCF_LOG(dcf::DCFLogLevel::WARNING, "Fallback send attempt " + std::to_string(attempt + 1) + " failed: " + e.what());
            std::this_thread::sleep_for(std::chrono::milliseconds(std::min(100 * (attempt + 1), 500)));
        }
    }
    dcf::DCF_LOG(dcf::DCFLogLevel::ERROR, "All fallback send retries failed; closing socket");
    udpSocket->close();
}

void AutohostInterface::ReceiveAsync() {
    if (!initialized || !running) return;
    if (usingFallback && udpSocket && udpSocket->is_open()) {
        udpSocket->async_receive_from(
            asio::buffer(receiveBuffer), remoteEndpoint,
            [this](const asio::error_code& error, size_t bytes_transferred) {
                HandleReceive(error, bytes_transferred);
            });
    } else if (dcfConnection) {
        // DCF receive handled in DCFConnection's threads
    }
}

void AutohostInterface::HandleReceive(const asio::error_code& error, size_t bytes_transferred) {
    if (!error && bytes_transferred > 0) {
        std::vector<std::uint8_t> data(receiveBuffer.begin(), receiveBuffer.begin() + bytes_transferred);
        while (!receiveQueue.push(data)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        dcf::DCF_LOG(dcf::DCFLogLevel::DEBUG, "Received " + std::to_string(bytes_transferred) + " bytes via fallback");
    } else if (error) {
        dcf::DCF_LOG(dcf::DCFLogLevel::ERROR, "Fallback receive error: " + error.message());
    }
    if (running && usingFallback && udpSocket->is_open()) ReceiveAsync();
}

void AutohostInterface::SendStart() {
    if (!initialized) return;
    std::vector<std::uint8_t> buffer(1, SERVER_STARTED);
    SendAsync(buffer);
}

void AutohostInterface::SendQuit() {
    if (!initialized) return;
    std::vector<std::uint8_t> buffer(1, SERVER_QUIT);
    SendAsync(buffer);
}

void AutohostInterface::SendStartPlaying(const uchar* gameID, const std::string& demoName) {
    if (!initialized) return;
    const auto msgsize = sizeof(std::uint32_t) + 16 * sizeof(uchar) + demoName.size() + 1;  // +1 for safety
    std::vector<std::uint8_t> buffer(msgsize);
    buffer[0] = SERVER_STARTPLAYING;
    std::memcpy(&buffer[1], &msgsize, sizeof(std::uint32_t));
    std::memcpy(&buffer[5], gameID, 16 * sizeof(uchar));
    std::copy(demoName.begin(), demoName.end(), buffer.begin() + 21);
    SendAsync(buffer);
}

void AutohostInterface::SendGameOver(uchar playerNum, const std::vector<uchar>& winningAllyTeams) {
    if (!initialized) return;
    const auto msgsize = 3 * sizeof(uchar) + winningAllyTeams.size();
    std::vector<std::uint8_t> buffer(msgsize);
    buffer[0] = SERVER_GAMEOVER;
    buffer[1] = playerNum;
    buffer[2] = static_cast<uchar>(winningAllyTeams.size() + 3);
    std::copy(winningAllyTeams.begin(), winningAllyTeams.end(), buffer.begin() + 3);
    SendAsync(buffer);
}

void AutohostInterface::SendPlayerJoined(uchar playerNum, const std::string& name) {
    if (!initialized) return;
    const auto msgsize = 2 * sizeof(uchar) + name.size() + 1;
    std::vector<std::uint8_t> buffer(msgsize);
    buffer[0] = PLAYER_JOINED;
    buffer[1] = playerNum;
    std::copy(name.begin(), name.end(), buffer.begin() + 2);
    SendAsync(buffer);
}

void AutohostInterface::SendPlayerLeft(uchar playerNum, uchar reason) {
    if (!initialized) return;
    std::vector<std::uint8_t> buffer(3);
    buffer[0] = PLAYER_LEFT;
    buffer[1] = playerNum;
    buffer[2] = reason;
    SendAsync(buffer);
}

void AutohostInterface::SendPlayerReady(uchar playerNum, uchar readyState) {
    if (!initialized) return;
    std::vector<std::uint8_t> buffer(3);
    buffer[0] = PLAYER_READY;
    buffer[1] = playerNum;
    buffer[2] = readyState;
    SendAsync(buffer);
}

void AutohostInterface::SendPlayerChat(uchar playerNum, uchar destination, const std::string& msg) {
    if (!initialized) return;
    const auto msgsize = 3 * sizeof(uchar) + msg.size() + 1;
    std::vector<std::uint8_t> buffer(msgsize);
    buffer[0] = PLAYER_CHAT;
    buffer[1] = playerNum;
    buffer[2] = destination;
    std::copy(msg.begin(), msg.end(), buffer.begin() + 3);
    SendAsync(buffer);
}

void AutohostInterface::SendPlayerDefeated(uchar playerNum) {
    if (!initialized) return;
    std::vector<std::uint8_t> buffer(2);
    buffer[0] = PLAYER_DEFEATED;
    buffer[1] = playerNum;
    SendAsync(buffer);
}

void AutohostInterface::SendLuaMsg(const std::uint8_t* msg, size_t msgSize) {
    if (!initialized) return;
    std::vector<std::uint8_t> buffer(msgSize + 1);
    buffer[0] = GAME_LUAMSG;
    std::copy(msg, msg + msgSize, buffer.begin() + 1);
    SendAsync(buffer);
}

void AutohostInterface::Send(const std::uint8_t* msg, size_t msgSize) {
    if (!initialized) return;
    std::vector<std::uint8_t> buffer(msgSize);
    std::copy(msg, msg + msgSize, buffer.begin());
    SendAsync(buffer);
}

std::vector<std::uint8_t> AutohostInterface::GetChatMessage() {
    if (!initialized) return {};
    if (dcfConnection && !usingFallback) {
        // Use DCF's GetData for receive
        auto pkt = dcfConnection->GetData();
        if (pkt) {
            return std::vector<std::uint8_t>(pkt->data, pkt->data + pkt->length);
        }
        return {};
    } else {
        std::vector<std::uint8_t> data;
        if (receiveQueue.pop(data)) {
            return data;
        }
        return {};
    }
}
