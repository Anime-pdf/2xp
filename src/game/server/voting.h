/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_VOTING_H
#define GAME_VOTING_H

enum
{
	VOTE_DESC_LENGTH = 64,
	VOTE_CMD_LENGTH = 512,
	VOTE_REASON_LENGTH = 16,

	MAX_VOTE_OPTIONS = 8192,
};

struct CVoteOptionServer
{
	CVoteOptionServer *m_pNext;
	CVoteOptionServer *m_pPrev;
	char m_aDescription[VOTE_DESC_LENGTH];
	char m_aCommand[1];
};

class IVoteManager : public IInterface
{

};

class IVoteEngine : public IVoteManager
{

};

class CVoteManager
{
	CGameContext *m_pGameServer;

	CHeap *m_pVoteOptionHeap;
	CVoteOptionServer *m_pVoteOptionFirst;
	CVoteOptionServer *m_pVoteOptionLast;
	int m_NumVoteOptions;

	char m_VoteState;
	int m_VoteVictim;
	int m_VoteEnforcer;

	int m_VoteCreator;
	int m_VoteType;
	int64 m_VoteCloseTime;
	bool m_VoteUpdate;
	char m_aVoteDescription[VOTE_DESC_LENGTH];
	char m_aSixupVoteDescription[VOTE_DESC_LENGTH];
	char m_aVoteCommand[VOTE_CMD_LENGTH];
	char m_aVoteReason[VOTE_REASON_LENGTH];

public:
	enum
	{
		// vote state
		VOTE_FINISHED = 1 << 0,
		VOTE_PASSED = 1 << 1,
		VOTE_FORCED = 1 << 2,
		VOTE_ABORTED = 1 << 3,

		// vote types
		VOTE_TYPE_UNKNOWN = 0,
		VOTE_TYPE_OPTION,
		VOTE_TYPE_KICK,
		VOTE_TYPE_SPECTATE,
	};

	void Reset(bool Clear = false);

	CVoteManager(CGameContext *pGameServer);
	~CVoteManager();

	CGameContext *GameServer() { return m_pGameServer; }
	IServer *Server() { return GameServer()->Server(); }

	char GetState() const { return m_VoteState; }
	int GetVictim() const { return m_VoteVictim; }
	int GetEnforcer() const { return m_VoteEnforcer; }
	struct CVoteOptionServer *GetVoteOption(int Index);
	struct CVoteOptionServer *GetVoteOptionByDesc(const char* Desc);

	inline bool IsOptionVote() const { return m_VoteType == VOTE_TYPE_OPTION; };
	inline bool IsKickVote() const { return m_VoteType == VOTE_TYPE_KICK; };
	inline bool IsSpecVote() const { return m_VoteType == VOTE_TYPE_SPECTATE; };

	bool IsUpdating() const { return m_VoteUpdate; }
	bool IsActive() const { return m_VoteCloseTime; }

	void SetUpdating(bool State) { m_VoteUpdate = State; }

	void TryCallVote(int ClientID, CNetMsg_Cl_CallVote *pMsg);
	/* make it private */void CallVote(int ClientID, const char *aDesc, const char *aCmd, const char *pReason, const char *aChatmsg, const char *pSixupDesc = 0);
	void ForceVote(int ClientID, const char *pType, const char *pValue, const char *pReason);
	void ForceCurrentVote(int EnforcerID, bool Success);
	/* make it private */ void ProgressVoteOptions(int ClientID);

	void ClearVotes();
	void AddVote(const char *pDescription, const char *pCommand);
	void RemoveVote(const char *pDescription);

	/* make it private */ void StartVote(const char *pDesc, const char *pCommand, const char *pReason, const char *pSixupDesc);
	void Tick();
	/* make it private */ void EndVote();

	/* make it private */ void SendVoteSet(int ClientID);
	/* make it private */ void SendVoteStatus(int ClientID, int Total, int Yes, int No);
	void AbortVoteKickOnDisconnect(int ClientID);

	// hooks
	void OnClientEnter(int ClientID);
};

#endif
