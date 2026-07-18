/*=========================================================================

  Program:   ITK-SNAP
  Module:    SegmentationAuditRecord.cxx
  Language:  C++
  Copyright (c) 2026 Paul A. Yushkevich

  This file is part of ITK-SNAP

  ITK-SNAP is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.

=========================================================================*/
#include "SegmentationAuditRecord.h"

#include <chrono>
#include <ctime>
#include <cstdio>
#include <sstream>

namespace
{

// Escape a string for embedding in a JSON string literal (RFC 8259).
std::string EscapeJsonString(const std::string &in)
{
  std::string out;
  out.reserve(in.size() + 2);
  for (unsigned char c : in)
  {
    switch (c)
    {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b";  break;
      case '\f': out += "\\f";  break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break;
      default:
        if (c < 0x20)
        {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        }
        else
        {
          out += static_cast<char>(c);
        }
    }
  }
  return out;
}

// Serialize a label->count histogram as a JSON object with string keys.
std::string CountsToJson(const std::map<LabelType, unsigned long> &counts)
{
  std::ostringstream ss;
  ss << '{';
  bool first = true;
  for (const auto &kv : counts)
  {
    if (!first)
      ss << ',';
    first = false;
    ss << '"' << static_cast<unsigned long>(kv.first) << "\":" << kv.second;
  }
  ss << '}';
  return ss.str();
}

} // anonymous namespace

const char *
SegmentationAuditRecord::ActorToString(Actor a)
{
  switch (a)
  {
    case HUMAN: return "human";
    case AGENT: return "agent";
    default:    return "unknown";
  }
}

SegmentationAuditRecord::Actor
SegmentationAuditRecord::ActorFromString(const std::string &s)
{
  if (s == "human" || s == "HUMAN")
    return HUMAN;
  if (s == "agent" || s == "AGENT")
    return AGENT;
  return UNKNOWN;
}

std::string
SegmentationAuditRecord::NowIso8601Utc()
{
  std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm     tm_utc{};
#ifdef _WIN32
  gmtime_s(&tm_utc, &t);
#else
  gmtime_r(&t, &tm_utc);
#endif
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
  return std::string(buf);
}

std::string
SegmentationAuditRecord::ToJSON() const
{
  std::ostringstream ss;
  ss << '{';
  ss << "\"op\":\"" << EscapeJsonString(op) << "\",";
  ss << "\"timestamp\":\"" << EscapeJsonString(timestamp) << "\",";
  ss << "\"actor\":\"" << ActorToString(actor) << "\",";
  ss << "\"time_point\":" << time_point << ',';
  ss << "\"changed_voxels\":" << changed_voxels << ',';
  ss << "\"rle_count\":" << rle_count << ',';

  ss << "\"bbox\":{\"valid\":" << (bbox_valid ? "true" : "false");
  if (bbox_valid)
  {
    ss << ",\"min\":[" << bbox_min[0] << ',' << bbox_min[1] << ',' << bbox_min[2] << ']';
    ss << ",\"max\":[" << bbox_max[0] << ',' << bbox_max[1] << ',' << bbox_max[2] << ']';
  }
  ss << "},";

  ss << "\"before_counts\":" << CountsToJson(before_counts) << ',';
  ss << "\"after_counts\":" << CountsToJson(after_counts);
  ss << '}';
  return ss.str();
}
