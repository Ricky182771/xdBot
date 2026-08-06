#pragma once

// Native (Linux) FFmpeg backend for the renderer.
//
// When Geometry Dash runs under Wine/Proton the mod is a Windows PE, so both of the
// existing backends (the in-process FFmpeg API and an ffmpeg.exe subprocess) are stuck
// with software encoding. This backend instead talks to the *host's* native ffmpeg:
//
//   1. a small /bin/sh script is generated into the mod's save dir,
//   2. it is launched with Wine's `start.exe /unix /bin/sh <script>`,
//   3. raw RGB24 frames are streamed to it over loopback TCP (ffmpeg listens, we connect),
//   4. the script writes ffmpeg's exit code into a "done" marker file we poll for.
//
// A pipe/FIFO is not usable here: start.exe returns immediately and does not let us
// inherit its stdin. Loopback TCP works because the pressure-vessel container shares
// the host network namespace.
//
// All of this only exists on Windows builds (Android never runs under Wine and has
// neither Winsock nor the Wine helper exports).

#include <Geode/DefaultInclude.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#ifdef GEODE_IS_WINDOWS

namespace NativeFFmpeg {

    // Negative results from runBlocking()/VideoPipe::finish(). Real ffmpeg exit codes
    // come from `echo $?` and are 0..255, so these can never collide.
    //
    // ErrTimeout is special: it does NOT mean ffmpeg failed, only that it has not
    // finished yet. The process is very likely still running and still writing its
    // output, so callers must not treat it as an error and must not delete/rename
    // anything. Everything else is a genuine failure.
    constexpr int ErrTimeout      = -1;
    constexpr int ErrLaunchFailed = -2;
    constexpr int ErrScriptFailed = -3;
    constexpr int ErrPathFailed   = -4;
    constexpr int ErrSocketFailed = -5;
    constexpr int ErrNotStarted   = -6;
    constexpr int ErrBadQuoting   = -7;

    // How long a single send() may stall before the connection is declared dead. Long
    // enough that a busy-but-healthy encoder never trips it, short enough that a hung one
    // surfaces as a failed render instead of a frozen game.
    constexpr int kSendTimeoutMs = 30000;

    // True when ffmpeg may still be working — see ErrTimeout above.
    inline bool isStillRunning(int code) { return code == ErrTimeout; }

    // True once the script has provably finished (a real exit code came back). Job files
    // must never be deleted while this is false: /bin/sh may simply have been slow to be
    // scheduled and would then write its .done/.log into files nobody collects.
    inline bool hasFinished(int code) { return code >= 0; }

    // A shell-syntax sanity check on the user's own argument fragments. They are
    // interpolated into /bin/sh unquoted on purpose (word-splitting is how multi-token
    // args reach ffmpeg), so an unbalanced quote turns into a shell syntax error and the
    // render dies with an empty log and nothing to show the user.
    bool hasBalancedQuotes(const std::string& args);

    // Invoked roughly every 5 seconds while waiting, with the elapsed whole seconds.
    using ProgressFn = std::function<void(int elapsedSeconds)>;

    // A generated script and its companion files, in both Windows and Unix flavours.
    struct ScriptJob {
        std::filesystem::path scriptWin;
        std::filesystem::path doneWin;
        std::filesystem::path logWin;
        std::filesystem::path startedWin;
        std::filesystem::path shellLogWin;
        std::string scriptUnix;
        std::string doneUnix;
        std::string logUnix;
        std::string startedUnix;
        std::string shellLogUnix;
        bool valid = false;
    };

    // ntdll!wine_get_version — non-null means we are running under Wine/Proton.
    bool isUnderWine();

    // isUnderWine() && the "native_ffmpeg" setting is enabled.
    bool isEnabled();

    // Windows -> Unix path translation through kernel32!wine_get_unix_file_name.
    // Files that do not exist yet cannot be translated directly, so the closest
    // existing ancestor is translated and the remaining components are appended.
    std::optional<std::string> toUnixPath(const std::filesystem::path& winPath);

    // Wraps s in single quotes, escaping embedded ones ('\'').
    std::string shQuote(const std::string& str);

    // Joins the non-empty (trimmed) parts with a single space.
    std::string joinArgs(const std::vector<std::string>& parts);

    // Joins the non-empty (trimmed, comma-stripped) parts with a single comma,
    // producing a filter chain that never has a doubled or trailing comma.
    std::string joinFilters(const std::vector<std::string>& parts);

    // Runs `ffmpeg <args>` and blocks until it exits. Returns ffmpeg's exit code,
    // or a negative value if it could not be launched / timed out.
    // Every path inside `args` must already be a shQuote'd Unix path.
    int runBlocking(const std::string& args, int timeoutMs = 1800000, ProgressFn onProgress = {});

    // Streaming video encode: raw frames go to ffmpeg over loopback TCP.
    class VideoPipe {
    public:
        VideoPipe() = default;
        ~VideoPipe();

        VideoPipe(const VideoPipe&) = delete;
        VideoPipe& operator=(const VideoPipe&) = delete;

        // argsBeforeInput ends up in front of the generated `-i tcp://...`,
        // argsAfterInput after it.
        bool start(const std::string& argsBeforeInput, const std::string& argsAfterInput);

        bool writeFrame(const void* data, size_t size);
        bool writeFrame(const std::vector<uint8_t>& frame) { return writeFrame(frame.data(), frame.size()); }

        // Closes the socket, waits for the done marker and returns ffmpeg's exit code
        // (negative if it never reported one). ErrTimeout leaves the job files in place
        // because ffmpeg is presumed to still be writing.
        int finish(int timeoutMs = 600000, ProgressFn onProgress = {});

        bool isStarted() const { return m_started; }

    private:
        void closeSocket();

        std::uintptr_t m_socket = static_cast<std::uintptr_t>(-1); // SOCKET / INVALID_SOCKET
        int m_port = 0;
        bool m_started = false;
        bool m_finished = false;
        bool m_broken = false;
        ScriptJob m_job;
    };

}

#endif
