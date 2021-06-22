/* (c) Shereef Marzouk. See "licence DDRace.txt" and the readme.txt in the root of the distribution for more information. */
#ifndef GAME_SERVER_GAMEMODES_2XP_H
#define GAME_SERVER_GAMEMODES_2XP_H
#include <game/server/entities/door.h>
#include <game/server/gamecontroller.h>

#include <map>
#include <vector>

struct CScoreInitResult;
class CGameControllerDDRace : public IGameController
{
public:
	CGameControllerDDRace(class CGameContext *pGameServer);
	~CGameControllerDDRace();

	virtual void OnCharacterSpawn(class CCharacter *pChr) override;
	virtual void HandleCharacterTiles(class CCharacter *pChr, int MapIndex) override;

	virtual void OnPlayerConnect(class CPlayer *pPlayer) override;
	virtual void OnPlayerDisconnect(class CPlayer *pPlayer, const char *pReason) override;

	virtual void Tick() override;

	virtual void DoTeamChange(class CPlayer *pPlayer, int Team, bool DoChatMsg = true) override;

	void InitTeleporter();

	std::map<int, std::vector<vec2>> m_TeleOuts;
	std::map<int, std::vector<vec2>> m_TeleCheckOuts;

	std::shared_ptr<CScoreInitResult> m_pInitResult;
};
#endif // GAME_SERVER_GAMEMODES_2XP_H
