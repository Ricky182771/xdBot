#include "../includes.hpp"
#include "../ui/game_ui.hpp"
#include "../utils/subprocess.hpp"
#include "native_ffmpeg.hpp"

#include <Geode/modify/FMODAudioEngine.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/EndLevelLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <Geode/modify/CCParticleSystemQuad.hpp>
#include <Geode/modify/CCCircleWave.hpp>
#include <Geode/utils/web.hpp>

#include <sstream>
#include <filesystem>
#include <fstream>
#include <thread>

class $modify(CCParticleSystemQuad) {

    static CCParticleSystemQuad* create(const char* v1, bool v2) {
        CCParticleSystemQuad* ret = CCParticleSystemQuad::create(v1, v2);
        if (!Global::get().renderer.recording) return ret;

        if (std::string_view(v1) == "levelComplete01.plist" && Mod::get()->getSavedValue<bool>("render_hide_levelcomplete"))
            ret->setVisible(false);

        return ret;
    }

};

class $modify(CCCircleWave) {

    static CCCircleWave* create(float v1, float v2, float v3, bool v4, bool v5) {
        CCCircleWave* ret = CCCircleWave::create(v1, v2, v3, v4, v5);

        if (!Global::get().renderer.recording || !PlayLayer::get()->m_levelEndAnimationStarted) return ret;

        if (Mod::get()->getSavedValue<bool>("render_hide_levelcomplete"))
            ret->setVisible(false);

        return ret;
    }

};

class $modify(PlayLayer) {

    void showCompleteText() {
        PlayLayer::showCompleteText();
        if (!Global::get().renderer.recording) return;

        if (m_levelEndAnimationStarted && Mod::get()->getSavedValue<bool>("render_hide_levelcomplete")) {
            for (CCNode* node : CCArrayExt<CCNode*>(getChildren())) {
                CCSprite* spr = typeinfo_cast<CCSprite*>(node);
                if (!spr) continue;
                if (!isSpriteFrameName(spr, "GJ_levelComplete_001.png")) continue;
                spr->setVisible(false);
            }
        }
    }
    
};

class $modify(EndLevelLayer) {
    
    void customSetup() {
        EndLevelLayer::customSetup();

        if (!PlayLayer::get()) return;
        if (Global::get().renderer.recording && PlayLayer::get()->m_levelEndAnimationStarted && Mod::get()->getSavedValue<bool>("render_hide_endscreen")) {
            Loader::get()->queueInMainThread([this] {
                setVisible(false);
            });
        }
    }

};

class $modify(FMODAudioEngine) {

    int playEffect(gd::string path, float speed, float p2, float volume) {
        if (path == "explode_11.ogg" && Global::get().renderer.recording) return 0;

        if (path != "playSound_01.ogg" || !Global::get().renderer.recordingAudio)
            return FMODAudioEngine::playEffect(path, speed, p2, volume);

        // playSound_01.ogg while recording audio is intentionally suppressed
        // (captured separately). v5 builds with -Werror=return-type, so the
        // previously-implicit fall-through must now return explicitly.
        return 0;
    }

};

class $modify(GJBaseGameLayer) {
    void update(float dt) {
        GJBaseGameLayer::update(dt);
        auto& g = Global::get();

        PlayLayer* pl = PlayLayer::get();

        if ((!g.renderer.recording && !g.renderer.recordingAudio) || !pl) return;
        
        int frame = Global::getCurrentFrame();

        if (g.renderer.recording && frame % static_cast<int>(Global::getTPS() / g.renderer.fps) == 0)
            return g.renderer.handleRecording(pl, frame);

        if (g.renderer.recordingAudio && !g.renderer.startedAudio) {
            return g.renderer.startAudio(pl);
        }

        if (g.renderer.recordingAudio && frame % static_cast<int>(Global::getTPS() / g.renderer.fps) == 0)
            return g.renderer.handleAudioRecording(pl, frame);
    }
};

float leftOver = 0.f;

class $modify(CCScheduler) {

    void update(float dt) {
        Renderer& r = Global::get().renderer;
        if (!r.recording) return CCScheduler::update(dt);

        r.changeRes(false);

        using namespace std::literals;
        
        float newDt = 1.f / Global::getTPS();

        auto startTime = std::chrono::high_resolution_clock::now();
        int mult = static_cast<int>((dt + leftOver) / newDt);

        for (int i = 0; i < mult; ++i) {
            CCScheduler::update(newDt);
            if (std::chrono::high_resolution_clock::now() - startTime > 33.333ms) {
                mult = i + 1;
                break;
            }
        }
        
        leftOver += (dt - newDt * mult);

        r.changeRes(true);
    }

};

VideoBackend Renderer::selectBackend() {
#ifdef GEODE_IS_WINDOWS

    // Running under Wine/Proton with the setting on beats everything else: it is the only
    // way to reach hardware encoders from a Windows build.
    if (NativeFFmpeg::isEnabled()) return VideoBackend::NativeUnix;

    bool foundApi = Loader::get()->isModLoaded("eclipse.ffmpeg-api");
    std::filesystem::path ffmpegPath = Mod::get()->getSettingValue<std::filesystem::path>("ffmpeg_path");
    bool foundExe = std::filesystem::exists(ffmpegPath) && ffmpegPath.filename().string() == "ffmpeg.exe";

    if (foundExe) return VideoBackend::WindowsExe;
    if (foundApi) return VideoBackend::FFmpegAPI;

    // Nothing available — Renderer::toggle() reports this to the user before we get to
    // actually encoding anything.
    return VideoBackend::WindowsExe;

#else

    return VideoBackend::FFmpegAPI;

#endif
}

bool Renderer::shouldUseAPI() {
    return Renderer::selectBackend() == VideoBackend::FFmpegAPI;
}

bool Renderer::hasVideoCodecArg(const std::string& args) {
    // Substring matching is not enough: "-c:v" would miss "-vcodec" entirely, and the
    // resulting duplicate codec silently loses to whichever ffmpeg parses last.
    std::istringstream stream(args);
    std::string token;

    while (stream >> token) {
        if (token == "-c:v" || token == "-codec:v" || token == "-vcodec") return true;
    }

    return false;
}

bool Renderer::hasHardwareUpload(const std::string& filters) {
    // The question is not "does the chain mention hwupload" but "do the frames leave the
    // chain on the GPU". A chain can go up and back down again
    // (format=nv12,hwupload,scale_vaapi=...,hwdownload,format=nv12 ends in software
    // frames and does still want an output pixel format), so only the *last* hardware
    // transfer decides.
    bool onGpu = false;

    std::stringstream stream(filters);
    std::string filter;

    // Filter options are separated by ':', so a plain split on ',' correctly walks a
    // linear chain. Labelled filtergraphs are not something this renderer builds.
    while (std::getline(stream, filter, ',')) {
        size_t start = filter.find_first_not_of(" \t");
        if (start == std::string::npos) continue;
        size_t end = filter.find_last_not_of(" \t");
        filter = filter.substr(start, end - start + 1);

        std::string name = filter.substr(0, filter.find('='));

        if (name == "hwdownload") onGpu = false;
        else if (name == "hwupload" || name == "hwupload_cuda") onGpu = true;
        // hwmap goes either way; mode=read (and read+write) maps the frames back for CPU
        // access, anything else maps up to the device.
        else if (name == "hwmap") onGpu = filter.find("mode=read") == std::string::npos;
    }

    return onGpu;
}

std::string Renderer::stripPixelFormatArg(const std::string& args) {
    // Removing the option is not the same as declining to add the default: xdBot seeds
    // render_args to "-pix_fmt yuv420p" for every user on first launch, so the value is
    // essentially always present and has to be taken back out for a hardware chain.
    // The matched tokens are spliced out of the original string rather than the string
    // being rebuilt from its tokens: rebuilding would re-join everything with a single
    // space and silently rewrite arguments this function is not supposed to touch
    // (-metadata title="my  render" would come back out with one space).
    constexpr const char* whitespace = " \t\r\n\f\v";

    std::string result = args;
    size_t pos = 0;

    while (true) {
        size_t start = result.find_first_not_of(whitespace, pos);
        if (start == std::string::npos) break;

        size_t end = result.find_first_of(whitespace, start);
        std::string token = result.substr(start, end == std::string::npos ? std::string::npos : end - start);

        if (token != "-pix_fmt" && token != "-pixel_format") {
            if (end == std::string::npos) break;
            pos = end;
            continue;
        }

        // The option's value, if it has one — a trailing "-pix_fmt" with nothing after it
        // is dropped on its own.
        size_t cutEnd = end;
        if (cutEnd != std::string::npos) {
            size_t valueStart = result.find_first_not_of(whitespace, cutEnd);
            cutEnd = (valueStart == std::string::npos)
                ? std::string::npos
                : result.find_first_of(whitespace, valueStart);
        }

        size_t eraseFrom = start;
        size_t eraseTo = result.size();

        if (cutEnd != std::string::npos) {
            // Swallow the separator up to the next argument, so the ones that stay keep
            // exactly the spacing they had.
            size_t next = result.find_first_not_of(whitespace, cutEnd);
            if (next != std::string::npos) eraseTo = next;
        }

        // Nothing follows, so the separator to swallow is the one in front instead.
        if (eraseTo >= result.size() && eraseFrom > 0) {
            size_t prevEnd = result.find_last_not_of(whitespace, eraseFrom - 1);
            eraseFrom = (prevEnd == std::string::npos) ? 0 : prevEnd + 1;
        }

        result.erase(eraseFrom, eraseTo - eraseFrom);
        pos = eraseFrom;
    }

    return result;
}

void Renderer::ensureNativeArgsMigrated() {
#ifdef GEODE_IS_WINDOWS

    Mod* mod = Mod::get();
    if (!mod || mod->getSavedValue<bool>("render_native_migrated")) return;

    // These used to be mod.json settings. They now live with the rest of the render
    // options, but a user who customised them must not silently lose that: read whatever
    // Geode persisted for the old settings before falling back to the defaults.
    auto legacyValue = [mod](const char* key, const char* fallback) -> std::string {
        // Still registered (older mod.json)? Then that is authoritative.
        std::string current = mod->getSettingValue<std::string>(key);
        if (!current.empty()) return current;

        const matjson::Value& data = mod->getSavedSettingsData();
        if (data.contains(key)) {
            std::string saved = data[key].asString().unwrapOr(std::string());
            if (!saved.empty()) return saved;
        }

        return fallback;
    };

    mod->setSavedValue("render_native_args", legacyValue("native_ffmpeg_args", Renderer::defaultNativeArgs));
    mod->setSavedValue("render_native_filters", legacyValue("native_ffmpeg_filters", Renderer::defaultNativeFilters));
    mod->setSavedValue("render_native_migrated", true);

#endif
}

bool Renderer::toggle() {
    auto& g = Global::get();
    if (Loader::get()->isModLoaded("syzzi.click_between_frames")) {
        FLAlertLayer::create("Render", "Disable CBF in Geode to render a level.", "OK")->show();
        return false;
    }

    bool foundApi = Loader::get()->isModLoaded("eclipse.ffmpeg-api");
    std::filesystem::path ffmpegPath = Mod::get()->getSettingValue<std::filesystem::path>("ffmpeg_path");
    bool foundExe = std::filesystem::exists(ffmpegPath) && ffmpegPath.filename().string() == "ffmpeg.exe";

    g.renderer.backend = Renderer::selectBackend();

    if (g.renderer.recording || g.renderer.recordingAudio) {
        g.renderer.recordingAudio ? g.renderer.stopAudio() : g.renderer.stop(Global::getCurrentFrame());
    }
    else {

#ifdef GEODE_IS_WINDOWS
        bool foundNative = g.renderer.backend == VideoBackend::NativeUnix;

        if (!foundExe && !foundApi && !foundNative) {
            std::string message = "<cl>FFmpeg</c> not found, set the path to the .exe in mod settings or install FFmpeg API.";

            // Under Wine/Proton there is a third option, so do not tell the user they are stuck.
            if (NativeFFmpeg::isUnderWine())
                message += "\nYou're on <cy>Wine/Proton</c>: enable <cl>Use Native FFmpeg</c> in mod settings to use your system's ffmpeg instead.";

            message += "\nOpen download link?";

            geode::createQuickPopup(
                "Error",
                message.c_str(),
                "Cancel", "Yes",
                [](auto, bool btn2) {
                    if (btn2) {
                        FLAlertLayer::create("Info", "Unzip the downloaded file and look for <cl>ffmpeg.exe</c> in the 'bin' folder.", "Ok")->show();
                        utils::web::openLinkInBrowser("https://www.gyan.dev/ffmpeg/builds/ffmpeg-git-essentials.7z");
                    }
                }
            );
            return false;
        }

        g.renderer.ffmpegPath = ffmpegPath.string();
#else
        if (!foundApi) {
            FLAlertLayer::create("Error", "<cl>FFmpeg API</c> not found. Download it to render a level.", "Ok")->show();
            return false;
        }
#endif

        if (!PlayLayer::get()) {
            FLAlertLayer::create("Warning", "<cl>Open a level</c> to start rendering it.", "Ok")->show();
            return false;
        }

        std::filesystem::path path = Mod::get()->getSettingValue<std::filesystem::path>("render_folder");

        if (std::filesystem::exists(path))
            g.renderer.start();
        else {
            if (utils::file::createDirectoryAll(path).isOk())
                g.renderer.start();
            else {
                FLAlertLayer::create("Error", "There was an error getting the renders folder. ID: 11", "Ok")->show();
                return false;
            }
        }
    }

    Interface::updateLabels();

    return true;
}

void Renderer::start() {
    PlayLayer* pl = PlayLayer::get();
    GameManager* gm = GameManager::sharedState();
    Mod* mod = Mod::get();
    fmod = FMODAudioEngine::sharedEngine();

    fps = std::stoi(mod->getSavedValue<std::string>("render_fps"));
    codec = mod->getSavedValue<std::string>("render_codec");
    bitrate = mod->getSavedValue<std::string>("render_bitrate") + "M";
    extraArgs = mod->getSavedValue<std::string>("render_args");
    videoArgs = mod->getSavedValue<std::string>("render_video_args");
    extraAudioArgs = mod->getSavedValue<std::string>("render_audio_args");
    SFXVolume = mod->getSavedValue<double>("render_sfx_volume");
    musicVolume = mod->getSavedValue<double>("render_music_volume");
    stopAfter = geode::utils::numFromString<float>(mod->getSavedValue<std::string>("render_seconds_after")).unwrapOr(0.f);
    audioMode = AudioMode::Off;
    std::string extension = mod->getSavedValue<std::string>("render_file_extension");

    if (mod->getSavedValue<bool>("render_only_song")) audioMode = AudioMode::Song;
    if (mod->getSavedValue<bool>("render_record_audio")) audioMode = AudioMode::Record;

    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
    
    std::string filename = fmt::format("render_{}_{}{}", std::string_view(pl->m_level->m_levelName), std::to_string(timestamp), extension);

    // `filename` is UTF-8 (it embeds the level name straight out of GD), but constructing
    // a path from a narrow string decodes it as the ANSI codepage. Build the real path
    // once, from the widened name, and keep it: `path` below is the lossy narrow form the
    // existing backends have always used, and round-tripping back through it would turn
    // any non-ASCII level name into U+FFFD.
    std::filesystem::path pathReal =
        Mod::get()->getSettingValue<std::filesystem::path>("render_folder") / std::filesystem::path(Utils::widen(filename));
    std::string path = pathReal.string();

    width = std::stoi(mod->getSavedValue<std::string>("render_width2"));
    height = std::stoi(mod->getSavedValue<std::string>("render_height"));

    if (width % 2 != 0)
        width++;
    if (height % 2 != 0)
        height++;

    renderer.width = width;
    renderer.height = height;
    ogRes = cocos2d::CCEGLView::get()->getDesignResolutionSize();
    ogScaleX = cocos2d::CCEGLView::get()->m_fScaleX;
    ogScaleY = cocos2d::CCEGLView::get()->m_fScaleY;

    dontRender = true;
    recording = true;
    frameHasData = false;
    encoderFailed = false;
    levelFinished = false;
    startedAudio = false;
    timeAfter = 0.f;
    finishFrame = 0;
    pauseAttempts = 0;
    lastFrame_t = extra_t = 0;

    if (!pl->m_levelEndAnimationStarted && pl->m_isPaused)
        Global::get().restart = true;

    if (Global::get().state != state::playing && !Global::get().macro.inputs.empty())
        Macro::togglePlaying();

    std::string songFile = pl->m_level->getAudioFileName();
    if (pl->m_level->m_songID == 0)
        songFile = cocos2d::CCFileUtils::sharedFileUtils()->fullPathForFilename(songFile.c_str(), false);

    float songOffset = pl->m_levelSettings->m_songOffset + (fmod->m_musicOffset / 1000.f) + (levelStartFrame / Global::getTPS());
    bool fadeIn = pl->m_levelSettings->m_fadeIn;
    bool fadeOut = pl->m_levelSettings->m_fadeOut;
    int bitrateApi = geode::utils::numFromString<int64_t>(mod->getSavedValue<std::string>("render_bitrate")).unwrapOr(30) * 1000000;

    currentFrame.resize(width * height * 3, 0);
    renderedFrames.clear();
    renderer.begin();
    changeRes(false);

    ffmpeg::RenderSettings settings;
    settings.m_pixelFormat = ffmpeg::PixelFormat::RGB24;
    settings.m_codec = codec;
    settings.m_bitrate = bitrateApi;
    settings.m_width = width;
    settings.m_height = height;
    settings.m_fps = fps;
    settings.m_outputFile = path;
    settings.m_colorspaceFilters = videoArgs;

    if (!Mod::get()->setSavedValue("first_render_", true)) {
        FLAlertLayer::create(
            "Warning",
            "If you have a macro for the level, <cl>let it run</c> to allow the level to render.",
            "Ok"
        )->show();
    }

    std::thread([&, path, pathReal, songFile, songOffset, fadeIn, fadeOut, extension, bitrateApi, settings]() {
        bool nativeBackend = false;
        std::string nativeArgs;
        std::string nativeFilters;

        #ifdef GEODE_IS_WINDOWS
        nativeBackend = backend == VideoBackend::NativeUnix;
        if (nativeBackend) {
            Renderer::ensureNativeArgsMigrated();
            nativeArgs = Mod::get()->getSavedValue<std::string>("render_native_args");
            nativeFilters = Mod::get()->getSavedValue<std::string>("render_native_filters");
        }
        #endif

        if (!codec.empty()) codec = "-c:v " + codec + " ";
        if (!bitrate.empty()) bitrate = "-b:v " + bitrate + " ";
        if (videoArgs.empty()) videoArgs = "colorspace=all=bt709:iall=bt470bg:fast=1";

        // native args normally carry their own video codec (h264_vaapi, ...); emitting the
        // render-settings codec as well would put two of them on the command line and
        // ffmpeg would silently keep the last, undoing the user's choice. Extra Args counts
        // too: before these settings had their own popup it was the obvious place to put
        // "-c:v h264_vaapi", so plenty of working configs have it there.
        std::string nativeCodec = Renderer::hasVideoCodecArg(nativeArgs + " " + extraArgs) ? "" : codec;

        float fadeInTime = geode::utils::numFromString<float>(Mod::get()->getSavedValue<std::string>("render_fade_in_time")).unwrapOr(0.f);
        bool fadeInVideo = Mod::get()->getSavedValue<bool>("render_fade_in") && fadeInTime != 0.f;
        float fadeOutTime = geode::utils::numFromString<float>(Mod::get()->getSavedValue<std::string>("render_fade_out_time")).unwrapOr(0.f);
        bool fadeOutVideo = Mod::get()->getSavedValue<bool>("render_fade_out") && fadeOutTime != 0.f;

        std::string fadeArgs;
        std::string command;
        #ifdef GEODE_IS_WINDOWS
        subprocess::Popen process;
        NativeFFmpeg::VideoPipe pipe;
        bool pipeBroke = false;
        #endif

        // The lossless path, straight from render_folder / filename — never reconstructed
        // from the narrow `path`, whose encoding the existing backends round-trip through
        // the ANSI codepage. Every native-backend filesystem/ffmpeg operation uses this.
        // Declared unconditionally so the capture is always used; only native reads it.
        [[maybe_unused]] const std::filesystem::path& videoPathW = pathReal;

        // Every give-up path in this thread MUST go through here. captureFrame() spins on
        // the main thread until either frameHasData clears or encoderFailed is set, and the
        // encoder thread is the only thing that can do either — so a bail-out that forgets
        // the flag (especially one that happens before the consume loop below is even
        // reached) freezes the game solid, with no way to show a popup and no way out but
        // killing the process. Routing every exit through one helper means it cannot be
        // forgotten the next time a failure path is added.
        auto abortRender = [&] {
            encoderFailed = true;
            audioMode = AudioMode::Off;
            stop();
        };

        if (fadeInVideo)
            fadeArgs = fmt::format(",fade=t=in:st=0:d={}", fadeInTime);

        // An output pixel format cannot be applied to frames that leave the filter chain on
        // the GPU. It is not enough to decline to add the default: xdBot seeds render_args
        // to "-pix_fmt yuv420p" on first launch (and Restore Defaults writes it back), so
        // the user's own value is essentially always there and has to be taken out.
        //
        // For a software chain it must stay: without it libavfilter negotiates yuv444p out
        // of the colorspace filter and x264 writes High 4:4:4 Predictive, which most
        // players and NLEs cannot open.
        std::string effectiveChain = videoArgs + "," + nativeFilters;

        if (nativeBackend && Renderer::hasHardwareUpload(effectiveChain))
            extraArgs = Renderer::stripPixelFormatArg(extraArgs);
        else if (extraArgs.empty())
            extraArgs = "-pix_fmt yuv420p";

        #ifdef GEODE_IS_WINDOWS
        if (nativeBackend) {
            // These fragments — and only these — are interpolated into /bin/sh unquoted,
            // because word splitting is how a multi-token argument string reaches ffmpeg at
            // all. That makes an unbalanced quote a shell syntax error, which would
            // otherwise kill the render with nothing to show for it.
            //
            // Video Args and Native FFmpeg Filters are deliberately NOT checked: they only
            // ever reach the script through NativeFFmpeg::shQuote (as part of the -vf
            // chain), where a lone ' becomes '\'' and is inert to the shell. Checking them
            // would reject filters like drawtext=text=don't that ffmpeg accepts happily —
            // and that the ffmpeg.exe backend renders without complaint.
            const std::vector<std::pair<std::string, std::string>> fragments = {
                { "Extra Args", extraArgs },
                { "Audio Args", extraAudioArgs },
                { "Native FFmpeg Args", nativeArgs }
            };

            for (const auto& fragment : fragments) {
                if (NativeFFmpeg::hasBalancedQuotes(fragment.second)) continue;

                log::error("Unbalanced quote in {}: {}", fragment.first, fragment.second);

                std::string message = fmt::format(
                    "<cy>{}</c> has an unbalanced quote, which the native FFmpeg backend cannot run.\nFix it in the render settings and try again.",
                    fragment.first
                );

                Loader::get()->queueInMainThread([message] {
                    FLAlertLayer::create("Error", message.c_str(), "Ok")->show();
                });

                return abortRender();
            }
        }
        #endif

        if (backend == VideoBackend::FFmpegAPI) {
            auto res = ffmpeg.init(settings);
            if (res.isErr()) {
                Loader::get()->queueInMainThread([] {
                    // std::string err = res.unwrapErr();
                    FLAlertLayer::create("Error", "FFmpeg API failed to initialize: ", "Ok")->show();
                });

                return abortRender();
            }

        } else {
            #ifdef GEODE_IS_WINDOWS
            if (nativeBackend) {
                auto outUnix = NativeFFmpeg::toUnixPath(videoPathW);
                if (!outUnix) {
                    Loader::get()->queueInMainThread([] {
                        FLAlertLayer::create("Error", "Could not translate the render path for native FFmpeg.", "Ok")->show();
                    });

                    return abortRender();
                }

                // Build the chain as pieces and join them: a stray or doubled comma makes
                // ffmpeg bail out, and a second -vf would silently override the first.
                std::string filters = NativeFFmpeg::joinFilters({ "vflip", videoArgs, fadeArgs, nativeFilters });

                std::string beforeInput = fmt::format(
                    "-y -hide_banner -f rawvideo -pix_fmt rgb24 -s {}x{} -r {}",
                    width, height, fps
                );

                std::string afterInput = NativeFFmpeg::joinArgs({
                    nativeCodec,
                    nativeArgs,
                    bitrate,
                    extraArgs,
                    "-vf", NativeFFmpeg::shQuote(filters),
                    "-an", NativeFFmpeg::shQuote(*outUnix)
                });

                if (!pipe.start(beforeInput, afterInput)) {
                    Loader::get()->queueInMainThread([] {
                        FLAlertLayer::create("Error", "Native <cl>FFmpeg</c> failed to start. Check the mod's log for details.", "Ok")->show();
                    });

                    return abortRender();
                }
            }
            else {
                command = fmt::format(
                    "\"{}\" -y -f rawvideo -pix_fmt rgb24 -s {}x{} -r {} -i - {}{}{} -vf \"vflip,{}{}\" -an \"{}\" ",
                    ffmpegPath,
                    std::to_string(width),
                    std::to_string(height),
                    std::to_string(fps),
                    codec,
                    bitrate,
                    extraArgs,
                    videoArgs,
                    fadeArgs,
                    path
                );

                log::info("Executing: {}", command);

                process = subprocess::Popen(command);
            }
            #endif
        }

        while (recording || pause || recordingAudio || frameHasData) {
            lock.lock();
            if (frameHasData) {
                const std::vector<uint8_t> frame = currentFrame;
                frameHasData = false;
                lock.unlock();
                if (backend == VideoBackend::FFmpegAPI) {
                    auto res = ffmpeg.writeFrame(frame);
                    if (res.isErr()) {
                        Loader::get()->queueInMainThread([] {
                            FLAlertLayer::create("Error", "FFmpeg API failed: ", "Ok")->show();
                        });

                        // Releases captureFrame before stop(), so the main thread can never
                        // be left spinning on a frame nobody is going to consume.
                        abortRender();
                        break;
                    }
                }
                #ifdef GEODE_IS_WINDOWS
                else if (nativeBackend) {
                    if (!pipe.writeFrame(frame)) {
                        Loader::get()->queueInMainThread([] {
                            FLAlertLayer::create(
                                "Error",
                                "Native <cl>FFmpeg</c> stopped accepting frames — it may have crashed, stalled or run out of disk space.\nCheck the mod's log for details.",
                                "Ok"
                            )->show();
                        });

                        pipeBroke = true;
                        abortRender();
                        break;
                    }
                }
                else
                    process.m_stdin.write(frame.data(), frame.size());
                #endif
            }
            else lock.unlock();
        }

        #ifdef GEODE_IS_WINDOWS
        // ffmpeg can legitimately take a long time to drain; keep the user informed rather
        // than leaving the game apparently frozen on "Saving Render...".
        auto nativeProgress = [](int elapsed) {
            if (elapsed % 15 == 0) log::info("Native FFmpeg still working ({}s elapsed)", elapsed);
            if (elapsed % 60 != 0) return;
            Loader::get()->queueInMainThread([elapsed] {
                Notification::create(
                    fmt::format("Still encoding... ({}s)", elapsed),
                    NotificationIcon::Loading,
                    NOTIFICATION_LONG_TIME
                )->show();
            });
        };

        // A timeout is not a failure — ffmpeg keeps running and keeps writing. Say so
        // instead of claiming an error, and skip every rename/delete that would fight it.
        auto reportStillRunning = [](const char* what) {
            std::string message = fmt::format(
                "Native <cl>FFmpeg</c> is taking unusually long ({}) and is <cy>still running</c> in the background.\n"
                "Leave the game open; the finished file should appear in your renders folder shortly.",
                what
            );
            Loader::get()->queueInMainThread([message] {
                FLAlertLayer::create("Render", message.c_str(), "Ok")->show();
            });
        };
        #endif

        if (backend == VideoBackend::FFmpegAPI) {
            ffmpeg.stop();
        }
        else {
            #ifdef GEODE_IS_WINDOWS
            if (nativeBackend) {
                // A broken pipe means ffmpeg is already gone or wedged; don't sit around
                // for ten minutes waiting on a corpse.
                int code = pipe.finish(pipeBroke ? 30000 : 600000, nativeProgress);

                if (NativeFFmpeg::isStillRunning(code)) {
                    // Only reassuring when nothing has gone wrong yet. After a broken pipe
                    // the user has already been told the encoder stopped taking frames, and
                    // telling them the render is fine and on its way would flatly contradict
                    // that — the timeout here is a wedged ffmpeg, not a slow one.
                    if (!pipeBroke) {
                        reportStillRunning("encoding the video");
                        return;
                    }
                    log::warn("Native FFmpeg did not exit after the pipe broke; leaving the output alone.");
                    return;
                }

                // A broken pipe was already reported above; don't stack a second popup.
                if (code != 0 || pipeBroke) {
                    if (!pipeBroke) {
                        Loader::get()->queueInMainThread([] {
                            FLAlertLayer::create("Error", "There was an error saving the render. Wrong render Args.", "Ok")->show();
                        });
                    }
                    return;
                }
            }
            else if (process.close()) {
                Loader::get()->queueInMainThread([] {
                    FLAlertLayer::create("Error", "There was an error saving the render. Wrong render Args.", "Ok")->show();
                });
                return;
            }
            #endif
        }

        Loader::get()->queueInMainThread([] {
            Notification::create("Saving Render...", NotificationIcon::Loading)->show();
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        if ((SFXVolume == 0.f && musicVolume == 0.f) || audioMode == AudioMode::Off || (audioMode == AudioMode::Song && !std::filesystem::exists(songFile)) || (audioMode == AudioMode::Record && !std::filesystem::exists("fmodoutput.wav"))) {
            if (audioMode != AudioMode::Off) {
                Loader::get()->queueInMainThread([] {
                    FLAlertLayer::create("Error", "Song File not found.", "Ok")->show();
                });

                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }

            Loader::get()->queueInMainThread([] {
                Notification::create("Render Saved Without Audio", NotificationIcon::Success)->show();
                if (!Mod::get()->getSavedValue<bool>("render_hide_endscreen")) return;
                if (PlayLayer* pl = PlayLayer::get())
                    if (EndLevelLayer* layer = pl->getChildByType<EndLevelLayer>(0))
                        layer->setVisible(true);
            });

            return;
        }

        std::filesystem::path tempPath = std::filesystem::path(path).parent_path() / ("temp_" + std::filesystem::path(path).filename().string());
        std::filesystem::path tempPathAudio = (Mod::get()->getSaveDir() / "temp_audio_file.wav");

        #ifdef GEODE_IS_WINDOWS
        // Native ffmpeg is handed the UTF-8 form of the path, so the temp file the rename
        // at the bottom of this function looks for must be derived from the same wide path
        // rather than from the ACP-decoded one.
        if (nativeBackend)
            tempPath = videoPathW.parent_path() / (std::wstring(L"temp_") + videoPathW.filename().wstring());
        #endif

        if (backend == VideoBackend::FFmpegAPI) {
            std::string file = audioMode == AudioMode::Song ? songFile : "fmodoutput.wav";
            auto res = ffmpeg::events::AudioMixer::mixVideoAudio(path, file, tempPath);
            log::debug("XD");
            if (res.isErr()) {
                Loader::get()->queueInMainThread([] {
                    FLAlertLayer::create("Error", "FFmpeg failed to add audio: ", "Ok")->show();
                });
                return;
            }
        }
        else {
            #ifdef GEODE_IS_WINDOWS

            double totalTime = lastFrame_t;
            if (fadeOutTime > totalTime) fadeOutTime = totalTime / 2;
            float fadeOutStart = totalTime - fadeOutTime;

            if (fadeOutVideo) {
                std::string fadedPath = path + "_temp" + extension;
                // Same concatenation as above, but built off the lossless path.
                std::filesystem::path fadedPathW = videoPathW.wstring() + L"_temp" + Utils::widen(extension);
                bool fadeFailed = true;
                bool fadeStillRunning = false;

                if (nativeBackend) {
                    auto inUnix = NativeFFmpeg::toUnixPath(videoPathW);
                    auto outUnix = NativeFFmpeg::toUnixPath(fadedPathW);

                    if (!inUnix || !outUnix)
                        log::warn("Failed to translate the fade out paths for native FFmpeg.");
                    else {
                        // The fade filter is software, so the hardware upload has to be
                        // re-appended after it, exactly like in the main encode.
                        std::string filters = NativeFFmpeg::joinFilters({
                            fmt::format("fade=t=out:st={}:d={}", fadeOutStart, fadeOutTime),
                            nativeFilters
                        });

                        std::string args = NativeFFmpeg::joinArgs({
                            "-y", "-hide_banner",
                            "-i", NativeFFmpeg::shQuote(*inUnix),
                            "-vf", NativeFFmpeg::shQuote(filters),
                            nativeCodec,
                            nativeArgs,
                            bitrate,
                            "-c:a copy",
                            NativeFFmpeg::shQuote(*outUnix)
                        });

                        // A full re-encode of the whole render, so give it a long leash.
                        int code = NativeFFmpeg::runBlocking(args, 1800000, nativeProgress);
                        fadeStillRunning = NativeFFmpeg::isStillRunning(code);
                        fadeFailed = code != 0;
                    }
                }
                else {
                    command = fmt::format("\"{}\" -i \"{}\" -vf \"fade=t=out:st={}:d={}\" {}{}-c:a copy \"{}\"", ffmpegPath, path, fadeOutStart, std::to_string(fadeOutTime), codec, bitrate, fadedPath);

                    log::info("Executing (Fade Out): {}", command);
                    process = subprocess::Popen(command);
                    fadeFailed = process.close();
                }

                // Bail out entirely rather than racing a still-running ffmpeg for the file.
                if (fadeStillRunning) {
                    reportStillRunning("applying the fade out");
                    return;
                }

                if (!fadeFailed) {
                    std::error_code ec;
                    std::filesystem::remove(nativeBackend ? videoPathW : std::filesystem::path(path), ec);
                    if (ec) log::warn("Failed to remove old render file.");
                    else {
                        ec.clear();
                        std::filesystem::rename(
                            nativeBackend ? fadedPathW : std::filesystem::path(fadedPath),
                            nativeBackend ? videoPathW : std::filesystem::path(path),
                            ec
                        );
                        if (ec) log::warn("Failed to rename temp render file.");
                    }
                } else log::debug("Fade Out Error xD");
            }

            if (audioMode == AudioMode::Record) {
                bool wavFailed = true;
                bool wavStillRunning = false;

                if (nativeBackend) {
                    // "fmodoutput.wav" is relative to the game's cwd, which the native
                    // ffmpeg does not share — resolve it before translating.
                    std::error_code ec;
                    std::filesystem::path fmodPath = std::filesystem::absolute("fmodoutput.wav", ec);
                    if (ec) fmodPath = "fmodoutput.wav";

                    auto inUnix = NativeFFmpeg::toUnixPath(fmodPath);
                    auto outUnix = NativeFFmpeg::toUnixPath(tempPathAudio);

                    if (!inUnix || !outUnix)
                        log::warn("Failed to translate the recorded audio paths for native FFmpeg.");
                    else {
                        std::string args = NativeFFmpeg::joinArgs({
                            "-y", "-hide_banner",
                            "-i", NativeFFmpeg::shQuote(*inUnix),
                            "-acodec pcm_s16le -ar 44100 -ac 2",
                            NativeFFmpeg::shQuote(*outUnix)
                        });

                        int code = NativeFFmpeg::runBlocking(args, 600000, nativeProgress);
                        wavStillRunning = NativeFFmpeg::isStillRunning(code);
                        wavFailed = code != 0;
                    }
                }
                else {
                    command = fmt::format("\"{}\" -i \"fmodoutput.wav\" -acodec pcm_s16le -ar 44100 -ac 2 \"{}\"",
                        ffmpegPath, tempPathAudio
                    );

                    process = subprocess::Popen(command);  // Fix ffmpeg not reading it
                    wavFailed = process.close();
                }

                if (wavStillRunning) {
                    reportStillRunning("converting the recorded audio");
                    return;
                }

                if (wavFailed) {
                    Loader::get()->queueInMainThread([] {
                        FLAlertLayer::create("Error", "There was an error adding the song. ID: 140", "Ok")->show();
                    });
                    return;
                }
            }

            {

                std::string fadeInString;
                if ((fadeIn && audioMode == AudioMode::Song) || fadeInVideo) 
                    fadeInString = fmt::format(", afade=t=in:d={}", fadeInVideo ? std::to_string(fadeInTime) : "2");

                std::string fadeOutString;
                if ((fadeOut && audioMode == AudioMode::Song) || fadeOutVideo) 
                    fadeOutString = fmt::format(
                        ", afade=t=out:d={}:st={}", 
                        fadeOutVideo ? std::to_string(fadeOutTime) : "2",
                        fadeOutVideo ? fadeOutStart : totalTime - timeAfter - 3.5f
                    );

                std::filesystem::path file = audioMode == AudioMode::Song ? songFile : tempPathAudio;
                float offset = audioMode == AudioMode::Song ? songOffset : (isPlatformer ? 0.28f : 0.f);

                if (!extraAudioArgs.empty()) extraAudioArgs += " ";

                std::string volume = audioMode == AudioMode::Song ? fmt::format(",volume={:.2f}", musicVolume) : "";

                bool audioFailed = true;
                bool audioStillRunning = false;

                if (nativeBackend) {
                    // tempPathAudio is already a real path object; songFile is a narrow
                    // string and needs the same UTF-8 treatment as the render path.
                    auto audioUnix = audioMode == AudioMode::Song
                        ? NativeFFmpeg::toUnixPath(std::filesystem::path(Utils::widen(songFile)))
                        : NativeFFmpeg::toUnixPath(tempPathAudio);
                    auto videoUnix = NativeFFmpeg::toUnixPath(videoPathW);
                    auto outUnix = NativeFFmpeg::toUnixPath(tempPath);

                    if (!audioUnix || !videoUnix || !outUnix)
                        log::warn("Failed to translate the audio mux paths for native FFmpeg.");
                    else {
                        // The video is stream-copied here, so no encoder options are needed.
                        std::string audioFilter = fmt::format("[1:a]adelay=0|0{}{}{}", fadeInString, fadeOutString, volume);

                        std::string args = NativeFFmpeg::joinArgs({
                            "-y", "-hide_banner",
                            "-ss", fmt::format("{}", offset),
                            "-i", NativeFFmpeg::shQuote(*audioUnix),
                            "-i", NativeFFmpeg::shQuote(*videoUnix),
                            "-t", fmt::format("{}", totalTime),
                            "-c:v copy",
                            extraAudioArgs,
                            "-filter:a", NativeFFmpeg::shQuote(audioFilter),
                            NativeFFmpeg::shQuote(*outUnix)
                        });

                        int code = NativeFFmpeg::runBlocking(args, 900000, nativeProgress);
                        audioStillRunning = NativeFFmpeg::isStillRunning(code);
                        audioFailed = code != 0;
                    }
                }
                else {
                    command = fmt::format(
                        "\"{}\" -y -ss {} -i \"{}\" -i \"{}\" -t {} -c:v copy {} -filter:a \"[1:a]adelay=0|0{}{}{}\" \"{}\"",
                        ffmpegPath,
                        offset,
                        file,
                        path,
                        totalTime,
                        extraAudioArgs,
                        fadeInString,
                        fadeOutString,
                        volume,
                        tempPath
                    );

                    log::info("Executing (Audio): {}", command);

                    auto process = subprocess::Popen(command);
                    audioFailed = process.close();
                }

                // Returning here without the rename below would leave an orphan temp file
                // next to a stale original, so make sure the user knows it is still coming.
                if (audioStillRunning) {
                    reportStillRunning("adding the audio");
                    return;
                }

                if (audioFailed) {
                    Loader::get()->queueInMainThread([] {
                        FLAlertLayer::create("Error", "There was an error adding the song. Wrong Audio Args.", "Ok")->show();
                    });
                    return;
                }
            }

            #endif 
        }

        std::error_code ec;
        // pathReal, never Utils::widen(path): `path` is the ACP-encoded narrow form of this
        // very path, and widen() decodes with CP_UTF8, so round-tripping a non-ASCII level
        // name through it yields U+FFFD. The rename would then fail and leave the finished
        // file sitting there as temp_render_<name>_....mp4 while the user is told the render
        // was saved.
        std::filesystem::remove(pathReal, ec);
        if (ec) log::warn("Failed to remove old render file.");
        else {
            ec.clear();
            std::filesystem::rename(tempPath, pathReal, ec);
            if (ec) log::warn("Failed to rename temp render file.");
        }

        ec.clear();
        std::filesystem::remove(tempPathAudio, ec);
        if (ec) log::warn("Failed to remove temp audio file.");

        ec.clear();
        std::filesystem::remove("fmodoutput.wav", ec);
        if (ec) log::warn("Failed to remove fmod audio file.");

        Loader::get()->queueInMainThread([] {
            Notification::create("Render Saved With Audio", NotificationIcon::Success)->show();
        });
        
        }).detach();
}

void Renderer::stop(int frame) {
    renderedFrames.clear();
    finishFrame = Global::getCurrentFrame();
    pause = true;
    recording = false;
    timeAfter = 0.f;

    // The in-process API backend cannot mux audio the way the ffmpeg CLI backends can.
    if (backend == VideoBackend::FFmpegAPI) audioMode = AudioMode::Off;

    if (PlayLayer* pl = PlayLayer::get()) {

        if (pl->m_isPaused && audioMode == AudioMode::Record) {
            if (PauseLayer* layer = Global::getPauseLayer()) {
                CCScene* scene = CCDirector::sharedDirector()->getRunningScene();
                if (RecordLayer* xdbot = scene->getChildByType<RecordLayer>(0))
                    xdbot->onClose(nullptr);
                
                layer->onResume(nullptr);
            }
        }
        else if (pl->m_levelEndAnimationStarted) {
            finishFrame = 0;
            levelFinished = true;
        }

    } else
        audioMode = AudioMode::Off;

    if (audioMode == AudioMode::Record) {
        recordingAudio = true;
        dontRecordAudio = true;
        Notification::create("Recording Audio...", NotificationIcon::Loading)->show();
    }

    pause = false;
    changeRes(true);
}

void Renderer::changeRes(bool og) {
    cocos2d::CCEGLView* view = cocos2d::CCEGLView::get();
    cocos2d::CCSize res = {0, 0};
    float scaleX = 1.f;
    float scaleY = 1.f;

    res = og ? ogRes : CCSize(320.f * (width / static_cast<float>(height)), 320.f);
    scaleX = og ? ogScaleX : (width / res.width);
    scaleY = og ? ogScaleY : (height / res.height);

    if (res == CCSize(0, 0) && !og) return changeRes(true);
       
    CCDirector::sharedDirector()->m_obWinSizeInPoints = res;
    view->setDesignResolutionSize(res.width, res.height, ResolutionPolicy::kResolutionExactFit);
    view->m_fScaleX = scaleX;
    view->m_fScaleY = scaleY;
}

void MyRenderTexture::begin() {
    // NOTE: this picks the *GL capture path*, not the encoder. Only the ffmpeg.exe
    // backend wants the EXT framebuffer functions.
    if (Global::get().renderer.usesCoreGL()) {
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &old_fbo);

        texture = new CCTexture2D();
        {
            std::unique_ptr<char, void(*)(void*)> data(static_cast<char*>(malloc(width * height * 3)), free);
            memset(data.get(), 0, width * height * 3);
            texture->initWithData(data.get(), kCCTexture2DPixelFormat_RGB888, width, height, CCSize(static_cast<float>(width), static_cast<float>(height)));
        }

        glGetIntegerv(GL_RENDERBUFFER_BINDING, &old_rbo);

        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);

        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, texture->getName(), 0);

        texture->setAliasTexParameters();

        texture->autorelease();

        glBindRenderbuffer(GL_RENDERBUFFER, old_rbo);
        glBindFramebuffer(GL_FRAMEBUFFER, old_fbo);
#ifdef GEODE_IS_WINDOWS
    } else {
        glGetIntegerv(GL_FRAMEBUFFER_BINDING_EXT, &old_fbo);

        texture = new CCTexture2D();
        {
            auto data = malloc(width * height * 3);
            memset(data, 0, width * height * 3);
            texture->initWithData(data, kCCTexture2DPixelFormat_RGB888, width, height, CCSize(static_cast<float>(width), static_cast<float>(height)));
            free(data);
        }

        glGetIntegerv(GL_RENDERBUFFER_BINDING_EXT, &old_rbo);

        glGenFramebuffersEXT(1, &fbo);
        glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo);

        glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, texture->getName(), 0);

        texture->setAliasTexParameters();

        texture->autorelease();

        glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, old_rbo);
        glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, old_fbo);
    }
#else
    }
#endif
}

void MyRenderTexture::capture(std::mutex& lock, std::vector<uint8_t>& data, volatile bool& hasData) {
    CCDirector* director = CCDirector::sharedDirector();
    PlayLayer* pl = PlayLayer::get();

    // Same as in begin(): GL capture path, not the encoder choice.
    if (Global::get().renderer.usesCoreGL()) {
        glViewport(0, 0, width, height);
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &old_fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);

        pl->visit();

        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        lock.lock();
        hasData = true;
        glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, data.data());
        lock.unlock();

        glBindFramebuffer(GL_FRAMEBUFFER, old_fbo);
        director->setViewport();
#ifdef GEODE_IS_WINDOWS
    } else {
        glViewport(-1, 1.0, width, height);
        glGetIntegerv(GL_FRAMEBUFFER_BINDING_EXT, &old_fbo);
        glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo);

        pl->visit();

        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        lock.lock();
        hasData = true;
        glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, data.data());
        lock.unlock();

        glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, old_fbo);
        director->setViewport();
    }
#else
    }
#endif
}

void Renderer::captureFrame() {
    // Runs on the main thread. Nothing but the encoder thread clears frameHasData, so a
    // dead encoder (stalled ffmpeg, full disk) would hang the game here with no way out
    // but killing the process.
    //
    // The yield matters: the native backend's very first frame waits a second or two while
    // ffmpeg binds its listening socket, and a bare spin turns that into a fully busy core
    // fighting the encoder thread for the CPU it is waiting on. Yielding only gives up the
    // rest of the current time slice when another thread is actually runnable, so the
    // normal case (the encoder has already consumed the frame) is unaffected.
    while (frameHasData && !encoderFailed) std::this_thread::yield();
    if (encoderFailed) return;

    renderer.capture(lock, currentFrame, frameHasData);
}
int wa = 0;
void Renderer::handleRecording(PlayLayer* pl, int frame) {
    if (!pl) stop(frame);
    isPlatformer = pl->m_levelSettings->m_platformerMode;
    if (dontRender || pl->m_player1->m_isDead) return;

    auto& g = Global::get();
    if (renderedFrames.contains(frame) && frame > 10)
        return;

    renderedFrames.insert(frame);

    if (!pl->m_hasCompletedLevel || timeAfter < stopAfter) {

        float dt = 1.f / static_cast<double>(fps);
        if (pl->m_hasCompletedLevel) {
            timeAfter += dt;
            levelFinished = true;
        }

        float time = pl->m_gameState.m_levelTime + extra_t - lastFrame_t;
        if (time >= dt) {
            extra_t = time - dt;
            lastFrame_t = pl->m_gameState.m_levelTime;

            int correctMusicTime = static_cast<int>((frame / static_cast<float>(Global::getTPS()) + pl->m_levelSettings->m_songOffset) * 1000);
            correctMusicTime += fmod->m_musicOffset;

            if (fmod->getMusicTimeMS(0) - correctMusicTime >= 110)
                fmod->setMusicTimeMS(correctMusicTime, true, 0);

            captureFrame();
        }
    }
    else stop(frame);
}

bool Renderer::tryPause() {
    if (!recordingAudio) return true;

    pauseAttempts++;

    if (pauseAttempts > 5) {
        pauseAttempts = 0;
        stopAudio();
        return true;
    }

    return false;
}

void Renderer::startAudio(PlayLayer* pl) {
    EndLevelLayer* endLevelLayer = pl->getChildByType<EndLevelLayer>(0);
    if (dontRecordAudio) return;

    if (pl->m_levelEndAnimationStarted && endLevelLayer != nullptr) {
        CCKeyboardDispatcher::get()->dispatchKeyboardMSG(enumKeyCodes::KEY_Space, true, false, 0.0);
        CCKeyboardDispatcher::get()->dispatchKeyboardMSG(enumKeyCodes::KEY_Space, false, false, 0.0);
    }
    else if (!pl->m_levelEndAnimationStarted) {

        if (!pl->m_isPaused)
            pl->pauseGame(false);

        if (Global::get().state != state::playing)
            Macro::togglePlaying();

        Global::get().restart = true;

        if (PauseLayer* layer = Global::getPauseLayer())
            layer->onResume(nullptr);

        auto fmod = FMODAudioEngine::sharedEngine();
        fmod->m_globalChannel->getVolume(&ogSFXVol);
        fmod->m_backgroundMusicChannel->getVolume(&ogMusicVol);

        FMODAudioEngine::sharedEngine()->m_system->setOutput(FMOD_OUTPUTTYPE_WAVWRITER);
        startedAudio = true;
        pauseAttempts = 0;
        if (CCNode* lbl = pl->getChildByID("recording-audio-label"_spr))
            lbl->setVisible(true);
    }
}

void Renderer::stopAudio() {
    FMODAudioEngine::sharedEngine()->m_system->setOutput(FMOD_OUTPUTTYPE_AUTODETECT);
    auto fmod = FMODAudioEngine::sharedEngine();
	fmod->m_globalChannel->setVolume(ogSFXVol);
	fmod->m_backgroundMusicChannel->setVolume(ogMusicVol);

    recordingAudio = false;
    if (PlayLayer* pl = PlayLayer::get())
        if (CCNode* lbl = pl->getChildByID("recording-audio-label"_spr))
                lbl->setVisible(false);
}

void Renderer::handleAudioRecording(PlayLayer* pl, int frame) {
    auto& g = Global::get();
    
	fmod->m_globalChannel->setVolume(SFXVolume);
	fmod->m_backgroundMusicChannel->setVolume(musicVolume);

    if (!pl) {
        g.renderer.stopAudio();
        return;
    }

    if (finishFrame != 0 && frame >= finishFrame) {
        g.renderer.stopAudio();
        return;
    }

    if (!pl->m_hasCompletedLevel || timeAfter < stopAfter) {
        float dt = 1.f / static_cast<double>(fps);
        if (pl->m_hasCompletedLevel)
            timeAfter += dt;
    }
    else
        g.renderer.stopAudio();
}