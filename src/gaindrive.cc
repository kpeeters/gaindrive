#include "gaindrive.hh"

#include <iostream>

GainDrive::GainDrive()
	{
	// Catch-all handler for /rest/* — individual endpoints come later.
	server_.Get("/rest/:endpoint", [](const httplib::Request&, httplib::Response& res) {
		res.set_content(
			R"(<?xml version="1.0" encoding="UTF-8"?>)"
			R"(<subsonic-response xmlns="http://subsonic.org/restapi" status="failed" version="1.16.1">)"
			R"(<error code="0" message="not implemented"/>)"
			R"(</subsonic-response>)",
			"application/xml");
		});
	}

void GainDrive::listen(const std::string& host, int port)
	{
	std::cout << "Listening on " << host << ":" << port << "\n";
	server_.listen(host, port);
	}
