#include <rtc/rtc.hpp>

#include <nlohmann/json.hpp>

#include <iostream>
#include <memory>
#include <string>
#include <variant>

namespace
{
using Json		   = nlohmann::json;
using PeerId	   = std::uint64_t;
using WebSocketPtr = std::shared_ptr<rtc::WebSocket>;

enum class SingnalingPhase : std::uint8_t
{
	None,
	Join,
	Offer,
	Answer,
	Candidate,
	Count
};

struct SignalingMessage
{
	SingnalingPhase type;
	std::string sdp;
	std::string candidate;
	std::string sdpMid;
	int sdpMLineIndex;
	PeerId to;
};

struct SignalingSession
{
	WebSocketPtr session;
	PeerId peerId;
	SingnalingPhase type;
};

struct ServerState
{
	PeerId sequencePeerId_ = 0;
	std::unordered_map<PeerId, SignalingSession> sessions_;
	std::unordered_map<WebSocketPtr, PeerId> peers_;
	PeerId awaitingAnotherPeerId_ = 0;
};

SignalingMessage convert(const Json& message)
{
	SignalingMessage res;
	res.sdp			  = message.value("sdp", "");
	res.candidate	  = message.value("candidate", "");
	res.sdpMid		  = message.value("sdpMid", "");
	res.sdpMLineIndex = message.value("sdpMLineIndex", 0);
	res.to			  = message.value("to", 0);

	std::string type  = message.value("type", "");
	if (type == "join")
	{
		res.type = SingnalingPhase::Join;
	}
	else if (type == "offer")
	{
		res.type = SingnalingPhase::Offer;
	}
	else if (type == "answer")
	{
		res.type = SingnalingPhase::Answer;
	}
	else if (type == "candidate")
	{
		res.type = SingnalingPhase::Candidate;
	}
	else
	{
		res.type = SingnalingPhase::None;
	}
	return res;
}

void send(const std::shared_ptr<rtc::WebSocket>& ws, const Json& message)
{
	if (ws && !ws->isClosed())
	{
		const std::string payload = message.dump();
		ws->send(payload);
	}
}
}  // namespace

int main()
{
	rtc::WebSocketServer::Configuration config;
	config.port		   = 8080;
	config.bindAddress = "0.0.0.0";

	rtc::WebSocketServer server(config);
	ServerState state{};

	server.onClient(
		[&state](std::shared_ptr<rtc::WebSocket> client)
		{
			std::cout << "[SERVER] New client: " << client->remoteAddress().value_or("unknown")
					  << std::endl;

			client->onOpen([]() { std::cout << "[CLIENT] Connected" << std::endl; });

			client->onMessage(
				[client, &state](rtc::message_variant message)
				{
					if (!std::holds_alternative<std::string>(message))
					{
						send(client,
							 {{"type", "error"},
							  {"message", "Signaling messages must be JSON text."}});
						return;
					}

					const std::string str = std::get<rtc::string>(message);
					std::cout << "[CLIENT] :" << str << std::endl;

					const SignalingMessage payload = convert(Json::parse(str));
					switch (payload.type)
					{
						case SingnalingPhase::Join:
						{
							PeerId id			 = ++state.sequencePeerId_;
							state.sessions_[id]	 = SignalingSession{.session = client,
																	.peerId	 = id,
																	.type = SingnalingPhase::Join};
							state.peers_[client] = id;

							if (state.awaitingAnotherPeerId_ == 0)
							{
								state.awaitingAnotherPeerId_ = id;

								send(client,
									 {
										 {"type", "joined"},
										 {"id", id},
										 {"role", "initiator"},
									 });
							}
							else
							{
								send(client,
									 {
										 {"type", "joined"},
										 {"id", id},
										 {"peerId", state.awaitingAnotherPeerId_},
										 {"role", "responder"},
									 });

								send(state.sessions_[state.awaitingAnotherPeerId_].session,
									 {
										 {"type", "peer-joined"},
										 {"peerId", id},
									 });
							}

							break;
						}
						case SingnalingPhase::Offer:
						{
							send(state.sessions_[payload.to].session,
								 {
									 {"type", "offer"},
									 {"sdp", payload.sdp},
								 });
							break;
						}
						case SingnalingPhase::Answer:
						{
							send(state.sessions_[payload.to].session,
								 {
									 {"type", "answer"},
									 {"sdp", payload.sdp},
								 });
							break;
						}

						case SingnalingPhase::Candidate:
						{
							send(state.sessions_[payload.to].session,
								 {
									 {"type", "candidate"},
									 {"candidate", payload.candidate},
									 {"sdpMid", payload.sdpMid},
									 {"sdpMLineIndex", payload.sdpMLineIndex},
								 });
							break;
						}
						default:
						{
							send(client,
								 {
									 {"type", "error"},
									 {"message", "Wrong protocol message"},
								 });
						}
					}
				});

			client->onClosed(
				[client, &state]()
				{
					const PeerId pid			 = state.peers_[client];

					state.awaitingAnotherPeerId_ = 0;
					state.sessions_.erase(pid);
					state.peers_.erase(client);

					std::cout << std::format("[CLIENT] Disconnected {}", pid) << std::endl;
				});
			client->onError([client](std::string error)
							{ std::cerr << "[CLIENT] Error: " << error << std::endl; });
		});

	std::cout << "WebSocket server started: ws://0.0.0.0:" << server.port() << std::endl;

	std::cin.get();
	server.stop();
	return 0;
}
