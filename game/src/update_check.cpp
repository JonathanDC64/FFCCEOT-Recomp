#include "update_check.hpp"

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

#include "release_info.hpp"
#include "report.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#else
#include "process.hpp"
#endif

namespace update {

namespace {

constexpr const char* kReleasesUrl = "https://api.github.com/repos/JonathanDC64/FFCCEOT-Recomp/releases?per_page=10";
constexpr size_t kMaxBody = 1024 * 1024;  // a release list is far smaller; anything bigger is not what we asked for

std::string user_agent(const std::string& current) { return "ffcc/" + current.substr(0, current.find(' ')); }

#ifdef _WIN32

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

struct Handle {
    HINTERNET h = nullptr;
    ~Handle() {
        if (h) WinHttpCloseHandle(h);
    }
};

// GET `url` with WinHTTP (the system's proxy settings and certificates). True with the body on a 200 answer.
bool http_get(const std::string& url, const std::string& agent, std::string& body) {
    const std::wstring wurl = widen(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof parts;
    wchar_t host[256] = {}, path[2048] = {};
    parts.lpszHostName = host;
    parts.dwHostNameLength = DWORD(std::size(host));
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = DWORD(std::size(path));
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &parts)) return false;

    Handle session, connection, request;
    session.h = WinHttpOpen(widen(agent).c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                            WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.h) return false;
    WinHttpSetTimeouts(session.h, 5000, 5000, 8000, 8000);
    connection.h = WinHttpConnect(session.h, host, parts.nPort, 0);
    if (!connection.h) return false;
    const DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    request.h = WinHttpOpenRequest(connection.h, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!request.h) return false;
    static const wchar_t kAccept[] = L"Accept: application/vnd.github+json";
    if (!WinHttpSendRequest(request.h, kAccept, DWORD(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.h, nullptr))
        return false;
    DWORD status = 0, size = sizeof status;
    if (!WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                             &status, &size, WINHTTP_NO_HEADER_INDEX) ||
        status != 200)
        return false;
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.h, &available)) return false;
        if (available == 0) return true;
        const size_t at = body.size();
        if (at + available > kMaxBody) return false;
        body.resize(at + available);
        DWORD read = 0;
        if (!WinHttpReadData(request.h, body.data() + at, available, &read)) return false;
        body.resize(at + read);
    }
}

#else

// Linux: the system's curl (found where distributions put it; there is no HTTP library in the program).
bool http_get(const std::string& url, const std::string& agent, std::string& body) {
    for (const char* curl : {"/usr/bin/curl", "/bin/curl", "/usr/local/bin/curl"}) {
        if (!std::filesystem::exists(curl)) continue;
        const proc::Result r = proc::run(curl, {"-fsSL", "--max-time", "10", "-A", agent, "-H", "Accept: application/vnd.github+json", url});
        if (!r.started || r.exit_code != 0 || r.output.size() > kMaxBody) return false;
        body = r.output;
        return true;
    }
    return false;
}

#endif

std::mutex g_mutex;
std::atomic<bool> g_started{false};
bool g_found = false;
Available g_available;

void check(std::string current) {
    std::string body;
    if (!http_get(kReleasesUrl, user_agent(current), body)) {
        std::printf("update check: no answer from GitHub\n");
        return;
    }
    std::vector<Release> releases;
    if (!parse_releases(body, releases)) {
        std::printf("update check: the answer was not a list of releases\n");
        return;
    }
    // The newest of the releases that is newer than this build (the list is newest first).
    for (const Release& r : releases) {
        if (!is_newer(r.tag, current)) continue;
        std::lock_guard<std::mutex> lock(g_mutex);
        g_available = {display_version(r.tag), r.url.empty() ? std::string(report::kRepositoryUrl) + "/releases" : r.url,
                       plain_notes(r.notes)};
        g_found = true;
        std::printf("update check: %s is available\n", g_available.version.c_str());
        return;
    }
    std::printf("update check: up to date\n");
}

}  // namespace

void start(const std::string& current) {
    if (g_started.exchange(true)) return;
    // Detached: the program may end while the request waits (the answer is of no use then).
    std::thread(check, current).detach();
}

bool available(Available& out) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_found) return false;
    out = g_available;
    return true;
}

}  // namespace update
