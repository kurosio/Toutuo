/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include <vector>

#include "base/system.h"
#include "gamecontext.h"
#include <antibot/antibot_data.h>
#include <base/logger.h>
#include <base/math.h>
#include <cstring>
#include <engine/console.h>
#include <engine/engine.h>
#include <engine/map.h>
#include <engine/server/server.h>
#include <engine/shared/config.h>
#include <engine/shared/datafile.h>
#include <engine/shared/linereader.h>
#include <engine/shared/memheap.h>
#include <engine/storage.h>

#include <game/collision.h>
#include <game/gamecore.h>
#include <game/mapitems.h>
#include <game/version.h>

#include "entities/character.h"
#include "gamemodes/DDRace.h"
#include "player.h"

#include <teeother/components/localization.h>

#include <game/server/core/command_processor/cmdchat.h>
#include <game/server/core/command_processor/cmdrcon.h>

#include <engine/server/sql_connect_pool.h>
#include <teeother/tl/singletion.h>

// Not thread-safe!
class CClientChatLogger : public ILogger
{
	CGameContext *m_pGameServer;
	int m_ClientID;
	ILogger *m_pOuterLogger;

public:
	CClientChatLogger(CGameContext *pGameServer, int ClientID, ILogger *pOuterLogger) :
		m_pGameServer(pGameServer),
		m_ClientID(ClientID),
		m_pOuterLogger(pOuterLogger)
	{
	}
	void Log(const CLogMessage *pMessage) override;
};

void CClientChatLogger::Log(const CLogMessage *pMessage)
{
	if(str_comp(pMessage->m_aSystem, "chatresp") == 0)
	{
		m_pGameServer->Chat(m_ClientID, pMessage->Message());
	}
	else
	{
		m_pOuterLogger->Log(pMessage);
	}
}

CGameContext::CGameContext()
{
	m_pServer = nullptr;
	m_pController = nullptr;

	for(auto &pPlayer : m_apPlayers)
		pPlayer = nullptr;

	m_aVoteCommand[0] = 0;
	m_VoteType = VOTE_TYPE_UNKNOWN;
	m_VoteCloseTime = 0;
	m_pVoteOptionFirst = nullptr;
	m_pVoteOptionLast = nullptr;
	m_NumVoteOptions = 0;
	m_NumMutes = 0;
	m_NumVoteMutes = 0;
	m_NonEmptySince = 0;
	m_aDeleteTempfile[0] = 0;

	m_pVoteOptionHeap = new CHeap();
	mem_zero(&m_aLastPlayerInput, sizeof(m_aLastPlayerInput));
	mem_zero(&m_aPlayerHasInput, sizeof(m_aPlayerHasInput));
}

CGameContext::~CGameContext()
{
	for(auto &pPlayer : m_apPlayers)
		delete pPlayer;

	delete m_pVoteOptionHeap;
	Antibot()->RoundEnd();
	DeleteTempfile();
	Collision()->Dest();

	delete m_pController;
	m_pController = nullptr;
}

CNetObj_PlayerInput CGameContext::GetLastPlayerInput(int ClientID) const
{
	dbg_assert(0 <= ClientID && ClientID < MAX_CLIENTS, "invalid ClientID");
	return m_aLastPlayerInput[ClientID];
}

CPlayer *CGameContext::GetPlayer(int ClientID) const
{
	if(ClientID < 0 || ClientID >= MAX_CLIENTS)
		return nullptr;
	return m_apPlayers[ClientID];
}

class CCharacter *CGameContext::GetPlayerChar(int ClientID) const
{
	if(ClientID < 0 || ClientID >= MAX_CLIENTS || !m_apPlayers[ClientID])
		return nullptr;
	return m_apPlayers[ClientID]->GetCharacter();
}

void CGameContext::FillAntibot(CAntibotRoundData *pData)
{
	if(!pData->m_Map.m_pTiles)
	{
		Collision()->FillAntibot(&pData->m_Map);
	}
	pData->m_Tick = Server()->Tick();
	mem_zero(pData->m_aCharacters, sizeof(pData->m_aCharacters));
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		CAntibotCharacterData *pChar = &pData->m_aCharacters[i];
		for(auto &LatestInput : pChar->m_aLatestInputs)
		{
			LatestInput.m_TargetX = -1;
			LatestInput.m_TargetY = -1;
		}
		pChar->m_Alive = false;
		pChar->m_Pause = false;
		pChar->m_Team = -1;

		pChar->m_Pos = vec2(-1, -1);
		pChar->m_Vel = vec2(0, 0);
		pChar->m_Angle = -1;
		pChar->m_HookedPlayer = -1;
		pChar->m_SpawnTick = -1;
		pChar->m_WeaponChangeTick = -1;

		if(m_apPlayers[i])
		{
			str_copy(pChar->m_aName, Server()->ClientName(i), sizeof(pChar->m_aName));
			CCharacter *pGameChar = m_apPlayers[i]->GetCharacter();
			pChar->m_Alive = (bool)pGameChar;
			pChar->m_Pause = m_apPlayers[i]->IsPaused();
			pChar->m_Team = m_apPlayers[i]->GetTeam();
			if(pGameChar)
			{
				pGameChar->FillAntibot(pChar);
			}
		}
	}
}

void CGameContext::CreateDamageInd(vec2 Pos, float Angle, int Amount, int64_t Mask)
{
	float a = 3 * 3.14159f / 2 + Angle;
	//float a = get_angle(dir);
	float s = a - pi / 3;
	float e = a + pi / 3;
	for(int i = 0; i < Amount; i++)
	{
		float f = mix(s, e, float(i + 1) / float(Amount + 2));
		CNetEvent_DamageInd *pEvent = (CNetEvent_DamageInd *)m_Events.Create(NETEVENTTYPE_DAMAGEIND, sizeof(CNetEvent_DamageInd), Mask);
		if(pEvent)
		{
			pEvent->m_X = (int)Pos.x;
			pEvent->m_Y = (int)Pos.y;
			pEvent->m_Angle = (int)(f * 256.0f);
		}
	}
}

void CGameContext::CreateHammerHit(vec2 Pos, int64_t Mask)
{
	// create the event
	CNetEvent_HammerHit *pEvent = (CNetEvent_HammerHit *)m_Events.Create(NETEVENTTYPE_HAMMERHIT, sizeof(CNetEvent_HammerHit), Mask);
	if(pEvent)
	{
		pEvent->m_X = (int)Pos.x;
		pEvent->m_Y = (int)Pos.y;
	}
}

void CGameContext::CreateExplosion(vec2 Pos, int Owner, int Weapon, bool NoDamage, int ActivatedTeam, int64_t Mask)
{
	// create the event
	CNetEvent_Explosion *pEvent = (CNetEvent_Explosion *)m_Events.Create(NETEVENTTYPE_EXPLOSION, sizeof(CNetEvent_Explosion), Mask);
	if(pEvent)
	{
		pEvent->m_X = (int)Pos.x;
		pEvent->m_Y = (int)Pos.y;
	}

	// deal damage
	CCharacter *apEnts[MAX_CLIENTS];
	float Radius = 135.0f;
	float InnerRadius = 48.0f;
	int Num = m_World.FindEntities(Pos, Radius, (CEntity **)apEnts, MAX_CLIENTS, CGameWorld::ENTTYPE_CHARACTER);
	int64_t TeamMask = -1;
	for(int i = 0; i < Num; i++)
	{
		vec2 Diff = apEnts[i]->m_Pos - Pos;
		vec2 ForceDir(0, 1);
		float l = length(Diff);
		if(l)
			ForceDir = normalize(Diff);
		l = 1 - clamp((l - InnerRadius) / (Radius - InnerRadius), 0.0f, 1.0f);
		float Strength;
		if(Owner == -1 || !m_apPlayers[Owner] || !m_apPlayers[Owner]->m_TuneZone)
			Strength = Tuning()->m_ExplosionStrength;
		else
			Strength = TuningList()[m_apPlayers[Owner]->m_TuneZone].m_ExplosionStrength;

		float Dmg = Strength * l;
		if(!(int)Dmg)
			continue;

		if((GetPlayerChar(Owner) ? !(GetPlayerChar(Owner)->m_Hit & CCharacter::DISABLE_HIT_GRENADE) : g_Config.m_SvHit) || NoDamage || Owner == apEnts[i]->GetPlayer()->GetCID())
		{
			if(Owner != -1 && apEnts[i]->IsAlive() && !apEnts[i]->CanCollide(Owner))
				continue;
			if(Owner == -1 && ActivatedTeam != -1 && apEnts[i]->IsAlive() && apEnts[i]->EventGroup() != ActivatedTeam)
				continue;

			// Explode at most once per team
			int PlayerTeam = apEnts[i]->EventGroup();
			if((GetPlayerChar(Owner) ? GetPlayerChar(Owner)->m_Hit & CCharacter::DISABLE_HIT_GRENADE : !g_Config.m_SvHit) || NoDamage)
			{
				if(!CmaskIsSet(TeamMask, PlayerTeam))
					continue;
				TeamMask = CmaskUnset(TeamMask, PlayerTeam);
			}

			apEnts[i]->TakeDamage(ForceDir * Dmg * 2, (int)Dmg, Owner, Weapon);
		}
	}
}

void CGameContext::CreatePlayerSpawn(vec2 Pos, int64_t Mask)
{
	// create the event
	CNetEvent_Spawn *ev = (CNetEvent_Spawn *)m_Events.Create(NETEVENTTYPE_SPAWN, sizeof(CNetEvent_Spawn), Mask);
	if(ev)
	{
		ev->m_X = (int)Pos.x;
		ev->m_Y = (int)Pos.y;
	}
}

void CGameContext::CreateDeath(vec2 Pos, int ClientID, int64_t Mask)
{
	// create the event
	CNetEvent_Death *pEvent = (CNetEvent_Death *)m_Events.Create(NETEVENTTYPE_DEATH, sizeof(CNetEvent_Death), Mask);
	if(pEvent)
	{
		pEvent->m_X = (int)Pos.x;
		pEvent->m_Y = (int)Pos.y;
		pEvent->m_ClientID = ClientID;
	}
}

void CGameContext::CreateSound(vec2 Pos, int Sound, int64_t Mask)
{
	if(Sound < 0)
		return;

	// create a sound
	CNetEvent_SoundWorld *pEvent = (CNetEvent_SoundWorld *)m_Events.Create(NETEVENTTYPE_SOUNDWORLD, sizeof(CNetEvent_SoundWorld), Mask);
	if(pEvent)
	{
		pEvent->m_X = (int)Pos.x;
		pEvent->m_Y = (int)Pos.y;
		pEvent->m_SoundID = Sound;
	}
}

void CGameContext::CreateSoundGlobal(int Sound, int Target)
{
	if(Sound < 0)
		return;

	CNetMsg_Sv_SoundGlobal Msg;
	Msg.m_SoundID = Sound;
	if(Target == -2)
		Server()->SendPackMsg(&Msg, MSGFLAG_NOSEND, -1);
	else
	{
		int Flag = MSGFLAG_VITAL;
		if(Target != -1)
			Flag |= MSGFLAG_NORECORD;
		Server()->SendPackMsg(&Msg, Flag, Target);
	}
}

void CGameContext::CallVote(int ClientID, const char *pDesc, const char *pCmd, const char *pReason, const char *pChatmsg)
{
	// check if a vote is already running
	if(m_VoteCloseTime)
		return;

	int64_t Now = Server()->Tick();
	CPlayer *pPlayer = m_apPlayers[ClientID];

	if(!pPlayer)
		return;

	SendChat(-1, CGameContext::CHAT_ALL, pChatmsg, -1);

	m_VoteCreator = ClientID;
	StartVote(pDesc, pCmd, pReason);
	pPlayer->m_Vote = 1;
	pPlayer->m_VotePos = m_VotePos = 1;
	pPlayer->m_LastVoteCall = Now;
}

void CGameContext::SendChat(int ChatterClientID, int Team, const char *pText, int SpamProtectionClientID)
{
	if(SpamProtectionClientID >= 0 && SpamProtectionClientID < MAX_CLIENTS)
		if(ProcessSpamProtection(SpamProtectionClientID))
			return;

	char aBuf[256], aText[256];
	str_copy(aText, pText, sizeof(aText));
	if(ChatterClientID >= 0 && ChatterClientID < MAX_CLIENTS)
		str_format(aBuf, sizeof(aBuf), "%d:%d:%s: %s", ChatterClientID, Team, Server()->ClientName(ChatterClientID), aText);
	else if(ChatterClientID == -2)
	{
		str_format(aBuf, sizeof(aBuf), "### %s", aText);
		str_copy(aText, aBuf, sizeof(aText));
		ChatterClientID = -1;
	}
	else
		str_format(aBuf, sizeof(aBuf), "*** %s", aText);
	Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, Team != CHAT_ALL ? "teamchat" : "chat", aBuf);

	if(Team == CHAT_ALL)
	{
		CNetMsg_Sv_Chat Msg;
		Msg.m_Team = 0;
		Msg.m_ClientID = ChatterClientID;
		Msg.m_pMessage = aText;

		// send to the clients
		for(int i = 0; i < Server()->MaxClients(); i++)
		{
			if(!m_apPlayers[i])
				continue;
			if(!m_apPlayers[i]->m_DND)
				Server()->SendPackMsg(&Msg, MSGFLAG_VITAL | MSGFLAG_NORECORD, i);
		}
	}
	else
	{
		CNetMsg_Sv_Chat Msg;
		Msg.m_Team = 1;
		Msg.m_ClientID = ChatterClientID;
		Msg.m_pMessage = aText;

		// send to the clients
		for(int i = 0; i < Server()->MaxClients(); i++)
		{
			if(m_apPlayers[i] != nullptr)
			{
				if(Team == CHAT_SPEC)
				{
					if(m_apPlayers[i]->GetTeam() == CHAT_SPEC)
					{
						Server()->SendPackMsg(&Msg, MSGFLAG_VITAL | MSGFLAG_NORECORD, i);
					}
				}
			}
		}
	}
}

// send a formatted message
void CGameContext::Chat(int ClientID, const char *pText, ...)
{
	const int Start = (ClientID < 0 ? 0 : ClientID);
	const int End = (ClientID < 0 ? MAX_PLAYERS : ClientID + 1);

	CNetMsg_Sv_Chat Msg;
	Msg.m_Team = 0;
	Msg.m_ClientID = -1;
	Msg.m_pMessage = pText;

	va_list VarArgs;
	va_start(VarArgs, pText);

	dynamic_string Buffer;
	for(int i = Start; i < End; i++)
	{
		if(m_apPlayers[i])
		{
			Server()->Localization()->Format_VL(Buffer, Server()->GetClientLanguage(i), pText, VarArgs);
			Msg.m_pMessage = Buffer.buffer();
			Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, i);
			Buffer.clear();
		}
	}

	va_end(VarArgs);
}

void CGameContext::AddBroadcast(int ClientID, const char *pText, GamePriority Priority, int LifeSpan)
{
	if(ClientID < 0 || ClientID >= MAX_PLAYERS)
		return;

	if(LifeSpan > 0)
	{
		if(m_aBroadcastStates[ClientID].m_TimedPriority > Priority)
			return;

		str_copy(m_aBroadcastStates[ClientID].m_aTimedMessage, pText, sizeof(m_aBroadcastStates[ClientID].m_aTimedMessage));
		m_aBroadcastStates[ClientID].m_LifeSpanTick = LifeSpan;
		m_aBroadcastStates[ClientID].m_TimedPriority = Priority;
	}
	else
	{
		if(m_aBroadcastStates[ClientID].m_Priority > Priority)
			return;

		str_copy(m_aBroadcastStates[ClientID].m_aNextMessage, pText, sizeof(m_aBroadcastStates[ClientID].m_aNextMessage));
		m_aBroadcastStates[ClientID].m_Priority = Priority;
	}
}

// formatted broadcast
void CGameContext::Broadcast(int ClientID, GamePriority Priority, int LifeSpan, const char *pText, ...)
{
	int Start = (ClientID < 0 ? 0 : ClientID);
	int End = (ClientID < 0 ? MAX_PLAYERS : ClientID + 1);

	va_list VarArgs;
	va_start(VarArgs, pText);
	for(int i = Start; i < End; i++)
	{
		if(m_apPlayers[i])
		{
			dynamic_string Buffer;
			Server()->Localization()->Format_VL(Buffer, Server()->GetClientLanguage(i), pText, VarArgs);
			AddBroadcast(i, Buffer.buffer(), Priority, LifeSpan);
			Buffer.clear();
		}
	}
	va_end(VarArgs);
}

// the tick of the broadcast and his life
void CGameContext::BroadcastTick(int ClientID)
{
	if(ClientID < 0 || ClientID >= MAX_PLAYERS)
		return;

	if(m_apPlayers[ClientID])
	{
		if(m_aBroadcastStates[ClientID].m_LifeSpanTick > 0 && m_aBroadcastStates[ClientID].m_TimedPriority > m_aBroadcastStates[ClientID].m_Priority)
			str_copy(m_aBroadcastStates[ClientID].m_aNextMessage, m_aBroadcastStates[ClientID].m_aTimedMessage, sizeof(m_aBroadcastStates[ClientID].m_aNextMessage));

		// send broadcast only if the message is different, or to fight auto-fading
		if(str_comp(m_aBroadcastStates[ClientID].m_aPrevMessage, m_aBroadcastStates[ClientID].m_aNextMessage) != 0 ||
			m_aBroadcastStates[ClientID].m_NoChangeTick > Server()->TickSpeed())
		{
			CNetMsg_Sv_Broadcast Msg;
			Msg.m_pMessage = m_aBroadcastStates[ClientID].m_aNextMessage;
			Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, ClientID);
			str_copy(m_aBroadcastStates[ClientID].m_aPrevMessage, m_aBroadcastStates[ClientID].m_aNextMessage, sizeof(m_aBroadcastStates[ClientID].m_aPrevMessage));
			m_aBroadcastStates[ClientID].m_NoChangeTick = 0;
		}
		else
			m_aBroadcastStates[ClientID].m_NoChangeTick++;

		// update broadcast state
		if(m_aBroadcastStates[ClientID].m_LifeSpanTick > 0)
			m_aBroadcastStates[ClientID].m_LifeSpanTick--;

		if(m_aBroadcastStates[ClientID].m_LifeSpanTick <= 0)
		{
			m_aBroadcastStates[ClientID].m_aTimedMessage[0] = 0;
			m_aBroadcastStates[ClientID].m_TimedPriority = GamePriority::BASIC;
		}
		m_aBroadcastStates[ClientID].m_aNextMessage[0] = 0;
		m_aBroadcastStates[ClientID].m_Priority = GamePriority::BASIC;
	}
	else
	{
		m_aBroadcastStates[ClientID].m_NoChangeTick = 0;
		m_aBroadcastStates[ClientID].m_LifeSpanTick = 0;
		m_aBroadcastStates[ClientID].m_aPrevMessage[0] = 0;
		m_aBroadcastStates[ClientID].m_aNextMessage[0] = 0;
		m_aBroadcastStates[ClientID].m_aTimedMessage[0] = 0;
		m_aBroadcastStates[ClientID].m_Priority = GamePriority::BASIC;
		m_aBroadcastStates[ClientID].m_TimedPriority = GamePriority::BASIC;
	}
}

void CGameContext::SendEmoticon(int ClientID, int Emoticon)
{
	CNetMsg_Sv_Emoticon Msg;
	Msg.m_ClientID = ClientID;
	Msg.m_Emoticon = Emoticon;
	Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, -1);
}

void CGameContext::SendWeaponPickup(int ClientID, int Weapon)
{
	CNetMsg_Sv_WeaponPickup Msg;
	Msg.m_Weapon = Weapon;
	Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, ClientID);
}

void CGameContext::SendMotd(int ClientID)
{
	CNetMsg_Sv_Motd Msg;
	Msg.m_pMessage = g_Config.m_SvMotd;
	Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, ClientID);
}

void CGameContext::StartVote(const char *pDesc, const char *pCommand, const char *pReason)
{
	// reset votes
	m_VoteEnforce = VOTE_ENFORCE_UNKNOWN;
	m_VoteEnforcer = -1;
	for(auto &pPlayer : m_apPlayers)
	{
		if(pPlayer)
		{
			pPlayer->m_Vote = 0;
			pPlayer->m_VotePos = 0;
		}
	}

	// start vote
	m_VoteCloseTime = time_get() + time_freq() * g_Config.m_SvVoteTime;
	str_copy(m_aVoteDescription, pDesc, sizeof(m_aVoteDescription));
	str_copy(m_aVoteCommand, pCommand, sizeof(m_aVoteCommand));
	str_copy(m_aVoteReason, pReason, sizeof(m_aVoteReason));
	SendVoteSet(-1);
	m_VoteUpdate = true;
}

void CGameContext::EndVote()
{
	m_VoteCloseTime = 0;
	SendVoteSet(-1);
}

void CGameContext::SendVoteSet(int ClientID)
{
	::CNetMsg_Sv_VoteSet Msg;

	if(m_VoteCloseTime)
	{
		Msg.m_Timeout = (m_VoteCloseTime - time_get()) / time_freq();
		Msg.m_pDescription = m_aVoteDescription;
		Msg.m_pReason = m_aVoteReason;
	}
	else
	{
		Msg.m_Timeout = 0;
		Msg.m_pDescription = "";
		Msg.m_pReason = "";
	}

	if(ClientID == -1)
	{
		for(int i = 0; i < Server()->MaxClients(); i++)
		{
			if(!m_apPlayers[i])
				continue;
			Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, i);
		}
	}
	else
	{
		Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, ClientID);
	}
}

void CGameContext::SendVoteStatus(int ClientID, int Total, int Yes, int No)
{
	if(ClientID == -1)
	{
		for(int i = 0; i < MAX_CLIENTS; ++i)
			if(Server()->ClientIngame(i))
				SendVoteStatus(i, Total, Yes, No);
		return;
	}

	if(Total > VANILLA_MAX_CLIENTS && m_apPlayers[ClientID] && m_apPlayers[ClientID]->GetClientVersion() <= VERSION_DDRACE)
	{
		Yes = float(Yes * VANILLA_MAX_CLIENTS) / float(Total);
		No = float(No * VANILLA_MAX_CLIENTS) / float(Total);
		Total = VANILLA_MAX_CLIENTS;
	}

	CNetMsg_Sv_VoteStatus Msg = {0};
	Msg.m_Total = Total;
	Msg.m_Yes = Yes;
	Msg.m_No = No;
	Msg.m_Pass = Total - (Yes + No);

	Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, ClientID);
}

void CGameContext::AbortVoteKickOnDisconnect(int ClientID)
{
	if(m_VoteCloseTime && ((str_startswith(m_aVoteCommand, "kick ") && str_toint(&m_aVoteCommand[5]) == ClientID) ||
				      (str_startswith(m_aVoteCommand, "set_team ") && str_toint(&m_aVoteCommand[9]) == ClientID)))
		m_VoteEnforce = VOTE_ENFORCE_ABORT;
}

void CGameContext::SendTuningParams(int ClientID, int Zone)
{
	if(ClientID == -1)
	{
		for(int i = 0; i < MAX_CLIENTS; ++i)
		{
			if(m_apPlayers[i])
			{
				if(m_apPlayers[i]->GetCharacter())
				{
					if(m_apPlayers[i]->GetCharacter()->m_TuneZone == Zone)
						SendTuningParams(i, Zone);
				}
				else if(m_apPlayers[i]->m_TuneZone == Zone)
				{
					SendTuningParams(i, Zone);
				}
			}
		}
		return;
	}

	CMsgPacker Msg(NETMSGTYPE_SV_TUNEPARAMS);
	int *pParams = nullptr;
	if(Zone == 0)
		pParams = (int *)&m_Tuning;
	else
		pParams = (int *)&(m_aTuningList[Zone]);

	for(unsigned i = 0; i < sizeof(m_Tuning) / sizeof(int); i++)
	{
		if(m_apPlayers[ClientID] && m_apPlayers[ClientID]->GetCharacter())
		{
			if((i == 31) // collision
				&& (m_apPlayers[ClientID]->GetCharacter()->NeededFaketuning() & FAKETUNE_SOLO || m_apPlayers[ClientID]->GetCharacter()->NeededFaketuning() & FAKETUNE_NOCOLL))
			{
				Msg.AddInt(0);
			}
			else if((i == 32) // hooking
				&& (m_apPlayers[ClientID]->GetCharacter()->NeededFaketuning() & FAKETUNE_SOLO || m_apPlayers[ClientID]->GetCharacter()->NeededFaketuning() & FAKETUNE_NOHOOK))
			{
				Msg.AddInt(0);
			}
			else if((i == 3) // ground jump impulse
				&& m_apPlayers[ClientID]->GetCharacter()->NeededFaketuning() & FAKETUNE_NOJUMP)
			{
				Msg.AddInt(0);
			}
			else if((i == 33) // jetpack
				&& !(m_apPlayers[ClientID]->GetCharacter()->NeededFaketuning() & FAKETUNE_JETPACK))
			{
				Msg.AddInt(0);
			}
			else if((i == 36) // hammer hit
				&& m_apPlayers[ClientID]->GetCharacter()->NeededFaketuning() & FAKETUNE_NOHAMMER)
			{
				Msg.AddInt(0);
			}
			else
			{
				Msg.AddInt(pParams[i]);
			}
		}
		else
			Msg.AddInt(pParams[i]); // if everything is normal just send true tunings
	}
	Server()->SendMsg(&Msg, MSGFLAG_VITAL, ClientID);
}

void CGameContext::OnTick()
{
	// copy tuning
	m_World.m_Core.m_Tuning[0] = m_Tuning;
	m_World.Tick();

	//if(world.paused) // make sure that the game object always updates
	m_pController->Tick();

	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(!m_apPlayers[i] || m_apPlayers[i]->GetPlayerWorldID() != m_WorldID)
			continue;

		// send vote options
		ProgressVoteOptions(i);

		m_apPlayers[i]->Tick();
		m_apPlayers[i]->PostTick();

		if(i < MAX_PLAYERS)
			BroadcastTick(i);
	}

	for(auto &pPlayer : m_apPlayers)
	{
		if(pPlayer)
			pPlayer->PostPostTick();
	}

	// update voting
	if(m_VoteCloseTime)
	{
		// abort the kick-vote on player-leave
		if(m_VoteEnforce == VOTE_ENFORCE_ABORT)
		{
			SendChat(-1, CGameContext::CHAT_ALL, "Vote aborted");
			EndVote();
		}
		else
		{
			int Total = 0, Yes = 0, No = 0;
			bool Veto = false, VetoStop = false;
			if(m_VoteUpdate)
			{
				// count votes
				char aaBuf[MAX_CLIENTS][NETADDR_MAXSTRSIZE] = {{0}}, *pIP = nullptr;
				bool SinglePlayer = true;
				for(int i = 0; i < MAX_CLIENTS; i++)
				{
					if(m_apPlayers[i])
					{
						Server()->GetClientAddr(i, aaBuf[i], NETADDR_MAXSTRSIZE);
						if(!pIP)
							pIP = aaBuf[i];
						else if(SinglePlayer && str_comp(pIP, aaBuf[i]))
							SinglePlayer = false;
					}
				}

				// remember checked players, only the first player with a specific ip will be handled
				bool aVoteChecked[MAX_CLIENTS] = {false};
				int64_t Now = Server()->Tick();
				for(int i = 0; i < MAX_CLIENTS; i++)
				{
					if(!m_apPlayers[i] || aVoteChecked[i])
						continue;

					if((IsKickVote() || IsSpecVote()) && (m_apPlayers[i]->GetTeam() == TEAM_SPECTATORS))
						continue;

					if(m_apPlayers[i]->m_Afk && i != m_VoteCreator)
						continue;

					// can't vote in kick and spec votes in the beginning after joining
					if((IsKickVote() || IsSpecVote()) && Now < m_apPlayers[i]->m_FirstVoteTick)
						continue;

					// connecting clients with spoofed ips can clog slots without being ingame
					if(((CServer *)Server())->m_aClients[i].m_State != CServer::CClient::STATE_INGAME)
						continue;

					// don't count votes by blacklisted clients
					if(g_Config.m_SvDnsblVote && !m_pServer->DnsblWhite(i) && !SinglePlayer)
						continue;

					int CurVote = m_apPlayers[i]->m_Vote;
					int CurVotePos = m_apPlayers[i]->m_VotePos;

					// only allow IPs to vote once, but keep veto ability
					// check for more players with the same ip (only use the vote of the one who voted first)
					for(int j = i + 1; j < MAX_CLIENTS; j++)
					{
						if(!m_apPlayers[j] || aVoteChecked[j] || str_comp(aaBuf[j], aaBuf[i]) != 0)
							continue;

						// count the latest vote by this ip
						if(CurVotePos < m_apPlayers[j]->m_VotePos)
						{
							CurVote = m_apPlayers[j]->m_Vote;
							CurVotePos = m_apPlayers[j]->m_VotePos;
						}

						aVoteChecked[j] = true;
					}

					Total++;
					if(CurVote > 0)
						Yes++;
					else if(CurVote < 0)
						No++;

					// veto right for players who have been active on server for long and who're not afk
					if(!IsKickVote() && !IsSpecVote() && g_Config.m_SvVoteVetoTime)
					{
						// look through all players with same IP again, including the current player
						for(int j = i; j < MAX_CLIENTS; j++)
						{
							// no need to check ip address of current player
							if(i != j && (!m_apPlayers[j] || str_comp(aaBuf[j], aaBuf[i]) != 0))
								continue;

							if(m_apPlayers[j] && !m_apPlayers[j]->m_Afk && m_apPlayers[j]->GetTeam() != TEAM_SPECTATORS &&
								(Server()->Tick() - m_apPlayers[j]->m_JoinTick) / (Server()->TickSpeed() * 60) > g_Config.m_SvVoteVetoTime)
							{
								if(CurVote == 0)
									Veto = true;
								else if(CurVote < 0)
									VetoStop = true;
								break;
							}
						}
					}
				}

				if(g_Config.m_SvVoteMaxTotal && Total > g_Config.m_SvVoteMaxTotal &&
					(IsKickVote() || IsSpecVote()))
					Total = g_Config.m_SvVoteMaxTotal;

				if((Yes > Total / (100.0f / g_Config.m_SvVoteYesPercentage)) && !Veto)
					m_VoteEnforce = VOTE_ENFORCE_YES;
				else if(No >= Total - Total / (100.0f / g_Config.m_SvVoteYesPercentage))
					m_VoteEnforce = VOTE_ENFORCE_NO;

				if(VetoStop)
					m_VoteEnforce = VOTE_ENFORCE_NO;

				m_VoteWillPass = Yes > (Yes + No) / (100.0f / g_Config.m_SvVoteYesPercentage);
			}

			if(time_get() > m_VoteCloseTime && !g_Config.m_SvVoteMajority)
				m_VoteEnforce = (m_VoteWillPass && !Veto) ? VOTE_ENFORCE_YES : VOTE_ENFORCE_NO;

			// / Ensure minimum time for vote to end when moderating.
			if(m_VoteEnforce == VOTE_ENFORCE_YES && !(PlayerModerating() &&
									(IsKickVote() || IsSpecVote()) && time_get() < m_VoteCloseTime))
			{
				Server()->SetRconCID(IServer::RCON_CID_VOTE);
				Console()->ExecuteLine(m_aVoteCommand);
				Server()->SetRconCID(IServer::RCON_CID_SERV);
				EndVote();
				SendChat(-1, CGameContext::CHAT_ALL, "Vote passed", -1);

				if(m_apPlayers[m_VoteCreator] && !IsKickVote() && !IsSpecVote())
					m_apPlayers[m_VoteCreator]->m_LastVoteCall = 0;
			}
			else if(m_VoteEnforce == VOTE_ENFORCE_YES_ADMIN)
			{
				char aBuf[64];
				str_format(aBuf, sizeof(aBuf), "Vote passed enforced by authorized player");
				Console()->ExecuteLine(m_aVoteCommand, m_VoteEnforcer);
				SendChat(-1, CGameContext::CHAT_ALL, aBuf, -1);
				EndVote();
			}
			else if(m_VoteEnforce == VOTE_ENFORCE_NO_ADMIN)
			{
				char aBuf[64];
				str_format(aBuf, sizeof(aBuf), "Vote failed enforced by authorized player");
				EndVote();
				SendChat(-1, CGameContext::CHAT_ALL, aBuf, -1);
			}
			//else if(m_VoteEnforce == VOTE_ENFORCE_NO || time_get() > m_VoteCloseTime)
			else if(m_VoteEnforce == VOTE_ENFORCE_NO || (time_get() > m_VoteCloseTime && g_Config.m_SvVoteMajority))
			{
				EndVote();
				if(VetoStop || (m_VoteWillPass && Veto))
					SendChat(-1, CGameContext::CHAT_ALL, "Vote failed because of veto. Find an empty server instead", -1);
				else
					SendChat(-1, CGameContext::CHAT_ALL, "Vote failed", -1);
			}
			else if(m_VoteUpdate)
			{
				m_VoteUpdate = false;
				SendVoteStatus(-1, Total, Yes, No);
			}
		}
	}
	for(int i = 0; i < m_NumMutes; i++)
	{
		if(m_aMutes[i].m_Expire <= Server()->Tick())
		{
			m_NumMutes--;
			m_aMutes[i] = m_aMutes[m_NumMutes];
		}
	}
	for(int i = 0; i < m_NumVoteMutes; i++)
	{
		if(m_aVoteMutes[i].m_Expire <= Server()->Tick())
		{
			m_NumVoteMutes--;
			m_aVoteMutes[i] = m_aVoteMutes[m_NumVoteMutes];
		}
	}

	if(Server()->Tick() % (g_Config.m_SvAnnouncementInterval * Server()->TickSpeed() * 60) == 0)
	{
		const char *Line = Server()->GetAnnouncementLine(g_Config.m_SvAnnouncementFileName);
		if(Line)
			SendChat(-1, CGameContext::CHAT_ALL, Line);
	}

	for(auto &Switcher : Switchers())
	{
		for(int j = 0; j < MAX_CLIENTS; ++j)
		{
			if(Switcher.m_EndTick[j] <= Server()->Tick() && Switcher.m_Type[j] == TILE_SWITCHTIMEDOPEN)
			{
				Switcher.m_Status[j] = false;
				Switcher.m_EndTick[j] = 0;
				Switcher.m_Type[j] = TILE_SWITCHCLOSE;
			}
			else if(Switcher.m_EndTick[j] <= Server()->Tick() && Switcher.m_Type[j] == TILE_SWITCHTIMEDCLOSE)
			{
				Switcher.m_Status[j] = true;
				Switcher.m_EndTick[j] = 0;
				Switcher.m_Type[j] = TILE_SWITCHOPEN;
			}
		}
	}

#ifdef CONF_DEBUG
	if(g_Config.m_DbgDummies)
	{
		for(int i = 0; i < g_Config.m_DbgDummies; i++)
		{
			CNetObj_PlayerInput Input = {0};
			Input.m_Direction = (i & 1) ? -1 : 1;
			m_apPlayers[MAX_CLIENTS - i - 1]->OnPredictedInput(&Input);
		}
	}
#endif

	// Warning: do not put code in this function directly above or below this comment
}

// Server hooks
void CGameContext::OnClientDirectInput(int ClientID, void *pInput)
{
	if(!m_World.m_Paused)
		m_apPlayers[ClientID]->OnDirectInput((CNetObj_PlayerInput *)pInput);

	int Flags = ((CNetObj_PlayerInput *)pInput)->m_PlayerFlags;
	if((Flags & 256) || (Flags & 512))
	{
		Server()->Kick(ClientID, "please update your client or use DDNet client");
	}
}

void CGameContext::OnClientPredictedInput(int ClientID, void *pInput)
{
	// early return if no input at all has been sent by a player
	if(pInput == nullptr && !m_aPlayerHasInput[ClientID])
		return;

	// set to last sent input when no new input has been sent
	CNetObj_PlayerInput *pApplyInput = (CNetObj_PlayerInput *)pInput;
	if(pApplyInput == nullptr)
	{
		pApplyInput = &m_aLastPlayerInput[ClientID];
	}

	if(!m_World.m_Paused)
		m_apPlayers[ClientID]->OnPredictedInput(pApplyInput);
}

void CGameContext::OnClientPredictedEarlyInput(int ClientID, void *pInput)
{
	// early return if no input at all has been sent by a player
	if(pInput == nullptr && !m_aPlayerHasInput[ClientID])
		return;

	// set to last sent input when no new input has been sent
	CNetObj_PlayerInput *pApplyInput = (CNetObj_PlayerInput *)pInput;
	if(pApplyInput == nullptr)
	{
		pApplyInput = &m_aLastPlayerInput[ClientID];
	}
	else
	{
		// Store input in this function and not in `OnClientPredictedInput`,
		// because this function is called on all inputs, while
		// `OnClientPredictedInput` is only called on the first input of each
		// tick.
		mem_copy(&m_aLastPlayerInput[ClientID], pApplyInput, sizeof(m_aLastPlayerInput[ClientID]));
		m_aPlayerHasInput[ClientID] = true;
	}

	if(!m_World.m_Paused)
		m_apPlayers[ClientID]->OnPredictedEarlyInput(pApplyInput);
}

struct CVoteOptionServer *CGameContext::GetVoteOption(int Index)
{
	CVoteOptionServer *pCurrent;
	for(pCurrent = m_pVoteOptionFirst;
		Index > 0 && pCurrent;
		Index--, pCurrent = pCurrent->m_pNext)
		;

	if(Index > 0)
		return nullptr;
	return pCurrent;
}

void CGameContext::ProgressVoteOptions(int ClientID)
{
	CPlayer *pPl = m_apPlayers[ClientID];

	if(pPl->m_SendVoteIndex == -1)
		return; // we didn't start sending options yet

	if(pPl->m_SendVoteIndex > m_NumVoteOptions)
		return; // shouldn't happen / fail silently

	int VotesLeft = m_NumVoteOptions - pPl->m_SendVoteIndex;
	int NumVotesToSend = minimum(g_Config.m_SvSendVotesPerTick, VotesLeft);

	if(!VotesLeft)
	{
		// player has up to date vote option list
		return;
	}

	// build vote option list msg
	int CurIndex = 0;

	CNetMsg_Sv_VoteOptionListAdd OptionMsg;
	OptionMsg.m_pDescription0 = "";
	OptionMsg.m_pDescription1 = "";
	OptionMsg.m_pDescription2 = "";
	OptionMsg.m_pDescription3 = "";
	OptionMsg.m_pDescription4 = "";
	OptionMsg.m_pDescription5 = "";
	OptionMsg.m_pDescription6 = "";
	OptionMsg.m_pDescription7 = "";
	OptionMsg.m_pDescription8 = "";
	OptionMsg.m_pDescription9 = "";
	OptionMsg.m_pDescription10 = "";
	OptionMsg.m_pDescription11 = "";
	OptionMsg.m_pDescription12 = "";
	OptionMsg.m_pDescription13 = "";
	OptionMsg.m_pDescription14 = "";

	// get current vote option by index
	CVoteOptionServer *pCurrent = GetVoteOption(pPl->m_SendVoteIndex);

	while(CurIndex < NumVotesToSend && pCurrent != nullptr)
	{
		switch(CurIndex)
		{
		case 0: OptionMsg.m_pDescription0 = pCurrent->m_aDescription; break;
		case 1: OptionMsg.m_pDescription1 = pCurrent->m_aDescription; break;
		case 2: OptionMsg.m_pDescription2 = pCurrent->m_aDescription; break;
		case 3: OptionMsg.m_pDescription3 = pCurrent->m_aDescription; break;
		case 4: OptionMsg.m_pDescription4 = pCurrent->m_aDescription; break;
		case 5: OptionMsg.m_pDescription5 = pCurrent->m_aDescription; break;
		case 6: OptionMsg.m_pDescription6 = pCurrent->m_aDescription; break;
		case 7: OptionMsg.m_pDescription7 = pCurrent->m_aDescription; break;
		case 8: OptionMsg.m_pDescription8 = pCurrent->m_aDescription; break;
		case 9: OptionMsg.m_pDescription9 = pCurrent->m_aDescription; break;
		case 10: OptionMsg.m_pDescription10 = pCurrent->m_aDescription; break;
		case 11: OptionMsg.m_pDescription11 = pCurrent->m_aDescription; break;
		case 12: OptionMsg.m_pDescription12 = pCurrent->m_aDescription; break;
		case 13: OptionMsg.m_pDescription13 = pCurrent->m_aDescription; break;
		case 14: OptionMsg.m_pDescription14 = pCurrent->m_aDescription; break;
		}

		CurIndex++;
		pCurrent = pCurrent->m_pNext;
	}

	// send msg
	OptionMsg.m_NumOptions = NumVotesToSend;
	Server()->SendPackMsg(&OptionMsg, MSGFLAG_VITAL, ClientID);

	pPl->m_SendVoteIndex += NumVotesToSend;
}

void CGameContext::OnClientEnter(int ClientID)
{
	m_pController->OnPlayerConnect(m_apPlayers[ClientID]);

	{
		int Empty = -1;
		for(int i = 0; i < MAX_CLIENTS; i++)
		{
			if(!Server()->ClientIngame(i))
			{
				Empty = i;
				break;
			}
		}
		CNetMsg_Sv_Chat Msg;
		Msg.m_Team = 0;
		Msg.m_ClientID = Empty;
		Msg.m_pMessage = "Do you know someone who uses a bot? Please report them to the moderators.";
		m_apPlayers[ClientID]->m_EligibleForFinishCheck = time_get();
		Server()->SendPackMsg(&Msg, MSGFLAG_VITAL | MSGFLAG_NORECORD, ClientID);
	}

	IServer::CClientInfo Info;
	Server()->GetClientInfo(ClientID, &Info);
	if(Info.m_GotDDNetVersion)
	{
		if(OnClientDDNetVersionKnown(ClientID))
			return; // kicked
	}

	if(!Server()->ClientPrevIngame(ClientID))
	{
		if(g_Config.m_SvWelcome[0] != 0)
			Chat(ClientID, g_Config.m_SvWelcome);

		if(g_Config.m_SvShowOthersDefault > SHOW_OTHERS_OFF)
		{
			if(g_Config.m_SvShowOthers)
				Chat(ClientID, "You can see other players. To disable this use DDNet client and type /showothers .");

			m_apPlayers[ClientID]->m_ShowOthers = g_Config.m_SvShowOthersDefault;
		}
	}
	m_VoteUpdate = true;

	// send active vote
	if(m_VoteCloseTime)
		SendVoteSet(ClientID);

	Server()->ExpireServerInfo();

	//CPlayer *pNewPlayer = m_apPlayers[ClientID];
	mem_zero(&m_aLastPlayerInput[ClientID], sizeof(m_aLastPlayerInput[ClientID]));
	m_aPlayerHasInput[ClientID] = false;

	// initial chat delay
	if(g_Config.m_SvChatInitialDelay != 0 && m_apPlayers[ClientID]->m_JoinTick > m_NonEmptySince + 10 * Server()->TickSpeed())
	{
		NETADDR Addr;
		Server()->GetClientAddr(ClientID, &Addr);
		Mute(&Addr, g_Config.m_SvChatInitialDelay, Server()->ClientName(ClientID), "Initial chat delay", true);
	}
}

void CGameContext::OnClientConnected(int ClientID)
{
	bool Empty = true;
	for(auto &pPlayer : m_apPlayers)
	{
		if(pPlayer)
		{
			Empty = false;
			break;
		}
	}
	if(Empty)
	{
		m_NonEmptySince = Server()->Tick();
	}

	if(m_apPlayers[ClientID])
		delete m_apPlayers[ClientID];

	const int AllocMemoryCell = ClientID + m_WorldID * MAX_CLIENTS;
	m_apPlayers[ClientID] = new(AllocMemoryCell) CPlayer(this, NextUniqueClientID, ClientID, TEAM_RED);
	NextUniqueClientID += 1;

#ifdef CONF_DEBUG
	if(g_Config.m_DbgDummies)
	{
		if(ClientID >= MAX_CLIENTS - g_Config.m_DbgDummies)
			return;
	}
#endif

	SendMotd(ClientID);
	Server()->ExpireServerInfo();
}

// change the world
void CGameContext::PrepareClientChangeWorld(int ClientID)
{
	int64_t UniqueID = -1;
	if(m_apPlayers[ClientID])
	{
		UniqueID = m_apPlayers[ClientID]->GetUniqueCID();
		m_apPlayers[ClientID]->KillCharacter(WEAPON_WORLD);
		delete m_apPlayers[ClientID];
		m_apPlayers[ClientID] = nullptr;
	}
	const int AllocMemoryCell = ClientID + m_WorldID * MAX_CLIENTS;
	m_apPlayers[ClientID] = new(AllocMemoryCell) CPlayer(this, UniqueID != -1 ? UniqueID : NextUniqueClientID, ClientID, TEAM_RED);
}

void CGameContext::OnClientDrop(int ClientID, const char *pReason)
{
	AbortVoteKickOnDisconnect(ClientID);
	m_pController->OnPlayerDisconnect(m_apPlayers[ClientID], pReason);
	delete m_apPlayers[ClientID];
	m_apPlayers[ClientID] = nullptr;

	//(void)m_pController->CheckTeamBalance();
	m_VoteUpdate = true;

	// update spectator modes
	for(auto &pPlayer : m_apPlayers)
	{
		if(pPlayer && pPlayer->m_SpectatorID == ClientID)
			pPlayer->m_SpectatorID = SPEC_FREEVIEW;
	}

	// update conversation targets
	for(auto &pPlayer : m_apPlayers)
	{
		if(pPlayer && pPlayer->m_LastWhisperTo == ClientID)
			pPlayer->m_LastWhisperTo = -1;
	}

	Server()->ExpireServerInfo();
}

bool CGameContext::OnClientDDNetVersionKnown(int ClientID)
{
	IServer::CClientInfo Info;
	Server()->GetClientInfo(ClientID, &Info);
	int ClientVersion = Info.m_DDNetVersion;
	dbg_msg("ddnet", "cid=%d version=%d", ClientID, ClientVersion);

	// Autoban known bot versions.
	if(g_Config.m_SvBannedVersions[0] != '\0' && IsVersionBanned(ClientVersion))
	{
		Server()->Kick(ClientID, "unsupported client");
		return true;
	}

	CPlayer *pPlayer = m_apPlayers[ClientID];
	if(ClientVersion >= VERSION_DDNET_GAMETICK)
		pPlayer->m_TimerType = g_Config.m_SvDefaultTimerType;

	// And report correct tunings.
	if(ClientVersion < VERSION_DDNET_EARLY_VERSION)
		SendTuningParams(ClientID, pPlayer->m_TuneZone);

	// Tell old clients to update.
	if(ClientVersion < VERSION_DDNET_UPDATER_FIXED && g_Config.m_SvClientSuggestionOld[0] != '\0')
		Broadcast(ClientID, GamePriority::MAIN, 100, g_Config.m_SvClientSuggestionOld);
	// Tell known bot clients that they're botting and we know it.
	if(((ClientVersion >= 15 && ClientVersion < 100) || ClientVersion == 502) && g_Config.m_SvClientSuggestionBot[0] != '\0')
		Broadcast(ClientID, GamePriority::MAIN, 100, g_Config.m_SvClientSuggestionBot);

	return false;
}

void CGameContext::CensorMessage(char *pCensoredMessage, const char *pMessage, int Size)
{
	str_copy(pCensoredMessage, pMessage, Size);

	for(auto &Item : m_vCensorlist)
	{
		char *pCurLoc = pCensoredMessage;
		do
		{
			pCurLoc = (char *)str_utf8_find_nocase(pCurLoc, Item.c_str());
			if(pCurLoc)
			{
				memset(pCurLoc, '*', Item.length());
				pCurLoc++;
			}
		} while(pCurLoc);
	}
}

void CGameContext::OnMessage(int MsgID, CUnpacker *pUnpacker, int ClientID)
{
	void *pRawMsg = m_NetObjHandler.SecureUnpackMsg(MsgID, pUnpacker);
	if(!pRawMsg)
		return;

	CPlayer *pPlayer = m_apPlayers[ClientID];

	if(Server()->ClientIngame(ClientID))
	{
		if(MsgID == NETMSGTYPE_CL_SAY)
		{
			CNetMsg_Cl_Say *pMsg = (CNetMsg_Cl_Say *)pRawMsg;
			if(!str_utf8_check(pMsg->m_pMessage))
			{
				return;
			}
			bool Check = !pPlayer->m_NotEligibleForFinish && pPlayer->m_EligibleForFinishCheck + 10 * time_freq() >= time_get();
			if(Check && str_comp(pMsg->m_pMessage, "xd sure chillerbot.png is lyfe") == 0 && pMsg->m_Team == 0)
			{
				pPlayer->m_NotEligibleForFinish = true;
				dbg_msg("hack", "bot detected, cid=%d", ClientID);
				return;
			}
			//int Team = pMsg->m_Team;

			// trim right and set maximum length to 256 utf8-characters
			int Length = 0;
			const char *p = pMsg->m_pMessage;
			const char *pEnd = nullptr;
			while(*p)
			{
				const char *pStrOld = p;
				int Code = str_utf8_decode(&p);

				// check if unicode is not empty
				if(!str_utf8_isspace(Code))
				{
					pEnd = nullptr;
				}
				else if(pEnd == nullptr)
					pEnd = pStrOld;

				if(++Length >= 256)
				{
					*(const_cast<char *>(p)) = 0;
					break;
				}
			}
			if(pEnd != nullptr)
				*(const_cast<char *>(pEnd)) = 0;

			// drop empty and autocreated spam messages (more than 32 characters per second)
			if(Length == 0 || (pMsg->m_pMessage[0] != '/' && (g_Config.m_SvSpamprotection && pPlayer->m_LastChat && pPlayer->m_LastChat + Server()->TickSpeed() * ((31 + Length) / 32) > Server()->Tick())))
				return;

			if(pMsg->m_pMessage[0] == '/')
			{
				CSingleton<CCommandsChatProcessor>::Get()->Execute(pMsg, this, pPlayer);
				return;
			}

			pPlayer->UpdatePlaytime();
			char aCensoredMessage[256];
			CensorMessage(aCensoredMessage, pMsg->m_pMessage, sizeof(aCensoredMessage));
			SendChat(ClientID, CHAT_ALL, aCensoredMessage, ClientID);
		}
		else if(MsgID == NETMSGTYPE_CL_CALLVOTE)
		{
			if(RateLimitPlayerVote(ClientID) || m_VoteCloseTime)
				return;

			m_apPlayers[ClientID]->UpdatePlaytime();

			m_VoteType = VOTE_TYPE_UNKNOWN;
			char aChatmsg[512] = {0};
			char aDesc[VOTE_DESC_LENGTH] = {0};
			char aCmd[VOTE_CMD_LENGTH] = {0};
			char aReason[VOTE_REASON_LENGTH] = "No reason given";
			CNetMsg_Cl_CallVote *pMsg = (CNetMsg_Cl_CallVote *)pRawMsg;
			if(!str_utf8_check(pMsg->m_Type) || !str_utf8_check(pMsg->m_Reason) || !str_utf8_check(pMsg->m_Value))
			{
				return;
			}
			if(pMsg->m_Reason[0])
			{
				str_copy(aReason, pMsg->m_Reason, sizeof(aReason));
			}

			if(str_comp_nocase(pMsg->m_Type, "option") == 0)
			{
				int Authed = Server()->GetAuthedState(ClientID);
				CVoteOptionServer *pOption = m_pVoteOptionFirst;
				while(pOption)
				{
					if(str_comp_nocase(pMsg->m_Value, pOption->m_aDescription) == 0)
					{
						if(!Console()->LineIsValid(pOption->m_aCommand))
						{
							Chat(ClientID, "Invalid option");
							return;
						}
						str_format(aChatmsg, sizeof(aChatmsg), "'%s' called vote to change server option '%s' (%s)", Server()->ClientName(ClientID),
							pOption->m_aDescription, aReason);
						str_format(aDesc, sizeof(aDesc), "%s", pOption->m_aDescription);

						if((str_endswith(pOption->m_aCommand, "random_map") || str_endswith(pOption->m_aCommand, "random_unfinished_map")) && str_length(aReason) == 1 && aReason[0] >= '0' && aReason[0] <= '5')
						{
							int Stars = aReason[0] - '0';
							str_format(aCmd, sizeof(aCmd), "%s %d", pOption->m_aCommand, Stars);
						}
						else
						{
							str_format(aCmd, sizeof(aCmd), "%s", pOption->m_aCommand);
						}
						
						break;
					}

					pOption = pOption->m_pNext;
				}

				if(!pOption)
				{
					if(Authed != AUTHED_ADMIN) // allow admins to call any vote they want
					{
						str_format(aChatmsg, sizeof(aChatmsg), "'%s' isn't an option on this server", pMsg->m_Value);
						Chat(ClientID, aChatmsg);
						return;
					}
					else
					{
						str_format(aChatmsg, sizeof(aChatmsg), "'%s' called vote to change server option '%s'", Server()->ClientName(ClientID), pMsg->m_Value);
						str_format(aDesc, sizeof(aDesc), "%s", pMsg->m_Value);
						str_format(aCmd, sizeof(aCmd), "%s", pMsg->m_Value);
					}
				}

				m_VoteType = VOTE_TYPE_OPTION;
			}
			else if(str_comp_nocase(pMsg->m_Type, "kick") == 0)
			{
				int Authed = Server()->GetAuthedState(ClientID);
				if(!Authed && time_get() < m_apPlayers[ClientID]->m_Last_KickVote + (time_freq() * 5))
					return;
				else if(!Authed && time_get() < m_apPlayers[ClientID]->m_Last_KickVote + (time_freq() * g_Config.m_SvVoteKickDelay))
				{
					str_format(aChatmsg, sizeof(aChatmsg), "There's a %d second wait time between kick votes for each player please wait %d second(s)",
						g_Config.m_SvVoteKickDelay,
						(int)(((m_apPlayers[ClientID]->m_Last_KickVote + (m_apPlayers[ClientID]->m_Last_KickVote * time_freq())) / time_freq()) - (time_get() / time_freq())));
					Chat(ClientID, aChatmsg);
					m_apPlayers[ClientID]->m_Last_KickVote = time_get();
					return;
				}
				//else if(!g_Config.m_SvVoteKick)
				else if(!g_Config.m_SvVoteKick && !Authed) // allow admins to call kick votes even if they are forbidden
				{
					Chat(ClientID, "Server does not allow voting to kick players");
					m_apPlayers[ClientID]->m_Last_KickVote = time_get();
					return;
				}

				if(g_Config.m_SvVoteKickMin)
				{
					char aaAddresses[MAX_CLIENTS][NETADDR_MAXSTRSIZE] = {{0}};
					for(int i = 0; i < MAX_CLIENTS; i++)
					{
						if(m_apPlayers[i])
						{
							Server()->GetClientAddr(i, aaAddresses[i], NETADDR_MAXSTRSIZE);
						}
					}
					int NumPlayers = 0;
					for(int i = 0; i < MAX_CLIENTS; ++i)
					{
						if(m_apPlayers[i] && m_apPlayers[i]->GetTeam() != TEAM_SPECTATORS)
						{
							NumPlayers++;
							for(int j = 0; j < i; j++)
							{
								if(m_apPlayers[j] && m_apPlayers[j]->GetTeam() != TEAM_SPECTATORS)
								{
									if(str_comp(aaAddresses[i], aaAddresses[j]) == 0)
									{
										NumPlayers--;
										break;
									}
								}
							}
						}
					}

					if(NumPlayers < g_Config.m_SvVoteKickMin)
					{
						str_format(aChatmsg, sizeof(aChatmsg), "Kick voting requires %d players", g_Config.m_SvVoteKickMin);
						Chat(ClientID, aChatmsg);
						return;
					}
				}

				int KickID = str_toint(pMsg->m_Value);

				if(KickID < 0 || KickID >= MAX_CLIENTS || !m_apPlayers[KickID])
				{
					Chat(ClientID, "Invalid client id to kick");
					return;
				}
				if(KickID == ClientID)
				{
					Chat(ClientID, "You can't kick yourself");
					return;
				}
				if(!Server()->ReverseTranslate(KickID, ClientID))
				{
					return;
				}
				int KickedAuthed = Server()->GetAuthedState(KickID);
				if(KickedAuthed > Authed)
				{
					Chat(ClientID, "You can't kick authorized players");
					m_apPlayers[ClientID]->m_Last_KickVote = time_get();
					char aBufKick[128];
					str_format(aBufKick, sizeof(aBufKick), "'%s' called for vote to kick you", Server()->ClientName(ClientID));
					Chat(KickID, aBufKick);
					return;
				}
				str_format(aChatmsg, sizeof(aChatmsg), "'%s' called for vote to kick '%s' (%s)", Server()->ClientName(ClientID), Server()->ClientName(KickID), aReason);

				if(!g_Config.m_SvVoteKickBantime)
				{
					str_format(aCmd, sizeof(aCmd), "kick %d Kicked by vote", KickID);
					str_format(aDesc, sizeof(aDesc), "Kick '%s'", Server()->ClientName(KickID));
				}
				else
				{
					char aAddrStr[NETADDR_MAXSTRSIZE] = {0};
					Server()->GetClientAddr(KickID, aAddrStr, sizeof(aAddrStr));
					str_format(aCmd, sizeof(aCmd), "ban %s %d Banned by vote", aAddrStr, g_Config.m_SvVoteKickBantime);
					str_format(aDesc, sizeof(aDesc), "Ban '%s'", Server()->ClientName(KickID));
				}
				m_apPlayers[ClientID]->m_Last_KickVote = time_get();
				m_VoteType = VOTE_TYPE_KICK;
				m_VoteVictim = KickID;
			}
			else if(str_comp_nocase(pMsg->m_Type, "spectate") == 0)
			{
				if(!g_Config.m_SvVoteSpectate)
				{
					Chat(ClientID, "Server does not allow voting to move players to spectators");
					return;
				}

				int SpectateID = str_toint(pMsg->m_Value);

				if(SpectateID < 0 || SpectateID >= MAX_CLIENTS || !m_apPlayers[SpectateID] || m_apPlayers[SpectateID]->GetTeam() == TEAM_SPECTATORS)
				{
					Chat(ClientID, "Invalid client id to move");
					return;
				}
				if(SpectateID == ClientID)
				{
					Chat(ClientID, "You can't move yourself");
					return;
				}
				if(!Server()->ReverseTranslate(SpectateID, ClientID))
				{
					return;
				}

				m_VoteType = VOTE_TYPE_SPECTATE;
				m_VoteVictim = SpectateID;
			}
		}
		else if(MsgID == NETMSGTYPE_CL_VOTE)
		{
			if(!m_VoteCloseTime)
				return;

			if(g_Config.m_SvSpamprotection && pPlayer->m_LastVoteTry && pPlayer->m_LastVoteTry + Server()->TickSpeed() * 3 > Server()->Tick())
				return;

			int64_t Now = Server()->Tick();

			pPlayer->m_LastVoteTry = Now;
			pPlayer->UpdatePlaytime();

			CNetMsg_Cl_Vote *pMsg = (CNetMsg_Cl_Vote *)pRawMsg;
			if(!pMsg->m_Vote)
				return;

			pPlayer->m_Vote = pMsg->m_Vote;
			pPlayer->m_VotePos = ++m_VotePos;
			m_VoteUpdate = true;
		}
		else if(MsgID == NETMSGTYPE_CL_SETTEAM && !m_World.m_Paused)
		{
			CNetMsg_Cl_SetTeam *pMsg = (CNetMsg_Cl_SetTeam *)pRawMsg;

			if(pPlayer->GetTeam() == pMsg->m_Team || (g_Config.m_SvSpamprotection && pPlayer->m_LastSetTeam && pPlayer->m_LastSetTeam + Server()->TickSpeed() * g_Config.m_SvTeamChangeDelay > Server()->Tick()))
				return;

		}
		else if(MsgID == NETMSGTYPE_CL_ISDDNETLEGACY)
		{
			IServer::CClientInfo Info;
			Server()->GetClientInfo(ClientID, &Info);
			if(Info.m_GotDDNetVersion)
			{
				return;
			}
			int DDNetVersion = pUnpacker->GetInt();
			if(pUnpacker->Error() || DDNetVersion < 0)
			{
				DDNetVersion = VERSION_DDRACE;
			}
			Server()->SetClientDDNetVersion(ClientID, DDNetVersion);
			OnClientDDNetVersionKnown(ClientID);
		}
		else if(MsgID == NETMSGTYPE_CL_SHOWOTHERSLEGACY)
		{
			if(g_Config.m_SvShowOthers && !g_Config.m_SvShowOthersDefault)
			{
				CNetMsg_Cl_ShowOthersLegacy *pMsg = (CNetMsg_Cl_ShowOthersLegacy *)pRawMsg;
				pPlayer->m_ShowOthers = pMsg->m_Show;
			}
		}
		else if(MsgID == NETMSGTYPE_CL_SHOWOTHERS)
		{
			if(g_Config.m_SvShowOthers && !g_Config.m_SvShowOthersDefault)
			{
				CNetMsg_Cl_ShowOthers *pMsg = (CNetMsg_Cl_ShowOthers *)pRawMsg;
				pPlayer->m_ShowOthers = pMsg->m_Show;
			}
		}
		else if(MsgID == NETMSGTYPE_CL_SHOWDISTANCE)
		{
			CNetMsg_Cl_ShowDistance *pMsg = (CNetMsg_Cl_ShowDistance *)pRawMsg;
			pPlayer->m_ShowDistance = vec2(pMsg->m_X, pMsg->m_Y);
		}
		else if(MsgID == NETMSGTYPE_CL_SETSPECTATORMODE && !m_World.m_Paused)
		{
			CNetMsg_Cl_SetSpectatorMode *pMsg = (CNetMsg_Cl_SetSpectatorMode *)pRawMsg;

			pMsg->m_SpectatorID = clamp(pMsg->m_SpectatorID, (int)SPEC_FOLLOW, MAX_CLIENTS - 1);

			if(pMsg->m_SpectatorID >= 0)
				if(!Server()->ReverseTranslate(pMsg->m_SpectatorID, ClientID))
					return;

			if((g_Config.m_SvSpamprotection && pPlayer->m_LastSetSpectatorMode && pPlayer->m_LastSetSpectatorMode + Server()->TickSpeed() / 4 > Server()->Tick()))
				return;

			pPlayer->m_LastSetSpectatorMode = Server()->Tick();
			pPlayer->UpdatePlaytime();
			if(pMsg->m_SpectatorID >= 0 && (!m_apPlayers[pMsg->m_SpectatorID] || m_apPlayers[pMsg->m_SpectatorID]->GetTeam() == TEAM_SPECTATORS))
				Chat(ClientID, "Invalid spectator id used");
			else
				pPlayer->m_SpectatorID = pMsg->m_SpectatorID;
		}
		else if(MsgID == NETMSGTYPE_CL_CHANGEINFO)
		{
			if(g_Config.m_SvSpamprotection && pPlayer->m_LastChangeInfo && pPlayer->m_LastChangeInfo + Server()->TickSpeed() * g_Config.m_SvInfoChangeDelay > Server()->Tick())
				return;

			CNetMsg_Cl_ChangeInfo *pMsg = (CNetMsg_Cl_ChangeInfo *)pRawMsg;
			if(!str_utf8_check(pMsg->m_pName) || !str_utf8_check(pMsg->m_pClan) || !str_utf8_check(pMsg->m_pSkin))
			{
				return;
			}
			pPlayer->m_LastChangeInfo = Server()->Tick();
			pPlayer->UpdatePlaytime();

			// set infos
			if(Server()->WouldClientNameChange(ClientID, pMsg->m_pName) && !ProcessSpamProtection(ClientID))
			{
				char aOldName[MAX_NAME_LENGTH];
				str_copy(aOldName, Server()->ClientName(ClientID), sizeof(aOldName));

				Server()->SetClientName(ClientID, pMsg->m_pName);

				char aChatText[256];
				str_format(aChatText, sizeof(aChatText), "'%s' changed name to '%s'", aOldName, Server()->ClientName(ClientID));
				SendChat(-1, CGameContext::CHAT_ALL, aChatText);
			}

			Server()->SetClientClan(ClientID, pMsg->m_pClan);
			Server()->SetClientCountry(ClientID, pMsg->m_Country);

			str_copy(pPlayer->m_TeeInfos.m_aSkinName, pMsg->m_pSkin, sizeof(pPlayer->m_TeeInfos.m_aSkinName));
			pPlayer->m_TeeInfos.m_UseCustomColor = pMsg->m_UseCustomColor;
			pPlayer->m_TeeInfos.m_ColorBody = pMsg->m_ColorBody;
			pPlayer->m_TeeInfos.m_ColorFeet = pMsg->m_ColorFeet;

			Server()->ExpireServerInfo();
		}
		else if(MsgID == NETMSGTYPE_CL_EMOTICON && !m_World.m_Paused)
		{
			CNetMsg_Cl_Emoticon *pMsg = (CNetMsg_Cl_Emoticon *)pRawMsg;

			if(g_Config.m_SvSpamprotection && pPlayer->m_LastEmote && pPlayer->m_LastEmote + Server()->TickSpeed() * g_Config.m_SvEmoticonDelay > Server()->Tick())
				return;

			pPlayer->m_LastEmote = Server()->Tick();
			pPlayer->UpdatePlaytime();

			SendEmoticon(ClientID, pMsg->m_Emoticon);
			CCharacter *pChr = pPlayer->GetCharacter();
			if(pChr && g_Config.m_SvEmotionalTees && pPlayer->m_EyeEmoteEnabled)
			{
				int EmoteType = EMOTE_NORMAL;
				switch(pMsg->m_Emoticon)
				{
				case EMOTICON_EXCLAMATION:
				case EMOTICON_GHOST:
				case EMOTICON_QUESTION:
				case EMOTICON_WTF:
					EmoteType = EMOTE_SURPRISE;
					break;
				case EMOTICON_DOTDOT:
				case EMOTICON_DROP:
				case EMOTICON_ZZZ:
					EmoteType = EMOTE_BLINK;
					break;
				case EMOTICON_EYES:
				case EMOTICON_HEARTS:
				case EMOTICON_MUSIC:
					EmoteType = EMOTE_HAPPY;
					break;
				case EMOTICON_OOP:
				case EMOTICON_SORRY:
				case EMOTICON_SUSHI:
					EmoteType = EMOTE_PAIN;
					break;
				case EMOTICON_DEVILTEE:
				case EMOTICON_SPLATTEE:
				case EMOTICON_ZOMG:
					EmoteType = EMOTE_ANGRY;
					break;
				default:
					break;
				}
				pChr->SetEmote(EmoteType, Server()->Tick() + 2 * Server()->TickSpeed());
			}
		}
		else if(MsgID == NETMSGTYPE_CL_KILL && !m_World.m_Paused)
		{
			if(m_VoteCloseTime && m_VoteCreator == ClientID && (IsKickVote() || IsSpecVote()))
			{
				Chat(ClientID, "You are running a vote please try again after the vote is done!");
				return;
			}
			if(pPlayer->m_LastKill && pPlayer->m_LastKill + Server()->TickSpeed() * g_Config.m_SvKillDelay > Server()->Tick())
				return;
			if(pPlayer->IsPaused())
				return;

			CCharacter *pChr = pPlayer->GetCharacter();
			if(!pChr)
				return;

			pPlayer->m_LastKill = Server()->Tick();
			pPlayer->KillCharacter(WEAPON_SELF);
			pPlayer->Respawn();
		}
	}
	if(MsgID == NETMSGTYPE_CL_STARTINFO)
	{
		if(pPlayer->m_IsReady)
			return;

		CNetMsg_Cl_StartInfo *pMsg = (CNetMsg_Cl_StartInfo *)pRawMsg;

		if(!str_utf8_check(pMsg->m_pName))
		{
			Server()->Kick(ClientID, "name is not valid utf8");
			return;
		}
		if(!str_utf8_check(pMsg->m_pClan))
		{
			Server()->Kick(ClientID, "clan is not valid utf8");
			return;
		}
		if(!str_utf8_check(pMsg->m_pSkin))
		{
			Server()->Kick(ClientID, "skin is not valid utf8");
			return;
		}

		pPlayer->m_LastChangeInfo = Server()->Tick();

		// set start infos
		Server()->SetClientName(ClientID, pMsg->m_pName);
		// trying to set client name can delete the player object, check if it still exists
		if(!m_apPlayers[ClientID])
		{
			return;
		}
		Server()->SetClientClan(ClientID, pMsg->m_pClan);
		Server()->SetClientCountry(ClientID, pMsg->m_Country);
		str_copy(pPlayer->m_TeeInfos.m_aSkinName, pMsg->m_pSkin, sizeof(pPlayer->m_TeeInfos.m_aSkinName));
		pPlayer->m_TeeInfos.m_UseCustomColor = pMsg->m_UseCustomColor;
		pPlayer->m_TeeInfos.m_ColorBody = pMsg->m_ColorBody;
		pPlayer->m_TeeInfos.m_ColorFeet = pMsg->m_ColorFeet;

		// send clear vote options
		CNetMsg_Sv_VoteClearOptions ClearMsg;
		Server()->SendPackMsg(&ClearMsg, MSGFLAG_VITAL, ClientID);

		// begin sending vote options
		pPlayer->m_SendVoteIndex = 0;

		// send tuning parameters to client
		SendTuningParams(ClientID, pPlayer->m_TuneZone);

		// client is ready to enter
		pPlayer->m_IsReady = true;
		CNetMsg_Sv_ReadyToEnter m;
		Server()->SendPackMsg(&m, MSGFLAG_VITAL | MSGFLAG_FLUSH, ClientID);

		Server()->ExpireServerInfo();
	}
}

void CGameContext::OnClearClientData(int ClientID)
{
	// todo add here clear client data
}


void CGameContext::AddVote(const char *pDescription, const char *pCommand)
{
	if(m_NumVoteOptions == MAX_VOTE_OPTIONS)
	{
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", "maximum number of vote options reached");
		return;
	}

	// check for valid option
	if(!Console()->LineIsValid(pCommand) || str_length(pCommand) >= VOTE_CMD_LENGTH)
	{
		char aBuf[256];
		str_format(aBuf, sizeof(aBuf), "skipped invalid command '%s'", pCommand);
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
		return;
	}
	while(*pDescription == ' ')
		pDescription++;
	if(str_length(pDescription) >= VOTE_DESC_LENGTH || *pDescription == 0)
	{
		char aBuf[256];
		str_format(aBuf, sizeof(aBuf), "skipped invalid option '%s'", pDescription);
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
		return;
	}

	// check for duplicate entry
	CVoteOptionServer *pOption = m_pVoteOptionFirst;
	while(pOption)
	{
		if(str_comp_nocase(pDescription, pOption->m_aDescription) == 0)
		{
			char aBuf[256];
			str_format(aBuf, sizeof(aBuf), "option '%s' already exists", pDescription);
			Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
			return;
		}
		pOption = pOption->m_pNext;
	}

	// add the option
	++m_NumVoteOptions;
	int Len = str_length(pCommand);

	pOption = (CVoteOptionServer *)m_pVoteOptionHeap->Allocate(sizeof(CVoteOptionServer) + Len, alignof(CVoteOptionServer));
	pOption->m_pNext = nullptr;
	pOption->m_pPrev = m_pVoteOptionLast;
	if(pOption->m_pPrev)
		pOption->m_pPrev->m_pNext = pOption;
	m_pVoteOptionLast = pOption;
	if(!m_pVoteOptionFirst)
		m_pVoteOptionFirst = pOption;

	str_copy(pOption->m_aDescription, pDescription, sizeof(pOption->m_aDescription));
	mem_copy(pOption->m_aCommand, pCommand, Len + 1);
}

void CGameContext::OnConsoleInit()
{
	m_pServer = Kernel()->RequestInterface<IServer>();
	m_pConfig = Kernel()->RequestInterface<IConfigManager>()->Values();
	m_pConsole = Kernel()->RequestInterface<IConsole>();
	m_pEngine = Kernel()->RequestInterface<IEngine>();
	m_pStorage = Kernel()->RequestInterface<IStorage>();

	CSingleton<CCommandsChatProcessor>::Get()->Init(m_pServer, m_pConsole);
	CSingleton<CCommandsRconProcessor>::Get()->Init(m_pServer, m_pConsole);
}

void CGameContext::OnInit(int WorldID/*class IKernel *pKernel*/)
{
	m_WorldID = WorldID;
	m_pServer = Kernel()->RequestInterface<IServer>();
	m_pConfig = Kernel()->RequestInterface<IConfigManager>()->Values();
	m_pConsole = Kernel()->RequestInterface<IConsole>();
	m_pEngine = Kernel()->RequestInterface<IEngine>();
	m_pStorage = Kernel()->RequestInterface<IStorage>();
	m_pAntibot = Kernel()->RequestInterface<IAntibot>();
	m_pAntibot->RoundStart(this);
	m_World.SetGameServer(this);
	m_Events.SetGameServer(this);

	m_GameUuid = RandomUuid();

	uint64_t aSeed[2];
	secure_random_fill(aSeed, sizeof(aSeed));
	m_Prng.Seed(aSeed);
	m_World.m_Core.m_pPrng = &m_Prng;

	DeleteTempfile();

	//if(!data) // only load once
	//data = load_data_from_memory(internal_data);

	for(int i = 0; i < NUM_NETOBJTYPES; i++)
		Server()->SnapSetStaticsize(i, m_NetObjHandler.GetObjSize(i));

	m_Layers.Init(Kernel(), WorldID);
	m_Collision.Init(&m_Layers);
	m_World.m_Core.InitSwitchers(m_Collision.m_HighestSwitchNumber);

	char aMapName[IO_MAX_PATH_LENGTH];
	int MapSize;
	SHA256_DIGEST MapSha256;
	unsigned MapCrc;
	Server()->GetMapInfo(m_WorldID, aMapName, sizeof(aMapName), &MapSize, &MapSha256, &MapCrc);

	// reset everything here
	//world = new GAMEWORLD;
	//players = new CPlayer[MAX_CLIENTS];

	// Reset Tunezones
	CTuningParams TuningParams;
	for(int i = 0; i < NUM_TUNEZONES; i++)
	{
		TuningList()[i] = TuningParams;
		TuningList()[i].Set("gun_curvature", 0);
		TuningList()[i].Set("gun_speed", 1400);
		TuningList()[i].Set("shotgun_curvature", 0);
		TuningList()[i].Set("shotgun_speed", 500);
		TuningList()[i].Set("shotgun_speeddiff", 0);
	}

	for(int i = 0; i < NUM_TUNEZONES; i++)
	{
		// Send no text by default when changing tune zones.
		m_aaZoneEnterMsg[i][0] = 0;
		m_aaZoneLeaveMsg[i][0] = 0;
	}
	// Reset Tuning
	if(g_Config.m_SvTuneReset)
	{
		ResetTuning();
	}
	else
	{
		Tuning()->Set("gun_speed", 1400);
		Tuning()->Set("gun_curvature", 0);
		Tuning()->Set("shotgun_speed", 500);
		Tuning()->Set("shotgun_speeddiff", 0);
		Tuning()->Set("shotgun_curvature", 0);
	}

	if(g_Config.m_SvDDRaceTuneReset)
	{
		g_Config.m_SvHit = 1;
		g_Config.m_SvEndlessDrag = 0;
		g_Config.m_SvOldLaser = 0;
		g_Config.m_SvOldTeleportHook = 0;
		g_Config.m_SvOldTeleportWeapons = 0;
		g_Config.m_SvTeleportHoldHook = 0;
		g_Config.m_SvShowOthersDefault = SHOW_OTHERS_OFF;

		for(auto &Switcher : Switchers())
			Switcher.m_Initial = true;
	}

	Console()->ExecuteFile(g_Config.m_SvResetFile, -1);

	LoadMapSettings();

	m_pController = new CGameControllerDDRace(this);

	const char *pCensorFilename = "censorlist.txt";
	IOHANDLE File = Storage()->OpenFile(pCensorFilename, IOFLAG_READ | IOFLAG_SKIP_BOM, IStorage::TYPE_ALL);
	if(!File)
	{
		dbg_msg("censorlist", "failed to open '%s'", pCensorFilename);
	}
	else
	{
		CLineReader LineReader;
		LineReader.Init(File);
		char *pLine;
		while((pLine = LineReader.Get()))
		{
			m_vCensorlist.emplace_back(pLine);
		}
		io_close(File);
	}

	// create all entities from the game layer
	CMapItemLayerTilemap *pTileMap = m_Layers.GameLayer();
	CTile *pTiles = (CTile *)Kernel()->RequestInterface<IMap>(WorldID)->GetData(pTileMap->m_Data);

	CTile *pFront = nullptr;
	CSwitchTile *pSwitch = nullptr;
	if(m_Layers.FrontLayer())
		pFront = (CTile *)Kernel()->RequestInterface<IMap>(WorldID)->GetData(m_Layers.FrontLayer()->m_Front);
	if(m_Layers.SwitchLayer())
		pSwitch = (CSwitchTile *)Kernel()->RequestInterface<IMap>(WorldID)->GetData(m_Layers.SwitchLayer()->m_Switch);

	for(int y = 0; y < pTileMap->m_Height; y++)
	{
		for(int x = 0; x < pTileMap->m_Width; x++)
		{
			int Index = pTiles[y * pTileMap->m_Width + x].m_Index;

			if(Index == TILE_OLDLASER)
			{
				g_Config.m_SvOldLaser = 1;
				dbg_msg("game_layer", "found old laser tile");
			}
			else if(Index == TILE_NPC)
			{
				m_Tuning.Set("player_collision", 0);
				dbg_msg("game_layer", "found no collision tile");
			}
			else if(Index == TILE_EHOOK)
			{
				g_Config.m_SvEndlessDrag = 1;
				dbg_msg("game_layer", "found unlimited hook time tile");
			}
			else if(Index == TILE_NOHIT)
			{
				g_Config.m_SvHit = 0;
				dbg_msg("game_layer", "found no weapons hitting others tile");
			}
			else if(Index == TILE_NPH)
			{
				m_Tuning.Set("player_hooking", 0);
				dbg_msg("game_layer", "found no player hooking tile");
			}

			if(Index >= ENTITY_OFFSET)
			{
				vec2 Pos(x * 32.0f + 16.0f, y * 32.0f + 16.0f);
				m_pController->OnEntity(Index - ENTITY_OFFSET, Pos, LAYER_GAME, pTiles[y * pTileMap->m_Width + x].m_Flags);
			}

			if(pFront)
			{
				Index = pFront[y * pTileMap->m_Width + x].m_Index;
				if(Index == TILE_OLDLASER)
				{
					g_Config.m_SvOldLaser = 1;
					dbg_msg("front_layer", "found old laser tile");
				}
				else if(Index == TILE_NPC)
				{
					m_Tuning.Set("player_collision", 0);
					dbg_msg("front_layer", "found no collision tile");
				}
				else if(Index == TILE_EHOOK)
				{
					g_Config.m_SvEndlessDrag = 1;
					dbg_msg("front_layer", "found unlimited hook time tile");
				}
				else if(Index == TILE_NOHIT)
				{
					g_Config.m_SvHit = 0;
					dbg_msg("front_layer", "found no weapons hitting others tile");
				}
				else if(Index == TILE_NPH)
				{
					m_Tuning.Set("player_hooking", 0);
					dbg_msg("front_layer", "found no player hooking tile");
				}
				if(Index >= ENTITY_OFFSET)
				{
					vec2 Pos(x * 32.0f + 16.0f, y * 32.0f + 16.0f);
					m_pController->OnEntity(Index - ENTITY_OFFSET, Pos, LAYER_FRONT, pFront[y * pTileMap->m_Width + x].m_Flags);
				}
			}
			if(pSwitch)
			{
				Index = pSwitch[y * pTileMap->m_Width + x].m_Type;
				// TODO: Add off by default door here
				// if (Index == TILE_DOOR_OFF)
				if(Index >= ENTITY_OFFSET)
				{
					vec2 Pos(x * 32.0f + 16.0f, y * 32.0f + 16.0f);
					m_pController->OnEntity(Index - ENTITY_OFFSET, Pos, LAYER_SWITCH, pSwitch[y * pTileMap->m_Width + x].m_Flags, pSwitch[y * pTileMap->m_Width + x].m_Number);
				}
			}
		}
	}

	//game.world.insert_entity(game.Controller);

	if(GIT_SHORTREV_HASH)
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "git-revision", GIT_SHORTREV_HASH);

#ifdef CONF_DEBUG
	if(g_Config.m_DbgDummies)
	{
		for(int i = 0; i < g_Config.m_DbgDummies; i++)
		{
			OnClientConnected(MAX_CLIENTS - i - 1);
		}
	}
#endif
}

void CGameContext::DeleteTempfile()
{
	if(m_aDeleteTempfile[0] != 0)
	{
		Storage()->RemoveFile(m_aDeleteTempfile, IStorage::TYPE_SAVE);
		m_aDeleteTempfile[0] = 0;
	}
}

void CGameContext::LoadMapSettings()
{
	IMap *pMap = Kernel()->RequestInterface<IMap>();
	int Start, Num;
	pMap->GetType(MAPITEMTYPE_INFO, &Start, &Num);
	for(int i = Start; i < Start + Num; i++)
	{
		int ItemID;
		CMapItemInfoSettings *pItem = (CMapItemInfoSettings *)pMap->GetItem(i, nullptr, &ItemID);
		int ItemSize = pMap->GetItemSize(i);
		if(!pItem || ItemID != 0)
			continue;

		if(ItemSize < (int)sizeof(CMapItemInfoSettings))
			break;
		if(!(pItem->m_Settings > -1))
			break;

		int Size = pMap->GetDataSize(pItem->m_Settings);
		char *pSettings = (char *)pMap->GetData(pItem->m_Settings);
		char *pNext = pSettings;
		while(pNext < pSettings + Size)
		{
			int StrSize = str_length(pNext) + 1;
			Console()->ExecuteLine(pNext, IConsole::CLIENT_ID_GAME);
			pNext += StrSize;
		}
		pMap->UnloadData(pItem->m_Settings);
		break;
	}

	char aBuf[IO_MAX_PATH_LENGTH];
	str_format(aBuf, sizeof(aBuf), "maps/%s.map.cfg", g_Config.m_SvMap);
	Console()->ExecuteFile(aBuf, IConsole::CLIENT_ID_NO_GAME);
}

void CGameContext::OnSnap(int ClientID)
{
	CPlayer *pPlayer = m_apPlayers[ClientID];
	if(!pPlayer || pPlayer->GetPlayerWorldID() != GetWorldID())
		return;

	m_pController->Snap(ClientID);

	for(auto &pPlayer : m_apPlayers)
	{
		if(pPlayer)
			pPlayer->Snap(ClientID);
	}

	if(ClientID > -1)
		m_apPlayers[ClientID]->FakeSnap();

	m_World.Snap(ClientID);
	m_Events.Snap(ClientID);
}
void CGameContext::OnPreSnap() {}
void CGameContext::OnPostSnap()
{
	m_Events.Clear();
}

bool CGameContext::IsClientReady(int ClientID) const
{
	return m_apPlayers[ClientID] && m_apPlayers[ClientID]->m_IsReady;
}

bool CGameContext::IsClientPlayer(int ClientID) const
{
	return m_apPlayers[ClientID] && m_apPlayers[ClientID]->GetTeam() != TEAM_SPECTATORS;
}

CUuid CGameContext::GameUuid() const { return m_GameUuid; }
const char *CGameContext::GameType() const { return m_pController && m_pController->m_pGameType ? m_pController->m_pGameType : ""; }
const char *CGameContext::Version() const { return GAME_VERSION; }
const char *CGameContext::NetVersion() const { return GAME_NETVERSION; }

IGameServer *CreateGameServer() { return new CGameContext; }

bool CGameContext::PlayerCollision()
{
	float Temp;
	m_Tuning.Get("player_collision", &Temp);
	return Temp != 0.0f;
}

bool CGameContext::PlayerHooking()
{
	float Temp;
	m_Tuning.Get("player_hooking", &Temp);
	return Temp != 0.0f;
}

float CGameContext::PlayerJetpack()
{
	float Temp;
	m_Tuning.Get("player_jetpack", &Temp);
	return Temp;
}

void CGameContext::OnSetAuthed(int ClientID, int Level)
{
	if(m_apPlayers[ClientID])
	{
		char aBuf[512], aIP[NETADDR_MAXSTRSIZE];
		Server()->GetClientAddr(ClientID, aIP, sizeof(aIP));
		str_format(aBuf, sizeof(aBuf), "ban %s %d Banned by vote", aIP, g_Config.m_SvVoteKickBantime);
		if(!str_comp_nocase(m_aVoteCommand, aBuf) && Level > Server()->GetAuthedState(m_VoteCreator))
		{
			m_VoteEnforce = CGameContext::VOTE_ENFORCE_NO_ADMIN;
			Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "CGameContext", "Vote aborted by authorized login.");
		}
	}
}

int CGameContext::ProcessSpamProtection(int ClientID, bool RespectChatInitialDelay)
{
	if(!m_apPlayers[ClientID])
		return 0;
	if(g_Config.m_SvSpamprotection && m_apPlayers[ClientID]->m_LastChat && m_apPlayers[ClientID]->m_LastChat + Server()->TickSpeed() * g_Config.m_SvChatDelay > Server()->Tick())
		return 1;
	else if(g_Config.m_SvDnsblChat && Server()->DnsblBlack(ClientID))
	{
		Chat(ClientID, "Players are not allowed to chat from VPNs at this time");
		return 1;
	}
	else
		m_apPlayers[ClientID]->m_LastChat = Server()->Tick();

	NETADDR Addr;
	Server()->GetClientAddr(ClientID, &Addr);

	int Muted = 0;
	for(int i = 0; i < m_NumMutes && Muted <= 0; i++)
	{
		if(!net_addr_comp_noport(&Addr, &m_aMutes[i].m_Addr))
		{
			if(RespectChatInitialDelay || m_aMutes[i].m_InitialChatDelay)
				Muted = (m_aMutes[i].m_Expire - Server()->Tick()) / Server()->TickSpeed();
		}
	}

	if(Muted > 0)
	{
		Chat(ClientID, "You are not permitted to talk for the next {INT} seconds.", Muted);
		return 1;
	}

	if(g_Config.m_SvSpamMuteDuration && (m_apPlayers[ClientID]->m_ChatScore += g_Config.m_SvChatPenalty) > g_Config.m_SvChatThreshold)
	{
		Mute(&Addr, g_Config.m_SvSpamMuteDuration, Server()->ClientName(ClientID));
		m_apPlayers[ClientID]->m_ChatScore = 0;
		return 1;
	}

	return 0;
}

void CGameContext::ResetTuning()
{
	CTuningParams TuningParams;
	m_Tuning = TuningParams;
	Tuning()->Set("gun_speed", 1400);
	Tuning()->Set("gun_curvature", 0);
	Tuning()->Set("shotgun_speed", 500);
	Tuning()->Set("shotgun_speeddiff", 0);
	Tuning()->Set("shotgun_curvature", 0);
	SendTuningParams(-1);
}

bool CGameContext::TryVoteMute(const NETADDR *pAddr, int Secs)
{
	// find a matching vote mute for this ip, update expiration time if found
	for(int i = 0; i < m_NumVoteMutes; i++)
	{
		if(net_addr_comp_noport(&m_aVoteMutes[i].m_Addr, pAddr) == 0)
		{
			m_aVoteMutes[i].m_Expire = Server()->Tick() + Secs * Server()->TickSpeed();
			return true;
		}
	}

	// nothing to update create new one
	if(m_NumVoteMutes < MAX_VOTE_MUTES)
	{
		m_aVoteMutes[m_NumVoteMutes].m_Addr = *pAddr;
		m_aVoteMutes[m_NumVoteMutes].m_Expire = Server()->Tick() + Secs * Server()->TickSpeed();
		m_NumVoteMutes++;
		return true;
	}
	// no free slot found
	Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "votemute", "vote mute array is full");
	return false;
}

bool CGameContext::VoteMute(const NETADDR *pAddr, int Secs, const char *pDisplayName, int AuthedID)
{
	if(!TryVoteMute(pAddr, Secs))
		return false;

	if(!pDisplayName)
		return true;

	char aBuf[128];
	str_format(aBuf, sizeof aBuf, "'%s' banned '%s' for %d seconds from voting.",
		Server()->ClientName(AuthedID), pDisplayName, Secs);
	Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "votemute", aBuf);
	return true;
}

bool CGameContext::VoteUnmute(const NETADDR *pAddr, const char *pDisplayName, int AuthedID)
{
	for(int i = 0; i < m_NumVoteMutes; i++)
	{
		if(net_addr_comp_noport(&m_aVoteMutes[i].m_Addr, pAddr) == 0)
		{
			m_NumVoteMutes--;
			m_aVoteMutes[i] = m_aVoteMutes[m_NumVoteMutes];
			if(pDisplayName)
			{
				char aBuf[128];
				str_format(aBuf, sizeof aBuf, "'%s' unbanned '%s' from voting.",
					Server()->ClientName(AuthedID), pDisplayName);
				Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "voteunmute", aBuf);
			}
			return true;
		}
	}
	return false;
}

bool CGameContext::TryMute(const NETADDR *pAddr, int Secs, const char *pReason, bool InitialChatDelay)
{
	// find a matching mute for this ip, update expiration time if found
	for(int i = 0; i < m_NumMutes; i++)
	{
		if(net_addr_comp_noport(&m_aMutes[i].m_Addr, pAddr) == 0)
		{
			const int NewExpire = Server()->Tick() + Secs * Server()->TickSpeed();
			if(NewExpire > m_aMutes[i].m_Expire)
			{
				m_aMutes[i].m_Expire = NewExpire;
				str_copy(m_aMutes[i].m_aReason, pReason, sizeof(m_aMutes[i].m_aReason));
				m_aMutes[i].m_InitialChatDelay = InitialChatDelay;
			}
			return true;
		}
	}

	// nothing to update create new one
	if(m_NumMutes < MAX_MUTES)
	{
		m_aMutes[m_NumMutes].m_Addr = *pAddr;
		m_aMutes[m_NumMutes].m_Expire = Server()->Tick() + Secs * Server()->TickSpeed();
		str_copy(m_aMutes[m_NumMutes].m_aReason, pReason, sizeof(m_aMutes[m_NumMutes].m_aReason));
		m_aMutes[m_NumMutes].m_InitialChatDelay = InitialChatDelay;
		m_NumMutes++;
		return true;
	}
	// no free slot found
	Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "mutes", "mute array is full");
	return false;
}

void CGameContext::Mute(const NETADDR *pAddr, int Secs, const char *pDisplayName, const char *pReason, bool InitialChatDelay)
{
	if(Secs <= 0)
		return;
	if(!TryMute(pAddr, Secs, pReason, InitialChatDelay))
		return;
	if(InitialChatDelay)
		return;
	if(!pDisplayName)
		return;

	char aBuf[128];
	if(pReason[0])
		str_format(aBuf, sizeof aBuf, "'%s' has been muted for %d seconds (%s)", pDisplayName, Secs, pReason);
	else
		str_format(aBuf, sizeof aBuf, "'%s' has been muted for %d seconds", pDisplayName, Secs);
	SendChat(-1, CHAT_ALL, aBuf);
}

bool CheckClientID2(int ClientID)
{
	return ClientID >= 0 && ClientID < MAX_CLIENTS;
}

void CGameContext::Whisper(int ClientID, char *pStr)
{
	char *pName;
	char *pMessage;
	int Error = 0;

	if(ProcessSpamProtection(ClientID))
		return;

	pStr = str_skip_whitespaces(pStr);

	int Victim;

	// add token
	if(*pStr == '"')
	{
		pStr++;

		pName = pStr;
		char *pDst = pStr; // we might have to process escape data
		while(true)
		{
			if(pStr[0] == '"')
			{
				break;
			}
			else if(pStr[0] == '\\')
			{
				if(pStr[1] == '\\')
					pStr++; // skip due to escape
				else if(pStr[1] == '"')
					pStr++; // skip due to escape
			}
			else if(pStr[0] == 0)
			{
				Error = 1;
				break;
			}

			*pDst = *pStr;
			pDst++;
			pStr++;
		}

		if(!Error)
		{
			// write null termination
			*pDst = 0;

			pStr++;

			for(Victim = 0; Victim < MAX_CLIENTS; Victim++)
				if(str_comp(pName, Server()->ClientName(Victim)) == 0)
					break;
		}
	}
	else
	{
		pName = pStr;
		while(true)
		{
			if(pStr[0] == 0)
			{
				Error = 1;
				break;
			}
			if(pStr[0] == ' ')
			{
				pStr[0] = 0;
				for(Victim = 0; Victim < MAX_CLIENTS; Victim++)
					if(str_comp(pName, Server()->ClientName(Victim)) == 0)
						break;

				pStr[0] = ' ';

				if(Victim < MAX_CLIENTS)
					break;
			}
			pStr++;
		}
	}

	if(pStr[0] != ' ')
	{
		Error = 1;
	}

	*pStr = 0;
	pStr++;

	pMessage = pStr;

	if(Error)
	{
		Chat(ClientID, "Invalid whisper");
		return;
	}

	if(Victim >= MAX_CLIENTS || !CheckClientID2(Victim))
	{
		Chat(ClientID, "No player with name \"{STR}\" found", pName);
		return;
	}

	WhisperID(ClientID, Victim, pMessage);
}

void CGameContext::WhisperID(int ClientID, int VictimID, const char *pMessage)
{
	if(!CheckClientID2(ClientID))
		return;

	if(!CheckClientID2(VictimID))
		return;

	if(m_apPlayers[ClientID])
		m_apPlayers[ClientID]->m_LastWhisperTo = VictimID;

	char aCensoredMessage[256];
	CensorMessage(aCensoredMessage, pMessage, sizeof(aCensoredMessage));
	if(GetClientVersion(ClientID) >= VERSION_DDNET_WHISPER)
	{
		CNetMsg_Sv_Chat Msg;
		Msg.m_Team = CHAT_WHISPER_SEND;
		Msg.m_ClientID = VictimID;
		Msg.m_pMessage = aCensoredMessage;
		Server()->SendPackMsg(&Msg, MSGFLAG_VITAL | MSGFLAG_NORECORD, ClientID);
	}
	else
	{
		Chat(ClientID, "[→ {STR}] {STR}", Server()->ClientName(VictimID), aCensoredMessage);
	}

	if(GetClientVersion(VictimID) >= VERSION_DDNET_WHISPER)
	{
		CNetMsg_Sv_Chat Msg2;
		Msg2.m_Team = CHAT_WHISPER_RECV;
		Msg2.m_ClientID = ClientID;
		Msg2.m_pMessage = aCensoredMessage;
		Server()->SendPackMsg(&Msg2, MSGFLAG_VITAL | MSGFLAG_NORECORD, VictimID);
	}
	else
	{
		Chat(ClientID, "[← {STR}] {STR}", Server()->ClientName(ClientID), aCensoredMessage);
	}
}

void CGameContext::Converse(int ClientID, char *pStr)
{
	CPlayer *pPlayer = m_apPlayers[ClientID];
	if(!pPlayer)
		return;

	if(ProcessSpamProtection(ClientID))
		return;

	if(pPlayer->m_LastWhisperTo < 0)
		Chat(ClientID, "You do not have an ongoing conversation. Whisper to someone to start one");
	else
	{
		WhisperID(ClientID, pPlayer->m_LastWhisperTo, pStr);
	}
}

bool CGameContext::IsVersionBanned(int Version)
{
	char aVersion[16];
	str_format(aVersion, sizeof(aVersion), "%d", Version);

	return str_in_list(g_Config.m_SvBannedVersions, ",", aVersion);
}

int CGameContext::GetClientVersion(int ClientID) const
{
	IServer::CClientInfo Info = {nullptr};
	Server()->GetClientInfo(ClientID, &Info);
	return Info.m_DDNetVersion;
}

int64_t CGameContext::ClientsMaskExcludeClientVersionAndHigher(int Version)
{
	int64_t Mask = 0;
	for(int i = 0; i < MAX_CLIENTS; ++i)
	{
		if(GetClientVersion(i) >= Version)
			continue;
		Mask |= 1LL << i;
	}
	return Mask;
}

bool CGameContext::PlayerModerating() const
{
	return std::any_of(std::begin(m_apPlayers), std::end(m_apPlayers), [](const CPlayer *pPlayer) { return pPlayer && pPlayer->m_Moderating; });
}

void CGameContext::ForceVote(int EnforcerID, bool Success)
{
	// check if there is a vote running
	if(!m_VoteCloseTime)
		return;

	m_VoteEnforce = Success ? CGameContext::VOTE_ENFORCE_YES_ADMIN : CGameContext::VOTE_ENFORCE_NO_ADMIN;
	m_VoteEnforcer = EnforcerID;

	char aBuf[256];
	const char *pOption = Success ? "yes" : "no";
	str_format(aBuf, sizeof(aBuf), "authorized player forced vote %s", pOption);
	Chat(-1, aBuf);
	str_format(aBuf, sizeof(aBuf), "forcing vote %s", pOption);
	Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
}

bool CGameContext::RateLimitPlayerVote(int ClientID)
{
	int64_t Now = Server()->Tick();
	int64_t TickSpeed = Server()->TickSpeed();
	CPlayer *pPlayer = m_apPlayers[ClientID];

	if(g_Config.m_SvRconVote && !Server()->GetAuthedState(ClientID))
	{
		Chat(ClientID, "You can only vote after logging in.");
		return true;
	}

	if(g_Config.m_SvDnsblVote && Server()->DistinctClientCount() > 1)
	{
		if(m_pServer->DnsblPending(ClientID))
		{
			Chat(ClientID, "You are not allowed to vote because we're currently checking for VPNs. Try again in ~30 seconds.");
			return true;
		}
		else if(m_pServer->DnsblBlack(ClientID))
		{
			Chat(ClientID, "You are not allowed to vote because you appear to be using a VPN. Try connecting without a VPN or contacting an admin if you think this is a mistake.");
			return true;
		}
	}

	if(g_Config.m_SvSpamprotection && pPlayer->m_LastVoteTry && pPlayer->m_LastVoteTry + TickSpeed * 3 > Now)
		return true;

	pPlayer->m_LastVoteTry = Now;
	if(m_VoteCloseTime)
	{
		Chat(ClientID, "Wait for current vote to end before calling a new one.");
		return true;
	}

	if(Now < pPlayer->m_FirstVoteTick)
	{
		Chat(ClientID, "You must wait {INT} seconds before making your first vote.", (int)((pPlayer->m_FirstVoteTick - Now) / TickSpeed) + 1);
		return true;
	}

	int TimeLeft = pPlayer->m_LastVoteCall + TickSpeed * g_Config.m_SvVoteDelay - Now;
	if(pPlayer->m_LastVoteCall && TimeLeft > 0)
	{
		Chat(ClientID, "You must wait {INT} seconds before making another vote.", (int)(TimeLeft / TickSpeed) + 1);
		return true;
	}

	NETADDR Addr;
	Server()->GetClientAddr(ClientID, &Addr);
	int VoteMuted = 0;
	for(int i = 0; i < m_NumVoteMutes && !VoteMuted; i++)
		if(!net_addr_comp_noport(&Addr, &m_aVoteMutes[i].m_Addr))
			VoteMuted = (m_aVoteMutes[i].m_Expire - Server()->Tick()) / Server()->TickSpeed();
	for(int i = 0; i < m_NumMutes && VoteMuted == 0; i++)
	{
		if(!net_addr_comp_noport(&Addr, &m_aMutes[i].m_Addr))
			VoteMuted = (m_aMutes[i].m_Expire - Server()->Tick()) / Server()->TickSpeed();
	}
	if(VoteMuted > 0)
	{
		Chat(ClientID, "You are not permitted to vote for the next {INT} seconds.", VoteMuted);
		return true;
	}

	return false;
}
