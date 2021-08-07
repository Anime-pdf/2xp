#include "building.h"

CBuilding::CBuilding(CGameWorld *pGameWorld, vec2 Pos) :
	CEntity(pGameWorld, CGameWorld::ENTTYPE_BUILDING, Pos)
{
	GameServer()->Collision()->SetCollisionAt(m_Pos.x, m_Pos.y, TILE_SOLID);
	GameWorld()->InsertEntity(this);
}

void CBuilding::Destroy()
{
	if(GameServer()->Collision()->GetTile(m_Pos.x, m_Pos.y) == TILE_SOLID)
		GameServer()->Collision()->SetCollisionAt(m_Pos.x, m_Pos.y, TILE_AIR);
	GameServer()->CreateDeath(m_Pos, -1);
	delete this;
}

void CBuilding::Snap(int SnappingClient)
{
	if(NetworkClipped(SnappingClient))
		return;

	CNetObj_Pickup *pObj = static_cast<CNetObj_Pickup *>(Server()->SnapNewItem(NETOBJTYPE_PICKUP, GetID(), sizeof(CNetObj_Pickup)));

	if(!pObj)
		return;

	pObj->m_X = m_Pos.x;
	pObj->m_Y = m_Pos.y;
	pObj->m_Type = POWERUP_ARMOR;
	pObj->m_Subtype = 0;
}