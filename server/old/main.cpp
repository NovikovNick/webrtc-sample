#include "server.hpp"

#include <csignal>
#include <cstdlib>
#include <format>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace
{
volatile std::sig_atomic_t g_signalReceived = 0;

void handleSignal(int signal)
{
	(void)signal;
	g_signalReceived = 1;  // async-signal-safe: only assign sig_atomic_t
}

void printUsage(const char* program)
{
	std::string usage = std::
		format("Usage: {} [options]\n\n"
			   "Options:\n"
			   "  --bind ADDRESS          Bind address (default 0.0.0.0)\n"
			   "  --port PORT             WebSocket port (default 8080)\n"
			   "  --udp-min PORT          First ICE UDP port (default 40000)\n"
			   "  --udp-max PORT          Last ICE UDP port (default 41000)\n"
			   "  --max-clients COUNT     Maximum simultaneous signaling clients (default 200)\n"
			   "  --allow-origin ORIGIN   Allowed browser Origin; repeatable\n"
			   "  --ice-server URL        STUN/TURN URL; repeatable (default public STUN)\n"
			   "  --tls-cert FILE         TLS certificate PEM (enable native WSS)\n"
			   "  --tls-key FILE          TLS private key PEM (enable native WSS)\n"
			   "  --help                  Show this help\n\n"
			   "Example (TLS terminated by nginx):\n"
			   "  {} --bind 127.0.0.1 --port 8080 "
			   "--allow-origin https://yourname.github.io\n\n"
			   "Example (native WSS):\n"
			   "  {} --port 8443 --allow-origin https://yourname.github.io "
			   "--tls-cert fullchain.pem --tls-key privkey.pem\n",
			   program,
			   program,
			   program);
	std::cout << usage;
}

std::string requireValue(int& index, int argc, char** argv, const std::string& option)
{
	if (index + 1 >= argc) throw std::invalid_argument("Missing value for " + option);
	return argv[++index];
}
}  // namespace

int main(int argc, char** argv)
{
	try
	{
		ServerOptions options;
		bool customIceServers = false;

		for (int i = 1; i < argc; ++i)
		{
			const std::string arg = argv[i];
			if (arg == "--help" || arg == "-h")
			{
				printUsage(argv[0]);
				return 0;
			}
			else if (arg == "--bind")
			{
				options.bindAddress = requireValue(i, argc, argv, arg);
			}
			else if (arg == "--port")
			{
				const auto value = std::stoul(requireValue(i, argc, argv, arg));
				if (value == 0 || value > 65535) throw std::invalid_argument("Invalid port");
				options.port = static_cast<std::uint16_t>(value);
			}
			else if (arg == "--udp-min")
			{
				const auto value = std::stoul(requireValue(i, argc, argv, arg));
				if (value == 0 || value > 65535)
					throw std::invalid_argument("Invalid UDP minimum port");
				options.udpPortRangeBegin = static_cast<std::uint16_t>(value);
			}
			else if (arg == "--udp-max")
			{
				const auto value = std::stoul(requireValue(i, argc, argv, arg));
				if (value == 0 || value > 65535)
					throw std::invalid_argument("Invalid UDP maximum port");
				options.udpPortRangeEnd = static_cast<std::uint16_t>(value);
			}
			else if (arg == "--max-clients")
			{
				const auto value = std::stoul(requireValue(i, argc, argv, arg));
				if (value == 0 || value > 100000)
					throw std::invalid_argument("Invalid max clients value");
				options.maxClients = static_cast<std::size_t>(value);
			}
			else if (arg == "--allow-origin")
			{
				options.allowedOrigins.push_back(requireValue(i, argc, argv, arg));
			}
			else if (arg == "--ice-server" || arg == "--stun")
			{
				if (!customIceServers)
				{
					options.iceServers.clear();
					customIceServers = true;
				}
				options.iceServers.push_back(requireValue(i, argc, argv, arg));
			}
			else if (arg == "--tls-cert")
			{
				options.tlsCertificate = requireValue(i, argc, argv, arg);
			}
			else if (arg == "--tls-key")
			{
				options.tlsPrivateKey = requireValue(i, argc, argv, arg);
			}
			else
			{
				throw std::invalid_argument("Unknown option: " + arg);
			}
		}

		if (options.udpPortRangeBegin > options.udpPortRangeEnd)
		{
			throw std::invalid_argument("--udp-min must be <= --udp-max");
		}
		WebRtcServer server(std::move(options));
		std::signal(SIGINT, handleSignal);
		std::signal(SIGTERM, handleSignal);
		server.run(&g_signalReceived);
		return 0;
	}
	catch (const std::exception& e)
	{
		std::cerr << "Fatal: " << e.what() << '\n';
		return 1;
	}
}
