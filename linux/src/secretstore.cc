#include "secretstore.hh"
#include "serverconfig.hh"
#include "stamp.hh"

#include <libsecret/secret.h>

#include <iostream>

namespace {
	const SecretSchema *schema()
		{
		static const SecretSchema s = {
			"org.gaindrive.linux-player.Server", SECRET_SCHEMA_NONE,
			{
				{ "key", SECRET_SCHEMA_ATTRIBUTE_STRING },
				{ nullptr, SECRET_SCHEMA_ATTRIBUTE_STRING }
			}
		};
		return &s;
		}

	void report(const char *what, GError *err)
		{
		std::cout << stamp() << "keyring " << what << " failed: " << err->message << std::endl;
		g_error_free(err);
		}
}

bool secret_save(const ServerConfig& server, const StoredToken& tok)
	{
	std::string label = "GainDrive " + server.user + " at " + server.url;
	std::string value = tok.salt + ":" + tok.token;
	GError *err = nullptr;
	gboolean ok = secret_password_store_sync(schema(), SECRET_COLLECTION_DEFAULT,
		label.c_str(), value.c_str(), nullptr, &err, "key", server.key.c_str(), nullptr);
	if(err) report("store", err);
	return ok;
	}

std::optional<StoredToken> secret_load(const ServerConfig& server)
	{
	GError *err = nullptr;
	gchar *value = secret_password_lookup_sync(schema(), nullptr, &err,
		"key", server.key.c_str(), nullptr);
	if(err) {
		report("lookup", err);
		return std::nullopt;
		}
	if(!value) return std::nullopt;

	std::string v(value);
	secret_password_free(value);
	auto colon = v.find(':');
	if(colon==std::string::npos) return std::nullopt;
	return StoredToken{v.substr(0, colon), v.substr(colon+1)};
	}

void secret_clear(const ServerConfig& server)
	{
	GError *err = nullptr;
	secret_password_clear_sync(schema(), nullptr, &err, "key", server.key.c_str(), nullptr);
	if(err) report("clear", err);
	}
