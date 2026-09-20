#include "gaindrive.hh"
#include "subsonic.hh"
#include "authz.hh"
#include "stamp.hh"
#include "textutil.hh"

#include <iostream>

#include <tinyxml2.h>
#include <nlohmann/json.hpp>

using namespace tinyxml2;

void GainDrive::routes_fallback()
	{
	// Catch-all for endpoints not yet implemented.

	server_.Get("/rest/:endpoint", [](const httplib::Request& req, httplib::Response& res) {
		// log_safe: this is the one unauthenticated route that echoed the
		// percent-decoded path into the log verbatim.
		std::cout << stamp() << "NOT IMPLEMENTED: " << log_safe(req.path) << std::endl;
		res.set_content(subsonic_error(0, "Not implemented."), "application/xml");
		});
	}
