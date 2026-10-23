#include "nats_secret.hpp"
#include "duckdb/catalog/catalog_transaction.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/secret/secret.hpp"
#include "duckdb/main/secret/secret_manager.hpp"
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <cstring>
#include <fstream>

namespace duckdb {

static constexpr const char *SECRET_TYPE = "nats";

// Named like the nats CLI's flags; the files are paths
static const char *const SECRET_OPTIONS[] = {"user",  "password", "token",    "creds",
                                             "nkey",  "tls_ca",   "tls_cert", "tls_key"};

static unique_ptr<BaseSecret> CreateNatsSecret(ClientContext &context, CreateSecretInput &input) {
    auto secret = make_uniq<KeyValueSecret>(input.scope, input.type, input.provider, input.name);
    for (auto option : SECRET_OPTIONS) {
        secret->TrySetValue(option, input);
    }
    auto has = [&](const char *option) {
        Value value;
        return secret->TryGetValue(option, value) && !value.IsNull();
    };
    int methods = int(has("user") || has("password")) + int(has("token")) + int(has("creds")) + int(has("nkey"));
    if (methods > 1) {
        throw InvalidInputException("A NATS secret takes one of USER and PASSWORD, TOKEN, CREDS or NKEY");
    }
    if (has("tls_cert") != has("tls_key")) {
        throw InvalidInputException("A NATS secret takes TLS_CERT and TLS_KEY together");
    }
    secret->redact_keys = {"password", "token"};
    return std::move(secret);
}

void NatsSecret::Register(ExtensionLoader &loader) {
    SecretType secret_type;
    secret_type.name = SECRET_TYPE;
    secret_type.deserializer = KeyValueSecret::Deserialize<KeyValueSecret>;
    secret_type.default_provider = "config";
    secret_type.extension = "nats_js";
    loader.RegisterSecretType(secret_type);

    CreateSecretFunction function;
    function.secret_type = SECRET_TYPE;
    function.provider = "config";
    function.function = CreateNatsSecret;
    for (auto option : SECRET_OPTIONS) {
        function.named_parameters[option] = LogicalType(LogicalTypeId::VARCHAR);
    }
    loader.RegisterFunction(function);
}

NatsConnectionSettings NatsSecret::Lookup(ClientContext &context, const string &name, const string &url) {
    NatsConnectionSettings settings;
    auto &secret_manager = SecretManager::Get(context);
    auto transaction = CatalogTransaction::GetSystemCatalogTransaction(context);
    unique_ptr<SecretEntry> entry;
    if (!name.empty()) {
        entry = secret_manager.GetSecretByName(transaction, name);
        if (!entry || entry->secret->GetType() != SECRET_TYPE) {
            throw BinderException("nats_scan: no NATS secret named '%s'", name);
        }
    } else {
        // A URL without a scheme is a nats:// URL
        auto match = secret_manager.LookupSecret(transaction, url.find("://") == string::npos ? "nats://" + url : url,
                                                 SECRET_TYPE);
        if (!match.HasMatch()) {
            return settings;
        }
        entry = std::move(match.secret_entry);
    }

    auto &secret = entry->secret->Cast<KeyValueSecret>();
    auto get = [&](const char *option) {
        Value value;
        return secret.TryGetValue(option, value) && !value.IsNull() ? value.ToString() : string();
    };
    settings.secret_name = secret.GetName().GetIdentifierName();
    settings.user = get("user");
    settings.password = get("password");
    settings.token = get("token");
    settings.creds = get("creds");
    settings.nkey = get("nkey");
    settings.tls_ca = get("tls_ca");
    settings.tls_cert = get("tls_cert");
    settings.tls_key = get("tls_key");
    return settings;
}

// NKeys are written in base32 (RFC 4648, without padding): a prefix, the key, and a CRC-16 (XMODEM) of both
static constexpr const char *BASE32_ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
static constexpr uint8_t NKEY_PREFIX_SEED = 18 << 3;
static constexpr uint8_t NKEY_PREFIX_USER = 20 << 3;

static bool Base32Decode(const string &text, vector<uint8_t> &result) {
    uint64_t buffer = 0;
    int bits = 0;
    for (char c : text) {
        auto pos = c == '\0' ? nullptr : strchr(BASE32_ALPHABET, c);
        if (pos == nullptr) {
            return false;
        }
        buffer = (buffer << 5) | uint64_t(pos - BASE32_ALPHABET);
        bits += 5;
        if (bits >= 8) {
            bits -= 8;
            result.push_back(uint8_t(buffer >> bits));
        }
    }
    return true;
}

static string Base32Encode(const vector<uint8_t> &data) {
    string result;
    uint64_t buffer = 0;
    int bits = 0;
    for (auto byte : data) {
        buffer = (buffer << 8) | byte;
        bits += 8;
        while (bits >= 5) {
            bits -= 5;
            result += BASE32_ALPHABET[(buffer >> bits) & 31];
        }
    }
    if (bits > 0) {
        result += BASE32_ALPHABET[(buffer << (5 - bits)) & 31];
    }
    return result;
}

static uint16_t Crc16(const uint8_t *data, idx_t len) {
    uint16_t crc = 0;
    for (idx_t i = 0; i < len; i++) {
        crc ^= uint16_t(data[i] << 8);
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 0x8000) ? uint16_t((crc << 1) ^ 0x1021) : uint16_t(crc << 1);
        }
    }
    return crc;
}

// The public key of the user NKey whose seed is in the file. The server needs it besides the signature, which the
// client library makes from the seed file itself.
static string NKeyPublicKey(const string &seed_file) {
    std::ifstream file(seed_file);
    if (!file) {
        throw IOException("Cannot read the NKey seed file '%s'", seed_file);
    }
    string seed;
    while (file >> seed && !(seed.size() == 58 && StringUtil::StartsWith(seed, "SU"))) {
    }
    vector<uint8_t> raw;
    bool valid = seed.size() == 58 && Base32Decode(seed, raw) && raw.size() == 36 &&
                 Crc16(raw.data(), 34) == uint16_t(raw[34] | (raw[35] << 8)) &&
                 (raw[0] & 248) == NKEY_PREFIX_SEED &&
                 (((raw[0] & 7) << 5) | ((raw[1] & 248) >> 3)) == NKEY_PREFIX_USER;

    unsigned char public_key[32];
    size_t public_key_len = sizeof(public_key);
    EVP_PKEY *key = valid ? EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, raw.data() + 2, 32) : nullptr;
    valid = key != nullptr && EVP_PKEY_get_raw_public_key(key, public_key, &public_key_len) == 1 &&
            public_key_len == sizeof(public_key);
    EVP_PKEY_free(key);
    if (!raw.empty()) {
        OPENSSL_cleanse(raw.data(), raw.size());
    }
    OPENSSL_cleanse(&seed[0], seed.size());
    if (!valid) {
        throw IOException("'%s' does not hold a user NKey seed", seed_file);
    }

    vector<uint8_t> encoded {NKEY_PREFIX_USER};
    encoded.insert(encoded.end(), public_key, public_key + sizeof(public_key));
    auto crc = Crc16(encoded.data(), encoded.size());
    encoded.push_back(uint8_t(crc & 0xFF));
    encoded.push_back(uint8_t(crc >> 8));
    return Base32Encode(encoded);
}

natsStatus NatsSecret::Apply(const NatsConnectionSettings &settings, natsOptions *opts) {
    natsStatus s = NATS_OK;
    if (!settings.user.empty() || !settings.password.empty()) {
        s = natsOptions_SetUserInfo(opts, settings.user.c_str(), settings.password.c_str());
    } else if (!settings.token.empty()) {
        s = natsOptions_SetToken(opts, settings.token.c_str());
    } else if (!settings.creds.empty()) {
        s = natsOptions_SetUserCredentialsFromFiles(opts, settings.creds.c_str(), nullptr);
    } else if (!settings.nkey.empty()) {
        s = natsOptions_SetNKeyFromSeed(opts, NKeyPublicKey(settings.nkey).c_str(), settings.nkey.c_str());
    }

    // tls:// URLs turn TLS on by themselves
    if (s == NATS_OK && (!settings.tls_ca.empty() || !settings.tls_cert.empty())) {
        s = natsOptions_SetSecure(opts, true);
    }
    if (s == NATS_OK && !settings.tls_ca.empty()) {
        s = natsOptions_LoadCATrustedCertificates(opts, settings.tls_ca.c_str());
    }
    if (s == NATS_OK && !settings.tls_cert.empty()) {
        s = natsOptions_LoadCertificatesChain(opts, settings.tls_cert.c_str(), settings.tls_key.c_str());
    }
    return s;
}

} // namespace duckdb
