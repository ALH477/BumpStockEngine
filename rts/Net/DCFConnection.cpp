#include "DCFConnection.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

using json = nlohmann::json;
using namespace std::chrono;

DCFConnection::DCFConnection(const std::string& configPath) 
    : client(nullptr, ClientDeleter)
{
    try {
        if (!ValidateConfiguration(configPath)) {
            DCF_THROW("Invalid configuration file");
        }
        
        InitializeClient(configPath);
        DCF_LOG(dcf::DCFLogLevel::INFO, "DCF networking initialized successfully");
        
    } catch (const std::exception& e) {
        DCF_LOG(dcf::DCFLogLevel::FATAL, std::string("Failed to initialize DCF: ") + e.what());
        throw;
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

        // Validate required fields
        const std::vector<std::string> requiredFields = {
            "transport", "host", "port", "mode", "node_id"
        };

        for (const auto& field : requiredFields) {
            if (!config.contains(field)) {
                DCF_LOG(dcf::DCFLogLevel::ERROR, "Missing required field in config: " + field);
                return false;
            }
        }

        return true;
    } catch (const json::exception& e) {
        DCF_LOG(dcf::DCFLogLevel::ERROR, std::string("JSON parsing error: ") + e.what());
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

void DCFConnection::SendData(std::shared_ptr<const RawPacket> data) {
    if (!initialized || muted) {
        DCF_LOG(dcf::DCFLogLevel::WARNING, "Attempted to send data while not initialized or muted");
        return;
    }
    
    try {
        DCFError err = dcf_client_send_message(
            client.get(), 
            reinterpret_cast<const char*>(data->data), 
            data->length, 
            "broadcast"
        );
        
        {
            std::lock_guard<std::mutex> lock(metricsMutex);
            if (err == DCF_SUCCESS) {
                metrics.totalPacketsSent++;
                metrics.totalBytesSent += data->length;
            } else {
                metrics.failedSendAttempts++;
                DCF_LOG(dcf::DCFLogLevel::WARNING, "Failed to send message through DCF");
            }
        }
        
    } catch (const std::exception& e) {
        DCF_LOG(dcf::DCFLogLevel::ERROR, std::string("Exception during send: ") + e.what());
    }
}

bool DCFConnection::HasIncomingData() const {
    std::lock_guard<std::mutex> lock(msgQueueMutex);
    return !msgQueue.empty();
}

std::shared_ptr<const RawPacket> DCFConnection::Peek(unsigned ahead) const {
    std::lock_guard<std::mutex> lock(msgQueueMutex);
    if (ahead >= msgQueue.size()) return nullptr;
    return msgQueue[ahead];
}

std::shared_ptr<const RawPacket> DCFConnection::GetData() {
    std::lock_guard<std::mutex> lock(msgQueueMutex);
    if (msgQueue.empty()) return nullptr;
    
    auto packet = msgQueue.front();
    msgQueue.pop_front();
    return packet;
}

void DCFConnection::HandleIncomingMessage(const char* data, size_t length) {
    if (!data || length == 0) {
        DCF_LOG(dcf::DCFLogLevel::WARNING, "Received invalid message data");
        return;
    }

    try {
        auto packet = std::make_shared<RawPacket>(length);
        memcpy(packet->data, data, length);

        {
            std::lock_guard<std::mutex> lock(msgQueueMutex);
            msgQueue.push_back(packet);
        }

        {
            std::lock_guard<std::mutex> metricsLock(metricsMutex);
            metrics.totalPacketsReceived++;
            metrics.totalBytesReceived += length;
        }

    } catch (const std::exception& e) {
        DCF_LOG(dcf::DCFLogLevel::ERROR, std::string("Failed to handle incoming message: ") + e.what());
    }
}

void DCFConnection::Update() {
    if (!initialized) return;
    
    try {
        // Process incoming messages
        char* data;
        size_t length;
        while (dcf_client_receive_message(client.get(), &data, &length) == DCF_SUCCESS) {
            if (data) {
                HandleIncomingMessage(data, length);
                free(data);
            }
        }
        
        // Update metrics periodically
        {
            std::lock_guard<std::mutex> lock(metricsMutex);
            auto now = system_clock::now();
            if (now - metrics.lastMetricsUpdate > seconds(5)) {
                ProcessMetrics();
                LogMetrics();
                metrics.lastMetricsUpdate = now;
            }
        }
        
    } catch (const std::exception& e) {
        DCF_LOG(dcf::DCFLogLevel::ERROR, std::string("Exception during update: ") + e.what());
    }
}

void DCFConnection::ProcessMetrics() {
    cJSON* metricsJson;
    if (dcf_client_get_metrics(client.get(), &metricsJson) == DCF_SUCCESS) {
        if (metricsJson) {
            UpdateMetrics(metricsJson);
            cJSON_Delete(metricsJson);
        }
    }
}

void DCFConnection::UpdateMetrics(const cJSON* metricsJson) {
    if (!metricsJson) return;

    try {
        // Extract RTT information
        cJSON* rttObj = cJSON_GetObjectItem(metricsJson, "average_rtt");
        if (rttObj && cJSON_IsNumber(rttObj)) {
            std::lock_guard<std::mutex> lock(metricsMutex);
            metrics.averageRTT = rttObj->valuedouble;
        }
        
    } catch (const std::exception& e) {
        DCF_LOG(dcf::DCFLogLevel::ERROR, std::string("Failed to update metrics: ") + e.what());
    }
}

void DCFConnection::LogMetrics() const {
    std::stringstream ss;
    ss << "DCF Metrics:\n"
       << "Packets Sent: " << metrics.totalPacketsSent << "\n"
       << "Packets Received: " << metrics.totalPacketsReceived << "\n"
       << "Bytes Sent: " << metrics.totalBytesSent << "\n"
       << "Bytes Received: " << metrics.totalBytesReceived << "\n"
       << "Failed Send Attempts: " << metrics.failedSendAttempts << "\n"
       << "Average RTT: " << metrics.averageRTT << "ms";
    
    DCF_LOG(dcf::DCFLogLevel::INFO, ss.str());
}

std::string DCFConnection::Statistics() const {
    std::lock_guard<std::mutex> lock(metricsMutex);
    std::stringstream ss;
    ss << "DCF Statistics:\n"
       << "Initialized: " << (initialized ? "Yes" : "No") << "\n"
       << "Muted: " << (muted ? "Yes" : "No") << "\n"
       << "Loss Factor: " << lossFactor << "\n"
       << "Total Packets Sent: " << metrics.totalPacketsSent << "\n"
       << "Total Packets Received: " << metrics.totalPacketsReceived << "\n"
       << "Total Bytes Sent: " << metrics.totalBytesSent << "\n"
       << "Total Bytes Received: " << metrics.totalBytesReceived << "\n"
       << "Failed Send Attempts: " << metrics.failedSendAttempts << "\n"
       << "Average RTT: " << metrics.averageRTT << "ms";
    return ss.str();
}

// [Previous utility methods remain the same]
