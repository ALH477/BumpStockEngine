#ifndef AUTOHOST_INTERFACE_H
#define AUTOHOST_INTERFACE_H

#include <string>
#include <cinttypes>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <boost/lockfree/spsc_queue.hpp>
#include "System/Net/DCFUtils.h"
#include "System/Net/DCFConnection.h"  // For DCF integration

/**
 * API for engine <-> autohost communication, integrated with DCF in BumpStockEngine.
 * Uses DCF for primary communication; falls back to async UDP on DCF errors.
 */
class AutohostInterface
{
public:
    typedef unsigned char uchar;

    AutohostInterface(const std::string& remoteIP, int remotePort,
                      const std::string& localIP = "", int localPort = 0);
    virtual ~AutohostInterface();

    bool IsInitialized() const { return initialized; }

    void SendStart();
    void SendQuit();
    void SendStartPlaying(const uchar* gameID, const std::string& demoName);
    void SendGameOver(uchar playerNum, const std::vector<uchar>& winningAllyTeams);
    void SendPlayerJoined(uchar playerNum, const std::string& name);
    void SendPlayerLeft(uchar playerNum, uchar reason);
    void SendPlayerReady(uchar playerNum, uchar readyState);
    void SendPlayerChat(uchar playerNum, uchar destination, const std::string& msg);
    void SendPlayerDefeated(uchar playerNum);
    void SendLuaMsg(const std::uint8_t* msg, size_t msgSize);
    void Send(const std::uint8_t* msg, size_t msgSize);

    std::vector<std::uint8_t> GetChatMessage();

private:
    void SendAsync(std::vector<std::uint8_t> buffer);
    void ReceiveAsync();
    void HandleReceive(const asio::error_code& error, size_t bytes_transferred);
    void TrySendWithRetry(const std::vector<std::uint8_t>& buffer, int maxRetries = 3);

    static std::string TryBindSocket(asio::ip::udp::socket& socket,
                                    const std::string& remoteIP, int remotePort,
                                    const std::string& localIP = "", int localPort = 0);

    std::unique_ptr<DCFConnection> dcfConnection;  // Primary DCF interface
    std::unique_ptr<asio::io_context> ioContext;  // Fallback UDP context
    std::shared_ptr<asio::ip::udp::socket> udpSocket;  // Fallback UDP socket
    asio::ip::udp::endpoint remoteEndpoint;
    std::vector<std::thread> workerThreads;
    static constexpr int NUM_WORKER_THREADS = 2;
    boost::lockfree::spsc_queue<std::vector<std::uint8_t>> receiveQueue{1024};
    std::atomic<bool> initialized{false};
    std::atomic<bool> running{false};
    std::atomic<bool> usingFallback{false};  // Flag for UDP fallback
    std::mutex sendMutex;
    std::vector<std::uint8_t> receiveBuffer{65536};
};

#endif // AUTOHOST_INTERFACE_H
