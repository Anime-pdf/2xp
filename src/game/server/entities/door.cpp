/* (c) Shereef Marzouk. See "licence DDRace.txt" and the readme.txt in the root of the distribution for more information. */
#include <engine/shared/config.h>
#include <game/generated/protocol.h>
#include <game/server/gamecontext.h>
#include <game/server/gamemodes/2xp.h>
#include <game/server/player.h>

#include "character.h"
#include "door.h"

CDoor::CDoor(CGameWorld *pGameWorld, vec2 Pos, float Rotation, int Length,
	int Number) :
	CEntity(pGameWorld, CGameWorld::ENTTYPE_LASER)
{
	m_Number = Number;
	m_Pos = Pos;
	m_Length = Length;
	m_Direction = vec2(sin(Rotation), cos(Rotation));
	vec2 To = Pos + normalize(m_Direction) * m_Length;

	m_PreviousStatus = GameServer()->Collision()->m_pSwitchers[Number].m_Status;

	GameServer()->Collision()->IntersectNoLaser(Pos, To, &this->m_To, 0);
	ResetCollision();
	GameWorld()->InsertEntity(this);
}

void CDoor::Open(int Tick)
{
	m_EvalTick = Server()->Tick();
	Open();
}

void CDoor::ResetCollision()
{
	for(int i = 0; i < m_Length - 1; i++)
	{
		GameServer()->Collision()->SetCollisionAt(
				m_Pos.x + (m_Direction.x * i),
				m_Pos.y + (m_Direction.y * i), TILE_NOHOOK);
	}
}

void CDoor::Open()
{
	for(int i = 0; i < m_Length - 1; i++)
	{
		GameServer()->Collision()->SetCollisionAt(
				m_Pos.x + (m_Direction.x * i),
			m_Pos.y + (m_Direction.y * i), TILE_AIR);
	}
}

void CDoor::Close()
{
	for(int i = 0; i < m_Length - 1; i++)
	{
		GameServer()->Collision()->SetCollisionAt(
				m_Pos.x + (m_Direction.x * i),
				m_Pos.y + (m_Direction.y * i), TILE_NOHOOK);
	}
}

void CDoor::Reset()
{
}

void CDoor::Tick()
{
	bool CurrentStatus = GameServer()->Collision()->m_pSwitchers[m_Number].m_Status;
	if (CurrentStatus != m_PreviousStatus)
	{
		CurrentStatus ? Close() : Open();
		m_PreviousStatus = CurrentStatus;
	}
}

void CDoor::Snap(int SnappingClient)
{
	if(NetworkClipped(SnappingClient, m_Pos) && NetworkClipped(SnappingClient, m_To))
		return;

	CNetObj_Laser *pObj = static_cast<CNetObj_Laser *>(Server()->SnapNewItem(
		NETOBJTYPE_LASER, GetID(), sizeof(CNetObj_Laser)));

	if(!pObj)
		return;

	pObj->m_X = (int)m_Pos.x;
	pObj->m_Y = (int)m_Pos.y;

	CCharacter *Char = GameServer()->GetPlayerChar(SnappingClient);
	int Tick = (Server()->Tick() % Server()->TickSpeed()) % 11;

	if(SnappingClient > -1 && GameServer()->m_apPlayers[SnappingClient]->GetTeam() == TEAM_SPECTATORS && GameServer()->m_apPlayers[SnappingClient]->m_SpectatorID != SPEC_FREEVIEW)
		Char = GameServer()->GetPlayerChar(GameServer()->m_apPlayers[SnappingClient]->m_SpectatorID);

	if(Char == 0)
		return;

	if(Char->IsAlive() && GameServer()->Collision()->m_NumSwitchers > 0 && !GameServer()->Collision()->m_pSwitchers[m_Number].m_Status && (!Tick))
		return;

	if(Char->IsAlive() && GameServer()->Collision()->m_NumSwitchers > 0 && GameServer()->Collision()->m_pSwitchers[m_Number].m_Status)
	{
		pObj->m_FromX = (int)m_To.x;
		pObj->m_FromY = (int)m_To.y;
	}
	else
	{
		pObj->m_FromX = (int)m_Pos.x;
		pObj->m_FromY = (int)m_Pos.y;
	}
	pObj->m_StartTick = Server()->Tick();
}
