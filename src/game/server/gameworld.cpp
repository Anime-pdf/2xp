/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */

#include "gameworld.h"
#include "entities/character.h"
#include "entity.h"
#include "gamecontext.h"
#include "gamecontroller.h"
#include "player.h"
#include <algorithm>
#include <engine/shared/config.h>
#include <utility>

#define FMT "[World] "

//////////////////////////////////////////////////
// game world
//////////////////////////////////////////////////
CGameWorld::CGameWorld()
{
	m_pGameServer = 0x0;
	m_pConfig = 0x0;
	m_pServer = 0x0;

	m_Paused = false;
	m_ResetRequested = false;
	for(auto &pFirstEntityType : m_apFirstEntityTypes)
		pFirstEntityType = 0;
}

CGameWorld::~CGameWorld()
{
	// delete all entities
	for(auto &pFirstEntityType : m_apFirstEntityTypes)
		while(pFirstEntityType)
			delete pFirstEntityType;
}

void CGameWorld::SetGameServer(CGameContext *pGameServer)
{
	m_pGameServer = pGameServer;
	m_pConfig = m_pGameServer->Config();
	m_pServer = m_pGameServer->Server();
}

CEntity *CGameWorld::FindFirst(int Type)
{
	return Type < 0 || Type >= NUM_ENTTYPES ? 0 : m_apFirstEntityTypes[Type];
}

int CGameWorld::FindEntities(vec2 Pos, float Radius, CEntity **ppEnts, int Max, int Type)
{
	if(Type < 0 || Type >= NUM_ENTTYPES)
		return 0;

	int Num = 0;
	for(CEntity *pEnt = m_apFirstEntityTypes[Type]; pEnt; pEnt = pEnt->m_pNextTypeEntity)
	{
		if(distance(pEnt->m_Pos, Pos) < Radius + pEnt->m_ProximityRadius)
		{
			if(ppEnts)
				ppEnts[Num] = pEnt;
			Num++;
			if(Num == Max)
				break;
		}
	}

	return Num;
}

void CGameWorld::InsertEntity(CEntity *pEnt)
{
#ifdef CONF_DEBUG
	for(CEntity *pCur = m_apFirstEntityTypes[pEnt->m_ObjType]; pCur; pCur = pCur->m_pNextTypeEntity)
		if(pCur == pEnt)
			spdlog::critical(FMT "Entity is already in list");
#endif

	// insert it
	if(m_apFirstEntityTypes[pEnt->m_ObjType])
		m_apFirstEntityTypes[pEnt->m_ObjType]->m_pPrevTypeEntity = pEnt;
	pEnt->m_pNextTypeEntity = m_apFirstEntityTypes[pEnt->m_ObjType];
	pEnt->m_pPrevTypeEntity = 0x0;
	m_apFirstEntityTypes[pEnt->m_ObjType] = pEnt;
}

void CGameWorld::RemoveEntity(CEntity *pEnt)
{
	// not in the list
	if(!pEnt->m_pNextTypeEntity && !pEnt->m_pPrevTypeEntity && m_apFirstEntityTypes[pEnt->m_ObjType] != pEnt)
		return;

	// remove
	if(pEnt->m_pPrevTypeEntity)
		pEnt->m_pPrevTypeEntity->m_pNextTypeEntity = pEnt->m_pNextTypeEntity;
	else
		m_apFirstEntityTypes[pEnt->m_ObjType] = pEnt->m_pNextTypeEntity;
	if(pEnt->m_pNextTypeEntity)
		pEnt->m_pNextTypeEntity->m_pPrevTypeEntity = pEnt->m_pPrevTypeEntity;

	// keep list traversing valid
	if(m_pNextTraverseEntity == pEnt)
		m_pNextTraverseEntity = pEnt->m_pNextTypeEntity;

	pEnt->m_pNextTypeEntity = 0;
	pEnt->m_pPrevTypeEntity = 0;
}

//
void CGameWorld::Snap(int SnappingClient)
{
	for(auto *pEnt : m_apFirstEntityTypes)
		for(; pEnt;)
		{
			m_pNextTraverseEntity = pEnt->m_pNextTypeEntity;
			pEnt->Snap(SnappingClient);
			pEnt = m_pNextTraverseEntity;
		}
}

void CGameWorld::Reset()
{
	// reset all entities
	for(auto *pEnt : m_apFirstEntityTypes)
		for(; pEnt;)
		{
			m_pNextTraverseEntity = pEnt->m_pNextTypeEntity;
			pEnt->Reset();
			pEnt = m_pNextTraverseEntity;
		}
	RemoveEntities();

	GameServer()->m_pController->OnReset();
	RemoveEntities();

	m_ResetRequested = false;
}

void CGameWorld::RemoveEntities()
{
	// destroy objects marked for destruction
	for(auto *pEnt : m_apFirstEntityTypes)
		for(; pEnt;)
		{
			m_pNextTraverseEntity = pEnt->m_pNextTypeEntity;
			if(pEnt->m_MarkedForDestroy)
			{
				RemoveEntity(pEnt);
				pEnt->Destroy();
			}
			pEnt = m_pNextTraverseEntity;
		}
}

bool distCompare(std::pair<float, int> a, std::pair<float, int> b)
{
	return (a.first < b.first);
}

void CGameWorld::UpdatePlayerMaps()
{
	if(Server()->Tick() % g_Config.m_SvMapUpdateRate != 0)
		return;

	std::pair<float, int> Dist[MAX_CLIENTS];
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(!Server()->ClientIngame(i))
			continue;
		int *pMap = Server()->GetIdMap(i);

		CPlayer *pFirstPlayer = GameServer()->GetPlayer(i);
		CCharacter *pFirstCharacter = GameServer()->GetCharacter(i);

		// compute distances
		for(int j = 0; j < MAX_CLIENTS; j++)
		{
			Dist[j].second = j;

			CPlayer *pSecondPlayer = GameServer()->GetPlayer(j);
			CCharacter *pSecondCharacter = GameServer()->GetCharacter(j);

			if(!Server()->ClientIngame(j) || !pSecondPlayer)
			{
				Dist[j].first = 1e10;
				continue;
			}
			else if(!pSecondCharacter)
			{
				Dist[j].first = 1e9;
				continue;
			}
			else if(pFirstCharacter &&
				!pFirstPlayer->Spectator() &&
				(!pFirstPlayer ||
					pFirstPlayer->GetClientVersion() == VERSION_VANILLA ||
					pFirstPlayer->GetClientVersion() >= VERSION_DDRACE))
				Dist[j].first = 1e8;
			else
				Dist[j].first = 0;

			Dist[j].first += distance(pFirstPlayer->m_ViewPos, pSecondCharacter->m_Pos);
		}

		// always send the player himself
		Dist[i].first = 0;

		// compute reverse map
		int rMap[MAX_CLIENTS];
		for(int &j : rMap)
		{
			j = -1;
		}
		for(int j = 0; j < VANILLA_MAX_CLIENTS; j++)
		{
			if(pMap[j] == -1)
				continue;
			if(Dist[pMap[j]].first > 5e9)
				pMap[j] = -1;
			else
				rMap[pMap[j]] = j;
		}

		std::nth_element(&Dist[0], &Dist[VANILLA_MAX_CLIENTS - 1], &Dist[MAX_CLIENTS], distCompare);

		int Mapc = 0;
		int Demand = 0;
		for(int j = 0; j < VANILLA_MAX_CLIENTS - 1; j++)
		{
			int k = Dist[j].second;
			if(rMap[k] != -1 || Dist[j].first > 5e9)
				continue;
			while(Mapc < VANILLA_MAX_CLIENTS && pMap[Mapc] != -1)
				Mapc++;
			if(Mapc < VANILLA_MAX_CLIENTS - 1)
				pMap[Mapc] = k;
			else
				Demand++;
		}
		for(int j = MAX_CLIENTS - 1; j > VANILLA_MAX_CLIENTS - 2; j--)
		{
			int k = Dist[j].second;
			if(rMap[k] != -1 && Demand-- > 0)
				pMap[rMap[k]] = -1;
		}
		pMap[VANILLA_MAX_CLIENTS - 1] = -1; // player with empty name to say chat msgs
	}
}

void CGameWorld::Tick()
{
	if(m_ResetRequested)
		Reset();

	if(!m_Paused)
	{
		for(auto *pEnt : m_apFirstEntityTypes)
			for(; pEnt;)
			{
				m_pNextTraverseEntity = pEnt->m_pNextTypeEntity;
				pEnt->Tick();
				pEnt = m_pNextTraverseEntity;
			}

		for(auto *pEnt : m_apFirstEntityTypes)
			for(; pEnt;)
			{
				m_pNextTraverseEntity = pEnt->m_pNextTypeEntity;
				pEnt->TickDefered();
				pEnt = m_pNextTraverseEntity;
			}
	}
	else
	{
		// update all objects
		for(auto *pEnt : m_apFirstEntityTypes)
			for(; pEnt;)
			{
				m_pNextTraverseEntity = pEnt->m_pNextTypeEntity;
				pEnt->TickPaused();
				pEnt = m_pNextTraverseEntity;
			}
	}

	RemoveEntities();

	UpdatePlayerMaps();

}

// TODO: should be more general
//CCharacter *CGameWorld::IntersectCharacter(vec2 Pos0, vec2 Pos1, float Radius, vec2& NewPos, CEntity *pNotThis)
CCharacter *CGameWorld::IntersectCharacter(vec2 Pos0, vec2 Pos1, float Radius, vec2 &NewPos, CCharacter *pNotThis, int CollideWith, class CCharacter *pThisOnly)
{
	// Find other players
	float ClosestLen = distance(Pos0, Pos1) * 100.0f;
	CCharacter *pClosest = 0;

	CCharacter *p = (CCharacter *)FindFirst(ENTTYPE_CHARACTER);
	for(; p; p = (CCharacter *)p->TypeNext())
	{
		if(p == pNotThis)
			continue;

		if(pThisOnly && p != pThisOnly)
			continue;

		vec2 IntersectPos;
		if(closest_point_on_line(Pos0, Pos1, p->m_Pos, IntersectPos))
		{
			float Len = distance(p->m_Pos, IntersectPos);
			if(Len < p->m_ProximityRadius + Radius)
			{
				Len = distance(Pos0, IntersectPos);
				if(Len < ClosestLen)
				{
					NewPos = IntersectPos;
					ClosestLen = Len;
					pClosest = p;
				}
			}
		}
	}

	return pClosest;
}

CCharacter *CGameWorld::ClosestCharacter(vec2 Pos, float Radius, CEntity *pNotThis)
{
	// Find other players
	float ClosestRange = Radius * 2;
	CCharacter *pClosest = 0;

	CCharacter *p = (CCharacter *)GameServer()->m_World.FindFirst(ENTTYPE_CHARACTER);
	for(; p; p = (CCharacter *)p->TypeNext())
	{
		if(p == pNotThis)
			continue;

		float Len = distance(Pos, p->m_Pos);
		if(Len < p->m_ProximityRadius + Radius)
		{
			if(Len < ClosestRange)
			{
				ClosestRange = Len;
				pClosest = p;
			}
		}
	}

	return pClosest;
}

std::list<class CCharacter *> CGameWorld::IntersectedCharacters(vec2 Pos0, vec2 Pos1, float Radius, class CEntity *pNotThis)
{
	std::list<CCharacter *> listOfChars;

	CCharacter *pChr = (CCharacter *)FindFirst(CGameWorld::ENTTYPE_CHARACTER);
	for(; pChr; pChr = (CCharacter *)pChr->TypeNext())
	{
		if(pChr == pNotThis)
			continue;

		vec2 IntersectPos;
		if(closest_point_on_line(Pos0, Pos1, pChr->m_Pos, IntersectPos))
		{
			float Len = distance(pChr->m_Pos, IntersectPos);
			if(Len < pChr->m_ProximityRadius + Radius)
			{
				pChr->m_Intersection = IntersectPos;
				listOfChars.push_back(pChr);
			}
		}
	}
	return listOfChars;
}

void CGameWorld::ReleaseHooked(int ClientID)
{
	CCharacter *pChr = (CCharacter *)CGameWorld::FindFirst(CGameWorld::ENTTYPE_CHARACTER);
	for(; pChr; pChr = (CCharacter *)pChr->TypeNext())
	{
		CCharacterCore *Core = pChr->Core();
		if(Core->m_HookedPlayer == ClientID)
		{
			Core->m_HookedPlayer = -1;
			Core->m_HookState = HOOK_RETRACTED;
			Core->m_TriggeredEvents |= COREEVENT_HOOK_RETRACT;
			Core->m_HookState = HOOK_RETRACTED;
		}
	}
}
