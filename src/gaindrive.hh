#pragma once

#include <string>
#include <httplib.h>

#include "mediastore.hh"

class GainDrive {
	public:
		GainDrive(const std::string& db_path,
		          const std::string& music_root,
		          bool no_scan,
		          bool debug = false);
		void listen(const std::string& host, int port);

	private:
		bool            debug_;
		MediaStore      store_;
		httplib::Server server_;
	};
