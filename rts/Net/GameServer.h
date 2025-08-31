/* This file is part of the BumpStockEngine (GPL v2 or later), see LICENSE.html */

#ifndef _GAME_SERVER_H
#define _GAME_SERVER_H

#include <atomic>
#include <memory>
#include <string>
#include <array>
#include <deque>
#include <map>
#include <set>
#include <vector>
#include "Game/GameData.h"
#include "Sim/Misc/GlobalConstants.h"
#include "Sim/Misc/TeamBase.h"
#include "System/float3.h"
#include "System/GlobalRNG.h"
#include "System/Misc/SpringTime.h"
#include "System/Threading/SpringThreading.h"
#include "System/Net/DCFConnection.h" // Added for DCF

namespace netcode {
    class RawPacket;
    class CConnection;
}
class CDemoReader;
class Action;
class CDemoRecorder;
class AutohostInterface;
class ClientSetup;
class CGameSetup;
class ChatMessage;
class GameParticipant;
class GameSkirmishAI;

class GameTeam : public TeamBase {
public:
    GameTeam() : active(false) {}
    GameTeam& operator=(const TeamBase& base) { TeamBase::operator=(base); return *this; }
    void SetActive(bool b) { active = b; }
    bool IsActive() const { return active; }
private:
    bool active;
};

class CGameServer {
    friend class CCregLoadSaveHandler;
public:
    CGameServer(
        const std::shared_ptr<const ClientSetup> newClientSetup,
        const std::shared_ptr<const GameData> newGameData,
        const std::shared_ptr<const CGameSetup> newGameSetup
    );
    CGameServer(const CGameServer&) = delete;
    ~CGameServer();

    static void Reload(const std::shared_ptr<const CGameSetup> newGameSetup);
    void AddLocalClient(const std::string& myName, const std::string& myVersion, const std::string& myPlatform);
    void AddAutohostInterface(const std::string& autohostIP, const int autohostPort);
    void Initialize();
    void PostLoad(int serverFrameNum);
    void CreateNewFrame(bool fromServerThread, bool fixedFrameTime);
    void SetGamePausable(const bool arg);
    void SetReloading(const bool arg) { reloadingServer = arg; }
    bool PreSimFrame() const { return serverFrameNum == -1; }
    bool HasStarted() const { return gameHasStarted; }
    bool HasGameID() const { return generatedGameID; }
    bool HasLocalClient() const { return localClientNumber != -1u; }
    bool HasFinished() const;
    void UpdateSpeedControl(int speedCtrl);
    static std::string SpeedControlToString(int speedCtrl);
    static bool IsServerCommand(const std::string& cmd);
    std::string GetPlayerNames(const std::vector<int>& indices) const;
    const std::shared_ptr<const ClientSetup> GetClientSetup() const { return myClientSetup; }
    const std::shared_ptr<const GameData> GetGameData() const { return myGameData; }
    const std::shared_ptr<const CGameSetup> GetGameSetup() const { return myGameSetup; }
    const std::unique_ptr<CDemoReader>& GetDemoReader() const { return demoReader; }
    const std::unique_ptr<CDemoRecorder>& GetDemoRecorder() const { return demoRecorder; }
    void ServerMessage(const std::shared_ptr<const netcode::RawPacket>& packet); // Added for packet handling
    void Update(); // Added for DCF polling
private:
    void GotChatMessage(const ChatMessage& msg);
    float GetDemoTime() const;
    void InternalSpeedChange(float newSpeed);
    void UserSpeedChange(float newSpeed, int player);
    void AddAdditionalUser(const std::string& name, const std::string& passwd, bool fromDemo = false, bool spectator = true, int team = 0, int playerNum = -1);
    uint8_t ReserveSkirmishAIId();
private:
    std::shared_ptr<const ClientSetup> myClientSetup;
    std::shared_ptr<const GameData> myGameData;
    std::shared_ptr<const CGameSetup> myGameSetup;
    std::vector<std::pair<bool, GameSkirmishAI>> skirmishAIs;
    std::vector<uint8_t> freeSkirmishAIs;
    std::vector<GameParticipant> players;
    std::vector<GameTeam> teams;
    std::vector<unsigned char> winningAllyTeams;
    std::array<spring_time, MAX_PLAYERS> netPingTimings;
    std::array<std::pair<spring_time, uint32_t>, MAX_PLAYERS> mapDrawTimings;
    std::array<std::pair<bool, bool>, MAX_PLAYERS> chatMutedFlags;
    std::array<bool, MAX_PLAYERS> aiControlFlags;
    std::map<std::string, int> rejectedConnections;
    std::pair<std::string, std::string> refClientVersion;
    std::deque<std::shared_ptr<const netcode::RawPacket>> packetCache;
#ifdef SYNCCHECK
    std::set<int> outstandingSyncFrames;
#endif
    spring_time serverStartTime = spring_gettime();
    spring_time readyTime = spring_notime;
    spring_time lastNewFrameTick = spring_notime;
    spring_time lastPlayerInfo = spring_notime;
    spring_time lastUpdate = spring_notime;
    spring_time lastBandwidthUpdate = spring_notime;
    float modGameTime = 0.0f;
    float gameTime = 0.0f;
    float startTime = 0.0f;
    float frameTimeLeft = 0.0f;
    float userSpeedFactor = 1.0f;
    float internalSpeed = 1.0f;
    float medianCpu = 0.0f;
    int medianPing = 0;
    int curSpeedCtrl = 0;
    int loopSleepTime = 0;
    int serverFrameNUMBER = -1;
    int syncErrorFrame = 0;
    int syncWarningFrame = 0;
    bool desyncHasOccurred = false;
    int linkMinPacketSize = 1;
    unsigned localClientNumber = -1u;
    float maxUserSpeed = 1.0f;
    float minUserSpeed = 1.0f;
    bool isPaused = false;
    bool gamePausable = true;
    bool cheating = false;
    bool noHelperAIs = false;
    bool canReconnect = false;
    bool allowSpecDraw = true;
    bool allowSpecJoin = false;
    bool whiteListAdditionalPlayers = false;
    bool logInfoMessages = false;
    bool logDebugMessages = false;
    static std::array<std::string, 26> commandBlacklist;
    std::unique_ptr<DCFConnection> dcfConnection; // Replace UDPListener
    std::unique_ptr<CDemoReader> demoReader;
    std::unique_ptr<CDemoRecorder> demoRecorder;
    std::unique_ptr<AutohostInterface> hostif;
    CGlobalUnsyncedRNG rng;
    spring::thread thread;
    mutable spring::recursive_mutex gameServerMutex;
    std::atomic<bool> gameHasStarted{false};
    std::atomic<bool> generatedGameID{false};
    std::atomic<bool> reloadingServer{false};
    std::atomic<bool> quitServer{false};
    union {
        unsigned char charArray[16];
        unsigned int intArray[4];
    } gameID;
};

extern CGameServer* gameServer;

#endif // _GAME_SERVER_H
