#include <game/server/gamecontext.h>
#include <game/server/entities/character.h>
#include <game/server/player.h>

#include <engine/antibot.h>
#include <engine/shared/config.h>
#include <engine/shared/protocol.h>

#include "cmdrcon.h"

void CCommandsRconProcessor::Init(IServer *pServer, IConsole *pConsole, CGameContext *pGameServer)
{
	m_pServer = pServer;
	m_pGameServer = pGameServer;

#define RconCommand(name, params, callback, help) pConsole->Register(name, params, CFGFLAG_SERVER, callback, m_pGameServer, help)
#define RconCommandAllowsMapCfg(name, params, callback, help) pConsole->Register(name, params, CFGFLAG_SERVER|CFGFLAG_GAME, callback, m_pGameServer, help)
#define RconCommandStore(name, params, callback, help) pConsole->Register(name, params, CFGFLAG_SERVER|CFGFLAG_STORE, callback, m_pGameServer, help)

	/************************************************************************/
	/*  Self commands                                                       */
	/************************************************************************/
	RconCommand("kill_pl", "v[id]", ConKillPlayer, "Kills player v and announces the kill");
	RconCommand("tele", "?i[id] ?i[id]", ConTeleport, "Teleports player i (or you) to player i (or you to where you look at)");
	RconCommand("jetpack", "", ConJetpack, "Gives jetpack to you");
	RconCommand("undeep", "", ConUnDeep, "Puts you out of deep freeze");
	RconCommand("freezehammer", "v[id]", ConFreezeHammer, "Gives a player Freeze Hammer");
	RconCommand("unfreezehammer", "v[id]", ConUnFreezeHammer, "Removes Freeze Hammer from a player");

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
	RconCommandStore("dump_antibot", "?i[seconds]", ConDumpAntibot, "Dumb antibot");

	/************************************************************************/
	/*  Chain                                                               */
	/************************************************************************/
	pConsole->Chain("sv_motd", ConchainSpecialMotdupdate, this);

#undef RconCommand
#undef RconCommandStore
#undef RconCommandAllowsMapCfg
}


/************************************************************************/
/*  Self commands                                                       */
/************************************************************************/
void CCommandsRconProcessor::ConKillPlayer(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = static_cast<CGameContext *>(pUserData);
	if(!pSelf->GetPlayer(pResult->m_ClientID))
		return;

	const int Victim = pResult->GetVictim();
	CPlayer *pVictimPlayer = pSelf->GetPlayer(Victim);
	if(pVictimPlayer)
	{
		pVictimPlayer->KillCharacter(WEAPON_GAME);
		pSelf->Chat(-1, "{STR} was killed by {STR}", pSelf->Server()->ClientName(Victim), pSelf->Server()->ClientName(pResult->m_ClientID));
	}
}

void CCommandsRconProcessor::ConUnDeep(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = static_cast<CGameContext *>(pUserData);
	CPlayer *pPlayer = pSelf->GetPlayer(pResult->m_ClientID);
	if(!pPlayer || !pPlayer->GetCharacter())
		return;

	pPlayer->GetCharacter()->m_DeepFreeze = false;
}

void CCommandsRconProcessor::ConJetpack(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = static_cast<CGameContext *>(pUserData);
	CPlayer *pPlayer = pSelf->GetPlayer(pResult->m_ClientID);
	if(!pPlayer || !pPlayer->GetCharacter())
		return;

	pPlayer->GetCharacter()->m_Jetpack = true;
}

void CCommandsRconProcessor::ConUnJetpack(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = static_cast<CGameContext *>(pUserData);
	CPlayer *pPlayer = pSelf->GetPlayer(pResult->m_ClientID);
	if(!pPlayer || !pPlayer->GetCharacter())
		return;

	pPlayer->GetCharacter()->m_Jetpack = false;
}

void CCommandsRconProcessor::ConTeleport(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = static_cast<CGameContext *>(pUserData);
	const int Tele = pResult->NumArguments() == 2 ? pResult->GetInteger(0) : pResult->m_ClientID;
	const int TeleTo = pResult->NumArguments() ? pResult->GetInteger(pResult->NumArguments() - 1) : pResult->m_ClientID;
	const int AuthLevel = pSelf->Server()->GetAuthedState(pResult->m_ClientID);

	if(Tele != pResult->m_ClientID && AuthLevel < g_Config.m_SvTeleOthersAuthLevel)
	{
		pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "tele", "you aren't allowed to tele others");
		return;
	}

	CCharacter *pChr = pSelf->GetPlayerChar(Tele);
	if(pChr && pSelf->GetPlayerChar(TeleTo))
	{
		pChr->Core()->m_Pos = pSelf->m_apPlayers[TeleTo]->m_ViewPos;
		pChr->m_Pos = pSelf->m_apPlayers[TeleTo]->m_ViewPos;
		pChr->m_PrevPos = pSelf->m_apPlayers[TeleTo]->m_ViewPos;
	}
}

void CCommandsRconProcessor::ConFreezeHammer(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = static_cast<CGameContext *>(pUserData);
	const int Victim = pResult->GetVictim();
	CCharacter *pChr = pSelf->GetPlayerChar(Victim);
	if(!pChr)
		return;

	pSelf->Chat(-1, "'{STR}' got freeze hammer!", pSelf->Server()->ClientName(Victim));
	pChr->m_FreezeHammer = true;
}

void CCommandsRconProcessor::ConUnFreezeHammer(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = static_cast<CGameContext *>(pUserData);
	const int Victim = pResult->GetVictim();
	CCharacter *pChr = pSelf->GetPlayerChar(Victim);
	if(!pChr)
		return;

	pSelf->Chat(-1, "'{STR}' lost freeze hammer!", pSelf->Server()->ClientName(Victim));
	pChr->m_FreezeHammer = false;
}


/************************************************************************/
/*  Tunning                                                             */
/************************************************************************/
void CCommandsRconProcessor::ConTuneParam(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = static_cast<CGameContext *>(pUserData);
	const char *pParamName = pResult->GetString(0);
	float NewValue = pResult->GetFloat(1);
	if(pSelf->Tuning()->Set(pParamName, NewValue))
	{
		char aBuf[256];
		str_format(aBuf, sizeof(aBuf), "%s changed to %.2f", pParamName, NewValue);
		pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "tuning", aBuf);
		pSelf->SendTuningParams(-1);
		return;
	}

	pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "tuning", "No such tuning parameter");
}

void CCommandsRconProcessor::ConToggleTuneParam(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = static_cast<CGameContext *>(pUserData);
	const char *pParamName = pResult->GetString(0);
	float OldValue;

	if(!pSelf->Tuning()->Get(pParamName, &OldValue))
	{
		pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "tuning", "No such tuning parameter");
		return;
	}

	float NewValue = fabs(static_cast<double>(OldValue) - static_cast<double>(pResult->GetFloat(1))) < 0.0001f ? pResult->GetFloat(2) : pResult->GetFloat(1);
	pSelf->Tuning()->Set(pParamName, NewValue);

	char aBuf[256];
	str_format(aBuf, sizeof(aBuf), "%s changed to %.2f", pParamName, NewValue);
	pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "tuning", aBuf);
	pSelf->SendTuningParams(-1);
}

void CCommandsRconProcessor::ConTuneDump(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = static_cast<CGameContext *>(pUserData);
	char aBuf[256];
	for(int i = 0; i < pSelf->Tuning()->Num(); i++)
	{
		float v;
		pSelf->Tuning()->Get(i, &v);
		str_format(aBuf, sizeof(aBuf), "%s %.2f", pSelf->Tuning()->ms_apNames[i], v);
		pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "tuning", aBuf);
	}
}

/************************************************************************/
/*  Mutes                                                               */
/************************************************************************/
void CCommandsRconProcessor::ConMute(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = static_cast<CGameContext *>(pUserData);
	pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "mutes", "Use either 'muteid <client_id> <seconds> <reason>' or 'muteip <ip> <seconds> <reason>'");
}

// mute through client id
void CCommandsRconProcessor::ConMuteID(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = static_cast<CGameContext *>(pUserData);
	const int Victim = pResult->GetVictim();
	if(!pSelf->GetPlayer(Victim))
	{
		pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "muteid", "Client id not found.");
		return;
	}

	NETADDR Addr;
	pSelf->Server()->GetClientAddr(Victim, &Addr);
	const char *pReason = pResult->NumArguments() > 2 ? pResult->GetString(2) : "";
	pSelf->Mute(&Addr, clamp(pResult->GetInteger(1), 1, 86400), pSelf->Server()->ClientName(Victim), pReason);
}

// mute through ip, arguments reversed to workaround parsing
void CCommandsRconProcessor::ConMuteIP(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = static_cast<CGameContext *>(pUserData);
	NETADDR Addr;
	if(net_addr_from_str(&Addr, pResult->GetString(0))) { pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "mutes", "Invalid network address to mute"); }
	const char *pReason = pResult->NumArguments() > 2 ? pResult->GetString(2) : "";
	pSelf->Mute(&Addr, clamp(pResult->GetInteger(1), 1, 86400), nullptr, pReason);
}

// unmute by mute list index
void CCommandsRconProcessor::ConUnmute(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = static_cast<CGameContext *>(pUserData);
	char aIpBuf[64];
	char aBuf[64];
	int Victim = pResult->GetVictim();

	if(Victim < 0 || Victim >= pSelf->m_NumMutes)
		return;

	pSelf->m_NumMutes--;
	pSelf->m_aMutes[Victim] = pSelf->m_aMutes[pSelf->m_NumMutes];

	net_addr_str(&pSelf->m_aMutes[Victim].m_Addr, aIpBuf, sizeof(aIpBuf), false);
	str_format(aBuf, sizeof(aBuf), "Unmuted %s", aIpBuf);
	pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "mutes", aBuf);
}

// list mutes
void CCommandsRconProcessor::ConMutes(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = static_cast<CGameContext *>(pUserData);
	if(pSelf->m_NumMutes <= 0)
	{
		pSelf->m_NumMutes = 0;
		pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "mutes", "There are no active mutes.");
		return;
	}

	char aIpBuf[64];
	char aBuf[128];
	pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "mutes",
		"Active mutes:");
	for(int i = 0; i < pSelf->m_NumMutes; i++)
	{
		net_addr_str(&pSelf->m_aMutes[i].m_Addr, aIpBuf, sizeof(aIpBuf), false);
		str_format(aBuf, sizeof aBuf, "%d: \"%s\", %d seconds left (%s)", i, aIpBuf,
			(pSelf->m_aMutes[i].m_Expire - pSelf->Server()->Tick()) / pSelf->Server()->TickSpeed(), pSelf->m_aMutes[i].m_aReason);
		pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "mutes", aBuf);
	}
}


/************************************************************************/
/*  Game                                                                */
/************************************************************************/
void CCommandsRconProcessor::ConBroadcast(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = static_cast<CGameContext *>(pUserData);

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
		else if(i != j)
			aBuf[j] = aBuf[i];
	}
	aBuf[j] = '\0';
	pSelf->Broadcast(-1, GamePriority::GLOBAL, 100, aBuf);
}

void CCommandsRconProcessor::ConSay(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = static_cast<CGameContext *>(pUserData);
	pSelf->SendChat(-1, CGameContext::CHAT_ALL, pResult->GetString(0));
}

void CCommandsRconProcessor::ConDumpAntibot(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = static_cast<CGameContext *>(pUserData);
	pSelf->Antibot()->Dump();
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
		CGameContext *pSelf = static_cast<CGameContext *>(pUserData);
		for(int i = 0; i < MAX_CLIENTS; ++i)
			if(pSelf->m_apPlayers[i])
				pSelf->Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, i);
	}
}
