#include "nats_secret.hpp"
#include "duckdb/catalog/catalog_transaction.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/secret/secret.hpp"
#include "duckdb/main/secret/secret_manager.hpp"
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <cstdlib>
#include <cstring>

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
    for (auto option : SECRET_OPTIONS) {
        Value value;
        if (has(option) && secret->TryGetValue(option, value) && value.ToString().empty()) {
            throw InvalidInputException("A NATS secret's %s cannot be empty", StringUtil::Upper(option));
        }
    }
    int methods = int(has("user") || has("password")) + int(has("token")) + int(has("creds")) + int(has("nkey"));
    if (methods > 1) {
        throw InvalidInputException("A NATS secret takes one of USER and PASSWORD, TOKEN, CREDS or NKEY");
    }
    if (has("user") != has("password")) {
        throw InvalidInputException("A NATS secret takes USER and PASSWORD together");
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

// The URL that scopes are matched with: without a scheme it is a nats:// URL, and its user and password are left out
static string ScopeURL(const string &url) {
    auto result = url.substr(0, url.find(',')).find("://") == string::npos ? "nats://" + url : url;
    auto start = result.find("://") + 3;
    auto at = result.find('@', start);
    auto end = result.find_first_of("/,", start);
    if (at != string::npos && (end == string::npos || at < end)) {
        result.erase(start, at + 1 - start);
    }
    return result;
}

// Whether a scope that url starts with, pos characters long, ends where a scheme, host or port does. A colon in an
// IPv6 address ([::1]) ends none.
static bool IsScopeEnd(const string &url, idx_t pos) {
    if (pos == url.size() || url[pos - 1] == '/' || url[pos] == '/' || url[pos] == ',') {
        return true;
    }
    if (url[pos] != ':') {
        return false;
    }
    auto open = url.rfind('[', pos);
    auto close = url.rfind(']', pos);
    return open == string::npos || (close != string::npos && close > open);
}

// The secret whose scope best matches url. DuckDB matches a scope as any start of the URL; here it must also end where
// a scheme, host or port does, so that 'nats://example.com:4222' is not for nats://example.com:42220, nor
// 'tls://example.com' for tls://example.com.other.net. No scope, or an empty one, matches every URL, least well.
static unique_ptr<SecretEntry> LookupScopedSecret(SecretManager &secret_manager, CatalogTransaction transaction,
                                                  const string &url) {
    int64_t best_score = -1;
    string best_scope;
    for (auto &entry : secret_manager.AllSecrets(transaction)) {
        if (entry.secret->GetType() != SECRET_TYPE) {
            continue;
        }
        auto &scopes = entry.secret->GetScope();
        if (scopes.empty() && best_score < 0) {
            best_score = 0;
        }
        for (auto &scope : scopes) {
            if (!scope.empty() && !(StringUtil::StartsWith(url, scope) && IsScopeEnd(url, scope.size()))) {
                continue;
            }
            auto score = NumericCast<int64_t>(scope.size());
            if (score > best_score) {
                best_score = score;
                best_scope = scope;
            }
        }
    }
    if (best_score < 0) {
        return nullptr;
    }
    // DuckDB chooses between secrets with this scope, by where they are stored
    auto match = secret_manager.LookupSecret(transaction, best_scope, SECRET_TYPE);
    return match.HasMatch() ? std::move(match.secret_entry) : nullptr;
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
        entry = LookupScopedSecret(secret_manager, transaction, ScopeURL(url));
        if (!entry) {
            return settings;
        }
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

// A secret file's contents, wiped when done with
struct SecretFileContents {
    string data;
    ~SecretFileContents() {
        if (!data.empty()) {
            OPENSSL_cleanse(&data[0], data.size());
        }
    }
};

// Opens a secret's file through DuckDB's file system, so that its settings apply (disabled_filesystems, and ~ for
// the home directory), and reads it into contents if given. Returns the path expanded, for files the client library
// reads itself.
static string ReadSecretFile(ClientContext &context, const string &path, SecretFileContents *contents) {
    static constexpr idx_t MAX_SECRET_FILE_SIZE = 1 << 20;
    auto &fs = FileSystem::GetFileSystem(context);
    auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_READ);
    if (contents != nullptr) {
        auto size = handle->GetFileSize();
        if (size > MAX_SECRET_FILE_SIZE) {
            throw IOException("'%s' is too large for a NATS secret file", path);
        }
        contents->data.resize(size);
        if (size > 0 && handle->Read(&contents->data[0], size) != NumericCast<int64_t>(size)) {
            throw IOException("Cannot read '%s'", path);
        }
    }
    return FileSystem::ExpandPath(path, ClientData::Get(context).file_opener.get());
}

// NKeys are written in base32 (RFC 4648, without padding): a prefix, the key, and a CRC-16 (XMODEM) of both
static constexpr const char *BASE32_ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
static constexpr uint8_t NKEY_PREFIX_SEED = 18 << 3;
static constexpr uint8_t NKEY_PREFIX_USER = 20 << 3;
static constexpr idx_t NKEY_SEED_LENGTH = 58;
// The decoded seed: two bytes of prefixes, the 32-byte Ed25519 private key and the CRC
static constexpr idx_t NKEY_SEED_BYTES = 36;

// Decodes text into exactly result_len bytes
static bool Base32Decode(const char *text, idx_t len, uint8_t *result, idx_t result_len) {
    uint64_t buffer = 0;
    int bits = 0;
    idx_t count = 0;
    bool valid = true;
    for (idx_t i = 0; i < len && valid; i++) {
        auto pos = text[i] == '\0' ? nullptr : strchr(BASE32_ALPHABET, text[i]);
        if (pos == nullptr) {
            valid = false;
            break;
        }
        buffer = (buffer << 5) | uint64_t(pos - BASE32_ALPHABET);
        bits += 5;
        if (bits >= 8) {
            bits -= 8;
            if (count == result_len) {
                valid = false;
                break;
            }
            result[count++] = uint8_t(buffer >> bits);
        }
    }
    OPENSSL_cleanse(&buffer, sizeof(buffer));
    return valid && count == result_len;
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

NatsNKey::~NatsNKey() {
    EVP_PKEY_free(key);
}

// The user JWT of a credentials file: the word after its BEGIN line, as nsc writes it
static string ReadUserJWT(const string &creds_file, const string &text) {
    static constexpr const char *BEGIN = "-----BEGIN NATS USER JWT-----";
    string jwt;
    auto pos = text.find(BEGIN);
    if (pos != string::npos) {
        pos += strlen(BEGIN);
        while (pos < text.size() && StringUtil::CharacterIsSpace(text[pos])) {
            pos++;
        }
        auto end = pos;
        while (end < text.size() && !StringUtil::CharacterIsSpace(text[end])) {
            end++;
        }
        jwt = text.substr(pos, end - pos);
    }
    // Without a JWT the client library would send the whole file, seed and all, as the JWT
    if (jwt.empty() || jwt[0] == '-') {
        throw IOException("'%s' is not a NATS credentials file: it has no user JWT", creds_file);
    }
    return jwt;
}

// The user NKey whose seed is the first word of the file that looks like one, as `nats` and nsc write them
static unique_ptr<NatsNKey> ReadNKey(const string &seed_file, const string &text) {
    auto nkey = make_uniq<NatsNKey>();
    idx_t pos = 0;
    while (pos < text.size()) {
        while (pos < text.size() && StringUtil::CharacterIsSpace(text[pos])) {
            pos++;
        }
        idx_t end = pos;
        while (end < text.size() && !StringUtil::CharacterIsSpace(text[end])) {
            end++;
        }
        if (end - pos == NKEY_SEED_LENGTH && text[pos] == 'S' && text[pos + 1] == 'U') {
            uint8_t raw[NKEY_SEED_BYTES];
            bool valid = Base32Decode(text.data() + pos, NKEY_SEED_LENGTH, raw, NKEY_SEED_BYTES) &&
                         Crc16(raw, 34) == uint16_t(raw[34] | (raw[35] << 8)) && (raw[0] & 248) == NKEY_PREFIX_SEED &&
                         (((raw[0] & 7) << 5) | ((raw[1] & 248) >> 3)) == NKEY_PREFIX_USER;
            if (valid) {
                nkey->key = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, raw + 2, 32);
            }
            OPENSSL_cleanse(raw, sizeof(raw));
            break;
        }
        pos = end;
    }
    if (nkey->key == nullptr) {
        throw IOException("'%s' does not hold a user NKey seed", seed_file);
    }
    return nkey;
}

static string NKeyPublicKey(const string &seed_file, const NatsNKey &nkey) {
    unsigned char public_key[32];
    size_t public_key_len = sizeof(public_key);
    if (EVP_PKEY_get_raw_public_key(nkey.key, public_key, &public_key_len) != 1 ||
        public_key_len != sizeof(public_key)) {
        throw IOException("'%s' does not hold a user NKey seed", seed_file);
    }
    vector<uint8_t> encoded {NKEY_PREFIX_USER};
    encoded.insert(encoded.end(), public_key, public_key + sizeof(public_key));
    auto crc = Crc16(encoded.data(), encoded.size());
    encoded.push_back(uint8_t(crc & 0xFF));
    encoded.push_back(uint8_t(crc >> 8));
    return Base32Encode(encoded);
}

// The client library's handlers for an NKey; it frees what they return with free
static natsStatus GetUserJWT(char **jwt, char **error, void *closure) {
    *jwt = strdup(static_cast<NatsNKey *>(closure)->jwt.c_str());
    return *jwt != nullptr ? NATS_OK : NATS_NO_MEMORY;
}

static natsStatus SignNonce(char **error, unsigned char **signature, int *signature_len, const char *nonce,
                            void *closure) {
    auto &nkey = *static_cast<NatsNKey *>(closure);
    // A server that does not ask for a signature sends no nonce
    if (nonce == nullptr) {
        nonce = "";
    }
    size_t len = 64;
    auto result = static_cast<unsigned char *>(malloc(len));
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    bool signed_nonce = result != nullptr && ctx != nullptr &&
                        EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, nkey.key) == 1 &&
                        EVP_DigestSign(ctx, result, &len, reinterpret_cast<const unsigned char *>(nonce),
                                       strlen(nonce)) == 1;
    EVP_MD_CTX_free(ctx);
    if (!signed_nonce) {
        free(result);
        *error = strdup("cannot sign the server's nonce with the NKey");
        return NATS_ERR;
    }
    *signature = result;
    *signature_len = int(len);
    return NATS_OK;
}

natsStatus NatsSecret::Apply(ClientContext &context, const NatsConnectionSettings &settings, natsOptions *opts,
                             unique_ptr<NatsNKey> &nkey) {
    natsStatus s = NATS_OK;
    if (!settings.user.empty()) {
        s = natsOptions_SetUserInfo(opts, settings.user.c_str(), settings.password.c_str());
    } else if (!settings.token.empty()) {
        s = natsOptions_SetToken(opts, settings.token.c_str());
    } else if (!settings.creds.empty()) {
        // Read here rather than by the client library, which would keep a copy of the seed
        SecretFileContents creds;
        ReadSecretFile(context, settings.creds, &creds);
        auto jwt = ReadUserJWT(settings.creds, creds.data);
        nkey = ReadNKey(settings.creds, creds.data);
        nkey->jwt = std::move(jwt);
        s = natsOptions_SetUserCredentialsCallbacks(opts, GetUserJWT, nkey.get(), SignNonce, nkey.get());
    } else if (!settings.nkey.empty()) {
        SecretFileContents seed;
        ReadSecretFile(context, settings.nkey, &seed);
        nkey = ReadNKey(settings.nkey, seed.data);
        s = natsOptions_SetNKey(opts, NKeyPublicKey(settings.nkey, *nkey).c_str(), SignNonce, nkey.get());
    }

    // tls:// URLs turn TLS on by themselves
    if (s == NATS_OK && (!settings.tls_ca.empty() || !settings.tls_cert.empty())) {
        s = natsOptions_SetSecure(opts, true);
    }
    if (s == NATS_OK && !settings.tls_ca.empty()) {
        s = natsOptions_LoadCATrustedCertificates(opts, ReadSecretFile(context, settings.tls_ca, nullptr).c_str());
    }
    if (s == NATS_OK && !settings.tls_cert.empty()) {
        // The client library reads the files itself, as only its file loader takes a certificate chain
        auto cert_path = ReadSecretFile(context, settings.tls_cert, nullptr);
        SecretFileContents key;
        auto key_path = ReadSecretFile(context, settings.tls_key, &key);
        // OpenSSL would ask for the passphrase on the terminal
        if (key.data.find("-----BEGIN ENCRYPTED") != string::npos ||
            key.data.find("Proc-Type: 4,ENCRYPTED") != string::npos) {
            throw IOException("'%s' is encrypted; TLS_KEY takes a key without a passphrase", settings.tls_key);
        }
        s = natsOptions_LoadCertificatesChain(opts, cert_path.c_str(), key_path.c_str());
    }
    return s;
}

} // namespace duckdb
