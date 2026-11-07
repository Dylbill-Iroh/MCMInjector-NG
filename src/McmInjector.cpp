#include "McmInjector.h"
#include "RE/T/TypeTraits.h"
#include <atomic>

// Native functions for McmInjector.psc: SkyUI MCM options with no 128-option limit per page,
// without editing SkyUI's .swf or .pex files.
//
// How it works:
// - SKI_ConfigBase keeps the page's options in 4 private 128-slot Papyrus arrays (_optionFlagsBuf, _textBuf,
//   _strValueBuf, _numValueBuf). After OnPageReset, WriteOptionBuffers sends them to configPanel.flushOptionBuffers,
//   which rebuilds the AS2 option list. The AS2 list itself has no size limit.
// - The cursor is SKI's own _cursorPosition / _cursorFillMode, read and written directly, so the native Add functions,
//   SetCursorPosition and SKI's own ST Add functions all share one cursor.
// - Options below index 128 are written straight into SKI's buffers, exactly like SKI's AddOption. So everything SKI
//   does with them (ST options, Set*OptionValue, dialogs) keeps working unchanged.
// - Options at index 128+ are queued here. configPanel.flushOptionBuffers is wrapped at runtime: the wrapper runs
//   SkyUI's original first, then adds the queued options to the list.
// - SKI's _stateOptionMap is grown past 128 so SKI's SelectOption / HighlightOption / dialog handlers don't log
//   out-of-range errors for options at 128+, and so the ST Add functions can store state names at 128+.
//   SKI's ClearOptionBuffers only clears slots 0-127 on each page, so slots 128+ are cleared here at the start of
//   the next page (detected by counting flushOptionBuffers calls). 

namespace MCM {
    constexpr const char* kPanelPath = "_root.ConfigPanelFader.configPanel";
    constexpr std::uint32_t kSkiBufferSize = 128;
    constexpr std::int32_t kStateReset = 1;  // SKI_ConfigBase STATE_RESET

    // Same values as SKI_ConfigBase OPTION_TYPE_*
    enum OptionType : int {
        kEmpty = 0,
        kHeader = 1,
        kText = 2,
        kToggle = 3,
        kSlider = 4,
        kMenu = 5,
        kColor = 6,
        kKeymap = 7,
        kInput = 8
    };

    struct PendingOption {
        std::uint32_t index = 0;  // list index, set when the option is added
        int type = kText;
        std::string text;
        std::string strValue;
        bool hasStrValue = true;  // false -> strValue = null (like Papyrus none)
        float numValue = 0.0f;
        int flags = 0;
    };

    std::mutex pendingMutex;
    std::vector<PendingOption> pendingOptions;  // options at index 128+, waiting for the next flushOptionBuffers

    // Counts completed page builds (flushOptionBuffers calls). Used to tell when a new page has started.
    std::atomic<std::uint32_t> flushSerial{ 0 };

    // State names written to _stateOptionMap at index 128+, and the flushSerial of the page they belong to.
    std::mutex stateMutex;
    bool highStatesWritten = false;
    std::uint32_t highStatesSerial = 0;

    void ClearPending() {
        pendingMutex.lock();
        pendingOptions.clear();
        pendingMutex.unlock();
    }

    // ---------------------------------------------------------------------------------------------------------------
    // UI side (runs on the UI thread)

    void SetEntryString(RE::GFxMovie* movie, RE::GFxValue& entry, const char* member, const std::string& str, RE::GFxValue* translator) {
        RE::GFxValue value;
        movie->CreateString(&value, str.c_str());

        // SkyUI translates option text with Translator.translateNested. Only call it for "$" strings so plain text stays fast.
        if (translator && !str.empty() && str[0] == '$') {
            RE::GFxValue translated;
            if (translator->Invoke("translateNested", &translated, &value, 1) && translated.IsString()) {
                entry.SetMember(member, translated);
                return;
            }
        }
        entry.SetMember(member, value);
    }

    void PushEmptyEntry(RE::GFxMovie* movie, RE::GFxValue& entries) {
        RE::GFxValue entry;
        movie->CreateObject(&entry);
        entry.SetMember("optionType", RE::GFxValue(static_cast<double>(kEmpty)));
        entries.PushBack(entry);
    }

    // Runs right after SkyUI's own flushOptionBuffers.
    void ApplyPending(RE::GFxMovie* movie, RE::GFxValue& panel) {
        pendingMutex.lock();
        if (pendingOptions.empty()) {
            pendingMutex.unlock();
            return;
        }
        std::vector<PendingOption> options = std::move(pendingOptions);
        pendingOptions.clear();
        pendingMutex.unlock();

        RE::GFxValue list;
        if (!panel.GetMember("_optionsList", &list) || !list.IsObject()) {
            logger::warn("mcm::ApplyPending: _optionsList not found");
            return;
        }

        RE::GFxValue entries;
        if (!list.GetMember("_entryList", &entries) || !entries.IsArray()) {
            logger::warn("mcm::ApplyPending: _entryList not found");
            return;
        }

        RE::GFxValue translator;
        movie->GetVariable(&translator, "_global.skyui.util.Translator");
        RE::GFxValue* pTranslator = translator.IsObject() ? &translator : nullptr;

        RE::GFxValue nullValue;
        nullValue.SetNull();

        std::uint32_t size = entries.GetArraySize();

        for (auto& opt : options) {
            while (size < opt.index) {
                PushEmptyEntry(movie, entries);
                size++;
            }

            RE::GFxValue entry;
            movie->CreateObject(&entry);
            entry.SetMember("optionType", RE::GFxValue(static_cast<double>(opt.type)));
            SetEntryString(movie, entry, "text", opt.text, pTranslator);

            if (opt.hasStrValue) {
                SetEntryString(movie, entry, "strValue", opt.strValue, nullptr);
            } else {
                entry.SetMember("strValue", nullValue);
            }

            entry.SetMember("numValue", RE::GFxValue(static_cast<double>(opt.numValue)));
            entry.SetMember("flags", RE::GFxValue(static_cast<double>(opt.flags & 0xFF)));

            if (opt.index < size) {
                entries.SetElement(opt.index, entry);
            } else {
                entries.PushBack(entry);
                size++;
            }
        }

        // SkyUI keeps the list length even, so keyboard navigation between the 2 columns stays symmetrical.
        if (entries.GetArraySize() % 2 != 0) {
            PushEmptyEntry(movie, entries);
        }

        list.Invoke("InvalidateData", nullptr, nullptr, 0);
    }

    class FlushOptionBuffersHandler : public RE::GFxFunctionHandler {
    public:
        static FlushOptionBuffersHandler* GetSingleton() {
            // Allocated once and never freed on purpose: GFx holds references to it from each Journal Menu movie.
            static FlushOptionBuffersHandler* singleton = new FlushOptionBuffersHandler();
            return singleton;
        }

        void Call(Params& params) override {
            RE::GFxValue panel;
            if (!params.movie || !params.movie->GetVariable(&panel, kPanelPath) || !panel.IsObject()) {
                return;
            }

            // Run SkyUI's original flushOptionBuffers(count) first.
            panel.Invoke("_dbOrigFlush", nullptr, params.args, params.argCount);

            ApplyPending(params.movie, panel);

            flushSerial++;  // this page is complete
        }

    private:
        FlushOptionBuffersHandler() = default;
    };

    // Wraps configPanel.flushOptionBuffers for this instance of the Journal Menu movie.
    // A new movie is created each time the menu opens, so this re-installs when needed.
    bool InstallFlushHook(RE::GFxMovieView* movie) {
        RE::GFxValue panel;
        if (!movie->GetVariable(&panel, kPanelPath) || !panel.IsObject()) {
            return false;
        }

        RE::GFxValue existing;
        if (panel.GetMember("_dbOrigFlush", &existing) && !existing.IsUndefined()) {
            return true;  // already installed on this movie
        }

        RE::GFxValue original;
        if (!panel.GetMember("flushOptionBuffers", &original) || original.IsUndefined()) {
            return false;
        }

        RE::GFxValue wrapper;
        movie->CreateFunction(&wrapper, FlushOptionBuffersHandler::GetSingleton());

        panel.SetMember("_dbOrigFlush", original);
        panel.SetMember("flushOptionBuffers", wrapper);  // instance member shadows the prototype method
        return true;
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Papyrus side

    // Finds the SKI_ConfigBase script object (or a script that extends it) attached to the quest.
    RE::BSTSmartPointer<RE::BSScript::Object> FindConfigObject(RE::BSScript::Internal::VirtualMachine* vm, RE::TESQuest* quest) {
        RE::BSTSmartPointer<RE::BSScript::Object> result;

        auto* policy = vm->GetObjectHandlePolicy();
        if (!policy || !quest) {
            return result;
        }

        RE::VMHandle handle = policy->GetHandleForObject(RE::FormType::Quest, quest);
        if (handle == policy->EmptyHandle()) {
            return result;
        }

        vm->attachedScriptsLock.Lock();
        auto it = vm->attachedScripts.find(handle);
        if (it != vm->attachedScripts.end()) {
            auto& scripts = it->second;
            for (std::uint32_t i = 0; i < scripts.size() && !result; i++) {
                auto* object = scripts[i].get();
                if (!object) {
                    continue;
                }
                for (auto* info = object->GetTypeInfo(); info; info = info->GetParent()) {
                    if (_stricmp(info->GetName(), "SKI_ConfigBase") == 0) {
                        result = RE::BSTSmartPointer<RE::BSScript::Object>(object);
                        break;
                    }
                }
            }
        }
        vm->attachedScriptsLock.Unlock();

        return result;
    }

    std::int32_t GetIntVar(RE::BSScript::Object* object, const char* name, std::int32_t defaultValue) {
        auto* var = object->GetVariable(name);
        return (var && var->IsInt()) ? var->GetSInt() : defaultValue;
    }

    void SetIntVar(RE::BSScript::Object* object, const char* name, std::int32_t value) {
        auto* var = object->GetVariable(name);
        if (var && var->IsInt()) {
            var->SetSInt(value);
        }
    }

    RE::BSTSmartPointer<RE::BSScript::Array> GetArrayVar(RE::BSScript::Object* object, const char* name) {
        auto* var = object->GetVariable(name);
        return (var && var->IsArray()) ? var->GetArray() : nullptr;
    }

    // SKI_ConfigBase's private _stateOptionMap is a string[128]. SelectOption, HighlightOption, ResetOption and
    // the Request*DialogData functions read _stateOptionMap[index] for every option. For index 128+ that logs
    // "array index out of range" errors. Growing it (extra slots are always "") lets SKI's own handlers run cleanly.
    // OpenConfig recreates it at 128 each time the MCM is opened, so this is checked on every call.
    void EnsureStateMapSize(RE::BSScript::Internal::VirtualMachine* vm, RE::BSScript::Object* object, std::uint32_t needed) {
        auto* var = object->GetVariable("_stateOptionMap");
        if (!var || !var->IsArray()) {
            logger::warn("mcm: _stateOptionMap not found");
            return;
        }

        auto oldArray = var->GetArray();
        std::uint32_t oldSize = oldArray ? oldArray->size() : 0;
        if (oldSize >= needed) {
            return;
        }

        RE::BSTSmartPointer<RE::BSScript::Array> newArray;
        if (!vm->CreateArray(RE::BSScript::TypeInfo(RE::BSScript::TypeInfo::RawType::kString), needed, newArray) || !newArray) {
            logger::warn("mcm: failed to create _stateOptionMap array");
            return;
        }

        for (std::uint32_t i = 0; i < needed; i++) {
            if (i < oldSize) {
                (*newArray)[i].SetString((*oldArray)[i].GetString());
            } else {
                (*newArray)[i].SetString("");
            }
        }

        var->SetArray(newArray);
    }

    // Clears state names at index 128+ left over from the previous page. SKI's ClearOptionBuffers only clears 0-127.
    // Only called while adding options (inside OnPageReset). After a page is built, flushSerial has moved on but the
    // page's events still need its state names, so this must not run from event-time functions.
    void ClearStaleHighStates(RE::BSScript::Object* object) {
        stateMutex.lock();
        bool stale = highStatesWritten && highStatesSerial != flushSerial.load();
        if (stale) {
            highStatesWritten = false;
        }
        stateMutex.unlock();

        if (!stale) {
            return;
        }

        auto map = GetArrayVar(object, "_stateOptionMap");
        if (!map) {
            return;
        }
        for (std::uint32_t i = kSkiBufferSize; i < map->size(); i++) {
            (*map)[i].SetString("");
        }
    }

    void MarkHighStateWritten() {
        stateMutex.lock();
        highStatesWritten = true;
        highStatesSerial = flushSerial.load();
        stateMutex.unlock();
    }

    // Same as SKI_ConfigBase.GetStateOptionIndex without the "" -> GetState() part. Papyrus string compares ignore case.
    std::int32_t FindStateOption(RE::BSScript::Object* object, std::string_view stateName) {
        auto map = GetArrayVar(object, "_stateOptionMap");
        if (!map || stateName.empty()) {
            return -1;
        }
        std::string name(stateName);
        for (std::uint32_t i = 0; i < map->size(); i++) {
            auto entry = (*map)[i].GetString();
            if (!entry.empty() && _stricmp(std::string(entry).c_str(), name.c_str()) == 0) {
                return static_cast<std::int32_t>(i);
            }
        }
        return -1;
    }

    void QueueInstallFlushHook() {
        // Queued before WriteOptionBuffers queues its UI.Invoke calls, and the UI task queue runs in order,
        // so the wrapper is in place before SkyUI calls flushOptionBuffers for this page.
        SKSE::GetTaskInterface()->AddUITask([]() {
            auto* ui = RE::UI::GetSingleton();
            auto menu = ui ? ui->GetMenu(RE::JournalMenu::MENU_NAME) : nullptr;
            if (!menu || !menu->uiMovie || !InstallFlushHook(menu->uiMovie.get())) {
                logger::warn("mcm: Journal Menu / MCM config panel not found, discarding queued options");
                ClearPending();
            }
        });
    }

    // Adds options at SKI's cursor, the same way SKI_ConfigBase.AddOption does, without the 128 cap.
    // Create one per native call, call Add for each option, then Finish.
    class OptionAdder {
    public:
        explicit OptionAdder(RE::TESQuest* quest) {
            vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
            if (!vm || !quest) {
                return;
            }

            object = FindConfigObject(vm, quest);
            if (!object) {
                logger::error("mcm: no SKI_ConfigBase script found on quest {:X}", quest->GetFormID());
                return;
            }

            auto* obj = object.get();
            if (GetIntVar(obj, "_state", 0) != kStateReset) {
                logger::error("mcm: cannot add options outside of OnPageReset() (quest {:X})", quest->GetFormID());
                return;
            }

            cursor = GetIntVar(obj, "_cursorPosition", 0);
            fillMode = GetIntVar(obj, "_cursorFillMode", 1);
            if (fillMode != 1 && fillMode != 2) {
                fillMode = 1;
            }
            pageOffset = GetIntVar(obj, "_currentPageNum", 0) * 0x100;

            flagsBuf = GetArrayVar(obj, "_optionFlagsBuf");
            textBuf = GetArrayVar(obj, "_textBuf");
            strValueBuf = GetArrayVar(obj, "_strValueBuf");
            numValueBuf = GetArrayVar(obj, "_numValueBuf");

            ClearStaleHighStates(obj);

            valid = true;
        }

        [[nodiscard]] bool IsValid() const { return valid; }

        // Returns the option ID (index + page * 0x100), or -1.
        std::int32_t Add(PendingOption&& opt) {
            if (!valid) {
                return -1;
            }

            if (cursor < 0) {
                // SKI's own AddOption (used by the ST Add functions) sets the cursor to -1 once it passes 127.
                logger::error("mcm: cursor is -1. ST options can only be added below index 128; use SetCursorPosition to move the cursor");
                return -1;
            }

            auto index = static_cast<std::uint32_t>(cursor);
            cursor += fillMode;

            if (index < kSkiBufferSize && WriteSkiBuffers(index, opt)) {
                return static_cast<std::int32_t>(index) + pageOffset;
            }

            if (opt.type != kEmpty) {  // empty slots past the end are padded automatically
                opt.index = index;
                injected.push_back(std::move(opt));
            }
            if (index > maxInjectedIndex) {
                maxInjectedIndex = index;
            }
            hasInjectedIndex = true;

            return static_cast<std::int32_t>(index) + pageOffset;
        }

        // Same as SKI_ConfigBase.AddOptionST: adds the option and stores its state name in _stateOptionMap.
        void AddST(const std::string& stateName, PendingOption&& opt) {
            if (!valid) {
                return;
            }

            if (stateName.empty()) {
                logger::error("mcm: state option name is empty");
                return;
            }

            auto* obj = object.get();
            if (FindStateOption(obj, stateName) != -1) {
                logger::error("mcm: state option name {} is already in use", stateName);
                return;
            }

            if (cursor < 0) {
                logger::error("mcm: cursor is -1; use SetCursorPosition to move the cursor");
                return;
            }

            auto index = static_cast<std::uint32_t>(cursor);
            EnsureStateMapSize(vm, obj, index + 2);

            auto map = GetArrayVar(obj, "_stateOptionMap");
            if (!map || index >= map->size()) {
                logger::error("mcm: _stateOptionMap not available for state option {}", stateName);
                return;
            }

            if (!(*map)[index].GetString().empty()) {
                logger::error("mcm: state option index {} already in use", index);
                return;
            }

            if (Add(std::move(opt)) < 0) {
                return;
            }

            (*map)[index].SetString(stateName);
            if (index >= kSkiBufferSize) {
                MarkHighStateWritten();
            }
        }

        void Finish() {
            if (!valid) {
                return;
            }

            SetIntVar(object.get(), "_cursorPosition", cursor);

            if (hasInjectedIndex) {
                EnsureStateMapSize(vm, object.get(), maxInjectedIndex + 2);  // +1 for the index itself, +1 for padding
            }

            if (injected.empty()) {
                return;
            }

            pendingMutex.lock();
            bool wasEmpty = pendingOptions.empty();
            for (auto& opt : injected) {
                pendingOptions.push_back(std::move(opt));
            }
            pendingMutex.unlock();
            injected.clear();

            if (wasEmpty) {
                QueueInstallFlushHook();  // once per page is enough
            }
        }

    private:
        // Same writes as SKI_ConfigBase.AddOption: flags buffer holds type + flags * 0x100.
        bool WriteSkiBuffers(std::uint32_t index, const PendingOption& opt) {
            if (!flagsBuf || !textBuf || !strValueBuf || !numValueBuf ||
                index >= flagsBuf->size() || index >= textBuf->size() || index >= strValueBuf->size() || index >= numValueBuf->size()) {
                return false;
            }

            (*flagsBuf)[index].SetSInt(opt.type + opt.flags * 0x100);
            (*textBuf)[index].SetString(opt.text);
            (*strValueBuf)[index].SetString(opt.hasStrValue ? std::string_view(opt.strValue) : std::string_view());
            (*numValueBuf)[index].SetFloat(opt.numValue);
            return true;
        }

        RE::BSScript::Internal::VirtualMachine* vm = nullptr;
        RE::BSTSmartPointer<RE::BSScript::Object> object;
        RE::BSTSmartPointer<RE::BSScript::Array> flagsBuf;
        RE::BSTSmartPointer<RE::BSScript::Array> textBuf;
        RE::BSTSmartPointer<RE::BSScript::Array> strValueBuf;
        RE::BSTSmartPointer<RE::BSScript::Array> numValueBuf;
        std::vector<PendingOption> injected;
        std::int32_t cursor = 0;
        std::int32_t fillMode = 1;
        std::int32_t pageOffset = 0;
        std::uint32_t maxInjectedIndex = 0;
        bool hasInjectedIndex = false;
        bool valid = false;
    };

    PendingOption MakeOption(int type, std::string text, std::string strValue, bool hasStrValue, float numValue, int flags) {
        PendingOption opt;
        opt.type = type;
        opt.text = std::move(text);
        opt.strValue = std::move(strValue);
        opt.hasStrValue = hasStrValue;
        opt.numValue = numValue;
        opt.flags = flags;
        return opt;
    }

    template <class T>
    T GetOr(const std::vector<T>& v, std::size_t i, T defaultValue) {
        return i < v.size() ? v[i] : defaultValue;
    }

    std::string TakeOr(std::vector<std::string>& v, std::size_t i, const char* defaultValue = "") {
        return i < v.size() ? std::move(v[i]) : std::string(defaultValue);
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Cursor

    // function SetCursorPosition(int position) Native  -- uncapped override
    void SetCursorPosition(RE::TESQuest* self, std::int32_t position) {
        if (position < 0) {
            return;
        }

        auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
        if (!vm) {
            return;
        }

        auto object = FindConfigObject(vm, self);
        if (object) {
            SetIntVar(object.get(), "_cursorPosition", position);
        }
    }

    // int function GetCursorPosition() Native
    std::int32_t GetCursorPosition(RE::TESQuest* self) {
        auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
        if (!vm) {
            return -1;
        }

        auto object = FindConfigObject(vm, self);
        return object ? GetIntVar(object.get(), "_cursorPosition", -1) : -1;
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Single option overrides. Same strValue / numValue as SKI_ConfigBase's versions pass to AddOption.

    std::int32_t AddOption(RE::TESQuest* self, PendingOption&& opt) {
        OptionAdder adder(self);
        std::int32_t id = adder.Add(std::move(opt));
        adder.Finish();
        return id;
    }

    std::int32_t AddEmptyOption(RE::TESQuest* self) {
        return AddOption(self, MakeOption(kEmpty, "", "", false, 0.0f, 0));
    }

    std::int32_t AddHeaderOption(RE::TESQuest* self, std::string text, std::int32_t flags) {
        return AddOption(self, MakeOption(kHeader, std::move(text), "", false, 0.0f, flags));
    }

    std::int32_t AddTextOption(RE::TESQuest* self, std::string text, std::string value, std::int32_t flags) {
        return AddOption(self, MakeOption(kText, std::move(text), std::move(value), true, 0.0f, flags));
    }

    std::int32_t AddToggleOption(RE::TESQuest* self, std::string text, bool checked, std::int32_t flags) {
        return AddOption(self, MakeOption(kToggle, std::move(text), "", false, checked ? 1.0f : 0.0f, flags));
    }

    std::int32_t AddSliderOption(RE::TESQuest* self, std::string text, float value, std::string formatString, std::int32_t flags) {
        return AddOption(self, MakeOption(kSlider, std::move(text), std::move(formatString), true, value, flags));
    }

    std::int32_t AddMenuOption(RE::TESQuest* self, std::string text, std::string value, std::int32_t flags) {
        return AddOption(self, MakeOption(kMenu, std::move(text), std::move(value), true, 0.0f, flags));
    }

    std::int32_t AddColorOption(RE::TESQuest* self, std::string text, std::int32_t color, std::int32_t flags) {
        return AddOption(self, MakeOption(kColor, std::move(text), "", false, static_cast<float>(color), flags));
    }

    std::int32_t AddKeyMapOption(RE::TESQuest* self, std::string text, std::int32_t keyCode, std::int32_t flags) {
        return AddOption(self, MakeOption(kKeymap, std::move(text), "", false, static_cast<float>(keyCode), flags));
    }

    std::int32_t AddInputOption(RE::TESQuest* self, std::string text, std::string value, std::int32_t flags) {
        return AddOption(self, MakeOption(kInput, std::move(text), std::move(value), true, 0.0f, flags));
    }

    // ---------------------------------------------------------------------------------------------------------------
    // ST option overrides, uncapped. Same values as SKI_ConfigBase's versions.

    void AddOptionST(RE::TESQuest* self, const std::string& stateName, PendingOption&& opt) {
        OptionAdder adder(self);
        adder.AddST(stateName, std::move(opt));
        adder.Finish();
    }

    void AddTextOptionST(RE::TESQuest* self, std::string stateName, std::string text, std::string value, std::int32_t flags) {
        AddOptionST(self, stateName, MakeOption(kText, std::move(text), std::move(value), true, 0.0f, flags));
    }

    void AddToggleOptionST(RE::TESQuest* self, std::string stateName, std::string text, bool checked, std::int32_t flags) {
        AddOptionST(self, stateName, MakeOption(kToggle, std::move(text), "", false, checked ? 1.0f : 0.0f, flags));
    }

    void AddSliderOptionST(RE::TESQuest* self, std::string stateName, std::string text, float value, std::string formatString, std::int32_t flags) {
        AddOptionST(self, stateName, MakeOption(kSlider, std::move(text), std::move(formatString), true, value, flags));
    }

    void AddMenuOptionST(RE::TESQuest* self, std::string stateName, std::string text, std::string value, std::int32_t flags) {
        AddOptionST(self, stateName, MakeOption(kMenu, std::move(text), std::move(value), true, 0.0f, flags));
    }

    void AddColorOptionST(RE::TESQuest* self, std::string stateName, std::string text, std::int32_t color, std::int32_t flags) {
        AddOptionST(self, stateName, MakeOption(kColor, std::move(text), "", false, static_cast<float>(color), flags));
    }

    void AddKeyMapOptionST(RE::TESQuest* self, std::string stateName, std::string text, std::int32_t keyCode, std::int32_t flags) {
        AddOptionST(self, stateName, MakeOption(kKeymap, std::move(text), "", false, static_cast<float>(keyCode), flags));
    }

    void AddInputOptionST(RE::TESQuest* self, std::string stateName, std::string text, std::string value, std::int32_t flags) {
        AddOptionST(self, stateName, MakeOption(kInput, std::move(text), std::move(value), true, 0.0f, flags));
    }

    // int Function McmiFindStateOptionIndex(string stateName) Native
    // List index of the option with this state name on the current page, or -1.
    std::int32_t McmiFindStateOptionIndex(RE::TESQuest* self, std::string stateName) {
        auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
        if (!vm) {
            return -1;
        }

        auto object = FindConfigObject(vm, self);
        if (!object) {
            return -1;
        }

        return FindStateOption(object.get(), stateName);
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Array versions. One option per text; missing values use the same defaults as the single versions.

    using IdArray = std::vector<std::int32_t>;
    using StringArray = std::vector<std::string>;
    using IntArray = std::vector<std::int32_t>;

    // Runs makeOption(i) for each i in [0, count) and returns the IDs.
    template <class F>
    IdArray AddOptions(RE::TESQuest* self, std::size_t count, F&& makeOption) {
        IdArray ids;
        OptionAdder adder(self);
        if (!adder.IsValid()) {
            return ids;
        }

        ids.reserve(count);
        for (std::size_t i = 0; i < count; i++) {
            ids.push_back(adder.Add(makeOption(i)));
        }
        adder.Finish();
        return ids;
    }

    IdArray AddEmptyOptions(RE::TESQuest* self, std::int32_t amount) {
        return AddOptions(self, amount > 0 ? static_cast<std::size_t>(amount) : 0, [](std::size_t) {
            return MakeOption(kEmpty, "", "", false, 0.0f, 0);
        });
    }

    IdArray AddHeaderOptions(RE::TESQuest* self, StringArray texts, IntArray flags) {
        return AddOptions(self, texts.size(), [&](std::size_t i) {
            return MakeOption(kHeader, std::move(texts[i]), "", false, 0.0f, GetOr(flags, i, 0));
        });
    }

    IdArray AddTextOptions(RE::TESQuest* self, StringArray texts, StringArray values, IntArray flags) {
        return AddOptions(self, texts.size(), [&](std::size_t i) {
            return MakeOption(kText, std::move(texts[i]), TakeOr(values, i), true, 0.0f, GetOr(flags, i, 0));
        });
    }

    IdArray AddToggleOptions(RE::TESQuest* self, StringArray texts, std::vector<bool> toggles, IntArray flags) {
        return AddOptions(self, texts.size(), [&](std::size_t i) {
            bool checked = i < toggles.size() && toggles[i];
            return MakeOption(kToggle, std::move(texts[i]), "", false, checked ? 1.0f : 0.0f, GetOr(flags, i, 0));
        });
    }

    IdArray AddSliderOptions(RE::TESQuest* self, StringArray texts, std::vector<float> values, StringArray formatStrings, IntArray flags) {
        return AddOptions(self, texts.size(), [&](std::size_t i) {
            std::string format = TakeOr(formatStrings, i, "{0}");
            if (format.empty()) {
                format = "{0}";
            }
            return MakeOption(kSlider, std::move(texts[i]), std::move(format), true, GetOr(values, i, 0.0f), GetOr(flags, i, 0));
        });
    }

    IdArray AddMenuOptions(RE::TESQuest* self, StringArray texts, StringArray values, IntArray flags) {
        return AddOptions(self, texts.size(), [&](std::size_t i) {
            return MakeOption(kMenu, std::move(texts[i]), TakeOr(values, i), true, 0.0f, GetOr(flags, i, 0));
        });
    }

    IdArray AddColorOptions(RE::TESQuest* self, StringArray texts, IntArray colors, IntArray flags) {
        return AddOptions(self, texts.size(), [&](std::size_t i) {
            return MakeOption(kColor, std::move(texts[i]), "", false, static_cast<float>(GetOr(colors, i, 0)), GetOr(flags, i, 0));
        });
    }

    IdArray AddKeyMapOptions(RE::TESQuest* self, StringArray texts, IntArray keyCodes, IntArray flags) {
        return AddOptions(self, texts.size(), [&](std::size_t i) {
            return MakeOption(kKeymap, std::move(texts[i]), "", false, static_cast<float>(GetOr(keyCodes, i, -1)), GetOr(flags, i, 0));
        });
    }

    IdArray AddInputOptions(RE::TESQuest* self, StringArray texts, StringArray values, IntArray flags) {
        return AddOptions(self, texts.size(), [&](std::size_t i) {
            return MakeOption(kInput, std::move(texts[i]), TakeOr(values, i), true, 0.0f, GetOr(flags, i, 0));
        });
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Mixed types, values given as strings and converted by type

    bool IsTrueString(const std::string& s) {
        return _stricmp(s.c_str(), "true") == 0 || (!s.empty() && std::strtod(s.c_str(), nullptr) != 0.0);
    }

    // Parses "123", "-1.5", "0xFF0000" (hex for colors). Returns 0 for anything that isn't a number.
    float ParseNumber(const std::string& s) {
        if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
            return static_cast<float>(std::strtoul(s.c_str(), nullptr, 16));
        }
        return std::strtof(s.c_str(), nullptr);
    }

    PendingOption MakeOptionFromString(int type, std::string text, std::string value, int flags) {
        if (_stricmp(value.c_str(), "none") == 0) {
            value.clear();
        }

        switch (type) {
        case kHeader:
            return MakeOption(kHeader, std::move(text), "", false, 0.0f, flags);

        case kText:
        case kMenu:
        case kInput:
            return MakeOption(type, std::move(text), std::move(value), true, 0.0f, flags);

        case kToggle:
            return MakeOption(kToggle, std::move(text), "", false, IsTrueString(value) ? 1.0f : 0.0f, flags);

        case kSlider: {
            // number, optionally followed by a format string: "0.5|{1}", "7|{0} arrows". Default format "{0}".
            auto bar = value.find('|');
            float number = ParseNumber(value.substr(0, bar));
            std::string format = bar == std::string::npos ? std::string("{0}") : value.substr(bar + 1);
            return MakeOption(kSlider, std::move(text), std::move(format), true, number, flags);
        }

        case kColor:
        case kKeymap:
            return MakeOption(type, std::move(text), "", false, ParseNumber(value), flags);

        default:
            return MakeOption(kEmpty, "", "", false, 0.0f, 0);
        }
    }

    // int[] Function AddInjectedOptions(string[] texts, string[] values = None, int[] types = None, int[] flags = None) Native
    IdArray AddInjectedOptions(RE::TESQuest* self, StringArray texts, StringArray values, IntArray types, IntArray flags) {
        return AddOptions(self, texts.size(), [&](std::size_t i) {
            int type = GetOr(types, i, static_cast<std::int32_t>(kText));
            if (type < kEmpty || type > kInput) {
                type = kText;
            }
            return MakeOptionFromString(type, std::move(texts[i]), TakeOr(values, i), GetOr(flags, i, 0));
        });
    }

	std::string GetMcmInjectorVersion(RE::StaticFunctionTag*) {
		auto* declaration = SKSE::PluginDeclaration::GetSingleton();
		if (!declaration) {
			return "";
		}
		auto version = declaration->GetVersion();
		return std::format("{}-{}-{}", version.major(), version.minor(), version.patch());
	}

    // ---------------------------------------------------------------------------------------------------------------

    bool BindPapyrusFunctions(RE::BSScript::IVirtualMachine* vm) {
        // true = callable from tasklets: the call runs immediately instead of waiting for the next frame,
        // so adding options one at a time in a loop stays fast. These only touch the calling script's own
        // variables and the mutex-guarded pending list, so that's safe. 

        vm->RegisterFunction("GetMcmInjectorVersion", "McmInjector", GetMcmInjectorVersion, true);
        
		vm->RegisterFunction("SetCursorPosition", "McmInjector", SetCursorPosition, true);
        vm->RegisterFunction("GetCursorPosition", "McmInjector", GetCursorPosition, true);

        vm->RegisterFunction("AddEmptyOption", "McmInjector", AddEmptyOption, true);
        vm->RegisterFunction("AddHeaderOption", "McmInjector", AddHeaderOption, true);
        vm->RegisterFunction("AddTextOption", "McmInjector", AddTextOption, true);
        vm->RegisterFunction("AddToggleOption", "McmInjector", AddToggleOption, true);
        vm->RegisterFunction("AddSliderOption", "McmInjector", AddSliderOption, true);
        vm->RegisterFunction("AddMenuOption", "McmInjector", AddMenuOption, true);
        vm->RegisterFunction("AddColorOption", "McmInjector", AddColorOption, true);
        vm->RegisterFunction("AddKeyMapOption", "McmInjector", AddKeyMapOption, true);
        vm->RegisterFunction("AddInputOption", "McmInjector", AddInputOption, true);

        vm->RegisterFunction("AddTextOptionST", "McmInjector", AddTextOptionST, true);
        vm->RegisterFunction("AddToggleOptionST", "McmInjector", AddToggleOptionST, true);
        vm->RegisterFunction("AddSliderOptionST", "McmInjector", AddSliderOptionST, true);
        vm->RegisterFunction("AddMenuOptionST", "McmInjector", AddMenuOptionST, true);
        vm->RegisterFunction("AddColorOptionST", "McmInjector", AddColorOptionST, true);
        vm->RegisterFunction("AddKeyMapOptionST", "McmInjector", AddKeyMapOptionST, true);
        vm->RegisterFunction("AddInputOptionST", "McmInjector", AddInputOptionST, true);
        vm->RegisterFunction("McmiFindStateOptionIndex", "McmInjector", McmiFindStateOptionIndex, true);

        vm->RegisterFunction("AddEmptyOptions", "McmInjector", AddEmptyOptions, true);
        vm->RegisterFunction("AddHeaderOptions", "McmInjector", AddHeaderOptions, true);
        vm->RegisterFunction("AddTextOptions", "McmInjector", AddTextOptions, true);
        vm->RegisterFunction("AddToggleOptions", "McmInjector", AddToggleOptions, true);
        vm->RegisterFunction("AddSliderOptions", "McmInjector", AddSliderOptions, true);
        vm->RegisterFunction("AddMenuOptions", "McmInjector", AddMenuOptions, true);
        vm->RegisterFunction("AddColorOptions", "McmInjector", AddColorOptions, true);
        vm->RegisterFunction("AddKeyMapOptions", "McmInjector", AddKeyMapOptions, true);
        vm->RegisterFunction("AddInputOptions", "McmInjector", AddInputOptions, true);

        vm->RegisterFunction("AddInjectedOptions", "McmInjector", AddInjectedOptions, true);
        return true;
    }
}
