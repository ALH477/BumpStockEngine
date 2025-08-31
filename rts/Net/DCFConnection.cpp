#include "DCFConnection.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <thread>
#include <algorithm>  // For std::min

using json = nlohmann::json;
using namespace std::chrono;

DCFConnection::DCFConnection(const std::string& configPath) : client(nullptr, ClientDeleter) {
    try {
        if (!ValidateConfiguration(configPath)) {
            DCF_THROW("Invalid configuration file");
        }
        InitializeClient(configPath);
        // Start multi-threaded update workers
        for (int i = 0; i < NUM_UPDATE_THREADS; ++i) {
            updateThreads.emplace_back(&DCFConnection::UpdateThreadLoop, this);
        }
        DCF_LOG(dcf::DCFLogLevel::INFO, "DCF networking initialized with multi-threading");
    } catch (const std::exception& e) {
        DCF_LOG(dcf::DCFLogLevel::FATAL, std::string("Failed to initialize DCF: ") + e.what());
        throw;
    }
}

DCFConnection::~DCFConnection() {
    Close(true);
    initialized = false;
    for (auto& th : updateThreads) {
        if (th.joinable()) {
            th.join();
        }
    }
}

bool DCFConnection::ValidateConfiguration(const std::string& configPath) {
    if (!std::filesystem::exists(configPath)) {
        DCF_LOG(dcf::DCFLogLevel::ERROR, "Configuration file not found: " + configPath);
        return false;
    }

    try {
        std::ifstream configFile(configPath);
        json config = json::parse(configFile);
        const std::vector<std::string> requiredFields = {
            "transport", "host", "port", "mode", "node_id", "group_rtt_threshold", "fallback_transport"
        };
        for (const auto& field : requiredFields) {
            if (!config.contains(field)) {
                DCF_LOG(dcf::DCFLogLevel::ERROR, "Missing required field: " + field);
                return false;
            }
        }
        rttThreshold = config["group_rtt_threshold"].get<double>();
        dcf::DCFLogger::Configure(config["logging"]["file"], static_cast<dcf::DCFLogLevel>(config["logging"]["level"].get<int>()));
        return true;
    } catch (const json::exception& e) {
        DCF_LOG(dcf::DCFLogLevel::ERROR, std::string("JSON parse error: ") + e.what());
        return false;
    }
}

void DCFConnection::InitializeClient(const std::string& configPath) {
    DCFClient* rawClient = dcf_client_new();
    if (!rawClient) {
        DCF_THROW("Failed to create DCF client");
    }
    client.reset(rawClient);

    if (dcf_client_initialize(client.get(), configPath.c_str()) != DCF_SUCCESS) {
        DCF_THROW("DCF client initialization failed");
    }
    redundancy = client->redundancy;
    if (!redundancy) {
        DCF_THROW("Failed to get redundancy manager");
    }
    if (dcf_client_start(client.get()) != DCF_SUCCESS) {
        DCF_THROW("DCF client start failed");
    }
    initialized = true;
}

std::error_code DCFConnection::SendWithRetry(const RawPacket& data, int maxRetries) {
    std::error_code ec;
    for (int attempt = 0; attempt < maxRetries; ++attempt) {
        DCFError err = dcf_client_send_message(client.get(), reinterpret_cast<const char*>(data.data), data.length, "broadcast");
        if (err == DCF_SUCCESS) {
            std::lock_guard<std::mutex> lock(metricsMutex);
            metrics.totalPacketsSent++;
            metrics.totalBytesSent += data.length;
            return ec;
        }
        if (err != DCF_SUCCESS) {
            DCF_LOG(dcf::DCFLogLevel::ERROR, "DCF SDK error code: " + std::to_string(err));
            // Explicit handling: retry or failover based on error
            if (err == DCF_TIMEOUT) {
                TriggerFailoverIfNeeded();
                ec = std::make_error_code(std::errc::timed_out);
            } else if (err == DCF_NETWORK_ERROR) {
                // Attempt reconnect
                if (NeedsReconnect()) {
                    ReconnectTo(*this);
                }
                ec = std::make_error_code(std::errc::network_down);
            } else {
                // Unknown error: abort retry
                return std::make_error_code(std::errc::io_error);
            }
        }
        metrics.failedSendAttempts++;
        DCF_LOG(dcf::DCFLogLevel::WARNING, "Send attempt " + std::to_string(attempt + 1) + " failed");
        std::this_thread::sleep_for(milliseconds(std::min(100 * (attempt + 1), 500)));  // Capped exponential backoff
    }
    return ec;
}

void DCFConnection::SendData(std::shared_ptr<const RawPacket> data) {
    if (!initialized || muted) {
        DCF_LOG(dcf::DCFLogLevel::WARNING, "Send blocked: not initialized or muted");
        return;
    }
    if (!data || data->length == 0 || data->length > 65535) {
        DCF_LOG(dcf::DCFLogLevel::ERROR, "Invalid packet data");
        return;
    }

    double rtt = metrics.averageRTT;
    if (!IsInRTTGroup(rtt)) {
        DCF_LOG(dcf::DCFLogLevel::WARNING, "High RTT (" + std::to_string(rtt) + "ms), rerouting");
        // Reroute via redundancy API if available
    }

    auto ec = SendWithRetry(*data);
    if (ec) {
        DCF_LOG(dcf::DCFLogLevel::ERROR, "Send failed: " + ec.message());
        TriggerFailoverIfNeeded();
    }
}

void DCFConnection::TriggerFailoverIfNeeded() {
    if (metrics.failedSendAttempts > 5 && redundancy) {
        dcf_redundancy_trigger_failover(redundancy);
        DCF_LOG(dcf::DCFLogLevel::INFO, "Triggered P2P failover due to high failures");
        std::lock_guard<std::mutex> lock(metricsMutex);
        metrics.failedSendAttempts = 0;
    }
}

void DCFConnection::HandleIncomingMessage(const char* data, size_t length) {
    if (!data || length == 0 || length > 65535) {
        DCF_LOG(dcf::DCFLogLevel::WARNING, "Invalid message data");
        return;
    }
    try {
        auto packet = std::make_shared<RawPacket>(reinterpret_cast<const unsigned char*>(data), length);
        while (!msgQueue.push(packet)) {
            std::this_thread::sleep_for(milliseconds(10));  // Backoff on full queue
        }
        std::lock_guard<std::mutex> lock(metricsMutex);
        metrics.totalPacketsReceived++;
        metrics.totalBytesReceived += length;
    } catch (const std::exception& e) {
        DCF_LOG(dcf::DCFLogLevel::ERROR, std::string("Handle message failed: ") + e.what());
    }
}

void DCFConnection::UpdateThreadLoop() {
    while (initialized) {
        try {
            char* data;
            size_t length;
            if (dcf_client_receive_message(client.get(), &data, &length) == DCF_SUCCESS && data) {
                HandleIncomingMessage(data, length);
                free(data);
            }
            std::this_thread::sleep_for(milliseconds(1));  // Low sleep for responsiveness
        } catch (const std::exception& e) {
            DCF_LOG(dcf::DCFLogLevel::ERROR, std::string("Update thread failed: ") + e.what());
        }
    }
}

void DCFConnection::Update() {
    if (!initialized) return;

    try {
        std::lock_guard<std::mutex> lock(metricsMutex);
        auto now = system_clock::now();
        if (duration_cast<seconds>(now - metrics.lastMetricsUpdate).count() > 2) {
            ProcessMetrics();
            LogMetrics();
            metrics.lastMetricsUpdate = now;
        }
        TriggerFailoverIfNeeded();
    } catch (const std::exception& e) {
        DCF_LOG(dcf::DCFLogLevel::ERROR, std::string("Update failed: ") + e.what());
    }
}

void DCFConnection::ProcessMetrics() {
    cJSON* metricsJson;
    if (dcf_client_get_metrics(client.get(), &metricsJson) == DCF_SUCCESS && metricsJson) {
        UpdateMetrics(metricsJson);
        cJSON_Delete(metricsJson);
    } else {
        DCF_LOG(dcf::DCFLogLevel::ERROR, "Failed to get DCF metrics from SDK");
    }
}

void DCFConnection::UpdateMetrics(const cJSON* metricsJson) {
    if (!metricsJson) return;
    try {
        cJSON* rttObj = cJSON_GetObjectItem(metricsJson, "average_rtt");
        if (rttObj && cJSON_IsNumber(rttObj)) {
            std::lock_guard<std::mutex> lock(metricsMutex);
            metrics.averageRTT = rttObj->valuedouble;
        }
    } catch (const std::exception& e) {
        DCF_LOG(dcf::DCFLogLevel::ERROR, std::string("Update metrics failed: ") + e.what());
    }
}

void DCFConnection::LogMetrics() const {
    std::stringstream ss;
    ss << "DCF Metrics: {"
       << "\"packets_sent\":" << metrics.totalPacketsSent << ","
       << "\"packets_received\":" << metrics.totalPacketsReceived << ","
       << "\"bytes_sent\":" << metrics.totalBytesSent << ","
       << "\"bytes_received\":" << metrics.totalBytesReceived << ","
       << "\"failed_attempts\":" << metrics.failedSendAttempts << ","
       << "\"average_rtt_ms\":" << metrics.averageRTT << "}";
    DCF_LOG(dcf::DCFLogLevel::INFO, ss.str());
}

bool DCFConnection::HasIncomingData() const {
    return !msgQueue.empty();
}

std::shared_ptr<const RawPacket> DCFConnection::Peek(unsigned ahead) const {
    std::shared_ptr<const RawPacket> pkt;
    if (ahead >= msgQueue.capacity()) {
        DCF_LOG(dcf::DCFLogLevel::DEBUG, "Peek out of bounds");
        return nullptr;
    }
    // Simulate peek on lock-free queue
    if (msgQueue.pop(pkt)) {
        msgQueue.push(pkt);  // Re-push
        return pkt;
    }
    return nullptr;
}

std::shared_ptr<const RawPacket> DCFConnection::GetData() {
    std::shared_ptr<const RawPacket> pkt;
    if (msgQueue.pop(pkt)) {
        return pkt;
    }
    return nullptr;
}

void DCFConnection::DeleteBufferPacketAt(unsigned index) {
    DCF_LOG(dcf::DCFLogLevel::DEBUG, "DeleteBufferPacketAt not directly supported; resetting queue");
    msgQueue.reset();
}

void DCFConnection::Flush(const bool forced) {
    if (forced && client) {
        dcf_client_flush(client.get());
    }
}

bool DCFConnection::CheckTimeout(int seconds, bool initial) const {
    auto now = system_clock::now();
    return (duration_cast<seconds>(now - metrics.lastMetricsUpdate).count() > seconds);
}

void DCFConnection::ReconnectTo(CConnection& conn) {
    if (CanReconnect()) {
        DCF_LOG(dcf::DCFLogLevel::INFO, "Reconnecting DCF client");
        // Reinitialize client
    }
}

bool DCFConnection::CanReconnect() const { return true; }

bool DCFConnection::NeedsReconnect() { return metrics.failedSendAttempts > 10; }

unsigned int DCFConnection::GetPacketQueueSize() const { return msgQueue.read_available(); }

std::string DCFConnection::Statistics() const {
    std::lock_guard<std::mutex> lock(metricsMutex);
    std::stringstream ss;
    ss << "DCF Statistics: {"
       << "\"initialized\":" << (initialized ? "true" : "false") << ","
       << "\"muted\":" << (muted ? "true" : "false") << ","
       << "\"loss_factor\":" << lossFactor << ","
       << "\"total_packets_sent\":" << metrics.totalPacketsSent << ","
       << "\"total_packets_received\":" << metrics.totalPacketsReceived << ","
       << "\"total_bytes_sent\":" << metrics.totalBytesSent << ","
       << "\"total_bytes_received\":" << metrics.totalBytesReceived << ","
       << "\"failed_send_attempts\":" << metrics.failedSendAttempts << ","
       << "\"average_rtt_ms\":" << metrics.averageRTT << "}";
    return ss.str();
}

std::string DCFConnection::GetFullAddress() const {
    return "dcf://" + std::string(client ? client->host : "unknown") + ":" + std::to_string(client ? client->port : 0);
}

void DCFConnection::Unmute() { muted = false; }

void DCFConnection::Close(bool flush) {
    if (flush) Flush(true);
    initialized = false;
    if (client) {
        dcf_client_stop(client.get());
    }
    DCF_LOG(dcf::DCFLogLevel::INFO, "DCF connection closed");
}

void DCFConnection::SetLossFactor(int factor) { lossFactor = factor; }
