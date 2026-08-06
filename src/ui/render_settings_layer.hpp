#pragma once

#include "../includes.hpp"

class RenderSettingsLayer : public geode::Popup, public TextInputDelegate {
	
public:

	Slider* sfxSlider = nullptr;
	Slider* musicSlider = nullptr;
	TextInput* fadeInInput = nullptr;
	TextInput* fadeOutInput = nullptr;
	TextInput* extensionInput = nullptr;

	CCTextInputNode* argsInput = nullptr;
	CCTextInputNode* audioArgsInput = nullptr;
	CCTextInputNode* secondsInput = nullptr;
	CCTextInputNode* videoArgsInput = nullptr;

	CCMenuItemToggler* onlySongToggle = nullptr;
	CCMenuItemToggler* recordAudioToggle = nullptr;

	// Only created when the native (Wine/Proton) backend is the selected one: a read-only
	// echo of the arguments that get prepended to every render, plus the gear that opens
	// the editor for them. Null on every other backend.
	CCLabelBMFont* nativePrefixLabel = nullptr;
	float nativePrefixWidth = 0.f;

	Mod* mod = nullptr;

private:

	bool setup();

public:

	STATIC_CREATE(RenderSettingsLayer, 396, 277)
	
	void open(CCObject*) {
		create()->show();
	}

	void close(CCObject*) {
		keyBackClicked();
	}

	void textChanged(CCTextInputNode* node) override;

	void onSlider(CCObject*);

	void onDefaults(CCObject*);

	void showInfoPopup(CCObject*);

	// Opens the compact editor for the native encoder args / filters.
	void onEditNativeArgs(CCObject*);

	// Re-reads render_native_args / render_native_filters and re-truncates the locked
	// preview. Safe to call when the label does not exist.
	void refreshNativePrefix();
};