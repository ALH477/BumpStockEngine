#ifndef _DCF_CONNECTION_H
#define _DCF_CONNECTION_H

#include <memory>
#include <string>
#include <vector>
#include <deque>
#include <mutex>
#include <atomic>

#include "Connection.h"
#include "RawPacket.h"
#include "DCFUtils.h"
#include <dcf_sdk/dcf_client.h>
#include <dcf_sdk/dcf_redundancy.h>

class DCFConnection : public CConnection {
public:
    explicit DCFConnection(const std::string& configPath = "config/dcf_network.json");
    ~DCFConnection();

    // Deleted copy constructor and assignment operator
    DCFConnection(const DCFConnection&) = delete;
    DCFConnection& operator=(const DCFConnection&) = delete;

    // CConnection interface implementation
    void SendData(std::shared_ptr<const RawPacket> data) override;
    bool HasIncomingData() const override;
    std::shared_ptr<const RawPacket> Peek(unsigned ahead) const override;
    std::shared_ptr<const RawPacket> GetData() override;
    void DeleteBufferPacketAt(unsigned index) override;
    void Flush(const bool forced = false) override;
    bool CheckTimeout(int seconds = 0, bool initial = false) const override;
    void ReconnectTo(CConnection& conn) override;
    bool CanReconnect() const override;
    bool NeedsReconnect() override;
    unsigned int GetPacketQueueSize() const override;
    std::string Statistics() const override;
    std::string GetFullAddress() const override;
    void Update() override;
    void Unmute() override;
    void Close(bool flush = false) override;
    void SetLossFactor(int factor) override;

private:
    struct Metrics {
        uint64_t totalPacketsSent{0};
        uint64_t totalPacketsReceived{0};
        uint64_t totalBytesReceived{0};
        uint64_t totalBytesSent{0};
        uint64_t failedSendAttempts{0};
        double averageRTT{0.0};
        std::chrono::system_clock::time_point lastMetricsUpdate;
    };

    std::unique_ptr<DCFClient, void(*)(DCFClient*)> client;
    DCFRedundancy* redundancy;  // Owned by client
    mutable std::mutex msgQueueMutex;
    std::deque<std::shared_ptr<const RawPacket>> msgQueue;
    std::atomic<bool> initialized{false};
    std::atomic<bool> muted{true};
    std::atomic<int> lossFactor{0};
    mutable std::mutex metricsMutex;
    Metrics metrics;
    
    void HandleIncomingMessage(const char* data, size_t length);
    void ProcessMetrics();
    void UpdateMetrics(const cJSON* metricsJson);
    bool ValidateConfiguration(const std::string& configPath);
    void InitializeClient(const std::string& configPath);
    void LogMetrics() const;

    static void ClientDeleter(DCFClient* client) {
        if (client) {
            dcf_client_stop(client);
            dcf_client_free(client);
        }
    }
};

#endif // _DCF_CONNECTION_H
