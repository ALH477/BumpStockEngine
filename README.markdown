# Recoil Engine – Open Source Real Time Strategy Game Engine

Visit the [Official Website](https://beyond-all-reason.github.io/RecoilEngine/) for news, documentation, and more.

Recoil Engine is a fork and continuation of an RTS engine (originally SpringRTS) now enhanced with modern networking features and improved performance. Our development continues version 105.0 and beyond.

---

## Advanced Networking with DeMoD Communications Framework (DCF)

This version of Recoil Engine integrates the DeMoD Communications Framework (DCF) into its networking layer. DCF introduces major improvements to network performance, reliability, and flexibility. Here’s what you need to know:

### Key Enhancements:
- **Multi-Transport Support:**  
  DCF supports UDP, TCP, WebSocket, and gRPC in a unified compatibility layer. This means a more robust and adaptable network backend for Recoil.

- **Self-Healing Peer-to-Peer Networking:**  
  With built-in redundancy and RTT-based grouping, the network automatically detects failures and reroutes data. This ensures a stable gaming experience even when connection quality changes dynamically.

- **Low Latency & Dynamic Role Assignment:**  
  The handshakeless design and sub-millisecond exchanges minimize latency. In AUTO mode, nodes can dynamically switch roles (client, server, or P2P) based on real-time network metrics and master node assignments.

- **Efficient Serialization & Plugin Extensibility:**  
  DCF leverages Protocol Buffers for efficient data serialization. Moreover, the framework includes a plugin system for custom transports (such as our RecoilTransport plugin) so that you can tweak and optimize the networking to fit your needs.

### How It Works:
- **Configuration:**  
  Edit the provided configuration file (`config/dcf_network.json`) to set up your transport, node ID, listening port, and other relevant settings. This file controls both the engine's network behavior and logging/metrics collection.

- **Client Integration:**  
  The new networking module is built on top of DCF. The `DCFConnection` class implements Recoil’s `CConnection` interface, handling data transmission, message queuing, and metrics logging. The client’s lifecycle is managed via RAII and smart pointers to ensure robust error handling and garbage collection.

- **Custom Transport Plugin:**  
  If you need custom network behavior, the RecoilTransport plugin (based on ASIO) is provided as a module. It handles asynchronous UDP operations and integrates into DCF via a standardized plugin interface.

---

## Getting Started with Recoil Engine and DCF

### Clone the Repository

```bash
git clone https://github.com/beyond-all-reason/RecoilEngine --recursive
```

Ensure that you include the DCF submodule and any related dependencies.

### Pre-Compiled Binaries

For those who prefer not to compile, check out our releases page:

* <https://github.com/beyond-all-reason/RecoilEngine/releases>

### Installation Requirements

- **Dependencies:**  
  * CMake (≥3.15)  
  * Ninja (or your preferred build system)  
  * Protobuf, gRPC, Asio, and nlohmann_json libraries  
  * DCF dependencies (see [DeMoD Communications Framework](https://github.com/ALH477/DeMoD-Communication-Framework))
  
- **Configuration:**  
  Before starting the engine, review the DCF configuration file located at `config/dcf_network.json`. Adjust transport, port, node identifier, and logging preferences as needed.

Example configuration:
```json
{
  "transport": "gRPC",
  "host": "0.0.0.0",
  "port": 8452,
  "mode": "auto",
  "node_id": "recoil_node",
  "peers": [],
  "group_rtt_threshold": 50,
  "plugins": {
    "transport": "librecoil_transport.so"
  },
  "logging": {
    "level": "info",
    "file": "logs/dcf_network.log",
    "metrics_interval": 5000
  },
  "network_settings": {
    "mtu": 1400,
    "reconnect_timeout": 15,
    "network_loss_factor": 0
  }
}
```

---

## Compiling the Engine

Detailed instructions for compiling Recoil Engine can be found in the [Building and Developing Engine Without Docker](https://github.com/beyond-all-reason/RecoilEngine/wiki/Building-and-developing-engine-without-docker) and [SpringRTS Build Environment (Docker)](https://github.com/beyond-all-reason/RecoilEngine/wiki/SpringRTS-Build-Environment-(Docker)) wiki pages.

**Quick Build Commands:**

```bash
cmake .
ninja
```

If you have issues with tags or branches, ensure your local repository is configured to track upstream:

```bash
git remote add upstream git@github.com:beyond-all-reason/RecoilEngine.git
git fetch --all --tags
git checkout master
git branch -u upstream/master
```

---

## How to Use This Version of Recoil Engine with DCF Integration

1. **Network Configuration:**  
   Make sure your `config/dcf_network.json` is set up correctly before running the engine, adjusting parameters such as transport type, port, node ID, and logging settings.

2. **Launching the Engine:**  
   When you run the engine, it will initialize using the DCFConnection class, thereby enabling all the advanced networking features. Monitor the console and log files (e.g., `logs/dcf_network.log`) to verify that the network is behaving as expected.

3. **Monitoring & Debugging:**  
   - **Logging:** Detailed logs including performance metrics, RTT measurements, and error messages are available via the logging system in DCFUtils. Use these logs to troubleshoot network issues.
   - **Metrics:** The DCFConnection class periodically logs network metrics. You can view these metrics via the in-engine console or by checking the log file.
   - **Self-Healing:** The integration is designed to automatically handle connection drops and perform failover. If you experience network hiccups, the metrics and logs will provide insight into how the engine recovers.

4. **Customization and Extensions:**  
   Developers can extend or modify the networking behavior through the provided plugin system. The RecoilTransport plugin is an example of how to implement a custom transport module. Feel free to adapt and extend this plugin to better suit specific networking environments or performance needs.

5. **Community & Contributions:**  
   Join our [Discord](https://discord.gg/GUpRg6Wz3e) community to share experiences, seek help, and contribute ideas. Contributions that improve networking performance or add new transport plugins are highly encouraged.

---

## License

The engine is released under the terms documented in the [LICENSE](LICENSE).

---

Happy gaming and coding!
