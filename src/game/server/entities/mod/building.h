#ifndef GAME_SERVER_ENTITIES_MOD_BUILDING_H
#define GAME_SERVER_ENTITIES_MOD_BUILDING_H

#include <game/server/entity.h>

class CBuilding : public CEntity
{

public:
	CBuilding(CGameWorld *pGameWorld, vec2 Pos);

	virtual void Destroy();
	virtual void Snap(int SnappingClient);
};

#endif