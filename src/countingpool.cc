#include "countingpool.hh"

CountingPool::CountingPool(size_t threads, size_t max_threads,
                           size_t queue_cap)
	: pool_(threads, max_threads, queue_cap),
	  threads_(static_cast<int>(threads)),
	  queue_cap_(static_cast<int>(queue_cap))
	{
	}

bool CountingPool::enqueue(std::function<void()> fn)
	{
	// Bumped before the inner enqueue: a worker may pick the job up before
	// enqueue() returns, and its decrement must not land on a count of zero.
	queued_.fetch_add(1);
	bool ok = pool_.enqueue([this, fn = std::move(fn)] {
		queued_.fetch_sub(1);
		busy_.fetch_add(1);
		fn();
		busy_.fetch_sub(1);
		});
	if (!ok) queued_.fetch_sub(1);   // refused at the bounded queue
	return ok;
	}

void CountingPool::shutdown()
	{
	pool_.shutdown();
	}
