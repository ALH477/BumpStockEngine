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
#include "System/Net/DCFConnection.h"  // DCF integration for BumpStockEngine

/**
 * "player" number for GameServer-generated messages
 */
#define SERVER_PLAYER 255

namespace netcode
{
	class RawPacket;
	class CConnection;
	class UDPListener;
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

class GameTeam : public TeamBase
{
public:
	GameTeam() : active(false) {}
	GameTeam& operator=(const TeamBase& base) { TeamBase::operator=(base); return *this; }

	void SetActive(bool b) { active = b; }
	bool IsActive() const { return active; }

private:
	bool active;
};

/**
 * @brief Server class for game handling
 * This class represents a gameserver. It is responsible for receiving,
 * checking and forwarding gamedata to the clients. It keeps track of the sync,
 * cpu and other stats and informs all clients about events.
 * In BumpStockEngine, this integrates the DeMoD Communications Framework (DCF)
 * for low-latency P2P networking with self-healing redundancy and RTT-based grouping.
 * Falls back to UDP if DCF initialization fails.
 */
class CGameServer
{
	friend class CCregLoadSaveHandler; // For initializing server state after load
public:
	CGameServer(
		const std::shared_ptr<const ClientSetup> newClientSetup,
		const std::shared_ptr<const GameData> newGameData,
		const std::shared_ptr<const CGameSetup> newGameSetup
	);

	CGameServer(const CGameServer&) = delete; // no-copy
	~CGameServer();

	static void Reload(const std::shared_ptr<const CGameSetup> newGameSetup);

	void AddLocalClient(const std::string& myName, const std::string& myVersion, const std::string& myPlatform);
	void AddAutohostInterface(const std::string& autohostIP, const int autohostPort);

	void Initialize();
	/**
	 * @brief Set frame after loading
	 * WARNING! No checks are done, so be careful
	 */
	void PostLoad(int serverFrameNum);

	void CreateNewFrame(bool fromServerThread, bool fixedFrameTime);

	void SetGamePausable(const bool arg);
	void SetReloading(const bool arg) { reloadingServer = arg; }

	bool PreSimFrame() const { return (serverFrameNum == -1); }
	bool HasStarted() const { return gameHasStarted; }
	bool HasGameID() const { return generatedGameID; }
	bool HasLocalClient() const { return (localClientNumber != -1u); }
	/// Is the server still running?
	bool HasFinished() const;

	void UpdateSpeedControl(int speedCtrl);
	static std::string SpeedControlToString(int speedCtrl);

	static bool IsServerCommand(const std::string& cmd) {
		const auto pred = [](const std::string& a, const std::string& b) { return (a < b); };
		const auto iter = std::lower_bound(commandBlacklist.begin(), commandBlacklist.end(), cmd, pred);
		return (iter != commandBlacklist.end() && *iter == cmd);
	}

	std::string GetPlayerNames(const std::vector<int>& indices) const;

	const std::shared_ptr<const ClientSetup> GetClientSetup() const { return myClientSetup; }
	const std::shared_ptr<const GameData> GetGameData() const { return myGameData; }
	const std::shared_ptr<const CGameSetup> GetGameSetup() const { return myGameSetup; }

	const std::vector<GameParticipant>& GetPlayers() const { return players; }
	const std::vector<GameTeam>& GetTeams() const { return teams; }
	const std::vector<unsigned char>& GetWinningAllyTeams() const { return winningAllyTeams; }

	const std::vector<uint8_t>& GetFreeSkirmishAIs() const { return freeSkirmishAIs; }

	const std::deque<std::shared_ptr<const netcode::RawPacket>>& GetPacketCache() const { return packetCache; }

#ifdef SYNCCHECK
	const std::set<int>& GetOutstandingSyncFrames() const { return outstandingSyncFrames; }
#endif

	spring_time GetServerStartTime() const { return serverStartTime; }
	spring_time GetReadyTime() const { return readyTime; }

	spring_time GetLastNewFrameTick() const { return lastNewFrameTick; }
	spring_time GetLastPlayerInfo() const { return lastPlayerInfo; }
	spring_time GetLastUpdate() const { return lastUpdate; }
	spring_time GetLastBandwidthUpdate() const { return lastBandwidthUpdate; }

	float GetModGameTime() const { return modGameTime; }
	float GetGameTime() const { return gameTime; }
	float GetStartTime() const { return startTime; }
	float GetFrameTimeLeft() const { return frameTimeLeft; }

	float GetUserSpeedFactor() const { return userSpeedFactor; }
	float GetInternalSpeed() const { return internalSpeed; }

	float GetMedianCpu() const { return medianCpu; }
	int GetMedianPing() const { return medianPing; }
	int GetCurSpeedCtrl() const { return curSpeedCtrl; }
	int GetLoopSleepTime() const { return loopSleepTime; }

	int GetServerFrameNum() const { return serverFrameNum; }

	int GetSyncErrorFrame() const { return syncErrorFrame; }
	int GetSyncWarningFrame() const { return syncWarningFrame; }
	bool GetDesyncHasOccurred() const { return desyncHasOccurred; }

	int GetLinkMinPacketSize() const { return linkMinPacketSize; }

	unsigned GetLocalClientNumber() const { return localClientNumber; }

	float GetMaxUserSpeed() const { return maxUserSpeed; }
	float GetMinUserSpeed() const { return minUserSpeed; }

	bool GetIsPaused() const { return isPaused; }
	bool GetGamePausable() const { return gamePausable; }

	bool GetCheating() const { return cheating; }
	bool GetNoHelperAIs() const { return noHelperAIs; }
	bool GetCanReconnect() const { return canReconnect; }
	bool GetAllowSpecDraw() const { return allowSpecDraw; }
	bool GetAllowSpecJoin() const { return allowSpecJoin; }
	bool GetWhiteListAdditionalPlayers() const { return whiteListAdditionalPlayers; }

	bool GetLogInfoMessages() const { return logInfoMessages; }
	bool GetLogDebugMessages() const { return logDebugMessages; }

	const std::unique_ptr<netcode::UDPListener>& GetUDPListener() const { return udpListener; }
	const std::unique_ptr<CDemoReader>& GetDemoReader() const { return demoReader; }
	const std::unique_ptr<CDemoRecorder>& GetDemoRecorder() const { return demoRecorder; }
	const std::unique_ptr<AutohostInterface>& GetHostIf() const { return hostif; }

	const CGlobalUnsyncedRNG& GetRNG() const { return rng; }
	const spring::thread& GetThread() const { return thread; }

	mutable spring::recursive_mutex& GetGameServerMutex() { return gameServerMutex; }

	std::atomic<bool> GetGameHasStarted() const { return gameHasStarted; }
	std::atomic<bool> GetGeneratedGameID() const { return generatedGameID; }
	std::atomic<bool> GetReloadingServer() const { return reloadingServer; }
	std::atomic<bool> GetQuitServer() const { return quitServer; }

	const union { unsigned char charArray[16]; unsigned int intArray[4]; }& GetGameID() const { return gameID; }

	// BumpStockEngine-specific: DCF integration
	const std::unique_ptr<DCFConnection>& GetDCFConnection() const { return dcfConnection; }

private:
	std::unique_ptr<DCFConnection> dcfConnection;  // Primary DCF networking
	std::unique_ptr<netcode::UDPListener> udpListener;  // Fallback UDP listener

	std::shared_ptr<const ClientSetup> myClientSetup;
	std::shared_ptr<const GameData> myGameData;
	std::shared_ptr<const CGameSetup> myGameSetup;

	std::vector<std::pair<bool, GameSkirmishAI>> skirmishAIs;
	std::vector<uint8_t> freeSkirmishAIs;

	std::vector<GameParticipant> players;
	std::vector<GameTeam> teams;
	std::vector<unsigned char> winningAllyTeams;

	std::array<spring_time, MAX_PLAYERS> netPingTimings; // throttles NETMSG_PING
	std::array<std::pair<spring_time, uint32_t>, MAX_PLAYERS> mapDrawTimings; // throttles NETMSG_MAPDRAW
	std::array<std::pair<bool, bool>, MAX_PLAYERS> chatMutedFlags; // blocks NETMSG_{CHAT,DRAW}
	std::array<bool, MAX_PLAYERS> aiControlFlags; // blocks NETMSG_AI_CREATED (aicontrol)

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

	int serverFrameNum = -1;

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
