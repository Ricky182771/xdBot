#include "render_settings_layer.hpp"
#include "record_layer.hpp"

#include <Geode/modify/SliderTouchLogic.hpp>

class $modify(SliderTouchLogic) {
    bool ccTouchBegan(cocos2d::CCTouch* v1, cocos2d::CCEvent* v2) {
        if (std::string_view(m_slider->getID()) == "disabled-slider"_spr) return false;
        return SliderTouchLogic::ccTouchBegan(v1, v2);
    }
};

namespace {

    // Uppercase is load-bearing, not cosmetic: ffmpeg option *values* are frequently
    // case-sensitive constants. VAAPI's rate control in particular only accepts
    // "-rc_mode CQP" — lowercase "cqp" fails with "Undefined constant" and produces no
    // output file at all. Same for CBR/VBR/QVBR/ICQ and "-profile:v Main/High".
    const char* const kArgsCharset =
        " 0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ-_:;.\"\\/[](){}+=<>|!*&'%@,";

    // Shortens `text` with an ellipsis until it fits `maxWidth` at `scale`. CCLabelBMFont
    // has no built-in truncation and setMaxLabelWidth would rescale the text instead,
    // which quickly becomes unreadable for a string this long.
    std::string truncateToWidth(const std::string& text, float scale, float maxWidth) {
        if (text.empty() || maxWidth <= 0.f) return text;

        CCLabelBMFont* probe = CCLabelBMFont::create(text.c_str(), "chatFont.fnt");
        if (!probe) return text;

        if (probe->getContentSize().width * scale <= maxWidth) return text;

        std::string shortened = text;
        while (shortened.size() > 1) {
            shortened.pop_back();
            probe->setString((shortened + "...").c_str());
            if (probe->getContentSize().width * scale <= maxWidth) break;
        }

        return shortened + "...";
    }

}

// Compact editor for the native backend's encoder args and filters. These used to be
// mod.json settings, which meant leaving the render menu to change the codec; they now
// live next to the rest of the render arguments.
class NativeArgsLayer : public geode::Popup {

public:

    TextInput* argsInput = nullptr;
    TextInput* filtersInput = nullptr;
    RenderSettingsLayer* owner = nullptr;

    STATIC_CREATE(NativeArgsLayer, 340, 190)

    ~NativeArgsLayer() {
        CC_SAFE_RELEASE(owner);
    }

    // The owner is retained: it sits underneath this popup and must stay alive for the
    // refresh callback even if it somehow gets closed first.
    static NativeArgsLayer* createFor(RenderSettingsLayer* owner) {
        NativeArgsLayer* ret = create();
        if (ret) {
            ret->owner = owner;
            CC_SAFE_RETAIN(owner);
        }
        return ret;
    }

    void save() {
        std::string args = argsInput->getString();
        std::string filters = filtersInput->getString();

        Mod* mod = Mod::get();
        mod->setSavedValue("render_native_args", args);
        mod->setSavedValue("render_native_filters", filters);

        if (owner) owner->refreshNativePrefix();
    }

    void onRestore(CCObject*) {
        argsInput->setString(Renderer::defaultNativeArgs);
        filtersInput->setString(Renderer::defaultNativeFilters);
        save();
    }

    void onOk(CCObject*) {
        save();
        keyBackClicked();
    }

    bool setup() {
        setTitle("Native FFmpeg Args");

        Mod* mod = Mod::get();
        Renderer::ensureNativeArgsMigrated();

        Utils::setBackgroundColor(m_bgSprite);

        CCMenu* menu = CCMenu::create();
        menu->setPosition(m_mainLayer->getContentSize() / 2);
        menu->setContentSize({ 0, 0 });
        m_mainLayer->addChild(menu);

        CCLabelBMFont* lbl = CCLabelBMFont::create(
            "Prepended to every native render.", "chatFont.fnt");
        lbl->setPosition({ 0, 58 });
        lbl->setScale(0.4f);
        lbl->setOpacity(160);
        menu->addChild(lbl);

        lbl = CCLabelBMFont::create("Encoder Args:", "bigFont.fnt");
        lbl->setPosition({ -150, 34 });
        lbl->setAnchorPoint({ 0, 0.5f });
        lbl->setScale(0.325f);
        lbl->setOpacity(200);
        menu->addChild(lbl);

        argsInput = TextInput::create(300.f, "encoder args", "chatFont.fnt");
        argsInput->setPosition({ 0, 14 });
        argsInput->setScale(0.9f);
        argsInput->setFilter(kArgsCharset);
        argsInput->setString(mod->getSavedValue<std::string>("render_native_args").c_str());
        menu->addChild(argsInput);

        lbl = CCLabelBMFont::create("Extra Filters:", "bigFont.fnt");
        lbl->setPosition({ -150, -12 });
        lbl->setAnchorPoint({ 0, 0.5f });
        lbl->setScale(0.325f);
        lbl->setOpacity(200);
        menu->addChild(lbl);

        filtersInput = TextInput::create(300.f, "extra filters", "chatFont.fnt");
        filtersInput->setPosition({ 0, -32 });
        filtersInput->setScale(0.9f);
        filtersInput->setFilter(kArgsCharset);
        filtersInput->setString(mod->getSavedValue<std::string>("render_native_filters").c_str());
        menu->addChild(filtersInput);

        // Installed only now that both inputs exist: save() reads them both, so wiring the
        // first one up before the second is constructed leaves a null dereference waiting.
        argsInput->setCallback([this](const std::string&) { save(); });
        filtersInput->setCallback([this](const std::string&) { save(); });

        lbl = CCLabelBMFont::create("Clear both for software encoding.", "chatFont.fnt");
        lbl->setPosition({ 0, -56 });
        lbl->setScale(0.35f);
        lbl->setOpacity(130);
        menu->addChild(lbl);

        ButtonSprite* spr = ButtonSprite::create("Restore");
        spr->setScale(0.5f);
        CCMenuItemSpriteExtra* btn = CCMenuItemSpriteExtra::create(
            spr, this, menu_selector(NativeArgsLayer::onRestore));
        btn->setPosition({ -60, -78 });
        menu->addChild(btn);

        spr = ButtonSprite::create("Ok");
        spr->setScale(0.5f);
        btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(NativeArgsLayer::onOk));
        btn->setPosition({ 60, -78 });
        menu->addChild(btn);

        return true;
    }

};

void RenderSettingsLayer::onEditNativeArgs(CCObject*) {
    if (NativeArgsLayer* layer = NativeArgsLayer::createFor(this))
        layer->show();
}

void RenderSettingsLayer::refreshNativePrefix() {
    if (!nativePrefixLabel) return;

    std::string args = Mod::get()->getSavedValue<std::string>("render_native_args");
    std::string filters = Mod::get()->getSavedValue<std::string>("render_native_filters");

    // The filters are actually merged into the single -vf chain alongside vflip/video
    // args/fades; "+vf" is shorthand for that, the popup shows the real values.
    std::string full = args;
    if (!filters.empty()) {
        if (!full.empty()) full += "  ";
        full += "+vf " + filters;
    }

    if (full.empty()) full = "(software - uses the codec above)";

    nativePrefixLabel->setString(truncateToWidth(full, nativePrefixLabel->getScale(), nativePrefixWidth).c_str());
}

void RenderSettingsLayer::textChanged(CCTextInputNode* node) {

    if (secondsInput->getString() != "" && node == secondsInput) {
        std::string value = secondsInput->getString();
        if (value == ".")
            secondsInput->setString("0.");
        else if (std::count(value.begin(), value.end(), '.') == 2)
            return secondsInput->setString(mod->getSavedValue<std::string>("render_seconds_after").c_str());
    }

    std::string secondsAfter = secondsInput->getString();
    std::string args = argsInput->getString();
    std::string audioArgs = audioArgsInput->getString();
    std::string videoArgs = videoArgsInput->getString();
    std::string fadeIn = fadeInInput->getString();
    std::string fadeOut = fadeOutInput->getString();
    std::string extension = extensionInput->getString();

    mod->setSavedValue("render_seconds_after", secondsAfter);
    mod->setSavedValue("render_args", args);
    mod->setSavedValue("render_audio_args", audioArgs);
    mod->setSavedValue("render_video_args", videoArgs);
    mod->setSavedValue("render_fade_in_time", fadeIn);
    mod->setSavedValue("render_fade_out_time", fadeOut);
    mod->setSavedValue("render_file_extension", extension);
}

void RenderSettingsLayer::onDefaults(CCObject*) {
	geode::createQuickPopup(
        "Restore",
        "<cr>Restore</c> default render settings?",
        "Cancel", "Yes",
        [this](auto, bool btn2) {
            auto& g = Global::get();
            
            g.mod->setSavedValue("render_args", std::string("-pix_fmt yuv420p"));
	        g.mod->setSavedValue("render_audio_args", std::string(""));
            #ifdef GEODE_IS_WINDOWS
            g.mod->setSavedValue("render_video_args", std::string("colorspace=all=bt709:iall=bt470bg:fast=1"));
            #else
            g.mod->setSavedValue("render_video_args", std::string(""));
            #endif
            g.mod->setSavedValue("render_record_audio", true);
            g.mod->setSavedValue("render_only_song", false);
            g.mod->setSavedValue("render_music_volume", 1.0);
            g.mod->setSavedValue("render_sfx_volume", 1.0);
            g.mod->setSavedValue("render_file_extension", std::string(".mp4"));
            g.mod->setSavedValue("render_fade_in", false);
            g.mod->setSavedValue("render_fade_out", false);
            g.mod->setSavedValue("render_fade_in_time", std::to_string(2));
            g.mod->setSavedValue("render_fade_out_time", std::to_string(2));
            g.mod->setSavedValue("render_hide_endscreen", false);
            g.mod->setSavedValue("render_hide_levelcomplete", false);
            g.mod->setSavedValue("render_native_args", std::string(Renderer::defaultNativeArgs));
            g.mod->setSavedValue("render_native_filters", std::string(Renderer::defaultNativeFilters));
            g.mod->setSavedValue("render_native_migrated", true);

	        CCArray* children = CCDirector::sharedDirector()->getRunningScene()->getChildren();
            for (CCObject* child : CCArrayExt<CCObject*>(children)) {
                if (RecordLayer* layer = typeinfo_cast<RecordLayer*>(child)) {
                    layer->onClose(nullptr);
                    break;
                }
            }

            this->keyBackClicked();
            RecordLayer::openMenu(true);
            RenderSettingsLayer* layer = create();
            layer->m_noElasticity = true;
            layer->show();
        }
    );
}

bool RenderSettingsLayer::setup() {
    setTitle("Render Settings");

    // shouldUseAPI() now means "the limited in-process FFmpeg API backend", so it is false
    // for the native backend — which is what we want: native drives the real ffmpeg CLI and
    // honours every one of the args/fade/extension/volume settings this block greys out.
    bool usingApi = Renderer::shouldUseAPI();

    bool usingNative = false;
    #ifdef GEODE_IS_WINDOWS
    usingNative = Renderer::selectBackend() == VideoBackend::NativeUnix;
    if (usingNative) Renderer::ensureNativeArgsMigrated();
    #endif

    cocos2d::CCPoint offset = (CCDirector::sharedDirector()->getWinSize() - m_mainLayer->getContentSize()) / 2;
    m_mainLayer->setPosition(m_mainLayer->getPosition() - offset);
    m_closeBtn->setPosition(m_closeBtn->getPosition() + offset);
    m_bgSprite->setPosition(m_bgSprite->getPosition() + offset);
    m_title->setPosition(m_title->getPosition() + offset);
    
    mod = Mod::get();
    
    Utils::setBackgroundColor(m_bgSprite);

    if (mod->getSavedValue<std::string>("render_seconds_after") == "")
        mod->setSavedValue("render_seconds_after", std::to_string(0));

    cocos2d::CCSize center = cocos2d::CCDirector::sharedDirector()->getWinSize() / 2;

    CCMenu* menu = CCMenu::create();
    m_mainLayer->addChild(menu);
    menu->setPositionX(menu->getPositionX() - 67);

    CCScale9Sprite* bg = CCScale9Sprite::create("square02b_001.png", { 0, 0, 80, 80 });
    bg->setScale(0.355f);
    bg->setColor({ 0,0,0 });
    bg->setOpacity(75);
    bg->setPosition(ccp(-28, 97));
    bg->setAnchorPoint({ 0, 1 });
    // The native backend gets a second line inside this row for the locked prefix. The row
    // is anchored at its top, so growing it extends downwards into the gap above the Audio
    // Args row: at 0.355 scale, 76 puts the bottom edge at 97 - 27 = 70, five clear pixels
    // above that row's top edge at 65.
    bg->setContentSize({ 392, usingNative ? 76.f : 55.f });
    menu->addChild(bg);

    if (usingApi) bg->setOpacity(40);

    CCLabelBMFont* lbl = CCLabelBMFont::create("Extra Args:", "bigFont.fnt");
    lbl->setPosition(ccp(-105, 88));
    lbl->setAnchorPoint({ 0, 0.5 });
    lbl->setOpacity(200);
    lbl->setScale(0.325);
    menu->addChild(lbl);

    if (usingApi) lbl->setOpacity(90);

    argsInput = CCTextInputNode::create(150, 30, "args", "chatFont.fnt");
    argsInput->m_textField->setAnchorPoint({ 0.5f, 0.5f });
    argsInput->ignoreAnchorPointForPosition(true);
    argsInput->m_textLabel->setAnchorPoint({ 0.5f, 0.5f });
    argsInput->setPosition(ccp(25, 86));
    argsInput->setLabelPlaceholderColor(ccc3(163, 135, 121));
    argsInput->setMouseEnabled(true);
    argsInput->setTouchEnabled(true);
    argsInput->setContentSize({ 120, 20 });
    argsInput->setMaxLabelWidth(170.f);
    argsInput->setScale(0.75);
    argsInput->setString(mod->getSavedValue<std::string>("render_args").c_str());
    argsInput->setDelegate(this);
    argsInput->setAllowedChars(kArgsCharset);
    menu->addChild(argsInput);

    if (usingNative) {
        // A locked, non-editable echo of what the native backend prepends. Deliberately a
        // separate node rather than a forced prefix inside argsInput: keeping a protected
        // region inside a CCTextInputNode means fighting its cursor and selection handling.
        //
        // The row spans x -28 .. 111.2 (-28 + 392 * 0.355), so everything here has to stay
        // inside that. Vertically the row now reaches down to y 70, and this line sits at
        // 78 with the input's text above it.
        const float rowRight = -28.f + 392.f * 0.355f;
        const float lineY = 78.f;

        CCSprite* gear = CCSprite::createWithSpriteFrameName("GJ_optionsBtn_001.png");
        gear->setScale(0.24f);
        CCMenuItemSpriteExtra* gearBtn = CCMenuItemSpriteExtra::create(
            gear, this, menu_selector(RenderSettingsLayer::onEditNativeArgs));
        // Anchored so the sprite's right edge lands just inside the row.
        float gearHalfWidth = gearBtn->getContentSize().width * 0.5f;
        float gearX = rowRight - 3.f - gearHalfWidth;
        gearBtn->setPosition({ gearX, lineY });
        menu->addChild(gearBtn);

        CCLabelBMFont* tag = CCLabelBMFont::create("Native:", "goldFont.fnt");
        tag->setAnchorPoint({ 0, 0.5f });
        tag->setScale(0.18f);
        tag->setPosition({ -25, lineY });
        menu->addChild(tag);

        float textStart = -25.f + tag->getContentSize().width * 0.18f + 4.f;
        nativePrefixWidth = (gearX - gearHalfWidth - 4.f) - textStart;

        nativePrefixLabel = CCLabelBMFont::create(" ", "chatFont.fnt");
        nativePrefixLabel->setAnchorPoint({ 0, 0.5f });
        nativePrefixLabel->setScale(0.26f);
        nativePrefixLabel->setPosition({ textStart, lineY });
        nativePrefixLabel->setColor({ 130, 220, 140 });
        menu->addChild(nativePrefixLabel);

        refreshNativePrefix();
    }

    bg = CCScale9Sprite::create("square02b_001.png", { 0, 0, 80, 80 });
    bg->setScale(0.355f);
    bg->setColor({ 0,0,0 });
    bg->setOpacity(75);
    bg->setPosition(ccp(-31, 65));
    bg->setAnchorPoint({ 0, 1 });
    bg->setContentSize({ 401, 55 });
    menu->addChild(bg);
    
    if (usingApi) bg->setOpacity(40);

    lbl = CCLabelBMFont::create("Audio Args:", "bigFont.fnt");
    lbl->setPosition(ccp(-105, 55));
    lbl->setAnchorPoint({ 0, 0.5 });
    lbl->setOpacity(200);
    lbl->setScale(0.325);
    menu->addChild(lbl);
    
    if (usingApi) lbl->setOpacity(90);

    audioArgsInput = CCTextInputNode::create(150, 30, "audio args", "chatFont.fnt");
    audioArgsInput->m_textField->setAnchorPoint({ 0.5f, 0.5f });
    audioArgsInput->ignoreAnchorPointForPosition(true);
    audioArgsInput->m_textLabel->setAnchorPoint({ 0.5f, 0.5f });
    audioArgsInput->setPosition(ccp(18, 53));
    audioArgsInput->setLabelPlaceholderColor(ccc3(163, 135, 121));
    audioArgsInput->setMouseEnabled(true);
    audioArgsInput->setTouchEnabled(true);
    audioArgsInput->setContentSize({ 180, 20 });
    audioArgsInput->setMaxLabelWidth(165.f);
    audioArgsInput->setScale(0.75);
    audioArgsInput->setString(mod->getSavedValue<std::string>("render_audio_args").c_str());
    audioArgsInput->setDelegate(this);
    audioArgsInput->setAllowedChars(kArgsCharset);
    menu->addChild(audioArgsInput);

    bg = CCScale9Sprite::create("square02b_001.png", { 0, 0, 80, 80 });
    bg->setScale(0.375f);
    bg->setColor({ 0,0,0 });
    bg->setOpacity(75);
    bg->setPosition(ccp(49, 4));
    bg->setAnchorPoint({ 0, 1 });
    bg->setContentSize({ 82, 55 });
    menu->addChild(bg);

    bg = CCScale9Sprite::create("square02b_001.png", { 0, 0, 80, 80 });
    bg->setScale(0.355f);
    bg->setColor({ 0,0,0 });
    bg->setOpacity(75);
    bg->setPosition({-29, 34});
    bg->setAnchorPoint({ 0, 1 });
    bg->setContentSize({ 395, 55 });
    menu->addChild(bg);

    videoArgsInput = CCTextInputNode::create(150, 30, "video args", "chatFont.fnt");
    videoArgsInput->m_textField->setAnchorPoint({ 0.5f, 0.5f });
    videoArgsInput->ignoreAnchorPointForPosition(true);
    videoArgsInput->m_textLabel->setAnchorPoint({ 0.5f, 0.5f });
    videoArgsInput->setPosition({19, 22});
    videoArgsInput->setLabelPlaceholderColor(ccc3(163, 135, 121));
    videoArgsInput->setMouseEnabled(true);
    videoArgsInput->setTouchEnabled(true);
    videoArgsInput->setContentSize({ 180, 20 });
    videoArgsInput->setMaxLabelWidth(165.f);
    videoArgsInput->setScale(0.75);
    videoArgsInput->setString(mod->getSavedValue<std::string>("render_video_args").c_str());
    videoArgsInput->setAllowedChars(kArgsCharset);
    videoArgsInput->setDelegate(this);
    menu->addChild(videoArgsInput);

    lbl = CCLabelBMFont::create("Video Args:", "bigFont.fnt");
    lbl->setAnchorPoint({0, 0.5});
    lbl->setOpacity(200);
    lbl->setScale(0.325f);
    lbl->setPosition({-105, 24});
    menu->addChild(lbl);

    lbl = CCLabelBMFont::create("Render after completion:", "bigFont.fnt");
    lbl->setPosition(ccp(-105, -5));
    lbl->setAnchorPoint({ 0, 0.5 });
    lbl->setOpacity(200);
    lbl->setScale(0.325);
    menu->addChild(lbl);

    secondsInput = CCTextInputNode::create(150, 30, "sec", "chatFont.fnt");
    secondsInput->m_textField->setAnchorPoint({ 0.5f, 0.5f });
    secondsInput->ignoreAnchorPointForPosition(true);
    secondsInput->m_textLabel->setAnchorPoint({ 0.5f, 0.5f });
    secondsInput->setPosition(ccp(50, -8));
    secondsInput->setLabelPlaceholderColor(ccc3(163, 135, 121));
    secondsInput->setMouseEnabled(true);
    secondsInput->setTouchEnabled(true);
    secondsInput->setContentSize({ 120, 20 });
    secondsInput->setMaxLabelWidth(90.f);
    secondsInput->setScale(0.75);
    secondsInput->setString(mod->getSavedValue<std::string>("render_seconds_after").c_str());
    secondsInput->setDelegate(this);
    secondsInput->setMaxLabelLength(2);
    secondsInput->setAllowedChars("0123456789.");
    menu->addChild(secondsInput);

    lbl = CCLabelBMFont::create("s", "chatFont.fnt");
    lbl->setPosition(ccp(84, -5));
    lbl->setAnchorPoint({ 0, 0.5 });
    lbl->setScale(0.825);
    menu->addChild(lbl);

    lbl = CCLabelBMFont::create("Legacy Audio:", "bigFont.fnt");
    lbl->setPosition(ccp(-105, -32));
    lbl->setAnchorPoint({ 0, 0.5 });
    lbl->setOpacity(200);
    lbl->setScale(0.325);
    menu->addChild(lbl);
    
    if (usingApi) lbl->setOpacity(90);

    onlySongToggle = CCMenuItemToggler::create(
        CCSprite::createWithSpriteFrameName("GJ_checkOff_001.png"),
        CCSprite::createWithSpriteFrameName("GJ_checkOn_001.png"),
        this, menu_selector(RecordLayer::toggleSetting));
    onlySongToggle->setPosition(ccp(0, -32));
    onlySongToggle->setScale(0.555);
    if (!usingApi) onlySongToggle->toggle(mod->getSavedValue<bool>("render_only_song"));
    onlySongToggle->setID("render_only_song");
    menu->addChild(onlySongToggle);

    lbl = CCLabelBMFont::create("Record Audio:", "bigFont.fnt");
    lbl->setPosition(ccp(-105, -58));
    lbl->setAnchorPoint({ 0, 0.5 });
    lbl->setOpacity(200);
    lbl->setScale(0.325);
    menu->addChild(lbl);
    
    if (usingApi) lbl->setOpacity(90);

    recordAudioToggle = CCMenuItemToggler::create(
        CCSprite::createWithSpriteFrameName("GJ_checkOff_001.png"),
        CCSprite::createWithSpriteFrameName("GJ_checkOn_001.png"),
        this, menu_selector(RecordLayer::toggleSetting));
    recordAudioToggle->setPosition(ccp(0, -58));
    recordAudioToggle->setScale(0.555);
    if (!usingApi) recordAudioToggle->toggle(mod->getSavedValue<bool>("render_record_audio"));
    recordAudioToggle->setID("render_record_audio");
    menu->addChild(recordAudioToggle);

    lbl = CCLabelBMFont::create("Fade In:", "bigFont.fnt");
    lbl->setPosition(ccp(25, -32));
    lbl->setAnchorPoint({ 0, 0.5 });
    lbl->setOpacity(200);
    lbl->setScale(0.325);
    menu->addChild(lbl);
    
    if (usingApi) lbl->setOpacity(90);

    fadeInInput = TextInput::create(50.f, "s", "bigFont.fnt");
    fadeInInput->setScale(0.5f);
    fadeInInput->setPosition(ccp(100, -32));
    fadeInInput->setString(Mod::get()->getSavedValue<std::string>("render_fade_in_time").c_str());
    fadeInInput->getInputNode()->setDelegate(this);
    fadeInInput->getInputNode()->setAllowedChars("0123456789.");
    menu->addChild(fadeInInput);
    
    CCMenuItemToggler* toggle = CCMenuItemToggler::create(
        CCSprite::createWithSpriteFrameName("GJ_checkOff_001.png"),
        CCSprite::createWithSpriteFrameName("GJ_checkOn_001.png"),
        this, menu_selector(RecordLayer::toggleSetting));
    toggle->setPosition(ccp(130, -32));
    toggle->setScale(0.555);
    if (!usingApi) toggle->toggle(mod->getSavedValue<bool>("render_fade_in"));
    toggle->setID("render_fade_in");
    menu->addChild(toggle);

    if (usingApi) {
        toggle->setCascadeOpacityEnabled(true);
        toggle->setEnabled(false);
        toggle->setOpacity(100);
    }

    lbl = CCLabelBMFont::create("Fade Out:", "bigFont.fnt");
    lbl->setPosition(ccp(25, -58));
    lbl->setAnchorPoint({ 0, 0.5 });
    lbl->setOpacity(200);
    lbl->setScale(0.325);
    menu->addChild(lbl);
    
    if (usingApi) lbl->setOpacity(90);

    lbl = CCLabelBMFont::create("File Extension:", "bigFont.fnt");
    lbl->setPosition(ccp(110, -5));
    lbl->setAnchorPoint({ 0, 0.5 });
    lbl->setOpacity(200);
    lbl->setScale(0.3f);
    menu->addChild(lbl);
    
    if (usingApi) lbl->setOpacity(90);

    extensionInput = TextInput::create(46.f, "", "chatFont.fnt");
    extensionInput->setScale(0.8f);
    extensionInput->setPosition(ccp(209, -5));
    extensionInput->setString(Mod::get()->getSavedValue<std::string>("render_file_extension").c_str());
    extensionInput->getInputNode()->setDelegate(this);
    extensionInput->getInputNode()->setAllowedChars("abcdefghijklmnñopqrstuvwxyz0123456789.");
    menu->addChild(extensionInput);

    fadeOutInput = TextInput::create(50.f, "s", "bigFont.fnt");
    fadeOutInput->setScale(0.5f);
    fadeOutInput->setPosition(ccp(100, -58));
    fadeOutInput->setString(Mod::get()->getSavedValue<std::string>("render_fade_out_time").c_str());
    fadeOutInput->getInputNode()->setDelegate(this);
    fadeOutInput->getInputNode()->setAllowedChars("0123456789.");
    menu->addChild(fadeOutInput);

    toggle = CCMenuItemToggler::create(
        CCSprite::createWithSpriteFrameName("GJ_checkOff_001.png"),
        CCSprite::createWithSpriteFrameName("GJ_checkOn_001.png"),
        this, menu_selector(RecordLayer::toggleSetting));
    toggle->setPosition(ccp(130, -58));
    toggle->setScale(0.555);
    if (!usingApi) toggle->toggle(mod->getSavedValue<bool>("render_fade_out"));
    toggle->setID("render_fade_out");
    menu->addChild(toggle);

    if (usingApi) {
        toggle->setCascadeOpacityEnabled(true);
        toggle->setEnabled(false);
        toggle->setOpacity(100);
    }

    lbl = CCLabelBMFont::create("Hide Endscreen:", "bigFont.fnt");
    lbl->setPosition(ccp(-105, -83));
    lbl->setAnchorPoint({ 0, 0.5 });
    lbl->setOpacity(200);
    lbl->setScale(0.325);
    menu->addChild(lbl);

    toggle = CCMenuItemToggler::create(
        CCSprite::createWithSpriteFrameName("GJ_checkOff_001.png"),
        CCSprite::createWithSpriteFrameName("GJ_checkOn_001.png"),
        this, menu_selector(RecordLayer::toggleSetting));
    toggle->setPosition(ccp(0, -83));
    toggle->setScale(0.555);
    toggle->toggle(mod->getSavedValue<bool>("render_hide_endscreen"));
    toggle->setID("render_hide_endscreen");
    menu->addChild(toggle);

    lbl = CCLabelBMFont::create("Hide Level Complete:", "bigFont.fnt");
    lbl->setPosition(ccp(-105, -108));
    lbl->setAnchorPoint({ 0, 0.5 });
    lbl->setOpacity(200);
    lbl->setScale(0.25);
    menu->addChild(lbl);

    toggle = CCMenuItemToggler::create(
        CCSprite::createWithSpriteFrameName("GJ_checkOff_001.png"),
        CCSprite::createWithSpriteFrameName("GJ_checkOn_001.png"),
        this, menu_selector(RecordLayer::toggleSetting));
    toggle->setPosition(ccp(0, -108));
    toggle->setScale(0.555);
    toggle->toggle(mod->getSavedValue<bool>("render_hide_levelcomplete"));
    toggle->setID("render_hide_levelcomplete");
    menu->addChild(toggle);

    lbl = CCLabelBMFont::create("SFX Volume", "goldFont.fnt");
    lbl->setScale(0.475f);
    lbl->setPosition({188, 42});
    menu->addChild(lbl);
    
    if (usingApi) lbl->setOpacity(90);

    sfxSlider = Slider::create(
		this,
		menu_selector(RenderSettingsLayer::onSlider),
		1.f
	);
	sfxSlider->setPosition({188, 24});
	sfxSlider->setAnchorPoint({ 0.f, 0.f });
	sfxSlider->setScale(0.545f);
	sfxSlider->setValue(Mod::get()->getSavedValue<double>("render_sfx_volume"));
	menu->addChild(sfxSlider);

    lbl = CCLabelBMFont::create("Music Volume", "goldFont.fnt");
    lbl->setScale(0.475f);
    lbl->setPosition({188, 87});
    menu->addChild(lbl);
    
    if (usingApi) lbl->setOpacity(90);

    musicSlider = Slider::create(
		this,
		menu_selector(RenderSettingsLayer::onSlider),
		1.f
	);
	musicSlider->setPosition({188, 69});
	musicSlider->setAnchorPoint({ 0.f, 0.f });
	musicSlider->setScale(0.545f);
	musicSlider->setValue(Mod::get()->getSavedValue<double>("render_music_volume"));
	menu->addChild(musicSlider);

    ButtonSprite* spr = ButtonSprite::create("Ok");
    spr->setScale(0.875);
    CCMenuItemSpriteExtra* btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(RenderSettingsLayer::close));
    btn->setPosition(ccp(67, -100));
    menu->addChild(btn);

    spr = ButtonSprite::create("Restore Defaults");
    spr->setScale(0.375f);
    btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(RenderSettingsLayer::onDefaults));
    btn->setPosition({211, -123});
    menu->addChild(btn);

    CCSprite* spr2 = CCSprite::createWithSpriteFrameName("GJ_infoIcon_001.png");
    spr2->setScale(0.33f);
    btn = CCMenuItemSpriteExtra::create(spr2, this, menu_selector(RenderSettingsLayer::showInfoPopup));
    btn->setPosition({13, -22});
    btn->setTag(1);
    menu->addChild(btn);

    spr2 = CCSprite::createWithSpriteFrameName("GJ_infoIcon_001.png");
    spr2->setScale(0.33f);
    btn = CCMenuItemSpriteExtra::create(spr2, this, menu_selector(RenderSettingsLayer::showInfoPopup));
    btn->setPosition({13, -48});
    btn->setTag(0);
    menu->addChild(btn);

    if (usingApi) {
        argsInput->m_textLabel->setOpacity(100);
        audioArgsInput->m_textLabel->setOpacity(100);

        argsInput->setID("disabled-input"_spr);
        audioArgsInput->setID("disabled-input"_spr);

        extensionInput->setEnabled(false);
        extensionInput->getInputNode()->m_textLabel->setOpacity(100);
        extensionInput->getBGSprite()->setOpacity(40);

        fadeOutInput->setEnabled(false);
        fadeOutInput->getInputNode()->m_textLabel->setOpacity(100);
        fadeOutInput->getBGSprite()->setOpacity(40);

        fadeInInput->setEnabled(false);
        fadeInInput->getInputNode()->m_textLabel->setOpacity(100);
        fadeInInput->getBGSprite()->setOpacity(40);

        onlySongToggle->setCascadeOpacityEnabled(true);
        onlySongToggle->setEnabled(false);
        recordAudioToggle->setCascadeOpacityEnabled(true);
        recordAudioToggle->setEnabled(false);

        onlySongToggle->setOpacity(100);
        recordAudioToggle->setOpacity(100);

        sfxSlider->setID("disabled-slider"_spr);
        musicSlider->setID("disabled-slider"_spr);

        sfxSlider->m_sliderBar->setOpacity(100);
        sfxSlider->m_groove->setOpacity(100);
        sfxSlider->m_touchLogic->setOpacity(100);
        musicSlider->m_sliderBar->setOpacity(100);
        musicSlider->m_groove->setOpacity(100);
        musicSlider->m_touchLogic->setOpacity(100);
    }

    return true;
}

void RenderSettingsLayer::onSlider(CCObject*) {
    Mod::get()->setSavedValue("render_sfx_volume", sfxSlider->getValue());
    Mod::get()->setSavedValue("render_music_volume", musicSlider->getValue());
}

void RenderSettingsLayer::showInfoPopup(CCObject* obj) {
    int tag = static_cast<CCNode*>(obj)->getTag();
    std::string title = tag ? "Legacy Audio" : "Record Audio";
    std::string msg = tag ? "Adds only the original level song to the video, ignoring all song and SFX triggers." : "Records the game's audio in another attempt and adds it to the video. This ensures all song and SFX triggers are captured.";
    FLAlertLayer::create(
        title.c_str(),
        msg.c_str(),
        "Ok"
    )->show();
};