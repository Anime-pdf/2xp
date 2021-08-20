#ifndef GAME_SERVER_ENTITIES_MOD_C4_H
#define GAME_SERVER_ENTITIES_MOD_C4_H

#include <game/server/entity.h>

class CC4 : public CEntity
{
	int m_Owner;
	int m_DestroyTicks;

public:
	CC4(CGameWorld *pGameWorld, vec2 Pos, int Owner);

	virtual void Destroy();
	virtual void Tick();
	virtual void Snap(int SnappingClient);
};

#endif