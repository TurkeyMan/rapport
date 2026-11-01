// rap::sys secrets: Windows' Credential Manager; elsewhere the Secret Service
// through libsecret, loaded on first use with the few functions it needs
// declared here, as http.cpp does libcurl.
#include "rapport/sys.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
// clang-format off
#include <windows.h>
#include <wincred.h>  // after windows.h, whose types it uses
// clang-format on
#endif

namespace rap::sys
{
namespace
{

thread_local std::string last_error;

bool failed(std::string why)
{
    last_error = std::move(why);
    return false;
}

}  // namespace

const std::string& secret_error()
{
    return last_error;
}

#if defined(_WIN32)

namespace
{

std::wstring wide(std::string_view s)
{
    std::wstring w(size_t(MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0)),
                   L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), int(w.size()));
    return w;
}

std::string why(const char* what)
{
    return std::string(what) + " (error " + std::to_string(GetLastError()) + ")";
}

}  // namespace

bool store_secret(std::string_view name, std::string_view secret, std::string_view label)
{
    last_error.clear();
    if (secret.size() > CRED_MAX_CREDENTIAL_BLOB_SIZE)
        return failed("too long for the Credential Manager (" +
                      std::to_string(CRED_MAX_CREDENTIAL_BLOB_SIZE) + " bytes at most)");
    std::wstring target = wide(name), comment = wide(label);
    CREDENTIALW c{};
    c.Type = CRED_TYPE_GENERIC;
    c.TargetName = target.data();
    c.Comment = comment.empty() ? nullptr : comment.data();
    c.CredentialBlobSize = DWORD(secret.size());  // UTF-8 bytes, as given
    c.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(secret.data()));
    c.Persist = CRED_PERSIST_LOCAL_MACHINE;  // still this user's; ENTERPRISE would roam
    return CredWriteW(&c, 0) || failed(why("the Credential Manager refused it"));
}

std::optional<std::string> load_secret(std::string_view name)
{
    last_error.clear();
    PCREDENTIALW c = nullptr;
    if (!CredReadW(wide(name).c_str(), CRED_TYPE_GENERIC, 0, &c))
    {
        if (GetLastError() != ERROR_NOT_FOUND)
            failed(why("the Credential Manager couldn't be read"));
        return std::nullopt;
    }
    std::string s(reinterpret_cast<const char*>(c->CredentialBlob), c->CredentialBlobSize);
    CredFree(c);
    return s;
}

bool erase_secret(std::string_view name)
{
    last_error.clear();
    return CredDeleteW(wide(name).c_str(), CRED_TYPE_GENERIC, 0) ||
           GetLastError() == ERROR_NOT_FOUND ||
           failed(why("the Credential Manager couldn't remove it"));
}

#else  // libsecret

namespace
{

// libsecret's simple password API, as secret.h has it.
struct schema_attribute
{
    const char* name;
    int type;  // SECRET_SCHEMA_ATTRIBUTE_STRING: 0
};
struct secret_schema
{
    const char* name;
    int flags;  // SECRET_SCHEMA_NONE: 0
    schema_attribute attributes[32];
    int reserved;
    void* reserved1[7];
};
struct g_error
{
    unsigned domain;
    int code;
    char* message;
};

// One attribute, "name": the caller's name for it.
const secret_schema k_schema{"rapport.Secret", 0, {{"name", 0}}, 0, {}};
constexpr const char* k_default_collection = "default";  // SECRET_COLLECTION_DEFAULT

struct secret_api
{
    int (*store)(const secret_schema*, const char* collection, const char* label,
                 const char* password, void* cancellable, g_error**, ...);
    char* (*lookup)(const secret_schema*, void* cancellable, g_error**, ...);
    int (*clear)(const secret_schema*, void* cancellable, g_error**, ...);
    void (*free_password)(char*);
    void (*free_error)(g_error*);
    std::string missing;  // set when libsecret couldn't be loaded
};

const secret_api& api()
{
    static secret_api a = []
    {
        secret_api a{};
        if (auto lib = library::open("libsecret-1.so.0"))
        {
            a.store = lib->function<int(const secret_schema*, const char*, const char*, const char*,
                                        void*, g_error**, ...)>("secret_password_store_sync");
            a.lookup = lib->function<char*(const secret_schema*, void*, g_error**, ...)>(
                "secret_password_lookup_sync");
            a.clear = lib->function<int(const secret_schema*, void*, g_error**, ...)>(
                "secret_password_clear_sync");
            a.free_password = lib->function<void(char*)>("secret_password_free");
            a.free_error = lib->function<void(g_error*)>("g_error_free");  // GLib's, a dependency
            if (a.store && a.lookup && a.clear && a.free_password && a.free_error)
            {
                lib->release();  // for the life of the process
                return a;
            }
        }
        a = {};
        a.missing = "the keychain needs libsecret (libsecret-1.so.0), which isn't installed";
        return a;
    }();
    return a;
}

// The error's message, freed; false.
bool failed(const secret_api& a, g_error* e, const char* what)
{
    std::string why = what;
    if (e)
    {
        if (e->message) why += std::string(": ") + e->message;
        a.free_error(e);
    }
    return failed(std::move(why));
}

}  // namespace

bool store_secret(std::string_view name, std::string_view secret, std::string_view label)
{
    last_error.clear();
    const secret_api& a = api();
    if (!a.missing.empty()) return failed(a.missing);
    const std::string n(name), s(secret), l(label.empty() ? name : label);
    g_error* e = nullptr;
    return a.store(&k_schema, k_default_collection, l.c_str(), s.c_str(), nullptr, &e, "name",
                   n.c_str(), nullptr) ||
           failed(a, e, "the keychain refused it");
}

std::optional<std::string> load_secret(std::string_view name)
{
    last_error.clear();
    const secret_api& a = api();
    if (!a.missing.empty())
    {
        failed(a.missing);
        return std::nullopt;
    }
    const std::string n(name);
    g_error* e = nullptr;
    char* found = a.lookup(&k_schema, nullptr, &e, "name", n.c_str(), nullptr);
    if (e)
    {
        failed(a, e, "the keychain couldn't be read");
        return std::nullopt;
    }
    if (!found) return std::nullopt;
    std::string s = found;
    a.free_password(found);  // wipes it
    return s;
}

bool erase_secret(std::string_view name)
{
    last_error.clear();
    const secret_api& a = api();
    if (!a.missing.empty()) return failed(a.missing);
    const std::string n(name);
    g_error* e = nullptr;
    a.clear(&k_schema, nullptr, &e, "name", n.c_str(), nullptr);  // false also when there was none
    return !e || failed(a, e, "the keychain couldn't remove it");
}

#endif

}  // namespace rap::sys
