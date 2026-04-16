#pragma once

#include <string>
#include <httplib.h>

#include "mediastore.hh"
#include "castmanager.hh"
#include "folderwatcher.hh"

class GainDrive {
	public:
		GainDrive(const std::string& db_path,
		          const std::string& music_root,
		          const std::string& upload_dir,
		          bool no_scan,
		          bool debug = false,
		          bool flat_multi_disc = true);
		void listen(const std::string& host, int port);

	private:
		bool            debug_;
		bool            flat_multi_disc_;
		std::string     upload_dir_;
		MediaStore      store_;
		CastManager     cast_manager_;
		FolderWatcher   watcher_;
		httplib::Server server_;
	};
