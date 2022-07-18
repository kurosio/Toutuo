#ifndef ENGINE_SERVER_SQL_CONNECT_POOL_H
#define ENGINE_SERVER_SQL_CONNECT_POOL_H

#include <cppconn/driver.h>
#include <cppconn/statement.h>
#include <cppconn/resultset.h>

#include <cstdarg>
#include <thread>
#include <mutex>

using namespace sql;
#define Sqlpool CConectionPool::GetInstance()
typedef std::unique_ptr<ResultSet> ResultPtr;
inline std::recursive_mutex g_SqlThreadRecursiveLock;


enum class TypeDB : size_t
{
	Select = 0,
	Insert,
	Update,
	Delete,
	Custom,
};

class CConectionPool
{
	CConectionPool();

	static std::shared_ptr<CConectionPool> m_Instance;
	class IServer *m_pServer;

	std::list<Connection*>m_ConnList;
	Driver *m_pDriver;

public:
	~CConectionPool();
	void Init(IServer *pServer) { m_pServer = pServer;  }

	Connection* GetConnection();
	Connection* CreateConnection();
	void ReleaseConnection(Connection* pConnection);
	void DisconnectConnection(Connection* pConnection);
	void DisconnectConnectionHeap();
	static CConectionPool& GetInstance();

	// database extraction function
private:
	class CResultBase
	{
	protected:
		friend class CConectionPool;
		std::string m_Query;
	public:
		const char *GetQuery() const { return m_Query.c_str(); }
	};

	class CResultSelect : public CResultBase
	{
	public:
		[[nodiscard]] ResultPtr GetResult() const
		{
			const char *pError = nullptr;

			g_SqlThreadRecursiveLock.lock();
			Sqlpool.m_pDriver->threadInit();
			Connection *pConnection = Sqlpool.GetConnection();
			ResultPtr pResult = nullptr;
			try
			{
				const std::unique_ptr<Statement> pStmt(pConnection->createStatement());
				pResult.reset(pStmt->executeQuery(m_Query.c_str()));
				pStmt->close();
			}
			catch(SQLException &e)
			{
				pError = e.what();
			}
			Sqlpool.ReleaseConnection(pConnection);
			Sqlpool.m_pDriver->threadEnd();
			g_SqlThreadRecursiveLock.unlock();

			if(pError != nullptr)
				dbg_msg("SQL", "%s", pError);

			return pResult;
		}

		void AtExecution(void (*pCallback)(IServer *, ResultPtr) = nullptr)
		{
			auto Item = [pCallback](const std::string Query)
			{
				const char *pError = nullptr;

				g_SqlThreadRecursiveLock.lock();
				Sqlpool.m_pDriver->threadInit();
				Connection *pConnection = Sqlpool.GetConnection();
				try
				{
					const std::unique_ptr<Statement> pStmt(pConnection->createStatement());
					ResultPtr pResult(pStmt->executeQuery(Query.c_str()));
					if(pCallback)
						pCallback(Sqlpool.m_pServer, std::move(pResult));
					pStmt->close();
				}
				catch(SQLException &e)
				{
					pError = e.what();
				}
				Sqlpool.ReleaseConnection(pConnection);
				Sqlpool.m_pDriver->threadEnd();
				g_SqlThreadRecursiveLock.unlock();

				if(pError != nullptr)
					dbg_msg("SQL", "%s", pError);
			};
			std::thread(Item, m_Query).detach();
		}
	};

	class CResultQuery : public CResultBase
	{
	public:
		void AtExecution(void (*pCallback)(IServer *) = nullptr)
		{
			auto Item = [pCallback](const std::string Query) {
				//if(Milliseconds > 0)
				//	std::this_thread::sleep_for(std::chrono::milliseconds(Milliseconds));

				const char *pError = nullptr;

				g_SqlThreadRecursiveLock.lock();
				Sqlpool.m_pDriver->threadInit();
				Connection *pConnection = Sqlpool.GetConnection();
				try
				{
					const std::unique_ptr<Statement> pStmt(pConnection->createStatement());
					pStmt->execute(Query.c_str());
					if(pCallback)
						pCallback(Sqlpool.m_pServer);
					pStmt->close();
				}
				catch(SQLException &e)
				{
					pError = e.what();
				}
				Sqlpool.ReleaseConnection(pConnection);
				Sqlpool.m_pDriver->threadEnd();
				g_SqlThreadRecursiveLock.unlock();

				if(pError != nullptr)
					dbg_msg("SQL", "%s", pError);
			};
			std::thread(Item, m_Query).detach();
		}
		void Execute() { return AtExecution(nullptr); }
	};

	class CResultQueryCustom : public CResultQuery
	{
	public:
		CResultQueryCustom &UpdateQuery(const char *pBuffer, ...)
		{
			char aBuf[1024];
			va_list VarArgs;
			va_start(VarArgs, pBuffer);
			vaformatsql(pBuffer, aBuf, sizeof(aBuf), VarArgs);
			va_end(VarArgs);

			m_Query = std::string(std::string(aBuf) + ";");
			return *this;
		}
	};


	static void vaformatsql(const char *pBuffer, char *pBuf, size_t Size, va_list VarArgs)
	{
#if defined(CONF_FAMILY_WINDOWS)
		_vsnprintf(pBuf, Size, pBuffer, VarArgs);
#else
		vsnprintf(pBuf, Size, pBuffer, VarArgs);
#endif
		pBuf[Size - 1] = '\0';
	}

public:
	template<TypeDB T>
	static std::enable_if_t<T == TypeDB::Select, CResultSelect> Prepare(const char *pSelect, const char *pTable, const char *pBuffer = "\0", ...)
	{
		char aBuf[1024];
		va_list VarArgs;
		va_start(VarArgs, pBuffer);
		vaformatsql(pBuffer, aBuf, sizeof(aBuf), VarArgs);
		va_end(VarArgs);

		CResultSelect Data;
		Data.m_Query = std::string("SELECT " + std::string(pSelect) + " FROM " + std::string(pTable) + " " + std::string(aBuf) + ";");
		return Data;
	}

	template<TypeDB T>
	static std::enable_if_t<T == TypeDB::Custom, CResultQueryCustom> Prepare(const char *pBuffer, ...)
	{
		char aBuf[1024];
		va_list VarArgs;
		va_start(VarArgs, pBuffer);
		vaformatsql(pBuffer, aBuf, sizeof(aBuf), VarArgs);
		va_end(VarArgs);

		CResultQueryCustom Data;
		Data.m_Query = std::string(std::string(aBuf) + ";");
		return Data;
	}

	template<TypeDB T>
	static std::enable_if_t<T != TypeDB::Select && T != TypeDB::Custom, CResultQuery> Prepare(const char *pTable, const char *pBuffer, ...)
	{
		char aBuf[1024];
		va_list VarArgs;
		va_start(VarArgs, pBuffer);
		vaformatsql(pBuffer, aBuf, sizeof(aBuf), VarArgs);
		va_end(VarArgs);

		CResultQuery Data;
		if constexpr(T == TypeDB::Insert)
			Data.m_Query = std::string("INSERT INTO " + std::string(pTable) + " " + std::string(aBuf) + ";");
		else if constexpr(T == TypeDB::Update)
			Data.m_Query = std::string("UPDATE " + std::string(pTable) + " SET " + std::string(aBuf) + ";");
		else if constexpr(T == TypeDB::Delete)
			Data.m_Query = std::string("DELETE FROM " + std::string(pTable) + " " + std::string(aBuf) + ";");
		return Data;
	}

};

#endif
