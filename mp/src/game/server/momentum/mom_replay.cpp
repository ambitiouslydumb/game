#include "cbase.h"
#include "utlbuffer.h"
#include "mom_replay.h"
#include "mom_replay_entity.h"
#include "Timer.h"
#include "util/mom_util.h"
#include "util/baseautocompletefilelist.h"

void CMomentumReplaySystem::BeginRecording(CBasePlayer *pPlayer)
{
    m_player = ToCMOMPlayer( pPlayer);
    //don't record if we're watching a preexisting replay or in practice mode
    if (!m_player->IsWatchingReplay() && !m_player->m_bHasPracticeMode) 
    {
        m_bIsRecording = true;
        Log("Recording began!\n");
        m_nCurrentTick = 1; //recoring begins at 1 ;)
    }
}
void CMomentumReplaySystem::StopRecording(CBasePlayer *pPlayer, bool throwaway, bool delay)
{
    if (throwaway) {
        m_bIsRecording = false;
        m_buf.Purge();
        return;
    }
    if (delay)
    {
        m_bShouldStopRec = true;
        m_fRecEndTime = gpGlobals->curtime;
    }
    else
    {
        m_bIsRecording = false;
        m_bShouldStopRec = false;
        CMomentumPlayer *pMOMPlayer = ToCMOMPlayer(pPlayer);
        char newRecordingName[MAX_PATH], newRecordingPath[MAX_PATH], runTime[BUFSIZETIME];
        mom_UTIL->FormatTime(g_Timer->GetLastRunTime(), runTime, 3, true);
        Q_snprintf(newRecordingName, MAX_PATH, "%s_%s_%s.momrec", (pMOMPlayer ? pMOMPlayer->GetPlayerName() : "Unnamed"), gpGlobals->mapname.ToCStr(), runTime);
        V_ComposeFileName(RECORDING_PATH, newRecordingName, newRecordingPath, MAX_PATH); //V_ComposeFileName calls all relevent filename functions for us! THANKS GABEN

        V_FixSlashes(RECORDING_PATH);
        filesystem->CreateDirHierarchy(RECORDING_PATH, "MOD"); //we have to create the directory here just in case it doesnt exist yet

        m_fhFileHandle = filesystem->Open(newRecordingPath, "w+b", "MOD");

        WriteRecordingToFile(&m_buf);

        filesystem->Close(m_fhFileHandle);
        Log("Recording Stopped! Ticks: %i\n", m_nCurrentTick);
        if( LoadRun(newRecordingName) ) //load the last run that we did in case we want to watch it
            StartReplay();
    }
}
void CMomentumReplaySystem::UpdateRecordingParams(CUtlBuffer *buf)
{
    m_nCurrentTick++; //increment recording tick

    m_currentFrame.m_nPlayerButtons = m_player->m_nButtons;
    m_currentFrame.m_qEyeAngles = m_player->EyeAngles();
    m_currentFrame.m_vPlayerOrigin = m_player->GetAbsOrigin();

    ByteSwap_replay_frame_t(m_currentFrame); //We need to byteswap all of our data first in order to write each byte in the correct order

    if(m_bShouldStopRec)
        if (gpGlobals->curtime - m_fRecEndTime >= END_RECORDING_PAUSE)
            StopRecording(UTIL_GetLocalPlayer(), false, false);

    Assert(buf && buf->IsValid());
    buf->Put(&m_currentFrame, sizeof(replay_frame_t)); //stick all the frame info into the buffer
}
replay_header_t CMomentumReplaySystem::CreateHeader()
{
    replay_header_t header;
    Q_strcpy(header.demofilestamp, REPLAY_HEADER_ID);
    header.demoProtoVersion = REPLAY_PROTOCOL_VERSION;
    Q_strcpy(header.mapName, gpGlobals->mapname.ToCStr());
    Q_strcpy(header.playerName, m_player->GetPlayerName());
  
    header.steamID64 = steamapicontext->SteamUser() ? steamapicontext->SteamUser()->GetSteamID().ConvertToUint64() : 0;

    header.interval_per_tick = gpGlobals->interval_per_tick;
    header.runTime = g_Timer->GetLastRunTime();
    time(&header.unixEpocDate);

    header.stats = m_player->m_PlayerRunStats; //copy ALL run stats using operator overload
    return header;
}
void CMomentumReplaySystem::WriteRecordingToFile(CUtlBuffer *buf)
{
    if (m_fhFileHandle)
    {
        //write header: Mapname, Playername, steam64, interval per tick
        replay_header_t littleEndianHeader = CreateHeader();
        ByteSwap_replay_header_t(littleEndianHeader); //byteswap again

        filesystem->Seek(m_fhFileHandle, 0, FILESYSTEM_SEEK_HEAD);
        filesystem->Write(&littleEndianHeader, sizeof(replay_header_t), m_fhFileHandle);
        DevLog("\n\nreplay header size: %i\n", sizeof(replay_header_t));

        Assert(buf && buf->IsValid());
        //write write from the CUtilBuffer to our filehandle:
        filesystem->Write(buf->Base(), buf->TellPut(), m_fhFileHandle);
        buf->Purge();
    }
}
//read a single frame (or tick) of a recording
replay_frame_t* CMomentumReplaySystem::ReadSingleFrame(FileHandle_t file, const char* filename)
{
    Assert(file != FILESYSTEM_INVALID_HANDLE);
    filesystem->Read(&m_currentFrame, sizeof(replay_frame_t), file);
    ByteSwap_replay_frame_t(m_currentFrame);

    return &m_currentFrame;
}
replay_header_t* CMomentumReplaySystem::ReadHeader(FileHandle_t file, const char* filename)
{
    Q_memset(&m_replayHeader, 0, sizeof(m_replayHeader));

    Assert(file != FILESYSTEM_INVALID_HANDLE);
    filesystem->Seek(file, 0, FILESYSTEM_SEEK_HEAD);
    filesystem->Read(&m_replayHeader, sizeof(replay_header_t), file);

    ByteSwap_replay_header_t(m_replayHeader);

    if (Q_strcmp(m_replayHeader.demofilestamp, REPLAY_HEADER_ID)) { //DEMO_HEADER_ID is __NOT__ the same as the stamp from the header we read from file
        ConMsg("%s has invalid replay header ID.\n", filename);
        return nullptr;
    }
    if (m_replayHeader.demoProtoVersion != REPLAY_PROTOCOL_VERSION) {
        ConMsg("ERROR: replay file protocol %i outdated, engine version is %i \n",
            m_replayHeader.demoProtoVersion, REPLAY_PROTOCOL_VERSION);

        return nullptr;
    }
    return &m_replayHeader;
}
bool CMomentumReplaySystem::LoadRun(const char* filename)
{
    m_vecRunData.Purge();
    char recordingName[MAX_PATH];
    V_ComposeFileName(RECORDING_PATH, filename, recordingName, MAX_PATH);
    m_fhFileHandle = filesystem->Open(recordingName, "r+b", "MOD");

    if (m_fhFileHandle != nullptr && filename != nullptr)
    {
        replay_header_t* header = ReadHeader(m_fhFileHandle, filename);
        if (header == nullptr) {
            return false;
        }

        m_loadedHeader = *header;
        while (!filesystem->EndOfFile(m_fhFileHandle))
        {
            replay_frame_t* frame = ReadSingleFrame(m_fhFileHandle, filename);
            m_vecRunData.AddToTail(*frame);
        }
        filesystem->Close(m_fhFileHandle);
        return true;
    }

    return false;
}
void CMomentumReplaySystem::StartReplay(bool firstperson)
{
    m_CurrentReplayGhost = static_cast<CMomentumReplayGhostEntity*>(CreateEntityByName("mom_replay_ghost"));
    if (m_CurrentReplayGhost != nullptr)
    {
        if (firstperson) g_Timer->Stop(false); //stop the timer just in case we started a replay while it was running...
        m_CurrentReplayGhost->StartRun(firstperson);
    }
}
void CMomentumReplaySystem::EndReplay()
{
    if (m_CurrentReplayGhost != nullptr)
    {
        m_CurrentReplayGhost->EndRun();
    }
}
void CMomentumReplaySystem::DispatchTimerStateMessage(CBasePlayer *pPlayer, bool started)
{
    if (pPlayer)
    {
        CSingleUserRecipientFilter user(pPlayer);
        user.MakeReliable();
        UserMessageBegin(user, "Timer_State");
        WRITE_BOOL(started);
        //MOM_TODO: This should be an offset # of ticks so the hud_timer can sync with the replay
        WRITE_LONG(gpGlobals->tickcount);
        MessageEnd();
    }
}
class CMOMReplayCommands
{
public:
    static void StartReplay(const CCommand &args, bool firstperson)
    {
        if (args.ArgC() > 0) //we passed any argument at all
        {
            char filename[MAX_PATH];
            if (Q_strstr(args.ArgS(), ".momrec"))
            {
                Q_snprintf(filename, MAX_PATH, "%s", args.ArgS());
            }
            else
            {
                Q_snprintf(filename, MAX_PATH, "%s.momrec", args.ArgS());
            }
            if (g_ReplaySystem->LoadRun(filename))
            {
                if (!Q_strcmp(STRING(gpGlobals->mapname), g_ReplaySystem->m_loadedHeader.mapName))
                {
                    g_ReplaySystem->StartReplay(firstperson);
                }
                else
                {
                    Warning("Error: Tried to start replay on incorrect map! Please load map %s", g_ReplaySystem->m_loadedHeader.mapName);
                }
            }
        }
    }
    static void PlayReplayGhost(const CCommand &args)
    {
        StartReplay(args, false);
    }
    static void PlayReplayFirstPerson(const CCommand &args)
    {
        StartReplay(args, true);
    }
};

CON_COMMAND_AUTOCOMPLETEFILE(playreplay_ghost, CMOMReplayCommands::PlayReplayGhost, "begins playback of a replay ghost", "recordings", momrec);
CON_COMMAND_AUTOCOMPLETEFILE(playreplay, CMOMReplayCommands::PlayReplayFirstPerson, "plays back a replay in first-person", "recordings", momrec);
CON_COMMAND(stop_replay, "Stops playing the current replay")
{
    g_ReplaySystem->EndReplay();
}
static CMomentumReplaySystem s_ReplaySystem("MOMReplaySystem");
CMomentumReplaySystem *g_ReplaySystem = &s_ReplaySystem;