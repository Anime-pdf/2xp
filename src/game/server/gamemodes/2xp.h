/* (c) Shereef Marzouk. See "licence DDRace.txt" and the readme.txt in the root of the distribution for more information. */
#ifndef GAME_SERVER_GAMEMODES_2XP_H
#define GAME_SERVER_GAMEMODES_2XP_H

#include <game/server/gamecontroller.h>

#include <map>
#include <vector>

class CGCTXP : public IGameController
{
public:
	CGCTXP(class CGameContext *pGameServer);
	~CGCTXP();

	virtual void OnCharacterSpawn(class CCharacter *pChr) override;

	virtual void OnPlayerConnect(class CPlayer *pPlayer) override;
	virtual void OnPlayerDisconnect(class CPlayer *pPlayer, const char *pReason) override;

	virtual void Tick() override;

	virtual void DoTeamChange(class CPlayer *pPlayer, int Team) override;

	void InitTeleporter();

	std::map<int, std::vector<vec2>> m_TeleOuts;
	std::map<int, std::vector<vec2>> m_TeleCheckOuts;
};
#endif // GAME_SERVER_GAMEMODES_2XP_H
