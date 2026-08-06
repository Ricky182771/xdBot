#include "../includes.hpp"
#include "ffmpeg/events.hpp"

enum AudioMode {
    Off = 0,
    Song = 1,
    Record = 2
};

// Which encoder the renderer feeds its frames to.
//
// NOTE: this used to be the single `usingApi` bool, which conflated the encoder choice
// with the OpenGL capture path. They are separate concerns now — use `usesCoreGL()` for
// the latter. FFmpegAPI and NativeUnix both want the core GL functions; only the
// ffmpeg.exe backend uses the EXT variants.
enum class VideoBackend {
    FFmpegAPI,  // the in-process `eclipse.ffmpeg-api` mod
    WindowsExe, // an ffmpeg.exe subprocess fed through stdin
    NativeUnix  // the host's native Linux ffmpeg, reached through Wine (see native_ffmpeg.hpp)
};

class MyRenderTexture {
public:
    unsigned width, height;
    int old_fbo, old_rbo;
    unsigned fbo;
    geode::prelude::CCTexture2D* texture = nullptr;
    void begin();
    void capture(std::mutex& lock, std::vector<uint8_t>& data, volatile bool& lul);
};

class Renderer {
public:

    Renderer() : width(1920), height(1080), fps(60) {}

    volatile bool frameHasData;
    // Set by the encoder thread when it gives up. Renderer::captureFrame busy-waits on
    // frameHasData from the *main thread*, and only the encoder thread ever clears it —
    // so if the encoder dies mid-render the game would spin at 100% forever. This is the
    // escape hatch for that wait.
    volatile bool encoderFailed = false;
    bool levelFinished = false;
    bool recording = false;
    bool pause = false;
    int audioMode = 0;
    float ogMusicVol;
    float ogSFXVol;
    float SFXVolume = 1.f;
    float musicVolume = 1.f;

#ifdef GEODE_IS_WINDOWS
    VideoBackend backend = VideoBackend::WindowsExe;
#else
    VideoBackend backend = VideoBackend::FFmpegAPI;
#endif

    // The ffmpeg.exe backend is the only one that captures through the EXT framebuffer
    // functions; everything else uses the core ones.
    bool usesCoreGL() const { return backend != VideoBackend::WindowsExe; }

    bool dontRender = false;
    bool dontRecordAudio = false;
    bool recordingAudio = false;
    bool startedAudio = false;
    bool isPlatformer = false;
    int finishFrame = 0;
    int levelStartFrame = 0;

    float stopAfter = 3.f;
    float timeAfter = 0.f;
    unsigned width, height;
    unsigned fps;
    double lastFrame_t, extra_t;
    int pauseAttempts = 0;

    MyRenderTexture renderer;
    ffmpeg::events::Recorder ffmpeg;
    std::vector<uint8_t> currentFrame;
    std::mutex lock;
    std::string codec = "", bitrate = "12M", extraArgs = "", videoArgs = "", extraAudioArgs = "", path = "";
    std::string ffmpegPath = (geode::dirs::getGameDir() / "ffmpeg.exe").string();
    std::unordered_set<int> renderedFrames;

    FMODAudioEngine* fmod = nullptr;
    cocos2d::CCSize ogRes = {0, 0};
    float ogScaleX = 1.f;
    float ogScaleY = 1.f;

    void captureFrame();
    void changeRes(bool og);

    void start();
    void startAudio(PlayLayer* pl);

    void stop(int frame = 0);
    void stopAudio();

    void handleRecording(PlayLayer* pl, int frame);
    void handleAudioRecording(PlayLayer* pl, int frame);
    
    static bool toggle();
    static VideoBackend selectBackend();
    // True only for the limited in-process FFmpeg API backend — the UI uses this to grey
    // out the settings that backend cannot honour.
    static bool shouldUseAPI();

    // Defaults for the native backend's encoder args / extra filters. These live in the
    // render settings (saved values), not in mod.json, so they can be edited from the
    // render options popup.
    static constexpr const char* defaultNativeArgs = "-vaapi_device /dev/dri/renderD128 -c:v h264_vaapi";
    static constexpr const char* defaultNativeFilters = "format=nv12,hwupload";

    // Seeds render_native_args/render_native_filters once, carrying over the values from
    // the old mod.json settings if the user had customised them.
    static void ensureNativeArgsMigrated();

    // True if `args` sets a video codec with any of ffmpeg's spellings (-c:v, -codec:v,
    // -vcodec), matched as whole whitespace-delimited tokens.
    static bool hasVideoCodecArg(const std::string& args);

    // True if the frames leave `filters` on the GPU, decided by the last hardware
    // transfer in the chain — an output -pix_fmt can no longer be applied to those.
    static bool hasHardwareUpload(const std::string& filters);

    // Returns `args` with any "-pix_fmt <value>" / "-pixel_format <value>" pair removed.
    static std::string stripPixelFormatArg(const std::string& args);
    bool tryPause();
};