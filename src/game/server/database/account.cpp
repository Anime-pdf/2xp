#include "account.h"

#include <base/hash_ctxt.h>
#include <engine/server/sql_connect_pool.h>
#include <engine/server/sql_string_helpers.h>

std::string Hash(const char *pPassword, const char* pSalt)
{
	char aPlaintext[128] = {0};

	SHA256_CTX Sha256Ctx;
	sha256_init(&Sha256Ctx);
	str_format(aPlaintext, sizeof(aPlaintext), "%s%s%s", pSalt, pPassword, pSalt);
	sha256_update(&Sha256Ctx, aPlaintext, str_length(aPlaintext));

	SHA256_DIGEST Digest = sha256_finish(&Sha256Ctx);

	char aHash[SHA256_MAXSTRSIZE];
	sha256_str(Digest, aHash, sizeof(aHash));

	return std::string(aHash);
}

CAccount::CAccount(CUuid Id, const char *Login)
{
	m_Self.Id = Id;
	m_Self.Login = std::string(Login);
}

CAccount* CAccount::Login(const char *Login, const char *Password, int &State)
{
	const char *ClearPassword = sqlstr::CSqlString<32>(Login).cstr();
	ResultPtr pRes = SJK.SSD("id, login, password, salt", "accounts", "WHERE login = '%s'", ClearPassword);

	if (pRes->next())
	{
		bool Success = false;

		std::string Salt(std::istreambuf_iterator<char>(*pRes->getBlob("salt")), std::istreambuf_iterator<char>());
		std::string Pass(std::istreambuf_iterator<char>(*pRes->getBlob("password")), std::istreambuf_iterator<char>());
		std::string HashP = Hash(sqlstr::CSqlString<32>(Password).cstr(), Salt.c_str()).c_str();
		if(!str_comp(Pass.c_str(), HashP.c_str()))
			Success = true;

		if(!Success)
		{
			State = WRONG_PASSWORD;
			return 0;
		}

		/* if (smth)
		{
			State = ALREADY_LOGIN;
			return 0;
		} */

		CUuid Uuid;

		if (!ParseUuid(&Uuid, pRes->getBlob("id")))
		{
			State = SUCCESS;
			return new CAccount(Uuid, pRes->getString("login").c_str());
		}
	}

	State = UNKNOWN_LOGIN;
	return 0;
}

CAccount *CAccount::Register(const char *Login, const char *Password, int &State)
{
	char ClearLogin[64];
	str_copy(ClearLogin, sqlstr::CSqlString<32>(Login).cstr(), sizeof(ClearLogin));

	{
		ResultPtr pRes = SJK.SSD("id", "accounts", "WHERE login = '%s'", ClearLogin);

		if(pRes->next())
		{
			State = ALREADY_REGISTERED;
			return 0;
		}
	}

	{
		struct membuf : std::streambuf
		{
			membuf(char *d, size_t s)
			{
				setg(d, d, d + s - 1); //  little trolling
			}
		};

		char aSalt[25] = {0};
		secure_random_password(aSalt, sizeof(aSalt), 24);

		char HashP[65];
		str_copy(HashP, Hash(sqlstr::CSqlString<32>(Password).cstr(), aSalt).c_str(), sizeof(HashP));

		SSqlArg Arguments[3] =
			{
				SSqlString(ClearLogin),
				SSqlBlob(&std::istream(&membuf(HashP, sizeof(HashP)))),
				SSqlBlob(&std::istream(&membuf(aSalt, sizeof(aSalt))))
			};

		SJK.SPS("INSERT INTO accounts (login, password, salt) VALUES (?, ?, ?)", 3, Arguments);

		ResultPtr pRes = SJK.SSD("id", "accounts", "WHERE login = '%s'", ClearLogin);

		// oof, should remake it with SJK.ASDS
		while(!pRes->next())
			pRes = SJK.SSD("id", "accounts", "WHERE login = '%s'", ClearLogin);

		CUuid Uuid;

		if(!ParseUuid(&Uuid, pRes->getBlob("id")))
		{
			State = SUCCESS;
			return new CAccount(Uuid, ClearLogin);
		}
	}

	State = FAIL;
	return 0;
}

void CAccount::Format(char *pBuffer, int BufferSize)
{
	char Id[UUID_MAXSTRSIZE];
	FormatUuid(m_Self.Id, Id, sizeof(Id));
	str_format(pBuffer, BufferSize, "%s %s", Id, m_Self.Login.c_str());
}