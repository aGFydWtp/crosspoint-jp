#pragma once
#include <I18n.h>

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "GfxRenderer.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

/**
 * Modal single-choice list drawn on top of the activity that owns it.
 *
 * Ported from the upstream CrossPoint component, minus its touch-input paths: the X3/X4 panels
 * have no touch digitiser, so selection is Up/Down + Confirm only and the cached hit-test layout
 * the upstream version needs is unnecessary here.
 *
 * Usage from an Activity:
 *   loop()   -- if (popup.isActive()) { popup.handleInput(mappedInput, [this]{ requestUpdate(); }); return; }
 *   render() -- if (popup.processRender(renderer, mappedInput)) return;   // before clearScreen()
 *
 * The popup is drawn over the existing framebuffer contents, so the caller must not clear the
 * screen first -- the activity's own list stays visible behind the dialog.
 */
class OptionPopup {
 public:
  void show(const StrId titleId, const StrId* optionIds, const int optionCount, const int currentIndex,
            std::function<void(int)> onSelect) {
    title = I18N.get(titleId);
    ownedStrings.resize(optionCount);
    for (int i = 0; i < optionCount; i++) {
      ownedStrings[i] = I18N.get(optionIds[i]);
    }
    selectedIndex = currentIndex;
    onSelectCallback = std::move(onSelect);
    active = true;
  }

  void show(const char* titleStr, const char* const* options, const int optionCount, const int currentIndex,
            std::function<void(int)> onSelect) {
    title = titleStr;
    ownedStrings.resize(optionCount);
    for (int i = 0; i < optionCount; i++) {
      ownedStrings[i] = options[i];
    }
    selectedIndex = currentIndex;
    onSelectCallback = std::move(onSelect);
    active = true;
  }

  void show(const StrId titleId, const std::vector<std::string>& options, const int currentIndex,
            std::function<void(int)> onSelect) {
    title = I18N.get(titleId);
    ownedStrings = options;
    selectedIndex = currentIndex;
    onSelectCallback = std::move(onSelect);
    active = true;
  }

  /// Consumes one input event. Returns true while the popup is up, so the caller can return early
  /// from loop() and keep the activity's own bindings from firing on the same press.
  bool handleInput(MappedInputManager& input, const std::function<void()>& requestUpdate) {
    if (!active) return false;

    const int count = static_cast<int>(ownedStrings.size());
    if (input.wasPressed(MappedInputManager::Button::Up) || input.wasPressed(MappedInputManager::Button::Left)) {
      selectedIndex = (selectedIndex - 1 + count) % count;
      requestUpdate();
    } else if (input.wasPressed(MappedInputManager::Button::Down) ||
               input.wasPressed(MappedInputManager::Button::Right)) {
      selectedIndex = (selectedIndex + 1) % count;
      requestUpdate();
    } else if (input.wasPressed(MappedInputManager::Button::Confirm)) {
      active = false;
      if (onSelectCallback) onSelectCallback(selectedIndex);
      requestUpdate();
    } else if (input.wasPressed(MappedInputManager::Button::Back)) {
      active = false;
      requestUpdate();
    }
    return true;
  }

  /// Draws the popup and pushes the framebuffer. Returns false when inactive so the caller falls
  /// through to its own rendering.
  bool processRender(GfxRenderer& renderer, const MappedInputManager& input) const {
    if (!active) return false;
    const auto popupLabels = input.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, popupLabels.btn1, popupLabels.btn2, popupLabels.btn3, popupLabels.btn4);
    render(renderer);
    renderer.displayBuffer();
    return true;
  }

  void render(const GfxRenderer& renderer) const {
    if (!active) return;
    GUI.drawOptionPopup(renderer, title.c_str(), ownedStrings, selectedIndex);
  }

  bool isActive() const { return active; }

 private:
  bool active = false;
  std::string title;
  std::vector<std::string> ownedStrings;
  int selectedIndex = 0;
  std::function<void(int)> onSelectCallback;
};
