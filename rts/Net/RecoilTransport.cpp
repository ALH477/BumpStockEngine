#include <dcf_sdk/dcf_plugin_manager.h>
#include <asio.hpp>
#include <memory>
#include <vector>
#include <mutex>
#include <queue>
#include <thread>
#include <condition_variable>
#include "DCFUtils.h"

class MessageQueue {
public:
    void Push(std::vector<uint8_t> data) {
        std::lock_guard<std::mutex> lock(mutex);
        queue.push(std::move(data));
        cv.notify_one();
    }
    
    bool Pop(std::vector<uint8_t>& data, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex);
        if (cv.wait_for(lock, timeout, [this] { return !queue.empty(); })) {
            data = std::move(queue.front());
            queue.pop();
            return true;
        }
        return false;
    }
    
private:
    std::mutex mutex;
    std::condition_variable cv;
    std::queue<std::vector<uint8_t>> queue;
};

struct RecoilTransport {
    std::unique_ptr<asio::io_service> service;
    std::shared_ptr<asio::ip::udp::socket> socket;
    std::unique_ptr<std::thread> serviceThread;
    std::atomic<bool> running{false};
    MessageQueue receiveQueue;
    std::mutex socketMutex;
    
    ~RecoilTransport() {
        if (running) {
            running = false;
            if (serviceThread && serviceThread->joinable()) {
                serviceThread->join();
            }
        }
    }
};

extern "C" {

void* create_plugin() {
    try {
        auto transport = std::make_unique<RecoilTransport>();
        transport->service = std::make_unique<asio::io_service>();
        return transport.release();
    } catch (const std::exception& e) {
        DCF_LOG(dcf::DCFLogLevel::ERROR, std::string("Failed to create transport: ") + e.what());
        return nullptr;
    }
}

bool setup(void* self, const char* host, int port) {
    auto* transport = static_cast<RecoilTransport*>(self);
    if (!transport) return false;
    
    try {
        transport->socket = std::make_shared<asio::ip::udp::socket>(
            *transport->service,
            asio::ip::udp::endpoint(asio::ip::udp::v4(), port)
        );
        
        transport->running = true;
        transport->serviceThread = std::make_unique<std::thread>([transport]() {
            while (transport->running) {
                try {
                    transport->service->run_one();
                } catch (const std::exception& e) {
                    DCF_LOG(dcf::DCFLogLevel::ERROR, std::string("IO service error: ") + e.what());
                }
            }
        });
        
        return true;
    } catch (const std::exception& e) {
        DCF_LOG(dcf::DCFLogLevel::ERROR, std::string("Setup failed: ") + e.what());
        return false;
    }
}

bool send(void* self, const uint8_t* data, size_t size, const char* target) {
    auto* transport = static_cast<RecoilTransport*>(self);
    if (!transport || !transport->running) return false;
    
    try {
        std::lock_guard<std::mutex> lock(transport->socketMutex);
        
        asio::ip::udp::resolver resolver(*transport->service);
        auto endpoint = *resolver.resolve(target, "8452").begin();
        
        transport->socket->async_send_to(
            asio::buffer(data, size),
            endpoint,
            [](const asio::error_code& error, std::size_t /*bytes*/) {
                if (error) {
                    DCF_LOG(dcf::DCFLogLevel::WARNING, 
                        std::string("Send error: ") + error.message());
                }
            }
        );
        
        return true;
    } catch (const std::exception& e) {
        DCF_LOG(dcf::DCFLogLevel::ERROR, std::string("Send failed: ") + e.what());
        return false;
    }
}

uint8_t* receive(void* self, size_t* size) {
    auto* transport = static_cast<RecoilTransport*>(self);
    if (!transport || !transport->running) {
        *size = 0;
        return nullptr;
    }
    
    std::vector<uint8_t> data;
    if (transport->receiveQueue.Pop(data, std::chrono::milliseconds(100))) {
        *size = data.size();
        uint8_t* result = new uint8_t[*size];
        std::memcpy(result, data.data(), *size);
        return result;
    }
    
    *size = 0;
    return nullptr;
}

void destroy(void* self) {
    if (auto* transport = static_cast<RecoilTransport*>(self)) {
        transport->running = false;
        if (transport->serviceThread && transport->serviceThread->joinable()) {
            transport->serviceThread->join();
        }
        delete transport;
    }
}

const char* get_plugin_version() {
    return "1.0.0";
}

// Export plugin interface
ITransport iface = {
    .setup = setup,
    .send = send,
    .receive = receive,
    .destroy = destroy
};

} // extern "C"
