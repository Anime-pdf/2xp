/* (c) Shereef Marzouk. See "licence DDRace.txt" and the readme.txt in the root of the distribution for more information. */
/* Based on Race mod stuff and tweaked by GreYFoX@GTi and others to fit our DDRace needs. */
#include "2xp.h"

#include <game/server/entities/character.h>
#include <game/server/player.h>
#include <game/version.h>

#define GAME "2xp"

CGCTXP::CGCTXP(class CGameContext *pGameServer) :
	IGameController(pGameServer)
{
	m_pGameType = GAME;

	InitTeleporter();
}

CGCTXP::~CGCTXP()
{
	// Nothing to clean
}

void CGCTXP::OnCharacterSpawn(CCharacter *pChr)
{
	IGameController::OnCharacterSpawn(pChr);
	pChr->SetTeleports(&m_TeleOuts, &m_TeleCheckOuts);
}

void CGCTXP::OnPlayerConnect(CPlayer *pPlayer)
{
	IGameController::OnPlayerConnect(pPlayer);
	int ClientID = pPlayer->GetCID();

	if(!Server()->ClientPrevIngame(ClientID))
	{
		char aBuf[512];
		str_format(aBuf, sizeof(aBuf), "'%s' entered and joined the %s", Server()->ClientName(ClientID), GetTeamName(pPlayer->GetTeam()));
		GameServer()->SendChat(-1, CGameContext::CHAT_ALL, aBuf, -1, CGameContext::CHAT_SIX);

		GameServer()->SendChatTarget(ClientID, "2xp Mod. Version: " GAME_VERSION);
		GameServer()->SendChatTarget(ClientID, "Based on DDNet (ddnet.tw)");
	}
}

void CGCTXP::OnPlayerDisconnect(CPlayer *pPlayer, const char *pReason)
{
	IGameController::OnPlayerDisconnect(pPlayer, pReason);
}

void CGCTXP::Tick()
{
	IGameController::Tick();
}

void CGCTXP::DoTeamChange(class CPlayer *pPlayer, int Team)
{
	IGameController::DoTeamChange(pPlayer, Team);
}

void CGCTXP::InitTeleporter()
{
	if(!GameServer()->Collision()->Layers()->TeleLayer())
		return;
	int Width = GameServer()->Collision()->Layers()->TeleLayer()->m_Width;
	int Height = GameServer()->Collision()->Layers()->TeleLayer()->m_Height;

	for(int i = 0; i < Width * Height; i++)
	{
		int Number = GameServer()->Collision()->TeleLayer()[i].m_Number;
		int Type = GameServer()->Collision()->TeleLayer()[i].m_Type;
		if(Number > 0)
		{
			if(Type == TILE_TELEOUT)
			{
				m_TeleOuts[Number - 1].push_back(
					vec2(i % Width * 32 + 16, i / Width * 32 + 16));
			}
			else if(Type == TILE_TELECHECKOUT)
			{
				m_TeleCheckOuts[Number - 1].push_back(
					vec2(i % Width * 32 + 16, i / Width * 32 + 16));
			}
		}
	}
}