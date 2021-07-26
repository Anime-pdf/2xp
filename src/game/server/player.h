/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_SERVER_PLAYER_H
#define GAME_SERVER_PLAYER_H

#include "alloc.h"

#include "teeinfo.h"
#include <game/generated/protocol.h>
#include <game/server/entities/character.h>
#include <game/server/database/account.h>

enum
{
	WEAPON_GAME = -3, // team switching etc
	WEAPON_SELF = -2, // console kill command
	WEAPON_WORLD = -1, // death tiles etc
};

// player object
class CPlayer
{
	MACRO_ALLOC_POOL_ID()

public:
	CPlayer(CGameContext *pGameServer, int ClientID, int Team);
	~CPlayer();

	void Reset();

	void TryRespawn();
	void Respawn(); // with WeakHook == true the character will be spawned after all calls of Tick from other Players
	void SetTeam(int Team);
	int GetTeam() const { return m_Team; };
	void SetGameTeam(int Team, bool DoChatMsg = true);
	int GetGameTeam() const { return m_GameTeam; }
	int GetCID() const { return m_ClientID; };
	int GetClientVersion() const;

	void Tick();
	void PostTick();

	// will be called after all Tick and PostTick calls from other players
	void PostPostTick();
	void Snap(int SnappingClient);
	void FakeSnap();

	void OnDirectInput(CNetObj_PlayerInput *NewInput);
	void OnPredictedInput(CNetObj_PlayerInput *NewInput);
	void OnPredictedEarlyInput(CNetObj_PlayerInput *NewInput);
	void OnDisconnect();

	void KillCharacter(int Weapon = WEAPON_GAME);
	CCharacter *GetCharacter();

	CAccount *GetAccount() const { return m_pAccount; }
	void SetAccount(CAccount *pNewAccount) { m_pAccount = pNewAccount; }

	void SpectatePlayerName(const char *pName);

	//---------------------------------------------------------
	// this is used for snapping so we know how we can clip the view for the player
	vec2 m_ViewPos;
	int m_TuneZone;
	int m_TuneZoneOld;

	// states if the client is chatting, accessing a menu etc.
	int m_PlayerFlags;

	// used for snapping to just update latency if the scoreboard is active
	int m_aActLatency[MAX_CLIENTS];

	// used for spectator mode
	int m_SpectatorID;

	bool m_IsReady;

	//
	int m_Vote;
	int m_VotePos;
	//
	int m_LastVoteCall;
	int m_LastVoteTry;
	int m_LastChat;
	int m_LastSetTeam;
	int m_LastSetSpectatorMode;
	int m_LastChangeInfo;
	int m_LastEmote;
	int m_LastKill;
	int m_LastCommands[4];
	int m_LastCommandPos;
	int m_LastWhisperTo;

	int m_SendVoteIndex;

	CTeeInfo m_TeeInfos;

	int m_DieTick;
	int m_PreviousDieTick;
	int m_Score;
	int m_JoinTick;
	bool m_ForceBalanced;
	int m_LastActionTick;
	int m_TeamChangeTick;
	struct
	{
		int m_TargetX;
		int m_TargetY;
	} m_LatestActivity;

	// network latency calculations
	struct
	{
		int m_Accum;
		int m_AccumMin;
		int m_AccumMax;
		int m_Avg;
		int m_Min;
		int m_Max;
	} m_Latency;

private:
	CCharacter *m_pCharacter;
	CAccount *m_pAccount;
	int m_NumInputs;
	CGameContext *m_pGameServer;

	CGameContext *GameServer() const { return m_pGameServer; }
	IServer *Server() const;

	//
	bool m_Spawning;
	int m_ClientID;
	int m_Team;
	int m_GameTeam;

	int m_DefEmote;
	int m_OverrideEmote;
	int m_OverrideEmoteReset;
public:
	enum
	{
		TIMERTYPE_DEFAULT = -1,
		TIMERTYPE_GAMETIMER = 0,
		TIMERTYPE_BROADCAST,
		TIMERTYPE_GAMETIMER_AND_BROADCAST,
		TIMERTYPE_SIXUP,
		TIMERTYPE_NONE,
	};

	int64 m_FirstVoteTick;

	bool IsPlaying();
	int64 m_Last_KickVote;
	int64 m_Last_Team;
	vec2 m_ShowDistance;
	bool m_NinjaJetpack;
	bool m_Afk;

	int m_ChatScore;

	bool AfkTimer(int new_target_x, int new_target_y); //returns true if kicked
	void UpdatePlaytime();
	void AfkVoteTimer(CNetObj_PlayerInput *NewTarget);
	int64 m_LastPlaytime;
	int64 m_LastEyeEmote;
	int64 m_LastBroadcast;
	bool m_LastBroadcastImportance;
	int m_LastTarget_x;
	int m_LastTarget_y;
	CNetObj_PlayerInput *m_pLastTarget;
	int m_Sent1stAfkWarning; // afk timer's 1st warning after 50% of sv_max_afk_time
	int m_Sent2ndAfkWarning; // afk timer's 2nd warning after 90% of sv_max_afk_time
	char m_pAfkMsg[160];
	bool m_EyeEmoteEnabled;
	int m_TimerType;

	int GetDefaultEmote() const;
	void OverrideDefaultEmote(int Emote, int Tick);
	bool CanOverrideDefaultEmote() const;

	bool m_FirstPacket;
	int64 m_LastSQLQuery;
};

#endif
