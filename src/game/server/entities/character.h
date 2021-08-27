/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_SERVER_ENTITIES_CHARACTER_H
#define GAME_SERVER_ENTITIES_CHARACTER_H

#include <engine/antibot.h>
#include <game/server/entity.h>

class CAntibot;
struct CAntibotCharacterData;

enum
{
	FAKETUNE_FREEZE = 1,
	FAKETUNE_SOLO = 2,
	FAKETUNE_NOJUMP = 4,
	FAKETUNE_NOCOLL = 8,
	FAKETUNE_NOHOOK = 16,
	FAKETUNE_JETPACK = 32,
	FAKETUNE_NOHAMMER = 64,
};

class CCharacter : public CEntity
{
	MACRO_ALLOC_POOL_ID()

public:
	//character's size
	static const int ms_PhysSize = 28;

	CCharacter(CGameWorld *pWorld);

	virtual void Reset();
	virtual void Destroy();
	virtual void Tick();
	virtual void TickDefered();
	virtual void TickPaused();
	virtual void Snap(int SnappingClient);

	bool IsGrounded();

	void SetWeapon(int W);
	void HandleWeaponSwitch();
	void DoWeaponSwitch();

	void HandleWeapons();
	void HandleNinja();

	void OnPredictedInput(CNetObj_PlayerInput *pNewInput);
	void OnDirectInput(CNetObj_PlayerInput *pNewInput);
	void ResetHook();
	void ResetInput();
	void FireWeapon();

	void Die(int Killer, int Weapon);
	bool TakeDamage(vec2 Force, int Dmg, int From, int Weapon);

	bool Spawn(class CPlayer *pPlayer, vec2 Pos);
	bool Remove();

	bool IncreaseHealth(int Amount);
	bool IncreaseArmor(int Amount);

	void GiveWeapon(int Weapon, bool Remove = false);
	void GiveNinja();
	void RemoveNinja();

	void SetEmote(int Emote, int Tick);

	int NeededFaketuning() { return m_NeededFaketuning; }
	bool IsAlive() const { return m_Alive; }
	class CPlayer *GetPlayer() { return m_pPlayer; }

private:
	// player controlling this character
	class CPlayer *m_pPlayer;

	bool m_Alive;
	int m_NeededFaketuning;

	// weapon info
	CEntity *m_apHitObjects[10];
	int m_NumObjectsHit;

	struct WeaponStat
	{
		int m_AmmoRegenStart;
		int m_Ammo;
		int m_Ammocost;
		bool m_Got;

	} m_aWeapons[NUM_WEAPONS];

	int m_LastWeapon;
	int m_QueuedWeapon;

	int m_ReloadTimer;
	int m_AttackTick;

	int m_DamageTaken;

	int m_EmoteType;
	int m_EmoteStop;

	// last tick that the player took any action ie some input
	int m_LastAction;
	int m_LastNoAmmoSound;

	// these are non-heldback inputs
	CNetObj_PlayerInput m_LatestPrevPrevInput;
	CNetObj_PlayerInput m_LatestPrevInput;
	CNetObj_PlayerInput m_LatestInput;

	// input
	CNetObj_PlayerInput m_PrevInput;
	CNetObj_PlayerInput m_Input;
	CNetObj_PlayerInput m_SavedInput;
	int m_NumInputs;
	int m_Jumped;

	int m_DamageTakenTick;

	int m_Health;
	int m_Armor;

	// ninja
	struct
	{
		vec2 m_ActivationDir;
		int m_ActivationTick;
		int m_CurrentMoveTime;
		int m_OldVelAmount;
	} m_Ninja;

	// the player core for the physics
	CCharacterCore m_Core;

	std::map<int, std::vector<vec2>> *m_pTeleOuts = nullptr;
	std::map<int, std::vector<vec2>> *m_pTeleCheckOuts = nullptr;

	// info for dead reckoning
	int m_ReckoningTick; // tick that we are performing dead reckoning From
	CCharacterCore m_SendCore; // core that we should send
	CCharacterCore m_ReckoningCore; // the dead reckoning core

	// DDRace

	void SnapCharacter(int SnappingClient, int ID);
	static bool IsSwitchActiveCb(int Number, void *pUser);
	void HandleTiles(int Index);
	void DDRaceInit();
	void HandleSkippableTiles(int Index);
	void DDRaceTick();
	void DDRacePostCoreTick();
	void HandleBroadcast();
	void HandleTuneLayer();
	void SendZoneMsgs();
	IAntibot *Antibot();

public:
	void SetTeleports(std::map<int, std::vector<vec2>> *pTeleOuts, std::map<int, std::vector<vec2>> *pTeleCheckOuts);

	void FillAntibot(CAntibotCharacterData *pData);
	void GiveAllWeapons();
	void ResetPickups();
	enum
	{
		HIT_ALL = 0,
		DISABLE_HIT_HAMMER = 1,
		DISABLE_HIT_SHOTGUN = 2,
		DISABLE_HIT_GRENADE = 4,
		DISABLE_HIT_LASER = 8
	};
	int m_Hit;
	int m_TuneZone;
	int m_TuneZoneOld;
	vec2 m_PrevPos;
	int m_TeleCheckpoint;

	int m_MoveRestrictions;

	vec2 m_Intersection;
	bool m_LastRefillJumps;

	int m_RespawnTick;
	int m_SpawnTick;
	int m_WeaponChangeTick;

	// Setters/Getters because i don't want to modify vanilla vars access modifiers
	int GetLastWeapon() const { return m_LastWeapon; };
	void SetLastWeapon(int LastWeap) { m_LastWeapon = LastWeap; };
	int GetActiveWeapon() const { return m_Core.m_ActiveWeapon; };
	void SetActiveWeapon(int ActiveWeap) { m_Core.m_ActiveWeapon = ActiveWeap; };
	void SetLastAction(int LastAction) { m_LastAction = LastAction; };
	int GetArmor() { return m_Armor; };
	void SetArmor(int Armor) { m_Armor = Armor; };
	CCharacterCore GetCore() { return m_Core; };
	void SetCore(CCharacterCore Core) { m_Core = Core; };
	CCharacterCore *Core() { return &m_Core; };
	bool GetWeaponGot(int Type) const { return m_aWeapons[Type].m_Got; };
	void SetWeaponGot(int Type, bool Value) { m_aWeapons[Type].m_Got = Value; };
	int GetWeaponAmmo(int Type) const { return m_aWeapons[Type].m_Ammo; };
	void SetWeaponAmmo(int Type, int Value) { m_aWeapons[Type].m_Ammo = Value; };
	void SetNinjaActivationDir(vec2 ActivationDir) { m_Ninja.m_ActivationDir = ActivationDir; };
	void SetNinjaActivationTick(int ActivationTick) { m_Ninja.m_ActivationTick = ActivationTick; };
	void SetNinjaCurrentMoveTime(int CurrentMoveTime) { m_Ninja.m_CurrentMoveTime = CurrentMoveTime; };

	int GetLastAction() const { return m_LastAction; }

	// 2xp
	bool m_Builder;
};

#endif
