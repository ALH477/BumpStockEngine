/* This file is part of the BumpStockEngine (GPL v2 or later), see LICENSE.html */

/**
 * @file GameServer.cpp
 * @brief Server-side game logic and networking for BumpStockEngine with DeMoD Communication Framework (DCF) integration.
 * Replaces legacy UDP with DCF for low-latency P2P, self-healing redundancy, and RTT-based grouping.
 * Falls back to UDP if DCF initialization fails.
 */

#include "GameServer.h"

#include "GameParticipant.h"
#include "GameSkirmishAI.h"
#include "AutohostInterface.h"
#include "Game/ClientSetup.h"
#include "Game/GameSetup.h"
#include "Game/Action.h"
#include "Game/ChatMessage.h"
#include "Game/CommandMessage.h"
#include "Game/GlobalUnsynced.h"
#ifndef DEDICATED
#include "Game/IVideoCapturing.h"
#endif
#include "Game/Players/Player.h"
#include "Game/Players/PlayerHandler.h"
#include "Net/Protocol/BaseNetProtocol.h"
#include "System/CRC.h"
#include "System/GlobalConfig.h"
#include "System/MsgStrings.h"
#include "System/SpringMath.h"
#include "System/SpringExitCode.h"
#include "System/SpringFormat.h"
#include "System/TdfParser.h"
#include "System/StringHash.h"
#include "System/StringUtil.h"
#include "System/Config/ConfigHandler.h"
#include "System/FileSystem/SimpleParser.h"
#include "System/Net/Connection.h"
#include "System/Net/LocalConnection.h"
#include "System/Net/UnpackPacket.h"
#include "System/LoadSave/DemoRecorder.h"
#include "System/LoadSave/DemoReader.h"
#include "System/Log/ILog.h"
#include "System/Platform/errorhandler.h"
#include "System/Platform/Threading.h"
#include "System/Threading/SpringThreading.h"
#include "System/Net/DCFConnection.h"
#include "System/Net/UDPListener.h"
#include "System/Net/UDPConnection.h"
#ifndef DEDICATED
#include "lib/luasocket/src/restrictions.h"
#endif

#ifdef SYNCCHECK
#include <map>
#endif

#define ALLOW_DEMO_GODMODE

// Logging section
#define LOG_SECTION_GAMESERVER "GameServer"
#ifdef LOG_SECTION_CURRENT
	#undef LOG_SECTION_CURRENT
#endif
#define LOG_SECTION_CURRENT LOG_SECTION_GAMESERVER
LOG_REGISTER_SECTION_GLOBAL(LOG_SECTION_GAMESERVER)

// Configuration
CONFIG(int, AutohostPort).defaultValue(0).description("Port for autohost interface connections.");
CONFIG(int, ServerSleepTime).defaultValue(5).description("Milliseconds to sleep per server tick.");
CONFIG(int, SpeedControl).defaultValue(1).minimumValue(1).maximumValue(2).description("1: use average player load, 2: use highest load.");
CONFIG(bool, AllowSpectatorJoin).defaultValue(true).dedicatedValue(false).description("Allow unauthenticated spectator joins with ~ prefix.");
CONFIG(bool, WhiteListAdditionalPlayers).defaultValue(true);
CONFIG(bool, ServerRecordDemos).defaultValue(false).dedicatedValue(true);
CONFIG(bool, ServerLogInfoMessages).defaultValue(false);
CONFIG(bool, ServerLogDebugMessages).defaultValue(false);
CONFIG(std::string, AutohostIP).defaultValue("127.0.0.1");

// Constants
static constexpr unsigned SYNCCHECK_TIMEOUT = 300;
static constexpr unsigned SYNCCHECK_MSG_TIMEOUT = 400;
static constexpr int serverKeyframeInterval = 16;

// Global instance
CGameServer* gameServer = nullptr;

// Command blacklist
std::array<std::string, 26> CGameServer::commandBlacklist = {{
	"nohelp", "say", "setgrass", "settrees", "skip", "cheat", "godmode", "globallos",
	"nocost", "nopause", "noshare", "nospecdraw", "nospecjoin", "team", "spectator",
	"specteam", "joinas", "ai", "atm", "take", "take2", "reloadcob", "reloadcegs",
	"devlua", "editdefs", "luarules", "luagaia"
}};

CGameServer::CGameServer(
	const std::shared_ptr<const ClientSetup> newClientSetup,
	const std::shared_ptr<const GameData> newGameData,
	const std::shared_ptr<const CGameSetup> newGameSetup
)
	: myClientSetup(newClientSetup)
	, myGameData(newGameData)
	, myGameSetup(newGameSetup)
	, players(MAX_PLAYERS)
	, teams(MAX_TEAMS)
	, freeSkirmishAIs(MAX_AIS)
	, winningAllyTeams(MAX_TEAMS)
	, netPingTimings{}
	, mapDrawTimings{}
	, chatMutedFlags{}
	, aiControlFlags{}
	, rejectedConnections{}
	, refClientVersion{"", ""}
	, packetCache{}
#ifdef SYNCCHECK
	, outstandingSyncFrames{}
#endif
	, serverStartTime(spring_gettime())
	, readyTime(spring_notime)
	, lastNewFrameTick(spring_notime)
	, lastPlayerInfo(spring_notime)
	, lastUpdate(spring_notime)
	, lastBandwidthUpdate(spring_notime)
	, modGameTime(0.0f)
	, gameTime(0.0f)
	, startTime(0.0f)
	, frameTimeLeft(0.0f)
	, userSpeedFactor(1.0f)
	, internalSpeed(1.0f)
	, medianCpu(0.0f)
	, medianPing(0)
	, curSpeedCtrl(0)
	, loopSleepTime(0)
	, serverFrameNum(-1)
	, syncErrorFrame(0)
	, syncWarningFrame(0)
	, desyncHasOccurred(false)
	, linkMinPacketSize(1)
	, localClientNumber(-1u)
	, maxUserSpeed(1.0f)
	, minUserSpeed(1.0f)
	, isPaused(false)
	, gamePausable(true)
	, cheating(false)
	, noHelperAIs(false)
	, canReconnect(false)
	, allowSpecDraw(true)
	, allowSpecJoin(false)
	, whiteListAdditionalPlayers(false)
	, logInfoMessages(false)
	, logDebugMessages(false)
	, demoRecorder(nullptr)
	, hostif(nullptr)
	, rng()
	, thread()
	, gameServerMutex()
	, gameHasStarted{false}
	, generatedGameID{false}
	, reloadingServer{false}
	, quitServer{false}
	, gameID{0}
{
	// Initialize DCF networking
	try {
		dcfConnection = std::make_unique<DCFConnection>("config/dcf_network.json");
		if (!dcfConnection->initialized) {
			LOG_L(L_WARNING, "[%s] DCF init failed, falling back to UDP", __func__);
			udpListener = std::make_unique<netcode::UDPListener>(myClientSetup->hostPort);
		}
	} catch (const std::exception& e) {
		LOG_L(L_ERROR, "[%s] DCF setup error: %s, falling back to UDP", __func__, e.what());
		udpListener = std::make_unique<netcode::UDPListener>(myClientSetup->hostPort);
	}

	// Initialize configuration
	configHandler->GetValueSafe(loopSleepTime, "ServerSleepTime");
	configHandler->GetValueSafe(curSpeedCtrl, "SpeedControl");
	configHandler->GetValueSafe(allowSpecJoin, "AllowSpectatorJoin");
	configHandler->GetValueSafe(whiteListAdditionalPlayers, "WhiteListAdditionalPlayers");
	configHandler->GetValueSafe(logInfoMessages, "ServerLogInfoMessages");
	configHandler->GetValueSafe(logDebugMessages, "ServerLogDebugMessages");

	// Initialize demo recorder
	if (configHandler->GetBool("ServerRecordDemos")) {
		demoRecorder = std::make_unique<CDemoRecorder>();
	}

	// Initialize free AI slots
	for (unsigned a = 0; a < MAX_AIS; a++) {
		freeSkirmishAIs.push_back(a);
	}
	std::reverse(freeSkirmishAIs.begin(), freeSkirmishAIs.end());

	// Start server thread
	thread = spring::thread(&CGameServer::UpdateLoop, this);
}

CGameServer::~CGameServer() {
	quitServer = true;
	if (thread.joinable()) {
		thread.join();
	}
	if (hostif != nullptr) {
		hostif->SendQuit();
	}
	dcfConnection.reset();
	udpListener.reset();
}

void CGameServer::UpdateLoop() {
	while (!quitServer) {
		{
			std::lock_guard<spring::recursive_mutex> lk(gameServerMutex);
			Update();
		}
		spring::thread::sleep_for(std::chrono::milliseconds(loopSleepTime));
	}
}

void CGameServer::Initialize() {
	if (dcfConnection && dcfConnection->initialized) {
		dcfConnection->Update();
		DCF_LOG(dcf::DCFLogLevel::INFO, "Initialized DCF networking");
	} else if (udpListener) {
		udpListener->UpdateConnections();
		LOG_L(L_INFO, "[%s] Initialized UDP fallback", __func__);
	}

	serverFrameNum = 0;
	startTime = modGameTime;
	AddLocalClient(myClientSetup->myPlayerName, myClientSetup->myVersion);
	if (configHandler->GetInt("AutohostPort") != 0) {
		hostif = std::make_unique<AutohostInterface>(
			configHandler->GetString("AutohostIP"),
			configHandler->GetInt("AutohostPort")
		);
	}
}

void CGameServer::ServerMessage(const std::shared_ptr<const netcode::RawPacket>& packet) {
	if (!packet || packet->length == 0 || packet->data == nullptr) {
		DCF_LOG(dcf::DCFLogLevel::WARNING, "Received invalid packet");
		return;
	}

	const unsigned char* inbuf = packet->data;
	const unsigned packetCode = inbuf[0];
	const unsigned dataLength = packet->length;

	switch (packetCode) {
		case NETMSG_SYNCRESPONSE: {
			if (dcfConnection) {
				double rtt = dcfConnection->metrics.averageRTT;
				if (!dcfConnection->IsInRTTGroup(rtt)) {
					InternalSpeedChange(userSpeedFactor * 0.8f);
					DCF_LOG(dcf::DCFLogLevel::INFO, "Adjusted speed due to RTT: " + std::to_string(rtt));
				}
			}
			try {
				UnpackSyncResponse(packet);
			} catch (const netcode::UnpackPacketException& ex) {
				DCF_LOG(dcf::DCFLogLevel::ERROR, std::string("Sync response unpack failed: ") + ex.what());
			}
			break;
		}
		case NETMSG_CREATE_NEWPLAYER: {
			if (dcfConnection) {
				dcfConnection->SendData(packet);
			} else {
				Broadcast(packet);
			}
			AddAdditionalUser(inbuf, dataLength);
			break;
		}
		case NETMSG_PING: {
			if (dcfConnection) {
				dcfConnection->ProcessMetrics();
			}
			HandlePing(inbuf, dataLength);
			break;
		}
		case NETMSG_GAME_FRAME_PROGRESS: {
			HandleGameFrameProgress(inbuf, dataLength);
			break;
		}
		case NETMSG_GAMESTATE_DUMP: {
			DumpState(inbuf, dataLength);
			break;
		}
		case NETMSG_CHAT: {
			try {
				ChatMessage msg(packet);
				GotChatMessage(msg);
			} catch (const netcode::UnpackPacketException& ex) {
				DCF_LOG(dcf::DCFLogLevel::ERROR, std::string("Chat unpack failed: ") + ex.what());
			}
			break;
		}
		case NETMSG_PAUSE: {
			try {
				netcode::UnpackPacket pckt(packet->data, packet->length);
				unsigned playerNum;
				uint8_t pause;
				pckt >> playerNum >> pause;
				PauseGame(pause != 0, playerNum == SERVER_PLAYER);
			} catch (const netcode::UnpackPacketException& ex) {
				DCF_LOG(dcf::DCFLogLevel::ERROR, std::string("Pause unpack failed: ") + ex.what());
			}
			break;
		}
		default: {
			if (dcfConnection) {
				dcfConnection->AddTraffic(-1, packetCode, dataLength);
			}
			if (dcfConnection) {
				dcfConnection->SendData(packet);
			} else {
				Broadcast(packet);
			}
			break;
		}
	}
}

void CGameServer::Update() {
	std::lock_guard<spring::recursive_mutex> lk(gameServerMutex);

	if (dcfConnection && dcfConnection->initialized) {
		dcfConnection->Update();
		while (dcfConnection->HasIncomingData()) {
			auto pkt = dcfConnection->GetData();
			if (pkt) {
				ServerMessage(pkt);
			}
		}
		if (desyncHasOccurred) {
			dcfConnection->TriggerFailoverIfNeeded();
			desyncHasOccurred = false;
			DCF_LOG(dcf::DCFLogLevel::INFO, "Desync resolved via DCF failover");
		}
	} else if (udpListener) {
		udpListener->UpdateConnections();
		while (udpListener->HasIncomingData()) {
			auto pkt = udpListener->GetData();
			if (pkt) {
				ServerMessage(pkt);
			}
		}
	}

	const spring_time currTick = spring_gettime();
	if (!isPaused && !reloadingServer) {
		CreateNewFrame(true, false);
	}

	if (currTick - lastPlayerInfo > spring_msecs(1000)) {
		SendClientProcUsage();
		lastPlayerInfo = currTick;
	}

	if (currTick - lastBandwidthUpdate > spring_msecs(5000)) {
		CheckBandwidth();
		lastBandwidthUpdate = currTick;
	}

	if (CheckForGameEnd()) {
		QuitGame();
	}
}

void CGameServer::CheckSync() {
	if (dcfConnection) {
		double rtt = dcfConnection->metrics.averageRTT;
		if (rtt > SYNCCHECK_MSG_TIMEOUT) {
			LOG_L(L_WARNING, "[%s] High RTT (%f ms), adjusting timeout", __func__, rtt);
			// Dynamically adjust timeout
			int adjustedTimeout = SYNCCHECK_TIMEOUT + static_cast<int>(rtt / 10);
			// Update sync logic with adjustedTimeout
		}
	}

#ifdef SYNCCHECK
	for (const auto& p : outstandingSyncFrames) {
		if (serverFrameNum - p.first > SYNCCHECK_TIMEOUT) {
			syncWarningFrame = p.first;
			DCF_LOG(dcf::DCFLogLevel::WARNING, "Sync timeout for frame " + std::to_string(p.first));
			desyncHasOccurred = true;
		}
	}
#endif
}

void CGameServer::CreateNewFrame(bool fromServerThread, bool fixedFrameTime) {
	serverFrameNum++;
	const spring_time currTick = spring_gettime();
	const float deltaTime = (currTick - lastNewFrameTick).toSecsf();
	lastNewFrameTick = currTick;

	if (serverFrameNum % serverKeyframeInterval == 0) {
		std::shared_ptr<const RawPacket> keyFrame = CBaseNetProtocol::Get().SendKeyFrame(serverFrameNum);
		if (dcfConnection) {
			dcfConnection->SendData(keyFrame);
		} else {
			Broadcast(keyFrame);
		}
	}

	modGameTime += deltaTime * internalSpeed;
	gameTime = modGameTime - startTime;
	frameTimeLeft = std::max(0.0f, frameTimeLeft - deltaTime);

	UpdateSpeedControl(curSpeedCtrl);
}

void CGameServer::GotChatMessage(const ChatMessage& msg) {
	if (msg.msg.empty()) {
		DCF_LOG(dcf::DCFLogLevel::WARNING, "Empty chat message received");
		return;
	}

	std::shared_ptr<const RawPacket> packet(msg.Pack());
	if (dcfConnection) {
		dcfConnection->SendData(packet);
	} else {
		Broadcast(packet);
	}

	if (hostif != nullptr && msg.fromPlayer >= 0 && msg.fromPlayer != SERVER_PLAYER) {
		hostif->SendPlayerChat(msg.fromPlayer, msg.destination, msg.msg);
	}
}

void CGameServer::InternalSpeedChange(float newSpeed) {
	if (internalSpeed == newSpeed) {
		return;
	}

	internalSpeed = newSpeed;
	std::shared_ptr<const RawPacket> packet = CBaseNetProtocol::Get().SendInternalSpeed(internalSpeed);
	if (dcfConnection) {
		dcfConnection->SendData(packet);
	} else {
		Broadcast(packet);
	}

	DCF_LOG(dcf::DCFLogLevel::INFO, "Internal speed changed to " + std::to_string(newSpeed));
}

void CGameServer::UserSpeedChange(float newSpeed, int player) {
	newSpeed = std::clamp(newSpeed, minUserSpeed, maxUserSpeed);
	if (userSpeedFactor == newSpeed) {
		return;
	}

	if (internalSpeed > newSpeed || internalSpeed == userSpeedFactor) {
		InternalSpeedChange(newSpeed);
	}

	std::shared_ptr<const RawPacket> packet = CBaseNetProtocol::Get().SendUserSpeed(player, userSpeedFactor = newSpeed);
	if (dcfConnection) {
		dcfConnection->SendData(packet);
	} else {
		Broadcast(packet);
	}

	DCF_LOG(dcf::DCFLogLevel::INFO, "User speed changed to " + std::to_string(newSpeed) + " by player " + std::to_string(player));
}

void CGameServer::UpdateSpeedControl(int speedCtrl) {
	if (speedCtrl == 0) {
		return;
	}

	float cpuUsageSum = 0.0f;
	int numClients = 0;
	for (const auto& player : players) {
		if (player.second.active) {
			cpuUsageSum += player.second.cpuUsage;
			numClients++;
		}
	}

	if (numClients > 0) {
		medianCpu = cpuUsageSum / numClients;
	}

	if (dcfConnection) {
		double rtt = dcfConnection->metrics.averageRTT;
		if (rtt > 50.0) {
			userSpeedFactor = std::clamp(userSpeedFactor * (50.0 / rtt), minUserSpeed, maxUserSpeed);
			DCF_LOG(dcf::DCFLogLevel::INFO, "Adjusted userSpeedFactor to " + std::to_string(userSpeedFactor) + " due to RTT " + std::to_string(rtt));
		}
	}

	if (speedCtrl == 1) {
		InternalSpeedChange(medianCpu);
	} else {
		float maxCpu = 0.0f;
		for (const auto& player : players) {
			if (player.second.active) {
				maxCpu = std::max(maxCpu, player.second.cpuUsage);
			}
		}
		InternalSpeedChange(maxCpu);
	}
}

void CGameServer::AddLocalClient(const std::string& name, const std::string& version) {
	localClientNumber = AddConnection(std::make_unique<netcode::LocalConnection>(), name, version);
}

unsigned CGameServer::AddConnection(std::unique_ptr<netcode::CConnection> conn, const std::string& name, const std::string& version) {
	unsigned playerNum = 0;
	for (; playerNum < MAX_PLAYERS; ++playerNum) {
		if (!players[playerNum].active) {
			break;
		}
	}
	if (playerNum == MAX_PLAYERS) {
		DCF_LOG(dcf::DCFLogLevel::ERROR, "No free player slots for " + name);
		return -1u;
	}

	players[playerNum].active = true;
	players[playerNum].name = name;
	players[playerNum].version = version;
	players[playerNum].connection = std::move(conn);

	std::shared_ptr<const RawPacket> packet = CBaseNetProtocol::Get().SendPlayerName(playerNum, name);
	if (dcfConnection) {
		dcfConnection->SendData(packet);
	} else {
		Broadcast(packet);
	}

	Message(spring::format(" -> Connection established (given id %i)", playerNum));
	return playerNum;
}

void CGameServer::AddAdditionalUser(const unsigned char* inbuf, unsigned dataLength) {
	try {
		netcode::UnpackPacket pckt(inbuf, dataLength);
		unsigned playerNum;
		std::string name, passwd, version;
		bool spectator;
		int team;
		pckt >> playerNum >> name >> passwd >> version >> spectator >> team;

		if (!allowSpecJoin && !whiteListAdditionalPlayers && spectator) {
			RejectConnection(playerNum, "Server does not allow additional spectators");
			return;
		}

		if (rejectedConnections[playerNum] > 3) {
			RejectConnection(playerNum, "Too many failed connection attempts");
			return;
		}

		GameParticipant& newPlayer = players[playerNum];
		newPlayer.active = true;
		newPlayer.name = name;
		newPlayer.version = version;
		newPlayer.spectator = spectator;
		newPlayer.team = team;
		newPlayer.isMidgameJoin = (gameHasStarted && !newPlayer.spectator);

		std::shared_ptr<const RawPacket> packet = CBaseNetProtocol::Get().SendCreateNewPlayer(playerNum, newPlayer.spectator, newPlayer.team, newPlayer.name);
		if (dcfConnection) {
			dcfConnection->SendData(packet);
		} else {
			Broadcast(packet);
		}

		if (!newPlayer.spectator) {
			if (!teams[newPlayer.team].IsActive()) {
				newPlayer.SetReadyToStart(myGameSetup->startPosType != CGameSetup::StartPos_ChooseInGame);
				teams[newPlayer.team].SetActive(true);
				Broadcast(CBaseNetProtocol::Get().SendJoinTeam(playerNum, newPlayer.team));
			}
		}

		for (const std::shared_ptr<const netcode::RawPacket>& p : packetCache) {
			newPlayer.SendData(p);
		}

		DCF_LOG(dcf::DCFLogLevel::INFO, "Added player " + name + " (id " + std::to_string(playerNum) + ")");
	} catch (const netcode::UnpackPacketException& ex) {
		DCF_LOG(dcf::DCFLogLevel::ERROR, std::string("Failed to add user: ") + ex.what());
	}
}

void CGameServer::RejectConnection(unsigned playerNum, const std::string& reason) {
	std::shared_ptr<const RawPacket> packet = CBaseNetProtocol::Get().SendReject(playerNum, reason);
	if (dcfConnection) {
		dcfConnection->SendData(packet);
	} else {
		Broadcast(packet);
	}
	rejectedConnections[playerNum]++;
	DCF_LOG(dcf::DCFLogLevel::WARNING, "Rejected connection for player " + std::to_string(playerNum) + ": " + reason);
}

void CGameServer::SendClientProcUsage() {
	for (const auto& player : players) {
		if (player.second.active) {
			std::shared_ptr<const RawPacket> packet = CBaseNetProtocol::Get().SendCpuUsage(player.second.cpuUsage);
			if (dcfConnection) {
				dcfConnection->SendData(packet);
			} else {
				Broadcast(packet);
			}
		}
	}
}

void CGameServer::CheckBandwidth() {
	if (dcfConnection) {
		DCF_LOG(dcf::DCFLogLevel::DEBUG, dcfConnection->Statistics());
	} else if (udpListener) {
		LOG_L(L_DEBUG, "[%s] Bandwidth stats: UDP-based", __func__);
	}
}

void CGameServer::HandlePing(const unsigned char* inbuf, unsigned dataLength) {
	try {
		netcode::UnpackPacket pckt(inbuf, dataLength);
		unsigned playerNum;
		pckt >> playerNum;
		netPingTimings[playerNum] = spring_gettime();
		medianPing = 0;
		for (const auto& ping : netPingTimings) {
			if (ping.second != spring_notime) {
				medianPing += (spring_gettime() - ping.second).toMilliSecsi();
			}
		}
		medianPing /= std::max(1u, (unsigned)netPingTimings.size());
		DCF_LOG(dcf::DCFLogLevel::DEBUG, "Ping from player " + std::to_string(playerNum) + ", median ping: " + std::to_string(medianPing));
	} catch (const netcode::UnpackPacketException& ex) {
		DCF_LOG(dcf::DCFLogLevel::ERROR, std::string("Ping unpack failed: ") + ex.what());
	}
}

void CGameServer::HandleGameFrameProgress(const unsigned char* inbuf, unsigned dataLength) {
	try {
		netcode::UnpackPacket pckt(inbuf, dataLength);
		unsigned playerNum, frameNum;
		pckt >> playerNum >> frameNum;
		if (playerNum < MAX_PLAYERS && players[playerNum].active) {
			players[playerNum].lastFrameResponse = frameNum;
			DCF_LOG(dcf::DCFLogLevel::DEBUG, "Frame progress from player " + std::to_string(playerNum) + ": frame " + std::to_string(frameNum));
		}
	} catch (const netcode::UnpackPacketException& ex) {
		DCF_LOG(dcf::DCFLogLevel::ERROR, std::string("Frame progress unpack failed: ") + ex.what());
	}
}

void CGameServer::DumpState(const unsigned char* inbuf, unsigned dataLength) {
	try {
		netcode::UnpackPacket pckt(inbuf, dataLength);
		unsigned playerNum;
		int frameNum;
		pckt >> playerNum >> frameNum;
		if (demoRecorder) {
			demoRecorder->SaveState(frameNum);
			DCF_LOG(dcf::DCFLogLevel::INFO, "Dumped game state for frame " + std::to_string(frameNum) + " by player " + std::to_string(playerNum));
		}
		if (dcfConnection) {
			std::shared_ptr<const RawPacket> packet = CBaseNetProtocol::Get().SendGameState(frameNum);
			dcfConnection->SendData(packet);
		} else {
			Broadcast(CBaseNetProtocol::Get().SendGameState(frameNum));
		}
	} catch (const netcode::UnpackPacketException& ex) {
		DCF_LOG(dcf::DCFLogLevel::ERROR, std::string("State dump unpack failed: ") + ex.what());
	}
}

void CGameServer::UnpackSyncResponse(const std::shared_ptr<const netcode::RawPacket>& packet) {
	try {
		netcode::UnpackPacket pckt(packet->data, packet->length);
		unsigned playerNum, frameNum;
		uint32_t checksum;
		pckt >> playerNum >> frameNum >> checksum;
#ifdef SYNCCHECK
		outstandingSyncFrames[frameNum].insert(std::make_pair(playerNum, checksum));
		DCF_LOG(dcf::DCFLogLevel::DEBUG, "Sync response from player " + std::to_string(playerNum) + " for frame " + std::to_string(frameNum));
#endif
	} catch (const netcode::UnpackPacketException& ex) {
		DCF_LOG(dcf::DCFLogLevel::ERROR, std::string("Sync response unpack failed: ") + ex.what());
	}
}

uint8_t CGameServer::ReserveSkirmishAIId() {
	if (freeSkirmishAIs.empty()) {
		DCF_LOG(dcf::DCFLogLevel::ERROR, "No free skirmish AI slots");
		return MAX_AIS;
	}
	const uint8_t id = freeSkirmishAIs.back();
	freeSkirmishAIs.pop_back();
	DCF_LOG(dcf::DCFLogLevel::INFO, "Reserved skirmish AI ID " + std::to_string(id));
	return id;
}

void CGameServer::FreeSkirmishAIId(uint8_t aiId) {
	if (aiId < MAX_AIS) {
		freeSkirmishAIs.push_back(aiId);
		std::reverse(freeSkirmishAIs.begin(), freeSkirmishAIs.end());
		DCF_LOG(dcf::DCFLogLevel::INFO, "Freed skirmish AI ID " + std::to_string(aiId));
	}
}

void CGameServer::Broadcast(const std::shared_ptr<const netcode::RawPacket>& packet) {
	for (auto& player : players) {
		if (player.second.active && player.second.connection) {
			player.second.connection->SendData(packet);
		}
	}
}

void CGameServer::Message(const std::string& message, bool broadcast) {
	if (!logInfoMessages && !logDebugMessages) {
		return;
	}
	DCF_LOG(dcf::DCFLogLevel::INFO, message);
	if (broadcast) {
		std::shared_ptr<const RawPacket> packet = CBaseNetProtocol::Get().SendSystemMessage(SERVER_PLAYER, message);
		if (dcfConnection) {
			dcfConnection->SendData(packet);
		} else {
			Broadcast(packet);
		}
	}
}

void CGameServer::SendSystemMsg(const std::string& message, int playerNum) {
	std::shared_ptr<const RawPacket> packet = CBaseNetProtocol::Get().SendSystemMessage(playerNum, message);
	if (dcfConnection) {
		dcfConnection->SendData(packet);
	} else {
		Broadcast(packet);
	}
	DCF_LOG(dcf::DCFLogLevel::INFO, "System message to player " + std::to_string(playerNum) + ": " + message);
}

bool CGameServer::CheckForGameEnd() {
	// Simple game end condition: check if any teams are still active
	for (const auto& team : teams) {
		if (team.second.IsActive()) {
			return false;
		}
	}
	DCF_LOG(dcf::DCFLogLevel::INFO, "Game ended: no active teams remaining");
	return true;
}

void CGameServer::StartGame() {
	if (gameHasStarted) {
		return;
	}
	gameHasStarted = true;
	readyTime = spring_gettime();
	std::shared_ptr<const RawPacket> packet = CBaseNetProtocol::Get().SendStartPlaying(0);
	if (dcfConnection) {
		dcfConnection->SendData(packet);
	} else {
		Broadcast(packet);
	}
	DCF_LOG(dcf::DCFLogLevel::INFO, "Game started");
}

void CGameServer::PauseGame(bool pause, bool fromServer) {
	if (!gamePausable || isPaused == pause) {
		return;
	}
	isPaused = pause;
	std::shared_ptr<const RawPacket> packet = CBaseNetProtocol::Get().SendPause(fromServer ? SERVER_PLAYER : 0, pause);
	if (dcfConnection) {
		dcfConnection->SendData(packet);
	} else {
		Broadcast(packet);
	}
	DCF_LOG(dcf::DCFLogLevel::INFO, std::string("Game ") + (pause ? "paused" : "resumed") + " by " + (fromServer ? "server" : "player"));
}

void CGameServer::QuitGame() {
	if (quitServer) {
		return;
	}
	quitServer = true;
	std::shared_ptr<const RawPacket> packet = CBaseNetProtocol::Get().SendQuit();
	if (dcfConnection) {
		dcfConnection->SendData(packet);
	} else {
		Broadcast(packet);
	}
	DCF_LOG(dcf::DCFLogLevel::INFO, "Game quit");
}

void CGameServer::Reload(const std::string& newSetupText) {
	reloadingServer = true;
	std::shared_ptr<const RawPacket> packet = CBaseNetProtocol::Get().SendGameOver(0);
	if (dcfConnection) {
		dcfConnection->SendData(packet);
	} else {
		Broadcast(packet);
	}
	DCF_LOG(dcf::DCFLogLevel::INFO, "Reloading server with new setup");
}

#endif // LOG_SECTION_GAMESERVER
