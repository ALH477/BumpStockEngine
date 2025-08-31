#include <dcf_sdk/dcf_plugin_manager.h>
#include <asio.hpp>
#include <memory>
#include <vector>
#include <thread>
#include <condition_variable>
#include <boost/lockfree/spsc_queue.hpp>
#include "DCFUtils.h"

/**
 * @class MessageQueue
 * @brief Lock-free queue for messages.
 */
class MessageQueue {
public:
    void Push(std::vector<uint8_t> data) {
        while (!queue.push(data)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    
    bool Pop(std::vector<uint8_t>& data, std::chrono::milliseconds timeout) {
        return queue.pop(data);
    }
    
private:
    boost::lockfree::spsc_queue<std::vector<uint8_t>> queue{1024};
};

struct BumpStockTransport {
    std::unique_ptr<asio::io_context> context;  // Changed to io_context for multi-threading
    std::shared_ptr<asio::ip::udp::socket> socket;
    std::vector<std::thread> workerThreads;
    std::atomic<bool> running{false};
    MessageQueue receiveQueue;
    
    ~BumpStockTransport() {
        if (running) {
            running = false;
            context->stop();
            for (auto& th : workerThreads) {
                if (th.joinable()) th.join();
            }
        }
    }
};

extern "C" {

void* create_plugin() {
    try {
        auto transport = std::make_unique<BumpStockTransport>();
        transport->context = std::make_unique<asio::io_context>();
        return transport.release();
    } catch (const std::exception& e) {
        DCF_LOG(dcf::DCFLogLevel::ERROR, std::string("Failed to create transport: ") + e.what());
        return nullptr;
    }
}

bool setup(void* self, const char* host, int port) {
    auto* transport = static_cast<BumpStockTransport*>(self);
    if (!transport) return false;
    
    try {
        transport->socket = std::make_shared<asio::ip::udp::socket>(
            *transport->context,
            asio::ip::udp::endpoint(asio::ip::udp::v4(), port)
        );
        transport->running = true;
        // Multi-threaded workers
        for (int i = 0; i < 4; ++i) {  // 4 threads for IO
            transport->workerThreads.emplace_back([transport]() {
                while (transport->running) {
                    try {
                        transport->context->run_one();
                    } catch (const std::exception& e) {
                        DCF_LOG(dcf::DCFLogLevel::ERROR, std::string("IO worker error: ") + e.what());
                    }
                }
            });
        }
        DCF_LOG(dcf::DCFLogLevel::INFO, "Transport setup with multi-threading on port " + std::to_string(port));
        return true;
    } catch (const std::exception& e) {
        DCF_LOG(dcf::DCFLogLevel::ERROR, std::string("Setup failed: ") + e.what());
        return false;
    }
}

bool send(void* self, const uint8_t* data, size_t size, const char* target) {
    auto* transport = static_cast<BumpStockTransport*>(self);
    if (!transport || !transport->running || size == 0 || size > 65535) return false;
    
    try {
        asio::ip::udp::resolver resolver(*transport->context);
        auto endpoint = *resolver.resolve(target, "8452").begin();
        transport->socket->async_send_to(
            asio::buffer(data, size),
            endpoint,
            [](const asio::error_code& error, std::size_t) {
                if (error) {
                    DCF_LOG(dcf::DCFLogLevel::WARNING, std::string("Send error: ") + error.message());
                }
            }
        );
        return true;
    } catch (const std::exception& e) {
        DCF_LOG(dcf::DCFLogLevel::ERROR, std::string("Send failed: ") + e.what());
        return false;
    }
}

// Changed to return std::vector<uint8_t> to avoid manual new/delete
std::vector<uint8_t> receive(void* self, size_t* size) {
    auto* transport = static_cast<BumpStockTransport*>(self);
    if (!transport || !transport->running) {
        *size = 0;
        return {};
    }
    
    std::vector<uint8_t> data;
    if (transport->receiveQueue.Pop(data, std::chrono::milliseconds(100))) {
        *size = data.size();
        return data;  // Return vector; no manual allocation
    }
    
    *size = 0;
    return {};
}

void destroy(void* self) {
    if (auto* transport = static_cast<BumpStockTransport*>(self)) {
        transport->running = false;
        transport->context->stop();
        for (auto& th : transport->workerThreads) {
            if (th.joinable()) th.join();
        }
        delete transport;
        DCF_LOG(dcf::DCFLogLevel::INFO, "Transport destroyed");
    }
}

const char* get_plugin_version() { return "1.0.0"; }

ITransport iface = {
    .setup = setup,
    .send = send,
    .receive = receive,  // Updated signature if needed; assume compatible
    .destroy = destroy
};

} // extern "C"
