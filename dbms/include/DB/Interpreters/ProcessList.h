#pragma once

#include <map>
#include <list>
#include <Poco/SharedPtr.h>
#include <Poco/Mutex.h>
#include <Poco/Condition.h>
#include <Poco/Net/IPAddress.h>
#include <statdaemons/Stopwatch.h>
#include <DB/Core/Defines.h>
#include <DB/Core/Exception.h>
#include <DB/Core/ErrorCodes.h>
#include <DB/IO/WriteHelpers.h>


namespace DB
{

/** Список исполняющихся в данный момент запросов.
  * Также реализует ограничение на их количество.
  */

class ProcessList
{
	friend class Entry;
public:
	/// Запрос и данные о его выполнении.
	struct Element
	{
		String query;
		String user;
		String query_id;
		Poco::Net::IPAddress ip_address;
		
		Stopwatch watch;

		volatile size_t rows_processed;
		volatile size_t bytes_processed;

		bool is_cancelled;
		

		Element(const String & query_, const String & user_, const String & query_id_, const Poco::Net::IPAddress & ip_address_)
			: query(query_), user(user_), query_id(query_id_), ip_address(ip_address_), rows_processed(0), bytes_processed(0),
			is_cancelled(false) {}

		bool update(size_t rows, size_t bytes) volatile
		{
			__sync_add_and_fetch(&rows_processed, rows);
			__sync_add_and_fetch(&bytes_processed, bytes);
			return !is_cancelled;
		}
	};

	/// list, чтобы итераторы не инвалидировались. NOTE: можно заменить на cyclic buffer, но почти незачем.
	typedef std::list<Element> Containter;
	/// Query_id -> Element *
	typedef std::unordered_map<String, Element *> QueryToElement;
	/// User -> Query_id -> Element *
	typedef std::unordered_map<String, QueryToElement> UserToQueries;

private:
	mutable Poco::FastMutex mutex;
	mutable Poco::Condition have_space;		/// Количество одновременно выполняющихся запросов стало меньше максимального.
	
	Containter cont;
	size_t cur_size;		/// В C++03 std::list::size не O(1).
	size_t max_size;		/// Если 0 - не ограничено. Иначе, если пытаемся добавить больше - кидается исключение.
	UserToQueries user_to_queries;

	/// Держит итератор на список, и удаляет элемент из списка в деструкторе.
	class Entry
	{
	private:
		ProcessList & parent;
		Containter::iterator it;
	public:
		Entry(ProcessList & parent_, Containter::iterator it_)
			: parent(parent_), it(it_) {}

		~Entry()
		{
			Poco::ScopedLock<Poco::FastMutex> lock(parent.mutex);
			parent.cont.erase(it);
			--parent.cur_size;
			parent.have_space.signal();
			UserToQueries::iterator queries = parent.user_to_queries.find(it->user);
			if (queries != parent.user_to_queries.end())
			{
				QueryToElement::iterator element = queries->second.find(it->query_id);
				if (element != queries->second.end())
					queries->second.erase(element);
			}
		}

		Element & get() { return *it; }
		const Element & get() const { return *it; }
	};

public:
	ProcessList(size_t max_size_ = 0) : cur_size(0), max_size(max_size_) {}

	typedef Poco::SharedPtr<Entry> EntryPtr;

	/** Зарегистрировать выполняющийся запрос. Возвращает refcounted объект, который удаляет запрос из списка при уничтожении.
	  * Если выполняющихся запросов сейчас слишком много - ждать не более указанного времени.
	  * Если времени не хватило - кинуть исключение.
	  */
	EntryPtr insert(const String & query_, const String & user_, const String & query_id_, const Poco::Net::IPAddress & ip_address_,
		size_t max_wait_milliseconds = DEFAULT_QUERIES_QUEUE_WAIT_TIME_MS, bool replace_running_query = false)
	{
		EntryPtr res;

		{
			Poco::ScopedLock<Poco::FastMutex> lock(mutex);

			if (max_size && cur_size >= max_size && (!max_wait_milliseconds || !have_space.tryWait(mutex, max_wait_milliseconds)))
				throw Exception("Too much simultaneous queries. Maximum: " + toString(max_size), ErrorCodes::TOO_MUCH_SIMULTANEOUS_QUERIES);

			UserToQueries::iterator queries = user_to_queries.find(user_);

			if (queries != user_to_queries.end())
			{
				QueryToElement::iterator element = queries->second.find(query_id_);
				if (element != queries->second.end())
				{
					if (!replace_running_query)
						throw Exception("Query with id = " + query_id_ + " is already running.",
										ErrorCodes::QUERY_ID_ALREADY_RUNNING);
					element->second->is_cancelled = true;
					queries->second.erase(element);
				}
			}

			++cur_size;

			res = new Entry(*this, cont.insert(cont.end(), Element(query_, user_, query_id_, ip_address_)));

			user_to_queries[user_][query_id_] = &res->get();
		}

		return res;
	}

	/// Количество одновременно выполняющихся запросов.
	size_t size() const
	{
		Poco::ScopedLock<Poco::FastMutex> lock(mutex);
		return cur_size;
	}

	/// Получить текущее состояние (копию) списка запросов.
	Containter get() const
	{
		Containter res;

		{
			Poco::ScopedLock<Poco::FastMutex> lock(mutex);
			res = cont;
		}

		return res;
	}

	void setMaxSize(size_t max_size_)
	{
		Poco::ScopedLock<Poco::FastMutex> lock(mutex);
		max_size = max_size_;
	}
};

}
