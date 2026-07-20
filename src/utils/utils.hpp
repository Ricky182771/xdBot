
#pragma once

#include "../includes.hpp"

class Utils {
public:
    static std::string toLower(std::string str);

    static std::time_t getFileCreationTime(const std::filesystem::path& path);

    static std::string formatTime(std::time_t time);

    static std::string getTexture();

    static std::string getSimplifiedString(std::string str);

    // Geode v5: Popup::m_bgSprite is now a geode::NineSlice (CCNodeRGBA), not a
    // CCScale9Sprite. CCNodeRGBA is the common base that also covers CCScale9Sprite.
    static void setBackgroundColor(cocos2d::CCNodeRGBA* bg);

    static std::vector<std::string> splitByChar(std::string str, char splitChar);

    static int copyFile(const std::string& sourcePath, const std::string& destinationPath);
    
    static std::string narrow(const wchar_t* str);
    static inline auto narrow(const std::wstring& str) {
        return narrow(str.c_str());
    }

    static std::wstring widen(const char* str);
    static inline auto widen(const std::string& str) {
        return widen(str.c_str());
    }

};