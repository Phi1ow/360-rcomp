// R-comp shelf (from PS5SX2's frontend): the shelf's text in the PS5's language (vk-285-110).
// See fe_i18n.h. Fixes go in /data/rcomp/lang/<code>.txt.
//
// Copyright (C) 2026 Spyros
// Modified for R-comp, 2026: the R-comp shelf's strings (games to play and to install).
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fe_i18n.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace fe
{
namespace
{
constexpr int kCount = static_cast<int>(Str::Count);

const char* const kKeys[kCount] = {
	"hint.play",
	"hint.browse",
	"hint.jump",
	"hint.install",
	"hint.update",
	"hint.cancel",
	"hint.refresh",
	"shelf.no_games",
	"covers.downloading",
	"size.gb",
	"size.mb",
	"size.decimal",
	"state.installed",
	"state.not_installed",
	"state.update",
	"install.progress",
	"install.done",
	"install.failed",
	"install.cancelled",
	"launch.starting",
	"launch.failed",
	"notify.covers_one",
	"notify.covers_many",
};

const char* const kEnglish[kCount] = {
	"Play",
	"Browse",
	"Jump",
	"Install",
	"Update",
	"Cancel",
	"Refresh",
	"No recompiled games in /data/homebrew or /data/rcomp/packages",
	"Downloading covers  %d / %d",
	"%s GB",
	"%s MB",
	".",
	"Installed",
	"Not installed",
	"Update available",
	"Installing  %s / %s",
	"Installed as %s",
	"Install failed: %s",
	"Install cancelled",
	"Starting %s",
	"%s didn't start (%s)",
	"R-comp: downloading %d cover",
	"R-comp: downloading %d covers",
};

const char* const kFrench[kCount] = {
	"Jouer",
	"Parcourir",
	"Sauter",
	"Installer",
	"Mettre à jour",
	"Annuler",
	"Actualiser",
	"Aucun jeu recompilé dans /data/homebrew ou /data/rcomp/packages",
	"Téléchargement des jaquettes  %d / %d",
	"%s Go",
	"%s Mo",
	",",
	"Installé",
	"Pas installé",
	"Mise à jour disponible",
	"Installation  %s / %s",
	"Installé sous %s",
	"Échec de l'installation : %s",
	"Installation annulée",
	"Lancement de %s",
	"%s n'a pas démarré (%s)",
	"R-comp : téléchargement de %d jaquette",
	"R-comp : téléchargement de %d jaquettes",
};

const char* const kSpanish[kCount] = {
	"Jugar",
	"Explorar",
	"Saltar",
	"Instalar",
	"Actualizar",
	"Cancelar",
	"Actualizar",
	"No hay juegos recompilados en /data/homebrew ni en /data/rcomp/packages",
	"Descargando carátulas  %d / %d",
	"%s GB",
	"%s MB",
	",",
	"Instalado",
	"No instalado",
	"Actualización disponible",
	"Instalando  %s / %s",
	"Instalado como %s",
	"Error de instalación: %s",
	"Instalación cancelada",
	"Iniciando %s",
	"%s no se ha iniciado (%s)",
	"R-comp: descargando %d carátula",
	"R-comp: descargando %d carátulas",
};

const char* const kSpanishLatAm[kCount] = {
	"Jugar",
	"Explorar",
	"Saltar",
	"Instalar",
	"Actualizar",
	"Cancelar",
	"Actualizar",
	"No hay juegos recompilados en /data/homebrew ni en /data/rcomp/packages",
	"Descargando portadas  %d / %d",
	"%s GB",
	"%s MB",
	".",
	"Instalado",
	"No instalado",
	"Actualización disponible",
	"Instalando  %s / %s",
	"Instalado como %s",
	"Error de instalación: %s",
	"Instalación cancelada",
	"Iniciando %s",
	"%s no se inició (%s)",
	"R-comp: descargando %d portada",
	"R-comp: descargando %d portadas",
};

const char* const kGerman[kCount] = {
	"Spielen",
	"Blättern",
	"Springen",
	"Installieren",
	"Aktualisieren",
	"Abbrechen",
	"Aktualisieren",
	"Keine rekompilierten Spiele in /data/homebrew oder /data/rcomp/packages",
	"Cover werden geladen  %d / %d",
	"%s GB",
	"%s MB",
	",",
	"Installiert",
	"Nicht installiert",
	"Update verfügbar",
	"Wird installiert  %s / %s",
	"Installiert als %s",
	"Installation fehlgeschlagen: %s",
	"Installation abgebrochen",
	"%s wird gestartet",
	"%s ist nicht gestartet (%s)",
	"R-comp: %d Cover wird geladen",
	"R-comp: %d Cover werden geladen",
};

const char* const kItalian[kCount] = {
	"Gioca",
	"Sfoglia",
	"Salta",
	"Installa",
	"Aggiorna",
	"Annulla",
	"Aggiorna elenco",
	"Nessun gioco ricompilato in /data/homebrew o /data/rcomp/packages",
	"Download copertine  %d / %d",
	"%s GB",
	"%s MB",
	",",
	"Installato",
	"Non installato",
	"Aggiornamento disponibile",
	"Installazione  %s / %s",
	"Installato come %s",
	"Installazione non riuscita: %s",
	"Installazione annullata",
	"Avvio di %s",
	"%s non si è avviato (%s)",
	"R-comp: download di %d copertina",
	"R-comp: download di %d copertine",
};

const char* const kDutch[kCount] = {
	"Spelen",
	"Bladeren",
	"Springen",
	"Installeren",
	"Bijwerken",
	"Annuleren",
	"Vernieuwen",
	"Geen gehercompileerde games in /data/homebrew of /data/rcomp/packages",
	"Covers downloaden  %d / %d",
	"%s GB",
	"%s MB",
	",",
	"Geïnstalleerd",
	"Niet geïnstalleerd",
	"Update beschikbaar",
	"Installeren  %s / %s",
	"Geïnstalleerd als %s",
	"Installatie mislukt: %s",
	"Installatie geannuleerd",
	"%s wordt gestart",
	"%s is niet gestart (%s)",
	"R-comp: %d cover downloaden",
	"R-comp: %d covers downloaden",
};

const char* const kPortuguese[kCount] = {
	"Jogar",
	"Navegar",
	"Saltar",
	"Instalar",
	"Atualizar",
	"Cancelar",
	"Atualizar lista",
	"Nenhum jogo recompilado em /data/homebrew ou /data/rcomp/packages",
	"A transferir capas  %d / %d",
	"%s GB",
	"%s MB",
	",",
	"Instalado",
	"Não instalado",
	"Atualização disponível",
	"A instalar  %s / %s",
	"Instalado como %s",
	"A instalação falhou: %s",
	"Instalação cancelada",
	"A iniciar %s",
	"%s não iniciou (%s)",
	"R-comp: a transferir %d capa",
	"R-comp: a transferir %d capas",
};

const char* const kPortugueseBrazil[kCount] = {
	"Jogar",
	"Navegar",
	"Pular",
	"Instalar",
	"Atualizar",
	"Cancelar",
	"Atualizar lista",
	"Nenhum jogo recompilado em /data/homebrew ou /data/rcomp/packages",
	"Baixando capas  %d / %d",
	"%s GB",
	"%s MB",
	",",
	"Instalado",
	"Não instalado",
	"Atualização disponível",
	"Instalando  %s / %s",
	"Instalado como %s",
	"A instalação falhou: %s",
	"Instalação cancelada",
	"Iniciando %s",
	"%s não iniciou (%s)",
	"R-comp: baixando %d capa",
	"R-comp: baixando %d capas",
};

// The region names x360db gives a disc (a comma-separated list), and the ones PS5SX2 knew.
constexpr int kRegionCount = 19;
const char* const kRegionsEnglish[kRegionCount] = {"USA", "Europe", "Japan", "Korea", "Asia", "World", "Australia",
	"France", "Germany", "Italy", "Spain", "UK", "Canada", "Brazil", "Russia", "China", "Taiwan", "Netherlands", "Sweden"};
const char* const kRegionsSpanish[kRegionCount] = {"EE. UU.", "Europa", "Japón", "Corea", "Asia", "Mundo", "Australia",
	"Francia", "Alemania", "Italia", "España", "Reino Unido", "Canadá", "Brasil", "Rusia", "China", "Taiwán",
	"Países Bajos", "Suecia"};
const char* const kRegionsFrench[kRegionCount] = {"États-Unis", "Europe", "Japon", "Corée", "Asie", "Monde",
	"Australie", "France", "Allemagne", "Italie", "Espagne", "Royaume-Uni", "Canada", "Brésil", "Russie", "Chine",
	"Taïwan", "Pays-Bas", "Suède"};
const char* const kRegionsGerman[kRegionCount] = {"USA", "Europa", "Japan", "Korea", "Asien", "Welt", "Australien",
	"Frankreich", "Deutschland", "Italien", "Spanien", "Großbritannien", "Kanada", "Brasilien", "Russland", "China",
	"Taiwan", "Niederlande", "Schweden"};
const char* const kRegionsItalian[kRegionCount] = {"USA", "Europa", "Giappone", "Corea", "Asia", "Mondo", "Australia",
	"Francia", "Germania", "Italia", "Spagna", "Regno Unito", "Canada", "Brasile", "Russia", "Cina", "Taiwan",
	"Paesi Bassi", "Svezia"};
const char* const kRegionsDutch[kRegionCount] = {"VS", "Europa", "Japan", "Korea", "Azië", "Wereld", "Australië",
	"Frankrijk", "Duitsland", "Italië", "Spanje", "VK", "Canada", "Brazilië", "Rusland", "China", "Taiwan",
	"Nederland", "Zweden"};
const char* const kRegionsPortuguese[kRegionCount] = {"EUA", "Europa", "Japão", "Coreia", "Ásia", "Mundo",
	"Austrália", "França", "Alemanha", "Itália", "Espanha", "Reino Unido", "Canadá", "Brasil", "Rússia", "China",
	"Taiwan", "Países Baixos", "Suécia"};
const char* const kRegionsPortugueseBrazil[kRegionCount] = {"EUA", "Europa", "Japão", "Coreia", "Ásia", "Mundo",
	"Austrália", "França", "Alemanha", "Itália", "Espanha", "Reino Unido", "Canadá", "Brasil", "Rússia", "China",
	"Taiwan", "Holanda", "Suécia"};

struct Language
{
	const char* code;
	const char* const* text;
	const char* const* regions;
};

const Language kLanguages[] = {
	{"en", kEnglish, kRegionsEnglish},
	{"fr", kFrench, kRegionsFrench},
	{"es", kSpanish, kRegionsSpanish},
	{"es-419", kSpanishLatAm, kRegionsSpanish},
	{"de", kGerman, kRegionsGerman},
	{"it", kItalian, kRegionsItalian},
	{"nl", kDutch, kRegionsDutch},
	{"pt", kPortuguese, kRegionsPortuguese},
	{"pt-BR", kPortugueseBrazil, kRegionsPortugueseBrazil},
};

const Language* s_lang = &kLanguages[0];
std::vector<std::string> s_text_over(kCount);
std::vector<std::string> s_region_over(kRegionCount);

// The PS5's language ids (as the PS4's: 0 Japanese, 1 English (US), 2 French, 3 Spanish, 4 German, 5 Italian,
// 6 Dutch, 7 Portuguese, 17 Portuguese (Brazil), 18 English (UK), 20 Spanish (Latin America), 22 French
// (Canada); the others have no table here).
const Language* LanguageFor(int ps5)
{
	switch (ps5)
	{
		case 2:
		case 22:
			return &kLanguages[1];
		case 3:
			return &kLanguages[2];
		case 20:
			return &kLanguages[3];
		case 4:
			return &kLanguages[4];
		case 5:
			return &kLanguages[5];
		case 6:
			return &kLanguages[6];
		case 7:
			return &kLanguages[7];
		case 17:
			return &kLanguages[8];
		default:
			return &kLanguages[0];
	}
}

std::string Trim(const std::string& s)
{
	size_t a = 0, b = s.size();
	while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n'))
		a++;
	while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n'))
		b--;
	return s.substr(a, b - a);
}

// "\n" in a value is a line break.
std::string Unescape(const std::string& s)
{
	std::string out;
	for (size_t i = 0; i < s.size(); i++)
	{
		if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == 'n')
		{
			out += '\n';
			i++;
		}
		else
			out += s[i];
	}
	return out;
}

// The conversions of a printf format, in order ("%d / %d" -> "dd"); an unfinished one gives "?".
std::string Conversions(const char* f)
{
	std::string out;
	for (const char* p = f; *p; p++)
	{
		if (*p != '%')
			continue;
		p++;
		if (*p == '%')
			continue;
		while (*p && std::strchr("-+ #0123456789.lhzjt", *p))
			p++;
		if (!*p)
		{
			out += '?';
			break;
		}
		out += *p;
	}
	return out;
}

void LoadOverrides(const std::string& path)
{
	FILE* f = std::fopen(path.c_str(), "rb");
	if (!f)
		return;
	int used = 0, refused = 0;
	char line[1024];
	while (std::fgets(line, sizeof(line), f))
	{
		std::string l = Trim(line);
		if (l.size() >= 3 && static_cast<unsigned char>(l[0]) == 0xEF && static_cast<unsigned char>(l[1]) == 0xBB &&
			static_cast<unsigned char>(l[2]) == 0xBF)
			l = Trim(l.substr(3)); // a byte-order mark from Notepad
		if (l.empty() || l[0] == '#' || l[0] == ';')
			continue;
		const size_t eq = l.find('=');
		if (eq == std::string::npos)
			continue;
		const std::string key = Trim(l.substr(0, eq));
		const std::string value = Unescape(Trim(l.substr(eq + 1)));
		bool known = false;
		for (int i = 0; i < kCount; i++)
			if (key == kKeys[i])
			{
				known = true;
				if (value.empty() || !SameFormat(kEnglish[i], value.c_str()))
				{
					std::printf("[i18n] %s: %s refused (it has to keep the English text's %%d and %%s, in order)\n", path.c_str(),
						key.c_str());
					refused++;
				}
				else
				{
					s_text_over[i] = value;
					used++;
				}
			}
		if (key.compare(0, 7, "region.") == 0)
			for (int i = 0; i < kRegionCount; i++)
				if (key.compare(7, std::string::npos, kRegionsEnglish[i]) == 0 && !value.empty() && value.find('%') == std::string::npos)
				{
					known = true;
					s_region_over[i] = value;
					used++;
				}
		if (!known)
		{
			std::printf("[i18n] %s: unknown key %s\n", path.c_str(), key.c_str());
			refused++;
		}
	}
	std::fclose(f);
	std::printf("[i18n] %s: %d entries used, %d refused\n", path.c_str(), used, refused);
}
} // namespace

void SetLanguage(int ps5_language, const std::string& lang_dir)
{
	s_lang = LanguageFor(ps5_language);
	for (std::string& s : s_text_over)
		s.clear();
	for (std::string& s : s_region_over)
		s.clear();
	std::printf("[i18n] PS5 language %d: text in %s\n", ps5_language, s_lang->code);
	if (!lang_dir.empty())
		LoadOverrides(lang_dir + "/" + s_lang->code + ".txt");
	std::fflush(stdout);
}

const char* Tr(Str id)
{
	const int i = static_cast<int>(id);
	if (i < 0 || i >= kCount)
		return "";
	if (!s_text_over[static_cast<size_t>(i)].empty())
		return s_text_over[static_cast<size_t>(i)].c_str();
	const char* t = s_lang->text[i];
	return t ? t : kEnglish[i];
}

const char* LanguageCode()
{
	return s_lang->code;
}

const char* Key(Str id)
{
	const int i = static_cast<int>(id);
	return i >= 0 && i < kCount ? kKeys[i] : "";
}

std::string Region(const std::string& region)
{
	std::string out;
	size_t pos = 0;
	while (pos <= region.size())
	{
		size_t comma = region.find(", ", pos);
		if (comma == std::string::npos)
			comma = region.size();
		const std::string token = region.substr(pos, comma - pos);
		std::string shown = token;
		for (int i = 0; i < kRegionCount; i++)
			if (token == kRegionsEnglish[i])
				shown = !s_region_over[static_cast<size_t>(i)].empty() ? s_region_over[static_cast<size_t>(i)] : s_lang->regions[i];
		if (!out.empty())
			out += ", ";
		out += shown;
		pos = comma + 2;
	}
	return out;
}

std::string Size(uint64_t bytes)
{
	char num[32];
	const double gb = static_cast<double>(bytes) / 1e9;
	const bool big = gb >= 1.0;
	if (big)
		std::snprintf(num, sizeof(num), "%.1f", gb);
	else
		std::snprintf(num, sizeof(num), "%.0f", static_cast<double>(bytes) / 1e6);
	std::string n = num;
	const std::string dec = Tr(Str::Decimal);
	const size_t dot = n.find('.');
	if (dot != std::string::npos)
		n.replace(dot, 1, dec);
	char out[64];
	std::snprintf(out, sizeof(out), Tr(big ? Str::SizeGB : Str::SizeMB), n.c_str());
	return out;
}

bool SameFormat(const char* a, const char* b)
{
	return Conversions(a) == Conversions(b);
}
} // namespace fe
