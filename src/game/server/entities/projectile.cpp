/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "projectile.h"
#include <game/generated/protocol.h>
#include <game/server/gamecontext.h>
#include <game/server/gamemodes/2xp.h>
#include <game/server/player.h>
#include <game/version.h>

#include <engine/shared/config.h>

#include "character.h"

CProjectile::CProjectile(
	CGameWorld *pGameWorld,
	int Type,
	int Owner,
	vec2 Pos,
	vec2 Dir,
	int Span,
	bool Freeze,
	bool Explosive,
	float Force,
	int SoundImpact,
	int Layer,
	int Number) :
	CEntity(pGameWorld, CGameWorld::ENTTYPE_PROJECTILE)
{
	m_Type = Type;
	m_Pos = Pos;
	m_Direction = Dir;
	m_LifeSpan = Span;
	m_Owner = Owner;
	m_Force = Force;
	//m_Damage = Damage;
	m_SoundImpact = SoundImpact;
	m_StartTick = Server()->Tick();
	m_Explosive = Explosive;

	m_Layer = Layer;
	m_Number = Number;

	m_TuneZone = GameServer()->Collision()->IsTune(GameServer()->Collision()->GetMapIndex(m_Pos));

	GameWorld()->InsertEntity(this);
}

void CProjectile::Reset()
{
	if(m_LifeSpan > -2)
		m_MarkedForDestroy = true;
}

vec2 CProjectile::GetPos(float Time)
{
	float Curvature = 0;
	float Speed = 0;

	switch(m_Type)
	{
	case WEAPON_GRENADE:
		if(!m_TuneZone)
		{
			Curvature = GameServer()->Tuning()->m_GrenadeCurvature;
			Speed = GameServer()->Tuning()->m_GrenadeSpeed;
		}
		else
		{
			Curvature = GameServer()->TuningList()[m_TuneZone].m_GrenadeCurvature;
			Speed = GameServer()->TuningList()[m_TuneZone].m_GrenadeSpeed;
		}

		break;

	case WEAPON_SHOTGUN:
		if(!m_TuneZone)
		{
			Curvature = GameServer()->Tuning()->m_ShotgunCurvature;
			Speed = GameServer()->Tuning()->m_ShotgunSpeed;
		}
		else
		{
			Curvature = GameServer()->TuningList()[m_TuneZone].m_ShotgunCurvature;
			Speed = GameServer()->TuningList()[m_TuneZone].m_ShotgunSpeed;
		}

		break;

	case WEAPON_GUN:
		if(!m_TuneZone)
		{
			Curvature = GameServer()->Tuning()->m_GunCurvature;
			Speed = GameServer()->Tuning()->m_GunSpeed;
		}
		else
		{
			Curvature = GameServer()->TuningList()[m_TuneZone].m_GunCurvature;
			Speed = GameServer()->TuningList()[m_TuneZone].m_GunSpeed;
		}
		break;
	}

	return CalcPos(m_Pos, m_Direction, Curvature, Speed, Time);
}

void CProjectile::Tick()
{
	float Pt = (Server()->Tick() - m_StartTick - 1) / (float)Server()->TickSpeed();
	float Ct = (Server()->Tick() - m_StartTick) / (float)Server()->TickSpeed();
	vec2 PrevPos = GetPos(Pt);
	vec2 CurPos = GetPos(Ct);
	vec2 ColPos;
	vec2 NewPos;
	int Collide = GameServer()->Collision()->IntersectLine(PrevPos, CurPos, &ColPos, &NewPos);
	CCharacter *pOwnerChar = 0;

	if(m_Owner >= 0)
		pOwnerChar = GameServer()->GetCharacter(m_Owner);

	CCharacter *pTargetChr = 0;

	if(pOwnerChar && !(pOwnerChar->m_Hit & CCharacter::DISABLE_HIT_GRENADE))
		pTargetChr = GameServer()->m_World.IntersectCharacter(PrevPos, ColPos, 6.0f, ColPos, pOwnerChar, m_Owner);

	if(m_LifeSpan > -1)
		m_LifeSpan--;

	if(((pTargetChr && (pOwnerChar && !(pOwnerChar->m_Hit & CCharacter::DISABLE_HIT_GRENADE))) || Collide))
	{
		if(m_Explosive /*??*/ && (!pTargetChr || (pTargetChr && (m_Type == WEAPON_SHOTGUN && Collide))))
		{
			int Number = 1;
			for(int i = 0; i < Number; i++)
			{
				GameServer()->CreateExplosion(ColPos, m_Owner, m_Type, m_Owner == -1);
				GameServer()->CreateSound(ColPos, m_SoundImpact);
			}
		}

		if(Collide && m_Bouncing != 0)
		{
			m_StartTick = Server()->Tick();
			m_Pos = NewPos + (-(m_Direction * 4));
			if(m_Bouncing == 1)
				m_Direction.x = -m_Direction.x;
			else if(m_Bouncing == 2)
				m_Direction.y = -m_Direction.y;
			if(fabs(m_Direction.x) < 1e-6)
				m_Direction.x = 0;
			if(fabs(m_Direction.y) < 1e-6)
				m_Direction.y = 0;
			m_Pos += m_Direction;
		}
		else if(m_Type == WEAPON_GUN)
		{
			GameServer()->CreateDamageInd(CurPos, -atan2(m_Direction.x, m_Direction.y), 10);
			m_MarkedForDestroy = true;
			return;
		}
	}
	if(m_LifeSpan == -1)
	{
		if(m_Explosive)
		{
			if(m_Owner >= 0)
				pOwnerChar = GameServer()->GetCharacter(m_Owner);

			GameServer()->CreateExplosion(ColPos, m_Owner, m_Type, m_Owner);
			GameServer()->CreateSound(ColPos, m_SoundImpact);
		}
		m_MarkedForDestroy = true;
		return;
	}

	int x = GameServer()->Collision()->GetIndex(PrevPos, CurPos);
	int z = GameServer()->Collision()->IsTeleportWeapon(x);
	CGCTXP *pControllerDDRace = (CGCTXP *)GameServer()->m_pController;
	if(z && pControllerDDRace->m_TeleOuts[z - 1].size())
	{
		int TeleOut = GameServer()->m_World.m_Core.RandomOr0(pControllerDDRace->m_TeleOuts[z - 1].size());
		m_Pos = pControllerDDRace->m_TeleOuts[z - 1][TeleOut];
		m_StartTick = Server()->Tick();
	}
}

void CProjectile::TickPaused()
{
	++m_StartTick;
}

void CProjectile::FillInfo(CNetObj_Projectile *pProj)
{
	pProj->m_X = (int)m_Pos.x;
	pProj->m_Y = (int)m_Pos.y;
	pProj->m_VelX = (int)(m_Direction.x * 100.0f);
	pProj->m_VelY = (int)(m_Direction.y * 100.0f);
	pProj->m_StartTick = m_StartTick;
	pProj->m_Type = m_Type;
}

void CProjectile::Snap(int SnappingClient)
{
	float Ct = (Server()->Tick() - m_StartTick) / (float)Server()->TickSpeed();

	if(NetworkClipped(SnappingClient, GetPos(Ct)))
		return;

	int SnappingClientVersion = GameServer()->GetClientVersion(SnappingClient);

	CNetObj_DDNetProjectile DDNetProjectile;
	if(SnappingClientVersion >= VERSION_DDNET_ANTIPING_PROJECTILE && FillExtraInfo(&DDNetProjectile))
	{
		int Type = SnappingClientVersion < VERSION_DDNET_MSG_LEGACY ? (int)NETOBJTYPE_PROJECTILE : NETOBJTYPE_DDNETPROJECTILE;
		void *pProj = Server()->SnapNewItem(Type, GetID(), sizeof(DDNetProjectile));
		if(!pProj)
		{
			return;
		}
		mem_copy(pProj, &DDNetProjectile, sizeof(DDNetProjectile));
	}
	else
	{
		CNetObj_Projectile *pProj = static_cast<CNetObj_Projectile *>(Server()->SnapNewItem(NETOBJTYPE_PROJECTILE, GetID(), sizeof(CNetObj_Projectile)));
		if(!pProj)
		{
			return;
		}
		FillInfo(pProj);
	}
}

// DDRace

void CProjectile::SetBouncing(int Value)
{
	m_Bouncing = Value;
}

bool CProjectile::FillExtraInfo(CNetObj_DDNetProjectile *pProj)
{
	const int MaxPos = 0x7fffffff / 100;
	if(abs((int)m_Pos.y) + 1 >= MaxPos || abs((int)m_Pos.x) + 1 >= MaxPos)
	{
		//If the modified data would be too large to fit in an integer, send normal data instead
		return false;
	}
	//Send additional/modified info, by modifiying the fields of the netobj
	float Angle = -atan2f(m_Direction.x, m_Direction.y);

	int Data = 0;
	Data |= (abs(m_Owner) & 255) << 0;
	if(m_Owner < 0)
		Data |= PROJECTILEFLAG_NO_OWNER;
	//This bit tells the client to use the extra info
	Data |= PROJECTILEFLAG_IS_DDNET;
	// PROJECTILEFLAG_BOUNCE_HORIZONTAL, PROJECTILEFLAG_BOUNCE_VERTICAL
	Data |= (m_Bouncing & 3) << 10;
	if(m_Explosive)
		Data |= PROJECTILEFLAG_EXPLOSIVE;

	pProj->m_X = (int)(m_Pos.x * 100.0f);
	pProj->m_Y = (int)(m_Pos.y * 100.0f);
	pProj->m_Angle = (int)(Angle * 1000000.0f);
	pProj->m_Data = Data;
	pProj->m_StartTick = m_StartTick;
	pProj->m_Type = m_Type;
	return true;
}
