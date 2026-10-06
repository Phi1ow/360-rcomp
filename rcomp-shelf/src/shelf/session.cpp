// R-comp shelf: a shelf session (see session.h).
//
// Copyright (C) 2026 R-comp contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "session.h"

#include "fe_i18n.h"

#include <cstdio>

namespace rshelf
{
std::string Format(const char* fmt, const std::string& a, const std::string& b)
{
	char buf[512];
	std::snprintf(buf, sizeof(buf), fmt, a.c_str(), b.c_str());
	return buf;
}

Session::~Session()
{
	Stop(-1);
}

bool Session::Start(fe::Renderer* renderer, const fe::Fonts* fonts, const SessionConfig& cfg, const std::string& preselect,
	const std::string& message)
{
	m_renderer = renderer;
	m_fonts = fonts;
	m_cfg = cfg;
	return Open(preselect, message);
}

bool Session::Open(const std::string& preselect, const std::string& message)
{
	std::vector<fe::GameInfo> games = ScanCatalog(m_cfg.catalog, m_cfg.log);
	int index = 0;
	for (size_t i = 0; i < games.size(); i++)
		if (games[i].stem == preselect)
			index = static_cast<int>(i);
	m_covers = std::make_unique<fe::CoverService>();
	fe::CoverConfig cc;
	cc.manual_dir = m_cfg.covers_dir;
	cc.cache_dir = m_cfg.cover_cache_dir;
	cc.url_template = kX360dbBoxartUrl;
	cc.allow_download = m_cfg.allow_download && static_cast<bool>(m_cfg.download);
	m_covers->Start(games, m_fonts, cc, m_cfg.download ? m_cfg.download : fe::DownloadFn([](const std::string&, std::vector<uint8_t>&) {
		return -1;
	}));
	m_app = std::make_unique<fe::App>();
	fe::AppConfig acfg;
	acfg.build_tag = m_cfg.build_tag;
	acfg.preselect = index;
	acfg.sound = m_cfg.sound;
	acfg.message = message;
	return m_app->Init(m_renderer, m_fonts, std::move(games), m_covers.get(), acfg);
}

void Session::Close()
{
	if (m_covers)
	{
		m_covers->RequestStop();
		if (m_cfg.abort_download)
			m_cfg.abort_download();
		m_covers->Stop(-1);
		if (m_cfg.resume_download)
			m_cfg.resume_download();
	}
	if (m_app)
		m_app->Shutdown();
	m_app.reset();
	m_covers.reset();
}

bool Session::Stop(int timeout_ms)
{
	m_installer.Cancel();
	m_installer.Finish();
	m_installing = -1;
	bool stopped = true;
	if (m_covers)
	{
		m_covers->RequestStop();
		if (m_cfg.abort_download)
			m_cfg.abort_download();
		stopped = m_covers->Stop(timeout_ms);
	}
	if (m_app)
	{
		m_app->Shutdown();
		m_app.reset();
	}
	if (stopped)
		m_covers.reset();
	return stopped;
}

bool Session::PlayChosen(fe::GameInfo& game) const
{
	if (!m_play)
		return false;
	game = m_play_game;
	return true;
}

void Session::Update(double dt, const fe::Input& in)
{
	m_app->Update(dt, in);
	fe::Action a;
	int index;
	while (m_app->TakeAction(a, index))
	{
		const std::vector<fe::GameInfo>& games = m_app->games();
		if (index < 0 || index >= static_cast<int>(games.size()))
		{
			if (a != fe::Action::Refresh)
				continue;
		}
		if (a == fe::Action::Play)
		{
			m_play = true;
			m_play_game = games[static_cast<size_t>(index)];
			if (m_cfg.log)
				m_cfg.log("play " + m_play_game.stem + " (" + m_play_game.title + ")");
		}
		else if (a == fe::Action::Install && m_installing < 0)
		{
			const fe::GameInfo& g = games[static_cast<size_t>(index)];
			InstallJob job{g.package_path, g.stem, m_cfg.catalog.homebrew_dir, m_cfg.trash_dir};
			if (g.package_path.empty() || !m_installer.Start(job))
				m_app->ShowMessage(Format(fe::Tr(fe::Str::InstallFailed), m_installer.error()));
			else
			{
				m_installing = index;
				m_installing_id = g.stem;
				if (m_cfg.log)
					m_cfg.log("install " + g.stem + " from " + g.package_path);
			}
		}
		else if (a == fe::Action::Cancel && m_installing >= 0)
			m_installer.Cancel();
		else if (a == fe::Action::Refresh && m_installing < 0)
		{
			const std::string keep = games.empty() ? std::string() : games[static_cast<size_t>(m_app->Chosen())].stem;
			Close();
			Open(keep, std::string());
		}
	}
	PollInstall();
}

void Session::PollInstall()
{
	if (m_installing < 0)
		return;
	const Installer::State st = m_installer.state();
	if (st == Installer::State::Running)
	{
		const uint64_t total = m_installer.total_bytes();
		m_app->SetBusy(m_installing, total ? static_cast<float>(m_installer.done_bytes()) / static_cast<float>(total) : -1.0f,
			Format(fe::Tr(fe::Str::Installing), fe::Size(m_installer.done_bytes()), fe::Size(total)));
		return;
	}
	std::string message;
	if (st == Installer::State::Done)
		message = Format(fe::Tr(fe::Str::InstallDone), m_installing_id);
	else if (st == Installer::State::Cancelled)
		message = fe::Tr(fe::Str::InstallCancelled);
	else
		message = Format(fe::Tr(fe::Str::InstallFailed), m_installer.error());
	if (m_cfg.log)
		m_cfg.log("install " + m_installing_id + ": " + message + (m_installer.trashed_path().empty() ? "" : " (previous copy in " +
			m_installer.trashed_path() + ")"));
	m_installer.Finish();
	m_app->ClearBusy();
	m_installing = -1;
	// The catalog changed: the shelf opens again on the same title, with the result.
	Close();
	Open(m_installing_id, message);
}
} // namespace rshelf
