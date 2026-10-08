// SPDX-License-Identifier: GPL-3.0-or-later
// PS5CEMU-HAR's UI kit: pictures (images.h).

#include "images.h"
#include "canvas.h"

// stb's loaders, static to this file so they meet no other copy in the app (ReShade's)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_TGA
#include "stb_image.h"
#pragma clang diagnostic pop

#include <algorithm>
#include <cmath>

namespace ui
{
	namespace
	{
		constexpr size_t kKept = 96;		// textures kept before the least recently drawn go
		constexpr int kUploadsPerFrame = 3; // decoded pictures handed to the GPU a frame
		constexpr int kLargest = 1280;		// a side, larger pictures are shrunk to it
		constexpr int kBlurred = 320;		// a backdrop's width, softened: lightly (7.4), so a game's
											// boot screen stays recognisable behind the menus
		constexpr int kBlurRadius = 3;

		// A picture shrunk by whole-pixel boxes to fit width x height
		std::vector<uint8_t> Shrink(const uint8_t* rgba, int width, int height, int toWidth, int toHeight)
		{
			std::vector<uint8_t> out((size_t)toWidth * toHeight * 4);
			for (int y = 0; y < toHeight; y++)
			{
				const int y0 = y * height / toHeight, y1 = std::max(y0 + 1, (y + 1) * height / toHeight);
				for (int x = 0; x < toWidth; x++)
				{
					const int x0 = x * width / toWidth, x1 = std::max(x0 + 1, (x + 1) * width / toWidth);
					uint32_t sum[4] = {};
					for (int sy = y0; sy < y1; sy++)
						for (int sx = x0; sx < x1; sx++)
							for (int c = 0; c < 4; c++)
								sum[c] += rgba[((size_t)sy * width + sx) * 4 + c];
					const uint32_t count = (uint32_t)((y1 - y0) * (x1 - x0));
					for (int c = 0; c < 4; c++)
						out[((size_t)y * toWidth + x) * 4 + c] = (uint8_t)(sum[c] / count);
				}
			}
			return out;
		}

		// Three box blurs, near enough a Gaussian
		void Blur(std::vector<uint8_t>& rgba, int width, int height, int radius)
		{
			std::vector<uint8_t> temp(rgba.size());
			for (int pass = 0; pass < 3; pass++)
				for (int vertical = 0; vertical < 2; vertical++)
				{
					const int lines = vertical ? width : height, length = vertical ? height : width;
					for (int line = 0; line < lines; line++)
						for (int at = 0; at < length; at++)
						{
							uint32_t sum[4] = {};
							int count = 0;
							for (int k = -radius; k <= radius; k++)
							{
								const int p = std::clamp(at + k, 0, length - 1);
								const size_t index = vertical ? ((size_t)p * width + line) * 4 : ((size_t)line * width + p) * 4;
								for (int c = 0; c < 4; c++)
									sum[c] += rgba[index + c];
								count++;
							}
							const size_t index = vertical ? ((size_t)at * width + line) * 4 : ((size_t)line * width + at) * 4;
							for (int c = 0; c < 4; c++)
								temp[index + c] = (uint8_t)(sum[c] / count);
						}
					rgba.swap(temp);
				}
		}

		// A colour with its luminance no more than most (white text on it keeps its contrast)
		uint32_t Darkened(float r, float g, float b, float most)
		{
			float luminance = Luminance(Rgba((uint8_t)r, (uint8_t)g, (uint8_t)b));
			for (int i = 0; i < 24 && luminance > most; i++)
			{
				r *= 0.9f, g *= 0.9f, b *= 0.9f;
				luminance = Luminance(Rgba((uint8_t)r, (uint8_t)g, (uint8_t)b));
			}
			return Rgba((uint8_t)r, (uint8_t)g, (uint8_t)b);
		}

		// The two ambient colours: the most frequent saturated colour of a 32 x 32 copy, and a dark
		// companion (the darker pixels' average, darker still)
		void Ambient(const uint8_t* rgba, int width, int height, uint32_t out[2])
		{
			const std::vector<uint8_t> small = Shrink(rgba, width, height, 32, 32);
			struct Bin
			{
				float weight = 0, r = 0, g = 0, b = 0;
			};
			std::vector<Bin> bins(512);
			float darkR = 0, darkG = 0, darkB = 0, darkCount = 0;
			for (size_t i = 0; i < small.size(); i += 4)
			{
				const float r = small[i], g = small[i + 1], b = small[i + 2], a = small[i + 3] / 255.0f;
				if (a < 0.5f)
					continue;
				const float high = std::max({r, g, b}), low = std::min({r, g, b});
				const float saturation = high > 0 ? (high - low) / high : 0;
				const float weight = 0.15f + saturation * saturation * (high / 255.0f);
				Bin& bin = bins[((int)r >> 5) << 6 | ((int)g >> 5) << 3 | ((int)b >> 5)];
				bin.weight += weight;
				bin.r += r * weight, bin.g += g * weight, bin.b += b * weight;
				if (high < 110)
				{
					darkR += r, darkG += g, darkB += b;
					darkCount++;
				}
			}
			const Bin* best = &bins[0];
			for (const Bin& bin : bins)
				if (bin.weight > best->weight)
					best = &bin;
			if (best->weight <= 0)
			{
				out[0] = out[1] = 0;
				return;
			}
			const float r = best->r / best->weight, g = best->g / best->weight, b = best->b / best->weight;
			out[0] = Darkened(r, g, b, 0.18f);
			if (darkCount > 0)
				out[1] = Darkened(darkR / darkCount * 0.7f, darkG / darkCount * 0.7f, darkB / darkCount * 0.7f, 0.03f);
			else
				out[1] = Darkened(r * 0.3f, g * 0.3f, b * 0.3f, 0.03f);
		}
	}

	Images::Images(Gfx& gfx) : m_gfx(gfx) {}

	Images::~Images()
	{
		Stop();
	}

	const Picture& Images::Get(const std::string& path, bool blurred)
	{
		static const Picture kNone{0, 0, 0, {}, true};
		if (path.empty())
			return kNone;
		const std::string key = blurred ? path + "#blurred" : path;
		Entry& entry = m_entries[key];
		entry.used = m_frame;
		if (!entry.requested)
		{
			entry.requested = true;
			std::lock_guard lock(m_lock);
			if (m_stopping)
				return entry.picture;
			// the newest first: what is on screen now
			m_jobs.push_front({key, path, blurred});
			if (!m_worker.joinable())
				m_worker = std::thread(&Images::Work, this);
			m_wake.notify_one();
		}
		return entry.picture;
	}

	void Images::Work()
	{
		for (;;)
		{
			Job job;
			{
				std::unique_lock lock(m_lock);
				m_wake.wait(lock, [this] { return m_stopping || !m_jobs.empty(); });
				if (m_stopping)
					return;
				job = std::move(m_jobs.front());
				m_jobs.pop_front();
			}
			Done done = Decode(job);
			std::lock_guard lock(m_lock);
			m_done.push_back(std::move(done));
		}
	}

	Images::Done Images::Decode(const Job& job)
	{
		Done done;
		done.key = job.key;
		int width = 0, height = 0, channels = 0;
		stbi_uc* pixels = stbi_load(job.path.c_str(), &width, &height, &channels, 4);
		if (!pixels || width <= 0 || height <= 0)
		{
			if (pixels)
				stbi_image_free(pixels);
			done.failed = true;
			return done;
		}
		Ambient(pixels, width, height, done.ambient);
		done.width = width;
		done.height = height;
		if (job.blurred)
		{
			const int toWidth = std::min(width, kBlurred), toHeight = std::max(1, height * toWidth / width);
			done.rgba = Shrink(pixels, width, height, toWidth, toHeight);
			Blur(done.rgba, toWidth, toHeight, kBlurRadius);
			done.width = toWidth;
			done.height = toHeight;
		}
		else if (width > kLargest || height > kLargest)
		{
			const float scale = std::min((float)kLargest / width, (float)kLargest / height);
			done.width = std::max(1, (int)(width * scale));
			done.height = std::max(1, (int)(height * scale));
			done.rgba = Shrink(pixels, width, height, done.width, done.height);
		}
		else
			done.rgba.assign(pixels, pixels + (size_t)width * height * 4);
		stbi_image_free(pixels);
		return done;
	}

	void Images::Update()
	{
		m_frame++;
		if (m_uploads)
			for (int i = 0; i < kUploadsPerFrame; i++)
			{
				Done done;
				{
					std::lock_guard lock(m_lock);
					if (m_done.empty())
						break;
					done = std::move(m_done.front());
					m_done.pop_front();
				}
				const auto it = m_entries.find(done.key);
				if (it == m_entries.end())
					continue; // let go of meanwhile
				Picture& picture = it->second.picture;
				picture.failed = done.failed;
				picture.ambient[0] = done.ambient[0];
				picture.ambient[1] = done.ambient[1];
				if (!done.failed)
				{
					picture.width = done.width;
					picture.height = done.height;
					picture.texture = m_gfx.CreateTexture(done.width, done.height, done.rgba.data());
				}
			}
		// past the limit, the pictures not drawn for longest go
		size_t textures = 0;
		for (const auto& [key, entry] : m_entries)
			textures += entry.picture.texture != 0;
		if (textures <= kKept)
			return;
		std::vector<std::pair<uint64_t, std::string>> order;
		for (const auto& [key, entry] : m_entries)
			if (entry.picture.texture && entry.used + 2 < m_frame)
				order.push_back({entry.used, key});
		std::sort(order.begin(), order.end());
		for (size_t i = 0; i < order.size() && textures > kKept; i++, textures--)
		{
			m_gfx.DestroyTexture(m_entries[order[i].second].picture.texture);
			m_entries.erase(order[i].second);
		}
	}

	void Images::Stop()
	{
		{
			std::lock_guard lock(m_lock);
			m_stopping = true;
			m_jobs.clear();
		}
		m_wake.notify_all();
		if (m_worker.joinable())
			m_worker.join();
		if (m_gfx.Running())
			for (auto& [key, entry] : m_entries)
				if (entry.picture.texture)
					m_gfx.DestroyTexture(entry.picture.texture);
		m_entries.clear();
		m_done.clear();
	}
}
