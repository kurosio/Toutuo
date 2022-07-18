#include <game/server/gamecontext.h>
#include <game/server/entities/character.h>
#include <game/server/player.h>

#include <engine/shared/config.h>
#include <engine/shared/protocol.h>

#include "cmdrcon.h"

void CCommandsRconProcessor::Init(IServer *pServer, IConsole *pConsole)
{
	m_pServer = pServer;

#define RconCommand(name, params, callback, help) pConsole->Register(name, params, CFGFLAG_SERVER, callback, m_pServer, help)
#define RconCommandAllowsMapCfg(name, params, callback, help) pConsole->Register(name, params, CFGFLAG_SERVER|CFGFLAG_GAME, callback, m_pServer, help)
#define RconCommandStore(name, params, callback, help) pConsole->Register(name, params, CFGFLAG_SERVER|CFGFLAG_STORE, callback, m_pServer, help)

	/************************************************************************/
	/*  Self commands                                                       */
	/************************************************************************/
	RconCommand("kill_pl", "v[id]", ConKillPlayer, "Kills player v and announces the kill");
	RconCommand("tele", "?i[id] ?i[id]", ConTeleport, "Teleports player i (or you) to player i (or you to where you look at)");
	RconCommand("undeep", "", ConUnDeep, "Puts you out of deep freeze");
	RconCommand("freezehammer", "v[id]", ConFreezeHammer, "Gives a player Freeze Hammer");
	RconCommand("unfreezehammer", "v[id]", ConUnFreezeHammer, "Removes Freeze Hammer from a player");
	RconCommand("change_world", "v[id] i[id]", ConChangeWorld, "Change world for player id");

	/************************************************************************/
	/*  Tunning                                                             */
	/************************************************************************/
	RconCommandAllowsMapCfg("tune", "s[tuning] i[value]", ConTuneParam, "Tune variable to value");
	RconCommandAllowsMapCfg("toggle_tune", "s[tuning] i[value 1] i[value 2]", ConToggleTuneParam, "Toggle tune variable");
	RconCommand("tune_dump", "", ConTuneDump, "Dump tuning");

	/************************************************************************/
	/*  Mutes                                                               */
	/************************************************************************/
	RconCommand("mute", "", ConMute, "");
	RconCommand("muteid", "v[id] i[seconds] ?r[reason]", ConMuteID, "");
	RconCommand("muteip", "s[ip] i[seconds] ?r[reason]", ConMuteIP, "");
	RconCommand("unmute", "v[id]", ConUnmute, "");
	RconCommand("mutes", "", ConMutes, "");

	/************************************************************************/
	/*  Game                                                                */
	/************************************************************************/
	RconCommand("broadcast", "r[message]", ConBroadcast, "Broadcast message");
	RconCommand("say", "r[message]", ConSay, "Say in chat");

	/************************************************************************/
	/*  Chain                                                               */
	/************************************************************************/
	pConsole->Chain("sv_motd", ConchainSpecialMotdupdate, m_pServer);

#undef RconCommand
#undef RconCommandStore
#undef RconCommandAllowsMapCfg
}


/************************************************************************/
/*  Self commands                                                       */
/************************************************************************/
void CCommandsRconProcessor::ConKillPlayer(IConsole::IResult *pResult, void *pUserData)
{
	const int ClientID = pResult->m_ClientID;
	IServer *pServer = (IServer *)pUserData;
	CGameContext *pGS = (CGameContext *)pServer->GameServer(pServer->GetClientWorldID(ClientID));

	if(!pGS->GetPlayer(pResult->m_ClientID))
		return;

	const int Victim = pResult->GetVictim();
	if(CPlayer *pVictimPlayer = pGS->GetPlayer(Victim))
	{
		pVictimPlayer->KillCharacter(WEAPON_GAME);
		pGS->Chat(-1, "{STR} was killed by {STR}", pGS->Server()->ClientName(Victim), pGS->Server()->ClientName(pResult->m_ClientID));
	}
}

void CCommandsRconProcessor::ConUnDeep(IConsole::IResult *pResult, void *pUserData)
{
	const int ClientID = pResult->m_ClientID;
	IServer *pServer = (IServer *)pUserData;
	CGameContext *pGS = (CGameContext *)pServer->GameServer(pServer->GetClientWorldID(ClientID));

	CPlayer *pPlayer = pGS->GetPlayer(ClientID);
	if(!pPlayer || !pPlayer->GetCharacter())
		return;

	pPlayer->GetCharacter()->m_DeepFreeze = false;
}

void CCommandsRconProcessor::ConTeleport(IConsole::IResult *pResult, void *pUserData)
{
	IServer *pServer = (IServer *)pUserData;
	CGameContext *pGS = (CGameContext *)pServer->GameServer(pServer->GetClientWorldID(pResult->m_ClientID));

	const int Tele = pResult->NumArguments() == 2 ? pResult->GetInteger(0) : pResult->m_ClientID;
	const int TeleTo = pResult->NumArguments() ? pResult->GetInteger(pResult->NumArguments() - 1) : pResult->m_ClientID;
	const int AuthLevel = pServer->GetAuthedState(pResult->m_ClientID);

	if(Tele != pResult->m_ClientID && AuthLevel < g_Config.m_SvTeleOthersAuthLevel)
	{
		pGS->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "tele", "you aren't allowed to tele others");
		return;
	}

	CCharacter *pChr = pGS->GetPlayerChar(Tele);
	if(pChr && pGS->GetPlayerChar(TeleTo))
	{
		pChr->Core()->m_Pos = pGS->m_apPlayers[TeleTo]->m_ViewPos;
		pChr->m_Pos = pGS->m_apPlayers[TeleTo]->m_ViewPos;
		pChr->m_PrevPos = pGS->m_apPlayers[TeleTo]->m_ViewPos;
	}
}

void CCommandsRconProcessor::ConFreezeHammer(IConsole::IResult *pResult, void *pUserData)
{
	IServer *pServer = (IServer *)pUserData;
	CGameContext *pGS = (CGameContext *)pServer->GameServer(pServer->GetClientWorldID(pResult->m_ClientID));

	const int Victim = pResult->GetVictim();
	CCharacter *pChr = pGS->GetPlayerChar(Victim);
	if(!pChr)
		return;

	pGS->Chat(-1, "'{STR}' got freeze hammer!", pServer->ClientName(Victim));
	pChr->m_FreezeHammer = true;
}

void CCommandsRconProcessor::ConUnFreezeHammer(IConsole::IResult *pResult, void *pUserData)
{
	IServer *pServer = (IServer *)pUserData;
	CGameContext *pGS = (CGameContext *)pServer->GameServer(pServer->GetClientWorldID(pResult->m_ClientID));

	const int Victim = pResult->GetVictim();
	CCharacter *pChr = pGS->GetPlayerChar(Victim);
	if(!pChr)
		return;

	pGS->Chat(-1, "'{STR}' lost freeze hammer!", pServer->ClientName(Victim));
	pChr->m_FreezeHammer = false;
}

void CCommandsRconProcessor::ConChangeWorld(IConsole::IResult *pResult, void *pUserData)
{
	IServer *pServer = (IServer *)pUserData;

	const int Victim = pResult->GetVictim();
	pServer->ChangeWorld(Victim, pResult->GetInteger(1));
}


/************************************************************************/
/*  Tunning                                                             */
/************************************************************************/
void CCommandsRconProcessor::ConTuneParam(IConsole::IResult *pResult, void *pUserData)
{
	IServer *pServer = (IServer *)pUserData;
	CGameContext *pGS = (CGameContext *)pServer->GameServer(pServer->GetClientWorldID(pResult->m_ClientID));

	const char *pParamName = pResult->GetString(0);
	float NewValue = pResult->GetFloat(1);
	if(pGS->Tuning()->Set(pParamName, NewValue))
	{
		char aBuf[256];
		str_format(aBuf, sizeof(aBuf), "%s changed to %.2f", pParamName, NewValue);
		pGS->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "tuning", aBuf);
		pGS->SendTuningParams(-1);
		return;
	}

	pGS->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "tuning", "No such tuning parameter");
}

void CCommandsRconProcessor::ConToggleTuneParam(IConsole::IResult *pResult, void *pUserData)
{
	IServer *pServer = (IServer *)pUserData;
	CGameContext *pGS = (CGameContext *)pServer->GameServer(pServer->GetClientWorldID(pResult->m_ClientID));

	const char *pParamName = pResult->GetString(0);
	float OldValue;

	if(!pGS->Tuning()->Get(pParamName, &OldValue))
	{
		pGS->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "tuning", "No such tuning parameter");
		return;
	}

	float NewValue = fabs(static_cast<double>(OldValue) - static_cast<double>(pResult->GetFloat(1))) < 0.0001f ? pResult->GetFloat(2) : pResult->GetFloat(1);
	pGS->Tuning()->Set(pParamName, NewValue);

	char aBuf[256];
	str_format(aBuf, sizeof(aBuf), "%s changed to %.2f", pParamName, NewValue);
	pGS->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "tuning", aBuf);
	pGS->SendTuningParams(-1);
}

void CCommandsRconProcessor::ConTuneDump(IConsole::IResult *pResult, void *pUserData)
{
	IServer *pServer = (IServer *)pUserData;
	CGameContext *pGS = (CGameContext *)pServer->GameServer(pServer->GetClientWorldID(pResult->m_ClientID));

	char aBuf[256];
	for(int i = 0; i < pGS->Tuning()->Num(); i++)
	{
		float v;
		pGS->Tuning()->Get(i, &v);
		str_format(aBuf, sizeof(aBuf), "%s %.2f", pGS->Tuning()->ms_apNames[i], v);
		pGS->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "tuning", aBuf);
	}
}

/************************************************************************/
/*  Mutes                                                               */
/************************************************************************/
void CCommandsRconProcessor::ConMute(IConsole::IResult *pResult, void *pUserData)
{
	IServer *pServer = (IServer *)pUserData;
	CGameContext *pGS = (CGameContext *)pServer->GameServer(pServer->GetClientWorldID(pResult->m_ClientID));
	pGS->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "mutes", "Use either 'muteid <client_id> <seconds> <reason>' or 'muteip <ip> <seconds> <reason>'");
}

// mute through client id
void CCommandsRconProcessor::ConMuteID(IConsole::IResult *pResult, void *pUserData)
{
	IServer *pServer = (IServer *)pUserData;
	CGameContext *pGS = (CGameContext *)pServer->GameServer(pServer->GetClientWorldID(pResult->m_ClientID));
	const int Victim = pResult->GetVictim();
	if(!pGS->GetPlayer(Victim))
	{
		pGS->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "muteid", "Client id not found.");
		return;
	}

	NETADDR Addr;
	pServer->GetClientAddr(Victim, &Addr);
	const char *pReason = pResult->NumArguments() > 2 ? pResult->GetString(2) : "";
	pGS->Mute(&Addr, clamp(pResult->GetInteger(1), 1, 86400), pServer->ClientName(Victim), pReason);
}

// mute through ip, arguments reversed to workaround parsing
void CCommandsRconProcessor::ConMuteIP(IConsole::IResult *pResult, void *pUserData)
{
	IServer *pServer = (IServer *)pUserData;
	CGameContext *pGS = (CGameContext *)pServer->GameServer(pServer->GetClientWorldID(pResult->m_ClientID));
	NETADDR Addr;
	if(net_addr_from_str(&Addr, pResult->GetString(0)))
	{
		pGS->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "mutes", "Invalid network address to mute");
	}
	const char *pReason = pResult->NumArguments() > 2 ? pResult->GetString(2) : "";
	pGS->Mute(&Addr, clamp(pResult->GetInteger(1), 1, 86400), nullptr, pReason);
}

// unmute by mute list index
void CCommandsRconProcessor::ConUnmute(IConsole::IResult *pResult, void *pUserData)
{
	IServer *pServer = (IServer *)pUserData;
	CGameContext *pGS = (CGameContext *)pServer->GameServer(pServer->GetClientWorldID(pResult->m_ClientID));
	char aIpBuf[64];
	char aBuf[64];
	int Victim = pResult->GetVictim();

	if(Victim < 0 || Victim >= pGS->m_NumMutes)
		return;

	pGS->m_NumMutes--;
	pGS->m_aMutes[Victim] = pGS->m_aMutes[pGS->m_NumMutes];

	net_addr_str(&pGS->m_aMutes[Victim].m_Addr, aIpBuf, sizeof(aIpBuf), false);
	str_format(aBuf, sizeof(aBuf), "Unmuted %s", aIpBuf);
	pGS->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "mutes", aBuf);
}

// list mutes
void CCommandsRconProcessor::ConMutes(IConsole::IResult *pResult, void *pUserData)
{
	IServer *pServer = (IServer *)pUserData;
	CGameContext *pGS = (CGameContext *)pServer->GameServer(pServer->GetClientWorldID(pResult->m_ClientID));
	if(pGS->m_NumMutes <= 0)
	{
		pGS->m_NumMutes = 0;
		pGS->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "mutes", "There are no active mutes.");
		return;
	}

	char aIpBuf[64];
	char aBuf[128];
	pGS->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "mutes",
		"Active mutes:");
	for(int i = 0; i < pGS->m_NumMutes; i++)
	{
		net_addr_str(&pGS->m_aMutes[i].m_Addr, aIpBuf, sizeof(aIpBuf), false);
		str_format(aBuf, sizeof aBuf, "%d: \"%s\", %d seconds left (%s)", i, aIpBuf,
			(pGS->m_aMutes[i].m_Expire - pServer->Tick()) / pServer->TickSpeed(), pGS->m_aMutes[i].m_aReason);
		pGS->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "mutes", aBuf);
	}
}


/************************************************************************/
/*  Game                                                                */
/************************************************************************/
void CCommandsRconProcessor::ConBroadcast(IConsole::IResult *pResult, void *pUserData)
{
	IServer *pServer = (IServer *)pUserData;
	CGameContext *pGS = (CGameContext *)pServer->GameServer(pServer->GetClientWorldID(pResult->m_ClientID));

	char aBuf[1024];
	str_copy(aBuf, pResult->GetString(0), sizeof(aBuf));

	int i, j;
	for(i = 0, j = 0; aBuf[i]; i++, j++)
	{
		if(aBuf[i] == '\\' && aBuf[i + 1] == 'n')
		{
			aBuf[j] = '\n';
			i++;
		}
		else if(aBuf[i] == '{' || aBuf[i] == '}')
			aBuf[i] = '?';
		else if(i != j)
			aBuf[j] = aBuf[i];
	}
	aBuf[j] = '\0';
	pGS->Broadcast(-1, GamePriority::GLOBAL, 100, aBuf);
}

void CCommandsRconProcessor::ConSay(IConsole::IResult *pResult, void *pUserData)
{
	IServer *pServer = (IServer *)pUserData;
	CGameContext *pGS = (CGameContext *)pServer->GameServer(pServer->GetClientWorldID(pResult->m_ClientID));
	pGS->SendChat(-1, CGameContext::CHAT_ALL, pResult->GetString(0));
}


/************************************************************************/
/*  Chain                                                               */
/************************************************************************/
void CCommandsRconProcessor::ConchainSpecialMotdupdate(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	pfnCallback(pResult, pCallbackUserData);
	if(pResult->NumArguments())
	{
		CNetMsg_Sv_Motd Msg;
		Msg.m_pMessage = g_Config.m_SvMotd;
		IServer *pServer = (IServer *)pUserData;
		for(int i = 0; i < MAX_CLIENTS; ++i)
		{
			if(!pServer->ClientIngame(i))
				continue;

			CGameContext *pGS = (CGameContext *)pServer->GameServer(pServer->GetClientWorldID(pResult->m_ClientID));
			if(pGS->m_apPlayers[i])
			{
				pServer->SendPackMsg(&Msg, MSGFLAG_VITAL, i);
			}
		}
	}
}
