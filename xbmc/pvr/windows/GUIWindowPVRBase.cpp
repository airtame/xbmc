/*
 *      Copyright (C) 2012-2013 Team XBMC
 *      http://xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#include "GUIWindowPVRBase.h"

#include "Application.h"
#include "ApplicationMessenger.h"
#include "dialogs/GUIDialogNumeric.h"
#include "dialogs/GUIDialogKaiToast.h"
#include "dialogs/GUIDialogOK.h"
#include "dialogs/GUIDialogYesNo.h"
#include "filesystem/StackDirectory.h"
#include "guilib/Key.h"
#include "guilib/GUIMessage.h"
#include "guilib/GUIWindowManager.h"
#include "dialogs/GUIDialogKaiToast.h"
#include "dialogs/GUIDialogSelect.h"
#include "pvr/PVRManager.h"
#include "pvr/addons/PVRClients.h"
#include "pvr/dialogs/GUIDialogPVRGuideInfo.h"
#include "pvr/dialogs/GUIDialogPVRRecordingInfo.h"
#include "pvr/timers/PVRTimers.h"
#include "epg/Epg.h"
#include "epg/GUIEPGGridContainer.h"
#include "settings/MediaSettings.h"
#include "settings/Settings.h"
#include "threads/SingleLock.h"
#include "utils/StringUtils.h"
#include "utils/Observer.h"

using namespace std;
using namespace PVR;
using namespace EPG;

CGUIWindowPVRBase::CGUIWindowPVRBase(bool bRadio, int id, const std::string &xmlFile) :
  CGUIMediaWindow(id, xmlFile.c_str()),
  m_bRadio(bRadio)
{
}

CGUIWindowPVRBase::~CGUIWindowPVRBase(void)
{
}

void CGUIWindowPVRBase::Notify(const Observable &obs, const ObservableMessage msg)
{
  CGUIMessage m(GUI_MSG_REFRESH_LIST, GetID(), msg);
  CApplicationMessenger::Get().SendGUIMessage(m);
}

bool CGUIWindowPVRBase::OnAction(const CAction &action)
{
  switch (action.GetID())
  {
    case ACTION_PREVIOUS_CHANNELGROUP:
    case ACTION_NEXT_CHANNELGROUP:
      // switch to next or previous group
      SetGroup(ACTION_NEXT_CHANNELGROUP ? m_group->GetNextGroup() : m_group->GetPreviousGroup());
      return true;
  }

  return CGUIMediaWindow::OnAction(action);
}

bool CGUIWindowPVRBase::OnBack(int actionID)
{
  if (actionID == ACTION_NAV_BACK)
  {
    // don't call CGUIMediaWindow as it will attempt to go to the parent folder which we don't want.
    if (GetPreviousWindow() != WINDOW_FULLSCREEN_LIVETV)
      g_windowManager.ActivateWindow(WINDOW_HOME);
    else
      return CGUIWindow::OnBack(actionID);
  }
  return CGUIMediaWindow::OnBack(actionID);
}

void CGUIWindowPVRBase::OnInitWindow(void)
{
  if (!g_PVRManager.IsStarted() || !g_PVRClients->HasConnectedClients())
  {
    g_windowManager.PreviousWindow();
    CGUIDialogKaiToast::QueueNotification(CGUIDialogKaiToast::Warning,
        g_localizeStrings.Get(19045),
        g_localizeStrings.Get(19044));
    return;
  }

  CGUIMediaWindow::OnInitWindow();
}

bool CGUIWindowPVRBase::OnMessage(CGUIMessage& message)
{
  switch (message.GetMessage())
  {
    case GUI_MSG_WINDOW_INIT:
    {
      m_group = g_PVRManager.GetPlayingGroup(m_bRadio);
      SetProperty("IsRadio", m_bRadio ? "true" : "");
    }
    break;
      
    case GUI_MSG_CLICKED:
    {
      switch (message.GetSenderId())
      {
        case CONTROL_BTNCHANNELGROUPS:
          return OpenGroupSelectionDialog();
      }
    }
    break;
  }

  return CGUIMediaWindow::OnMessage(message);
}

void CGUIWindowPVRBase::SetInvalid()
{
  VECFILEITEMS items = m_vecItems->GetList();
  for (VECFILEITEMS::iterator it = items.begin(); it != items.end(); ++it)
    (*it)->SetInvalid();
  CGUIMediaWindow::SetInvalid();
}

bool CGUIWindowPVRBase::OpenGroupSelectionDialog(void)
{
  CGUIDialogSelect *dialog = (CGUIDialogSelect*)g_windowManager.GetWindow(WINDOW_DIALOG_SELECT);
  if (!dialog)
    return false;

  CFileItemList options;
  g_PVRChannelGroups->Get(m_bRadio)->GetGroupList(&options);

  dialog->Reset();
  dialog->SetHeading(g_localizeStrings.Get(19146));
  dialog->SetItems(&options);
  dialog->SetMultiSelection(false);
  dialog->SetSelected(m_group->GroupName());
  dialog->DoModal();

  if (!dialog->IsConfirmed())
    return false;

  const CFileItemPtr item = dialog->GetSelectedItem();
  if (!item)
    return false;

  SetGroup(g_PVRChannelGroups->Get(m_bRadio)->GetByName(item->m_strTitle));

  return true;
}

CPVRChannelGroupPtr CGUIWindowPVRBase::GetGroup(void)
{
  CSingleLock lock(m_critSection);
  return m_group;
}

void CGUIWindowPVRBase::SetGroup(CPVRChannelGroupPtr group)
{
  CSingleLock lock(m_critSection);
  if (!group)
    return;

  if (m_group != group)
  {
    if (m_group)
      m_group->UnregisterObserver(this);
    m_group = group;
    // we need to register the window to receive changes from the new group
    m_group->RegisterObserver(this);
    g_PVRManager.SetPlayingGroup(m_group);
    Refresh();
  }
}

bool CGUIWindowPVRBase::PlayFile(CFileItem *item, bool bPlayMinimized /* = false */)
{
  if (item->m_bIsFolder)
  {
    return false;
  }

  if (item->GetPath() == g_application.CurrentFile())
  {
    CGUIMessage msg(GUI_MSG_FULLSCREEN, 0, GetID());
    g_windowManager.SendMessage(msg);
    return true;
  }

  CMediaSettings::Get().SetVideoStartWindowed(bPlayMinimized);

  if (item->HasPVRRecordingInfoTag())
  {
    return PlayRecording(item, bPlayMinimized);
  }
  else
  {
    bool bSwitchSuccessful(false);

    CPVRChannel *channel = item->HasPVRChannelInfoTag() ? item->GetPVRChannelInfoTag() : NULL;

    if (channel && g_PVRManager.CheckParentalLock(*channel))
    {
      /* try a fast switch */
      if (channel && (g_PVRManager.IsPlayingTV() || g_PVRManager.IsPlayingRadio()) &&
         (channel->IsRadio() == g_PVRManager.IsPlayingRadio()))
      {
        if (channel->StreamURL().empty())
          bSwitchSuccessful = g_application.m_pPlayer->SwitchChannel(*channel);
      }

      if (!bSwitchSuccessful)
      {
        CApplicationMessenger::Get().PlayFile(*item, false);
        return true;
      }
    }

    if (!bSwitchSuccessful)
    {
      CStdString channelName = g_localizeStrings.Get(19029); // Channel
      if (channel)
        channelName = channel->ChannelName();
      CStdString msg = StringUtils::Format(g_localizeStrings.Get(19035).c_str(), channelName.c_str()); // CHANNELNAME could not be played. Check the log for details.

      CGUIDialogKaiToast::QueueNotification(CGUIDialogKaiToast::Error,
              g_localizeStrings.Get(19166), // PVR information
              msg);
      return false;
    }
  }

  return true;
}

bool CGUIWindowPVRBase::StartRecordFile(const CFileItem &item)
{
  if (!item.HasEPGInfoTag())
    return false;

  const CEpgInfoTag *tag = item.GetEPGInfoTag();
  CPVRChannelPtr channel;
  if (tag)
    channel = tag->ChannelTag();

  if (!channel || !g_PVRManager.CheckParentalLock(*channel))
    return false;

  CFileItemPtr timer = g_PVRTimers->GetTimerForEpgTag(&item);
  if (timer && timer->HasPVRTimerInfoTag())
  {
    CGUIDialogOK::ShowAndGetInput(19033,19034,0,0);
    return false;
  }

  // ask for confirmation before starting a timer
  CGUIDialogYesNo* pDialog = (CGUIDialogYesNo*)g_windowManager.GetWindow(WINDOW_DIALOG_YES_NO);
  if (!pDialog)
    return false;
  pDialog->SetHeading(264);
  pDialog->SetLine(0, tag->PVRChannelName());
  pDialog->SetLine(1, "");
  pDialog->SetLine(2, tag->Title());
  pDialog->DoModal();

  if (!pDialog->IsConfirmed())
    return false;

  CPVRTimerInfoTag *newTimer = CPVRTimerInfoTag::CreateFromEpg(*tag);
  bool bReturn(false);
  if (newTimer)
  {
    bReturn = g_PVRTimers->AddTimer(*newTimer);
    delete newTimer;
  }
  return bReturn;
}

bool CGUIWindowPVRBase::StopRecordFile(const CFileItem &item)
{
  if (!item.HasEPGInfoTag())
    return false;

  const CEpgInfoTag *tag = item.GetEPGInfoTag();
  if (!tag || !tag->HasPVRChannel())
    return false;

  CFileItemPtr timer = g_PVRTimers->GetTimerForEpgTag(&item);
  if (!timer || !timer->HasPVRTimerInfoTag() || timer->GetPVRTimerInfoTag()->m_bIsRepeating)
    return false;

  return g_PVRTimers->DeleteTimer(*timer);
}

bool CGUIWindowPVRBase::PlayRecording(CFileItem *item, bool bPlayMinimized /* = false */)
{
  if (!item->HasPVRRecordingInfoTag())
    return false;

  CStdString stream = item->GetPVRRecordingInfoTag()->m_strStreamURL;
  if (stream.empty())
  {
    CApplicationMessenger::Get().PlayFile(*item, false);
    return true;
  }

  /* Isolate the folder from the filename */
  size_t found = stream.find_last_of("/");
  if (found == CStdString::npos)
    found = stream.find_last_of("\\");

  if (found != CStdString::npos)
  {
    /* Check here for asterisk at the begin of the filename */
    if (stream[found+1] == '*')
    {
      /* Create a "stack://" url with all files matching the extension */
      CStdString ext = URIUtils::GetExtension(stream);
      CStdString dir = stream.substr(0, found).c_str();

      CFileItemList items;
      XFILE::CDirectory::GetDirectory(dir, items);
      items.Sort(SortByFile, SortOrderAscending);

      vector<int> stack;
      for (int i = 0; i < items.Size(); ++i)
      {
        if (URIUtils::HasExtension(items[i]->GetPath(), ext))
          stack.push_back(i);
      }

      if (stack.empty())
      {
        /* If we have a stack change the path of the item to it */
        XFILE::CStackDirectory dir;
        CStdString stackPath = dir.ConstructStackPath(items, stack);
        item->SetPath(stackPath);
      }
    }
    else
    {
      /* If no asterisk is present play only the given stream URL */
      item->SetPath(stream);
    }
  }
  else
  {
    CLog::Log(LOGERROR, "CGUIWindowPVRCommon - %s - can't open recording: no valid filename", __FUNCTION__);
    CGUIDialogOK::ShowAndGetInput(19033,0,19036,0);
    return false;
  }

  CApplicationMessenger::Get().PlayFile(*item, false);

  return true;
}

void CGUIWindowPVRBase::ShowRecordingInfo(CFileItem *item)
{
  CGUIDialogPVRRecordingInfo* pDlgInfo = (CGUIDialogPVRRecordingInfo*)g_windowManager.GetWindow(WINDOW_DIALOG_PVR_RECORDING_INFO);
  if (item->IsPVRRecording() && pDlgInfo)
  {
    pDlgInfo->SetRecording(item);
    pDlgInfo->DoModal();
  }
}

void CGUIWindowPVRBase::ShowEPGInfo(CFileItem *item)
{
  CFileItem *tag = NULL;
  bool bHasChannel(false);
  CPVRChannel channel;
  if (item->IsEPG())
  {
    tag = new CFileItem(*item);
    if (item->GetEPGInfoTag()->HasPVRChannel())
    {
      channel = *item->GetEPGInfoTag()->ChannelTag();
      bHasChannel = true;
    }
  }
  else if (item->IsPVRChannel())
  {
    CEpgInfoTag epgnow;
    channel = *item->GetPVRChannelInfoTag();
    bHasChannel = true;
    if (!item->GetPVRChannelInfoTag()->GetEPGNow(epgnow))
    {
      CGUIDialogOK::ShowAndGetInput(19033,0,19055,0);
      return;
    }
    tag = new CFileItem(epgnow);
  }

  CGUIDialogPVRGuideInfo* pDlgInfo = (CGUIDialogPVRGuideInfo*)g_windowManager.GetWindow(WINDOW_DIALOG_PVR_GUIDE_INFO);
  if (tag && (!bHasChannel || g_PVRManager.CheckParentalLock(channel)) && pDlgInfo)
  {
    pDlgInfo->SetProgInfo(tag);
    pDlgInfo->DoModal();
  }

  delete tag;
}

bool CGUIWindowPVRBase::ActionInputChannelNumber(int input)
{
  CStdString strInput = StringUtils::Format("%i", input);
  if (CGUIDialogNumeric::ShowAndGetNumber(strInput, g_localizeStrings.Get(19103)))
  {
    int iChannelNumber = atoi(strInput.c_str());
    if (iChannelNumber >= 0)
    {
      int itemIndex = 0;
      VECFILEITEMS items = m_vecItems->GetList();
      for (VECFILEITEMS::iterator it = items.begin(); it != items.end(); ++it)
      {
        if(((*it)->HasPVRChannelInfoTag() && (*it)->GetPVRChannelInfoTag()->ChannelNumber() == iChannelNumber) ||
           ((*it)->HasEPGInfoTag() && (*it)->GetEPGInfoTag()->HasPVRChannel() && (*it)->GetEPGInfoTag()->PVRChannelNumber() == iChannelNumber))
        {
          // different handling for guide grid
          if ((GetID() == WINDOW_TV_GUIDE || GetID() == WINDOW_RADIO_GUIDE) &&
              m_viewControl.GetCurrentControl() == GUIDE_VIEW_TIMELINE)
          {
            CGUIEPGGridContainer* epgGridContainer = (CGUIEPGGridContainer*) GetControl(m_viewControl.GetCurrentControl());
            epgGridContainer->SetChannel((*(*it)->GetEPGInfoTag()->ChannelTag()));
          }
          else
            m_viewControl.SetSelectedItem(itemIndex);
          return true;
        }
        itemIndex++;
      }
    }
  }

  return false;
}

bool CGUIWindowPVRBase::ActionPlayChannel(CFileItem *item)
{
  bool bReturn = false;

  if (item->GetPath() == "pvr://channels/.add.channel")
  {
    /* show "add channel" dialog */
    CGUIDialogOK::ShowAndGetInput(19033,0,19038,0);
    bReturn = true;
  }
  else
  {
    /* open channel */
    bReturn = PlayFile(item, CSettings::Get().GetBool("pvrplayback.playminimized"));
  }

  return bReturn;
}

bool CGUIWindowPVRBase::ActionPlayEpg(CFileItem *item)
{
  if (!item || !item->HasEPGInfoTag())
    return false;

  CPVRChannelPtr channel;
  CEpgInfoTag *epgTag = item->GetEPGInfoTag();
  if (epgTag->HasPVRChannel())
    channel = epgTag->ChannelTag();

  if (!channel || !g_PVRManager.CheckParentalLock(*channel))
    return false;

  CFileItem fileItem;
  if (epgTag->HasRecording())
    fileItem = CFileItem(*epgTag->Recording());
  else
    fileItem = CFileItem(*channel);

  g_application.SwitchToFullScreen();
  if (!PlayFile(&fileItem))
  {
    // CHANNELNAME could not be played. Check the log for details.
    CStdString msg = StringUtils::Format(g_localizeStrings.Get(19035).c_str(), channel->ChannelName().c_str());
    CGUIDialogOK::ShowAndGetInput(19033, 0, msg, 0);
    return false;
  }

  return true;
}

bool CGUIWindowPVRBase::ActionDeleteChannel(CFileItem *item)
{
  CPVRChannel *channel = item->GetPVRChannelInfoTag();

  /* check if the channel tag is valid */
  if (!channel || channel->ChannelNumber() <= 0)
    return false;

  /* show a confirmation dialog */
  CGUIDialogYesNo* pDialog = (CGUIDialogYesNo*) g_windowManager.GetWindow(WINDOW_DIALOG_YES_NO);
  if (!pDialog)
    return false;
  pDialog->SetHeading(19039);
  pDialog->SetLine(0, "");
  pDialog->SetLine(1, channel->ChannelName());
  pDialog->SetLine(2, "");
  pDialog->DoModal();

  /* prompt for the user's confirmation */
  if (!pDialog->IsConfirmed())
    return false;

  g_PVRChannelGroups->GetGroupAll(channel->IsRadio())->RemoveFromGroup(*channel);
  Refresh(true);

  return true;
}

bool CGUIWindowPVRBase::ActionDeleteRecording(CFileItem *item)
{
  bool bReturn = false;

  /* check if the recording tag is valid */
  CPVRRecording *recTag = (CPVRRecording *) item->GetPVRRecordingInfoTag();
  if (!recTag || recTag->m_strRecordingId.empty())
    return bReturn;

  /* show a confirmation dialog */
  CGUIDialogYesNo* pDialog = (CGUIDialogYesNo*)g_windowManager.GetWindow(WINDOW_DIALOG_YES_NO);
  if (!pDialog)
    return bReturn;

  pDialog->SetHeading(122); // Confirm delete
  pDialog->SetLine(0, item->m_bIsFolder ? 19113 : 19112); // Are you sure?
  pDialog->SetLine(1, "");
  pDialog->SetLine(2, item->GetLabel());
  pDialog->SetChoice(1, 117); // Delete

  /* prompt for the user's confirmation */
  pDialog->DoModal();
  if (!pDialog->IsConfirmed())
    return bReturn;

  /* delete the recording */
  if (g_PVRRecordings->Delete(*item))
  {
    g_PVRManager.TriggerRecordingsUpdate();
    bReturn = true;
  }

  return bReturn;
}

bool CGUIWindowPVRBase::ActionRecord(CFileItem *item)
{
  bool bReturn = false;

  CEpgInfoTag *epgTag = item->GetEPGInfoTag();
  if (!epgTag)
    return bReturn;

  CPVRChannelPtr channel = epgTag->ChannelTag();
  if (!channel || !g_PVRManager.CheckParentalLock(*channel))
    return bReturn;

  if (epgTag->Timer() == NULL)
  {
    /* create a confirmation dialog */
    CGUIDialogYesNo* pDialog = (CGUIDialogYesNo*) g_windowManager.GetWindow(WINDOW_DIALOG_YES_NO);
    if (!pDialog)
      return bReturn;

    pDialog->SetHeading(264);
    pDialog->SetLine(0, "");
    pDialog->SetLine(1, epgTag->Title());
    pDialog->SetLine(2, "");
    pDialog->DoModal();

    /* prompt for the user's confirmation */
    if (!pDialog->IsConfirmed())
      return bReturn;

    CPVRTimerInfoTag *newTimer = CPVRTimerInfoTag::CreateFromEpg(*epgTag);
    if (newTimer)
    {
      bReturn = g_PVRTimers->AddTimer(*newTimer);
      delete newTimer;
    }
    else
    {
      bReturn = false;
    }
  }
  else
  {
    CGUIDialogOK::ShowAndGetInput(19033,19034,0,0);
    bReturn = true;
  }

  return bReturn;
}

bool CGUIWindowPVRBase::UpdateEpgForChannel(CFileItem *item)
{
  CPVRChannel *channel = item->GetPVRChannelInfoTag();
  CEpg *epg = channel->GetEPG();
  if (!epg)
    return false;

  epg->ForceUpdate();
  return true;
}

bool CGUIWindowPVRBase::Update(const std::string &strDirectory, bool updateFilterPath /* = true */)
{
  return CGUIMediaWindow::Update(strDirectory, updateFilterPath);
}

void CGUIWindowPVRBase::UpdateButtons(void)
{
  CGUIMediaWindow::UpdateButtons();
  SET_CONTROL_LABEL(CONTROL_BTNCHANNELGROUPS, g_localizeStrings.Get(19141) + ": " + (m_group->GroupType() == PVR_GROUP_TYPE_INTERNAL ? g_localizeStrings.Get(19287) : m_group->GroupName()));
}
