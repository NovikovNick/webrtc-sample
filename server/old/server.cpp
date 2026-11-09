#include "server.hpp"

#include <nlohmann/json.hpp>
#include <rtc/rtc.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <utility>

using json = nlohmann::json;

namespace
{
std::string trim(std::string value)
{
	const auto first = value.find_first_not_of(" \t\r\n");
	if (first == std::string::npos) return {};
	const auto last = value.find_last_not_of(" \t\r\n");
	return value.substr(first, last - first + 1);
}

}  // namespace

class WebRtcServer::SignalingRouter final
{
  public:
	using Send = std::function<void(const std::string&)>;

	struct Client
	{
		std::string room;
		Send send;
	};

	void addClient(const std::string& id, Send send)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		clients_[id] = Client{"", std::move(send)};
	}

	void onMessage(const std::string& id, const std::string& payload)
	{
		json msg;

		try
		{
			msg = json::parse(payload);
		}
		catch (...)
		{
			sendTo(id, {{"type", "error"}, {"message", "Invalid JSON"}});
			return;
		}

		const std::string type = msg.value("type", "");

		if (type == "join")
		{
			join(id, msg.value("room", "room1"));
			return;
		}

		// Только signaling-сообщения. DataChannel-содержимое
		// через WebSocket не пересылаем.
		if (type != "offer" && type != "answer" && type != "candidate")
		{
			sendTo(id, {{"type", "error"}, {"message", "Unsupported message type"}});
			return;
		}

		const std::string to = msg.value("to", "");
		if (to.empty() || to == id)
		{
			sendTo(id, {{"type", "error"}, {"message", "Invalid recipient"}});
			return;
		}

		Send targetSend;

		{
			std::lock_guard<std::mutex> lock(mutex_);

			auto fromIt = clients_.find(id);
			auto toIt	= clients_.find(to);

			if (fromIt == clients_.end() || toIt == clients_.end() || fromIt->second.room.empty() ||
				fromIt->second.room != toIt->second.room)
			{
				// Отправляем ошибку после выхода из mutex.
			}
			else
			{
				targetSend = toIt->second.send;
			}
		}

		if (!targetSend)
		{
			sendTo(id, {{"type", "error"}, {"message", "Peer not found in your room"}});
			return;
		}

		msg["from"] = id;
		targetSend(msg.dump());
	}

	void disconnect(const std::string& id)
	{
		std::vector<std::string> peers;
		std::string room;

		{
			std::lock_guard<std::mutex> lock(mutex_);

			auto it = clients_.find(id);
			if (it == clients_.end()) return;

			room = it->second.room;
			clients_.erase(it);

			for (const auto& [otherId, client] : clients_)
			{
				if (!room.empty() && client.room == room) peers.push_back(otherId);
			}
		}

		for (const auto& peer : peers)
		{
			sendTo(peer, {{"type", "peer-left"}, {"peerId", id}});
		}
	}

  private:
	std::mutex mutex_;
	std::unordered_map<std::string, Client> clients_;

	void sendTo(const std::string& id, const json& message)
	{
		Send send;

		{
			std::lock_guard<std::mutex> lock(mutex_);
			auto it = clients_.find(id);
			if (it != clients_.end()) send = it->second.send;
		}

		if (send) send(message.dump());
	}

	void join(const std::string& id, const std::string& room)
	{
		if (room.empty())
		{
			sendTo(id, {{"type", "error"}, {"message", "Room cannot be empty"}});
			return;
		}

		Send newcomerSend;
		std::string existingId;
		Send existingSend;

		{
			std::lock_guard<std::mutex> lock(mutex_);

			auto it = clients_.find(id);
			if (it == clients_.end()) return;

			if (!it->second.room.empty())
			{
				// Повторный join в этой минимальной версии запрещён.
				newcomerSend = it->second.send;
			}
			else
			{
				for (const auto& [otherId, client] : clients_)
				{
					if (client.room == room)
					{
						existingId	 = otherId;
						existingSend = client.send;
						break;
					}
				}

				if (!existingId.empty())
				{
					auto existing = clients_.find(existingId);
					if (existing == clients_.end())
					{
						existingId.clear();
						existingSend = nullptr;
					}
				}

				it->second.room = room;
				newcomerSend	= it->second.send;
			}
		}

		if (!newcomerSend) return;

		if (existingId.empty())
		{
			newcomerSend(
				json{{"type", "joined"}, {"id", id}, {"role", "initiator"}, {"peerId", nullptr}}
					.dump());
		}
		else
		{
			newcomerSend(
				json{{"type", "joined"}, {"id", id}, {"role", "responder"}, {"peerId", existingId}}
					.dump());

			existingSend(json{{"type", "peer-joined"}, {"peerId", id}}.dump());
		}
	}
};

class WebRtcServer::ClientSession final
	: public std::enable_shared_from_this<WebRtcServer::ClientSession>
{
  public:
	ClientSession(WebRtcServer& server,
				  std::string id,
				  std::shared_ptr<rtc::WebSocket> ws,
				  rtc::Configuration rtcConfig)
		: server_(server), id_(std::move(id)), ws_(std::move(ws)), rtcConfig_(std::move(rtcConfig))
	{
	}

	void start()
	{
		const std::weak_ptr<ClientSession> weak = shared_from_this();

		// WebSocketServer::onClient is called when TCP is accepted, before
		// the HTTP Upgrade handshake. Wait for onOpen before reading Origin
		// headers or sending the initial signaling message.
		ws_->onOpen(
			[weak]()
			{
				if (const auto self = weak.lock())
				{
					self->handleWebSocketOpen();
				}
			});

		ws_->onMessage(
			[weak](rtc::message_variant message)
			{
				if (const auto self = weak.lock())
				{
					self->handleSignalingMessage(std::move(message));
				}
			});

		ws_->onClosed(
			[weak]()
			{
				if (const auto self = weak.lock())
				{
					self->close("signaling websocket closed", false);
				}
			});

		ws_->onError(
			[weak](std::string error)
			{
				if (const auto self = weak.lock())
				{
					std::cerr << "[" << self->id_ << "] WebSocket error: " << error << '\n';
				}
			});

		std::cout << "[" << id_ << "] TCP accepted; waiting for WebSocket handshake" << std::endl;
	}

	void handleWebSocketOpen()
	{
		// if (!server_.originAllowed(ws_))
		// {
		//     const auto headers = ws_->requestHeaders();
		//     const auto it = headers.find("Origin");
		//     const std::string origin = it == headers.end() ? "<missing>" : it->second;
		//     std::cerr << "[" << id_ << "] rejected WebSocket Origin: " << origin << std::endl;
		//     close("Origin not allowed", true);
		//     return;
		// }

		const std::weak_ptr<ClientSession> weak = shared_from_this();
		server_.router_->addClient(id_,
								   [weak](const std::string& text)
								   {
									   if (const auto self = weak.lock())
									   {
										   self->sendJson(text);
									   }
								   });

		sendJson({{"type", "ready"}, {"sessionId", id_}});
		std::cout << "[" << id_ << "] signaling WebSocket connected" << std::endl;
	}

	void close(const std::string& reason, bool closeWebSocket)
	{
		bool shouldRemove = false;
		{
			std::lock_guard<std::mutex> lock(stateMutex_);
			if (!closed_)
			{
				closed_		 = true;
				shouldRemove = true;
			}
		}
		if (!shouldRemove) return;

		std::cout << "[" << id_ << "] closing: " << reason << std::endl;

		std::shared_ptr<rtc::PeerConnection> peer;
		{
			std::lock_guard<std::mutex> lock(stateMutex_);
			peer = peer_;
			dataChannel_.reset();
		}
		if (peer)
		{
			try
			{
				peer->close();
			}
			catch (const std::exception& e)
			{
				std::cerr << "[" << id_ << "] peer close error: " << e.what() << '\n';
			}
		}
		if (closeWebSocket && ws_ && !ws_->isClosed())
		{
			try
			{
				ws_->close();
			}
			catch (...)
			{
			}
		}
		server_.removeSession(id_);
	}

  private:
	void sendJson(const json& message)
	{
		if (!ws_ || ws_->isClosed()) return;
		const std::string payload = message.dump();
		std::lock_guard<std::mutex> lock(sendMutex_);
		if (ws_ && !ws_->isClosed())
		{
			ws_->send(payload);
		}
	}

	void handleSignalingMessage(rtc::message_variant message)
	{
		if (!std::holds_alternative<rtc::string>(message))
		{
			sendJson({{"type", "error"}, {"message", "Signaling messages must be JSON text."}});
			return;
		}

		try
		{
			const auto payload	   = json::parse(std::get<rtc::string>(message));
			const std::string type = payload.value("type", "");

			if (type == "offer")
			{
				handleOffer(payload);
			}
			else if (type == "candidate")
			{
				handleCandidate(payload);
			}
			else if (type == "ping")
			{
				sendJson({{"type", "pong"}});
			}
			else
			{
				sendJson({{"type", "error"},
						  {"message", "Unsupported signaling message type: " + type}});
			}
		}
		catch (const json::exception& e)
		{
			sendJson({{"type", "error"}, {"message", std::string("Invalid JSON: ") + e.what()}});
		}
		catch (const std::exception& e)
		{
			sendJson({{"type", "error"}, {"message", e.what()}});
			std::cerr << "[" << id_ << "] signaling error: " << e.what() << '\n';
		}
	}

	void handleOffer(const json& payload)
	{
		const std::string sdp = payload.at("sdp").get<std::string>();
		if (sdp.empty()) throw std::runtime_error("SDP offer is empty");

		std::shared_ptr<rtc::PeerConnection> peer;
		{
			std::lock_guard<std::mutex> lock(stateMutex_);
			if (closed_) return;
			if (peer_)
			{
				sendJson({{"type", "error"},
						  {"message", "This signaling session already has a PeerConnection."}});
				return;
			}
			peer_ = std::make_shared<rtc::PeerConnection>(rtcConfig_);
			peer  = peer_;
		}

		configurePeer(peer);
		std::cout << "[" << id_ << "] received SDP offer" << std::endl;

		// A browser-created DataChannel is negotiated inside the incoming SDP offer.
		// libdatachannel automatically creates the SDP answer for a remote offer.
		peer->setRemoteDescription(rtc::Description(sdp, "offer"));

		std::vector<rtc::Candidate> queued;
		{
			std::lock_guard<std::mutex> lock(stateMutex_);
			remoteDescriptionSet_ = true;
			queued.swap(pendingCandidates_);
		}
		for (auto& candidate : queued)
		{
			peer->addRemoteCandidate(std::move(candidate));
		}
	}

	void handleCandidate(const json& payload)
	{
		const std::string candidateText = payload.value("candidate", "");
		if (candidateText.empty()) return;	// Browser's end-of-candidates notification.

		std::string mid = payload.value("sdpMid", "data");
		rtc::Candidate candidate(candidateText, mid);

		std::shared_ptr<rtc::PeerConnection> peer;
		bool queue = false;
		{
			std::lock_guard<std::mutex> lock(stateMutex_);
			if (closed_) return;
			peer = peer_;
			if (!peer || !remoteDescriptionSet_)
			{
				pendingCandidates_.push_back(std::move(candidate));
				queue = true;
			}
		}
		if (!queue && peer)
		{
			peer->addRemoteCandidate(std::move(candidate));
		}
	}

	void configurePeer(const std::shared_ptr<rtc::PeerConnection>& peer)
	{
		const std::weak_ptr<ClientSession> weak = shared_from_this();

		peer->onLocalDescription(
			[weak](rtc::Description description)
			{
				if (const auto self = weak.lock())
				{
					self->sendJson({{"type", "answer"}, {"sdp", std::string(description)}});
					std::cout << "[" << self->id_ << "] sent SDP answer" << std::endl;
				}
			});

		peer->onLocalCandidate(
			[weak](rtc::Candidate candidate)
			{
				if (const auto self = weak.lock())
				{
					self->sendJson({{"type", "candidate"},
									{"candidate", candidate.candidate()},
									{"sdpMid", candidate.mid()}});
				}
			});

		peer->onStateChange(
			[weak](rtc::PeerConnection::State state)
			{
				if (const auto self = weak.lock())
				{
					std::cout << "[" << self->id_ << "] PeerConnection state: " << state
							  << std::endl;
					if (state == rtc::PeerConnection::State::Failed ||
						state == rtc::PeerConnection::State::Closed)
					{
						self->sendJson(
							{{"type", "peer-state"},
							 {"state",
							  state == rtc::PeerConnection::State::Failed ? "failed" : "closed"}});
					}
				}
			});

		peer->onDataChannel(
			[weak](std::shared_ptr<rtc::DataChannel> channel)
			{
				if (const auto self = weak.lock())
				{
					self->attachDataChannel(std::move(channel));
				}
			});
	}

	void attachDataChannel(std::shared_ptr<rtc::DataChannel> channel)
	{
		{
			std::lock_guard<std::mutex> lock(stateMutex_);
			if (closed_)
			{
				channel->close();
				return;
			}
			dataChannel_ = channel;
		}

		const std::weak_ptr<ClientSession> weak			  = shared_from_this();
		const std::weak_ptr<rtc::DataChannel> weakChannel = channel;

		channel->onOpen(
			[weak]()
			{
				if (const auto self = weak.lock())
				{
					std::cout << "[" << self->id_ << "] DataChannel opened" << std::endl;
					self->sendJson({{"type", "datachannel"}, {"state", "open"}});
				}
			});

		channel->onClosed(
			[weak]()
			{
				if (const auto self = weak.lock())
				{
					std::cout << "[" << self->id_ << "] DataChannel closed" << std::endl;
					self->sendJson({{"type", "datachannel"}, {"state", "closed"}});
				}
			});

		channel->onMessage(
			[weak, weakChannel](rtc::message_variant message)
			{
				const auto self = weak.lock();
				const auto dc	= weakChannel.lock();
				if (!self || !dc) return;

				if (std::holds_alternative<rtc::string>(message))
				{
					const auto& text = std::get<rtc::string>(message);
					std::cout << "[" << self->id_ << "] DataChannel text (" << text.size()
							  << " bytes): " << text << std::endl;
					// Demo behavior: echo text back to the same browser client.
					dc->send(rtc::string("echo: ") + text);
				}
				else
				{
					const auto& bytes = std::get<rtc::binary>(message);
					std::cout << "[" << self->id_ << "] DataChannel binary (" << bytes.size()
							  << " bytes)" << std::endl;
					// Demo behavior: echo the received binary frame back unchanged.
					dc->send(message);
				}
			});
	}

	WebRtcServer& server_;
	const std::string id_;
	std::shared_ptr<rtc::WebSocket> ws_;
	rtc::Configuration rtcConfig_;

	std::mutex stateMutex_;
	std::mutex sendMutex_;
	bool closed_			   = false;
	bool remoteDescriptionSet_ = false;
	std::shared_ptr<rtc::PeerConnection> peer_;
	std::shared_ptr<rtc::DataChannel> dataChannel_;
	std::vector<rtc::Candidate> pendingCandidates_;
};

WebRtcServer::WebRtcServer(ServerOptions options) : options_(std::move(options))
{
	rtc::InitLogger(rtc::LogLevel::Debug);
	router_ = std::make_unique<SignalingRouter>();

	for (const auto& url : options_.iceServers)
	{
		if (!trim(url).empty()) rtcConfig_.iceServers.emplace_back(trim(url));
	}
	rtcConfig_.maxMessageSize = 16 * 1024 * 1024;
	rtcConfig_.portRangeBegin = options_.udpPortRangeBegin;
	rtcConfig_.portRangeEnd	  = options_.udpPortRangeEnd;

	rtc::WebSocketServerConfiguration wsConfig;
	wsConfig.port			= options_.port;
	wsConfig.bindAddress	= options_.bindAddress;
	wsConfig.maxMessageSize = 1024 * 1024;

	const bool hasCert		= !options_.tlsCertificate.empty();
	const bool hasKey		= !options_.tlsPrivateKey.empty();
	if (hasCert != hasKey)
	{
		throw std::invalid_argument("TLS requires both --tls-cert and --tls-key");
	}
	if (hasCert)
	{
		wsConfig.enableTls			= true;
		wsConfig.certificatePemFile = options_.tlsCertificate;
		wsConfig.keyPemFile			= options_.tlsPrivateKey;
	}
	wsServer_ = std::make_unique<rtc::WebSocketServer>(wsConfig);
}

WebRtcServer::~WebRtcServer()
{
	stop();
}

bool WebRtcServer::originAllowed(const std::shared_ptr<rtc::WebSocket>& /*ws*/) const
{
	// if (options_.allowedOrigins.empty())
	//     return true;

	// const auto headers = ws->requestHeaders();
	// const auto it = headers.find("Origin");
	// if (it == headers.end())
	//     return false;

	// return std::find(options_.allowedOrigins.begin(),
	//                  options_.allowedOrigins.end(), it->second) != options_.allowedOrigins.end();

	return true;
}

void WebRtcServer::run(const volatile std::sig_atomic_t* signalFlag)
{
	wsServer_->onClient(
		[this](std::shared_ptr<rtc::WebSocket> ws)
		{
			if (stopping_.load())
			{
				ws->close();
				return;
			}

			std::string id;
			std::shared_ptr<ClientSession> session;
			{
				std::lock_guard<std::mutex> lock(sessionsMutex_);
				if (sessions_.size() >= options_.maxClients)
				{
					std::cerr << std::format("Rejected WebSocket: client limit reached ({})",
											 options_.maxClients)
							  << std::endl;
					ws->close();
					return;
				}
				id		= "client-" + std::to_string(nextSessionId_++);
				session = std::make_shared<ClientSession>(*this, id, std::move(ws), rtcConfig_);
				sessions_.emplace(id, session);
			}
			session->start();
		});

	std::cout << "WebRTC signaling server listening on "
			  << (options_.tlsCertificate.empty() ? "ws://" : "wss://") << options_.bindAddress
			  << ':' << wsServer_->port() << std::endl;
	std::cout << "Each signaling client receives its own PeerConnection." << std::endl;
	if (options_.allowedOrigins.empty())
	{
		std::cerr << "WARNING: no --allow-origin supplied; any Origin is accepted." << std::endl;
	}
	if (options_.iceServers.empty())
	{
		std::cerr << "WARNING: no STUN/TURN server configured; connectivity may be limited."
				  << std::endl;
	}

	// libdatachannel's WebSocketServer runs its own I/O threads. Keep this process alive.
	while (!stopping_.load() && (!signalFlag || *signalFlag == 0))
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(250));
	}
	stop();
}

void WebRtcServer::stop()
{
	if (stopping_.exchange(true)) return;

	if (wsServer_) wsServer_->stop();

	std::vector<std::shared_ptr<ClientSession>> sessions;
	{
		std::lock_guard<std::mutex> lock(sessionsMutex_);
		for (auto& item : sessions_)
			sessions.push_back(item.second);
		sessions_.clear();
	}
	for (auto& session : sessions)
	{
		session->close("server shutdown", true);
	}
}

void WebRtcServer::removeSession(const std::string& id)
{
	std::lock_guard<std::mutex> lock(sessionsMutex_);
	sessions_.erase(id);
}
