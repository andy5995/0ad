/* Copyright (C) 2017 Wildfire Games.
 * This file is part of 0 A.D.
 *
 * 0 A.D. is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * 0 A.D. is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with 0 A.D.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "precompiled.h"

#include "NetClient.h"

#include "NetClientTurnManager.h"
#include "NetMessage.h"
#include "NetSession.h"

#include "lib/byte_order.h"
#include "lib/external_libraries/enet.h"
#include "lib/sysdep/sysdep.h"
#include "ps/CConsole.h"
#include "ps/CLogger.h"
#include "ps/Compress.h"
#include "ps/CStr.h"
#include "ps/Game.h"
#include "ps/GUID.h"
#include "ps/Loader.h"
#include "scriptinterface/ScriptInterface.h"
#include "scriptinterface/ScriptRuntime.h"
#include "simulation2/Simulation2.h"

CNetClient *g_NetClient = NULL;

/**
 * Async task for receiving the initial game state when rejoining an
 * in-progress network game.
 */
class CNetFileReceiveTask_ClientRejoin : public CNetFileReceiveTask
{
	NONCOPYABLE(CNetFileReceiveTask_ClientRejoin);
public:
	CNetFileReceiveTask_ClientRejoin(CNetClientWorker& client)
		: m_Client(client)
	{
	}

	virtual void OnComplete()
	{
		// We've received the game state from the server

		// Save it so we can use it after the map has finished loading
		m_Client.m_JoinSyncBuffer = m_Buffer;

		// Pretend the server told us to start the game
		CGameStartMessage start;
		m_Client.HandleMessage(&start);
	}

private:
	CNetClientWorker& m_Client;
};

CNetClientWorker::CNetClientWorker(CGame* game, bool isLocalClient) :
	m_Session(NULL),
	m_UserName(L"anonymous"),
	m_Shutdown(false),
	m_ScriptInterface(NULL),
	m_GUID(ps_generate_guid()), m_HostID((u32)-1), m_ClientTurnManager(NULL), m_Game(game),
	m_GameAttributes(game->GetSimulation2()->GetScriptInterface().GetContext()),
	m_IsLocalClient(isLocalClient),
	m_LastConnectionCheck(0),
	m_Rejoin(false)
{
	m_State = NCS_UNCONNECTED;
	m_Game->SetTurnManager(NULL); // delete the old local turn manager so we don't accidentally use it

	void* context = this;

		// Set up transitions for session
	AddTransition(NCS_UNCONNECTED, (uint)NMT_CONNECT_COMPLETE, NCS_CONNECT, (void*)&OnConnect, context);

	AddTransition(NCS_CONNECT, (uint)NMT_SERVER_HANDSHAKE, NCS_HANDSHAKE, (void*)&OnHandshake, context);

	AddTransition(NCS_HANDSHAKE, (uint)NMT_SERVER_HANDSHAKE_RESPONSE, NCS_AUTHENTICATE, (void*)&OnHandshakeResponse, context);

	AddTransition(NCS_AUTHENTICATE, (uint)NMT_AUTHENTICATE_RESULT, NCS_INITIAL_GAMESETUP, (void*)&OnAuthenticate, context);

	AddTransition(NCS_INITIAL_GAMESETUP, (uint)NMT_GAME_SETUP, NCS_PREGAME, (void*)&OnGameSetup, context);

	AddTransition(NCS_PREGAME, (uint)NMT_CHAT, NCS_PREGAME, (void*)&OnChat, context);
	AddTransition(NCS_PREGAME, (uint)NMT_READY, NCS_PREGAME, (void*)&OnReady, context);
	AddTransition(NCS_PREGAME, (uint)NMT_GAME_SETUP, NCS_PREGAME, (void*)&OnGameSetup, context);
	AddTransition(NCS_PREGAME, (uint)NMT_PLAYER_ASSIGNMENT, NCS_PREGAME, (void*)&OnPlayerAssignment, context);
	AddTransition(NCS_PREGAME, (uint)NMT_KICKED, NCS_PREGAME, (void*)&OnKicked, context);
	AddTransition(NCS_PREGAME, (uint)NMT_CLIENT_TIMEOUT, NCS_PREGAME, (void*)&OnClientTimeout, context);
	AddTransition(NCS_PREGAME, (uint)NMT_CLIENT_PERFORMANCE, NCS_PREGAME, (void*)&OnClientPerformance, context);
	AddTransition(NCS_PREGAME, (uint)NMT_GAME_START, NCS_LOADING, (void*)&OnGameStart, context);
	AddTransition(NCS_PREGAME, (uint)NMT_JOIN_SYNC_START, NCS_JOIN_SYNCING, (void*)&OnJoinSyncStart, context);

	AddTransition(NCS_JOIN_SYNCING, (uint)NMT_CHAT, NCS_JOIN_SYNCING, (void*)&OnChat, context);
	AddTransition(NCS_JOIN_SYNCING, (uint)NMT_GAME_SETUP, NCS_JOIN_SYNCING, (void*)&OnGameSetup, context);
	AddTransition(NCS_JOIN_SYNCING, (uint)NMT_PLAYER_ASSIGNMENT, NCS_JOIN_SYNCING, (void*)&OnPlayerAssignment, context);
	AddTransition(NCS_JOIN_SYNCING, (uint)NMT_KICKED, NCS_JOIN_SYNCING, (void*)&OnKicked, context);
	AddTransition(NCS_JOIN_SYNCING, (uint)NMT_CLIENT_TIMEOUT, NCS_JOIN_SYNCING, (void*)&OnClientTimeout, context);
	AddTransition(NCS_JOIN_SYNCING, (uint)NMT_CLIENT_PERFORMANCE, NCS_JOIN_SYNCING, (void*)&OnClientPerformance, context);
	AddTransition(NCS_JOIN_SYNCING, (uint)NMT_GAME_START, NCS_JOIN_SYNCING, (void*)&OnGameStart, context);
	AddTransition(NCS_JOIN_SYNCING, (uint)NMT_SIMULATION_COMMAND, NCS_JOIN_SYNCING, (void*)&OnInGame, context);
	AddTransition(NCS_JOIN_SYNCING, (uint)NMT_END_COMMAND_BATCH, NCS_JOIN_SYNCING, (void*)&OnJoinSyncEndCommandBatch, context);
	AddTransition(NCS_JOIN_SYNCING, (uint)NMT_LOADED_GAME, NCS_INGAME, (void*)&OnLoadedGame, context);

	AddTransition(NCS_LOADING, (uint)NMT_CHAT, NCS_LOADING, (void*)&OnChat, context);
	AddTransition(NCS_LOADING, (uint)NMT_GAME_SETUP, NCS_LOADING, (void*)&OnGameSetup, context);
	AddTransition(NCS_LOADING, (uint)NMT_PLAYER_ASSIGNMENT, NCS_LOADING, (void*)&OnPlayerAssignment, context);
	AddTransition(NCS_LOADING, (uint)NMT_KICKED, NCS_LOADING, (void*)&OnKicked, context);
	AddTransition(NCS_LOADING, (uint)NMT_CLIENT_TIMEOUT, NCS_LOADING, (void*)&OnClientTimeout, context);
	AddTransition(NCS_LOADING, (uint)NMT_CLIENT_PERFORMANCE, NCS_LOADING, (void*)&OnClientPerformance, context);
	AddTransition(NCS_LOADING, (uint)NMT_CLIENTS_LOADING, NCS_LOADING, (void*)&OnClientsLoading, context);
	AddTransition(NCS_LOADING, (uint)NMT_LOADED_GAME, NCS_INGAME, (void*)&OnLoadedGame, context);

	AddTransition(NCS_INGAME, (uint)NMT_REJOINED, NCS_INGAME, (void*)&OnRejoined, context);
	AddTransition(NCS_INGAME, (uint)NMT_KICKED, NCS_INGAME, (void*)&OnKicked, context);
	AddTransition(NCS_INGAME, (uint)NMT_CLIENT_TIMEOUT, NCS_INGAME, (void*)&OnClientTimeout, context);
	AddTransition(NCS_INGAME, (uint)NMT_CLIENT_PERFORMANCE, NCS_INGAME, (void*)&OnClientPerformance, context);
	AddTransition(NCS_INGAME, (uint)NMT_CLIENTS_LOADING, NCS_INGAME, (void*)&OnClientsLoading, context);
	AddTransition(NCS_INGAME, (uint)NMT_CLIENT_PAUSED, NCS_INGAME, (void*)&OnClientPaused, context);
	AddTransition(NCS_INGAME, (uint)NMT_CHAT, NCS_INGAME, (void*)&OnChat, context);
	AddTransition(NCS_INGAME, (uint)NMT_GAME_SETUP, NCS_INGAME, (void*)&OnGameSetup, context);
	AddTransition(NCS_INGAME, (uint)NMT_PLAYER_ASSIGNMENT, NCS_INGAME, (void*)&OnPlayerAssignment, context);
	AddTransition(NCS_INGAME, (uint)NMT_SIMULATION_COMMAND, NCS_INGAME, (void*)&OnInGame, context);
	AddTransition(NCS_INGAME, (uint)NMT_SYNC_ERROR, NCS_INGAME, (void*)&OnInGame, context);
	AddTransition(NCS_INGAME, (uint)NMT_END_COMMAND_BATCH, NCS_INGAME, (void*)&OnInGame, context);

	// Set first state
	SetFirstState(NCS_UNCONNECTED);
}

CNetClientWorker::~CNetClientWorker()
{
	DestroyConnection();

	if (m_State != NCS_UNCONNECTED)
	{
		// Tell the thread to shut down
		{
			CScopeLock lock(m_WorkerMutex);
			m_Shutdown = true;
		}

		// Wait for it to shut down cleanly
		int ret = pthread_join(m_WorkerThread, NULL);
		printf ("%d - return join\n", ret);
	}

	// Clean up resources

	// delete m_Stats;

	/*for (CNetClientSession* session : m_Session)
	{
		session->Disconnect(NCS_UNCONNECTED);
		delete session;
	}*

	if (m_Host)
		enet_host_destroy(m_Host);

	delete m_ServerTurnManager; */
}

void CNetClientWorker::SetUserName(const CStrW& username)
{
	ENSURE(!m_Session); // must be called before we start the connection

	m_UserName = username;
}

bool CNetClientWorker::SetupConnection(const CStr& server, const u16 port)
{
	CNetClientSession* session = new CNetClientSession(*this);
	bool ok = session->Connect(server, port, m_IsLocalClient, enetClient);
	SetAndOwnSession(session);

	m_State = NCS_PREGAME;

	// Launch the worker thread
	int ret = pthread_create(&m_WorkerThread, NULL, &RunThread, this);
	ENSURE(ret == 0);

	return ok;
}

void CNetClientWorker::SetAndOwnSession(CNetClientSession* session)
{
	delete m_Session;
	m_Session = session;
}

void CNetClientWorker::DestroyConnection()
{
	// Send network messages from the current frame before connection is destroyed.
	if (m_ClientTurnManager)
	{
		m_ClientTurnManager->OnDestroyConnection(); // End sending of commands for scheduled turn.
		Flush(); // Make sure the messages are sent.
	}
	SAFE_DELETE(m_Session);
}

void CNetClientWorker::Poll()
{
	if (!m_Session)
		return;

	CheckServerConnection();
	m_Session->Poll();
}

void* CNetClientWorker::RunThread(void* data)
{
	// debug_SetThreadName("NetClient");
	static_cast<CNetClientWorker*>(data)->Run();

	return NULL;
}

void CNetClientWorker::Run()
{
	// The script runtime uses the profiler and therefore the thread must be registered before the runtime is created
	g_Profiler2.RegisterCurrentThread("Net client");
	m_ScriptInterface = new ScriptInterface("Engine", "Net client", ScriptInterface::CreateRuntime(g_ScriptRuntime));

	m_GameAttributes.init(m_ScriptInterface->GetJSRuntime(), JS::UndefinedValue());

	while (true)
	{
		if (!RunStep())
			break;

		// Implement autostart mode
		//if (m_State == CLIENT_STATE_PREGAME && (int)m_PlayerAssignments.size() == m_AutostartPlayers)
		//	StartGame();

		// Update profiler stats
		//m_Stats->LatchHostState(m_Host);
	}

	SAFE_DELETE(m_ScriptInterface);
}

bool CNetClientWorker::RunStep()
{
	// Check for messages from the game thread.
	// (Do as little work as possible while the mutex is held open,
	// to avoid performance problems and deadlocks.)
	m_ScriptInterface->GetRuntime()->MaybeIncrementalGC(0.5f);

	JSContext* cx = m_ScriptInterface->GetContext();
	JSAutoRequest rq(cx);

	std::vector<bool> newStartGame;
	std::vector<std::string> newGameAttributes;
	std::vector<u32> newTurnLength;

	{
		CScopeLock lock(m_WorkerMutex);

		if (m_Shutdown)
			return false;

		newStartGame.swap(m_StartGameQueue);
		newGameAttributes.swap(m_GameAttributesQueue);
		newTurnLength.swap(m_TurnLengthQueue);
	}

	CheckServerConnection();

	return true;
}

void CNetClientWorker::CheckServerConnection()
{
	// Trigger local warnings if the connection to the server is bad.
	// At most once per second.
	std::time_t now = std::time(nullptr);
	if (now <= m_LastConnectionCheck)
		return;

	m_LastConnectionCheck = now;

	JSContext* cx = GetScriptInterface().GetContext();
	JSAutoRequest rq(cx);

	// Report if we are losing the connection to the server
	u32 lastReceived = m_Session->GetLastReceivedTime();
	if (lastReceived > NETWORK_WARNING_TIMEOUT)
	{
		JS::RootedValue msg(cx);
		GetScriptInterface().Eval("({ 'type':'netwarn', 'warntype': 'server-timeout' })", &msg);
		GetScriptInterface().SetProperty(msg, "lastReceivedTime", lastReceived);
		PushGuiMessage(msg);
		return;
	}

	// Report if we have a bad ping to the server
	u32 meanRTT = m_Session->GetMeanRTT();
	if (meanRTT > DEFAULT_TURN_LENGTH_MP)
	{
		JS::RootedValue msg(cx);
		GetScriptInterface().Eval("({ 'type':'netwarn', 'warntype': 'server-latency' })", &msg);
		GetScriptInterface().SetProperty(msg, "meanRTT", meanRTT);
		PushGuiMessage(msg);
	}
}

void CNetClientWorker::Flush()
{
	if (m_Session)
		m_Session->Flush();
}

void CNetClientWorker::GuiPoll(JS::MutableHandleValue ret)
{
	if (m_GuiMessageQueue.empty())
	{
		ret.setUndefined();
		return;
	}

	ret.set(m_GuiMessageQueue.front());
	m_GuiMessageQueue.pop_front();
}

void CNetClientWorker::PushGuiMessage(const JS::HandleValue message)
{
	ENSURE(!message.isUndefined());

	m_GuiMessageQueue.push_back(JS::Heap<JS::Value>(message));
}

std::string CNetClientWorker::TestReadGuiMessages()
{
	JSContext* cx = GetScriptInterface().GetContext();
	JSAutoRequest rq(cx);

	std::string r;
	JS::RootedValue msg(cx);
	while (true)
	{
		GuiPoll(&msg);
		if (msg.isUndefined())
			break;
		r += GetScriptInterface().ToString(&msg) + "\n";
	}
	return r;
}

ScriptInterface& CNetClientWorker::GetScriptInterface()
{
	return m_Game->GetSimulation2()->GetScriptInterface();
}

void CNetClientWorker::PostPlayerAssignmentsToScript()
{
	JSContext* cx = GetScriptInterface().GetContext();
	JSAutoRequest rq(cx);

	JS::RootedValue msg(cx);
	GetScriptInterface().Eval("({'type':'players', 'newAssignments':{}})", &msg);

	JS::RootedValue newAssignments(cx);
	GetScriptInterface().GetProperty(msg, "newAssignments", &newAssignments);

	for (const std::pair<CStr, PlayerAssignment>& p : m_PlayerAssignments)
	{
		JS::RootedValue assignment(cx);
		GetScriptInterface().Eval("({})", &assignment);
		GetScriptInterface().SetProperty(assignment, "name", CStrW(p.second.m_Name), false);
		GetScriptInterface().SetProperty(assignment, "player", p.second.m_PlayerID, false);
		GetScriptInterface().SetProperty(assignment, "status", p.second.m_Status, false);
		GetScriptInterface().SetProperty(newAssignments, p.first.c_str(), assignment, false);
	}

	PushGuiMessage(msg);
}

bool CNetClientWorker::SendMessage(const CNetMessage* message)
{
	if (!m_Session)
		return false;

	return m_Session->SendMessage(message);
}

void CNetClientWorker::HandleConnect()
{
	Update((uint)NMT_CONNECT_COMPLETE, NULL);
}

void CNetClientWorker::HandleDisconnect(u32 reason)
{
	JSContext* cx = GetScriptInterface().GetContext();
	JSAutoRequest rq(cx);

	JS::RootedValue msg(cx);
	GetScriptInterface().Eval("({'type':'netstatus','status':'disconnected'})", &msg);
	GetScriptInterface().SetProperty(msg, "reason", (int)reason, false);
	PushGuiMessage(msg);

	SAFE_DELETE(m_Session);

	// Update the state immediately to UNCONNECTED (don't bother with FSM transitions since
	// we'd need one for every single state, and we don't need to use per-state actions)
	SetCurrState(NCS_UNCONNECTED);
}

void CNetClientWorker::SendGameSetupMessage(JS::MutableHandleValue attrs, ScriptInterface& scriptInterface)
{
	CGameSetupMessage gameSetup(scriptInterface);
	gameSetup.m_Data = attrs;
	SendMessage(&gameSetup);
}

void CNetClientWorker::SendAssignPlayerMessage(const int playerID, const CStr& guid)
{
	CAssignPlayerMessage assignPlayer;
	assignPlayer.m_PlayerID = playerID;
	assignPlayer.m_GUID = guid;
	SendMessage(&assignPlayer);
}

void CNetClientWorker::SendChatMessage(const std::wstring& text)
{
	CChatMessage chat;
	chat.m_Message = text;
	SendMessage(&chat);
}

void CNetClientWorker::SendReadyMessage(const int status)
{
	CReadyMessage readyStatus;
	readyStatus.m_Status = status;
	SendMessage(&readyStatus);
}

void CNetClientWorker::SendClearAllReadyMessage()
{
	CClearAllReadyMessage clearAllReady;
	SendMessage(&clearAllReady);
}

void CNetClientWorker::SendStartGameMessage()
{
	CGameStartMessage gameStart;
	SendMessage(&gameStart);
}

void CNetClientWorker::SendRejoinedMessage()
{
	CRejoinedMessage rejoinedMessage;
	SendMessage(&rejoinedMessage);
}

void CNetClientWorker::SendKickPlayerMessage(const CStrW& playerName, bool ban)
{
	CKickedMessage kickPlayer;
	kickPlayer.m_Name = playerName;
	kickPlayer.m_Ban = ban;
	SendMessage(&kickPlayer);
}

void CNetClientWorker::SendPausedMessage(bool pause)
{
	CClientPausedMessage pausedMessage;
	pausedMessage.m_Pause = pause;
	SendMessage(&pausedMessage);
}

bool CNetClientWorker::HandleMessage(CNetMessage* message)
{
	// Handle non-FSM messages first

	Status status = m_Session->GetFileTransferer().HandleMessageReceive(message);
	if (status == INFO::OK)
		return true;
	if (status != INFO::SKIPPED)
		return false;

	if (message->GetType() == NMT_FILE_TRANSFER_REQUEST)
	{
		CFileTransferRequestMessage* reqMessage = (CFileTransferRequestMessage*)message;

		// TODO: we should support different transfer request types, instead of assuming
		// it's always requesting the simulation state

		std::stringstream stream;

		LOGMESSAGERENDER("Serializing game at turn %u for rejoining player", m_ClientTurnManager->GetCurrentTurn());
		u32 turn = to_le32(m_ClientTurnManager->GetCurrentTurn());
		stream.write((char*)&turn, sizeof(turn));

		bool ok = m_Game->GetSimulation2()->SerializeState(stream);
		ENSURE(ok);

		// Compress the content with zlib to save bandwidth
		// (TODO: if this is still too large, compressing with e.g. LZMA works much better)
		std::string compressed;
		CompressZLib(stream.str(), compressed, true);

		m_Session->GetFileTransferer().StartResponse(reqMessage->m_RequestID, compressed);

		return true;
	}

	// Update FSM
	bool ok = Update(message->GetType(), message);
	if (!ok)
		LOGERROR("Net client: Error running FSM update (type=%d state=%d)", (int)message->GetType(), (int)GetCurrState());
	return ok;
}

void CNetClientWorker::LoadFinished()
{
	JSContext* cx = GetScriptInterface().GetContext();
	JSAutoRequest rq(cx);

	if (!m_JoinSyncBuffer.empty())
	{
		// We're rejoining a game, and just finished loading the initial map,
		// so deserialize the saved game state now

		std::string state;
		DecompressZLib(m_JoinSyncBuffer, state, true);

		std::stringstream stream(state);

		u32 turn;
		stream.read((char*)&turn, sizeof(turn));
		turn = to_le32(turn);

		LOGMESSAGE("Rejoining client deserializing state at turn %u\n", turn);

		bool ok = m_Game->GetSimulation2()->DeserializeState(stream);
		ENSURE(ok);

		m_ClientTurnManager->ResetState(turn, turn);

		JS::RootedValue msg(cx);
		GetScriptInterface().Eval("({'type':'netstatus','status':'join_syncing'})", &msg);
		PushGuiMessage(msg);
	}
	else
	{
		// Connecting at the start of a game, so we'll wait for other players to finish loading
		JS::RootedValue msg(cx);
		GetScriptInterface().Eval("({'type':'netstatus','status':'waiting_for_players'})", &msg);
		PushGuiMessage(msg);
	}

	CLoadedGameMessage loaded;
	loaded.m_CurrentTurn = m_ClientTurnManager->GetCurrentTurn();
	SendMessage(&loaded);
}

bool CNetClientWorker::OnConnect(void* context, CFsmEvent* event)
{
	ENSURE(event->GetType() == (uint)NMT_CONNECT_COMPLETE);

	CNetClientWorker* client = (CNetClientWorker*)context;

	JSContext* cx = client->GetScriptInterface().GetContext();
	JSAutoRequest rq(cx);

	JS::RootedValue msg(cx);
	client->GetScriptInterface().Eval("({'type':'netstatus','status':'connected'})", &msg);
	client->PushGuiMessage(msg);

	return true;
}

bool CNetClientWorker::OnHandshake(void* context, CFsmEvent* event)
{
	ENSURE(event->GetType() == (uint)NMT_SERVER_HANDSHAKE);

	CNetClientWorker* client = (CNetClientWorker*)context;

	CCliHandshakeMessage handshake;
	handshake.m_MagicResponse = PS_PROTOCOL_MAGIC_RESPONSE;
	handshake.m_ProtocolVersion = PS_PROTOCOL_VERSION;
	handshake.m_SoftwareVersion = PS_PROTOCOL_VERSION;
	client->SendMessage(&handshake);

	return true;
}

bool CNetClientWorker::OnHandshakeResponse(void* context, CFsmEvent* event)
{
	ENSURE(event->GetType() == (uint)NMT_SERVER_HANDSHAKE_RESPONSE);

	CNetClientWorker* client = (CNetClientWorker*)context;

	CAuthenticateMessage authenticate;
	authenticate.m_GUID = client->m_GUID;
	authenticate.m_Name = client->m_UserName;
	authenticate.m_Password = L""; // TODO
	authenticate.m_IsLocalClient = client->m_IsLocalClient;
	client->SendMessage(&authenticate);

	return true;
}

bool CNetClientWorker::OnAuthenticate(void* context, CFsmEvent* event)
{
	ENSURE(event->GetType() == (uint)NMT_AUTHENTICATE_RESULT);

	CNetClientWorker* client = (CNetClientWorker*)context;

	JSContext* cx = client->GetScriptInterface().GetContext();
	JSAutoRequest rq(cx);

	CAuthenticateResultMessage* message = (CAuthenticateResultMessage*)event->GetParamRef();

	LOGMESSAGE("Net: Authentication result: host=%u, %s", message->m_HostID, utf8_from_wstring(message->m_Message));

	client->m_HostID = message->m_HostID;
	client->m_Rejoin = message->m_Code == ARC_OK_REJOINING;

	JS::RootedValue msg(cx);
	client->GetScriptInterface().Eval("({'type':'netstatus','status':'authenticated'})", &msg);
	client->GetScriptInterface().SetProperty(msg, "rejoining", client->m_Rejoin);
	client->PushGuiMessage(msg);

	return true;
}

bool CNetClientWorker::OnChat(void* context, CFsmEvent* event)
{
	ENSURE(event->GetType() == (uint)NMT_CHAT);

	CNetClientWorker* client = (CNetClientWorker*)context;
	JSContext* cx = client->GetScriptInterface().GetContext();
	JSAutoRequest rq(cx);

	CChatMessage* message = (CChatMessage*)event->GetParamRef();

	JS::RootedValue msg(cx);
	client->GetScriptInterface().Eval("({'type':'chat'})", &msg);
	client->GetScriptInterface().SetProperty(msg, "guid", std::string(message->m_GUID), false);
	client->GetScriptInterface().SetProperty(msg, "text", std::wstring(message->m_Message), false);
	client->PushGuiMessage(msg);

	return true;
}

bool CNetClientWorker::OnReady(void* context, CFsmEvent* event)
{
	ENSURE(event->GetType() == (uint)NMT_READY);

	CNetClientWorker* client = (CNetClientWorker*)context;
	JSContext* cx = client->GetScriptInterface().GetContext();
	JSAutoRequest rq(cx);

	CReadyMessage* message = (CReadyMessage*)event->GetParamRef();

	JS::RootedValue msg(cx);
	client->GetScriptInterface().Eval("({'type':'ready'})", &msg);
	client->GetScriptInterface().SetProperty(msg, "guid", std::string(message->m_GUID), false);
	client->GetScriptInterface().SetProperty(msg, "status", int (message->m_Status), false);
	client->PushGuiMessage(msg);

	return true;
}

bool CNetClientWorker::OnGameSetup(void* context, CFsmEvent* event)
{
	ENSURE(event->GetType() == (uint)NMT_GAME_SETUP);

	CNetClientWorker* client = (CNetClientWorker*)context;
	JSContext* cx = client->GetScriptInterface().GetContext();
	JSAutoRequest rq(cx);

	CGameSetupMessage* message = (CGameSetupMessage*)event->GetParamRef();

	client->m_GameAttributes = message->m_Data;

	JS::RootedValue msg(cx);
	client->GetScriptInterface().Eval("({'type':'gamesetup'})", &msg);
	client->GetScriptInterface().SetProperty(msg, "data", message->m_Data, false);
	client->PushGuiMessage(msg);

	return true;
}

bool CNetClientWorker::OnPlayerAssignment(void* context, CFsmEvent* event)
{
	ENSURE(event->GetType() == (uint)NMT_PLAYER_ASSIGNMENT);

	CNetClientWorker* client = (CNetClientWorker*)context;

	CPlayerAssignmentMessage* message = (CPlayerAssignmentMessage*)event->GetParamRef();

	// Unpack the message
	PlayerAssignmentMap newPlayerAssignments;
	for (size_t i = 0; i < message->m_Hosts.size(); ++i)
	{
		PlayerAssignment assignment;
		assignment.m_Enabled = true;
		assignment.m_Name = message->m_Hosts[i].m_Name;
		assignment.m_PlayerID = message->m_Hosts[i].m_PlayerID;
		assignment.m_Status = message->m_Hosts[i].m_Status;
		newPlayerAssignments[message->m_Hosts[i].m_GUID] = assignment;
	}

	client->m_PlayerAssignments.swap(newPlayerAssignments);

	client->PostPlayerAssignmentsToScript();

	return true;
}

bool CNetClientWorker::OnGameStart(void* context, CFsmEvent* event)
{
	ENSURE(event->GetType() == (uint)NMT_GAME_START);

	CNetClientWorker* client = (CNetClientWorker*)context;
	JSContext* cx = client->GetScriptInterface().GetContext();
	JSAutoRequest rq(cx);

	// Find the player assigned to our GUID
	int player = -1;
	if (client->m_PlayerAssignments.find(client->m_GUID) != client->m_PlayerAssignments.end())
		player = client->m_PlayerAssignments[client->m_GUID].m_PlayerID;

	client->m_ClientTurnManager = new CNetClientTurnManager(
			*client->m_Game->GetSimulation2(), *client, client->m_HostID, client->m_Game->GetReplayLogger());

	client->m_Game->SetPlayerID(player);
	client->m_Game->StartGame(&client->m_GameAttributes, "");

	JS::RootedValue msg(cx);
	client->GetScriptInterface().Eval("({'type':'start'})", &msg);
	client->PushGuiMessage(msg);

	return true;
}

bool CNetClientWorker::OnJoinSyncStart(void* context, CFsmEvent* event)
{
	ENSURE(event->GetType() == (uint)NMT_JOIN_SYNC_START);

	CNetClientWorker* client = (CNetClientWorker*)context;

	// The server wants us to start downloading the game state from it, so do so
	client->m_Session->GetFileTransferer().StartTask(
		shared_ptr<CNetFileReceiveTask>(new CNetFileReceiveTask_ClientRejoin(*client))
	);

	return true;
}

bool CNetClientWorker::OnJoinSyncEndCommandBatch(void* context, CFsmEvent* event)
{
	ENSURE(event->GetType() == (uint)NMT_END_COMMAND_BATCH);

	CNetClientWorker* client = (CNetClientWorker*)context;

	CEndCommandBatchMessage* endMessage = (CEndCommandBatchMessage*)event->GetParamRef();

	client->m_ClientTurnManager->FinishedAllCommands(endMessage->m_Turn, endMessage->m_TurnLength);

	// Execute all the received commands for the latest turn
	client->m_ClientTurnManager->UpdateFastForward();

	return true;
}

bool CNetClientWorker::OnRejoined(void* context, CFsmEvent* event)
{
	ENSURE(event->GetType() == (uint)NMT_REJOINED);

	CNetClientWorker* client = (CNetClientWorker*)context;
	JSContext* cx = client->GetScriptInterface().GetContext();
	JSAutoRequest rq(cx);

	CRejoinedMessage* message = (CRejoinedMessage*)event->GetParamRef();
	JS::RootedValue msg(cx);
	client->GetScriptInterface().Eval("({'type':'rejoined'})", &msg);
	client->GetScriptInterface().SetProperty(msg, "guid", std::string(message->m_GUID), false);
	client->PushGuiMessage(msg);

	return true;
}

bool CNetClientWorker::OnKicked(void *context, CFsmEvent* event)
{
	ENSURE(event->GetType() == (uint)NMT_KICKED);

	CNetClientWorker* client = (CNetClientWorker*)context;
	JSContext* cx = client->GetScriptInterface().GetContext();
	JSAutoRequest rq(cx);

	CKickedMessage* message = (CKickedMessage*)event->GetParamRef();
	JS::RootedValue msg(cx);

	client->GetScriptInterface().Eval("({})", &msg);
	client->GetScriptInterface().SetProperty(msg, "username", message->m_Name);
	client->GetScriptInterface().SetProperty(msg, "type", CStr("kicked"));
	client->GetScriptInterface().SetProperty(msg, "banned", message->m_Ban != 0);
	client->PushGuiMessage(msg);

	return true;
}

bool CNetClientWorker::OnClientTimeout(void *context, CFsmEvent* event)
{
	// Report the timeout of some other client

	ENSURE(event->GetType() == (uint)NMT_CLIENT_TIMEOUT);

	CNetClientWorker* client = (CNetClientWorker*)context;
	JSContext* cx = client->GetScriptInterface().GetContext();
	JSAutoRequest rq(cx);

	if (client->GetCurrState() == NCS_LOADING)
		return true;

	CClientTimeoutMessage* message = (CClientTimeoutMessage*)event->GetParamRef();
	JS::RootedValue msg(cx);

	client->GetScriptInterface().Eval("({ 'type':'netwarn', 'warntype': 'client-timeout' })", &msg);
	client->GetScriptInterface().SetProperty(msg, "guid", std::string(message->m_GUID));
	client->GetScriptInterface().SetProperty(msg, "lastReceivedTime", message->m_LastReceivedTime);
	client->PushGuiMessage(msg);

	return true;
}

bool CNetClientWorker::OnClientPerformance(void *context, CFsmEvent* event)
{
	// Performance statistics for one or multiple clients

	ENSURE(event->GetType() == (uint)NMT_CLIENT_PERFORMANCE);

	CNetClientWorker* client = (CNetClientWorker*)context;
	JSContext* cx = client->GetScriptInterface().GetContext();
	JSAutoRequest rq(cx);

	if (client->GetCurrState() == NCS_LOADING)
		return true;

	CClientPerformanceMessage* message = (CClientPerformanceMessage*)event->GetParamRef();

	// Display warnings for other clients with bad ping
	for (size_t i = 0; i < message->m_Clients.size(); ++i)
	{
		if (message->m_Clients[i].m_MeanRTT < DEFAULT_TURN_LENGTH_MP || message->m_Clients[i].m_GUID == client->m_GUID)
			continue;

		JS::RootedValue msg(cx);
		client->GetScriptInterface().Eval("({ 'type':'netwarn', 'warntype': 'client-latency' })", &msg);
		client->GetScriptInterface().SetProperty(msg, "guid", message->m_Clients[i].m_GUID);
		client->GetScriptInterface().SetProperty(msg, "meanRTT", message->m_Clients[i].m_MeanRTT);
		client->PushGuiMessage(msg);
	}

	return true;
}

bool CNetClientWorker::OnClientsLoading(void *context, CFsmEvent *event)
{
	ENSURE(event->GetType() == (uint)NMT_CLIENTS_LOADING);

	CClientsLoadingMessage* message = (CClientsLoadingMessage*)event->GetParamRef();

	std::vector<CStr> guids;
	guids.reserve(message->m_Clients.size());
	for (const CClientsLoadingMessage::S_m_Clients& client : message->m_Clients)
		guids.push_back(client.m_GUID);

	CNetClientWorker* client = (CNetClientWorker*)context;
	JSContext* cx = client->GetScriptInterface().GetContext();
	JSAutoRequest rq(cx);

	JS::RootedValue msg(cx);
	client->GetScriptInterface().Eval("({ 'type':'clients-loading' })", &msg);
	client->GetScriptInterface().SetProperty(msg, "guids", guids);
	client->PushGuiMessage(msg);
	return true;
}

bool CNetClientWorker::OnClientPaused(void *context, CFsmEvent *event)
{
	ENSURE(event->GetType() == (uint)NMT_CLIENT_PAUSED);

	CNetClientWorker* client = (CNetClientWorker*)context;
	JSContext* cx = client->GetScriptInterface().GetContext();
	JSAutoRequest rq(cx);

	CClientPausedMessage* message = (CClientPausedMessage*)event->GetParamRef();

	JS::RootedValue msg(cx);
	client->GetScriptInterface().Eval("({ 'type':'paused' })", &msg);
	client->GetScriptInterface().SetProperty(msg, "pause", message->m_Pause != 0);
	client->GetScriptInterface().SetProperty(msg, "guid", message->m_GUID);
	client->PushGuiMessage(msg);

	return true;
}

bool CNetClientWorker::OnLoadedGame(void* context, CFsmEvent* event)
{
	ENSURE(event->GetType() == (uint)NMT_LOADED_GAME);

	CNetClientWorker* client = (CNetClientWorker*)context;
	JSContext* cx = client->GetScriptInterface().GetContext();
	JSAutoRequest rq(cx);

	// All players have loaded the game - start running the turn manager
	// so that the game begins
	client->m_Game->SetTurnManager(client->m_ClientTurnManager);

	JS::RootedValue msg(cx);
	client->GetScriptInterface().Eval("({'type':'netstatus','status':'active'})", &msg);
	client->PushGuiMessage(msg);

	// If we have rejoined an in progress game, send the rejoined message to the server.
	if (client->m_Rejoin)
		client->SendRejoinedMessage();

	return true;
}

bool CNetClientWorker::OnInGame(void *context, CFsmEvent* event)
{
	// TODO: should split each of these cases into a separate method

	CNetClientWorker* client = (CNetClientWorker*)context;

	CNetMessage* message = (CNetMessage*)event->GetParamRef();
	if (message)
	{
		if (message->GetType() == NMT_SIMULATION_COMMAND)
		{
			CSimulationMessage* simMessage = static_cast<CSimulationMessage*> (message);
			client->m_ClientTurnManager->OnSimulationMessage(simMessage);
		}
		else if (message->GetType() == NMT_SYNC_ERROR)
		{
			CSyncErrorMessage* syncMessage = static_cast<CSyncErrorMessage*> (message);
			client->m_ClientTurnManager->OnSyncError(syncMessage->m_Turn, syncMessage->m_HashExpected, syncMessage->m_PlayerNames);
		}
		else if (message->GetType() == NMT_END_COMMAND_BATCH)
		{
			CEndCommandBatchMessage* endMessage = static_cast<CEndCommandBatchMessage*> (message);
			client->m_ClientTurnManager->FinishedAllCommands(endMessage->m_Turn, endMessage->m_TurnLength);
		}
	}

	return true;
}

CNetClient::CNetClient(CGame* game, bool isLocalClient) :
	m_Worker(new CNetClientWorker(game, isLocalClient))
{
}

CNetClient::~CNetClient()
{
	delete m_Worker;
}

void CNetClient::SetUserName(const CStrW& username)
{
	m_Worker->m_UserName = username;
}

bool CNetClient::SetupConnection(const CStr& server, const u16 port)
{
	return m_Worker->SetupConnection(server, port);
}

void CNetClient::SendGameSetupMessage(JS::MutableHandleValue attrs, ScriptInterface& scriptInterface)
{
	m_Worker->SendGameSetupMessage(attrs, scriptInterface);
}

void CNetClient::SendStartGameMessage()
{
	m_Worker->SendStartGameMessage();
}

/**
 * Call to kick/ban a client
 */
void CNetClient::SendKickPlayerMessage(const CStrW& playerName, bool ban)
{
	m_Worker->SendKickPlayerMessage(playerName, ban);
}

void CNetClient::LoadFinished()
{
	m_Worker->LoadFinished();
}

void CNetClient::GuiPoll(JS::MutableHandleValue ret)
{
	m_Worker->GuiPoll(ret);
}

ScriptInterface& CNetClient::GetScriptInterface()
{
	return m_Worker->GetScriptInterface();
}

void CNetClient::SendAssignPlayerMessage(const int playerID, const CStr& guid)
{
	m_Worker->SendAssignPlayerMessage(playerID, guid);
}

void CNetClient::SendChatMessage(const std::wstring& text)
{
	m_Worker->SendChatMessage(text);
}

void CNetClient::SendReadyMessage(const int status)
{
	m_Worker->SendReadyMessage(status);
}

void CNetClient::SendClearAllReadyMessage()
{
	m_Worker->SendClearAllReadyMessage();
}

void CNetClient::SendPausedMessage(bool pause)
{
	m_Worker->SendPausedMessage(pause);
}

void CNetClient::DestroyConnection()
{
	m_Worker->DestroyConnection();
}

void CNetClient::Poll()
{
	m_Worker->Poll();
}

void CNetClient::Flush()
{
	m_Worker->Flush();
}

std::string CNetClient::TestReadGuiMessages()
{
	return m_Worker->TestReadGuiMessages();
}

