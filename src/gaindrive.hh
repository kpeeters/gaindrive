#pragma once

#include <string>
#include <httplib.h>

class GainDrive {
	public:
		GainDrive();
		void listen(const std::string& host, int port);

	private:
		httplib::Server server_;
	};
