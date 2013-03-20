/******************************************************************************
/ SnM_RegionPlaylist.cpp
/
/ Copyright (c) 2012-2013 Jeffos
/ http://www.standingwaterstudios.com/reaper
/
/ Permission is hereby granted, free of charge, to any person obtaining a copy
/ of this software and associated documentation files (the "Software"), to deal
/ in the Software without restriction, including without limitation the rights to
/ use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
/ of the Software, and to permit persons to whom the Software is furnished to
/ do so, subject to the following conditions:
/ 
/ The above copyright notice and this permission notice shall be included in all
/ copies or substantial portions of the Software.
/ 
/ THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
/ EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
/ OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
/ NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
/ HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
/ WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
/ FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
/ OTHER DEALINGS IN THE SOFTWARE.
/
******************************************************************************/

#include "stdafx.h"
#include "SnM.h"
#include "../Prompt.h"
#include "../reaper/localize.h"


#define MUTEX_PLAYLISTS			SWS_SectionLock lock(&g_plsMutex) // for quick un-mutexing

#define DELETE_MSG				0xF000
#define PERFORM_MSG				0xF001
#define CROP_PRJ_MSG			0xF002
#define NEW_PLAYLIST_MSG		0xF003
#define COPY_PLAYLIST_MSG		0xF004
#define DEL_PLAYLIST_MSG		0xF005
#define REN_PLAYLIST_MSG		0xF006
#define ADD_ALL_REGIONS_MSG		0xF007
#define CROP_PRJTAB_MSG			0xF008
#define APPEND_PRJ_MSG			0xF009
#define PASTE_CURSOR_MSG		0xF00A
#define APPEND_SEL_RGN_MSG		0xF00B
#define PASTE_SEL_RGN_MSG		0xF00C
#define TGL_INFINITE_LOOP_MSG	0xF00D
#define TGL_SCROLL_VIEW_MSG		0xF00E
#define TGL_SEEK_PLAY_MSG		0xF00F
#define ADD_REGION_START_MSG	0xF100
#define ADD_REGION_END_MSG		0xF7FF // => 1791 marker/region *indexes* supported, oh well..
#define INSERT_REGION_START_MSG	0xF800
#define INSERT_REGION_END_MSG	0xFEFF // => 1791 marker/region indexes supported

#define UNDO_PLAYLIST_STR		__LOCALIZE("Region Playlist edition", "sws_undo")


enum {
  BTNID_LOCK=2000, //JFB would be great to have _APS_NEXT_CONTROL_VALUE *always* defined
  BTNID_PLAY,
  BTNID_STOP,
  BTNID_REPEAT,
  TXTID_PLAYLIST,
  CMBID_PLAYLIST,
  WNDID_ADD_DEL,
  BTNID_NEW_PLAYLIST,
  BTNID_DEL_PLAYLIST,
  BTNID_PASTE,
  TXTID_MONITOR_PL,
  WNDID_MONITORS,
  TXTID_MON0,
  TXTID_MON1,
  TXTID_MON2,
  TXTID_MON3,
  TXTID_MON4
};


SNM_RegionPlaylistWnd* g_pRgnPlaylistWnd = NULL;
SWSProjConfig<SNM_Playlists> g_pls;
SWS_Mutex g_plsMutex;

// user prefs
char g_rgnplBigFontName[64] = SNM_DYN_FONT_NAME;
bool g_monitorMode = false;
bool g_repeatPlaylist = false;	// playlist repeat state
bool g_scrollView = true;
bool g_seekPlay = true;

// see PlaylistRun()
int g_playPlaylist = -1;		// -1: stopped, playlist id otherwise
bool g_unsync = false;			// true when switching to a position that is not part of the playlist
int g_playCur = -1;				// index of the item being played, -1 means "not playing yet"
int g_playNext = -1;			// index of the next item to be played, -1 means "the end"
int g_rgnLoop = 0;				// region loop count: 0 not looping, <0 infinite loop, n>0 looping n times
bool g_plLoop = false;			// other (corner case) loops in the playlist?
double g_lastRunPos = -1.0;
double g_nextRgnPos, g_nextRgnEnd;
double g_curRgnPos = 0.0, g_curRgnEnd = -1.0; // to detect unsync, end<pos means non relevant

int g_oldSeekPref = -1;
int g_oldStopprojlenPref = -1;
int g_oldRepeatState = -1;


SNM_Playlist* GetPlaylist(int _plId = -1) //-1 for the current (i.e. edited) playlist
{
	MUTEX_PLAYLISTS;
	if (_plId < 0) _plId = g_pls.Get()->m_editId;
	return g_pls.Get()->Get(_plId);
}


///////////////////////////////////////////////////////////////////////////////
// SNM_Playlist
///////////////////////////////////////////////////////////////////////////////

SNM_Playlist::SNM_Playlist(SNM_Playlist* _pl, const char* _name)
	: m_name(_name), WDL_PtrList<SNM_PlaylistItem>()
{
	if (_pl)
	{
		for (int i=0; i<_pl->GetSize(); i++)
			Add(new SNM_PlaylistItem(_pl->Get(i)->m_rgnId, _pl->Get(i)->m_cnt));
		if (!_name)
			m_name.Set(_pl->m_name.Get());
	}
}

bool SNM_Playlist::IsValidIem(int _i)
{
	if (SNM_PlaylistItem* item = Get(_i))
		return item->IsValidIem();
	return false;
}

// return the first found playlist idx for _pos
int SNM_Playlist::IsInPlaylist(double _pos, bool _repeat, int _startWith)
{
	double rgnpos, rgnend;
	for (int i=_startWith; i<GetSize(); i++)
		if (SNM_PlaylistItem* plItem = Get(i))
			if (plItem->m_rgnId>0 && plItem->m_cnt!=0 && EnumMarkerRegionById(NULL, plItem->m_rgnId, NULL, &rgnpos, &rgnend, NULL, NULL, NULL))
				if (_pos >= rgnpos && _pos <= rgnend)
					return i;
	// 2nd try
	if (_repeat)
		for (int i=0; i<_startWith; i++)
			if (SNM_PlaylistItem* plItem = Get(i))
				if (plItem->m_rgnId>0 && plItem->m_cnt!=0 && EnumMarkerRegionById(NULL, plItem->m_rgnId, NULL, &rgnpos, &rgnend, NULL, NULL, NULL))
					if (_pos >= rgnpos && _pos <= rgnend)
						return i;
	return -1;
}

int SNM_Playlist::IsInfinite()
{
	int num;
	for (int i=0; i<GetSize(); i++)
		if (SNM_PlaylistItem* plItem = Get(i))
			if (plItem->m_rgnId>0 && plItem->m_cnt<0 && EnumMarkerRegionById(NULL, plItem->m_rgnId, NULL, NULL, NULL, NULL, &num, NULL))
				return num;
	return -1;
}

// returns playlist length is seconds, with a negative value if it contains infinite loops
double SNM_Playlist::GetLength()
{
	bool infinite = false;
	double length=0.0;
	for (int i=0; i<GetSize(); i++)
	{
		double rgnpos, rgnend;
		if (SNM_PlaylistItem* plItem = Get(i))
		{
			if (plItem->m_rgnId>0 && plItem->m_cnt!=0 && EnumMarkerRegionById(NULL, plItem->m_rgnId, NULL, &rgnpos, &rgnend, NULL, NULL, NULL)) {
				infinite |= plItem->m_cnt<0;
				length += ((rgnend-rgnpos) * abs(plItem->m_cnt));
			}
		}
	}
	return infinite ? length*(-1) : length;
}

// get the 1st marker/region num which has nested marker/region
int SNM_Playlist::GetNestedMarkerRegion()
{
	for (int i=0; i<GetSize(); i++)
	{
		if (SNM_PlaylistItem* plItem = Get(i))
		{
			double rgnpos, rgnend;
			int num, rgnidx = EnumMarkerRegionById(NULL, plItem->m_rgnId, NULL, &rgnpos, &rgnend, NULL, &num, NULL);
			if (rgnidx>0)
			{
				int x=0, lastx=0; double dPos, dEnd; bool isRgn;
				while (x = EnumProjectMarkers2(NULL, x, &isRgn, &dPos, &dEnd, NULL, NULL))
				{
					if (rgnidx != lastx)
					{
						if (isRgn) {
							if ((dPos > rgnpos && dPos < rgnend) || (dEnd > rgnpos && dEnd < rgnend))
								return num;
						}
						else {
							if (dPos > rgnpos && dPos < rgnend)
								return num;
						}
					}
					lastx=x;
				}
			}
		}
	}
	return -1;
}

// get the 1st marker/region num which has a marker/region > _pos
int SNM_Playlist::GetGreaterMarkerRegion(double _pos)
{
	for (int i=0; i<GetSize(); i++)
		if (SNM_PlaylistItem* plItem = Get(i))
		{
			double rgnpos; int num;
			if (EnumMarkerRegionById(NULL, plItem->m_rgnId, NULL, &rgnpos, NULL, NULL, &num, NULL) && rgnpos>_pos)
				return num;
		}
	return -1;
}


///////////////////////////////////////////////////////////////////////////////
// SNM_PlaylistView
///////////////////////////////////////////////////////////////////////////////

enum {
  COL_RGN=0,
  COL_RGN_NAME,
  COL_RGN_COUNT,
  COL_RGN_START,
  COL_RGN_END,
  COL_RGN_LEN,
  COL_COUNT
};

// !WANT_LOCALIZE_STRINGS_BEGIN:sws_DLG_165
static SWS_LVColumn g_playlistCols[] = { {50,2,"#"}, {150,1,"Name"}, {70,1,"Loop count"}, {50,2,"Start"}, {50,2,"End"}, {50,2,"Length"} };
// !WANT_LOCALIZE_STRINGS_END

SNM_PlaylistView::SNM_PlaylistView(HWND hwndList, HWND hwndEdit)
	: SWS_ListView(hwndList, hwndEdit, COL_COUNT, g_playlistCols, "RgnPlaylistViewState", false, "sws_DLG_165", false)
{
}

void SNM_PlaylistView::Update()
{
	MUTEX_PLAYLISTS;
	SWS_ListView::Update();
}

// "compact" the playlist (e.g. 2 consecutive regions "3" are merged into one, its counter is incremented)
void SNM_PlaylistView::UpdateCompact()
{
	MUTEX_PLAYLISTS;
	if (SNM_Playlist* pl = GetPlaylist())
		for (int i=pl->GetSize()-1; i>=0 ; i--)
			if (SNM_PlaylistItem* item = pl->Get(i))
				if ((i-1)>=0 && pl->Get(i-1) && item->m_rgnId == pl->Get(i-1)->m_rgnId)
				{
					bool infinite = (pl->Get(i-1)->m_cnt<0 || item->m_cnt<0);
					pl->Get(i-1)->m_cnt = abs(pl->Get(i-1)->m_cnt) + abs(item->m_cnt);
					if (infinite)
						pl->Get(i-1)->m_cnt *= (-1);
					pl->Delete(i, true);
				}
	Update();
}

void SNM_PlaylistView::GetItemText(SWS_ListItem* item, int iCol, char* str, int iStrMax)
{
	MUTEX_PLAYLISTS;
	if (str) *str = '\0';
	if (SNM_PlaylistItem* pItem = (SNM_PlaylistItem*)item)
	{
		switch (iCol)
		{
			case COL_RGN: {
				SNM_Playlist* curpl = GetPlaylist();
				_snprintfSafe(str, iStrMax, "%s %d", 
					curpl && g_playPlaylist>=0 && curpl==GetPlaylist(g_playPlaylist) ? // current playlist being played?
					(!g_unsync && curpl->Get(g_playCur)==pItem ? UTF8_BULLET : (curpl->Get(g_playNext)==pItem ? UTF8_CIRCLE : " ")) : " ", GetMarkerRegionNumFromId(pItem->m_rgnId));
				break;
			}
			case COL_RGN_NAME:
				if (!EnumMarkerRegionDescById(NULL, pItem->m_rgnId, str, iStrMax, SNM_REGION_MASK, false, true, false) /* && *str */)
					lstrcpyn(str, __LOCALIZE("Unknown region","sws_DLG_165"), iStrMax);
				break;
			case COL_RGN_COUNT:
				if (pItem->m_cnt < 0)
					_snprintfSafe(str, iStrMax, "%s", UTF8_INFINITY);
				else
					_snprintfSafe(str, iStrMax, "%d", pItem->m_cnt);
				break;
			case COL_RGN_START: {
				double pos;
				if (EnumMarkerRegionById(NULL, pItem->m_rgnId, NULL, &pos, NULL, NULL, NULL, NULL))
					format_timestr_pos(pos, str, iStrMax, -1);
				break;
			}
			case COL_RGN_END: {
				double end;
				if (EnumMarkerRegionById(NULL, pItem->m_rgnId, NULL, NULL, &end, NULL, NULL, NULL))
					format_timestr_pos(end, str, iStrMax, -1);
				break;
			}
			case COL_RGN_LEN: {
				double pos, end;
				if (EnumMarkerRegionById(NULL, pItem->m_rgnId, NULL, &pos, &end, NULL, NULL, NULL))
					format_timestr_len(end-pos, str, iStrMax, pos, -1);
				break;
			}
		}
	}
}

void SNM_PlaylistView::GetItemList(SWS_ListItemList* pList)
{
	MUTEX_PLAYLISTS;
	if (SNM_Playlist* pl = GetPlaylist())
		for (int i=0; i < pl->GetSize(); i++)
			if (SWS_ListItem* item = (SWS_ListItem*)pl->Get(i))
				pList->Add(item);
}

void SNM_PlaylistView::SetItemText(SWS_ListItem* item, int iCol, const char* str)
{
	MUTEX_PLAYLISTS;
	if (SNM_PlaylistItem* pItem = (SNM_PlaylistItem*)item)
	{
		switch (iCol)
		{
			case COL_RGN_COUNT:
				pItem->m_cnt = str && *str ? atoi(str) : 0;
				Undo_OnStateChangeEx2(NULL, UNDO_PLAYLIST_STR, UNDO_STATE_MISCCFG, -1); 
				PlaylistResync();
				break;
			case COL_RGN_NAME:
			{
				bool isrgn; double pos, end; int num, col;
				if (str && *str && EnumMarkerRegionById(NULL, pItem->m_rgnId, &isrgn, &pos, &end, NULL, &num, &col))
					SNM_SetProjectMarker(NULL, num, isrgn, pos, end, str, col ? col | 0x1000000 : 0); // update notified back through PlaylistMarkerRegionSubscriber
				break;
			}
		}
	}
}

void SNM_PlaylistView::OnItemClk(SWS_ListItem* item, int iCol, int iKeyState) {
	if (SNM_PlaylistItem* pItem = (SNM_PlaylistItem*)item)
		SetEditCurPos2(NULL, pItem->GetPos(), g_scrollView, false);
}

void SNM_PlaylistView::OnItemDblClk(SWS_ListItem* item, int iCol) {
	if (g_pRgnPlaylistWnd)
		g_pRgnPlaylistWnd->OnCommand(PERFORM_MSG, 0);
}

// "disable" sort
int SNM_PlaylistView::OnItemSort(SWS_ListItem* _item1, SWS_ListItem* _item2) 
{
	MUTEX_PLAYLISTS;
	int i1=-1, i2=-1;
	if (SNM_Playlist* pl = GetPlaylist())
	{
		for (int i=0; (i1<0 && i2<0) || i<pl->GetSize(); i++)
		{
			SWS_ListItem* item = (SWS_ListItem*)pl->Get(i);
			if (item == _item1) i1=i;
			if (item == _item2) i2=i;
		}
		if (i1 >= 0 && i2 >= 0) {
			if (i1 > i2) return 1;
			else if (i1 < i2) return -1;
		}
	}
	return 0;
}

void SNM_PlaylistView::OnBeginDrag(SWS_ListItem* item) {
	SetCapture(GetParent(m_hwndList));
}

void SNM_PlaylistView::OnDrag()
{
	MUTEX_PLAYLISTS;
	SNM_Playlist* pl  = GetPlaylist();
	if (!pl) return;

	POINT p; GetCursorPos(&p);
	if (SNM_PlaylistItem* hitItem = (SNM_PlaylistItem*)GetHitItem(p.x, p.y, NULL))
	{
		int iNewPriority = pl->Find(hitItem);
		int x=0, iSelPriority;
		while(SNM_PlaylistItem* selItem = (SNM_PlaylistItem*)EnumSelected(&x))
		{
			iSelPriority = pl->Find(selItem);
			if (iNewPriority == iSelPriority) return;
			m_draggedItems.Add(selItem);
		}

		bool bDir = iNewPriority > iSelPriority;
		for (int i = bDir ? 0 : m_draggedItems.GetSize()-1; bDir ? i < m_draggedItems.GetSize() : i >= 0; bDir ? i++ : i--) {
			pl->Delete(pl->Find(m_draggedItems.Get(i)), false);
			pl->Insert(iNewPriority, m_draggedItems.Get(i));
		}

		ListView_DeleteAllItems(m_hwndList); // because of the special sort criteria ("not sortable" somehow)
		Update(); // no UpdateCompact() here, it would crash! see OnEndDrag()

		for (int i=0; i < m_draggedItems.GetSize(); i++)
			SelectByItem((SWS_ListItem*)m_draggedItems.Get(i), i==0, i==0);
	}
}

void SNM_PlaylistView::OnEndDrag()
{
	MUTEX_PLAYLISTS;
	UpdateCompact();
	if (m_draggedItems.GetSize()) {
		Undo_OnStateChangeEx2(NULL, UNDO_PLAYLIST_STR, UNDO_STATE_MISCCFG, -1);
		m_draggedItems.Empty(false);
		PlaylistResync();
	}
}


///////////////////////////////////////////////////////////////////////////////
// SNM_RegionPlaylistWnd
///////////////////////////////////////////////////////////////////////////////

SNM_RegionPlaylistWnd::SNM_RegionPlaylistWnd()
	: SWS_DockWnd(IDD_SNM_RGNPLAYLIST, __LOCALIZE("Region Playlist","sws_DLG_165"), "SnMRgnPlaylist", SWSGetCommandID(OpenRegionPlaylist))
{
	// must call SWS_DockWnd::Init() to restore parameters and open the window if necessary
	Init();
}

void SNM_RegionPlaylistWnd::OnInitDlg()
{
	m_resize.init_item(IDC_LIST, 0.0, 0.0, 1.0, 1.0);
	m_pLists.Add(new SNM_PlaylistView(GetDlgItem(m_hwnd, IDC_LIST), GetDlgItem(m_hwnd, IDC_EDIT)));

	m_vwnd_painter.SetGSC(WDL_STYLE_GetSysColor);
	m_parentVwnd.SetRealParent(m_hwnd);
	
	m_btnLock.SetID(BTNID_LOCK);
	m_parentVwnd.AddChild(&m_btnLock);

	m_btnPlay.SetID(BTNID_PLAY);
	m_parentVwnd.AddChild(&m_btnPlay);
	m_btnStop.SetID(BTNID_STOP);
	m_parentVwnd.AddChild(&m_btnStop);
	m_btnRepeat.SetID(BTNID_REPEAT);
	m_parentVwnd.AddChild(&m_btnRepeat);

	m_txtPlaylist.SetID(TXTID_PLAYLIST);
	m_parentVwnd.AddChild(&m_txtPlaylist);

	m_cbPlaylist.SetID(CMBID_PLAYLIST);
	FillPlaylistCombo();
	m_parentVwnd.AddChild(&m_cbPlaylist);

	m_btnAdd.SetID(BTNID_NEW_PLAYLIST);
	m_btnsAddDel.AddChild(&m_btnAdd);
	m_btnDel.SetID(BTNID_DEL_PLAYLIST);
	m_btnsAddDel.AddChild(&m_btnDel);
	m_btnsAddDel.SetID(WNDID_ADD_DEL);
	m_parentVwnd.AddChild(&m_btnsAddDel);

	m_btnCrop.SetID(BTNID_PASTE);
	m_parentVwnd.AddChild(&m_btnCrop);

	m_monPl.SetID(TXTID_MONITOR_PL);
	m_monPl.SetFontName(g_rgnplBigFontName);
	m_monPl.SetAlign(DT_LEFT, DT_CENTER);
	m_parentVwnd.AddChild(&m_monPl);

	for (int i=0; i<5; i++)
		m_txtMon[i].SetID(TXTID_MON0+i);
	m_mons.AddMonitors(&m_txtMon[0], &m_txtMon[1], &m_txtMon[2], &m_txtMon[3], &m_txtMon[4]);
	m_mons.SetID(WNDID_MONITORS);
	m_mons.SetFontName(g_rgnplBigFontName);
	m_mons.SetTitles(__LOCALIZE("CURRENT","sws_DLG_165"), " ", __LOCALIZE("NEXT","sws_DLG_165"), " ");  // " " trick to get a lane
	m_parentVwnd.AddChild(&m_mons);

	Update();

	RegisterToMarkerRegionUpdates(&m_mkrRgnSubscriber);
}

void SNM_RegionPlaylistWnd::OnDestroy() {
	UnregisterToMarkerRegionUpdates(&m_mkrRgnSubscriber);
	m_cbPlaylist.Empty();
	m_mons.RemoveAllChildren(false);
	m_btnsAddDel.RemoveAllChildren(false);
}

// ScheduledJob used because of multi-notifs
void SNM_RegionPlaylistWnd::CSurfSetTrackListChange() {
	AddOrReplaceScheduledJob(new PlaylistUpdateJob());
}

void SNM_RegionPlaylistWnd::Update(int _flags)
{
	MUTEX_PLAYLISTS;

	static bool bRecurseCheck = false;
	if (bRecurseCheck)
		return;
	bRecurseCheck = true;

	ShowWindow(GetDlgItem(m_hwnd, IDC_LIST), !g_monitorMode && g_pls.Get()->GetSize() ? SW_SHOW : SW_HIDE);

	if (!_flags)
	{
		if (m_pLists.GetSize())
			((SNM_PlaylistView*)m_pLists.Get(0))->UpdateCompact();

		UpdateMonitoring();
		m_parentVwnd.RequestRedraw(NULL);
	}
	// "fast" update (used while playing) - exclusive with the above!
	else if (g_playPlaylist>=0)
	{
		// monitoring mode
		if (g_monitorMode)
			UpdateMonitoring();
		// edition mode
		else if (g_pls.Get()->m_editId == g_playPlaylist && m_pLists.GetSize()) // is it the displayed playlist?
			((SNM_PlaylistView*)m_pLists.Get(0))->Update(); // no playlist compacting
	}

	bRecurseCheck = false;
}

// just update monitoring VWnds
void SNM_RegionPlaylistWnd::UpdateMonitoring()
{
	char bufPl[128]="", bufCur[128]="", bufCurNum[16]="", bufNext[128]="", bufNextNum[16]="";
	if (g_playPlaylist>=0)
	{
		if (SNM_Playlist* curpl = GetPlaylist(g_playPlaylist))
		{
			// *** playlist info ***
			_snprintfSafe(bufPl, sizeof(bufPl), "%s%d \"%s\"", __LOCALIZE("Playlist #","sws_DLG_165"), g_playPlaylist+1, curpl->m_name.Get());

			// *** current playlist item ***		
			if (g_unsync)
			{
				*bufCurNum = '!';
				lstrcpyn(bufCur, __LOCALIZE("(sync!)","sws_DLG_165"), sizeof(bufCur));
			}
			else if (SNM_PlaylistItem* curItem = curpl->Get(g_playCur))
			{
				_snprintfSafe(bufCurNum, sizeof(bufCurNum), "%d", GetMarkerRegionNumFromId(curItem->m_rgnId));
				EnumMarkerRegionDescById(NULL, curItem->m_rgnId, bufCur, sizeof(bufCur), SNM_REGION_MASK, false, true, false);
			}
			// current item not foud (e.g. playlist switch, g_playCur = -1) 
			// => best effort: find region by play pos
			else
			{
				int id, idx = FindMarkerRegion(NULL, GetPlayPositionEx(NULL), SNM_REGION_MASK, &id);
				if (id > 0)
				{
					_snprintfSafe(bufCurNum, sizeof(bufCurNum), "%d", GetMarkerRegionNumFromId(id));
					EnumMarkerRegionDesc(NULL, idx, bufCur, sizeof(bufCur), SNM_REGION_MASK, false, true, false);
				}
			}

			// *** next playlist item ***
			if (g_playNext<0)
			{
				*bufNextNum = '-';
				lstrcpyn(bufNext, __LOCALIZE("(end)","sws_DLG_165"), sizeof(bufNext));
			}
			else if (!g_unsync && g_rgnLoop && g_playCur>=0 && g_playCur==g_playNext)
			{
				if (g_rgnLoop>0)
				{
					lstrcpyn(bufNextNum, bufCurNum, sizeof(bufNextNum));
					_snprintfSafe(bufNext, sizeof(bufNext), __LOCALIZE_VERFMT("(loop: %d)","sws_DLG_165"), g_rgnLoop);
				}
				else if (g_rgnLoop<0)
				{
					lstrcpyn(bufNextNum, bufCurNum, sizeof(bufNextNum));
					lstrcpyn(bufNext, UTF8_INFINITY, sizeof(bufNext));
				}
			}
			else if (SNM_PlaylistItem* nextItem = curpl->Get(g_playNext))
			{
				_snprintfSafe(bufNextNum, sizeof(bufNextNum), "%d", GetMarkerRegionNumFromId(nextItem->m_rgnId));
				EnumMarkerRegionDescById(NULL, nextItem->m_rgnId, bufNext, sizeof(bufNext), SNM_REGION_MASK, false, true, false);
			}
		}
	}
	m_monPl.SetText(g_playPlaylist>=0 ? bufPl : __LOCALIZE("Playlist: none (stopped)","sws_DLG_165"));
	if (g_playPlaylist>=0) _snprintfSafe(bufPl, sizeof(bufPl), "#%d", g_playPlaylist+1);
	else *bufPl = '\0';

	m_mons.SetText(0, bufPl, 0, 16);
	m_mons.SetText(1, bufCurNum, g_playPlaylist<0 ? 0 : g_unsync ? SNM_COL_RED_MONITOR : 0);
	m_mons.SetText(2, bufCur, g_playPlaylist<0 ? 0 : g_unsync ? SNM_COL_RED_MONITOR : 0);
	m_mons.SetText(3, bufNextNum, 0, 153);
	m_mons.SetText(4, bufNext, 0, 153);
}

void SNM_RegionPlaylistWnd::FillPlaylistCombo()
{
	MUTEX_PLAYLISTS;
	m_cbPlaylist.Empty();
	for (int i=0; i < g_pls.Get()->GetSize(); i++)
	{
		char name[128]="";
		_snprintfSafe(name, sizeof(name), "%d - %s", i+1, g_pls.Get()->Get(i)->m_name.Get());
		m_cbPlaylist.AddItem(name);
	}
	m_cbPlaylist.SetCurSel(g_pls.Get()->m_editId);
}

void SNM_RegionPlaylistWnd::OnCommand(WPARAM wParam, LPARAM lParam)
{
	MUTEX_PLAYLISTS;
	switch(LOWORD(wParam))
	{
		case BTNID_LOCK:
			if (!HIWORD(wParam))
				ToggleLock();
			break;
		case CMBID_PLAYLIST:
			if (HIWORD(wParam)==CBN_SELCHANGE)
			{
				// stop cell editing (changing the list content would be ignored otherwise,
				// leading to unsynchronized dropdown box vs list view)
				m_pLists.Get(0)->EditListItemEnd(false);
				g_pls.Get()->m_editId = m_cbPlaylist.GetCurSel();
				Undo_OnStateChangeEx2(NULL, UNDO_PLAYLIST_STR, UNDO_STATE_MISCCFG, -1);
				Update();
			}
			break;
		case NEW_PLAYLIST_MSG:
		case BTNID_NEW_PLAYLIST:
		case COPY_PLAYLIST_MSG:
		{
			char name[64]="";
			lstrcpyn(name, __LOCALIZE("Untitled","sws_DLG_165"), 64);
			if (PromptUserForString(m_hwnd, __LOCALIZE("S&M - Add playlist","sws_DLG_165"), name, 64, true) && *name)
			{
				if (g_pls.Get()->Add(new SNM_Playlist(LOWORD(wParam)==COPY_PLAYLIST_MSG ? GetPlaylist() : NULL, name)))
				{
					g_pls.Get()->m_editId = g_pls.Get()->GetSize()-1;
					FillPlaylistCombo();
					Undo_OnStateChangeEx2(NULL, UNDO_PLAYLIST_STR, UNDO_STATE_MISCCFG, -1); 
					Update();
				}
			}
			break;
		}
		case DEL_PLAYLIST_MSG:
		case BTNID_DEL_PLAYLIST:
			if (GetPlaylist() && g_pls.Get()->GetSize() > 0)
			{
				WDL_PtrList_DeleteOnDestroy<SNM_Playlist> delItems;
				int reply = IDYES;
				if (GetPlaylist()->GetSize()) // do not ask if empty
				{
					char msg[128] = "";
					_snprintfSafe(msg, sizeof(msg), __LOCALIZE_VERFMT("Are you sure you want to delete the playlist #%d \"%s\"?","sws_DLG_165"), g_pls.Get()->m_editId+1, GetPlaylist()->m_name.Get());
					reply = MessageBox(m_hwnd, msg, __LOCALIZE("S&M - Delete playlist","sws_DLG_165"), MB_YESNO);
				}
				if (reply != IDNO)
				{
					// updatte vars if playing
					if (g_playPlaylist>=0) {
						if (g_playPlaylist==g_pls.Get()->m_editId) PlaylistStop();
						else if (g_playPlaylist>g_pls.Get()->m_editId) g_playPlaylist--;
					}
					delItems.Add(g_pls.Get()->Get(g_pls.Get()->m_editId));
					g_pls.Get()->Delete(g_pls.Get()->m_editId, false); // no deletion yet (still used in GUI)
					g_pls.Get()->m_editId = BOUNDED(g_pls.Get()->m_editId-1, 0, g_pls.Get()->GetSize()-1);
					FillPlaylistCombo();
					Undo_OnStateChangeEx2(NULL, UNDO_PLAYLIST_STR, UNDO_STATE_MISCCFG, -1); 
					Update();
				}
			} // + delItems cleanup
			break;
		case REN_PLAYLIST_MSG:
			if (GetPlaylist() && g_pls.Get()->GetSize() > 0)
			{
				char newName[64]="";
				lstrcpyn(newName, GetPlaylist()->m_name.Get(), 64);
				if (PromptUserForString(m_hwnd, __LOCALIZE("S&M - Rename playlist","sws_DLG_165"), newName, 64, true) && *newName)
				{
					GetPlaylist()->m_name.Set(newName);
					FillPlaylistCombo();
					Undo_OnStateChangeEx2(NULL, UNDO_PLAYLIST_STR, UNDO_STATE_MISCCFG, -1); 
					Update();
				}
			}
			break;
		case BTNID_PLAY:
			PlaylistPlay(g_pls.Get()->m_editId, GetNextValidItem(g_pls.Get()->m_editId, 0, true, g_repeatPlaylist));
			break;
		case BTNID_STOP:
			OnStopButton();
			break;
		case BTNID_REPEAT:
			SetPlaylistRepeat(NULL);
			break;
		case BTNID_PASTE:
			RECT r; m_btnCrop.GetPositionInTopVWnd(&r);
			ClientToScreen(m_hwnd, (LPPOINT)&r);
			ClientToScreen(m_hwnd, ((LPPOINT)&r)+1);
			SendMessage(m_hwnd, WM_CONTEXTMENU, 0, MAKELPARAM((UINT)(r.left), (UINT)(r.bottom+SNM_1PIXEL_Y)));
			break;
		case CROP_PRJ_MSG:
			AppendPasteCropPlaylist(GetPlaylist(), 0);
			break;
		case CROP_PRJTAB_MSG:
			AppendPasteCropPlaylist(GetPlaylist(), 1);
			break;
		case APPEND_PRJ_MSG:
			AppendPasteCropPlaylist(GetPlaylist(), 2);
			break;
		case PASTE_CURSOR_MSG:
			AppendPasteCropPlaylist(GetPlaylist(), 3);
			break;
		case DELETE_MSG:
		{
			int x=0, slot; bool updt = false;
			WDL_PtrList_DeleteOnDestroy<SNM_PlaylistItem> delItems;
			while(SNM_PlaylistItem* item = (SNM_PlaylistItem*)m_pLists.Get(0)->EnumSelected(&x))
			{
				slot = GetPlaylist()->Find(item);
				if (slot>=0)
				{
					delItems.Add(item);
					GetPlaylist()->Delete(slot, false);  // no deletion yet (still used in GUI)
					updt=true;
				}
			}
			if (updt)
			{
				Undo_OnStateChangeEx2(NULL, UNDO_PLAYLIST_STR, UNDO_STATE_MISCCFG, -1);
				PlaylistResync();
				Update();
			} // + delItems cleanup
			break;
		}
		case TGL_INFINITE_LOOP_MSG:
		{
			int x=0; bool updt = false;
			while(SNM_PlaylistItem* item = (SNM_PlaylistItem*)m_pLists.Get(0)->EnumSelected(&x)) {
				item->m_cnt *= (-1);
				updt = true;
			}
			if (updt)
			{
				Undo_OnStateChangeEx2(NULL, UNDO_PLAYLIST_STR, UNDO_STATE_MISCCFG, -1); 
				PlaylistResync();
				Update();
			}
			break;
		}
		case TGL_SCROLL_VIEW_MSG:
			g_scrollView = !g_scrollView;
			break;
		case TGL_SEEK_PLAY_MSG:
			g_seekPlay = !g_seekPlay;
			break;
		case APPEND_SEL_RGN_MSG:
		case PASTE_SEL_RGN_MSG:
		{
			SNM_Playlist p("temp");
			int x=0;
			while(SNM_PlaylistItem* item = (SNM_PlaylistItem*)m_pLists.Get(0)->EnumSelected(&x))
				p.Add(new SNM_PlaylistItem(item->m_rgnId, item->m_cnt));
			AppendPasteCropPlaylist(&p, LOWORD(wParam)==PASTE_SEL_RGN_MSG ? 3:2);
			break;
		}
		case PERFORM_MSG: 
			if (GetPlaylist())
			{
				int x=0;
				if (SNM_PlaylistItem* pItem = (SNM_PlaylistItem*)m_pLists.Get(0)->EnumSelected(&x))
					PlaylistPlay(g_pls.Get()->m_editId, GetPlaylist()->Find(pItem));
			}
			break;
		case ADD_ALL_REGIONS_MSG:
		{
			int x=0, y, num; bool isRgn, updt=false;
			while (y = EnumProjectMarkers2(NULL, x, &isRgn, NULL, NULL, NULL, &num))
			{
				if (isRgn) {
					SNM_PlaylistItem* newItem = new SNM_PlaylistItem(MakeMarkerRegionId(num, isRgn));
					updt |= (GetPlaylist() && GetPlaylist()->Add(newItem) != NULL);
				}
				x=y;
			}
			if (updt)
			{
				Undo_OnStateChangeEx2(NULL, UNDO_PLAYLIST_STR, UNDO_STATE_MISCCFG, -1);
				PlaylistResync();
				Update();
			}
			else
				MessageBox(m_hwnd, __LOCALIZE("No region found in project!","sws_DLG_165"), __LOCALIZE("S&M - Error","sws_DLG_165"), MB_OK);
			break;
		}
		default:
			if (LOWORD(wParam) >= INSERT_REGION_START_MSG && LOWORD(wParam) <= INSERT_REGION_END_MSG)
			{
				SNM_Playlist* pl = GetPlaylist();
				if (!pl) break;
				SNM_PlaylistItem* newItem = new SNM_PlaylistItem(GetMarkerRegionIdFromIndex(NULL, LOWORD(wParam)-INSERT_REGION_START_MSG));
				if (pl->GetSize())
				{
					if (SNM_PlaylistItem* item = (SNM_PlaylistItem*)m_pLists.Get(0)->EnumSelected(NULL))
					{
						int slot = pl->Find(item);
						if (slot >= 0 && pl->Insert(slot, newItem))
						{
							Undo_OnStateChangeEx2(NULL, UNDO_PLAYLIST_STR, UNDO_STATE_MISCCFG, -1); 
							PlaylistResync();
							Update();
							m_pLists.Get(0)->SelectByItem((SWS_ListItem*)newItem);
							return;
						}
					}
				}
				// empty list, no selection, etc.. => add
				if (pl->Add(newItem))
				{
					Undo_OnStateChangeEx2(NULL, UNDO_PLAYLIST_STR, UNDO_STATE_MISCCFG, -1); 
					PlaylistResync();
					Update();
					m_pLists.Get(0)->SelectByItem((SWS_ListItem*)newItem);
				}
			}
			else if (LOWORD(wParam) >= ADD_REGION_START_MSG && LOWORD(wParam) <= ADD_REGION_END_MSG)
			{
				SNM_PlaylistItem* newItem = new SNM_PlaylistItem(GetMarkerRegionIdFromIndex(NULL, LOWORD(wParam)-ADD_REGION_START_MSG));
				if (GetPlaylist() && GetPlaylist()->Add(newItem))
				{
					Undo_OnStateChangeEx2(NULL, UNDO_PLAYLIST_STR, UNDO_STATE_MISCCFG, -1); 
					PlaylistResync();
					Update();
					m_pLists.Get(0)->SelectByItem((SWS_ListItem*)newItem);
				}
			}
			else
				Main_OnCommand((int)wParam, (int)lParam);
			break;
	}
}

int SNM_RegionPlaylistWnd::OnKey(MSG* _msg, int _iKeyState) 
{
	if (_msg->message == WM_KEYDOWN && !_iKeyState)
	{
		switch(_msg->wParam)
		{
			case VK_DELETE:
				OnCommand(DELETE_MSG, 0);
				return 1;
			case VK_RETURN:
				OnCommand(PERFORM_MSG, 0);
				return 1;
		}
	}
	return 0;
}

// play/stop/pause buttons are right aligned when _RGNPL_TRANSPORT_RIGHT is defined
#define _RGNPL_TRANSPORT_RIGHT

void SNM_RegionPlaylistWnd::DrawControls(LICE_IBitmap* _bm, const RECT* _r, int* _tooltipHeight)
{
	MUTEX_PLAYLISTS;

	LICE_CachedFont* font = SNM_GetThemeFont();
	IconTheme* it = SNM_GetIconTheme();
	int x0=_r->left+SNM_GUI_X_MARGIN, h=SNM_GUI_TOP_H;
	if (_tooltipHeight)
		*_tooltipHeight = h;
	
	bool hasPlaylists = (g_pls.Get()->GetSize()>0);
	SNM_Playlist* pl = GetPlaylist();

	SNM_SkinButton(&m_btnLock, it ? &it->toolbar_lock[!g_monitorMode] : NULL, g_monitorMode ? __LOCALIZE("Edition mode","sws_DLG_165") : __LOCALIZE("Monitoring mode","sws_DLG_165"));
	if (SNM_AutoVWndPosition(DT_LEFT, &m_btnLock, NULL, _r, &x0, _r->top, h))
	{
#ifndef _RGNPL_TRANSPORT_RIGHT
		SNM_SkinButton(&m_btnPlay, it ? &it->gen_play[g_playPlaylist>=0?1:0] : NULL, __LOCALIZE("Play","sws_DLG_165"));
		if (SNM_AutoVWndPosition(DT_LEFT, &m_btnPlay, NULL, _r, &x0, _r->top, h, 0))
		{
			SNM_SkinButton(&m_btnStop, it ? &(it->gen_stop) : NULL, __LOCALIZE("Stop","sws_DLG_165"));
			if (SNM_AutoVWndPosition(DT_LEFT, &m_btnStop, NULL, _r, &x0, _r->top, h, 0))
			{
				SNM_SkinButton(&m_btnRepeat, it ? &it->gen_repeat[g_repeatPlaylist?1:0] : NULL, __LOCALIZE("Repeat","sws_DLG_165"));
				if (SNM_AutoVWndPosition(DT_LEFT, &m_btnRepeat, NULL, _r, &x0, _r->top, h))
#endif
				{
					// *** monitoring ***
					if (g_monitorMode)
					{
						RECT r = *_r;

						r.top += int(SNM_GUI_TOP_H/2+0.5);
						r.bottom -= int(SNM_GUI_TOP_H/2);
						m_mons.SetPosition(&r);
						m_mons.SetVisible(true);
						
						r = *_r;

						// playlist name
						r.left = x0;
						r.bottom = h;
#ifndef _RGNPL_TRANSPORT_RIGHT
						LICE_IBitmap* logo = SNM_GetThemeLogo();
//						r.right -= logo ? logo->getWidth()+5 : 5; // 5: margin
#else
						r.right -= (75+SNM_GUI_X_MARGIN); //JFB lazy here.. should be play+stop+repeat btn widths
#endif
						m_monPl.SetPosition(&r);
						m_monPl.SetVisible(true);
					}

					// *** edition ***
					else
					{
						m_txtPlaylist.SetFont(font);
						m_txtPlaylist.SetText(hasPlaylists ? __LOCALIZE("Playlist #","sws_DLG_165") : __LOCALIZE("Playlist: None","sws_DLG_165"));
						if (SNM_AutoVWndPosition(DT_LEFT, &m_txtPlaylist, NULL, _r, &x0, _r->top, h, hasPlaylists?4:SNM_DEF_VWND_X_STEP))
						{
							m_cbPlaylist.SetFont(font);
							if (!hasPlaylists || (hasPlaylists && SNM_AutoVWndPosition(DT_LEFT, &m_cbPlaylist, &m_txtPlaylist, _r, &x0, _r->top, h, 4)))
							{
								m_btnDel.SetEnabled(hasPlaylists);
								if (SNM_AutoVWndPosition(DT_LEFT, &m_btnsAddDel, NULL, _r, &x0, _r->top, h))
								{
									if (abs(hasPlaylists && pl ? pl->GetLength() : 0.0) > 0.0) // <0.0 means infinite
									{
										SNM_SkinToolbarButton(&m_btnCrop, __LOCALIZE("Crop/paste","sws_DLG_165"));
										if (SNM_AutoVWndPosition(DT_LEFT, &m_btnCrop, NULL, _r, &x0, _r->top, h))
										{
#ifndef _RGNPL_TRANSPORT_RIGHT
											SNM_AddLogo(_bm, _r, x0, h);
#endif
										}
									}
#ifndef _RGNPL_TRANSPORT_RIGHT
									else
										SNM_AddLogo(_bm, _r, x0, h);
#endif
								}
							}
						}
					}
				}
#ifndef _RGNPL_TRANSPORT_RIGHT
			}
		}
#endif
	}

#ifdef _RGNPL_TRANSPORT_RIGHT
	x0 = _r->right-SNM_GUI_X_MARGIN;
	SNM_SkinButton(&m_btnRepeat, it ? &it->gen_repeat[g_repeatPlaylist?1:0] : NULL, __LOCALIZE("Repeat","sws_DLG_165"));
	if (SNM_AutoVWndPosition(DT_RIGHT, &m_btnRepeat, NULL, _r, &x0, _r->top, h, 0))
	{
		SNM_SkinButton(&m_btnStop, it ? &(it->gen_stop) : NULL, __LOCALIZE("Stop","sws_DLG_165"));
		if (SNM_AutoVWndPosition(DT_RIGHT, &m_btnStop, NULL, _r, &x0, _r->top, h, 0))
		{
			SNM_SkinButton(&m_btnPlay, it ? &it->gen_play[g_playPlaylist>=0?1:0] : NULL, __LOCALIZE("Play","sws_DLG_165"));
			SNM_AutoVWndPosition(DT_RIGHT, &m_btnPlay, NULL, _r, &x0, _r->top, h, 0);
		}
	}
	if (g_monitorMode)
		SNM_AddLogo(_bm, _r);
#endif
}

void SNM_RegionPlaylistWnd::AddPasteContextMenu(HMENU _menu)
{
	if (GetMenuItemCount(_menu))
		AddToMenu(_menu, SWS_SEPARATOR, 0);
	AddToMenu(_menu, __LOCALIZE("Crop project to playlist","sws_DLG_165"), CROP_PRJ_MSG, -1, false, GetPlaylist() && GetPlaylist()->GetSize() ? 0 : MF_GRAYED);
	AddToMenu(_menu, __LOCALIZE("Crop project to playlist (new project tab)","sws_DLG_165"), CROP_PRJTAB_MSG, -1, false, GetPlaylist() && GetPlaylist()->GetSize() ? 0 : MF_GRAYED);
	AddToMenu(_menu, __LOCALIZE("Append playlist to project","sws_DLG_165"), APPEND_PRJ_MSG, -1, false, GetPlaylist() && GetPlaylist()->GetSize() ? 0 : MF_GRAYED);
	AddToMenu(_menu, __LOCALIZE("Paste playlist at edit cursor","sws_DLG_165"), PASTE_CURSOR_MSG, -1, false, GetPlaylist() && GetPlaylist()->GetSize() ? 0 : MF_GRAYED);

	int x=0; bool hasSel = m_pLists.Get(0)->EnumSelected(&x) != NULL;
	AddToMenu(_menu, SWS_SEPARATOR, 0);
	AddToMenu(_menu, __LOCALIZE("Append selected regions to project","sws_DLG_165"), APPEND_SEL_RGN_MSG, -1, false, hasSel ? 0 : MF_GRAYED);
	AddToMenu(_menu, __LOCALIZE("Paste selected regions at edit cursor","sws_DLG_165"), PASTE_SEL_RGN_MSG, -1, false, hasSel ? 0 : MF_GRAYED);
}

HMENU SNM_RegionPlaylistWnd::OnContextMenu(int _x, int _y, bool* _wantDefaultItems)
{
	if (g_monitorMode)
		return NULL;

	MUTEX_PLAYLISTS;

	HMENU hMenu = CreatePopupMenu();

	// specific context menu for the paste button
	POINT p; GetCursorPos(&p);
	ScreenToClient(m_hwnd,&p);
	if (WDL_VWnd* v = m_parentVwnd.VirtWndFromPoint(p.x,p.y,1))
		switch (v->GetID()) {
			case BTNID_PASTE:
				*_wantDefaultItems = false;
				AddPasteContextMenu(hMenu);
				return hMenu;
		}

	int x=0; bool hasSel = (m_pLists.Get(0)->EnumSelected(&x) != NULL);
	*_wantDefaultItems = !(m_pLists.Get(0)->GetHitItem(_x, _y, &x) && x >= 0);

	if (*_wantDefaultItems)
	{
		HMENU hPlaylistSubMenu = CreatePopupMenu();
		AddSubMenu(hMenu, hPlaylistSubMenu, __LOCALIZE("Playlists","sws_DLG_165"));
		AddToMenu(hPlaylistSubMenu, __LOCALIZE("Copy playlist...","sws_DLG_165"), COPY_PLAYLIST_MSG, -1, false, GetPlaylist() ? MF_ENABLED : MF_GRAYED);
		AddToMenu(hPlaylistSubMenu, __LOCALIZE("Delete","sws_DLG_165"), DEL_PLAYLIST_MSG, -1, false, GetPlaylist() ? MF_ENABLED : MF_GRAYED);
		AddToMenu(hPlaylistSubMenu, __LOCALIZE("New playlist...","sws_DLG_165"), NEW_PLAYLIST_MSG);
		AddToMenu(hPlaylistSubMenu, __LOCALIZE("Rename...","sws_DLG_165"), REN_PLAYLIST_MSG, -1, false, GetPlaylist() ? MF_ENABLED : MF_GRAYED);

		AddToMenu(hMenu, SWS_SEPARATOR, 0);
		HMENU hCropPasteSubMenu = CreatePopupMenu();
		AddSubMenu(hMenu, hCropPasteSubMenu, __LOCALIZE("Crop/paste","sws_DLG_165"));
		AddPasteContextMenu(hCropPasteSubMenu);
	}

	if (GetPlaylist())
	{
		if (GetMenuItemCount(hMenu))
			AddToMenu(hMenu, SWS_SEPARATOR, 0);

		AddToMenu(hMenu, __LOCALIZE("Add all regions","sws_DLG_165"), ADD_ALL_REGIONS_MSG);
		HMENU hAddSubMenu = CreatePopupMenu();
		AddSubMenu(hMenu, hAddSubMenu, __LOCALIZE("Add region","sws_DLG_165"));
		FillMarkerRegionMenu(NULL, hAddSubMenu, ADD_REGION_START_MSG, SNM_REGION_MASK);

		if (!*_wantDefaultItems) 
		{
			HMENU hInsertSubMenu = CreatePopupMenu();
			AddSubMenu(hMenu, hInsertSubMenu, __LOCALIZE("Insert region","sws_DLG_165"), -1, hasSel ? 0 : MF_GRAYED);
			FillMarkerRegionMenu(NULL, hInsertSubMenu, INSERT_REGION_START_MSG, SNM_REGION_MASK);
			AddToMenu(hMenu, __LOCALIZE("Remove regions","sws_DLG_165"), DELETE_MSG, -1, false, hasSel ? 0 : MF_GRAYED);
			AddToMenu(hMenu, SWS_SEPARATOR, 0);
			AddToMenu(hMenu, __LOCALIZE("Toggle infinite loop","sws_DLG_165"), TGL_INFINITE_LOOP_MSG, -1, false, hasSel ? 0 : MF_GRAYED);
			AddToMenu(hMenu, SWS_SEPARATOR, 0);
			AddToMenu(hMenu, __LOCALIZE("Append selected regions to project","sws_DLG_165"), APPEND_SEL_RGN_MSG, -1, false, hasSel ? 0 : MF_GRAYED);
			AddToMenu(hMenu, __LOCALIZE("Paste selected regions at edit cursor","sws_DLG_165"), PASTE_SEL_RGN_MSG, -1, false, hasSel ? 0 : MF_GRAYED);
		}
	}

	if (*_wantDefaultItems)
	{
		if (GetMenuItemCount(hMenu))
			AddToMenu(hMenu, SWS_SEPARATOR, 0);
		AddToMenu(hMenu, __LOCALIZE("Repeat playlist","sws_DLG_165"), BTNID_REPEAT, -1, false, g_repeatPlaylist ? MF_CHECKED : MF_UNCHECKED);	
		AddToMenu(hMenu, __LOCALIZE("Scroll view when selecting regions","sws_DLG_165"), TGL_SCROLL_VIEW_MSG, -1, false, g_scrollView ? MF_CHECKED : MF_UNCHECKED);	
		AddToMenu(hMenu, __LOCALIZE("Seek play","sws_DLG_165"), TGL_SEEK_PLAY_MSG, -1, false, g_seekPlay ? MF_CHECKED : MF_UNCHECKED);	
	}
	return hMenu;
}

bool SNM_RegionPlaylistWnd::GetToolTipString(int _xpos, int _ypos, char* _bufOut, int _bufOutSz)
{
	if (WDL_VWnd* v = m_parentVwnd.VirtWndFromPoint(_xpos,_ypos,1))
	{
		switch (v->GetID())
		{
			case BTNID_LOCK:
				lstrcpyn(_bufOut, __LOCALIZE("Toggle monitoring/edition mode","sws_DLG_165"), _bufOutSz);
				return true;
			case BTNID_PLAY:
				if (g_playPlaylist>=0) _snprintfSafe(_bufOut, _bufOutSz, __LOCALIZE_VERFMT("Playing playlist #%d","sws_DLG_165"), g_playPlaylist+1);
				else lstrcpyn(_bufOut, __LOCALIZE("Play preview","sws_DLG_165"), _bufOutSz);
				return true;
			case BTNID_STOP:
				lstrcpyn(_bufOut, __LOCALIZE("Stop","sws_DLG_165"), _bufOutSz);
				return true;
			case BTNID_REPEAT:
				lstrcpyn(_bufOut, __LOCALIZE("Repeat playlist","sws_DLG_165"), _bufOutSz);
				return true;
			case CMBID_PLAYLIST:
				if (SNM_Playlist* pl = GetPlaylist())
				{
					double len = pl->GetLength();
					char timeStr[64]="";
					if (len >= 0.0) format_timestr_pos(len, timeStr, sizeof(timeStr), -1);
					else lstrcpyn(timeStr, __LOCALIZE("infinite","sws_DLG_165"), sizeof(timeStr));
					_snprintfSafe(_bufOut, _bufOutSz, __LOCALIZE_VERFMT("Edited playlist: #%d \"%s\"\nLength: %s","sws_DLG_165"), g_pls.Get()->m_editId+1, pl->m_name.Get(), timeStr);
					return true;
				}
			case BTNID_NEW_PLAYLIST:
				lstrcpyn(_bufOut, __LOCALIZE("Add playlist","sws_DLG_165"), _bufOutSz);
				return true;
			case BTNID_DEL_PLAYLIST:
				lstrcpyn(_bufOut, __LOCALIZE("Delete playlist","sws_DLG_165"), _bufOutSz);
				return true;
			case BTNID_PASTE:
				lstrcpyn(_bufOut, __LOCALIZE("Crop/paste","sws_DLG_165"), _bufOutSz);
				return true;
			case TXTID_MONITOR_PL:
				if (g_playPlaylist>=0) _snprintfSafe(_bufOut, _bufOutSz, __LOCALIZE_VERFMT("Playing playlist: #%d \"%s\"","sws_DLG_165"), g_playPlaylist+1, GetPlaylist(g_playPlaylist)->m_name.Get());
				return (g_playPlaylist>=0);
		}
	}
	return false;
}

void SNM_RegionPlaylistWnd::ToggleLock()
{
	g_monitorMode = !g_monitorMode;
	RefreshToolbar(SWSGetCommandID(ToggleRegionPlaylistLock));
	Update();
}


///////////////////////////////////////////////////////////////////////////////

int IsInPlaylists(double _pos)
{
	MUTEX_PLAYLISTS;
	for (int i=0; i < g_pls.Get()->GetSize(); i++)
		if (SNM_Playlist* pl = g_pls.Get()->Get(i))
			if (pl->IsInPlaylist(_pos, false, 0) >= 0)
				return i;
	return -1;
}


///////////////////////////////////////////////////////////////////////////////
// Polling on play: PlaylistRun() and related funcs
///////////////////////////////////////////////////////////////////////////////

// never use things like playlist->Get(i+1) but this func!
int GetNextValidItem(int _plId, int _itemId, bool _startWith, bool _repeat)
{
	MUTEX_PLAYLISTS;
	if (_plId>=0 && _itemId>=0)
	{
		if (SNM_Playlist* pl = GetPlaylist(_plId))
		{
			for (int i=_itemId+(_startWith?0:1); i<pl->GetSize(); i++)
				if (pl->IsValidIem(i))
					return i;
			if (_repeat)
				for (int i=0; i<pl->GetSize() && i<(_itemId+(_startWith?1:0)); i++)
					if (pl->IsValidIem(i))
						return i;
			// not found if we are here..
			if (_repeat && pl->IsValidIem(_itemId))
				return _itemId;
		}
	}
	return -1;
}

// never use things like playlist->Get(i-1) but this func!
int GetPrevValidItem(int _plId, int _itemId, bool _startWith, bool _repeat)
{
	MUTEX_PLAYLISTS;
	if (_plId>=0 && _itemId>=0)
	{
		if (SNM_Playlist* pl = GetPlaylist(_plId))
		{
			for (int i=_itemId-(_startWith?0:1); i>=0; i--)
				if (pl->IsValidIem(i))
					return i;
			if (_repeat)
				for (int i=pl->GetSize()-1; i>=0 && i>(_itemId-(_startWith?1:0)); i--)
					if (pl->IsValidIem(i))
						return i;
			// not found if we are here..
			if (_repeat && pl->IsValidIem(_itemId))
				return _itemId;
		}
	}
	return -1;
}

bool SeekItem(int _plId, int _nextItemId, int _curItemId)
{
	MUTEX_PLAYLISTS;
	if (SNM_Playlist* pl = g_pls.Get()->Get(_plId))
	{
		// trick to stop the playlist in sync: smooth seek to the end of the project (!)
		if (_nextItemId<0)
		{
			// temp override of the "stop play at project end" option
			if (int* opt = (int*)GetConfigVar("stopprojlen")) {
				g_oldStopprojlenPref = *opt;
				*opt = 1;
			}
			g_playNext = -1;
			g_rgnLoop = 0;
			g_nextRgnPos = GetProjectLength()+1.0;
			g_nextRgnEnd = g_nextRgnPos+1.0;
			SeekPlay(g_nextRgnPos);
			return true;
		}
		else if (SNM_PlaylistItem* next = pl->Get(_nextItemId))
		{
			double a, b;
			if (EnumMarkerRegionById(NULL, next->m_rgnId, NULL, &a, &b, NULL, NULL, NULL))
			{
				g_playNext = _nextItemId;
				g_playCur = _plId==g_playPlaylist ? g_playCur : _curItemId;
				g_rgnLoop = next->m_cnt<0 ? -1 : next->m_cnt>1 ? next->m_cnt : 0;
				g_nextRgnPos = a;
				g_nextRgnEnd = b;
				if (_curItemId<0) {
					g_curRgnPos = 0.0;
					g_curRgnEnd = -1.0;
				}
				SeekPlay(g_nextRgnPos);
				return true;
			}
		}
	}
	return false;
}

// the meat, designed to be as idle as possible..
// handle with care, remember we always look 1 region ahead!
void PlaylistRun()
{
	MUTEX_PLAYLISTS;

	static bool bRecurseCheck = false;
	if (bRecurseCheck || g_playPlaylist<0)
		return;
	bRecurseCheck = true;

	if (g_playPlaylist>=0)
	{
		bool updateUI = false;
		double pos = GetPlayPositionEx(NULL);

		if (pos>=g_nextRgnPos && pos<=g_nextRgnEnd)
		{
			// a bunch of calls end here when looping!!

			if (!g_plLoop || (g_plLoop && (g_unsync || pos<g_lastRunPos)))
			{
				g_plLoop = false;

				bool first = false;
				if (g_playCur != g_playNext /*JFB vars altered only here || g_curRgnPos != g_nextRgnPos || g_curRgnEnd != g_nextRgnEnd */)
				{
					first = updateUI = true;
					g_playCur = g_playNext;
					g_curRgnPos = g_nextRgnPos;
					g_curRgnEnd = g_nextRgnEnd;
				}

				// region loop?
				if (g_rgnLoop && (first || g_unsync || pos<g_lastRunPos))
				{
					updateUI = true;
					if (g_rgnLoop>0)
						g_rgnLoop--;
					if (g_rgnLoop)
						SeekPlay(g_nextRgnPos); // then exit
				}

				if (!g_rgnLoop) // if, not else if!
				{
					int nextId = GetNextValidItem(g_playPlaylist, g_playCur, false, g_repeatPlaylist);

					// loop corner cases
					// ex: 1 item in the playlist + repeat on, or repeat on + last region == first region,
					//     or playlist = region3, then unknown region (e.g. deleted) and region3 again, or etc..
					if (nextId>=0)
						if (SNM_Playlist* pl = GetPlaylist(g_playPlaylist))
							if (SNM_PlaylistItem* next = pl->Get(nextId)) 
								if (SNM_PlaylistItem* cur = pl->Get(g_playCur)) 
									g_plLoop = (cur->m_rgnId==next->m_rgnId); // valid regions at this point

					if (!SeekItem(g_playPlaylist, nextId, g_playCur))
						SeekItem(g_playPlaylist, -1, g_playCur); // end of playlist..
					updateUI = true;
				}
			}
			g_unsync = false;
		}
		else if (g_curRgnPos<g_curRgnEnd) // relevant vars?
		{
			// seek play requested, waiting for region switch..
			if (pos>=g_curRgnPos && pos<=g_curRgnEnd)
			{
				// a bunch of calls end here!
				g_unsync = false;
			}
			// playlist no more in sync?
			else if (!g_unsync)
			{
				updateUI = g_unsync = true;
				int spareItemId = -1;
				if (SNM_Playlist* pl = g_pls.Get()->Get(g_playPlaylist))
					spareItemId = pl->IsInPlaylist(pos, g_repeatPlaylist, g_playCur>=0?g_playCur:0);
				if (spareItemId<0 || !SeekItem(g_playPlaylist, spareItemId, -1))
					SeekPlay(g_nextRgnPos);	// try to resync the expected region, best effort
			}
		}

		g_lastRunPos = pos;
		if (updateUI && g_pRgnPlaylistWnd)
			g_pRgnPlaylistWnd->Update(1);
	}

	bRecurseCheck = false;
}

// _itemId: callers must not use no hard coded value but GetNextValidItem() or GetPrevValidItem()
void PlaylistPlay(int _plId, int _itemId)
{
	MUTEX_PLAYLISTS;

	if (!g_pls.Get()->GetSize()) {
		PlaylistStop();
		MessageBox(g_pRgnPlaylistWnd?g_pRgnPlaylistWnd->GetHWND():GetMainHwnd(), __LOCALIZE("No playlist defined!\nUse the tiny button \"+\" to add one.","sws_DLG_165"), __LOCALIZE("S&M - Error","sws_DLG_165"),MB_OK);
		return;
	}

	SNM_Playlist* pl = GetPlaylist(_plId);
	if (!pl) // e.g. when called via actions
	{
		PlaylistStop();
		char msg[128] = "";
		_snprintfSafe(msg, sizeof(msg), __LOCALIZE_VERFMT("Unknown playlist #%d!","sws_DLG_165"), _plId+1);
		MessageBox(g_pRgnPlaylistWnd?g_pRgnPlaylistWnd->GetHWND():GetMainHwnd(), msg, __LOCALIZE("S&M - Error","sws_DLG_165"),MB_OK);
		return;
	}

	double prjlen = GetProjectLength();
	if (prjlen>0.1)
	{
		//JFB REAPER bug? workaround, the native pref "stop play at project end" does not work when the project is empty (should not play at all..)
		int num = pl->GetGreaterMarkerRegion(prjlen);
		if (num>0)
		{
			char msg[256] = "";
			_snprintfSafe(msg, sizeof(msg), __LOCALIZE_VERFMT("The playlist #%d might end unexpectedly!\nIt constains regions that start after the end of project (region %d at least).","sws_DLG_165"), _plId+1, num);
			if (IDCANCEL == MessageBox(g_pRgnPlaylistWnd?g_pRgnPlaylistWnd->GetHWND():GetMainHwnd(), msg, __LOCALIZE("S&M - Warning","sws_DLG_165"), MB_OKCANCEL)) {
				PlaylistStop();
				return;
			}
		}

		num = pl->GetNestedMarkerRegion();
		if (num>0)
		{
			char msg[256] = "";
			_snprintfSafe(msg, sizeof(msg), __LOCALIZE_VERFMT("The playlist #%d might not work as expected!\nIt contains nested markers/regions (inside region %d at least).","sws_DLG_165"), _plId+1, num);
			if (IDCANCEL == MessageBox(g_pRgnPlaylistWnd?g_pRgnPlaylistWnd->GetHWND():GetMainHwnd(), msg, __LOCALIZE("S&M - Warning","sws_DLG_165"), MB_OKCANCEL)) {
				PlaylistStop();
				return;
			}
		}

		// handle empty project corner case
		if (pl->IsValidIem(_itemId))
		{
			if (!g_seekPlay)
				PlaylistStop();

			// temp override of the "smooth seek" option
			if (int* opt = (int*)GetConfigVar("smoothseek")) {
				g_oldSeekPref = *opt;
				*opt = 3;
			}
			// temp override of the repeat/loop state option
			if (GetSetRepeat(-1) == 1) {
				g_oldRepeatState = 1;
				GetSetRepeat(0);
			}

			g_plLoop = false;
			g_unsync = false;
			g_lastRunPos = GetProjectLength()+1.0;
			if (SeekItem(_plId, _itemId, g_playPlaylist==_plId ? g_playCur : -1))
			{
				g_playPlaylist = _plId; // enables PlaylistRun()
				if (g_pRgnPlaylistWnd)
					g_pRgnPlaylistWnd->Update(); // for the play button, next/previous region actions, etc....
			}
			else
				PlaylistStopped(); // reset vars & native prefs
		}
	}

	if (g_playPlaylist<0)
	{
		char msg[128] = "";
		_snprintfSafe(msg, sizeof(msg), __LOCALIZE_VERFMT("Playlist #%d: nothing to play!\n(empty playlist, empty project, removed regions, etc..)","sws_DLG_165"), _plId+1);
		MessageBox(g_pRgnPlaylistWnd?g_pRgnPlaylistWnd->GetHWND():GetMainHwnd(), msg, __LOCALIZE("S&M - Error","sws_DLG_165"),MB_OK);
	}
}

// _ct=NULL or _ct->user=-1 to play the edited playlist, use the provided playlist id otherwise
void PlaylistPlay(COMMAND_T* _ct)
{
	int plId = _ct ? (int)_ct->user : -1;
	plId = plId>=0 ? plId : g_pls.Get()->m_editId;
	PlaylistPlay(plId, GetNextValidItem(plId, 0, true, g_repeatPlaylist));
}

// always cycle (whatever is g_repeatPlaylist)
void PlaylistSeekPrevNext(COMMAND_T* _ct)
{
	if (g_playPlaylist<0)
		PlaylistPlay(NULL);
	else
	{
		int itemId;
		if ((int)_ct->user>0)
			itemId = GetNextValidItem(g_playPlaylist, g_playNext, false, true);
		else
		{
			itemId = GetPrevValidItem(g_playPlaylist, g_playNext, false, true);
			if (itemId == g_playCur)
				itemId = GetPrevValidItem(g_playPlaylist, g_playCur, false, true);
		}
		PlaylistPlay(g_playPlaylist, itemId);
	}
}

void PlaylistStop()
{
	if (g_playPlaylist>=0 || (GetPlayStateEx(NULL)&1) == 1) {
		OnStopButton();
		PlaylistStopped();
	}
}

void PlaylistStopped()
{
	if (g_playPlaylist>=0)
	{
		MUTEX_PLAYLISTS;

		g_playPlaylist = -1;

		// restore options
		if (g_oldSeekPref >= 0)
			if (int* opt = (int*)GetConfigVar("smoothseek")) {
				*opt = g_oldSeekPref;
				g_oldSeekPref = -1;
			}
		if (g_oldStopprojlenPref >= 0)
			if (int* opt = (int*)GetConfigVar("stopprojlen")) {
				*opt = g_oldStopprojlenPref;
				g_oldStopprojlenPref = -1;
			}
		if (g_oldRepeatState >=0) {
			GetSetRepeat(g_oldRepeatState);
			g_oldRepeatState = -1;
		}

		if (g_pRgnPlaylistWnd)
			g_pRgnPlaylistWnd->Update();
	}
}

// used when editing the playlist/regions while playing (required because we always look one region ahead)
void PlaylistResync()
{
	if (SNM_Playlist* pl = GetPlaylist(g_playPlaylist))
		if (SNM_PlaylistItem* item = pl->Get(g_playCur))
			SeekItem(g_playPlaylist, GetNextValidItem(g_playPlaylist, g_playCur, item->m_cnt<0 || item->m_cnt>1, g_repeatPlaylist), g_playCur);
}

void SetPlaylistRepeat(COMMAND_T* _ct)
{
	int mode = _ct ? (int)_ct->user : -1; // toggle if no COMMAND_T is specified
	switch(mode) {
		case -1: g_repeatPlaylist=!g_repeatPlaylist; break;
		case 0: g_repeatPlaylist=false; break;
		case 1: g_repeatPlaylist=true; break;
	}
	RefreshToolbar(SWSGetCommandID(SetPlaylistRepeat, -1));
	PlaylistResync();
	if (g_pRgnPlaylistWnd)
		g_pRgnPlaylistWnd->Update();
}

int IsPlaylistRepeat(COMMAND_T*) {
	return g_repeatPlaylist;
}


///////////////////////////////////////////////////////////////////////////////

//JFB nothing to see here.. please move on :)
// (doing things that are not really possible with the current v4.3 api => macro-ish, etc..)
// _mode: 0=crop current project, 1=crop to new project tab, 2=append to current project, 3=paste at cursor position
// note: moves/copies env points too, makes polled items, etc.. according to user prefs
//JFB TODO? crop => markers removed
void AppendPasteCropPlaylist(SNM_Playlist* _playlist, int _mode)
{
	MUTEX_PLAYLISTS;

	if (!_playlist || !_playlist->GetSize())
		return;

	int rgnNum = _playlist->IsInfinite();
	if (rgnNum>=0)
	{
		char msg[256] = "";
		_snprintfSafe(msg, sizeof(msg), __LOCALIZE_VERFMT("Paste/crop aborted: infinite loop (for region %d at least)!","sws_DLG_165"), rgnNum);
		MessageBox(g_pRgnPlaylistWnd?g_pRgnPlaylistWnd->GetHWND():GetMainHwnd(), msg, __LOCALIZE("S&M - Error","sws_DLG_165"),MB_OK);
		return;
	}

	bool updated = false;
	double prjlen=GetProjectLength(), startPos=prjlen, endPos=prjlen;

	// insert empty space?
	if (_mode==3)
	{
		startPos = endPos = GetCursorPositionEx(NULL);
		if (startPos<prjlen)
		{
			if (_playlist->IsInPlaylist(startPos, false, 0) >=0 ) {
				MessageBox(g_pRgnPlaylistWnd?g_pRgnPlaylistWnd->GetHWND():GetMainHwnd(), 
					__LOCALIZE("Aborted: pasting inside a region which is used in the playlist!","sws_DLG_165"),
					__LOCALIZE("S&M - Error","sws_DLG_165"), MB_OK);
				return;
			}
			if (IsInPlaylists(startPos) >=0 ) {
				if (IDNO == MessageBox(g_pRgnPlaylistWnd?g_pRgnPlaylistWnd->GetHWND():GetMainHwnd(), 
					__LOCALIZE("Warning: pasting inside a region which is used in other playlists!\nThis region will be larger and those playlists will change, are you sure you want to continue?","sws_DLG_165"),
					__LOCALIZE("S&M - Warning","sws_DLG_165"), MB_YESNO))
					return;
			}

			updated = true;
			Undo_BeginBlock2(NULL);
			if (PreventUIRefresh)
				PreventUIRefresh(1);

			InsertSilence(NULL, startPos, _playlist->GetLength());
		}
	}

//	OnStopButton();

	// make sure some envelope options are enabled: move with items + add edge points
	int oldOpt[2] = {-1,-1};
	int* options[2] = {NULL,NULL};
	if (options[0] = (int*)GetConfigVar("envattach")) {
		oldOpt[0] = *options[0];
		*options[0] = 1;
	}
	if (options[1] = (int*)GetConfigVar("env_reduce")) {
		oldOpt[1] = *options[1];
		*options[1] = 2;
	}

	WDL_PtrList_DeleteOnDestroy<MarkerRegion> rgns;
	for (int i=0; i<_playlist->GetSize(); i++)
	{
		if (SNM_PlaylistItem* plItem = _playlist->Get(i))
		{
			int rgnnum, rgncol=0; double rgnpos, rgnend; char* rgnname;
			if (EnumMarkerRegionById(NULL, plItem->m_rgnId, NULL, &rgnpos, &rgnend, &rgnname, &rgnnum, &rgncol))
			{
				WDL_PtrList<void> itemsToKeep;
				if (GetItemsInInterval(&itemsToKeep, rgnpos, rgnend, false))
				{
					if (!updated) { // to do once (for undo stability)
						updated = true;
						Undo_BeginBlock2(NULL);
						if (PreventUIRefresh)
							PreventUIRefresh(1);
					}

					// store regions
					bool found = false;
					for (int k=0; !found && k<rgns.GetSize(); k++)
						found |= (rgns.Get(k)->GetNum() == rgnnum);
					if (!found)
						rgns.Add(new MarkerRegion(true, endPos-startPos, endPos+rgnend-rgnpos-startPos, rgnname, rgnnum, rgncol));

					// store data needed to "unsplit"
					// note: not needed when croping as those items will get removed
					WDL_PtrList_DeleteOnDestroy<SNM_ItemChunk> itemSates;
					if (_mode==2 || _mode==3)
						for (int j=0; j < itemsToKeep.GetSize(); j++)
							itemSates.Add(new SNM_ItemChunk((MediaItem*)itemsToKeep.Get(j)));

					WDL_PtrList<void> splitItems;
					SplitSelectItemsInInterval(NULL, rgnpos, rgnend, false, _mode==2 || _mode==3 ? &splitItems : NULL);

					// REAPER "bug": the last param of ApplyNudge() is ignored although
					// it is used in duplicate mode => use a loop instead
					// note: DupSelItems() is an override of the native ApplyNudge()
					if (plItem->m_cnt>0)
						for (int k=0; k < plItem->m_cnt; k++) {
							DupSelItems(NULL, endPos-rgnpos, &itemsToKeep);
							endPos += (rgnend-rgnpos);
						}

					// "unsplit" items (itemSates & splitItems are empty when croping, see above)
					for (int j=0; j < itemSates.GetSize(); j++)
						if (SNM_ItemChunk* ic = itemSates.Get(j)) {
								SNM_ChunkParserPatcher p(ic->m_item);
								p.SetChunk(ic->m_chunk.Get());
							}
					for (int j=0; j < splitItems.GetSize(); j++)
						if (MediaItem* item = (MediaItem*)splitItems.Get(j))
							if (itemsToKeep.Find(item) < 0)
								DeleteTrackMediaItem(GetMediaItem_Track(item), item);
				}
			}
		}
	}

	// restore options
	if (options[0]) *options[0] = oldOpt[0];
	if (options[1]) *options[1] = oldOpt[1];

	// nothing done..
	if (!updated)
	{
		if (PreventUIRefresh)
			PreventUIRefresh(-1);
		return;
	}

	///////////////////////////////////////////////////////////////////////////
	// append/paste to current project
	if (_mode == 2 || _mode == 3)
	{
//		Main_OnCommand(40289, 0); // unselect all items
		SetEditCurPos2(NULL, endPos, true, false);

		if (PreventUIRefresh)
			PreventUIRefresh(-1);

		UpdateTimeline(); // ruler+arrange

		Undo_EndBlock2(NULL, _mode==2 ? __LOCALIZE("Append playlist to project","sws_undo") : __LOCALIZE("Paste playlist at edit cursor","sws_undo"), UNDO_STATE_ALL);
		return;
	}

	///////////////////////////////////////////////////////////////////////////
	// crop current project

	// dup the playlist (needed when cropping to new project tab)
	SNM_Playlist* dupPlaylist = _mode==1 ? new SNM_Playlist(_playlist) : NULL;

	// crop current project
	GetSet_LoopTimeRange(true, false, &startPos, &endPos, false);
	Main_OnCommand(40049, 0); // crop project to time sel
//	Main_OnCommand(40289, 0); // unselect all items

	// restore (updated) regions
	for (int i=0; i<rgns.GetSize(); i++)
		rgns.Get(i)->AddToProject();

	// cleanup playlists (some other regions may have been removed)
	for (int i=g_pls.Get()->GetSize()-1; i>=0; i--)
		if (SNM_Playlist* pl = g_pls.Get()->Get(i))
			if (pl != _playlist)
				for (int j=0; j<pl->GetSize(); j++)
					if (pl->Get(j) && GetMarkerRegionIndexFromId(NULL, pl->Get(j)->m_rgnId) < 0) {
						g_pls.Get()->Delete(i, true);
						break;
					}
	g_pls.Get()->m_editId = g_pls.Get()->Find(_playlist);
	if (g_pls.Get()->m_editId < 0)
		g_pls.Get()->m_editId = 0; // just in case..

	// crop current proj?
	if (!_mode)
	{
		// clear time sel + edit cursor position
		GetSet_LoopTimeRange(true, false, &g_d0, &g_d0, false);
		SetEditCurPos2(NULL, 0.0, true, false);

		if (PreventUIRefresh)
			PreventUIRefresh(-1);
		UpdateTimeline();

		Undo_EndBlock2(NULL, __LOCALIZE("Crop project to playlist","sws_undo"), UNDO_STATE_ALL);

		if (g_pRgnPlaylistWnd) {
			g_pRgnPlaylistWnd->FillPlaylistCombo();
			g_pRgnPlaylistWnd->Update();
		}
		return;
	}

	///////////////////////////////////////////////////////////////////////////
	// crop to new project tab
	// macro-ish solution because copying the project the proper way (e.g. 
	// copying all track states) is totally unstable
	// note: so the project is "copied" w/o extension data, project bay, etc..

	// store master track
	WDL_FastString mstrStates;
	if (MediaTrack* tr = CSurf_TrackFromID(0, false)) {
		SNM_ChunkParserPatcher p(tr);
		mstrStates.Set(p.GetChunk());
	}

	Main_OnCommand(40296, 0); // select all tracks
	Main_OnCommand(40210, 0); // copy tracks

	if (PreventUIRefresh)
		PreventUIRefresh(-1);

	// trick!
	Undo_EndBlock2(NULL, __LOCALIZE("Crop project to playlist","sws_undo"), UNDO_STATE_ALL);
	if (!Undo_DoUndo2(NULL))
		return;
/*JFB no-op, unfortunately
	CSurf_FlushUndo(true);
*/

	// *** NEW PROJECT TAB***
	Main_OnCommand(40859, 0);

	Undo_BeginBlock2(NULL);

	if (PreventUIRefresh)
		PreventUIRefresh(1);

	Main_OnCommand(40058, 0); // paste item/tracks
	Main_OnCommand(40297, 0); // unselect all tracks
	if (OpenClipboard(GetMainHwnd())) { // clear clipboard
	    EmptyClipboard();
		CloseClipboard();
	}
	if (MediaTrack* tr = CSurf_TrackFromID(0, false)) {
		SNM_ChunkParserPatcher p(tr);
		p.SetChunk(mstrStates.Get());
	}
	// restore regions (in the new project this time)
	for (int i=0; i<rgns.GetSize(); i++)
		rgns.Get(i)->AddToProject();

	// new project: the playlist is empty at this point
	g_pls.Get()->Add(dupPlaylist);
	g_pls.Get()->m_editId = 0;

	if (PreventUIRefresh)
		PreventUIRefresh(-1);
	SNM_UIRefresh(NULL);

	Undo_EndBlock2(NULL, __LOCALIZE("Crop project to playlist","sws_undo"), UNDO_STATE_ALL);

	if (g_pRgnPlaylistWnd) {
		g_pRgnPlaylistWnd->FillPlaylistCombo();
		g_pRgnPlaylistWnd->Update();
	}
}

void AppendPasteCropPlaylist(COMMAND_T* _ct) {
	AppendPasteCropPlaylist(GetPlaylist(), (int)_ct->user);
}


///////////////////////////////////////////////////////////////////////////////

void PlaylistUpdateJob::Perform() {
	if (g_pRgnPlaylistWnd) {
		g_pRgnPlaylistWnd->FillPlaylistCombo();
		g_pRgnPlaylistWnd->Update();
	}
}


///////////////////////////////////////////////////////////////////////////////

// ScheduledJob used because of multi-notifs
void PlaylistMarkerRegionSubscriber::NotifyMarkerRegionUpdate(int _updateFlags) {
	PlaylistResync();
	AddOrReplaceScheduledJob(new PlaylistUpdateJob());
}


///////////////////////////////////////////////////////////////////////////////

static bool ProcessExtensionLine(const char *line, ProjectStateContext *ctx, bool isUndo, struct project_config_extension_t *reg)
{
	LineParser lp(false);
	if (lp.parse(line) || lp.getnumtokens() < 1)
		return false;

	if (!strcmp(lp.gettoken_str(0), "<S&M_RGN_PLAYLIST"))
	{
		MUTEX_PLAYLISTS;
		if (SNM_Playlist* playlist = g_pls.Get()->Add(new SNM_Playlist(lp.gettoken_str(1))))
		{
			int success;
			if (lp.gettoken_int(2, &success) && success)
				g_pls.Get()->m_editId = g_pls.Get()->GetSize()-1;

			char linebuf[SNM_MAX_CHUNK_LINE_LENGTH]="";
			while(true)
			{
				if (!ctx->GetLine(linebuf,sizeof(linebuf)) && !lp.parse(linebuf))
				{
					if (lp.getnumtokens() && lp.gettoken_str(0)[0] == '>')
						break;
					else if (lp.getnumtokens() == 2)
						playlist->Add(new SNM_PlaylistItem(lp.gettoken_int(0), lp.gettoken_int(1)));
				}
				else
					break;
			}
			if (g_pRgnPlaylistWnd) {
				g_pRgnPlaylistWnd->FillPlaylistCombo();
				g_pRgnPlaylistWnd->Update();
			}
			return true;
		}
	}
	return false;
}

static void SaveExtensionConfig(ProjectStateContext *ctx, bool isUndo, struct project_config_extension_t *reg)
{
	MUTEX_PLAYLISTS;
	for (int j=0; j < g_pls.Get()->GetSize(); j++)
	{
		WDL_FastString confStr("<S&M_RGN_PLAYLIST "), escStr;
		makeEscapedConfigString(GetPlaylist(j)->m_name.Get(), &escStr);
		confStr.Append(escStr.Get());
		if (j == g_pls.Get()->m_editId)
			confStr.Append(" 1\n");
		else
			confStr.Append("\n");
		int iHeaderLen = confStr.GetLength();

		for (int i=0; i < GetPlaylist(j)->GetSize(); i++)
			if (SNM_PlaylistItem* item = GetPlaylist(j)->Get(i))
				confStr.AppendFormatted(128,"%d %d\n", item->m_rgnId, item->m_cnt);

		// ignore empty playlists when saving but always take them into account for undo
		if (isUndo || (!isUndo && confStr.GetLength() > iHeaderLen)) {
			confStr.Append(">\n");
			StringToExtensionConfig(&confStr, ctx);
		}
	}
}

static void BeginLoadProjectState(bool isUndo, struct project_config_extension_t *reg)
{
	MUTEX_PLAYLISTS;
	g_pls.Cleanup();
	g_pls.Get()->Empty(true);
	g_pls.Get()->m_editId=0;
}

static project_config_extension_t g_projectconfig = {
	ProcessExtensionLine, SaveExtensionConfig, BeginLoadProjectState, NULL
};


///////////////////////////////////////////////////////////////////////////////

int RegionPlaylistInit()
{
	// load prefs
	g_monitorMode = (GetPrivateProfileInt("RegionPlaylist", "MonitorMode", 0, g_SNM_IniFn.Get()) == 1);
	g_repeatPlaylist = (GetPrivateProfileInt("RegionPlaylist", "Repeat", 0, g_SNM_IniFn.Get()) == 1);
	g_scrollView = (GetPrivateProfileInt("RegionPlaylist", "ScrollView", 1, g_SNM_IniFn.Get()) == 1);
	g_seekPlay = (GetPrivateProfileInt("RegionPlaylist", "SeekPlay", 1, g_SNM_IniFn.Get()) == 1);
	GetPrivateProfileString("RegionPlaylist", "BigFontName", SNM_DYN_FONT_NAME, g_rgnplBigFontName, sizeof(g_rgnplBigFontName), g_SNM_IniFn.Get());

	g_pRgnPlaylistWnd = new SNM_RegionPlaylistWnd();
	if (!g_pRgnPlaylistWnd || !plugin_register("projectconfig", &g_projectconfig))
		return 0;
	return 1;
}

void RegionPlaylistExit()
{
	// save prefs
	WritePrivateProfileString("RegionPlaylist", "MonitorMode", g_monitorMode?"1":"0", g_SNM_IniFn.Get()); 
	WritePrivateProfileString("RegionPlaylist", "Repeat", g_repeatPlaylist?"1":"0", g_SNM_IniFn.Get()); 
	WritePrivateProfileString("RegionPlaylist", "ScrollView", g_scrollView?"1":"0", g_SNM_IniFn.Get()); 
	WritePrivateProfileString("RegionPlaylist", "SeekPlay", g_seekPlay?"1":"0", g_SNM_IniFn.Get()); 
	WritePrivateProfileString("RegionPlaylist", "BigFontName", g_rgnplBigFontName, g_SNM_IniFn.Get());

	DELETE_NULL(g_pRgnPlaylistWnd);
}

void OpenRegionPlaylist(COMMAND_T*) {
	if (g_pRgnPlaylistWnd)
		g_pRgnPlaylistWnd->Show(true, true);
}

int IsRegionPlaylistDisplayed(COMMAND_T*){
	return (g_pRgnPlaylistWnd && g_pRgnPlaylistWnd->IsValidWindow());
}

void ToggleRegionPlaylistLock(COMMAND_T*) {
	if (g_pRgnPlaylistWnd)
		g_pRgnPlaylistWnd->ToggleLock();
}

int IsRegionPlaylistMonitoring(COMMAND_T*) {
	return g_monitorMode;
}
