#include "workpool.hh"
#include "stamp.hh"

#include <exception>
#include <iostream>

WorkPool::WorkPool(int threads)
	{
	for(int i=0; i<threads; ++i)
		threads_.emplace_back(&WorkPool::run, this);
	}

WorkPool::~WorkPool()
	{
	{
	std::lock_guard<std::mutex> lock(mtx_);
	stop_=true;
	jobs_.clear();
	}
	cv_.notify_all();
	for(auto& t: threads_)
		t.join();
	}

void WorkPool::submit(std::function<void()> job)
	{
	{
	std::lock_guard<std::mutex> lock(mtx_);
	if(stop_) return;
	jobs_.push_back(std::move(job));
	}
	cv_.notify_one();
	}

void WorkPool::run()
	{
	for(;;) {
		std::function<void()> job;
		{
		std::unique_lock<std::mutex> lock(mtx_);
		cv_.wait(lock, [this]{ return stop_ || !jobs_.empty(); });
		if(stop_) return;
		job = std::move(jobs_.front());
		jobs_.pop_front();
		}
		// An exception escaping a thread is std::terminate; one failed job
		// must not take the player down.
		try {
			job();
			}
		catch(const std::exception& ex) {
			std::cout << stamp() << "WorkPool: job failed: " << ex.what() << std::endl;
			}
		catch(...) {
			std::cout << stamp() << "WorkPool: job failed with a non-standard exception" << std::endl;
			}
		}
	}
