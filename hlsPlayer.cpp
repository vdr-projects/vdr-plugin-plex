#include "hlsPlayer.h"

#include <Poco/Net/HTTPRequest.h>
#include <Poco/Net/HTTPResponse.h>
#include <Poco/Exception.h>
//#include <Poco/StreamCopier.h>

#include <pcrecpp.h>

#include "Plexservice.h"

static cMutex hlsMutex;

//--- cHlsSegmentLoader

cHlsSegmentLoader::cHlsSegmentLoader(std::string startm3u8)
{
	m_newList = false;
	m_bufferFilled = false;
	m_lastLoadedSegment = 0;
	m_segmentsToBuffer = 3;
	m_pBuffer = new uchar[8192];

	// Initialize members
	m_pClientSession = NULL;

	m_ringBufferSize = MEGABYTE(32);
	m_pRingbuffer = NULL;

	m_startUri = Poco::URI(startm3u8);
	m_startParser = cM3u8Parser();
	m_indexParser = cM3u8Parser();
}

cHlsSegmentLoader::~cHlsSegmentLoader()
{
	// Stop Thread
	Cancel(0);

	delete m_pClientSession;
	delete[] m_pBuffer;
	delete m_pRingbuffer;
}

bool cHlsSegmentLoader::LoadM3u8(std::string uri)
{
	LOCK_THREAD;
	m_startUri = Poco::URI(uri);
	return m_newList = true;
}

void cHlsSegmentLoader::Action(void)
{
	if(!LoadLists()) return;

	int estSize = EstimateSegmentSize();
	m_ringBufferSize = MEGABYTE(estSize*3);

	isyslog("[plex]%s Create Ringbuffer %d MB", __FUNCTION__, estSize*3);

	m_pRingbuffer = new cRingBufferLinear(m_ringBufferSize, 2*TS_SIZE);

	while(Running()) {
		if(m_newList) {
			hlsMutex.Lock();
			m_bufferFilled = false;
			m_lastLoadedSegment = 0;
			LoadLists();
			m_newList = false;
			m_pRingbuffer->Clear();
			isyslog("[plex] Ringbuffer Cleared, loading new segments");
			hlsMutex.Unlock();
		}
		if (!DoLoad()) {
			isyslog("[plex] No further segments to load");
			StopLoader();
			Cancel();
		}
		cCondWait::SleepMs(3);
	}
}

bool cHlsSegmentLoader::LoadLists(void)
{
	if( LoadStartList() ) {
		if( false == LoadIndexList() ) {
			esyslog("[plex]LoadIndexList failed!");
			return false;
		}
	} else {
		esyslog("[plex]LoadStartList failed!");
		return false;
	}
	return true;
}

bool cHlsSegmentLoader::LoadIndexList(void)
{
	bool res = false;
	ConnectToServer();
	if(m_startParser.MasterPlaylist && m_startParser.vPlaylistItems.size() > 0) {
		// Todo: make it universal, might only work for Plexmediaserver
		ConnectToServer();

		std::string startUri = m_startUri.toString();
		startUri = startUri.substr(0, startUri.find_last_of("/")+1);

		Poco::URI indexUri(startUri+m_startParser.vPlaylistItems[0].file);

		Poco::Net::HTTPRequest request(Poco::Net::HTTPRequest::HTTP_GET, indexUri.getPath());
		AddHeader(request);
		// same server
		m_pClientSession->sendRequest(request);

		Poco::Net::HTTPResponse responseStart;
		std::istream& indexFile = m_pClientSession->receiveResponse(responseStart);

		if(responseStart.getStatus() != 200) {
			esyslog("[plex]%s Response Not Valid", __FUNCTION__);
			return res;
		}
		m_indexParser = cM3u8Parser();
		res =  m_indexParser.Parse(indexFile);

		if(res) {
			// Segment URI is relative to index.m3u8
			std::string path = indexUri.getPath();
			m_segmentUriPart = path.substr(0, path.find_last_of("/")+1);
		}
		if(m_indexParser.TargetDuration > 3) {
			m_segmentsToBuffer = 1;
		} else {
			m_segmentsToBuffer = 2;
		}
	}
	return res;
}

bool cHlsSegmentLoader::LoadStartList(void)
{
	bool res = false;
	ConnectToServer();

	Poco::Net::HTTPRequest request(Poco::Net::HTTPRequest::HTTP_GET, m_startUri.getPathAndQuery());
	AddHeader(request);
	m_pClientSession->sendRequest(request);

	Poco::Net::HTTPResponse responseStart;
	std::istream& startFile = m_pClientSession->receiveResponse(responseStart);

	if(responseStart.getStatus() != 200) {
		esyslog("[plex]%s Response Not Valid", __FUNCTION__);
		return res;
	}

	m_startParser = cM3u8Parser();
	res = m_startParser.Parse(startFile);
	if(res) {
		// Get GUID
		pcrecpp::RE re("([0-9A-F]{8}-[0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{12})", pcrecpp::RE_Options(PCRE_CASELESS));
		string value;
		re.PartialMatch(m_startParser.vPlaylistItems[0].file, &value);
		m_sessionCookie = value;
	}
	return res;
}

int cHlsSegmentLoader::EstimateSegmentSize()
{
	if(&m_startParser.vPlaylistItems[0] == NULL) {
		esyslog("[plex]%s first element NULL", __FUNCTION__);
	}
	double bandw = m_startParser.vPlaylistItems[0].bandwidth / 8.0 / 1000.0 / 1000.0;

	int len = m_indexParser.TargetDuration;
	double estSize = (bandw) * len;
	estSize = max(estSize, 1.0); // default
	return ceil(estSize);
}

bool cHlsSegmentLoader::LoadSegment(std::string segmentUri)
{
	Poco::Net::HTTPRequest segmentRequest(Poco::Net::HTTPRequest::HTTP_GET, segmentUri);
	AddHeader(segmentRequest);
	m_pClientSession->sendRequest(segmentRequest);

	Poco::Net::HTTPResponse segmentResponse;
	std::istream& segmentFile = m_pClientSession->receiveResponse(segmentResponse);

	if(segmentResponse.getStatus() != 200) {
		// error
		esyslog("[plex]%s Loading Segment: %s failed.", __FUNCTION__, segmentUri.c_str());
		return false;
	}
	dsyslog("[plex]%s Loading Segment: %s successfully.", __FUNCTION__, segmentUri.c_str());

	// copy response

	int m = 0;
	segmentFile.read(reinterpret_cast<char*>(m_pBuffer), sizeof(m_pBuffer));
	std::streamsize n = segmentFile.gcount();
	while(n > 0) {
		m = m_pRingbuffer->Put(m_pBuffer, n);
		if(m < n) {
			//
			esyslog("[plex]%s oops, this should not happen. Segment doesn't fitted completly into ringbuffer", __FUNCTION__);
			break;
		} else {
			segmentFile.read(reinterpret_cast<char*>(m_pBuffer), sizeof(m_pBuffer));
			n = segmentFile.gcount();
		}
	}
	return true;
}

int cHlsSegmentLoader::GetSegmentSize(int segmentIndex)
{
	//dsyslog("[plex]%s Segment %d", __FUNCTION__, segmentIndex);
	if(m_indexParser.vPlaylistItems[segmentIndex].size > 0) {
		return m_indexParser.vPlaylistItems[segmentIndex].size;
	}
	try {
		Poco::Net::HTTPRequest req(Poco::Net::HTTPRequest::HTTP_HEAD, GetSegmentUri(segmentIndex));
		AddHeader(req);
		m_pClientSession->sendRequest(req);
		Poco::Net::HTTPResponse reqResponse;
		m_pClientSession->receiveResponse(reqResponse);
		return m_indexParser.vPlaylistItems[segmentIndex].size = reqResponse.getContentLength();
	} catch(Poco::IOException& exc) {
		esyslog("[plex]%s %s ", __FUNCTION__, exc.displayText().c_str());
		return INT_MAX;
	}
}

std::string cHlsSegmentLoader::GetSegmentUri(int segmentIndex) const
{
	return m_segmentUriPart + m_indexParser.vPlaylistItems[segmentIndex].file;
}

void cHlsSegmentLoader::CloseConnection(void)
{
	if(m_pClientSession)
		m_pClientSession->abort();

	delete m_pClientSession;
	m_pClientSession = NULL;
}

bool cHlsSegmentLoader::ConnectToServer(void)
{
	dsyslog("[plex]%s", __FUNCTION__);
	if(!m_pClientSession)
		m_pClientSession = new Poco::Net::HTTPClientSession(m_startUri.getHost(), m_startUri.getPort());

	return m_pClientSession->connected();
}

bool cHlsSegmentLoader::DoLoad(void)
{
	LOCK_THREAD;
	bool result = true;
	if(m_lastLoadedSegment <  m_indexParser.vPlaylistItems.size()) {
		int nextSegmentSize = GetSegmentSize(m_lastLoadedSegment);

		if(m_pRingbuffer->Free() > nextSegmentSize) {

			if(nextSegmentSize <= TS_SIZE) { // skip empty segments
				nextSegmentSize = GetSegmentSize(m_lastLoadedSegment++);
			} else {
				std::string segmentUri = GetSegmentUri(m_lastLoadedSegment++);
				result = LoadSegment(segmentUri);
			}
		}
		m_bufferFilled = result;
	} else {
		result = false;
	}
	return result;
}

bool cHlsSegmentLoader::BufferFilled(void)
{
	return m_bufferFilled;
}

bool cHlsSegmentLoader::StopLoader(void)
{
	dsyslog("[plex]%s", __FUNCTION__);
	try {
		std::string stopUri = "/video/:/transcode/universal/stop?session=" + m_sessionCookie;
		Poco::Net::HTTPRequest req(Poco::Net::HTTPRequest::HTTP_GET, stopUri);
		m_pClientSession->sendRequest(req);
		Poco::Net::HTTPResponse reqResponse;
		m_pClientSession->receiveResponse(reqResponse);

		Cancel();

		return reqResponse.getStatus() == 200;
	} catch(Poco::Exception& exc) {
		esyslog("[plex]%s %s ", __FUNCTION__, exc.displayText().c_str());
		return false;
	}
}
void cHlsSegmentLoader::AddHeader(Poco::Net::HTTPRequest& req)
{
	req.add("X-Plex-Client-Identifier", Config::GetInstance().GetUUID());
	req.add("X-Plex-Product", "Plex Home Theater");
	req.add("X-Plex-Device", "PC");
	req.add("X-Plex-Platform", "Plex Home Theater");
	req.add("X-Plex-Model", "Linux");
}

bool cHlsSegmentLoader::Active(void)
{
	return Running();
}

//--- cHlsPlayer

cHlsPlayer::cHlsPlayer(std::string startm3u8, plexclient::Video* Video)
{
	m_pSegmentLoader = new cHlsSegmentLoader(startm3u8);
	m_pVideo = Video;
	m_timeOffset = 0;
	m_doJump = false;
	m_isBuffering = false;
}

cHlsPlayer::~cHlsPlayer()
{
	delete m_pSegmentLoader;
	m_pSegmentLoader = NULL;
}


void cHlsPlayer::Action(void)
{
	// Start SegmentLoader
	m_pSegmentLoader->Start();

	while (Running()) {
		if(m_doJump && m_pSegmentLoader && m_pSegmentLoader->BufferFilled()) {
			LOCK_THREAD;

			DeviceFreeze();
			m_doJump = false;
			std::string uri = plexclient::Plexservice::GetUniversalTranscodeUrl(m_pVideo, m_jumpOffset);
			m_pSegmentLoader->LoadM3u8(uri);
			DeviceClear();
			DevicePlay();
		} else if(m_pSegmentLoader && m_pSegmentLoader->BufferFilled()) {
			DoPlay();
		} else {
			// Pause
			cCondWait::SleepMs(3);
		}
	}
}

bool cHlsPlayer::DoPlay(void)
{
	cPoller Poller;
	if(DevicePoll(Poller, 10)) {
		hlsMutex.Lock();

// Handle running out of packets. Buffer-> Play-> Pause-> Buffer-> Play

		for(int i = 0; i < 10; i++) {
			// Get a pointer to start of the data and the number of avaliable bytes
			int bytesAvaliable = 0;
			uchar* toPlay = m_pSegmentLoader->m_pRingbuffer->Get(bytesAvaliable);
			if(bytesAvaliable >= TS_SIZE) {
				int playedBytes = PlayTs(toPlay, TS_SIZE, false);
				m_pSegmentLoader->m_pRingbuffer->Del(playedBytes);
			} else {
				// Pause & Buffer
				break;
			}
		}
		hlsMutex.Unlock();
	}
	return true;
}

void cHlsPlayer::Activate(bool On)
{
	if(On) {
		Start();
	} else {
		Cancel(1);
	}
}

bool cHlsPlayer::GetIndex(int& Current, int& Total, bool SnapToIFrame)
{
	long stc = DeviceGetSTC();
	Total = m_pVideo->m_pMedia->m_lDuration / 1000 * 25;//FramesPerSecond(); // milliseconds
	Current = stc / (100 * 1000) * FramesPerSecond(); // 100ns per Tick
	Current += m_timeOffset; // apply offset
	return true;
}

bool cHlsPlayer::GetReplayMode(bool& Play, bool& Forward, int& Speed)
{
	Play = (playMode == pmPlay);
	Forward = true;
	Speed = -1;
	return true;
}

void cHlsPlayer::Pause(void)
{
	LOCK_THREAD;
	dsyslog("[plex]%s", __FUNCTION__);
	if (playMode == pmPause) {
		Play();
	} else {
		DeviceFreeze();
		playMode = pmPause;
	}
}

void cHlsPlayer::Play(void)
{
	LOCK_THREAD;
	dsyslog("[plex]%s", __FUNCTION__);
	if (playMode != pmPlay) {
		DevicePlay();
		playMode = pmPlay;
	}
}

bool cHlsPlayer::Active(void)
{
	return Running() && m_pSegmentLoader && m_pSegmentLoader->Active();
}

void cHlsPlayer::Stop(void)
{
	LOCK_THREAD;
	dsyslog("[plex]%s", __FUNCTION__);
	if (m_pSegmentLoader)
		m_pSegmentLoader->StopLoader();
	Cancel(0);
}

double cHlsPlayer::FramesPerSecond(void)
{
	return m_pVideo->m_pMedia->m_VideoFrameRate ? m_pVideo->m_pMedia->m_VideoFrameRate : DEFAULTFRAMESPERSECOND;
}

void cHlsPlayer::JumpRelative(int seconds)
{
	long stc = DeviceGetSTC();
	if(stc > 0) {
		int current = stc / (100 * 1000);
		JumpTo(current + seconds);
	} else {
		JumpTo(m_jumpOffset + seconds);
	}
}

void cHlsPlayer::JumpTo(int seconds)
{
	LOCK_THREAD;
	dsyslog("[plex]%s %d", __FUNCTION__, seconds);
	if(seconds < 0) seconds = 0;
	m_jumpOffset = seconds;
	m_doJump = true;
}