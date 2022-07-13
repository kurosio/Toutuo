/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_SERVER_SERVER_CORE_COMMAND_PROCESSOR_RCON_H
#define GAME_SERVER_SERVER_CORE_COMMAND_PROCESSOR_RCON_H

class CCommandsRconProcessor
{
	class IServer* m_pServer;
	class CGameContext* m_pGameServer;
	CGameContext* GS() const { return m_pGameServer; }
	IServer* Server() const { return m_pServer; }

	/************************************************************************/
	/*  Self commands                                                       */
	/************************************************************************/
	static void ConKillPlayer(IConsole::IResult* pResult, void* pUserData);
	static void ConUnDeep(IConsole::IResult* pResult, void* pUserData);
	static void ConJetpack(IConsole::IResult* pResult, void* pUserData);
	static void ConUnJetpack(IConsole::IResult* pResult, void* pUserData);
	static void ConTeleport(IConsole::IResult* pResult, void* pUserData);
	static void ConFreezeHammer(IConsole::IResult* pResult, void* pUserData);
	static void ConUnFreezeHammer(IConsole::IResult* pResult, void* pUserData);

	/************************************************************************/
	/*  Tunning                                                             */
	/************************************************************************/
	static void ConTuneParam(IConsole::IResult* pResult, void* pUserData);
	static void ConToggleTuneParam(IConsole::IResult* pResult, void* pUserData);
	static void ConTuneDump(IConsole::IResult* pResult, void* pUserData);

	/************************************************************************/
	/*  Mutes                                                               */
	/************************************************************************/
	static void ConMute(IConsole::IResult* pResult, void* pUserData);
	static void ConMuteID(IConsole::IResult* pResult, void* pUserData);
	static void ConMuteIP(IConsole::IResult* pResult, void* pUserData);
	static void ConUnmute(IConsole::IResult* pResult, void* pUserData);
	static void ConMutes(IConsole::IResult* pResult, void* pUserData);

	/************************************************************************/
	/*  Game                                                                */
	/************************************************************************/
	static void ConBroadcast(IConsole::IResult* pResult, void* pUserData);
	static void ConSay(IConsole::IResult* pResult, void* pUserData);
	static void ConDumpAntibot(IConsole::IResult* pResult, void* pUserData);

	/************************************************************************/
	/*  Chain                                                               */
	/************************************************************************/
	static void ConchainSpecialMotdupdate(IConsole::IResult* pResult, void* pUserData, IConsole::FCommandCallback pfnCallback, void* pCallbackUserData);

public:
	CCommandsRconProcessor() = default;
	void Init(class IServer* pServer, class IConsole *pConsole, CGameContext* pGameServer);
};

#endif