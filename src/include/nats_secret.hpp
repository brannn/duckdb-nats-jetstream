#pragma once

#include "duckdb.hpp"
#include <nats/nats.h>

namespace duckdb {

class ExtensionLoader;

// How nats_scan authenticates and secures its connection, from a secret of type nats
struct NatsConnectionSettings {
    string secret_name;
    string user;
    string password;
    string token;
    // Paths: a credentials file (user JWT and seed), an NKey seed file, and TLS files
    string creds;
    string nkey;
    string tls_ca;
    string tls_cert;
    string tls_key;
};

struct NatsSecret {
    static void Register(ExtensionLoader &loader);
    // The secret with the given name, or else the one whose scope best matches url; empty settings if there is none
    static NatsConnectionSettings Lookup(ClientContext &context, const string &name, const string &url);
    // Sets the connection options from the settings
    static natsStatus Apply(const NatsConnectionSettings &settings, natsOptions *opts);
};

} // namespace duckdb
