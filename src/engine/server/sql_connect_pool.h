#ifndef ENGINE_SERVER_SQL_CONNECTIONPOOL_H
#define ENGINE_SERVER_SQL_CONNECTIONPOOL_H

#include <boost/scoped_ptr.hpp>
#include <cppconn/statement.h>
#include <cppconn/prepared_statement.h>
#include <functional>

using namespace sql;
#define SJK CConnectionPool::GetInstance()
typedef std::unique_ptr<ResultSet> ResultPtr;


// shittiest way to work with but

enum
{
	SQL_TYPE_STRING,
	SQL_TYPE_BLOB,
	SQL_TYPE_INT
};

struct SSqlArg
{
	int _Type;
	void *_Data;
	virtual int Type() const { return _Type; }
	virtual void *Get() const { return _Data; }
};

struct SSqlString : public SSqlArg
{
	SSqlString(const char *S) { _Data = (void *)S; _Type = SQL_TYPE_STRING; }
};

struct SSqlBlob : public SSqlArg
{
	SSqlBlob(std::istream *S) { _Data = (void *)S; _Type = SQL_TYPE_BLOB; }
};

class CConnectionPool 
{
	CConnectionPool();
	
	static std::shared_ptr<CConnectionPool> m_Instance;

	std::list<class Connection*>m_ConnList;
	class Driver *m_pDriver;

	void InsertFormated(int Milliseconds, const char *Table, const char *Buffer, va_list args);
	void UpdateFormated(int Milliseconds, const char *Table, const char *Buffer, va_list args);
	void DeleteFormated(int Milliseconds, const char *Table, const char *Buffer, va_list args);

public:
	~CConnectionPool();

	class Connection* GetConnection();
	class Connection* CreateConnection();
	void ReleaseConnection(class Connection* pConnection);
	void DisconnectConnection(class Connection* pConnection);
	void DisconnectConnectionHeap();
	static CConnectionPool& GetInstance();

	// simply inserts data
	void ID(const char *Table, const char *Buffer, ...);
	void IDS(int Milliseconds, const char *Table, const char *Buffer, ...);
	
	// simply update the data that will be specified
	void UD(const char *Table, const char *Buffer, ...);
	void UDS(int Milliseconds, const char *Table, const char *Buffer, ...);
	
	// simply deletes the data that will be specified
	void DD(const char *Table, const char *Buffer, ...);
	void DDS(int Milliseconds, const char *Table, const char *Buffer, ...);

	// Synchronous SELECT data
	ResultPtr SSD(const char *Select, const char *Table, const char *Buffer = "", ...);
	// Asynchronous SELECT data
	void ASD(std::function<void(ResultPtr)> func, const char *Select, const char *Table, const char *Buffer = "", ...);
	// Asynchronous SELECT data suspended
	void ASDS(int Milliseconds, std::function<void(ResultPtr)> func, const char *Select, const char *Table, const char *Buffer = "", ...);

	// std::unique_ptr<PreparedStatement> SPS(const char *Q); // wtf is that, do you really want to use it
	void SPS(const char *Q, int AN, SSqlArg* A); // wtf is that, do you really want to use it
};

#endif
