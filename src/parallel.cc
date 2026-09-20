#include "parallel.hh"

#include <algorithm>
#include <atomic>
#include <exception>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>
#include "stamp.hh"

void parallel_for(std::size_t n, int jobs,
                  const std::function<void(std::size_t)>& body)
	{
	if (n == 0) return;

	// One job means no thread, and on the caller's own stack: --scan-jobs 1 has
	// to reproduce the sequential behaviour exactly, or it is no use as an
	// answer to "is this bug the parallelism?".
	if (jobs <= 1 || n == 1) {
		for (std::size_t i = 0; i < n; ++i) body(i);
		return;
		}

	std::atomic<std::size_t> next{0};
	std::atomic<bool>        failed{false};
	std::mutex               err_mu;
	std::exception_ptr       err;

	auto worker = [&] {
		while (!failed.load(std::memory_order_relaxed)) {
			std::size_t i = next.fetch_add(1, std::memory_order_relaxed);
			if (i >= n) return;
			try { body(i); }
			catch (...) {
				std::lock_guard<std::mutex> lock(err_mu);
				if (!err) err = std::current_exception();
				failed.store(true, std::memory_order_relaxed);
				return;
				}
			}
		};

	// One fewer thread than the width, because the caller runs a share too.
	// Otherwise it sits blocked in join() while a core goes unused, which for a
	// live rescan of three files is the whole of the work.
	std::size_t width = std::min<std::size_t>(n, static_cast<std::size_t>(jobs));
	std::vector<std::thread> pool;
	pool.reserve(width - 1);
	for (std::size_t t = 1; t < width; ++t) pool.emplace_back(worker);
	worker();
	for (auto& t : pool) t.join();

	if (err) std::rethrow_exception(err);
	}

void log_line(const std::string& text)
	{
	static std::mutex mu;
	std::lock_guard<std::mutex> lock(mu);
	std::cout << stamp() << text << std::endl;
	}
