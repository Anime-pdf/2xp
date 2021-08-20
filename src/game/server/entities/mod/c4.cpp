#include "c4.h"

CC4::CC4(CGameWorld *pGameWorld, vec2 Pos, int Owner) :
	CEntity(pGameWorld, CGameWorld::ENTTYPE_C4, Pos)
{
	m_Owner = Owner;
	m_DestroyTicks = 7 * Server()->TickSpeed();
	GameWorld()->InsertEntity(this);
}

void CC4::Destroy()
{
	GameServer()->CreateExplosion(m_Pos, m_Owner, WEAPON_GRENADE, false);
	delete this;
}

void CC4::Tick()
{
	if(!GameWorld()->m_Paused)
		m_DestroyTicks--;
	if(m_DestroyTicks < 0)
		Destroy();
}

void CC4::Snap(int SnappingClient)
{
	if(NetworkClipped(SnappingClient))
		return;

	CNetObj_Projectile *pObj = static_cast<CNetObj_Projectile *>(Server()->SnapNewItem(NETOBJTYPE_PROJECTILE, GetID(), sizeof(CNetObj_Projectile)));

	if(!pObj)
		return;

	pObj->m_X = m_Pos.x;
	pObj->m_Y = m_Pos.y;
	pObj->m_StartTick = Server()->Tick();
	pObj->m_VelX = 0;
	pObj->m_VelY = 0;
	pObj->m_Type = WEAPON_GRENADE;
}