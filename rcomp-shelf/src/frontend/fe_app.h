// R-comp shelf (from PS5SX2's frontend): the cover-flow shelf itself: input, animation and the frame it draws.
// Platform code owns the Vulkan device and the display; it calls Update and Build once a frame, and does what
// the shelf asks (TakeAction): start a title, install a package, cancel an install.
//
// Copyright (C) 2026 Spyros
// Modified for R-comp, 2026: Xbox 360 titles to play or to install; no options sheet and no settings page.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "fe_covers.h"
#include "fe_games.h"
#include "fe_renderer.h"
#include "fe_sound.h"
#include "fe_text.h"

#include <string>
#include <vector>

namespace fe
{
struct Input
{
	bool left = false, right = false, cross = false, options = false, l1 = false, r1 = false;
	bool up = false, down = false, square = false, triangle = false, circle = false;
	bool l2 = false, r2 = false;
};

struct AppConfig
{
	std::string build_tag;      // shown small in the corner
	int preselect = 0;          // the game selected at start
	SoundSink* sound = nullptr; // told about steps and the launch (may be null)
	std::string message;        // shown for a few seconds at start ("" for none): why the last launch failed
};

// What the shelf wants done.
enum class Action
{
	None,
	Play,    // the launch animation has played; the platform closes the shelf and starts the title
	Install, // install (or update from) the game's package
	Cancel,  // cancel the install in progress
	Refresh, // look for titles and packages again (a USB drive was plugged in)
};

class App
{
public:
	bool Init(Renderer* renderer, const Fonts* fonts, std::vector<GameInfo> games, CoverService* covers, const AppConfig& cfg);
	void Shutdown();

	// Advances `dt` seconds with the pad's current buttons.
	void Update(double dt, const Input& in);

	// Fills `f` for this moment; `clock` is the time of day to show ("" for none).
	void Build(FrameDesc& f, const std::string& clock);

	// The next thing the shelf asks for, and the game it is about. Play also ends the shelf (Done()).
	bool TakeAction(Action& action, int& index);

	// An install is running for game `index`: `fraction` 0..1 (negative: unknown), `line` under the title.
	// Only Circle (Cancel) is read until ClearBusy.
	void SetBusy(int index, float fraction, const std::string& line);
	void ClearBusy();
	bool Busy() const { return m_busy_index >= 0; }

	// Game `index` changed (installed, updated): its new facts; its textures stay.
	void ReplaceGame(int index, const GameInfo& g);

	// A line shown for a few seconds over the hints.
	void ShowMessage(const std::string& text);

	// True once a game was picked to play and its launch animation has played.
	bool Done() const { return m_done; }
	int Chosen() const { return m_selected; }

	const std::vector<GameInfo>& games() const { return m_games; }

private:
	struct Slot
	{
		Texture* cover = nullptr;
		Texture* placeholder = nullptr;
		Texture* spine = nullptr;
		VkDescriptorSet set = VK_NULL_HANDLE;
		bool dirty = false;
		float cover_mix = 0;     // animates to 1 once the cover arrives
		bool has_cover = false;
		float glow[3] = {0.38f, 0.80f, 0.34f};
		bool has_glow = false;
	};

	bool Step(int dir); // true when the selection moved
	void Sound(Sfx sfx, float pan);
	void PollCovers();
	void Pose(float d, float t, Mat4& model, float& brightness) const;
	void Ask(Action a);

	Renderer* m_renderer = nullptr;
	const Fonts* m_fonts = nullptr;
	CoverService* m_covers = nullptr;
	AppConfig m_cfg;
	std::vector<GameInfo> m_games;
	std::vector<Slot> m_slots;

	int m_selected = 0;
	float m_scroll = 0, m_scroll_vel = 0; // the shelf's position (a game index), sprung to m_selected
	double m_time = 0;
	double m_select_time = -10;            // when the selection last changed (for the sheen)
	float m_glow[3] = {0.38f, 0.80f, 0.34f};

	// Held-direction repeat.
	int m_held = 0;
	double m_held_for = 0, m_next_repeat = 0;
	Input m_prev;
	bool m_released = false; // buttons held at start count only once released

	Action m_action = Action::None;
	int m_action_index = -1;

	int m_busy_index = -1;
	float m_busy_fraction = -1;
	std::string m_busy_line;

	std::string m_message;
	double m_message_time = -100;

	bool m_launching = false;
	double m_launch_time = 0;
	bool m_done = false;
	Texture* m_atlas = nullptr;
};
} // namespace fe
