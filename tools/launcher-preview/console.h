// SPDX-License-Identifier: GPL-3.0-or-later
// PS5CEMU-HAR: what the launcher's preview on a PC (tools/preview-shell.sh) needs of the console: the
// script of DualSense presses and shots, the
// frame the shots save as PNGs, and in console.cpp the console and the emulators in brief, with
// sample Wii U games, graphic packs and controllers, and a DualSense the script presses.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace preview
{
	constexpr int kWidth = 1920, kHeight = 1080;

	// sticks, pressed like buttons in the script
	constexpr uint32_t kLeftUp = 1u << 24, kLeftDown = 1u << 25, kLeftLeft = 1u << 26, kLeftRight = 1u << 27;
	constexpr uint32_t kRightUp = 1u << 28, kRightDown = 1u << 29, kRightLeft = 1u << 30;
	constexpr uint64_t kRightRight = 1ull << 31; // shares a bit with kIntercepted, which the launchers ignore

	extern std::string output;		   // where the shots go; its boxart/ and covers/ hold the pictures shown
	extern uint64_t timeUs;			   // the console's clock: the preview's frames are 1/60 s apart
	extern uint32_t buttons;		   // what the script holds down for the next frame
	extern std::vector<uint8_t> frame; // BGRA, kWidth x kHeight: what a shot saves

	// The script (shell.cpp's comment says how it reads)
	void LoadScript(const std::string& path);
	// After each frame: the buttons for the next, and the shots; the process ends with the script
	void AdvanceScript();
	void WritePng(const std::string& path, int level = 6);
}
