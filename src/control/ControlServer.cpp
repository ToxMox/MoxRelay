// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ToxMox / MoxRelay contributors
//
// ControlServer implementation. See ControlServer.hpp for the threading/lifecycle contract and
// docs/control-api.asyncapi.yaml for the wire contract this serves.

#include "ControlServer.hpp"

#include "control/ControlVerbs.hpp"

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocketServer.h>

#include <QMetaObject>
#include <QTimer>

#include <chrono>
#include <cstdio>
#include <future>
#include <vector>

namespace moxrelay {

namespace {

using nlohmann::json;

// How long a connection thread waits for the main loop to execute a verb before answering 1009.
// Verbs are main-thread state operations (milliseconds); the deadline only fires when the event
// loop is gone or wedged -- it is the deadlock guard, not a latency budget.
constexpr std::chrono::milliseconds kDispatchDeadline{2500};

std::string protocolError(const json &id, int code, const std::string &message)
{
	return json{{"id", id}, {"error", {{"code", code}, {"message", message}}}}.dump();
}

// Extract the value of ?token= from a raw request-target like "/control?token=abc".
// Returns "" when absent. No URL-decoding needed: the token is [0-9a-f] only.
std::string parseTokenQueryParam(const std::string &uri)
{
	const auto q = uri.find('?');
	if (q == std::string::npos)
		return {};
	std::string qs = uri.substr(q + 1);
	// split on '&', find key "token"
	size_t pos = 0;
	while (pos < qs.size()) {
		size_t amp = qs.find('&', pos);
		std::string pair = qs.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
		const auto eq = pair.find('=');
		if (eq != std::string::npos && pair.substr(0, eq) == "token")
			return pair.substr(eq + 1);
		if (amp == std::string::npos)
			break;
		pos = amp + 1;
	}
	return {};
}

// Returns true iff `origin` is an allowed NATIVE self-origin (not browser-reachable):
//   scheme is "ws" or "wss" (case-insensitive) AND host is loopback
//   ("127.0.0.1", "localhost", "::1", or "[::1]", case-insensitive for localhost).
// Returns false for empty, "null", http/https origins, non-loopback hosts, or anything
// that does not parse as scheme://host. Browsers ALWAYS send http/https/null and can never
// forge a ws(s) loopback origin, so this blocks every browser-reachable origin while
// allowing native clients (incl. ixwebsocket's default ws://127.0.0.1:<port>).
bool isAllowedLoopbackOrigin(const std::string &origin)
{
	const auto sep = origin.find("://");
	if (sep == std::string::npos)
		return false;

	std::string scheme = origin.substr(0, sep);
	for (char &c : scheme)
		c = char(::tolower(static_cast<unsigned char>(c)));
	if (scheme != "ws" && scheme != "wss")
		return false;

	const std::string rest = origin.substr(sep + 3);
	std::string host;
	if (!rest.empty() && rest[0] == '[') {
		// IPv6 literal: host is the content through the matching ']' (brackets stripped so it
		// compares to the bare "::1"). A missing ']' is malformed -> reject.
		const auto end = rest.find(']');
		if (end == std::string::npos)
			return false;
		// Only a port (':') or path ('/') may follow the bracket; anything else
		// (e.g. "[::1].evil", "[::1]@evil.com") is malformed -> reject, so a non-loopback
		// host cannot be smuggled in after a loopback-looking bracketed literal.
		if (end + 1 < rest.size() && rest[end + 1] != ':' && rest[end + 1] != '/')
			return false;
		host = rest.substr(1, end - 1);
	} else {
		// host runs up to the first ':' (port), '/' (path), or end.
		const auto stop = rest.find_first_of(":/");
		host = stop == std::string::npos ? rest : rest.substr(0, stop);
	}
	for (char &c : host)
		c = char(::tolower(static_cast<unsigned char>(c)));

	return host == "127.0.0.1" || host == "localhost" || host == "::1";
}

// Length-aware constant-time compare (no early-out on first mismatch, no length leak via a
// differing loop bound). No OpenSSL dependency at this layer.
bool constantTimeEquals(const std::string &a, const std::string &b)
{
	unsigned diff = static_cast<unsigned>(a.size() ^ b.size());
	const size_t n = a.size() < b.size() ? a.size() : b.size();
	for (size_t i = 0; i < n; ++i)
		diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
	return diff == 0;
}

} // namespace

ControlServer::ControlServer(ControlVerbs *verbs, QObject *parent) : QObject(parent), verbs_(verbs)
{
	// The verb layer's events flow out through this server (gated per-connection); wiring it
	// here guarantees every construction site gets the sink (worker, GUI, selftest).
	verbs_->setEventSink([this](const std::string &name, const nlohmann::json &data) {
		publishEvent(name, data);
	});

	timer_ = new QTimer(this);
	connect(timer_, &QTimer::timeout, this, [this] {
		// Always poll (senderNameResolved detection feeds replies too); the status EVENT
		// itself is emitted only while at least one connection subscribes to it.
		verbs_->pollTick();
		bool anyStatus = false;
		{
			std::lock_guard<std::mutex> lock(connMutex_);
			for (const auto &entry : conns_) {
				if (entry.second.events.count("status")) {
					anyStatus = true;
					break;
				}
			}
		}
		if (anyStatus)
			publishEvent("status", verbs_->statusEventData());
	});

	// audioLevels rides the same opt-in periodic pattern at its own 100 ms cadence. The data
	// build (which drains the engine's meter window) runs ONLY while at least one consumer
	// exists -- a subscribed connection or the in-process sink (the GUI meters). A headless
	// idle instance pays a registry scan and nothing else.
	levelsTimer_ = new QTimer(this);
	connect(levelsTimer_, &QTimer::timeout, this, [this] {
		bool anyLevels = static_cast<bool>(levelsSink_);
		if (!anyLevels) {
			std::lock_guard<std::mutex> lock(connMutex_);
			for (const auto &entry : conns_) {
				if (entry.second.events.count("audioLevels")) {
					anyLevels = true;
					break;
				}
			}
		}
		if (anyLevels && !levelsActive_) {
			// 0 -> 1 consumer transition: the meter window has been accumulating since
			// the last drain (potentially forever on an idle instance). Discard one
			// window so the first PUBLISHED frame covers a single tick -- the contract
			// frames peak/rms/clipped as "since the previous emit".
			verbs_->audioLevelsEventData();
			levelsActive_ = true;
			return;
		}
		levelsActive_ = anyLevels;
		if (!anyLevels)
			return;
		// ONE data build serves both consumers (the drain is single-reader by contract).
		const nlohmann::json data = verbs_->audioLevelsEventData();
		publishEvent("audioLevels", data);
		if (levelsSink_)
			levelsSink_(data);
	});
}

void ControlServer::setLevelsSink(std::function<void(const nlohmann::json &)> sink)
{
	levelsSink_ = std::move(sink);
}

ControlServer::~ControlServer()
{
	stop();
}

void ControlServer::setControlToken(std::string token)
{
	controlToken_ = std::move(token);
}

bool ControlServer::start(int port)
{
	if (running_)
		return true;

	ix::initNetSystem();
	server_ = std::make_unique<ix::WebSocketServer>(port, "127.0.0.1");
	server_->disablePerMessageDeflate(); // text JSON on loopback; no compression on this wire

	server_->setOnClientMessageCallback([this](std::shared_ptr<ix::ConnectionState> state,
						   ix::WebSocket &socket, const ix::WebSocketMessagePtr &msg) {
		const std::string connId = state ? state->getId() : std::string();
		switch (msg->type) {
		case ix::WebSocketMessageType::Open:
			onOpen(connId, socket, msg);
			break;
		case ix::WebSocketMessageType::Close:
			onClose(connId);
			break;
		case ix::WebSocketMessageType::Message:
			if (!msg->binary)
				onMessageText(connId, socket, msg->str);
			break;
		default:
			break; // ping/pong/fragment/error: nothing to do on this wire
		}
	});

	const std::pair<bool, std::string> res = server_->listen();
	if (!res.first) {
		std::fprintf(stderr, "[control] listen failed on 127.0.0.1:%d: %s\n", port, res.second.c_str());
		server_.reset();
		ix::uninitNetSystem();
		return false;
	}
	server_->start();

	const auto events = verbs_->subscribableEvents();
	acceptedEvents_ = std::set<std::string>(events.begin(), events.end());

	port_ = port;
	accepting_ = true;
	running_ = true;
	timer_->start(1000);
	levelsTimer_->start(100);
	std::printf("[control] serving ws://127.0.0.1:%d/control\n", port);
	std::fflush(stdout);
	return true;
}

void ControlServer::stop()
{
	if (!running_)
		return;
	accepting_ = false;
	timer_->stop();
	levelsTimer_->stop();
	server_->stop(); // joins the accept loop + every connection thread
	server_.reset();
	{
		std::lock_guard<std::mutex> lock(connMutex_);
		conns_.clear();
	}
	ix::uninitNetSystem();
	running_ = false;
}

// ---------------------------------------------------------------------------------------------
// Event publication (main thread)
// ---------------------------------------------------------------------------------------------

void ControlServer::publishEvent(const std::string &name, const nlohmann::json &data)
{
	std::vector<std::shared_ptr<ix::WebSocket>> targets;
	{
		std::lock_guard<std::mutex> lock(connMutex_);
		for (const auto &entry : conns_) {
			if (entry.second.events.count(name) && entry.second.socket)
				targets.push_back(entry.second.socket);
		}
	}
	if (targets.empty())
		return;
	const std::string payload = json{{"event", name}, {"data", data}}.dump();
	for (const auto &socket : targets)
		socket->send(payload); // non-blocking enqueue; no-op on a closed socket
}

void ControlServer::publishInstanceShuttingDown(const std::string &reason)
{
	if (!running_)
		return;
	std::vector<std::shared_ptr<ix::WebSocket>> targets;
	{
		std::lock_guard<std::mutex> lock(connMutex_);
		for (const auto &entry : conns_) {
			if (entry.second.socket)
				targets.push_back(entry.second.socket);
		}
	}
	if (targets.empty())
		return;
	const std::string payload =
		json{{"event", "instanceShuttingDown"}, {"data", verbs_->shuttingDownEventData(reason)}}.dump();
	for (const auto &socket : targets)
		socket->send(payload);
}

// ---------------------------------------------------------------------------------------------
// Connection-thread paths
// ---------------------------------------------------------------------------------------------

void ControlServer::onOpen(const std::string &connId, ix::WebSocket &socket,
			   const ix::WebSocketMessagePtr &msg)
{
	if (connId.empty() || !server_)
		return;

	// Enforce the upgrade handshake BEFORE resolving/inserting the connection, so a rejected
	// upgrade never lands in conns_ and no verb ever dispatches for it. The Open callback carries
	// the full request-target URI + headers (msg->openInfo) and fires before the connection runs.

	// GATE 1: reject browser-reachable origins. An upgrade with NO Origin header is allowed
	// (native WebSocket clients typically send none). An upgrade WITH an Origin is
	// rejected unless it is a ws(s)://loopback self-origin -- which a browser can never forge
	// (browsers always send their page's http/https origin or "null"). This still fences out
	// every web page while letting native clients that DO send a default Origin through (e.g.
	// ixwebsocket's ws://127.0.0.1:<port>). openInfo.headers is a case-insensitive map, so
	// find("Origin") also catches origin/ORIGIN.
	if (msg) {
		const auto it = msg->openInfo.headers.find("Origin");
		if (it != msg->openInfo.headers.end() && !isAllowedLoopbackOrigin(it->second)) {
			socket.close(4403, "origin not allowed");
			return;
		}
	}

	// GATE 2: token. Empty controlToken_ => auth disabled (RNG failed): fall through (fail-open
	// ONLY on our own RNG failure, never on a missing client token). Otherwise require an exact,
	// constant-time match on ?token=.
	if (!controlToken_.empty()) {
		const std::string uri = msg ? msg->openInfo.uri : std::string();
		const std::string supplied = parseTokenQueryParam(uri);
		if (!constantTimeEquals(supplied, controlToken_)) {
			socket.close(4401, "unauthorized");
			return;
		}
	}

	// Resolve the shared_ptr owning this connection's socket so event fan-out can hold a
	// reference across a concurrent close (the registry never stores a raw pointer).
	std::shared_ptr<ix::WebSocket> shared;
	for (const auto &client : server_->getClients()) {
		if (client.get() == &socket) {
			shared = client;
			break;
		}
	}
	if (!shared)
		return;
	std::lock_guard<std::mutex> lock(connMutex_);
	conns_[connId] = Conn{std::move(shared), {}};
}

void ControlServer::onClose(const std::string &connId)
{
	std::lock_guard<std::mutex> lock(connMutex_);
	conns_.erase(connId);
}

std::string ControlServer::handleSubscription(const std::string &connId, bool subscribe, const json &id,
					      const json &params)
{
	json events = json::array();
	if (params.is_object() && params.contains("events") && params["events"].is_array())
		events = params["events"];
	if (events.empty())
		return protocolError(id, -32602, "Invalid params: 'events' must be a non-empty array");

	for (const auto &e : events) {
		if (!e.is_string())
			return protocolError(id, -32602, "Invalid params: 'events' entries must be strings");
	}

	std::vector<std::string> acked;
	{
		std::lock_guard<std::mutex> lock(connMutex_);
		auto it = conns_.find(connId);
		if (it != conns_.end()) {
			for (const auto &e : events) {
				const std::string name = e.get<std::string>();
				if (subscribe) {
					// Names this instance does not support are silently
					// omitted from the ack (capability discovery).
					if (!acceptedEvents_.count(name))
						continue;
					if (it->second.events.insert(name).second)
						acked.push_back(name);
				} else {
					if (it->second.events.erase(name) > 0)
						acked.push_back(name);
				}
			}
		}
	}
	return json{{"id", id}, {"result", {{subscribe ? "subscribed" : "unsubscribed", acked}}}}.dump();
}

void ControlServer::onMessageText(const std::string &connId, ix::WebSocket &socket, const std::string &text)
{
	json req = json::parse(text, nullptr, /*allow_exceptions=*/false);
	if (req.is_discarded()) {
		socket.send(protocolError(nullptr, -32700, "Parse error: invalid JSON"));
		return;
	}
	if (!req.is_object()) {
		socket.send(protocolError(nullptr, -32600, "Invalid request: not a JSON object"));
		return;
	}

	const json id = req.contains("id") ? req["id"] : json(nullptr);
	if (!req.contains("method") || !req["method"].is_string()) {
		socket.send(protocolError(id, -32600, "Invalid request: missing string 'method'"));
		return;
	}
	const std::string method = req["method"].get<std::string>();
	const json params = req.contains("params") ? req["params"] : json::object();

	// Subscribe/Unsubscribe mutate only the per-connection registry: answered here on the
	// connection thread (registry mutex), no main-loop round trip.
	if (method == "Subscribe") {
		socket.send(handleSubscription(connId, /*subscribe=*/true, id, params));
		return;
	}
	if (method == "Unsubscribe") {
		socket.send(handleSubscription(connId, /*subscribe=*/false, id, params));
		return;
	}

	// Shutdown bypasses the accepting_ gate: a repeated Shutdown must reach the verb layer for its
	// idempotent re-ack (contract 1.5.0) rather than the 1009 ShuttingDown error -- the re-ack
	// path REPLACES 1009 for this one verb. (Once the event loop has exited, server.stop() has run
	// and no connection remains to deliver to, so this only matters while the loop still drains.)
	if (!accepting_ && method != "Shutdown") {
		socket.send(protocolError(id, 1009, "Instance is shutting down"));
		return;
	}

	// Marshal the verb onto the Qt main thread. The promise is shared so a deadline miss
	// leaves a still-valid target for the (now pointless) queued handler.
	auto promise = std::make_shared<std::promise<std::string>>();
	std::future<std::string> reply = promise->get_future();
	QMetaObject::invokeMethod(
		this,
		[this, promise, id, method, params] {
			std::string out;
			try {
				out = verbs_->dispatch(id, method, params).dump();
			} catch (const std::exception &e) {
				out = protocolError(id, -32602, std::string("Invalid params: ") + e.what());
			}
			promise->set_value(std::move(out));
		},
		Qt::QueuedConnection);

	if (reply.wait_for(kDispatchDeadline) == std::future_status::ready)
		socket.send(reply.get());
	else
		socket.send(protocolError(id, 1009, "Instance is shutting down"));
}

} // namespace moxrelay
