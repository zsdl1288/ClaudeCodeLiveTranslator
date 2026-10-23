#pragma once
#include <cstdint>
#include <string>
namespace cclive {
std::string enumerate_windows_json();
int reader_info_main(std::uint64_t hwnd, bool fixture = false);
int reader_main(std::uint64_t hwnd, bool once, bool fixture = false);
}

