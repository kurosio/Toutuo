#include "sql_connect_pool.h"

#include <mutex>
#include <engine/shared/config.h>

#include <cstdarg>

/*
	I don't see the point in using SELECT operations in the thread,
	since this will lead to unnecessary code, which may cause confusion,
	and by calculations if (SQL server / server) = localhost,
	this will not do any harm (but after the release is complete,
	it is advisable to use the Thread function with Callback)

	And in General, you should review the SQL system,
	it works (and has been tested by time and tests),
	but this implementation is not very narrowly focused

	This approach works if the old query is not executed before,
	 a new query it will create a reserve.

	 It may seem that it does not use Pool,
	 but in fact it is and is created as a reserve when running
	 <tlock>

	 Usage is performed in turn following synchronously
	 working running through each request in order

	 This pool is not asynchronous
*/
// multithread mutex :: warning recursive
std::recursive_mutex SqlThreadRecursiveLock;
std::atomic_flag g_atomic_lock;

// #####################################################
// SQL CONNECTION POOL
// #####################################################
std::shared_ptr<CConectionPool> CConectionPool::m_Instance;
CConectionPool::CConectionPool()
{
	try
	{
		m_pDriver = get_driver_instance();
		for(int i = 0; i < g_Config.m_SvMySqlPoolSize; ++i)
			this->CreateConnection();

	}
	catch(SQLException &e)
	{
		dbg_msg("Sql Exception", "%s", e.what());
		exit(0);
	}
}

CConectionPool::~CConectionPool()
{
	DisconnectConnectionHeap();
}

CConectionPool& CConectionPool::GetInstance()
{
	if (m_Instance.get() == nullptr)
		m_Instance.reset(new CConectionPool());
	return *m_Instance.get();
}

Connection* CConectionPool::CreateConnection()
{
	Connection *pConnection = nullptr;
	while(pConnection == nullptr)
	{
		try
		{
			std::string Hostname(g_Config.m_SvMySqlHost);
			Hostname.append(":" + std::to_string(g_Config.m_SvMySqlPort));
			pConnection = m_pDriver->connect(Hostname.c_str(), g_Config.m_SvMySqlLogin, g_Config.m_SvMySqlPassword);
			
			pConnection->setClientOption("OPT_CHARSET_NAME", "utf8mb4");
			pConnection->setClientOption("OPT_CONNECT_TIMEOUT", "10");
			pConnection->setClientOption("OPT_READ_TIMEOUT", "10");
			pConnection->setClientOption("OPT_WRITE_TIMEOUT", "20");
			pConnection->setClientOption("OPT_RECONNECT", "1");
			
			pConnection->setSchema(g_Config.m_SvMySqlDatabase);
		}
		catch(SQLException &e)
		{
			dbg_msg("Sql Exception", "%s", e.what());
			DisconnectConnection(pConnection);
		}
	}
	g_atomic_lock.test_and_set(std::memory_order_acquire);
	m_ConnList.push_back(pConnection);
	g_atomic_lock.clear(std::memory_order_release);
	return pConnection;
}

Connection* CConectionPool::GetConnection()
{
	Connection* pConnection;
	if(m_ConnList.empty())
	{
		pConnection = CreateConnection();
		return pConnection;
	}

	g_atomic_lock.test_and_set(std::memory_order_acquire);
	pConnection = m_ConnList.front();
	m_ConnList.pop_front();
	g_atomic_lock.clear(std::memory_order_relaxed);

	if(pConnection->isClosed())
	{
		delete pConnection;
		pConnection = nullptr;
		pConnection = CreateConnection();
	}

	return pConnection;
}

void CConectionPool::ReleaseConnection(Connection* pConnection)
{
	if(pConnection)
	{
		g_atomic_lock.test_and_set(std::memory_order_acquire);
		m_ConnList.push_back(pConnection);
		g_atomic_lock.clear(std::memory_order_release);
	}
}

void CConectionPool::DisconnectConnection(Connection* pConnection)
{
	if(pConnection)
	{
		try
		{
			pConnection->close();
		}
		catch(SQLException& e)
		{
			dbg_msg("Sql Exception", "%s", e.what());
		}
	}
	g_atomic_lock.test_and_set(std::memory_order_acquire);
	m_ConnList.remove(pConnection);
	delete pConnection;
	pConnection = nullptr;
	g_atomic_lock.clear(std::memory_order_release);
}

void CConectionPool::DisconnectConnectionHeap()
{
	for(auto& iconn : m_ConnList)
		DisconnectConnection(iconn);
}

// #####################################################
// INSERT SQL
// #####################################################
void CConectionPool::ID(const char *pTable, const char *pBuffer, ...)
{
	va_list Arguments;
	va_start(Arguments, pBuffer);
	InsertFormated(0, pTable, pBuffer, Arguments);
	va_end(Arguments);
}

void CConectionPool::IDS(int Milliseconds, const char *pTable, const char *pBuffer, ...)
{
	va_list Arguments;
	va_start(Arguments, pBuffer);
	InsertFormated(Milliseconds, pTable, pBuffer, Arguments);
	va_end(Arguments);
}

void CConectionPool::InsertFormated(int Milliseconds, const char *pTable, const char *pBuffer, va_list args)
{
	char aBuf[1024];
#if defined(CONF_FAMILY_WINDOWS)
	_vsnprintf(aBuf, sizeof(aBuf), pBuffer, args);
#else
	vsnprintf(aBuf, sizeof(aBuf), pBuffer, args);
#endif
	aBuf[sizeof(aBuf) - 1] = '\0';

	std::string Query("INSERT INTO " + std::string(pTable) + " " + std::string(aBuf) + ";");
	std::thread Thread([this, Query, Milliseconds]()
	{
		if(Milliseconds > 0)
			std::this_thread::sleep_for(std::chrono::milliseconds(Milliseconds));

		const char* pError = nullptr;

		SqlThreadRecursiveLock.lock();
		m_pDriver->threadInit();
		Connection* pConnection = SJK.GetConnection();
		try
		{
			std::unique_ptr<Statement> pStmt(pConnection->createStatement());
			pStmt->executeUpdate(Query.c_str());
			pStmt->close();
		}
		catch (SQLException & e)
		{
			pError = e.what();
		}
		SJK.ReleaseConnection(pConnection);
		m_pDriver->threadEnd();
		SqlThreadRecursiveLock.unlock();

		if(pError != nullptr)
			dbg_msg("SQL", "%s", pError);
	});
	Thread.detach();
}

// #####################################################
// UPDATE SQL
// #####################################################
void CConectionPool::UD(const char *pTable, const char *pBuffer, ...)
{
	va_list Arguments;
	va_start(Arguments, pBuffer);
	UpdateFormated(0, pTable, pBuffer, Arguments);
	va_end(Arguments);
}

void CConectionPool::UDS(int Milliseconds, const char *pTable, const char *pBuffer, ...)
{
	va_list Arguments;
	va_start(Arguments, pBuffer);
	UpdateFormated(Milliseconds, pTable, pBuffer, Arguments);
	va_end(Arguments);
}

void CConectionPool::UpdateFormated(int Milliseconds, const char *pTable, const char *pBuffer, va_list args)
{
	char aBuf[1024];
#if defined(CONF_FAMILY_WINDOWS)
	_vsnprintf(aBuf, sizeof(aBuf), pBuffer, args);
#else
	vsnprintf(aBuf, sizeof(aBuf), pBuffer, args);
#endif
	aBuf[sizeof(aBuf) - 1] = '\0';

	std::string Query("UPDATE " + std::string(pTable) + " SET " + std::string(aBuf) + ";");
	std::thread Thread([this, Query, Milliseconds]()
	{
		if(Milliseconds > 0)
			std::this_thread::sleep_for(std::chrono::milliseconds(Milliseconds));

		const char* pError = nullptr;

		SqlThreadRecursiveLock.lock();
		m_pDriver->threadInit();
		Connection* pConnection = SJK.GetConnection();
		try
		{
			std::unique_ptr<Statement> pStmt(pConnection->createStatement());
			pStmt->executeUpdate(Query.c_str());
			pStmt->close();
		}
		catch(SQLException& e)
		{
			pError = e.what();
		}
		SJK.ReleaseConnection(pConnection);
		m_pDriver->threadEnd();
		SqlThreadRecursiveLock.unlock();

		if(pError != nullptr)
			dbg_msg("SQL", "%s", pError);
	});
	Thread.detach();
}

// #####################################################
// DELETE SQL
// #####################################################
void CConectionPool::DD(const char *pTable, const char *pBuffer, ...)
{
	va_list Arguments;
	va_start(Arguments, pBuffer);
	DeleteFormated(0, pTable, pBuffer, Arguments);
	va_end(Arguments);
}

void CConectionPool::DDS(int Milliseconds, const char *pTable, const char *pBuffer, ...)
{
	va_list Arguments;
	va_start(Arguments, pBuffer);
	DeleteFormated(Milliseconds, pTable, pBuffer, Arguments);
	va_end(Arguments);
}

void CConectionPool::DeleteFormated(int Milliseconds, const char *pTable, const char *pBuffer, va_list args)
{
	char aBuf[256];
#if defined(CONF_FAMILY_WINDOWS)
	_vsnprintf(aBuf, sizeof(aBuf), pBuffer, args);
#else
	vsnprintf(aBuf, sizeof(aBuf), pBuffer, args);
#endif
	aBuf[sizeof(aBuf) - 1] = '\0';

	std::string Query("DELETE FROM " + std::string(pTable) + " " + std::string(aBuf) + ";");
	std::thread Thread([this, Query, Milliseconds]()
	{
		if(Milliseconds > 0)
			std::this_thread::sleep_for(std::chrono::milliseconds(Milliseconds));

		const char* pError = nullptr;

		SqlThreadRecursiveLock.lock();
		m_pDriver->threadInit();
		Connection* pConnection = SJK.GetConnection();
		try
		{
			std::unique_ptr<Statement> pStmt(pConnection->createStatement());
			pStmt->executeUpdate(Query.c_str());
			pStmt->close();
		}
		catch (SQLException& e)
		{
			pError = e.what();
		}
		SJK.ReleaseConnection(pConnection);
		m_pDriver->threadEnd();
		SqlThreadRecursiveLock.unlock();

		if(pError != nullptr)
			dbg_msg("SQL", "%s", pError);
	});
	Thread.detach();
}

// #####################################################
// SELECT SQL
// #####################################################
ResultPtr CConectionPool::ResultData::GetResult() const
{
	const char *pError = nullptr;

	SqlThreadRecursiveLock.lock();
	SJK.m_pDriver->threadInit();
	Connection *pConnection = SJK.GetConnection();
	ResultPtr pResult = nullptr;
	try
	{
		std::unique_ptr<Statement> pStmt(pConnection->createStatement());
		pResult.reset(pStmt->executeQuery(m_Query.c_str()));
		pStmt->close();
	}
	catch(SQLException &e)
	{
		pError = e.what();
	}
	SJK.ReleaseConnection(pConnection);
	SJK.m_pDriver->threadEnd();
	SqlThreadRecursiveLock.unlock();

	if(pError != nullptr)
		dbg_msg("SQL", "%s", pError);

	return pResult;
}

void CConectionPool::ResultData::OnCompletion(void (*pCallback)(IServer *, ResultPtr))
{
	auto Selected = [pCallback](const std::string Query)
	{
		const char *pError = nullptr;

		SqlThreadRecursiveLock.lock();
		SJK.m_pDriver->threadInit();
		Connection *pConnection = SJK.GetConnection();
		try
		{
			std::unique_ptr<Statement> pStmt(pConnection->createStatement());
			ResultPtr pResult(pStmt->executeQuery(Query.c_str()));
			pCallback(SJK.m_pServer, std::move(pResult));
			pStmt->close();
		}
		catch(SQLException &e)
		{
			pError = e.what();
		}
		SJK.ReleaseConnection(pConnection);
		SJK.m_pDriver->threadEnd();
		SqlThreadRecursiveLock.unlock();

		if(pError != nullptr)
			dbg_msg("SQL", "%s", pError);
	};

	std::thread(Selected, m_Query).detach();
}

CConectionPool::ResultData CConectionPool::SD(const char *Select, const char *Table, const char *Buffer, ...)
{
	char aBuf[1024];
	va_list VarArgs;
	va_start(VarArgs, Buffer);
#if defined(CONF_FAMILY_WINDOWS)
	_vsnprintf(aBuf, sizeof(aBuf), Buffer, VarArgs);
#else
	vsnprintf(aBuf, sizeof(aBuf), Buffer, VarArgs);
#endif
	va_end(VarArgs);
	aBuf[sizeof(aBuf) - 1] = '\0';

	ResultData Data;
	Data.m_Query = std::string("SELECT " + std::string(Select) + " FROM " + std::string(Table) + " " + std::string(aBuf) + ";");
	return Data;
}