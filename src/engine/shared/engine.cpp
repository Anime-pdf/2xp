/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */

#include <base/system.h>

#include <engine/console.h>
#include <engine/engine.h>
#include <engine/shared/config.h>
#include <engine/shared/network.h>
#include <engine/storage.h>

#include "spdlog/spdlog.h"

#define FMT "[Engine] "

CHostLookup::CHostLookup()
{
}

CHostLookup::CHostLookup(const char *pHostname, int Nettype)
{
	str_copy(m_aHostname, pHostname, sizeof(m_aHostname));
	m_Nettype = Nettype;
}

void CHostLookup::Run()
{
	m_Result = net_host_lookup(m_aHostname, &m_Addr, m_Nettype);
}

class CEngine : public IEngine
{
public:
	IConsole *m_pConsole;
	IStorage *m_pStorage;
	bool m_Logging;

	static void Con_DbgLognetwork(IConsole::IResult *pResult, void *pUserData)
	{
		CEngine *pEngine = static_cast<CEngine *>(pUserData);

		if(pEngine->m_Logging)
		{
			CNetBase::CloseLog();
			pEngine->m_Logging = false;
		}
		else
		{
			char aBuf[32];
			str_timestamp(aBuf, sizeof(aBuf));
			char aFilenameSent[128], aFilenameRecv[128];
			str_format(aFilenameSent, sizeof(aFilenameSent), "dumps/network_sent_%s.txt", aBuf);
			str_format(aFilenameRecv, sizeof(aFilenameRecv), "dumps/network_recv_%s.txt", aBuf);
			CNetBase::OpenLog(pEngine->m_pStorage->OpenFile(aFilenameSent, IOFLAG_WRITE, IStorage::TYPE_SAVE),
				pEngine->m_pStorage->OpenFile(aFilenameRecv, IOFLAG_WRITE, IStorage::TYPE_SAVE));
			pEngine->m_Logging = true;
		}
	}

	CEngine(bool Test, const char *pAppname, bool Silent, int Jobs)
	{
		if(!Test)
		{
			spdlog::set_pattern("[%T %z] [^%l%$] %v");
			spdlog::set_level(spdlog::level::critical);
			if(!Silent)
				spdlog::set_level(spdlog::level::info);
#ifdef CONF_DEBUG
			spdlog::set_level(spdlog::level::debug);
#endif

			//
			spdlog::info(FMT "Running on {}-{}-{}", CONF_FAMILY_STRING, CONF_PLATFORM_STRING, CONF_ARCH_STRING);
#ifdef CONF_ARCH_ENDIAN_LITTLE
			spdlog::info(FMT "Arch: Little Endian");
#elif defined(CONF_ARCH_ENDIAN_BIG)
			spdlog::info(FMT "Arch: Big Endian");
#else
			spdlog::warn(FMT "Arch: Unknown");
#endif

			// init the network
			net_init();
			CNetBase::Init();
		}

		m_JobPool.Init(Jobs);

		m_Logging = false;
	}

	void Init()
	{
		m_pConsole = Kernel()->RequestInterface<IConsole>();
		m_pStorage = Kernel()->RequestInterface<IStorage>();

		if(!m_pConsole || !m_pStorage)
			return;

		m_pConsole->Register("dbg_lognetwork", "", CFGFLAG_SERVER | CFGFLAG_CLIENT, Con_DbgLognetwork, this, "Log the network");
	}

	void InitLogfile()
	{
		// open logfile if needed
		//if(g_Config.m_Logfile[0])
			//dbg_logger_file(g_Config.m_Logfile);
	}

	void AddJob(std::shared_ptr<IJob> pJob)
	{
		spdlog::debug(FMT "Job added");
		m_JobPool.Add(std::move(pJob));
	}
};

void IEngine::RunJobBlocking(IJob *pJob)
{
	CJobPool::RunBlocking(pJob);
}

IEngine *CreateEngine(const char *pAppname, bool Silent, int Jobs) { return new CEngine(false, pAppname, Silent, Jobs); }
IEngine *CreateTestEngine(const char *pAppname, int Jobs) { return new CEngine(true, pAppname, true, Jobs); }
