// SPDX-License-Identifier: GPL-3.0-or-later
// PS5CEMU-HAR: the launcher's sound (sound.h). As Cemu's PS5AudioAPI and Azahar's sink play theirs:
// a thread feeds AudioOut 256-frame stereo grains at 48 kHz, and sceAudioOutOutput, blocking until
// the previous grain plays, paces it. The thread mixes the music and the menu's sounds into each
// grain, and reads the files itself, so the launcher never waits on them.
//
// The files are WAVs (tools/render-sounds.py): the music IMA ADPCM in stereo, the menu's sounds
// 16-bit PCM in mono, all at 48 kHz. Each is decoded whole, into 16-bit stereo frames, as it is
// first needed.

#include "sound.h"
#include "ui_host.h"
#include "../ps5/log.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <thread>
#include <vector>

extern "C"
{
	int sceAudioOutInit(void);
	int sceAudioOutOpen(int userId, int type, int index, uint32_t length, uint32_t frequency, uint32_t format);
	int sceAudioOutClose(int handle);
	int sceAudioOutOutput(int handle, const void* samples);
	int sceAudioOutSetVolume(int handle, int flags, const int* volumes);
	void* scePthreadSelf();
	int scePthreadSetprio(void* thread, int priority);
}

namespace ps5sound
{
	namespace
	{
		constexpr int kUserSystem = 0xff; // the port belongs to the system, not to one user
		constexpr int kPortMain = 0;
		constexpr uint32_t kGrain = 256; // frames per sceAudioOutOutput
		constexpr uint32_t kRate = 48000;
		constexpr uint32_t kFormatStereoS16 = 1;
		constexpr int kVolumeFlagsLeftRight = 3;
		constexpr int kVolume0dB = 32768;
		// above the launcher's other threads (the game scans, the box art), as Cemu's and Azahar's
		// sound threads are above their emulation: it wakes every 5 ms and works briefly
		constexpr int kSoundPriority = 384;
		// how fast the music's level moves, of full level a second: fading in, and out as the
		// launcher closes
		constexpr float kFade = 0.5f / kRate, kCloseFade = 2.0f / kRate;
		constexpr size_t kVoices = 6; // menu sounds at once
		constexpr const char* kEffects[] = {"move", "select", "back", "denied", "launch"};

		// a sound, decoded: 16-bit stereo frames at 48 kHz
		using Clip = std::vector<int16_t>;

		uint32_t Read16(const uint8_t* at) { return at[0] | at[1] << 8; }
		uint32_t Read32(const uint8_t* at) { return Read16(at) | Read16(at + 2) << 16; }

		// IMA ADPCM's 4-bit samples, as Microsoft's WAVs have them: blocks of a header for each
		// channel (its first sample and step index), then 4-byte groups of eight samples, a channel's
		// group after the other's
		constexpr int kSteps[89] = {7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60,
			66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598,
			658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026,
			4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350,
			22385, 24623, 27086, 29794, 32767};
		constexpr int kIndexSteps[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

		int16_t DecodeIma(int code, int& predictor, int& index)
		{
			const int step = kSteps[index];
			int delta = step >> 3;
			if (code & 4)
				delta += step;
			if (code & 2)
				delta += step >> 1;
			if (code & 1)
				delta += step >> 2;
			predictor = std::clamp(code & 8 ? predictor - delta : predictor + delta, -32768, 32767);
			index = std::clamp(index + kIndexSteps[code & 7], 0, 88);
			return (int16_t)predictor;
		}

		void DecodeImaBlocks(const uint8_t* data, size_t size, uint32_t channels, size_t blockAlign, Clip& clip)
		{
			const size_t perBlock = (blockAlign - 4 * channels) * 8 / (4 * channels) + 1;
			// a frame's sample for a channel; a mono sound's on both sides
			auto put = [&](size_t frame, uint32_t channel, int16_t sample) {
				clip[frame * 2 + channel] = sample;
				if (channels == 1)
					clip[frame * 2 + 1] = sample;
			};
			for (size_t at = 0; at + blockAlign <= size; at += blockAlign)
			{
				const uint8_t* block = data + at;
				const size_t first = clip.size() / 2;
				clip.resize((first + perBlock) * 2);
				int predictor[2]{}, index[2]{};
				for (uint32_t channel = 0; channel < channels; channel++)
				{
					predictor[channel] = (int16_t)Read16(block + 4 * channel);
					index[channel] = std::min<int>(block[4 * channel + 2], 88);
					put(first, channel, (int16_t)predictor[channel]);
				}
				const uint8_t* group = block + 4 * channels;
				for (size_t frame = first + 1; frame < first + perBlock; frame += 8)
					for (uint32_t channel = 0; channel < channels; channel++, group += 4)
						for (int i = 0; i < 8; i++)
							put(frame + i, channel, DecodeIma((group[i / 2] >> (i % 2 * 4)) & 15, predictor[channel], index[channel]));
			}
		}

		// A WAV of the launcher's: false when it is missing or not one of the forms above
		bool ReadWav(const std::string& path, Clip& clip)
		{
			std::ifstream file(path, std::ios::binary);
			const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
			if (bytes.size() < 12 || std::memcmp(bytes.data(), "RIFF", 4) != 0 || std::memcmp(bytes.data() + 8, "WAVE", 4) != 0)
				return false;
			uint32_t format = 0, channels = 0, rate = 0, blockAlign = 0, bits = 0, frames = 0;
			const uint8_t* data = nullptr;
			size_t dataSize = 0;
			for (size_t at = 12; at + 8 <= bytes.size();)
			{
				const uint8_t* chunk = bytes.data() + at;
				const size_t size = Read32(chunk + 4), available = std::min(size, bytes.size() - at - 8);
				if (std::memcmp(chunk, "fmt ", 4) == 0 && available >= 16)
				{
					format = Read16(chunk + 8);
					channels = Read16(chunk + 10);
					rate = Read32(chunk + 12);
					blockAlign = Read16(chunk + 20);
					bits = Read16(chunk + 22);
				}
				else if (std::memcmp(chunk, "fact", 4) == 0 && available >= 4)
					frames = Read32(chunk + 8);
				else if (std::memcmp(chunk, "data", 4) == 0)
				{
					data = chunk + 8;
					dataSize = available;
				}
				at += 8 + size + (size & 1);
			}
			if (!data || rate != kRate || channels < 1 || channels > 2)
				return false;
			clip.clear();
			if (format == 1 && bits == 16)
			{
				const size_t count = dataSize / (2 * channels);
				clip.resize(count * 2);
				for (size_t frame = 0; frame < count; frame++)
					for (uint32_t side = 0; side < 2; side++)
						clip[frame * 2 + side] = (int16_t)Read16(data + (frame * channels + std::min(side, channels - 1)) * 2);
			}
			else if (format == 0x11 && bits == 4 && blockAlign > 4 * channels && blockAlign % (4 * channels) == 0)
			{
				DecodeImaBlocks(data, dataSize, channels, blockAlign, clip);
				// the last block is padded: the fact chunk has the sound's length
				if (frames && frames < clip.size() / 2)
					clip.resize(frames * 2);
			}
			return !clip.empty();
		}

		std::string SoundPath(const std::string& name)
		{
			return ps5ui::AssetPath("sounds/" + name + ".wav");
		}

		// what the launcher asks of the sound thread
		struct Wanted
		{
			std::mutex mutex;
			std::string music;
			float level = 0;
			bool menuSounds = true;
			bool closing = false;
			std::vector<Effect> effects; // to start
		};

		Wanted s_wanted;
		std::thread s_thread;

		// The music's level for its volume setting: background music under the menu's sounds, so its
		// full level is -9 dB, and the setting follows the ear (squared): 50% is about -21 dB
		constexpr float kMusicHeadroom = 0.35f;

		float LevelOf(const std::string& music, int volume)
		{
			const float setting = (float)std::clamp(volume, 0, 100) / 100.0f;
			return music == "setup" ? setting * setting * kMusicHeadroom : 0.0f;
		}

		void RaisePriority()
		{
			const int result = scePthreadSetprio(scePthreadSelf(), kSoundPriority);
			if (result != 0)
				ps5log::Line("[sound] the thread's priority stays ({:#x})", (uint32_t)result);
		}

		void Mix(int port)
		{
			RaisePriority();
			std::array<Clip, std::size(kEffects)> effects;
			for (size_t i = 0; i < effects.size(); i++)
				if (!ReadWav(SoundPath(kEffects[i]), effects[i]))
					ps5log::Line("[sound] {} is missing or unreadable", SoundPath(kEffects[i]));
			struct Voice
			{
				const Clip* clip;
				size_t at;
			};
			std::vector<Voice> voices;
			Clip music;
			std::string playing; // the piece in music
			size_t position = 0; // the next frame of it
			float level = 0;	 // its level now, gliding to the wanted one
			std::array<float, kGrain * 2> mix;
			std::array<int16_t, kGrain * 2> grain;
			for (;;)
			{
				std::string want;
				float target;
				bool closing;
				std::vector<Effect> starts;
				{
					std::lock_guard lock(s_wanted.mutex);
					want = s_wanted.music;
					target = s_wanted.level;
					closing = s_wanted.closing;
					starts.swap(s_wanted.effects);
				}
				// another piece: the one playing fades out, then the next is read and fades in
				if (want != playing)
				{
					target = 0;
					if (level == 0 || music.empty())
					{
						playing = want;
						position = 0;
						level = 0;
						music.clear();
						if (LevelOf(want, 100) > 0 && !ReadWav(SoundPath("music-" + want), music))
							ps5log::Line("[sound] {} is missing or unreadable", SoundPath("music-" + want));
					}
				}
				if (closing)
					target = 0;
				for (Effect effect : starts)
					if (voices.size() < kVoices && !effects[(size_t)effect].empty())
						voices.push_back({&effects[(size_t)effect], 0});

				mix.fill(0);
				const float fade = closing ? kCloseFade : kFade;
				const size_t frames = music.size() / 2;
				for (uint32_t frame = 0; frame < kGrain && frames; frame++)
				{
					level = level < target ? std::min(target, level + fade) : std::max(target, level - fade);
					mix[frame * 2] += music[position * 2] * level;
					mix[frame * 2 + 1] += music[position * 2 + 1] * level;
					position = (position + 1) % frames;
				}
				if (!frames)
					level = 0;
				for (Voice& voice : voices)
				{
					const size_t count = std::min<size_t>(kGrain, voice.clip->size() / 2 - voice.at);
					for (size_t frame = 0; frame < count; frame++)
					{
						mix[frame * 2] += (*voice.clip)[(voice.at + frame) * 2];
						mix[frame * 2 + 1] += (*voice.clip)[(voice.at + frame) * 2 + 1];
					}
					voice.at += count;
				}
				std::erase_if(voices, [](const Voice& voice) { return voice.at * 2 >= voice.clip->size(); });
				for (size_t i = 0; i < grain.size(); i++)
					grain[i] = (int16_t)std::clamp(mix[i], -32768.0f, 32767.0f);

				if (sceAudioOutOutput(port, grain.data()) < 0)
				{
					ps5log::Line("[sound] AudioOut output failed: the launcher is silent");
					break;
				}
				if (closing && level == 0 && voices.empty())
					break;
			}
			sceAudioOutOutput(port, nullptr); // wait for the last grain
			sceAudioOutClose(port);
		}
	}

	void Start(const std::string& music, int volume, bool menuSounds)
	{
		if (s_thread.joinable())
			return;
		static const int initialized = sceAudioOutInit(); // Cemu's and Azahar's own calls then report it done
		(void)initialized;
		const int port = sceAudioOutOpen(kUserSystem, kPortMain, 0, kGrain, kRate, kFormatStereoS16);
		if (port < 0)
		{
			ps5log::Line("[sound] AudioOut did not open ({:#x}): the launcher is silent", (uint32_t)port);
			return;
		}
		const std::array<int, 8> volumes{kVolume0dB, kVolume0dB, kVolume0dB, kVolume0dB, kVolume0dB, kVolume0dB, kVolume0dB, kVolume0dB};
		sceAudioOutSetVolume(port, kVolumeFlagsLeftRight, volumes.data());
		{
			std::lock_guard lock(s_wanted.mutex);
			s_wanted.music = music;
			s_wanted.level = LevelOf(music, volume);
			s_wanted.menuSounds = menuSounds;
			s_wanted.closing = false;
			s_wanted.effects.clear();
		}
		s_thread = std::thread(Mix, port);
	}

	void SetMusic(const std::string& music, int volume)
	{
		std::lock_guard lock(s_wanted.mutex);
		s_wanted.music = music;
		s_wanted.level = LevelOf(music, volume);
	}

	void SetMenuSounds(bool on)
	{
		std::lock_guard lock(s_wanted.mutex);
		s_wanted.menuSounds = on;
	}

	void Play(Effect effect)
	{
		std::lock_guard lock(s_wanted.mutex);
		// (a thread that has stopped on an error takes none)
		if (s_wanted.menuSounds && s_thread.joinable() && s_wanted.effects.size() < kVoices)
			s_wanted.effects.push_back(effect);
	}

	void Stop()
	{
		if (!s_thread.joinable())
			return;
		{
			std::lock_guard lock(s_wanted.mutex);
			s_wanted.closing = true;
		}
		s_thread.join();
	}
}
