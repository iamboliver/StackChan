/*
 * Custom server configuration — overrides the default secret_logic weak symbols.
 * Replace YOUR_LINUX_LAN_IP with your Linux machine's local network IP (e.g. 192.168.1.50)
 */
#include "secret_logic.h"

namespace secret_logic {

std::string get_server_url()
{
    return "http://192.168.1.240:12800";
}

std::string generate_auth_token()
{
    return "hi-stack-chan";
}

std::string generate_handshake_token(std::string_view data)
{
    return "hi-stack-chan";
}

}  // namespace secret_logic
