#pragma once

#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace SQLite { class Database; }

// Which level-1 folders (artists, categories) a user gets to see.
//
// The one place that answers the question, for listings and for reads by id
// alike. Listings ask through the gd_visible() SQL function installed below,
// reads by id through item_read_allowed(); both end in state(), so the two
// cannot drift apart the way a filter repeated in every query would.
//
// Tags sit on level-1 folders. An admin tag (owner 0) has a cell per user,
// the user's row in client.user_tags or else the tag's default; a user tag
// belongs to one user and means nothing to anyone else. DATABASE.md has the
// tables.
//
// Everything is held in memory: the tables are small, and gd_visible() runs
// once per candidate row of a listing.
class Visibility {
	public:
		// `soft` hides from browsing and search but still serves by id, so a
		// shared playlist keeps working; `hard` is refused everywhere.
		enum class State { visible, soft, hard };

		enum class Cell { blocked, shown, choice_on, choice_off };

		static std::optional<Cell> parse_cell(const std::string& s);
		static const char*         cell_name(Cell c);

		// Re-reads users, tags, folder_tags and user_tags. The caller holds
		// the database mutex, and calls this after every write to any of
		// them; nothing here depends on the scan.
		void reload(SQLite::Database& db, const std::string& uploads_root);

		// Registers gd_visible(user_id, path) on the connection, 1 when
		// state() is `visible`, for listings; and gd_readable(user_id, path),
		// 1 unless it is `hard`, for lists the user built themselves
		// (playlists, play queue, bookmarks). `this` must outlive the
		// connection.
		void install(SQLite::Database& db);

		State state(const std::string& username, const std::string& rel_path) const;
		State state(int user_id, const std::string& rel_path) const;

		// -1 for a name with no account.
		int user_id(const std::string& username) const;

	private:
		struct User {
			int         id = 0;
			std::string name;
			bool        is_admin       = false;
			bool        block_untagged = false;
			};
		struct Tag {
			int  owner_id = 0;   // 0 for an admin tag
			Cell def      = Cell::choice_on;
			};

		State state_locked(const User* u, const std::string& rel_path) const;

		mutable std::shared_mutex mutex_;
		std::string uploads_root_;
		std::unordered_map<std::string, int> id_by_name_;
		std::unordered_map<int, User>        users_;
		std::unordered_map<int, Tag>         tags_;
		std::unordered_map<std::string, std::vector<int>> folder_tags_;
		// Keyed on (user id << 32) | tag id.
		std::unordered_map<long long, Cell>  cells_;
	};
