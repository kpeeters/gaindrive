#include "gaindrive.hh"
#include "stamp.hh"

#include <iostream>
#include <thread>

GainDrive::GainDrive(const std::string& db_path,
                     const std::string& music_root,
                     bool no_scan)
	: store_(db_path, music_root)
	{
	server_.set_logger([](const httplib::Request& req, const httplib::Response& res) {
		std::cout << stamp(req.remote_addr)
		          << req.method << " " << req.path
		          << " -> " << res.status << std::endl;
		});

	// Catch-all handler for /rest/* — individual endpoints come later.
	server_.Get("/rest/:endpoint", [](const httplib::Request&, httplib::Response& res) {
		res.set_content(
			R"(<?xml version="1.0" encoding="UTF-8"?>)"
			R"(<subsonic-response xmlns="http://subsonic.org/restapi" status="failed" version="1.16.1">)"
			R"(<error code="0" message="not implemented"/>)"
			R"(</subsonic-response>)",
			"application/xml");
		});

	if (!no_scan) {
		// Scan in background so the server starts accepting requests immediately.
		std::thread([this]{ store_.scan(); }).detach();
		}
	}

void GainDrive::listen(const std::string& host, int port)
	{
	std::cout << stamp() << "Listening on " << host << ":" << port << std::endl;
	server_.listen(host, port);
	}
