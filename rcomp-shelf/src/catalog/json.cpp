// R-comp shelf: the JSON reader (see json.h).
//
// Copyright (C) 2026 R-comp contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "json.h"

#include <cctype>
#include <cstdint>

namespace rshelf
{
const Json* Json::Get(const char* key) const
{
	for (const Member& m : members)
		if (m.key == key)
			return &m.value;
	return nullptr;
}

std::string Json::Str(const char* key) const
{
	const Json* j = Get(key);
	return j && j->kind == String ? j->str : std::string();
}

namespace
{
class Reader
{
public:
	explicit Reader(const std::string& s) : m_s(s) {}

	bool Document(Json& out)
	{
		if (!Value(out, 0))
			return false;
		Space();
		return m_p == m_s.size();
	}

private:
	static constexpr int kMaxDepth = 32;

	void Space()
	{
		while (m_p < m_s.size() && (m_s[m_p] == ' ' || m_s[m_p] == '\t' || m_s[m_p] == '\n' || m_s[m_p] == '\r'))
			m_p++;
	}
	bool Eat(char c)
	{
		Space();
		if (m_p < m_s.size() && m_s[m_p] == c)
		{
			m_p++;
			return true;
		}
		return false;
	}
	bool Word(const char* w)
	{
		size_t i = 0;
		while (w[i] && m_p + i < m_s.size() && m_s[m_p + i] == w[i])
			i++;
		if (w[i])
			return false;
		m_p += i;
		return true;
	}
	static void Utf8(uint32_t cp, std::string& out)
	{
		if (cp < 0x80)
			out += static_cast<char>(cp);
		else if (cp < 0x800)
		{
			out += static_cast<char>(0xC0 | (cp >> 6));
			out += static_cast<char>(0x80 | (cp & 0x3F));
		}
		else if (cp < 0x10000)
		{
			out += static_cast<char>(0xE0 | (cp >> 12));
			out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
			out += static_cast<char>(0x80 | (cp & 0x3F));
		}
		else
		{
			out += static_cast<char>(0xF0 | (cp >> 18));
			out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
			out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
			out += static_cast<char>(0x80 | (cp & 0x3F));
		}
	}
	bool Hex4(uint32_t& v)
	{
		if (m_p + 4 > m_s.size())
			return false;
		v = 0;
		for (int i = 0; i < 4; i++)
		{
			const char c = m_s[m_p++];
			v <<= 4;
			if (c >= '0' && c <= '9')
				v |= static_cast<uint32_t>(c - '0');
			else if (c >= 'a' && c <= 'f')
				v |= static_cast<uint32_t>(c - 'a' + 10);
			else if (c >= 'A' && c <= 'F')
				v |= static_cast<uint32_t>(c - 'A' + 10);
			else
				return false;
		}
		return true;
	}
	bool String(std::string& out)
	{
		if (!Eat('"'))
			return false;
		out.clear();
		while (m_p < m_s.size())
		{
			const char c = m_s[m_p++];
			if (c == '"')
				return true;
			if (static_cast<unsigned char>(c) < 0x20)
				return false;
			if (c != '\\')
			{
				out += c;
				continue;
			}
			if (m_p >= m_s.size())
				return false;
			const char e = m_s[m_p++];
			switch (e)
			{
				case '"': out += '"'; break;
				case '\\': out += '\\'; break;
				case '/': out += '/'; break;
				case 'b': out += '\b'; break;
				case 'f': out += '\f'; break;
				case 'n': out += '\n'; break;
				case 'r': out += '\r'; break;
				case 't': out += '\t'; break;
				case 'u':
				{
					uint32_t cp;
					if (!Hex4(cp))
						return false;
					if (cp >= 0xD800 && cp < 0xDC00)
					{
						uint32_t lo;
						if (!Word("\\u") || !Hex4(lo) || lo < 0xDC00 || lo >= 0xE000)
							return false;
						cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
					}
					Utf8(cp, out);
					break;
				}
				default: return false;
			}
		}
		return false;
	}
	bool Value(Json& out, int depth)
	{
		if (depth > kMaxDepth)
			return false;
		Space();
		if (m_p >= m_s.size())
			return false;
		const char c = m_s[m_p];
		if (c == '"')
		{
			out.kind = Json::String;
			return String(out.str);
		}
		if (c == '{')
		{
			m_p++;
			out.kind = Json::Object;
			if (Eat('}'))
				return true;
			do
			{
				std::string key;
				Json v;
				if (!String(key) || !Eat(':') || !Value(v, depth + 1))
					return false;
				out.members.push_back({std::move(key), std::move(v)});
			} while (Eat(','));
			return Eat('}');
		}
		if (c == '[')
		{
			m_p++;
			out.kind = Json::Array;
			if (Eat(']'))
				return true;
			do
			{
				Json v;
				if (!Value(v, depth + 1))
					return false;
				out.items.push_back(std::move(v));
			} while (Eat(','));
			return Eat(']');
		}
		if (Word("true") || Word("false"))
		{
			out.kind = Json::Bool;
			return true;
		}
		if (Word("null"))
		{
			out.kind = Json::Null;
			return true;
		}
		// A number: kept as its text.
		const size_t start = m_p;
		while (m_p < m_s.size() && (std::isdigit(static_cast<unsigned char>(m_s[m_p])) || m_s[m_p] == '-' || m_s[m_p] == '+' ||
									   m_s[m_p] == '.' || m_s[m_p] == 'e' || m_s[m_p] == 'E'))
			m_p++;
		if (m_p == start)
			return false;
		out.kind = Json::Number;
		out.str = m_s.substr(start, m_p - start);
		return true;
	}

	const std::string& m_s;
	size_t m_p = 0;
};
} // namespace

bool ParseJson(const std::string& text, Json& out)
{
	out = Json();
	return Reader(text).Document(out);
}
} // namespace rshelf
