#ifndef GAME_SERVER_CORE_CONTEXT_H
#define GAME_SERVER_CORE_CONTEXT_H

enum class GamePriority : short
{
	BASIC = 0,
	MAIN,
	GLOBAL
};

enum
{
	// main
	MAX_BROADCAST_SIZE = 1024,
};

#endif
