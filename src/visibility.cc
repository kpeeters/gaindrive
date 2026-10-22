#include "visibility.hh"
#include "stamp.hh"

#include <iostream>
#include <mutex>

#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>

std::optional<Visibility::Cell> Visibility::parse_cell(const std::string& s)
	{
	if (s == "blocked")    return Cell::blocked;
	if (s == "shown")      return Cell::shown;
	if (s == "choice_on")  return Cell::choice_on;
	if (s == "choice_off") return Cell::choice_off;
	return std::nullopt;
	}

const char* Visibility::cell_name(Cell c)
	{
	switch (c) {
		case Cell::blocked:    return "blocked";
		case Cell::shown:      return "shown";
		case Cell::choice_on:  return "choice_on";
		case Cell::choice_off: return "choice_off";
		}
	return "choice_on";
	}

static long long cell_key(int user_id, int tag_id)
	{
	return (static_cast<long long>(user_id) << 32) | static_cast<unsigned int>(tag_id);
	}

void Visibility::reload(SQLite::Database& db, const std::string& uploads_root)
	{
	std::unordered_map<std::string, int> id_by_name;
	std::unordered_map<int, User>        users;
	std::unordered_map<int, Tag>         tags;
	std::unordered_map<std::string, std::vector<int>> folder_tags;
	std::unordered_map<long long, Cell>  cells;

	{
	SQLite::Statement q(db,
		"SELECT id, username, is_admin, COALESCE(block_untagged, 0) FROM client.users");
	while (q.executeStep()) {
		User u;
		u.id             = q.getColumn(0).getInt();
		u.name           = q.getColumn(1).getString();
		u.is_admin       = q.getColumn(2).getInt() != 0;
		u.block_untagged = q.getColumn(3).getInt() != 0;
		id_by_name[u.name] = u.id;
		users[u.id] = std::move(u);
		}
	}
	{
	SQLite::Statement q(db,
		"SELECT id, COALESCE(owner_id, 0), default_state FROM client.tags");
	while (q.executeStep()) {
		Tag t;
		t.owner_id = q.getColumn(1).getInt();
		// A user tag has no default; its owner's row is its only cell, and
		// a missing one means switched on, as when it was created.
		auto def = q.getColumn(2).isNull() ? std::nullopt
		                                   : parse_cell(q.getColumn(2).getString());
		t.def = def ? *def : Cell::choice_on;
		tags[q.getColumn(0).getInt()] = t;
		}
	}
	{
	SQLite::Statement q(db, "SELECT path, tag_id FROM client.folder_tags");
	while (q.executeStep())
		folder_tags[q.getColumn(0).getString()].push_back(q.getColumn(1).getInt());
	}
	{
	SQLite::Statement q(db, "SELECT user_id, tag_id, state FROM client.user_tags");
	while (q.executeStep()) {
		auto c = parse_cell(q.getColumn(2).getString());
		if (c) cells[cell_key(q.getColumn(0).getInt(), q.getColumn(1).getInt())] = *c;
		}
	}

	std::unique_lock lock(mutex_);
	uploads_root_ = uploads_root;
	id_by_name_   = std::move(id_by_name);
	users_        = std::move(users);
	tags_         = std::move(tags);
	folder_tags_  = std::move(folder_tags);
	cells_        = std::move(cells);
	}

int Visibility::user_id(const std::string& username) const
	{
	std::shared_lock lock(mutex_);
	auto it = id_by_name_.find(username);
	return it == id_by_name_.end() ? -1 : it->second;
	}

Visibility::State Visibility::state(const std::string& username,
                                    const std::string& rel_path) const
	{
	std::shared_lock lock(mutex_);
	auto it = id_by_name_.find(username);
	const User* u = nullptr;
	if (it != id_by_name_.end()) {
		auto ut = users_.find(it->second);
		if (ut != users_.end()) u = &ut->second;
		}
	return state_locked(u, rel_path);
	}

Visibility::State Visibility::state(int user_id, const std::string& rel_path) const
	{
	std::shared_lock lock(mutex_);
	auto it = users_.find(user_id);
	return state_locked(it == users_.end() ? nullptr : &it->second, rel_path);
	}

Visibility::State Visibility::state_locked(const User* u,
                                           const std::string& rel_path) const
	{
	// A root row, or nothing at all: there is no level-1 folder to judge.
	const auto slash = rel_path.find('/');
	if (slash == std::string::npos) return State::visible;

	// Fail closed. Every caller has authenticated, so this is a caller
	// passing the wrong name, and that must not show everything.
	if (!u) return State::hard;

	const auto end   = rel_path.find('/', slash + 1);
	const std::string root   = rel_path.substr(0, slash);
	const std::string second = rel_path.substr(slash + 1,
		end == std::string::npos ? std::string::npos : end - slash - 1);

	// The uploads root is personal space: its level-1 folders are the
	// accounts, and tags never apply there.
	if (!uploads_root_.empty() && root == uploads_root_)
		return (u->is_admin || second == u->name) ? State::visible : State::hard;

	const std::string top = root + "/" + second;
	bool allowed = false;   // some admin tag lets this user in
	bool soft    = false;
	auto ft = folder_tags_.find(top);
	if (ft != folder_tags_.end()) {
		for (int tag_id : ft->second) {
			auto tt = tags_.find(tag_id);
			if (tt == tags_.end()) continue;
			const Tag& tag = tt->second;
			auto ct = cells_.find(cell_key(u->id, tag_id));

			if (tag.owner_id != 0) {
				// Someone else's tag does not exist for this user. Their own
				// can only be switched, and never counts towards
				// block_untagged: a kid must not unlock a folder by tagging it.
				if (tag.owner_id != u->id) continue;
				if (ct != cells_.end() && ct->second == Cell::choice_off) soft = true;
				continue;
				}

			const Cell cell = ct != cells_.end() ? ct->second : tag.def;
			switch (cell) {
				case Cell::blocked:
					// Admins cannot be locked out; their own switches still
					// apply, below.
					if (!u->is_admin) return State::hard;
					break;
				case Cell::shown:
				case Cell::choice_on:
					allowed = true;
					break;
				case Cell::choice_off:
					soft = true;
					break;
				}
			}
		}

	if (u->block_untagged && !u->is_admin && !allowed) return State::hard;
	return soft ? State::soft : State::visible;
	}

// gd_visible(user_id, path) and gd_readable(user_id, path). A NULL path
// answers 0 rather than 1: a listing that loses rows to a LEFT JOIN miss is a
// visible bug, one that gains them is a leak.
template<bool readable>
static void sql_visible(sqlite3_context* ctx, int, sqlite3_value** argv)
	{
	const char* fn = readable ? "gd_readable" : "gd_visible";
	try {
		auto* self = static_cast<const Visibility*>(sqlite3_user_data(ctx));
		const unsigned char* p = sqlite3_value_text(argv[1]);
		if (!p) { sqlite3_result_int(ctx, 0); return; }
		const auto st = self->state(sqlite3_value_int(argv[0]),
		                            reinterpret_cast<const char*>(p));
		const bool ok = readable ? st != Visibility::State::hard
		                         : st == Visibility::State::visible;
		sqlite3_result_int(ctx, ok ? 1 : 0);
		}
	catch (const std::exception& e) {
		std::cout << stamp() << fn << ": " << e.what() << std::endl;
		sqlite3_result_error(ctx, "visibility check failed", -1);
		}
	catch (...) {
		std::cout << stamp() << fn << ": unknown exception" << std::endl;
		sqlite3_result_error(ctx, "visibility check failed", -1);
		}
	}

void Visibility::install(SQLite::Database& db)
	{
	// Not deterministic: the answer changes whenever reload() runs.
	db.createFunction("gd_visible",  2, false, this, &sql_visible<false>);
	db.createFunction("gd_readable", 2, false, this, &sql_visible<true>);
	}
