/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include <base/system.h>
#include <engine/kernel.h>

class CKernel : public IKernel
{
	enum
	{
		MAX_INTERFACES = 256,
	};

	class CInterfaceInfo
	{
	public:
		CInterfaceInfo()
		{
			m_aID = 0;
			m_aName[0] = 0;
			m_pInterface = nullptr;
			m_AutoDestroy = false;
		}

		int m_aID;
		char m_aName[64];
		IInterface *m_pInterface;
		bool m_AutoDestroy;
	};

	CInterfaceInfo m_aInterfaces[MAX_INTERFACES];
	int m_NumInterfaces;

	CInterfaceInfo *FindInterfaceInfo(const char *pName, int ID = 0)
	{
		for(int i = 0; i < m_NumInterfaces; i++)
		{
			if(ID == m_aInterfaces[i].m_aID && str_comp(pName, m_aInterfaces[i].m_aName) == 0)
				return &m_aInterfaces[i];
		}
		return nullptr;
	}

public:
	CKernel()
	{
		m_NumInterfaces = 0;
	}

	~CKernel() override
	{
		// delete interfaces in reverse order just the way it would happen to objects on the stack
		for(int i = m_NumInterfaces - 1; i >= 0; i--)
		{
			if(m_aInterfaces[i].m_AutoDestroy)
			{
				delete m_aInterfaces[i].m_pInterface;
				m_aInterfaces[i].m_pInterface = nullptr;
			}
		}
	}

	bool RegisterInterfaceImpl(const char *pName, IInterface *pInterface, bool Destroy, int ID = 0) override
	{
		// TODO: More error checks here
		if(!pInterface)
		{
			dbg_msg("kernel", "ERROR: couldn't register interface %s. null pointer given", pName);
			return false;
		}

		if(m_NumInterfaces == MAX_INTERFACES)
		{
			dbg_msg("kernel", "ERROR: couldn't register interface '%s'. maximum of interfaces reached", pName);
			return false;
		}

		if(FindInterfaceInfo(pName, ID) != nullptr)
		{
			dbg_msg("kernel", "ERROR: couldn't register interface '%s'. interface already exists", pName);
			return false;
		}

		pInterface->m_pKernel = this;
		m_aInterfaces[m_NumInterfaces].m_pInterface = pInterface;
		m_aInterfaces[m_NumInterfaces].m_aID = ID;
		str_copy(m_aInterfaces[m_NumInterfaces].m_aName, pName, sizeof(m_aInterfaces[m_NumInterfaces].m_aName));
		m_aInterfaces[m_NumInterfaces].m_AutoDestroy = Destroy;
		m_NumInterfaces++;

		return true;
	}

	bool ReregisterInterfaceImpl(const char *pName, IInterface *pInterface, int ID = 0) override
	{
		if(FindInterfaceInfo(pName, ID) == nullptr)
		{
			dbg_msg("kernel", "ERROR: couldn't reregister interface '%s'. interface doesn't exist", pName);
			return false;
		}

		pInterface->m_pKernel = this;

		return true;
	}

	IInterface *RequestInterfaceImpl(const char *pName, int ID = 0) override
	{
		const CInterfaceInfo *pInfo = FindInterfaceInfo(pName, ID);
		if(!pInfo)
		{
			dbg_msg("kernel", "failed to find interface with the name '%s', ID '%d'", pName, ID);
			return nullptr;
		}
		return pInfo->m_pInterface;
	}
};

IKernel *IKernel::Create() { return new CKernel; }