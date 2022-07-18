/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef ENGINE_KERNEL_H
#define ENGINE_KERNEL_H

#include <base/system.h>

class IKernel;
class IInterface;

class IInterface
{
	// friend with the kernel implementation
	friend class CKernel;
	IKernel *m_pKernel;

protected:
	IKernel *Kernel() { return m_pKernel; }

public:
	IInterface() :
		m_pKernel(nullptr) {}
	virtual ~IInterface() {}
};

#define MACRO_INTERFACE(Name, ver) \
public: \
	static const char *InterfaceName() { return Name; } \
\
private:

// This kernel thingie makes the structure very flat and basiclly singletons.
// I'm not sure if this is a good idea but it works for now.
class IKernel
{
	// hide the implementation
	virtual bool RegisterInterfaceImpl(const char *InterfaceName, IInterface *pInterface, bool Destroy, int ID = 0) = 0;
	virtual bool ReregisterInterfaceImpl(const char *InterfaceName, IInterface *pInterface, int ID = 0) = 0;
	virtual IInterface *RequestInterfaceImpl(const char *InterfaceName, int ID = 0) = 0;

public:
	static IKernel *Create();
	virtual ~IKernel() {}

	// templated access to handle pointer convertions and interface names
	template<class TINTERFACE>
	bool RegisterInterface(TINTERFACE *pInterface, bool Destroy = true, int ID = 0)
	{
		return RegisterInterfaceImpl(TINTERFACE::InterfaceName(), pInterface, Destroy, ID);
	}
	template<class TINTERFACE>
	bool ReregisterInterface(TINTERFACE *pInterface, int ID = 0)
	{
		return ReregisterInterfaceImpl(TINTERFACE::InterfaceName(), pInterface, ID);
	}

	// Usage example:
	//		IMyInterface *pMyHandle = Kernel()->RequestInterface<IMyInterface>()
	template<class TINTERFACE>
	TINTERFACE *RequestInterface(int ID = 0)
	{
		return reinterpret_cast<TINTERFACE *>(RequestInterfaceImpl(TINTERFACE::InterfaceName(), ID));
	}
};

#endif