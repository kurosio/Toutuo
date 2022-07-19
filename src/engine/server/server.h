/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef ENGINE_SERVER_SERVER_H
#define ENGINE_SERVER_SERVER_H

#include <base/hash.h>

#include <engine/console.h>
#include <engine/server.h>

#include <engine/shared/econ.h>
#include <engine/shared/fifo.h>
#include <engine/shared/netban.h>
#include <engine/shared/network.h>
#include <engine/shared/protocol.h>
#include <engine/shared/snapshot.h>
#include <engine/shared/uuid_manager.h>

#include <list>
#include <memory>
#include <vector>

#include "antibot.h"
#include "authmanager.h"
#include "name_ban.h"

#if defined(CONF_UPNP)
#include "upnp.h"
#endif

class CConfig;
class CHostLookup;
class CLogMessage;
class CMsgPacker;
class CPacker;

class CSnapIDPool
{
	enum
	{
		MAX_IDS = 32 * 1024,
	};

	// State of a Snap ID
	enum
	{
		ID_FREE = 0,
		ID_ALLOCATED = 1,
		ID_TIMED = 2,
	};

	class CID
	{
	public:
		short m_Next;
		short m_State; // 0 = free, 1 = allocated, 2 = timed
		int m_Timeout;
	};

	CID m_aIDs[MAX_IDS];

	int m_FirstFree;
	int m_FirstTimed;
	int m_LastTimed;
	int m_Usage;
	int m_InUsage;

public:
	CSnapIDPool();

	void Reset();
	void RemoveFirstTimeout();
	int NewID();
	void TimeoutIDs();
	void FreeID(int ID);
};

class CServerBan : public CNetBan
{
	class CServer *m_pServer;

	template<class T>
	int BanExt(T *pBanPool, const typename T::CDataType *pData, int Seconds, const char *pReason);

public:
	CServer *Server() const { return m_pServer; }

	void InitServerBan(class IConsole *pConsole, class IStorage *pStorage, class CServer *pServer);

	int BanAddr(const NETADDR *pAddr, int Seconds, const char *pReason) override;
	int BanRange(const CNetRange *pRange, int Seconds, const char *pReason) override;

	static void ConBanExt(class IConsole::IResult *pResult, void *pUser);
	static void ConBanRegion(class IConsole::IResult *pResult, void *pUser);
	static void ConBanRegionRange(class IConsole::IResult *pResult, void *pUser);
};

class CServer : public IServer
{
	friend class CServerLogger;
	class CConfig *m_pConfig;
	class IConsole *m_pConsole;
	class IStorage *m_pStorage;
	class IEngineAntibot *m_pAntibot;
	class IRegister *m_pRegister;
	class CMultiWorlds *m_pMultiWorlds;

#if defined(CONF_UPNP)
	CUPnP m_UPnP;
#endif

#if defined(CONF_FAMILY_UNIX)
	UNIXSOCKETADDR m_ConnLoggingDestAddr;
	bool m_ConnLoggingSocketCreated;
	UNIXSOCKET m_ConnLoggingSocket;
#endif

public:
	class IGameServer *GameServer(int WorldID = 0) override;
	class CConfig *Config() { return m_pConfig; }
	const CConfig *Config() const { return m_pConfig; }
	class IConsole *Console() { return m_pConsole; }
	class IStorage *Storage() { return m_pStorage; }
	class IEngineAntibot *Antibot() { return m_pAntibot; }
	class CMultiWorlds *MultiWorlds() const { return m_pMultiWorlds; }

	enum
	{
		MAX_RCONCMD_SEND = 16,
	};

	class CClient
	{
	public:
		enum
		{
			STATE_EMPTY = 0,
			STATE_PREAUTH,
			STATE_AUTH,
			STATE_CONNECTING,
			STATE_READY,
			STATE_INGAME,

			SNAPRATE_INIT = 0,
			SNAPRATE_FULL,
			SNAPRATE_RECOVER,

			DNSBL_STATE_NONE = 0,
			DNSBL_STATE_PENDING,
			DNSBL_STATE_BLACKLISTED,
			DNSBL_STATE_WHITELISTED,
		};

		class CInput
		{
		public:
			int m_aData[MAX_INPUT_SIZE];
			int m_GameTick; // the tick that was chosen for the input
		};

		// connection state info
		int m_State;
		int m_Latency;
		int m_SnapRate;

		double m_Traffic;
		int64_t m_TrafficSince;

		int m_LastAckedSnapshot;
		int m_LastInputTick;
		CSnapshotStorage m_Snapshots;

		CInput m_LatestInput;
		CInput m_aInputs[200]; // TODO: handle input better
		int m_CurrentInput;

		char m_aName[MAX_NAME_LENGTH];
		char m_aClan[MAX_CLAN_LENGTH];
		char m_aLanguage[MAX_LANGUAGE_LENGTH];
		int m_Country;
		int m_Score;
		int m_Authed;
		int m_AuthKey;
		int m_AuthTries;
		int m_NextMapChunk;
		int m_Flags;
		bool m_ShowIps;

		int m_WorldID;
		int m_OldWorldID;
		bool m_ChangeMap;

		const IConsole::CCommandInfo *m_pRconCmdToSend;

		void Reset();

		// DDRace
		NETADDR m_Addr;
		bool m_GotDDNetVersionPacket;
		bool m_DDNetVersionSettled;
		int m_DDNetVersion;
		char m_aDDNetVersionStr[64];
		CUuid m_ConnectionID;

		// DNSBL
		int m_DnsblState;
		std::shared_ptr<CHostLookup> m_pDnsblLookup;
	};

	CClient m_aClients[MAX_CLIENTS];
	int m_aIdMap[MAX_CLIENTS * VANILLA_MAX_CLIENTS];

	CSnapshotDelta m_SnapshotDelta;
	CSnapshotBuilder m_SnapshotBuilder;
	CSnapIDPool m_IDPool;
	CNetServer m_NetServer;
	CEcon m_Econ;
#if defined(CONF_FAMILY_UNIX)
	CFifo m_Fifo;
#endif
	CServerBan m_ServerBan;

	int64_t m_GameStartTime;
	//int m_CurrentGameTick;

	enum
	{
		UNINITIALIZED = 0,
		RUNNING = 1,
		STOPPING = 2
	};

	int m_RunServer;

	bool m_HeavyReload;
	int m_RconClientID;
	int m_RconAuthLevel;
	int m_PrintCBIndex;
	char m_aShutdownReason[128];

	CAuthManager m_AuthManager;

	int64_t m_ServerInfoFirstRequest;
	int m_ServerInfoNumRequests;

	char m_aErrorShutdownReason[128];

	std::vector<CNameBan> m_vNameBans;

	CServer();
	~CServer();

	bool IsClientNameAvailable(int ClientID, const char *pNameRequest);
	bool SetClientNameImpl(int ClientID, const char *pNameRequest, bool Set);

	bool WouldClientNameChange(int ClientID, const char *pNameRequest) override;
	void SetClientName(int ClientID, const char *pName) override;
	void SetClientClan(int ClientID, char const *pClan) override;
	void SetClientCountry(int ClientID, int Country) override;
	void SetClientScore(int ClientID, int Score) override;
	void SetClientFlags(int ClientID, int Flags) override;
	void SetClientLanguage(int ClientID, const char *pLanguage) override;
	const char *GetClientLanguage(int ClientID) const override;

	void ChangeWorld(int ClientID, int NewWorldID) override;
	int GetClientWorldID(int ClientID) override;
	const char *GetWorldName(int WorldID) override;

	void Kick(int ClientID, const char *pReason) override;
	void Ban(int ClientID, int Seconds, const char *pReason) override;

	//int Tick()
	int64_t TickStartTime(int Tick);
	//int TickSpeed()

	int Init();

	void SendLogLine(const CLogMessage *pMessage);
	void SetRconCID(int ClientID) override;
	int GetAuthedState(int ClientID) const override;
	const char *GetAuthName(int ClientID) const override;
	void GetMapInfo(int WorldID, char *pMapName, int MapNameSize, int *pMapSize, SHA256_DIGEST *pMapSha256, unsigned *pMapCrc) override;
	int GetClientInfo(int ClientID, CClientInfo *pInfo) const override;
	void SetClientDDNetVersion(int ClientID, int DDNetVersion) override;
	void GetClientAddr(int ClientID, char *pAddrStr, int Size) const override;
	const char *ClientName(int ClientID) const override;
	const char *ClientClan(int ClientID) const override;
	int ClientCountry(int ClientID) const override;
	bool ClientIngame(int ClientID) const override;
	bool ClientAuthed(int ClientID) const override;
	int Port() const override;
	int MaxClients() const override;
	int ClientCount() const override;
	int DistinctClientCount() const override;

	virtual int SendMsg(CMsgPacker *pMsg, int Flags, int ClientID, int64_t Mask = -1, int WorldID = -1);

	void DoSnapshot(int WorldID);

	static int NewClientCallback(int ClientID, void *pUser);
	static int NewClientNoAuthCallback(int ClientID, void *pUser);
	static int DelClientCallback(int ClientID, const char *pReason, void *pUser);

	static int ClientRejoinCallback(int ClientID, void *pUser);

	void SendRconType(int ClientID, bool UsernameReq);
	void SendCapabilities(int ClientID);
	void SendMap(int ClientID);
	void SendMapData(int ClientID, int Chunk);
	void SendConnectionReady(int ClientID);
	void SendRconLine(int ClientID, const char *pLine);
	// Accepts -1 as ClientID to mean "all clients with at least auth level admin"
	void SendRconLogLine(int ClientID, const CLogMessage *pMessage);

	void SendRconCmdAdd(const IConsole::CCommandInfo *pCommandInfo, int ClientID);
	void SendRconCmdRem(const IConsole::CCommandInfo *pCommandInfo, int ClientID);
	void UpdateClientRconCommands();

	void ProcessClientPacket(CNetChunk *pPacket);

	class CCache
	{
	public:
		class CCacheChunk
		{
		public:
			CCacheChunk(const void *pData, int Size);
			CCacheChunk(const CCacheChunk &) = delete;

			std::vector<uint8_t> m_vData;
		};

		std::list<CCacheChunk> m_Cache;

		CCache();
		~CCache();

		void AddChunk(const void *pData, int Size);
		void Clear();
	};
	CCache m_aServerInfoCache[3 * 2];
	bool m_ServerInfoNeedsUpdate;

	void ExpireServerInfo() override;
	void CacheServerInfo(CCache *pCache, int Type, bool SendClients);
	void SendServerInfo(const NETADDR *pAddr, int Token, int Type, bool SendClients);
	bool RateLimitServerInfoConnless();
	void SendServerInfoConnless(const NETADDR *pAddr, int Token, int Type);
	void UpdateRegisterServerInfo();
	void UpdateServerInfo(bool Resend = false);

	void PumpNetwork(bool PacketWaiting);

	bool LoadMap(int ID);
	int Run();

	static void ConTestingCommands(IConsole::IResult *pResult, void *pUser);
	static void ConKick(IConsole::IResult *pResult, void *pUser);
	static void ConStatus(IConsole::IResult *pResult, void *pUser);
	static void ConShutdown(IConsole::IResult *pResult, void *pUser);
	static void ConLogout(IConsole::IResult *pResult, void *pUser);
	static void ConShowIps(IConsole::IResult *pResult, void *pUser);

	static void ConAuthAdd(IConsole::IResult *pResult, void *pUser);
	static void ConAuthAddHashed(IConsole::IResult *pResult, void *pUser);
	static void ConAuthUpdate(IConsole::IResult *pResult, void *pUser);
	static void ConAuthUpdateHashed(IConsole::IResult *pResult, void *pUser);
	static void ConAuthRemove(IConsole::IResult *pResult, void *pUser);
	static void ConAuthList(IConsole::IResult *pResult, void *pUser);

	static void ConNameBan(IConsole::IResult *pResult, void *pUser);
	static void ConNameUnban(IConsole::IResult *pResult, void *pUser);
	static void ConNameBans(IConsole::IResult *pResult, void *pUser);

	static void ConchainLoglevel(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData);
	static void ConchainSpecialInfoupdate(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData);
	static void ConchainMaxclientsperipUpdate(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData);
	static void ConchainCommandAccessUpdate(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData);

	void LogoutClient(int ClientID, const char *pReason);
	void LogoutKey(int Key, const char *pReason);

	void ConchainRconPasswordChangeGeneric(int Level, const char *pCurrent, IConsole::IResult *pResult);
	static void ConchainRconPasswordChange(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData);
	static void ConchainRconModPasswordChange(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData);
	static void ConchainRconHelperPasswordChange(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData);

#if defined(CONF_FAMILY_UNIX)
	static void ConchainConnLoggingServerChange(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData);
#endif

	void RegisterCommands();

	int SnapNewID() override;
	void SnapFreeID(int ID) override;
	void *SnapNewItem(int Type, int ID, int Size) override;
	void SnapSetStaticsize(int ItemType, int Size) override;

	// DDRace

	void GetClientAddr(int ClientID, NETADDR *pAddr) const override;
	int m_aPrevStates[MAX_CLIENTS];
	const char *GetAnnouncementLine(char const *pFileName) override;
	unsigned m_AnnouncementLastLine;

	int *GetIdMap(int ClientID) override;

	void InitDnsbl(int ClientID);
	bool DnsblWhite(int ClientID) override
	{
		return m_aClients[ClientID].m_DnsblState == CClient::DNSBL_STATE_NONE ||
		       m_aClients[ClientID].m_DnsblState == CClient::DNSBL_STATE_WHITELISTED;
	}
	bool DnsblPending(int ClientID) override
	{
		return m_aClients[ClientID].m_DnsblState == CClient::DNSBL_STATE_PENDING;
	}
	bool DnsblBlack(int ClientID) override
	{
		return m_aClients[ClientID].m_DnsblState == CClient::DNSBL_STATE_BLACKLISTED;
	}

	void AuthRemoveKey(int KeySlot);
	bool ClientPrevIngame(int ClientID) override { return m_aPrevStates[ClientID] == CClient::STATE_INGAME; }
	const char *GetNetErrorString(int ClientID) override { return m_NetServer.ErrorString(ClientID); }
	void ResetNetErrorString(int ClientID) override { m_NetServer.ResetErrorString(ClientID); }
	bool SetTimedOut(int ClientID, int OrigID) override;
	void SetTimeoutProtected(int ClientID) override { m_NetServer.SetTimeoutProtected(ClientID); }

	void SendMsgRaw(int ClientID, const void *pData, int Size, int Flags) override;

	bool ErrorShutdown() const { return m_aErrorShutdownReason[0] != 0; }
	void SetErrorShutdown(const char *pReason) override;

#ifdef CONF_FAMILY_UNIX
	enum CONN_LOGGING_CMD
	{
		OPEN_SESSION = 1,
		CLOSE_SESSION = 2,
	};

	void SendConnLoggingCommand(CONN_LOGGING_CMD Cmd, const NETADDR *pAddr);
#endif
};

#endif
