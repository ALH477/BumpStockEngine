# BumpStockEngine – Open-Source Real-Time Strategy Game Engine

**Version 105.0.0 | August 30, 2025**  
**License:** GNU General Public License v2.0 or later (GPL-2.0+)  
**Repository:** [github.com/ALH477/BumpStockEngine](https://github.com/ALH477/BumpStockEngine)  
**Community:** [Discord](https://discord.gg/GUpRg6Wz3e)

BumpStockEngine is an advanced fork of the SpringRTS engine, continuing development from version 105.0. It enhances the original engine with modern networking capabilities, improved performance, and robust modularity, tailored for large-scale real-time strategy (RTS) games. The cornerstone of this fork is the integration of the DeMoD Communications Framework (DCF), delivering low-latency, resilient, and extensible networking for multiplayer scenarios supporting up to 160 players.

## Advanced Networking with DeMoD Communications Framework (DCF)

BumpStockEngine integrates the DeMoD Communications Framework (DCF) version 5.0.0, a free and open-source (GPL-3.0) networking solution designed for low-latency, interoperable data exchange. DCF replaces legacy UDP-based netcode with a modular, self-healing peer-to-peer (P2P) system, significantly improving performance and reliability for large-scale RTS multiplayer.

### Key Networking Features
- **Multi-Transport Compatibility**: Supports UDP, TCP, WebSocket, and gRPC through a unified layer, ensuring flexibility across network environments.
- **Self-Healing P2P Networking**: Automatically detects failures and reroutes data using redundant paths and RTT-based grouping (<50ms clusters), minimizing desyncs (`desyncHasOccurred` flag).
- **Sub-Millisecond Latency**: Handshakeless design and Protocol Buffers serialization achieve <1% overhead, ideal for real-time sync in massive battles.
- **Dynamic Role Assignment**: AUTO mode enables nodes to switch between client, server, or P2P roles under master node control, optimizing network topology dynamically.
- **Plugin Extensibility**: Custom transport plugins (e.g., `RecoilTransport` using ASIO for UDP) allow tailored networking solutions, loaded via `librecoil_transport.so`.
- **Adaptive Speed Control**: Integrates DCF metrics (e.g., `averageRTT`) into `UpdateSpeedControl` for real-time adjustment of `userSpeedFactor`, reducing lag in high-player scenarios.

### Technical Implementation
- **DCFConnection**: Implements the `CConnection` interface, managing packet transmission, queuing, and metrics (packets sent/received, bytes, RTT). Uses RAII and smart pointers for robust resource management.
- **RecoilTransport Plugin**: ASIO-based UDP transport for asynchronous sends/receives, integrated via DCF’s `ITransport` interface. Supports port 8452 with lock-free queues for thread safety.
- **GameServer Integration**: Replaces `UDPListener` with `DCFConnection` in `CGameServer`, handling `NETMSG_*` packets (e.g., `NETMSG_SYNCRESPONSE`, `NETMSG_CREATE_NEWPLAYER`) with redundancy for mid-game joins.
- **Fallback Mechanism**: Reverts to vanilla UDP if DCF initialization fails, configurable via `fallback_transport` in `dcf_network.json`.
- **Metrics and Logging**: Periodic RTT and traffic logging (`DCFUtils.h`) to `logs/recoil_dcf.log`, with customizable intervals.

## Getting Started

### Prerequisites
- **Dependencies**:
  - CMake ≥3.15
  - Ninja or compatible build system
  - Libraries: Protobuf, gRPC, nlohmann_json, ASIO, pthreads
  - DCF SDK: [github.com/ALH477/DeMoD-Communication-Framework](https://github.com/ALH477/DeMoD-Communication-Framework)
- **Supported Platforms**: Linux, Windows, macOS; Android/iOS bindings planned.
- **Hardware**: Minimum 4GB RAM, 2GHz CPU; tested on Raspberry Pi for embedded use.

### Clone the Repository
```bash
git clone --recurse-submodules https://github.com/ALH477/BumpStockEngine.git
cd BumpStockEngine
```

### Pre-Compiled Binaries
Download from [Releases](https://github.com/ALH477/BumpStockEngine/releases) for quick setup.

### Configuration
Edit `config/dcf_network.json` to customize networking:
```json
{
  "transport": "gRPC",
  "host": "0.0.0.0",
  "port": 8452,
  "mode": "auto",
  "node_id": "bumpstock_server",
  "peers": [],
  "group_rtt_threshold": 50,
  "plugins": {"transport": "librecoil_transport.so"},
  "logging": {"level": "info", "file": "logs/recoil_dcf.log", "metrics_interval": 2000},
  "fallback_transport": "udp",
  "max_players": 160,
  "network_settings": {"mtu": 1400, "reconnect_timeout": 15, "network_loss_factor": 0}
}
```
Override via environment variables (e.g., `export DCF_HOST=localhost; export DCF_PORT=8453`).

### Building
Detailed instructions: [Building Without Docker](https://github.com/ALH477/BumpStockEngine/wiki/Building-and-Developing-Without-Docker), [Docker Build Environment](https://github.com/ALH477/BumpStockEngine/wiki/Build-Environment-Docker).
```bash
cmake .
ninja
```
For upstream tracking:
```bash
git remote add upstream git@github.com:ALH477/BumpStockEngine.git
git fetch --all --tags
git checkout master
git branch -u upstream/master
```

## Using BumpStockEngine with DCF

1. **Configure Networking**: Verify `dcf_network.json` settings (transport, port, node ID). Ensure peers are auto-discovered or manually listed.
2. **Launch the Engine**: Run `./bumpstock_engine` to initialize with DCF. Monitor `logs/recoil_dcf.log` for startup status and metrics.
3. **Monitor Performance**:
   - **Logs**: View detailed metrics (RTT, packets, bytes) in `logs/recoil_dcf.log` or in-game console.
   - **Self-Healing**: DCF automatically reroutes on connection drops, logged as failover events.
   - **Sync Checks**: DCF’s RTT adjusts `SYNCCHECK_TIMEOUT` dynamically, reducing desyncs in `CheckSync()`.
4. **Extend Networking**: Customize via plugins in `plugins/`. Modify `RecoilTransport.cpp` for specific needs (e.g., WebSocket support).
5. **Test Large Lobbies**: Simulate 32-160 players using Docker (`docker-compose.yml`) to verify stability and latency.

## Contributing
We welcome contributions to enhance BumpStockEngine, especially for networking, performance, and cross-platform support (e.g., Android/iOS bindings). To contribute:
1. Fork the repository.
2. Create a feature branch (`git checkout -b feature/xyz`).
3. Add tests (Catch2 for C++, pytest for Python bindings) and format code (`clang-format` for C/C++, `black` for Python).
4. Submit a pull request using the [PR Template](docs/PR_TEMPLATE.md).
5. Discuss ideas via [GitHub Issues](https://github.com/ALH477/BumpStockEngine/issues) or [Discord](https://discord.gg/GUpRg6Wz3e).

All code is human-verified for memory safety (Valgrind) and style compliance. New DCF plugins or SDKs (e.g., Python, Rust) are encouraged, ensuring GPL compliance and RTT-based grouping.

## Documentation
- **Architecture**: See `docs/dcf_design_spec.md` for DCF details, including AUTO mode and plugin system.
- **C SDK**: `c_sdk/C-SDKreadme.markdown` for low-level integration.
- **Wiki**: [github.com/ALH477/BumpStockEngine/wiki](https://github.com/ALH477/BumpStockEngine/wiki) for build and dev guides.

## License
BumpStockEngine is licensed under GPL-2.0+ (see [LICENSE](LICENSE)). DCF components are GPL-3.0, ensuring open-source derivatives.

## Network Flow
```mermaid
graph TD
    A[BumpStockEngine GameServer] -->|NETMSG_*| B[DCFConnection]
    B -->|gRPC/UDP| C[RecoilTransport]
    C -->|P2P Redundancy| D[Client Nodes]
    D -->|RTT <50ms| E[Master Node]
    E -->|AUTO Mode| F[Role Assignment]
```

Join us in revolutionizing RTS networking! Happy coding and gaming!