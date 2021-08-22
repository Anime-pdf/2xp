/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include <engine/shared/config.h>
#include <game/mapitems.h>

#include <game/generated/protocol.h>

#include "entities/character.h"
#include "entities/pickup.h"
#include "gamecontext.h"
#include "gamecontroller.h"
#include "player.h"

#include "entities/door.h"
#include "entities/projectile.h"
#include <game/layers.h>

IGameController::IGameController(class CGameContext *pGameServer)
{
	m_pGameServer = pGameServer;
	m_pConfig = m_pGameServer->Config();
	m_pServer = m_pGameServer->Server();
	m_pGameType = "unknown";

	//
	m_GameOverTick = 0;
	m_SuddenDeath = 0;
	m_RoundStartTick = Server()->Tick();
	m_RoundCount = 0;
	m_GameFlags = 0;

	m_GameState = GAMESTATE_NONE;
	m_Warmup = 0;
}

IGameController::~IGameController()
{
}

void IGameController::DoActivityCheck()
{
	if(g_Config.m_SvInactiveKickTime == 0)
		return;
}

bool IGameController::CanSpawn(int Team, vec2 *pOutPos)
{
	if(Team == GAMETEAM_HUMAN)
	{
		if(!m_aSpawnPoints.empty())
		{
			*pOutPos = m_aSpawnPoints[m_aSpawnPoints.size() * frandom()];
			return true;
		}
	}
	else if(Team == GAMETEAM_ZOMBIE)
	{
		CPlayer *Center = GameServer()->GetRandomPlayingPlayer();

		float InnerRadius = 50.f; // TODO: CONFIG:
		float Radius = 30.F; // TODO: CONFIG:

		float RealRadius = InnerRadius + frandom() * Radius;

		for(float i = 1, rnd = frandom(), Angle = pi * 2 * rnd; i < 2 * pi; i += 0.3f, Angle = pi * 2 * (rnd + i))
		{
			vec2 Pos(RealRadius * cos(Angle), RealRadius * sin(Angle));

			if(!GameServer()->Collision()->CheckPoint(Pos))
			{
				*pOutPos = Pos;
				return true;
			}
		}
	}
	return false;
}

bool IGameController::OnEntity(int Index, vec2 Pos, int Layer, int Flags, int Number)
{
	if(Index < 0)
		return false;

	int Type = -1;
	int SubType = 0;

	if(Index >= ENTITY_SPAWN && Index <= ENTITY_SPAWN_BLUE)
	{
		m_aSpawnPoints.push_back(Pos);
	}
	else if(Index == ENTITY_DOOR)
	{
		int x, y;
		x = (Pos.x - 16.0f) / 32.0f;
		y = (Pos.y - 16.0f) / 32.0f;
		int sides[8];
		sides[0] = GameServer()->Collision()->Entity(x, y + 1, Layer);
		sides[1] = GameServer()->Collision()->Entity(x + 1, y + 1, Layer);
		sides[2] = GameServer()->Collision()->Entity(x + 1, y, Layer);
		sides[3] = GameServer()->Collision()->Entity(x + 1, y - 1, Layer);
		sides[4] = GameServer()->Collision()->Entity(x, y - 1, Layer);
		sides[5] = GameServer()->Collision()->Entity(x - 1, y - 1, Layer);
		sides[6] = GameServer()->Collision()->Entity(x - 1, y, Layer);
		sides[7] = GameServer()->Collision()->Entity(x - 1, y + 1, Layer);
		for(int i = 0; i < 8; i++)
		{
			if(sides[i] >= ENTITY_LASER_SHORT && sides[i] <= ENTITY_LASER_LONG)
			{
				new CDoor(
					&GameServer()->m_World, //GameWorld
					Pos, //Pos
					pi / 4 * i, //Rotation
					32 * 3 + 32 * (sides[i] - ENTITY_LASER_SHORT) * 3, //Length
					Number //Number
				);
			}
		}
	}
	else if(Index == ENTITY_ARMOR_1)
		Type = POWERUP_ARMOR;
	else if(Index == ENTITY_HEALTH_1)
		Type = POWERUP_HEALTH;
	else if(Index == ENTITY_WEAPON_SHOTGUN)
	{
		Type = POWERUP_WEAPON;
		SubType = WEAPON_SHOTGUN;
	}
	else if(Index == ENTITY_WEAPON_GRENADE)
	{
		Type = POWERUP_WEAPON;
		SubType = WEAPON_GRENADE;
	}
	else if(Index == ENTITY_WEAPON_LASER)
	{
		Type = POWERUP_WEAPON;
		SubType = WEAPON_LASER;
	}
	else if(Index == ENTITY_POWERUP_NINJA)
	{
		Type = POWERUP_NINJA;
		SubType = WEAPON_NINJA;
	}

	if(Type != -1)
	{
		CPickup *pPickup = new CPickup(&GameServer()->m_World, Type, SubType, Layer, Number);
		pPickup->m_Pos = Pos;
		return true;
	}

	return false;
}

void IGameController::OnEntityEnd()
{
	m_aSpawnPoints.shrink_to_fit();
}

void IGameController::OnPlayerConnect(CPlayer *pPlayer)
{
	int ClientID = pPlayer->GetCID();

	if(!Server()->ClientPrevIngame(ClientID))
	{
		char aBuf[128];
		str_format(aBuf, sizeof(aBuf), "team_join player='%d:%s' team=%d", ClientID, Server()->ClientName(ClientID), pPlayer->GetTeam());
		GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_DEBUG, "game", aBuf);
	}
}

void IGameController::OnPlayerDisconnect(class CPlayer *pPlayer, const char *pReason)
{
	pPlayer->OnDisconnect();
	int ClientID = pPlayer->GetCID();
	if(Server()->ClientIngame(ClientID))
	{
		char aBuf[512];
		if(pReason && *pReason)
			str_format(aBuf, sizeof(aBuf), "'%s' has left the game (%s)", Server()->ClientName(ClientID), pReason);
		else
			str_format(aBuf, sizeof(aBuf), "'%s' has left the game", Server()->ClientName(ClientID));
		GameServer()->SendChat(-1, CGameContext::CHAT_ALL, aBuf, -1, CGameContext::CHAT_SIX);

		str_format(aBuf, sizeof(aBuf), "leave player='%d:%s'", ClientID, Server()->ClientName(ClientID));
		GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "game", aBuf);
	}
}

void IGameController::EndRound()
{
	if(m_Warmup) // game can't end when we are running warmup
		return;

	GameServer()->m_World.m_Paused = true;
	m_GameOverTick = Server()->Tick();
	m_GameState = GAMESTATE_END;
	m_SuddenDeath = 0;
}

void IGameController::ResetGame()
{
	GameServer()->m_World.m_ResetRequested = true;
	OnReset();
}

const char *IGameController::GetTeamName(int Team)
{
	if(Team == 0)
		return "game";
	return "spectators";
}

void IGameController::StartRound()
{
	if(m_Warmup)
		return;

	m_RoundStartTick = Server()->Tick();
	m_SuddenDeath = 0;
	m_GameOverTick = 0;

	CPlayer *pRandom = GameServer()->GetRandomPlayingPlayer();

	if(pRandom)
	{
		pRandom->SetTeam(GAMETEAM_ZOMBIE);
	}

	m_GameState = GAMESTATE_RUNNING;
	GameServer()->m_World.m_Paused = false;
	Server()->DemoRecorder_HandleAutoStart();

	char aBuf[256];
	str_format(aBuf, sizeof(aBuf), "start round type='%s' teamplay='%d'", m_pGameType, m_GameFlags & GAMEFLAG_TEAMS);
	GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_DEBUG, "game", aBuf);
}

void IGameController::ChangeMap(const char *pToMap)
{
	str_copy(g_Config.m_SvMap, pToMap, sizeof(g_Config.m_SvMap));
	EndRound();
}

void IGameController::OnReset()
{
	for(auto &pPlayer : GameServer()->m_apPlayers)
		if(pPlayer)
			pPlayer->Respawn();
}

int IGameController::OnCharacterDeath(class CCharacter *pVictim, class CPlayer *pKiller, int Weapon)
{
	return 0;
}

void IGameController::OnCharacterSpawn(class CCharacter *pChr)
{
	// default health
	pChr->IncreaseHealth(10);

	// give default weapons
	pChr->GiveWeapon(WEAPON_HAMMER);
	pChr->GiveWeapon(WEAPON_GUN);
}

bool IGameController::ArePlayersEnough()
{
	int Amount = 0;

	for(auto &pPlayer : GameServer()->m_apPlayers)
		if(pPlayer && !pPlayer->Spectator())
			Amount++;

	if(Amount > 1) // TODO: CONFIG:
		return true;
	return false;
}

bool IGameController::HandleWarmup(int Seconds)
{
	if(m_Warmup)
	{
		m_Warmup--;
			
		if(!m_Warmup)
			return true;
	}
	else
	{
		if(ArePlayersEnough() && m_GameState == GAMESTATE_NONE)
		{
			ResetGame();
			DoWarmup(Seconds);
			m_GameState = GAMESTATE_START;
		}
	}

	return false;
}

void IGameController::DoWarmup(int Seconds)
{
	if(Seconds < 0)
		m_Warmup = 0;
	else
		m_Warmup = Seconds * Server()->TickSpeed();
}

bool IGameController::IsForceBalanced()
{
	return false;
}

bool IGameController::CanBeMovedOnBalance(int ClientID)
{
	return true;
}

void IGameController::Tick()
{
	if(HandleWarmup(30))
		StartRound();

	DoWinCheck();

	if(m_GameOverTick)
	{
		m_GameState = GAMESTATE_END;
		if(Server()->Tick() > m_GameOverTick + Server()->TickSpeed() * 3) // g_Config.m_SvGameOver
		{
			GameServer()->m_World.m_Paused = true;
			m_GameOverTick = 0;
			m_GameState = GAMESTATE_NONE;
			m_RoundCount++;
		}
	}

	DoActivityCheck();
}

void IGameController::Snap(int SnappingClient)
{
	CNetObj_GameInfo *pGameInfoObj = (CNetObj_GameInfo *)Server()->SnapNewItem(NETOBJTYPE_GAMEINFO, 0, sizeof(CNetObj_GameInfo));
	if(!pGameInfoObj)
		return;

	pGameInfoObj->m_GameFlags = m_GameFlags;
	pGameInfoObj->m_GameStateFlags = 0;
	if(m_GameOverTick)
		pGameInfoObj->m_GameStateFlags |= GAMESTATEFLAG_GAMEOVER;
	if(m_SuddenDeath)
		pGameInfoObj->m_GameStateFlags |= GAMESTATEFLAG_SUDDENDEATH;
	if(GameServer()->m_World.m_Paused)
		pGameInfoObj->m_GameStateFlags |= GAMESTATEFLAG_PAUSED;
	pGameInfoObj->m_RoundStartTick = m_RoundStartTick;
	pGameInfoObj->m_WarmupTimer = m_Warmup;

	pGameInfoObj->m_RoundNum = 0;
	pGameInfoObj->m_RoundCurrent = m_RoundCount + 1;

	CNetObj_GameInfoEx *pGameInfoEx = (CNetObj_GameInfoEx *)Server()->SnapNewItem(NETOBJTYPE_GAMEINFOEX, 0, sizeof(CNetObj_GameInfoEx));
	if(!pGameInfoEx)
		return;

	pGameInfoEx->m_Flags =
		GAMEINFOFLAG_PREDICT_DDRACE_TILES |
		GAMEINFOFLAG_UNLIMITED_AMMO |
		GAMEINFOFLAG_GAMETYPE_PLUS |
		GAMEINFOFLAG_ENTITIES_DDRACE;
	pGameInfoEx->m_Flags2 = 0;
	pGameInfoEx->m_Version = GAMEINFO_CURVERSION;

	if(Server()->IsSixup(SnappingClient))
	{
		protocol7::CNetObj_GameData *pGameData = static_cast<protocol7::CNetObj_GameData *>(Server()->SnapNewItem(-protocol7::NETOBJTYPE_GAMEDATA, 0, sizeof(protocol7::CNetObj_GameData)));
		if(!pGameData)
			return;

		pGameData->m_GameStartTick = m_RoundStartTick;
		pGameData->m_GameStateFlags = 0;
		if(m_GameOverTick)
			pGameData->m_GameStateFlags |= protocol7::GAMESTATEFLAG_GAMEOVER;
		if(m_SuddenDeath)
			pGameData->m_GameStateFlags |= protocol7::GAMESTATEFLAG_SUDDENDEATH;
		if(GameServer()->m_World.m_Paused)
			pGameData->m_GameStateFlags |= protocol7::GAMESTATEFLAG_PAUSED;

		pGameData->m_GameStateEndTick = 0;
	}
}

bool IGameController::CanJoinTeam(int Team, int NotThisID)
{
	if(!GameServer()->m_apPlayers[NotThisID])
		return false;
	else if(Team == TEAM_SPECTATORS)
		return true;
	else if(!GameServer()->m_apPlayers[NotThisID]->GetAccount())
		return false;
	return true;
}

int IGameController::ClampTeam(int Team)
{
	if (Team < 0)
		return TEAM_SPECTATORS;
	return 0;
}

void IGameController::DoTeamChange(CPlayer *pPlayer, int Team)
{
	pPlayer->SetSpectator(ClampTeam(Team) == TEAM_SPECTATORS);

	if(!pPlayer->Spectator())
	{
		switch(m_GameState)
		{
		case GAMESTATE_START:
			pPlayer->SetTeam(GAMETEAM_HUMAN);
			break;
		case GAMESTATE_RUNNING:
			pPlayer->SetTeam(GAMETEAM_ZOMBIE); // TODO: CONFIG: set zombie or force to spectators
			break;
		case GAMESTATE_NONE: // don't do anything, game is frozen
		case GAMESTATE_END:
		default:
			break;
		}
	}
}

void IGameController::DoWinCheck()
{

}