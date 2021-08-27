#ifdef CONF_UPNP

#include "upnp.h"
#include <base/system.h>
#include <engine/shared/config.h>
#include <game/version.h>
#include <miniupnpc/miniupnpc.h>
#include <miniupnpc/upnpcommands.h>
#include <miniupnpc/upnperrors.h>

#include "spdlog/spdlog.h"

#define FMT "[UPnP] "

void CUPnP::Open(NETADDR Address)
{
	if(g_Config.m_SvUseUPnP)
	{
		m_Enabled = false;
		m_Addr = Address;
		m_UPnPUrls = (struct UPNPUrls *)malloc(sizeof(struct UPNPUrls));
		m_UPnPData = (struct IGDdatas *)malloc(sizeof(struct IGDdatas));

		char aLanAddr[64];
		char aPort[6];
		int Error;

		m_UPnPDevice = upnpDiscover(2000, NULL, NULL, 0, 0, 2, &Error);

		int Status = UPNP_GetValidIGD(m_UPnPDevice, m_UPnPUrls, m_UPnPData, aLanAddr, sizeof(aLanAddr));
		spdlog::info(FMT "Status: {}. LAN Address: {}", Status, aLanAddr);

		if(Status == 1)
		{
			m_Enabled = true;
			spdlog::info(FMT "Found valid IGD: {}", m_UPnPUrls->controlURL);
			str_format(aPort, sizeof(aPort), "%d", m_Addr.port);
			Error = UPNP_AddPortMapping(m_UPnPUrls->controlURL, m_UPnPData->first.servicetype,
				aPort, aPort, aLanAddr,
				"2XP Server" GAME_RELEASE_VERSION,
				"UDP", NULL, "0");

			if(Error)
				spdlog::info(FMT "Failed to map port: {}", strupnperror(Error));
			else
				spdlog::info(FMT "Successfully mapped port");
		}
		else
			spdlog::info(FMT "No valid IGD found, disabled");
	}
}

void CUPnP::Shutdown()
{
	if(g_Config.m_SvUseUPnP)
	{
		if(m_Enabled)
		{
			char aPort[6];
			str_format(aPort, sizeof(aPort), "%d", m_Addr.port);
			int Error = UPNP_DeletePortMapping(m_UPnPUrls->controlURL, m_UPnPData->first.servicetype, aPort, "UDP", NULL);

			if(Error != 0)
			{
				spdlog::info(FMT "Failed to delete port mapping on shutdown: {}", strupnperror(Error));
			}
			FreeUPNPUrls(m_UPnPUrls);
			freeUPNPDevlist(m_UPnPDevice);
		}
		free(m_UPnPUrls);
		free(m_UPnPData);
		m_UPnPUrls = NULL;
		m_UPnPData = NULL;
	}
}

#endif