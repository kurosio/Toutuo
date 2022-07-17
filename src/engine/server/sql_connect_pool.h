#ifndef ENGINE_SERVER_SQL_CONNECT_POOL_H
#define ENGINE_SERVER_SQL_CONNECT_POOL_H

#include <cppconn/resultset.h>

using namespace sql;
#define SJK CConectionPool::GetInstance()
typedef std::unique_ptr<ResultSet> ResultPtr;

class CConectionPool
{
	CConectionPool();

	static std::shared_ptr<CConectionPool> m_Instance;
	class IServer *m_pServer;

	std::list<class Connection*>m_ConnList;
	class Driver *m_pDriver;

	void InsertFormated(int Milliseconds, const char *pTable, const char *Buffer, va_list args);
	void UpdateFormated(int Milliseconds, const char *pTable, const char *Buffer, va_list args);
	void DeleteFormated(int Milliseconds, const char *pTable, const char *Buffer, va_list args);

public:
	~CConectionPool();
	void Init(IServer *pServer)
	{
		m_pServer = pServer;
	};

	class Connection* GetConnection();
	class Connection* CreateConnection();
	void ReleaseConnection(class Connection* pConnection);
	void DisconnectConnection(class Connection* pConnection);
	void DisconnectConnectionHeap();
	static CConectionPool& GetInstance();

	// simply inserts data
	void ID(const char *pTable, const char *pBuffer, ...);
	void IDS(int Milliseconds, const char *pTable, const char *pBuffer, ...);

	// simply update the data that will be specified
	void UD(const char *pTable, const char *pBuffer, ...);
	void UDS(int Milliseconds, const char *pTable, const char *pBuffer, ...);

	// simply deletes the data that will be specified
	void DD(const char *pTable, const char *pBuffer, ...);
	void DDS(int Milliseconds, const char *pTable, const char *pBuffer, ...);

	// database extraction function
	class ResultData
	{
		friend class CConectionPool;
		std::string m_Query;

	public:
		[[nodiscard]] ResultPtr GetResult() const;
		void OnCompletion(void (*pCallback)(IServer *, ResultPtr));
	};
	ResultData SD(const char *pSelect, const char *pTable, const char *Buffer = "", ...);
};

#endif
