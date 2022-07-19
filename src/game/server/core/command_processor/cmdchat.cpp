#include <game/server/gamecontext.h>
#include <game/server/player.h>

#include <engine/shared/config.h>
#include <game/version.h>

#include "cmdchat.h"

void CCommandsChatProcessor::Init(IServer *pServer, IConsole *pConsole)
{
	m_pServer = pServer;

#define ChatCommand(name, params, callback, help) pConsole->Register(name, params, CFGFLAG_CHAT, callback, m_pServer, help)

	ChatCommand("credits", "", ConCredits, "Shows the credits of the DDNet mod");
	ChatCommand("rules", "", ConRules, "Shows the server rules");
	ChatCommand("help", "?r[command]", ConHelp, "Shows help to command r, general help if left blank");
	ChatCommand("info", "", ConInfo, "Shows info about this server");
	ChatCommand("me", "r[message]", ConMe, "Like the famous irc command '/me says hi' will display '<yourname> says hi'");
	ChatCommand("w", "s[player name] r[message]", ConWhisper, "Whisper something to someone (private message)");
	ChatCommand("whisper", "s[player name] r[message]", ConWhisper, "Whisper something to someone (private message)");
	ChatCommand("c", "r[message]", ConConverse, "Converse with the last person you whispered to (private message)");
	ChatCommand("converse", "r[message]", ConConverse, "Converse with the last person you whispered to (private message)");
	ChatCommand("timeout", "?s[code]", ConTimeout, "Set timeout protection code s");

#undef ChatCommand
}

void CCommandsChatProcessor::ConCredits(IConsole::IResult *pResult, void *pUserData)
{
	const int ClientID = pResult->m_ClientID;
	IServer *pServer = (IServer *)pUserData;
	CGameContext *pGS = (CGameContext *)pServer->GameServer(pServer->GetClientWorldID(ClientID));

	pGS->Chat(pResult->m_ClientID, "Is nothing more than Teeworlds by Teeworlds staff");
	pGS->Chat(pResult->m_ClientID, "Based on DDNet staff (DDNet.tw/staff)");
}

void CCommandsChatProcessor::ConInfo(IConsole::IResult *pResult, void *pUserData)
{
	const int ClientID = pResult->m_ClientID;
	IServer *pServer = (IServer *)pUserData;
	CGameContext *pGS = (CGameContext *)pServer->GameServer(pServer->GetClientWorldID(ClientID));

	pGS->Chat(ClientID, "DDraceNetwork Mod. Version: " GAME_VERSION);
	if(GIT_SHORTREV_HASH)
		pGS->Chat(ClientID, "Git revision hash: {STR}", GIT_SHORTREV_HASH);
	pGS->Chat(ClientID, "For more info: /cmdlist");
}

void CCommandsChatProcessor::ConHelp(IConsole::IResult *pResult, void *pUserData)
{
	const int ClientID = pResult->m_ClientID;
	IServer *pServer = (IServer *)pUserData;
	CGameContext *pGS = (CGameContext *)pServer->GameServer(pServer->GetClientWorldID(ClientID));

	if(pResult->NumArguments() == 0)
	{
		pGS->Chat(ClientID, "cmdlist will show a list of all chat commands");
		pGS->Chat(ClientID, "/help + any command will show you the help for this command");
		pGS->Chat(ClientID, "Example /help settings will display the help about /settings");
		return;
	}

	const char *pArg = pResult->GetString(0);
	const IConsole::CCommandInfo *pCmdInfo = pGS->Console()->GetCommandInfo(pArg, CFGFLAG_CHAT, false);
	if(pCmdInfo)
	{
		if(pCmdInfo->m_pParams)
			pGS->Chat(ClientID, "Usage: {STR} {STR}", pCmdInfo->m_pName, pCmdInfo->m_pParams);
		if(pCmdInfo->m_pHelp)
			pGS->Chat(ClientID, pCmdInfo->m_pHelp);
		return;
	}

	pGS->Chat(ClientID, "Command is either unknown or you have given a blank command without any parameters.");
}

void CCommandsChatProcessor::ConRules(IConsole::IResult *pResult, void *pUserData)
{
	const int ClientID = pResult->m_ClientID;
	IServer *pServer = (IServer *)pUserData;
	CGameContext *pGS = (CGameContext *)pServer->GameServer(pServer->GetClientWorldID(ClientID));

	bool Printed = false;
#define _RL(n) g_Config.m_SvRulesLine##n
	char *pRuleLines[] = {_RL(1), _RL(2), _RL(3), _RL(4), _RL(5), _RL(6), _RL(7), _RL(8), _RL(9), _RL(10)};
	for(auto &pRuleLine : pRuleLines)
	{
		if(pRuleLine[0])
		{
			pGS->Chat(ClientID, pRuleLine);
			Printed = true;
		}
	}
	if(!Printed)
		pGS->Chat(ClientID, "No Rules Defined, Kill em all!!");
}

void CCommandsChatProcessor::ConTimeout(IConsole::IResult *pResult, void *pUserData)
{
	const int ClientID = pResult->m_ClientID;
	IServer *pServer = (IServer *)pUserData;
	CGameContext *pGS = (CGameContext *)pServer->GameServer(pServer->GetClientWorldID(ClientID));

	CPlayer *pPlayer = pGS->GetPlayer(ClientID);
	if(!pPlayer)
		return;

	const char *pTimeout = pResult->NumArguments() > 0 ? pResult->GetString(0) : pPlayer->m_aTimeoutCode;
	for(int i = 0; i < pGS->Server()->MaxClients(); i++)
	{
		if(i == ClientID || !pGS->m_apPlayers[i] || str_comp(pGS->m_apPlayers[i]->m_aTimeoutCode, pTimeout))
			continue;

		if(pGS->Server()->SetTimedOut(i, ClientID))
		{
			if(pGS->m_apPlayers[i]->GetCharacter())
				pGS->SendTuningParams(i);
			return;
		}
	}

	pGS->Server()->SetTimeoutProtected(pResult->m_ClientID);
	str_copy(pPlayer->m_aTimeoutCode, pResult->GetString(0), sizeof(pPlayer->m_aTimeoutCode));
}

void CCommandsChatProcessor::ConMe(IConsole::IResult *pResult, void *pUserData)
{
	const int ClientID = pResult->m_ClientID;
	IServer *pServer = (IServer *)pUserData;
	CGameContext *pGS = (CGameContext *)pServer->GameServer(pServer->GetClientWorldID(ClientID));

	if(!pGS->GetPlayer(ClientID))
		return;

	if(!g_Config.m_SvSlashMe)
	{
		pGS->Chat(ClientID, "/me is disabled on this server");
		return;
	}

	pGS->Chat(-1, "### {STR} {STR}", pGS->Server()->ClientName(ClientID), pResult->GetString(0));
}

void CCommandsChatProcessor::ConConverse(IConsole::IResult *pResult, void *pUserData)
{
	// This will never be called
}

void CCommandsChatProcessor::ConWhisper(IConsole::IResult *pResult, void *pUserData)
{
	// This will never be called
}

void CCommandsChatProcessor::Execute(CNetMsg_Cl_Say *pMsg, CGameContext *pGS, CPlayer *pPlayer)
{
	const int ClientID = pPlayer->GetCID();
	if(str_comp_nocase_num(pMsg->m_pMessage + 1, "w ", 2) == 0)
	{
		char aWhisperMsg[256];
		str_copy(aWhisperMsg, pMsg->m_pMessage + 3, 256);
		pGS->Whisper(pPlayer->GetCID(), aWhisperMsg);
	}
	else if(str_comp_nocase_num(pMsg->m_pMessage + 1, "whisper ", 8) == 0)
	{
		char aWhisperMsg[256];
		str_copy(aWhisperMsg, pMsg->m_pMessage + 9, 256);
		pGS->Whisper(pPlayer->GetCID(), aWhisperMsg);
	}
	else if(str_comp_nocase_num(pMsg->m_pMessage + 1, "c ", 2) == 0)
	{
		char aWhisperMsg[256];
		str_copy(aWhisperMsg, pMsg->m_pMessage + 3, 256);
		pGS->Converse(pPlayer->GetCID(), aWhisperMsg);
	}
	else if(str_comp_nocase_num(pMsg->m_pMessage + 1, "converse ", 9) == 0)
	{
		char aWhisperMsg[256];
		str_copy(aWhisperMsg, pMsg->m_pMessage + 10, 256);
		pGS->Converse(pPlayer->GetCID(), aWhisperMsg);
	}
	else
	{
		if(g_Config.m_SvSpamprotection && !str_startswith(pMsg->m_pMessage + 1, "timeout ") && pPlayer->m_LastCommands[0] && pPlayer->m_LastCommands[0] + Server()->TickSpeed() > Server()->Tick() && pPlayer->m_LastCommands[1] && pPlayer->m_LastCommands[1] + Server()->TickSpeed() > Server()->Tick() && pPlayer->m_LastCommands[2] && pPlayer->m_LastCommands[2] + Server()->TickSpeed() > Server()->Tick() && pPlayer->m_LastCommands[3] && pPlayer->m_LastCommands[3] + Server()->TickSpeed() > Server()->Tick())
			return;

		int64_t Now = Server()->Tick();
		pPlayer->m_LastCommands[pPlayer->m_LastCommandPos] = Now;
		pPlayer->m_LastCommandPos = (pPlayer->m_LastCommandPos + 1) % 4;

		char aBuf[256];
		str_format(aBuf, sizeof(aBuf), "%d used %s", ClientID, pMsg->m_pMessage);
		pGS->Console()->Print(IConsole::OUTPUT_LEVEL_DEBUG, "chat-command", aBuf);

		int PositionChar = 0;
		char aBufCommand[256] = {0};
		for(int i = 1; i < str_length(pMsg->m_pMessage); i++)
		{
			if(pMsg->m_pMessage[i] != ' ')
			{
				aBufCommand[PositionChar] = pMsg->m_pMessage[i];
				PositionChar++;
				continue;
			}
			break;
		}

		if(!pGS->Console()->IsCommand(aBufCommand, CFGFLAG_CHAT))
		{
			pGS->Chat(ClientID, "Command \"/{STR}\" not found!", aBufCommand);
			return;
		}

		pGS->Console()->ExecuteLineFlag(pMsg->m_pMessage + 1, CFGFLAG_CHAT, ClientID, false);
	}
}
