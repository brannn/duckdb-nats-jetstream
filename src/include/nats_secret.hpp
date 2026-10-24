#pragma once

#include "duckdb.hpp"
#include <nats/nats.h>

struct evp_pkey_st;

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

// The user NKey a connection signs the server's nonce with, on connecting and on reconnecting
struct NatsNKey {
    evp_pkey_st *key = nullptr;
    ~NatsNKey();
};

struct NatsSecret {
    static void Register(ExtensionLoader &loader);
    // The secret with the given name, or else the one whose scope best matches url; empty settings if there is none
    static NatsConnectionSettings Lookup(ClientContext &context, const string &name, const string &url);
    // Sets the connection options from the settings, reading their files through DuckDB's file system. An NKey is
    // kept in nkey, which must outlive the connection.
    static natsStatus Apply(ClientContext &context, const NatsConnectionSettings &settings, natsOptions *opts,
                            unique_ptr<NatsNKey> &nkey);
};

} // namespace duckdb
