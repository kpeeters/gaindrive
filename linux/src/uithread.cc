#include "uithread.hh"

#include <glibmm/dispatcher.h>

#include <deque>
#include <memory>
#include <mutex>

namespace {
	std::mutex                         mtx;
	std::deque<std::function<void()>>  queue;
	bool                               closed = false;
	// A Dispatcher delivers to the main context of the thread that made it,
	// hence ui_init() rather than a static.
	std::unique_ptr<Glib::Dispatcher>  dispatcher;

	void drain()
		{
		std::deque<std::function<void()>> todo;
		{
		std::lock_guard<std::mutex> lock(mtx);
		todo.swap(queue);
		}
		for(auto& fn: todo) {
			// ui_close() may be called by one of these.
			{
			std::lock_guard<std::mutex> lock(mtx);
			if(closed) return;
			}
			fn();
			}
		}
}

void ui_init()
	{
	dispatcher = std::make_unique<Glib::Dispatcher>();
	dispatcher->connect(&drain);
	}

void on_ui(std::function<void()> fn)
	{
	{
	std::lock_guard<std::mutex> lock(mtx);
	if(closed) return;
	queue.push_back(std::move(fn));
	}
	// One emit per post; drain() empties the whole queue, so surplus wakeups
	// find nothing and cost nothing.
	dispatcher->emit();
	}

void ui_close()
	{
	std::lock_guard<std::mutex> lock(mtx);
	closed = true;
	queue.clear();
	}
