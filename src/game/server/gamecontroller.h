/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_SERVER_GAMECONTROLLER_H
#define GAME_SERVER_GAMECONTROLLER_H

#include <base/vmath.h>
#include <engine/map.h>

#include <deque>

/*
	Class: Game Controller
		Controls the main game logic. Keeping track of team and player score,
		winning conditions and specific game logic.
*/
class IGameController
{
	std::deque<vec2> m_aSpawnPoints;

	enum
	{
		GAMESTATE_NONE,
		GAMESTATE_START,
		GAMESTATE_RUNNING,
		GAMESTATE_END,
	};

	class CGameContext *m_pGameServer;
	struct CConfig *m_pConfig;
	class IServer *m_pServer;

protected:
	CGameContext *GameServer() const { return m_pGameServer; }
	CConfig *Config() { return m_pConfig; }
	IServer *Server() const { return m_pServer; }

	void DoActivityCheck();

	void ResetGame();

	int m_GameState;

	int m_RoundStartTick;
	int m_GameOverTick;
	int m_SuddenDeath;

	int m_Warmup;
	int m_RoundCount;

	int m_GameFlags;

public:
	enum
	{
		GAMETEAM_NONE,
		GAMETEAM_HUMAN,
		GAMETEAM_ZOMBIE
	};

	const char *m_pGameType;

	IGameController(class CGameContext *pGameServer);
	virtual ~IGameController();

	// event
	/*
		Function: OnCharacterDeath
			Called when a CCharacter in the world dies.

		Arguments:
			victim - The CCharacter that died.
			killer - The player that killed it.
			weapon - What weapon that killed it. Can be -1 for undefined
				weapon when switching team or player suicides.
	*/
	virtual int OnCharacterDeath(class CCharacter *pVictim, class CPlayer *pKiller, int Weapon);

	/*
		Function: OnCharacterSpawn
			Called when a CCharacter spawns into the game world.

		Arguments:
			chr - The CCharacter that was spawned.
	*/
	virtual void OnCharacterSpawn(class CCharacter *pChr);

	/*
		Function: OnEntity
			Called when the map is loaded to process an entity
			in the map.

		Arguments:
			index - Entity index.
			pos - Where the entity is located in the world.

		Returns:
			bool?
	*/
	virtual bool OnEntity(int Index, vec2 Pos, int Layer, int Flags, int Number = 0);
	virtual void OnEntityEnd();

	virtual void OnPlayerConnect(class CPlayer *pPlayer);
	virtual void OnPlayerDisconnect(class CPlayer *pPlayer, const char *pReason);

	void OnReset();

	// game
	void DoWarmup(int Seconds);

	void StartRound();
	void EndRound();
	void ChangeMap(const char *pToMap);

	bool IsFriendlyFire(int ClientID1, int ClientID2) { return true; }

	bool IsForceBalanced();

	/*

	*/
	virtual bool CanBeMovedOnBalance(int ClientID);

	virtual void Tick();

	virtual void Snap(int SnappingClient);

	//spawn
	virtual bool CanSpawn(int Team, vec2 *pPos);

	virtual void DoWinCheck();

	virtual void DoTeamChange(class CPlayer *pPlayer, int Team);
	/*

	*/
	virtual const char *GetTeamName(int Team);
	virtual bool CanJoinTeam(int Team, int NotThisID);
	int ClampTeam(int Team);

	bool HandleWarmup(int Seconds);
	bool ArePlayersEnough();
};

#endif
