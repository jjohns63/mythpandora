/*
Copyright (c) 2010
	Doug Turner < dougt@dougt.org >

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

// POSIX headers
#include <unistd.h>
#include <assert.h>

#include <QUrl>

// MythTV headers
#include "mythuibutton.h"
#include "mythuitext.h"
#include "mythmainwindow.h"
#include "mythcontext.h"
#include "mythdirs.h"
#include "lcddevice.h"

// MythPandora headers
#include "mythpandora.h"
#include "player.h"

char tlsFingerprint[20] = { 0xD9, 0x98, 0x0B, 0xA2, 0xCC, 0x0F, 0x97, 0xBB, \
    0x03, 0x82, 0x2C, 0x62, 0x11, 0xEA, 0xEA, 0x4A, 0x06, 0xEE, 0xF4, 0x27 };

MythPianoService::MythPianoService()
  : m_Piano(NULL),
    m_PlayerThread(NULL),
    m_AudioOutput(NULL),
    m_Playlist(NULL),
    m_CurrentStation(NULL),
    m_CurrentSong(NULL),
    m_Listener(NULL),
    m_Timer(NULL)
{
    if (class LCD *lcd = LCD::Get())
    {
        lcd->switchToTime();
        lcd->setFunctionLEDs(FUNC_MUSIC, true);
    }
}

MythPianoService::~MythPianoService()
{
    // This really should have already been deleted by StopPlayback()
    assert(!m_AudioOutput);

    if (class LCD *lcd = LCD::Get())
    {
        lcd->switchToTime();
        lcd->setFunctionLEDs(FUNC_MUSIC, false);
    }

    if (m_Piano)
      Logout();
}

void
MythPianoService::SetMessageListener(MythPianoServiceListener* listener)
{
  m_Listener = listener;
}

void
MythPianoService::RemoveMessageListener(MythPianoServiceListener* listener)
{
  if (listener == m_Listener)
    m_Listener = NULL;
}

void MythPianoService::BroadcastMessage(const char *format, ...)
{
  QString buffer;
  va_list args;
  va_start(args, format);
  buffer.vsprintf(format, args);
  va_end(args);

    printf("**** MythPianoService: %s\n", buffer.ascii());

  if (m_Listener)
    m_Listener->RecvMessage(buffer.ascii());
}

void MythPianoService::PauseToggle()
{
  if (pthread_mutex_trylock(&m_Player.pauseMutex) == EBUSY) {
    pthread_mutex_unlock(&m_Player.pauseMutex);
  }
}

void MythPianoService::Logout()
{
  PianoDestroy(m_Piano);
  m_Piano = NULL;

  PianoDestroyPlaylist(m_Playlist);
  m_Playlist = NULL;
  m_CurrentSong = NULL;

  // Leak?
  m_CurrentStation = NULL;

  WaitressFree (&m_Waith);
  gnutls_global_deinit ();
}

int MythPianoService::Login()
{
  m_Piano = (PianoHandle_t*) malloc(sizeof(PianoHandle_t));

  gnutls_global_init();
  PianoInit (m_Piano);

  WaitressInit (&m_Waith);
  m_Waith.url.host = strdup (PIANO_RPC_HOST);
  m_Waith.url.tls = true;
  m_Waith.tlsFingerprint = tlsFingerprint;

  memset (&m_Player, 0, sizeof (m_Player));


  QString username = gCoreContext->GetSetting("pandora-username");
  QString password = gCoreContext->GetSetting("pandora-password");

  //wtf really?
  char* usernameBuff = strndup(username.toUtf8().data(), 1024);
  char* passwordBuff = strndup(password.toUtf8().data(), 1024);

  PianoRequestDataLogin_t reqData;
  reqData.user = usernameBuff;
  reqData.password = passwordBuff;
  reqData.step = 0;

  PianoReturn_t pRet;
  WaitressReturn_t wRet;
  BroadcastMessage("Login... ");
  if (! PianoCall (PIANO_REQUEST_LOGIN, &reqData, &pRet, &wRet)) {
    return -1;
  }

  // wtf
  free(usernameBuff);
  free(passwordBuff);

  if (! PianoCall (PIANO_REQUEST_GET_STATIONS, &reqData, &pRet, &wRet)) {
    return -1;
  }

  m_CurrentStation = m_Piano->stations;
  return 0;
}

void MythPianoService::GetPlaylist()
{
  PianoReturn_t pRet;
  WaitressReturn_t wRet;
  PianoRequestDataGetPlaylist_t reqData;
  reqData.station = m_CurrentStation;
  reqData.format = PIANO_AF_AACPLUS;

  BroadcastMessage("Receiving new playlist... ");
  if (!PianoCall(PIANO_REQUEST_GET_PLAYLIST, &reqData, &pRet, &wRet)) {
    m_CurrentStation = NULL;
    m_CurrentSong = NULL;
  } else {
    m_Playlist = reqData.retPlaylist; 
    m_CurrentSong = m_Playlist;
    if (m_Playlist == NULL) {
      BroadcastMessage("No tracks left.\n");
      m_CurrentStation = NULL;
      m_CurrentSong = NULL;
    }
  }
}

static void WriteAudioCallback(void* ctx, char* samples, size_t bytes)
{
  MythPianoService* m = (MythPianoService*)ctx;
  m->WriteAudio(samples, bytes);
}

void MythPianoService::StartPlayback()
{
  BroadcastMessage("Starting playback");

  if (m_Playlist == NULL) {
    BroadcastMessage("Empty playlist");
    return;
  }

  if (m_Player.mode != audioPlayer::PLAYER_FREED &&
      m_Player.mode != audioPlayer::PLAYER_FINISHED_PLAYBACK) {
    BroadcastMessage("So sorry, We thinkg we are already playing.  Try again (%d).", m_Player.mode);
    return;
  }


  memset (&m_Player, 0, sizeof (m_Player));
  WaitressInit (&m_Player.waith);
  WaitressSetUrl (&m_Player.waith, m_CurrentSong->audioUrl);
  m_Player.waith.url.tls = true;
  m_Player.waith.tlsFingerprint = tlsFingerprint;
  m_Player.gain = m_CurrentSong->fileGain;
  m_Player.audioFormat = m_CurrentSong->audioFormat;
  m_Player.mode = audioPlayer::PLAYER_STARTING;
  m_Player.writer = &WriteAudioCallback;
  m_Player.writerCtx = (void*) this;

  pthread_create (&m_PlayerThread,
		  NULL,
		  BarPlayerThread,
		  &m_Player);

  BroadcastMessage("New Song");

  if (class LCD *lcd = LCD::Get())
  {
    lcd->switchToMusic(m_CurrentSong->artist,
                       m_CurrentSong->album,
                       m_CurrentSong->title);
  }

  if (m_Timer) {
    m_Timer->stop();
    m_Timer->disconnect();
    delete m_Timer;
  }
  m_Timer = new QTimer(this);
  connect(m_Timer, SIGNAL(timeout()), this, SLOT(heartbeat()));
  m_Timer->start(1000);
}

void
MythPianoService::StopPlayback()
{
  if (m_Timer) {
    m_Timer->stop();
    m_Timer->disconnect();
    delete m_Timer;
    m_Timer = NULL;
  }

  if (m_PlayerThread) {
    m_Player.doQuit = true;
    pthread_mutex_unlock(&m_Player.pauseMutex);
    pthread_join(m_PlayerThread, NULL);

    m_PlayerThread = NULL;
  }

  if (m_AudioOutput) {
    delete m_AudioOutput;
    m_AudioOutput = NULL;
  }

  if (class LCD *lcd = LCD::Get())
  {
    lcd->switchToTime();
  }
}

int
MythPianoService::Volume()
{
  // 0-100;
  if (m_AudioOutput)
    return m_AudioOutput->GetCurrentVolume();
  return 0;
}

void
MythPianoService::VolumeUp()
{
  if (m_AudioOutput)
    m_AudioOutput->AdjustCurrentVolume(2);
}

void
MythPianoService::VolumeDown()
{
  if (m_AudioOutput)
    m_AudioOutput->AdjustCurrentVolume(-2);
}

void
MythPianoService::ToggleMute()
{
  if (m_AudioOutput)
    m_AudioOutput->ToggleMute();
}

void
MythPianoService::NextSong()
{
    StopPlayback();

    if (m_Playlist != NULL) {
      m_CurrentSong = m_CurrentSong->next;
    }

    if (m_CurrentSong == NULL) {
      GetPlaylist();
    }

    StartPlayback();
}

void
MythPianoService::heartbeat(void)
{
  if (m_Player.mode >= audioPlayer::PLAYER_FINISHED_PLAYBACK ||
      m_Player.mode == audioPlayer::PLAYER_FREED) {
    
    if (m_Playlist != NULL) {
      m_CurrentSong = m_CurrentSong->next;
    }

    if (m_CurrentSong == NULL) {
      GetPlaylist();
    }

    StartPlayback();
  }
}

WaitressReturn_t 
MythPianoService::PianoHttpRequest(WaitressHandle_t *waith,
				   PianoRequest_t *req) {
  waith->extraHeaders = "Content-Type: text/xml\r\n";
  waith->postData = req->postData;
  waith->method = WAITRESS_METHOD_POST;
  waith->url.path = req->urlPath;

  return WaitressFetchBuf (waith, &req->responseData);
}

int
MythPianoService::PianoCall(PianoRequestType_t type,
			    void *data,
			    PianoReturn_t *pRet,
			    WaitressReturn_t *wRet) {
  if (!m_Piano)
    return -1;

  PianoRequest_t req;
  memset (&req, 0, sizeof (req));

  /* repeat as long as there are http requests to do */
  do {
    req.data = data;

    *pRet = PianoRequest (m_Piano, &req, type);

    if (*pRet != PIANO_RET_OK) {
      BroadcastMessage("Error: %s\n", PianoErrorToStr (*pRet));
      PianoDestroyRequest (&req);
      return 0;
    }

    *wRet = PianoHttpRequest(&m_Waith, &req);
    if (*wRet != WAITRESS_RET_OK) {
      BroadcastMessage ("Network error: %s\n", WaitressErrorToStr (*wRet));
      if (req.responseData != NULL) {
	free (req.responseData);
      }
      PianoDestroyRequest (&req);
      return 0;
    }

    *pRet = PianoResponse (m_Piano, &req);
    if (*pRet != PIANO_RET_CONTINUE_REQUEST) {
      /* checking for request type avoids infinite loops */
      if (*pRet == PIANO_RET_AUTH_TOKEN_INVALID &&
	  type != PIANO_REQUEST_LOGIN) {
	/* reauthenticate */
        PianoReturn_t authpRet;
        WaitressReturn_t authwRet;
        PianoRequestDataLogin_t reqData;

        QString username = gCoreContext->GetSetting("pandora-username");
        QString password = gCoreContext->GetSetting("pandora-password");

        //wtf really?
        char* usernameBuff = strndup(username.toUtf8().data(), 1024);
        char* passwordBuff = strndup(password.toUtf8().data(), 1024);

        reqData.user = usernameBuff;
        reqData.password = passwordBuff;
        reqData.step = 0;

        BroadcastMessage ("Reauthentication required... ");
        if (!PianoCall(PIANO_REQUEST_LOGIN, &reqData, &authpRet, &authwRet)) {
          *pRet = authpRet;
          *wRet = authwRet;
          if (req.responseData != NULL) {
            free (req.responseData);
          }
          PianoDestroyRequest (&req);
          return 0;
        } else {
          /* try again */
          *pRet = PIANO_RET_CONTINUE_REQUEST;
          BroadcastMessage("Trying again... ");
        }

        // wtf
        free(usernameBuff);
        free(passwordBuff);

      } else if (*pRet != PIANO_RET_OK) {
	BroadcastMessage("Error: %s\n", PianoErrorToStr (*pRet));
	if (req.responseData != NULL) {
	  free (req.responseData);
	}
	PianoDestroyRequest (&req);
	return 0;
      } else {
	BroadcastMessage("Login Ok.\n");
      }
    }
    /* we can destroy the request at this point, even when this call needs
     * more than one http request. persistent data (step counter, e.g.) is
     * stored in req.data */
    if (req.responseData != NULL) {
      free (req.responseData);
    }
    PianoDestroyRequest (&req);
  } while (*pRet == PIANO_RET_CONTINUE_REQUEST);

  return 1;
}

void MythPianoService::WriteAudio(char* samples, size_t bytes)
{
  if (!m_AudioOutput) {

    BroadcastMessage("Setting up audio rate(%d), channels(%d)\n",
                     m_Player.samplerate,
                     m_Player.channels);

    QString passthru = gCoreContext->GetNumSetting("PassThruDeviceOverride", false) ? gCoreContext->GetSetting("PassThruOutputDevice") : QString::null;
    QString main = gCoreContext->GetSetting("AudioOutputDevice");
    QString errMsg;

    m_AudioOutput = AudioOutput::OpenAudio(main, passthru,
					   FORMAT_S16,
					   m_Player.channels,
					   0,
					   m_Player.samplerate,
					   AUDIOOUTPUT_MUSIC,
					   true, false);
  }

  if (!m_AudioOutput) {
    BroadcastMessage("Error in WriteAudio.  m_AudioOutput is null");
    return;
  }

  if (bytes == 0)
    return;

  m_AudioOutput->AddFrames(samples, bytes / 4, -1);
  m_AudioOutput->Drain();
}


/** \brief Creates a new MythPandora Screen
 *  \param parent Pointer to the screen stack
 *  \param name The name of the window
 */
MythPandora::MythPandora(MythScreenStack *parent, QString name) :
  MythScreenType(parent, name),
  m_coverArtFetcher(NULL),
  m_coverArtTempFile(NULL),
  m_Timer(NULL)
{
  //example of how to find the configuration dir currently used.
  QString confdir = GetConfDir();
  VERBOSE(VB_IMPORTANT, "MythPandora Conf dir:"  + confdir);
}

MythPandora::~MythPandora()
{
  MythPianoService* service = GetMythPianoService();
  service->RemoveMessageListener(this);
  service->StopPlayback();

  if (m_coverArtTempFile)
    delete m_coverArtTempFile;

  if (m_coverArtFetcher)
    delete m_coverArtFetcher;
}

void
MythPandora::RecvMessage(const char* message) {
  if (!strcmp(message, "New Song")) {
    MythPianoService* service = GetMythPianoService();
    if (service->GetCurrentSong()) {
      m_songText->SetText(QString(service->GetCurrentSong()->title));
      m_artistText->SetText(QString(service->GetCurrentSong()->artist));
      m_albumText->SetText(QString(service->GetCurrentSong()->album));
      m_playTimeText->SetText(QString("0:00"));

      // kick off cover art load
      if (m_coverArtFetcher)
	delete m_coverArtFetcher;

      if (m_coverArtTempFile)
	delete m_coverArtTempFile;

      m_coverArtFetcher = new QHttp();
      connect(m_coverArtFetcher, SIGNAL(done(bool)), this, SLOT(coverArtFetched()));  
      QUrl u(service->GetCurrentSong()->coverArt);
      QHttp::ConnectionMode conn_mode = QHttp::ConnectionModeHttp;
      m_coverArtFetcher->setHost(u.host(), conn_mode, 80);
      QByteArray path = QUrl::toPercentEncoding(u.path(), "!$&'()*+,;=:@/");
      m_coverArtFetcher->get(path);

    }
  }
  else if (m_outText)
    m_outText->SetText(QString(message));
}

void
MythPandora::coverArtFetched(void)
{
  QByteArray array = m_coverArtFetcher->readAll();

  m_coverArtTempFile = new QTemporaryFile();
  m_coverArtTempFile->open();
  m_coverArtTempFile->write(array);
  m_coverArtTempFile->flush();
  m_coverArtTempFile->waitForBytesWritten(-1);
  m_coverArtTempFile->close();

  QString filename = m_coverArtTempFile->fileName();

  m_coverartImage->SetFilename(filename);
  m_coverartImage->Load();
}


bool MythPandora::Create(void)
{
  bool foundtheme = false;

  // Load the theme for this screen
  foundtheme = LoadWindowFromXML("pandora-ui.xml", "pandora", this);
    
  if (!foundtheme)
    return false;

  bool err = false;
  UIUtilE::Assign(this, m_titleText,  "title", &err);
  UIUtilE::Assign(this, m_outText,    "outtext", &err);
  UIUtilE::Assign(this, m_songText,   "song", &err);
  UIUtilE::Assign(this, m_artistText, "artist", &err);
  UIUtilE::Assign(this, m_albumText,  "album", &err);
  UIUtilE::Assign(this, m_playTimeText,  "playtime", &err);
  UIUtilE::Assign(this, m_coverartImage, "coverart", &err);
  UIUtilE::Assign(this, m_logoutBtn,     "logoutBtn", &err);
  UIUtilE::Assign(this, m_stationsBtn,   "stationsBtn", &err);
  UIUtilE::Assign(this, m_stationText,   "station", &err);

  if (err) {
    VERBOSE(VB_IMPORTANT, "Cannot load screen 'pandora'");
    return false;
  }

  connect(m_logoutBtn, SIGNAL(Clicked()), this, SLOT(logoutCallback()));
  connect(m_stationsBtn, SIGNAL(Clicked()), this, SLOT(selectStationCallback()));

  BuildFocusList();

  SetFocusWidget(m_coverartImage);
  // dummy image needed
  m_coverartImage->SetFilename("/etc/alternatives/emacs-16x16.png");
  m_coverartImage->Load();

  MythPianoService* service = GetMythPianoService();

  m_stationText->SetText(QString(service->GetCurrentStation()->name));

  service->SetMessageListener(this);

  service->GetPlaylist();
  service->StartPlayback();

  m_Timer = new QTimer(this);
  connect(m_Timer, SIGNAL(timeout()), this, SLOT(heartbeat()));
  m_Timer->start(1000);

  return true;
}


void
MythPandora::heartbeat(void)
{
  MythPianoService* service = GetMythPianoService();
  long played = 0, duration = 0;
  
  service->GetTimes(&played, &duration);
  played = played / BAR_PLAYER_MS_TO_S_FACTOR;
  duration = duration / BAR_PLAYER_MS_TO_S_FACTOR;

  QString time_string = getTimeString(played, duration);
  m_playTimeText->SetText(time_string);

  if (class LCD *lcd = LCD::Get())
  {
    float percent_heard = duration <= 0 ? 0.0 : ((float)played /
                           (float)duration) * 1000.0;
    if (time_string.length() > (int)lcd->getLCDWidth())
      time_string.remove(' ');
    lcd->setMusicProgress(time_string, percent_heard);
  }
}


bool MythPandora::keyPressEvent(QKeyEvent *event)
{
  if (GetFocusWidget() && GetFocusWidget()->keyPressEvent(event))
    return true;
    
  bool handled = false;
  QStringList actions;
  handled = GetMythMainWindow()->TranslateKeyPress("MythPandora", event, actions);

  for (int i = 0; i < actions.size() && !handled; i++) {
    QString action = actions[i];
    handled = true;
    
    if (action == "ESCAPE") {
      GetScreenStack()->PopScreen(false, true);
    }
    else if (action == "NEXTTRACK")
    {
        MythPianoService* service = GetMythPianoService();
        service->NextSong();
    }
    else if (action == "PAUSE" || action == "PLAY")
    {
	  MythPianoService* service = GetMythPianoService();
	  service->PauseToggle();
	}
    
    else if (action == "MUTE")
	{
	  // XXX UI needed
	  MythPianoService* service = GetMythPianoService();
	  service->ToggleMute();
	}
    else if (action == "VOLUMEDOWN")
	{
	  // XXX UI needed
	  MythPianoService* service = GetMythPianoService();
	  service->VolumeDown();
	}
    else if (action == "VOLUMEUP")
	{
	  // XXX UI needed
	  MythPianoService* service = GetMythPianoService();
	  service->VolumeUp();
	}
    else
      handled = false;
  }

  if (!handled && MythScreenType::keyPressEvent(event))
    handled = true;

  return handled;
}

void MythPandora::logoutCallback()
{
  // Not really sure if we should blow these away.  Instead, just go back to the login dialog.
  //  gCoreContext->SaveSetting("pandora-username", QString(""));
  //  gCoreContext->SaveSetting("pandora-password", QString(""));

  MythPianoService* service = GetMythPianoService();
  service->StopPlayback();
  service->Logout();

  GetScreenStack()->PopScreen(false, true);
  showLoginDialog();
}

void MythPandora::selectStationCallback()
{
  GetScreenStack()->PopScreen(false, true);
  showStationSelectDialog();
}

QString MythPandora::getTimeString(int exTime, int maxTime)
{
  QString time_string;
  
  int eh = exTime / 3600;
  int em = (exTime / 60) % 60;
  int es = exTime % 60;

  int maxh = maxTime / 3600;
  int maxm = (maxTime / 60) % 60;
  int maxs = maxTime % 60;

  if (maxTime <= 0)
  {
    if (eh > 0)
      time_string.sprintf("%d:%02d:%02d", eh, em, es);
    else
      time_string.sprintf("%02d:%02d", em, es);
  }
  else
  {
    if (maxh > 0)
      time_string.sprintf("%d:%02d:%02d / %d:%02d:%02d", eh, em,
          es, maxh, maxm, maxs);
    else
      time_string.sprintf("%02d:%02d / %02d:%02d", em, es, maxm,
          maxs);
  }

  return time_string;
}


MythPandoraConfig::MythPandoraConfig(MythScreenStack *parent, QString name)
    : MythScreenType(parent, name)
{
}

bool MythPandoraConfig::Create()
{
  bool foundtheme = false;

  // Load the theme for this screen
  foundtheme = LoadWindowFromXML("pandora-ui.xml", "pandorasettings", this);

  if (!foundtheme)
    return false;

  bool err = false;
  UIUtilE::Assign(this, m_nameEdit,    "username", &err);
  UIUtilE::Assign(this, m_passwordEdit,"password", &err);
  UIUtilE::Assign(this, m_loginBtn,    "loginBtn", &err);
  UIUtilE::Assign(this, m_outText,     "outtext",  &err);
  if (err) {
    VERBOSE(VB_IMPORTANT, "Cannot load screen 'pandora'");
    return false;
  }

  connect(m_loginBtn, SIGNAL(Clicked()), this, SLOT(loginCallback()));

  BuildFocusList();

  m_passwordEdit->SetPassword(true);

  QString username = gCoreContext->GetSetting("pandora-username");
  QString password = gCoreContext->GetSetting("pandora-password");

  m_nameEdit->SetText(username);
  m_passwordEdit->SetText(password);

  MythPianoService* service = GetMythPianoService();
  service->SetMessageListener(this);

  return true;
}

MythPandoraConfig::~MythPandoraConfig()
{
  MythPianoService* service = GetMythPianoService();
  service->RemoveMessageListener(this);
}

bool MythPandoraConfig::keyPressEvent(QKeyEvent *event)
{
    if (GetFocusWidget()->keyPressEvent(event))
        return true;

    bool handled = false;

    if (!handled && MythScreenType::keyPressEvent(event))
        handled = true;

    return handled;
}

void MythPandoraConfig::loginCallback()
{
  gCoreContext->SaveSetting("pandora-username", m_nameEdit->GetText());
  gCoreContext->SaveSetting("pandora-password", m_passwordEdit->GetText());

  MythPianoService* service = GetMythPianoService();
  if (service->Login() == 0) {
    GetScreenStack()->PopScreen(false, true);
    showStationSelectDialog();
  }
}


MythPandoraStationSelect::MythPandoraStationSelect(MythScreenStack *parent, QString name)
  : MythScreenType(parent, name)
{
}

MythPandoraStationSelect::~MythPandoraStationSelect()
{
}

bool
MythPandoraStationSelect::Create(void)
{
  bool foundtheme = false;

  // Load the theme for this screen
  foundtheme = LoadWindowFromXML("pandora-ui.xml", "pandorastations", this);

  if (!foundtheme)
    return false;

  bool err = false;
  UIUtilE::Assign(this, m_stations, "stations", &err);

  if (err) {
    VERBOSE(VB_IMPORTANT, "Cannot load screen 'pandora'");
    return false;
  }

  BuildFocusList();

  MythPianoService* service = GetMythPianoService();
  PianoStation_t* head = service->GetStations();

  while (head) {
    MythUIButtonListItem* item = new MythUIButtonListItem(m_stations, QString(head->name));
    item->SetData(QString(head->name));
    head = head->next;
  }

  connect(m_stations, SIGNAL(itemClicked(MythUIButtonListItem*)),
	  this, SLOT(stationSelectedCallback(MythUIButtonListItem*)));

  return true;
}

bool
MythPandoraStationSelect::keyPressEvent(QKeyEvent *event)
{
  if (GetFocusWidget()->keyPressEvent(event))
    return true;
  
  bool handled = false;
  
  if (!handled && MythScreenType::keyPressEvent(event))
    handled = true;
  
  return handled;
}

void
MythPandoraStationSelect::stationSelectedCallback(MythUIButtonListItem *item)
{
  MythPianoService* service = GetMythPianoService();
  QString name = item->GetData().toString();

  // find it.  XXX it would be better to save the station in the data.
  PianoStation_t* head = service->GetStations();
  while (head) { 
    if (QString(head->name) == name) {
      break;
    }
    head = head->next;
  }

  service->SetCurrentStation(head);
  if (head == NULL)
    return;

  GetScreenStack()->PopScreen(false, true);
  showPlayerDialog();
}
