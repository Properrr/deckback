#include "caption_settings.hpp"

#include <algorithm>

#include "config_store.hpp"
#include "json.hpp"
#include "scripts.hpp"

namespace deckback {
namespace {

// The caption_type policy, in ←/→ cycle order. Values match Config::caption_type and the JS.
const std::vector<CaptionOption> kTypeOptions = {
    {"author_first", "Author, then auto"},
    {"auto_first", "Auto, then author"},
    {"author_only", "Author only"},
    {"auto_only", "Auto (generated) only"},
};

const std::vector<CaptionOption> kControlOptions = {
    {"local", "Local override"},
    {"youtube", "Use YouTube"},
};

const std::vector<CaptionOption> kOnOffOptions = {
    {"on", "On"},
    {"off", "Off"},
};

// The short list ←/→ cycles and the picker shows first — the languages most people want, in rough
// global-usage order.
const std::vector<LangName> kCurated = {
    {"en", "English"},    {"es", "Spanish"}, {"pt", "Portuguese"}, {"fr", "French"},
    {"de", "German"},     {"it", "Italian"}, {"uk", "Ukrainian"},  {"hi", "Hindi"},
    {"ja", "Japanese"},   {"ko", "Korean"},  {"zh", "Chinese"},    {"ar", "Arabic"},
    {"id", "Indonesian"}, {"tr", "Turkish"},
};

// The full picker list (ISO 639-1 primary languages YouTube commonly carries). The launcher matches
// on the primary subtag, so regional variants (pt-BR, zh-Hant) collapse onto these.
const std::vector<LangName> kAll = {
    {"af", "Afrikaans"},  {"sq", "Albanian"},    {"am", "Amharic"},    {"ar", "Arabic"},
    {"hy", "Armenian"},   {"az", "Azerbaijani"}, {"eu", "Basque"},     {"be", "Belarusian"},
    {"bn", "Bengali"},    {"bs", "Bosnian"},     {"bg", "Bulgarian"},  {"my", "Burmese"},
    {"ca", "Catalan"},    {"zh", "Chinese"},     {"hr", "Croatian"},   {"cs", "Czech"},
    {"da", "Danish"},     {"nl", "Dutch"},       {"en", "English"},    {"et", "Estonian"},
    {"fil", "Filipino"},  {"fi", "Finnish"},     {"fr", "French"},     {"gl", "Galician"},
    {"ka", "Georgian"},   {"de", "German"},      {"el", "Greek"},      {"gu", "Gujarati"},
    {"he", "Hebrew"},     {"hi", "Hindi"},       {"hu", "Hungarian"},  {"is", "Icelandic"},
    {"id", "Indonesian"}, {"it", "Italian"},     {"ja", "Japanese"},   {"kn", "Kannada"},
    {"kk", "Kazakh"},     {"km", "Khmer"},       {"ko", "Korean"},     {"lo", "Lao"},
    {"lv", "Latvian"},    {"lt", "Lithuanian"},  {"mk", "Macedonian"}, {"ms", "Malay"},
    {"ml", "Malayalam"},  {"mr", "Marathi"},     {"mn", "Mongolian"},  {"ne", "Nepali"},
    {"no", "Norwegian"},  {"fa", "Persian"},     {"pl", "Polish"},     {"pt", "Portuguese"},
    {"pa", "Punjabi"},    {"ro", "Romanian"},    {"ru", "Russian"},    {"sr", "Serbian"},
    {"si", "Sinhala"},    {"sk", "Slovak"},      {"sl", "Slovenian"},  {"es", "Spanish"},
    {"sw", "Swahili"},    {"sv", "Swedish"},     {"ta", "Tamil"},      {"te", "Telugu"},
    {"th", "Thai"},       {"tr", "Turkish"},     {"uk", "Ukrainian"},  {"ur", "Urdu"},
    {"uz", "Uzbek"},      {"vi", "Vietnamese"},  {"zu", "Zulu"},
};

bool contains(const std::vector<std::string>& v, std::string_view s) {
  return std::find(v.begin(), v.end(), s) != v.end();
}

bool is_caption_type(std::string_view value) {
  return std::any_of(kTypeOptions.begin(), kTypeOptions.end(),
                     [&](const CaptionOption& option) { return option.value == value; });
}

}  // namespace

const std::vector<LangName>& curated_languages() { return kCurated; }
const std::vector<LangName>& all_languages() { return kAll; }
const std::vector<CaptionOption>& caption_type_options() { return kTypeOptions; }

std::string language_name(std::string_view code) {
  for (const LangName& l : kAll)
    if (l.code == code) return std::string(l.name);
  return std::string(code);
}

CaptionSettings::CaptionSettings(ConfigStore* store, std::string control,
                                 std::vector<std::string> languages, std::string type,
                                 bool remember, bool toast, bool on, std::string system_lang)
    : store_(store),
      control_(std::move(control)),
      languages_(std::move(languages)),
      type_(std::move(type)),
      remember_(remember),
      toast_(toast),
      on_(on),
      on_known_(remember),
      system_lang_(std::move(system_lang)) {
  if (control_ != "local" && control_ != "youtube") control_ = "local";
  if (!is_caption_type(type_)) type_ = "author_first";
  if (languages_.empty()) languages_.push_back("system");
}

std::vector<std::string> CaptionSettings::effective_languages() const {
  std::vector<std::string> langs;
  langs.reserve(languages_.size());
  for (const std::string& code : languages_) {
    std::string c = code;
    if (c == "system") {
      if (system_lang_.empty()) continue;
      c = system_lang_;
    }
    if (!contains(langs, c)) langs.push_back(c);
  }
  return langs;
}

std::string CaptionSettings::label_for(std::string_view code) const {
  if (code == "system") {
    if (system_lang_.empty()) return "System language";
    return "System (" + language_name(system_lang_) + ")";
  }
  return language_name(code);
}

std::string CaptionSettings::toggle_js() const {
  return ScriptLibrary::instance().render("toggle_captions",
                                          ScriptParams()
                                              .set("op", std::string_view("toggle"))
                                              .set("mode", std::string_view(control_))
                                              .set("langs", effective_languages())
                                              .set("type", std::string_view(type_))
                                              .set("sys", std::string_view(system_lang_)));
}

std::string CaptionSettings::apply_js(bool relax) const {
  return ScriptLibrary::instance().render("toggle_captions",
                                          ScriptParams()
                                              .set("op", std::string_view("apply"))
                                              .set("mode", std::string_view(control_))
                                              .set("on", on_)
                                              .set("known", on_known_)
                                              .set("relax", relax)
                                              .set("langs", effective_languages())
                                              .set("type", std::string_view(type_))
                                              .set("sys", std::string_view(system_lang_)));
}

void CaptionSettings::set_on(bool on) {
  on_ = on;
  on_known_ = true;
  if (remember_ && store_) store_->set_bool("caption_on", on_);
}

void CaptionSettings::note_apply(std::string_view result) {
  if (result.rfind("seed_on:", 0) == 0)
    set_on(true);
  else if (result == "seed_off")
    set_on(false);
}

void CaptionSettings::persist_languages() const {
  if (store_) store_->set_string_list("caption_languages", languages_);
}

std::vector<CaptionRow> CaptionSettings::osd_rows() const {
  std::vector<CaptionRow> rows;

  CaptionRow control;
  control.key = "control";
  control.label = "Caption control";
  control.kind = "combo";
  control.options = kControlOptions;
  control.value = control_;
  rows.push_back(std::move(control));

  CaptionRow langs;
  langs.key = "languages";
  langs.label = "Preferred languages";
  langs.kind = "langlist";
  for (const std::string& code : languages_) langs.items.push_back({code, label_for(code)});
  rows.push_back(std::move(langs));

  CaptionRow type;
  type.key = "type";
  type.label = "Caption type";
  type.kind = "combo";
  type.options = kTypeOptions;
  type.value = type_;
  rows.push_back(std::move(type));

  CaptionRow remember;
  remember.key = "remember";
  remember.label = "Remember on/off after restart";
  remember.kind = "combo";
  remember.options = kOnOffOptions;
  remember.value = remember_ ? "on" : "off";
  rows.push_back(std::move(remember));

  CaptionRow toast;
  toast.key = "toast";
  toast.label = "Confirmation toast";
  toast.kind = "combo";
  toast.options = kOnOffOptions;
  toast.value = toast_ ? "on" : "off";
  rows.push_back(std::move(toast));

  return rows;
}

std::vector<CaptionOption> CaptionSettings::picker_languages() const {
  std::vector<CaptionOption> out;
  std::vector<std::string> seen;
  out.reserve(kAll.size() + 1);
  seen.reserve(kAll.size() + 1);
  auto add = [&](std::string_view code, std::string_view name) {
    if (contains(seen, code)) return;
    seen.emplace_back(code);
    out.push_back({std::string(code), std::string(name)});
  };
  add("system", label_for("system"));
  for (const LangName& l : kCurated) add(l.code, l.name);
  for (const LangName& l : kAll) add(l.code, l.name);
  return out;
}

std::string CaptionSettings::osd_model_json() const {
  using json::Value;
  auto options_array = [](const std::vector<CaptionOption>& opts) {
    std::vector<Value> a;
    a.reserve(opts.size());
    for (const CaptionOption& o : opts)
      a.push_back(Value::object({{"value", Value(o.value)}, {"label", Value(o.label)}}));
    return Value::array(std::move(a));
  };

  std::vector<Value> rows;
  rows.reserve(5);
  for (const CaptionRow& r : osd_rows()) {
    std::vector<json::Member> m = {
        {"key", Value(r.key)}, {"label", Value(r.label)}, {"kind", Value(r.kind)}};
    if (r.kind == "combo") {
      m.emplace_back("value", Value(r.value));
      m.emplace_back("options", options_array(r.options));
    } else {
      m.emplace_back("items", options_array(r.items));
    }
    rows.push_back(Value::object(std::move(m)));
  }

  Value model = Value::object(
      {{"rows", Value::array(std::move(rows))}, {"langs", options_array(picker_languages())}});
  return json::dump(model, -1);
}

bool CaptionSettings::apply_action(std::string_view action) {
  constexpr std::string_view kPrefix = "cc.";
  if (action.substr(0, kPrefix.size()) != kPrefix) return false;
  action.remove_prefix(kPrefix.size());
  const size_t eq = action.find('=');
  const std::string_view key = action.substr(0, eq);
  const std::string_view val =
      eq == std::string_view::npos ? std::string_view{} : action.substr(eq + 1);

  if (key == "control") {
    if ((val != "local" && val != "youtube") || control_ == val) return false;
    control_ = std::string(val);
    if (store_) store_->set_string("caption_control", control_);
    return true;
  }
  if (key == "type") {
    if (!is_caption_type(val) || type_ == val) return false;
    type_ = std::string(val);
    if (store_) store_->set_string("caption_type", type_);
    return true;
  }
  if (key == "remember" || key == "toast") {
    const bool on = (val == "on");
    bool& field = (key == "remember") ? remember_ : toast_;
    if (field == on) return false;
    field = on;
    if (store_) store_->set_bool(key == "remember" ? "caption_remember" : "captions_toast", on);
    return true;
  }
  if (key == "lang.add") {
    if (val.empty() || contains(languages_, val)) return false;
    languages_.emplace_back(val);
    persist_languages();
    return true;
  }
  if (key == "lang.remove") {
    auto it = std::find(languages_.begin(), languages_.end(), val);
    if (it == languages_.end()) return false;
    languages_.erase(it);
    if (languages_.empty()) languages_.push_back("system");
    persist_languages();
    return true;
  }
  if (key == "lang.up" || key == "lang.down") {
    auto it = std::find(languages_.begin(), languages_.end(), val);
    if (it == languages_.end()) return false;
    const size_t i = static_cast<size_t>(std::distance(languages_.begin(), it));
    const size_t j = (key == "lang.up") ? (i == 0 ? i : i - 1) : (i + 1);
    if (j == i || j >= languages_.size()) return false;
    std::swap(languages_[i], languages_[j]);
    persist_languages();
    return true;
  }
  return false;
}

}  // namespace deckback
