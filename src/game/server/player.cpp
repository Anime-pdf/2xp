/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "player.h"
#include <engine/shared/config.h>
#include <engine/server.h>
#include <game/gamecore.h>
#include <game/version.h>

#include "entities/character.h"
#include "gamecontext.h"
#include "gamecontroller.h"

MACRO_ALLOC_POOL_ID_IMPL(CPlayer, MAX_CLIENTS)

IServer *CPlayer::Server() const { return m_pGameServer->Server(); }

CPlayer::CPlayer(CGameContext *pGameServer, int ClientID, int Team)
{
	m_pGameServer = pGameServer;
	m_ClientID = ClientID;
	m_NumInputs = 0;
	m_Team = Team;
	Reset();
	GameServer()->Antibot()->OnPlayerInit(m_ClientID);
}

CPlayer::~CPlayer()
{
	GameServer()->Antibot()->OnPlayerDestroy(m_ClientID);
	delete m_pLastTarget;
	delete m_pCharacter;
	m_pCharacter = 0;
}

void CPlayer::Reset()
{
	m_DieTick = Server()->Tick();
	m_PreviousDieTick = m_DieTick;
	m_JoinTick = Server()->Tick();
	delete m_pCharacter;
	m_pCharacter = 0;
	m_SpectatorID = SPEC_FREEVIEW;
	m_LastActionTick = Server()->Tick();
	m_TeamChangeTick = Server()->Tick();

	m_pAccount = 0;

	int *pIdMap = Server()->GetIdMap(m_ClientID);
	for(int i = 1; i < VANILLA_MAX_CLIENTS; i++)
	{
		pIdMap[i] = -1;
	}
	pIdMap[0] = m_ClientID;

	// DDRace

	m_LastCommandPos = 0;
	m_LastPlaytime = 0;
	m_LastEyeEmote = 0;
	m_Sent1stAfkWarning = 0;
	m_Sent2ndAfkWarning = 0;
	m_ChatScore = 0;
	m_EyeEmoteEnabled = true;
	if(Server()->IsSixup(m_ClientID))
		m_TimerType = TIMERTYPE_SIXUP;
	else
		m_TimerType = TIMERTYPE_GAMETIMER;

	m_DefEmote = EMOTE_NORMAL;
	m_Afk = true;
	m_LastWhisperTo = -1;
	m_LastSetSpectatorMode = 0;
	delete m_pLastTarget;
	m_pLastTarget = nullptr;
	m_TuneZone = 0;
	m_TuneZoneOld = m_TuneZone;
	m_FirstPacket = true;

	m_SendVoteIndex = -1;
	m_OverrideEmoteReset = -1;

	m_ShowDistance = vec2(1200, 800);
	m_NinjaJetpack = false;

	m_GameTeam = IGameController::TXP_TEAM_DIED;

	m_Score = 0;

	// Variable initialized:
	m_Last_Team = 0;
	m_LastSQLQuery = 0;

	m_FirstVoteTick = Server()->Tick();
}

static int PlayerFlags_SevenToSix(int Flags)
{
	int Six = 0;
	if(Flags & protocol7::PLAYERFLAG_CHATTING)
		Six |= PLAYERFLAG_CHATTING;
	if(Flags & protocol7::PLAYERFLAG_SCOREBOARD)
		Six |= PLAYERFLAG_SCOREBOARD;
	if(Flags & protocol7::PLAYERFLAG_AIM)
		Six |= PLAYERFLAG_AIM;

	return Six;
}

static int PlayerFlags_SixToSeven(int Flags)
{
	int Seven = 0;
	if(Flags & PLAYERFLAG_CHATTING)
		Seven |= protocol7::PLAYERFLAG_CHATTING;
	if(Flags & PLAYERFLAG_SCOREBOARD)
		Seven |= protocol7::PLAYERFLAG_SCOREBOARD;

	return Seven;
}

void CPlayer::Tick()
{
#ifdef CONF_DEBUG
	if(!g_Config.m_DbgDummies || m_ClientID < MAX_CLIENTS - g_Config.m_DbgDummies)
#endif

	if(!Server()->ClientIngame(m_ClientID))
		return;

	if(m_ChatScore > 0)
		m_ChatScore--;

	Server()->SetClientScore(m_ClientID, m_Score);

	// do latency stuff
	{
		IServer::CClientInfo Info;
		if(Server()->GetClientInfo(m_ClientID, &Info))
		{
			m_Latency.m_Accum += Info.m_Latency;
			m_Latency.m_AccumMax = maximum(m_Latency.m_AccumMax, Info.m_Latency);
			m_Latency.m_AccumMin = minimum(m_Latency.m_AccumMin, Info.m_Latency);
		}
		// each second
		if(Server()->Tick() % Server()->TickSpeed() == 0)
		{
			m_Latency.m_Avg = m_Latency.m_Accum / Server()->TickSpeed();
			m_Latency.m_Max = m_Latency.m_AccumMax;
			m_Latency.m_Min = m_Latency.m_AccumMin;
			m_Latency.m_Accum = 0;
			m_Latency.m_AccumMin = 1000;
			m_Latency.m_AccumMax = 0;
		}
	}

	if(Server()->GetNetErrorString(m_ClientID)[0])
	{
		Server()->Kick(m_ClientID, "Net error, try to reconnect");
	}

	if(!GameServer()->m_World.m_Paused)
	{
		int EarliestRespawnTick = m_PreviousDieTick + Server()->TickSpeed() * 3;
		int RespawnTick = maximum(m_DieTick, EarliestRespawnTick) + 2;
		if(!m_pCharacter && RespawnTick <= Server()->Tick())
			m_Spawning = true;

		if(m_pCharacter)
		{
			if(m_pCharacter->IsAlive())
			{
				m_ViewPos = m_pCharacter->m_Pos;
			}
		}
		else if(m_Spawning)
			TryRespawn();
	}
	else
	{
		++m_DieTick;
		++m_PreviousDieTick;
		++m_JoinTick;
		++m_LastActionTick;
		++m_TeamChangeTick;
	}

	m_TuneZoneOld = m_TuneZone; // determine needed tunings with viewpos
	int CurrentIndex = GameServer()->Collision()->GetMapIndex(m_ViewPos);
	m_TuneZone = GameServer()->Collision()->IsTune(CurrentIndex);

	if(m_TuneZone != m_TuneZoneOld) // don't send tunings all the time
	{
		GameServer()->SendTuningParams(m_ClientID, m_TuneZone);
	}

	if(m_OverrideEmoteReset >= 0 && m_OverrideEmoteReset <= Server()->Tick())
	{
		m_OverrideEmoteReset = -1;
	}
}

void CPlayer::PostTick()
{
	// update latency value
	if(m_PlayerFlags & PLAYERFLAG_SCOREBOARD)
	{
		for(int i = 0; i < MAX_CLIENTS; ++i)
		{
			if(GameServer()->m_apPlayers[i] && GameServer()->m_apPlayers[i]->GetTeam() != TEAM_SPECTATORS)
				m_aActLatency[i] = GameServer()->m_apPlayers[i]->m_Latency.m_Min;
		}
	}

	// update view pos for spectators
	if(m_Team == TEAM_SPECTATORS && m_SpectatorID != SPEC_FREEVIEW && GameServer()->m_apPlayers[m_SpectatorID] && GameServer()->m_apPlayers[m_SpectatorID]->GetCharacter())
		m_ViewPos = GameServer()->m_apPlayers[m_SpectatorID]->GetCharacter()->m_Pos;
}

void CPlayer::PostPostTick()
{
#ifdef CONF_DEBUG
	if(!g_Config.m_DbgDummies || m_ClientID < MAX_CLIENTS - g_Config.m_DbgDummies)
#endif
		if(!Server()->ClientIngame(m_ClientID))
			return;

	if(!GameServer()->m_World.m_Paused && !m_pCharacter && m_Spawning)
		TryRespawn();
}

void CPlayer::Snap(int SnappingClient)
{
#ifdef CONF_DEBUG
	if(!g_Config.m_DbgDummies || m_ClientID < MAX_CLIENTS - g_Config.m_DbgDummies)
#endif
		if(!Server()->ClientIngame(m_ClientID))
			return;

	int id = m_ClientID;
	if(SnappingClient > -1 && !Server()->Translate(id, SnappingClient))
		return;

	CNetObj_ClientInfo *pClientInfo = static_cast<CNetObj_ClientInfo *>(Server()->SnapNewItem(NETOBJTYPE_CLIENTINFO, id, sizeof(CNetObj_ClientInfo)));
	if(!pClientInfo)
		return;

	StrToInts(&pClientInfo->m_Name0, 4, Server()->ClientName(m_ClientID));
	StrToInts(&pClientInfo->m_Clan0, 3, Server()->ClientClan(m_ClientID));
	pClientInfo->m_Country = Server()->ClientCountry(m_ClientID);
	StrToInts(&pClientInfo->m_Skin0, 6, m_TeeInfos.m_SkinName);
	pClientInfo->m_UseCustomColor = m_TeeInfos.m_UseCustomColor;
	pClientInfo->m_ColorBody = m_TeeInfos.m_ColorBody;
	pClientInfo->m_ColorFeet = m_TeeInfos.m_ColorFeet;

	int SnappingClientVersion = GameServer()->GetClientVersion(SnappingClient);
	int Latency = SnappingClient == -1 ? m_Latency.m_Min : GameServer()->m_apPlayers[SnappingClient]->m_aActLatency[m_ClientID];
	int Score = abs(m_Score) * -1;

	if(SnappingClient < 0 || !Server()->IsSixup(SnappingClient))
	{
		CNetObj_PlayerInfo *pPlayerInfo = static_cast<CNetObj_PlayerInfo *>(Server()->SnapNewItem(NETOBJTYPE_PLAYERINFO, id, sizeof(CNetObj_PlayerInfo)));
		if(!pPlayerInfo)
			return;

		pPlayerInfo->m_Latency = Latency;
		pPlayerInfo->m_Score = Score;
		pPlayerInfo->m_Local = (int)(m_ClientID == SnappingClient && SnappingClientVersion >= VERSION_DDNET_OLD);
		pPlayerInfo->m_ClientID = id;
		pPlayerInfo->m_Team = m_Team;
	}
	else
	{
		protocol7::CNetObj_PlayerInfo *pPlayerInfo = static_cast<protocol7::CNetObj_PlayerInfo *>(Server()->SnapNewItem(NETOBJTYPE_PLAYERINFO, id, sizeof(protocol7::CNetObj_PlayerInfo)));
		if(!pPlayerInfo)
			return;

		pPlayerInfo->m_PlayerFlags = PlayerFlags_SixToSeven(m_PlayerFlags);
		if(SnappingClientVersion >= VERSION_DDRACE && (m_PlayerFlags & PLAYERFLAG_AIM))
			pPlayerInfo->m_PlayerFlags |= protocol7::PLAYERFLAG_AIM;
		if(Server()->ClientAuthed(m_ClientID))
			pPlayerInfo->m_PlayerFlags |= protocol7::PLAYERFLAG_ADMIN;

		// Times are in milliseconds for 0.7
		pPlayerInfo->m_Score = Score;
		pPlayerInfo->m_Latency = Latency;
	}

	if(m_ClientID == SnappingClient && m_Team == TEAM_SPECTATORS)
	{
		if(SnappingClient < 0 || !Server()->IsSixup(SnappingClient))
		{
			CNetObj_SpectatorInfo *pSpectatorInfo = static_cast<CNetObj_SpectatorInfo *>(Server()->SnapNewItem(NETOBJTYPE_SPECTATORINFO, m_ClientID, sizeof(CNetObj_SpectatorInfo)));
			if(!pSpectatorInfo)
				return;

			pSpectatorInfo->m_SpectatorID = m_SpectatorID;
			pSpectatorInfo->m_X = m_ViewPos.x;
			pSpectatorInfo->m_Y = m_ViewPos.y;
		}
		else
		{
			protocol7::CNetObj_SpectatorInfo *pSpectatorInfo = static_cast<protocol7::CNetObj_SpectatorInfo *>(Server()->SnapNewItem(NETOBJTYPE_SPECTATORINFO, m_ClientID, sizeof(protocol7::CNetObj_SpectatorInfo)));
			if(!pSpectatorInfo)
				return;

			pSpectatorInfo->m_SpecMode = m_SpectatorID == SPEC_FREEVIEW ? protocol7::SPEC_FREEVIEW : protocol7::SPEC_PLAYER;
			pSpectatorInfo->m_SpectatorID = m_SpectatorID;
			pSpectatorInfo->m_X = m_ViewPos.x;
			pSpectatorInfo->m_Y = m_ViewPos.y;
		}
	}

	CNetObj_DDNetPlayer *pDDNetPlayer = static_cast<CNetObj_DDNetPlayer *>(Server()->SnapNewItem(NETOBJTYPE_DDNETPLAYER, id, sizeof(CNetObj_DDNetPlayer)));
	if(!pDDNetPlayer)
		return;

	pDDNetPlayer->m_AuthLevel = Server()->GetAuthedState(id);
	pDDNetPlayer->m_Flags = 0;
	if(m_Afk)
		pDDNetPlayer->m_Flags |= EXPLAYERFLAG_AFK;
}

void CPlayer::FakeSnap()
{
	if(GetClientVersion() >= VERSION_DDNET_OLD)
		return;

	if(Server()->IsSixup(m_ClientID))
		return;

	int FakeID = VANILLA_MAX_CLIENTS - 1;

	CNetObj_ClientInfo *pClientInfo = static_cast<CNetObj_ClientInfo *>(Server()->SnapNewItem(NETOBJTYPE_CLIENTINFO, FakeID, sizeof(CNetObj_ClientInfo)));

	if(!pClientInfo)
		return;

	StrToInts(&pClientInfo->m_Name0, 4, " ");
	StrToInts(&pClientInfo->m_Clan0, 3, "");
	StrToInts(&pClientInfo->m_Skin0, 6, "default");

	CNetObj_PlayerInfo *pPlayerInfo = static_cast<CNetObj_PlayerInfo *>(Server()->SnapNewItem(NETOBJTYPE_PLAYERINFO, FakeID, sizeof(CNetObj_PlayerInfo)));
	if(!pPlayerInfo)
		return;

	pPlayerInfo->m_Latency = m_Latency.m_Min;
	pPlayerInfo->m_Local = 1;
	pPlayerInfo->m_ClientID = FakeID;
	pPlayerInfo->m_Score = -9999;
	pPlayerInfo->m_Team = TEAM_SPECTATORS;

	CNetObj_SpectatorInfo *pSpectatorInfo = static_cast<CNetObj_SpectatorInfo *>(Server()->SnapNewItem(NETOBJTYPE_SPECTATORINFO, FakeID, sizeof(CNetObj_SpectatorInfo)));
	if(!pSpectatorInfo)
		return;

	pSpectatorInfo->m_SpectatorID = m_SpectatorID;
	pSpectatorInfo->m_X = m_ViewPos.x;
	pSpectatorInfo->m_Y = m_ViewPos.y;
}

void CPlayer::OnDisconnect()
{
	KillCharacter();
}

void CPlayer::OnPredictedInput(CNetObj_PlayerInput *NewInput)
{
	// skip the input if chat is active
	if((m_PlayerFlags & PLAYERFLAG_CHATTING) && (NewInput->m_PlayerFlags & PLAYERFLAG_CHATTING))
		return;

	AfkVoteTimer(NewInput);

	m_NumInputs++;

	if(m_pCharacter)
		m_pCharacter->OnPredictedInput(NewInput);
}

void CPlayer::OnDirectInput(CNetObj_PlayerInput *NewInput)
{
	if(Server()->IsSixup(m_ClientID))
		NewInput->m_PlayerFlags = PlayerFlags_SevenToSix(NewInput->m_PlayerFlags);

	if(NewInput->m_PlayerFlags)
		Server()->SetClientFlags(m_ClientID, NewInput->m_PlayerFlags);

	if(AfkTimer(NewInput->m_TargetX, NewInput->m_TargetY))
		return; // we must return if kicked, as player struct is already deleted
	AfkVoteTimer(NewInput);

	if(!m_pCharacter && m_Team == TEAM_SPECTATORS && m_SpectatorID == SPEC_FREEVIEW)
		m_ViewPos = vec2(NewInput->m_TargetX, NewInput->m_TargetY);

	if(NewInput->m_PlayerFlags & PLAYERFLAG_CHATTING)
	{
		// skip the input if chat is active
		if(m_PlayerFlags & PLAYERFLAG_CHATTING)
			return;

		// reset input
		if(m_pCharacter)
			m_pCharacter->ResetInput();

		m_PlayerFlags = NewInput->m_PlayerFlags;
		return;
	}

	m_PlayerFlags = NewInput->m_PlayerFlags;

	if(m_pCharacter)
		m_pCharacter->ResetInput();

	if(!m_pCharacter && m_Team != TEAM_SPECTATORS)
		m_Spawning = true;

	// check for activity
	if(NewInput->m_Direction || m_LatestActivity.m_TargetX != NewInput->m_TargetX ||
		m_LatestActivity.m_TargetY != NewInput->m_TargetY || NewInput->m_Jump ||
		NewInput->m_Fire & 1 || NewInput->m_Hook)
	{
		m_LatestActivity.m_TargetX = NewInput->m_TargetX;
		m_LatestActivity.m_TargetY = NewInput->m_TargetY;
		m_LastActionTick = Server()->Tick();
	}
}

void CPlayer::OnPredictedEarlyInput(CNetObj_PlayerInput *NewInput)
{
	// skip the input if chat is active
	if((m_PlayerFlags & PLAYERFLAG_CHATTING) && (NewInput->m_PlayerFlags & PLAYERFLAG_CHATTING))
		return;

	if(m_pCharacter)
		m_pCharacter->OnDirectInput(NewInput);
}

int CPlayer::GetClientVersion() const
{
	return m_pGameServer->GetClientVersion(m_ClientID);
}

CCharacter *CPlayer::GetCharacter()
{
	if(m_pCharacter && m_pCharacter->IsAlive())
		return m_pCharacter;
	return 0;
}

void CPlayer::KillCharacter(int Weapon)
{
	if(m_pCharacter)
	{
		m_pCharacter->Die(m_ClientID, Weapon);

		delete m_pCharacter;
		m_pCharacter = 0;
	}
}

void CPlayer::Respawn()
{
	if(m_Team != TEAM_SPECTATORS)
	{
		m_Spawning = true;
	}

	//char aBuf[128];
	//str_format(aBuf, sizeof(aBuf), "\n[HHHHHHHHHHHHHHHH]\n%s", Server()->ClientName(m_ClientID)); // 7 symbols -> nickname height

	//GameServer()->SendBroadcastToActivePlayers(aBuf);
}

void CPlayer::SetTeam(int Team)
{
	KillCharacter();

	m_Team = Team;
	m_LastSetTeam = Server()->Tick();
	m_LastActionTick = Server()->Tick();
	m_SpectatorID = SPEC_FREEVIEW;

	protocol7::CNetMsg_Sv_Team Msg;
	Msg.m_ClientID = m_ClientID;
	Msg.m_Team = m_Team;
	Msg.m_Silent = true;
	Msg.m_CooldownTick = m_LastSetTeam + Server()->TickSpeed() * g_Config.m_SvTeamChangeDelay;
	Server()->SendPackMsg(&Msg, MSGFLAG_VITAL | MSGFLAG_NORECORD, -1);

	if(Team == TEAM_SPECTATORS)
	{
		// update spectator modes
		for(auto &pPlayer : GameServer()->m_apPlayers)
		{
			if(pPlayer && pPlayer->m_SpectatorID == m_ClientID)
				pPlayer->m_SpectatorID = SPEC_FREEVIEW;
		}
	}
}

void CPlayer::SetGameTeam(int Team, bool DoChatMsg)
{
	if(m_GameTeam == Team)
		return;

	if (Team == IGameController::TXP_TEAM_DIED)
	{
		SetTeam(TEAM_SPECTATORS);

		if (m_GameTeam == IGameController::TXP_TEAM_H)
		{
			if (DoChatMsg)
			{
				char aBuf[256];
				str_format(aBuf, sizeof(aBuf), "\n\n\n\n\n\n\n\n\n\n\n\n[Human]\n\n%s died.\n\n\nR.I.P");

				GameServer()->SendBroadcastToActivePlayers(aBuf);
			}
		}
		else
		{
			if(DoChatMsg)
			{
				char aBuf[256];
				str_format(aBuf, sizeof(aBuf), "\n\n\n\n\n\n\n\n\n\n\n\n[Zombie]\n\n%s died.\n\n\nR.I.P");

				GameServer()->SendBroadcastToActivePlayers(aBuf);
			}
		}
	}
	else if(Team == IGameController::TXP_TEAM_H)
	{
		if(DoChatMsg)
		{
			char aBuf[256];
			str_format(aBuf, sizeof(aBuf), "\n\n\n\n\n\n\n\n\n\n\n\n[Human]\n\n%s revived.\n\n\nR.I.P");

			GameServer()->SendBroadcastToActivePlayers(aBuf);
		}
	}
	else
	{
		if(DoChatMsg)
		{
			char aBuf[256];
			str_format(aBuf, sizeof(aBuf), "\n\n\n\n\n\n\n\n\n\n\n\n[Human]\n\n%s was infected.\n\n\nR.I.P");

			GameServer()->SendBroadcastToActivePlayers(aBuf);
		}
	}

	m_GameTeam = Team;
}

void CPlayer::TryRespawn()
{
	vec2 SpawnPos;

	if(!GameServer()->m_pController->CanSpawn(m_Team, &SpawnPos))
		return;

	m_Spawning = false;
	m_pCharacter = new(m_ClientID) CCharacter(&GameServer()->m_World);
	m_ViewPos = SpawnPos;
	m_pCharacter->Spawn(this, SpawnPos);
	GameServer()->CreatePlayerSpawn(SpawnPos);
}

bool CPlayer::AfkTimer(int NewTargetX, int NewTargetY)
{
	/*
		afk timer (x, y = mouse coordinates)
		Since a player has to move the mouse to play, this is a better method than checking
		the player's position in the game world, because it can easily be bypassed by just locking a key.
		Frozen players could be kicked as well, because they can't move.
		It also works for spectators.
		returns true if kicked
	*/

	if(Server()->GetAuthedState(m_ClientID))
		return false; // don't kick admins
	if(g_Config.m_SvMaxAfkTime == 0)
		return false; // 0 = disabled

	if(NewTargetX != m_LastTarget_x || NewTargetY != m_LastTarget_y)
	{
		UpdatePlaytime();
		m_LastTarget_x = NewTargetX;
		m_LastTarget_y = NewTargetY;
		m_Sent1stAfkWarning = 0; // afk timer's 1st warning after 50% of sv_max_afk_time
		m_Sent2ndAfkWarning = 0;
	}
	else
	{
		// not playing, check how long
		if(m_Sent1stAfkWarning == 0 && m_LastPlaytime < time_get() - time_freq() * (int)(g_Config.m_SvMaxAfkTime * 0.5))
		{
			str_format(m_pAfkMsg, sizeof(m_pAfkMsg),
				"You have been afk for %d seconds now. Please note that you get kicked after not playing for %d seconds.",
				(int)(g_Config.m_SvMaxAfkTime * 0.5),
				g_Config.m_SvMaxAfkTime);
			m_pGameServer->SendChatTarget(m_ClientID, m_pAfkMsg);
			m_Sent1stAfkWarning = 1;
		}
		else if(m_Sent2ndAfkWarning == 0 && m_LastPlaytime < time_get() - time_freq() * (int)(g_Config.m_SvMaxAfkTime * 0.9))
		{
			str_format(m_pAfkMsg, sizeof(m_pAfkMsg),
				"You have been afk for %d seconds now. Please note that you get kicked after not playing for %d seconds.",
				(int)(g_Config.m_SvMaxAfkTime * 0.9),
				g_Config.m_SvMaxAfkTime);
			m_pGameServer->SendChatTarget(m_ClientID, m_pAfkMsg);
			m_Sent2ndAfkWarning = 1;
		}
		else if(m_LastPlaytime < time_get() - time_freq() * g_Config.m_SvMaxAfkTime)
		{
			m_pGameServer->Server()->Kick(m_ClientID, "Away from keyboard");
			return true;
		}
	}
	return false;
}

void CPlayer::UpdatePlaytime()
{
	m_LastPlaytime = time_get();
}

void CPlayer::AfkVoteTimer(CNetObj_PlayerInput *NewTarget)
{
	if(g_Config.m_SvMaxAfkVoteTime == 0)
		return;

	if(!m_pLastTarget)
	{
		m_pLastTarget = new CNetObj_PlayerInput(*NewTarget);
		m_LastPlaytime = 0;
		m_Afk = true;
		return;
	}
	else if(mem_comp(NewTarget, m_pLastTarget, sizeof(CNetObj_PlayerInput)) != 0)
	{
		UpdatePlaytime();
		mem_copy(m_pLastTarget, NewTarget, sizeof(CNetObj_PlayerInput));
	}
	else if(m_LastPlaytime < time_get() - time_freq() * g_Config.m_SvMaxAfkVoteTime)
	{
		m_Afk = true;
		return;
	}

	m_Afk = false;
}

int CPlayer::GetDefaultEmote() const
{
	if(m_OverrideEmoteReset >= 0)
		return m_OverrideEmote;

	return m_DefEmote;
}

void CPlayer::OverrideDefaultEmote(int Emote, int Tick)
{
	m_OverrideEmote = Emote;
	m_OverrideEmoteReset = Tick;
	m_LastEyeEmote = Server()->Tick();
}

bool CPlayer::CanOverrideDefaultEmote() const
{
	return m_LastEyeEmote == 0 || m_LastEyeEmote + (int64)g_Config.m_SvEyeEmoteChangeDelay * Server()->TickSpeed() < Server()->Tick();
}

bool CPlayer::IsPlaying()
{
	if(m_pCharacter && m_pCharacter->IsAlive())
		return true;
	return false;
}

void CPlayer::SpectatePlayerName(const char *pName)
{
	if(!pName)
		return;

	for(int i = 0; i < MAX_CLIENTS; ++i)
	{
		if(i != m_ClientID && Server()->ClientIngame(i) && !str_comp(pName, Server()->ClientName(i)))
		{
			m_SpectatorID = i;
			return;
		}
	}
}
