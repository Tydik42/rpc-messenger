#pragma once

#include <string_view>

bool validNickname(std::string_view nick);
void runChat(std::string_view room, std::string_view nick, bool listen, bool color);
