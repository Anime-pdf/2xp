/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */

#include <string.h>
#include <base/math.h>
#include <base/tl/sorted_array.h>

#include <antibot/antibot_data.h>

#include <engine/console.h>
#include <engine/engine.h>
#include <engine/storage.h>
#include <engine/map.h>
#include <engine/server/server.h>
#include <engine/shared/config.h>
#include <engine/shared/datafile.h>
#include <engine/shared/linereader.h>
#include <engine/shared/memheap.h>

#include <game/collision.h>
#include <game/gamecore.h>
#include <game/version.h>

#include <game/generated/protocol7.h>
#include <game/generated/protocolglue.h>

#include "entities/character.h"
#include "gamemodes/2xp.h"

#include "teeinfo.h"
#include "player.h"
#include "voting.h"

#include "gamecontext.h"

enum
{
	RESET,
	NO_RESET
};

void CGameContext::Construct(int Resetting)
{
	m_Resetting = 0;
	m_pServer = 0;

	for(auto &pPlayer : m_apPlayers)
		pPlayer = 0;

	m_pController = 0;

	if(Resetting == NO_RESET)
	{
		m_NumMutes = 0;
		m_NumVoteMutes = 0;
	}
	m_ChatResponseTargetID = -1;
	m_aDeleteTempfile[0] = 0;
	m_TeeHistorianActive = false;
}

CGameContext::CGameContext(int Resetting)
{
	Construct(Resetting);
}

CGameContext::CGameContext()
{
	Construct(NO_RESET);
}

CGameContext::~CGameContext()
{
	for(auto &pPlayer : m_apPlayers)
		delete pPlayer;
	if(!m_Resetting)
		delete m_pVoteManager;
}

void CGameContext::Clear()
{
	CTuningParams Tuning = m_Tuning;

	m_Resetting = true;
	this->~CGameContext();
	mem_zero(this, sizeof(*this));
	new(this) CGameContext(RESET);

	m_Tuning = Tuning;
}

void CGameContext::TeeHistorianWrite(const void *pData, int DataSize, void *pUser)
{
	CGameContext *pSelf = (CGameContext *)pUser;
	aio_write(pSelf->m_pTeeHistorianFile, pData, DataSize);
}

void CGameContext::CommandCallback(int ClientID, int FlagMask, const char *pCmd, IConsole::IResult *pResult, void *pUser)
{
	CGameContext *pSelf = (CGameContext *)pUser;
	if(pSelf->m_TeeHistorianActive)
	{
		pSelf->m_TeeHistorian.RecordConsoleCommand(ClientID, FlagMask, pCmd, pResult);
	}
}

class CCharacter *CGameContext::GetPlayerChar(int ClientID)
{
	if(ClientID < 0 || ClientID >= MAX_CLIENTS || !m_apPlayers[ClientID])
		return 0;
	return m_apPlayers[ClientID]->GetCharacter();
}

void CGameContext::FillAntibot(CAntibotRoundData *pData)
{
	if(!pData->m_Map.m_pTiles)
	{
		Collision()->FillAntibot(&pData->m_Map);
	}
	pData->m_Tick = Server()->Tick();
	mem_zero(pData->m_aCharacters, sizeof(pData->m_aCharacters));
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		CAntibotCharacterData *pChar = &pData->m_aCharacters[i];
		for(auto &LatestInput : pChar->m_aLatestInputs)
		{
			LatestInput.m_TargetX = -1;
			LatestInput.m_TargetY = -1;
		}
		pChar->m_Alive = false;
		pChar->m_Pause = false;
		pChar->m_Team = -1;

		pChar->m_Pos = vec2(-1, -1);
		pChar->m_Vel = vec2(0, 0);
		pChar->m_Angle = -1;
		pChar->m_HookedPlayer = -1;
		pChar->m_SpawnTick = -1;
		pChar->m_WeaponChangeTick = -1;

		if(m_apPlayers[i])
		{
			str_copy(pChar->m_aName, Server()->ClientName(i), sizeof(pChar->m_aName));
			CCharacter *pGameChar = m_apPlayers[i]->GetCharacter();
			pChar->m_Alive = (bool)pGameChar;
			pChar->m_Team = m_apPlayers[i]->GetTeam();
			if(pGameChar)
			{
				pGameChar->FillAntibot(pChar);
			}
		}
	}
}

void CGameContext::CreateDamageInd(vec2 Pos, float Angle, int Amount)
{
	float a = 3 * 3.14159f / 2 + Angle;
	//float a = get_angle(dir);
	float s = a - pi / 3;
	float e = a + pi / 3;
	for(int i = 0; i < Amount; i++)
	{
		float f = mix(s, e, float(i + 1) / float(Amount + 2));
		CNetEvent_DamageInd *pEvent = (CNetEvent_DamageInd *)m_Events.Create(NETEVENTTYPE_DAMAGEIND, sizeof(CNetEvent_DamageInd));
		if(pEvent)
		{
			pEvent->m_X = (int)Pos.x;
			pEvent->m_Y = (int)Pos.y;
			pEvent->m_Angle = (int)(f * 256.0f);
		}
	}
}

void CGameContext::CreateHammerHit(vec2 Pos)
{
	// create the event
	CNetEvent_HammerHit *pEvent = (CNetEvent_HammerHit *)m_Events.Create(NETEVENTTYPE_HAMMERHIT, sizeof(CNetEvent_HammerHit));
	if(pEvent)
	{
		pEvent->m_X = (int)Pos.x;
		pEvent->m_Y = (int)Pos.y;
	}
}

void CGameContext::CreateExplosion(vec2 Pos, int Owner, int Weapon, bool NoDamage)
{
	// create the event
	CNetEvent_Explosion *pEvent = (CNetEvent_Explosion *)m_Events.Create(NETEVENTTYPE_EXPLOSION, sizeof(CNetEvent_Explosion));
	if(pEvent)
	{
		pEvent->m_X = (int)Pos.x;
		pEvent->m_Y = (int)Pos.y;
	}

	// deal damage
	CCharacter *apEnts[MAX_CLIENTS];
	float Radius = 135.0f;
	float InnerRadius = 48.0f;
	int Num = m_World.FindEntities(Pos, Radius, (CEntity **)apEnts, MAX_CLIENTS, CGameWorld::ENTTYPE_CHARACTER);
	for(int i = 0; i < Num; i++)
	{
		vec2 Diff = apEnts[i]->m_Pos - Pos;
		vec2 ForceDir(0, 1);
		float l = length(Diff);
		if(l)
			ForceDir = normalize(Diff);
		l = 1 - clamp((l - InnerRadius) / (Radius - InnerRadius), 0.0f, 1.0f);
		float Strength;
		if(Owner == -1 || !m_apPlayers[Owner] || !m_apPlayers[Owner]->m_TuneZone)
			Strength = Tuning()->m_ExplosionStrength;
		else
			Strength = TuningList()[m_apPlayers[Owner]->m_TuneZone].m_ExplosionStrength;

		float Dmg = Strength * l;
		if(!(int)Dmg)
			continue;

		if((GetPlayerChar(Owner) && !(GetPlayerChar(Owner)->m_Hit & CCharacter::DISABLE_HIT_GRENADE) || NoDamage) || Owner == apEnts[i]->GetPlayer()->GetCID())
		{
			apEnts[i]->TakeDamage(ForceDir * Dmg * 2, (int)Dmg, Owner, Weapon);
		}
	}
}

void CGameContext::CreatePlayerSpawn(vec2 Pos)
{
	// create the event
	CNetEvent_Spawn *ev = (CNetEvent_Spawn *)m_Events.Create(NETEVENTTYPE_SPAWN, sizeof(CNetEvent_Spawn));
	if(ev)
	{
		ev->m_X = (int)Pos.x;
		ev->m_Y = (int)Pos.y;
	}
}

void CGameContext::CreateDeath(vec2 Pos, int ClientID)
{
	// create the event
	CNetEvent_Death *pEvent = (CNetEvent_Death *)m_Events.Create(NETEVENTTYPE_DEATH, sizeof(CNetEvent_Death));
	if(pEvent)
	{
		pEvent->m_X = (int)Pos.x;
		pEvent->m_Y = (int)Pos.y;
		pEvent->m_ClientID = ClientID;
	}
}

void CGameContext::CreateSound(vec2 Pos, int Sound)
{
	if(Sound < 0)
		return;

	// create a sound
	CNetEvent_SoundWorld *pEvent = (CNetEvent_SoundWorld *)m_Events.Create(NETEVENTTYPE_SOUNDWORLD, sizeof(CNetEvent_SoundWorld));
	if(pEvent)
	{
		pEvent->m_X = (int)Pos.x;
		pEvent->m_Y = (int)Pos.y;
		pEvent->m_SoundID = Sound;
	}
}

void CGameContext::CreateSoundGlobal(int Sound, int Target)
{
	if(Sound < 0)
		return;

	CNetMsg_Sv_SoundGlobal Msg;
	Msg.m_SoundID = Sound;
	if(Target == -2)
		Server()->SendPackMsg(&Msg, MSGFLAG_NOSEND, -1);
	else
	{
		int Flag = MSGFLAG_VITAL;
		if(Target != -1)
			Flag |= MSGFLAG_NORECORD;
		Server()->SendPackMsg(&Msg, Flag, Target);
	}
}

void CGameContext::SendChatTarget(int To, const char *pText, int Flags)
{
	CNetMsg_Sv_Chat Msg;
	Msg.m_Team = 0;
	Msg.m_ClientID = -1;
	Msg.m_pMessage = pText;

	if(g_Config.m_SvDemoChat)
		Server()->SendPackMsg(&Msg, MSGFLAG_VITAL | MSGFLAG_NOSEND, -1);

	if(To == -1)
	{
		for(int i = 0; i < MAX_CLIENTS; i++)
		{
			if(!((Server()->IsSixup(i) && (Flags & CHAT_SIXUP)) ||
				   (!Server()->IsSixup(i) && (Flags & CHAT_SIX))))
				continue;

			Server()->SendPackMsg(&Msg, MSGFLAG_VITAL | MSGFLAG_NORECORD, i);
		}
	}
	else
	{
		if(!((Server()->IsSixup(To) && (Flags & CHAT_SIXUP)) ||
			   (!Server()->IsSixup(To) && (Flags & CHAT_SIX))))
			return;

		Server()->SendPackMsg(&Msg, MSGFLAG_VITAL | MSGFLAG_NORECORD, To);
	}
}

void CGameContext::SendChatTeam(int Team, const char *pText)
{
	for(int i = 0; i < MAX_CLIENTS; i++)
		SendChatTarget(i, pText);
}

void CGameContext::SendChat(int ChatterClientID, int Team, const char *pText, int SpamProtectionClientID, int Flags)
{
	if(SpamProtectionClientID >= 0 && SpamProtectionClientID < MAX_CLIENTS)
		if(ProcessSpamProtection(SpamProtectionClientID))
			return;

	char aBuf[256], aText[256];
	str_copy(aText, pText, sizeof(aText));
	if(ChatterClientID >= 0 && ChatterClientID < MAX_CLIENTS)
		str_format(aBuf, sizeof(aBuf), "%d:%d:%s: %s", ChatterClientID, Team, Server()->ClientName(ChatterClientID), aText);
	else if(ChatterClientID == -2)
	{
		str_format(aBuf, sizeof(aBuf), "### %s", aText);
		str_copy(aText, aBuf, sizeof(aText));
		ChatterClientID = -1;
	}
	else
		str_format(aBuf, sizeof(aBuf), "*** %s", aText);
	Console()->Print(IConsole::OUTPUT_LEVEL_ADDINFO, Team != CHAT_ALL ? "teamchat" : "chat", aBuf);

	if(Team == CHAT_ALL)
	{
		CNetMsg_Sv_Chat Msg;
		Msg.m_Team = 0;
		Msg.m_ClientID = ChatterClientID;
		Msg.m_pMessage = aText;

		// pack one for the recording only
		if(g_Config.m_SvDemoChat)
			Server()->SendPackMsg(&Msg, MSGFLAG_VITAL | MSGFLAG_NOSEND, -1);

		// send to the clients
		for(int i = 0; i < MAX_CLIENTS; i++)
		{
			if(!m_apPlayers[i])
				continue;
			bool Send = (Server()->IsSixup(i) && (Flags & CHAT_SIXUP)) ||
				    (!Server()->IsSixup(i) && (Flags & CHAT_SIX));

			if(Send)
				Server()->SendPackMsg(&Msg, MSGFLAG_VITAL | MSGFLAG_NORECORD, i);
		}
	}
	else
	{
		CNetMsg_Sv_Chat Msg;
		Msg.m_Team = 1;
		Msg.m_ClientID = ChatterClientID;
		Msg.m_pMessage = aText;

		// pack one for the recording only
		if(g_Config.m_SvDemoChat)
			Server()->SendPackMsg(&Msg, MSGFLAG_VITAL | MSGFLAG_NOSEND, -1);

		// send to the clients
		for(int i = 0; i < MAX_CLIENTS; i++)
		{
			if(m_apPlayers[i] != 0)
			{
				if(Team == CHAT_SPEC)
				{
					if(m_apPlayers[i]->GetTeam() == CHAT_SPEC)
					{
						Server()->SendPackMsg(&Msg, MSGFLAG_VITAL | MSGFLAG_NORECORD, i);
					}
				}
				else
				{
					Server()->SendPackMsg(&Msg, MSGFLAG_VITAL | MSGFLAG_NORECORD, i);
				}
			}
		}
	}
}

void CGameContext::SendEmoticon(int ClientID, int Emoticon)
{
	CNetMsg_Sv_Emoticon Msg;
	Msg.m_ClientID = ClientID;
	Msg.m_Emoticon = Emoticon;
	Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, -1);
}

void CGameContext::SendWeaponPickup(int ClientID, int Weapon)
{
	CNetMsg_Sv_WeaponPickup Msg;
	Msg.m_Weapon = Weapon;
	Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, ClientID);
}

void CGameContext::SendMotd(int ClientID)
{
	CNetMsg_Sv_Motd Msg;
	Msg.m_pMessage = g_Config.m_SvMotd;
	Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, ClientID);
}

void CGameContext::SendSettings(int ClientID)
{
	if(Server()->IsSixup(ClientID))
	{
		protocol7::CNetMsg_Sv_ServerSettings Msg;
		Msg.m_KickVote = g_Config.m_SvVoteKick;
		Msg.m_KickMin = g_Config.m_SvVoteKickMin;
		Msg.m_SpecVote = g_Config.m_SvVoteSpectate;
		Msg.m_TeamLock = 0;
		Msg.m_TeamBalance = 0;
		Msg.m_PlayerSlots = g_Config.m_SvMaxClients - g_Config.m_SvSpectatorSlots;
		Server()->SendPackMsg(&Msg, MSGFLAG_VITAL | MSGFLAG_NORECORD, ClientID);
	}
}

void CGameContext::SendBroadcast(const char *pText, int ClientID, bool IsImportant)
{
	CNetMsg_Sv_Broadcast Msg;
	Msg.m_pMessage = pText;

	if(ClientID == -1)
	{
		dbg_assert(IsImportant, "broadcast messages to all players must be important");
		Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, ClientID);

		for(auto &pPlayer : m_apPlayers)
		{
			if(pPlayer)
			{
				pPlayer->m_LastBroadcastImportance = true;
				pPlayer->m_LastBroadcast = Server()->Tick();
			}
		}
		return;
	}

	if(!m_apPlayers[ClientID])
		return;

	if(!IsImportant && m_apPlayers[ClientID]->m_LastBroadcastImportance && m_apPlayers[ClientID]->m_LastBroadcast > Server()->Tick() - Server()->TickSpeed() * 10)
		return;

	Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, ClientID);
	m_apPlayers[ClientID]->m_LastBroadcast = Server()->Tick();
	m_apPlayers[ClientID]->m_LastBroadcastImportance = IsImportant;
}

void CGameContext::SendBroadcastToActivePlayers(const char *pText)
{
	CNetMsg_Sv_Broadcast Msg;
	Msg.m_pMessage = pText;

	for(auto &pPlayer : m_apPlayers)
	{
		if(pPlayer)
		{
			Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, pPlayer->GetCID());
		}
	}
}

void CGameContext::SendTuningParams(int ClientID, int Zone)
{
	if(ClientID == -1)
	{
		for(int i = 0; i < MAX_CLIENTS; ++i)
		{
			if(m_apPlayers[i])
			{
				if(m_apPlayers[i]->GetCharacter())
				{
					if(m_apPlayers[i]->GetCharacter()->m_TuneZone == Zone)
						SendTuningParams(i, Zone);
				}
				else if(m_apPlayers[i]->m_TuneZone == Zone)
				{
					SendTuningParams(i, Zone);
				}
			}
		}
		return;
	}

	CMsgPacker Msg(NETMSGTYPE_SV_TUNEPARAMS);
	int *pParams = 0;
	if(Zone == 0)
		pParams = (int *)&m_Tuning;
	else
		pParams = (int *)&(m_aTuningList[Zone]);

	unsigned int Last = sizeof(m_Tuning) / sizeof(int);
	if(m_apPlayers[ClientID])
	{
		int ClientVersion = m_apPlayers[ClientID]->GetClientVersion();
		if(ClientVersion < VERSION_DDNET_EXTRATUNES)
			Last = 33;
		else if(ClientVersion < VERSION_DDNET_HOOKDURATION_TUNE)
			Last = 37;
		else if(ClientVersion < VERSION_DDNET_FIREDELAY_TUNE)
			Last = 38;
	}

	for(unsigned i = 0; i < Last; i++)
	{
		if(m_apPlayers[ClientID] && m_apPlayers[ClientID]->GetCharacter())
		{
			if((i == 3) // ground jump impulse
				&& m_apPlayers[ClientID]->GetCharacter()->NeededFaketuning() & FAKETUNE_NOJUMP)
			{
				Msg.AddInt(0);
			}
			else if((i == 30) // laser_damage is removed from 0.7
				&& (Server()->IsSixup(ClientID)))
			{
				continue;
			}
			else if((i == 31) // collision
				&& (m_apPlayers[ClientID]->GetCharacter()->NeededFaketuning() & FAKETUNE_SOLO || m_apPlayers[ClientID]->GetCharacter()->NeededFaketuning() & FAKETUNE_NOCOLL))
			{
				Msg.AddInt(0);
			}
			else if((i == 32) // hooking
				&& (m_apPlayers[ClientID]->GetCharacter()->NeededFaketuning() & FAKETUNE_SOLO || m_apPlayers[ClientID]->GetCharacter()->NeededFaketuning() & FAKETUNE_NOHOOK))
			{
				Msg.AddInt(0);
			}
			else if((i == 33) // jetpack
				&& !(m_apPlayers[ClientID]->GetCharacter()->NeededFaketuning() & FAKETUNE_JETPACK))
			{
				Msg.AddInt(0);
			}
			else if((i == 36) // hammer hit
				&& m_apPlayers[ClientID]->GetCharacter()->NeededFaketuning() & FAKETUNE_NOHAMMER)
			{
				Msg.AddInt(0);
			}
			else
			{
				Msg.AddInt(pParams[i]);
			}
		}
		else
			Msg.AddInt(pParams[i]); // if everything is normal just send true tunings
	}
	Server()->SendMsg(&Msg, MSGFLAG_VITAL, ClientID);
}

void CGameContext::OnTick()
{
	if(m_TeeHistorianActive)
	{
		int Error = aio_error(m_pTeeHistorianFile);
		if(Error)
		{
			dbg_msg("teehistorian", "error writing to file, err=%d", Error);
			Server()->SetErrorShutdown("teehistorian io error");
		}

		if(!m_TeeHistorian.Starting())
		{
			m_TeeHistorian.EndInputs();
			m_TeeHistorian.EndTick();
		}
		m_TeeHistorian.BeginTick(Server()->Tick());
		m_TeeHistorian.BeginPlayers();
	}

	// copy tuning
	m_World.m_Core.m_Tuning[0] = m_Tuning;
	m_World.Tick();

	VoteManager()->Tick();

	//if(world.paused) // make sure that the game object always updates
	m_pController->Tick();

	if(m_TeeHistorianActive)
	{
		for(int i = 0; i < MAX_CLIENTS; i++)
		{
			if(m_apPlayers[i] && m_apPlayers[i]->GetCharacter())
			{
				CNetObj_CharacterCore Char;
				m_apPlayers[i]->GetCharacter()->GetCore().Write(&Char);
				m_TeeHistorian.RecordPlayer(i, &Char);
			}
			else
			{
				m_TeeHistorian.RecordDeadPlayer(i);
			}
		}
		m_TeeHistorian.EndPlayers();
		m_TeeHistorian.BeginInputs();
	}

	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(m_apPlayers[i])
		{
			m_apPlayers[i]->Tick();
			m_apPlayers[i]->PostTick();
		}
	}

	for(auto &pPlayer : m_apPlayers)
	{
		if(pPlayer)
			pPlayer->PostPostTick();
	}

	for(int i = 0; i < m_NumMutes; i++)
	{
		if(m_aMutes[i].m_Expire <= Server()->Tick())
		{
			m_NumMutes--;
			m_aMutes[i] = m_aMutes[m_NumMutes];
		}
	}

	for(int i = 0; i < m_NumVoteMutes; i++)
	{
		if(m_aVoteMutes[i].m_Expire <= Server()->Tick())
		{
			m_NumVoteMutes--;
			m_aVoteMutes[i] = m_aVoteMutes[m_NumVoteMutes];
		}
	}

	if(Server()->Tick() % (g_Config.m_SvAnnouncementInterval * Server()->TickSpeed() * 60) == 0)
	{
		const char *Line = Server()->GetAnnouncementLine(g_Config.m_SvAnnouncementFileName);
		if(Line)
			SendChat(-1, CHAT_ALL, Line);
	}

	if(Collision()->m_NumSwitchers > 0)
	{
		for(int i = 0; i < Collision()->m_NumSwitchers + 1; ++i)
		{
			if(Collision()->m_pSwitchers[i].m_EndTick <= Server()->Tick() && Collision()->m_pSwitchers[i].m_Type == TILE_SWITCHTIMEDOPEN)
			{
				Collision()->m_pSwitchers[i].m_Status = false;
				Collision()->m_pSwitchers[i].m_EndTick = 0;
				Collision()->m_pSwitchers[i].m_Type = TILE_SWITCHCLOSE;
			}
			else if(Collision()->m_pSwitchers[i].m_EndTick <= Server()->Tick() && Collision()->m_pSwitchers[i].m_Type == TILE_SWITCHTIMEDCLOSE)
			{
				Collision()->m_pSwitchers[i].m_Status = true;
				Collision()->m_pSwitchers[i].m_EndTick = 0;
				Collision()->m_pSwitchers[i].m_Type = TILE_SWITCHOPEN;
			}
		}
	}

#ifdef CONF_DEBUG
	if(g_Config.m_DbgDummies)
	{
		for(int i = 0; i < g_Config.m_DbgDummies; i++)
		{
			CNetObj_PlayerInput Input = {0};
			Input.m_Direction = (i & 1) ? -1 : 1;
			m_apPlayers[MAX_CLIENTS - i - 1]->OnPredictedInput(&Input);
		}
	}
#endif
}

// Server hooks
void CGameContext::OnClientDirectInput(int ClientID, void *pInput)
{
	if(!m_World.m_Paused)
		m_apPlayers[ClientID]->OnDirectInput((CNetObj_PlayerInput *)pInput);

	int Flags = ((CNetObj_PlayerInput *)pInput)->m_PlayerFlags;
	if((Flags & 256) || (Flags & 512))
	{
		Server()->Kick(ClientID, "please update your client or use DDNet client");
	}
}

void CGameContext::OnClientPredictedInput(int ClientID, void *pInput)
{
	if(!m_World.m_Paused)
		m_apPlayers[ClientID]->OnPredictedInput((CNetObj_PlayerInput *)pInput);
}

void CGameContext::OnClientPredictedEarlyInput(int ClientID, void *pInput)
{
	if(!m_World.m_Paused)
		m_apPlayers[ClientID]->OnPredictedEarlyInput((CNetObj_PlayerInput *)pInput);

	if(m_TeeHistorianActive)
	{
		m_TeeHistorian.RecordPlayerInput(ClientID, (CNetObj_PlayerInput *)pInput);
	}
}

void CGameContext::OnClientEnter(int ClientID)
{
	m_pController->OnPlayerConnect(m_apPlayers[ClientID]);

	if(Server()->IsSixup(ClientID))
	{
		{
			// DON'T SUPPORT 0.7, TODO: support 0.7
			protocol7::CNetMsg_Sv_GameInfo Msg;
			Msg.m_GameFlags = protocol7::GAMEFLAG_RACE;
			Msg.m_MatchCurrent = 1;
			Msg.m_MatchNum = 0;
			Msg.m_ScoreLimit = 0;
			Msg.m_TimeLimit = 0;
			Server()->SendPackMsg(&Msg, MSGFLAG_VITAL | MSGFLAG_NORECORD, ClientID);
		}

		for(const IConsole::CCommandInfo *pCmd = Console()->FirstCommandInfo(IConsole::ACCESS_LEVEL_USER, CFGFLAG_CHAT);
			pCmd; pCmd = pCmd->NextCommandInfo(IConsole::ACCESS_LEVEL_USER, CFGFLAG_CHAT))
		{
			if(!str_comp(pCmd->m_pName, "w") || !str_comp(pCmd->m_pName, "whisper"))
				continue;

			const char *pName = pCmd->m_pName;

			protocol7::CNetMsg_Sv_CommandInfo Msg;
			Msg.m_Name = pName;
			Msg.m_ArgsFormat = pCmd->m_pParams;
			Msg.m_HelpText = pCmd->m_pHelp;
			Server()->SendPackMsg(&Msg, MSGFLAG_VITAL | MSGFLAG_NORECORD, ClientID);
		}
	}

	if(!Server()->ClientPrevIngame(ClientID))
	{
		if(g_Config.m_SvWelcome[0] != 0)
			SendChatTarget(ClientID, g_Config.m_SvWelcome);

		IServer::CClientInfo Info;
		Server()->GetClientInfo(ClientID, &Info);
		if(Info.m_GotDDNetVersion)
		{
			if(OnClientDDNetVersionKnown(ClientID))
				return; // kicked
		}
	}
	VoteManager()->SetUpdating(true);

	// send active vote
	VoteManager()->OnClientEnter(ClientID);

	Server()->ExpireServerInfo();

	CPlayer *pNewPlayer = m_apPlayers[ClientID];

	// new info for others
	protocol7::CNetMsg_Sv_ClientInfo NewClientInfoMsg;
	NewClientInfoMsg.m_ClientID = ClientID;
	NewClientInfoMsg.m_Local = 0;
	NewClientInfoMsg.m_Team = pNewPlayer->GetTeam();
	NewClientInfoMsg.m_pName = Server()->ClientName(ClientID);
	NewClientInfoMsg.m_pClan = Server()->ClientClan(ClientID);
	NewClientInfoMsg.m_Country = Server()->ClientCountry(ClientID);
	NewClientInfoMsg.m_Silent = false;

	for(int p = 0; p < 6; p++)
	{
		NewClientInfoMsg.m_apSkinPartNames[p] = pNewPlayer->m_TeeInfos.m_apSkinPartNames[p];
		NewClientInfoMsg.m_aUseCustomColors[p] = pNewPlayer->m_TeeInfos.m_aUseCustomColors[p];
		NewClientInfoMsg.m_aSkinPartColors[p] = pNewPlayer->m_TeeInfos.m_aSkinPartColors[p];
	}

	// update client infos (others before local)
	for(int i = 0; i < MAX_CLIENTS; ++i)
	{
		if(i == ClientID || !m_apPlayers[i] || !Server()->ClientIngame(i))
			continue;

		CPlayer *pPlayer = m_apPlayers[i];

		if(Server()->IsSixup(i))
			Server()->SendPackMsg(&NewClientInfoMsg, MSGFLAG_VITAL | MSGFLAG_NORECORD, i);

		if(Server()->IsSixup(ClientID))
		{
			// existing infos for new player
			protocol7::CNetMsg_Sv_ClientInfo ClientInfoMsg;
			ClientInfoMsg.m_ClientID = i;
			ClientInfoMsg.m_Local = 0;
			ClientInfoMsg.m_Team = pPlayer->GetTeam();
			ClientInfoMsg.m_pName = Server()->ClientName(i);
			ClientInfoMsg.m_pClan = Server()->ClientClan(i);
			ClientInfoMsg.m_Country = Server()->ClientCountry(i);
			ClientInfoMsg.m_Silent = 0;

			for(int p = 0; p < 6; p++)
			{
				ClientInfoMsg.m_apSkinPartNames[p] = pPlayer->m_TeeInfos.m_apSkinPartNames[p];
				ClientInfoMsg.m_aUseCustomColors[p] = pPlayer->m_TeeInfos.m_aUseCustomColors[p];
				ClientInfoMsg.m_aSkinPartColors[p] = pPlayer->m_TeeInfos.m_aSkinPartColors[p];
			}

			Server()->SendPackMsg(&ClientInfoMsg, MSGFLAG_VITAL | MSGFLAG_NORECORD, ClientID);
		}
	}

	// local info
	if(Server()->IsSixup(ClientID))
	{
		NewClientInfoMsg.m_Local = 1;
		Server()->SendPackMsg(&NewClientInfoMsg, MSGFLAG_VITAL | MSGFLAG_NORECORD, ClientID);
	}
}

bool CGameContext::OnClientDataPersist(int ClientID, void *pData)
{
	CPersistentClientData *pPersistent = (CPersistentClientData *)pData;
	if(!m_apPlayers[ClientID])
	{
		return false;
	}
	pPersistent->m_IsSpectator = m_apPlayers[ClientID]->GetTeam() == TEAM_SPECTATORS;
	return true;
}

void CGameContext::OnClientConnected(int ClientID, void *pData)
{
	// Check which team the player should be on
	const int StartTeam = m_pController->GetAutoTeam(ClientID);

	if(!m_apPlayers[ClientID])
		m_apPlayers[ClientID] = new(ClientID) CPlayer(this, ClientID, StartTeam);
	else
	{
		delete m_apPlayers[ClientID];
		m_apPlayers[ClientID] = new(ClientID) CPlayer(this, ClientID, StartTeam);
		//	//m_apPlayers[ClientID]->Reset();
		//	//((CServer*)Server())->m_aClients[ClientID].Reset();
		//	((CServer*)Server())->m_aClients[ClientID].m_State = 4;
	}
	//players[client_id].init(client_id);
	//players[client_id].client_id = client_id;

#ifdef CONF_DEBUG
	if(g_Config.m_DbgDummies)
	{
		if(ClientID >= MAX_CLIENTS - g_Config.m_DbgDummies)
			return;
	}
#endif

	SendMotd(ClientID);
	SendSettings(ClientID);

	Server()->ExpireServerInfo();
}

void CGameContext::OnClientDrop(int ClientID, const char *pReason)
{
	VoteManager()->AbortVoteKickOnDisconnect(ClientID);
	m_pController->OnPlayerDisconnect(m_apPlayers[ClientID], pReason);
	delete m_apPlayers[ClientID];
	m_apPlayers[ClientID] = 0;

	VoteManager()->SetUpdating(true);

	// update spectator modes
	for(auto &pPlayer : m_apPlayers)
	{
		if(pPlayer && pPlayer->m_SpectatorID == ClientID)
			pPlayer->m_SpectatorID = SPEC_FREEVIEW;
	}

	// update conversation targets
	for(auto &pPlayer : m_apPlayers)
	{
		if(pPlayer && pPlayer->m_LastWhisperTo == ClientID)
			pPlayer->m_LastWhisperTo = -1;
	}

	protocol7::CNetMsg_Sv_ClientDrop Msg;
	Msg.m_ClientID = ClientID;
	Msg.m_pReason = pReason;
	Msg.m_Silent = false;
	Server()->SendPackMsg(&Msg, MSGFLAG_VITAL | MSGFLAG_NORECORD, -1);

	Server()->ExpireServerInfo();
}

void CGameContext::OnClientEngineJoin(int ClientID, bool Sixup)
{
	if(m_TeeHistorianActive)
	{
		m_TeeHistorian.RecordPlayerJoin(ClientID, !Sixup ? CTeeHistorian::PROTOCOL_6 : CTeeHistorian::PROTOCOL_7);
	}
}

void CGameContext::OnClientEngineDrop(int ClientID, const char *pReason)
{
	if(m_TeeHistorianActive)
	{
		m_TeeHistorian.RecordPlayerDrop(ClientID, pReason);
	}
}

bool CGameContext::OnClientDDNetVersionKnown(int ClientID)
{
	IServer::CClientInfo Info;
	Server()->GetClientInfo(ClientID, &Info);
	int ClientVersion = Info.m_DDNetVersion;
	dbg_msg("ddnet", "cid=%d version=%d", ClientID, ClientVersion);

	if(m_TeeHistorianActive)
	{
		if(Info.m_pConnectionID && Info.m_pDDNetVersionStr)
		{
			m_TeeHistorian.RecordDDNetVersion(ClientID, *Info.m_pConnectionID, ClientVersion, Info.m_pDDNetVersionStr);
		}
		else
		{
			m_TeeHistorian.RecordDDNetVersionOld(ClientID, ClientVersion);
		}
	}

	// Autoban known bot versions.
	if(g_Config.m_SvBannedVersions[0] != '\0' && IsVersionBanned(ClientVersion))
	{
		Server()->Kick(ClientID, "unsupported client");
		return true;
	}

	CPlayer *pPlayer = m_apPlayers[ClientID];
	if(ClientVersion >= VERSION_DDNET_GAMETICK)
		pPlayer->m_TimerType = 0;

	// And report correct tunings.
	if(ClientVersion >= VERSION_DDNET_EXTRATUNES)
		SendTuningParams(ClientID, pPlayer->m_TuneZone);

	return false;
}

void *CGameContext::PreProcessMsg(int *MsgID, CUnpacker *pUnpacker, int ClientID)
{
	if(Server()->IsSixup(ClientID) && *MsgID < OFFSET_UUID)
	{
		void *pRawMsg = m_NetObjHandler7.SecureUnpackMsg(*MsgID, pUnpacker);
		if(!pRawMsg)
			return 0;

		CPlayer *pPlayer = m_apPlayers[ClientID];
		static char s_aRawMsg[1024];

		if(*MsgID == protocol7::NETMSGTYPE_CL_SAY)
		{
			protocol7::CNetMsg_Cl_Say *pMsg7 = (protocol7::CNetMsg_Cl_Say *)pRawMsg;
			// Should probably use a placement new to start the lifetime of the object to avoid future weirdness
			::CNetMsg_Cl_Say *pMsg = (::CNetMsg_Cl_Say *)s_aRawMsg;

			if(pMsg7->m_Target >= 0)
			{
				if(ProcessSpamProtection(ClientID))
					return 0;

				// Should we maybe recraft the message so that it can go through the usual path?
				WhisperID(ClientID, pMsg7->m_Target, pMsg7->m_pMessage);
				return 0;
			}

			pMsg->m_Team = pMsg7->m_Mode == protocol7::CHAT_TEAM;
			pMsg->m_pMessage = pMsg7->m_pMessage;
		}
		else if(*MsgID == protocol7::NETMSGTYPE_CL_STARTINFO)
		{
			protocol7::CNetMsg_Cl_StartInfo *pMsg7 = (protocol7::CNetMsg_Cl_StartInfo *)pRawMsg;
			::CNetMsg_Cl_StartInfo *pMsg = (::CNetMsg_Cl_StartInfo *)s_aRawMsg;

			pMsg->m_pName = pMsg7->m_pName;
			pMsg->m_pClan = pMsg7->m_pClan;
			pMsg->m_Country = pMsg7->m_Country;

			CTeeInfo Info(pMsg7->m_apSkinPartNames, pMsg7->m_aUseCustomColors, pMsg7->m_aSkinPartColors);
			Info.FromSixup();
			pPlayer->m_TeeInfos = Info;

			str_copy(s_aRawMsg + sizeof(*pMsg), Info.m_SkinName, sizeof(s_aRawMsg) - sizeof(*pMsg));

			pMsg->m_pSkin = s_aRawMsg + sizeof(*pMsg);
			pMsg->m_UseCustomColor = pPlayer->m_TeeInfos.m_UseCustomColor;
			pMsg->m_ColorBody = pPlayer->m_TeeInfos.m_ColorBody;
			pMsg->m_ColorFeet = pPlayer->m_TeeInfos.m_ColorFeet;
		}
		else if(*MsgID == protocol7::NETMSGTYPE_CL_SKINCHANGE)
		{
			protocol7::CNetMsg_Cl_SkinChange *pMsg = (protocol7::CNetMsg_Cl_SkinChange *)pRawMsg;
			if(g_Config.m_SvSpamprotection && pPlayer->m_LastChangeInfo &&
				pPlayer->m_LastChangeInfo + Server()->TickSpeed() * g_Config.m_SvInfoChangeDelay > Server()->Tick())
				return 0;

			pPlayer->m_LastChangeInfo = Server()->Tick();

			CTeeInfo Info(pMsg->m_apSkinPartNames, pMsg->m_aUseCustomColors, pMsg->m_aSkinPartColors);
			Info.FromSixup();
			pPlayer->m_TeeInfos = Info;

			protocol7::CNetMsg_Sv_SkinChange Msg;
			Msg.m_ClientID = ClientID;
			for(int p = 0; p < 6; p++)
			{
				Msg.m_apSkinPartNames[p] = pMsg->m_apSkinPartNames[p];
				Msg.m_aSkinPartColors[p] = pMsg->m_aSkinPartColors[p];
				Msg.m_aUseCustomColors[p] = pMsg->m_aUseCustomColors[p];
			}

			Server()->SendPackMsg(&Msg, MSGFLAG_VITAL | MSGFLAG_NORECORD, -1);

			return 0;
		}
		else if(*MsgID == protocol7::NETMSGTYPE_CL_SETSPECTATORMODE)
		{
			protocol7::CNetMsg_Cl_SetSpectatorMode *pMsg7 = (protocol7::CNetMsg_Cl_SetSpectatorMode *)pRawMsg;
			::CNetMsg_Cl_SetSpectatorMode *pMsg = (::CNetMsg_Cl_SetSpectatorMode *)s_aRawMsg;

			if(pMsg7->m_SpecMode == protocol7::SPEC_FREEVIEW)
				pMsg->m_SpectatorID = SPEC_FREEVIEW;
			else if(pMsg7->m_SpecMode == protocol7::SPEC_PLAYER)
				pMsg->m_SpectatorID = pMsg7->m_SpectatorID;
			else
				pMsg->m_SpectatorID = SPEC_FREEVIEW; // Probably not needed
		}
		else if(*MsgID == protocol7::NETMSGTYPE_CL_SETTEAM)
		{
			protocol7::CNetMsg_Cl_SetTeam *pMsg7 = (protocol7::CNetMsg_Cl_SetTeam *)pRawMsg;
			::CNetMsg_Cl_SetTeam *pMsg = (::CNetMsg_Cl_SetTeam *)s_aRawMsg;

			pMsg->m_Team = pMsg7->m_Team;
		}
		else if(*MsgID == protocol7::NETMSGTYPE_CL_COMMAND)
		{
			protocol7::CNetMsg_Cl_Command *pMsg7 = (protocol7::CNetMsg_Cl_Command *)pRawMsg;
			::CNetMsg_Cl_Say *pMsg = (::CNetMsg_Cl_Say *)s_aRawMsg;

			str_format(s_aRawMsg + sizeof(*pMsg), sizeof(s_aRawMsg) - sizeof(*pMsg), "/%s %s", pMsg7->m_Name, pMsg7->m_Arguments);
			pMsg->m_pMessage = s_aRawMsg + sizeof(*pMsg);
			dbg_msg("debug", "line='%s'", s_aRawMsg + sizeof(*pMsg));
			pMsg->m_Team = 0;

			*MsgID = NETMSGTYPE_CL_SAY;
			return s_aRawMsg;
		}
		else if(*MsgID == protocol7::NETMSGTYPE_CL_CALLVOTE)
		{
			protocol7::CNetMsg_Cl_CallVote *pMsg7 = (protocol7::CNetMsg_Cl_CallVote *)pRawMsg;
			::CNetMsg_Cl_CallVote *pMsg = (::CNetMsg_Cl_CallVote *)s_aRawMsg;

			int Authed = Server()->GetAuthedState(ClientID);
			if(pMsg7->m_Force)
			{
				str_format(s_aRawMsg, sizeof(s_aRawMsg), "force_vote \"%s\" \"%s\" \"%s\"", pMsg7->m_Type, pMsg7->m_Value, pMsg7->m_Reason);
				Console()->SetAccessLevel(Authed == AUTHED_ADMIN ? IConsole::ACCESS_LEVEL_ADMIN : Authed == AUTHED_MOD ? IConsole::ACCESS_LEVEL_MOD : IConsole::ACCESS_LEVEL_HELPER);
				Console()->ExecuteLine(s_aRawMsg, ClientID, false);
				Console()->SetAccessLevel(IConsole::ACCESS_LEVEL_ADMIN);
				return 0;
			}

			pMsg->m_Value = pMsg7->m_Value;
			pMsg->m_Reason = pMsg7->m_Reason;
			pMsg->m_Type = pMsg7->m_Type;
		}
		else if(*MsgID == protocol7::NETMSGTYPE_CL_EMOTICON)
		{
			protocol7::CNetMsg_Cl_Emoticon *pMsg7 = (protocol7::CNetMsg_Cl_Emoticon *)pRawMsg;
			::CNetMsg_Cl_Emoticon *pMsg = (::CNetMsg_Cl_Emoticon *)s_aRawMsg;

			pMsg->m_Emoticon = pMsg7->m_Emoticon;
		}
		else if(*MsgID == protocol7::NETMSGTYPE_CL_VOTE)
		{
			protocol7::CNetMsg_Cl_Vote *pMsg7 = (protocol7::CNetMsg_Cl_Vote *)pRawMsg;
			::CNetMsg_Cl_Vote *pMsg = (::CNetMsg_Cl_Vote *)s_aRawMsg;

			pMsg->m_Vote = pMsg7->m_Vote;
		}

		*MsgID = Msg_SevenToSix(*MsgID);

		return s_aRawMsg;
	}
	else
		return m_NetObjHandler.SecureUnpackMsg(*MsgID, pUnpacker);
}

void CGameContext::OnMessage(int MsgID, CUnpacker *pUnpacker, int ClientID)
{
	if(m_TeeHistorianActive)
	{
		if(m_NetObjHandler.TeeHistorianRecordMsg(MsgID))
		{
			m_TeeHistorian.RecordPlayerMessage(ClientID, pUnpacker->CompleteData(), pUnpacker->CompleteSize());
		}
	}

	void *pRawMsg = PreProcessMsg(&MsgID, pUnpacker, ClientID);

	if(!pRawMsg)
		return;

	CPlayer *pPlayer = m_apPlayers[ClientID];

	if(Server()->ClientIngame(ClientID))
	{
		if(MsgID == NETMSGTYPE_CL_SAY)
		{
			CNetMsg_Cl_Say *pMsg = (CNetMsg_Cl_Say *)pRawMsg;
			if(!str_utf8_check(pMsg->m_pMessage))
			{
				return;
			}
			int Team = pMsg->m_Team;

			// trim right and set maximum length to 256 utf8-characters
			int Length = 0;
			const char *p = pMsg->m_pMessage;
			const char *pEnd = 0;
			while(*p)
			{
				const char *pStrOld = p;
				int Code = str_utf8_decode(&p);

				// check if unicode is not empty
				if(!str_utf8_isspace(Code))
				{
					pEnd = 0;
				}
				else if(pEnd == 0)
					pEnd = pStrOld;

				if(++Length >= 256)
				{
					*(const_cast<char *>(p)) = 0;
					break;
				}
			}
			if(pEnd != 0)
				*(const_cast<char *>(pEnd)) = 0;

			// drop empty and autocreated spam messages (more than 32 characters per second)
			if(Length == 0 || (pMsg->m_pMessage[0] != '/' && (g_Config.m_SvSpamprotection && pPlayer->m_LastChat && pPlayer->m_LastChat + Server()->TickSpeed() * ((31 + Length) / 32) > Server()->Tick())))
				return;

			pPlayer->UpdatePlaytime();

			Team = CHAT_ALL;

			if(pMsg->m_pMessage[0] == '/')
			{
				if(str_comp_nocase_num(pMsg->m_pMessage + 1, "w ", 2) == 0)
				{
					char aWhisperMsg[256];
					str_copy(aWhisperMsg, pMsg->m_pMessage + 3, 256);
					Whisper(pPlayer->GetCID(), aWhisperMsg);
				}
				else if(str_comp_nocase_num(pMsg->m_pMessage + 1, "whisper ", 8) == 0)
				{
					char aWhisperMsg[256];
					str_copy(aWhisperMsg, pMsg->m_pMessage + 9, 256);
					Whisper(pPlayer->GetCID(), aWhisperMsg);
				}
				else if(str_comp_nocase_num(pMsg->m_pMessage + 1, "c ", 2) == 0)
				{
					char aWhisperMsg[256];
					str_copy(aWhisperMsg, pMsg->m_pMessage + 3, 256);
					Converse(pPlayer->GetCID(), aWhisperMsg);
				}
				else if(str_comp_nocase_num(pMsg->m_pMessage + 1, "converse ", 9) == 0)
				{
					char aWhisperMsg[256];
					str_copy(aWhisperMsg, pMsg->m_pMessage + 10, 256);
					Converse(pPlayer->GetCID(), aWhisperMsg);
				}
				else
				{
					if(g_Config.m_SvSpamprotection && pPlayer->m_LastCommands[0] && pPlayer->m_LastCommands[0] + Server()->TickSpeed() > Server()->Tick() && pPlayer->m_LastCommands[1] && pPlayer->m_LastCommands[1] + Server()->TickSpeed() > Server()->Tick() && pPlayer->m_LastCommands[2] && pPlayer->m_LastCommands[2] + Server()->TickSpeed() > Server()->Tick() && pPlayer->m_LastCommands[3] && pPlayer->m_LastCommands[3] + Server()->TickSpeed() > Server()->Tick())
						return;

					int64 Now = Server()->Tick();
					pPlayer->m_LastCommands[pPlayer->m_LastCommandPos] = Now;
					pPlayer->m_LastCommandPos = (pPlayer->m_LastCommandPos + 1) % 4;

					m_ChatResponseTargetID = ClientID;
					Server()->RestrictRconOutput(ClientID);
					Console()->SetFlagMask(CFGFLAG_CHAT);

					int Authed = Server()->GetAuthedState(ClientID);
					if(Authed)
						Console()->SetAccessLevel(Authed == AUTHED_ADMIN ? IConsole::ACCESS_LEVEL_ADMIN : Authed == AUTHED_MOD ? IConsole::ACCESS_LEVEL_MOD : IConsole::ACCESS_LEVEL_HELPER);
					else
						Console()->SetAccessLevel(IConsole::ACCESS_LEVEL_USER);
					Console()->SetPrintOutputLevel(m_ChatPrintCBIndex, 0);

					Console()->ExecuteLine(pMsg->m_pMessage + 1, ClientID, false);
					// m_apPlayers[ClientID] can be NULL, if the player used a
					// timeout code and replaced another client.
					if (g_Config.m_Debug)
					{
						char aBuf[256];
						str_format(aBuf, sizeof(aBuf), "%d used %s", ClientID, pMsg->m_pMessage);
						Console()->Print(IConsole::OUTPUT_LEVEL_DEBUG, "chat-command", aBuf);
					}

					Console()->SetAccessLevel(IConsole::ACCESS_LEVEL_ADMIN);
					Console()->SetFlagMask(CFGFLAG_SERVER);
					m_ChatResponseTargetID = -1;
					Server()->RestrictRconOutput(-1);
				}
			}
			else
			{
				SendChat(ClientID, Team, pMsg->m_pMessage, ClientID);
			}
		}
		else if(MsgID == NETMSGTYPE_CL_CALLVOTE)
		{
			m_apPlayers[ClientID]->UpdatePlaytime();

			VoteManager()->TryCallVote(ClientID, (CNetMsg_Cl_CallVote *)pRawMsg);
		}
		else if(MsgID == NETMSGTYPE_CL_VOTE)
		{
			if(!VoteManager()->IsActive())
				return;

			if(g_Config.m_SvSpamprotection && pPlayer->m_LastVoteTry && pPlayer->m_LastVoteTry + Server()->TickSpeed() * 3 > Server()->Tick())
				return;

			pPlayer->m_LastVoteTry = Server()->Tick();
			pPlayer->UpdatePlaytime();

			CNetMsg_Cl_Vote *pMsg = (CNetMsg_Cl_Vote *)pRawMsg;
			if(!pMsg->m_Vote)
				return;

			pPlayer->m_Vote = pMsg->m_Vote;
			pPlayer->m_VotePos = VoteManager()->GetVotePos();
			VoteManager()->SetUpdating(true);
		}
		else if(MsgID == NETMSGTYPE_CL_SETTEAM && !m_World.m_Paused)
		{
			CNetMsg_Cl_SetTeam *pMsg = (CNetMsg_Cl_SetTeam *)pRawMsg;

			if(pPlayer->GetTeam() == pMsg->m_Team || (g_Config.m_SvSpamprotection && pPlayer->m_LastSetTeam && pPlayer->m_LastSetTeam + Server()->TickSpeed() * g_Config.m_SvTeamChangeDelay > Server()->Tick()))
				return;

			if(pPlayer->m_TeamChangeTick > Server()->Tick())
			{
				pPlayer->m_LastSetTeam = Server()->Tick();
				int TimeLeft = (pPlayer->m_TeamChangeTick - Server()->Tick()) / Server()->TickSpeed();
				char aTime[32];
				str_time((int64)TimeLeft * 100, TIME_HOURS, aTime, sizeof(aTime));
				char aBuf[128];
				str_format(aBuf, sizeof(aBuf), "Time to wait before changing team: %s", aTime);
				SendBroadcast(aBuf, ClientID);
				return;
			}

			// Switch team on given client and kill/respawn him
			if(m_pController->CanJoinTeam(pMsg->m_Team, ClientID))
			{
				if(pPlayer->GetTeam() == TEAM_SPECTATORS || pMsg->m_Team == TEAM_SPECTATORS)
					VoteManager()->SetUpdating(true);
				m_pController->DoTeamChange(pPlayer, pMsg->m_Team);
				pPlayer->m_TeamChangeTick = Server()->Tick();
			}
			else
			{
				char aBuf[128];
				str_format(aBuf, sizeof(aBuf), "Only %d active players are allowed", Server()->MaxClients() - g_Config.m_SvSpectatorSlots);
				SendBroadcast(aBuf, ClientID);
			}
		}
		else if(MsgID == NETMSGTYPE_CL_ISDDNETLEGACY)
		{
			IServer::CClientInfo Info;
			Server()->GetClientInfo(ClientID, &Info);
			if(Info.m_GotDDNetVersion)
			{
				return;
			}
			int DDNetVersion = pUnpacker->GetInt();
			if(pUnpacker->Error() || DDNetVersion < 0)
			{
				DDNetVersion = VERSION_DDRACE;
			}
			Server()->SetClientDDNetVersion(ClientID, DDNetVersion);
			OnClientDDNetVersionKnown(ClientID);
		}
		else if(MsgID == NETMSGTYPE_CL_SHOWOTHERSLEGACY)
		{

		}
		else if(MsgID == NETMSGTYPE_CL_SHOWOTHERS)
		{

		}
		else if(MsgID == NETMSGTYPE_CL_SHOWDISTANCE)
		{
			CNetMsg_Cl_ShowDistance *pMsg = (CNetMsg_Cl_ShowDistance *)pRawMsg;
			pPlayer->m_ShowDistance = vec2(pMsg->m_X, pMsg->m_Y);
		}
		else if(MsgID == NETMSGTYPE_CL_SETSPECTATORMODE && !m_World.m_Paused)
		{
			CNetMsg_Cl_SetSpectatorMode *pMsg = (CNetMsg_Cl_SetSpectatorMode *)pRawMsg;

			pMsg->m_SpectatorID = clamp(pMsg->m_SpectatorID, (int)SPEC_FOLLOW, MAX_CLIENTS - 1);

			if(pMsg->m_SpectatorID >= 0)
				if(!Server()->ReverseTranslate(pMsg->m_SpectatorID, ClientID))
					return;

			if((g_Config.m_SvSpamprotection && pPlayer->m_LastSetSpectatorMode && pPlayer->m_LastSetSpectatorMode + Server()->TickSpeed() / 4 > Server()->Tick()))
				return;

			pPlayer->m_LastSetSpectatorMode = Server()->Tick();
			pPlayer->UpdatePlaytime();
			if(pMsg->m_SpectatorID >= 0 && (!m_apPlayers[pMsg->m_SpectatorID] || m_apPlayers[pMsg->m_SpectatorID]->GetTeam() == TEAM_SPECTATORS))
				SendChatTarget(ClientID, "Invalid spectator id used");
			else
				pPlayer->m_SpectatorID = pMsg->m_SpectatorID;
		}
		else if(MsgID == NETMSGTYPE_CL_CHANGEINFO)
		{
			if(g_Config.m_SvSpamprotection && pPlayer->m_LastChangeInfo && pPlayer->m_LastChangeInfo + Server()->TickSpeed() * g_Config.m_SvInfoChangeDelay > Server()->Tick())
				return;

			bool SixupNeedsUpdate = false;

			CNetMsg_Cl_ChangeInfo *pMsg = (CNetMsg_Cl_ChangeInfo *)pRawMsg;
			if(!str_utf8_check(pMsg->m_pName) || !str_utf8_check(pMsg->m_pClan) || !str_utf8_check(pMsg->m_pSkin))
			{
				return;
			}
			pPlayer->m_LastChangeInfo = Server()->Tick();
			pPlayer->UpdatePlaytime();

			// set infos
			if(Server()->WouldClientNameChange(ClientID, pMsg->m_pName) && !ProcessSpamProtection(ClientID))
			{
				char aOldName[MAX_NAME_LENGTH];
				str_copy(aOldName, Server()->ClientName(ClientID), sizeof(aOldName));

				Server()->SetClientName(ClientID, pMsg->m_pName);

				char aChatText[256];
				str_format(aChatText, sizeof(aChatText), "'%s' changed name to '%s'", aOldName, Server()->ClientName(ClientID));
				SendChat(-1, CHAT_ALL, aChatText);

				m_apPlayers[ClientID]->m_Score = 0;

				SixupNeedsUpdate = true;
			}

			if(str_comp(Server()->ClientClan(ClientID), pMsg->m_pClan))
				SixupNeedsUpdate = true;
			Server()->SetClientClan(ClientID, pMsg->m_pClan);

			if(Server()->ClientCountry(ClientID) != pMsg->m_Country)
				SixupNeedsUpdate = true;
			Server()->SetClientCountry(ClientID, pMsg->m_Country);

			str_copy(pPlayer->m_TeeInfos.m_SkinName, pMsg->m_pSkin, sizeof(pPlayer->m_TeeInfos.m_SkinName));
			pPlayer->m_TeeInfos.m_UseCustomColor = pMsg->m_UseCustomColor;
			pPlayer->m_TeeInfos.m_ColorBody = pMsg->m_ColorBody;
			pPlayer->m_TeeInfos.m_ColorFeet = pMsg->m_ColorFeet;
			if(!Server()->IsSixup(ClientID))
				pPlayer->m_TeeInfos.ToSixup();

			if(SixupNeedsUpdate)
			{
				protocol7::CNetMsg_Sv_ClientDrop Drop;
				Drop.m_ClientID = ClientID;
				Drop.m_pReason = "";
				Drop.m_Silent = true;

				protocol7::CNetMsg_Sv_ClientInfo Info;
				Info.m_ClientID = ClientID;
				Info.m_pName = Server()->ClientName(ClientID);
				Info.m_Country = pMsg->m_Country;
				Info.m_pClan = pMsg->m_pClan;
				Info.m_Local = 0;
				Info.m_Silent = true;
				Info.m_Team = pPlayer->GetTeam();

				for(int p = 0; p < 6; p++)
				{
					Info.m_apSkinPartNames[p] = pPlayer->m_TeeInfos.m_apSkinPartNames[p];
					Info.m_aSkinPartColors[p] = pPlayer->m_TeeInfos.m_aSkinPartColors[p];
					Info.m_aUseCustomColors[p] = pPlayer->m_TeeInfos.m_aUseCustomColors[p];
				}

				for(int i = 0; i < MAX_CLIENTS; i++)
				{
					if(i != ClientID)
					{
						Server()->SendPackMsg(&Drop, MSGFLAG_VITAL | MSGFLAG_NORECORD, i);
						Server()->SendPackMsg(&Info, MSGFLAG_VITAL | MSGFLAG_NORECORD, i);
					}
				}
			}
			else
			{
				protocol7::CNetMsg_Sv_SkinChange Msg;
				Msg.m_ClientID = ClientID;
				for(int p = 0; p < 6; p++)
				{
					Msg.m_apSkinPartNames[p] = pPlayer->m_TeeInfos.m_apSkinPartNames[p];
					Msg.m_aSkinPartColors[p] = pPlayer->m_TeeInfos.m_aSkinPartColors[p];
					Msg.m_aUseCustomColors[p] = pPlayer->m_TeeInfos.m_aUseCustomColors[p];
				}

				Server()->SendPackMsg(&Msg, MSGFLAG_VITAL | MSGFLAG_NORECORD, -1);
			}

			Server()->ExpireServerInfo();
		}
		else if(MsgID == NETMSGTYPE_CL_EMOTICON && !m_World.m_Paused)
		{
			CNetMsg_Cl_Emoticon *pMsg = (CNetMsg_Cl_Emoticon *)pRawMsg;

			if(g_Config.m_SvSpamprotection && pPlayer->m_LastEmote && pPlayer->m_LastEmote + Server()->TickSpeed() * g_Config.m_SvEmoticonDelay > Server()->Tick())
				return;

			pPlayer->m_LastEmote = Server()->Tick();
			pPlayer->UpdatePlaytime();

			SendEmoticon(ClientID, pMsg->m_Emoticon);
			CCharacter *pChr = pPlayer->GetCharacter();
			if(pChr && g_Config.m_SvEmotionalTees && pPlayer->m_EyeEmoteEnabled)
			{
				int EmoteType = EMOTE_NORMAL;
				switch(pMsg->m_Emoticon)
				{
				case EMOTICON_EXCLAMATION:
				case EMOTICON_GHOST:
				case EMOTICON_QUESTION:
				case EMOTICON_WTF:
					EmoteType = EMOTE_SURPRISE;
					break;
				case EMOTICON_DOTDOT:
				case EMOTICON_DROP:
				case EMOTICON_ZZZ:
					EmoteType = EMOTE_BLINK;
					break;
				case EMOTICON_EYES:
				case EMOTICON_HEARTS:
				case EMOTICON_MUSIC:
					EmoteType = EMOTE_HAPPY;
					break;
				case EMOTICON_OOP:
				case EMOTICON_SORRY:
				case EMOTICON_SUSHI:
					EmoteType = EMOTE_PAIN;
					break;
				case EMOTICON_DEVILTEE:
				case EMOTICON_SPLATTEE:
				case EMOTICON_ZOMG:
					EmoteType = EMOTE_ANGRY;
					break;
				default:
					break;
				}
				pChr->SetEmote(EmoteType, Server()->Tick() + 2 * Server()->TickSpeed());
			}
		}
		else if(MsgID == NETMSGTYPE_CL_KILL && !m_World.m_Paused)
		{
			if(pPlayer->m_LastKill && pPlayer->m_LastKill + Server()->TickSpeed() * g_Config.m_SvKillDelay > Server()->Tick())
				return;

			CCharacter *pChr = pPlayer->GetCharacter();
			if(!pChr)
				return;

			pPlayer->m_LastKill = Server()->Tick();
			pPlayer->KillCharacter(WEAPON_SELF);
			pPlayer->Respawn();
		}
	}
	if(MsgID == NETMSGTYPE_CL_STARTINFO)
	{
		if(pPlayer->m_IsReady)
			return;

		CNetMsg_Cl_StartInfo *pMsg = (CNetMsg_Cl_StartInfo *)pRawMsg;

		if(!str_utf8_check(pMsg->m_pName))
		{
			Server()->Kick(ClientID, "name is not valid utf8");
			return;
		}
		if(!str_utf8_check(pMsg->m_pClan))
		{
			Server()->Kick(ClientID, "clan is not valid utf8");
			return;
		}
		if(!str_utf8_check(pMsg->m_pSkin))
		{
			Server()->Kick(ClientID, "skin is not valid utf8");
			return;
		}

		pPlayer->m_LastChangeInfo = Server()->Tick();

		// set start infos
		Server()->SetClientName(ClientID, pMsg->m_pName);
		Server()->SetClientClan(ClientID, pMsg->m_pClan);
		Server()->SetClientCountry(ClientID, pMsg->m_Country);
		str_copy(pPlayer->m_TeeInfos.m_SkinName, pMsg->m_pSkin, sizeof(pPlayer->m_TeeInfos.m_SkinName));
		pPlayer->m_TeeInfos.m_UseCustomColor = pMsg->m_UseCustomColor;
		pPlayer->m_TeeInfos.m_ColorBody = pMsg->m_ColorBody;
		pPlayer->m_TeeInfos.m_ColorFeet = pMsg->m_ColorFeet;
		if(!Server()->IsSixup(ClientID))
			pPlayer->m_TeeInfos.ToSixup();

		// send clear vote options
		CNetMsg_Sv_VoteClearOptions ClearMsg;
		Server()->SendPackMsg(&ClearMsg, MSGFLAG_VITAL, ClientID);

		// begin sending vote options
		pPlayer->m_SendVoteIndex = 0;

		// send tuning parameters to client
		SendTuningParams(ClientID, pPlayer->m_TuneZone);

		// client is ready to enter
		pPlayer->m_IsReady = true;
		CNetMsg_Sv_ReadyToEnter m;
		Server()->SendPackMsg(&m, MSGFLAG_VITAL | MSGFLAG_FLUSH, ClientID);

		Server()->ExpireServerInfo();
	}
}

void CGameContext::ConTuneParam(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	const char *pParamName = pResult->GetString(0);
	float NewValue = pResult->GetFloat(1);

	if(pSelf->Tuning()->Set(pParamName, NewValue))
	{
		char aBuf[256];
		str_format(aBuf, sizeof(aBuf), "%s changed to %.2f", pParamName, NewValue);
		pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "tuning", aBuf);
		pSelf->SendTuningParams(-1);
	}
	else
		pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "tuning", "No such tuning parameter");
}

void CGameContext::ConToggleTuneParam(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	const char *pParamName = pResult->GetString(0);
	float OldValue;

	if(!pSelf->Tuning()->Get(pParamName, &OldValue))
	{
		pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "tuning", "No such tuning parameter");
		return;
	}

	float NewValue = fabs(OldValue - pResult->GetFloat(1)) < 0.0001f ? pResult->GetFloat(2) : pResult->GetFloat(1);

	pSelf->Tuning()->Set(pParamName, NewValue);

	char aBuf[256];
	str_format(aBuf, sizeof(aBuf), "%s changed to %.2f", pParamName, NewValue);
	pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "tuning", aBuf);
	pSelf->SendTuningParams(-1);
}

void CGameContext::ConTuneReset(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	/*CTuningParams TuningParams;
	*pSelf->Tuning() = TuningParams;
	pSelf->SendTuningParams(-1);
	pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "tuning", "Tuning reset");*/
	pSelf->ResetTuning();
	pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "tuning", "Tuning reset");
}

void CGameContext::ConTuneDump(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	char aBuf[256];
	for(int i = 0; i < pSelf->Tuning()->Num(); i++)
	{
		float v;
		pSelf->Tuning()->Get(i, &v);
		str_format(aBuf, sizeof(aBuf), "%s %.2f", pSelf->Tuning()->ms_apNames[i], v);
		pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "tuning", aBuf);
	}
}

void CGameContext::ConTuneZone(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	int List = pResult->GetInteger(0);
	const char *pParamName = pResult->GetString(1);
	float NewValue = pResult->GetFloat(2);

	if(List >= 0 && List < NUM_TUNEZONES)
	{
		if(pSelf->TuningList()[List].Set(pParamName, NewValue))
		{
			char aBuf[256];
			str_format(aBuf, sizeof(aBuf), "%s in zone %d changed to %.2f", pParamName, List, NewValue);
			pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "tuning", aBuf);
			pSelf->SendTuningParams(-1, List);
		}
		else
			pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "tuning", "No such tuning parameter");
	}
}

void CGameContext::ConTuneDumpZone(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	int List = pResult->GetInteger(0);
	char aBuf[256];
	if(List >= 0 && List < NUM_TUNEZONES)
	{
		for(int i = 0; i < pSelf->TuningList()[List].Num(); i++)
		{
			float v;
			pSelf->TuningList()[List].Get(i, &v);
			str_format(aBuf, sizeof(aBuf), "zone %d: %s %.2f", List, pSelf->TuningList()[List].ms_apNames[i], v);
			pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "tuning", aBuf);
		}
	}
}

void CGameContext::ConTuneResetZone(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	CTuningParams TuningParams;
	if(pResult->NumArguments())
	{
		int List = pResult->GetInteger(0);
		if(List >= 0 && List < NUM_TUNEZONES)
		{
			pSelf->TuningList()[List] = TuningParams;
			char aBuf[256];
			str_format(aBuf, sizeof(aBuf), "Tunezone %d reset", List);
			pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "tuning", aBuf);
			pSelf->SendTuningParams(-1, List);
		}
	}
	else
	{
		for(int i = 0; i < NUM_TUNEZONES; i++)
		{
			*(pSelf->TuningList() + i) = TuningParams;
			pSelf->SendTuningParams(-1, i);
		}
		pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "tuning", "All Tunezones reset");
	}
}

void CGameContext::ConTuneSetZoneMsgEnter(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	if(pResult->NumArguments())
	{
		int List = pResult->GetInteger(0);
		if(List >= 0 && List < NUM_TUNEZONES)
		{
			str_copy(pSelf->m_aaZoneEnterMsg[List], pResult->GetString(1), sizeof(pSelf->m_aaZoneEnterMsg[List]));
		}
	}
}

void CGameContext::ConTuneSetZoneMsgLeave(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	if(pResult->NumArguments())
	{
		int List = pResult->GetInteger(0);
		if(List >= 0 && List < NUM_TUNEZONES)
		{
			str_copy(pSelf->m_aaZoneLeaveMsg[List], pResult->GetString(1), sizeof(pSelf->m_aaZoneLeaveMsg[List]));
		}
	}
}

void CGameContext::ConSwitchOpen(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	int Switch = pResult->GetInteger(0);

	if(pSelf->Collision()->m_NumSwitchers > 0 && Switch >= 0 && Switch < pSelf->Collision()->m_NumSwitchers + 1)
	{
		pSelf->Collision()->m_pSwitchers[Switch].m_Initial = false;
		char aBuf[256];
		str_format(aBuf, sizeof(aBuf), "switch %d opened by default", Switch);
		pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
	}
}

void CGameContext::ConPause(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;

	pSelf->m_World.m_Paused ^= 1;
}

void CGameContext::ConChangeMap(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	pSelf->m_pController->ChangeMap(pResult->NumArguments() ? pResult->GetString(0) : "");
}

void CGameContext::ConRestart(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	if(pResult->NumArguments())
		pSelf->m_pController->DoWarmup(pResult->GetInteger(0));
	else
		pSelf->m_pController->StartRound();
}

void CGameContext::ConBroadcast(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;

	char aBuf[1024];
	str_copy(aBuf, pResult->GetString(0), sizeof(aBuf));

	int i, j;
	for(i = 0, j = 0; aBuf[i]; i++, j++)
	{
		if(aBuf[i] == '\\' && aBuf[i + 1] == 'n')
		{
			aBuf[j] = '\n';
			i++;
		}
		else if(i != j)
		{
			aBuf[j] = aBuf[i];
		}
	}
	aBuf[j] = '\0';

	pSelf->SendBroadcast(aBuf, -1);
}

void CGameContext::ConSay(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	pSelf->SendChat(-1, CHAT_ALL, pResult->GetString(0));
}

void CGameContext::ConAddVote(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	const char *pDescription = pResult->GetString(0);
	const char *pCommand = pResult->GetString(1);

	pSelf->VoteManager()->AddVote(pDescription, pCommand);
}

void CGameContext::ConRemoveVote(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	const char *pDescription = pResult->GetString(0);

	pSelf->VoteManager()->RemoveVote(pDescription);
}

void CGameContext::ConForceVote(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	const char *pType = pResult->GetString(0);
	const char *pValue = pResult->GetString(1);
	const char *pReason = pResult->NumArguments() > 2 && pResult->GetString(2)[0] ? pResult->GetString(2) : "No reason";
	char aBuf[128] = {0};

	pSelf->VoteManager()->ForceVote(pResult->m_ClientID, pType, pValue, pReason);
}

void CGameContext::ConClearVotes(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;

	CNetMsg_Sv_VoteClearOptions VoteClearOptionsMsg;
	pSelf->Server()->SendPackMsg(&VoteClearOptionsMsg, MSGFLAG_VITAL, -1);

	pSelf->VoteManager()->ClearVotes();

	// reset sending of vote options
	for(auto &pPlayer : pSelf->m_apPlayers)
	{
		if(pPlayer)
			pPlayer->m_SendVoteIndex = 0;
	}
}

struct CMapNameItem
{
	char m_aName[MAX_PATH_LENGTH - 4];

	bool operator<(const CMapNameItem &Other) const { return str_comp_nocase(m_aName, Other.m_aName) < 0; }
};

void CGameContext::ConAddMapVotes(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;

	sorted_array<CMapNameItem> MapList;
	pSelf->Storage()->ListDirectory(IStorage::TYPE_ALL, "maps", MapScan, &MapList);

	for(int i = 0; i < MapList.size(); i++)
	{
		char aDescription[64];
		str_format(aDescription, sizeof(aDescription), "Map: %s", MapList[i].m_aName);

		char aCommand[MAX_PATH_LENGTH * 2 + 10];
		char aMapEscaped[MAX_PATH_LENGTH * 2];
		char *pDst = aMapEscaped;
		str_escape(&pDst, MapList[i].m_aName, aMapEscaped + sizeof(aMapEscaped));
		str_format(aCommand, sizeof(aCommand), "change_map \"%s\"", aMapEscaped);

		pSelf->VoteManager()->AddVote(aDescription, aCommand);
	}

	pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", "added maps to votes");
}

int CGameContext::MapScan(const char *pName, int IsDir, int DirType, void *pUserData)
{
	sorted_array<CMapNameItem> *pMapList = (sorted_array<CMapNameItem> *)pUserData;

	if(IsDir || !str_endswith(pName, ".map"))
		return 0;

	CMapNameItem Item;
	int Length = str_length(pName);
	str_truncate(Item.m_aName, sizeof(Item.m_aName), pName, Length - 4);
	pMapList->add(Item);

	return 0;
}

void CGameContext::ConVote(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;

	if(str_comp_nocase(pResult->GetString(0), "yes") == 0)
		pSelf->VoteManager()->ForceCurrentVote(pResult->m_ClientID, true);
	else if(str_comp_nocase(pResult->GetString(0), "no") == 0)
		pSelf->VoteManager()->ForceCurrentVote(pResult->m_ClientID, false);
}

void CGameContext::ConchainSpecialMotdupdate(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	pfnCallback(pResult, pCallbackUserData);
	if(pResult->NumArguments())
	{
		CGameContext *pSelf = (CGameContext *)pUserData;
		pSelf->SendMotd(-1);
	}
}

void CGameContext::OnConsoleInit()
{
	m_pServer = Kernel()->RequestInterface<IServer>();
	m_pConfig = Kernel()->RequestInterface<IConfigManager>()->Values();
	m_pConsole = Kernel()->RequestInterface<IConsole>();
	m_pEngine = Kernel()->RequestInterface<IEngine>();
	m_pStorage = Kernel()->RequestInterface<IStorage>();

	m_ChatPrintCBIndex = Console()->RegisterPrintCallback(0, SendChatResponse, this);

	Console()->Register("tune", "s[tuning] i[value]", CFGFLAG_SERVER | CFGFLAG_GAME, ConTuneParam, this, "Tune variable to value");
	Console()->Register("toggle_tune", "s[tuning] i[value 1] i[value 2]", CFGFLAG_SERVER | CFGFLAG_GAME, ConToggleTuneParam, this, "Toggle tune variable");
	Console()->Register("tune_reset", "", CFGFLAG_SERVER, ConTuneReset, this, "Reset tuning");
	Console()->Register("tune_dump", "", CFGFLAG_SERVER, ConTuneDump, this, "Dump tuning");
	Console()->Register("tune_zone", "i[zone] s[tuning] i[value]", CFGFLAG_SERVER | CFGFLAG_GAME, ConTuneZone, this, "Tune in zone a variable to value");
	Console()->Register("tune_zone_dump", "i[zone]", CFGFLAG_SERVER, ConTuneDumpZone, this, "Dump zone tuning in zone x");
	Console()->Register("tune_zone_reset", "?i[zone]", CFGFLAG_SERVER, ConTuneResetZone, this, "reset zone tuning in zone x or in all zones");
	Console()->Register("tune_zone_enter", "i[zone] r[message]", CFGFLAG_SERVER | CFGFLAG_GAME, ConTuneSetZoneMsgEnter, this, "which message to display on zone enter; use 0 for normal area");
	Console()->Register("tune_zone_leave", "i[zone] r[message]", CFGFLAG_SERVER | CFGFLAG_GAME, ConTuneSetZoneMsgLeave, this, "which message to display on zone leave; use 0 for normal area");
	Console()->Register("switch_open", "i[switch]", CFGFLAG_SERVER | CFGFLAG_GAME, ConSwitchOpen, this, "Whether a switch is deactivated by default (otherwise activated)");
	Console()->Register("pause_game", "", CFGFLAG_SERVER, ConPause, this, "Pause/unpause game");
	Console()->Register("change_map", "?r[map]", CFGFLAG_SERVER | CFGFLAG_STORE, ConChangeMap, this, "Change map");
	Console()->Register("restart", "?i[seconds]", CFGFLAG_SERVER | CFGFLAG_STORE, ConRestart, this, "Restart in x seconds (0 = abort)");
	Console()->Register("broadcast", "r[message]", CFGFLAG_SERVER, ConBroadcast, this, "Broadcast message");
	Console()->Register("say", "r[message]", CFGFLAG_SERVER, ConSay, this, "Say in chat");

	Console()->Register("add_vote", "s[name] r[command]", CFGFLAG_SERVER, ConAddVote, this, "Add a voting option");
	Console()->Register("remove_vote", "r[name]", CFGFLAG_SERVER, ConRemoveVote, this, "Remove a voting option");
	Console()->Register("force_vote", "s[name] s[command] ?r[reason]", CFGFLAG_SERVER, ConForceVote, this, "Force a voting option");
	Console()->Register("clear_votes", "", CFGFLAG_SERVER, ConClearVotes, this, "Clears the voting options");
	Console()->Register("add_map_votes", "", CFGFLAG_SERVER, ConAddMapVotes, this, "Automatically adds voting options for all maps");
	Console()->Register("vote", "r['yes'|'no']", CFGFLAG_SERVER, ConVote, this, "Force a vote to yes/no");
	Console()->Register("dump_antibot", "", CFGFLAG_SERVER, ConDumpAntibot, this, "Dumps the antibot status");

	Console()->Chain("sv_motd", ConchainSpecialMotdupdate, this);

#define CONSOLE_COMMAND(name, params, flags, callback, userdata, help) m_pConsole->Register(name, params, flags, callback, userdata, help);
#include <game/consolecommands.h>
#define CHAT_COMMAND(name, params, flags, callback, userdata, help) m_pConsole->Register(name, params, flags, callback, userdata, help);
#include <game/chatcommands.h>
}

void CGameContext::OnInit(/*class IKernel *pKernel*/)
{
	m_pServer = Kernel()->RequestInterface<IServer>();
	m_pConfig = Kernel()->RequestInterface<IConfigManager>()->Values();
	m_pConsole = Kernel()->RequestInterface<IConsole>();
	m_pEngine = Kernel()->RequestInterface<IEngine>();
	m_pStorage = Kernel()->RequestInterface<IStorage>();
	m_pAntibot = Kernel()->RequestInterface<IAntibot>();
	m_pAntibot->RoundStart(this);
	m_World.SetGameServer(this);
	m_Events.SetGameServer(this);

	m_pVoteManager = new CVoteManager(this);

	m_GameUuid = RandomUuid();
	Console()->SetTeeHistorianCommandCallback(CommandCallback, this);

	uint64 aSeed[2];
	secure_random_fill(aSeed, sizeof(aSeed));
	m_Prng.Seed(aSeed);
	m_World.m_Core.m_pPrng = &m_Prng;

	DeleteTempfile();

	for(int i = 0; i < NUM_NETOBJTYPES; i++)
		Server()->SnapSetStaticsize(i, m_NetObjHandler.GetObjSize(i));

	m_Layers.Init(Kernel());
	m_Collision.Init(&m_Layers);

	char aMapName[128];
	int MapSize;
	SHA256_DIGEST MapSha256;
	int MapCrc;
	Server()->GetMapInfo(aMapName, sizeof(aMapName), &MapSize, &MapSha256, &MapCrc);

	// reset everything here
	//world = new GAMEWORLD;
	//players = new CPlayer[MAX_CLIENTS];

	// Reset Tunezones
	CTuningParams TuningParams;
	for(int i = 0; i < NUM_TUNEZONES; i++)
	{
		TuningList()[i] = TuningParams;
		TuningList()[i].Set("gun_curvature", 0);
		TuningList()[i].Set("gun_speed", 1400);
		TuningList()[i].Set("shotgun_curvature", 0);
		TuningList()[i].Set("shotgun_speed", 500);
		TuningList()[i].Set("shotgun_speeddiff", 0);
	}

	for(int i = 0; i < NUM_TUNEZONES; i++)
	{
		// Send no text by default when changing tune zones.
		m_aaZoneEnterMsg[i][0] = 0;
		m_aaZoneLeaveMsg[i][0] = 0;
	}
	// Reset Tuning
	if(g_Config.m_SvTuneReset)
	{
		ResetTuning();
	}
	else
	{
		Tuning()->Set("gun_speed", 1400);
		Tuning()->Set("gun_curvature", 0);
		Tuning()->Set("shotgun_speed", 500);
		Tuning()->Set("shotgun_speeddiff", 0);
		Tuning()->Set("shotgun_curvature", 0);
	}

	Console()->ExecuteFile(g_Config.m_SvResetFile, -1);

	LoadMapSettings();

	m_pController = new CGCTXP(this);

	m_TeeHistorianActive = g_Config.m_SvTeeHistorian;
	if(m_TeeHistorianActive)
	{
		char aGameUuid[UUID_MAXSTRSIZE];
		FormatUuid(m_GameUuid, aGameUuid, sizeof(aGameUuid));

		char aFilename[64];
		str_format(aFilename, sizeof(aFilename), "teehistorian/%s.teehistorian", aGameUuid);

		IOHANDLE File = Storage()->OpenFile(aFilename, IOFLAG_WRITE, IStorage::TYPE_SAVE);
		if(!File)
		{
			dbg_msg("teehistorian", "failed to open '%s'", aFilename);
			Server()->SetErrorShutdown("teehistorian open error");
			return;
		}
		else
		{
			dbg_msg("teehistorian", "recording to '%s'", aFilename);
		}
		m_pTeeHistorianFile = aio_new(File);

		char aVersion[128];
		if(GIT_SHORTREV_HASH)
		{
			str_format(aVersion, sizeof(aVersion), "%s (%s)", GAME_VERSION, GIT_SHORTREV_HASH);
		}
		else
		{
			str_format(aVersion, sizeof(aVersion), "%s", GAME_VERSION);
		}
		CTeeHistorian::CGameInfo GameInfo;
		GameInfo.m_GameUuid = m_GameUuid;
		GameInfo.m_pServerVersion = aVersion;
		GameInfo.m_StartTime = time(0);
		GameInfo.m_pPrngDescription = m_Prng.Description();

		GameInfo.m_pServerName = g_Config.m_SvName;
		GameInfo.m_ServerPort = Server()->Port();
		GameInfo.m_pGameType = m_pController->m_pGameType;

		GameInfo.m_pConfig = &g_Config;
		GameInfo.m_pTuning = Tuning();
		GameInfo.m_pUuids = &g_UuidManager;

		GameInfo.m_pMapName = aMapName;
		GameInfo.m_MapSize = MapSize;
		GameInfo.m_MapSha256 = MapSha256;
		GameInfo.m_MapCrc = MapCrc;

		m_TeeHistorian.Reset(&GameInfo, TeeHistorianWrite, this);

		for(int i = 0; i < MAX_CLIENTS; i++)
		{
			int Level = Server()->GetAuthedState(i);
			if(Level)
			{
				m_TeeHistorian.RecordAuthInitial(i, Level, Server()->GetAuthName(i));
			}
		}
	}
	// setup core world
	//for(int i = 0; i < MAX_CLIENTS; i++)
	//	game.players[i].core.world = &game.world.core;

	// create all entities from the game layer
	CMapItemLayerTilemap *pTileMap = m_Layers.GameLayer();
	CTile *pTiles = (CTile *)Kernel()->RequestInterface<IMap>()->GetData(pTileMap->m_Data);

	CTile *pFront = 0;
	CSwitchTile *pSwitch = 0;
	if(m_Layers.FrontLayer())
		pFront = (CTile *)Kernel()->RequestInterface<IMap>()->GetData(m_Layers.FrontLayer()->m_Front);
	if(m_Layers.SwitchLayer())
		pSwitch = (CSwitchTile *)Kernel()->RequestInterface<IMap>()->GetData(m_Layers.SwitchLayer()->m_Switch);

	for(int y = 0; y < pTileMap->m_Height; y++)
	{
		for(int x = 0; x < pTileMap->m_Width; x++)
		{
			int Index = pTiles[y * pTileMap->m_Width + x].m_Index;

			if(Index == TILE_NPC)
			{
				m_Tuning.Set("player_collision", 0);
				dbg_msg("game_layer", "found no collision tile");
			}
			else if(Index == TILE_NPH)
			{
				m_Tuning.Set("player_hooking", 0);
				dbg_msg("game_layer", "found no player hooking tile");
			}

			if(Index >= ENTITY_OFFSET)
			{
				vec2 Pos(x * 32.0f + 16.0f, y * 32.0f + 16.0f);
				//m_pController->OnEntity(Index-ENTITY_OFFSET, Pos);
				m_pController->OnEntity(Index - ENTITY_OFFSET, Pos, LAYER_GAME, pTiles[y * pTileMap->m_Width + x].m_Flags);
			}

			if(pFront)
			{
				Index = pFront[y * pTileMap->m_Width + x].m_Index;

				if(Index == TILE_NPC)
				{
					m_Tuning.Set("player_collision", 0);
					dbg_msg("front_layer", "found no collision tile");
				}
				else if(Index == TILE_NPH)
				{
					m_Tuning.Set("player_hooking", 0);
					dbg_msg("front_layer", "found no player hooking tile");
				}
				if(Index >= ENTITY_OFFSET)
				{
					vec2 Pos(x * 32.0f + 16.0f, y * 32.0f + 16.0f);
					m_pController->OnEntity(Index - ENTITY_OFFSET, Pos, LAYER_FRONT, pFront[y * pTileMap->m_Width + x].m_Flags);
				}
			}
			if(pSwitch)
			{
				Index = pSwitch[y * pTileMap->m_Width + x].m_Type;
				// TODO: Add off by default door here
				// if (Index == TILE_DOOR_OFF)
				if(Index >= ENTITY_OFFSET)
				{
					vec2 Pos(x * 32.0f + 16.0f, y * 32.0f + 16.0f);
					m_pController->OnEntity(Index - ENTITY_OFFSET, Pos, LAYER_SWITCH, pSwitch[y * pTileMap->m_Width + x].m_Flags, pSwitch[y * pTileMap->m_Width + x].m_Number);
				}
			}
		}
	}

	//game.world.insert_entity(game.Controller);

#ifdef CONF_DEBUG
	if(g_Config.m_DbgDummies)
	{
		for(int i = 0; i < g_Config.m_DbgDummies; i++)
		{
			OnClientConnected(MAX_CLIENTS - i - 1, 0);
		}
	}
#endif
}

void CGameContext::DeleteTempfile()
{
	if(m_aDeleteTempfile[0] != 0)
	{
		Storage()->RemoveFile(m_aDeleteTempfile, IStorage::TYPE_SAVE);
		m_aDeleteTempfile[0] = 0;
	}
}

void CGameContext::OnMapChange(char *pNewMapName, int MapNameSize)
{
	LoadMapSettings(pNewMapName, MapNameSize);
}

void CGameContext::LoadMapSettings(char *pNewMapName, int MapNameSize)
{
	char aConfig[128];
	char aTemp[128];
	str_format(aConfig, sizeof(aConfig), "maps/%s/.cfg", g_Config.m_SvMap);
	str_format(aTemp, sizeof(aTemp), "%s.%d.tmp", pNewMapName, pid());

	IOHANDLE File = Storage()->OpenFile(aConfig, IOFLAG_READ, IStorage::TYPE_ALL);
	if(!File)
	{
		// No map-specific config, just return.
		return;
	}
	CLineReader LineReader;
	LineReader.Init(File);

	array<char *> aLines;
	char *pLine;
	int TotalLength = 0;
	while((pLine = LineReader.Get()))
	{
		int Length = str_length(pLine) + 1;
		char *pCopy = (char *)malloc(Length);
		mem_copy(pCopy, pLine, Length);
		aLines.add(pCopy);
		TotalLength += Length;
	}
	io_close(File);

	char *pSettings = (char *)malloc(maximum(1, TotalLength));
	int Offset = 0;
	for(int i = 0; i < aLines.size(); i++)
	{
		int Length = str_length(aLines[i]) + 1;
		mem_copy(pSettings + Offset, aLines[i], Length);
		Offset += Length;
		free(aLines[i]);
	}

	CDataFileReader Reader;
	Reader.Open(Storage(), pNewMapName, IStorage::TYPE_ALL);

	CDataFileWriter Writer;
	Writer.Init();

	int SettingsIndex = Reader.NumData();
	bool FoundInfo = false;
	for(int i = 0; i < Reader.NumItems(); i++)
	{
		int TypeID;
		int ItemID;
		int *pData = (int *)Reader.GetItem(i, &TypeID, &ItemID);
		int Size = Reader.GetItemSize(i);
		CMapItemInfoSettings MapInfo;
		if(TypeID == MAPITEMTYPE_INFO && ItemID == 0)
		{
			FoundInfo = true;
			CMapItemInfoSettings *pInfo = (CMapItemInfoSettings *)pData;
			if(Size >= (int)sizeof(CMapItemInfoSettings))
			{
				if(pInfo->m_Settings > -1)
				{
					SettingsIndex = pInfo->m_Settings;
					char *pMapSettings = (char *)Reader.GetData(SettingsIndex);
					int DataSize = Reader.GetDataSize(SettingsIndex);
					if(DataSize == TotalLength && mem_comp(pSettings, pMapSettings, DataSize) == 0)
					{
						// Configs coincide, no need to update map.
						free(pSettings);
						return;
					}
					Reader.UnloadData(pInfo->m_Settings);
				}
				else
				{
					MapInfo = *pInfo;
					MapInfo.m_Settings = SettingsIndex;
					pData = (int *)&MapInfo;
					Size = sizeof(MapInfo);
				}
			}
			else
			{
				*(CMapItemInfo *)&MapInfo = *(CMapItemInfo *)pInfo;
				MapInfo.m_Settings = SettingsIndex;
				pData = (int *)&MapInfo;
				Size = sizeof(MapInfo);
			}
		}
		Writer.AddItem(TypeID, ItemID, Size, pData);
	}

	if(!FoundInfo)
	{
		CMapItemInfoSettings Info;
		Info.m_Version = 1;
		Info.m_Author = -1;
		Info.m_MapVersion = -1;
		Info.m_Credits = -1;
		Info.m_License = -1;
		Info.m_Settings = SettingsIndex;
		Writer.AddItem(MAPITEMTYPE_INFO, 0, sizeof(Info), &Info);
	}

	for(int i = 0; i < Reader.NumData() || i == SettingsIndex; i++)
	{
		if(i == SettingsIndex)
		{
			Writer.AddData(TotalLength, pSettings);
			continue;
		}
		unsigned char *pData = (unsigned char *)Reader.GetData(i);
		int Size = Reader.GetDataSize(i);
		Writer.AddData(Size, pData);
		Reader.UnloadData(i);
	}

	dbg_msg("mapchange", "imported settings");
	free(pSettings);
	Reader.Close();
	Writer.OpenFile(Storage(), aTemp);
	Writer.Finish();

	str_copy(pNewMapName, aTemp, MapNameSize);
	str_copy(m_aDeleteTempfile, aTemp, sizeof(m_aDeleteTempfile));
}

void CGameContext::OnShutdown()
{
	Antibot()->RoundEnd();

	if(m_TeeHistorianActive)
	{
		m_TeeHistorian.Finish();
		aio_close(m_pTeeHistorianFile);
		aio_wait(m_pTeeHistorianFile);
		int Error = aio_error(m_pTeeHistorianFile);
		if(Error)
		{
			dbg_msg("teehistorian", "error closing file, err=%d", Error);
			Server()->SetErrorShutdown("teehistorian close error");
		}
		aio_free(m_pTeeHistorianFile);
	}

	DeleteTempfile();
	Console()->ResetServerGameSettings();
	Collision()->Dest();
	delete m_pController;
	m_pController = 0;
	Clear();
}

void CGameContext::LoadMapSettings()
{
	IMap *pMap = Kernel()->RequestInterface<IMap>();
	int Start, Num;
	pMap->GetType(MAPITEMTYPE_INFO, &Start, &Num);
	for(int i = Start; i < Start + Num; i++)
	{
		int ItemID;
		CMapItemInfoSettings *pItem = (CMapItemInfoSettings *)pMap->GetItem(i, 0, &ItemID);
		int ItemSize = pMap->GetItemSize(i);
		if(!pItem || ItemID != 0)
			continue;

		if(ItemSize < (int)sizeof(CMapItemInfoSettings))
			break;
		if(!(pItem->m_Settings > -1))
			break;

		int Size = pMap->GetDataSize(pItem->m_Settings);
		char *pSettings = (char *)pMap->GetData(pItem->m_Settings);
		char *pNext = pSettings;
		while(pNext < pSettings + Size)
		{
			int StrSize = str_length(pNext) + 1;
			Console()->ExecuteLine(pNext, IConsole::CLIENT_ID_GAME);
			pNext += StrSize;
		}
		pMap->UnloadData(pItem->m_Settings);
		break;
	}

	char aBuf[128];
	str_format(aBuf, sizeof(aBuf), "maps/%s/.map.cfg", g_Config.m_SvMap);
	Console()->ExecuteFile(aBuf, IConsole::CLIENT_ID_NO_GAME);
}

void CGameContext::OnSnap(int ClientID)
{
	// add tuning to demo
	CTuningParams StandardTuning;
	if(ClientID == -1 && Server()->DemoRecorder_IsRecording() && mem_comp(&StandardTuning, &m_Tuning, sizeof(CTuningParams)))
	{
		CMsgPacker Msg(NETMSGTYPE_SV_TUNEPARAMS);
		int *pParams = (int *)&m_Tuning;
		for(unsigned i = 0; i < sizeof(m_Tuning) / sizeof(int); i++)
			Msg.AddInt(pParams[i]);
		Server()->SendMsg(&Msg, MSGFLAG_RECORD | MSGFLAG_NOSEND, ClientID);
	}

	m_World.Snap(ClientID);
	m_pController->Snap(ClientID);
	m_Events.Snap(ClientID);

	for(auto &pPlayer : m_apPlayers)
	{
		if(pPlayer)
			pPlayer->Snap(ClientID);
	}

	if(ClientID > -1)
		m_apPlayers[ClientID]->FakeSnap();
}
void CGameContext::OnPreSnap() {}
void CGameContext::OnPostSnap()
{
	m_Events.Clear();
}

bool CGameContext::IsClientReady(int ClientID) const
{
	return m_apPlayers[ClientID] && m_apPlayers[ClientID]->m_IsReady ? true : false;
}

bool CGameContext::IsClientPlayer(int ClientID) const
{
	return m_apPlayers[ClientID] && m_apPlayers[ClientID]->GetTeam() != TEAM_SPECTATORS;
}

CUuid CGameContext::GameUuid() const { return m_GameUuid; }
const char *CGameContext::GameType() const { return m_pController && m_pController->m_pGameType ? m_pController->m_pGameType : ""; }
const char *CGameContext::Version() const { return GAME_VERSION; }
const char *CGameContext::NetVersion() const { return GAME_NETVERSION; }

IGameServer *CreateGameServer() { return new CGameContext; }

void CGameContext::SendChatResponseAll(const char *pLine, void *pUser)
{
	CGameContext *pSelf = (CGameContext *)pUser;

	static volatile int ReentryGuard = 0;
	const char *pLineOrig = pLine;

	if(ReentryGuard)
		return;
	ReentryGuard++;

	if(*pLine == '[')
		do
			pLine++;
		while((pLine - 2 < pLineOrig || *(pLine - 2) != ':') && *pLine != 0); // remove the category (e.g. [Console]: No Such Command)

	pSelf->SendChat(-1, CHAT_ALL, pLine);

	ReentryGuard--;
}

void CGameContext::SendChatResponse(const char *pLine, void *pUser, ColorRGBA PrintColor)
{
	CGameContext *pSelf = (CGameContext *)pUser;
	int ClientID = pSelf->m_ChatResponseTargetID;

	if(ClientID < 0 || ClientID >= MAX_CLIENTS)
		return;

	const char *pLineOrig = pLine;

	static volatile int ReentryGuard = 0;

	if(ReentryGuard)
		return;
	ReentryGuard++;

	if(pLine[0] == '[')
	{
		// Remove time and category: [20:39:00][Console]
		pLine = str_find(pLine, "]: ");
		if(pLine)
			pLine += 3;
		else
			pLine = pLineOrig;
	}

	pSelf->SendChatTarget(ClientID, pLine);

	ReentryGuard--;
}

bool CGameContext::PlayerCollision()
{
	float Temp;
	m_Tuning.Get("player_collision", &Temp);
	return Temp != 0.0f;
}

bool CGameContext::PlayerHooking()
{
	float Temp;
	m_Tuning.Get("player_hooking", &Temp);
	return Temp != 0.0f;
}

float CGameContext::PlayerJetpack()
{
	float Temp;
	m_Tuning.Get("player_jetpack", &Temp);
	return Temp;
}

void CGameContext::OnSetAuthed(int ClientID, int Level)
{
	if(m_apPlayers[ClientID])
	{
		// TODO: redesign it

		//char aBuf[512], aIP[NETADDR_MAXSTRSIZE];
		//Server()->GetClientAddr(ClientID, aIP, sizeof(aIP));
		//str_format(aBuf, sizeof(aBuf), "ban %s %d Banned by vote", aIP, g_Config.m_SvVoteKickBantime);
		//if(!str_comp_nocase(m_aVoteCommand, aBuf) && Level > Server()->GetAuthedState(m_VoteCreator))
		//{
		//	m_VoteEnforce = VOTE_ENFORCE_NO_ADMIN;
		//	Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "CGameContext", "Vote aborted by authorized login.");
		//}
	}
	if(m_TeeHistorianActive)
	{
		if(Level)
		{
			m_TeeHistorian.RecordAuthLogin(ClientID, Level, Server()->GetAuthName(ClientID));
		}
		else
		{
			m_TeeHistorian.RecordAuthLogout(ClientID);
		}
	}
}

int CGameContext::ProcessSpamProtection(int ClientID)
{
	if(!m_apPlayers[ClientID])
		return 0;
	if(g_Config.m_SvSpamprotection && m_apPlayers[ClientID]->m_LastChat && m_apPlayers[ClientID]->m_LastChat + Server()->TickSpeed() * g_Config.m_SvChatDelay > Server()->Tick())
		return 1;
	else if(g_Config.m_SvDnsblChat && Server()->DnsblBlack(ClientID))
	{
		SendChatTarget(ClientID, "Players are not allowed to chat from VPNs at this time");
		return 1;
	}
	else
		m_apPlayers[ClientID]->m_LastChat = Server()->Tick();

	NETADDR Addr;
	Server()->GetClientAddr(ClientID, &Addr);

	int Muted = 0;

	if(Muted <= 0)
	{
		for(int i = 0; i < m_NumMutes && Muted <= 0; i++)
		{
			if(!net_addr_comp_noport(&Addr, &m_aMutes[i].m_Addr))
				Muted = (m_aMutes[i].m_Expire - Server()->Tick()) / Server()->TickSpeed();
		}
	}

	if(Muted > 0)
	{
		char aBuf[128];
		str_format(aBuf, sizeof aBuf, "You are not permitted to talk for the next %d seconds.", Muted);
		SendChatTarget(ClientID, aBuf);
		return 1;
	}

	if((m_apPlayers[ClientID]->m_ChatScore += g_Config.m_SvChatPenalty) > g_Config.m_SvChatThreshold)
	{
		Mute(&Addr, g_Config.m_SvSpamMuteDuration, Server()->ClientName(ClientID));
		m_apPlayers[ClientID]->m_ChatScore = 0;
		return 1;
	}

	return 0;
}

void CGameContext::ResetTuning()
{
	CTuningParams TuningParams;
	m_Tuning = TuningParams;
	Tuning()->Set("gun_speed", 1400);
	Tuning()->Set("gun_curvature", 0);
	Tuning()->Set("shotgun_speed", 500);
	Tuning()->Set("shotgun_speeddiff", 0);
	Tuning()->Set("shotgun_curvature", 0);
	SendTuningParams(-1);
}

bool CheckClientID(int ClientID)
{
	if(ClientID < 0 || ClientID >= MAX_CLIENTS)
		return false;
	return true;
}

void CGameContext::Whisper(int ClientID, char *pStr)
{
	char *pName;
	char *pMessage;
	int Error = 0;

	if(ProcessSpamProtection(ClientID))
		return;

	pStr = str_skip_whitespaces(pStr);

	int Victim;

	// add token
	if(*pStr == '"')
	{
		pStr++;

		pName = pStr;
		char *pDst = pStr; // we might have to process escape data
		while(1)
		{
			if(pStr[0] == '"')
				break;
			else if(pStr[0] == '\\')
			{
				if(pStr[1] == '\\')
					pStr++; // skip due to escape
				else if(pStr[1] == '"')
					pStr++; // skip due to escape
			}
			else if(pStr[0] == 0)
				Error = 1;

			*pDst = *pStr;
			pDst++;
			pStr++;
		}

		// write null termination
		*pDst = 0;

		pStr++;

		for(Victim = 0; Victim < MAX_CLIENTS; Victim++)
			if(str_comp(pName, Server()->ClientName(Victim)) == 0)
				break;
	}
	else
	{
		pName = pStr;
		while(1)
		{
			if(pStr[0] == 0)
			{
				Error = 1;
				break;
			}
			if(pStr[0] == ' ')
			{
				pStr[0] = 0;
				for(Victim = 0; Victim < MAX_CLIENTS; Victim++)
					if(str_comp(pName, Server()->ClientName(Victim)) == 0)
						break;

				pStr[0] = ' ';

				if(Victim < MAX_CLIENTS)
					break;
			}
			pStr++;
		}
	}

	if(pStr[0] != ' ')
	{
		Error = 1;
	}

	*pStr = 0;
	pStr++;

	pMessage = pStr;

	char aBuf[256];

	if(Error)
	{
		str_format(aBuf, sizeof(aBuf), "Invalid whisper");
		SendChatTarget(ClientID, aBuf);
		return;
	}

	if(Victim >= MAX_CLIENTS || !CheckClientID(Victim))
	{
		str_format(aBuf, sizeof(aBuf), "No player with name \"%s\" found", pName);
		SendChatTarget(ClientID, aBuf);
		return;
	}

	WhisperID(ClientID, Victim, pMessage);
}

void CGameContext::WhisperID(int ClientID, int VictimID, const char *pMessage)
{
	if(!CheckClientID(ClientID))
		return;

	if(!CheckClientID(VictimID))
		return;

	if(m_apPlayers[ClientID])
		m_apPlayers[ClientID]->m_LastWhisperTo = VictimID;

	char aBuf[256];

	if(Server()->IsSixup(ClientID))
	{
		protocol7::CNetMsg_Sv_Chat Msg;
		Msg.m_ClientID = ClientID;
		Msg.m_Mode = protocol7::CHAT_WHISPER;
		Msg.m_pMessage = pMessage;
		Msg.m_TargetID = VictimID;

		Server()->SendPackMsg(&Msg, MSGFLAG_VITAL | MSGFLAG_NORECORD, ClientID);
	}
	else if(GetClientVersion(ClientID) >= VERSION_DDNET_WHISPER)
	{
		CNetMsg_Sv_Chat Msg;
		Msg.m_Team = CHAT_WHISPER_SEND;
		Msg.m_ClientID = VictimID;
		Msg.m_pMessage = pMessage;
		if(g_Config.m_SvDemoChat)
			Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, ClientID);
		else
			Server()->SendPackMsg(&Msg, MSGFLAG_VITAL | MSGFLAG_NORECORD, ClientID);
	}
	else
	{
		str_format(aBuf, sizeof(aBuf), "[→ %s] %s", Server()->ClientName(VictimID), pMessage);
		SendChatTarget(ClientID, aBuf);
	}

	if(Server()->IsSixup(VictimID))
	{
		protocol7::CNetMsg_Sv_Chat Msg;
		Msg.m_ClientID = ClientID;
		Msg.m_Mode = protocol7::CHAT_WHISPER;
		Msg.m_pMessage = pMessage;
		Msg.m_TargetID = VictimID;

		Server()->SendPackMsg(&Msg, MSGFLAG_VITAL | MSGFLAG_NORECORD, VictimID);
	}
	else if(GetClientVersion(VictimID) >= VERSION_DDNET_WHISPER)
	{
		CNetMsg_Sv_Chat Msg2;
		Msg2.m_Team = CHAT_WHISPER_RECV;
		Msg2.m_ClientID = ClientID;
		Msg2.m_pMessage = pMessage;
		if(g_Config.m_SvDemoChat)
			Server()->SendPackMsg(&Msg2, MSGFLAG_VITAL, VictimID);
		else
			Server()->SendPackMsg(&Msg2, MSGFLAG_VITAL | MSGFLAG_NORECORD, VictimID);
	}
	else
	{
		str_format(aBuf, sizeof(aBuf), "[← %s] %s", Server()->ClientName(ClientID), pMessage);
		SendChatTarget(VictimID, aBuf);
	}
}

void CGameContext::Converse(int ClientID, char *pStr)
{
	CPlayer *pPlayer = m_apPlayers[ClientID];
	if(!pPlayer)
		return;

	if(ProcessSpamProtection(ClientID))
		return;

	if(pPlayer->m_LastWhisperTo < 0)
		SendChatTarget(ClientID, "You do not have an ongoing conversation. Whisper to someone to start one");
	else
	{
		WhisperID(ClientID, pPlayer->m_LastWhisperTo, pStr);
	}
}

bool CGameContext::IsVersionBanned(int Version)
{
	char aVersion[16];
	str_format(aVersion, sizeof(aVersion), "%d", Version);

	return str_in_list(g_Config.m_SvBannedVersions, ",", aVersion);
}

void CGameContext::List(int ClientID, const char *pFilter)
{
	int Total = 0;
	char aBuf[256];
	int Bufcnt = 0;
	if(pFilter[0])
		str_format(aBuf, sizeof(aBuf), "Listing players with \"%s\" in name:", pFilter);
	else
		str_format(aBuf, sizeof(aBuf), "Listing all players:");
	SendChatTarget(ClientID, aBuf);
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(m_apPlayers[i])
		{
			Total++;
			const char *pName = Server()->ClientName(i);
			if(str_find_nocase(pName, pFilter) == NULL)
				continue;
			if(Bufcnt + str_length(pName) + 4 > 256)
			{
				SendChatTarget(ClientID, aBuf);
				Bufcnt = 0;
			}
			if(Bufcnt != 0)
			{
				str_format(&aBuf[Bufcnt], sizeof(aBuf) - Bufcnt, ", %s", pName);
				Bufcnt += 2 + str_length(pName);
			}
			else
			{
				str_format(&aBuf[Bufcnt], sizeof(aBuf) - Bufcnt, "%s", pName);
				Bufcnt += str_length(pName);
			}
		}
	}
	if(Bufcnt != 0)
		SendChatTarget(ClientID, aBuf);
	str_format(aBuf, sizeof(aBuf), "%d players online", Total);
	SendChatTarget(ClientID, aBuf);
}

int CGameContext::GetClientVersion(int ClientID) const
{
	IServer::CClientInfo Info = {0};
	Server()->GetClientInfo(ClientID, &Info);
	return Info.m_DDNetVersion;
}

CPlayer *CGameContext::GetPlayer(int ClientID)
{
	if(!CheckClientID(ClientID) && m_apPlayers[ClientID])
		return 0;

	return m_apPlayers[ClientID];
}