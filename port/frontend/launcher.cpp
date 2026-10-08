// SPDX-License-Identifier: GPL-3.0-or-later
// PS5CEMU-HAR: the launcher (launcher.h).
//
// Each side (Cemu's main.rml, Azahar's azahar.rml: tools/render-layout.py) has three screens behind
// tabs along the top, Home, Library and Settings, a game's page (Details), and the pages they open:
// a game's graphic packs, a player's controls and buttons, the folder browser for games and
// installs, Artic Base, a dropdown's choices and a setting's longer help. Box art comes first and
// words only where they are needed: a setting says what it does in one line, Triangle has the rest.
// The start screen (start.rml) chooses the side. The folder browser reads folders with
// sceKernelGetdents, as ProsperoEden's does; the graphic packs, the controls and the installs are
// laid out as Cemu's own windows have them.

#include "launcher.h"
#include "actions.h"
#include "sound.h"
#include "ui_host.h"
#include "../app/boxart.h"
#include "../app/compatibility.h"
#include "../app/gameinfo.h"
#include "../app/pack_updates.h"
#include "../app/paths.h"
#include "../app/updates.h"
#include "../app/usb_devices.h"
#include "../ps5/display.h"
#include "../ps5/kernel.h"
#include "../ps5/log.h"
#include "../ps5/notify.h"
#include "../ps5/pad.h"
#include "../ps5/privilege.h"
#include "../azahar/azahar.h"
#include "../azahar/controls.h"
#include "../azahar/library.h"

#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/StringUtilities.h>

#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <map>
#include <netinet/in.h>
#include <set>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ps5launcher
{
	namespace
	{
		using namespace ps5actions;

		enum class Key
		{
			Up,
			Down,
			Left,
			Right,
			Cross,
			Circle,
			Triangle,
			Square,
			L1,
			R1,
			L2,
			R2,
		};

		// The DualSense as key presses: any connected controller, the left stick as the D-pad, and
		// the directions and the shoulder buttons repeating while held.
		class Input
		{
		public:
			std::vector<Key> Poll()
			{
				uint32_t buttons = 0;
				for (int player = 0; player < ps5pad::kMaxPlayers; player++)
				{
					ps5pad::Data data;
					if (!ps5pad::Read(player, data))
						continue;
					buttons |= data.buttons;
					if (data.leftY < 64)
						buttons |= ps5pad::kUp;
					else if (data.leftY > 192)
						buttons |= ps5pad::kDown;
					if (data.leftX < 64)
						buttons |= ps5pad::kLeft;
					else if (data.leftX > 192)
						buttons |= ps5pad::kRight;
				}
				const uint64_t now = sceKernelGetProcessTime();
				std::vector<Key> keys;
				for (const auto& [mask, key, repeats] : kMap)
				{
					const bool down = buttons & mask, was = m_held & mask;
					const size_t slot = (size_t)key;
					if (down && !was)
					{
						keys.push_back(key);
						m_repeatAt[slot] = now + kRepeatDelayUs;
					}
					else if (down && repeats && now >= m_repeatAt[slot])
					{
						keys.push_back(key);
						m_repeatAt[slot] = now + kRepeatRateUs;
					}
				}
				m_held = buttons;
				return keys;
			}

		private:
			static constexpr uint64_t kRepeatDelayUs = 400000, kRepeatRateUs = 90000;
			struct Mapping
			{
				uint32_t mask;
				Key key;
				bool repeats;
			};
			static constexpr Mapping kMap[] = {
				{ps5pad::kUp, Key::Up, true},
				{ps5pad::kDown, Key::Down, true},
				{ps5pad::kLeft, Key::Left, true},
				{ps5pad::kRight, Key::Right, true},
				{ps5pad::kCross, Key::Cross, false},
				{ps5pad::kCircle, Key::Circle, false},
				{ps5pad::kTriangle, Key::Triangle, false},
				{ps5pad::kSquare, Key::Square, false},
				{ps5pad::kL1, Key::L1, true},
				{ps5pad::kR1, Key::R1, true},
				{ps5pad::kL2, Key::L2, true},
				{ps5pad::kR2, Key::R2, true},
			};
			uint32_t m_held = ~0u; // nothing counts as pressed until it has been released once
			std::array<uint64_t, 12> m_repeatAt{};
		};

		void SetClass(Rml::ElementDocument* document, const std::string& id, const char* name, bool enabled)
		{
			if (Rml::Element* element = document->GetElementById(id))
				element->SetClass(name, enabled);
		}

		// Text in what the launcher's fonts have (tools/render-fonts.py: ASCII, Latin-1 and the
		// typographic marks below), so a name such as "Pokémon" keeps its accent; anything else is
		// left out rather than drawn as a missing glyph.
		std::string Printable(const std::string& text)
		{
			std::string out;
			for (size_t i = 0; i < text.size();)
			{
				const unsigned char lead = (unsigned char)text[i];
				const int length = lead < 0x80 ? 1 : lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
				uint32_t c = length == 1 ? lead : lead & (0xFF >> (length + 1));
				for (int k = 1; k < length && i + k < text.size(); k++)
					c = c << 6 | ((unsigned char)text[i + k] & 0x3F);
				const std::string_view whole(text.data() + i, std::min((size_t)length, text.size() - i));
				i += length;
				if (c < 0x80)
					out += (char)c;
				else if (c == 0xA0)
					out += ' ';
				else if ((c > 0xA0 && c <= 0xFF) || c == 0x2013 || c == 0x2014 || c == 0x2018 || c == 0x2019 || c == 0x201C ||
					c == 0x201D || c == 0x2022 || c == 0x2026 || c == 0x20AC || c == 0x2122)
					out += whole;
			}
			return out;
		}

		void SetText(Rml::ElementDocument* document, const std::string& id, const std::string& text)
		{
			if (Rml::Element* element = document->GetElementById(id))
				element->SetInnerRML(Rml::StringUtilities::EncodeRml(Printable(text)));
		}

		// Text whose lines end with '\n', a line each (empty ones dropped).
		void SetLines(Rml::ElementDocument* document, const std::string& id, const std::string& text)
		{
			std::string rml;
			size_t start = 0;
			while (start < text.size())
			{
				size_t end = text.find('\n', start);
				if (end == std::string::npos)
					end = text.size();
				if (end > start)
					rml += (rml.empty() ? "" : "<br/>") + Rml::StringUtilities::EncodeRml(Printable(text.substr(start, end - start)));
				start = end + 1;
			}
			if (Rml::Element* element = document->GetElementById(id))
				element->SetInnerRML(rml);
		}

		// Paragraphs (separated by a blank line) with a blank line between them
		void SetParagraphs(Rml::ElementDocument* document, const std::string& id, const std::string& text)
		{
			std::string rml;
			size_t start = 0;
			bool blank = false;
			while (start <= text.size())
			{
				size_t end = text.find('\n', start);
				if (end == std::string::npos)
					end = text.size();
				if (end == start)
					blank = !rml.empty();
				else
				{
					rml += (rml.empty() ? "" : blank ? "<br/><br/>" : "<br/>") + Rml::StringUtilities::EncodeRml(Printable(text.substr(start, end - start)));
					blank = false;
				}
				start = end + 1;
			}
			if (Rml::Element* element = document->GetElementById(id))
				element->SetInnerRML(rml);
		}

		void SetImage(Rml::ElementDocument* document, const std::string& id, const std::string& source)
		{
			if (Rml::Element* element = document->GetElementById(id))
				element->SetAttribute("src", source);
		}

		// A picture fitted whole inside a box (left, top, width, height), centred in it; its own size
		// from its TGA's header
		void FitImage(Rml::ElementDocument* document, const std::string& id, const std::string& source, float left, float top, float width,
			float height)
		{
			Rml::Element* element = document->GetElementById(id);
			if (!element)
				return;
			int w = 0, h = 0;
			float fittedWidth = width, fittedHeight = height;
			// the layout's own pictures are named from the UI's folder
			const std::string file = !source.empty() && source[0] != '/' ? ps5ui::AssetPath(source) : source;
			if (ps5boxart::ImageSize(file, w, h) && w > 0 && h > 0)
			{
				const float scale = std::min(width / w, height / h);
				fittedWidth = std::round(w * scale);
				fittedHeight = std::round(h * scale);
			}
			element->SetAttribute("src", source);
			element->SetProperty("width", fmt::format("{}px", fittedWidth));
			element->SetProperty("height", fmt::format("{}px", fittedHeight));
			element->SetProperty("left", fmt::format("{}px", std::round(left + (width - fittedWidth) / 2)));
			element->SetProperty("top", fmt::format("{}px", std::round(top + (height - fittedHeight) / 2)));
		}

		// The first row shown of a list with `visible` rows, keeping `selected` in view.
		int Scroll(int selected, int visible)
		{
			return selected < visible ? 0 : selected - (visible - 1);
		}

		// Up and Down move through a list, wrapping; L1 and R1 a page at a time. False for other keys.
		bool Browse(Key key, int& selected, int count, int page)
		{
			if (count <= 0)
				return false;
			if (key == Key::Up)
				selected = (selected + count - 1) % count;
			else if (key == Key::Down)
				selected = (selected + 1) % count;
			else if (key == Key::L1 || key == Key::L2)
				selected = std::max(0, selected - page);
			else if (key == Key::R1 || key == Key::R2)
				selected = std::min(count - 1, selected + page);
			else
				return false;
			return true;
		}

		enum Screen
		{
			kHome,
			kLibrary,
			kDetails,
			kSettings,
			kPacks,
			kPlayer,
			kMapping,
			kFiles,
			kArtic,
			kLoading,
		};

		// The tabs along the top, and which screen each is
		constexpr Screen kTabs[] = {kHome, kLibrary, kSettings};
		constexpr const char* kScreenIds[] = {"home", "library", "details", "settings"};
		constexpr const char* kPageIds[] = {"packs-dialog", "player-dialog", "mapping-dialog", "files-dialog", "artic-dialog"};

		// Settings' categories, as render-layout.py lays out the rail (the same order)
		constexpr const char* kCategoriesWiiU[] = {"video", "audio", "controls", "usb", "files", "installs", "online", "diagnostics", "about"};
		constexpr const char* kCategories3ds[] = {"video", "audio", "controls", "borders", "system", "files", "installs", "online", "diagnostics", "about"};
		constexpr int kSettingRows = 7, kHints = 4;

		constexpr const char* kBorderThemes[] = {"None", "Midnight", "Waves", "Aurora", "Shell", "PS5CEMU-HAR"};
		constexpr int kBorderThemeCount = (int)std::size(kBorderThemes);
		constexpr const char* kRegions[] = {"Automatic", "Japan", "USA", "Europe", "Australia", "China", "Korea", "Taiwan"};
		// Automatic (-1), then Azahar's SystemLanguage 0 to 11
		constexpr const char* kLanguages[] = {"Automatic", "Japanese", "English", "French", "German", "Italian", "Spanish",
			"Chinese (simplified)", "Korean", "Dutch", "Portuguese", "Russian", "Chinese (traditional)"};
		constexpr const char* kResolutions[] = {"", "1x (400x240)", "2x (800x480)", "3x (1200x720)", "4x (1600x960)", "5x (2000x1200)",
			"6x (2400x1440)", "7x (2800x1680)", "8x (3200x1920)", "9x (3600x2160)", "10x (4000x2400)"};
		constexpr const char* kLayouts[] = {"Top above bottom", "Top screen only", "Large top screen", "Side by side"};
		constexpr const char* kTextureFilters[] = {"None", "Anime4K", "Bicubic", "ScaleForce", "xBRZ", "MMPX"};
		constexpr const char* kUpscaleFilters[] = {"Linear", "Bicubic", "Bicubic Hermite", "Nearest neighbour"};
		constexpr const char* kMusic[] = {"setup", "off"};

		// Artic Base's page (the 3DS's side)
		enum ArticRow
		{
			kRowArticAddress,
			kRowArticConnect,
			kRowArticSetupOld,
			kRowArticSetupNew,
			kArticRows,
		};

		// A player's settings (the Wii U's)
		enum PlayerRow
		{
			kRowType,
			kRowMotion,
			kRowRumble,
			kRowLeftDeadzone,
			kRowRightDeadzone,
			kRowButtons,
			kRowReset,
			kPlayerRows,
		};

		constexpr int kListRows = 7, kFileRows = 6, kPresetRows = 4, kPickerRows = 6;
		constexpr uint64_t kCaptureUs = 6000000; // how long a button mapping waits for a press

		// What the screen shows, to tell whether a key press did anything, for its sound: the layout
		// (its text, pictures and places) and which screens are open and which rows focused.
		std::string Showing(Rml::ElementDocument* document)
		{
			std::string showing = document->GetInnerRML();
			Rml::ElementList elements;
			document->QuerySelectorAll(elements, ".open, .focused, .selected");
			for (Rml::Element* element : elements)
				showing += (element->IsClassSet("open") ? "\nopen " : "\nfocused ") + element->GetId();
			return showing;
		}

		// A key press's sound (sound.h): moving when the cursor or a value moved, choosing or going back
		// when something opened, closed or changed, and a bump when Cross was pressed on what does
		// nothing.
		void Feedback(Key key, bool changed)
		{
			using ps5sound::Effect;
			if (key == Key::Cross)
				ps5sound::Play(changed ? Effect::Select : Effect::Denied);
			else if (!changed)
				return;
			else if (key == Key::Circle)
				ps5sound::Play(Effect::Back);
			else if (key == Key::Triangle || key == Key::Square)
				ps5sound::Play(Effect::Select);
			else
				ps5sound::Play(Effect::Move);
		}

		// The hints along the bottom right: a glyph (render-glyphs.py) and a word or two each
		struct Hint
		{
			const char* glyph;
			std::string text;
		};

		void SetHints(Rml::ElementDocument* document, const std::vector<Hint>& hints)
		{
			for (int i = 0; i < kHints; i++)
			{
				const bool used = i < (int)hints.size();
				SetClass(document, fmt::format("hint-{}", i), "unused", !used);
				if (!used)
					continue;
				SetImage(document, fmt::format("hint-img-{}", i), fmt::format("glyphs/{}-30.tga", hints[i].glyph));
				SetText(document, fmt::format("hint-text-{}", i), hints[i].text);
			}
		}

		class Launcher
		{
		public:
			Launcher(Rml::ElementDocument* document, System system, ps5settings::Launcher& settings, const Status& status)
				: m_document(document), m_system(system), m_settings(settings), m_status(status)
			{
			}

			void Initialize()
			{
				m_scanning = CoreReady() && SystemScanning();
				if (CoreReady())
					m_games = SystemGames();
				if (!m_scanning)
				{
					FetchBoxArt();
					if (CoreReady())
						RememberCount();
				}
				m_homeRow = kHeroRow;
				m_homeColumn = 0;
				ShowTab(kHome);
				Poll();
			}

			bool Done() const { return m_launch.has_value() && m_screen == kLoading; }
			// A button mapping waiting for a press: the controller is PollCapture's.
			bool Capturing() const { return m_capture.active; }
			// The player went back to the start screen.
			bool Leaving() const { return m_leaving; }
			const std::optional<ps5emu::Game>& Choice() const { return m_launch; }

			// The clock, the library once the side has finished looking for games, box art arriving,
			// a button being mapped, an install, and the graphic packs' update.
			void Poll()
			{
				const std::time_t minute = std::time(nullptr) / 60;
				if (minute != m_shownMinute)
				{
					m_shownMinute = minute;
					const std::time_t now = minute * 60;
					char label[16]{};
					if (const std::tm* local = std::localtime(&now))
						std::strftime(label, sizeof(label), "%H:%M", local);
					SetText(m_document, "menu-clock", label);
				}
				if (m_scanning && !SystemScanning())
				{
					m_scanning = false;
					m_games = SystemGames();
					ps5log::Line("[launcher] {} games", m_games.size());
					RememberCount();
					FetchBoxArt();
					Refresh();
				}
				// a cover arrived: one on the screen may be it
				if (ps5boxart::Arrivals() != m_boxArrivals)
				{
					m_boxArrivals = ps5boxart::Arrivals();
					if (m_screen == kHome || m_screen == kLibrary || m_screen == kDetails)
						Refresh();
				}
				if (m_capture.active)
					PollCapture();
				if (m_installing)
					PollInstall();
				PollPacks();
			}

			void HandleKey(Key key)
			{
				if (m_picker.open)
				{
					PickerKey(key);
					return;
				}
				if (m_info)
				{
					// any of these closes a setting's help
					if (key == Key::Circle || key == Key::Triangle || key == Key::Cross)
						CloseInfo();
					return;
				}
				switch (m_screen)
				{
				case kHome: HomeKey(key); break;
				case kLibrary: LibraryKey(key); break;
				case kDetails: DetailsKey(key); break;
				case kSettings: SettingsKey(key); break;
				case kPacks: PacksKey(key); break;
				case kPlayer: PlayerKey(key); break;
				case kMapping: MappingKey(key); break;
				case kFiles: FilesKey(key); break;
				case kArtic: ArticKey(key); break;
				case kLoading: break;
				}
			}

		private:
			// -- the emulator this launcher is --------------------------------------------------

			bool Is3ds() const { return m_system == System::N3ds; }
			bool HasPacks() const { return !Is3ds(); }
			// Cemu's games need its core; Azahar's are read by the launcher itself
			bool CoreReady() const { return Is3ds() || m_status.coreReady; }
			const std::string& Notice() const { return Is3ds() ? m_status.notice3ds : m_status.notice; }
			bool SystemScanning() const { return Is3ds() ? ps5azahar::Scanning() : ps5emu::Scanning(); }
			std::vector<ps5emu::Game> SystemGames() const { return Is3ds() ? ps5azahar::ListGames() : ps5emu::ListGames(); }
			std::string& GamesFolder() { return Is3ds() ? m_settings.n3ds.gamesFolder : m_settings.gamesFolder; }
			uint64_t& LastGame() { return Is3ds() ? m_settings.n3ds.lastGame : m_settings.lastGame; }
			std::vector<uint64_t>& Recent() { return Is3ds() ? m_settings.n3ds.recent : m_settings.recent; }
			int& Volume() { return Is3ds() ? m_settings.n3ds.volume : m_settings.volume; }
			const char* Icon() const { return Is3ds() ? "icons/azahar.tga" : "icons/ps5cemu.tga"; }
			std::string CoverOf(uint64_t titleId) const { return Is3ds() ? ps5azahar::CoverPath(titleId) : ps5emu::CoverPath(titleId); }
			ps5boxart::System BoxSystem() const { return Is3ds() ? ps5boxart::System::N3ds : ps5boxart::System::WiiU; }
			// the library's grid: Wii U box art is 5:7, 3DS box art wider than tall (render-layout.py)
			int GridColumns() const { return 7; }
			int GridRows() const { return Is3ds() ? 3 : 2; }
			float TileWidth() const { return 212; }
			float TileHeight() const { return Is3ds() ? 196.0f : 290.0f; }
			// Home: the last game's icon on a card, and the recent games' icons (render-layout.py's sizes)
			static constexpr float kHeroLeft = 1384, kHeroTop = 150, kHeroSize = 416, kShelfSize = 160;
			int CategoryCount() const { return Is3ds() ? (int)std::size(kCategories3ds) : (int)std::size(kCategoriesWiiU); }
			std::string Category(int index) const
			{
				return Is3ds() ? kCategories3ds[std::clamp(index, 0, CategoryCount() - 1)] : kCategoriesWiiU[std::clamp(index, 0, CategoryCount() - 1)];
			}

			// How many games the start screen says this side has, next time: it looks for none itself,
			// as neither emulator runs on it
			void RememberCount()
			{
				int& count = Is3ds() ? m_settings.n3ds.gameCount : m_settings.gameCount;
				if (count == (int)m_games.size())
					return;
				count = (int)m_games.size();
				SaveSettings();
			}

			// GameTDB's covers for the games that have none yet (boxart.h)
			void FetchBoxArt()
			{
				std::vector<std::string> ids;
				for (const auto& game : m_games)
					if (!game.gameId.empty())
						ids.push_back(game.gameId);
				ps5boxart::Fetch(BoxSystem(), ids);
			}

			void SaveSettings()
			{
				if (!ps5settings::Save(m_settings))
					ps5log::Line("[launcher] could not save {}", ps5paths::kLauncherSettings);
			}

			// -- a game's pictures and facts ----------------------------------------------------

			// Its box art when GameTDB had one, else its icon, else the side's own
			std::string ArtOf(const ps5emu::Game& game, bool& boxArt) const
			{
				const std::string box = ps5boxart::Path(BoxSystem(), game.gameId);
				boxArt = !box.empty();
				if (boxArt)
					return box;
				const std::string icon = CoverOf(game.titleId);
				return icon.empty() ? Icon() : icon;
			}

			struct Facts
			{
				ps5gameinfo::Info info;
				bool known = false;
				const ps5compat::Report* report = nullptr;
			};

			Facts FactsOf(const ps5emu::Game& game) const
			{
				Facts facts;
				facts.known = ps5gameinfo::Find(BoxSystem(), game.gameId, facts.info);
				facts.report = ps5compat::Find(Is3ds(), game.name);
				return facts;
			}

			// "Nintendo  /  2017", or what the game itself says when GameTDB has nothing
			std::string Byline(const ps5emu::Game& game, const Facts& facts) const
			{
				if (facts.known && (!facts.info.publisher.empty() || !facts.info.released.empty()))
					return Join({facts.info.publisher, ps5gameinfo::Year(facts.info.released)});
				if (Is3ds())
					return Join({game.publisher, game.format});
				return Join({fmt::format("v{}", game.version), game.dlcCount ? "DLC" : "", game.format});
			}

			void ShowStatus(const std::string& id, const Facts& facts)
			{
				const std::string status = facts.report ? facts.report->status : std::string();
				SetText(m_document, id, status);
				SetClass(m_document, id, "unused", status.empty());
				const std::string kind = ps5compat::Kind(status);
				for (const char* name : {"good", "warn", "bad"})
					SetClass(m_document, id, name, kind == name);
			}

			// A tile (the shelf's or the grid's): the game's art fitted in it, its name under an icon
			void ShowTile(const std::string& prefix, int slot, const ps5emu::Game* game, float width, float height)
			{
				const std::string tile = fmt::format("{}-{}", prefix, slot);
				SetClass(m_document, tile, "unused", !game);
				if (!game)
					return;
				bool boxArt = false;
				const std::string art = ArtOf(*game, boxArt);
				SetClass(m_document, tile, "noart", !boxArt);
				SetText(m_document, fmt::format("{}-text-{}", prefix, slot), game->name);
				if (boxArt)
					FitImage(m_document, fmt::format("{}-img-{}", prefix, slot), art, 0, 0, width, height);
				else // the icon, square, above the name
				{
					const float size = std::min(width * 0.62f, height * 0.5f);
					FitImage(m_document, fmt::format("{}-img-{}", prefix, slot), art, (width - size) / 2, height * 0.12f, size, size);
				}
				m_shownArt.insert(art);
			}

			// A game's cover in a box: its box art filling it, or, without any, its icon on a card the box's size
			void ShowCover(const std::string& card, const std::string& image, const std::string& art, bool boxArt, float left, float top,
				float width, float height)
			{
				SetClass(m_document, card, "visible", !boxArt);
				if (boxArt)
				{
					FitImage(m_document, image, art, left, top, width, height);
					return;
				}
				if (Rml::Element* element = m_document->GetElementById(card))
				{
					element->SetProperty("left", fmt::format("{}px", left));
					element->SetProperty("top", fmt::format("{}px", top));
					element->SetProperty("width", fmt::format("{}px", width));
					element->SetProperty("height", fmt::format("{}px", height));
				}
				const float size = std::round(std::min(width, height) * 0.56f);
				FitImage(m_document, image, art, left + (width - size) / 2, top + (height - size) / 2, size, size);
			}

			// A game's own icon (from its files), or "" when it has none
			std::string GameIcon(const ps5emu::Game& game) const { return CoverOf(game.titleId); }

			// How large an icon is drawn in a square of size: a small one (the 3DS's are 48 pixels) at a
			// whole multiple of its own size, so its pixels stay sharp; a larger one (the Wii U's 128)
			// fills it, scaled smoothly
			static float IconSize(const std::string& icon, float size)
			{
				int width = 0, height = 0;
				const std::string file = !icon.empty() && icon[0] != '/' ? ps5ui::AssetPath(icon) : icon;
				if (!ps5boxart::ImageSize(file, width, height) || width <= 0 || width > 64)
					return size;
				return std::floor(size / width) * width;
			}

			// Home's pictures: the game's icon on a square card, at (left, top), size wide; the side's
			// own icon when the game has none (box art is the library's)
			void ShowIcon(const std::string& card, const std::string& image, const std::string& icon, float left, float top, float size)
			{
				SetClass(m_document, card, "visible", true);
				if (Rml::Element* element = m_document->GetElementById(card))
				{
					element->SetProperty("left", fmt::format("{}px", left));
					element->SetProperty("top", fmt::format("{}px", top));
					element->SetProperty("width", fmt::format("{}px", size));
					element->SetProperty("height", fmt::format("{}px", size));
				}
				const std::string art = icon.empty() ? Icon() : icon;
				const float inner = icon.empty() ? std::round(size * 0.56f) : IconSize(art, size - 32);
				FitImage(m_document, image, art, left + (size - inner) / 2, top + (size - inner) / 2, inner, inner);
				m_shownArt.insert(art);
			}

			// A recent game on Home's shelf: its icon filling the tile, or the side's icon and its name
			void ShowIconTile(const std::string& prefix, int slot, const ps5emu::Game* game, float size)
			{
				const std::string tile = fmt::format("{}-{}", prefix, slot);
				SetClass(m_document, tile, "unused", !game);
				if (!game)
					return;
				const std::string icon = GameIcon(*game);
				SetClass(m_document, tile, "noart", icon.empty());
				SetText(m_document, fmt::format("{}-text-{}", prefix, slot), game->name);
				const std::string art = icon.empty() ? Icon() : icon;
				const float inner = icon.empty() ? std::round(size * 0.5f) : IconSize(art, size);
				FitImage(m_document, fmt::format("{}-img-{}", prefix, slot), art, (size - inner) / 2, icon.empty() ? size * 0.12f : (size - inner) / 2,
					inner, inner);
				m_shownArt.insert(art);
			}

			// The covers loaded are let go of once many have been: a long scroll keeps them all otherwise
			void ReleaseArt()
			{
				if (m_shownArt.size() <= 64)
					return;
				for (const auto& art : m_shownArt)
					Rml::ReleaseTexture(art);
				m_shownArt.clear();
			}

			// -- screens and tabs -----------------------------------------------------------------

			void ShowScreen(Screen screen)
			{
				m_screen = screen;
				for (int i = 0; i < (int)std::size(kScreenIds); i++)
					SetClass(m_document, kScreenIds[i], "open", (int)screen == i);
				const char* page = screen == kPacks ? "packs-dialog" : screen == kPlayer ? "player-dialog" : screen == kMapping ? "mapping-dialog" :
					screen == kFiles ? "files-dialog" : screen == kArtic ? "artic-dialog" : nullptr;
				for (const char* id : kPageIds)
					SetClass(m_document, id, "open", page && std::strcmp(page, id) == 0);
				SetClass(m_document, "loading-screen", "open", screen == kLoading);
				SetClass(m_document, "app", "page-open", page != nullptr || screen == kLoading || screen == kDetails);
			}

			// The tab of a screen, along the top; the screen under it shown
			void ShowTab(Screen screen)
			{
				m_tab = screen;
				for (int i = 0; i < (int)std::size(kTabs); i++)
				{
					SetClass(m_document, fmt::format("tab-{}", i), "selected", kTabs[i] == screen);
					SetClass(m_document, fmt::format("tab-{}", i), "focused", m_onTabs && kTabs[i] == screen);
				}
				ShowScreen(screen);
				Refresh();
			}

			// L1 and R1, and Left and Right on the tabs: the next tab along
			void NextTab(int direction)
			{
				int at = 0;
				for (int i = 0; i < (int)std::size(kTabs); i++)
					if (kTabs[i] == m_tab)
						at = i;
				at = (at + direction + (int)std::size(kTabs)) % (int)std::size(kTabs);
				ShowTab(kTabs[at]);
			}

			// The tabs, focused: Left and Right change them, Down goes into the screen. True when the
			// key was the tabs'.
			bool TabsKey(Key key)
			{
				if (key == Key::L1 || key == Key::R1)
				{
					NextTab(key == Key::R1 ? 1 : -1);
					return true;
				}
				if (!m_onTabs)
					return false;
				if (key == Key::Left || key == Key::Right)
					NextTab(key == Key::Right ? 1 : -1);
				else if (key == Key::Down || key == Key::Cross)
				{
					m_onTabs = false;
					ShowTab(m_tab);
				}
				else if (key == Key::Circle)
				{
					if (m_tab == kHome)
						m_leaving = true;
					else
						ShowTab(kHome);
				}
				return true;
			}

			void FocusTabs()
			{
				m_onTabs = true;
				ShowTab(m_tab);
			}

			// What is shown now, again (after the games or their art changed)
			void Refresh()
			{
				switch (m_screen)
				{
				case kHome: UpdateHome(); break;
				case kLibrary: UpdateLibrary(); break;
				case kDetails: UpdateDetails(); break;
				case kSettings: UpdateSettings(); break;
				default: break;
				}
			}

			// -- a dropdown --------------------------------------------------------------------

			struct Picker
			{
				bool open = false;
				std::vector<std::string> options;
				int active = -1; // the one in use
				int selected = 0;
				std::function<void(int)> choose;
			};

			void OpenPicker(const std::string& kicker, const std::string& title, std::vector<std::string> options, int active,
				std::function<void(int)> choose)
			{
				if (options.empty())
					return;
				m_picker = {true, std::move(options), active, std::max(active, 0), std::move(choose)};
				SetText(m_document, "picker-kicker", Upper(kicker));
				SetText(m_document, "picker-title", title);
				SetClass(m_document, "picker", "open", true);
				UpdatePicker();
			}

			void PickerKey(Key key)
			{
				if (key == Key::Circle)
					m_picker.open = false;
				else if (key == Key::Cross)
				{
					m_picker.open = false;
					SetClass(m_document, "picker", "open", false);
					m_picker.choose(m_picker.selected);
					RestoreHints();
					return;
				}
				else
					Browse(key, m_picker.selected, (int)m_picker.options.size(), kPickerRows);
				SetClass(m_document, "picker", "open", m_picker.open);
				UpdatePicker();
				if (!m_picker.open)
					RestoreHints();
			}

			void UpdatePicker()
			{
				const int count = (int)m_picker.options.size();
				const int scroll = Scroll(m_picker.selected, kPickerRows);
				for (int row = 0; row < kPickerRows; row++)
				{
					const int index = scroll + row;
					SetClass(m_document, fmt::format("picker-row-{}", row), "focused", index == m_picker.selected);
					SetClass(m_document, fmt::format("picker-row-{}", row), "offscreen", index >= count);
					SetText(m_document, fmt::format("picker-name-{}", row), index < count ? m_picker.options[index] : "");
					SetText(m_document, fmt::format("picker-mark-{}", row), index == m_picker.active ? "In use" : "");
				}
				SetText(m_document, "picker-position", count > kPickerRows ? fmt::format("{} of {}", m_picker.selected + 1, count) : "");
				SetHints(m_document, {{"cross", "Choose"}, {"circle", "Cancel"}});
			}

			// -- a setting's longer help (Triangle) ----------------------------------------------

			void OpenInfo(const std::string& title, const std::string& text)
			{
				m_info = true;
				SetText(m_document, "info-title", title);
				SetLines(m_document, "info-text", text);
				SetClass(m_document, "info", "open", true);
				SetHints(m_document, {{"circle", "Close"}});
			}

			void CloseInfo()
			{
				m_info = false;
				SetClass(m_document, "info", "open", false);
				RestoreHints();
			}

			// The hints of the screen under a dropdown or the help, once it closes
		public:
			void RefreshHints() { RestoreHints(); }

		private:
			void RestoreHints()
			{
				switch (m_screen)
				{
				case kHome: UpdateHome(); break;
				case kLibrary: UpdateLibrary(); break;
				case kDetails: UpdateDetails(); break;
				case kSettings: UpdateSettings(); break;
				case kPacks: UpdatePacks(); break;
				case kPlayer: UpdatePlayer(); break;
				case kMapping: UpdateMapping(); break;
				case kFiles: UpdateFiles(); break;
				case kArtic: UpdateArtic(); break;
				case kLoading: break;
				}
			}

			// -- home --------------------------------------------------------------------------
			// The last game large, with its buttons and box art, and the recent ones on a shelf.

			enum HomeRow
			{
				kHeroRow,
				kShelfRow,
			};

			int FindGame(uint64_t titleId) const
			{
				for (size_t i = 0; i < m_games.size(); i++)
					if (m_games[i].titleId == titleId)
						return (int)i;
				return -1;
			}

			// The hero's buttons, in their order
			enum class HeroAction
			{
				Play,
				Details,
				Library,
				Settings,
				Artic,
			};

			std::vector<std::pair<HeroAction, const char*>> HeroActions() const
			{
				std::vector<std::pair<HeroAction, const char*>> actions;
				if (!Notice().empty() || !CoreReady())
					actions = {{HeroAction::Settings, "Settings"}};
				else if (m_lastIndex >= 0)
					actions = {{HeroAction::Play, "Play"}, {HeroAction::Details, "Details"}};
				else
					actions = {{HeroAction::Library, "Library"}, {HeroAction::Settings, "Settings"}};
				if (Is3ds())
					actions.push_back({HeroAction::Artic, "Artic Base"});
				return actions;
			}

			// The shelf: the recent games (but the last, which the hero has), and "All games"
			int ShelfCount() const { return (int)m_recent.size() + 1; }

			void UpdateHome()
			{
				m_lastIndex = CoreReady() ? FindGame(LastGame()) : -1;
				m_recent.clear();
				for (uint64_t titleId : Recent())
					if (const int index = FindGame(titleId); index >= 0 && index != m_lastIndex && m_recent.size() < 5)
						m_recent.push_back(index);
				const bool notice = !Notice().empty();
				SetClass(m_document, "home", "notice", notice);
				SetClass(m_document, "home-notice", "visible", notice);
				SetText(m_document, "home-notice", Notice());
				const ps5emu::Game* last = m_lastIndex >= 0 ? &m_games[m_lastIndex] : nullptr;
				if (notice || !CoreReady())
				{
					SetText(m_document, "hero-kicker", "SETUP");
					SetText(m_document, "hero-title", notice ? "Something needs a look" : "Starting");
					SetText(m_document, "hero-facts", "");
					SetClass(m_document, "hero-status", "unused", true);
					ShowIcon("hero-card", "hero-cover", "", kHeroLeft, kHeroTop, kHeroSize);
				}
				else if (last)
				{
					const Facts facts = FactsOf(*last);
					SetText(m_document, "hero-kicker", "CONTINUE");
					SetText(m_document, "hero-title", last->name);
					SetText(m_document, "hero-facts", Byline(*last, facts));
					ShowStatus("hero-status", facts);
					ShowIcon("hero-card", "hero-cover", GameIcon(*last), kHeroLeft, kHeroTop, kHeroSize);
				}
				else
				{
					SetText(m_document, "hero-kicker", m_scanning ? "LOOKING FOR GAMES" : "WELCOME");
					SetText(m_document, "hero-title", m_games.empty() ? (m_scanning ? "Your games" : "No games yet") : "Pick a game");
					SetText(m_document, "hero-facts", m_games.empty() ? (m_scanning ? "" : "Put them in the game files folder (Settings)") :
															  Plural((int)m_games.size(), "game", "games"));
					SetClass(m_document, "hero-status", "unused", true);
					ShowIcon("hero-card", "hero-cover", "", kHeroLeft, kHeroTop, kHeroSize);
				}
				const auto actions = HeroActions();
				if (m_homeRow == kHeroRow)
					m_homeColumn = std::clamp(m_homeColumn, 0, std::max(0, (int)actions.size() - 1));
				for (int i = 0; i < 3; i++)
				{
					const bool used = i < (int)actions.size();
					SetClass(m_document, fmt::format("hero-{}", i), "unused", !used);
					SetClass(m_document, fmt::format("hero-{}", i), "focused", used && !m_onTabs && m_homeRow == kHeroRow && m_homeColumn == i);
					if (used)
						SetText(m_document, fmt::format("hero-label-{}", i), actions[i].second);
				}
				// the play glyph on the first button only when it plays
				if (Rml::Element* glyph = m_document->GetElementById("hero-0"))
					if (Rml::Element* image = glyph->GetFirstChild(); image && image->GetTagName() == "img")
						image->SetProperty("display", actions.front().first == HeroAction::Play ? "inline-block" : "none");
				SetClass(m_document, "hero-0", "primary", true);

				// the shelf
				if (m_homeRow == kShelfRow)
					m_homeColumn = std::clamp(m_homeColumn, 0, ShelfCount() - 1);
				for (int i = 0; i < 5; i++)
				{
					const ps5emu::Game* game = i < (int)m_recent.size() ? &m_games[m_recent[i]] : nullptr;
					ShowIconTile("shelf", i, game, kShelfSize);
					SetClass(m_document, fmt::format("shelf-{}", i), "focused", !m_onTabs && m_homeRow == kShelfRow && m_homeColumn == i);
				}
				SetClass(m_document, "shelf-all", "focused", !m_onTabs && m_homeRow == kShelfRow && m_homeColumn == (int)m_recent.size());
				SetText(m_document, "shelf-label", m_recent.empty() ? "Your library" : "Recent");
				if (!m_onTabs && m_homeRow == kShelfRow && m_homeColumn < (int)m_recent.size())
				{
					const auto& game = m_games[m_recent[m_homeColumn]];
					SetText(m_document, "shelf-name", game.name);
					SetText(m_document, "shelf-meta", Byline(game, FactsOf(game)));
				}
				else
				{
					SetText(m_document, "shelf-name", !m_onTabs && m_homeRow == kShelfRow ? "All games" : "");
					SetText(m_document, "shelf-meta", !m_onTabs && m_homeRow == kShelfRow ? Plural((int)m_games.size(), "game", "games") : "");
				}
				ReleaseArt();

				if (m_onTabs)
					SetHints(m_document, {{"leftright", "Tabs"}, {"cross", "Open"}, {"circle", "Change emulator"}});
				else if (m_homeRow == kHeroRow && actions[m_homeColumn].first == HeroAction::Play)
					SetHints(m_document, {{"cross", "Play"}, {"square", "Details"}, {"circle", "Change emulator"}});
				else
					SetHints(m_document, {{"cross", "Choose"}, {"circle", "Change emulator"}});
			}

			void HomeKey(Key key)
			{
				if (TabsKey(key))
					return;
				const auto actions = HeroActions();
				const bool shelf = Notice().empty() && CoreReady();
				switch (key)
				{
				case Key::Circle: m_leaving = true; return;
				case Key::Up:
					if (m_homeRow == kShelfRow)
					{
						m_homeRow = kHeroRow;
						m_homeColumn = 0;
					}
					else
					{
						FocusTabs();
						return;
					}
					break;
				case Key::Down:
					if (m_homeRow == kHeroRow && shelf)
					{
						m_homeRow = kShelfRow;
						m_homeColumn = 0;
					}
					break;
				case Key::Left:
				case Key::Right:
				{
					const int count = m_homeRow == kHeroRow ? (int)actions.size() : ShelfCount();
					m_homeColumn = (m_homeColumn + (key == Key::Right ? 1 : count - 1)) % count;
					break;
				}
				case Key::Square:
					if (m_lastIndex >= 0 && m_homeRow == kHeroRow)
					{
						OpenDetails(m_lastIndex, kHome);
						return;
					}
					if (m_homeRow == kShelfRow && m_homeColumn < (int)m_recent.size())
					{
						OpenDetails(m_recent[m_homeColumn], kHome);
						return;
					}
					break;
				case Key::Cross:
					if (m_homeRow == kHeroRow)
					{
						switch (actions[m_homeColumn].first)
						{
						case HeroAction::Play: Launch(m_lastIndex); return;
						case HeroAction::Details: OpenDetails(m_lastIndex, kHome); return;
						case HeroAction::Library: ShowTab(kLibrary); return;
						case HeroAction::Settings: OpenSettings(Notice().empty() ? 0 : CategoryIndex("files")); return;
						case HeroAction::Artic: OpenArtic(); return;
						}
					}
					else if (m_homeColumn < (int)m_recent.size())
					{
						Launch(m_recent[m_homeColumn]);
						return;
					}
					else
					{
						ShowTab(kLibrary);
						return;
					}
					break;
				default: break;
				}
				UpdateHome();
			}

			void Launch(int index)
			{
				if (index < 0 || index >= (int)m_games.size())
					return;
				const auto& game = m_games[index];
				ps5settings::AddRecent(LastGame(), Recent(), game.titleId);
				ps5settings::Save(m_settings);
				SetText(m_document, "loading-title", game.name);
				SetText(m_document, "loading-caption", "Starting");
				const std::string icon = GameIcon(game).empty() ? Icon() : GameIcon(game);
				const float size = IconSize(icon, 384);
				FitImage(m_document, "loading-cover", icon, 960 - size / 2, 220 + (384 - size) / 2, size, size);
				ShowScreen(kLoading);
				SetHints(m_document, {});
				m_launch = game;
			}

			// -- library -----------------------------------------------------------------------
			// Every game's box art in a grid that scrolls by rows; the focused one's name below it.

			void LibraryKey(Key key)
			{
				if (TabsKey(key))
					return;
				const int count = (int)m_games.size();
				const int columns = GridColumns();
				if (key == Key::Circle)
				{
					ShowTab(kHome);
					return;
				}
				if (count == 0)
				{
					if (key == Key::Up)
						FocusTabs();
					return;
				}
				int& at = m_librarySelected;
				switch (key)
				{
				case Key::Left: at = std::max(0, at - 1); break;
				case Key::Right: at = std::min(count - 1, at + 1); break;
				case Key::Up:
					if (at < columns)
					{
						FocusTabs();
						return;
					}
					at -= columns;
					break;
				case Key::Down: at = std::min(count - 1, at + columns); break;
				case Key::L2: at = std::max(0, at - columns * GridRows()); break;
				case Key::R2: at = std::min(count - 1, at + columns * GridRows()); break;
				case Key::Cross: Launch(at); return;
				case Key::Square: OpenDetails(at, kLibrary); return;
				case Key::Triangle:
					if (HasPacks())
					{
						OpenPacks(at, kLibrary);
						return;
					}
					break;
				default: break;
				}
				UpdateLibrary();
			}

			void UpdateLibrary()
			{
				const int count = (int)m_games.size();
				const int columns = GridColumns(), rows = GridRows();
				m_librarySelected = std::clamp(m_librarySelected, 0, std::max(0, count - 1));
				// the rows shown: the focused one stays in view, moving them as little as can be
				const int row = m_librarySelected / columns;
				if (row < m_gridTop)
					m_gridTop = row;
				if (row >= m_gridTop + rows)
					m_gridTop = row - rows + 1;
				const int totalRows = (count + columns - 1) / columns;
				m_gridTop = std::clamp(m_gridTop, 0, std::max(0, totalRows - rows));
				for (int slot = 0; slot < columns * rows; slot++)
				{
					const int index = m_gridTop * columns + slot;
					ShowTile("cell", slot, index < count ? &m_games[index] : nullptr, TileWidth(), TileHeight());
					SetClass(m_document, fmt::format("cell-{}", slot), "focused", !m_onTabs && index == m_librarySelected);
				}
				ReleaseArt();
				SetText(m_document, "library-count", m_scanning ? "Looking for games..." : Plural(count, "game", "games"));
				SetText(m_document, "library-empty", m_scanning ? "Looking for games..." : "No games yet. Settings > Game files says where to put them.");
				SetClass(m_document, "library-empty", "visible", count == 0);
				SetClass(m_document, "grid-scroll", "unused", totalRows <= rows);
				if (totalRows > rows)
					if (Rml::Element* thumb = m_document->GetElementById("grid-thumb"))
					{
						const float height = 680.0f * rows / totalRows;
						thumb->SetProperty("height", fmt::format("{}px", std::round(height)));
						thumb->SetProperty("top", fmt::format("{}px", std::round((680.0f - height) * m_gridTop / (totalRows - rows))));
					}
				if (count > 0)
				{
					const auto& game = m_games[m_librarySelected];
					const Facts facts = FactsOf(game);
					SetText(m_document, "lib-name", game.name);
					SetText(m_document, "lib-meta", Byline(game, facts));
					ShowStatus("lib-status", facts);
				}
				else
				{
					SetText(m_document, "lib-name", "");
					SetText(m_document, "lib-meta", "");
					SetClass(m_document, "lib-status", "unused", true);
				}
				if (m_onTabs)
					SetHints(m_document, {{"leftright", "Tabs"}, {"cross", "Open"}, {"circle", "Home"}});
				else if (HasPacks())
					SetHints(m_document, {{"cross", "Play"}, {"square", "Details"}, {"triangle", "Graphic packs"}, {"circle", "Home"}});
				else
					SetHints(m_document, {{"cross", "Play"}, {"square", "Details"}, {"circle", "Home"}});
			}

			// -- details: a game's page --------------------------------------------------------

			enum class DetailsAction
			{
				Play,
				Packs,
			};

			std::vector<std::pair<DetailsAction, std::string>> DetailsActions() const
			{
				std::vector<std::pair<DetailsAction, std::string>> actions{{DetailsAction::Play, "Play"}};
				if (HasPacks() && m_detailsGame < (int)m_games.size())
				{
					const int enabled = ps5emu::EnabledGraphicPackCount(m_games[m_detailsGame].titleId);
					const int count = (int)ps5emu::ListGraphicPacks(m_games[m_detailsGame].titleId).size();
					if (count > 0)
						actions.push_back({DetailsAction::Packs, enabled ? fmt::format("Graphic packs  /  {} on", enabled) : "Graphic packs"});
				}
				return actions;
			}

			void OpenDetails(int index, Screen from)
			{
				if (index < 0 || index >= (int)m_games.size())
					return;
				m_detailsGame = index;
				m_detailsFrom = from;
				m_detailsAction = 0;
				m_synopsisTop = 0;
				ShowScreen(kDetails);
				UpdateDetails();
			}

			void DetailsKey(Key key)
			{
				const auto actions = DetailsActions();
				switch (key)
				{
				case Key::Circle:
					if (m_detailsFrom == kLibrary)
						m_librarySelected = m_detailsGame;
					ShowTab(m_detailsFrom == kLibrary ? kLibrary : kHome);
					return;
				case Key::Left:
				case Key::Right:
					m_detailsAction = (m_detailsAction + (key == Key::Right ? 1 : (int)actions.size() - 1)) % (int)actions.size();
					break;
				case Key::Up:
				case Key::Down:
					// the description scrolls a few lines at a time
					m_synopsisTop = std::max(0, m_synopsisTop + (key == Key::Down ? 3 : -3));
					break;
				case Key::L1:
				case Key::R1:
					// the game before or after, in the library's order
					m_detailsGame = (m_detailsGame + (key == Key::R1 ? 1 : (int)m_games.size() - 1)) % (int)m_games.size();
					m_detailsAction = 0;
					m_synopsisTop = 0;
					break;
				case Key::Cross:
					if (actions[m_detailsAction].first == DetailsAction::Play)
						Launch(m_detailsGame);
					else
						OpenPacks(m_detailsGame, kDetails);
					return;
				case Key::Triangle:
					if (HasPacks())
					{
						OpenPacks(m_detailsGame, kDetails);
						return;
					}
					break;
				default: break;
				}
				UpdateDetails();
			}

			void UpdateDetails()
			{
				if (m_detailsGame < 0 || m_detailsGame >= (int)m_games.size())
					return;
				const auto& game = m_games[m_detailsGame];
				const Facts facts = FactsOf(game);
				bool boxArt = false;
				const std::string art = ArtOf(game, boxArt);
				ShowCover("details-card", "details-cover", art, boxArt, 120, 136, 560, Is3ds() ? 520 : 740);
				SetText(m_document, "details-cover-text", "");
				m_shownArt.insert(art);
				// "WII U  /  ALZE01  /  NTSC-U"
				SetText(m_document, "details-kicker", Join({Is3ds() ? "NINTENDO 3DS" : "WII U", game.gameId, facts.known ? facts.info.region : ""}));
				SetText(m_document, "details-title", game.name);
				// what is known for sure: the status, the version, DLC, the format
				std::vector<std::pair<std::string, const char*>> chips;
				if (facts.report)
					chips.push_back({facts.report->status, ps5compat::Kind(facts.report->status)});
				if (!Is3ds())
				{
					chips.push_back({game.hasUpdate ? fmt::format("Update v{}", game.version) : fmt::format("v{}", game.version), ""});
					if (game.dlcCount)
						chips.push_back({"DLC", ""});
				}
				else if (!game.publisher.empty() && !facts.known)
					chips.push_back({game.publisher, ""});
				chips.push_back({game.format, ""});
				for (int i = 0; i < 4; i++)
				{
					const std::string id = fmt::format("chip-{}", i);
					const bool used = i < (int)chips.size() && !chips[i].first.empty();
					SetClass(m_document, id, "unused", !used);
					SetText(m_document, id, used ? chips[i].first : "");
					for (const char* kind : {"good", "warn", "bad"})
						SetClass(m_document, id, kind, used && std::strcmp(chips[i].second, kind) == 0);
				}
				// GameTDB's facts, those it has, in a grid
				std::vector<std::pair<const char*, std::string>> lines;
				if (facts.known)
				{
					lines = {{"Developer", facts.info.developer}, {"Publisher", facts.info.publisher},
						{"Released", ps5gameinfo::ReleaseDate(facts.info.released)}, {"Genre", ps5gameinfo::Genres(facts.info.genre, 2)},
						{"Players", facts.info.players > 0 ? std::to_string(facts.info.players) : ""}, {"Rating", facts.info.rating}};
					std::erase_if(lines, [](const auto& line) { return line.second.empty(); });
				}
				if (lines.empty())
					lines.push_back({"Title ID", Hex(game.titleId)});
				for (int i = 0; i < 6; i++)
				{
					const bool used = i < (int)lines.size();
					SetClass(m_document, fmt::format("fact-{}", i), "unused", !used);
					SetText(m_document, fmt::format("fact-label-{}", i), used ? lines[i].first : "");
					SetText(m_document, fmt::format("fact-value-{}", i), used ? lines[i].second : "");
				}
				// GameTDB's description alone
				std::string synopsis = facts.known ? facts.info.synopsis : std::string();
				if (synopsis.empty())
					synopsis = game.gameId.empty() ? "GameTDB knows this game by the ID on its box, which this dump does not have." :
													 "GameTDB has no description of this game yet.";
				SetParagraphs(m_document, "synopsis", synopsis);
				if (Rml::Element* text = m_document->GetElementById("synopsis"))
				{
					// scrolled by Up and Down, a line of 36 pixels at a time, no further than its end
					const int lines = (int)std::ceil(text->GetOffsetHeight() / 36.0f);
					m_synopsisTop = std::clamp(m_synopsisTop, 0, std::max(0, lines - 7));
					text->SetProperty("margin-top", fmt::format("{}px", -36 * m_synopsisTop));
				}
				const auto actions = DetailsActions();
				m_detailsAction = std::clamp(m_detailsAction, 0, (int)actions.size() - 1);
				for (int i = 0; i < 4; i++)
				{
					const bool used = i < (int)actions.size();
					SetClass(m_document, fmt::format("action-{}", i), "unused", !used);
					SetClass(m_document, fmt::format("action-{}", i), "focused", used && i == m_detailsAction);
					SetText(m_document, fmt::format("action-label-{}", i), used ? actions[i].second : "");
				}
				std::vector<Hint> hints = {{"cross", actions[m_detailsAction].first == DetailsAction::Play ? "Play" : "Open"}};
				if (m_games.size() > 1)
					hints.push_back({"l1", "Other games"});
				hints.push_back({"updown", "Scroll"});
				hints.push_back({"circle", "Back"});
				SetHints(m_document, hints);
			}

			// -- graphic packs -----------------------------------------------------------------

			// A row of the packs' list: a pack (its index in ListGraphicPacks), or the heading of a
			// folder of them, as Cemu's window shows them in a tree.
			struct PackItem
			{
				int pack = -1;
				std::string heading;
			};

			uint64_t PacksTitle() const { return m_games[m_packsGame].titleId; }

			// The packs as they are now; the ones in no folder first, then each folder's.
			void RefreshPacks()
			{
				m_packs = ps5emu::ListGraphicPacks(PacksTitle());
				const int selectedPack = m_packItem < (int)m_packItems.size() ? m_packItems[m_packItem].pack : -1;
				m_packItems.clear();
				std::vector<std::string> folders;
				for (int i = 0; i < (int)m_packs.size(); i++)
				{
					if (m_packs[i].folder.empty())
						m_packItems.push_back({i, {}});
					else if (std::find(folders.begin(), folders.end(), m_packs[i].folder) == folders.end())
						folders.push_back(m_packs[i].folder);
				}
				for (const auto& folder : folders)
				{
					m_packItems.push_back({-1, folder});
					for (int i = 0; i < (int)m_packs.size(); i++)
						if (m_packs[i].folder == folder)
							m_packItems.push_back({i, {}});
				}
				m_packItem = 0;
				for (int i = 0; i < (int)m_packItems.size() && selectedPack >= 0; i++)
					if (m_packItems[i].pack == selectedPack)
						m_packItem = i;
				SettlePackItem(1);
			}

			// Off a heading, onto the next pack in the direction given (or the other way at the end).
			void SettlePackItem(int direction)
			{
				const int count = (int)m_packItems.size();
				for (int tries = 0; tries < count && m_packItems[m_packItem].pack < 0; tries++)
				{
					const int next = m_packItem + direction;
					if (next < 0 || next >= count)
						direction = -direction;
					else
						m_packItem = next;
				}
			}

			const ps5emu::GraphicPackInfo* SelectedPack() const
			{
				if (m_packItem >= (int)m_packItems.size() || m_packItems[m_packItem].pack < 0)
					return nullptr;
				return &m_packs[m_packItems[m_packItem].pack];
			}

			void OpenPacks(int gameIndex, Screen from)
			{
				if (gameIndex < 0 || gameIndex >= (int)m_games.size())
					return;
				m_packsGame = gameIndex;
				m_packsFrom = from;
				m_packItem = 0;
				m_packItems.clear();
				m_presetsFocus = false;
				m_presetSelected = 0;
				ShowScreen(kPacks);
				SetText(m_document, "packs-game", m_games[gameIndex].name);
				RefreshPacks();
				UpdatePacks();
			}

			void ChoosePreset(int choiceIndex, int presetIndex)
			{
				const auto* pack = SelectedPack();
				if (!pack || choiceIndex >= (int)pack->choices.size())
					return;
				const auto& choice = pack->choices[choiceIndex];
				if (presetIndex < 0 || presetIndex >= (int)choice.presets.size())
					return;
				ps5emu::SetGraphicPackPreset(PacksTitle(), m_packItems[m_packItem].pack, choice.category, choice.presets[presetIndex]);
				RefreshPacks();
				m_presetSelected = std::clamp(m_presetSelected, 0, std::max(0, (int)(SelectedPack() ? SelectedPack()->choices.size() : 1) - 1));
			}

			void ClosePacks()
			{
				if (m_packsFrom == kDetails)
				{
					ShowScreen(kDetails);
					UpdateDetails();
				}
				else if (m_packsFrom == kLibrary)
				{
					m_librarySelected = m_packsGame;
					ShowTab(kLibrary);
				}
				else
					ShowTab(kHome);
			}

			void PacksKey(Key key)
			{
				if (key == Key::Circle)
				{
					if (m_presetsFocus)
						m_presetsFocus = false;
					else
					{
						ClosePacks();
						return;
					}
				}
				const auto* pack = SelectedPack();
				if (!pack)
				{
					UpdatePacks();
					return;
				}
				if (!m_presetsFocus)
				{
					const int count = (int)m_packItems.size();
					if (key == Key::Up || key == Key::Down || key == Key::L1 || key == Key::R1)
					{
						Browse(key, m_packItem, count, kListRows);
						SettlePackItem(key == Key::Up || key == Key::L1 ? -1 : 1);
					}
					else if (key == Key::Cross)
					{
						ps5emu::ToggleGraphicPack(PacksTitle(), m_packItems[m_packItem].pack);
						RefreshPacks();
					}
					else if ((key == Key::Right || key == Key::Square) && !pack->choices.empty())
					{
						m_presetsFocus = true;
						m_presetSelected = 0;
					}
				}
				else if (pack->choices.empty())
					m_presetsFocus = false;
				else if (!Browse(key, m_presetSelected, (int)pack->choices.size(), kPresetRows))
				{
					const auto& choice = pack->choices[std::min(m_presetSelected, (int)pack->choices.size() - 1)];
					if (key == Key::Cross)
					{
						const int choiceIndex = m_presetSelected;
						OpenPicker(choice.category.empty() ? "Preset" : choice.category, pack->name, choice.presets, choice.active,
							[this, choiceIndex](int preset) {
								ChoosePreset(choiceIndex, preset);
								UpdatePacks();
							});
						return; // the dropdown's hints stay
					}
					else if (key == Key::Left || key == Key::Right)
					{
						const int presets = (int)choice.presets.size();
						ChoosePreset(m_presetSelected, (choice.active + (key == Key::Right ? 1 : presets - 1)) % presets);
					}
				}
				UpdatePacks();
			}

			void UpdatePacks()
			{
				const int count = (int)m_packItems.size();
				const int scroll = Scroll(m_packItem, kListRows);
				int packNumber = 0, packCount = 0;
				for (int i = 0; i < count; i++)
					if (m_packItems[i].pack >= 0)
					{
						packCount++;
						if (i <= m_packItem)
							packNumber++;
					}
				for (int row = 0; row < kListRows; row++)
				{
					const int index = scroll + row;
					const std::string id = fmt::format("pack-row-{}", row);
					const bool present = index < count;
					const bool heading = present && m_packItems[index].pack < 0;
					SetClass(m_document, id, "focused", index == m_packItem && !m_presetsFocus);
					SetClass(m_document, id, "chosen", index == m_packItem && m_presetsFocus);
					SetClass(m_document, id, "offscreen", !present);
					SetClass(m_document, id, "heading", heading);
					const auto* pack = present && !heading ? &m_packs[m_packItems[index].pack] : nullptr;
					SetText(m_document, fmt::format("pack-name-{}", row), heading ? Upper(m_packItems[index].heading) : pack ? pack->name : "");
					SetText(m_document, fmt::format("pack-state-{}", row), pack ? (pack->enabled ? "On" : "Off") : "");
					SetClass(m_document, fmt::format("pack-state-{}", row), "on", pack && pack->enabled);
				}
				SetClass(m_document, "packs-empty", "visible", packCount == 0);
				SetText(m_document, "packs-position", packCount ? fmt::format("{} of {}", packNumber, packCount) : "");
				if (m_presetsFocus)
					SetHints(m_document, {{"cross", "Choose"}, {"leftright", "Change"}, {"circle", "Back to the packs"}});
				else
					SetHints(m_document, {{"cross", "On / off"}, {"leftright", "Presets"}, {"circle", "Back"}});

				const auto* pack = SelectedPack();
				if (!pack)
				{
					for (const char* id : {"pack-detail-title", "pack-detail-description", "pack-detail-kicker", "presets-kicker"})
						SetText(m_document, id, "");
					SetClass(m_document, "presets-empty", "visible", false);
					for (int row = 0; row < kPresetRows; row++)
						SetClass(m_document, fmt::format("preset-row-{}", row), "offscreen", true);
					return;
				}
				SetText(m_document, "pack-detail-kicker", pack->folder.empty() ? "GRAPHIC PACK" : "GRAPHIC PACK  /  " + Upper(pack->folder));
				SetText(m_document, "pack-detail-title", pack->name);
				SetLines(m_document, "pack-detail-description", pack->description.empty() ? "This pack has no description." : pack->description);
				const int choices = (int)pack->choices.size();
				SetText(m_document, "presets-kicker", choices == 0 ? "PRESETS" : pack->enabled ? "PRESETS" : "PRESETS  /  CHOOSING ONE TURNS IT ON");
				SetClass(m_document, "presets-empty", "visible", choices == 0);
				m_presetSelected = std::clamp(m_presetSelected, 0, std::max(0, choices - 1));
				const int presetScroll = Scroll(m_presetSelected, kPresetRows);
				for (int row = 0; row < kPresetRows; row++)
				{
					const int index = presetScroll + row;
					const std::string id = fmt::format("preset-row-{}", row);
					SetClass(m_document, id, "offscreen", index >= choices);
					SetClass(m_document, id, "focused", m_presetsFocus && index == m_presetSelected);
					if (index >= choices)
						continue;
					const auto& choice = pack->choices[index];
					SetText(m_document, fmt::format("preset-label-{}", row), choice.category.empty() ? "Preset" : choice.category);
					SetText(m_document, fmt::format("preset-value-{}", row), choice.presets.empty() ? "" : choice.presets[std::clamp(choice.active, 0, (int)choice.presets.size() - 1)]);
				}
				SetText(m_document, "presets-position", choices > kPresetRows ? fmt::format("{} of {}", m_presetSelected + 1, choices) : "");
			}

			// -- settings ----------------------------------------------------------------------
			// The categories down the left; the focused one's settings on the right, each with a line
			// saying what it does, and its longer help behind Triangle.

			struct SettingRow
			{
				std::string id;
				std::string label, value;
				std::string description; // one line, under it while it has the focus
				std::string help;		 // Triangle's
				bool dimmed = false;
			};

			int CategoryIndex(const char* name) const
			{
				for (int i = 0; i < CategoryCount(); i++)
					if (Category(i) == name)
						return i;
				return 0;
			}

			void OpenSettings(int category)
			{
				m_category = std::clamp(category, 0, CategoryCount() - 1);
				m_onRail = true;
				m_settingRow = 0;
				m_confirmClear = m_resetArmed = false;
				m_onTabs = false;
				ShowTab(kSettings);
			}

			std::vector<SettingRow> SettingRows(const std::string& category)
			{
				std::vector<SettingRow> rows;
				auto& n3ds = m_settings.n3ds;
				const auto onOff = [](bool on) { return std::string(on ? "On" : "Off"); };
				if (category == "video" && Is3ds())
					rows = {
						{"resolution", "Internal resolution", kResolutions[std::clamp(n3ds.resolution, 1, 10)], "Higher is sharper and asks more of the GPU.",
							"How large the 3DS's 3D scenes are drawn before they are scaled to the TV. Higher is sharper and asks more of the "
							"GPU, and each time a game reads a picture back the wait grows with it."},
						{"layout", "Screen layout", kLayouts[std::clamp(n3ds.layout, 0, 3)], "How the two screens share the TV.",
							"One above the other, the top one alone, the top one large with the bottom one beside it, or the two side by "
							"side. In a game, touchpad click + R1 goes to the next."},
						{"filter", "Texture filter", kTextureFilters[std::clamp(n3ds.textureFilter, 0, 5)], "Smooths textures as they are scaled up.",
							"Smooths the game's textures as they are scaled up; None keeps them as the 3DS draws them. A filter redraws every "
							"texture at the internal resolution: at high resolutions it is the costliest setting. If a game stutters, try None first."},
						{"customtextures", "Custom textures", onOff(n3ds.customTextures), "Texture packs from azahar/load/textures.",
							"Texture packs in /data/ps5cemu/azahar/load/textures/<title ID>, as the desktop Azahar loads them."},
					};
				else if (category == "video")
					rows = {
						{"upscaling", "Upscaling to 4K", kUpscaleFilters[std::clamp(m_settings.upscaleFilter, 0, 3)], "How the picture is scaled to the TV.",
							"Bicubic is sharp, Bicubic Hermite a little softer, Linear softer still; Nearest neighbour keeps pixels square."},
						{"highframerate", "120 Hz output", m_settings.highFrameRate ? "On, where the TV has it" : "Off", "For displays that take 120 Hz.",
							"The 119.88 Hz mode, on displays that support it. Games still run at their own speed, and a frame that misses a "
							"refresh waits 8 ms for the next instead of 17."},
						{"framepacing", "Frame pacing", ps5display::FramePacingName(m_settings.framePacing, m_settings.highFrameRate),
							"Holds a game to an even frame rate.",
							"Each frame stays on screen for at least two or three refreshes, so a game that cannot hold the display's rate "
							"runs at an even one: 60 or 40 fps with 120 Hz output, 30 or 20 without. 60 fps with 120 Hz output suits a game "
							"at 4K that sometimes drops under 60; 30 fps at 60 Hz suits one at 8K. Also in the in-game menu."},
						{"overlay", "Performance overlay", onOff(m_settings.overlay), "Frame rate, CPU and memory in a corner.",
							"Frames per second, CPU and memory use in the top left corner, as Cemu shows them. Also in the in-game menu."},
						{"async", "Async shader compile", onOff(m_settings.asyncShaders), "No stutter while new shaders build.",
							"On, a new shader is built while the game carries on, so it does not stutter, but some things may be missing for a "
							"moment. Off, the game waits for each one: stutter, but nothing drawn wrong."},
					};
				else if (category == "audio")
				{
					rows = {
						{"volume", "Game volume", fmt::format("{}%", Volume()), "The games' sound.", "The games' sound. Left and Right change it by 10%."},
						{"music", "Launcher music", m_settings.music == "setup" ? "Setup theme" : "Off",
							"The music under the menus.", "The launcher's own music, in the spirit of a console's setup screen, or none."},
						{"musicvolume", "Music volume", fmt::format("{}%", m_settings.musicVolume), "How loud the menus' music is.",
							"How loud the launcher's music is. Left and Right change it by 10%."},
						{"menusounds", "Menu sounds", onOff(m_settings.menuSounds), "The sounds of moving and choosing.",
							"The launcher's sounds as you move, choose and go back."},
					};
					if (!Is3ds())
						rows.push_back({"gamepadspeaker", "GamePad speaker", m_settings.gamePadSpeaker ? "DualSense speaker" : "Off",
							"The GamePad's own sound, on the DualSense.",
							"The sounds games play on the Wii U GamePad's speaker, from player 1's DualSense speaker. Many games send their "
							"whole sound there too, so it is off unless you want it. Applies to the next game."});
				}
				else if (category == "controls" && Is3ds())
				{
					const auto mappings = ps5azahar::ListMappings(n3ds);
					const int mapped = (int)std::count_if(mappings.begin(), mappings.end(), [](const ps5emu::ButtonMapping& m) { return !m.input.empty(); });
					rows = {
						{"motion", "Motion controls", onOff(n3ds.motion), "The DualSense's motion as the 3DS's.",
							"The DualSense's gyroscope and accelerometer as the 3DS's own, for the games that aim or steer by tilting it."},
						{"deadzone", "Stick deadzone", fmt::format("{}%", n3ds.deadzone), "How far a stick moves before it counts.",
							"How far a stick moves before the game sees it, for the circle pad and the C-stick. Raise it if something drifts "
							"when you let go of the stick; lower it for finer control."},
						{"buttons", "Buttons", Plural(mapped, "button set", "buttons set"), "Which DualSense button is which.",
							"Which DualSense button is which of the 3DS's. A is on Circle and B on Cross by default, where the 3DS has them; the "
							"circle pad is the left stick, the C-stick the right one, and the touchpad the touch screen."},
						{"reset", "Reset to defaults", m_resetArmed ? "Press Cross again" : "", "Default buttons, motion and deadzone.",
							"The default buttons, motion and deadzone."},
					};
				}
				else if (category == "controls")
					for (int player = 0; player < ps5pad::kMaxPlayers; player++)
					{
						const auto controls = ps5emu::GetPlayerControls(player);
						rows.push_back({fmt::format("player{}", player), fmt::format("Player {}", player + 1), TypeName(controls.type),
							controls.connected ? "Cross: this player's controller, motion and buttons." : "No DualSense for this player yet.",
							"What the game sees in this player's hands, and its motion, vibration, deadzones and buttons. Player 1 is the "
							"signed-in user who started the app; the other signed-in users' DualSenses are players 2 to 4.",
							!controls.connected});
					}
				else if (category == "usb")
					for (ps5usb::Device device : ps5usb::kDevices)
						rows.push_back({fmt::format("usb{}", (int)device), ps5usb::Name(device),
							!m_status.coreReady ? "-" : ps5usb::Enabled(device) ? "On" : "Off",
							device == ps5usb::Device::Skylanders ? "For the Skylanders games." :
							device == ps5usb::Device::Infinity	 ? "For Disney Infinity 3.0." :
																   "For LEGO Dimensions.",
							"Cemu's emulated portal, plugged in as a game starts. In the game, the menu's USB devices category puts "
							"figures on it: dumps in " + ps5usb::Folder(device) + ". A real portal on the PS5's USB is not reached.",
							!m_status.coreReady});
				else if (category == "borders")
					rows = {{"border", "Border", kBorderThemes[std::clamp(n3ds.border, 0, kBorderThemeCount - 1)], "Artwork around the screens.",
						"Artwork around the 3DS screens, never over them. It follows every layout, and the in-game menu changes it too."}};
				else if (category == "system")
				{
					ps5emu::Game home;
					const bool homeMenu = ps5azahar::HomeMenu(n3ds.region, home);
					rows = {
						{"region", "Region", kRegions[std::clamp(n3ds.region, -1, 6) + 1], "The emulated 3DS's region.",
							"Automatic takes each game's own region. A game made for another region may refuse to start or show other "
							"languages. Applies to the next game."},
						{"language", "Language", kLanguages[std::clamp(n3ds.language, -1, 11) + 1], "The emulated 3DS's language.",
							"The language games that follow the console's show their text in. Applies to the next game."},
						{"homemenu", "Home Menu", homeMenu ? "Start" : "Run Artic Setup first", "The 3DS Home Menu, from your console's files.",
							"Starts the 3DS Home Menu, once Artic Base's setup has copied your own console's system files (Home: Artic Base).",
							!homeMenu},
					};
				}
				else if (category == "files")
					rows = {{"gamesfolder", "Game folder", ShortPath(GamesFolder(), 40), "Where your games are. Cross picks another.",
						Is3ds() ? "Your 3DS games: .3ds or .cci, .cxi, .3dsx and Azahar's compressed dumps, decrypted, here or in the folders in "
								  "it. CIA files are installed (Install CIA files). Encrypted dumps need the 3DS's aes_keys.txt in "
								  "/data/ps5cemu/azahar/sysdata." :
								  "Your Wii U games: .wua, .wud, .wux, or folders with code, content and meta. Encrypted .wud and .wux need their "
								  "keys in /data/ps5cemu/keys.txt."}};
				else if (category == "installs")
					rows = {{"install", Is3ds() ? "Install a CIA file" : "Install from a folder", "", Is3ds() ? "A game, an update or DLC." :
						"An update, DLC or game (code, content and meta).",
						Is3ds() ? "Installs a CIA into the 3DS's storage, as Azahar's Install CIA does: an update or DLC goes with its game, and "
								  "a game joins the library. Circle cancels while it runs." :
								  "Installs into the Wii U's storage (mlc01), as Cemu's Install game title, update or DLC does. Updates and DLC "
								  "in the game files folder work as they are, too."}};
				else if (category == "online")
				{
					rows = {{"boxart", "Box art from GameTDB", onOff(m_settings.boxArt), "Covers for the library, downloaded once.",
						"The first time a game shows up, its cover is downloaded from GameTDB (art.gametdb.com) by the ID on its box. "
						"Off: nothing more is downloaded."}};
					if (HasPacks())
						rows.push_back({"packs", "Community graphic packs", PacksStatus(), "Cross checks GitHub for newer ones.",
							"Cemu's community graphic packs, from GitHub's latest release when it is newer than the ones installed. The packs "
							"bundled with the app stay as the fallback."});
					rows.push_back({"appupdate", "PS5CEMU-HAR updates", AppUpdateStatus(), "Cross checks GitHub for a newer version.",
						"PS5CEMU-HAR asks GitHub for its latest release each time it starts. Cross asks again, or installs the newer version "
						"it found: the release's files are downloaded, checked and put in place of these, and the app starts again. Your "
						"games, saves and settings stay as they are."});
				}
				else if (category == "diagnostics")
					rows = {
						{"copylogs", "Copy logs to USB", m_diagnosticsDone[0], "The logs a report needs, onto a USB drive.",
							"Copies the last five sessions' logs and the settings into a dated folder on a USB drive, to attach to a report."},
						{"clearcaches", Is3ds() ? "Clear 3DS shader caches" : "Clear Wii U shader caches", m_diagnosticsDone[1],
							"For a game that crashes on a bad cache.", "Deletes the shader caches (press Cross twice). Games build them again as "
							"they run: the first minutes stutter."},
					};
				return rows;
			}

			// What the panel says under (or instead of) its rows
			std::string PanelText(const std::string& category) const
			{
				if (category == "diagnostics")
				{
					std::string text;
					for (const auto& line : m_status.diagnostics)
						text += line + "\n";
					return text;
				}
				if (category == "usb")
					return "Figure dumps go in /data/ps5cemu/figures: skylanders, infinity and dimensions. Switch a portal on here, "
						   "start the game, then put figures on it from the in-game menu (Touchpad + Options > USB devices).";
				if (category == "about")
					return Is3ds() ?
						"Azahar, the 3DS emulator, by the Azahar team and the Citra contributors before them; Mihawk-99's PS5 port of it "
						"and dynarmic.\nBox art and game information: GameTDB.\nThe launcher's font: Lexend.\nAn unofficial port, not affiliated "
						"with the Azahar team, Nintendo or Sony.\n\nGames: /data/ps5cemu/azahar/games\n3DS storage: /data/ps5cemu/azahar/sdmc\n"
						"Version " + ps5update::Readable(PS5CEMU_VERSION) :
						"Cemu, the Wii U emulator, by the Cemu team and its contributors; RADV on the PS5 by Mihawk-99 and mpereiraesaa; the "
						"community graphic packs' authors.\nBox art and game information: GameTDB.\nThe launcher's font: Lexend.\nAn unofficial "
						"port, not affiliated with the Cemu team, Nintendo or Sony.\n\nGames: /data/ps5cemu/games\nWii U storage and saves: "
						"/data/ps5cemu/mlc01\nKeys: /data/ps5cemu/keys.txt\nVersion " + ps5update::Readable(PS5CEMU_VERSION);
				return {};
			}

			void SettingsKey(Key key)
			{
				if (TabsKey(key))
					return;
				const std::string category = Category(m_category);
				auto rows = SettingRows(category);
				if (m_onRail)
				{
					switch (key)
					{
					case Key::Up:
						if (m_category == 0)
						{
							FocusTabs();
							return;
						}
						m_category--;
						break;
					case Key::Down: m_category = std::min(CategoryCount() - 1, m_category + 1); break;
					case Key::Right:
					case Key::Cross:
						if (!rows.empty())
						{
							m_onRail = false;
							m_settingRow = 0;
						}
						break;
					case Key::Circle: ShowTab(kHome); return;
					default: break;
					}
					m_settingRow = 0;
					m_confirmClear = m_resetArmed = false;
					UpdateSettings();
					return;
				}
				if (rows.empty())
				{
					m_onRail = true;
					UpdateSettings();
					return;
				}
				m_settingRow = std::clamp(m_settingRow, 0, (int)rows.size() - 1);
				const SettingRow& row = rows[m_settingRow];
				switch (key)
				{
				case Key::Circle: m_onRail = true; break;
				case Key::Up:
				case Key::Down:
					Browse(key, m_settingRow, (int)rows.size(), (int)rows.size());
					m_confirmClear = m_resetArmed = false;
					break;
				case Key::Triangle: OpenInfo(row.label, row.help); return;
				case Key::Left:
				case Key::Right:
				case Key::Cross:
					if (ChangeSetting(row.id, key))
						return; // it opened a page
					break;
				default: break;
				}
				UpdateSettings();
			}

			// A setting changed by Left, Right or Cross; true when it opened a page instead
			bool ChangeSetting(const std::string& id, Key key)
			{
				auto& n3ds = m_settings.n3ds;
				const bool cross = key == Key::Cross, slide = key == Key::Left || key == Key::Right;
				const int step = key == Key::Left ? -1 : 1;
				bool changed = true;
				if (id == "resolution")
					n3ds.resolution = (n3ds.resolution - 1 + step + 10) % 10 + 1;
				else if (id == "layout")
					n3ds.layout = (n3ds.layout + step + 4) % 4;
				else if (id == "filter")
					n3ds.textureFilter = (n3ds.textureFilter + step + 6) % 6;
				else if (id == "customtextures")
					n3ds.customTextures = !n3ds.customTextures;
				else if (id == "upscaling")
					m_settings.upscaleFilter = (m_settings.upscaleFilter + step + 4) % 4;
				else if (id == "highframerate")
					m_settings.highFrameRate = !m_settings.highFrameRate;
				else if (id == "framepacing")
					m_settings.framePacing = (m_settings.framePacing - 1 + step + 3) % 3 + 1;
				else if (id == "overlay")
					m_settings.overlay = !m_settings.overlay;
				else if (id == "async")
					m_settings.asyncShaders = !m_settings.asyncShaders;
				else if (id == "volume" && slide)
					Volume() = std::clamp(Volume() + step * 10, 0, 100);
				else if (id == "music")
				{
					const int music = (int)(std::find(std::begin(kMusic), std::end(kMusic), m_settings.music) - std::begin(kMusic));
					m_settings.music = kMusic[(music + step + 2) % 2];
				}
				else if (id == "musicvolume" && slide)
					m_settings.musicVolume = std::clamp(m_settings.musicVolume + step * 10, 0, 100);
				else if (id == "menusounds")
					m_settings.menuSounds = !m_settings.menuSounds;
				else if (id == "gamepadspeaker")
					m_settings.gamePadSpeaker = !m_settings.gamePadSpeaker;
				else if (id == "motion")
					n3ds.motion = !n3ds.motion;
				else if (id == "deadzone")
				{
					int value = n3ds.deadzone + step * 5;
					if (cross && value > 50)
						value = 0;
					n3ds.deadzone = std::clamp(value, 0, 50);
				}
				else if (id == "buttons" && cross)
				{
					OpenMapping();
					return true;
				}
				else if (id == "reset" && cross)
				{
					if (m_resetArmed)
						ps5azahar::ResetControls(n3ds);
					m_resetArmed = !m_resetArmed;
				}
				else if (id.rfind("player", 0) == 0 && cross)
				{
					OpenPlayer(id.back() - '0');
					return true;
				}
				else if (id == "border")
					n3ds.border = (std::clamp(n3ds.border, 0, kBorderThemeCount - 1) + step + kBorderThemeCount) % kBorderThemeCount;
				else if (id == "region")
					n3ds.region = (n3ds.region + 1 + step + 8) % 8 - 1;
				else if (id == "language")
					n3ds.language = (n3ds.language + 1 + step + 13) % 13 - 1;
				else if (id.rfind("usb", 0) == 0 && id.size() == 4 && m_status.coreReady)
				{
					// Cemu's settings.xml keeps it, as its Emulated USB Devices window does
					const auto device = (ps5usb::Device)(id[3] - '0');
					ps5usb::SetEnabled(device, !ps5usb::Enabled(device));
					changed = false; // not the launcher's own settings
				}
				else if (id == "homemenu" && cross)
				{
					// the 3DS Home Menu, from the console's files Artic Setup copied
					ps5emu::Game home;
					if (ps5azahar::HomeMenu(n3ds.region, home))
					{
						SetText(m_document, "loading-title", home.name);
						SetText(m_document, "loading-caption", "Starting the 3DS");
						FitImage(m_document, "loading-cover", Icon(), 760, 220, 400, 400);
						ShowScreen(kLoading);
						SetHints(m_document, {});
						m_launch = home;
						return true;
					}
					changed = false;
				}
				else if (id == "gamesfolder" && cross)
				{
					OpenFiles(FilesMode::GamesFolder);
					return true;
				}
				else if (id == "install" && cross)
				{
					OpenFiles(Is3ds() ? FilesMode::InstallCia : FilesMode::Install);
					return true;
				}
				else if (id == "boxart")
				{
					// box art from GameTDB: off stops what is queued, and nothing more is asked for
					m_settings.boxArt = !m_settings.boxArt;
					ps5boxart::SetEnabled(m_settings.boxArt);
					if (m_settings.boxArt)
						FetchBoxArt();
				}
				else if (id == "appupdate" && cross)
				{
					// installing shows over the screen (UpdatePrompt); otherwise GitHub is asked again
					const auto state = ps5update::GetStatus().state;
					if (state == ps5update::Status::State::Available)
						ps5update::Install();
					else if (state == ps5update::Status::State::Ready)
						ps5update::Restart();
					else
						ps5update::Check();
					changed = false;
				}
				else if (id == "packs" && cross)
				{
					ps5packs::Start(); // GitHub's latest community packs, when newer
					changed = false;
				}
				else if (id == "copylogs" && cross)
				{
					m_diagnosticsDone[0] = CopyLogsToUsb();
					changed = false;
				}
				else if (id == "clearcaches" && cross)
				{
					// deleting caches takes a second press: they take long to build again
					m_diagnosticsDone[1] = m_confirmClear ? ClearShaderCaches(Is3ds()) : "Press Cross again to delete";
					m_confirmClear = !m_confirmClear;
					changed = false;
				}
				else
					changed = false;
				if (changed)
				{
					SaveSettings();
					ps5sound::SetMusic(m_settings.music, m_settings.musicVolume);
					ps5sound::SetMenuSounds(m_settings.menuSounds);
				}
				return false;
			}

			void UpdateSettings()
			{
				const std::string category = Category(m_category);
				const auto rows = SettingRows(category);
				for (int i = 0; i < CategoryCount(); i++)
				{
					SetClass(m_document, fmt::format("rail-{}", i), "selected", i == m_category);
					SetClass(m_document, fmt::format("rail-{}", i), "focused", !m_onTabs && m_onRail && i == m_category);
				}
				static const std::map<std::string, std::string> kTitles = {{"video", "Video"}, {"audio", "Audio"}, {"controls", "Controls"},
					{"usb", "USB devices"},
					{"borders", "Borders"}, {"system", "System"}, {"files", "Game files"}, {"installs", "Installs"}, {"online", "Online and updates"},
					{"diagnostics", "Diagnostics"}, {"about", "About"}};
				SetText(m_document, "panel-title", kTitles.count(category) ? kTitles.at(category) : category);
				m_settingRow = std::clamp(m_settingRow, 0, std::max(0, (int)rows.size() - 1));
				for (int i = 0; i < kSettingRows; i++)
				{
					const bool used = i < (int)rows.size();
					const std::string id = fmt::format("srow-{}", i);
					SetClass(m_document, id, "unused", !used);
					SetClass(m_document, id, "focused", used && !m_onTabs && !m_onRail && i == m_settingRow);
					SetClass(m_document, id, "dimmed", used && rows[i].dimmed);
					SetText(m_document, fmt::format("srow-label-{}", i), used ? rows[i].label : "");
					SetText(m_document, fmt::format("srow-value-{}", i), used ? rows[i].value : "");
					SetText(m_document, fmt::format("srow-desc-{}", i), used ? rows[i].description : "");
				}
				// the panel's text, under the rows
				SetLines(m_document, "panel-text", PanelText(category));
				if (Rml::Element* text = m_document->GetElementById("panel-text"))
					text->SetProperty("top", fmt::format("{}px", 214 + (int)rows.size() * 96 + (rows.empty() ? 0 : 50)));
				if (m_onTabs)
					SetHints(m_document, {{"leftright", "Tabs"}, {"cross", "Open"}, {"circle", "Home"}});
				else if (m_onRail)
					SetHints(m_document, {{"updown", "Categories"}, {"cross", "Open"}, {"circle", "Home"}});
				else
					SetHints(m_document, {{"leftright", "Change"}, {"cross", "Choose"}, {"triangle", "More"}, {"circle", "Back"}});
			}

			// The community graphic packs' update, as its row shows it
			std::string PacksStatus() const
			{
				using State = ps5packs::Status::State;
				const auto status = ps5packs::GetStatus();
				const std::string installed = ps5packs::InstalledVersion();
				switch (status.state)
				{
				case State::Checking: return "Checking GitHub...";
				case State::Downloading:
					return status.total ? fmt::format("Downloading: {}%", (int)(status.received * 100 / status.total)) :
										  fmt::format("Downloading: {} MB", status.received >> 20);
				case State::Installing: return "Installing...";
				case State::UpToDate: return "Up to date: " + installed;
				case State::Done: return "Installed: " + status.version;
				case State::Failed: return status.message;
				case State::Idle: break;
				}
				return installed.empty() ? "Cross to download" : installed;
			}

			// The app's own update, as its row shows it
			static std::string AppUpdateStatus()
			{
				using State = ps5update::Status::State;
				const auto status = ps5update::GetStatus();
				switch (status.state)
				{
				case State::Checking: return "Checking GitHub...";
				case State::UpToDate: return "Up to date: " + ps5update::Readable(PS5CEMU_VERSION);
				case State::Available: return ps5update::Readable(status.latest) + ": Cross to install";
				case State::Downloading:
					return status.total ? fmt::format("Downloading: {}%", (int)(status.received * 100 / status.total)) : "Downloading...";
				case State::Verifying: return "Checking the download...";
				case State::Installing: return "Installing...";
				case State::Ready: return "Installed: Cross to restart";
				case State::Failed: return status.message;
				case State::Idle: break;
				}
				return "Cross to check";
			}

			// The update's row while it runs; Cemu's packs read again once newer ones are in
			void PollPacks()
			{
				const auto state = ps5packs::GetStatus().state;
				const std::string shown = PacksStatus() + "\n" + AppUpdateStatus();
				if (shown != m_packsShown)
				{
					m_packsShown = shown;
					if (m_screen == kSettings)
						UpdateSettings();
				}
				if (state == ps5packs::Status::State::Done && !m_packsReloaded && m_status.coreReady)
				{
					m_packsReloaded = true;
					ps5emu::ReloadGraphicPacks();
				}
			}

			// -- settings > controls > a player (the Wii U's) -----------------------------------------

			void OpenPlayer(int player)
			{
				m_player = std::clamp(player, 0, ps5pad::kMaxPlayers - 1);
				m_playerRow = 0;
				m_resetArmed = false;
				ShowScreen(kPlayer);
				SetText(m_document, "player-title", fmt::format("Player {}", m_player + 1));
				UpdatePlayer();
			}

			// The emulated controllers a player can have: Cemu has two GamePads at most.
			std::vector<ps5emu::EmulatedType> TypesFor(int player) const
			{
				int otherGamePads = 0;
				for (int other = 0; other < ps5pad::kMaxPlayers; other++)
					if (other != player && ps5emu::GetPlayerControls(other).type == ps5emu::EmulatedType::GamePad)
						otherGamePads++;
				std::vector<ps5emu::EmulatedType> types;
				if (otherGamePads < 2)
					types.push_back(ps5emu::EmulatedType::GamePad);
				for (auto type : {ps5emu::EmulatedType::Pro, ps5emu::EmulatedType::Classic, ps5emu::EmulatedType::Wiimote, ps5emu::EmulatedType::Nunchuk})
					types.push_back(type);
				return types;
			}

			void PlayerKey(Key key)
			{
				if (key == Key::Circle)
				{
					OpenSettings(CategoryIndex("controls"));
					m_onRail = false;
					m_settingRow = m_player;
					UpdateSettings();
					return;
				}
				if (key == Key::Up || key == Key::Down)
				{
					Browse(key, m_playerRow, kPlayerRows, kPlayerRows);
					m_resetArmed = false;
					UpdatePlayer();
					return;
				}
				const auto controls = ps5emu::GetPlayerControls(m_player);
				const bool left = key == Key::Left, right = key == Key::Right, cross = key == Key::Cross;
				if (!left && !right && !cross)
					return;
				switch (m_playerRow)
				{
				case kRowType:
				{
					const auto types = TypesFor(m_player);
					int active = -1;
					for (int i = 0; i < (int)types.size(); i++)
						if (types[i] == controls.type)
							active = i;
					if (cross)
					{
						std::vector<std::string> names;
						for (auto type : types)
							names.push_back(TypeName(type));
						OpenPicker("Emulated controller", fmt::format("Player {}", m_player + 1), names, active, [this, types](int index) {
							ps5emu::SetEmulatedType(m_player, types[index]);
							UpdatePlayer();
						});
						return;
					}
					const int count = (int)types.size();
					ps5emu::SetEmulatedType(m_player, types[((active < 0 ? 0 : active) + (right ? 1 : count - 1)) % count]);
					break;
				}
				case kRowMotion:
					if (controls.hasMotion)
						ps5emu::SetMotion(m_player, !controls.motion);
					break;
				case kRowRumble:
				{
					int rumble = controls.rumble + (left ? -10 : 10);
					if (cross && rumble > 100)
						rumble = 0;
					rumble = std::clamp(rumble, 0, 100);
					ps5emu::SetRumble(m_player, rumble);
					if (rumble > 0 && !m_settings.rumble)
					{
						// the launcher's own switch would keep the motors still
						m_settings.rumble = true;
						ps5pad::SetVibrationEnabled(true);
						SaveSettings();
					}
					break;
				}
				case kRowLeftDeadzone:
				case kRowRightDeadzone:
				{
					const bool leftStick = m_playerRow == kRowLeftDeadzone;
					int value = (leftStick ? controls.leftDeadzone : controls.rightDeadzone) + (left ? -5 : 5);
					if (cross && value > 50)
						value = 0;
					value = std::clamp(value, 0, 50);
					ps5emu::SetDeadzones(m_player, leftStick ? value : controls.leftDeadzone, leftStick ? controls.rightDeadzone : value);
					break;
				}
				case kRowButtons:
					if (cross)
					{
						OpenMapping();
						return;
					}
					break;
				case kRowReset:
					if (cross)
					{
						if (m_resetArmed)
						{
							ps5emu::ResetControls(m_player);
							m_resetArmed = false;
						}
						else
							m_resetArmed = true;
					}
					break;
				}
				UpdatePlayer();
			}

			void UpdatePlayer()
			{
				const auto controls = ps5emu::GetPlayerControls(m_player);
				const auto mappings = ps5emu::ListMappings(m_player);
				const int mapped = (int)std::count_if(mappings.begin(), mappings.end(), [](const ps5emu::ButtonMapping& m) { return !m.input.empty(); });
				SetText(m_document, "player-copy", fmt::format("{}, on {}", TypeName(controls.type),
					controls.connected ? "this player's DualSense" : "a DualSense that is not connected"));
				struct Row
				{
					const char* name;
					std::string value;
					bool dimmed;
					const char* help;
				};
				const Row rows[kPlayerRows] = {
					{"Emulated controller", TypeName(controls.type), false,
						"What the game sees in this player's hands.\n\nMost games want the Wii U GamePad for player 1: its screen is the "
						"second one PS5CEMU-HAR shows, and the touchpad touches it. Others take Pro Controllers, or Wii Remotes for "
						"games such as New Super Mario Bros. U.\nA Wii Remote points where a finger rests on the touchpad, or where you "
						"aim the DualSense.\nCemu has two GamePads at most."},
					{"Motion controls", !controls.hasMotion ? "None on this one" : controls.motion ? "On" : "Off", !controls.hasMotion,
						"The DualSense's gyroscope and accelerometer as the controller's own, for the games that aim or steer by "
						"tilting the GamePad or the Wii Remote.\nThe Pro Controller and the Classic Controller have none."},
					{"Vibration", controls.rumble ? fmt::format("{}%", controls.rumble) : "Off", false,
						"How strongly the DualSense rumbles when the game makes the controller vibrate.\nLeft and Right change it by 10%."},
					{"Left stick deadzone", fmt::format("{}%", controls.leftDeadzone), false,
						"How far the left stick moves before the game sees it. Raise it if a character drifts when you let go of "
						"the stick; lower it for finer control."},
					{"Right stick deadzone", fmt::format("{}%", controls.rightDeadzone), false,
						"How far the right stick moves before the game sees it. Raise it if the camera drifts when you let go of "
						"the stick; lower it for finer control."},
					{"Buttons", Plural(mapped, "button set", "buttons set"), false,
						"Which DualSense button is which of the controller's.\nA is on Circle and B on Cross by default, where the "
						"Wii U has them."},
					{"Reset to defaults", m_resetArmed ? "Press Cross again" : "", false,
						"The default buttons, vibration, motion and deadzones for this controller."},
				};
				for (int row = 0; row < kListRows; row++)
				{
					const std::string id = fmt::format("player-row-{}", row);
					const bool present = row < kPlayerRows;
					SetClass(m_document, id, "offscreen", !present);
					SetClass(m_document, id, "focused", row == m_playerRow);
					SetClass(m_document, id, "dimmed", present && rows[row].dimmed);
					SetText(m_document, fmt::format("player-name-{}", row), present ? rows[row].name : "");
					SetText(m_document, fmt::format("player-value-{}", row), present ? rows[row].value : "");
				}
				SetText(m_document, "player-detail-title", rows[m_playerRow].name);
				SetLines(m_document, "player-help", rows[m_playerRow].help);
				SetHints(m_document, {{"cross", m_playerRow == kRowButtons ? "Open" : "Choose"}, {"leftright", "Change"}, {"circle", "Back"}});
			}

			// -- settings > controls > buttons -------------------------------------------------

			struct Capture
			{
				bool active = false;
				bool released = false; // everything let go since it started
				uint64_t until = 0;
			};

			void OpenMapping()
			{
				m_mapSelected = 0;
				m_capture = {};
				m_mapMessage.clear();
				ShowScreen(kMapping);
				if (Is3ds())
				{
					SetText(m_document, "mapping-copy", "The DualSense's buttons for the 3DS's");
					SetText(m_document, "mapping-kicker", "NINTENDO 3DS");
				}
				else
				{
					const auto controls = ps5emu::GetPlayerControls(m_player);
					SetText(m_document, "mapping-copy", fmt::format("Player {}: the DualSense's buttons for the {}", m_player + 1, TypeName(controls.type)));
					SetText(m_document, "mapping-kicker", Upper(TypeName(controls.type)));
				}
				UpdateMapping();
			}

			std::vector<ps5emu::ButtonMapping> Mappings() const
			{
				return Is3ds() ? ps5azahar::ListMappings(m_settings.n3ds) : ps5emu::ListMappings(m_player);
			}

			void SetMapping(int index, ps5emu::PadInput input)
			{
				if (!Is3ds())
				{
					ps5emu::SetMapping(m_player, index, input);
					return;
				}
				ps5azahar::SetMapping(m_settings.n3ds, index, input);
				SaveSettings();
			}

			void MappingKey(Key key)
			{
				if (m_capture.active)
					return; // PollCapture has the controller
				const int count = (int)Mappings().size();
				if (key == Key::Circle)
				{
					if (Is3ds())
					{
						OpenSettings(CategoryIndex("controls"));
						m_onRail = false;
						m_settingRow = 2; // Buttons
						UpdateSettings();
						return;
					}
					OpenPlayer(m_player);
					m_playerRow = kRowButtons;
					UpdatePlayer();
					return;
				}
				if (key == Key::Cross && count)
				{
					m_capture = {true, false, sceKernelGetProcessTime() + kCaptureUs};
					m_mapMessage.clear();
				}
				else if (key == Key::Square && count)
				{
					SetMapping(m_mapSelected, ps5emu::PadInput::None);
					m_mapMessage = "Cleared: no DualSense button is this one now.";
				}
				else if (Browse(key, m_mapSelected, count, kListRows))
					m_mapMessage.clear();
				UpdateMapping();
			}

			// Waits for everything to be let go (the Cross that started it), then takes the next
			// press. The touchpad cancels.
			void PollCapture()
			{
				ps5pad::Data data{};
				if (!ps5pad::Read(m_player, data) && !ps5pad::Read(0, data))
					return;
				const bool idle = Pressed(data) == ps5emu::PadInput::None && !(data.buttons & ps5pad::kTouchPad) &&
					data.l2 < 60 && data.r2 < 60;
				if (sceKernelGetProcessTime() > m_capture.until)
				{
					m_capture.active = false;
					m_mapMessage = "No button was pressed, so it stays as it was.";
				}
				else if (!m_capture.released)
					m_capture.released = idle;
				else if (data.buttons & ps5pad::kTouchPad)
				{
					m_capture.active = false;
					m_mapMessage = "Cancelled: it stays as it was.";
				}
				else if (const auto input = Pressed(data); input != ps5emu::PadInput::None)
				{
					m_capture.active = false;
					SetMapping(m_mapSelected, input);
					m_mapMessage = "Done.";
				}
				UpdateMapping();
			}

			void UpdateMapping()
			{
				const auto mappings = Mappings();
				const int count = (int)mappings.size();
				m_mapSelected = std::clamp(m_mapSelected, 0, std::max(0, count - 1));
				const int scroll = Scroll(m_mapSelected, kListRows);
				for (int row = 0; row < kListRows; row++)
				{
					const int index = scroll + row;
					const std::string id = fmt::format("map-row-{}", row);
					SetClass(m_document, id, "focused", index == m_mapSelected);
					SetClass(m_document, id, "offscreen", index >= count);
					SetText(m_document, fmt::format("map-name-{}", row), index < count ? mappings[index].button : "");
					const bool unset = index < count && mappings[index].input.empty();
					SetText(m_document, fmt::format("map-value-{}", row), index >= count ? "" : unset ? "Not set" : mappings[index].input);
					SetClass(m_document, fmt::format("map-value-{}", row), "unset", unset);
				}
				SetText(m_document, "mapping-position", fmt::format("{} of {}", count ? m_mapSelected + 1 : 0, count));
				if (m_capture.active)
					SetHints(m_document, {{"touchpad", "Cancel"}});
				else
					SetHints(m_document, {{"cross", "Assign"}, {"square", "Clear"}, {"circle", "Back"}});
				if (count == 0)
					return;
				const auto& mapping = mappings[m_mapSelected];
				SetText(m_document, "map-title", mapping.button);
				SetText(m_document, "map-input", mapping.input.empty() ? "Not set" : mapping.input);
				std::string message;
				if (m_capture.active)
				{
					const uint64_t now = sceKernelGetProcessTime();
					const int seconds = (int)((m_capture.until > now ? m_capture.until - now : 0) / 1000000) + 1;
					message = fmt::format("Press the DualSense button, trigger or stick direction for {} now.\n\n{} s left. A touchpad click cancels.",
						mapping.button, seconds);
				}
				else
					message = (m_mapMessage.empty() ? "" : m_mapMessage + "\n\n") +
						"Cross, then a DualSense button, trigger or stick direction: it becomes this one.\nSquare clears it.";
				SetLines(m_document, "map-message", message);
			}

			// -- Artic Base, on Azahar's side: a game from a 3DS on the network --------------------
			// The 3DS's address (edited a number at a time, or typed on a keyboard), connecting, and
			// the Artic Setup Tool's two modes.

			void OpenArtic()
			{
				m_articRow = 0;
				m_articEditing = false;
				m_articArmed = false;
				if (!ParseAddress(m_settings.n3ds.articAddress, m_articOctets))
					m_articOctets = OwnAddress();
				ShowScreen(kArtic);
				UpdateArtic();
			}

			std::string ArticAddress() const
			{
				return fmt::format("{}.{}.{}.{}", m_articOctets[0], m_articOctets[1], m_articOctets[2], m_articOctets[3]);
			}

			void ArticKey(Key key)
			{
				if (m_articEditing)
				{
					switch (key)
					{
					case Key::Left: m_articOctet = (m_articOctet + 3) % 4; break;
					case Key::Right: m_articOctet = (m_articOctet + 1) % 4; break;
					case Key::Up:
					case Key::Down:
					case Key::L1:
					case Key::R1:
					{
						const int step = key == Key::Up ? 1 : key == Key::Down ? -1 : key == Key::R1 ? 10 : -10;
						int& octet = m_articOctets[m_articOctet];
						octet = (octet + step + 256) % 256;
						break;
					}
					case Key::Cross:
					case Key::Circle:
						m_articEditing = false;
						m_settings.n3ds.articAddress = ArticAddress();
						SaveSettings();
						break;
					default: break;
					}
					UpdateArtic();
					return;
				}
				if (key == Key::Circle)
				{
					ShowTab(kHome);
					return;
				}
				if (key == Key::Up || key == Key::Down)
				{
					Browse(key, m_articRow, kArticRows, kArticRows);
					m_articArmed = false;
					UpdateArtic();
					return;
				}
				if (key != Key::Cross)
					return;
				switch (m_articRow)
				{
				case kRowArticAddress:
					m_articEditing = true;
					m_articOctet = 3; // the last number is the one that differs on a home network
					break;
				case kRowArticConnect: LaunchArtic(ps5azahar::kArticBase, "Artic Base"); return;
				case kRowArticSetupOld:
				case kRowArticSetupNew:
					// it writes the 3DS's own data into Azahar's: a second press, to be sure
					if (m_articArmed)
					{
						LaunchArtic(m_articRow == kRowArticSetupOld ? ps5azahar::kArticSetupOld : ps5azahar::kArticSetupNew, "Artic Setup Tool");
						return;
					}
					m_articArmed = true;
					break;
				}
				UpdateArtic();
			}

			// As Launch, for a game that is on the 3DS: no library entry, nothing to add to the recent ones.
			void LaunchArtic(const char* scheme, const char* what)
			{
				m_settings.n3ds.articAddress = ArticAddress();
				SaveSettings();
				ps5emu::Game game;
				game.name = fmt::format("{} ({})", what, ArticAddress());
				game.path = std::string(scheme) + ArticAddress();
				game.format = "ARTIC";
				SetText(m_document, "loading-title", game.name);
				SetText(m_document, "loading-caption", "Connecting to the 3DS");
				FitImage(m_document, "loading-cover", Icon(), 760, 220, 400, 400);
				ShowScreen(kLoading);
				SetHints(m_document, {});
				m_launch = game;
			}

			void UpdateArtic()
			{
				std::string address = ArticAddress();
				if (m_articEditing)
				{
					address.clear();
					for (int i = 0; i < 4; i++)
						address += (i ? "." : "") + (i == m_articOctet ? fmt::format("[{}]", m_articOctets[i]) : std::to_string(m_articOctets[i]));
				}
				const char* again = "Press Cross again";
				struct Row
				{
					const char* name;
					std::string value;
					const char* help;
				};
				const Row rows[kArticRows] = {
					{"3DS address", address,
						"The address Artic Base shows on the 3DS's screen when it is ready.\nCross edits it: Left and Right choose a "
						"number, Up and Down change it by 1, L1 and R1 by 10, and Cross again keeps it.\nThe 3DS and the PS5 must be "
						"on the same network."},
					{"Connect and play", "",
						"On the 3DS, start the Artic Base app (homebrew, from Azahar's team) and choose a game in it: the cartridge "
						"or an installed one. Then connect from here: the game plays on the PS5 from the 3DS, and its saves stay on "
						"the 3DS.\nIt is only as smooth as the network: a strong Wi-Fi signal for the 3DS and a wired PS5 help."},
					{"Set up from an Old 3DS", m_articArmed && m_articRow == kRowArticSetupOld ? again : "",
						"With the Artic Setup Tool app running on an Old 3DS or 2DS: copies its system files and its own data "
						"(system settings, friend code, Mii and eShop data) into Azahar, so games that need the 3DS's system "
						"applets or files start, and the Home Menu can (Settings > System).\nThat data is your console's: do not "
						"share Azahar's folder afterwards."},
					{"Set up from a New 3DS", m_articArmed && m_articRow == kRowArticSetupNew ? again : "",
						"As above, from a New 3DS or New 2DS running the Artic Setup Tool app."},
				};
				for (int row = 0; row < kArticRows; row++)
				{
					const std::string id = fmt::format("artic-row-{}", row);
					SetClass(m_document, id, "focused", row == m_articRow);
					SetText(m_document, fmt::format("artic-name-{}", row), rows[row].name);
					SetText(m_document, fmt::format("artic-value-{}", row), rows[row].value);
				}
				SetText(m_document, "artic-detail-title", rows[m_articRow].name);
				SetLines(m_document, "artic-help", rows[m_articRow].help);
				if (m_articEditing)
					SetHints(m_document, {{"leftright", "Number"}, {"updown", "Change"}, {"cross", "Keep"}});
				else
					SetHints(m_document, {{"cross", m_articRow == kRowArticAddress ? "Edit" : m_articRow == kRowArticConnect ? "Connect" : "Set up"},
						{"circle", "Back"}});
			}

			// -- settings > game files, and installs ---------------------------------------------

			enum class FilesMode
			{
				GamesFolder,
				Install,	// Cemu's: a folder with code, content and meta
				InstallCia, // Azahar's: a CIA file
			};

			void OpenFiles(FilesMode mode)
			{
				m_filesMode = mode;
				const std::string& games = GamesFolder();
				std::string start = mode != FilesMode::GamesFolder && !m_installFolder.empty() ? m_installFolder :
					games.empty() ? ps5paths::kGames : games;
				while (!BrowseTo(start) && start != "/")
					start = ParentPath(start);
				m_filesMessage.clear();
				ShowScreen(kFiles);
				const bool install = mode != FilesMode::GamesFolder;
				const bool cia = mode == FilesMode::InstallCia;
				SetText(m_document, "files-title", cia ? "Install CIA files" : install ? "Install updates and DLC" : "Game files");
				SetText(m_document, "files-copy", cia ? "Choose a CIA file: a game, an update or DLC" :
					install ? "Choose a folder with an update, DLC or game (code, content and meta)" :
					Is3ds() ? "Choose the folder that holds your 3DS games" : "Choose the folder that holds your Wii U games");
				SetText(m_document, "files-kicker", install ? "TO INSTALL" : "THIS FOLDER");
				UpdateFiles();
			}

			// A folder's subfolders, and its CIA files when they are what is being chosen
			bool BrowseTo(const std::string& folder)
			{
				bool ok = false;
				auto folders = ListEntries(folder, true, ok);
				if (!ok)
					return false;
				m_browseFolder = folder;
				m_browseEntries.clear();
				if (folder != "/")
					m_browseEntries.push_back("..");
				// the drives plugged in, as shortcuts, except the one already open (and in /mnt, which
				// lists them itself)
				if (folder != "/mnt")
					for (const std::string& drive : ConnectedDrives())
						if (folder != drive && !folder.starts_with(drive + "/"))
							m_browseEntries.push_back(drive);
				m_browseEntries.insert(m_browseEntries.end(), folders.begin(), folders.end());
				m_browseFiles = 0;
				if (m_filesMode == FilesMode::InstallCia)
					for (const auto& file : ListEntries(folder, false, ok))
						if (Lower(file).ends_with(".cia"))
						{
							m_browseEntries.push_back(file);
							m_browseFiles++;
						}
				m_browseSelected = 0;
				return true;
			}

			bool IsFileEntry(int index) const
			{
				return index >= (int)m_browseEntries.size() - m_browseFiles && index < (int)m_browseEntries.size();
			}

			const ps5emu::InstallCandidate& Inspect(const std::string& folder)
			{
				auto it = m_inspected.find(folder);
				if (it == m_inspected.end())
					it = m_inspected.emplace(folder, ps5emu::InspectInstall(folder)).first;
				return it->second;
			}

			void CloseFiles()
			{
				OpenSettings(CategoryIndex(m_filesMode == FilesMode::GamesFolder ? "files" : "installs"));
				m_onRail = false;
				UpdateSettings();
			}

			void FilesKey(Key key)
			{
				const int count = (int)m_browseEntries.size();
				if (m_installing)
				{
					if (key == Key::Circle)
					{
						// Cemu's install puts back what it replaced; Azahar's writes in place, so the title's
						// contents go (as when an install fails part-way), its saves staying
						if (m_filesMode == FilesMode::InstallCia)
						{
							ps5azahar::CancelInstall();
							m_filesMessage = "Cancelling: what it copied is removed...";
						}
						else
						{
							ps5emu::CancelInstall();
							m_filesMessage = "Cancelling: what was installed before is put back...";
						}
						UpdateFiles();
					}
					return;
				}
				if (key == Key::Circle)
				{
					CloseFiles();
					return;
				}
				if (key == Key::Cross && count && !IsFileEntry(m_browseSelected))
				{
					const std::string entry = m_browseEntries[m_browseSelected];
					const std::string from = m_browseFolder;
					const bool up = entry == "..";
					if (!BrowseTo(up ? ParentPath(m_browseFolder) : JoinPath(m_browseFolder, entry)))
						m_filesMessage = "This folder cannot be opened.";
					else
					{
						m_filesMessage.clear();
						if (up) // keep the folder just left in view
						{
							const std::string left = from.substr(from.find_last_of('/') + 1);
							for (int i = 0; i < (int)m_browseEntries.size(); i++)
								if (m_browseEntries[i] == left)
									m_browseSelected = i;
						}
					}
				}
				else if (key == Key::Triangle && m_filesMode == FilesMode::GamesFolder)
				{
					GamesFolder() = m_browseFolder;
					const bool saved = ps5settings::Save(m_settings);
					if (Is3ds())
					{
						ps5azahar::StartScan(GamesFolder());
						m_scanning = true;
						m_games.clear();
					}
					else if (m_status.coreReady)
					{
						ps5emu::ApplyOptions({m_settings.gamesFolder, m_settings.overlay, m_settings.volume, m_settings.upscaleFilter,
							m_settings.asyncShaders, m_settings.gamePadSpeaker});
						m_scanning = true;
						m_games.clear();
					}
					m_filesMessage = saved ? "Saved. PS5CEMU-HAR is looking for games there." : "The folder could not be saved. Please try again.";
				}
				else if (key == Key::Triangle && m_filesMode == FilesMode::InstallCia)
				{
					std::string error;
					m_installFolder = m_browseFolder;
					if (!IsFileEntry(m_browseSelected))
						m_filesMessage = "Choose a CIA file first.";
					else if (ps5azahar::StartInstall(JoinPath(m_browseFolder, m_browseEntries[m_browseSelected]), error))
					{
						m_installing = true;
						m_filesMessage.clear();
					}
					else
						m_filesMessage = error;
				}
				else if (key == Key::Triangle && m_filesMode == FilesMode::Install)
				{
					std::string error;
					m_installFolder = m_browseFolder;
					if (!m_status.coreReady)
						m_filesMessage = "Cemu did not start, so nothing can be installed.";
					else if (ps5emu::StartInstall(m_browseFolder, error))
					{
						m_installing = true;
						m_filesMessage.clear();
					}
					else
						m_filesMessage = error;
				}
				else if (!Browse(key, m_browseSelected, count, kFileRows))
					return;
				UpdateFiles();
			}

			void PollInstall()
			{
				const auto status = m_filesMode == FilesMode::InstallCia ? ps5azahar::GetInstallStatus() : ps5emu::GetInstallStatus();
				using State = ps5emu::InstallStatus::State;
				if (status.state == State::Running)
				{
					const int percent = status.total ? (int)(status.copied * 100 / status.total) : 0;
					const std::string progress = status.total ?
						fmt::format("Installing: {}%, {} of {}.\n\nCircle cancels.", percent, Gigabytes(status.copied), Gigabytes(status.total)) :
						"Installing: counting the files...";
					if (m_installProgress != progress)
					{
						m_installProgress = progress;
						if (m_filesMessage.rfind("Cancelling", 0) != 0)
							SetLines(m_document, "files-message", progress);
					}
					return;
				}
				m_installing = false;
				m_installProgress.clear();
				m_inspected.clear();
				if (status.state == State::Done)
				{
					m_filesMessage = "Installed. The library has it with its game.";
					if (Is3ds())
						ps5azahar::StartScan(GamesFolder());
					else
						ps5emu::Rescan();
					m_scanning = true;
				}
				else if (status.state == State::Cancelled && m_filesMode == FilesMode::InstallCia)
					m_filesMessage = "Cancelled. What it copied was removed, with any earlier copy of the same title: install it again to "
						"play it. Its saves are kept.";
				else if (status.state == State::Cancelled)
					m_filesMessage = "Cancelled. What was installed before is as it was.";
				else
					m_filesMessage = "It could not be installed: " + status.message;
				UpdateFiles();
			}

			void UpdateFiles()
			{
				const bool install = m_filesMode == FilesMode::Install;
				const bool cia = m_filesMode == FilesMode::InstallCia;
				const int count = (int)m_browseEntries.size();
				const int scroll = Scroll(m_browseSelected, kFileRows);
				for (int row = 0; row < kFileRows; row++)
				{
					const int index = scroll + row;
					const bool up = index < count && m_browseEntries[index] == "..";
					const std::string id = fmt::format("files-row-{}", row);
					SetClass(m_document, id, "focused", index == m_browseSelected);
					SetClass(m_document, id, "offscreen", index >= count);
					SetClass(m_document, id, "folder", index < count && !IsFileEntry(index));
					const bool drive = index < count && m_browseEntries[index].starts_with('/');
					SetText(m_document, fmt::format("files-name-{}", row), index >= count ? "" : up ? "Parent folder" : drive ? DriveName(m_browseEntries[index]) : m_browseEntries[index]);
					std::string meta = IsFileEntry(index) ? "CIA" : drive ? "Drive" : "";
					if (install && index < count && !up && !drive)
					{
						const auto& candidate = Inspect(JoinPath(m_browseFolder, m_browseEntries[index]));
						if (candidate.kind != ps5emu::InstallCandidate::Kind::None)
							meta = KindName(candidate.kind);
					}
					SetText(m_document, fmt::format("files-meta-{}", row), meta);
				}
				SetClass(m_document, "files-empty", "visible", count == 0);
				SetText(m_document, "files-path", Upper(ShortPath(m_browseFolder, 52)));
				SetText(m_document, "files-position", fmt::format("{} of {}", count ? m_browseSelected + 1 : 0, count));

				// TRIANGLE uses the folder shown: show what the side would find in it
				std::pair<std::string, std::string> lines[3];
				bool ready[3]{};
				std::string message = m_filesMessage;
				if (cia)
				{
					const bool file = IsFileEntry(m_browseSelected);
					const auto title = file ? ps5azahar::Inspect(JoinPath(m_browseFolder, m_browseEntries[m_browseSelected])) : ps5azahar::Title{};
					SetText(m_document, "files-current", !file ? ShortPath(m_browseFolder, 40) : title.name.empty() ? m_browseEntries[m_browseSelected] : title.name);
					lines[0] = {"TYPE", !file ? "A folder" : title.titleId ? CiaKind(title.titleId) : "CIA"};
					lines[1] = {"TITLE ID", file && title.titleId ? Hex(title.titleId) : "-"};
					lines[2] = {"VERSION", file && title.titleId ? fmt::format("v{}", title.version) : "-"};
					ready[0] = ready[1] = ready[2] = file;
					if (message.empty())
						message = file ? "Triangle installs it into the 3DS's storage: an update or DLC goes with its game, and a game joins the library." :
										 "Choose a CIA file to install: a game, an update or DLC.";
				}
				else if (Is3ds() && !install)
				{
					SetText(m_document, "files-current", ShortPath(m_browseFolder, 40));
					const int games = Count3dsGames(m_browseFolder);
					lines[0] = {"GAMES", games < 0 ? "Cannot be read" : Plural(games, "game", "games") + " here"};
					lines[1] = {"IN USE", ShortPath(GamesFolder(), 40)};
					lines[2] = {"", ""};
					ready[0] = games > 0, ready[1] = GamesFolder() == m_browseFolder;
					if (message.empty())
						message = "Games can be .3ds or .cci, .cxi, .cia or .3dsx, decrypted, here or in the folders in it. Triangle uses the folder shown.";
				}
				else if (!install)
				{
					SetText(m_document, "files-current", ShortPath(m_browseFolder, 40));
					const int games = CountGames(m_browseFolder);
					const bool keys = IsFile(std::string(ps5paths::kRoot) + "/keys.txt");
					lines[0] = {"GAMES", games < 0 ? "Cannot be read" : Plural(games, "game", "games")};
					lines[1] = {"KEYS.TXT", keys ? "Found in /data/ps5cemu" : "Missing (only .wud/.wux need it)"};
					lines[2] = {"IN USE", ShortPath(m_settings.gamesFolder, 40)};
					ready[0] = games > 0, ready[1] = keys, ready[2] = m_settings.gamesFolder == m_browseFolder;
					if (message.empty())
						message = "Games can be .wua, .wud, .wux, or folders with code, content and meta. Triangle uses the folder shown.";
				}
				else
				{
					const auto& candidate = Inspect(m_browseFolder);
					const bool valid = candidate.kind != ps5emu::InstallCandidate::Kind::None;
					SetText(m_document, "files-current", valid && !candidate.name.empty() ? candidate.name : ShortPath(m_browseFolder, 40));
					lines[0] = {"TYPE", valid ? KindName(candidate.kind) : "Nothing to install"};
					lines[1] = {"TITLE ID", valid ? Hex(candidate.titleId) : "-"};
					lines[2] = {"VERSION", !valid ? "-" : candidate.installedVersion < 0 ?
						fmt::format("v{}, not installed yet", candidate.version) :
						fmt::format("v{}, v{} installed now", candidate.version, candidate.installedVersion)};
					ready[0] = ready[1] = valid;
					ready[2] = valid && candidate.installedVersion < (int)candidate.version;
					if (message.empty())
						message = valid ? "Triangle installs it into the Wii U's storage (mlc01). Updates and DLC in the game files folder work as they are, too." :
										  candidate.note;
				}
				for (int i = 0; i < 3; i++)
				{
					SetText(m_document, fmt::format("files-label-{}", i), lines[i].first);
					SetText(m_document, fmt::format("files-value-{}", i), lines[i].second);
					SetClass(m_document, fmt::format("files-value-{}", i), "ready", ready[i]);
				}
				if (m_installing && !m_installProgress.empty() && m_filesMessage.empty())
					message = m_installProgress;
				SetLines(m_document, "files-message", message);
				if (m_installing)
					SetHints(m_document, {{"circle", "Cancel"}});
				else
					SetHints(m_document, {{"cross", "Open"}, {"triangle", install || cia ? "Install" : "Use this folder"}, {"circle", "Back"}});
			}

			Rml::ElementDocument* m_document;
			System m_system;
			ps5settings::Launcher& m_settings;
			const Status& m_status;
			Screen m_screen = kHome;
			Screen m_tab = kHome;	// the tab selected
			bool m_onTabs = false;	// the focus is on the tabs
			std::optional<ps5emu::Game> m_launch;
			bool m_leaving = false;
			Picker m_picker;
			bool m_info = false;	// a setting's help is open

			std::vector<ps5emu::Game> m_games;
			bool m_scanning = false;
			uint32_t m_boxArrivals = 0;		  // ps5boxart::Arrivals() when the covers were last drawn
			std::set<std::string> m_shownArt; // the covers loaded since they were last let go of
			std::time_t m_shownMinute = 0;

			int m_homeRow = kHeroRow, m_homeColumn = 0;
			int m_lastIndex = -1;
			std::vector<int> m_recent; // indices into m_games

			int m_librarySelected = 0;
			int m_gridTop = 0; // the first row of the grid shown

			int m_detailsGame = -1;
			Screen m_detailsFrom = kLibrary;
			int m_detailsAction = 0;
			int m_synopsisTop = 0; // the description's first line shown

			int m_packsGame = 0;
			Screen m_packsFrom = kHome;
			std::vector<ps5emu::GraphicPackInfo> m_packs;
			std::vector<PackItem> m_packItems;
			int m_packItem = 0;
			bool m_presetsFocus = false;
			int m_presetSelected = 0;

			int m_category = 0;	   // Settings' category on the rail
			bool m_onRail = true;  // the focus on the rail, not on the category's settings
			int m_settingRow = 0;

			int m_player = 0;
			int m_playerRow = 0;
			bool m_resetArmed = false;
			int m_mapSelected = 0;
			Capture m_capture;
			std::string m_mapMessage;

			int m_articRow = 0;
			bool m_articEditing = false; // the address's numbers being changed
			int m_articOctet = 3;		 // which of them
			std::array<int, 4> m_articOctets{192, 168, 1, 2};
			bool m_articArmed = false;	 // a setup row pressed once

			FilesMode m_filesMode = FilesMode::GamesFolder;
			std::string m_browseFolder;
			std::vector<std::string> m_browseEntries; // ".." first unless at "/", then subfolders, then files
			int m_browseFiles = 0;					  // how many of them, at the end, are files
			int m_browseSelected = 0;
			std::string m_filesMessage;
			std::string m_installFolder;
			std::map<std::string, ps5emu::InstallCandidate> m_inspected;
			bool m_installing = false;
			std::string m_installProgress;
			std::string m_diagnosticsDone[2]; // what Diagnostics' two actions did, on their rows
			bool m_confirmClear = false;	  // Clear shader caches was pressed once
			std::string m_packsShown;		  // the graphic packs' and app updates' rows as last shown
			bool m_packsReloaded = false;	  // Cemu read the newly installed packs
		};

		// The app's own update (app/updates.h), over whichever screen is up: a newer release asked
		// about, then its download, check and install, then the restart into it. It has the keys
		// while it is up.
		class UpdatePrompt
		{
		public:
			explicit UpdatePrompt(Rml::ElementDocument* document) : m_document(document) {}

			// Shown or not, and what it says, as the update stands; true when it opened or closed (the
			// screen under it shows its own hints again then)
			bool Poll()
			{
				const bool show = ps5update::Prompting();
				const bool changed = show != m_open;
				m_open = show;
				SetClass(m_document, "update", "open", show);
				if (show)
					Show(ps5update::GetStatus());
				else
					m_shown.clear();
				return changed;
			}

			bool Open() const { return m_open; }

			void HandleKey(Key key)
			{
				const auto status = ps5update::GetStatus();
				const auto choices = Choices(status);
				if ((key == Key::Left || key == Key::Right) && choices.size() > 1)
					m_choice = 1 - m_choice;
				else if (key == Key::Circle)
					ps5update::Dismiss();
				else if (key == Key::Cross && !choices.empty())
				{
					const std::string choice = choices[std::min<size_t>(m_choice, choices.size() - 1)];
					if (choice == "Update now")
						ps5update::Install();
					else if (choice == "Restart now")
						ps5update::Restart();
					else
						ps5update::Dismiss(); // Later, OK
					m_choice = 0;
				}
				// the next Poll shows what changed, and tells the screen under it when it closed
			}

		private:
			static std::vector<std::string> Choices(const ps5update::Status& status)
			{
				using State = ps5update::Status::State;
				switch (status.state)
				{
				case State::Available: return {"Update now", "Later"};
				case State::Ready: return {"Restart now"};
				case State::Failed: return {"OK"};
				default: return {};
				}
			}

			void Show(const ps5update::Status& status)
			{
				using State = ps5update::Status::State;
				const std::string next = ps5update::Readable(status.latest), mine = ps5update::Readable(PS5CEMU_VERSION);
				std::string title, text;
				float done = -1.0f; // the bar's fill, when it shows
				switch (status.state)
				{
				case State::Available:
					title = fmt::format("PS5CEMU-HAR {} is out", next);
					text = fmt::format("This is {}. Download and install it now? Your games, saves and settings stay as they are.", mine);
					break;
				case State::Downloading:
					title = fmt::format("Updating to {}", next);
					text = status.total ? fmt::format("Downloading: {} of {} MB", status.received >> 20, status.total >> 20) : "Downloading...";
					done = status.total ? (float)status.received / (float)status.total : 0.0f;
					break;
				case State::Verifying:
					title = fmt::format("Updating to {}", next);
					text = "Checking the download";
					done = 1.0f;
					break;
				case State::Installing:
					title = fmt::format("Updating to {}", next);
					text = "Installing";
					done = 1.0f;
					break;
				case State::Ready:
					title = fmt::format("{} is installed", next);
					text = "PS5CEMU-HAR starts again to finish the update.";
					break;
				case State::Failed:
					title = "The update did not finish";
					text = fmt::format("{}\nPS5CEMU-HAR is still {}.", status.message, mine);
					break;
				default:
					title = "Checking for updates";
					break;
				}
				const auto choices = Choices(status);
				m_choice = std::min<int>(m_choice, std::max<int>(0, (int)choices.size() - 1));
				// only what changed is laid out again
				const std::string shown = fmt::format("{}|{}|{}|{}|{}", title, text, done, m_choice, choices.size());
				if (shown == m_shown)
					return;
				m_shown = shown;
				SetText(m_document, "update-title", title);
				SetLines(m_document, "update-text", text);
				SetClass(m_document, "update-bar", "visible", done >= 0.0f);
				if (Rml::Element* fill = m_document->GetElementById("update-fill"))
					fill->SetProperty("width", fmt::format("{}px", std::round(920.0f * std::clamp(done, 0.0f, 1.0f))));
				for (int i = 0; i < 2; i++)
				{
					const bool used = i < (int)choices.size();
					SetClass(m_document, fmt::format("update-{}", i), "unused", !used);
					SetClass(m_document, fmt::format("update-{}", i), "focused", used && i == m_choice);
					SetText(m_document, fmt::format("update-label-{}", i), used ? choices[i] : "");
				}
				if (choices.size() > 1)
					SetHints(m_document, {{"leftright", "Choose"}, {"cross", "Select"}, {"circle", "Later"}});
				else if (!choices.empty())
					SetHints(m_document, {{"cross", choices.front() == "OK" ? "OK" : "Restart"}});
				else
					SetHints(m_document, {});
			}

			Rml::ElementDocument* m_document;
			bool m_open = false;
			int m_choice = 0;
			std::string m_shown; // what it last laid out
		};

		// The start screen: Cemu on the left half, Azahar on the right; Left and Right choose, Cross starts.
		// Neither emulator runs behind it: the game counts are those their libraries had last time.
		class StartScreen
		{
		public:
			StartScreen(Rml::ElementDocument* document, const Status& status, const ps5settings::Launcher& settings, System selected)
				: m_document(document), m_status(status), m_selected(selected)
			{
				auto count = [](int games) { return games < 0 ? std::string("Open to look for games") : Plural(games, "game", "games"); };
				SetText(m_document, "start-version", ps5update::Readable(PS5CEMU_VERSION));
				SetText(m_document, "start-status-wiiu", !m_status.notice.empty() ? "Setup required" : count(settings.gameCount));
				SetText(m_document, "start-status-3ds",
					ps5azahar::Available() ? count(settings.n3ds.gameCount) : count(settings.n3ds.gameCount) + "  /  core not in this build");
				RestoreHints();
				Update();
			}

			void RestoreHints() { SetHints(m_document, {{"leftright", "Choose"}, {"cross", "Start"}}); }

			// While the chosen emulator starts (Cemu takes a few seconds): on its card.
			void ShowStarting(System system)
			{
				SetText(m_document, system == System::WiiU ? "start-status-wiiu" : "start-status-3ds",
					system == System::WiiU ? "Starting Cemu" : "Starting Azahar");
			}

			// The emulator chosen, once Cross is pressed.
			std::optional<System> HandleKey(Key key)
			{
				if (key == Key::Left || key == Key::Right)
				{
					m_selected = m_selected == System::WiiU ? System::N3ds : System::WiiU;
					Update();
				}
				else if (key == Key::Cross)
					return m_selected;
				return std::nullopt;
			}

		private:
			void Update()
			{
				const bool wiiu = m_selected == System::WiiU;
				SetClass(m_document, "start-wiiu", "focused", wiiu);
				SetClass(m_document, "start-wiiu", "dim", !wiiu);
				SetClass(m_document, "start-3ds", "focused", !wiiu);
				SetClass(m_document, "start-3ds", "dim", wiiu);
			}

			Rml::ElementDocument* m_document;
			const Status& m_status;
			System m_selected;
		};
	}

	namespace
	{
		// Before a game or a fresh process: no more box art (a cover on its way is waited for), and
		// the side's game scan finished, the screen kept drawn meanwhile
		template<typename Frame>
		void WaitForScan(System system, const Status& status, Frame& frame)
		{
			for (int waited = 0; system == System::N3ds ? ps5azahar::Scanning() : status.coreReady && ps5emu::Scanning(); waited++)
			{
				if (waited == 0)
					ps5log::Line("[launcher] waiting for the library's scan to finish");
				frame();
				sceKernelUsleep(16000);
			}
		}

		template<typename Frame>
		void StopBackgroundWork(System system, const Status& status, Frame& frame)
		{
			ps5boxart::Stop();
			ps5packs::Stop();
			WaitForScan(system, status, frame);
		}
	}

	std::optional<Choice> Run(ps5settings::Launcher& settings, Status& status, const std::function<void(System)>& prepare)
	{
		std::string error;
		if (!ps5ui::Start(error))
		{
			ps5log::Line("[launcher] {}", error);
			ps5notify::Send("The launcher cannot show: " + error);
			return std::nullopt;
		}
		ps5sound::Start(settings.music, settings.musicVolume, settings.menuSounds);
		ps5boxart::SetEnabled(settings.boxArt);
		// After a game, the launcher opens on its emulator's side (and only then: next time, the
		// start screen)
		System system = settings.side == "3ds" ? System::N3ds : System::WiiU;
		bool choosing = settings.side.empty();
		std::optional<System> prepared;
		if (!choosing)
		{
			settings.side.clear();
			ps5settings::Save(settings);
		}
		Input input;
		uint64_t frames = 0;
		auto frame = [&] {
			if (++frames % 120 == 0)
				ps5pad::Rescan(); // controllers joining or leaving, about every two seconds
			ps5ui::Frame();
			if (frames == 1)
				sceSystemServiceHideSplashScreen();
		};
		for (;;)
		{
			if (choosing)
			{
				ps5ui::SetScene(ps5ui::Scene::Both);
				Rml::ElementDocument* document = ps5ui::Show("start.rml");
				if (!document)
					break;
				StartScreen start(document, status, settings, system);
				UpdatePrompt update(document);
				std::optional<System> chosen;
				std::time_t shownMinute = 0;
				while (!chosen)
				{
					for (const Key key : input.Poll())
						if (!chosen)
						{
							const std::string before = Showing(document);
							if (update.Open())
								update.HandleKey(key);
							else
								chosen = start.HandleKey(key);
							Feedback(key, chosen || Showing(document) != before);
						}
					(void)shownMinute;
					if (update.Poll() && !update.Open())
						start.RestoreHints();
					frame();
				}
				system = *chosen;
				choosing = false;
				// the chosen emulator starts now, its card saying so
				start.ShowStarting(system);
				frame();
			}
			// the side's game list and settings. Either way needs no fresh process: each emulator's core
			// starts only for a game, so only the other side's game list was read (finished first)
			if (prepared != system)
			{
				if (prepared)
					WaitForScan(*prepared, status, frame);
				prepare(system);
				prepared = system;
			}
			const bool n3ds = system == System::N3ds;
			ps5ui::SetScene(n3ds ? ps5ui::Scene::Wave : ps5ui::Scene::Bubbles);
			Rml::ElementDocument* document = ps5ui::Show(n3ds ? "azahar.rml" : "main.rml");
			if (!document)
				break;
			Launcher launcher(document, system, settings, status);
			launcher.Initialize();
			UpdatePrompt update(document);
			while (!launcher.Done() && !launcher.Leaving())
			{
				for (const Key key : input.Poll())
				{
					if (launcher.Done() || launcher.Capturing())
					{
						launcher.HandleKey(key);
						continue;
					}
					const std::string before = Showing(document);
					if (update.Open())
						update.HandleKey(key);
					else
						launcher.HandleKey(key);
					if (launcher.Done())
						ps5sound::Play(ps5sound::Effect::Launch);
					else
						Feedback(key, launcher.Capturing() || Showing(document) != before);
				}
				launcher.Poll();
				if (update.Poll() && !update.Open())
					launcher.RefreshHints();
				frame();
			}
			if (launcher.Leaving())
			{
				// back to the start screen, in this process
				ps5log::Line("[launcher] leaving {}'s side for the start screen", n3ds ? "Azahar" : "Cemu");
				choosing = true;
				continue;
			}
			// the launcher's background work stops before the game (the loading screen shows meanwhile)
			StopBackgroundWork(system, status, frame);
			// the loading screen stays on VideoOut while the launcher makes way for the emulator's
			// renderer, and while its music fades and the launch's sound plays out
			ps5ui::Frame();
			ps5sound::Stop();
			ps5ui::Stop();
			ps5log::Line("[launcher] starting {} ({:016x}) on {}", launcher.Choice()->name, launcher.Choice()->titleId, n3ds ? "Azahar" : "Cemu");
			return Choice{system, *launcher.Choice()};
		}
		ps5sound::Stop();
		ps5ui::Stop();
		ps5notify::Send("The launcher's layout did not load.");
		return std::nullopt;
	}
}
