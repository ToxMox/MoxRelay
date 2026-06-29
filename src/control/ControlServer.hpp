// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ToxMox / MoxRelay contributors
//
// ControlServer -- the per-instance WebSocket control endpoint (ws://127.0.0.1:{port}/control;
// wire contract in docs/control-api.asyncapi.yaml). One ControlServer per process, instantiated
// identically by the GUI (supervisor + tier-1) and the headless worker.
//
// THREADING MODEL:
//   - The socket server runs one thread per connection (vendored IXWebSocket). The message
//     callback PARSES on that connection thread, answers protocol errors and
//     Subscribe/Unsubscribe inline (the subscription registry has its own mutex), and marshals
//     every other verb onto the Qt MAIN thread (queued invocation onto this QObject), where
//     ControlVerbs executes it; the reply is then sent back on the connection thread.
//   - The connection thread waits on the queued handler with a BOUNDED deadline. If the main
//     loop is gone (request raced the event-loop exit), the wait times out and the connection
//     replies 1009 ShuttingDown instead of deadlocking -- which keeps every connection thread
//     joinable, so stop() can always complete. Verb handlers never block on a connection
//     thread; publishing events from handlers is safe (send is a non-blocking enqueue).
//   - Event fan-out: snapshot the subscribed sockets under the registry mutex, release, send.
//     Sockets are held as shared_ptr so a concurrently-closing connection cannot free under a
//     send; send() on a closed socket is a safe no-op.
//
// LIFECYCLE: start() after engine bring-up. On teardown: publishInstanceShuttingDown() FIRST,
// then stop() (synchronous; joins all connection threads), then the engine/bootstrap teardown
// proceeds in its established order. stop() is idempotent.

#pragma once

#include <nlohmann/json.hpp>

#include <ixwebsocket/IXWebSocketMessage.h> // ix::WebSocketMessagePtr (onOpen reads the upgrade handshake)

#include <QObject>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>

class QTimer;

namespace ix {
class ConnectionState;
class WebSocket;
class WebSocketServer;
struct WebSocketMessage;
} // namespace ix

namespace moxrelay {

class ControlVerbs;

class ControlServer : public QObject {
	Q_OBJECT

public:
	explicit ControlServer(ControlVerbs *verbs, QObject *parent = nullptr);
	~ControlServer() override; // stop()

	// Set the auth token enforced on the WS upgrade. Call BEFORE start().
	// Empty token => auth disabled (legacy/degraded; the startup call site in main.cpp logs
	// the one-time WARNING). Reject any upgrade carrying an Origin header regardless of token.
	void setControlToken(std::string token);

	// Bind 127.0.0.1:{port} and start serving. False = bind/listen failure (caller decides
	// the failure policy: a worker hard-fails, the GUI degrades to Spout-only).
	bool start(int port);

	// Synchronous stop: no new requests after return; all connection threads joined.
	void stop();

	bool isRunning() const { return running_; }
	int port() const { return port_; }

	// In-process audioLevels consumer (the GUI meters). While set, the 100 ms tick runs --
	// and drains the meter window -- regardless of WS subscribers, delivering every frame to
	// this sink as well as to subscribed connections: one tick, one drain, ONE data build for
	// the wire and the GUI. Main thread only; clear (nullptr) before the consumer is destroyed.
	void setLevelsSink(std::function<void(const nlohmann::json &)> sink);

	// Main-thread event publishers (also the ControlVerbs event sink).
	void publishEvent(const std::string &name, const nlohmann::json &data); // subscription-gated
	void publishInstanceShuttingDown(const std::string &reason);            // UNGATED, all clients

private:
	struct Conn {
		std::shared_ptr<ix::WebSocket> socket;
		std::set<std::string> events; // subscribed names (per-connection, lost on disconnect)
	};

	// Connection-thread paths.
	void onOpen(const std::string &connId, ix::WebSocket &socket, const ix::WebSocketMessagePtr &msg);
	void onClose(const std::string &connId);
	void onMessageText(const std::string &connId, ix::WebSocket &socket, const std::string &text);
	std::string handleSubscription(const std::string &connId, bool subscribe, const nlohmann::json &id,
				       const nlohmann::json &params);

	ControlVerbs *verbs_ = nullptr; // borrowed; outlives the server
	std::unique_ptr<ix::WebSocketServer> server_;
	QTimer *timer_ = nullptr;       // 1s poll/status tick (main thread)
	QTimer *levelsTimer_ = nullptr; // 100ms audioLevels tick (main thread; emits only while consumed)
	bool levelsActive_ = false;     // last tick had a consumer (drain-discard on the 0 -> 1 edge)
	std::function<void(const nlohmann::json &)> levelsSink_; // in-process consumer (main thread)
	std::atomic<bool> accepting_{false};
	bool running_ = false;
	int port_ = 0;
	std::string controlToken_; // per-launch auth token enforced on the WS upgrade (empty = disabled)

	std::mutex connMutex_; // guards conns_ (accessed from connection threads + main thread)
	std::map<std::string, Conn> conns_;
	std::set<std::string> acceptedEvents_; // snapshot of verbs->subscribableEvents() at start
};

} // namespace moxrelay
