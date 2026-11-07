Scriptname McmInjector extends SKI_ConfigBase
{This script adds and overrides functions for SkyUi's MCM to allow adding more than 128 options per page, uncapped / unlimited 
and to add batches of options quickly with array function arguments.

Have your script extend McmInjector instead of SKI_ConfigBase like you normally would when making an MCM script. 
McmInjector extends SKI_ConfigBase so you can use all of the normal MCM functions and events, plus the new ones added by this script.

Note that you can still only add a max of 127 state options.
This is because a papyrus script can only have a max of 128 states. 127 Named states plust the default empty "" state. 

- SetCursorPosition and the Add Option functions work like SkyUI's, without the 128 cap.
- The Add Options array versions add one option per text at the cursor, following the current SetCursorFillMode setting, and return the option IDs.
- SetOptionValue and SetOptionFlags work for every option, at any index.
- Options are only added inside OnPageReset, like SkyUI's.

The page has 2 columns. List index 0 is top left, 1 top right, 2 second row left, and so on.}

; Names are prefixed with MCMI_ / Injected so they don't collide with SKI_ConfigBase's private
; functions and properties, which aren't in the SDK header but do exist at runtime.

; Get current McmInjector.dll version. Major-Minor-Patch, E.G "1-0-0"
String Function GetMcmInjectorVersion() Native Global

string property MCMI_MENU = "Journal Menu" autoReadonly
string property MCMI_ROOT = "_root.ConfigPanelFader.configPanel" autoReadonly

; Option types for AddInjectedOptions (same values as SKI_ConfigBase's private OPTION_TYPE_* properties)
int property MCMI_TYPE_EMPTY  = 0x00 autoReadonly
int property MCMI_TYPE_HEADER = 0x01 autoReadonly
int property MCMI_TYPE_TEXT   = 0x02 autoReadonly
int property MCMI_TYPE_TOGGLE = 0x03 autoReadonly
int property MCMI_TYPE_SLIDER = 0x04 autoReadonly
int property MCMI_TYPE_MENU   = 0x05 autoReadonly
int property MCMI_TYPE_COLOR  = 0x06 autoReadonly
int property MCMI_TYPE_KEYMAP = 0x07 autoReadonly
int property MCMI_TYPE_INPUT  = 0x08 autoReadonly 

; CURSOR ------------------------------------------------------------------------------------------
; SetCursorFillMode isn't capped, so SkyUI's own version is used.

function SetCursorPosition(int a_position) Native

; Returns the current cursor position: the list index the next Add*Option call will use.
; Returns -1 if the config script wasn't found.
int function GetCursorPosition() Native

; ADD (uncapped overrides) ------------------------------------------------------------------------

int function AddEmptyOption() Native
int function AddHeaderOption(string a_text, int a_flags = 0) Native
int function AddTextOption(string a_text, string a_value, int a_flags = 0) Native
int function AddToggleOption(string a_text, bool a_checked, int a_flags = 0) Native
int function AddSliderOption(string a_text, float a_value, string a_formatString = "{0}", int a_flags = 0) Native
int function AddMenuOption(string a_text, string a_value, int a_flags = 0) Native
int function AddColorOption(string a_text, int a_color, int a_flags = 0) Native
int function AddKeyMapOption(string a_text, int a_keyCode, int a_flags = 0) Native
int function AddInputOption(string a_text, string a_value, int a_flags = 0) Native

; ADD STATE OPTIONS (uncapped overrides) ---------------------------------------------------------

function AddTextOptionST(string a_stateName, string a_text, string a_value, int a_flags = 0) Native
function AddToggleOptionST(string a_stateName, string a_text, bool a_checked, int a_flags = 0) Native
function AddSliderOptionST(string a_stateName, string a_text, float a_value, string a_formatString = "{0}", int a_flags = 0) Native
function AddMenuOptionST(string a_stateName, string a_text, string a_value, int a_flags = 0) Native
function AddColorOptionST(string a_stateName, string a_text, int a_color, int a_flags = 0) Native
function AddKeyMapOptionST(string a_stateName, string a_text, int a_keyCode, int a_flags = 0) Native
function AddInputOptionST(string a_stateName, string a_text, string a_value, int a_flags = 0) Native

; ADD ARRAYS --------------------------------------------------------------------------------------
; One option per text, added at the cursor in the current fill mode, like calling the single version in a loop.
; Returns the option IDs in the same order. Missing values use the single version's default; flags default to 0.

int[] function AddEmptyOptions(int amount) Native
int[] function AddHeaderOptions(string[] a_texts, int[] a_flags = None) Native
int[] function AddTextOptions(string[] a_texts, string[] a_values, int[] a_flags = None) Native
int[] function AddToggleOptions(string[] a_texts, bool[] a_toggles, int[] a_flags = None) Native
int[] function AddSliderOptions(string[] a_texts, float[] a_values, string[] a_formatStrings = None, int[] a_flags = None) Native ; format defaults to "{0}"
int[] function AddMenuOptions(string[] a_texts, string[] a_values, int[] a_flags = None) Native
int[] function AddColorOptions(string[] a_texts, int[] a_colors, int[] a_flags = None) Native
int[] function AddKeyMapOptions(string[] a_texts, int[] a_keyCodes, int[] a_flags = None) Native ; key code defaults to -1 (unmapped)
int[] function AddInputOptions(string[] a_texts, string[] a_values, int[] a_flags = None) Native

; Mixed option types in one call. types: MCMI_TYPE_* values, text if None. flags: OPTION_FLAG_* values, 0 if None.
; values are converted by type:
;   header: ignored.  text, menu, input: the string as is.  toggle: "true" / "false" (or "1" / "0").
;   slider: the number, optionally followed by a format string: "0.5|{1}", "7|{0} arrows". The default format is "{0}".
;   color: a number, decimal or hex ("0xFF0000").  keymap: the DX key code.
int[] Function AddInjectedOptions(string[] texts, string[] values = None, int[] types = None, int[] flags = None) Native

; IDS ---------------------------------------------------------------------------------------------

; Same value as SKI_ConfigBase's private _currentPageNum: 0 for the start page, 1 for Pages[0], 2 for Pages[1], and so on.
int Function GetInjectedPageNum()
	return Pages.Find(CurrentPage) + 1
EndFunction

; Option ID (the a_option you get in events) -> list index. Works for any index, including 256+.
int Function GetInjectedIndex(int a_option)
	return a_option - GetInjectedPageNum() * 0x100
EndFunction

; List index -> option ID
int Function GetInjectedOptionId(int a_index)
	return a_index + GetInjectedPageNum() * 0x100
EndFunction

; List index of a state option on the current page, or -1. "" = the current state, like SkyUI's ST functions.
int Function GetInjectedStateIndex(string a_stateName = "")
	if a_stateName == ""
		a_stateName = GetState()
	endIf
	if a_stateName == ""
		return -1
	endIf
	return McmiFindStateOptionIndex(a_stateName)
EndFunction

int Function McmiFindStateOptionIndex(string a_stateName) Native

; SET (overrides) ---------------------------------------------------------------------------------
; Below index 128, SkyUI's own versions run. From 128 up, the option is changed directly in the UI, the same way
; SkyUI does internally. Like SkyUI's, don't call these inside OnPageReset.

function SetOptionFlags(int a_option, int a_flags, bool a_noUpdate = false)
	if GetInjectedIndex(a_option) < 128
		parent.SetOptionFlags(a_option, a_flags, a_noUpdate)
		return
	endIf
	int[] params = new int[2]
	params[0] = GetInjectedIndex(a_option)
	params[1] = a_flags
	UI.InvokeIntA(MCMI_MENU, MCMI_ROOT + ".setOptionFlags", params)
	if !a_noUpdate
		UI.Invoke(MCMI_MENU, MCMI_ROOT + ".invalidateOptionData")
	endIf
endFunction

function SetTextOptionValue(int a_option, string a_value, bool a_noUpdate = false)
	if GetInjectedIndex(a_option) < 128
		parent.SetTextOptionValue(a_option, a_value, a_noUpdate)
	else
		SetInjectedOptionStrValue(a_option, a_value, a_noUpdate)
	endIf
endFunction

function SetToggleOptionValue(int a_option, bool a_checked, bool a_noUpdate = false)
	if GetInjectedIndex(a_option) < 128
		parent.SetToggleOptionValue(a_option, a_checked, a_noUpdate)
	else
		SetInjectedOptionNumValue(a_option, a_checked as float, a_noUpdate)
	endIf
endFunction

function SetSliderOptionValue(int a_option, float a_value, string a_formatString = "{0}", bool a_noUpdate = false)
	if GetInjectedIndex(a_option) < 128
		parent.SetSliderOptionValue(a_option, a_value, a_formatString, a_noUpdate)
	else
		SetInjectedOptionValues(a_option, a_formatString, a_value, a_noUpdate)
	endIf
endFunction

function SetMenuOptionValue(int a_option, string a_value, bool a_noUpdate = false)
	if GetInjectedIndex(a_option) < 128
		parent.SetMenuOptionValue(a_option, a_value, a_noUpdate)
	else
		SetInjectedOptionStrValue(a_option, a_value, a_noUpdate)
	endIf
endFunction

function SetColorOptionValue(int a_option, int a_color, bool a_noUpdate = false)
	if GetInjectedIndex(a_option) < 128
		parent.SetColorOptionValue(a_option, a_color, a_noUpdate)
	else
		SetInjectedOptionNumValue(a_option, a_color as float, a_noUpdate)
	endIf
endFunction

function SetKeyMapOptionValue(int a_option, int a_keyCode, bool a_noUpdate = false)
	if GetInjectedIndex(a_option) < 128
		parent.SetKeyMapOptionValue(a_option, a_keyCode, a_noUpdate)
	else
		SetInjectedOptionNumValue(a_option, a_keyCode as float, a_noUpdate)
	endIf
endFunction

function SetInputOptionValue(int a_option, string a_value, bool a_noUpdate = false)
	if GetInjectedIndex(a_option) < 128
		parent.SetInputOptionValue(a_option, a_value, a_noUpdate)
	else
		SetInjectedOptionStrValue(a_option, a_value, a_noUpdate)
	endIf
endFunction

; SET STATE OPTIONS (overrides) -------------------------------------------------------------------
; SkyUI's versions pass the raw list index to Set*OptionValue, which only works below 128 (and on the start page).
; These pass the option ID instead, so they work at any index.

; Option ID of a state option, or -1 (logged like SkyUI's error).
int Function McmiGetStateOptionId(string a_stateName, string a_functionName)
	int index = GetInjectedStateIndex(a_stateName)
	if index < 0
		Debug.Trace(self + " ERROR: Cannot use " + a_functionName + " outside a valid option state")
		return -1
	endIf
	return GetInjectedOptionId(index)
EndFunction

function SetOptionFlagsST(int a_flags, bool a_noUpdate = false, string a_stateName = "")
	int id = McmiGetStateOptionId(a_stateName, "SetOptionFlagsST")
	if id >= 0
		SetOptionFlags(id, a_flags, a_noUpdate)
	endIf
endFunction

function SetTextOptionValueST(string a_value, bool a_noUpdate = false, string a_stateName = "")
	int id = McmiGetStateOptionId(a_stateName, "SetTextOptionValueST")
	if id >= 0
		SetTextOptionValue(id, a_value, a_noUpdate)
	endIf
endFunction

function SetToggleOptionValueST(bool a_checked, bool a_noUpdate = false, string a_stateName = "")
	int id = McmiGetStateOptionId(a_stateName, "SetToggleOptionValueST")
	if id >= 0
		SetToggleOptionValue(id, a_checked, a_noUpdate)
	endIf
endFunction

function SetSliderOptionValueST(float a_value, string a_formatString = "{0}", bool a_noUpdate = false, string a_stateName = "")
	int id = McmiGetStateOptionId(a_stateName, "SetSliderOptionValueST")
	if id >= 0
		SetSliderOptionValue(id, a_value, a_formatString, a_noUpdate)
	endIf
endFunction

function SetMenuOptionValueST(string a_value, bool a_noUpdate = false, string a_stateName = "")
	int id = McmiGetStateOptionId(a_stateName, "SetMenuOptionValueST")
	if id >= 0
		SetMenuOptionValue(id, a_value, a_noUpdate)
	endIf
endFunction

function SetColorOptionValueST(int a_color, bool a_noUpdate = false, string a_stateName = "")
	int id = McmiGetStateOptionId(a_stateName, "SetColorOptionValueST")
	if id >= 0
		SetColorOptionValue(id, a_color, a_noUpdate)
	endIf
endFunction

function SetKeyMapOptionValueST(int a_keyCode, bool a_noUpdate = false, string a_stateName = "")
	int id = McmiGetStateOptionId(a_stateName, "SetKeyMapOptionValueST")
	if id >= 0
		SetKeyMapOptionValue(id, a_keyCode, a_noUpdate)
	endIf
endFunction

function SetInputOptionValueST(string a_value, bool a_noUpdate = false, string a_stateName = "")
	int id = McmiGetStateOptionId(a_stateName, "SetInputOptionValueST")
	if id >= 0
		SetInputOptionValue(id, a_value, a_noUpdate)
	endIf
endFunction

; UI helpers --------------------------------------------------------------------------------------
; Change an option's values directly in the UI, at any index.

Function SetInjectedOptionStrValue(int a_option, string a_value, bool a_noUpdate = false)
	UI.SetInt(MCMI_MENU, MCMI_ROOT + ".optionCursorIndex", GetInjectedIndex(a_option))
	UI.SetString(MCMI_MENU, MCMI_ROOT + ".optionCursor.strValue", a_value)
	if !a_noUpdate
		UI.Invoke(MCMI_MENU, MCMI_ROOT + ".invalidateOptionData")
	endif
EndFunction

Function SetInjectedOptionNumValue(int a_option, float a_value, bool a_noUpdate = false)
	UI.SetInt(MCMI_MENU, MCMI_ROOT + ".optionCursorIndex", GetInjectedIndex(a_option))
	UI.SetFloat(MCMI_MENU, MCMI_ROOT + ".optionCursor.numValue", a_value)
	if !a_noUpdate
		UI.Invoke(MCMI_MENU, MCMI_ROOT + ".invalidateOptionData")
	endif
EndFunction

Function SetInjectedOptionValues(int a_option, string a_strValue, float a_numValue, bool a_noUpdate = false)
	UI.SetInt(MCMI_MENU, MCMI_ROOT + ".optionCursorIndex", GetInjectedIndex(a_option))
	UI.SetString(MCMI_MENU, MCMI_ROOT + ".optionCursor.strValue", a_strValue)
	UI.SetFloat(MCMI_MENU, MCMI_ROOT + ".optionCursor.numValue", a_numValue)
	if !a_noUpdate
		UI.Invoke(MCMI_MENU, MCMI_ROOT + ".invalidateOptionData")
	endif
EndFunction
