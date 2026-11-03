/*=========================================================================

  Program:   ITK-SNAP
  Module:    TraceLog.cxx
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

=========================================================================*/
#include "TraceLog.h"
#include "LabelImageWrapper.h"
#include "ImageWrapperBase.h"
#include "SegmentationAuditRecord.h"
#include "UndoDataManager.h"
#include "itksys/SystemTools.hxx"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <random>
#include <sstream>

TraceLog *TraceLog::s_Active = nullptr;

namespace
{

std::string MakeSessionId()
{
  std::random_device rd;
  std::ostringstream oss;
  oss << std::hex << std::setfill('0');
  for (int i = 0; i < 4; i++)
    oss << std::setw(4) << (rd() & 0xffff);
  return oss.str();
}

std::string CompactUtcTimestamp()
{
  std::time_t now = std::time(nullptr);
  std::tm tm_utc;
#ifdef _WIN32
  gmtime_s(&tm_utc, &now);
#else
  gmtime_r(&now, &tm_utc);
#endif
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm_utc);
  return buf;
}

void MergeInto(Json::Value &target, const Json::Value &fields)
{
  if (fields.isObject())
    for (const std::string &name : fields.getMemberNames())
      target[name] = fields[name];
}

Json::Value LabelCountsToJSON(const std::map<LabelType, unsigned long> &counts)
{
  Json::Value v(Json::objectValue);
  for (const auto &kv : counts)
    v[std::to_string(kv.first)] = (Json::UInt64)kv.second;
  return v;
}

// Builds the run list of EncodeChange as JSON text, merging neighbouring
// voxels that changed the same way (or did not change), and counts the labels
// of the changed voxels
class RunEncoder
{
public:
  RunEncoder(TraceLog::EncodedChange &change) : m_Change(change)
  {
    m_Change.runs = "[";
  }

  void Push(bool changed, LabelType old_label, LabelType new_label)
  {
    if (m_Length > 0 && changed == m_Changed &&
        (!changed || (old_label == m_Old && new_label == m_New)))
    {
      ++m_Length;
    }
    else
    {
      Flush();
      m_Length = 1;
      m_Changed = changed;
      m_Old = old_label;
      m_New = new_label;
    }
  }

  void Finish()
  {
    Flush();
    m_Change.runs += ']';
  }

private:
  void Flush()
  {
    if (m_Length == 0)
      return;
    std::string &out = m_Change.runs;
    out += out.size() > 1 ? ",[" : "[";
    Append(m_Length);
    if (m_Changed)
    {
      out += ',';
      Append(m_Old);
      out += ',';
      Append(m_New);
      m_Change.before[m_Old] += m_Length;
      m_Change.after[m_New] += m_Length;
      m_Change.changed += m_Length;
    }
    out += ']';
    m_Length = 0;
  }

  void Append(unsigned long v)
  {
    char buf[24];
    auto res = std::to_chars(buf, buf + sizeof(buf), v);
    m_Change.runs.append(buf, res.ptr);
  }

  TraceLog::EncodedChange &m_Change;
  unsigned long            m_Length = 0;
  bool                     m_Changed = false;
  LabelType                m_Old = 0, m_New = 0;
};

} // namespace

TraceLog::TraceLog() {}

TraceLog::~TraceLog()
{
  this->Stop();
}

bool
TraceLog::Start(const std::string &dir, const std::string &user, const std::string &app_version)
{
  std::lock_guard<std::recursive_mutex> lock(m_Mutex);
  this->Stop();

  itksys::SystemTools::MakeDirectory(dir);
  m_SessionId = MakeSessionId();
  m_FileName = dir + "/trace-" + CompactUtcTimestamp() + "-" + m_SessionId.substr(0, 8) + ".jsonl";

  // Binary, so that lines end in \n on every platform
  m_Stream.open(m_FileName.c_str(), std::ios::out | std::ios::trunc | std::ios::binary);
  if (!m_Stream.is_open())
    return false;

  m_User = user;
  m_WriteError.clear();
  m_NumberOfEvents = 0;
  m_StartTime = std::chrono::steady_clock::now();
  m_SessionLine.clear();
  m_ImageOffset = 0;
  m_Pending.clear();
  m_LastState.clear();
  m_Context.clear();
  m_KnownLayers.clear();
  s_Active = this;

  Json::Value f(Json::objectValue);
  f["format"] = FormatName();
  f["version"] = FORMAT_VERSION;
  f["session"] = m_SessionId;
  f["user"] = user;
  f["app"] = app_version;
  f["started"] = SegmentationAuditRecord::NowIso8601Utc();
  this->LogLocked("session", f);
  return this->IsRecording();
}

void
TraceLog::Stop()
{
  std::lock_guard<std::recursive_mutex> lock(m_Mutex);
  if (!m_Stream.is_open())
    return;

  this->FlushPendingLocked();
  Json::Value ev(Json::objectValue);
  ev["type"] = "stop";
  this->WriteLocked(ev);
  m_Stream.close();
  m_Context.clear();

  if (s_Active == this)
    s_Active = nullptr;
}

void
TraceLog::FailLocked(const std::string &what)
{
  m_WriteError = what;
  m_Stream.close();
  m_Pending.clear();
  m_Context.clear();
  if (s_Active == this)
    s_Active = nullptr;

  // Let the user interface know that recording stopped
  this->InvokeEvent(itk::ModifiedEvent());
}

unsigned long long
TraceLog::ElapsedMs() const
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
           std::chrono::steady_clock::now() - m_StartTime).count();
}

void
TraceLog::WriteLocked(Json::Value &event, const char *raw_key, const std::string *raw_value)
{
  if (!m_Stream.is_open())
    return;

  event["seq"] = (Json::UInt64)(++m_NumberOfEvents);
  if (!event.isMember("ms"))
    event["ms"] = (Json::UInt64)ElapsedMs();

  // FastWriter writes one line, ending in "}\n". A raw member goes in last.
  Json::FastWriter writer;
  std::string      line = writer.write(event);
  if (raw_key && raw_value)
  {
    line.resize(line.size() - 2);
    line += ",\"";
    line += raw_key;
    line += "\":";
    line += *raw_value;
    line += "}\n";
  }

  // Remember where the current main image starts, and the session line, for
  // the copy saved next to a segmentation
  std::string type = event["type"].asString();
  if (type == "session")
    m_SessionLine = line;
  else if (type == "image" && event["loaded"].asBool())
    m_ImageOffset = m_Stream.tellp();

  // Flush every event so that the log survives a crash
  m_Stream << line;
  m_Stream.flush();
  if (!m_Stream.good())
    this->FailLocked("Could not write the trace log " + m_FileName +
                     ". Recording has stopped.");
}

void
TraceLog::LogLocked(const std::string &type,
                    const Json::Value &fields,
                    const char        *raw_key,
                    const std::string *raw_value)
{
  if (!this->IsRecording())
    return;

  this->FlushPendingLocked();
  Json::Value ev(Json::objectValue);
  MergeInto(ev, fields);
  ev["type"] = type;

  // Say where the event came from (the view and action), if anyone said
  if (!ev.isMember("ctx"))
  {
    Json::Value ctx = this->GetContext();
    if (!ctx.isNull())
      ev["ctx"] = ctx;
  }
  this->WriteLocked(ev, raw_key, raw_value);
}

void
TraceLog::Log(const std::string &type, const Json::Value &fields)
{
  std::lock_guard<std::recursive_mutex> lock(m_Mutex);
  this->LogLocked(type, fields);
}

void
TraceLog::LogStateChange(const std::string &type,
                         const std::string &key,
                         const Json::Value &state,
                         const Json::Value &fields)
{
  std::lock_guard<std::recursive_mutex> lock(m_Mutex);
  if (!this->IsRecording())
    return;

  auto it = m_LastState.find(key);
  if (it != m_LastState.end() && it->second == state)
    return;

  Json::Value from = (it != m_LastState.end()) ? it->second : Json::Value();
  m_LastState[key] = state;

  unsigned long long now = ElapsedMs();
  for (PendingChange &p : m_Pending)
  {
    if (p.key == key)
    {
      if (p.event["type"].asString() == type && now - p.last_ms <= COALESCE_MS)
      {
        p.event["to"] = state;
        p.event["n"] = p.event["n"].asUInt() + 1;
        p.event["ms_end"] = (Json::UInt64)now;
        p.last_ms = now;
        return;
      }

      // This key's gesture is over: write it, and the others that started
      // before it, so that events stay in order
      this->FlushPendingLocked();
      break;
    }
  }

  PendingChange p;
  p.key = key;
  p.last_ms = now;
  p.event = Json::Value(Json::objectValue);
  MergeInto(p.event, fields);
  p.event["type"] = type;
  p.event["from"] = from;
  p.event["to"] = state;
  p.event["n"] = 1;
  p.event["ms"] = (Json::UInt64)now;
  p.event["ms_end"] = (Json::UInt64)now;

  // The context (such as the view being dragged) is the one at the start of
  // the gesture
  Json::Value ctx = this->GetContext();
  if (!ctx.isNull())
    p.event["ctx"] = ctx;

  m_Pending.push_back(p);
}

void
TraceLog::PrimeState(const std::string &key, const Json::Value &state)
{
  std::lock_guard<std::recursive_mutex> lock(m_Mutex);
  m_LastState[key] = state;
}

void
TraceLog::FlushPendingState()
{
  std::lock_guard<std::recursive_mutex> lock(m_Mutex);
  this->FlushPendingLocked();
}

void
TraceLog::FlushPendingLocked()
{
  std::vector<PendingChange> pending;
  pending.swap(m_Pending);
  for (PendingChange &p : pending)
    this->WriteLocked(p.event);
}

TraceLog::ScopedContext::ScopedContext(const Json::Value &ctx)
  : m_Log(ctx.isNull() ? nullptr : TraceLog::GetActive())
{
  if (m_Log)
  {
    std::lock_guard<std::recursive_mutex> lock(m_Log->m_Mutex);
    m_Log->m_Context.push_back(ctx);
  }
}

TraceLog::ScopedContext::~ScopedContext()
{
  if (m_Log)
  {
    std::lock_guard<std::recursive_mutex> lock(m_Log->m_Mutex);
    if (!m_Log->m_Context.empty())
      m_Log->m_Context.pop_back();
  }
}

Json::Value
TraceLog::GetContext() const
{
  std::lock_guard<std::recursive_mutex> lock(m_Mutex);
  if (m_Context.empty())
    return Json::Value();

  Json::Value merged(Json::objectValue);
  for (const Json::Value &c : m_Context)
    MergeInto(merged, c);
  return merged;
}

Json::Value
TraceLog::RegionToJSON(const RegionType &region)
{
  Json::Value v(Json::objectValue), index(Json::arrayValue), size(Json::arrayValue);
  for (unsigned int d = 0; d < 3; d++)
  {
    index.append((Json::Int64)region.GetIndex(d));
    size.append((Json::UInt64)region.GetSize(d));
  }
  v["index"] = index;
  v["size"] = size;
  return v;
}

void
TraceLog::EncodeChange(const LabelImage *image,
                       const DeltaList  &deltas,
                       int               sign,
                       EncodedChange    &change)
{
  change = EncodedChange();
  RunEncoder enc(change);
  if (deltas.empty())
  {
    enc.Finish();
    return;
  }

  // Bounding region of all the deltas
  itk::Index<3> lo = deltas.front()->GetRegion().GetIndex();
  itk::Index<3> hi = deltas.front()->GetRegion().GetUpperIndex();
  for (Delta *delta : deltas)
  {
    for (unsigned int d = 0; d < 3; d++)
    {
      lo[d] = std::min(lo[d], delta->GetRegion().GetIndex(d));
      hi[d] = std::max(hi[d], delta->GetRegion().GetUpperIndex()[d]);
    }
  }
  RegionType &region = change.region;
  region.SetIndex(lo);
  region.SetUpperIndex(hi);

  // The image already holds the new labels; a delta holds new - old (applied
  // with the given sign), so old = new - sign * delta
  auto old_label = [sign](LabelType new_label, LabelType d) -> LabelType {
    return (LabelType)(sign > 0 ? new_label - d : new_label + d);
  };

  typedef itk::ImageRegionConstIterator<LabelImage> IterType;
  if (deltas.size() == 1)
  {
    // Common case: walk the delta and the image together
    Delta   *delta = deltas.front();
    IterType it(image, region);
    for (size_t i = 0; i < delta->GetNumberOfRLEs(); i++)
    {
      size_t    n = delta->GetRLELength(i);
      LabelType d = delta->GetRLEValue(i);
      for (size_t j = 0; j < n && !it.IsAtEnd(); j++, ++it)
      {
        if (d == 0)
          enc.Push(false, 0, 0);
        else
        {
          LabelType new_label = it.Get();
          enc.Push(true, old_label(new_label, d), new_label);
        }
      }
    }
  }
  else
  {
    // Several deltas, as in a paintbrush drag or a 3D spray: sum them per
    // voxel, so that a voxel changed twice is reported once with its net
    // change (labels wrap around, so the sum is exact). Only changed voxels
    // are stored, as (offset in the bounding region, delta), because spray
    // points can be far apart and their bounding region large.
    const size_t sx = region.GetSize(0), sy = region.GetSize(1);
    std::vector<std::pair<size_t, LabelType>> changes;
    for (Delta *delta : deltas)
    {
      const RegionType &dr = delta->GetRegion();
      const size_t dx = dr.GetSize(0), dy = dr.GetSize(1);
      const size_t ox = dr.GetIndex(0) - lo[0], oy = dr.GetIndex(1) - lo[1],
                   oz = dr.GetIndex(2) - lo[2];
      size_t k = 0;
      for (size_t i = 0; i < delta->GetNumberOfRLEs(); i++)
      {
        size_t    n = delta->GetRLELength(i);
        LabelType d = delta->GetRLEValue(i);
        if (d == 0)
        {
          k += n;
          continue;
        }
        for (size_t j = 0; j < n; j++, k++)
        {
          size_t x = k % dx, y = (k / dx) % dy, z = k / (dx * dy);
          changes.push_back(std::make_pair(((z + oz) * sy + (y + oy)) * sx + (x + ox), d));
        }
      }
    }

    // Sort by offset and sum the deltas of each voxel
    std::sort(changes.begin(), changes.end(),
              [](const std::pair<size_t, LabelType> &a, const std::pair<size_t, LabelType> &b) {
                return a.first < b.first;
              });
    size_t m = 0;
    for (size_t i = 0; i < changes.size(); i++)
    {
      if (m > 0 && changes[m - 1].first == changes[i].first)
        changes[m - 1].second += changes[i].second;
      else
        changes[m++] = changes[i];
    }
    changes.resize(m);

    IterType it(image, region);
    size_t   next = 0;
    for (size_t off = 0; !it.IsAtEnd(); ++it, ++off)
    {
      LabelType d = 0;
      if (next < changes.size() && changes[next].first == off)
        d = changes[next++].second;

      if (d == 0)
        enc.Push(false, 0, 0);
      else
      {
        LabelType new_label = it.Get();
        enc.Push(true, old_label(new_label, d), new_label);
      }
    }
  }

  enc.Finish();
}

void
TraceLog::LogSegmentationChange(const char                    *kind,
                                unsigned long                  layer,
                                unsigned int                   tp,
                                const LabelImage              *image,
                                const DeltaList               &deltas,
                                int                            sign,
                                const std::string             &op,
                                const SegmentationAuditRecord *audit)
{
  std::lock_guard<std::recursive_mutex> lock(m_Mutex);
  if (!this->IsRecording())
    return;

  EncodedChange change;
  EncodeChange(image, deltas, sign, change);

  // A commit whose voxels all ended where they started (e.g. painted and
  // erased in one stroke) changes nothing
  if (change.changed == 0)
    return;

  // The label counts come from the net change. (The audit record's counts
  // walk each delta separately, which miscounts voxels that overlapping
  // deltas changed twice.)
  Json::Value ev(Json::objectValue);
  ev["layer"] = (Json::UInt64)layer;
  ev["tp"] = tp;
  ev["op"] = op;
  ev["region"] = RegionToJSON(change.region);
  ev["changed"] = (Json::UInt64)change.changed;
  ev["before"] = LabelCountsToJSON(change.before);
  ev["after"] = LabelCountsToJSON(change.after);
  if (audit)
    ev["actor"] = SegmentationAuditRecord::ActorToString(audit->actor);

  this->LogLocked(kind, ev, "runs", &change.runs);
}

void
TraceLog::LogLayer(LabelImageWrapper *seg, const char *kind)
{
  std::lock_guard<std::recursive_mutex> lock(m_Mutex);
  if (!this->IsRecording() || !seg)
    return;

  Json::Value ev(Json::objectValue), size(Json::arrayValue);
  Vector3ui   sz = seg->GetSize();
  for (unsigned int d = 0; d < 3; d++)
    size.append(sz[d]);
  ev["layer"] = (Json::UInt64)seg->GetUniqueId();
  ev["kind"] = kind;
  ev["size"] = size;
  ev["tps"] = seg->GetNumberOfTimePoints();
  ev["checksum"] = ComputeChecksum(seg);

  m_KnownLayers.insert(seg->GetUniqueId());
  this->LogLocked("layer", ev);
}

void
TraceLog::TraceLayers(const std::vector<LabelImageWrapper *> &layers, LabelImageWrapper *loaded)
{
  std::lock_guard<std::recursive_mutex> lock(m_Mutex);
  if (!this->IsRecording())
    return;

  std::set<unsigned long> current;
  for (LabelImageWrapper *seg : layers)
  {
    current.insert(seg->GetUniqueId());
    if (seg == loaded)
      this->LogLayer(seg, "loaded");
    else if (!m_KnownLayers.count(seg->GetUniqueId()))
      this->LogLayer(seg, "added");
  }

  std::set<unsigned long> known = m_KnownLayers;
  for (unsigned long id : known)
  {
    if (!current.count(id))
    {
      m_KnownLayers.erase(id);
      Json::Value ev(Json::objectValue);
      ev["layer"] = (Json::UInt64)id;
      this->LogLocked("layer_removed", ev);
    }
  }
}

void
TraceLog::LogUndoCleared(unsigned long layer)
{
  Json::Value ev(Json::objectValue);
  ev["layer"] = (Json::UInt64)layer;
  this->Log("undo_cleared", ev);
}

void
TraceLog::LogMainImage(ImageWrapperBase *main)
{
  Json::Value ev(Json::objectValue);
  ev["loaded"] = (main != nullptr);
  if (main)
  {
    Json::Value size(Json::arrayValue), spacing(Json::arrayValue), origin(Json::arrayValue),
      direction(Json::arrayValue);
    Vector3ui                        sz = main->GetSize();
    const ImageWrapperBase::ImageBaseType *ib = main->GetImageBase();
    for (unsigned int d = 0; d < 3; d++)
    {
      size.append(sz[d]);
      spacing.append(ib->GetSpacing()[d]);
      origin.append(ib->GetOrigin()[d]);
      for (unsigned int e = 0; e < 3; e++)
        direction.append(ib->GetDirection()(d, e));
    }
    ev["size"] = size;
    ev["tps"] = main->GetNumberOfTimePoints();
    ev["spacing"] = spacing;
    ev["origin"] = origin;
    ev["direction"] = direction;
  }

  // Segmentation layers get their starting point again after the image event,
  // even if they were seen while the image was loading, since the copy of the
  // log saved with a segmentation starts at the image event
  std::lock_guard<std::recursive_mutex> lock(m_Mutex);
  this->LogLocked("image", ev);
  m_KnownLayers.clear();
}

bool
TraceLog::LogSegmentationSaved(LabelImageWrapper *seg, const std::string &fname)
{
  std::lock_guard<std::recursive_mutex> lock(m_Mutex);
  if (!this->IsRecording() || !seg)
    return true;

  Json::Value ev(Json::objectValue);
  ev["layer"] = (Json::UInt64)seg->GetUniqueId();
  ev["tps"] = seg->GetNumberOfTimePoints();
  ev["checksum"] = ComputeChecksum(seg);
  this->LogLocked("save", ev);
  if (!this->IsRecording())
    return false;

  // The copy: the session line, then everything from the event of the
  // current main image on
  std::ifstream in(m_FileName.c_str(), std::ios::in | std::ios::binary);
  std::ofstream out(SidecarFileName(fname).c_str(),
                    std::ios::out | std::ios::trunc | std::ios::binary);
  if (!in.good() || !out.good())
    return false;
  if (m_ImageOffset > 0)
  {
    out << m_SessionLine;
    in.seekg(m_ImageOffset);
  }
  out << in.rdbuf();
  out.flush();
  return out.good();
}

std::string
TraceLog::ComputeChecksum(LabelImageWrapper *seg)
{
  ChecksumHasher hasher;
  for (unsigned int t = 0; t < seg->GetNumberOfTimePoints(); t++)
    AddToChecksum(hasher, seg->GetImageByTimePoint(t).GetPointer());
  return FormatChecksum(hasher.GetHash());
}

void
TraceLog::AddToChecksum(ChecksumHasher &hasher, const LabelImage *image)
{
  // Each pixel of the RLE buffer is one row, stored as runs
  typedef LabelImage::BufferType BufferType;
  const BufferType *buffer = image->GetBuffer();
  for (itk::ImageRegionConstIterator<BufferType> it(buffer, buffer->GetBufferedRegion());
       !it.IsAtEnd(); ++it)
  {
    for (const auto &segment : it.Get())
      hasher.Add(segment.second, (std::uint32_t)segment.first);
    hasher.EndRow();
  }
}

std::string
TraceLog::FormatChecksum(unsigned long long hash)
{
  char buf[64];
  snprintf(buf, sizeof(buf), "fnv1a64:%016llx", hash);
  return buf;
}

std::string
TraceLog::SidecarFileName(const std::string &seg_file)
{
  return seg_file + ".trace.jsonl";
}
