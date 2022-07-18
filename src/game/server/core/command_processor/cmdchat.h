/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_SERVER_CORE_COMMAND_PROCESSOR_CMDCHAT_H
#define GAME_SERVER_CORE_COMMAND_PROCESSOR_CMDCHAT_H

class CCommandsChatProcessor
{
	class IServer *m_pServer;
	IServer *Server() const { return m_pServer; }

	static void ConCredits(IConsole::IResult *pResult, void *pUserData);
	static void ConRules(IConsole::IResult *pResult, void *pUserData);
	static void ConHelp(IConsole::IResult *pResult, void *pUserData);
	static void ConInfo(IConsole::IResult *pResult, void *pUserData);
	static void ConList(IConsole::IResult *pResult, void *pUserData);
	static void ConMe(IConsole::IResult *pResult, void *pUserData);
	static void ConWhisper(IConsole::IResult *pResult, void *pUserData);
	static void ConConverse(IConsole::IResult *pResult, void *pUserData);
	static void ConTimeout(IConsole::IResult *pResult, void *pUserData);

public:
	CCommandsChatProcessor() = default;
	void Init(class IServer *pServer, class IConsole *pConsole);
	void Execute(CNetMsg_Cl_Say *pMsg, CGameContext* pGS, CPlayer *pPlayer);
};

#endif
