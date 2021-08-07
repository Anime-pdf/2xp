/* (c) Shereef Marzouk. See "licence DDRace.txt" and the readme.txt in the root of the distribution for more information. */
#ifndef GAME_SERVER_ENTITIES_DOOR_H
#define GAME_SERVER_ENTITIES_DOOR_H

#include <game/server/entity.h>

class CTrigger;

class CDoor : public CEntity
{
	vec2 m_To;
	int m_EvalTick;
	void ResetCollision();
	int m_Length;
	vec2 m_Direction;

	bool m_PreviousStatus;

	int m_LaserSnapID;
	int m_HeartSnapID;

public:
	void Open(int Tick);
	void Open();
	void Close();
	CDoor(CGameWorld *pGameWorld, vec2 Pos, float Rotation, int Length,
		int Number);

	virtual void Reset();
	virtual void Tick();
	virtual void Snap(int SnappingClient);
};

#endif // GAME_SERVER_ENTITIES_DOOR_H
