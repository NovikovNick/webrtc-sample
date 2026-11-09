#pragma once

#include <rtc/rtc.hpp>
#include <rtc/websocketserver.hpp>

#include <atomic>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

struct ServerOptions
{
	std::string bindAddress			= "0.0.0.0";
	std::uint16_t port				= 8080;
	std::uint16_t udpPortRangeBegin = 40000;
	std::uint16_t udpPortRangeEnd	= 41000;
	std::size_t maxClients			= 200;
	std::string tlsCertificate;
	std::string tlsPrivateKey;
	std::vector<std::string> allowedOrigins;
	std::vector<std::string> iceServers = {"stun:stun.l.google.com:19302"};
};

class WebRtcServer
{
  public:
	explicit WebRtcServer(ServerOptions options);
	~WebRtcServer();

	WebRtcServer(const WebRtcServer&)			 = delete;
	WebRtcServer& operator=(const WebRtcServer&) = delete;

	void run(const volatile std::sig_atomic_t* signalFlag = nullptr);
	void stop();

  private:
	class ClientSession;
	friend class ClientSession;

	class SignalingRouter;

	void removeSession(const std::string& id);
	bool originAllowed(const std::shared_ptr<rtc::WebSocket>& ws) const;

	ServerOptions options_;
	rtc::Configuration rtcConfig_;
	std::unique_ptr<rtc::WebSocketServer> wsServer_;
	mutable std::mutex sessionsMutex_;
	std::unordered_map<std::string, std::shared_ptr<ClientSession>> sessions_;
	std::uint64_t nextSessionId_ = 1;
	std::atomic<bool> stopping_{false};

	std::unique_ptr<SignalingRouter> router_;
};
