// Native (Linux) FFmpeg backend — see native_ffmpeg.hpp for the design notes.
//
// <winsock2.h> MUST be included before anything drags in <windows.h>, otherwise the
// old winsock 1.1 declarations in windows.h collide with winsock2's.
#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <windows.h>
#endif

#include "native_ffmpeg.hpp"

#ifdef GEODE_IS_WINDOWS

#include <Geode/loader/Log.hpp>
#include <Geode/loader/Mod.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <thread>

// NOTE: `using namespace geode;` rather than `namespace log = geode::log;` — the CRT
// declares a global ::log(double), which would clash with a namespace alias of that name.
// Qualified lookup for `log::` ignores the function, so this form is safe.
using namespace geode;

namespace {

    typedef const char* (CDECL* wine_get_version_t)(void);
    typedef char* (CDECL* wine_get_unix_file_name_t)(const WCHAR*);

    constexpr const char* kWhitespace = " \t\r\n";

    std::string trim(const std::string& str) {
        size_t start = str.find_first_not_of(kWhitespace);
        if (start == std::string::npos) return "";
        size_t end = str.find_last_not_of(kWhitespace);
        return str.substr(start, end - start + 1);
    }

    // Strips whitespace plus any leading/trailing commas, so hand-built filter fragments
    // like ",fade=t=in:st=0:d=1" can be fed straight into joinFilters.
    std::string trimFilter(const std::string& str) {
        std::string out = trim(str);
        while (!out.empty() && out.front() == ',') out = trim(out.substr(1));
        while (!out.empty() && out.back() == ',') out = trim(out.substr(0, out.size() - 1));
        return out;
    }

    std::string utf8FromWide(const std::wstring& wide) {
        if (wide.empty()) return "";
        int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
        if (size <= 0) return "";
        std::string out(static_cast<size_t>(size), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), out.data(), size, nullptr, nullptr);
        return out;
    }

    // The mirror of utf8FromWide. Unix paths come out of wine_get_unix_file_name as UTF-8,
    // so anything that hands one to a Win32 API has to widen it with CP_UTF8 — passing the
    // narrow bytes to an ...A entry point would have them re-decoded as the ANSI codepage
    // and mangle every non-ASCII home directory.
    std::wstring wideFromUtf8(const std::string& utf8) {
        if (utf8.empty()) return L"";
        int size = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
        if (size <= 0) return L"";
        std::wstring out(static_cast<size_t>(size), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), out.data(), size);
        return out;
    }

    wine_get_unix_file_name_t getUnixNameFn() {
        static wine_get_unix_file_name_t fn = reinterpret_cast<wine_get_unix_file_name_t>(
            GetProcAddress(GetModuleHandleA("kernel32.dll"), "wine_get_unix_file_name")
        );
        return fn;
    }

    // Winsock is refcounted; GD/curl also uses it, so we start it once and never call
    // WSACleanup (releasing a reference we do not own would be worse than leaking one).
    bool ensureWinsock() {
        static bool ok = [] {
            WSADATA wsa{};
            int res = WSAStartup(MAKEWORD(2, 2), &wsa);
            if (res != 0) {
                log::error("[NativeFFmpeg] WSAStartup failed ({})", res);
                return false;
            }
            return true;
        }();
        return ok;
    }

    // Binds a probe socket to port 0 and reads back what the kernel handed out. There is
    // a benign race between closing this socket and ffmpeg binding the same port.
    int findFreePort() {
        SOCKET probe = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (probe == INVALID_SOCKET) return -1;

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;

        if (bind(probe, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            closesocket(probe);
            return -1;
        }

        int len = sizeof(addr);
        if (getsockname(probe, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
            closesocket(probe);
            return -1;
        }

        int port = static_cast<int>(ntohs(addr.sin_port));
        closesocket(probe);
        return port;
    }

    // The runtime probe from the spec: prefer the host's ffmpeg (reached through the
    // host's dynamic loader, because a Fedora binary cannot use the container's libs),
    // otherwise fall back to whatever the user configured.
    //
    // Both branches define the same `xdbot_ffmpeg` function so the call site is uniform.
    // The host branch deliberately leaves $FF unquoted — it has to word-split into the
    // loader, --library-path, the path list and the binary. The user branch must NOT
    // word-split, or a configured path containing a space ("/opt/my ffmpeg/ffmpeg")
    // would be torn into two words and fail with "not found" / exit 127.
    std::string buildPrelude() {
        std::string fallback;
        if (Mod* mod = Mod::get())
            fallback = trim(mod->getSettingValue<std::string>("native_ffmpeg_path"));
        if (fallback.empty()) fallback = "ffmpeg";

        return fmt::format(
            "H=/run/host\n"
            "if [ -x \"$H/usr/bin/ffmpeg\" ]; then\n"
            "  LP=$H/lib64:$H/usr/lib64:$H/usr/lib64/ffmpeg:$H/usr/lib64/pipewire-0.3/jack:$H/usr/lib64/pulseaudio:$H/usr/lib64/samba\n"
            "  if [ -d \"$H/usr/lib64/dri-freeworld\" ]; then export LIBVA_DRIVERS_PATH=\"$H/usr/lib64/dri-freeworld\"; fi\n"
            "  FF=\"$H/usr/lib64/ld-linux-x86-64.so.2 --library-path $LP $H/usr/bin/ffmpeg\"\n"
            "  xdbot_ffmpeg() {{ $FF \"$@\"; }}\n"
            "else\n"
            "  xdbot_ffmpeg() {{ {} \"$@\"; }}\n"
            "fi\n",
            NativeFFmpeg::shQuote(fallback)
        );
    }

    // Removes job files left behind by a previous session — a run that timed out is
    // deliberately not cleaned up (ffmpeg may still be writing to it), so without this
    // sweep those files would accumulate in the save dir forever.
    void sweepStaleJobs(const std::filesystem::path& dir) {
        std::error_code ec;
        auto now = std::filesystem::file_time_type::clock::now();

        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (ec) break;

            std::string name = entry.path().filename().string();
            if (name.rfind("xdbot_native_", 0) != 0) continue;

            std::error_code timeEc;
            auto written = std::filesystem::last_write_time(entry.path(), timeEc);
            if (timeEc) continue;

            // A day is far longer than any render, so this can never race a live job.
            if (now - written < std::chrono::hours(24)) continue;

            std::error_code removeEc;
            std::filesystem::remove(entry.path(), removeEc);
        }
    }

    NativeFFmpeg::ScriptJob makeJob(const char* tag) {
        static std::atomic<unsigned> counter{0};

        NativeFFmpeg::ScriptJob job;

        Mod* mod = Mod::get();
        if (!mod) return job;

        std::filesystem::path dir = mod->getSaveDir();

        std::error_code ec;
        std::filesystem::create_directories(dir, ec);

        sweepStaleJobs(dir);

        auto dirUnix = NativeFFmpeg::toUnixPath(dir);
        if (!dirUnix) {
            log::error("[NativeFFmpeg] could not translate the save dir to a Unix path");
            return job;
        }

        auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();

        std::string name = fmt::format("xdbot_native_{}_{}_{}", tag, stamp, counter.fetch_add(1));

        job.scriptWin = dir / (name + ".sh");
        job.doneWin = dir / (name + ".done");
        job.logWin = dir / (name + ".log");
        job.startedWin = dir / (name + ".started");
        job.shellLogWin = dir / (name + ".shell");

        std::string base = *dirUnix;
        while (!base.empty() && base.back() == '/') base.pop_back();

        job.scriptUnix = base + "/" + name + ".sh";
        job.doneUnix = base + "/" + name + ".done";
        job.logUnix = base + "/" + name + ".log";
        job.startedUnix = base + "/" + name + ".started";
        job.shellLogUnix = base + "/" + name + ".shell";
        job.valid = true;

        return job;
    }

    // Builds the full script: clear the markers, announce that the shell actually got
    // going (so a script that never runs can be detected in seconds instead of after the
    // full timeout), probe for ffmpeg, run it, record $?.
    std::string buildScript(const NativeFFmpeg::ScriptJob& job, const std::string& args) {
        return fmt::format(
            "#!/bin/sh\n"
            "rm -f {} {}\n"
            // Anything the shell itself complains about (a syntax error from a stray quote
            // in the user's args, an unusable interpreter, ...) goes here. ffmpeg's own
            // stderr is redirected separately below, so the two never mix.
            "exec 2> {}\n"
            "echo 1 > {}\n"
            "{}"
            "xdbot_ffmpeg {} > {} 2>&1\n"
            "echo $? > {}\n",
            NativeFFmpeg::shQuote(job.doneUnix),
            NativeFFmpeg::shQuote(job.logUnix),
            NativeFFmpeg::shQuote(job.shellLogUnix),
            NativeFFmpeg::shQuote(job.startedUnix),
            buildPrelude(),
            args,
            NativeFFmpeg::shQuote(job.logUnix),
            NativeFFmpeg::shQuote(job.doneUnix)
        );
    }

    bool writeScript(const NativeFFmpeg::ScriptJob& job, const std::string& body) {
        std::ofstream file(job.scriptWin, std::ios::binary | std::ios::trunc);
        if (!file) {
            log::error("[NativeFFmpeg] could not write {}", job.scriptWin.string());
            return false;
        }
        file.write(body.data(), static_cast<std::streamsize>(body.size()));
        file.close();
        return true;
    }

    // start.exe returns immediately — it does not wait for the child and its stdin
    // cannot be inherited, which is why everything else here uses files and a socket.
    bool launchScript(const std::string& scriptUnix) {
        std::string arg = scriptUnix;
        if (arg.find_first_of(" \t") != std::string::npos)
            arg = "\"" + arg + "\"";

        // scriptUnix is UTF-8 (it came from wine_get_unix_file_name), so the command line
        // has to be widened here and handed to CreateProcessW. CreateProcessA would decode
        // it as the ANSI codepage and a home directory like /home/renée would turn into a
        // path that does not exist — the script would silently never run.
        std::wstring cmd = wideFromUtf8("start.exe /unix /bin/sh " + arg);

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};

        std::vector<wchar_t> mutable_cmd(cmd.begin(), cmd.end());
        mutable_cmd.push_back(L'\0');

        if (!CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
            log::error("[NativeFFmpeg] CreateProcess failed for start.exe (error {})", GetLastError());
            return false;
        }

        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return true;
    }

    std::optional<int> readDone(const NativeFFmpeg::ScriptJob& job) {
        std::error_code ec;
        if (!std::filesystem::exists(job.doneWin, ec) || ec) return std::nullopt;

        std::ifstream file(job.doneWin, std::ios::binary);
        if (!file) return std::nullopt;

        std::string line;
        std::getline(file, line);
        line = trim(line);
        if (line.empty()) return std::nullopt; // created but not flushed yet

        char* end = nullptr;
        long value = std::strtol(line.c_str(), &end, 10);
        if (end == line.c_str()) return std::nullopt;

        return static_cast<int>(value);
    }

    // If the shell has not even reached its first line after this long, it almost
    // certainly never ran. Generous, because the machine is busy rendering and a slow
    // /bin/sh must not be mistaken for a dead one.
    constexpr int kStartupGraceMs = 45000;

    // `startupGraceMs` of 0 disables the "script never started" check — used where the
    // caller has already waited long enough to know, so that a short poll here cannot
    // masquerade as a launch failure.
    int waitForDone(
        const NativeFFmpeg::ScriptJob& job,
        int timeoutMs,
        const NativeFFmpeg::ProgressFn& onProgress,
        int startupGraceMs = kStartupGraceMs
    ) {
        int attempts = timeoutMs / 100;
        if (attempts < 1) attempts = 1;

        // A grace period longer than the whole budget could never elapse, which would
        // silently disable the check instead of reporting it.
        if (startupGraceMs > timeoutMs) startupGraceMs = 0;

        bool started = false;
        int lastReported = 0;

        for (int i = 0; i < attempts; ++i) {
            if (auto code = readDone(job)) return *code;

            int elapsedMs = i * 100;

            if (!started) {
                std::error_code ec;
                started = std::filesystem::exists(job.startedWin, ec) && !ec;

                if (!started && startupGraceMs > 0 && elapsedMs >= startupGraceMs) {
                    log::error("[NativeFFmpeg] the launcher script never started — is /bin/sh reachable from Wine?");
                    return NativeFFmpeg::ErrLaunchFailed;
                }
            }

            int elapsedSeconds = elapsedMs / 1000;
            if (onProgress && elapsedSeconds >= lastReported + 5) {
                lastReported = elapsedSeconds;
                onProgress(elapsedSeconds);
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        // NOT a failure: ffmpeg is almost certainly still encoding. The caller must leave
        // the output alone and the job files must survive so the script can finish.
        log::warn("[NativeFFmpeg] ffmpeg has not finished after {}s; leaving it running", timeoutMs / 1000);
        return NativeFFmpeg::ErrTimeout;
    }

    std::string readTail(const std::filesystem::path& path, size_t maxLen = 4000) {
        std::ifstream file(path, std::ios::binary);
        if (!file) return "";

        std::string contents((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (contents.size() > maxLen)
            contents = contents.substr(contents.size() - maxLen);

        return contents;
    }

    void dumpLog(const NativeFFmpeg::ScriptJob& job) {
        std::string shellOutput = readTail(job.shellLogWin);
        if (!shellOutput.empty())
            log::error("[NativeFFmpeg] shell error (not from ffmpeg):\n{}", shellOutput);

        std::string contents = readTail(job.logWin);
        if (!contents.empty())
            log::warn("[NativeFFmpeg] ffmpeg output:\n{}", contents);
        else if (shellOutput.empty())
            log::warn("[NativeFFmpeg] ffmpeg produced no output at all");
    }

    // Only ever call this once the script is known to have finished — deleting these out
    // from under a running /bin/sh means the .done and .log it writes afterwards are
    // recreated and never collected.
    void cleanupJob(const NativeFFmpeg::ScriptJob& job) {
        if (!job.valid) return;
        std::error_code ec;
        std::filesystem::remove(job.scriptWin, ec);
        ec.clear();
        std::filesystem::remove(job.doneWin, ec);
        ec.clear();
        std::filesystem::remove(job.logWin, ec);
        ec.clear();
        std::filesystem::remove(job.startedWin, ec);
        ec.clear();
        std::filesystem::remove(job.shellLogWin, ec);
    }

}

namespace NativeFFmpeg {

    bool isUnderWine() {
        static bool result = [] {
            auto fn = reinterpret_cast<wine_get_version_t>(
                GetProcAddress(GetModuleHandleA("ntdll.dll"), "wine_get_version")
            );
            if (!fn) return false;
            const char* version = fn();
            log::info("[NativeFFmpeg] running under Wine/Proton {}", version ? version : "(unknown)");
            return true;
        }();
        return result;
    }

    bool isEnabled() {
        if (!isUnderWine()) return false;
        Mod* mod = Mod::get();
        if (!mod) return false;
        return mod->getSettingValue<bool>("native_ffmpeg");
    }

    std::optional<std::string> toUnixPath(const std::filesystem::path& winPath) {
        auto fn = getUnixNameFn();
        if (!fn) return std::nullopt;

        std::filesystem::path path = winPath;

        std::error_code ec;
        if (path.is_relative()) {
            std::filesystem::path abs = std::filesystem::absolute(path, ec);
            if (!ec) path = abs;
            ec.clear();
        }

        path = path.lexically_normal();

        // wine_get_unix_file_name fails for files that do not exist yet, so walk up to
        // the closest existing ancestor and re-append what we stripped off.
        std::vector<std::string> tail;

        for (int depth = 0; depth < 64; ++depth) {
            std::wstring wide = path.wstring();
            if (wide.empty()) break;

            // A trailing separator makes the translation fail for drive-relative paths.
            while (wide.size() > 3 && (wide.back() == L'\\' || wide.back() == L'/'))
                wide.pop_back();

            if (char* raw = fn(wide.c_str())) {
                std::string result(raw);
                HeapFree(GetProcessHeap(), 0, raw);

                while (result.size() > 1 && result.back() == '/') result.pop_back();

                // tail holds the stripped components deepest-first, so re-append in reverse.
                for (auto it = tail.rbegin(); it != tail.rend(); ++it) {
                    if (result.empty() || result.back() != '/') result += "/";
                    result += *it;
                }

                return result;
            }

            std::filesystem::path parent = path.parent_path();
            if (parent.empty() || parent == path) break;

            tail.push_back(utf8FromWide(path.filename().wstring()));
            path = parent;
        }

        return std::nullopt;
    }

    std::string shQuote(const std::string& str) {
        std::string out;
        out.reserve(str.size() + 2);
        out += '\'';
        for (char c : str) {
            if (c == '\'') out += "'\\''";
            else out += c;
        }
        out += '\'';
        return out;
    }

    std::string joinArgs(const std::vector<std::string>& parts) {
        std::string out;
        for (const std::string& part : parts) {
            std::string piece = trim(part);
            if (piece.empty()) continue;
            if (!out.empty()) out += " ";
            out += piece;
        }
        return out;
    }

    std::string joinFilters(const std::vector<std::string>& parts) {
        std::string out;
        for (const std::string& part : parts) {
            std::string piece = trimFilter(part);
            if (piece.empty()) continue;
            if (!out.empty()) out += ",";
            out += piece;
        }
        return out;
    }

    bool hasBalancedQuotes(const std::string& args) {
        bool inSingle = false;
        bool inDouble = false;

        for (size_t i = 0; i < args.size(); ++i) {
            char c = args[i];

            if (inSingle) {
                // Nothing at all escapes inside single quotes, not even a backslash.
                if (c == '\'') inSingle = false;
                continue;
            }

            if (inDouble) {
                if (c == '\\' && i + 1 < args.size()) { ++i; continue; }
                if (c == '"') inDouble = false;
                continue;
            }

            if (c == '\\' && i + 1 < args.size()) { ++i; continue; }
            if (c == '\'') inSingle = true;
            else if (c == '"') inDouble = true;
        }

        return !inSingle && !inDouble;
    }

    int runBlocking(const std::string& args, int timeoutMs, ProgressFn onProgress) {
        if (!hasBalancedQuotes(args)) {
            log::error("[NativeFFmpeg] refusing to run: unbalanced quote in the arguments:\n{}", args);
            return ErrBadQuoting;
        }

        ScriptJob job = makeJob("run");
        if (!job.valid) return ErrPathFailed;

        if (!writeScript(job, buildScript(job, args))) {
            cleanupJob(job);
            return ErrScriptFailed;
        }

        log::info("[NativeFFmpeg] executing: ffmpeg {}", args);

        if (!launchScript(job.scriptUnix)) {
            cleanupJob(job);
            return ErrLaunchFailed;
        }

        int code = waitForDone(job, timeoutMs, onProgress);

        // The script is only provably done once it reports an exit code. Anything else —
        // a timeout, or a /bin/sh that was merely slow to be scheduled — means it may
        // still be alive, so its files must survive (the 24h sweep collects them) and its
        // half-written log must not be presented as the reason for a failure.
        if (!hasFinished(code)) return code;

        if (code != 0) dumpLog(job);

        cleanupJob(job);
        return code;
    }

    VideoPipe::~VideoPipe() {
        if (m_started && !m_finished) finish();
        closeSocket();
    }

    void VideoPipe::closeSocket() {
        SOCKET sock = static_cast<SOCKET>(m_socket);
        if (sock != INVALID_SOCKET) closesocket(sock);
        m_socket = static_cast<std::uintptr_t>(-1);
    }

    bool VideoPipe::start(const std::string& argsBeforeInput, const std::string& argsAfterInput) {
        if (m_started) return false;
        if (!ensureWinsock()) return false;

        m_port = findFreePort();
        if (m_port <= 0) {
            log::error("[NativeFFmpeg] could not reserve a loopback port");
            return false;
        }

        m_job = makeJob("video");
        if (!m_job.valid) return false;

        std::string url = fmt::format("tcp://127.0.0.1:{}?listen=1&listen_timeout=20000", m_port);
        std::string args = joinArgs({ argsBeforeInput, "-i", shQuote(url), argsAfterInput });

        if (!hasBalancedQuotes(args)) {
            log::error("[NativeFFmpeg] refusing to run: unbalanced quote in the arguments:\n{}", args);
            cleanupJob(m_job);
            m_job.valid = false;
            return false;
        }

        if (!writeScript(m_job, buildScript(m_job, args))) {
            cleanupJob(m_job);
            m_job.valid = false;
            return false;
        }

        log::info("[NativeFFmpeg] executing: ffmpeg {}", args);

        if (!launchScript(m_job.scriptUnix)) {
            cleanupJob(m_job);
            m_job.valid = false;
            return false;
        }

        // ffmpeg needs roughly a second to bind; in testing the connection landed on
        // attempt 8-12 at 100 ms intervals. The budget must outlast the listen_timeout=20s
        // in the tcp:// URL, otherwise we give up first and report an empty log while
        // ffmpeg is still waiting for us.
        SOCKET sock = INVALID_SOCKET;

        for (int attempt = 0; attempt < 300; ++attempt) {
            // If ffmpeg already exited it will never listen — bail out early.
            if (auto code = readDone(m_job)) {
                log::error("[NativeFFmpeg] ffmpeg exited before accepting a connection (code {})", *code);
                dumpLog(m_job);
                cleanupJob(m_job);
                m_job.valid = false;
                return false;
            }

            sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (sock != INVALID_SOCKET) {
                sockaddr_in addr{};
                addr.sin_family = AF_INET;
                addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                addr.sin_port = htons(static_cast<u_short>(m_port));

                if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
                    log::info("[NativeFFmpeg] connected to ffmpeg on port {} (attempt {})", m_port, attempt + 1);
                    break;
                }

                closesocket(sock);
                sock = INVALID_SOCKET;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        if (sock == INVALID_SOCKET) {
            log::error("[NativeFFmpeg] could not connect to ffmpeg on port {}", m_port);

            // The connect loop above already ran far longer than the startup grace, so
            // this is the honest place to report a shell that never got going. (Asking
            // waitForDone below to decide it would be pointless: its budget here is far
            // shorter than the grace period.)
            std::error_code ec;
            if (!std::filesystem::exists(m_job.startedWin, ec) || ec)
                log::error("[NativeFFmpeg] the launcher script never started — is /bin/sh reachable from Wine?");

            // Give the script a moment to record why, otherwise we would tell the user to
            // check a log that ffmpeg has not written yet.
            int code = waitForDone(m_job, 5000, {}, 0);

            dumpLog(m_job);

            // Only reclaim the files once the script is provably finished.
            if (hasFinished(code)) cleanupJob(m_job);

            m_job.valid = false;
            return false;
        }

        // Without this every send() is unbounded: if ffmpeg stops draining the socket the
        // write blocks forever, which deadlocks the whole render (see writeFrame).
        DWORD sendTimeout = static_cast<DWORD>(kSendTimeoutMs);
        if (setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&sendTimeout), sizeof(sendTimeout)) != 0)
            log::warn("[NativeFFmpeg] could not set a send timeout (winsock error {})", WSAGetLastError());

        m_socket = static_cast<std::uintptr_t>(sock);
        m_started = true;
        m_finished = false;
        m_broken = false;
        return true;
    }

    bool VideoPipe::writeFrame(const void* data, size_t size) {
        if (!m_started || m_finished || m_broken) return false;

        SOCKET sock = static_cast<SOCKET>(m_socket);
        if (sock == INVALID_SOCKET) return false;

        const char* bytes = static_cast<const char*>(data);
        size_t sent = 0;

        while (sent < size) {
            constexpr size_t maxChunk = 0x100000;
            size_t remaining = size - sent;
            int chunk = static_cast<int>(remaining > maxChunk ? maxChunk : remaining);

            int written = send(sock, bytes + sent, chunk, 0);
            if (written <= 0) {
                int error = WSAGetLastError();

                // SO_SNDTIMEO fired: ffmpeg stopped draining the socket (a full disk or a
                // stalled encoder does this) and TCP closed the window. Without the
                // timeout this send would never return, the render thread would never
                // clear frameHasData, and the main thread would spin on it forever.
                if (error == WSAETIMEDOUT)
                    log::error("[NativeFFmpeg] ffmpeg stopped reading frames for {}s — treating the connection as broken", kSendTimeoutMs / 1000);
                else
                    log::error("[NativeFFmpeg] send() failed (winsock error {})", error);

                // Windows leaves a timed-out socket in an indeterminate state, so it can
                // never be written to again.
                m_broken = true;
                return false;
            }

            sent += static_cast<size_t>(written);
        }

        return true;
    }

    int VideoPipe::finish(int timeoutMs, ProgressFn onProgress) {
        if (!m_started) return ErrNotStarted;
        if (m_finished) return ErrNotStarted;

        m_finished = true;

        SOCKET sock = static_cast<SOCKET>(m_socket);
        if (sock != INVALID_SOCKET) shutdown(sock, SD_SEND);
        closeSocket();

        int code = waitForDone(m_job, timeoutMs, onProgress, 0);
        m_started = false;

        // See runBlocking: unless the script reported an exit code it may still be alive,
        // so leave its files (and its still-being-written log) alone.
        if (!hasFinished(code)) return code;

        if (code != 0) dumpLog(m_job);

        cleanupJob(m_job);
        m_job.valid = false;

        return code;
    }

}

#endif
