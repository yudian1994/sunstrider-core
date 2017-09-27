
/** \file
    \ingroup u2w
*/

#include "WorldSocket.h"                                    // must be first to make ACE happy with ACE includes in it
#include "Common.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "Opcodes.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "Player.h"
#include "ObjectMgr.h"
#include "Group.h"
#include "Guild.h"
#include "World.h"
#include "MapManager.h"
#include "ObjectAccessor.h"
#include "BattleGroundMgr.h"
#include "OutdoorPvPMgr.h"
#include "Language.h"                                       // for CMSG_CANCEL_MOUNT_AURA handler
#include "Chat.h"
#include "SocialMgr.h"
#include "ScriptMgr.h"
#include "WardenWin.h"
#include "WardenMac.h"
#include "BigNumber.h"
#include "AddonMgr.h"
#include "zlib.h"
#include "AccountMgr.h"
#include "PacketLog.h"
#include "BattleGround.h"
#include "WardenBase.h"
#include "PacketUtilities.h"
#include "ReplayRecorder.h"
#include "ReplayPlayer.h"

#ifdef PLAYERBOT
#include "playerbot.h"
#endif

namespace {

std::string const DefaultPlayerName = "<none>";

} // namespace

bool MapSessionFilter::Process(WorldPacket * packet)
{
    ClientOpcodeHandler const* opHandle = opcodeTable.GetHandler(static_cast<OpcodeClient>(packet->GetOpcode()), m_pSession->GetClientBuild());

    //let's check if our opcode can be really processed in Map::Update()
    if(opHandle->ProcessingPlace == PROCESS_INPLACE)
        return true;

    //we do not process thread-unsafe packets
    if(opHandle->ProcessingPlace == PROCESS_THREADUNSAFE)
        return false;

    Player * plr = m_pSession->GetPlayer();
    if(!plr)
        return false;

    //in Map::Update() we do not process packets where player is not in world!
    return plr->IsInWorld();
}

//we should process ALL packets when player is not in world/logged in
//OR packet handler is not thread-safe!
bool WorldSessionFilter::Process(WorldPacket* packet)
{
    ClientOpcodeHandler const* opHandle = opcodeTable.GetHandler(static_cast<OpcodeClient>(packet->GetOpcode()), m_pSession->GetClientBuild());

    //check if packet handler is supposed to be safe
    if(opHandle->ProcessingPlace == PROCESS_INPLACE)
        return true;

    //thread-unsafe packets should be processed in World::UpdateSessions()
    if(opHandle->ProcessingPlace == PROCESS_THREADUNSAFE)
        return true;

    //no player attached? -> our client! ^^
    Player * plr = m_pSession->GetPlayer();
    if(!plr)
        return true;

    //lets process all packets for non-in-the-world player
    return (plr->IsInWorld() == false);
}

/// WorldSession constructor
WorldSession::WorldSession(uint32 id, ClientBuild clientBuild, std::string&& name, std::shared_ptr<WorldSocket> sock, AccountTypes sec, uint8 expansion, time_t mute_time, LocaleConstant locale, uint32 recruiter, bool isARecruiter) :
m_clientBuild(clientBuild),
LookingForGroup_auto_join(false),
LookingForGroup_auto_add(false),
AntiDOS(this),
m_muteTime(mute_time),
_player(NULL),
m_Socket(sock),
_security(sec),
_accountId(id),
_accountName(std::move(name)),
m_expansion(expansion),
m_sessionDbcLocale(sWorld->GetAvailableDbcLocale(locale)),
m_sessionDbLocaleIndex(locale),
_logoutTime(0),
m_inQueue(false),
m_playerLoading(false),
m_playerLogout(false),
m_playerRecentlyLogout(false),
m_playerSave(false),
m_latency(0),
m_clientTimeDelay(0),
m_TutorialsChanged(false),
_warden(NULL),
lastCheatWarn(time(NULL)),
forceExit(false),
expireTime(60000)
{
    memset(m_Tutorials, 0, sizeof(m_Tutorials));

    if (sock)
    {
        m_Address = sock->GetRemoteIpAddress().to_string();
        ResetTimeOutTime();
        LoginDatabase.PExecute("UPDATE account SET online = 1 WHERE id = %u;", GetAccountId());
    }
}

/// WorldSession destructor
WorldSession::~WorldSession()
{
    ///- unload player if not unloaded
    if (_player)
        LogoutPlayer (true);

    /// - If have unclosed socket, close it
    if (m_Socket)
    {
        m_Socket->CloseSocket();
        m_Socket = nullptr;
    }

    if (_warden)
        delete _warden;

    ///- empty incoming packet queue
    WorldPacket* packet = NULL;
    while (_recvQueue.next(packet))
        delete packet;

    LoginDatabase.AsyncPQuery("UPDATE account SET online = 0 WHERE id = %u;", GetAccountId());
    CharacterDatabase.AsyncPQuery("UPDATE characters SET online = 0 WHERE account = %u;", GetAccountId());
}

ClientBuild WorldSession::GetClientBuild() const
{
    return m_clientBuild;
}

/// Get the player name
std::string const& WorldSession::GetPlayerName() const
{
    return GetPlayer() ? GetPlayer()->GetName() : DefaultPlayerName;
}

/** Send a packet to the client
When sending a packet for a LK client: You can either use the BC or the LK enum. LK opcodes have offsetted opcode nums starting at OPCODE_START_EXTRA_OFFSET_AT,
but this is handled in this function and you can safely ignore it.
*/
void WorldSession::SendPacket(WorldPacket* packet)
{
    ASSERT(packet->GetOpcode() != NULL_OPCODE);

    #ifdef PLAYERBOT
    // Playerbot mod: send packet to bot AI
    if (GetPlayer())
    {
        if (GetPlayer()->GetPlayerbotAI())
            GetPlayer()->GetPlayerbotAI()->HandleBotOutgoingPacket(*packet);
        else if (GetPlayer()->GetPlayerbotMgr())
            GetPlayer()->GetPlayerbotMgr()->HandleMasterOutgoingPacket(*packet);
    }
    #endif

    if (!m_Socket)
        return;

    #ifdef TRINITY_DEBUG

    // Code for network use statistic
    static uint64 sendPacketCount = 0;
    static uint64 sendPacketBytes = 0;

    static time_t firstTime = time(NULL);
    static time_t lastTime = firstTime;                     // next 60 secs start time

    static uint64 sendLastPacketCount = 0;
    static uint64 sendLastPacketBytes = 0;

    time_t cur_time = time(NULL);

    if((cur_time - lastTime) < 60)
    {
        sendPacketCount+=1;
        sendPacketBytes+=packet->size();

        sendLastPacketCount+=1;
        sendLastPacketBytes+=packet->size();
    }
    else
    {
        uint64 minTime = uint64(cur_time - lastTime);
        uint64 fullTime = uint64(lastTime - firstTime);
        TC_LOG_DEBUG("misc","Send all time packets count: " UI64FMTD " bytes: " UI64FMTD " avr.count/sec: %f avr.bytes/sec: %f time: %u",sendPacketCount,sendPacketBytes,float(sendPacketCount)/fullTime,float(sendPacketBytes)/fullTime,uint32(fullTime));
        TC_LOG_DEBUG("misc","Send last min packets count: " UI64FMTD " bytes: " UI64FMTD " avr.count/sec: %f avr.bytes/sec: %f",sendLastPacketCount,sendLastPacketBytes,float(sendLastPacketCount)/minTime,float(sendLastPacketBytes)/minTime);

        lastTime = cur_time;
        sendLastPacketCount = 1;
        sendLastPacketBytes = packet->wpos();               // wpos is real written size
    }

    #endif                                                  // !TRINITY_DEBUG

#ifdef BUILD_335_SUPPORT
    if(GetClientBuild() == BUILD_335)
    {
        uint16 opcode = packet->GetOpcode();
        uint32 maxOpcodeBC = NUM_MSG_TYPES;
        //a BC offset was given, offset it by one if needed
        if(opcode < maxOpcodeBC && opcode >= OPCODE_START_EXTRA_OFFSET_AT)
            packet->SetOpcode(opcode + 1);
    }
#endif

    //    sScriptMgr->OnPacketSend(this, *packet);

    TC_LOG_TRACE("network.opcode", "S->C: %s %s", GetPlayerInfo().c_str(), GetOpcodeNameForLogging(static_cast<OpcodeServer>(packet->GetOpcode())).c_str());
    m_Socket->SendPacket(*packet);

    // Log packet for replay
    if (m_replayRecorder)
        m_replayRecorder->AddPacket(packet);
}

/// Add an incoming packet to the queue
void WorldSession::QueuePacket(WorldPacket* new_packet)
{
    _recvQueue.add(new_packet);
}

/// Logging helper for unexpected opcodes
void WorldSession::LogUnexpectedOpcode(WorldPacket* packet, const char* status, const char *reason)
{
    TC_LOG_ERROR("network.opcode", "Received unexpected opcode %s Status: %s Reason: %s from %s",
        GetOpcodeNameForLogging(static_cast<OpcodeClient>(packet->GetOpcode())).c_str(), status, reason, GetPlayerInfo().c_str());
}

/// Logging helper for unexpected opcodes
void WorldSession::LogUnprocessedTail(WorldPacket* packet)
{
    if (!sLog->ShouldLog("network.opcode", LOG_LEVEL_TRACE) || packet->rpos() >= packet->wpos())
        return;

    TC_LOG_TRACE("network.opcode", "Unprocessed tail data (read stop at %u from %u) Opcode %s from %s",
        uint32(packet->rpos()), uint32(packet->wpos()), GetOpcodeNameForLogging(static_cast<OpcodeClient>(packet->GetOpcode())).c_str(), GetPlayerInfo().c_str());
    packet->print_storage();
}

/// Update the WorldSession (triggered by World update)
bool WorldSession::Update(uint32 diff, PacketFilter& updater)
{
    #ifdef PLAYERBOT
    if (GetPlayer() && GetPlayer()->GetPlayerbotAI()) return true;
    #endif

    /// Update Timeout timer.
    UpdateTimeOutTime(diff);

    ///- Before we process anything:
    /// If necessary, kick the player from the game
    if (IsConnectionIdle())
    {
        if(sWorld->getConfig(CONFIG_DEBUG_LOG_LAST_PACKETS))
        {
            boost::shared_mutex& lock = m_Socket->GetLastPacketsSentMutex();
            lock.lock_shared();
            auto list = m_Socket->GetLastPacketsSent();
            if(!list.empty())
            {
                TC_LOG_ERROR("network.opcode","Client from account %u timed out. Dumping last packet sent since last response (up to 10) :",GetAccountId());
                for(auto itr : list)
                    sPacketLog->DumpPacket(LOG_LEVEL_ERROR,SERVER_TO_CLIENT,itr,GetPlayerInfo());

                lock.unlock_shared(); //unlock before calling ClearLastPacketsSent
                TC_LOG_ERROR("network.opcode","==================================================================");
                m_Socket->ClearLastPacketsSent();
            } else
                lock.unlock_shared();
        }
        m_Socket->CloseSocket();
    }

    ///- Retrieve packets from the receive queue and call the appropriate handlers
    /// not process packets if socket already closed
    WorldPacket* packet = nullptr;

    //! Delete packet after processing by default
    bool deletePacket = true;
    std::vector<WorldPacket*> requeuePackets;
    uint32 processedPackets = 0;
    time_t currentTime = time(NULL);


    //reset hasMoved info
    if(_player)
        _player->SetHasMovedInUpdate(false);

    while (m_Socket && _recvQueue.next(packet, updater))
    {
        ClientOpcodeHandler const* opHandle = opcodeTable.GetHandler(static_cast<OpcodeClient>(packet->GetOpcode()), GetClientBuild());

        try
        {
            switch (opHandle->Status)
            {
                case STATUS_LOGGEDIN:
                    if(!_player)
                    {
                        // skip STATUS_LOGGEDIN opcode unexpected errors if player logout sometime ago - this can be network lag delayed packets
                        //! If player didn't log out a while ago, it means packets are being sent while the server does not recognize
                        //! the client to be in world yet. We will re-add the packets to the bottom of the queue and process them later.
                        if(!m_playerRecentlyLogout)
                        {
                            //! Prevent infinite loop
                            requeuePackets.push_back(packet);
                            //! Because checking a bool is faster than reallocating memory
                            deletePacket = false;
                            //! Log
                            TC_LOG_DEBUG("network", "Re-enqueueing packet with opcode %s with with status STATUS_LOGGEDIN. "
                                "Player is currently not in world yet.", GetOpcodeNameForLogging(packet->GetOpcode()).c_str());
                        //   LogUnexpectedOpcode(packet, "the player has not logged in yet");
                        }
                    }
                    else if(_player->IsInWorld() && AntiDOS.EvaluateOpcode(*packet, currentTime))
                    {
                        //sScriptMgr->OnPacketReceive(this, *packet);
                        opHandle->Call(this, *packet);
                        LogUnprocessedTail(packet);

                        #ifdef PLAYERBOT
                        if (_player && _player->GetPlayerbotMgr())
                            _player->GetPlayerbotMgr()->HandleMasterIncomingPacket(*packet);
                        #endif
                    }
                    // lag can cause STATUS_LOGGEDIN opcodes to arrive after the player started a transfer
                    break;
                case STATUS_LOGGEDIN_OR_RECENTLY_LOGGOUT:
                        if (!_player && !m_playerRecentlyLogout && !m_playerLogout) // There's a short delay between _player = null and m_playerRecentlyLogout = true during logout
                            LogUnexpectedOpcode(packet, "STATUS_LOGGEDIN_OR_RECENTLY_LOGGOUT",
                                "the player has not logged in yet and not recently logout");
                        else if (AntiDOS.EvaluateOpcode(*packet, currentTime))
                        {
                            // not expected _player or must checked in packet handler
                            //sScriptMgr->OnPacketReceive(this, *packet);
                            opHandle->Call(this, *packet);
                            LogUnprocessedTail(packet);
                        }
                        break;
                case STATUS_TRANSFER:
                    if(!_player)
                        LogUnexpectedOpcode(packet, "STATUS_TRANSFER", "the player has not logged in yet");
                    else if(_player->IsInWorld())
                        LogUnexpectedOpcode(packet, "STATUS_TRANSFER", "the player is still in world");
                    else if(AntiDOS.EvaluateOpcode(*packet, currentTime))
                    {
                        //sScriptMgr->OnPacketReceive(this, *packet);
                        opHandle->Call(this, *packet);
                        LogUnprocessedTail(packet);
                    }
                    break;
                case STATUS_AUTHED:
                    // prevent cheating with skip queue wait
                    if(m_inQueue)
                    {
                        LogUnexpectedOpcode(packet, "STATUS_AUTHED", "the player not pass queue yet");
                        break;
                    }

                    // some auth opcodes can be recieved before STATUS_LOGGEDIN_OR_RECENTLY_LOGGOUT opcodes
                    // however when we recieve CMSG_CHAR_ENUM we are surely no longer during the logout process.
                    if (packet->GetOpcode() == CMSG_CHAR_ENUM)
                        m_playerRecentlyLogout = false;

                    if (AntiDOS.EvaluateOpcode(*packet, currentTime))
                    {
                        //sScriptMgr->OnPacketReceive(this, *packet);
                        opHandle->Call(this, *packet);
                        LogUnprocessedTail(packet);
                    }
                    break;
                case STATUS_NEVER:
                    TC_LOG_ERROR("network.opcode", "Received not allowed opcode %s from %s", GetOpcodeNameForLogging(packet->GetOpcode()).c_str()
                            , GetPlayerInfo().c_str());
                        break;
                case STATUS_UNHANDLED:
                    TC_LOG_DEBUG("network.opcode", "Received not handled opcode %s from %s", GetOpcodeNameForLogging(packet->GetOpcode()).c_str()
                            , GetPlayerInfo().c_str());
                    break;
            }
        }
        catch (WorldPackets::PacketArrayMaxCapacityException const& pamce)
        {
            TC_LOG_ERROR("network", "PacketArrayMaxCapacityException: %s while parsing %s from %s.",
                pamce.what(), GetOpcodeNameForLogging(static_cast<OpcodeClient>(packet->GetOpcode())).c_str(), GetPlayerInfo().c_str());
        }
        catch (ByteBufferException const&)
        {
            TC_LOG_ERROR("misc", "WorldSession::Update ByteBufferException occured while parsing a packet (opcode: %u) from client %s, accountid=%i. Skipped packet.",
                    packet->GetOpcode(), GetRemoteAddress().c_str(), GetAccountId());
            packet->hexlike();
        }

        if (deletePacket)
            delete packet;

        //restore default behavior for next packet
        deletePacket = true;

#define MAX_PROCESSED_PACKETS_IN_SAME_WORLDSESSION_UPDATE 100
        processedPackets++;

        //process only a max amout of packets in 1 Update() call.
        //Any leftover will be processed in next update
        if (processedPackets > MAX_PROCESSED_PACKETS_IN_SAME_WORLDSESSION_UPDATE)
            break;
    }

    #ifdef PLAYERBOT
    if (GetPlayer() && GetPlayer()->GetPlayerbotMgr())
        GetPlayer()->GetPlayerbotMgr()->UpdateSessions(0);
    #endif

    _recvQueue.readd(requeuePackets.begin(), requeuePackets.end());

    if (m_Socket && m_Socket->IsOpen() && _warden)
        _warden->Update();

    ProcessQueryCallbacks();

    ///- If necessary, log the player out
    //check if we are safe to proceed with logout
    //logout procedure should happen only in World::UpdateSessions() method!!!
    if(updater.ProcessLogout())
    {
        ///- If necessary, log the player out
        time_t currTime = time(NULL);
        if (ShouldLogOut(currTime) && !m_playerLoading)
            LogoutPlayer(true);

        if (m_Socket && GetPlayer() && _warden)
            _warden->Update();

        ///- Cleanup socket pointer if need
        if (m_Socket && !m_Socket->IsOpen())
        {
            expireTime -= expireTime > diff ? diff : expireTime;
            if (expireTime < diff || forceExit || !GetPlayer())
            {
                m_Socket = nullptr;
            }
        }

        if (!m_Socket)
            return false;                                       //Will remove this session from the world session map
    }

    if (m_replayPlayer)
    {
        bool result = m_replayPlayer->UpdateReplay();
        if (!result) //ended or error
            StopReplaying();
    }

    return true;
}


void WorldSession::ProcessQueryCallbacks()
{
    _queryProcessor.ProcessReadyQueries();

    if (_realmAccountLoginCallback.valid() && _realmAccountLoginCallback.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        InitializeSessionCallback(_realmAccountLoginCallback.get());

    //! HandlePlayerLoginOpcode
    if (_charLoginCallback.valid() && _charLoginCallback.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        HandlePlayerLogin(reinterpret_cast<LoginQueryHolder*>(_charLoginCallback.get()));
}

void WorldSession::InitWarden(BigNumber *K, std::string os)
{
    if (os == "Win" || os == "niW")                                        // Windows
        _warden = (WardenBase*)new WardenWin();
    else                                                    // MacOS
        _warden = (WardenBase*)new WardenMac();

    _warden->Init(this, K);
}

/// %Log the player out
void WorldSession::LogoutPlayer(bool Save)
{
    // finish pending transfers before starting the logout
    while (_player && _player->IsBeingTeleportedFar())
        HandleMoveWorldportAck();

    m_playerLogout = true;
    m_playerSave = Save;

    if (_player)
    {
        if (uint64 lguid = GetPlayer()->GetLootGUID())
            DoLootRelease(lguid);

        #ifdef PLAYERBOT
        // Playerbot mod: log out all player bots owned by this toon
        if (GetPlayer()->GetPlayerbotMgr())
            GetPlayer()->GetPlayerbotMgr()->LogoutAllBots();
        sRandomPlayerbotMgr.OnPlayerLogout(_player);
        #endif

        ///- If the player just died before logging out, make him appear as a ghost
        //FIXME: logout must be delayed in case lost connection with client in time of combat
        if (_player->GetDeathTimer())
        {
            _player->GetHostileRefManager().deleteReferences();
            _player->BuildPlayerRepop();
            _player->RepopAtGraveyard();
        }
        else if (!_player->GetAttackers().empty())
        {
            _player->CombatStop();
            _player->GetHostileRefManager().setOnlineOfflineState(false);
            _player->RemoveAllAurasOnDeath();

            // build set of player who attack _player or who have pet attacking of _player
            std::set<Player*> aset;
            for(Unit::AttackerSet::const_iterator itr = _player->GetAttackers().begin(); itr != _player->GetAttackers().end(); ++itr)
            {
                Unit* owner = (*itr)->GetOwner();           // including player controlled case
                if(owner)
                {
                    if(owner->GetTypeId()==TYPEID_PLAYER)
                        aset.insert(owner->ToPlayer());
                }
                else
                if((*itr)->GetTypeId()==TYPEID_PLAYER)
                    aset.insert((*itr)->ToPlayer());
            }

            _player->SetPvPDeath(!aset.empty());
            _player->KillPlayer();
            _player->BuildPlayerRepop();
            _player->RepopAtGraveyard();

            // give honor to all attackers from set like group case
            for(std::set<Player*>::const_iterator itr = aset.begin(); itr != aset.end(); ++itr)
                (*itr)->RewardHonor(_player,aset.size());

            // give bg rewards and update counters like kill by first from attackers
            // this can't be called for all attackers.
            if(!aset.empty())
                if(Battleground *bg = _player->GetBattleground())
                    bg->HandleKillPlayer(_player,*aset.begin());
        }
        else if(_player->HasAuraType(SPELL_AURA_SPIRIT_OF_REDEMPTION))
        {
            // this will kill character by SPELL_AURA_SPIRIT_OF_REDEMPTION
            _player->RemoveAurasByType(SPELL_AURA_MOD_SHAPESHIFT);
            _player->KillPlayer();
            _player->BuildPlayerRepop();
            _player->RepopAtGraveyard();
        }

        //drop a flag if player is carrying it
        if(Battleground *bg = _player->GetBattleground())
            bg->EventPlayerLoggedOut(_player);

        ///- Teleport to home if the player is in an invalid instance
        if (!_player->m_InstanceValid && !_player->IsGameMaster())
            _player->TeleportTo(_player->m_homebindMapId, _player->m_homebindX, _player->m_homebindY, _player->m_homebindZ, _player->GetOrientation());

        sOutdoorPvPMgr->HandlePlayerLeaveZone(_player,_player->GetZoneId());

        for (int i=0; i < PLAYER_MAX_BATTLEGROUND_QUEUES; i++)
        {
            if (BattlegroundQueueTypeId bgQueueTypeId = _player->GetBattlegroundQueueTypeId(i))
            {
                //TC has deserter code here too... why ?

                _player->RemoveBattlegroundQueueId(bgQueueTypeId);
                BattlegroundQueue& queue = sBattlegroundMgr->GetBattlegroundQueue(bgQueueTypeId);
                queue.RemovePlayer(_player->GetGUID(), true);
            }
        }

        // Repop at GraveYard or other player far teleport will prevent saving player because of not present map
        // Teleport player immediately for correct player save
        while (_player->IsBeingTeleportedFar())
            HandleMoveWorldportAck();

        ///- If the player is in a guild, update the guild roster and broadcast a logout message to other guild members
        Guild *guild = sObjectMgr->GetGuildById(_player->GetGuildId());
        if(guild)
        {
            guild->LoadPlayerStatsByGuid(_player->GetGUID());
            guild->UpdateLogoutTime(_player->GetGUID());

            WorldPacket data(SMSG_GUILD_EVENT, (1+1+12+8)); // name limited to 12 in character table.
            data<<(uint8)GE_SIGNED_OFF;
            data<<(uint8)1;
            data<<_player->GetName();
            data<<_player->GetGUID();
            guild->BroadcastPacket(&data);
          }

        ///- Remove pet
        _player->RemovePet(NULL,PET_SAVE_AS_CURRENT, true);

        ///- empty buyback items and save the player in the database
        // some save parts only correctly work in case player present in map/player_lists (pets, etc)
        if(Save)
        {
            uint32 eslot;
            for(int j = BUYBACK_SLOT_START; j < BUYBACK_SLOT_END; j++)
            {
                eslot = j - BUYBACK_SLOT_START;
                _player->SetUInt64Value(PLAYER_FIELD_VENDORBUYBACK_SLOT_1+eslot*2, 0);
                _player->SetUInt32Value(PLAYER_FIELD_BUYBACK_PRICE_1+eslot, 0);
                _player->SetUInt32Value(PLAYER_FIELD_BUYBACK_TIMESTAMP_1+eslot, 0);
            }
            _player->SaveToDB();
        }

        ///- Leave all channels before player delete...
        _player->CleanupChannels();

        ///- If the player is in a group (or invited), remove him. If the group if then only 1 person, disband the group.
        _player->UninviteFromGroup();

        // remove player from the group if he is:
        // a) in group; b) not in raid group; c) logging out normally (not being kicked or disconnected)
        /*if(_player->GetGroup() && !_player->GetGroup()->isRaidGroup() && m_Socket)
            _player->RemoveFromGroup();*/

        ///- Remove the player from the world
        // the player may not be in the world when logging out
        // e.g if he got disconnected during a transfer to another map
        // calls to GetMap in this case may cause crashes
        if(_player->IsInWorld())
            _player->GetMap()->RemovePlayerFromMap(_player, false);

        // RemoveFromWorld does cleanup that requires the player to be in the accessor
        ObjectAccessor::RemoveObject(_player);

        ///- Inform the group about leaving and send update to other members
        if(_player->GetGroup())
        {
            _player->GetGroup()->CheckLeader(_player->GetGUID(), true); //logout check leader
            _player->GetGroup()->SendUpdate();
        }


        ///- Broadcast a logout message to the player's friends
        sSocialMgr->SendFriendStatus(_player, FRIEND_OFFLINE, _player->GetGUIDLow(), true);

        ///- Delete the player object
        _player->CleanupsBeforeDelete();                    // do some cleanup before deleting to prevent crash at crossreferences to already deleted data

        sSocialMgr->RemovePlayerSocial (_player->GetGUIDLow ());
        delete _player;
        _player = NULL;

        ///- Send the 'logout complete' packet to the client
        WorldPacket data( SMSG_LOGOUT_COMPLETE, 0 );
        SendPacket( &data );
        TC_LOG_DEBUG("network", "SESSION: Sent SMSG_LOGOUT_COMPLETE Message");

        ///- Since each account can only have one online character at any given time, ensure all characters for active account are marked as offline
        CharacterDatabase.PExecute("UPDATE characters SET online = 0 WHERE account = '%u'", GetAccountId());
    }

    //Hook for OnLogout Event
    //    sScriptMgr->OnPlayerLogout(_player);

    m_playerLogout = false;
    m_playerSave = false;
    m_playerRecentlyLogout = true;
    LogoutRequest(0);
}

/// Kick a player out of the World
void WorldSession::KickPlayer()
{
    if (m_Socket)
    {
        m_Socket->CloseSocket();
        forceExit = true;
    }
}

/// Cancel channeling handler

void WorldSession::SendAreaTriggerMessage(const char* Text, ...)
{
    va_list ap;
    char szStr [1024];
    szStr[0] = '\0';

    va_start(ap, Text);
    vsnprintf( szStr, 1024, Text, ap );
    va_end(ap);

    uint32 length = strlen(szStr)+1;
    WorldPacket data(SMSG_AREA_TRIGGER_MESSAGE, 4+length);
    data << length;
    data << szStr;
    SendPacket(&data);
}

void WorldSession::SendNotification(const char *format,...)
{
    if(format)
    {
        va_list ap;
        char szStr [1024];
        szStr[0] = '\0';
        va_start(ap, format);
        vsnprintf( szStr, 1024, format, ap );
        va_end(ap);

        WorldPacket data(SMSG_NOTIFICATION, (strlen(szStr)+1));
        data << szStr;
        SendPacket(&data);
    }
}

void WorldSession::SendNotification(int32 string_id,...)
{
    char const* format = GetTrinityString(string_id);
    if(format)
    {
        va_list ap;
        char szStr [1024];
        szStr[0] = '\0';
        va_start(ap, string_id);
        vsnprintf( szStr, 1024, format, ap );
        va_end(ap);

        WorldPacket data(SMSG_NOTIFICATION, (strlen(szStr)+1));
        data << szStr;
        SendPacket(&data);
    }
}

const char * WorldSession::GetTrinityString( int32 entry ) const
{
    return sObjectMgr->GetTrinityString(entry,GetSessionDbcLocale());
}

void WorldSession::Handle_NULL( WorldPacket& null )
{
    TC_LOG_ERROR("network.opcode", "Received unhandled opcode %s from %s", GetOpcodeNameForLogging(static_cast<OpcodeClient>(null.GetOpcode())).c_str(), GetPlayerInfo().c_str());
}

void WorldSession::Handle_EarlyProccess( WorldPacket& recvPacket )
{
    TC_LOG_ERROR("network.opcode", "Received opcode %s that must be processed in WorldSocket::OnRead from %s"
        , GetOpcodeNameForLogging(static_cast<OpcodeClient>(recvPacket.GetOpcode())).c_str(), GetPlayerInfo().c_str());
}

void WorldSession::Handle_ServerSide( WorldPacket& recvPacket )
{
    TC_LOG_ERROR("network.opcode", "Received server-side opcode %s from %s"
        , GetOpcodeNameForLogging(static_cast<OpcodeServer>(recvPacket.GetOpcode())).c_str(), GetPlayerInfo().c_str());
}

void WorldSession::Handle_Deprecated( WorldPacket& recvPacket )
{
    TC_LOG_ERROR("network.opcode", "Received deprecated opcode %s from %s"
        , GetOpcodeNameForLogging(static_cast<OpcodeClient>(recvPacket.GetOpcode())).c_str(), GetPlayerInfo().c_str());
}

void WorldSession::SendAuthWaitQue(uint32 position)
{
    if(position == 0)
    {
        WorldPacket packet( SMSG_AUTH_RESPONSE, 1 );
        packet << uint8( AUTH_OK );
        SendPacket(&packet);
    }
    else
    {
        WorldPacket packet( SMSG_AUTH_RESPONSE, 5 );
        packet << uint8( AUTH_WAIT_QUEUE );
        packet << uint32 (position);
        SendPacket(&packet);
    }
}

void WorldSession::LoadAccountData(PreparedQueryResult result, uint32 mask)
{
#ifdef LICH_KING
    for (uint32 i = 0; i < NUM_ACCOUNT_DATA_TYPES; ++i)
        if (mask & (1 << i))
            m_accountData[i] = AccountData();

    if (!result)
        return;

    do
    {
        Field* fields = result->Fetch();
        uint32 type = fields[0].GetUInt8();
        if (type >= NUM_ACCOUNT_DATA_TYPES)
        {
            TC_LOG_ERROR("misc", "Table `%s` have invalid account data type (%u), ignore.",
                mask == GLOBAL_CACHE_MASK ? "account_data" : "character_account_data", type);
            continue;
        }

        if ((mask & (1 << type)) == 0)
        {
            TC_LOG_ERROR("misc", "Table `%s` have non appropriate for table  account data type (%u), ignore.",
                mask == GLOBAL_CACHE_MASK ? "account_data" : "character_account_data", type);
            continue;
        }

        m_accountData[type].Time = time_t(fields[1].GetUInt32());
        m_accountData[type].Data = fields[2].GetString();
    } while (result->NextRow());
#endif
}

std::string WorldSession::GetLocalizedItemName(const ItemTemplate* proto)
{
    std::string name = proto->Name1;
    LocaleConstant loc_idx = GetSessionDbcLocale();
    if ( loc_idx >= 0 )
    {
        ItemLocale const *il = sObjectMgr->GetItemLocale(proto->ItemId);
        if (il)
        {
            if (il->Name.size() > size_t(loc_idx) && !il->Name[loc_idx].empty())
                name = il->Name[loc_idx];
        }
    }
    return name;
}

std::string WorldSession::GetLocalizedItemName(uint32 itemId)
{
    if(const ItemTemplate* proto = sObjectMgr->GetItemTemplate(itemId))
        return GetLocalizedItemName(proto);
    else
        return std::string();
}

std::string WorldSession::GetPlayerInfo() const
{
    std::ostringstream ss;

    ss << "[Player: " << GetPlayerName()
       << " (Guid: " << (_player != NULL ? _player->GetGUID() : 0)
       << ", Account: " << GetAccountId() << ")]";

    return ss.str();
}

//same structure LK & BC
void WorldSession::SendAddonsInfo()
{
    uint8 addonPublicKey[256] =
    {
        0xC3, 0x5B, 0x50, 0x84, 0xB9, 0x3E, 0x32, 0x42, 0x8C, 0xD0, 0xC7, 0x48, 0xFA, 0x0E, 0x5D, 0x54,
        0x5A, 0xA3, 0x0E, 0x14, 0xBA, 0x9E, 0x0D, 0xB9, 0x5D, 0x8B, 0xEE, 0xB6, 0x84, 0x93, 0x45, 0x75,
        0xFF, 0x31, 0xFE, 0x2F, 0x64, 0x3F, 0x3D, 0x6D, 0x07, 0xD9, 0x44, 0x9B, 0x40, 0x85, 0x59, 0x34,
        0x4E, 0x10, 0xE1, 0xE7, 0x43, 0x69, 0xEF, 0x7C, 0x16, 0xFC, 0xB4, 0xED, 0x1B, 0x95, 0x28, 0xA8,
        0x23, 0x76, 0x51, 0x31, 0x57, 0x30, 0x2B, 0x79, 0x08, 0x50, 0x10, 0x1C, 0x4A, 0x1A, 0x2C, 0xC8,
        0x8B, 0x8F, 0x05, 0x2D, 0x22, 0x3D, 0xDB, 0x5A, 0x24, 0x7A, 0x0F, 0x13, 0x50, 0x37, 0x8F, 0x5A,
        0xCC, 0x9E, 0x04, 0x44, 0x0E, 0x87, 0x01, 0xD4, 0xA3, 0x15, 0x94, 0x16, 0x34, 0xC6, 0xC2, 0xC3,
        0xFB, 0x49, 0xFE, 0xE1, 0xF9, 0xDA, 0x8C, 0x50, 0x3C, 0xBE, 0x2C, 0xBB, 0x57, 0xED, 0x46, 0xB9,
        0xAD, 0x8B, 0xC6, 0xDF, 0x0E, 0xD6, 0x0F, 0xBE, 0x80, 0xB3, 0x8B, 0x1E, 0x77, 0xCF, 0xAD, 0x22,
        0xCF, 0xB7, 0x4B, 0xCF, 0xFB, 0xF0, 0x6B, 0x11, 0x45, 0x2D, 0x7A, 0x81, 0x18, 0xF2, 0x92, 0x7E,
        0x98, 0x56, 0x5D, 0x5E, 0x69, 0x72, 0x0A, 0x0D, 0x03, 0x0A, 0x85, 0xA2, 0x85, 0x9C, 0xCB, 0xFB,
        0x56, 0x6E, 0x8F, 0x44, 0xBB, 0x8F, 0x02, 0x22, 0x68, 0x63, 0x97, 0xBC, 0x85, 0xBA, 0xA8, 0xF7,
        0xB5, 0x40, 0x68, 0x3C, 0x77, 0x86, 0x6F, 0x4B, 0xD7, 0x88, 0xCA, 0x8A, 0xD7, 0xCE, 0x36, 0xF0,
        0x45, 0x6E, 0xD5, 0x64, 0x79, 0x0F, 0x17, 0xFC, 0x64, 0xDD, 0x10, 0x6F, 0xF3, 0xF5, 0xE0, 0xA6,
        0xC3, 0xFB, 0x1B, 0x8C, 0x29, 0xEF, 0x8E, 0xE5, 0x34, 0xCB, 0xD1, 0x2A, 0xCE, 0x79, 0xC3, 0x9A,
        0x0D, 0x36, 0xEA, 0x01, 0xE0, 0xAA, 0x91, 0x20, 0x54, 0xF0, 0x72, 0xD8, 0x1E, 0xC7, 0x89, 0xD2
    };

    WorldPacket data(SMSG_ADDON_INFO, 4);

    for (auto itr = m_addonsList.begin(); itr != m_addonsList.end(); ++itr)
    {
        if(itr->build != GetClientBuild())
            continue;

        data << uint8(itr->State);

        uint8 crcpub = itr->UsePublicKeyOrCRC;
        data << uint8(crcpub);
        if (crcpub)
        {
            uint64 standardCRC = AddonMgr::GetStandardAddonCRC(GetClientBuild());
            uint8 usepk = (itr->CRC != standardCRC); // If addon is Standard addon CRC
            data << uint8(usepk);
            if (usepk)                                      // if CRC is wrong, add public key (client need it)
            {
                TC_LOG_INFO("misc", "ADDON: CRC (0x%x) for addon %s is wrong (does not match expected 0x%lx), sending pubkey",
                    itr->CRC, itr->Name.c_str(), standardCRC);

                data.append(addonPublicKey, sizeof(addonPublicKey));
            }

            data << uint32(0);                              /// @todo Find out the meaning of this.
        }

        data << uint8(0);       // uses URL
        //if (usesURL)
        //    data << uint8(0); // URL
    }

    m_addonsList.clear();

    //LK code here, not sure this is correct for BC
    AddonMgr::BannedAddonList const* bannedAddons = AddonMgr::GetBannedAddons();
    data << uint32(bannedAddons->size());
    for (AddonMgr::BannedAddonList::const_iterator itr = bannedAddons->begin(); itr != bannedAddons->end(); ++itr)
    {
        data << uint32(itr->Id);
        data.append(itr->NameMD5, sizeof(itr->NameMD5));
        data.append(itr->VersionMD5, sizeof(itr->VersionMD5));
        data << uint32(itr->Timestamp);
        data << uint32(1);  // IsBanned
    }

    SendPacket(&data);
}

void WorldSession::ReadAddon(ByteBuffer& addonInfo)
{
    // check next addon data format correctness
    if (addonInfo.rpos() + 1 > addonInfo.size())
        return;

    std::string addonName;
    uint8 enabled;
    uint32 crc, unk1;
    ClientBuild build = GetClientBuild();

    addonInfo >> addonName;

    addonInfo >> enabled >> crc >> unk1;

    TC_LOG_TRACE("misc", "ADDON: Name: %s, Enabled: 0x%x, CRC: 0x%x, Unknown2: 0x%x", addonName.c_str(), enabled, crc, unk1);

    AddonInfo addon(addonName, enabled, crc, 2, true, build);

    SavedAddon const* savedAddon = AddonMgr::GetAddonInfo(addonName, build);
    if (savedAddon)
    {
        if (addon.CRC != savedAddon->CRC)
            TC_LOG_DEBUG("misc", "ADDON: %s was known, but didn't match known CRC (0x%x)!", addon.Name.c_str(), savedAddon->CRC);
        else
            TC_LOG_TRACE("misc", "ADDON: %s was known, CRC is correct (0x%x)", addon.Name.c_str(), savedAddon->CRC);
    }
    else
    {
        AddonMgr::SaveAddon(addon);

        TC_LOG_DEBUG("misc", "ADDON: %s (0x%x) was not known, saving...", addon.Name.c_str(), addon.CRC);
    }

    /// @todo Find out when to not use CRC/pubkey, and other possible states.
    m_addonsList.push_back(addon);
}

void WorldSession::ReadAddonsInfo(ByteBuffer &data)
{
    if (data.rpos() + 4 > data.size())
        return;

    uint32 size;
    data >> size;

    if (!size)
        return;

    if (size > 0xFFFFF)
    {
        TC_LOG_ERROR("misc", "WorldSession::ReadAddonsInfo addon info too big, size %u", size);
        return;
    }

    uLongf uSize = size;

    uint32 pos = data.rpos();

    ByteBuffer addonInfo;
    addonInfo.resize(size);

    if (uncompress(addonInfo.contents(), &uSize, data.contents() + pos, data.size() - pos) == Z_OK)
    {
        switch(GetClientBuild())
        {
#ifdef LICH_KING
            case BUILD_335:
            {
                uint32 addonsCount;
                addonInfo >> addonsCount;                         // addons count

                for (uint32 i = 0; i < addonsCount; ++i)
                    ReadAddon(addonInfo);

                uint32 currentTime;
                addonInfo >> currentTime;
                TC_LOG_DEBUG("network", "ADDON: CurrentTime: %u", currentTime);
            } break;
#endif
            case BUILD_243:
            default:
            {
                while(addonInfo.rpos() < addonInfo.size())
                    ReadAddon(addonInfo);
            } break;
        }
    }
    else
        TC_LOG_ERROR("misc", "Addon packet uncompress error!");
}

void WorldSession::LoadTutorialsData(PreparedQueryResult result)
{
    memset(m_Tutorials, 0, sizeof(uint32) * MAX_ACCOUNT_TUTORIAL_VALUES);

    if (result)
        for (uint8 i = 0; i < MAX_ACCOUNT_TUTORIAL_VALUES; ++i)
            m_Tutorials[i] = (*result)[i].GetUInt32();

    m_TutorialsChanged = false;
}

void WorldSession::SendTutorialsData()
{
    WorldPacket data(SMSG_TUTORIAL_FLAGS, 4 * MAX_ACCOUNT_TUTORIAL_VALUES);
    for (uint8 i = 0; i < MAX_ACCOUNT_TUTORIAL_VALUES; ++i)
        data << m_Tutorials[i];

    SendPacket(&data);
}

void WorldSession::SaveTutorialsData(SQLTransaction &trans)
{
    if (!m_TutorialsChanged)
        return;

    PreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_SEL_HAS_TUTORIALS);
    stmt->setUInt32(0, GetAccountId());
    bool hasTutorials = bool(CharacterDatabase.Query(stmt));
    // Modify data in DB
    stmt = CharacterDatabase.GetPreparedStatement(hasTutorials ? CHAR_UPD_TUTORIALS : CHAR_INS_TUTORIALS);
    for (uint8 i = 0; i < MAX_ACCOUNT_TUTORIAL_VALUES; ++i)
        stmt->setUInt32(i, m_Tutorials[i]);
    stmt->setUInt32(MAX_ACCOUNT_TUTORIAL_VALUES, GetAccountId());
    trans->Append(stmt);

    m_TutorialsChanged = false;
}


/* TC
void WorldSession::LoadPermissions()
{
    uint32 id = GetAccountId();
    uint8 secLevel = GetSecurity();

    _RBACData = new rbac::RBACData(id, _accountName, realmID, secLevel);
    _RBACData->LoadFromDB();
}

QueryCallback WorldSession::LoadPermissionsAsync()
{
    uint32 id = GetAccountId();
    uint8 secLevel = GetSecurity();

    TC_LOG_DEBUG("rbac", "WorldSession::LoadPermissions [AccountId: %u, Name: %s, realmId: %d, secLevel: %u]",
        id, _accountName.c_str(), realmID, secLevel);

    _RBACData = new rbac::RBACData(id, _accountName, realm.Id.Realm, secLevel);
    return _RBACData->LoadFromDBAsync();

}
*/

class AccountInfoQueryHolderPerRealm : public SQLQueryHolder
{
public:
    enum
    {
#ifdef LICH_KING
        GLOBAL_ACCOUNT_DATA = 0,
#endif
        TUTORIALS,

        MAX_QUERIES
    };

    AccountInfoQueryHolderPerRealm() { SetSize(MAX_QUERIES); }

    bool Initialize(uint32 accountId)
    {
        bool ok = true;

        PreparedStatement* stmt;
#ifdef LICH_KING
        stmt = CharacterDatabase.GetPreparedStatement(CHAR_SEL_ACCOUNT_DATA);
        stmt->setUInt32(0, accountId);
        ok = SetPreparedQuery(GLOBAL_ACCOUNT_DATA, stmt) && ok;
#endif

        stmt = CharacterDatabase.GetPreparedStatement(CHAR_SEL_TUTORIALS);
        stmt->setUInt32(0, accountId);
        ok = SetPreparedQuery(TUTORIALS, stmt) && ok;

        return ok;
    }
};

void WorldSession::InitializeSession()
{
    AccountInfoQueryHolderPerRealm* realmHolder = new AccountInfoQueryHolderPerRealm();
    if (!realmHolder->Initialize(GetAccountId()))
    {
        delete realmHolder;
        SendAuthResponse(AUTH_SYSTEM_ERROR, false);
        return;
    }

    _realmAccountLoginCallback = CharacterDatabase.DelayQueryHolder(realmHolder);
}

void WorldSession::InitializeSessionCallback(SQLQueryHolder* realmHolder)
{
#ifdef LICH_KING
    LoadAccountData(realmHolder->GetPreparedResult(AccountInfoQueryHolderPerRealm::GLOBAL_ACCOUNT_DATA), GLOBAL_CACHE_MASK);
#endif

    LoadTutorialsData(realmHolder->GetPreparedResult(AccountInfoQueryHolderPerRealm::TUTORIALS));

    if (!m_inQueue)
        SendAuthResponse(AUTH_OK, true);
    else
        SendAuthWaitQue(0);

    SetInQueue(false);
    ResetTimeOutTime();

    SendAddonsInfo();
#ifdef LICH_KING
    SendClientCacheVersion(sWorld->getIntConfig(CONFIG_CLIENTCACHE_VERSION));
    SendTutorialsData(); //ON 243 it seems to be sent after adding player to map
#endif

    delete realmHolder;
}

bool WorldSession::DosProtection::EvaluateOpcode(WorldPacket& p, time_t time) const
{
    uint32 maxPacketCounterAllowed = GetMaxPacketCounterAllowed(p.GetOpcode());

    // Return true if there no limit for the opcode
    if (!maxPacketCounterAllowed)
        return true;

    PacketCounter& packetCounter = _PacketThrottlingMap[p.GetOpcode()];
    if (packetCounter.lastReceiveTime != time)
    {
        packetCounter.lastReceiveTime = time;
        packetCounter.amountCounter = 0;
    }

    // Check if player is flooding some packets
    if (++packetCounter.amountCounter <= maxPacketCounterAllowed)
        return true;

    TC_LOG_WARN("network", "AntiDOS: Account %u, IP: %s, Ping: %u, Character: %s, flooding packet (opc: %s (0x%X), count: %u)",
        Session->GetAccountId(), Session->GetRemoteAddress().c_str(), Session->GetLatency(), Session->GetPlayerName().c_str(),
        GetOpcodeNameForLogging(static_cast<OpcodeClient>(p.GetOpcode())).c_str(), p.GetOpcode(), packetCounter.amountCounter);

    switch (_policy)
    {
        case POLICY_LOG:
            return true;
        case POLICY_KICK:
            TC_LOG_INFO("network", "AntiDOS: Player kicked!");
            Session->KickPlayer();
            return false;
        case POLICY_BAN:
        {
            SanctionType bm = (SanctionType)sWorld->getIntConfig(CONFIG_PACKET_SPOOF_BANMODE);
            uint32 duration = sWorld->getIntConfig(CONFIG_PACKET_SPOOF_BANDURATION); // in seconds
            std::string nameOrIp = "";
            switch (bm)
            {
                case SANCTION_BAN_CHARACTER: // not supported, ban account
                case SANCTION_BAN_ACCOUNT:   sAccountMgr->GetName(Session->GetAccountId(), nameOrIp); break;
                case SANCTION_BAN_IP:        nameOrIp = Session->GetRemoteAddress();                  break;
            }
            sWorld->BanAccount(bm, nameOrIp, duration, "DOS (Packet Flooding/Spoofing", "Server: AutoDOS", nullptr);
            TC_LOG_INFO("network", "AntiDOS: Player automatically banned for %u seconds.", duration);
            Session->KickPlayer();
            return false;
        }
        default: // invalid policy
            return true;
    }
}

uint32 WorldSession::DosProtection::GetMaxPacketCounterAllowed(uint16 opcode) const
{
    uint32 maxPacketCounterAllowed;
    switch (opcode)
    {
        // CPU usage sending 2000 packets/second on a 3.70 GHz 4 cores on Win x64
        //                                              [% CPU mysqld]   [%CPU worldserver RelWithDebInfo]
        case CMSG_PLAYER_LOGIN:                         //   0               0.5
        case CMSG_NAME_QUERY:                           //   0               1
        case CMSG_PET_NAME_QUERY:                       //   0               1
        case CMSG_NPC_TEXT_QUERY:                       //   0               1
        case CMSG_ATTACKSTOP:                           //   0               1
#ifdef LICH_KING
        case CMSG_QUERY_QUESTS_COMPLETED:               //   0               1
#endif
        case CMSG_QUERY_TIME:                           //   0               1
#ifdef LICH_KING
        case CMSG_CORPSE_MAP_POSITION_QUERY:            //   0               1
#endif // LICH_KING
        case CMSG_MOVE_TIME_SKIPPED:                    //   0               1
        case MSG_QUERY_NEXT_MAIL_TIME:                  //   0               1
        case CMSG_SETSHEATHED:                          //   0               1
        case MSG_RAID_TARGET_UPDATE:                    //   0               1
        case CMSG_PLAYER_LOGOUT:                        //   0               1
        case CMSG_LOGOUT_REQUEST:                       //   0               1
        case CMSG_PET_RENAME:                           //   0               1
        case CMSG_QUESTGIVER_CANCEL:                    //   0               1
        case CMSG_QUESTGIVER_REQUEST_REWARD:            //   0               1
        case CMSG_COMPLETE_CINEMATIC:                   //   0               1
        case CMSG_BANKER_ACTIVATE:                      //   0               1
        case CMSG_BUY_BANK_SLOT:                        //   0               1
        case CMSG_OPT_OUT_OF_LOOT:                      //   0               1
        case CMSG_DUEL_ACCEPTED:                        //   0               1
        case CMSG_DUEL_CANCELLED:                       //   0               1
#ifdef LICH_KING
        case CMSG_CALENDAR_COMPLAIN:                    //   0               1
#endif
        case CMSG_QUEST_QUERY:                          //   0               1.5
        case CMSG_ITEM_QUERY_SINGLE:                    //   0               1.5
        case CMSG_ITEM_NAME_QUERY:                      //   0               1.5
        case CMSG_GAMEOBJECT_QUERY:                     //   0               1.5
        case CMSG_CREATURE_QUERY:                       //   0               1.5
        case CMSG_QUESTGIVER_STATUS_QUERY:              //   0               1.5
        case CMSG_GUILD_QUERY:                          //   0               1.5
        case CMSG_ARENA_TEAM_QUERY:                     //   0               1.5
        case CMSG_TAXINODE_STATUS_QUERY:                //   0               1.5
        case CMSG_TAXIQUERYAVAILABLENODES:              //   0               1.5
        case CMSG_QUESTGIVER_QUERY_QUEST:               //   0               1.5
        case CMSG_PAGE_TEXT_QUERY:                      //   0               1.5
        case MSG_QUERY_GUILD_BANK_TEXT:                 //   0               1.5
        case MSG_CORPSE_QUERY:                          //   0               1.5
        case MSG_MOVE_SET_FACING:                       //   0               1.5
        case CMSG_REQUEST_PARTY_MEMBER_STATS:           //   0               1.5
        case CMSG_QUESTGIVER_COMPLETE_QUEST:            //   0               1.5
        case CMSG_SET_ACTION_BUTTON:                    //   0               1.5
        case CMSG_RESET_INSTANCES:                      //   0               1.5
#ifdef LICH_KING
        case CMSG_HEARTH_AND_RESURRECT:                 //   0               1.5
#endif
        case CMSG_TOGGLE_PVP:                           //   0               1.5
        case CMSG_PET_ABANDON:                          //   0               1.5
        case CMSG_ACTIVATETAXIEXPRESS:                  //   0               1.5
        case CMSG_ACTIVATETAXI:                         //   0               1.5
        case CMSG_SELF_RES:                             //   0               1.5
        case CMSG_UNLEARN_SKILL:                        //   0               1.5
#ifdef LICH_KING
        case CMSG_EQUIPMENT_SET_SAVE:                   //   0               1.5
        case CMSG_DELETEEQUIPMENT_SET:                  //   0               1.5
        case CMSG_DISMISS_CRITTER:                      //   0               1.5
#endif
        case CMSG_REPOP_REQUEST:                        //   0               1.5
        case CMSG_GROUP_INVITE:                         //   0               1.5
        case CMSG_GROUP_DECLINE:                        //   0               1.5
        case CMSG_GROUP_ACCEPT:                         //   0               1.5
        case CMSG_GROUP_UNINVITE_GUID:                  //   0               1.5
        case CMSG_GROUP_UNINVITE:                       //   0               1.5
        case CMSG_GROUP_DISBAND:                        //   0               1.5
        case CMSG_BATTLEMASTER_JOIN_ARENA:              //   0               1.5
        case CMSG_LEAVE_BATTLEFIELD:                    //   0               1.5
        case MSG_GUILD_BANK_LOG_QUERY:                  //   0               2
        case CMSG_LOGOUT_CANCEL:                        //   0               2
        case CMSG_REALM_SPLIT:                          //   0               2
#ifdef LICH_KING
        case CMSG_ALTER_APPEARANCE:                     //   0               2
#endif
        case CMSG_QUEST_CONFIRM_ACCEPT:                 //   0               2
        case MSG_GUILD_EVENT_LOG_QUERY:                 //   0               2.5
#ifdef LICH_KING
        case CMSG_READY_FOR_ACCOUNT_DATA_TIMES:         //   0               2.5
#endif
        case CMSG_QUESTGIVER_STATUS_MULTIPLE_QUERY:     //   0               2.5
        case CMSG_BEGIN_TRADE:                          //   0               2.5
        case CMSG_INITIATE_TRADE:                       //   0               3
        case CMSG_MESSAGECHAT:                          //   0               3.5
        case CMSG_INSPECT:                              //   0               3.5
        case CMSG_AREA_SPIRIT_HEALER_QUERY:             // not profiled
        case CMSG_STANDSTATECHANGE:                     // not profiled
        case MSG_RANDOM_ROLL:                           // not profiled
        case CMSG_TIME_SYNC_RESP:                       // not profiled
        case CMSG_TRAINER_BUY_SPELL:                    // not profiled
        case CMSG_FORCE_RUN_SPEED_CHANGE_ACK:           // not profiled
        case CMSG_REQUEST_PET_INFO:                     // not profiled
        {
            // "0" is a magic number meaning there's no limit for the opcode.
            // All the opcodes above must cause little CPU usage and no sync/async database queries at all
            maxPacketCounterAllowed = 0;
            break;
        }

        case CMSG_QUESTGIVER_ACCEPT_QUEST:              //   0               4
        case CMSG_QUESTLOG_REMOVE_QUEST:                //   0               4
        case CMSG_QUESTGIVER_CHOOSE_REWARD:             //   0               4
        case CMSG_CONTACT_LIST:                         //   0               5
#ifdef LICH_KING
        case CMSG_LEARN_PREVIEW_TALENTS:                //   0               6
#endif
        case CMSG_AUTOBANK_ITEM:                        //   0               6
        case CMSG_AUTOSTORE_BANK_ITEM:                  //   0               6
        case CMSG_WHO:                                  //   0               7
#ifdef LICH_KING
        case CMSG_PLAYER_VEHICLE_ENTER:                 //   0               8
        case CMSG_LEARN_PREVIEW_TALENTS_PET:            // not profiled
#endif
        case MSG_MOVE_HEARTBEAT:
        {
            maxPacketCounterAllowed = 200;
            break;
        }

        case CMSG_GUILD_SET_PUBLIC_NOTE:                //   1               2         1 async db query
        case CMSG_GUILD_SET_OFFICER_NOTE:               //   1               2         1 async db query
        case CMSG_SET_CONTACT_NOTES:                    //   1               2.5       1 async db query
#ifdef LICH_KING
        case CMSG_CALENDAR_GET_CALENDAR:                //   0               1.5       medium upload bandwidth usage
#endif
        case CMSG_GUILD_BANK_QUERY_TAB:                 //   0               3.5       medium upload bandwidth usage
#ifdef LICH_KING
        case CMSG_QUERY_INSPECT_ACHIEVEMENTS:           //   0              13         high upload bandwidth usage
        case CMSG_GAMEOBJ_REPORT_USE:                   // not profiled
#endif
        case CMSG_GAMEOBJ_USE:                          // not profiled
        case MSG_PETITION_DECLINE:                      // not profiled
        {
            maxPacketCounterAllowed = 50;
            break;
        }
#ifdef LICH_KING
        case CMSG_QUEST_POI_QUERY:                      //   0              25         very high upload bandwidth usage
        {
            maxPacketCounterAllowed = MAX_QUEST_LOG_SIZE;
            break;
        }
        case CMSG_GM_REPORT_LAG:                        //   1               3         1 async db query

#endif
        case CMSG_SPELLCLICK:                           // not profiled
#ifdef LICH_KING
        case CMSG_REMOVE_GLYPH:                         // not profiled
        case CMSG_DISMISS_CONTROLLED_VEHICLE:           // not profiled
        {
            maxPacketCounterAllowed = 20;
            break;
        }
#endif
        case CMSG_PETITION_SIGN:                        //   9               4         2 sync 1 async db queries
        case CMSG_TURN_IN_PETITION:                     //   8               5.5       2 sync db query
        case CMSG_GROUP_CHANGE_SUB_GROUP:               //   6               5         1 sync 1 async db queries
        case CMSG_PETITION_QUERY:                       //   4               3.5       1 sync db query
#ifdef LICH_KING
        case CMSG_CHAR_RACE_CHANGE:                     //   5               4         1 sync db query
        case CMSG_CHAR_CUSTOMIZE:                       //   5               5         1 sync db query
        case CMSG_CHAR_FACTION_CHANGE:                  //   5               5         1 sync db query
#endif
        case CMSG_CHAR_DELETE:                          //   4               4         1 sync db query
        case CMSG_DEL_FRIEND:                           //   7               5         1 async db query
        case CMSG_ADD_FRIEND:                           //   6               4         1 async db query
        case CMSG_CHAR_RENAME:                          //   5               3         1 async db query
        case CMSG_GMSURVEY_SUBMIT:                      //   2               3         1 async db query
        case CMSG_BUG:                                  //   1               1         1 async db query
        case CMSG_GROUP_SET_LEADER:                     //   1               2         1 async db query
        case CMSG_GROUP_RAID_CONVERT:                   //   1               5         1 async db query
        case CMSG_GROUP_ASSISTANT_LEADER:               //   1               2         1 async db query
#ifdef LICH_KING
        case CMSG_CALENDAR_ADD_EVENT:                   //  21              10         2 async db query
#endif
        case CMSG_PETITION_BUY:                         // not profiled                1 sync 1 async db queries
#ifdef LICH_KING
        case CMSG_CHANGE_SEATS_ON_CONTROLLED_VEHICLE:   // not profiled
        case CMSG_REQUEST_VEHICLE_PREV_SEAT:            // not profiled
        case CMSG_REQUEST_VEHICLE_NEXT_SEAT:            // not profiled
        case CMSG_REQUEST_VEHICLE_SWITCH_SEAT:          // not profiled
        case CMSG_REQUEST_VEHICLE_EXIT:                 // not profiled
        case CMSG_CONTROLLER_EJECT_PASSENGER:           // not profiled
        case CMSG_ITEM_REFUND:                          // not profiled
#endif
        case CMSG_SOCKET_GEMS:                          // not profiled
        case CMSG_WRAP_ITEM:                            // not profiled
        case CMSG_REPORT_PVP_AFK:                       // not profiled
        {
            maxPacketCounterAllowed = 10;
            break;
        }

        case CMSG_CHAR_CREATE:                          //   7               5         3 async db queries
        case CMSG_CHAR_ENUM:                            //  22               3         2 async db queries
        case CMSG_GMTICKET_CREATE:                      //   1              25         1 async db query
        case CMSG_GMTICKET_UPDATETEXT:                  //   0              15         1 async db query
        case CMSG_GMTICKET_DELETETICKET:                //   1              25         1 async db query
#ifdef LICH_KING
        case CMSG_GMRESPONSE_RESOLVE:                   //   1              25         1 async db query
        case CMSG_CALENDAR_UPDATE_EVENT:                // not profiled
        case CMSG_CALENDAR_REMOVE_EVENT:                // not profiled
        case CMSG_CALENDAR_COPY_EVENT:                  // not profiled
        case CMSG_CALENDAR_EVENT_INVITE:                // not profiled
        case CMSG_CALENDAR_EVENT_SIGNUP:                // not profiled
        case CMSG_CALENDAR_EVENT_RSVP:                  // not profiled
        case CMSG_CALENDAR_EVENT_REMOVE_INVITE:         // not profiled
        case CMSG_CALENDAR_EVENT_MODERATOR_STATUS:      // not profiled
#endif
        case CMSG_ARENA_TEAM_INVITE:                    // not profiled
        case CMSG_ARENA_TEAM_ACCEPT:                    // not profiled
        case CMSG_ARENA_TEAM_DECLINE:                   // not profiled
        case CMSG_ARENA_TEAM_LEAVE:                     // not profiled
        case CMSG_ARENA_TEAM_DISBAND:                   // not profiled
        case CMSG_ARENA_TEAM_REMOVE:                    // not profiled
        case CMSG_ARENA_TEAM_LEADER:                    // not profiled
        case CMSG_LOOT_METHOD:                          // not profiled
        case CMSG_GUILD_INVITE:                         // not profiled
        case CMSG_GUILD_ACCEPT:                         // not profiled
        case CMSG_GUILD_DECLINE:                        // not profiled
        case CMSG_GUILD_LEAVE:                          // not profiled
        case CMSG_GUILD_DISBAND:                        // not profiled
        case CMSG_GUILD_LEADER:                         // not profiled
        case CMSG_GUILD_MOTD:                           // not profiled
        case CMSG_GUILD_RANK:                           // not profiled
        case CMSG_GUILD_ADD_RANK:                       // not profiled
        case CMSG_GUILD_DEL_RANK:                       // not profiled
        case CMSG_GUILD_INFO_TEXT:                      // not profiled
        case CMSG_GUILD_BANK_DEPOSIT_MONEY:             // not profiled
        case CMSG_GUILD_BANK_WITHDRAW_MONEY:            // not profiled
        case CMSG_GUILD_BANK_BUY_TAB:                   // not profiled
        case CMSG_GUILD_BANK_UPDATE_TAB:                // not profiled
        case CMSG_SET_GUILD_BANK_TEXT:                  // not profiled
        case MSG_SAVE_GUILD_EMBLEM:                     // not profiled
        case MSG_PETITION_RENAME:                       // not profiled
        case MSG_TALENT_WIPE_CONFIRM:                   // not profiled
        case MSG_SET_DUNGEON_DIFFICULTY:                // not profiled
#ifdef LICH_KING
        case MSG_SET_RAID_DIFFICULTY:                   // not profiled
#endif
        case MSG_PARTY_ASSIGNMENT:                      // not profiled
        case MSG_RAID_READY_CHECK:                      // not profiled
        {
            maxPacketCounterAllowed = 3;
            break;
        }
#ifdef LICH_KING
        case CMSG_ITEM_REFUND_INFO:                     // not profiled
        {
            maxPacketCounterAllowed = PLAYER_SLOTS_COUNT;
            break;
        }
#endif
        default:
        {
            maxPacketCounterAllowed = 100;
            break;
        }
    }

    return maxPacketCounterAllowed;
}

void WorldSession::SendAccountDataTimes(uint32 mask /* = 0 */)
{
    switch(GetClientBuild())
    {
#ifdef LICH_KING
    case BUILD_335:
    {
        WorldPacket data(SMSG_ACCOUNT_DATA_TIMES, 4 + 1 + 4 + NUM_ACCOUNT_DATA_TYPES * 4);
        data << uint32(time(NULL));                             // Server time
        data << uint8(1);
        data << uint32(mask);                                   // type mask
        for (uint32 i = 0; i < NUM_ACCOUNT_DATA_TYPES; ++i)
            if (mask & (1 << i))
                data << uint32(GetAccountData(AccountDataType(i))->Time);// also unix time
        SendPacket(&data);
    }
    break;
#endif
    case BUILD_243:
    default:
    {
        WorldPacket data(SMSG_ACCOUNT_DATA_TIMES, 128 );
        for(int i = 0; i < 32; i++)
            data << uint32(0);
        SendPacket(&data);
    }
    break;
    }
}

void WorldSession::SendMountResult(MountResult res)
{
    WorldPacket data(SMSG_MOUNTRESULT, 4 );
    data << uint32(res);
    SendPacket(&data);
}

void WorldSession::SendMinimapPing(uint64 guid, uint32 x, uint32 y)
{
    WorldPacket data(MSG_MINIMAP_PING, (8+4+4));
    data << uint64(guid);
    data << uint32(x);
    data << uint32(y);
    SendPacket(&data);
}

void WorldSession::SendSoundFromObject(uint32 soundId, uint64 guid)
{
    WorldPacket data(SMSG_PLAY_OBJECT_SOUND,4+8);
    data << uint32(soundId);
    data << uint64(guid);
    SendPacket(&data);
}

void WorldSession::SendMotd()
{
    WorldPacket data(SMSG_MOTD, 50);
    data << (uint32)0;

    uint32 linecount=0;
    std::string str_motd = sWorld->GetMotd();
    std::string::size_type pos, nextpos;

    pos = 0;
    while ( (nextpos= str_motd.find('@',pos)) != std::string::npos )
    {
        if (nextpos != pos)
        {
            data << str_motd.substr(pos,nextpos-pos);
            ++linecount;
        }
        pos = nextpos+1;
    }

    if (pos<str_motd.length())
    {
        data << str_motd.substr(pos);
        ++linecount;
    }

    data.put(0, linecount);

    SendPacket( &data );
    //TC_LOG_DEBUG( "network", "WORLD: Sent motd (SMSG_MOTD)" );
}

void WorldSession::SendTitleEarned(uint32 titleIndex, bool earned)
{
    WorldPacket data(SMSG_TITLE_EARNED, 4+4);
    data << uint32(titleIndex);
    data << uint32(earned);
    SendPacket(&data);
}

void WorldSession::SendClearTarget(uint64 target)
{
    WorldPacket data(SMSG_CLEAR_TARGET, 4);
    data << uint64(target);
    SendPacket(&data);
}

void WorldSession::HandleSpellClick(WorldPacket& recvData)
{
    uint64 guid;
    recvData >> guid;

    // this will get something not in world. crash
    Creature* unit = ObjectAccessor::GetCreatureOrPetOrVehicle(*_player, guid);

    if (!unit)
        return;

    /// @todo Unit::SetCharmedBy: 28782 is not in world but 0 is trying to charm it! -> crash
    if (!unit->IsInWorld())
        return;

    unit->HandleSpellClick(_player);
}


void WorldSession::ReadMovementInfo(WorldPacket &data, MovementInfo* mi)
{
    data >> mi->flags;
    data >> mi->flags2;
    data >> mi->time;
    data >> mi->pos.PositionXYZOStream();

    if (mi->HasMovementFlag(MOVEMENTFLAG_ONTRANSPORT))
    {
#ifdef LICH_KING
        data.readPackGUID(mi->transport.guid);

        data >> mi->transport.pos.PositionXYZOStream();
        data >> mi->transport.time;
        data >> mi->transport.seat;

        if (mi->HasExtraMovementFlag(MOVEMENTFLAG2_INTERPOLATED_MOVEMENT))
            data >> mi->transport.time2;
#else
        data >> mi->transport.guid;
        data >> mi->transport.pos.PositionXYZOStream();
        data >> mi->transport.time;
#endif
    }

    if (mi->HasMovementFlag(MovementFlags(MOVEMENTFLAG_SWIMMING | MOVEMENTFLAG_PLAYER_FLYING))
#ifdef LICH_KING
        || (mi->HasExtraMovementFlag(MOVEMENTFLAG2_ALWAYS_ALLOW_PITCHING))
#endif
        )
        data >> mi->pitch;

    data >> mi->fallTime;

    if (mi->HasMovementFlag(MOVEMENTFLAG_JUMPING_OR_FALLING))
    {
        data >> mi->jump.zspeed;
        data >> mi->jump.sinAngle;
        data >> mi->jump.cosAngle;
        data >> mi->jump.xyspeed;
    }

    if (mi->HasMovementFlag(MOVEMENTFLAG_SPLINE_ELEVATION))
        data >> mi->splineElevation;

    //! Anti-cheat checks. Please keep them in seperate if() blocks to maintain a clear overview.
    //! Might be subject to latency, so just remove improper flags.
#ifdef TRINITY_DEBUG
#define REMOVE_VIOLATING_FLAGS(check, maskToRemove) \
    { \
        if (check) \
        { \
            TC_LOG_DEBUG("entities.unit", "WorldSession::ReadMovementInfo: Violation of MovementFlags found (%s). " \
                "MovementFlags: %u, MovementFlags2: %u for player GUID: %u. Mask %u will be removed.", \
                STRINGIZE(check), mi->GetMovementFlags(), mi->GetExtraMovementFlags(), GetPlayer()->GetGUIDLow(), maskToRemove); \
            mi->RemoveMovementFlag((maskToRemove)); \
        } \
    }
#else
#define REMOVE_VIOLATING_FLAGS(check, maskToRemove) \
        if (check) \
            mi->RemoveMovementFlag((maskToRemove));
#endif


    /*! This must be a packet spoofing attempt. MOVEMENTFLAG_ROOT sent from the client is not valid
    in conjunction with any of the moving movement flags such as MOVEMENTFLAG_FORWARD.
    It will freeze clients that receive this player's movement info.
    */
    REMOVE_VIOLATING_FLAGS(mi->HasMovementFlag(MOVEMENTFLAG_ROOT),
        MOVEMENTFLAG_ROOT);

    //! Cannot hover without SPELL_AURA_HOVER
    REMOVE_VIOLATING_FLAGS(mi->HasMovementFlag(MOVEMENTFLAG_HOVER) && !GetPlayer()->m_unitMovedByMe->HasAuraType(SPELL_AURA_HOVER), // sunwell: added m_unitMovedByMe
        MOVEMENTFLAG_HOVER);

    //! Cannot ascend and descend at the same time
    REMOVE_VIOLATING_FLAGS(mi->HasMovementFlag(MOVEMENTFLAG_ASCENDING) && mi->HasMovementFlag(MOVEMENTFLAG_DESCENDING),
        MOVEMENTFLAG_ASCENDING | MOVEMENTFLAG_DESCENDING);

    //! Cannot move left and right at the same time
    REMOVE_VIOLATING_FLAGS(mi->HasMovementFlag(MOVEMENTFLAG_LEFT) && mi->HasMovementFlag(MOVEMENTFLAG_RIGHT),
        MOVEMENTFLAG_LEFT | MOVEMENTFLAG_RIGHT);

    //! Cannot strafe left and right at the same time
    REMOVE_VIOLATING_FLAGS(mi->HasMovementFlag(MOVEMENTFLAG_STRAFE_LEFT) && mi->HasMovementFlag(MOVEMENTFLAG_STRAFE_RIGHT),
        MOVEMENTFLAG_STRAFE_LEFT | MOVEMENTFLAG_STRAFE_RIGHT);

    //! Cannot pitch up and down at the same time
    REMOVE_VIOLATING_FLAGS(mi->HasMovementFlag(MOVEMENTFLAG_PITCH_UP) && mi->HasMovementFlag(MOVEMENTFLAG_PITCH_DOWN),
        MOVEMENTFLAG_PITCH_UP | MOVEMENTFLAG_PITCH_DOWN);

    //! Cannot move forwards and backwards at the same time
    REMOVE_VIOLATING_FLAGS(mi->HasMovementFlag(MOVEMENTFLAG_FORWARD) && mi->HasMovementFlag(MOVEMENTFLAG_BACKWARD),
        MOVEMENTFLAG_FORWARD | MOVEMENTFLAG_BACKWARD);

    //! Cannot walk on water without SPELL_AURA_WATER_WALK
    REMOVE_VIOLATING_FLAGS(mi->HasMovementFlag(MOVEMENTFLAG_WATERWALKING) && !GetPlayer()->m_unitMovedByMe->HasAuraType(SPELL_AURA_WATER_WALK) && !GetPlayer()->m_unitMovedByMe->HasAuraType(SPELL_AURA_GHOST), // sunwell: added m_unitMovedByMe
        MOVEMENTFLAG_WATERWALKING);

    //! Cannot feather fall without SPELL_AURA_FEATHER_FALL
    REMOVE_VIOLATING_FLAGS(mi->HasMovementFlag(MOVEMENTFLAG_FALLING_SLOW) && !GetPlayer()->m_unitMovedByMe->HasAuraType(SPELL_AURA_FEATHER_FALL), // sunwell: added m_unitMovedByMe
        MOVEMENTFLAG_FALLING_SLOW);

    /*! Cannot fly if no fly auras present. Exception is being a GM.
    Note that we check for account level instead of Player::IsGameMaster() because in some
    situations it may be feasable to use .gm fly on as a GM without having .gm on,
    e.g. aerial combat.
    */

    // sunwell: remade this condition
    bool canFly = GetPlayer()->m_unitMovedByMe->HasAuraType(SPELL_AURA_FLY) || GetPlayer()->m_unitMovedByMe->HasAuraType(SPELL_AURA_MOD_INCREASE_MOUNTED_FLIGHT_SPEED) ||
        (GetPlayer()->m_unitMovedByMe->GetTypeId() == TYPEID_UNIT && GetPlayer()->m_unitMovedByMe->ToCreature()->CanFly()) || GetSecurity() > SEC_PLAYER;
    REMOVE_VIOLATING_FLAGS(mi->HasMovementFlag(MOVEMENTFLAG_PLAYER_FLYING | MOVEMENTFLAG_CAN_FLY) && !canFly,
        MOVEMENTFLAG_PLAYER_FLYING | MOVEMENTFLAG_CAN_FLY);

    // sunwell: added condition for disable gravity
    REMOVE_VIOLATING_FLAGS(mi->HasMovementFlag(MOVEMENTFLAG_DISABLE_GRAVITY) && (GetPlayer()->m_unitMovedByMe->GetTypeId() == TYPEID_PLAYER || !canFly),
        MOVEMENTFLAG_DISABLE_GRAVITY);

    //! Cannot fly and fall at the same time
    REMOVE_VIOLATING_FLAGS(mi->HasMovementFlag(MOVEMENTFLAG_CAN_FLY | MOVEMENTFLAG_DISABLE_GRAVITY) && mi->HasMovementFlag(MOVEMENTFLAG_JUMPING_OR_FALLING),
        MOVEMENTFLAG_JUMPING_OR_FALLING);

    // Xinef: Spline enabled flag should be never sent by client, its internal movementflag
    REMOVE_VIOLATING_FLAGS(!GetPlayer()->m_unitMovedByMe->m_movementInfo.HasMovementFlag(MOVEMENTFLAG_SPLINE_ENABLED),
        MOVEMENTFLAG_SPLINE_ENABLED);

#undef REMOVE_VIOLATING_FLAGS
}

void WorldSession::WriteMovementInfo(WorldPacket* data, MovementInfo* mi)
{
#ifdef LICH_KING
    data->appendPackGUID(mi->guid);
#endif
    *data << mi->flags;
    *data << mi->flags2;
    *data << mi->time;
    *data << mi->pos.PositionXYZOStream();

    if (mi->HasMovementFlag(MOVEMENTFLAG_ONTRANSPORT))
    {
#ifdef LICH_KING
        data->appendPackGUID(mi->transport.guid);

        *data << mi->transport.pos.PositionXYZOStream();
        *data << mi->transport.time;
        *data << mi->transport.seat;

        if (mi->HasExtraMovementFlag(MOVEMENTFLAG2_INTERPOLATED_MOVEMENT))
            *data << mi->transport.time2;
#else
        *data << mi->transport.guid;
        *data << mi->transport.pos.PositionXYZOStream();
        *data << mi->transport.time;
#endif
    }

    if (mi->HasMovementFlag(MovementFlags(MOVEMENTFLAG_SWIMMING | MOVEMENTFLAG_PLAYER_FLYING))
#ifdef LICH_KING
        || mi->HasExtraMovementFlag(MOVEMENTFLAG2_ALWAYS_ALLOW_PITCHING)
#endif
        )
        *data << mi->pitch;

    *data << mi->fallTime;

    if (mi->HasMovementFlag(MOVEMENTFLAG_JUMPING_OR_FALLING))
    {
        *data << mi->jump.zspeed;
        *data << mi->jump.sinAngle;
        *data << mi->jump.cosAngle;
        *data << mi->jump.xyspeed;
    }

    if (mi->HasMovementFlag(MOVEMENTFLAG_SPLINE_ELEVATION))
        *data << mi->splineElevation;
}


// send update packet with apropriate version for session. Will build packets for version only if needed.
// /!\ You'll need to destroy them yourself by calling this
void WorldSession::SendUpdateDataPacketForBuild(UpdateData& data, WorldPacket*& packetBC, WorldPacket*& packetLK, WorldSession* session, bool hasTransport)
{
    switch(session->GetClientBuild())
    {
#ifdef LICH_KING
    case BUILD_335:
        if(!packetLK)
        {
            packetLK = new WorldPacket();
            data.BuildPacket(packetLK, BUILD_335, hasTransport);
        }

        session->SendPacket(packetLK);
        break;
#endif
    case BUILD_243:
    default:
        if(!packetBC)
        {
            packetBC = new WorldPacket();
            data.BuildPacket(packetBC, BUILD_243, hasTransport);
        }

        session->SendPacket(packetBC);
        break;
    }
}


#ifdef PLAYERBOT
void WorldSession::HandleBotPackets()
{
    WorldPacket* packet;
    while (_recvQueue.next(packet))
    {
        ClientOpcodeHandler const* opHandle = opcodeTable.GetHandler(static_cast<OpcodeClient>(packet->GetOpcode()), BUILD_243);
        opHandle->Call(this, *packet);
        delete packet;
    }
}
#endif

bool WorldSession::StartRecording(std::string const& recordName)
{
    if (recordName == "")
        return false;

    if (m_replayRecorder)
        return false; //recorder already running

    if (!_player)
        return false;

    _player->GetPosition();

    _player->m_clientGUIDs.clear(); //clear objects for this client to force re sending them for record
    m_replayRecorder = std::make_shared<ReplayRecorder>(_player->GetGUIDLow());
    return m_replayRecorder->StartPacketDump(recordName.c_str(), WorldLocation(*_player));
}

bool WorldSession::StopRecording()
{
    if (m_replayRecorder)
    {
        m_replayRecorder = nullptr;
        return true;
    }
    else
        return false;
}

bool WorldSession::StartReplaying(std::string const& recordName)
{
    if (m_replayPlayer)
        //player already running
        return false;

    //no player to play for
    if (!_player)
        return false;

    if (recordName == "")
        return false;

    m_replayPlayer = std::make_shared<ReplayPlayer>(_player);
    WorldLocation teleportTo;
    bool result = m_replayPlayer->ReadFromFile(recordName, teleportTo);
    if (!result)
        return false;

    _player->TeleportTo(teleportTo);
    return true;
}

bool WorldSession::StopReplaying()
{
    if (m_replayPlayer == nullptr)
        return false;

    m_replayPlayer = nullptr;
    if (_player)
        ChatHandler(_player).SendSysMessage("Replaying stopped");

    return true;
}
