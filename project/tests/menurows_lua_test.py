"""The Audio / Video options handler with PC rows (project/rfom_menurows.cpp, R1_MENU_ROWS),
run against mock Anark objects and a Python copy of the native PC_ protocol.
Needs lupa (pip install lupa); skipped (exit 0) without it.
  python project/tests/menurows_lua_test.py"""
import os, re, sys
here = os.path.dirname(os.path.abspath(__file__))
src = open(os.path.join(here, '..', 'rfom_menurows.cpp'), encoding='utf-8').read()

# ---- every PC row has a category (no lupa needed): the More PC Options keys and the F10 rows' titles ----
pcx = open(os.path.join(here, '..', 'rfom_pcextra.cpp'), encoding='utf-8').read()
pcx_keys = [re.search(r'(?<!L)"([a-z_0-9]+)"', seg).group(1) for seg in pcx.split('opt_add(')[1:] if not seg.startswith('const')]
cat_of = dict(re.findall(r'\{ "([a-z_0-9]+)", (CAT_[A-Z]+) \}', src))
pm_keys = re.findall(r'\{ "[A-Za-z -]+", "([a-z_]+)" \}', src)
labels_c = re.search(r'kCatLabel\[NCATS\] = \{ (.*?) \};', src).group(1)
cat_names = re.findall(r'"([^"]+)"', labels_c)
assert cat_names == ['PC DISPLAY', 'PC GRAPHICS', 'PC EFFECTS', 'PC CONTROLS & HUD', 'PC AUDIO & LANGUAGE'], cat_names
assert len(pcx_keys) >= 35 and len(pm_keys) == 5, (len(pcx_keys), pm_keys)
missing = [k for k in pm_keys + pcx_keys if k not in cat_of]
assert not missing, 'PC rows without a category: %s' % missing
print('categories OK: %d rows, %s' % (len(pm_keys) + len(pcx_keys),
      {c: sum(1 for k in pm_keys + pcx_keys if cat_of[k] == c) for c in sorted(set(cat_of.values()))}))

try:
    from lupa import LuaRuntime
except ImportError:
    print('skipped: pip install lupa'); sys.exit(0)
lua_src = re.search(r'kLua\[\] = R"LUA\((.*?)\)LUA"', src, re.S).group(1)
mmu_src = re.search(r'kMmu\[\] = R"LUA\((.*?)\)LUA"', src, re.S).group(1)
win_src = re.search(r'kWin\[\] = R"LUA\((.*?)\)LUA"', src, re.S).group(1)
fe_prefix = re.search(r'kFePrefix\[\] = "(.*?)\\n"', src).group(1) + '\n'
# the mock rows by category (the categories with rows, in list order, as the native side reports them)
rows = [['FRAME RATE LIMIT', ['60','72','90']], ['ANTI-ALIASING', ['OFF','FSR']]] + [['OPT%d' % i, ['A','B']] for i in range(7)]
cats = [('PC DISPLAY', [0, 2, 3, 4, 5, 6]), ('PC GRAPHICS', [1]), ('PC CONTROLS & HUD', [7, 8])]
sel = [0]*len(rows); slots = {}; ctr = [0]; log = []
def put(s):
    k = ctr[0] % 16; ctr[0] += 1; slots[0x7F000 + k] = s.upper(); return 0x7F000 + k
def text(g): return '' if g < 0 or g >= len(rows) else '%s: %s' % (rows[g][0], rows[g][1][sel[g]])
def cat_row(c, n): return cats[c - 1][1][n] if 1 <= c <= len(cats) and 0 <= n < len(cats[c - 1][1]) else -1
def cat_pages(c): return (len(cats[c - 1][1]) + 3) // 4 if 1 <= c <= len(cats) else 0
st = {'here': 1, 'entry': 1, 'open': 0, 'show': None, 'qcat': 0, 'reports': [], 'menus': [], 'diag': []}
# main menu Back (PC_KEEP / PC_HOLD): a Python copy of keep_report / keep_query on a fake clock (ms)
clk = [0]; nv = {'hold': 0, 'keep': -1, 'open': 0, 'keep_t': 0, 'nav_t': 0}
def keep_report(v):
    paired = nv['open'] and clk[0] - nv['nav_t'] <= 250
    nv['open'] = 0; nv['keep'] = -1 if paired else v; nv['keep_t'] = clk[0]
def keep_query():
    if nv['keep'] >= 0 and clk[0] - nv['keep_t'] <= 250: v = nv['keep']; nv['open'] = 0
    else: v = nv['hold']; nv['open'] = 1; nv['nav_t'] = clk[0]
    nv['keep'] = -1; return v
def getkey(_, name):
    if name == 'PC_KEEP': return 0x7F0F8
    if name in ('PC_KEEP:0', 'PC_KEEP:1'): keep_report(int(name[8])); return 0x7F0F8
    if name in ('PC_HOLD:0', 'PC_HOLD:1'): nv['hold'] = int(name[8]); return 0x7F0F8
    if name in ('PC_PAGES', 'PC_CATS'): return 0x7F0F6
    # each menu (0 pause, 1 main) names itself; st['menus'] records which one asked
    if name in ('PC_ENTRY:0', 'PC_ENTRY:1', 'PC_HERE:0', 'PC_HERE:1'):
        st['menus'].append(name); return {'PC_ENTRY': 0x7F0F1, 'PC_HERE': 0x7F0F2}[name[:-2]]
    if name == 'PC_OPEN': return 0x7F0F3
    if name == 'PC_OPEN:1': st['open'] = 1; return 0x7F0F3
    if name == 'PC_OPEN:0': st['open'] = 2; return 0x7F0F3
    if name.startswith('PC_DIAG:'): st['diag'].append(name[8:]); return 0x7F0F0
    if name.startswith('PC_ENTRY:ok:') or name.startswith('PC_ENTRY:fail:'):
        r = name[9:]; m = r[3] if r.startswith('ok:') else r[5]
        st['menus'].append('report:' + m); st['reports'].append(r.replace(':' + m, '', 1)); return 0x7F0F1
    if name.startswith('PC_SHOW:'):
        st['show'] = int(name[8:].split(':')[0])
        if st['show'] == 0: nv['hold'] = 0
        return 0x7F0F0
    if name.startswith('PC_TITLE:'):
        c = int(float(name[9:])); return put(cats[c - 1][0] if 1 <= c <= len(cats) else 'PC SETTINGS')
    if name.startswith('PC_CAT:'):
        c = int(float(name[7:])); return put(cats[c - 1][0] if 1 <= c <= len(cats) else '')
    if name.startswith('PC_CPAGES:'): st['qcat'] = int(float(name[10:])); return 0x7F0F7
    if name.startswith('PC_ROW:'):
        c, n = (int(float(x)) for x in name[7:].split(':')); return put(text(cat_row(c, n)))
    if name.startswith('PC_RNAME:') or name.startswith('PC_RVAL:'):
        c, n = (int(float(x)) for x in name.split(':')[1:]); g = cat_row(c, n)
        if g < 0 or g >= len(rows): return put('')
        return put(rows[g][0] if name.startswith('PC_RNAME:') else rows[g][1][sel[g]])
    if name.startswith('PC_STEP:'):
        c, n, dd = (int(float(x)) for x in name[8:].split(':')); g = cat_row(c, n)
        if g >= 0: sel[g] = (sel[g] + dd) % len(rows[g][1]); log.append((g, dd))
        return put(text(g))
    if name.startswith('PC_NAV:'):
        c, p = (int(float(x)) for x in name[7:].split(':')); np = cat_pages(c)
        return put('BACK' if np <= 1 else ('NEXT PAGE (%d/%d)' if p < np else 'BACK (%d/%d)') % (p, np))
    return {'SubtitlesOn': 3, 'HelpTextOn': 4, 'InvertAxisY': 5}[name]
def getnum(_, k):
    if k == 0x7F0F6: return len(cats)
    if k == 0x7F0F7: return cat_pages(st['qcat'])
    if k == 0x7F0F1: return st['entry']
    if k == 0x7F0F2: return st['here']
    if k == 0x7F0F3:
        v = st['open']; st['open'] = 0; return v
    if k == 0x7F0F8: return keep_query()
    return 1 if k == 3 else 0
L = LuaRuntime(unpack_returned_tuples=True)
g = L.globals()
L.execute('''
calls = {}
CtlAnarkAVOptionsSelect = setmetatable({}, {__index=function(t,k) return function(_, a) table.insert(calls, k .. tostring(a)) end end})
AnarkEffectsConduitInterface = { EffectEventTriggered = function() end }
function SetLocalizedText(n, id) if type(id) == "string" then n.textString = id else n.textString = "LOC" .. id end end
function SetDelayAnarkRender() end
scene = { goToSlide = function(_, n) table.insert(calls, "goToSlide" .. n) end }
self = { _useOnActivateDeactivate = function() end,
  getObjectReference = function() return { getNumChildren = function() return 5 end } end }
items = {}
local labs = { "SUBTITLES:", "MUSIC VOLUME:", "EFFECTS VOLUME:", "SPEECH VOLUME:", "HELP TEXT:" }
for i = 1, 5 do items[i] = { Label = { textString = labs[i] }, Value = { opacity = 100, textString = "" } } end
''')
mgr = L.eval('{}')
mgr['GetKey'] = getkey; mgr['GetNumber'] = getnum
L.execute('function _mkcs(k) return { m_char_string = _slot(k) } end')
g._slot = lambda k: slots.get(int(k))
mgr['GetCharString'] = L.eval('function(_, k) return _mkcs(k) end')
g.ExposedDataManager = mgr
L.execute(lua_src)
S = g.self
def shown(i):
    lab, val = L.eval('items[%d].Label.textString' % i), L.eval('items[%d].Value.textString' % i)
    return lab + ': ' + val if val else lab
def labels(): return [shown(i) for i in range(1, 6)]   # the label column, then the value column
def ops(): return [L.eval('items[%d].Value.opacity' % i) for i in range(1, 6)]
def at(i): L.execute('self:onSelectedItemChanged(%d, items[%d], false)' % (i, i))
CATS = ['PC DISPLAY: OPEN', 'PC GRAPHICS: OPEN', 'PC CONTROLS & HUD: OPEN', '', '']
L.execute('self:onInitialize(); for i=1,5 do self:setListItemValue(i, items[i]) end')
assert L.eval('items[1].Value.textString') == 'LOC79'
L.execute('self:onSelectedItemChanged(1, items[1], true); self:handleSelect()')
assert 'onMenuSubtitlesnil' in list(L.eval('calls').values())
L.execute('self:onSelectedItemChanged(2, items[2], false); self:onRight()')
assert list(L.eval('calls').values())[-1] == 'onMenuMusicVolume1'
# volume-row way (PC_HERE): X on a volume row -> the category list
L.execute('self:handleSelect()')
print(labels()); assert labels() == CATS and st['show'] == 1 and ops() == [100]*5, labels()
L.execute('self:onLeft()'); assert labels() == CATS                       # Left does nothing on the list
at(4); L.execute('self:handleSelect()'); assert labels() == CATS and L.eval('self.pcCat') == 0   # no 4th category
at(1); L.execute('self:handleSelect()')                                    # X opens PC DISPLAY
print(labels()); assert labels() == ['FRAME RATE LIMIT: 60', 'OPT0: A', 'OPT1: A', 'OPT2: A', 'NEXT PAGE (1/2)'] and st['show'] == 2, labels()
# the name in the label column, the value in the row's own Value text (the right column), shown
assert L.eval('items[1].Label.textString') == 'FRAME RATE LIMIT' and L.eval('items[1].Value.textString') == '60' and ops() == [100]*5
L.execute('self:onRight(); self:onRight(); self:onLeft()')
assert labels()[0] == 'FRAME RATE LIMIT: 72', labels()
L.execute('self:handleSelect()'); assert labels()[0] == 'FRAME RATE LIMIT: 90'
at(5); L.execute('self:handleSelect()'); print(labels())
assert labels() == ['OPT3: A', 'OPT4: A', '', '', 'BACK (2/2)'], labels()
at(3); L.execute('self:onRight()'); assert len(log) == 4 and labels()[2] == ''   # an empty row: no change
at(5); L.execute('self:onLeft()'); assert labels()[0] == 'FRAME RATE LIMIT: 90'  # page back
L.execute('self:onLeft()'); assert labels() == CATS and st['show'] == 1        # before page 1 -> categories
at(3); L.execute('self:onRight()')                                          # Right also opens (PC CONTROLS & HUD)
assert labels() == ['OPT5: A', 'OPT6: A', '', '', 'BACK'], labels()
at(2); L.execute('self:onRight()'); assert labels()[1] == 'OPT6: B' and log[-1] == (8, 1)
L.execute('self:handleBack()'); assert labels() == CATS and L.eval('self.pcMode') == True   # Back: a category -> categories
at(2); L.execute('self:handleSelect()'); assert labels()[0] == 'ANTI-ALIASING: OFF' and labels()[4] == 'BACK'
at(5); L.execute('self:handleSelect()'); assert labels() == CATS               # past the last page -> categories
L.execute('self:handleBack()')                                              # Back on the categories -> the audio rows
print(labels(), ops()); assert labels()[1] == 'MUSIC VOLUME:' and ops() == [100]*5 and L.eval('self.pcMode') == False
assert L.eval('items[1].Value.textString') == 'LOC79' and st['show'] == 0
assert [L.eval('items[%d].Value.textString' % i) for i in range(2, 5)] == [''] * 3   # the values put back
L.execute('self:onSelectedItemChanged(3, items[3], false); self:handleSelect(); self:handleBack()')
assert labels()[2] == 'EFFECTS VOLUME:' and L.eval('self.pcMode') == False
L.execute('self:handleBack()'); assert list(L.eval('calls').values())[-1] == 'goToSlide1'
L.execute('self:handleSelect(); self:onDeactivate(); self:onActivate()'); assert L.eval('self.pcMode') == False
assert st['show'] == 0
# R1_MENU_ROWS=1 with the entry live: X on a volume row does nothing (as in the original)
st['here'] = 0
L.execute('self:onSelectedItemChanged(2, items[2], false); self:handleSelect()'); assert L.eval('self.pcMode') == False
# Options > PC SETTINGS: PC_OPEN set, the screen activates -> straight into the categories; Back there -> Options
st['open'] = 1
L.execute('self:onActivate()'); assert L.eval('self.pcMode') == True and L.eval('self.pcDirect') == True and st['show'] == 1
assert labels() == CATS, labels()
at(1); L.execute('self:handleSelect()'); at(5); L.execute('self:handleSelect(); self:handleSelect()')   # 1/2 -> 2/2 -> categories
assert labels() == CATS and L.eval('self.pcMode') == True
at(1); L.execute('self:handleSelect()'); assert labels()[4] == 'NEXT PAGE (1/2)'
n = len(list(L.eval('calls').values()))
L.execute('self:handleBack()'); assert labels() == CATS and list(L.eval('calls').values())[n:] == []
L.execute('self:handleBack()'); assert L.eval('self.pcMode') == False and st['show'] == 0
assert list(L.eval('calls').values())[n:] == ['goToSlide1'] and labels()[1] == 'MUSIC VOLUME:'
# PC_OPEN before the list items are known: opens on the first setListItemValue round
L.execute('self.pcItems = {}'); st['open'] = 1
L.execute('self:onActivate()'); assert L.eval('self.pcMode') == False and L.eval('self.pcWant') == True
L.execute('for i=1,5 do self:setListItemValue(i, items[i]) end'); assert L.eval('self.pcMode') == True and labels() == CATS
L.execute('self:handleBack()')
# reopening starts on the categories again (not the last category)
st['open'] = 1; L.execute('self:onActivate()'); assert labels() == CATS and L.eval('self.pcCat') == 0
L.execute('self:handleBack()')
# the plain Audio / Video entry (no PC_OPEN) stays the original screen
L.execute('self:onActivate()'); assert L.eval('self.pcMode') == False
print('lua sim OK; steps', log)

# ---- MainMenuUpdater (pause menu) with the PC SETTINGS entry ----
def mmu_run(entry, buttons=4):
    st['entry'] = entry; st['reports'] = []; st['open'] = 0
    M = LuaRuntime(unpack_returned_tuples=True)
    M.execute('''
calls = {}
table.getn = function(t) return #t end
ak = { output = function(s) end }
local function rec(name) return setmetatable({}, {__index=function(t,k) return function(_, a) table.insert(calls, name .. "." .. k .. tostring(a)) end end}) end
CtlAnarkPauseMenuSelect = rec("Pause")
CtlAnarkPauseMenuSelect.wentToIntelMenu = function() return 0 end
CtlAnarkOptionsMenuSelect = rec("Options")
CtlAnarkLoadMenuSelect = { getIsCoop = function() return 0 end }
AnarkEffectsConduitInterface = { EffectEventTriggered = function() end }
function SetLocalizedText(n, id) if type(id) == "string" then n.textString = id else n.textString = "LOC" .. id end end
function SetDelayAnarkRender() end
scene = { goToSlide = function(_, n) table.insert(calls, "goToSlide" .. n) end }
buttons = {}
for i = 1, NBUTTONS do
  local mat = { diffuse = {} }
  buttons[i] = { opacity = 0, text = { textString = "" },
    getChildByIndex = function(b, j) if j == 1 then return b.text end return { getChildByIndex = function() return mat end } end }
end
local menuItems = { getNumChildren = function() return NBUTTONS end, getChildByIndex = function(_, i) return buttons[i] end }
listBehavior = { hasFocus = true, resetList = function() table.insert(calls, "resetList") end,
  setSelectedIndex = function(_, i) table.insert(calls, "select" .. tostring(i)) end }
local refs = { menuItems = menuItems, listBehavior = listBehavior, quitControl = { ActivateQuitControl = function() end } }
self = { _useOnActivateDeactivate = function() end, getObjectReference = function(_, n) return refs[n] end }
'''.replace('NBUTTONS', str(buttons)))
    mgr2 = M.eval('{}'); mgr2['GetKey'] = getkey; mgr2['GetNumber'] = getnum
    M.globals().ExposedDataManager = mgr2
    M.execute(mmu_src)
    M.execute('self:onInitialize(); self:onActivate(); self:onSelectedItemChanged(1, buttons[1], true)')
    return M
def mmu_fill(M):
    M.execute('for i = 1, 4 do if i <= self:getNumListItems() then self:setListItemValue(i, buttons[i]) else self:clearListItemValue(i, buttons[i]) end end')
    return [M.eval('buttons[%d].text.textString' % i) for i in range(1, 5)]
M = mmu_run(1)
assert mmu_fill(M) == ['LOC14', 'LOC15', 'LOC16', 'LOC17'] and st['reports'] == ['ok']
M.execute('self:handleSelect()')   # Options
assert M.eval('self.currentMenuIndex') == 2 and M.eval('self.currentMainMenuIndex') == 1
t = mmu_fill(M); print(t); assert t == ['LOC18', 'LOC19', 'PC SETTINGS', 'LOC20'], t
M.execute('self:onSelectedItemChanged(3, buttons[3], false); self:handleSelect()')
c = list(M.eval('calls').values()); assert c[-2:] == ['Options.onMenuAVOptionsnil', 'goToSlide2'], c
assert st['open'] == 1
M.execute('self:onSelectedItemChanged(1, buttons[1], false); self:handleSelect()')   # AUDIO / VIDEO: plain request
c = list(M.eval('calls').values()); assert c[-2:] == ['Options.onMenuAVOptionsnil', 'goToSlide2'] and st['open'] == 2, c
st['open'] = 0
M.execute('self:onSelectedItemChanged(4, buttons[4], false); self:handleSelect()')   # Back
assert M.eval('self.currentMenuIndex') == 1 and list(M.eval('calls').values())[-1] == 'select1'
M.execute('self:onSelectedItemChanged(1, buttons[1], false); self:handleSelect(); self:onSelectedItemChanged(2, buttons[2], false); self:handleSelect()')
assert mmu_fill(M) == ['LOC18', 'LOC19', 'PC SETTINGS', 'LOC20']
M.execute('self:handleBack()'); assert M.eval('self.currentMenuIndex') == 1
M.execute('self:handleBack()'); assert list(M.eval('calls').values())[-1] == 'Pause.onMenuContinuenil'
# the original lists without the entry (R1_MENU_ROWS=2 / 0 never swap, PC_ENTRY 0 = no rows / OptionHandler failed)
M = mmu_run(0)
M.execute('self:handleSelect()'); assert mmu_fill(M) == ['LOC18', 'LOC19', 'LOC20', ''] and st['reports'] == []
M.execute('self:handleBack(); self.currentIndex = 3; self:handleSelect()'); assert mmu_fill(M) == ['LOC22', 'LOC21', 'LOC20', '']
# too few buttons for 4 entries: no entry, the reason is reported
M = mmu_run(1, buttons=3)
M.execute('self:handleSelect()'); assert [M.eval('self:getNumListItems()')] == [3] and st['reports'][0].startswith('fail:the list has 3 buttons'), st['reports']
print('main menu sim OK')

# ---- the frontend.ig OptionHandler (kFePrefix + kLua): its On / Off ids and Back (save + exit effect) ----
L.execute('calls = {}; ANARK_EFFECT_EVENT_TYPE_EXITED = "EXITED"; AnarkEffectsConduitInterface = { EffectEventTriggered = function(_, e) table.insert(calls, "fx" .. e) end }')
L.execute(fe_prefix + lua_src)
L.execute('self:onInitialize(); for i=1,5 do self:setListItemValue(i, items[i]) end')
assert L.eval('items[1].Value.textString') == 'LOC101', L.eval('items[1].Value.textString')
L.execute('self:handleBack()'); assert list(L.eval('calls').values()) == ['onMenuSaveConfignil', 'fxEXITED'], list(L.eval('calls').values())
st['open'] = 1; L.execute('calls = {}; self:onActivate()'); assert L.eval('self.pcMode') == True
L.execute('self:handleBack()'); assert L.eval('self.pcMode') == False and list(L.eval('calls').values()) == ['onMenuSaveConfignil', 'fxEXITED']
# frontend.ig binds the list's Back to handleBack and FrontEndMenuNav.gotoFrontEndMenuMain (kNav), either order:
# a category -> the categories, the categories -> the main menu slide (direct) / the audio rows (volume-row way)
nav_src = re.search(r'kNav\[\] = R"LUA\((.*?)\)LUA"', src, re.S).group(1)
N = LuaRuntime(unpack_returned_tuples=True)
N.execute('''scene = { focusNone = function() end }
FrontEndAnarkEvents = { isGameEnding = function() return false end }
self = { getObjectReference = function() return {} end, goToSlide = function() end }''')
nm = N.eval('{}'); nm['GetKey'] = getkey; nm['GetNumber'] = getnum
N.globals().ExposedDataManager = nm
N.execute(nav_src); N.execute('self:onInitialize()')
L.execute('ANARK_EFFECT_EVENT_TYPE_LOST_FOCUS = "LOST"; ANARK_EFFECT_EVENT_TYPE_SELECTED = "SEL"')
def fe_back(nav_first):
    clk[0] += 1000; N.execute('self.targetSlide = nil')
    if nav_first: N.execute('self:gotoFrontEndMenuMain()')
    L.execute('self:handleBack()')
    if not nav_first: N.execute('self:gotoFrontEndMenuMain()')
    return N.eval('self.targetSlide')
here0 = st['here']
for nav_first in (False, True):
    st['open'] = 1; L.execute('self:onDeactivate(); self:onActivate()'); assert L.eval('self.pcMode') == True and L.eval('self.pcDirect') == True
    at(2); L.execute('self:handleSelect()'); assert L.eval('self.pcCat') == 2
    assert fe_back(nav_first) is None and L.eval('self.pcMode') == True and L.eval('self.pcCat') == 0, nav_first
    assert fe_back(nav_first) == 1 and L.eval('self.pcMode') == False, nav_first
    assert fe_back(nav_first) == 1, nav_first                    # plain Audio / Video: Back -> the main menu slide
    st['here'] = 1; at(2); L.execute('self:handleSelect()'); assert L.eval('self.pcMode') == True and L.eval('self.pcDirect') == False
    at(1); L.execute('self:handleSelect()'); assert L.eval('self.pcCat') == 1
    assert fe_back(nav_first) is None and L.eval('self.pcCat') == 0, nav_first
    assert fe_back(nav_first) is None and L.eval('self.pcMode') == False, nav_first   # volume-row way: the audio rows
    assert fe_back(nav_first) == 1, nav_first
    # a second Back within the pairing window still pairs with its own handleBack
    st['open'] = 1; L.execute('self:onDeactivate(); self:onActivate()'); at(1); L.execute('self:handleSelect()')
    clk[0] += 1000; N.execute('self.targetSlide = nil')
    for _ in range(2):
        clk[0] += 50
        if nav_first: N.execute('self:gotoFrontEndMenuMain()')
        L.execute('self:handleBack()')
        if not nav_first: N.execute('self:gotoFrontEndMenuMain()')
    assert N.eval('self.targetSlide') == 1 and L.eval('self.pcMode') == False, nav_first
st['here'] = here0
print('front end Back (kNav) sim OK')
print('front end Audio / Video sim OK')

# ---- windowHandler (main menu OPTIONS list): PC SETTINGS + EXTRAS (Movies / Credits) in the 4 buttons ----
def win_run(entry, cont=1, comps=0, movies=True, buttons=4):
    st['entry'] = entry; st['reports'] = []; st['open'] = 0
    W = LuaRuntime(unpack_returned_tuples=True)
    W.execute('''
calls = {}
table.getn = function(t) return #t end
ak = { output = function(s) end }
Event = { new = function(_, n) return n end }
AnarkEffectsConduitInterface = { EffectEventTriggered = function() end }
function SetLocalizedText(n, id) if type(id) == "string" then n.textString = id else n.textString = "LOC" .. id end end
CtlAnarkLoadMenuSelect = { getContinueLevel_new = function() return CONT end, getNumCompletes_new = function() return COMPS end,
  getHighestDifficultyBeaten = function() return 1 end }
CtlAnarkControlConfigSelect = setmetatable({}, {__index=function(t,k) return function() table.insert(calls, k) end end})
FrontEndAnarkEvents = { isMovieUnlocked = function() return MOVIES end, debug_areAllMoviesUnlocked = function() return false end }
LobbyAnarkEvents = { GetLocalizeTerritory = function() return 0 end }
SkillPoints = { isSuperhumanUnlocked = function() return 1 end }
scene = { mainMenuButtonRealm = 4, selectedCoop = 0 }
local function list() local l = { hasFocus = true, selectedItemIndex = 1, itemsGroup = { getNumChildren = function() return BUTTONS end },
  releaseFocus = function() table.insert(calls, "release") end, takeFocus = function() table.insert(calls, "take") end,
  listUpdater = { fireEvent = function(_, e) table.insert(calls, "event:" .. e) end }, parent = {} }
  l.resetList = function(s) table.insert(calls, "reset"); s.selectedItemIndex = 1 end
  l.setSelectedIndex = function(s, i) table.insert(calls, "select" .. i); s.selectedItemIndex = i end
  l.moveSelection = function(s, d) s.selectedItemIndex = s.selectedItemIndex + d end
  return l end
local refs = { listBehavior = list(), prevList = list(), nextList = list(), screenWindow = {}, numCompsValue = { parent = {} } }
self = { name = "windowHandler", _useOnActivateDeactivate = function() end, getObjectReference = function(_, n) return refs[n] end,
  fireEvent = function() end }
items = {}
for i = 1, BUTTONS do items[i] = { textString = "" } end
'''.replace('BUTTONS', str(buttons)).replace('CONT', str(cont)).replace('COMPS', str(comps)).replace('MOVIES', 'true' if movies else 'false'))
    m = W.eval('{}'); m['GetKey'] = getkey; m['GetNumber'] = getnum
    W.globals().ExposedDataManager = m
    W.execute(win_src)
    W.execute('self:onInitialize(); self:onUpdate()')
    return W
def win_fill(W):
    n = W.eval('self:getNumListItems()')
    assert n <= W.eval('#items'), 'more entries (%d) than buttons' % n
    W.execute('for i = 1, self:getNumListItems() do self:setListItemValue(i, items[i]) end')
    return [W.eval('items[%d].textString' % i) for i in range(1, n + 1)]
def win_select(W, i):
    W.execute('calls = {}; self.listBehavior.selectedItemIndex = %d; self:handleSelect()' % i)
    return list(W.eval('calls').values())
def win_back(W):
    W.execute('calls = {}; self:handleBack()'); return list(W.eval('calls').values())
TOP = ['LOC116', 'LOC117', 'PC SETTINGS', 'EXTRAS']
W = win_run(1)
t = win_fill(W); assert t == TOP, t
assert st['reports'] == ['ok'] and W.eval('self.MOVIES_INDEX') == 0
assert win_select(W, 3)[-1] == 'event:avOption' and st['open'] == 1
st['open'] = 0
assert win_select(W, 1)[-1] == 'event:avOption' and st['open'] == 2    # plain AUDIO / VIDEO says so
st['open'] = 0
assert win_select(W, 2)[-1] == 'event:controlConfig'
# EXTRAS: the same buttons show Movies, Credits; no event, the realm stays 4
c = win_select(W, 4); assert c == ['reset'] and W.eval('scene.mainMenuButtonRealm') == 4, c
assert win_fill(W) == ['LOC118', 'LOC119'] and W.eval('self.MOVIES_INDEX') == 1
# Back there -> OPTIONS again, EXTRAS highlighted (not the left list)
assert win_back(W) == ['reset', 'select4'] and win_fill(W) == TOP and W.eval('self.listBehavior.selectedItemIndex') == 4
# Movies / Credits fire their own events; OPTIONS is restored first (so the list is right on return)
win_select(W, 4); c = win_select(W, 1); assert c[-1] == 'event:movies' and 'reset' in c and win_fill(W) == TOP, c
win_select(W, 4); c = win_select(W, 2); assert c[-1] == 'event:credits' and win_fill(W) == TOP and st['open'] == 0, c
# Back on OPTIONS -> the left list (as the original)
assert win_back(W) == ['release', 'take']
# reactivated while EXTRAS was open: OPTIONS again
win_select(W, 4); W.execute('self:onActivate()'); assert win_fill(W) == TOP
W.execute('scene.mainMenuButtonRealm = 3'); assert win_fill(W) == ['LOC113', 'LOC114', 'LOC115'] and win_select(W, 2)[-1] == 'event:mpOffline'
W.execute('scene.mainMenuButtonRealm = 1'); assert win_fill(W) == ['LOC110', 'LOC111', 'LOC112'] and win_select(W, 3)[-1] == 'event:spLevelSelect'
# movies locked: Movies (in EXTRAS) is dimmed and skipped, the OPTIONS entries are not
W = win_run(1, movies=False); win_fill(W)
assert [W.eval('items[%d].opacity' % i) for i in range(1, 5)] == [60] * 4 and all(W.eval('self:isSelectable(%d)' % i) for i in range(1, 5))
win_select(W, 4); win_fill(W); W.execute('self:onUpdate()')
assert W.eval('items[1].opacity') == 30 and W.eval('self:isSelectable(1)') == False and W.eval('self.listBehavior.selectedItemIndex') == 2
# new game with no checkpoint: difficulty page, then the invert-Y prompt, then the new game
W = win_run(1, cont=0)
W.execute('scene.mainMenuButtonRealm = 1'); win_select(W, 1); assert W.eval('scene.mainMenuButtonRealm') == 5
assert win_fill(W) == ['LOC120', 'LOC121', 'LOC122', 'LOC123']
win_select(W, 2); assert W.eval('scene.mainMenuButtonRealm') == 6 and W.eval('scene.difficultyLevel') == 2
c = win_select(W, 4); assert c == ['onMenuInvertY', 'onMenuSaveConfig', 'event:spNewGame'], c
# no entry (R1_MENU_ROWS=2/0 never swap; PC_ENTRY 0): the original list
W = win_run(0); assert win_fill(W) == ['LOC116', 'LOC117', 'LOC118', 'LOC119'] and st['reports'] == []
assert W.eval('self.MOVIES_INDEX') == 3 and win_select(W, 3)[-1] == 'event:movies' and win_select(W, 4)[-1] == 'event:credits' and st['open'] == 0
# fewer buttons than the 4 entries: no entry, the reason is reported, the original list; a nil row is ignored
W = win_run(1, buttons=3); assert len(st['reports']) == 1 and st['reports'][0].startswith('fail:the list has 3 buttons'), st['reports']
assert W.eval('self.pcAt') is None and W.eval('self.MOVIES_INDEX') == 3 and win_select(W, 3)[-1] == 'event:movies'
W.execute('self:setListItemValue(5, nil); self:clearListItemValue(5, nil)')
print('front end main menu sim OK')

# ---- every script names its own menu (0 pause, 1 main): the native side keeps one entry state per menu ----
st['menus'] = []; L.execute('self:onSelectedItemChanged(2, items[2], true); self:handleSelect()'); assert st['menus'] == ['PC_HERE:1'], st['menus']
st['menus'] = []; L.execute('self.pcFE = nil; self:handleSelect()'); assert st['menus'] == ['PC_HERE:0'], st['menus']
st['menus'] = []; mmu_run(1); assert st['menus'] == ['PC_ENTRY:0', 'report:0'], st['menus']
st['menus'] = []; win_run(1); assert st['menus'] == ['PC_ENTRY:1', 'report:1'], st['menus']
print('menu ids OK')

# ---- the real way in: Options > PC SETTINGS (slot 3) -> goToSlide(2) -> the Audio / Video slide's lifecycle ----
# The slide's ScrollableList (pause.ig) fills its rows itself on activation (resetList / updateAllItems ->
# listUpdater:setListItemValue(i, itemsGroup:getChildByIndex(i))); its order against the OptionHandler's own
# onInitialize / onActivate is the engine's. On the first visit the handler is initialised during the activation,
# so its onActivate is not called for it (the old script then showed plain Audio / Video). Every order must open
# the PC SETTINGS page: the category list in the slide's own rows, retitled, Back restoring the slide.
AV_TITLE = 'AUDIO / VISUAL OPTIONS'      # what the title's localization_tag draws
PC_TITLE = 'PC SETTINGS'
STEP = {'init': 'handler:onInitialize()', 'act': 'handler:onActivate()', 'list': 'listActivate()',
        'upd': 'if handler.onUpdate then handler:onUpdate() end'}
def new_slide(lsrc, prefix='', no_attr=False):
    """The texts as the game draws them (EBOOT func_000EFEBC): allow_text_on_element set -> textString;
    else a localization_tag -> the string table's text (textString ignored); else textString.
    no_attr: set_customproperty has no effect (the SetLocalizedText fallback)."""
    T = LuaRuntime(unpack_returned_tuples=True)
    T.execute('''
calls = {}
CtlAnarkAVOptionsSelect = setmetatable({}, {__index=function(t,k) return function(_, a) table.insert(calls, k .. tostring(a)) end end})
AnarkEffectsConduitInterface = { EffectEventTriggered = function() end }
LOCTAB = { L1 = "SUBTITLES:", L2 = "MUSIC VOLUME:", L3 = "EFFECTS VOLUME:", L4 = "SPEECH VOLUME:", L5 = "HELP TEXT:",
  AVT = "AUDIO / VISUAL OPTIONS", OFFT = "OFF" }
function shown(n)
  local a = n.attrs
  if a.allow_text_on_element == 1 then return n.textString end
  local t = a.localization_tag
  if t and t ~= 0 then return LOCTAB[t] or ("LOC" .. t) end
  return n.textString
end
function SetLocalizedText(n, id)
  if type(id) == "string" then n.textString = id; n.attrs.localization_tag = 0; n.attrs.allow_text_on_element = 1
  else n.attrs.localization_tag = id; n.attrs.allow_text_on_element = 0 end
  n.attrs.element_text_okay = 0
end
function SetDelayAnarkRender() end
scene = { goToSlide = function(_, n) table.insert(calls, "goToSlide" .. n) end }
local function node(name, kids, tag) local n = { name = name, kids = kids or {}, attrs = { localization_tag = tag } }
  n.getNumChildren = function(s) return #s.kids end
  n.getChildByIndex = function(s, i) return s.kids[i] end
  n.get_customproperty = function(s, k) return s.attrs[k] end
  n.set_customproperty = function(s, k, v) if not NO_ATTR then s.attrs[k] = tonumber(v) or v end end
  for _, k in ipairs(n.kids) do k.parent = n end
  return n end
local labs = { "SUBTITLES:", "MUSIC VOLUME:", "EFFECTS VOLUME:", "SPEECH VOLUME:", "HELP TEXT:" }
rows = {}
-- as in frontend.ig / pause.ig: SUBTITLES / HELP have a tagged Value text ("OFF"), the volume rows a Value group
-- of ten untagged texts "0".."9" (right-aligned like the labels' column; the last one at x = 367); the labels
-- and the title are tagged
for i = 1, 5 do
  local v
  if i == 1 or i == 5 then v = node("Value", nil, "OFFT"); v.textString = "OFF"
  else
    local d = {}
    for j = 1, 10 do d[j] = node(tostring(j - 1)); d[j].textString = tostring(j - 1); d[j].opacity = 100 end
    v = node("Value", d)
  end
  rows[i] = node("Item" .. i, { node("Label", nil, "L" .. i), v }); rows[i].Label = rows[i].kids[1]; rows[i].Value = v
  rows[i].Label.textString = labs[i]; rows[i].Value.opacity = 100 end
optionsList = node("AVOptionsList", rows)
title = node("Title", nil, "AVT")
slide = node("AVSlide", { node("Background"), node("Header", { title }), optionsList })
handler = { _useOnActivateDeactivate = function() end, getObjectReference = function(_, n) if n == "optionsList" then return optionsList end end }
self = handler
function listActivate()
  if not handler.avOptionsData then return end
  for i = 1, optionsList:getNumChildren() do handler:setListItemValue(i, optionsList:getChildByIndex(i)) end
end
'''.replace('NO_ATTR', 'true' if no_attr else 'false'))
    T.globals().title.textString = '\\\\OPTIONS\\AUDIO VISUAL'
    m = T.eval('{}'); m['GetKey'] = getkey; m['GetNumber'] = getnum
    m['GetCharString'] = T.eval('function(_, k) return { m_char_string = _slot(k) } end')
    T.globals()._slot = lambda k: slots.get(int(k))
    T.globals().ExposedDataManager = m
    T.execute(prefix + lsrc)
    return T
def run(T, steps):
    for x in steps: T.execute(STEP[x])
def rlabels(T): return [T.eval('shown(rows[%d].Label)' % i) for i in range(1, 6)]
def rvals(T):
    """the right column: the Value text, or the last text of a Value group"""
    return [T.eval('shown(rows[%d].Value.kids[10] or rows[%d].Value)' % (i, i)) for i in range(1, 6)]
def digits(T, i): return [T.eval('shown(rows[%d].Value.kids[%d])' % (i, j)) for j in range(1, 11)]
SCATS = ['PC DISPLAY', 'PC GRAPHICS', 'PC CONTROLS & HUD', '', '']
FIRST = {'first visit, list fills after init': ['init', 'list', 'upd'],
         'first visit, list fills before init': ['list', 'init', 'upd'],
         'first visit, no row callbacks': ['init', 'upd'],
         'first visit, activation after the first update': ['init', 'upd', 'act'],
         'first visit, list, update, activation': ['init', 'list', 'upd', 'act'],
         'first visit, list first, activation last': ['list', 'init', 'upd', 'act'],
         'first visit, update, list, activation': ['init', 'upd', 'list', 'upd', 'act', 'upd']}
REVISIT = {'revisit, handler first': ['act', 'list', 'upd'], 'revisit, list first': ['list', 'act', 'upd'],
           'revisit, no row callbacks': ['act', 'upd']}
def slide_cases(lsrc, prefix=''):
    """yields (case, T) with the slide just entered through PC SETTINGS"""
    for name, steps in FIRST.items():
        st['open'] = 0; T = new_slide(lsrc, prefix)
        getkey(None, 'PC_OPEN:1')            # PC SETTINGS selected (MainMenuUpdater / windowHandler), goToSlide(2)
        run(T, steps); yield name, T
    for name, steps in REVISIT.items():
        st['open'] = 0; T = new_slide(lsrc, prefix)
        getkey(None, 'PC_OPEN:0')
        run(T, ['init', 'list', 'upd']); T.execute('handler:handleBack(); if handler.onDeactivate then handler:onDeactivate() end')   # an earlier plain Audio / Video visit
        assert T.eval('handler.pcMode') == False and rlabels(T)[1] == 'MUSIC VOLUME:'
        getkey(None, 'PC_OPEN:1')
        run(T, steps); yield name, T
n_cases = 0
for pre, who in (('', 'pause menu'), (fe_prefix, 'main menu')):
    for name, T in slide_cases(lua_src, pre):
        n_cases += 1
        lab = rlabels(T)
        assert T.eval('handler.pcMode') == True and lab == SCATS and st['show'] == 1, (who, name, lab)
        assert T.eval('shown(title)') == PC_TITLE, (who, name, T.eval('shown(title)'))
        # the value column is shown, not hidden: the categories read OPEN, a volume row's other digits blank
        assert [T.eval('rows[%d].Value.opacity' % i) for i in range(1, 6)] == [100] * 5, (who, name)
        assert rvals(T) == ['OPEN'] * 3 + ['', ''] and digits(T, 2) == [''] * 9 + ['OPEN'], (who, name, rvals(T))
        T.execute('handler:onSelectedItemChanged(1, rows[1], false); handler:handleSelect()')
        assert T.eval('shown(title)') == 'PC DISPLAY' and rlabels(T)[4] == 'NEXT PAGE (1/2)', (who, name)
        assert rlabels(T)[:4] == ['FRAME RATE LIMIT', 'OPT0', 'OPT1', 'OPT2'], (who, name, rlabels(T))
        assert rvals(T) == [rows[0][1][sel[0]], 'A', 'A', 'A', ''] and digits(T, 3) == [''] * 9 + ['A'], (who, name, rvals(T))
        T.execute('handler:onSelectedItemChanged(2, rows[2], false); handler:onRight()')
        assert rvals(T)[1] == 'B', rvals(T)
        T.execute('handler:onLeft()'); assert rvals(T)[1] == 'A', rvals(T)
        T.execute('rows[3].Value.kids[10].opacity = 0; handler:onUpdate()')    # dimmed by the volume code: shown again
        assert T.eval('rows[3].Value.kids[10].opacity') == 100
        T.execute('handler:handleBack()'); assert T.eval('shown(title)') == PC_TITLE and rlabels(T) == SCATS
        T.execute('handler:handleBack()')     # Back -> the Options list; the slide's own title and rows again
        assert T.eval('handler.pcMode') == False and T.eval('shown(title)') == AV_TITLE and st['show'] == 0, (who, name)
        assert rlabels(T)[1] == 'MUSIC VOLUME:' and T.eval('rows[2].Value.opacity') == 100
        assert digits(T, 2) == [str(j) for j in range(10)] and digits(T, 4) == [str(j) for j in range(10)], (who, name, digits(T, 2))
        assert T.eval('shown(rows[1].Value)') in ('OFF', 'LOC79', 'LOC101'), T.eval('shown(rows[1].Value)')
        assert list(T.eval('calls').values())[-1] in ('goToSlide1', 'onMenuSaveConfignil'), list(T.eval('calls').values())
        # the texts draw from their tags again: tags untouched, allow_text_on_element back off
        assert [T.eval('rows[%d].Label.attrs.localization_tag' % i) for i in range(1, 6)] == ['L%d' % i for i in range(1, 6)]
        assert [T.eval('rows[%d].Label.attrs.allow_text_on_element' % i) for i in range(1, 6)] == [0] * 5, (who, name)
        assert T.eval('title.attrs.localization_tag') == 'AVT' and T.eval('title.attrs.allow_text_on_element') == 0
# set_customproperty without effect: a tagged title / label keeps the game's text (its tag is never cleared: a
# float Lua number cannot carry it back); the values still show (untagged digits; the On / Off texts through
# SetLocalizedText(obj, "string"), re-tagged by setListItemValue on leaving)
GAME_LABS = ['SUBTITLES:', 'MUSIC VOLUME:', 'EFFECTS VOLUME:', 'SPEECH VOLUME:', 'HELP TEXT:']
st['open'] = 0; T = new_slide(lua_src, no_attr=True); getkey(None, 'PC_OPEN:1'); run(T, ['init', 'list', 'upd'])
assert rlabels(T) == GAME_LABS and T.eval('shown(title)') == AV_TITLE and rvals(T) == ['OPEN'] * 3 + ['', ''], (rlabels(T), rvals(T))
T.execute('handler:onSelectedItemChanged(1, rows[1], false); handler:handleSelect()')
assert rvals(T)[0] == rows[0][1][sel[0]] and T.eval('handler.pcHow') == 'game text kept (tagged, allow_text_on_element not settable)'
st['diag'] = []; T.execute('handler:handleBack()'); T.execute('handler:handleBack()')
assert T.eval('handler.pcMode') == False and rlabels(T) == GAME_LABS and T.eval('shown(title)') == AV_TITLE
assert [T.eval('rows[%d].Label.attrs.localization_tag' % i) for i in range(1, 6)] == ['L%d' % i for i in range(1, 6)]
assert T.eval('title.attrs.localization_tag') == 'AVT' and T.eval('shown(rows[1].Value)') == 'LOC79' and T.eval('shown(rows[5].Value)') == 'LOC80'
print('localized text fallback OK')
# AUDIO / VIDEO (slot 1, no PC_OPEN) stays the original screen in every order
for name, steps in list(FIRST.items()):
    st['open'] = 0; T = new_slide(lua_src); getkey(None, 'PC_OPEN:0'); run(T, steps)
    assert T.eval('handler.pcMode') == False and T.eval('shown(title)') == AV_TITLE, name
    if steps[0] == 'init' and 'list' in steps: assert rlabels(T)[1] == 'MUSIC VOLUME:' and T.eval('shown(rows[1].Value)') == 'LOC79', name
# the page stays open: more frames, a second activation callback of the same visit
for name, T in slide_cases(lua_src):
    run(T, ['upd', 'upd', 'act', 'upd']); assert T.eval('handler.pcMode') == True and rlabels(T) == SCATS, name
# left without Back (the pause menu closed: onDeactivate), then plain AUDIO / VIDEO -> the original page;
# a revisit with no request at all also ends PC mode; PC SETTINGS again -> the categories
for req, steps in (('PC_OPEN:0', ['act', 'list', 'upd']), (None, ['act', 'upd']), ('PC_OPEN:0', ['list', 'upd', 'act'])):
    st['open'] = 0; T = new_slide(lua_src); getkey(None, 'PC_OPEN:1'); run(T, ['init', 'list', 'upd'])
    T.execute('handler:onSelectedItemChanged(1, rows[1], false); handler:handleSelect(); handler:onDeactivate()')
    if req: getkey(None, req)
    run(T, steps)
    assert T.eval('handler.pcMode') == False and T.eval('shown(title)') == AV_TITLE and rlabels(T)[1] == 'MUSIC VOLUME:', (req, steps)
    assert T.eval('rows[2].Value.opacity') == 100 and st['show'] == 0
    T.execute('handler:onDeactivate()'); getkey(None, 'PC_OPEN:1'); run(T, ['upd', 'act'])
    assert T.eval('handler.pcMode') == True and rlabels(T) == SCATS and T.eval('handler.pcCat') == 0
# PC_OPEN noticed only by onUpdate (no activation callback at all on this visit)
st['open'] = 0; T = new_slide(lua_src); getkey(None, 'PC_OPEN:0'); run(T, ['init', 'list', 'upd'])
T.execute('handler:handleBack()'); getkey(None, 'PC_OPEN:1'); run(T, ['upd'])
assert T.eval('handler.pcMode') == True and rlabels(T) == SCATS
# the diagnostics: what was painted and the input in PC mode reach the log
st['diag'] = []; T.execute('handler:handleBack()'); getkey(None, 'PC_OPEN:1'); run(T, ['act'])
assert st['diag'] == ['back (category 0, row 1)', 'painted: 5 rows, 5 value columns, title %s, first row PC DISPLAY = OPEN, drawn by allow_text_on_element (category 0, row 1)' % PC_TITLE], st['diag']
st['diag'] = []; T.execute('handler:onSelectedItemChanged(2, rows[2], false); handler:onRight()')
assert st['diag'][:2] == ['move (category 0, row 2)', 'right (category 0, row 2)'], st['diag']
assert st['diag'][2].startswith('category 2: 5 rows, 5 value columns, title PC GRAPHICS, first row ANTI-ALIASING = '), st['diag']
# a row with no Value text at all (another layout): the old "NAME: VALUE" label, the empty Value hidden
st['open'] = 0; T = new_slide(lua_src)
T.execute('for i = 1, 5 do rows[i].Value.textString = nil; rows[i].Value.kids = {} end')
getkey(None, 'PC_OPEN:1'); run(T, ['init', 'list', 'upd'])
T.execute('handler:onSelectedItemChanged(1, rows[1], false); handler:handleSelect()')
assert rlabels(T)[0].startswith('FRAME RATE LIMIT: ') and T.eval('rows[1].Value.opacity') == 0, rlabels(T)
T.execute('handler:handleBack(); handler:handleBack()'); assert T.eval('rows[1].Value.opacity') == 100
# the script before this fix (rows only from setListItemValue, PC_OPEN only in onActivate) fails the first visit
old = os.environ.get('MENUROWS_OLD_SRC')
if old:
    o = open(old, encoding='utf-8').read()
    old_lua = re.search(r'kLua\[\] = R"LUA\((.*?)\)LUA"', o, re.S).group(1)
    bad = [n for n, T in slide_cases(old_lua) if not (T.eval('self.pcMode') == True and rlabels(T) == SCATS and T.eval('shown(title)') == PC_TITLE)]
    print('old script fails:', bad); assert bad
print('slide transition sim OK (%d cases)' % n_cases)

# ---- no overlay on these paths: nothing is drawn over the game's menus (the old hint box is gone) ----
root = os.path.join(here, '..', '..')
for rel in (('project', 'rfom_menurows.cpp'), ('project', 'rfom_pcextra.cpp'),
            ('ps3recomp', 'libs', 'video', 'rsx_pc_menu.inc'), ('ps3recomp', 'libs', 'video', 'rsx_d3d12_backend.c')):
    t = open(os.path.join(root, *rel), encoding='utf-8', errors='replace').read()
    for bad in ('menu_hint', 'pm_hint', 'RFOM_MENU_PC_HINT', 'PC menu:  F10'):
        assert bad not in t, '%s still has %s' % (rel[-1], bad)
print('no overlay OK')
