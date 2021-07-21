#include <base/math.h>
#include <base/system.h>

#include <engine/server/server.h>
#include <engine/shared/config.h>
#include <engine/shared/memheap.h>

#include "gamecontext.h"
#include "player.h"

#include "voting.h"

void CVoteManager::Reset(bool Clear)
{
	if(Clear)
	{
		ClearVotes();
		delete m_pVoteOptionHeap;

		m_pVoteOptionHeap = new CHeap();
	}
	
	m_VoteType = VOTE_TYPE_UNKNOWN;
	m_VoteCloseTime = 0;
	m_pVoteOptionFirst = 0;
	m_pVoteOptionLast = 0;
	m_NumVoteOptions = 0;
}

void CVoteManager::ClearVotes()
{
	m_pVoteOptionHeap->Reset();
	m_pVoteOptionFirst = 0;
	m_pVoteOptionLast = 0;
	m_NumVoteOptions = 0;
}

CVoteManager::CVoteManager(CGameContext *pGameServer)
{
	m_pVoteOptionHeap = new CHeap();
	m_VoteCloseTime = 0;
	ClearVotes();
	m_pGameServer = pGameServer;
}

CVoteManager::~CVoteManager()
{
	ClearVotes();
	delete m_pVoteOptionHeap;
	m_pVoteOptionHeap = 0;
}

void CVoteManager::CallVote(int ClientID, const char *pDesc, const char *pCmd, const char *pReason, const char *pChatmsg, const char *pSixupDesc)
{
	// check if a vote is already running
	if(m_VoteCloseTime)
		return;

	CPlayer *pPlayer = GameServer()->GetPlayer(ClientID);

	if(!pPlayer)
		return;

	GameServer()->SendChat(-1, CGameContext::CGameContext::CHAT_ALL, pChatmsg, -1, CGameContext::CHAT_SIX);
	if(!pSixupDesc)
		pSixupDesc = pDesc;

	m_VoteCreator = ClientID;
	StartVote(pDesc, pCmd, pReason, pSixupDesc);
	pPlayer->m_Vote = 1;
	pPlayer->m_VotePos = m_VotePos = 1;
	pPlayer->m_LastVoteCall = Server()->Tick();
}

void CVoteManager::StartVote(const char *pDesc, const char *pCommand, const char *pReason, const char *pSixupDesc)
{
	// reset votes
	m_VoteState &= 0;
	m_VoteEnforcer = -1;
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(CPlayer *pPlayer = GameServer()->GetPlayer(i))
		{
			pPlayer->m_Vote = 0;
			pPlayer->m_VotePos = 0;
		}
	}

	// start vote
	m_VoteCloseTime = time_get() + time_freq() * GameServer()->Config()->m_SvVoteTime;
	str_copy(m_aVoteDescription, pDesc, sizeof(m_aVoteDescription));
	str_copy(m_aSixupVoteDescription, pSixupDesc, sizeof(m_aSixupVoteDescription));
	str_copy(m_aVoteCommand, pCommand, sizeof(m_aVoteCommand));
	str_copy(m_aVoteReason, pReason, sizeof(m_aVoteReason));
	SendVoteSet(-1);
	m_VoteUpdate = true;
}

void CVoteManager::EndVote()
{
	m_VoteState = 0;
	m_VoteUpdate = false;
	m_VoteCloseTime = 0;
	SendVoteSet(-1);
}

void CVoteManager::SendVoteSet(int ClientID)
{
	::CNetMsg_Sv_VoteSet Msg6;
	protocol7::CNetMsg_Sv_VoteSet Msg7;

	Msg7.m_ClientID = m_VoteCreator;
	if(m_VoteCloseTime)
	{
		Msg6.m_Timeout = Msg7.m_Timeout = (m_VoteCloseTime - time_get()) / time_freq();
		Msg6.m_pDescription = m_aVoteDescription;
		Msg7.m_pDescription = m_aSixupVoteDescription;
		Msg6.m_pReason = Msg7.m_pReason = m_aVoteReason;

		int &Type = (Msg7.m_Type = protocol7::VOTE_UNKNOWN);
		if(IsKickVote())
			Type = protocol7::VOTE_START_KICK;
		else if(IsSpecVote())
			Type = protocol7::VOTE_START_SPEC;
		else if(IsOptionVote())
			Type = protocol7::VOTE_START_OP;
	}
	else
	{
		Msg6.m_Timeout = Msg7.m_Timeout = 0;
		Msg6.m_pDescription = Msg7.m_pDescription = "";
		Msg6.m_pReason = Msg7.m_pReason = "";

		int &Type = (Msg7.m_Type = protocol7::VOTE_UNKNOWN);
		if(m_VoteState & VOTE_PASSED)
			Type = protocol7::VOTE_END_PASS;
		else
			Type = protocol7::VOTE_END_FAIL;
		if(m_VoteState & VOTE_ABORTED)
			Type = protocol7::VOTE_END_ABORT;

		if(m_VoteState & VOTE_FORCED)
			Msg7.m_ClientID = m_VoteEnforcer;
	}

	if(ClientID == -1)
	{
		for(int i = 0; i < MAX_CLIENTS; i++)
		{
			if(!GameServer()->GetPlayer(i))
				continue;
			if(!Server()->IsSixup(i))
				Server()->SendPackMsg(&Msg6, MSGFLAG_VITAL, i);
			else
				Server()->SendPackMsg(&Msg7, MSGFLAG_VITAL, i);
		}
	}
	else
	{
		if(!Server()->IsSixup(ClientID))
			Server()->SendPackMsg(&Msg6, MSGFLAG_VITAL, ClientID);
		else
			Server()->SendPackMsg(&Msg7, MSGFLAG_VITAL, ClientID);
	}
}

void CVoteManager::SendVoteStatus(int ClientID, int Total, int Yes, int No)
{
	if(ClientID == -1)
	{
		for(int i = 0; i < MAX_CLIENTS; ++i)
			if(Server()->ClientIngame(i))
				SendVoteStatus(i, Total, Yes, No);
		return;
	}

	if(Total > VANILLA_MAX_CLIENTS && GameServer()->GetPlayer(ClientID) && GameServer()->GetPlayer(ClientID)->GetClientVersion() <= VERSION_DDRACE)
	{
		Yes = float(Yes) * VANILLA_MAX_CLIENTS / float(Total);
		No = float(No) * VANILLA_MAX_CLIENTS / float(Total);
		Total = VANILLA_MAX_CLIENTS;
	}

	CNetMsg_Sv_VoteStatus Msg = {0};
	Msg.m_Total = Total;
	Msg.m_Yes = Yes;
	Msg.m_No = No;
	Msg.m_Pass = Total - (Yes + No);

	Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, ClientID);
}

void CVoteManager::AbortVoteKickOnDisconnect(int ClientID)
{
	if(m_VoteCloseTime && ((str_startswith(m_aVoteCommand, "kick ") && str_toint(&m_aVoteCommand[5]) == ClientID) ||
				      (str_startswith(m_aVoteCommand, "set_team ") && str_toint(&m_aVoteCommand[9]) == ClientID)))
		m_VoteState |= VOTE_ABORTED;
}

void CVoteManager::Tick()
{
	if(!m_VoteCloseTime)
		return;
	else if(time_get() > m_VoteCloseTime)
		m_VoteState |= VOTE_FINISHED;

	// update voting
	static int Total, Yes, No;
	// abort the kick-vote on player-leave
	if(m_VoteState & VOTE_ABORTED)
	{
		GameServer()->SendChat(-1, CGameContext::CHAT_ALL, "[VOTE] Status:", -1, CGameContext::CHAT_SIX);
		GameServer()->SendChat(-1, CGameContext::CHAT_ALL, "[VOTE] Aborted");
		EndVote();
	}
	else if(m_VoteState & VOTE_FINISHED)
	{
		GameServer()->SendChat(-1, CGameContext::CHAT_ALL, "[VOTE] Status:", -1, CGameContext::CHAT_SIX);
		if(m_VoteState & VOTE_PASSED)
		{
			GameServer()->SendChat(-1, CGameContext::CHAT_ALL, "[VOTE] Passed", -1, CGameContext::CHAT_SIX);

			Server()->SetRconCID(IServer::RCON_CID_VOTE);
			GameServer()->Console()->ExecuteLine(m_aVoteCommand);
			Server()->SetRconCID(IServer::RCON_CID_SERV);

			if(GameServer()->GetPlayer(m_VoteCreator))
				GameServer()->GetPlayer(m_VoteCreator)->m_LastVoteCall = 0;
		}
		else
		{
			GameServer()->SendChat(-1, CGameContext::CHAT_ALL, "[VOTE] Failed", -1, CGameContext::CHAT_SIX);
		}

		if(m_VoteState & VOTE_FORCED)
		{
			GameServer()->SendChat(-1, CGameContext::CHAT_ALL, "[VOTE] By Authorized player", -1, CGameContext::CHAT_SIX);
		}

		Total = 0, Yes = 0, No = 0;

		EndVote();
	}
	else
	{
		if(m_VoteUpdate)
		{
			// count votes
			char aaBuf[MAX_CLIENTS][NETADDR_MAXSTRSIZE] = {{0}}, *pIP = NULL;
			bool SinglePlayer = true;
			for(int i = 0; i < MAX_CLIENTS; i++)
			{
				if(GameServer()->GetPlayer(i))
				{
					Server()->GetClientAddr(i, aaBuf[i], NETADDR_MAXSTRSIZE);
					if(!pIP)
						pIP = aaBuf[i];
					else if(SinglePlayer && str_comp(pIP, aaBuf[i]))
						SinglePlayer = false;
				}
			}

			// remember checked players, only the first player with a specific ip will be handled
			bool aVoteChecked[MAX_CLIENTS] = {false};
			int64 Now = Server()->Tick();
			for(int i = 0; i < MAX_CLIENTS; i++)
			{
				if(!GameServer()->GetPlayer(i) || aVoteChecked[i])
					continue;

				if((IsKickVote() || IsSpecVote()) && GameServer()->GetPlayer(i)->GetTeam() == TEAM_SPECTATORS)
					continue;

				if(GameServer()->GetPlayer(i)->m_Afk && i != m_VoteCreator)
					continue;

				// connecting clients with spoofed ips can clog slots without being ingame
				if(((CServer *)Server())->m_aClients[i].m_State != CServer::CClient::STATE_INGAME)
					continue;

				// don't count votes by blacklisted clients
				if(GameServer()->Config()->m_SvDnsblVote && !Server()->DnsblWhite(i) && !SinglePlayer)
					continue;

				int ActVote = GameServer()->GetPlayer(i)->m_Vote;
				int ActVotePos = GameServer()->GetPlayer(i)->m_VotePos;

				// only allow IPs to vote once, but keep veto ability
				// check for more players with the same ip (only use the vote of the one who voted first)
				for(int j = i + 1; j < MAX_CLIENTS; j++)
				{
					if(!GameServer()->GetPlayer(i) || aVoteChecked[j] || str_comp(aaBuf[j], aaBuf[i]) != 0)
						continue;

					// count the latest vote by this ip
					if(ActVotePos < GameServer()->GetPlayer(i)->m_VotePos)
					{
						ActVote = GameServer()->GetPlayer(i)->m_Vote;
						ActVotePos = GameServer()->GetPlayer(i)->m_VotePos;
					}

					aVoteChecked[j] = true;
				}

				Total++;
				if(ActVote > 0)
					Yes++;
				else if(ActVote < 0)
					No++;
			}

			if((Yes > Total / (100.0f / GameServer()->Config()->m_SvVoteYesPercentage)))
				m_VoteState |= VOTE_PASSED;
			else if(No >= Total - Total / (100.0f / GameServer()->Config()->m_SvVoteYesPercentage))
				m_VoteState &= ~VOTE_PASSED;

			if(Yes > (Yes + No) / (100.0f / GameServer()->Config()->m_SvVoteYesPercentage))
				m_VoteState |= VOTE_FINISHED;
		}
	}

	for(int i = 0; i < MAX_CLIENTS; i++)
		ProgressVoteOptions(i);
}

struct CVoteOptionServer *CVoteManager::GetVoteOption(int Index)
{
	CVoteOptionServer *pCurrent;
	for(pCurrent = m_pVoteOptionFirst;
		Index > 0 && pCurrent;
		Index--, pCurrent = pCurrent->m_pNext)
		;

	if(Index > 0)
		return 0;
	return pCurrent;
}

void CVoteManager::ProgressVoteOptions(int ClientID)
{
	CPlayer *pPl = GameServer()->GetPlayer(ClientID);

	if(pPl->m_SendVoteIndex == -1)
		return; // we didn't start sending options yet

	if(pPl->m_SendVoteIndex > m_NumVoteOptions)
		return; // shouldn't happen / fail silently

	int VotesLeft = m_NumVoteOptions - pPl->m_SendVoteIndex;
	int NumVotesToSend = minimum(GameServer()->Config()->m_SvSendVotesPerTick, VotesLeft);

	if(!VotesLeft)
	{
		// player has up to date vote option list
		return;
	}

	// build vote option list msg
	int CurIndex = 0;

	CNetMsg_Sv_VoteOptionListAdd OptionMsg;
	OptionMsg.m_pDescription0 = "";
	OptionMsg.m_pDescription1 = "";
	OptionMsg.m_pDescription2 = "";
	OptionMsg.m_pDescription3 = "";
	OptionMsg.m_pDescription4 = "";
	OptionMsg.m_pDescription5 = "";
	OptionMsg.m_pDescription6 = "";
	OptionMsg.m_pDescription7 = "";
	OptionMsg.m_pDescription8 = "";
	OptionMsg.m_pDescription9 = "";
	OptionMsg.m_pDescription10 = "";
	OptionMsg.m_pDescription11 = "";
	OptionMsg.m_pDescription12 = "";
	OptionMsg.m_pDescription13 = "";
	OptionMsg.m_pDescription14 = "";

	// get current vote option by index
	CVoteOptionServer *pCurrent = GetVoteOption(pPl->m_SendVoteIndex);

	while(pCurrent && CurIndex < NumVotesToSend)
	{
		switch(CurIndex)
		{
		case 0: OptionMsg.m_pDescription0 = pCurrent->m_aDescription; break;
		case 1: OptionMsg.m_pDescription1 = pCurrent->m_aDescription; break;
		case 2: OptionMsg.m_pDescription2 = pCurrent->m_aDescription; break;
		case 3: OptionMsg.m_pDescription3 = pCurrent->m_aDescription; break;
		case 4: OptionMsg.m_pDescription4 = pCurrent->m_aDescription; break;
		case 5: OptionMsg.m_pDescription5 = pCurrent->m_aDescription; break;
		case 6: OptionMsg.m_pDescription6 = pCurrent->m_aDescription; break;
		case 7: OptionMsg.m_pDescription7 = pCurrent->m_aDescription; break;
		case 8: OptionMsg.m_pDescription8 = pCurrent->m_aDescription; break;
		case 9: OptionMsg.m_pDescription9 = pCurrent->m_aDescription; break;
		case 10: OptionMsg.m_pDescription10 = pCurrent->m_aDescription; break;
		case 11: OptionMsg.m_pDescription11 = pCurrent->m_aDescription; break;
		case 12: OptionMsg.m_pDescription12 = pCurrent->m_aDescription; break;
		case 13: OptionMsg.m_pDescription13 = pCurrent->m_aDescription; break;
		case 14: OptionMsg.m_pDescription14 = pCurrent->m_aDescription; break;
		}

		CurIndex++;
		pCurrent = pCurrent->m_pNext;
	}

	// send msg
	OptionMsg.m_NumOptions = NumVotesToSend;
	Server()->SendPackMsg(&OptionMsg, MSGFLAG_VITAL, ClientID);

	pPl->m_SendVoteIndex += NumVotesToSend;
}

void CVoteManager::TryCallVote(int ClientID, CNetMsg_Cl_CallVote *pMsg)
{
	if(IsActive())
		return;

	/*m_VoteType = VOTE_TYPE_UNKNOWN;*/
	char aChatmsg[512] = {0};
	char aDesc[VOTE_DESC_LENGTH] = {0};
	char aSixupDesc[VOTE_DESC_LENGTH] = {0};
	char aCmd[VOTE_CMD_LENGTH] = {0};
	char aReason[VOTE_REASON_LENGTH] = "No reason";

	CPlayer *pPlayer = GameServer()->GetPlayer(ClientID);

	if(!str_utf8_check(pMsg->m_Type) || !str_utf8_check(pMsg->m_Reason) || !str_utf8_check(pMsg->m_Value))
	{
		return;
	}
	if(pMsg->m_Reason[0])
	{
		str_copy(aReason, pMsg->m_Reason, sizeof(aReason));
	}

	if(str_comp_nocase(pMsg->m_Type, "option") == 0)
	{
		int Authed = Server()->GetAuthedState(ClientID);
		CVoteOptionServer *pOption = m_pVoteOptionFirst;
		while(pOption)
		{
			if(str_comp_nocase(pMsg->m_Value, pOption->m_aDescription) == 0)
			{
				if(!GameServer()->Console()->LineIsValid(pOption->m_aCommand))
				{
					GameServer()->SendChatTarget(ClientID, "Invalid option");
					return;
				}

				str_format(aChatmsg, sizeof(aChatmsg), "'%s' called vote to change server option '%s' (%s)", Server()->ClientName(ClientID),
					pOption->m_aDescription, aReason);
				str_format(aDesc, sizeof(aDesc), "%s", pOption->m_aDescription);
				str_format(aCmd, sizeof(aCmd), "%s", pOption->m_aCommand);
				break;
			}

			pOption = pOption->m_pNext;
		}

		if(!pOption)
		{
			if(Authed != AUTHED_ADMIN) // allow admins to call any vote they want
			{
				str_format(aChatmsg, sizeof(aChatmsg), "'%s' isn't an option on this server", pMsg->m_Value);
				GameServer()->SendChatTarget(ClientID, aChatmsg);
				return;
			}
			else
			{
				str_format(aChatmsg, sizeof(aChatmsg), "'%s' called vote to change server option '%s'", Server()->ClientName(ClientID), pMsg->m_Value);
				str_format(aDesc, sizeof(aDesc), "%s", pMsg->m_Value);
				str_format(aCmd, sizeof(aCmd), "%s", pMsg->m_Value);
			}
		}

		m_VoteType = VOTE_TYPE_OPTION;
	}
	else if(str_comp_nocase(pMsg->m_Type, "kick") == 0)
	{
		int Authed = Server()->GetAuthedState(ClientID);
		if(!Authed && time_get() < pPlayer->m_Last_KickVote + (time_freq() * 5))
			return;
		else if(!Authed && time_get() < pPlayer->m_Last_KickVote + (time_freq() * GameServer()->Config()->m_SvVoteKickDelay))
		{
			str_format(aChatmsg, sizeof(aChatmsg), "There's a %d second wait time between kick votes for each player please wait %d second(s)",
				GameServer()->Config()->m_SvVoteKickDelay,
				(int)(((pPlayer->m_Last_KickVote + (pPlayer->m_Last_KickVote * time_freq())) / time_freq()) - (time_get() / time_freq())));
			GameServer()->SendChatTarget(ClientID, aChatmsg);
			pPlayer->m_Last_KickVote = time_get();
			return;
		}
		//else if(!GameServer()->Config()->m_SvVoteKick)
		else if(!GameServer()->Config()->m_SvVoteKick && !Authed) // allow admins to call kick votes even if they are forbidden
		{
			GameServer()->SendChatTarget(ClientID, "Server does not allow voting to kick players");
			pPlayer->m_Last_KickVote = time_get();
			return;
		}

		int KickID = str_toint(pMsg->m_Value);

		if(KickID < 0 || KickID >= MAX_CLIENTS || !GameServer()->GetPlayer(KickID))
		{
			GameServer()->SendChatTarget(ClientID, "Invalid client id to kick");
			return;
		}
		if(KickID == ClientID)
		{
			GameServer()->SendChatTarget(ClientID, "You can't kick yourself");
			return;
		}
		if(!Server()->ReverseTranslate(KickID, ClientID))
		{
			return;
		}
		int KickedAuthed = Server()->GetAuthedState(KickID);
		if(KickedAuthed > Authed)
		{
			GameServer()->SendChatTarget(ClientID, "You can't kick authorized players");
			pPlayer->m_Last_KickVote = time_get();
			char aBufKick[128];
			str_format(aBufKick, sizeof(aBufKick), "'%s' called for vote to kick you", Server()->ClientName(ClientID));
			GameServer()->SendChatTarget(KickID, aBufKick);
			return;
		}

		str_format(aChatmsg, sizeof(aChatmsg), "'%s' called for vote to kick '%s' (%s)", Server()->ClientName(ClientID), Server()->ClientName(KickID), aReason);
		str_format(aSixupDesc, sizeof(aSixupDesc), "%2d: %s", KickID, Server()->ClientName(KickID));

		pPlayer->m_Last_KickVote = time_get();
		m_VoteType = VOTE_TYPE_KICK;
		m_VoteVictim = KickID;
	}
	else if(str_comp_nocase(pMsg->m_Type, "spectate") == 0)
	{
		if(!GameServer()->Config()->m_SvVoteSpectate)
		{
			GameServer()->SendChatTarget(ClientID, "Server does not allow voting to move players to spectators");
			return;
		}

		int SpectateID = str_toint(pMsg->m_Value);

		if(SpectateID < 0 || SpectateID >= MAX_CLIENTS || !pPlayer || pPlayer->GetTeam() == TEAM_SPECTATORS)
		{
			GameServer()->SendChatTarget(ClientID, "Invalid client id to move");
			return;
		}
		if(SpectateID == ClientID)
		{
			GameServer()->SendChatTarget(ClientID, "You can't move yourself");
			return;
		}
		if(!Server()->ReverseTranslate(SpectateID, ClientID))
		{
			return;
		}

		str_format(aSixupDesc, sizeof(aSixupDesc), "%2d: %s", SpectateID, Server()->ClientName(SpectateID));
		m_VoteType = VOTE_TYPE_SPECTATE;
		m_VoteVictim = SpectateID;
	}

	if(aCmd[0] && str_comp(aCmd, "info") != 0)
		CallVote(ClientID, aDesc, aCmd, aReason, aChatmsg, aSixupDesc[0] ? aSixupDesc : 0);
}

void CVoteManager::AddVote(const char *pDescription, const char *pCommand)
{
	if(m_NumVoteOptions == MAX_VOTE_OPTIONS)
	{
		GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", "maximum number of vote options reached");
		return;
	}

	// check for valid option
	if(!GameServer()->Console()->LineIsValid(pCommand) || str_length(pCommand) >= VOTE_CMD_LENGTH)
	{
		char aBuf[256];
		str_format(aBuf, sizeof(aBuf), "skipped invalid command '%s'", pCommand);
		GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
		return;
	}
	while(*pDescription == ' ')
		pDescription++;
	if(str_length(pDescription) >= VOTE_DESC_LENGTH || *pDescription == 0)
	{
		char aBuf[256];
		str_format(aBuf, sizeof(aBuf), "skipped invalid option '%s'", pDescription);
		GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
		return;
	}

	// check for duplicate entry
	CVoteOptionServer *pOption = m_pVoteOptionFirst;
	while(pOption)
	{
		if(str_comp_nocase(std::string(pDescription).c_str(), pOption->m_aDescription) == 0)
		{
			char aBuf[256];
			str_format(aBuf, sizeof(aBuf), "option '%s' already exists", pDescription);
			GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
			return;
		}
		pOption = pOption->m_pNext;
	}

	// add the option
	++m_NumVoteOptions;
	int Len = str_length(pCommand);

	pOption = (CVoteOptionServer *)m_pVoteOptionHeap->Allocate(sizeof(CVoteOptionServer) + Len, alignof(CVoteOptionServer));
	pOption->m_pNext = 0;
	pOption->m_pPrev = m_pVoteOptionLast;
	if(pOption->m_pPrev)
		pOption->m_pPrev->m_pNext = pOption;
	m_pVoteOptionLast = pOption;
	if(!m_pVoteOptionFirst)
		m_pVoteOptionFirst = pOption;

	str_copy(pOption->m_aDescription, pDescription, sizeof(pOption->m_aDescription));
	mem_copy(pOption->m_aCommand, pCommand, Len + 1);
}

void CVoteManager::RemoveVote(const char *pDescription)
{
	// check for valid option
	CVoteOptionServer *pOption = m_pVoteOptionFirst;
	while(pOption)
	{
		if(str_comp_nocase(pDescription, pOption->m_aDescription) == 0)
			break;
		pOption = pOption->m_pNext;
	}
	if(!pOption)
	{
		char aBuf[256];
		str_format(aBuf, sizeof(aBuf), "option '%s' does not exist", pDescription);
		GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
		return;
	}

	// start reloading vote option list
	// clear vote options
	CNetMsg_Sv_VoteClearOptions VoteClearOptionsMsg;
	Server()->SendPackMsg(&VoteClearOptionsMsg, MSGFLAG_VITAL, -1);

	// reset sending of vote options
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(CPlayer *pPlayer = GameServer()->GetPlayer(i))
			pPlayer->m_SendVoteIndex = 0;
	}

	// TODO: improve this
	// remove the option
	--m_NumVoteOptions;

	CHeap *pVoteOptionHeap = new CHeap();
	CVoteOptionServer *pVoteOptionFirst = 0;
	CVoteOptionServer *pVoteOptionLast = 0;
	int NumVoteOptions = m_NumVoteOptions;
	for(CVoteOptionServer *pSrc = m_pVoteOptionFirst; pSrc; pSrc = pSrc->m_pNext)
	{
		if(pSrc == pOption)
			continue;

		// copy option
		int Len = str_length(pSrc->m_aCommand);
		CVoteOptionServer *pDst = (CVoteOptionServer *)pVoteOptionHeap->Allocate(sizeof(CVoteOptionServer) + Len);
		pDst->m_pNext = 0;
		pDst->m_pPrev = pVoteOptionLast;
		if(pDst->m_pPrev)
			pDst->m_pPrev->m_pNext = pDst;
		pVoteOptionLast = pDst;
		if(!pVoteOptionFirst)
			pVoteOptionFirst = pDst;

		str_copy(pDst->m_aDescription, pSrc->m_aDescription, sizeof(pDst->m_aDescription));
		mem_copy(pDst->m_aCommand, pSrc->m_aCommand, Len + 1);
	}

	// clean up
	delete m_pVoteOptionHeap;
	m_pVoteOptionHeap = pVoteOptionHeap;
	m_pVoteOptionFirst = pVoteOptionFirst;
	m_pVoteOptionLast = pVoteOptionLast;
	m_NumVoteOptions = NumVoteOptions;
}

void CVoteManager::ForceCurrentVote(int EnforcerID, bool Success)
{
	if(!IsActive())
		return;

	m_VoteState |= VOTE_FINISHED;
	m_VoteState = Success ? m_VoteState | VOTE_PASSED : m_VoteState & ~VOTE_PASSED;
	m_VoteEnforcer = EnforcerID;

	char aBuf[256];
	const char *pOption = Success ? "yes" : "no";
	str_format(aBuf, sizeof(aBuf), "authorized player forced vote %s", pOption);
	GameServer()->SendChatTarget(-1, aBuf);
	str_format(aBuf, sizeof(aBuf), "forcing vote %s", pOption);
	GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
}

void CVoteManager::ForceVote(int ClientID, const char *pType, const char *pValue, const char *pReason)
{
	char aBuf[256];
	if(str_comp_nocase(pType, "option") == 0)
	{
		CVoteOptionServer *pOption = m_pVoteOptionFirst;
		while(pOption)
		{
			if(str_comp_nocase(pValue, pOption->m_aDescription) == 0)
			{
				str_format(aBuf, sizeof(aBuf), "authorized player forced server option '%s' (%s)", pValue, pReason);
				GameServer()->SendChatTarget(-1, aBuf, CGameContext::CHAT_SIX);
				GameServer()->Console()->ExecuteLine(pOption->m_aCommand);
				break;
			}

			pOption = pOption->m_pNext;
		}

		if(!pOption)
		{
			str_format(aBuf, sizeof(aBuf), "'%s' isn't an option on this server", pValue);
			GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
			return;
		}
	}
	else if(str_comp_nocase(pType, "kick") == 0)
	{
		int KickID = str_toint(pValue);
		if(KickID < 0 || KickID >= MAX_CLIENTS || !GameServer()->GetPlayer(KickID))
		{
			GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", "Invalid client id to kick");
			return;
		}

		if(!GameServer()->Config()->m_SvVoteKickBantime)
		{
			str_format(aBuf, sizeof(aBuf), "kick %d %s", KickID, pReason);
			GameServer()->Console()->ExecuteLine(aBuf);
		}
		else
		{
			char aAddrStr[NETADDR_MAXSTRSIZE] = {0};
			Server()->GetClientAddr(KickID, aAddrStr, sizeof(aAddrStr));
			str_format(aBuf, sizeof(aBuf), "ban %s %d %s", aAddrStr, GameServer()->Config()->m_SvVoteKickBantime, pReason);
			GameServer()->Console()->ExecuteLine(aBuf);
		}
	}
	else if(str_comp_nocase(pType, "spectate") == 0)
	{
		int SpectateID = str_toint(pValue);
		if(SpectateID < 0 || SpectateID >= MAX_CLIENTS || !GameServer()->GetPlayer(SpectateID) || GameServer()->GetPlayer(SpectateID)->GetTeam() == TEAM_SPECTATORS)
		{
			GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", "Invalid client id to move");
			return;
		}

		str_format(aBuf, sizeof(aBuf), "'%s' was moved to spectator (%s)", Server()->ClientName(SpectateID), pReason);
		GameServer()->SendChatTarget(-1, aBuf);
		str_format(aBuf, sizeof(aBuf), "set_team %d -1 %d", SpectateID, GameServer()->Config()->m_SvVoteSpectateRejoindelay);
		GameServer()->Console()->ExecuteLine(aBuf);
	}
}

void CVoteManager::OnClientEnter(int ClientID)
{

}