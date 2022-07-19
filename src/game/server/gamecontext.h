/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_SERVER_GAMECONTEXT_H
#define GAME_SERVER_GAMECONTEXT_H

#include <engine/console.h>
#include <engine/server.h>

#include <game/collision.h>
#include <game/layers.h>
#include <game/voting.h>

#include "eventhandler.h"
#include "game/generated/protocol.h"
#include "gameworld.h"

#include "core/context.h"

#include <memory>
#include <string>

/*
	Tick
		Game Context (CGameContext::tick)
			Game World (GAMEWORLD::tick)
				Reset world if requested (GAMEWORLD::reset)
				All entities in the world (ENTITY::tick)
				All entities in the world (ENTITY::tick_defered)
				Remove entities marked for deletion (GAMEWORLD::remove_entities)
			Game Controller (GAMECONTROLLER::tick)
			All players (CPlayer::tick)


	Snap
		Game Context (CGameContext::snap)
			Game World (GAMEWORLD::snap)
				All entities in the world (ENTITY::snap)
			Game Controller (GAMECONTROLLER::snap)
			Events handler (EVENT_HANDLER::snap)
			All players (CPlayer::snap)

*/

enum
{
	NUM_TUNEZONES = 256
};

class CCharacter;
class CConfig;
class CHeap;
class CPlayer;
class CScore;
class CUnpacker;
class IAntibot;
class IGameController;
class IEngine;
class IStorage;
struct CAntibotRoundData;

class CGameContext : public IGameServer
{
	friend class CCommandsChatProcessor;
	friend class CCommandsRconProcessor;

	IServer *m_pServer;
	CConfig *m_pConfig;
	IConsole *m_pConsole;
	IEngine *m_pEngine;
	IStorage *m_pStorage;
	IAntibot *m_pAntibot;
	CLayers m_Layers;
	CCollision m_Collision;
	CNetObjHandler m_NetObjHandler;
	CTuningParams m_Tuning;
	CTuningParams m_aTuningList[NUM_TUNEZONES];
	std::vector<std::string> m_vCensorlist;

	CUuid m_GameUuid;
	CPrng m_Prng;

	void AddVote(const char *pDescription, const char *pCommand);

public:
	IServer *Server() const { return m_pServer; }
	CConfig *Config() { return m_pConfig; }
	IConsole *Console() { return m_pConsole; }
	IEngine *Engine() { return m_pEngine; }
	IStorage *Storage() { return m_pStorage; }
	CCollision *Collision() { return &m_Collision; }
	CTuningParams *Tuning() { return &m_Tuning; }
	CTuningParams *TuningList() { return &m_aTuningList[0]; }
	IAntibot *Antibot() { return m_pAntibot; }

	CGameContext();
	~CGameContext();

	CEventHandler m_Events;
	CPlayer *m_apPlayers[MAX_CLIENTS];
	// keep last input to always apply when none is sent
	CNetObj_PlayerInput m_aLastPlayerInput[MAX_CLIENTS];
	bool m_aPlayerHasInput[MAX_CLIENTS];

	// returns last input if available otherwise nulled PlayerInput object
	// ClientID has to be valid
	CNetObj_PlayerInput GetLastPlayerInput(int ClientID) const;

	IGameController *m_pController;
	CGameWorld m_World;

	// helper functions
	class CPlayer *GetPlayer(int ClientID) const;
	class CCharacter *GetPlayerChar(int ClientID) const;
	std::vector<SSwitchers> &Switchers() { return m_World.m_Core.m_vSwitchers; }

	// voting
	void StartVote(const char *pDesc, const char *pCommand, const char *pReason);
	void EndVote();
	void SendVoteSet(int ClientID);
	void SendVoteStatus(int ClientID, int Total, int Yes, int No);
	void AbortVoteKickOnDisconnect(int ClientID);

	int m_VoteCreator;
	int m_VoteType;
	int64_t m_VoteCloseTime;
	bool m_VoteUpdate;
	int m_VotePos;
	char m_aVoteDescription[VOTE_DESC_LENGTH];
	char m_aVoteCommand[VOTE_CMD_LENGTH];
	char m_aVoteReason[VOTE_REASON_LENGTH];
	int m_NumVoteOptions;
	int m_VoteEnforce;
	char m_aaZoneEnterMsg[NUM_TUNEZONES][256]; // 0 is used for switching from or to area without tunings
	char m_aaZoneLeaveMsg[NUM_TUNEZONES][256];

	char m_aDeleteTempfile[128];
	void DeleteTempfile();

	enum
	{
		VOTE_ENFORCE_UNKNOWN = 0,
		VOTE_ENFORCE_NO,
		VOTE_ENFORCE_YES,
		VOTE_ENFORCE_ABORT,
	};
	CHeap *m_pVoteOptionHeap;
	CVoteOptionServer *m_pVoteOptionFirst;
	CVoteOptionServer *m_pVoteOptionLast;

	// helper functions
	void CreateDamageInd(vec2 Pos, float AngleMod, int Amount, int64_t Mask = -1);
	void CreateExplosion(vec2 Pos, int Owner, int Weapon, bool NoDamage, int ActivatedTeam, int64_t Mask);
	void CreateHammerHit(vec2 Pos, int64_t Mask = -1);
	void CreatePlayerSpawn(vec2 Pos, int64_t Mask = -1);
	void CreateDeath(vec2 Pos, int ClientID, int64_t Mask = -1);
	void CreateSound(vec2 Pos, int Sound, int64_t Mask = -1);
	void CreateSoundGlobal(int Sound, int Target = -1);

	// chat
	enum
	{
		CHAT_ALL = -2,
		CHAT_SPEC = -1,
		CHAT_RED = 0,
		CHAT_BLUE = 1,
		CHAT_WHISPER_SEND = 2,
		CHAT_WHISPER_RECV = 3,
	};
	void Chat(int ClientID, const char *pText, ...);

	void SendChat(int ClientID, int Team, const char *pText, int SpamProtectionClientID = -1);
	
	// broadcast
private:
	struct CBroadcastState
	{
		int m_NoChangeTick;
		char m_aPrevMessage[MAX_BROADCAST_SIZE];

		GamePriority m_Priority;
		char m_aNextMessage[MAX_BROADCAST_SIZE];

		int m_LifeSpanTick;
		GamePriority m_TimedPriority;
		char m_aTimedMessage[MAX_BROADCAST_SIZE];
	};
	CBroadcastState m_aBroadcastStates[MAX_PLAYERS];

public:
	void BroadcastTick(int ClientID);
	void AddBroadcast(int ClientID, const char *pText, GamePriority Priority, int LifeSpan);
	void Broadcast(int ClientID, GamePriority Priority, int LifeSpan, const char *pText, ...);

	// network
	void CallVote(int ClientID, const char *pDesc, const char *pCmd, const char *pReason, const char *pChatmsg);
	void SendEmoticon(int ClientID, int Emoticon);
	void SendWeaponPickup(int ClientID, int Weapon);
	void SendMotd(int ClientID);
	void SendTuningParams(int ClientID, int Zone = 0);

	struct CVoteOptionServer *GetVoteOption(int Index);
	void ProgressVoteOptions(int ClientID);

	//
	void LoadMapSettings();

	// engine events
	void OnInit(int WorldID) override;
	void OnConsoleInit() override;

	void OnTick() override;
	void OnPreSnap() override;
	void OnSnap(int ClientID) override;
	void OnPostSnap() override;
	
	void CensorMessage(char *pCensoredMessage, const char *pMessage, int Size);
	void OnMessage(int MsgID, CUnpacker *pUnpacker, int ClientID) override;
	void OnClearClientData(int ClientID) override;

	void OnClientConnected(int ClientID) override;
	void PrepareClientChangeWorld(int ClientID) override;
	void OnClientEnter(int ClientID) override;
	void OnClientDrop(int ClientID, const char *pReason) override;
	void OnClientDirectInput(int ClientID, void *pInput) override;
	void OnClientPredictedInput(int ClientID, void *pInput) override;
	void OnClientPredictedEarlyInput(int ClientID, void *pInput) override;

	bool IsClientReady(int ClientID) const override;
	bool IsClientPlayer(int ClientID) const override;
	int GetWorldID() const { return m_WorldID; }

	CUuid GameUuid() const override;
	const char *GameType() const override;
	const char *Version() const override;
	const char *NetVersion() const override;

	// DDRace
	bool OnClientDDNetVersionKnown(int ClientID);
	void FillAntibot(CAntibotRoundData *pData) override;
	int ProcessSpamProtection(int ClientID, bool RespectChatInitialDelay = true);
	// Describes the time when the first player joined the server.
	int64_t m_NonEmptySince;
	int GetClientVersion(int ClientID) const;
	int64_t ClientsMaskExcludeClientVersionAndHigher(int Version);
	bool PlayerExists(int ClientID) const override { return m_apPlayers[ClientID]; }
	// Returns true if someone is actively moderating.
	bool PlayerModerating() const;
	void ForceVote(int EnforcerID, bool Success);

	// Checks if player can vote and notify them about the reason
	bool RateLimitPlayerVote(int ClientID);

private:
	// starting 1 to make 0 the special value "no client id"
	uint32_t NextUniqueClientID = 1;
	bool m_VoteWillPass;

	enum
	{
		MAX_MUTES = 32,
		MAX_VOTE_MUTES = 32,
	};
	struct CMute
	{
		NETADDR m_Addr;
		int m_Expire;
		char m_aReason[128];
		bool m_InitialChatDelay;
	};

	int m_WorldID;
	CMute m_aMutes[MAX_MUTES];
	int m_NumMutes;
	CMute m_aVoteMutes[MAX_VOTE_MUTES];
	int m_NumVoteMutes;
	bool TryMute(const NETADDR *pAddr, int Secs, const char *pReason, bool InitialChatDelay);
	void Mute(const NETADDR *pAddr, int Secs, const char *pDisplayName, const char *pReason = "", bool InitialChatDelay = false);
	bool TryVoteMute(const NETADDR *pAddr, int Secs);
	bool VoteMute(const NETADDR *pAddr, int Secs, const char *pDisplayName, int AuthedID);
	bool VoteUnmute(const NETADDR *pAddr, const char *pDisplayName, int AuthedID);
	void Whisper(int ClientID, char *pStr);
	void WhisperID(int ClientID, int VictimID, const char *pMessage);
	void Converse(int ClientID, char *pStr);
	bool IsVersionBanned(int Version);

public:
	CLayers *Layers() { return &m_Layers; }

	enum
	{
		VOTE_ENFORCE_NO_ADMIN = VOTE_ENFORCE_YES + 1,
		VOTE_ENFORCE_YES_ADMIN,

		VOTE_TYPE_UNKNOWN = 0,
		VOTE_TYPE_OPTION,
		VOTE_TYPE_KICK,
		VOTE_TYPE_SPECTATE,
	};
	int m_VoteVictim;
	int m_VoteEnforcer;

	bool IsOptionVote() const { return m_VoteType == VOTE_TYPE_OPTION; }
	bool IsKickVote() const { return m_VoteType == VOTE_TYPE_KICK; }
	bool IsSpecVote() const { return m_VoteType == VOTE_TYPE_SPECTATE; }

	void OnSetAuthed(int ClientID, int Level) override;
	virtual bool PlayerCollision();
	virtual bool PlayerHooking();
	virtual float PlayerJetpack();

	void ResetTuning();
};

inline int64_t CmaskAll() { return -1LL; }
inline int64_t CmaskOne(int ClientID) { return 1LL << ClientID; }
inline int64_t CmaskUnset(int64_t Mask, int ClientID) { return Mask ^ CmaskOne(ClientID); }
inline int64_t CmaskAllExceptOne(int ClientID) { return CmaskUnset(CmaskAll(), ClientID); }
inline bool CmaskIsSet(int64_t Mask, int ClientID) { return (Mask & CmaskOne(ClientID)) != 0; }
#endif
