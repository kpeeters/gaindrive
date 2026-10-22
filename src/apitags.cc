#include "gaindrive.hh"
#include "subsonic.hh"
#include "authz.hh"
#include "netaddr.hh"
#include "stamp.hh"
#include "textutil.hh"
#include "visibility.hh"

#include <iostream>
#include <optional>

#include <nlohmann/json.hpp>

// Padding is never part of a name: "Jazz " and "Jazz" would otherwise be two
// tags that look the same in every list.
static std::string trim(const std::string& s)
	{
	const auto b = s.find_first_not_of(" \t\r\n");
	if (b == std::string::npos) return {};
	const auto e = s.find_last_not_of(" \t\r\n");
	return s.substr(b, e - b + 1);
	}

// Tags on level-1 folders, which decide who sees what; see visibility.hh.
//
// The first half is the admin's, the second every user's own. JSON only,
// like getServerSettings: these are gaindrive extensions no third-party client
// knows to ask for, and gaindrive's own clients all speak JSON. A folder is named by its folder id, which is what a
// client has in hand, and reported back with its path as well, since a tag can
// outlive the folder it was put on.
void GainDrive::routes_tags()
	{
	// Every admin handler below opens the same way. Answers false having
	// already written the refusal.
	auto admin_only = [this](const httplib::Request& req, httplib::Response& res) {
		if (!check_auth(req, res, store_)) return false;
		auto ui = store_.get_user(req.get_param_value("u"));
		if (ui && ui->is_admin) return true;
		res.set_content(subsonic_error_json(50, "User is not authorized for this operation."),
		                "application/json");
		return false;
		};
	auto err = [](httplib::Response& res, int code, const char* msg) {
		res.set_content(subsonic_error_json(code, msg), "application/json");
		};
	auto ok = [](httplib::Response& res) {
		res.set_content(subsonic_ok_json(), "application/json");
		};

	// getTags - every tag with its folders, and every grid cell that has
	// been set. A cell that is absent is the tag's defaultState.
	server_.Get("/rest/getTags.view", [this, admin_only](const httplib::Request& req,
	                                                     httplib::Response& res) {
		if (!admin_only(req, res)) return;
		auto tags  = store_.list_tags();
		auto cells = store_.list_tag_cells();

		nlohmann::json tag_arr = nlohmann::json::array();
		for (const auto& t : tags) {
			nlohmann::json folders = nlohmann::json::array();
			for (const auto& p : t.folders) {
				nlohmann::json f = {{"path", p}};
				if (auto id = store_.folder_id_by_path(p)) f["id"] = sid(*id);
				folders.push_back(std::move(f));
				}
			nlohmann::json e = {{"id", t.id}, {"name", t.name}, {"folder", folders}};
			if (!t.owner.empty())         e["owner"]        = t.owner;
			if (!t.default_state.empty()) e["defaultState"] = t.default_state;
			if (!t.personal_of.empty())   e["personalOf"]   = t.personal_of;
			tag_arr.push_back(std::move(e));
			}
		nlohmann::json cell_arr = nlohmann::json::array();
		for (const auto& c : cells)
			cell_arr.push_back({{"username", c.username}, {"tagId", c.tag_id},
			                    {"state", c.state}});

		res.set_content(subsonic_ok_json([&](nlohmann::json& r) {
			r["tags"] = {{"tag", tag_arr}, {"cell", cell_arr}};
			}), "application/json");
		});

	// createTag(name, defaultState)
	server_.Get("/rest/createTag.view", [this, admin_only, err](const httplib::Request& req,
	                                                            httplib::Response& res) {
		if (!admin_only(req, res)) return;
		const std::string name = trim(req.get_param_value("name"));
		const std::string def  = req.get_param_value("defaultState");
		if (name.empty()) { err(res, 10, "Required parameter missing: name."); return; }
		if (!Visibility::parse_cell(def)) {
			err(res, 10, "defaultState must be blocked, shown, choice_on or choice_off.");
			return;
			}
		auto id = store_.create_tag(name, def);
		if (!id) { err(res, 0, "A tag of that name exists."); return; }
		std::cout << stamp(client_addr(req)) << "Tag created: " << log_safe(name, 64)
		          << " (" << def << ")" << std::endl;
		res.set_content(subsonic_ok_json([&](nlohmann::json& r) {
			r["tag"] = {{"id", *id}};
			}), "application/json");
		});

	// updateTag(id, name, defaultState) - an omitted field keeps its value.
	// Admin tags only; a user's own tag is theirs to rename.
	server_.Get("/rest/updateTag.view", [this, admin_only, err, ok](const httplib::Request& req,
	                                                                httplib::Response& res) {
		if (!admin_only(req, res)) return;
		const int id = to_int(req.get_param_value("id"), -1);
		std::optional<MediaStore::TagInfo> cur;
		for (auto& t : store_.list_tags())
			if (t.id == id && t.owner.empty()) cur = std::move(t);
		if (!cur) { err(res, 70, "Tag not found."); return; }

		const std::string name = req.has_param("name")
			? trim(req.get_param_value("name")) : cur->name;
		const std::string def  = req.has_param("defaultState")
			? req.get_param_value("defaultState") : cur->default_state;
		if (name.empty()) { err(res, 10, "name must not be empty."); return; }
		if (!Visibility::parse_cell(def)) {
			err(res, 10, "defaultState must be blocked, shown, choice_on or choice_off.");
			return;
			}
		if (!store_.update_tag(id, name, def)) { err(res, 0, "A tag of that name exists."); return; }
		ok(res);
		});

	// deleteTag(id) - any tag but a personal one, which goes with its
	// account.
	server_.Get("/rest/deleteTag.view", [this, admin_only, err, ok](const httplib::Request& req,
	                                                                httplib::Response& res) {
		if (!admin_only(req, res)) return;
		const int id = to_int(req.get_param_value("id"), -1);
		if (!store_.delete_tag(id)) {
			err(res, 70, "Tag not found, or it is an account's personal tag.");
			return;
			}
		std::cout << stamp(client_addr(req)) << "Tag deleted: " << id << std::endl;
		ok(res);
		});

	// promoteTag(id, defaultState) - a user's own tag becomes an admin tag,
	// for when others want it too.
	server_.Get("/rest/promoteTag.view", [this, admin_only, err, ok](const httplib::Request& req,
	                                                                 httplib::Response& res) {
		if (!admin_only(req, res)) return;
		const int id = to_int(req.get_param_value("id"), -1);
		const std::string def = req.get_param_value("defaultState");
		if (!Visibility::parse_cell(def)) {
			err(res, 10, "defaultState must be blocked, shown, choice_on or choice_off.");
			return;
			}
		if (!store_.promote_tag(id, def)) {
			err(res, 0, "Not a user's tag, or an admin tag of that name exists.");
			return;
			}
		ok(res);
		});

	// setFolderTags(id, tagId...) - replaces the admin tags on one level-1
	// folder. No tagId clears them. Users' own tags on it are left alone.
	server_.Get("/rest/setFolderTags.view", [this, admin_only, err, ok](const httplib::Request& req,
	                                                                    httplib::Response& res) {
		if (!admin_only(req, res)) return;
		const std::string path = store_.get_folder_path(to_int(req.get_param_value("id"), -1));
		if (path.empty()) { err(res, 70, "Folder not found."); return; }
		std::vector<int> tag_ids;
		const size_t n = req.get_param_value_count("tagId");
		for (size_t i = 0; i < n; ++i)
			tag_ids.push_back(to_int(req.get_param_value("tagId", i), -1));
		if (!store_.set_folder_tags(path, tag_ids)) {
			err(res, 0, "Not an artist or category folder, or not an admin tag.");
			return;
			}
		ok(res);
		});

	// setUserTag(username, tagId, state) - one cell of the grid. An empty or
	// absent state puts the cell back to the tag's default.
	server_.Get("/rest/setUserTag.view", [this, admin_only, err, ok](const httplib::Request& req,
	                                                                 httplib::Response& res) {
		if (!admin_only(req, res)) return;
		const std::string state = req.get_param_value("state");
		if (!state.empty() && !Visibility::parse_cell(state)) {
			err(res, 10, "state must be blocked, shown, choice_on or choice_off.");
			return;
			}
		if (!store_.set_user_tag(req.get_param_value("username"),
		                         to_int(req.get_param_value("tagId"), -1), state)) {
			err(res, 70, "User or admin tag not found.");
			return;
			}
		ok(res);
		});

	// ---- Every user, on their own tags and cells ----------------------

	// getMyTags - the admin tags not blocked for me, with my cell, and my
	// own tags with their switch. `switchable` says whether setTagChoice
	// will take it.
	server_.Get("/rest/getMyTags.view", [this](const httplib::Request& req,
	                                           httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		auto tags = store_.list_my_tags(req.get_param_value("u"));
		nlohmann::json arr = nlohmann::json::array();
		for (const auto& t : tags) {
			nlohmann::json folders = nlohmann::json::array();
			for (const auto& p : t.folders) {
				nlohmann::json f = {{"path", p}};
				if (auto id = store_.folder_id_by_path(p)) f["id"] = sid(*id);
				folders.push_back(std::move(f));
				}
			arr.push_back({{"id", t.id}, {"name", t.name}, {"own", t.own},
			               {"state", t.state}, {"switchable", t.switchable},
			               {"folder", folders}});
			}
		res.set_content(subsonic_ok_json([&](nlohmann::json& r) {
			r["myTags"] = {{"tag", arr}};
			}), "application/json");
		});

	// createMyTag(name) - starts out switched on, so it hides nothing yet.
	server_.Get("/rest/createMyTag.view", [this, err](const httplib::Request& req,
	                                                  httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		const std::string name = trim(req.get_param_value("name"));
		if (name.empty()) { err(res, 10, "Required parameter missing: name."); return; }
		auto id = store_.create_my_tag(req.get_param_value("u"), name);
		if (!id) { err(res, 0, "You already have a tag of that name."); return; }
		res.set_content(subsonic_ok_json([&](nlohmann::json& r) {
			r["tag"] = {{"id", *id}};
			}), "application/json");
		});

	// renameMyTag(id, name)
	server_.Get("/rest/renameMyTag.view", [this, err, ok](const httplib::Request& req,
	                                                      httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		const std::string name = trim(req.get_param_value("name"));
		if (name.empty()) { err(res, 10, "Required parameter missing: name."); return; }
		if (!store_.rename_my_tag(req.get_param_value("u"),
		                          to_int(req.get_param_value("id"), -1), name)) {
			err(res, 70, "Tag not found, or you already have one of that name.");
			return;
			}
		ok(res);
		});

	// deleteMyTag(id)
	server_.Get("/rest/deleteMyTag.view", [this, err, ok](const httplib::Request& req,
	                                                      httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		if (!store_.delete_my_tag(req.get_param_value("u"),
		                          to_int(req.get_param_value("id"), -1))) {
			err(res, 70, "Tag not found.");
			return;
			}
		ok(res);
		});

	// setMyFolderTag(id, tagId, on) - one of my tags on, or off, one level-1
	// folder (by folder id). A folder hidden from me outright answers like a
	// missing one.
	server_.Get("/rest/setMyFolderTag.view", [this, err, ok](const httplib::Request& req,
	                                                         httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		const std::string path = store_.get_folder_path(to_int(req.get_param_value("id"), -1));
		if (path.empty() || !store_.set_my_folder_tag(req.get_param_value("u"), path,
		                                              to_int(req.get_param_value("tagId"), -1),
		                                              req.get_param_value("on") != "false")) {
			err(res, 70, "Tag or artist/category folder not found.");
			return;
			}
		ok(res);
		});

	// setTagChoice(tagId, on) - my own tag's switch, or my cell of an admin
	// tag that leaves me the choice.
	server_.Get("/rest/setTagChoice.view", [this, err, ok](const httplib::Request& req,
	                                                       httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		const std::string on = req.get_param_value("on");
		if (on != "true" && on != "false") {
			err(res, 10, "on must be true or false.");
			return;
			}
		if (!store_.set_tag_choice(req.get_param_value("u"),
		                           to_int(req.get_param_value("tagId"), -1), on == "true")) {
			err(res, 70, "Tag not found, or not yours to switch.");
			return;
			}
		ok(res);
		});
	}
