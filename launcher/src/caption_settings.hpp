#pragma once
#include <string>
#include <string_view>
#include <vector>

namespace deckback {

class ConfigStore;

// The live "CC button" configuration: the ordered preferred-language list, the track-source policy
// (authored vs auto-generated vs machine translation), remember-last, and the confirmation toast.
// It is the single owner of caption behaviour, shared by pointer between the input layer (which
// renders + evaluates the View toggle and toasts the result) and the OSD Captions sub-tab (which
// displays and edits it). Edits persist through the ConfigStore (user.json). Single-threaded: only
// the input thread touches it — GamepadInput::toggle_captions and OsdMenuController::exec both run
// there.

struct CaptionOption {
  std::string value;  // stored value ("author_first", "en", ...)
  std::string label;  // shown to the user ("Author, then auto", "English")
};

// One row of the OSD Captions sub-tab. `kind` is "combo" (a value cycled with ←/→) or "langlist"
// (the dynamic preferred-language list: A on an entry removes it, A on the trailing add-row opens
// the picker).
struct CaptionRow {
  std::string key;                     // "type" | "remember" | "toast" | "languages"
  std::string label;                   // "Caption type"
  std::string kind;                    // "combo" | "langlist"
  std::vector<CaptionOption> options;  // combo: the choices in cycle order
  std::string value;                   // combo: the current option value
  std::vector<CaptionOption> items;    // langlist: current entries {code,label}, priority order
};

// A (code, English name) pair for the language tables.
struct LangName {
  std::string_view code;
  std::string_view name;
};

class CaptionSettings {
 public:
  // `control` is the master kill switch: "local" (our preferred-language override, the default) or
  // "youtube" (defer to YouTube; View is a plain on/off toggle in the system language). `on` is the
  // persisted on/off state; `remember` is whether that state (and the toggle) survive a restart.
  // `system_lang` is the resolved SteamOS-locale subtag ("en").
  CaptionSettings(ConfigStore* store, std::string control, std::vector<std::string> languages,
                  std::string type, bool remember, bool toast, bool on, std::string system_lang);

  bool local_mode() const { return control_ == "local"; }
  bool toast_enabled() const { return toast_; }

  // The toggle_captions.js call for a View press (op:"toggle") — reads the actual current caption
  // state and flips it (our language in local mode, the system language in youtube mode).
  std::string toggle_js() const;
  // The op:"apply" call, evaluated repeatedly across a short window on a video start in local mode:
  // enforce our on/off state in our preferred language, or seed the state from YouTube's own on the
  // first video. `relax` (set only on the launcher's final ticks) permits the cross-language
  // fallback once the wait for a preferred language is nearly up; until then apply holds out.
  std::string apply_js(bool relax = false) const;

  // Record the on/off state after a View toggle (persisted when remember is on). `note_apply`
  // records a seeded state from an apply result ("seed_on:.."/"seed_off").
  void set_on(bool on);
  void note_apply(std::string_view result);

  // OSD model: the sub-tab rows (current values), plus the picker's curated + full language lists.
  std::vector<CaptionRow> osd_rows() const;
  std::vector<CaptionOption> picker_languages() const;  // curated first, then the rest

  // The whole OSD model as a compact JSON object {rows:[...], langs:[...]}, handed to osd.js
  // verbatim (via ScriptParams::set_raw) so the sub-tab renders combos + the language list + the
  // picker.
  std::string osd_model_json() const;

  // Apply an "cc.*" action from the OSD; true when a setting actually changed (OSD then re-reads).
  bool apply_action(std::string_view action);

 private:
  void persist_languages() const;
  std::string label_for(std::string_view code) const;
  std::vector<std::string> effective_languages() const;  // "system"-expanded, deduped

  ConfigStore* store_;
  std::string control_;
  std::vector<std::string> languages_;
  std::string type_;
  bool remember_;
  bool toast_;
  bool on_;        // current local on/off state
  bool on_known_;  // whether on_ is a real state yet (false until seeded from the first video)
  std::string system_lang_;
};

// The curated ←/→ shortlist and the full picker list (code -> English name), and the caption_type
// option table. Free functions so the OSD and tests can reach them without a CaptionSettings.
const std::vector<LangName>& curated_languages();
const std::vector<LangName>& all_languages();
const std::vector<CaptionOption>& caption_type_options();
std::string language_name(std::string_view code);  // "en" -> "English"; unknown -> the code itself

}  // namespace deckback
