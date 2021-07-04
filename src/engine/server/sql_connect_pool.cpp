#include <mutex>
#include <thread>
#include <stdarg.h>
#include <mysql_connection.h>
#include <cppconn/driver.h>
#include <base/system.h>
#include <engine/shared/config.h>
#include "sql_connect_pool.h"

/*
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

	 @kurosio
*/
// sql pool connections mutex
std::mutex SqlConnectionLock;
// multithread mutex :: warning recursive
std::recursive_mutex SqlThreadRecursiveLock;

// #####################################################
// SQL CONNECTION POOL
// #####################################################
std::shared_ptr<CConnectionPool> CConnectionPool::m_Instance;
CConnectionPool::CConnectionPool()
{
	try
	{
		m_pDriver = get_driver_instance();

		SqlConnectionLock.lock();

		for(int i = 0; i < g_Config.m_SvMySqlPoolSize; ++i) 
			this->CreateConnection();

		SqlConnectionLock.unlock();
	}
	catch(SQLException &e) 
	{ 
		dbg_msg("sql", "%s", e.what());
		exit(0);
	}
}

CConnectionPool::~CConnectionPool()
{
	DisconnectConnectionHeap();
}

CConnectionPool& CConnectionPool::GetInstance()
{
	if (m_Instance.get() == nullptr)
		m_Instance.reset(new CConnectionPool());
	return *m_Instance.get();
}

Connection* CConnectionPool::CreateConnection()
{
	Connection *pConnection = nullptr;
	while(pConnection == nullptr)
	{
		try
		{
			pConnection = m_pDriver->connect(g_Config.m_SvMySqlHost, g_Config.m_SvMySqlLogin, g_Config.m_SvMySqlPassword);
			pConnection->setClientOption("OPT_CHARSET_NAME", "utf8mb4");
			pConnection->setSchema(g_Config.m_SvMySqlDatabase);
		}
		catch(SQLException &e) 
		{
			dbg_msg("sql", "%s", e.what());
			DisconnectConnection(pConnection);
		}
	}
	m_ConnList.push_back(pConnection);
	return pConnection;
}

Connection* CConnectionPool::GetConnection()
{
	SqlConnectionLock.lock();

	Connection* pConnection = nullptr;
	if(m_ConnList.empty())
	{
		pConnection = CreateConnection();

		SqlConnectionLock.unlock();
		return pConnection;
	}

	pConnection = m_ConnList.front();
	m_ConnList.pop_front();
	if(pConnection->isClosed())
	{
		delete pConnection;
		pConnection = nullptr;
		pConnection = CreateConnection();
	}

	SqlConnectionLock.unlock();
	return pConnection;
}

void CConnectionPool::ReleaseConnection(Connection* pConnection)
{
	SqlConnectionLock.lock();

	if(pConnection)
	{
		m_ConnList.push_back(pConnection);
	}

	SqlConnectionLock.unlock();
}

void CConnectionPool::DisconnectConnection(Connection* pConnection)
{
	if(pConnection)
	{
		try
		{
			pConnection->close();
		}
		catch(SQLException& e)
		{
			dbg_msg("sql", "%s", e.what());
		}
	}
	m_ConnList.remove(pConnection);
	delete pConnection;
	pConnection = nullptr;
}

void CConnectionPool::DisconnectConnectionHeap()
{
	SqlConnectionLock.lock();

	for(auto& iconn : m_ConnList)
		DisconnectConnection(iconn);
		
	m_ConnList.clear();

	SqlConnectionLock.unlock();
}

// #####################################################
// INSERT SQL
// #####################################################
void CConnectionPool::ID(const char *Table, const char *Buffer, ...)
{
	va_list Arguments;
	va_start(Arguments, Buffer);
	InsertFormated(0, Table, Buffer, Arguments);
	va_end(Arguments);
}

void CConnectionPool::IDS(int Milliseconds, const char *Table, const char *Buffer, ...)
{
	va_list Arguments;
	va_start(Arguments, Buffer);
	InsertFormated(Milliseconds, Table, Buffer, Arguments);
	va_end(Arguments);	
}

void CConnectionPool::InsertFormated(int Milliseconds, const char *Table, const char *Buffer, va_list args)
{
	char aBuf[1024];
	#if defined(CONF_FAMILY_WINDOWS)
	_vsnprintf(aBuf, sizeof(aBuf), Buffer, args);
	#else
	vsnprintf(aBuf, sizeof(aBuf), Buffer, args);
	#endif
	aBuf[sizeof(aBuf) - 1] = '\0';
	std::string Query("INSERT INTO " + std::string(Table) + " " + std::string(aBuf) + ";");
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
			dbg_msg("sql", "%s", pError);
	});
	Thread.detach();
}

// #####################################################
// UPDATE SQL
// #####################################################
void CConnectionPool::UD(const char *Table, const char *Buffer, ...)
{
	va_list Arguments;
	va_start(Arguments, Buffer);
	UpdateFormated(0, Table, Buffer, Arguments);
	va_end(Arguments);
}

void CConnectionPool::UDS(int Milliseconds, const char *Table, const char *Buffer, ...)
{
	va_list Arguments;
	va_start(Arguments, Buffer);
	UpdateFormated(Milliseconds, Table, Buffer, Arguments);
	va_end(Arguments);
}

void CConnectionPool::UpdateFormated(int Milliseconds, const char *Table, const char *Buffer, va_list args)
{
	char aBuf[1024];
	#if defined(CONF_FAMILY_WINDOWS)
	_vsnprintf(aBuf, sizeof(aBuf), Buffer, args);
	#else
	vsnprintf(aBuf, sizeof(aBuf), Buffer, args);
	#endif
	aBuf[sizeof(aBuf) - 1] = '\0';
	std::string Query("UPDATE " + std::string(Table) + " SET " + std::string(aBuf) + ";");
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
			dbg_msg("sql", "%s", pError);
	});
	Thread.detach();
}

// #####################################################
// DELETE SQL
// #####################################################
void CConnectionPool::DD(const char *Table, const char *Buffer, ...)
{
	va_list Arguments;
	va_start(Arguments, Buffer);
	DeleteFormated(0, Table, Buffer, Arguments);
	va_end(Arguments);
}

void CConnectionPool::DDS(int Milliseconds, const char *Table, const char *Buffer, ...)
{
	va_list Arguments;
	va_start(Arguments, Buffer);
	DeleteFormated(Milliseconds, Table, Buffer, Arguments);
	va_end(Arguments);
}

void CConnectionPool::DeleteFormated(int Milliseconds, const char *Table, const char *Buffer, va_list args)
{
	char aBuf[256];
	#if defined(CONF_FAMILY_WINDOWS)
	_vsnprintf(aBuf, sizeof(aBuf), Buffer, args);
	#else
	vsnprintf(aBuf, sizeof(aBuf), Buffer, args);
	#endif
	aBuf[sizeof(aBuf) - 1] = '\0';
	std::string Query("DELETE FROM " + std::string(Table) + " " + std::string(aBuf) + ";");
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
			dbg_msg("sql", "%s", pError);
	});
	Thread.detach();
}

/*
	I don't see the point in using SELECT operations in the thread, 
	since this will lead to unnecessary code, which may cause confusion, 
	and by calculations if (SQL server / server) = localhost, 
	this will not do any harm (but after the release is complete, 
	it is advisable to use the Thread function with Callback)
*/

// #####################################################
// SELECT SQL
// #####################################################
ResultPtr CConnectionPool::SSD(const char* Select, const char* Table, const char* Buffer, ...)
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

	const char* pError = nullptr;

	SqlThreadRecursiveLock.lock();
	std::string Query("SELECT " + std::string(Select) + " FROM " + std::string(Table) + " " + std::string(aBuf) + ";");
	m_pDriver->threadInit();
	Connection* pConnection = SJK.GetConnection();
	ResultPtr pResult = nullptr;
	try
	{
		std::unique_ptr<Statement> pStmt(pConnection->createStatement());
		pResult.reset(pStmt->executeQuery(Query.c_str()));
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
		dbg_msg("sql", "%s", pError);

	return pResult;
}

void CConnectionPool::ASD(std::function<void(ResultPtr)> func, const char *Select, const char *Table, const char *Buffer, ...)
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
	const std::string Query("SELECT " + std::string(Select) + " FROM " + std::string(Table) + " " + std::string(aBuf) + ";");
	std::thread Thread([this, Query, func]()
	{
		const char* pError = nullptr;

		SqlThreadRecursiveLock.lock();
		m_pDriver->threadInit();
		Connection* pConnection = SJK.GetConnection();
		try
		{
			std::unique_ptr<Statement> pStmt(pConnection->createStatement());
			ResultPtr pResult(pStmt->executeQuery(Query.c_str()));
			func(std::move(pResult));
		}
		catch(SQLException& e)
		{
			pError = e.what();
		}
		SJK.ReleaseConnection(pConnection);
		m_pDriver->threadEnd();
		SqlThreadRecursiveLock.unlock();

		if(pError != nullptr)
			dbg_msg("sql", "%s", pError);
	});
	Thread.detach();
}

void CConnectionPool::ASDS(int Milliseconds, std::function<void(ResultPtr)> func, const char *Select, const char *Table, const char *Buffer, ...)
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
	const std::string Query("SELECT " + std::string(Select) + " FROM " + std::string(Table) + " " + std::string(aBuf) + ";");
	std::thread Thread([this, Query, Milliseconds, func]() {
		if(Milliseconds > 0)
			std::this_thread::sleep_for(std::chrono::milliseconds(Milliseconds));

		const char *pError = nullptr;

		SqlThreadRecursiveLock.lock();
		m_pDriver->threadInit();
		Connection *pConnection = SJK.GetConnection();
		try
		{
			std::unique_ptr<Statement> pStmt(pConnection->createStatement());
			ResultPtr pResult(pStmt->executeQuery(Query.c_str()));
			func(std::move(pResult));
		}
		catch(SQLException &e)
		{
			pError = e.what();
		}
		SJK.ReleaseConnection(pConnection);
		m_pDriver->threadEnd();
		SqlThreadRecursiveLock.unlock();

		if(pError != nullptr)
			dbg_msg("sql", "%s", pError);
	});
	Thread.detach();
}

void CConnectionPool::SPS(const char *Q, int AN, SSqlArg *A)
{
	const char *pError = nullptr;

	SqlThreadRecursiveLock.lock();
	m_pDriver->threadInit();
	Connection *pConnection = SJK.GetConnection();
	ResultPtr pResult = nullptr;
	PreparedStatement* ps;
	
	try
	{
		ps = pConnection->prepareStatement(Q);
		for(int index = 0; index < AN; ++index)
		{
			switch((A + index)->Type())
			{
			case SQL_TYPE_STRING:
			{
				ps->setString(index + 1, (const char *)(((SSqlString *)(A + index))->Get()));
				dbg_msg("sql", "here str");
				break;
			}
			case SQL_TYPE_BLOB:
			{
				ps->setBlob(index + 1, (std::istream *)((SSqlBlob *)(A + index)->Get()));
				dbg_msg("sql", "here blob");
				break;
			}
			case SQL_TYPE_INT:
			{
				ps->setInt(index + 1, (int)((size_t)(A + index)->Get())); // warning C4311: 'type cast': pointer truncation from 'void *' to 'int'
				break;
			}
			default:
				dbg_msg("sql", "here ????");
				break;
			}
		}

		ps->execute();

		delete ps;
	}
	catch(SQLException &e)
	{
		pError = e.what();
	}

	SJK.ReleaseConnection(pConnection);
	m_pDriver->threadEnd();
	SqlThreadRecursiveLock.unlock();

	if(pError != nullptr)
		dbg_msg("sql", "%s", pError);
}
