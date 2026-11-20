/* Resistance: Fall of Man PC port: the PC settings inside the game's own menus.
 *
 * 1. A "PC SETTINGS" entry in the pause menu's Options list (AUDIO / VIDEO,
 *    CONTROLS, PC SETTINGS, BACK). That list is data in a Lua 5.0 script,
 *    MainMenuUpdater (pause.ig), so the chunk is swapped (only the exact pause.ig
 *    version, FNV-1a checked) for kMmu below: the original updater, statement for
 *    statement, plus one menuData entry. It opens the Audio / Video screen straight
 *    into the PC category list; Circle / Esc there goes back to the Options list.
 *    The main menu's OPTIONS list is windowHandler (frontend.ig), swapped the same
 *    way (exact version only) for kWin. It has only 4 buttons (no room to scroll to
 *    a fifth entry), so kWin makes it AUDIO / VIDEO, CONTROLS, PC SETTINGS, EXTRAS,
 *    and EXTRAS opens a sub-list in the same buttons: MOVIES, CREDITS (Circle / Esc
 *    = back to OPTIONS). PC SETTINGS fires Audio / Video's own event after PC_OPEN:1.
 * 2. The PC pages themselves live in the Audio / Video options screen, another
 *    Anark scene driven by OptionHandler, swapped for kLua below the same way: the
 *    original handler plus a "PC mode" (the pause.ig and frontend.ig versions differ
 *    only in their On / Off text ids and Back, kLua does both: self.pcFE). PC mode
 *    first shows the categories (PC DISPLAY, PC GRAPHICS, PC EFFECTS, PC CONTROLS & HUD,
 *    PC AUDIO & LANGUAGE: the same names and order as the R2 / R3 menus), one per row,
 *    under the screen's own title retitled (PC SETTINGS, PC DISPLAY ...);
 *    everything is the screen's own text objects, font, highlight, sounds and Back: no overlay.
 *    The screen's texts are localized: a text with a localization_tag custom attribute draws the
 *    string table's text for the tag and ignores textString, unless its allow_text_on_element
 *    attribute is set (the game's text draw callback, EBOOT func_000EFEBC; the game's own
 *    LOC.SetText / SetLocalizedText(obj, "string") sets it). So every text the page rewrites
 *    (title, labels, the SUBTITLES / HELP Value texts) gets textString plus
 *    allow_text_on_element = 1 (set_customproperty), its tag untouched; leaving puts
 *    textString and allow_text_on_element back, so the tag draws the game's text again. (Before
 *    1116 only textString was written: the values in the untagged "0".."9" texts showed, the
 *    title, the labels and the On / Off texts kept the game's strings.) Lua numbers are floats
 *    here, so a tag cannot be read and written back exactly (and SetLocalizedText(obj, n) takes a
 *    script string index, not a tag): if the attribute cannot be set, a tagged text keeps the
 *    game's text (only the SUBTITLES / HELP Value texts, which setListItemValue re-tags on
 *    leaving, go through SetLocalizedText(obj, "string")); untagged texts draw textString anyway.
 *    The rows are found by the list group itself (optionsList:getChildByIndex), so the page opens
 *    in the slide's first onInitialize / onActivate whatever order the ScrollableList fills them.
 *    The open request (PC_OPEN: 1 = PC SETTINGS, 2 = plain AUDIO / VIDEO) is polled in onInitialize,
 *    onActivate and onUpdate: the scene's activation callback (AnarkLibraryExtensions,
 *    _useOnActivateDeactivate) can run after the slide's first onUpdate, so nothing may depend on
 *    their order; PC mode only ends on Back, a plain AUDIO / VIDEO request or a revisit after
 *    onDeactivate.
 *    X / Right opens one. A category shows its options four at a time in the screen's two
 *    columns: the short name in the Label text, the value in the row's own Value text (the
 *    SUBTITLES / HELP "OFF" text; the last of a volume row's "0".."9" texts, the other nine
 *    blanked; all restored on leaving). The categories read "OPEN" there. (Before 1115 the
 *    Value column was hidden and "NAME: VALUE" went into the label, so the page looked like an
 *    empty Audio / Video screen.) Only a row with no Value text falls back to "NAME: VALUE".
 *    Left / Right (or X) change the highlighted one, the fifth row pages (X / Right next,
 *    Left back; past either end = the categories) and Circle / Esc goes back to the
 *    categories; Circle / Esc there leaves PC mode.
 *    When the entry is not there (another script version, R1_MENU_ROWS=2) PC mode
 *    opens with X / Enter on one of the volume rows instead (where X does nothing in
 *    the original); leaving it goes back to the audio rows.
 *
 * The Lua talks to the port through ExposedDataManager, the engine's own
 * script data interface, hooked here (first lines of the func_ bodies, tools/hooks):
 *   GetKey("PC_CAT:<c>") / ("PC_ROW:<c>:<n>") / ("PC_RNAME:<c>:<n>") / ("PC_RVAL:<c>:<n>") / ("PC_NAV:<c>:<page>")
 *     / ("PC_STEP:<c>:<n>:<d>") / ("PC_TITLE:<c|0>")
 *                                                                   a text slot key (c = 1-based category)
 *   GetCharString(slot key).m_char_string                           the text
 *   GetNumber(GetKey("PC_CATS")) / (GetKey("PC_CPAGES:<c>"))        the category / page count
 *   GetNumber(GetKey("PC_ENTRY:<m>" / "PC_HERE:<m>" / "PC_OPEN"))   add the entry? / rows on
 *                                                                   the volume rows? / opened by it?
 *   GetKey("PC_ENTRY:ok:<m>" / "PC_ENTRY:fail:<m>:<why>" / "PC_OPEN:<1|0>" / "PC_SHOW:<0|1|2:c>")   reports
 *                                                                   (PC_OPEN: PC SETTINGS / plain AUDIO / VIDEO
 *                                                                   chosen; PC_SHOW: off / categories / a category)
 *   GetKey("PC_KEEP:<0|1>" / "PC_HOLD:<0|1>")                       main menu Back: this Back stays on the
 *                                                                   PC page / a Back now would (see kNav)
 *   GetNumber(GetKey("PC_KEEP"))                                    kNav: 1 = skip the main menu slide
 *   GetKey("PC_DIAG:<text>")                                        a log line (each action the page gets:
 *                                                                   move / select / left / right / back, with
 *                                                                   the port 0 buttons the game read, e.g.
 *                                                                   "pad CIRCLE"; what was painted), at most
 *                                                                   10 a second / 400 in all
 * <m> = the menu: 0 the pause menu (pause.ig), 1 the main menu (frontend.ig); each has its own
 * entry state, so one menu's entry never turns off the other's volume-row fallback.
 * Names without the PC_ prefix and every other key go to the game untouched.
 * The rows are the F10 PC Options (rsx_pc_menu.inc) then More PC Options
 * (rfom_pcextra.cpp), changed through the same code: settings.ini +
 * settings.bat + the live setter, "(restart)" when it applies on the next start.
 * Nothing is drawn over the game's menus (the old pause-menu hint box is gone); F10 stays the
 * separate quick PC menu (rsx_pc_menu.inc).
 *
 *   R1_MENU_ROWS=1 (default; or RFOM_MENU_ROWS)  Options > PC SETTINGS; falls back to 2 (logged)
 *   R1_MENU_ROWS=2                                 only X on a volume row in Audio / Video
 *   R1_MENU_ROWS=0                                 the original scripts, no PC rows
 *   R1_MENU_BACKNAV=1 (default; or RFOM_MENU_BACKNAV)  main menu: Back in a PC category goes to the
 *                                                  categories, Back there to the Options list (kNav swapped)
 *   R1_MENU_BACKNAV=0                              FrontEndMenuNav left alone: every Back on the main menu's
 *                                                  PC page also leaves for the main menu (the pre-1116 behaviour)
 * -------------------------------------------------------------------------*/
#ifndef RFOM_MENUROWS_TEST   /* project/tests/menurows_state_test.cpp brings its own stand-ins */
#include "ppu_recomp.h"
#endif
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <string>
#include <vector>
#include <chrono>
extern "C" uint8_t* vm_base;
extern "C" uint32_t ppu_hle_inject_base;
extern "C" int rfom_pm_rows(void);                                         /* rsx_pc_menu.inc */
extern "C" void rfom_pm_row(int i, char* title, int tcap, char* val, int vcap);
extern "C" void rfom_pm_change(int i, int d);
extern "C" int rfom_pcx_rows(void);                                        /* rfom_pcextra.cpp */
extern "C" void rfom_pcx_row(int i, char* title, int tcap, char* val, int vcap);
extern "C" void rfom_pcx_change(int i, int d);
extern "C" const char* rfom_pcx_key(int i);                                 /* its settings.ini key */
extern "C" volatile uint32_t g_rfom_pad0_btns;                              /* cellPad.c: port 0 buttons the game read last */

#define LOG(...) do { fprintf(stderr, "[menurows] " __VA_ARGS__); fputc('\n', stderr); } while (0)

static const char kLua[] = R"LUA(
self.onInitialize = function(self)
  self:_useOnActivateDeactivate()
  self.optionsList = self:getObjectReference("optionsList")
  self:SetupAVOptionsData()
  self:pcCheckOpen()
  if self.pcWant then self:pcTryOpen() end
end
self.onActivate = function(self)
  SetDelayAnarkRender(false)
  self:pcCheckOpen()
  if self.pcMode and self.pcAway then self:pcLeave() end
  self.pcAway = false
  if self.pcWant then self:pcTryOpen() end
end
self.onDeactivate = function(self)
  if self.pcMode then self.pcAway = true end
  if self.pcFE then ExposedDataManager:GetKey("PC_HOLD:0") end
end
self.onUpdate = function(self)
  self:pcCheckOpen()
  if self.pcMode then self:pcKeepVals() end
  if self.pcWant then
    self:pcTryOpen()
    if self.pcWant then
      self.pcWaited = self.pcWaited + 1
      if self.pcWaited > 30 then
        self.pcWant = false
        ExposedDataManager:GetKey("PC_ENTRY:fail:" .. self:pcMenu() .. ":the PC page did not open (no list rows)")
      end
    end
  end
end
self.getNumListItems = function(self)
  return self.optionsList:getNumChildren()
end
self.setListItemValue = function(self, itemIndex, itemObject)
  if self.pcItems then self.pcItems[itemIndex] = itemObject end
  if self.pcWant then self:pcTryOpen() end
  if self.pcMode then
    self:pcPaintRow(itemIndex)
    return
  end
  local currentSelection = self.avOptionsData[itemIndex]
  local dataKey = currentSelection[self.DATA_KEY_INDEX]
  if dataKey then
    if ExposedDataManager:GetNumber(dataKey) == 1 then
      SetLocalizedText(itemObject.Value, currentSelection[self.ON_INDEX])
    else
      SetLocalizedText(itemObject.Value, currentSelection[self.OFF_INDEX])
    end
  end
end
self.clearListItemValue = function(self, itemIndex, itemObject)
end
self.onSelectedItemChanged = function(self, itemIndex, itemObject, isReset)
  self.currentIndex = itemIndex
  self.currentObject = itemObject
  if self.pcItems then self.pcItems[itemIndex] = itemObject end
  if self.pcMode then self:pcDiag("move") end
  if isReset == false then
    AnarkEffectsConduitInterface:EffectEventTriggered(ANARK_EFFECT_EVENT_TYPE_LOST_FOCUS, ANARK_UPDATE_CLASS_HIGHLIGHT_BOX, self._EFFECTS_CONDUIT_TAG)
  end
end
self.handleSelect = function(self)
  if self.pcMode then
    self:pcDiag("select")
    if self.pcCat == 0 then
      self:pcOpenCat(self.currentIndex)
    elseif self.currentIndex == self.HELP_INDEX then
      self:pcTurn(1)
    else
      self:pcStep(self.currentIndex, 1)
    end
    AnarkEffectsConduitInterface:EffectEventTriggered(ANARK_EFFECT_EVENT_TYPE_SELECTED, ANARK_UPDATE_CLASS_HIGHLIGHT_BOX, self._EFFECTS_CONDUIT_TAG)
    return
  end
  local currentSelection = self.avOptionsData[self.currentIndex]
  local callback = currentSelection[self.SELECT_CALLBACK_INDEX]
  if callback then
    callback()
    local isItemOn = ExposedDataManager:GetNumber(currentSelection[self.DATA_KEY_INDEX])
    if self.currentObject.Value then
      if isItemOn == 1 then
        SetLocalizedText(self.currentObject.Value, currentSelection[self.ON_INDEX])
      else
        SetLocalizedText(self.currentObject.Value, currentSelection[self.OFF_INDEX])
      end
    end
    AnarkEffectsConduitInterface:EffectEventTriggered(ANARK_EFFECT_EVENT_TYPE_SELECTED, ANARK_UPDATE_CLASS_HIGHLIGHT_BOX, self._EFFECTS_CONDUIT_TAG)
  elseif self.currentIndex >= self.MUSIC_INDEX and self.currentIndex <= self.SPEECH_INDEX then
    if self:pcHere() and self:pcEnter(false) then
      AnarkEffectsConduitInterface:EffectEventTriggered(ANARK_EFFECT_EVENT_TYPE_SELECTED, ANARK_UPDATE_CLASS_HIGHLIGHT_BOX, self._EFFECTS_CONDUIT_TAG)
    end
  end
end
self.handleBack = function(self)
  if self.pcMode then
    self:pcDiag("back")
    if self.pcCat > 0 then
      self:pcToList(self.pcCat)
      self:pcKeep(1)
      return
    end
    local direct = self.pcDirect
    self:pcLeave()
    if direct then
      self:pcKeep(0)
      self:pcBack()
    else
      self:pcKeep(1)
    end
    return
  end
  self:pcKeep(0)
  self:pcBack()
end
self.pcKeep = function(self, v)
  if self.pcFE then ExposedDataManager:GetKey("PC_KEEP:" .. v) end
end
self.pcSyncHold = function(self)
  if not self.pcFE then return end
  local h = 0
  if self.pcMode and (self.pcCat > 0 or not self.pcDirect) then h = 1 end
  ExposedDataManager:GetKey("PC_HOLD:" .. h)
end
self.pcMenu = function(self)
  if self.pcFE then
    return 1
  end
  return 0
end
self.pcBack = function(self)
  if self.pcFE then
    CtlAnarkAVOptionsSelect:onMenuSaveConfig()
    AnarkEffectsConduitInterface:EffectEventTriggered(ANARK_EFFECT_EVENT_TYPE_EXITED, ANARK_UPDATE_CLASS_HIGHLIGHT_BOX, self._EFFECTS_CONDUIT_TAG)
  else
    scene:goToSlide(1)
  end
end
self.onLeft = function(self)
  if self.pcMode then
    self:pcDiag("left")
    if self.pcCat == 0 then return end
    if self.currentIndex == self.HELP_INDEX then self:pcTurn(-1) else self:pcStep(self.currentIndex, -1) end
    return
  end
  local currentSelection = self.avOptionsData[self.currentIndex]
  local callback = currentSelection[self.LEFT_CALLBACK_INDEX]
  if callback then
    callback()
  end
end
self.onRight = function(self)
  if self.pcMode then
    self:pcDiag("right")
    if self.pcCat == 0 then
      self:pcOpenCat(self.currentIndex)
    elseif self.currentIndex == self.HELP_INDEX then
      self:pcTurn(1)
    else
      self:pcStep(self.currentIndex, 1)
    end
    return
  end
  local currentSelection = self.avOptionsData[self.currentIndex]
  local callback = currentSelection[self.RIGHT_CALLBACK_INDEX]
  if callback then
    callback()
  end
end
self.SetupAVOptionsData = function(self)
  self.currentIndex = 1
  self.currentObject = nil
  self.SUBTITLES_INDEX = 1
  self.MUSIC_INDEX = 2
  self.EFFECTS_INDEX = 3
  self.SPEECH_INDEX = 4
  self.HELP_INDEX = 5
  self.SELECT_CALLBACK_INDEX = 1
  self.LEFT_CALLBACK_INDEX = 2
  self.RIGHT_CALLBACK_INDEX = 3
  self.ON_INDEX = 4
  self.OFF_INDEX = 5
  self.DATA_KEY_INDEX = 6
  local on = 79
  local off = 80
  if self.pcFE then
    on = 101
    off = 102
  end
  self.avOptionsData = {}
  self.avOptionsData[self.SUBTITLES_INDEX] = {}
  self.avOptionsData[self.SUBTITLES_INDEX][self.SELECT_CALLBACK_INDEX] = self.onMenuSubtitles
  self.avOptionsData[self.SUBTITLES_INDEX][self.ON_INDEX] = on
  self.avOptionsData[self.SUBTITLES_INDEX][self.OFF_INDEX] = off
  self.avOptionsData[self.SUBTITLES_INDEX][self.DATA_KEY_INDEX] = ExposedDataManager:GetKey("SubtitlesOn")
  self.avOptionsData[self.MUSIC_INDEX] = {}
  self.avOptionsData[self.MUSIC_INDEX][self.LEFT_CALLBACK_INDEX] = self.onMenuMusicVolumeDown
  self.avOptionsData[self.MUSIC_INDEX][self.RIGHT_CALLBACK_INDEX] = self.onMenuMusicVolumeUp
  self.avOptionsData[self.EFFECTS_INDEX] = {}
  self.avOptionsData[self.EFFECTS_INDEX][self.LEFT_CALLBACK_INDEX] = self.onMenuEffectsVolumeDown
  self.avOptionsData[self.EFFECTS_INDEX][self.RIGHT_CALLBACK_INDEX] = self.onMenuEffectsVolumeUp
  self.avOptionsData[self.SPEECH_INDEX] = {}
  self.avOptionsData[self.SPEECH_INDEX][self.LEFT_CALLBACK_INDEX] = self.onMenuSpeechVolumeDown
  self.avOptionsData[self.SPEECH_INDEX][self.RIGHT_CALLBACK_INDEX] = self.onMenuSpeechVolumeUp
  self.avOptionsData[self.HELP_INDEX] = {}
  self.avOptionsData[self.HELP_INDEX][self.SELECT_CALLBACK_INDEX] = self.onMenuHelp
  self.avOptionsData[self.HELP_INDEX][self.ON_INDEX] = on
  self.avOptionsData[self.HELP_INDEX][self.OFF_INDEX] = off
  self.avOptionsData[self.HELP_INDEX][self.DATA_KEY_INDEX] = ExposedDataManager:GetKey("HelpTextOn")
  self.pcItems = {}
  self.pcMode = false
  self.pcDirect = false
  self.pcWant = false
  self.pcAway = false
  self.pcWaited = 0
  self.pcTitle = nil
  self.pcTitleRec = nil
  self.pcHow = nil
  self.pcPage = 1
  self.pcPages = 0
  self.pcCat = 0
  self.pcCats = 0
  self.pcSaved = {}
  self.pcVals = {}
end
self.onMenuSubtitles = function(self)
  CtlAnarkAVOptionsSelect:onMenuSubtitles()
end
self.onMenuHelp = function(self)
  CtlAnarkAVOptionsSelect:onMenuHelpText()
end
self.onMenuMusicVolumeUp = function(self)
  CtlAnarkAVOptionsSelect:onMenuMusicVolume(1)
end
self.onMenuMusicVolumeDown = function(self)
  CtlAnarkAVOptionsSelect:onMenuMusicVolume(-1)
end
self.onMenuEffectsVolumeUp = function(self)
  CtlAnarkAVOptionsSelect:onMenuEffectsVolume(1)
end
self.onMenuEffectsVolumeDown = function(self)
  CtlAnarkAVOptionsSelect:onMenuEffectsVolume(-1)
end
self.onMenuSpeechVolumeUp = function(self)
  CtlAnarkAVOptionsSelect:onMenuSpeechVolume(1)
end
self.onMenuSpeechVolumeDown = function(self)
  CtlAnarkAVOptionsSelect:onMenuSpeechVolume(-1)
end
self.onMenuSaveConfig = function(self)
  CtlAnarkAVOptionsSelect:onMenuSaveConfig()
end
self.onMenuCancel = function(self)
  CtlAnarkAVOptionsSelect:onMenuCancel()
end
self.pcDiag = function(self, what)
  ExposedDataManager:GetKey("PC_DIAG:" .. what .. " (category " .. tostring(self.pcCat) .. ", row " .. tostring(self.currentIndex) .. ")")
end
self.pcSet = function(self, node, key, v)
  if pcall then
    pcall(function() node[key] = v end)
  else
    node[key] = v
  end
end
self.pcGet = function(self, node, key)
  local r = nil
  if pcall then
    pcall(function() r = node[key] end)
  else
    r = node[key]
  end
  return r
end
self.pcText = function(self, name)
  local s = ExposedDataManager:GetCharString(ExposedDataManager:GetKey(name))
  if s then return s.m_char_string end
  return nil
end
self.pcRow = function(self, i)
  local o = nil
  local l = self.optionsList
  if l then
    if pcall then
      pcall(function() o = l:getChildByIndex(i) end)
    else
      o = l:getChildByIndex(i)
    end
  end
  if not o and self.pcItems then o = self.pcItems[i] end
  if o and self.pcItems then self.pcItems[i] = o end
  return o
end
self.pcFind = function(self, node, name, depth)
  if not node or depth < 1 then return nil end
  if not self:pcGet(node, "getNumChildren") or not self:pcGet(node, "getChildByIndex") then return nil end
  local n = 0
  pcall(function() n = node:getNumChildren() end)
  if not n then return nil end
  local kids = {}
  for i = 1, n do
    local c = nil
    pcall(function() c = node:getChildByIndex(i) end)
    if c then
      if self:pcGet(c, "name") == name then return c end
      kids[i] = c
    end
  end
  for i = 1, n do
    if kids[i] and kids[i] ~= self.optionsList then
      local r = self:pcFind(kids[i], name, depth - 1)
      if r then return r end
    end
  end
  return nil
end
self.pcFindTitle = function(self)
  if not pcall or not self.optionsList then return nil end
  local p = self:pcGet(self.optionsList, "parent")
  for up = 1, 2 do
    if not p then return nil end
    local t = self:pcFind(p, "Title", 4 - up)
    if t then return t end
    p = self:pcGet(p, "parent")
  end
  return nil
end
self.pcTextRec = function(self, node)
  local r = { text = self:pcGet(node, "textString") }
  pcall(function() r.allow = node:get_customproperty("allow_text_on_element") end)
  local t = nil
  if pcall(function() t = node:get_customproperty("localization_tag") end) then
    r.tagged = t ~= nil and t ~= 0 and t ~= "0" and t ~= false
  else
    r.tagged = self:pcGet(node, "get_customproperty") ~= nil
  end
  return r
end
self.pcWrite = function(self, node, s, rec)
  self:pcSet(node, "textString", s)
  local ok = false
  pcall(function()
    node:set_customproperty("allow_text_on_element", 1)
    local a = node:get_customproperty("allow_text_on_element")
    ok = a == 1 or a == true
  end)
  if ok then
    self.pcHow = "allow_text_on_element"
    return
  end
  if rec and rec.tagged and not rec.retag then
    self.pcHow = "game text kept (tagged, allow_text_on_element not settable)"
    return
  end
  if SetLocalizedText and pcall(function() SetLocalizedText(node, s) end) then
    self.pcHow = "SetLocalizedText"
    if rec then rec.lost = true end
  else
    self.pcHow = "textString only"
  end
end
self.pcRestoreText = function(self, node, rec)
  if not node or not rec then return end
  if rec.lost then
    if rec.text and SetLocalizedText then pcall(function() SetLocalizedText(node, rec.text) end) end
  else
    local a = 0
    if rec.allow == 1 or rec.allow == true then a = 1 end
    pcall(function() node:set_customproperty("allow_text_on_element", a) end)
  end
  if rec.text then self:pcSet(node, "textString", rec.text) end
end
self.pcPaintTitle = function(self)
  if not self.pcTitle then return end
  local t = self:pcText("PC_TITLE:" .. self.pcCat)
  if t then self:pcWrite(self.pcTitle, t, self.pcTitleRec) end
end
self.pcIsText = function(self, node)
  if not node then return false end
  local s = self:pcGet(node, "textString")
  return s ~= nil and tostring(s) == s
end
self.pcValNode = function(self, i, o)
  local c = self.pcVals[i]
  if c ~= nil then return c end
  c = false
  local v = o.Value
  if v then
    if self:pcIsText(v) then
      c = { node = v, rec = self:pcTextRec(v), op = self:pcGet(v, "opacity"), others = {}, nothers = 0 }
    elseif self:pcGet(v, "getNumChildren") and self:pcGet(v, "getChildByIndex") then
      local n = 0
      pcall(function() n = v:getNumChildren() end)
      if not n then n = 0 end
      local texts = {}
      local nt = 0
      for j = 1, n do
        local k = nil
        pcall(function() k = v:getChildByIndex(j) end)
        if self:pcIsText(k) then
          nt = nt + 1
          texts[nt] = k
        end
      end
      if nt > 0 then
        local last = texts[nt]
        c = { node = last, rec = self:pcTextRec(last), op = self:pcGet(last, "opacity"), others = {}, nothers = nt - 1 }
        for j = 1, nt - 1 do
          c.others[j] = { node = texts[j], text = self:pcGet(texts[j], "textString") }
        end
      end
    end
  end
  if c and (i == self.SUBTITLES_INDEX or i == self.HELP_INDEX) then c.rec.retag = true end
  self.pcVals[i] = c
  return c
end
self.pcPaintRow = function(self, i)
  local o = self:pcRow(i)
  if not o then return end
  local vn = self:pcValNode(i, o)
  local name = nil
  local val = ""
  if self.pcCat == 0 then
    name = self:pcText("PC_CAT:" .. i)
    if name and name ~= "" then val = "OPEN" end
  elseif i == self.HELP_INDEX then
    name = self:pcText("PC_NAV:" .. self.pcCat .. ":" .. self.pcPage)
  else
    local e = (self.pcPage - 1) * 4 + i - 1
    if vn then
      name = self:pcText("PC_RNAME:" .. self.pcCat .. ":" .. e)
      val = self:pcText("PC_RVAL:" .. self.pcCat .. ":" .. e)
    else
      name = self:pcText("PC_ROW:" .. self.pcCat .. ":" .. e)
    end
  end
  if vn then
    if o.Value then self:pcSet(o.Value, "opacity", 100) end
    self:pcSet(vn.node, "opacity", 100)
    self:pcWrite(vn.node, val or "", vn.rec)
    for j = 1, vn.nothers do self:pcSet(vn.others[j].node, "textString", "") end
  elseif o.Value then
    self:pcSet(o.Value, "opacity", 0)
  end
  if o.Label and name then
    local sv = self.pcSaved[i]
    if not sv then
      sv = {}
      self.pcSaved[i] = sv
    end
    if not sv.label then sv.label = self:pcTextRec(o.Label) end
    self:pcWrite(o.Label, name, sv.label)
  end
end
self.pcKeepVals = function(self)
  for i = 1, 5 do
    local c = self.pcVals[i]
    if c and self:pcGet(c.node, "opacity") ~= 100 then self:pcSet(c.node, "opacity", 100) end
  end
end
self.pcRestoreVals = function(self)
  for i = 1, 5 do
    local c = self.pcVals[i]
    if c then
      self:pcRestoreText(c.node, c.rec)
      if c.op then self:pcSet(c.node, "opacity", c.op) end
      for j = 1, c.nothers do
        local k = c.others[j]
        if k.text then self:pcSet(k.node, "textString", k.text) end
      end
    end
  end
  self.pcVals = {}
end
self.pcPaintAll = function(self)
  self:pcPaintTitle()
  for i = 1, 5 do
    self:pcPaintRow(i)
  end
end
self.pcHere = function(self)
  return ExposedDataManager:GetNumber(ExposedDataManager:GetKey("PC_HERE:" .. self:pcMenu())) == 1
end
self.pcCheckOpen = function(self)
  local v = ExposedDataManager:GetNumber(ExposedDataManager:GetKey("PC_OPEN"))
  if v == 1 then
    self.pcAway = false
    if self.pcMode then
      self.pcDirect = true
      self:pcToList()
    else
      self.pcWant = true
      self.pcWaited = 0
    end
    return true
  end
  if v == 2 then
    self.pcWant = false
    if self.pcMode then self:pcLeave() end
  end
  return false
end
self.pcTryOpen = function(self)
  for i = 1, 5 do
    if not self:pcRow(i) then return end
  end
  self.pcWant = false
  if self.pcMode then return end
  if not self:pcEnter(true) then
    ExposedDataManager:GetKey("PC_ENTRY:fail:" .. self:pcMenu() .. ":the PC pages did not open in Audio / Video")
  end
end
self.pcEnter = function(self, direct)
  local cats = ExposedDataManager:GetNumber(ExposedDataManager:GetKey("PC_CATS"))
  if not cats or cats < 1 then return false end
  if self:getNumListItems() < 5 then return false end
  self.pcCats = cats
  self.pcSaved = {}
  for i = 1, 5 do
    local o = self:pcRow(i)
    if o then
      local sv = {}
      if o.Label then sv.label = self:pcTextRec(o.Label) end
      if o.Value then sv.op = self:pcGet(o.Value, "opacity") end
      self.pcSaved[i] = sv
    end
  end
  self.pcTitle = self:pcFindTitle()
  self.pcTitleRec = nil
  if self.pcTitle then self.pcTitleRec = self:pcTextRec(self.pcTitle) end
  self.pcVals = {}
  self.pcMode = true
  self.pcDirect = direct
  self.pcCat = 0
  self.pcPage = 1
  self:pcPaintAll()
  ExposedDataManager:GetKey("PC_SHOW:1")
  self:pcSyncHold()
  self:pcDiagPaint("painted")
  return true
end
self.pcDiagPaint = function(self, what)
  local found = 0
  local vals = 0
  for i = 1, 5 do
    if self:pcRow(i) then found = found + 1 end
    if self.pcVals[i] then vals = vals + 1 end
  end
  local o = self:pcRow(1)
  local row1 = nil
  if o and o.Label then row1 = self:pcGet(o.Label, "textString") end
  local v1 = nil
  if self.pcVals[1] then v1 = self:pcGet(self.pcVals[1].node, "textString") end
  local tt = nil
  if self.pcTitle then tt = self:pcGet(self.pcTitle, "textString") end
  self:pcDiag(what .. ": " .. found .. " rows, " .. vals .. " value columns, title " .. tostring(tt) .. ", first row " .. tostring(row1) .. " = " .. tostring(v1) .. ", drawn by " .. tostring(self.pcHow))
end
self.pcOpenCat = function(self, c)
  if not c or c > self.pcCats then
    self:pcDiag("no category here")
    return
  end
  local n = ExposedDataManager:GetNumber(ExposedDataManager:GetKey("PC_CPAGES:" .. c))
  if not n or n < 1 then
    self:pcDiag("category " .. c .. " has no pages")
    return
  end
  self.pcCat = c
  self.pcPages = n
  self.pcPage = 1
  self:pcPaintAll()
  ExposedDataManager:GetKey("PC_SHOW:2:" .. c)
  self:pcSyncHold()
  self:pcDiagPaint("category " .. c)
end
self.pcListBeh = function(self)
  if self.pcList then return self.pcList end
  local ps = { self:pcGet(self, "parent"), self:pcGet(self.optionsList, "parent") }
  for k = 1, 2 do
    local p = ps[k]
    if p then
      local c = self:pcGet(p, "AvOptionsScrollableList")
      if c and self:pcGet(c, "setSelectedIndex") then
        self.pcList = c
        return c
      end
      local n = 0
      pcall(function() n = p:getNumChildren() end)
      if not n then n = 0 end
      for i = 1, n do
        c = nil
        pcall(function() c = p:getChildByIndex(i) end)
        if c and self:pcGet(c, "setSelectedIndex") and (self:pcGet(c, "listUpdater") == self or self:pcGet(c, "itemsGroup") == self.optionsList) then
          self.pcList = c
          return c
        end
      end
    end
  end
  return nil
end
self.pcSelect = function(self, row)
  local lb = self:pcListBeh()
  local ok = false
  if lb then ok = pcall(function() lb:setSelectedIndex(row, true) end) end
  if ok then
    local o = self:pcRow(row)
    self.currentIndex = row
    if o then self.currentObject = o end
  else
    self:pcDiag("row " .. row .. " not reselected (no list behavior)")
  end
end
self.pcToList = function(self, row)
  self.pcCat = 0
  self.pcPage = 1
  if row then self:pcSelect(row) end
  self:pcPaintAll()
  ExposedDataManager:GetKey("PC_SHOW:1")
  self:pcSyncHold()
end
self.pcLeave = function(self)
  self.pcMode = false
  self.pcDirect = false
  self.pcCat = 0
  ExposedDataManager:GetKey("PC_SHOW:0")
  self:pcSyncHold()
  self:pcRestoreText(self.pcTitle, self.pcTitleRec)
  self.pcTitle = nil
  self.pcTitleRec = nil
  self:pcRestoreVals()
  for i = 1, 5 do
    local o = self:pcRow(i)
    if o then
      local sv = self.pcSaved[i]
      local op = 100
      if sv and sv.op then op = sv.op end
      if o.Label and sv and sv.label then self:pcRestoreText(o.Label, sv.label) end
      if o.Value then self:pcSet(o.Value, "opacity", op) end
      if i == self.SUBTITLES_INDEX or i == self.HELP_INDEX then self:setListItemValue(i, o) end
    end
  end
end
self.pcStep = function(self, i, d)
  self:pcText("PC_STEP:" .. self.pcCat .. ":" .. ((self.pcPage - 1) * 4 + i - 1) .. ":" .. d)
  self:pcPaintRow(i)
end
self.pcTurn = function(self, d)
  local p = self.pcPage + d
  if p < 1 or p > self.pcPages then
    self:pcToList(self.pcCat)
    return
  end
  self.pcPage = p
  self:pcPaintAll()
end
)LUA";

/* MainMenuUpdater (pause.ig chunk, 10866 bytes, FNV-1a 0x29cc95a0): the pause menu's button list
 * (Options / Intel / Extras / Quit, then Audio / Video, Controls, Back ...), written back from the
 * bytecode function for function, plus pcAddEntry / onMenuPcSettings and the PC_TEXT case in SetText. */
static const char kMmu[] = R"LUA(
self.onInitialize = function(self)
  self.menuItems = self:getObjectReference("menuItems")
  self.listBehavior = self:getObjectReference("listBehavior")
  self.quitControl = self:getObjectReference("quitControl")
  self:ClearAllButtons()
  self.setColor = {}
  self.setColor[0] = 200
  self.setColor[1] = 200
  self.setColor[2] = 200
  self.clearColor = {}
  self.clearColor[0] = 175
  self.clearColor[1] = 175
  self.clearColor[2] = 175
  self:SetupMenus()
  local startButton = self.menuItems:getChildByIndex(1)
  self:SetButtonColor(startButton)
  self.isActive = true
  self:_useOnActivateDeactivate()
end
self.onUpdate = function(self)
  self.isActive = self.listBehavior.hasFocus
end
self.onActivate = function(self)
  local wentToIntelMenu = CtlAnarkPauseMenuSelect:wentToIntelMenu()
  if wentToIntelMenu == 1 then
    ak.output("went to intel menu is set\n")
    self.currentMainMenuIndex = 2
    self.currentIndex = 2
    self:onMenuBack()
    CtlAnarkPauseMenuSelect:clearWentToIntelMenu()
  else
    ak.output("went to intel menu is not set: " .. wentToIntelMenu .. "\n")
  end
end
self.handleSelect = function(self)
  if self.isActive == false then
    return
  end
  local currentMenu = self.menuData[self.currentMenuIndex]
  AnarkEffectsConduitInterface:EffectEventTriggered(ANARK_EFFECT_EVENT_TYPE_SELECTED, ANARK_UPDATE_CLASS_BUTTON, self._EFFECTS_CONDUIT_TAG)
  if currentMenu then
    local currentMenuItem = currentMenu[self.currentIndex]
    if currentMenuItem then
      local callbackFunction = currentMenuItem[2]
      if callbackFunction then
        if self.currentMenuIndex == self.MAIN_MENU_INDEX then
          self.currentMainMenuIndex = self.currentIndex
        end
        callbackFunction(self)
      else
        ak.output("Bogus callback function for: " .. self.currentIndex .. " for menu " .. self.currentMenuIndex .. "\n")
      end
    else
      ak.output("Bogus item: " .. self.currentIndex .. " for menu " .. self.currentMenuIndex .. "\n")
    end
  else
    ak.output("Bogus menu index: " .. self.currentMenuIndex .. "\n")
  end
end
self.handleBack = function(self)
  if self.isActive == false then
    return
  end
  if self.currentMenuIndex == self.MAIN_MENU_INDEX then
    self:onMenuContinue()
  else
    self:onMenuBack()
  end
end
self.setListItemValue = function(self, itemIndex, itemObject)
  self:ActivateItem(itemIndex, itemObject)
end
self.clearListItemValue = function(self, itemIndex, itemObject)
  self:DeactivateItem(itemObject)
end
self.getNumListItems = function(self)
  local currentMenu = self.menuData[self.currentMenuIndex]
  if currentMenu then
    local numItems = table.getn(currentMenu)
    return numItems
  else
    return 0
  end
end
self.isSelectable = function(self, selectionIndex)
  local currentMenu = self.menuData[self.currentMenuIndex]
  if currentMenu then
    if currentMenu[selectionIndex] then
      return true
    else
      return false
    end
  else
    ak.output("Bad current menu index: " .. self.currentMenuIndex .. "\n")
    return false
  end
  return false
end
self.onSelectedItemChanged = function(self, itemIndex, itemObject, isReset)
  self:ClearAllButtons()
  self:SetButtonColor(itemObject)
  self.currentIndex = itemIndex
  if isReset == false then
    AnarkEffectsConduitInterface:EffectEventTriggered(ANARK_EFFECT_EVENT_TYPE_LOST_FOCUS, ANARK_UPDATE_CLASS_BUTTON, self._EFFECTS_CONDUIT_TAG)
  end
end
self.SetupMenus = function(self)
  self.MAIN_MENU_INDEX = 1
  self.OPTIONS_MENU_INDEX = 2
  self.EXTRAS_MENU_INDEX = 3
  self.currentMenuIndex = self.MAIN_MENU_INDEX
  self.currentIndex = 1
  self.menuData = {}
  self.menuData[self.MAIN_MENU_INDEX] = {}
  self.menuData[self.MAIN_MENU_INDEX][1] = { 14, self.onMenuOptions }
  self.menuData[self.MAIN_MENU_INDEX][2] = { 15, self.onMenuIntel }
  self.menuData[self.MAIN_MENU_INDEX][3] = { 16, self.onMenuExtras }
  self.menuData[self.MAIN_MENU_INDEX][4] = { 17, self.onMenuQuit }
  self.menuData[self.OPTIONS_MENU_INDEX] = {}
  self.menuData[self.OPTIONS_MENU_INDEX][1] = { 18, self.onMenuPlainAV }
  self.menuData[self.OPTIONS_MENU_INDEX][2] = { 19, self.onMenuControlConfig }
  self.menuData[self.OPTIONS_MENU_INDEX][3] = { 20, self.onMenuBack }
  self.menuData[self.EXTRAS_MENU_INDEX] = {}
  local coop = CtlAnarkLoadMenuSelect:getIsCoop()
  if coop == 1 then
    self.menuData[self.EXTRAS_MENU_INDEX][1] = { 21, self.onMenuRewards }
    self.menuData[self.EXTRAS_MENU_INDEX][2] = { 20, self.onMenuBack }
  else
    self.menuData[self.EXTRAS_MENU_INDEX][1] = { 22, self.onMenuSkillPoints }
    self.menuData[self.EXTRAS_MENU_INDEX][2] = { 21, self.onMenuRewards }
    self.menuData[self.EXTRAS_MENU_INDEX][3] = { 20, self.onMenuBack }
  end
  self:pcAddEntry()
end
self.ActivateItem = function(self, itemIndex, itemObject)
  local menuData = self.menuData[self.currentMenuIndex]
  if menuData then
    local menuItem = menuData[itemIndex]
    if menuItem then
      local itemText = menuItem[1]
      self:SetText(itemText, itemObject)
    else
      ak.output("Bogus data for menu: " .. self.currentMenuIndex .. " with index: " .. itemIndex .. "\n")
    end
  else
    ak.output("Bogus data for menu: " .. self.currentMenuIndex .. "\n")
  end
  itemObject.opacity = 100
end
self.DeactivateItem = function(self, itemObject)
  self:SetText("", itemObject)
  itemObject.opacity = 0
end
self.SetText = function(self, text_value, itemObject)
  local text_item = itemObject:getChildByIndex(1)
  if text_value == self.PC_TEXT then
    SetLocalizedText(text_item, "")
    if not self:pcSetString(text_item, self.PC_TEXT) then
      ExposedDataManager:GetKey("PC_ENTRY:fail:0:the button text could not be set")
    end
    return
  end
  SetLocalizedText(text_item, text_value)
end
self.ClearAllButtons = function(self)
  local numButtons = self.menuItems:getNumChildren()
  for i = 1, numButtons do
    local button = self.menuItems:getChildByIndex(i)
    self:ClearButtonColor(button)
  end
end
self.ClearButtonColor = function(self, button)
  self:SetButtonDiffuseMaterialColor(button, 100, 100, 100)
end
self.SetButtonColor = function(self, button)
  self:SetButtonDiffuseMaterialColor(button, 225, 225, 225)
end
self.SetButtonDiffuseMaterialColor = function(self, button, r, g, b)
  if button == nil then
    return
  end
  local model = button:getChildByIndex(2)
  local material = model:getChildByIndex(1)
  material.diffuse.r = r
  material.diffuse.g = g
  material.diffuse.b = b
end
self.onMenuOptions = function(self)
  CtlAnarkPauseMenuSelect:onMenuOptions()
  self.currentMenuIndex = self.OPTIONS_MENU_INDEX
  self.listBehavior:resetList()
end
self.onMenuIntel = function(self)
  CtlAnarkPauseMenuSelect:onMenuIntel()
end
self.onMenuExtras = function(self)
  self.currentMenuIndex = self.EXTRAS_MENU_INDEX
  self.listBehavior:resetList()
end
self.onMenuQuit = function(self)
  self.quitControl:ActivateQuitControl()
end
self.onMenuGameOptions = function(self)
  CtlAnarkOptionsMenuSelect:onMenuGameOptions()
end
self.onMenuAVOptions = function(self)
  CtlAnarkOptionsMenuSelect:onMenuAVOptions()
  scene:goToSlide(2)
  SetDelayAnarkRender(true)
end
self.onMenuControlConfig = function(self)
  scene:goToSlide(3)
  SetDelayAnarkRender(true)
end
self.onMenuSkillPoints = function(self)
  scene:goToSlide(4)
end
self.onMenuRewards = function(self)
  scene:goToSlide(5)
end
self.onMenuBack = function(self)
  self.currentMenuIndex = self.MAIN_MENU_INDEX
  self.listBehavior:setSelectedIndex(self.currentMainMenuIndex)
  ak.output("current main menu index: " .. self.currentMainMenuIndex .. "\n")
end
self.onMenuContinue = function(self)
  CtlAnarkPauseMenuSelect:onMenuContinue()
end
self.PC_TEXT = "PC SETTINGS"
self.pcSetString = function(self, node, v)
  if pcall then
    return pcall(function() node.textString = v end)
  end
  node.textString = v
  return true
end
self.pcAddEntry = function(self)
  if ExposedDataManager:GetNumber(ExposedDataManager:GetKey("PC_ENTRY:0")) ~= 1 then
    return
  end
  local opts = self.menuData[self.OPTIONS_MENU_INDEX]
  local k = table.getn(opts)
  local n = 0
  if self.menuItems then n = self.menuItems:getNumChildren() end
  if n < k + 1 then
    ExposedDataManager:GetKey("PC_ENTRY:fail:0:the list has " .. n .. " buttons, the Options menu would need " .. (k + 1))
    return
  end
  opts[k + 1] = opts[k]
  opts[k] = { self.PC_TEXT, self.onMenuPcSettings }
  ExposedDataManager:GetKey("PC_ENTRY:ok:0")
end
self.onMenuPlainAV = function(self)
  ExposedDataManager:GetKey("PC_OPEN:0")
  self:onMenuAVOptions()
end
self.onMenuPcSettings = function(self)
  ExposedDataManager:GetKey("PC_OPEN:1")
  self:onMenuAVOptions()
end
)LUA";

/* windowHandler (frontend.ig chunk, 10033 bytes, FNV-1a 0xd262367b): the main menu's second list
 * (realm 4 = OPTIONS: Audio / Video, Controls, Movies, Credits; also the single player / co-op /
 * online lists and the difficulty pages), written back from the bytecode function for function, plus
 * pcAddEntry. The list is a ScrollableList that needs numVisibleRows + 1 buttons to scroll, and the
 * shipped frontend.ig has 4 (Text1..Text4, 4 visible): a fifth entry would index a nil row and raise a
 * script error on every list update. So OPTIONS stays 4 entries: Audio / Video, Controls, PC SETTINGS
 * (Audio / Video's own event after PC_OPEN:1), EXTRAS; EXTRAS swaps realm 4's tables for Movies, Credits
 * in the same buttons (no new realm: other scripts read mainMenuButtonRealm), Back / a choice there
 * swaps them back. MOVIES_INDEX follows the movies entry (0 = not shown). */
static const char kWin[] = R"LUA(
self.onInitialize = function(self)
  self.whatToDoMatrix = {}
  self.whatToDoMatrix[1] = { "spNewGame", "spContinue", "spLevelSelect" }
  self.whatToDoMatrix[2] = { "coopNewGame", "coopContinue", "coopLevelSelect" }
  self.whatToDoMatrix[3] = { "mpOnline", "mpOffline", "MPOptions" }
  self.whatToDoMatrix[4] = { "avOption", "controlConfig", "movies", "credits" }
  self.optionText = {}
  self.optionText[1] = { 110, 111, 112 }
  self.optionText[2] = { 110, 111, 112 }
  self.optionText[3] = { 113, 114, 115 }
  self.optionText[4] = { 116, 117, 118, 119 }
  self.optionText[5] = { 120, 121, 122, 123 }
  self.optionText[6] = { 124, "", 125, 126 }
  self:_useOnActivateDeactivate()
  self.listBehavior = self:getObjectReference("listBehavior")
  self.prevList = self:getObjectReference("prevList")
  self.nextList = self:getObjectReference("nextList")
  self.screenWindow = self:getObjectReference("screenWindow")
  self.numCompsValue = self:getObjectReference("numCompsValue")
  self.MOVIES_INDEX = 3
  self.pcAt = nil
  self.pcXAt = nil
  self.pcInX = false
  self:pcAddEntry()
end
self.onActivate = function(self)
  if self.pcInX then self:pcShowExtras(false) end
  ak.output(self.name .. " has focus [ " .. tostring(self.listBehavior.hasFocus) .. " ]\n")
  self:fireEvent(Event:new("onPostActivate"))
end
self.onPostActivate = function(self)
  if self.listBehavior.startDisabled == false then
    ak.output(self.name .. " list taking focus\n")
    self.listBehavior:takeFocus()
  end
end
self.handleSelect = function(self)
  if self.listIsActive then
    AnarkEffectsConduitInterface:EffectEventTriggered(ANARK_EFFECT_EVENT_TYPE_SELECTED, ANARK_UPDATE_CLASS_HIGHLIGHT_BOX, self._EFFECTS_CONDUIT_TAG)
    if scene.mainMenuButtonRealm == 1 or scene.mainMenuButtonRealm == 2 then
      if self.listBehavior.selectedItemIndex == 1 then
        if CtlAnarkLoadMenuSelect:getContinueLevel_new(scene.selectedCoop) < 1 then
          scene.mainMenuButtonRealm = 5
          self.listBehavior:resetList()
          if LobbyAnarkEvents.GetLocalizeTerritory() ~= 0 and LobbyAnarkEvents.GetLocalizeTerritory() ~= 4 then
            self.listBehavior:setSelectedIndex(2)
          end
          return
        else
          self.listBehavior:releaseFocus()
          self.nextList:takeFocus()
          self.nextList:setSelectedIndex(2)
          self.nextList.parent.opacity = 100
          return
        end
      end
    elseif scene.mainMenuButtonRealm == 5 then
      scene.difficultyLevel = self.listBehavior.selectedItemIndex
      local tempKey = ExposedDataManager:GetKey("InvertAxisY")
      local invertedY = ExposedDataManager:GetNumber(tempKey)
      if CtlAnarkLoadMenuSelect:getContinueLevel_new(scene.selectedCoop) < 1 and scene.selectedCoop == 0 and invertedY == 0 and CtlAnarkLoadMenuSelect:getNumCompletes_new(scene.selectedCoop) < 1 then
        scene.mainMenuButtonRealm = 6
        self.listBehavior:resetList()
        return
      end
      if scene.selectedCoop == 1 then
        scene.mainMenuButtonRealm = 2
      else
        scene.mainMenuButtonRealm = 1
      end
      self.prevList.listUpdater:fireEvent(Event:new(self.whatToDoMatrix[scene.mainMenuButtonRealm][1]))
      return
    elseif scene.mainMenuButtonRealm == 6 then
      if self.listBehavior.selectedItemIndex == 4 then
        ak.output("Y-INVERTED!\n")
        CtlAnarkControlConfigSelect:onMenuInvertY()
        CtlAnarkControlConfigSelect:onMenuSaveConfig()
      end
      if scene.selectedCoop == 1 then
        scene.mainMenuButtonRealm = 2
      else
        scene.mainMenuButtonRealm = 1
      end
      self.prevList.listUpdater:fireEvent(Event:new(self.whatToDoMatrix[scene.mainMenuButtonRealm][1]))
      return
    end
    local ev = self.whatToDoMatrix[scene.mainMenuButtonRealm][self.listBehavior.selectedItemIndex]
    if scene.mainMenuButtonRealm == 4 and self.pcXAt then
      if self.pcInX then
        self:pcShowExtras(false)
        self.listBehavior:resetList()
        self.listBehavior:setSelectedIndex(self.pcXAt)
      elseif self.listBehavior.selectedItemIndex == self.pcXAt then
        self:pcShowExtras(true)
        self.listBehavior:resetList()
        return
      elseif self.listBehavior.selectedItemIndex == self.pcAt then
        ExposedDataManager:GetKey("PC_OPEN:1")
      elseif self.listBehavior.selectedItemIndex == 1 then
        ExposedDataManager:GetKey("PC_OPEN:0")
      end
    end
    self.listBehavior.startDisabled = false
    self.prevList:releaseFocus()
    self.prevList.startDisabled = true
    self.prevList.listUpdater:fireEvent(Event:new(ev))
  end
end
self.handleBack = function(self)
  if self.listIsActive then
    AnarkEffectsConduitInterface:EffectEventTriggered(ANARK_EFFECT_EVENT_TYPE_WENT_BACK, ANARK_UPDATE_CLASS_HIGHLIGHT_BOX, self._EFFECTS_CONDUIT_TAG)
    if scene.mainMenuButtonRealm == 4 and self.pcInX then
      self:pcShowExtras(false)
      self.listBehavior:resetList()
      self.listBehavior:setSelectedIndex(self.pcXAt)
    elseif scene.mainMenuButtonRealm == 5 then
      if scene.selectedCoop == 1 then
        scene.mainMenuButtonRealm = 2
        self.listBehavior:resetList()
      else
        scene.mainMenuButtonRealm = 1
        self.listBehavior:resetList()
      end
    elseif scene.mainMenuButtonRealm == 6 then
      scene.mainMenuButtonRealm = 5
      self.listBehavior:resetList()
    else
      self.listBehavior:releaseFocus()
      self.prevList:takeFocus()
      self.screenWindow.opacity = 0
    end
  end
end
self.setListItemValue = function(self, itemIndex, itemObject)
  if not itemObject then
    return
  end
  if 0 < scene.mainMenuButtonRealm then
    local v = self.optionText[scene.mainMenuButtonRealm][itemIndex]
    if v == self.PC_TEXT or v == self.PC_XTEXT then
      SetLocalizedText(itemObject, "")
      if not self:pcSetString(itemObject, v) then
        ExposedDataManager:GetKey("PC_ENTRY:fail:1:the button text could not be set")
      end
    else
      SetLocalizedText(itemObject, v)
    end
  end
  if (scene.mainMenuButtonRealm == 1 or scene.mainMenuButtonRealm == 2) and itemIndex == 2 and CtlAnarkLoadMenuSelect:getContinueLevel_new(scene.selectedCoop) < 1 and 0 < CtlAnarkLoadMenuSelect:getNumCompletes_new(scene.selectedCoop) then
    itemObject.opacity = 30
  elseif scene.mainMenuButtonRealm == 4 and itemIndex == self.MOVIES_INDEX and not FrontEndAnarkEvents.isMovieUnlocked(0) then
    itemObject.opacity = 30
  else
    itemObject.opacity = 60
  end
  if (scene.mainMenuButtonRealm == 1 or scene.mainMenuButtonRealm == 2) and 0 < CtlAnarkLoadMenuSelect:getNumCompletes_new(scene.selectedCoop) then
    self.numCompsValue.textString = CtlAnarkLoadMenuSelect:getNumCompletes_new(scene.selectedCoop)
    self.numCompsValue.parent.opacity = 100
  else
    self.numCompsValue.parent.opacity = 0
  end
end
self.clearListItemValue = function(self, itemIndex, itemObject)
  if not itemObject then
    return
  end
  SetLocalizedText(itemObject, "")
end
self.getNumListItems = function(self)
  if 0 < scene.mainMenuButtonRealm then
    if scene.mainMenuButtonRealm == 1 or scene.mainMenuButtonRealm == 2 then
      if CtlAnarkLoadMenuSelect:getContinueLevel_new(scene.selectedCoop) < 1 and CtlAnarkLoadMenuSelect:getNumCompletes_new(scene.selectedCoop) < 1 then
        ak.output("You don't have any checkpoints!\n")
        return table.getn(self.optionText[scene.mainMenuButtonRealm]) - 2
      elseif CtlAnarkLoadMenuSelect:getHighestDifficultyBeaten(1, scene.selectedCoop) < 1 then
        ak.output("You've never beaten the first level!\n")
        return table.getn(self.optionText[scene.mainMenuButtonRealm]) - 1
      else
        return table.getn(self.optionText[scene.mainMenuButtonRealm])
      end
    elseif scene.mainMenuButtonRealm == 5 and SkillPoints.isSuperhumanUnlocked() < 1 then
      return table.getn(self.optionText[scene.mainMenuButtonRealm]) - 1
    else
      return table.getn(self.optionText[scene.mainMenuButtonRealm])
    end
  else
    return 0
  end
end
self.isSelectable = function(self, itemIndex)
  if scene.mainMenuButtonRealm == 6 and (itemIndex == 1 or itemIndex == 2) then
    return false
  end
  if (scene.mainMenuButtonRealm == 1 or scene.mainMenuButtonRealm == 2) and itemIndex == 2 and CtlAnarkLoadMenuSelect:getContinueLevel_new(scene.selectedCoop) < 1 and 0 < CtlAnarkLoadMenuSelect:getNumCompletes_new(scene.selectedCoop) then
    return false
  end
  if scene.mainMenuButtonRealm == 4 and itemIndex == self.MOVIES_INDEX and not FrontEndAnarkEvents.isMovieUnlocked(0) then
    local debug_areMoviesUnlocked = FrontEndAnarkEvents.debug_areAllMoviesUnlocked()
    if debug_areMoviesUnlocked == true then
      return true
    else
      return false
    end
  end
  return true
end
self.onSelectedItemChanged = function(self, itemIndex, itemObject, isReset)
  if isReset == false then
    AnarkEffectsConduitInterface:EffectEventTriggered(ANARK_EFFECT_EVENT_TYPE_LOST_FOCUS, ANARK_UPDATE_CLASS_HIGHLIGHT_BOX, self._EFFECTS_CONDUIT_TAG)
  end
end
self.onDeactivate = function(self)
end
self.onUpdate = function(self)
  self.listIsActive = self.listBehavior.hasFocus
  if self.listBehavior.selectedItemIndex == 1 and not self:isSelectable(1) then
    ak.output("MOVED HIGHLIGHT\n")
    self.listBehavior:moveSelection(1)
  end
end
self.PC_TEXT = "PC SETTINGS"
self.PC_XTEXT = "EXTRAS"
self.pcSetString = function(self, node, v)
  if pcall then
    return pcall(function() node.textString = v end)
  end
  node.textString = v
  return true
end
self.pcShowExtras = function(self, on)
  self.pcInX = on
  local t = self.pcTop
  if on then t = self.pcSub end
  self.whatToDoMatrix[4] = t[1]
  self.optionText[4] = t[2]
  if on then self.MOVIES_INDEX = 1 else self.MOVIES_INDEX = 0 end
end
self.pcAddEntry = function(self)
  if ExposedDataManager:GetNumber(ExposedDataManager:GetKey("PC_ENTRY:1")) ~= 1 then
    return
  end
  local opts = self.whatToDoMatrix[4]
  local texts = self.optionText[4]
  local k = table.getn(opts)
  if k ~= 4 or table.getn(texts) ~= 4 or opts[3] ~= "movies" or opts[4] ~= "credits" then
    ExposedDataManager:GetKey("PC_ENTRY:fail:1:the main menu's Options list is not Audio / Video, Controls, Movies, Credits")
    return
  end
  local n = 0
  local lb = self.listBehavior
  if lb and lb.itemsGroup then n = lb.itemsGroup:getNumChildren() end
  if n < k then
    ExposedDataManager:GetKey("PC_ENTRY:fail:1:the list has " .. n .. " buttons, the Options list needs " .. k)
    return
  end
  self.pcTop = { { opts[1], opts[2], "avOption", "pcExtras" }, { texts[1], texts[2], self.PC_TEXT, self.PC_XTEXT } }
  self.pcSub = { { opts[3], opts[4] }, { texts[3], texts[4] } }
  self:pcShowExtras(false)
  self.pcAt = 3
  self.pcXAt = 4
  ExposedDataManager:GetKey("PC_ENTRY:ok:1")
end
)LUA";

/* FrontEndMenuNav (frontend.ig chunk, 3767 bytes, FNV-1a 0xd8983eaa): the main menu's slide changes, written
 * back from the bytecode function for function. frontend.ig binds the Audio / Video list's onBackPressed to
 * two actions, OptionHandler.handleBack and this script's gotoFrontEndMenuMain (pause.ig binds only
 * handleBack, which goes to the slide itself), so every Back on the PC page also left Audio / Video for the
 * main menu slide (1116: Back in a PC category left the PC page for the main menu, not the categories).
 * gotoFrontEndMenuMain now first asks PC_KEEP: 1 when the PC page takes this Back (a category -> the
 * categories, or the volume-row way's categories -> the audio rows). The two actions may run in either
 * order, so the native side answers from handleBack's PC_KEEP report when it came first, else from the
 * PC_HOLD state the page keeps (keep_query). R1_MENU_BACKNAV=0 leaves this chunk alone. */
static const char kNav[] = R"LUA(
self.onInitialize = function(self)
  self.targetSlide = nil
  self.slideChangeCountDown = 0
  self.fadeToBlack = self:getObjectReference("fadeToBlack")
  if FrontEndAnarkEvents.isGameEnding() then
    self:gotoFrontEndResults()
  end
end
self.onUpdate = function(self)
  if self.targetSlide then
    if self.slideChangeCountDown == 0 then
      self:goToSlide(self.targetSlide)
      self.targetSlide = nil
      self.slideChangeCountDown = 0
    else
      self.slideChangeCountDown = self.slideChangeCountDown - 1
    end
  end
end
self.gotoFrontEndMenuMain = function(self)
  if ExposedDataManager:GetNumber(ExposedDataManager:GetKey("PC_KEEP")) == 1 then
    return
  end
  if not self.targetSlide then
    scene.focusNone()
    self.targetSlide = 1
    self.slideChangeCountDown = 4
  end
end
self.gotoFrontEndMenuProfileSelect = function(self)
  if not self.targetSlide then
    scene.focusNone()
    self.targetSlide = 2
    self.slideChangeCountDown = 4
  end
end
self.gotoFrontEndMenuAVOptions = function(self)
  if not self.targetSlide then
    SetDelayAnarkRender(true)
    scene.focusNone()
    self.targetSlide = 4
    self.slideChangeCountDown = 4
  end
end
self.gotoFrontEndMenuControlConfig = function(self)
  if not self.targetSlide then
    SetDelayAnarkRender(true)
    scene.focusNone()
    self.targetSlide = 5
    self.slideChangeCountDown = 4
  end
end
self.gotoFrontEndMenuLevelLoad = function(self)
  if not self.targetSlide then
    scene.focusNone()
    self.targetSlide = 3
    self.slideChangeCountDown = 4
  end
end
self.gotoFrondEndMenuMPOffline = function(self)
  if not self.targetSlide then
    SetDelayAnarkRender(true)
    scene.focusNone()
    self.targetSlide = 6
    self.slideChangeCountDown = 4
  end
end
self.gotoFrontEndResults = function(self)
  if not self.targetSlide then
    scene.focusNone()
    self.targetSlide = 7
    self.slideChangeCountDown = 4
  end
end
self.gotoSplitStaging = function(self)
  if not self.targetSlide then
    scene.focusNone()
    self.targetSlide = 8
    self.slideChangeCountDown = 4
  end
end
self.gotoMPOptions = function(self)
  if not self.targetSlide then
    scene.focusNone()
    self.targetSlide = 9
    self.slideChangeCountDown = 4
  end
end
self.gotoLobby = function(self)
  FrontEndAnarkEvents.goToLobby()
end
self.gotoMovies = function(self)
  if not self.targetSlide then
    scene.focusNone()
    self.targetSlide = 10
    self.slideChangeCountDown = 4
  end
end
self.beginFade = function(self)
  self.fadeToBlack:goToSlide(2)
end
)LUA";

/* Compile-time limits of the game's menus: the Audio / Video list has 5 buttons (4 PC rows + the
 * page row, or the 5 categories), the pause menu list has 4 buttons (the main menu) and its Options
 * menu 3 entries, so PC SETTINGS makes 4 (checked again at run time: kMmu reports fail when the list
 * is shorter). The main menu's OPTIONS list keeps its 4 entries (EXTRAS holds Movies / Credits; kWin
 * checks the buttons at run time). Each page needs 2 * AV_ROWS + 1 text slots alive at once (a name and a
 * value per row, the title; two pages' worth for a repaint during a step). The sources are copied below
 * the text slots. */
enum { PER_PAGE = 4, AV_ROWS = 5, MAIN_BUTTONS = 4, OPTIONS_ENTRIES = 3, SLOTS = 48, SLOT_LEN = 256, MAX_PAGES = 99 };
static_assert(PER_PAGE + 1 == AV_ROWS, "a PC page is the Audio / Video list: rows + the page row");
static_assert(OPTIONS_ENTRIES + 1 <= MAIN_BUTTONS, "PC SETTINGS must fit the pause menu's button list");
static_assert(SLOTS >= 2 * (2 * AV_ROWS + 1), "text slots for two pages");
static_assert(SLOTS * SLOT_LEN <= 0x8000, "the text slots fit the upper half of the area");
static_assert(0x7F000u + SLOTS <= 0x7F0F0u, "slot keys below the fixed keys");
static const uint32_t KEY_SLOT0 = 0x7F000u, KEY_PAGES = 0x7F0F0u, KEY_ENTRY = 0x7F0F1u, KEY_HERE = 0x7F0F2u,
                      KEY_OPEN = 0x7F0F3u, KEY_ENTRY_MAIN = 0x7F0F4u, KEY_HERE_MAIN = 0x7F0F5u, KEY_CATS = 0x7F0F6u,
                      KEY_CPAGES = 0x7F0F7u, KEY_KEEP = 0x7F0F8u, KEY_LAST = KEY_KEEP;
static int s_slot = 0, s_qcat = 0;   /* s_qcat: the category PC_CPAGES:<c> asked about */

enum { MODE_OFF = 0, MODE_ENTRY = 1, MODE_ROWS = 2 };
static int mode(void)
{
    static int v = -1;
    if (v < 0) {
        const char* e = getenv("R1_MENU_ROWS");
        if (!e || !*e) e = getenv("RFOM_MENU_ROWS");
        v = (e && *e) ? atoi(e) : MODE_ENTRY;
        if (v < MODE_OFF || v > MODE_ROWS) v = MODE_ENTRY;
        LOG("PC settings in the game's menus: %s", v == MODE_ENTRY ? "Options > PC SETTINGS (R1_MENU_ROWS=1; 2 = X on a volume row, 0 = off)"
                                                  : v == MODE_ROWS ? "X on a volume row in Audio / Video (R1_MENU_ROWS=2)" : "off (R1_MENU_ROWS=0)");
    }
    return v;
}
static int enabled(void) { return mode() != MODE_OFF; }
static int back_nav(void)
{
    static int v = -1;
    if (v < 0) {
        const char* e = getenv("R1_MENU_BACKNAV");
        if (!e || !*e) e = getenv("RFOM_MENU_BACKNAV");
        v = !(e && *e && atoi(e) == 0);
        LOG("main menu PC page Back: %s", v ? "stays on the PC page (R1_MENU_BACKNAV=1; 0 = the original FrontEndMenuNav)"
                                             : "original FrontEndMenuNav, every Back leaves for the main menu (R1_MENU_BACKNAV=0)");
    }
    return v;
}

/* Back on the main menu's PC page: frontend.ig binds onBackPressed to OptionHandler.handleBack and
 * FrontEndMenuNav.gotoFrontEndMenuMain, the order not known here. handleBack reports PC_KEEP:<v> (v = 1
 * when the page takes this Back) and the page keeps PC_HOLD:<v> (v = what a Back would report now).
 * kNav asks PC_KEEP: a report less than KEEP_WINDOW_MS old answers it (handleBack ran first); else HOLD
 * does (kNav ran first), and the report that follows within the window belongs to that same Back. */
enum { KEEP_WINDOW_MS = 250 };
static int s_hold = 0, s_keep = -1, s_nav_open = 0;
static uint64_t s_keep_ms = 0, s_nav_ms = 0;
static void keep_report(int v, uint64_t now)
{
    const int paired = s_nav_open && now - s_nav_ms <= KEEP_WINDOW_MS;
    s_nav_open = 0;
    s_keep = paired ? -1 : v;
    s_keep_ms = now;
}
static int keep_query(uint64_t now)
{
    int v;
    if (s_keep >= 0 && now - s_keep_ms <= KEEP_WINDOW_MS) { v = s_keep; s_nav_open = 0; }
    else { v = s_hold; s_nav_open = 1; s_nav_ms = now; }
    s_keep = -1;
    return v;
}
static uint64_t steady_ms(void)
{
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

/* runtime state of the entry, per menu (the pause menu and the main menu each have their own Options
 * list and Audio / Video script): list swapped, Audio / Video swapped, entry added, why not */
enum { MENU_PAUSE = 0, MENU_MAIN = 1, MENUS = 2 };
static const char* const kMenuName[MENUS] = { "the pause menu", "the main menu" };
struct MenuState { int list_ok, oh_bad, entry_live; std::string fail; bool here_logged; };
static MenuState s_menu[MENUS];
static volatile int s_cur_menu = -1;   /* the menu that used the protocol last (the PC_SHOW log) */
static volatile int s_open = 0, s_show = 0, s_direct = 0;
static void entry_failed(int m, const std::string& why)
{
    MenuState& st = s_menu[m];
    st.entry_live = 0;
    if (st.fail.empty()) {
        st.fail = why;
        LOG("no PC SETTINGS entry in %s's Options: %s; the PC rows stay in its Audio / Video (X / Enter on a volume row)",
            kMenuName[m], why.c_str());
    }
}
/* a menu's list script: swapped (ok), failed to load, or another version (left alone). Another version
 * only counts while that menu's list is not swapped: intel.ig has its own MainMenuUpdater. */
static void list_swapped(int m, bool ok)
{
    s_cur_menu = m;
    if (!ok) { entry_failed(m, std::string("the ") + (m == MENU_MAIN ? "main" : "pause") + " menu script did not load"); return; }
    MenuState& st = s_menu[m];
    st.list_ok = 1; st.fail.clear(); st.here_logged = false;
}
static void list_mismatch(int m)
{
    if (!s_menu[m].list_ok) entry_failed(m, std::string("the ") + (m == MENU_MAIN ? "main" : "pause") + " menu script is another version");
}
static void oh_bad(int m, const char* why)
{
    s_menu[m].oh_bad = 1;
    entry_failed(m, why);
}
static uint32_t area(void) { return ppu_hle_inject_base + 0xFF0000u; }   /* last 64 KB of the HLE inject region */
static std::string guest_str(uint32_t ea, size_t max)
{
    std::string r;
    if (!ea || !vm_base) return r;
    for (size_t i = 0; i < max; i++) { const char c = (char)vm_base[ea + i]; if (!c) break; r += c; }
    return r;
}
static int rows(void) { return rfom_pm_rows() + rfom_pcx_rows(); }

/* The categories, the same names and order in all three ports (R3 / R2 menus too). A row's category
 * comes from its settings key; F10 PC Options rows have none, so their titles name them. */
enum { CAT_DISPLAY, CAT_GRAPHICS, CAT_EFFECTS, CAT_CONTROLS, CAT_AUDIO, NCATS };
static const char* const kCatLabel[NCATS] = { "PC DISPLAY", "PC GRAPHICS", "PC EFFECTS", "PC CONTROLS & HUD", "PC AUDIO & LANGUAGE" };
static_assert(NCATS <= AV_ROWS, "the category list is the Audio / Video list: one row per category");
static const struct { const char* title; const char* key; } kPmKey[] = {
    { "Frame rate", "fps" }, { "Anti-aliasing", "fsr" }, { "Mouse sensitivity", "mouse_sens" },
    { "Field of view", "fov" }, { "Button prompts", "padglyphs" },
};
static const struct { const char* key; int cat; } kCatOf[] = {
    { "fps", CAT_DISPLAY }, { "fov", CAT_DISPLAY }, { "gamma", CAT_DISPLAY }, { "hdr", CAT_DISPLAY },
    { "hdr_paper", CAT_DISPLAY }, { "hdr_peak", CAT_DISPLAY }, { "hdr_sat", CAT_DISPLAY }, { "uiscale", CAT_DISPLAY },
    { "winremember", CAT_DISPLAY }, { "discord", CAT_DISPLAY },
    { "fsr", CAT_GRAPHICS }, { "reflex", CAT_GRAPHICS }, { "latency", CAT_GRAPHICS }, { "dlss_fg", CAT_GRAPHICS },
    { "fg_backend", CAT_GRAPHICS }, { "ovgraph", CAT_GRAPHICS }, { "benchkey", CAT_GRAPHICS },
    { "colorblind", CAT_EFFECTS },
    { "mouse_sens", CAT_CONTROLS }, { "padglyphs", CAT_CONTROLS }, { "crosshair", CAT_CONTROLS },
    { "xhair_color", CAT_CONTROLS }, { "xhair_size", CAT_CONTROLS }, { "cutscene_skip", CAT_CONTROLS },
    { "cutscene_skip_all", CAT_CONTROLS },
    { "skipintro", CAT_CONTROLS }, { "pad_disconnect_pause", CAT_CONTROLS }, { "padbackend", CAT_CONTROLS },
    { "gyro", CAT_CONTROLS }, { "gyrosens", CAT_CONTROLS }, { "gyroinvy", CAT_CONTROLS }, { "gyrospace", CAT_CONTROLS },
    { "lightbar", CAT_CONTROLS }, { "nswap", CAT_CONTROLS }, { "rumble", CAT_CONTROLS }, { "enterbtn", CAT_CONTROLS },
    { "padbg", CAT_CONTROLS }, { "outerdz", CAT_CONTROLS }, { "antidz", CAT_CONTROLS }, { "trigdz", CAT_CONTROLS },
    { "turnboost", CAT_CONTROLS }, { "crouchmode", CAT_CONTROLS },
    { "volume", CAT_AUDIO }, { "audio_mono", CAT_AUDIO }, { "language", CAT_AUDIO },
};
static std::string row_key(int g)
{
    const int npm = rfom_pm_rows();
    if (g >= npm) { const char* k = rfom_pcx_key(g - npm); return k ? k : ""; }
    char t[96], v[96];
    rfom_pm_row(g, t, sizeof t, v, sizeof v);
    for (const auto& e : kPmKey) if (!strcmp(e.title, t)) return e.key;
    return "";
}
static int cat_of(const std::string& key)
{
    for (const auto& e : kCatOf) if (key == e.key) return e.cat;
    return -1;
}
/* the rows of each category (global row numbers, in the F10 + More PC Options order) and the
 * categories that have rows, in list order; built once (the row lists are fixed after start) */
struct Cats { std::vector<int> rows[NCATS]; std::vector<int> present; };
static const Cats& cats(void)
{
    static Cats c;
    static bool built = false;
    if (built) return c;
    built = true;
    const int n = rows();
    for (int g = 0; g < n; g++) {
        const std::string k = row_key(g);
        int ct = cat_of(k);
        if (ct < 0) { LOG("PC row %d (key '%s') has no category; shown under %s", g, k.c_str(), kCatLabel[CAT_DISPLAY]); ct = CAT_DISPLAY; }
        c.rows[ct].push_back(g);
    }
    for (int i = 0; i < NCATS; i++) if (!c.rows[i].empty()) c.present.push_back(i);
    LOG("%d PC rows in %d categories", n, (int)c.present.size());
    return c;
}
/* ci = the category's place in the list (1-based, as the Lua shows them); NULL when there is none */
static const std::vector<int>* cat_rows(int ci)
{
    const Cats& c = cats();
    return ci >= 1 && ci <= (int)c.present.size() ? &c.rows[c.present[ci - 1]] : nullptr;
}
static const char* cat_label(int ci)
{
    const Cats& c = cats();
    return ci >= 1 && ci <= (int)c.present.size() ? kCatLabel[c.present[ci - 1]] : "";
}
static int cat_pages(int ci)
{
    const std::vector<int>* r = cat_rows(ci);
    const int p = r && !r->empty() ? ((int)r->size() + PER_PAGE - 1) / PER_PAGE : 0;
    return p > MAX_PAGES ? MAX_PAGES : p;
}
/* the global row of entry n (0-based) of category ci, or -1 */
static int cat_row(int ci, int n)
{
    const std::vector<int>* r = cat_rows(ci);
    return r && n >= 0 && n < (int)r->size() ? (*r)[n] : -1;
}
/* The label column of the Audio / Video rows: size-60 text from x = -370, so about 28 characters
 * reach the screen edge (the old 48-character "TITLE: VALUE" ran off it). Rows show a short name
 * (by settings key; the title when there is none) and the value, cut to fit. */
enum { ROW_CAP = 28, TITLE_CAP = 40 };
static const struct { const char* key; const char* name; } kShort[] = {
    { "fps", "FRAME RATE" }, { "fov", "FIELD OF VIEW" }, { "gamma", "BRIGHTNESS" }, { "hdr", "HDR OUTPUT" },
    { "hdr_paper", "HDR PAPER WHITE" }, { "hdr_peak", "HDR PEAK" }, { "hdr_sat", "HDR SATURATION" },
    { "uiscale", "UI SCALE" }, { "winremember", "WINDOW POSITION" }, { "discord", "DISCORD" },
    { "fsr", "ANTI-ALIASING" }, { "reflex", "LOW LATENCY" }, { "latency", "LATENCY MODE" },
    { "dlss_fg", "FRAME GEN" }, { "fg_backend", "FRAME GEN TYPE" }, { "ovgraph", "FRAME GRAPH" },
    { "benchkey", "BENCHMARK KEY" }, { "colorblind", "COLOUR-BLIND" }, { "mouse_sens", "MOUSE SPEED" },
    { "padglyphs", "BUTTON PROMPTS" }, { "crosshair", "CROSSHAIR" }, { "xhair_color", "XHAIR COLOUR" },
    { "xhair_size", "XHAIR SIZE" }, { "cutscene_skip", "HOLD TO SKIP" }, { "cutscene_skip_all", "SKIP UNWATCHED" }, { "skipintro", "SKIP INTRO" },
    { "pad_disconnect_pause", "PAD LOSS PAUSE" }, { "padbackend", "CONTROLLERS" }, { "gyro", "GYRO AIM" },
    { "gyrosens", "GYRO SPEED" }, { "gyroinvy", "GYRO VERTICAL" }, { "gyrospace", "GYRO AXIS" },
    { "lightbar", "LIGHT BAR" }, { "nswap", "NINTENDO LAYOUT" }, { "rumble", "VIBRATION" },
    { "enterbtn", "CONFIRM BUTTON" }, { "padbg", "PAD IN BACKGROUND" }, { "outerdz", "OUTER DEADZONE" },
    { "antidz", "ANTI-DEADZONE" }, { "trigdz", "TRIGGER DEADZONE" }, { "turnboost", "TURN BOOST" },
    { "crouchmode", "CROUCH" }, { "volume", "MASTER VOLUME" }, { "audio_mono", "MONO AUDIO" },
    { "language", "LANGUAGE" },
};
/* the game's menu font: plain ASCII, upper case like the game's own labels */
static uint32_t put_slot(const std::string& s, size_t cap = ROW_CAP)
{
    const int k = s_slot; s_slot = (s_slot + 1) % SLOTS;
    const uint32_t ea = area() + 0x8000u + (uint32_t)k * SLOT_LEN;
    size_t n = 0;
    for (size_t i = 0; i < s.size() && n < cap; i++) {
        const unsigned char c = (unsigned char)s[i];
        if (i + 1 < s.size() && c == ' ' && s[i + 1] == ' ') continue;
        vm_base[ea + n++] = (char)(c >= 0x20 && c < 0x7F ? toupper(c) : '?');
    }
    vm_base[ea + n] = 0;
    return KEY_SLOT0 + (uint32_t)k;
}
static std::string row_text(int g)
{
    char t[96], v[96];
    const int npm = rfom_pm_rows();
    if (g < 0 || g >= rows()) return "";
    if (g < npm) rfom_pm_row(g, t, sizeof t, v, sizeof v);
    else rfom_pcx_row(g - npm, t, sizeof t, v, sizeof v);
    return std::string(t) + ": " + v;
}
/* what a row shows: "SHORT NAME: VALUE" within ROW_CAP (the value cut with ".." when needed) */
static std::string row_label(int g)
{
    char t[96], v[96];
    const int npm = rfom_pm_rows();
    if (g < 0 || g >= rows()) return "";
    if (g < npm) rfom_pm_row(g, t, sizeof t, v, sizeof v);
    else rfom_pcx_row(g - npm, t, sizeof t, v, sizeof v);
    std::string name = t;
    const std::string key = row_key(g);
    for (const auto& e : kShort) if (key == e.key) { name = e.name; break; }
    if (name.size() > ROW_CAP - 6) name.resize(ROW_CAP - 6);
    std::string val = v;
    const size_t room = ROW_CAP - name.size() - 2;
    if (val.size() > room) { val.resize(room - 2); while (!val.empty() && val.back() == ' ') val.pop_back(); val += ".."; }
    return name + ": " + val;
}
/* the same row for the two columns: the short name in the label column, the value in the
 * game's own value column (right-aligned at x = 370), so name + gap + value stay within ROW_CAP */
enum { NAME_CAP = 17, VAL_MIN = 8 };
static void row_parts(int g, std::string& name, std::string& val)
{
    char t[96], v[96];
    name.clear(); val.clear();
    const int npm = rfom_pm_rows();
    if (g < 0 || g >= rows()) return;
    if (g < npm) rfom_pm_row(g, t, sizeof t, v, sizeof v);
    else rfom_pcx_row(g - npm, t, sizeof t, v, sizeof v);
    name = t;
    const std::string key = row_key(g);
    for (const auto& e : kShort) if (key == e.key) { name = e.name; break; }
    if (name.size() > NAME_CAP) name.resize(NAME_CAP);
    while (!name.empty() && name.back() == ' ') name.pop_back();
    val = v;
    size_t room = ROW_CAP - 3 - name.size();
    if (room < VAL_MIN) room = VAL_MIN;
    if (val.size() > room) { val.resize(room - 2); while (!val.empty() && val.back() == ' ') val.pop_back(); val += ".."; }
    if (val.empty()) val = "-";
}
static void row_change(int g, int d)
{
    const int npm = rfom_pm_rows();
    if (g < 0 || g >= rows()) return;
    if (g < npm) rfom_pm_change(g, d); else rfom_pcx_change(g - npm, d);
    LOG("row %d -> %s", g, row_text(g).c_str());
}

extern "C" PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);

static uint32_t be32(uint32_t ea) { const uint8_t* q = vm_base + ea; return (uint32_t)q[0] << 24 | q[1] << 16 | q[2] << 8 | q[3]; }

/* luaL_loadbuffer(L, buff, size, name): OptionHandler -> kLua, MainMenuUpdater (pause.ig) -> kMmu.
 * Both EBOOT and game.self keep Lua 5.0's parser (lua_load: first byte 0x1B -> luaU_undump,
 * else luaY_parser; "unexpected symbol" / "%s near `%s'" are present), so plain source loads.
 * The source is loaded by calling the lifted luaL_loadbuffer (`self`) once from here; if that
 * fails, the error message is popped and the original precompiled chunk loads instead, and that
 * chunk's swap stays off for the rest of the run. Returns 1 when the call is complete (status in r3).
 * The hooks have C++ linkage: the lifted code declares them at block scope. */
struct Swap { const char* name; const char* src; size_t len; bool inside, failed; };
static Swap s_oh = { "OptionHandler (pause menu)", kLua, sizeof kLua - 1, false, false };
static Swap s_ohfe = { "OptionHandler (main menu)", kLua, sizeof kLua - 1, false, false };
static Swap s_mmu = { "MainMenuUpdater", kMmu, sizeof kMmu - 1, false, false };
static Swap s_win = { "windowHandler", kWin, sizeof kWin - 1, false, false };
static Swap s_nav = { "FrontEndMenuNav", kNav, sizeof kNav - 1, false, false };
static const char kFePrefix[] = "self.pcFE = true\n";   /* kLua for frontend.ig: its On / Off texts and Back */
static_assert(sizeof kFePrefix + sizeof kLua < 0x8000 && sizeof kMmu < 0x8000 && sizeof kWin < 0x8000 && sizeof kNav < 0x8000,
              "the Lua sources must fit below the text slots");
/* the exact chunks the sources are written for (size, FNV-1a of those bytes) */
static const uint32_t MMU_SIZE = 10866u, MMU_FNV = 0x29cc95a0u;     /* pause.ig MainMenuUpdater */
static const uint32_t WIN_SIZE = 10033u, WIN_FNV = 0xd262367bu;     /* frontend.ig windowHandler */
static const uint32_t NAV_SIZE = 3767u, NAV_FNV = 0xd8983eaau;      /* frontend.ig FrontEndMenuNav */
static const uint32_t OH_SIZE = 7125u, OH_FNV = 0xe656c381u;        /* pause.ig OptionHandler */
static const uint32_t OHFE_SIZE = 7345u, OHFE_FNV = 0x7caeb93cu;    /* frontend.ig OptionHandler */

static uint32_t fnv1a(const uint8_t* p, size_t n)
{
    uint32_t h = 0x811c9dc5u;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x01000193u; }
    return h;
}
static int swap_load(ppu_context* ctx, void (*self)(ppu_context*), Swap& s, uint32_t size)
{
    memcpy(vm_base + area(), s.src, s.len);
    const uint64_t r3 = ctx->gpr[3], r4 = ctx->gpr[4], r5 = ctx->gpr[5], r6 = ctx->gpr[6], lr = ctx->lr;
    ctx->gpr[4] = area();
    ctx->gpr[5] = s.len;
    s.inside = true;
    self(ctx);
    while (g_trampoline_fn) { void (*tf)(void*) = g_trampoline_fn; g_trampoline_fn = 0; tf(ctx); }
    s.inside = false;
    ctx->lr = lr;
    const uint32_t status = (uint32_t)ctx->gpr[3];
    if (status == 0) {
        LOG("%s chunk (%u bytes) -> PC rows source (%u bytes)", s.name, size, (unsigned)s.len);
        return 1;
    }
    /* Lua 5.0, 32-bit: L->top at +8, TObject {int tt; Value v} = 8 bytes, TString data at +16 */
    std::string msg = "?";
    const uint32_t L = (uint32_t)r3, top = be32(L + 8);
    if (be32(top - 8) == 4 /* LUA_TSTRING */) {
        const uint32_t ts = be32(top - 4), len = be32(ts + 12);
        if (ts && len < 512) msg = guest_str(ts + 16, len);
    }
    for (char& c : msg) if ((unsigned char)c < 0x20 || (unsigned char)c >= 0x7F) c = '?';
    {
        const uint32_t t = top - 8;   /* lua_pop(L, 1) */
        uint8_t* q = vm_base + L + 8;
        q[0] = (uint8_t)(t >> 24); q[1] = (uint8_t)(t >> 16); q[2] = (uint8_t)(t >> 8); q[3] = (uint8_t)t;
    }
    s.failed = true;
    LOG("%s: PC rows source failed to load (status %u: %s); using the original chunk", s.name, status, msg.c_str());
    ctx->gpr[3] = r3; ctx->gpr[4] = r4; ctx->gpr[5] = r5; ctx->gpr[6] = r6;
    return 0;
}
int rfom_menu_rows_loadbuf(ppu_context* ctx, void (*self)(ppu_context*))
{
    if (s_oh.inside || s_ohfe.inside || s_mmu.inside || s_win.inside || s_nav.inside) return 0;
    const uint32_t buf = (uint32_t)ctx->gpr[4], size = (uint32_t)ctx->gpr[5];
    if (!vm_base || size < 40 || size > 0x100000u) return 0;
    const uint8_t* p = vm_base + buf;
    if (memcmp(p, "\x1bLuaP", 5)) return 0;
    if (!memcmp(p + 18, "\x0e\0\0\0OptionHandler", 18)) {
        if (!enabled()) return 0;
        const bool pause = size >= OH_SIZE && fnv1a(p, OH_SIZE) == OH_FNV;
        const bool fe = !pause && size >= OHFE_SIZE && fnv1a(p, OHFE_SIZE) == OHFE_FNV;
        if (!pause && !fe) {
            /* which menu it belongs to is unknown: neither may add an entry that opens unchanged Audio / Video */
            LOG("OptionHandler chunk (%u bytes): neither the pause.ig (%u, %08x) nor the frontend.ig (%u, %08x) "
                "Audio / Video script; left alone, no PC rows in this menu", size, OH_SIZE, OH_FNV, OHFE_SIZE, OHFE_FNV);
            for (int m = 0; m < MENUS; m++) oh_bad(m, "unknown Audio / Video script version");
            return 0;
        }
        const int m = fe ? MENU_MAIN : MENU_PAUSE;
        Swap& sw = fe ? s_ohfe : s_oh;
        if (sw.failed) return 0;
        static std::string fe_src;
        if (fe && fe_src.empty()) { fe_src = std::string(kFePrefix) + kLua; sw.src = fe_src.c_str(); sw.len = fe_src.size(); }
        const int r = swap_load(ctx, self, sw, size);
        if (!r) oh_bad(m, "the Audio / Video script (OptionHandler) did not load");
        return r;
    }
    if (!memcmp(p + 18, "\x10\0\0\0MainMenuUpdater", 20)) {
        if (mode() != MODE_ENTRY || s_mmu.failed) return 0;
        const uint32_t h = size >= MMU_SIZE ? fnv1a(p, MMU_SIZE) : 0;
        if (size < MMU_SIZE || h != MMU_FNV) {
            LOG("MainMenuUpdater chunk (%u bytes, FNV-1a %08x): not the pause.ig menu this entry is written for "
                "(%u bytes, %08x); left alone", size, h, MMU_SIZE, MMU_FNV);
            list_mismatch(MENU_PAUSE);
            return 0;
        }
        const int r = swap_load(ctx, self, s_mmu, size);
        list_swapped(MENU_PAUSE, r != 0);
        return r;
    }
    if (!memcmp(p + 18, "\x0e\0\0\0windowHandler", 18)) {
        if (mode() != MODE_ENTRY || s_win.failed) return 0;
        const uint32_t h = size >= WIN_SIZE ? fnv1a(p, WIN_SIZE) : 0;
        if (size < WIN_SIZE || h != WIN_FNV) {
            LOG("windowHandler chunk (%u bytes, FNV-1a %08x): not the frontend.ig menu this entry is written for "
                "(%u bytes, %08x); left alone", size, h, WIN_SIZE, WIN_FNV);
            list_mismatch(MENU_MAIN);
            return 0;
        }
        const int r = swap_load(ctx, self, s_win, size);
        list_swapped(MENU_MAIN, r != 0);
        return r;
    }
    if (!memcmp(p + 18, "\x10\0\0\0FrontEndMenuNav", 20)) {
        if (!enabled() || !back_nav() || s_nav.failed) return 0;
        const uint32_t h = size >= NAV_SIZE ? fnv1a(p, NAV_SIZE) : 0;
        if (size < NAV_SIZE || h != NAV_FNV) {
            LOG("FrontEndMenuNav chunk (%u bytes, FNV-1a %08x): not the frontend.ig version the Back fix is written for "
                "(%u bytes, %08x); left alone, Back on the main menu's PC page also leaves for the main menu",
                size, h, NAV_SIZE, NAV_FNV);
            return 0;
        }
        return swap_load(ctx, self, s_nav, size);
    }
    return 0;
}
/* port 0's held buttons as names ("CROSS|START"), host bit order: DIGITAL1 in the low byte, DIGITAL2 above */
static std::string pad_names(uint32_t b)
{
    static const char* const kBit[16] = { "SELECT", "L3", "R3", "START", "UP", "RIGHT", "DOWN", "LEFT",
                                          "L2", "R2", "L1", "R1", "TRIANGLE", "CIRCLE", "CROSS", "SQUARE" };
    std::string s;
    for (int i = 0; i < 16; i++)
        if (b & (1u << i)) { if (!s.empty()) s += '|'; s += kBit[i]; }
    return s.empty() ? "none" : s;
}
/* one page diagnostic line with the pad buttons the game read; at most DIAG_BURST lines a second
 * and DIAG_TOTAL in all, the dropped ones counted on the next line that gets through */
enum { DIAG_BURST = 10, DIAG_TOTAL = 400 };
static std::string diag_line(const char* what, uint64_t now_ms)
{
    static uint64_t s_sec = ~0ull;
    static int s_in_sec = 0, s_total = 0, s_dropped = 0;
    const uint64_t sec = now_ms / 1000;
    if (sec != s_sec) { s_sec = sec; s_in_sec = 0; }
    if (s_total >= DIAG_TOTAL || s_in_sec >= DIAG_BURST) { ++s_dropped; return ""; }
    ++s_total; ++s_in_sec;
    char b[48] = "";
    if (s_dropped) snprintf(b, sizeof b, " [%d lines dropped]", s_dropped);
    s_dropped = 0;
    std::string line = std::string("PC page (") + kMenuName[s_cur_menu == MENU_MAIN ? MENU_MAIN : MENU_PAUSE] + "): " + what
                     + "; pad " + pad_names(g_rfom_pad0_btns) + b;
    if (s_total == DIAG_TOTAL) line += " (no more diagnostic lines)";
    return line;
}
static void diag_line(const char* what)
{
    const uint64_t ms = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const std::string line = diag_line(what, ms);
    if (!line.empty()) LOG("%s", line.c_str());
}
/* ExposedDataManager::GetKey(name) -> key */
int rfom_menu_rows_getkey(ppu_context* ctx)
{
    if (!vm_base || !ctx->gpr[3]) return 0;
    const uint8_t* p = vm_base + (uint32_t)ctx->gpr[3];
    if (p[0] != 'P' || p[1] != 'C' || p[2] != '_') return 0;
    const std::string n = guest_str((uint32_t)ctx->gpr[3], 160);
    uint32_t key;
    if (n == "PC_PAGES") key = KEY_PAGES;
    else if (n == "PC_ENTRY:0" || n == "PC_ENTRY:1") { s_cur_menu = n[9] - '0'; key = s_cur_menu ? KEY_ENTRY_MAIN : KEY_ENTRY; }
    else if (n == "PC_HERE:0" || n == "PC_HERE:1") { s_cur_menu = n[8] - '0'; key = s_cur_menu ? KEY_HERE_MAIN : KEY_HERE; }
    else if (n == "PC_OPEN") key = KEY_OPEN;
    else if (n == "PC_KEEP") key = KEY_KEEP;
    else if (n == "PC_KEEP:0" || n == "PC_KEEP:1") { keep_report(n[8] - '0', steady_ms()); key = KEY_KEEP; }
    else if (n == "PC_HOLD:0" || n == "PC_HOLD:1") { s_hold = n[8] - '0'; key = KEY_KEEP; }
    else if (n == "PC_OPEN:1") { s_open = 1; key = KEY_OPEN; }
    else if (n == "PC_OPEN:0") { s_open = 2; key = KEY_OPEN; }
    else if (!n.compare(0, 8, "PC_DIAG:")) {
        diag_line(n.c_str() + 8);
        key = KEY_PAGES;
    }
    else if (n == "PC_ENTRY:ok:0" || n == "PC_ENTRY:ok:1") {
        const int m = s_cur_menu = n[12] - '0';
        s_menu[m].entry_live = s_menu[m].fail.empty();
        if (s_menu[m].entry_live) LOG("PC SETTINGS entry added to %s's Options list", kMenuName[m]);
        key = KEY_ENTRY;
    } else if (!n.compare(0, 14, "PC_ENTRY:fail:") && n.size() > 16 && (n[14] == '0' || n[14] == '1') && n[15] == ':') {
        const int m = s_cur_menu = n[14] - '0';
        entry_failed(m, n.substr(16));
        key = KEY_ENTRY;
    }
    else if (!n.compare(0, 8, "PC_SHOW:")) {
        const int was = s_show, ci = n.size() > 10 && n[9] == ':' ? atoi(n.c_str() + 10) : 0;
        s_show = atoi(n.c_str() + 8);
        if (!s_show) s_direct = 0, s_hold = 0;
        if (s_show == 2) LOG("PC page: %s", cat_label(ci));
        else if (s_show != was) LOG("PC SETTINGS page %s (%s)", s_show ? "open: the category list" : "closed",
                                    s_cur_menu == MENU_MAIN ? kMenuName[MENU_MAIN] : kMenuName[MENU_PAUSE]);
        key = KEY_PAGES;
    } else if (!n.compare(0, 9, "PC_TITLE:")) {
        const int ci = atoi(n.c_str() + 9);
        const char* lab = cat_label(ci);
        /* drawn literally (allow_text_on_element), so plain text, not the .ig's "\\OPTIONS\AUDIO VISUAL" key form */
        key = put_slot(ci > 0 && *lab ? std::string(lab) : std::string("PC SETTINGS"), TITLE_CAP);
    } else if (n == "PC_CATS") key = KEY_CATS;
    else if (!n.compare(0, 7, "PC_CAT:")) key = put_slot(cat_label(atoi(n.c_str() + 7)));
    else if (!n.compare(0, 10, "PC_CPAGES:")) { s_qcat = atoi(n.c_str() + 10); key = KEY_CPAGES; }
    else if (!n.compare(0, 7, "PC_ROW:")) {
        int ci = 0, e = -1;
        sscanf(n.c_str() + 7, "%d:%d", &ci, &e);
        const int g = cat_row(ci, e);
        std::string t = g >= 0 ? row_label(g) : "";
        if (t.empty() && g >= 0) {
            static int s_bad_row = -1;
            if (s_bad_row != g) { s_bad_row = g; LOG("PC row %d (%s) has no text; shown as (unavailable)", g, cat_label(ci)); }
            t = "(unavailable)";
        }
        key = put_slot(t);
    } else if (!n.compare(0, 9, "PC_RNAME:") || !n.compare(0, 8, "PC_RVAL:")) {
        const bool nm = n[4] == 'N';
        int ci = 0, e = -1;
        sscanf(n.c_str() + (nm ? 9 : 8), "%d:%d", &ci, &e);
        const int g = cat_row(ci, e);
        std::string name, val;
        row_parts(g, name, val);
        if (name.empty() && g >= 0) name = "(unavailable)";
        key = put_slot(nm ? name : val);
    } else if (!n.compare(0, 8, "PC_STEP:")) {
        int ci = 0, e = -1, d = 1;
        sscanf(n.c_str() + 8, "%d:%d:%d", &ci, &e, &d);
        const int g = cat_row(ci, e);
        if (g >= 0) row_change(g, d);
        key = put_slot(g >= 0 ? row_label(g) : "");
    } else if (!n.compare(0, 7, "PC_NAV:")) {
        int ci = 0, p = 1;
        sscanf(n.c_str() + 7, "%d:%d", &ci, &p);
        const int np = cat_pages(ci);   /* the category is in the title: the row is "NEXT PAGE (1/6)" */
        char b[64];
        if (np <= 1) snprintf(b, sizeof b, "BACK");
        else snprintf(b, sizeof b, p < np ? "NEXT PAGE (%d/%d)" : "BACK (%d/%d)", p, np);
        key = put_slot(b);
    } else return 0;
    ctx->gpr[3] = key;
    return 1;
}
/* ExposedDataManager::GetNumber(key) -> f1 */
int rfom_menu_rows_getnum(ppu_context* ctx)
{
    const uint32_t k = (uint32_t)ctx->gpr[3];
    if (k < KEY_SLOT0 || k > KEY_LAST) return 0;
    double v = 0.0;
    const int ncats = (int)cats().present.size();
    if (k == KEY_PAGES || k == KEY_CATS) v = (double)ncats;
    else if (k == KEY_CPAGES) v = (double)cat_pages(s_qcat);
    else if (k == KEY_ENTRY || k == KEY_ENTRY_MAIN) {
        const int m = k == KEY_ENTRY_MAIN ? MENU_MAIN : MENU_PAUSE;
        const MenuState& st = s_menu[m];
        v = mode() == MODE_ENTRY && st.list_ok && !st.oh_bad && ncats > 0;
        if (mode() == MODE_ENTRY && !v)
            entry_failed(m, !st.list_ok ? "the Options menu script was not swapped"
                            : st.oh_bad ? "the Audio / Video script was not swapped" : "no PC rows");
    } else if (k == KEY_HERE || k == KEY_HERE_MAIN) {
        const int m = k == KEY_HERE_MAIN ? MENU_MAIN : MENU_PAUSE;
        MenuState& st = s_menu[m];
        v = mode() == MODE_ROWS || (mode() == MODE_ENTRY && !st.entry_live);
        if (v && mode() == MODE_ENTRY && !st.here_logged) {
            st.here_logged = true;
            LOG("PC rows on the volume rows of %s's Audio / Video (X / Enter): %s", kMenuName[m],
                st.fail.empty() ? "no PC SETTINGS entry in this menu (another script)" : st.fail.c_str());
        }
    } else if (k == KEY_OPEN) {
        v = s_open;
        if (s_open == 1) s_direct = 1;
        s_open = 0;
    } else if (k == KEY_KEEP) {
        v = keep_query(steady_ms());
        if (v) LOG("main menu: Back stays on the PC page (FrontEndMenuNav skipped)");
    }
    ctx->fpr[1] = v;
    return 1;
}
/* ExposedDataManager::GetCharString(out, key): [out] = char* */
int rfom_menu_rows_getstr(ppu_context* ctx)
{
    const uint32_t out = (uint32_t)ctx->gpr[3], k = (uint32_t)ctx->gpr[4];
    if (k < KEY_SLOT0 || k >= KEY_SLOT0 + SLOTS || !out) return 0;
    vm_write32(out, area() + 0x8000u + (k - KEY_SLOT0) * SLOT_LEN);
    return 1;
}
