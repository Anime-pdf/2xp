#ifndef GAME_SERVER_DATABASE_ACCOUNT_H
#define GAME_SERVER_DATABASE_ACCOUNT_H

#include <engine/shared/uuid_manager.h>

class CAccount
{
	CAccount(CUuid Id, const char *Login);
	struct
	{
		CUuid Id;
		std::string Login;
	} m_Self;
public:
	enum
	{
		SUCCESS,
		ALREADY_LOGIN,
		UNKNOWN_LOGIN,
		WRONG_PASSWORD,
		ALREADY_REGISTERED,
		FAIL
	};

	static CAccount *Login(const char *Login, const char *Password, int &State);
	static CAccount *Register(const char *Login, const char *Password, int &State);

	void Format(char *pBuffer, int BufferSize);
};

#endif 