/******************************************************************************
/ SnM_Windows.cpp
/
/ Copyright (c) 2009-2010 Tim Payne (SWS), JF Bédague 
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
#include "SnM_Actions.h"
#include "SNM_ChunkParserPatcher.h"


///////////////////////////////////////////////////////////////////////////////
// Routing, env windows
//
// Jeffos' note: 
// I know... I'm not happy with this "get window by name" solution either.
//
///////////////////////////////////////////////////////////////////////////////

#ifdef _WIN32
bool toggleShowHideWin(const char * _title) {
	HWND w = FindWindow(NULL, _title);
	if (w != NULL)
	{
		ShowWindow(w, IsWindowVisible(w) ? SW_HIDE : SW_SHOW);
		return true;
	}
	return false;
}

bool closeWin(const char * _title) {
	HWND w = FindWindow(NULL, _title);
	if (w != NULL)
	{
		SendMessage(w, WM_SYSCOMMAND, SC_CLOSE, 0);
		return true;
	}
	return false;
}

void closeOrToggleWindows(bool _chain, bool _fx, bool _routing, bool _env, bool _toggle)
{
	for (int i=0; i <= GetNumTracks(); i++)
	{
		MediaTrack *tr = CSurf_TrackFromID(i, false);
		if (tr)
		{
			char trName[128];
			strcpy(trName, GetTrackInfo((int)tr, NULL));

			//  *** Routing ***
			if (_routing)
			{
				char routingName[128];
				if (!i) strcpy(routingName, "Outputs for Master Track");
				else sprintf(routingName, "Routing for track %d \"%s\"", i, trName);

				if (_toggle) toggleShowHideWin(routingName);
				else closeWin(routingName);
			}

			// *** Env ***
			if (_env)
			{
				char envName[128];
				if (!i) strcpy(envName, "Envelopes for Master Track");
				else sprintf(envName, "Envelopes for track %d \"%s\"", i, trName);

				if (_toggle) toggleShowHideWin(envName);
				else closeWin(envName);
			}

			// *** FX chain & add FX ***
			if (_chain)
			{
				char fxChainName[128];
				char addFXName[128];
				if (strlen(trName) == 0)
				{
					if (!i) strcpy(fxChainName, "FX: Master Track");
					else sprintf(fxChainName, "FX: track %d", i);

					if (!i) strcpy(addFXName, "Add FX to: Master Track");
					else sprintf(addFXName, "Add FX to: track %d ", i); // " " !
				}
				else 
				{
					sprintf(fxChainName, "FX: track %d \"%s\"", i, trName);
					sprintf(addFXName, "Add FX to: track %d \"%s\"", i, trName);
				}
//dbg				MessageBox(0, fxChainName, "dbg", MB_OK);

				if (_toggle &&  *(int*)GetSetMediaTrackInfo(tr, "I_SELECTED", NULL)) 
				{
					if (!toggleShowHideWin(fxChainName))
						showFXChain(tr,0);
//					toggleShowHideWin(addFXName);
				}
				else 
				{
					closeWin(fxChainName);
					closeWin(addFXName);
				}
			}

			// *** floating FXs ***
			if (_fx)
			{
				int nbFx = TrackFX_GetCount(tr);
				for (int j=0; j < nbFx; j++)
				{
					char fxName[128];
					TrackFX_GetFXName(tr, j, fxName, 128);

					char fxDlgName[128];
					if (strlen(trName) == 0)
					{
						if (!i) sprintf(fxDlgName, "%s - Master Track", fxName);
						else sprintf(fxDlgName, "%s - track %d", fxName, i); 
					}
					else sprintf(fxDlgName, "%s - track %d \"%s\"", fxName, i, trName);

					if (!closeWin(fxDlgName))
					{
						char fxDlgName2[128];
						sprintf(fxDlgName2, "BYPASSED - %s", fxDlgName);
						if (!closeWin(fxDlgName2))
						{
							char fxDlgName3[128];
							sprintf(fxDlgName3, "%s [BYPASSED]", fxDlgName);
							if (!closeWin(fxDlgName3))
							{
								char fxDlgName4[128];
								sprintf(fxDlgName4, "BYPASSED - %s [BYPASSED]", fxDlgName);
								closeWin(fxDlgName4);
							}
						}
					}
				}
			}
		}
	}
}

void closeRoutingWindows(COMMAND_T * _c) {
	closeOrToggleWindows(false, false, true, false, false);
}

void closeEnvWindows(COMMAND_T * _c) {
	closeOrToggleWindows(false, false, false, true, false);
}

void closeFloatingFXWindows(COMMAND_T * _c) {
	closeOrToggleWindows(false, true, false, false, false);
}

void closeFXChainsWindows(COMMAND_T * _c) {
	closeOrToggleWindows(true, false, false, false, false);
}

void toggleRoutingWindows(COMMAND_T * _c) {
	closeOrToggleWindows(false, false, true, false, true);
}

void toggleEnvWindows(COMMAND_T * _c) {
	closeOrToggleWindows(false, false, false, true, true);
}

void toggleFXChainsWindows(COMMAND_T * _c) {
	closeOrToggleWindows(true, false, false, false, true);
}

#endif


///////////////////////////////////////////////////////////////////////////////
// FX chain windows
///////////////////////////////////////////////////////////////////////////////

void showFXChain(MediaTrack* _tr, int _fx)
{
	if (_tr && _fx >= 0)
	{
		char pShow[4] = ""; //4 in case there're many FXs
		sprintf(pShow,"%d", _fx+1);
		char pLastSel[4] = "";
		sprintf(pLastSel,"%d", _fx);

		SNM_ChunkParserPatcher p(_tr);
		p.ParsePatch(SNM_SET_CHUNK_CHAR, 2, "FXCHAIN", "LASTSEL",2,0,1,pLastSel) >= 0 &&
		p.ParsePatch(SNM_SET_CHUNK_CHAR, 2, "FXCHAIN", "SHOW",2,0,1,pShow) >= 0;
	}
}

void showFXChain(COMMAND_T* _ct) 
{
	int focusedFX = (int)_ct->user; 
	for (int i = 0; i <= GetNumTracks(); i++)
	{
		MediaTrack* tr = CSurf_TrackFromID(i, false); // include master
		if (tr && *(int*)GetSetMediaTrackInfo(tr, "I_SELECTED", NULL))
			showFXChain(tr, (focusedFX == -1 ? getSelectedFX(tr) : focusedFX));
	}
	// no undo
}


///////////////////////////////////////////////////////////////////////////////
// FX windows
///////////////////////////////////////////////////////////////////////////////

void floatFX(MediaTrack* _tr, int _fx)
{
	if (_tr && _fx >= 0)
	{
		char pShow[4] = "";
		sprintf(pShow,"%d", _fx+1);
		char pLastSel[4] = "";
		sprintf(pLastSel,"%d", _fx);

		SNM_ChunkParserPatcher p(_tr);
		char floating[6] = "FLOAT";
		char nonFloating[9] = "FLOATPOS";

		// perf. remark: this would be better to do a dedicated parser/patcher 
		// to do the job in one go (i.e. inheriting SNM_ChunkParserPatcher for SAX-ish parsing)
		p.ParsePatch(SNM_SET_CHUNK_CHAR,2,"FXCHAIN","LASTSEL",2,0,1,&pLastSel); // update sel. FX
		p.ParsePatch(SNM_SETALL_CHUNK_CHAR_EXCEPT,2,"FXCHAIN","FLOAT",5,255,0,&nonFloating); //unfloat all

		// set a default pos (if needed)
		char posx[6] = "";
		bool setPos = false;
		if (p.Parse(SNM_GET_CHUNK_CHAR,2,"FXCHAIN","FLOATPOS",5,_fx,1,posx) > 0)
			if (!strcmp(posx, "0")) // don't scratch user's pos !
				setPos = (p.ParsePatch(SNM_REPLACE_SUBCHUNK,2,"FXCHAIN","FLOATPOS",-1,_fx,-1,(void*)"FLOAT 300 300 300 300\n") > 0);
		if (!setPos)
			p.ParsePatch(SNM_SETALL_CHUNK_CHAR_EXCEPT,2,"FXCHAIN","FLOATPOS",5,_fx,0,&nonFloating, &floating);
	}
	// no undo
}

// not used for the moment: REAPER doesn't obey!
void unfloatFX(MediaTrack* _tr, int _fx)
{
	if (_tr && _fx >= 0)
	{
		SNM_ChunkParserPatcher p(_tr);
		// perf. remark: this would be better to do a dedicated parser/patcher 
		// to do the job in one go (i.e. inheriting SNM_ChunkParserPatcher for SAX-ish parsing)
		char tmp[6] = "";
		char nonFloating[9] = "FLOATPOS";
		if (p.Parse(SNM_GET_CHUNK_CHAR,2,"FXCHAIN","FLOAT",5,_fx,0,tmp) > 0)
			p.ParsePatch(SNM_SET_CHUNK_CHAR,2,"FXCHAIN","FLOAT",5,_fx,0,&nonFloating); //unfloat
		ShowConsoleMsg(p.GetChunk()->Get());
	}
}

void floatFX(COMMAND_T* _ct) 
{
	int focusedFX = (int)_ct->user; 
	for (int i = 0; i <= GetNumTracks(); i++)
	{
		MediaTrack* tr = CSurf_TrackFromID(i, false);
		if (tr && *(int*)GetSetMediaTrackInfo(tr, "I_SELECTED", NULL))
			floatFX(tr, (focusedFX == -1 ? getSelectedFX(tr) : focusedFX));
	}
}

void unfloatFX(COMMAND_T* _ct) 
{
	int focusedFX = (int)_ct->user; 
	for (int i = 0; i <= GetNumTracks(); i++)
	{
		MediaTrack* tr = CSurf_TrackFromID(i, false);
		if (tr && *(int*)GetSetMediaTrackInfo(tr, "I_SELECTED", NULL))
			unfloatFX(tr, (focusedFX == -1 ? getSelectedFX(tr) : focusedFX));
	}
}

void setMainWindowActive(COMMAND_T* _ct) {
	SetForegroundWindow(GetMainHwnd()); 
}