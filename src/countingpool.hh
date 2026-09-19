#pragma once

#include <atomic>
#include <functional>

#include <httplib.h>

// httplib's ThreadPool is final and keeps its busy and queued counts
// private, so this wraps one: every job is passed through with a lambda
// around it that maintains the two counters getServerStatus reports.
// Pool semantics, including the bounded-queue refusal, are untouched.
class CountingPool : public httplib::TaskQueue
	{
	public:
		CountingPool(size_t threads, size_t max_threads, size_t queue_cap);
		bool enqueue(std::function<void()> fn) override;
		void shutdown() override;

		int busy()      const { return busy_.load(); }
		int queued()    const { return queued_.load(); }
		int threads()   const { return threads_; }
		int queue_cap() const { return queue_cap_; }

	private:
		httplib::ThreadPool pool_;
		std::atomic<int>    busy_{0};
		std::atomic<int>    queued_{0};
		int                 threads_;
		int                 queue_cap_;
	};
