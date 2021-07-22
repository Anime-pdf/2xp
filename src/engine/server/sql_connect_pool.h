#ifndef ENGINE_SERVER_SQL_CONNECTIONPOOL_H
#define ENGINE_SERVER_SQL_CONNECTIONPOOL_H

#include <boost/scoped_ptr.hpp>
#include <cppconn/statement.h>
#include <cppconn/prepared_statement.h>

#include <base/tl/array.h>

#include <functional>
#include <map>
#include <any>

using namespace sql;
#define SJK CConnectionPool::GetInstance()
typedef std::unique_ptr<ResultSet> ResultPtr;

template <typename T, typename R>
struct SPseudoMap
{
	SPseudoMap(int Size)
	{
		Keys.set_size(Size);
		Values.set_size(Size);
	}

	void Add(T Key, R Value)
	{
		Keys.add(Key);
		Values.add(Value);
	}

	T GetKey(int Index) const
	{
		return Keys[Index];
	}

	R GetValue(int Index) const
	{
		return Values[Index];
	}

	int Size() const
	{
		return Keys.size();
	}

	array<T> Keys;
	array<R> Values;
};

typedef SPseudoMap<std::string, std::any> SqlArgs;

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

	void SPS(const char *T, SqlArgs A); // wtf is that, do you really want to use it
};

#endif
