/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include <base/system.h>
#include <engine/kernel.h>

#include "spdlog/spdlog.h"

#define FMT "[Kernel] "

class CKernel : public IKernel
{
	enum
	{
		MAX_INTERFACES = 32,
	};

	class CInterfaceInfo
	{
	public:
		CInterfaceInfo()
		{
			m_aName[0] = 0;
			m_pInterface = 0x0;
			m_AutoDestroy = false;
		}

		char m_aName[64];
		IInterface *m_pInterface;
		bool m_AutoDestroy;
	};

	CInterfaceInfo m_aInterfaces[MAX_INTERFACES];
	int m_NumInterfaces;

	CInterfaceInfo *FindInterfaceInfo(const char *pName)
	{
		for(int i = 0; i < m_NumInterfaces; i++)
		{
			if(str_comp(pName, m_aInterfaces[i].m_aName) == 0)
				return &m_aInterfaces[i];
		}
		return 0x0;
	}

public:
	CKernel()
	{
		m_NumInterfaces = 0;
	}

	virtual ~CKernel()
	{
		// delete interfaces in reverse order just the way it would happen to objects on the stack
		for(int i = m_NumInterfaces - 1; i >= 0; i--)
		{
			if(m_aInterfaces[i].m_AutoDestroy)
			{
				delete m_aInterfaces[i].m_pInterface;
				m_aInterfaces[i].m_pInterface = 0;
			}
		}
	}

	virtual bool RegisterInterfaceImpl(const char *pName, IInterface *pInterface, bool Destroy)
	{
		// TODO: More error checks here
		if(!pInterface)
		{
			spdlog::error(FMT "Couldn't register interface {}. Null pointer given", pName);
			return false;
		}

		if(m_NumInterfaces == MAX_INTERFACES)
		{
			spdlog::error(FMT "Couldn't register interface {}. Maximum of interfaces reached", pName);
			return false;
		}

		if(FindInterfaceInfo(pName) != 0)
		{
			spdlog::error(FMT "Couldn't register interface {}. Interface already exists", pName);
			return false;
		}

		pInterface->m_pKernel = this;
		m_aInterfaces[m_NumInterfaces].m_pInterface = pInterface;
		str_copy(m_aInterfaces[m_NumInterfaces].m_aName, pName, sizeof(m_aInterfaces[m_NumInterfaces].m_aName));
		m_aInterfaces[m_NumInterfaces].m_AutoDestroy = Destroy;
		m_NumInterfaces++;

		return true;
	}

	virtual bool ReregisterInterfaceImpl(const char *pName, IInterface *pInterface)
	{
		if(FindInterfaceInfo(pName) == 0)
		{
			spdlog::error(FMT "Couldn't re-register interface {}. Interface doesn't exist", pName);
			return false;
		}

		pInterface->m_pKernel = this;

		return true;
	}

	virtual IInterface *RequestInterfaceImpl(const char *pName)
	{
		CInterfaceInfo *pInfo = FindInterfaceInfo(pName);
		if(!pInfo)
		{
			spdlog::error(FMT "Failed to find interface with the name '{}'", pName);
			return 0;
		}
		return pInfo->m_pInterface;
	}
};

IKernel *IKernel::Create() { return new CKernel; }
