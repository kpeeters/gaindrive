#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

// A fixed set of threads for blocking work: HTTP requests, file and keyring
// access. Jobs still queued at destruction are dropped; running ones are
// waited for, which is why requests carry short timeouts.
class WorkPool {
	public:
		explicit WorkPool(int threads);
		~WorkPool();

		void submit(std::function<void()> job);

	private:
		void run();

		std::mutex                         mtx_;
		std::condition_variable            cv_;
		std::deque<std::function<void()>>  jobs_;
		bool                               stop_=false;
		std::vector<std::thread>           threads_;
};
