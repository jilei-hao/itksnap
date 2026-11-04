/*=========================================================================

  Program:   ITK-SNAP
  Module:    TraceLogReplay.cxx
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
#include "TraceLogReplay.h"
#include "TraceLog.h"
#include "IRISException.h"
#include "itkImageRegionIterator.h"

#include <algorithm>
#include <cstdio>
#include <fstream>

namespace
{

// Parse the runs of a step ([[n],[n,old,new],...]) or the content of a layer
// ([[n,label],...]), as TraceLog writes them, calling add(v, k) for each
// tuple of k numbers. Faster and much smaller than a JSON tree for millions
// of runs. Returns false if the text is not in that form, or add refuses.
template <class TAdd>
bool ParseTuples(const char *p, const char *end, TAdd add)
{
  auto skip = [&]() { while (p < end && (*p == ' ' || *p == '\t')) ++p; };
  auto number = [&](unsigned long long &v) -> bool {
    skip();
    const char *start = p;
    v = 0;
    while (p < end && *p >= '0' && *p <= '9')
      v = v * 10 + (unsigned long long)(*p++ - '0');
    return p > start;
  };
  auto expect = [&](char c) -> bool {
    skip();
    if (p < end && *p == c)
    {
      ++p;
      return true;
    }
    return false;
  };

  if (!expect('['))
    return false;
  if (expect(']'))
    return true;
  do
  {
    unsigned long long v[3];
    int                k = 0;
    if (!expect('[') || !number(v[k++]))
      return false;
    while (expect(','))
      if (k == 3 || !number(v[k++]))
        return false;
    if (!expect(']') || !add(v, k))
      return false;
  } while (expect(','));
  if (!expect(']'))
    return false;

  // Nothing may follow, or the runs were not the last member
  skip();
  return p == end;
}

} // namespace

void
TraceLogReplay::Read(const std::string &file)
{
  std::ifstream in(file.c_str(), std::ios::in | std::ios::binary);
  if (!in.good())
    throw IRISException("Error: Cannot read the trace log. Could not open %s.", file.c_str());

  *this = TraceLogReplay();

  Json::Reader  reader;
  std::string   line;
  unsigned long line_no = 0;

  // Layer content by the event that holds it (see TraceLog::LogLayer)
  std::map<unsigned long, std::vector<std::pair<unsigned long, LabelType>>> contents_by_seq;
  while (std::getline(in, line))
  {
    ++line_no;
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (line.empty())
      continue;

    // TraceLog writes the runs of a step, or the content of a layer, last;
    // read them separately from the rest of the event when they are
    static const std::string runs_key = ",\"runs\":", content_key = ",\"content\":";
    std::vector<Run>                               runs;
    std::vector<std::pair<unsigned long, LabelType>> content;
    bool   have_runs = false, have_content = false;
    size_t pos = line.rfind(runs_key), key_size = runs_key.size();
    size_t cpos = line.rfind(content_key);
    bool   is_content = cpos != std::string::npos && (pos == std::string::npos || cpos > pos);
    if (is_content)
    {
      pos = cpos;
      key_size = content_key.size();
    }
    if (pos != std::string::npos && line.back() == '}')
    {
      const char *begin = line.data() + pos + key_size, *end = line.data() + line.size() - 1;
      bool        ok;
      if (is_content)
        ok = ParseTuples(begin, end, [&](const unsigned long long *v, int k) {
          content.push_back(std::make_pair((unsigned long)v[0], (LabelType)v[1]));
          return k == 2;
        });
      else
        ok = ParseTuples(begin, end, [&](const unsigned long long *v, int k) {
          Run run;
          run.length = (unsigned long)v[0];
          if (k == 3)
          {
            run.changed = true;
            run.old_label = (LabelType)v[1];
            run.new_label = (LabelType)v[2];
          }
          runs.push_back(run);
          return k != 2;
        });

      if (ok)
      {
        line.resize(pos);
        line += '}';
        (is_content ? have_content : have_runs) = true;
      }
      else
      {
        runs.clear();
        content.clear();
      }
    }

    Json::Value ev;
    if (!reader.parse(line, ev, false) || !ev.isObject())
    {
      // A crash can leave the last line unfinished; ignore it
      if (in.peek() == EOF)
        break;
      throw IRISException("Error: Invalid trace log. Line %lu of %s is not valid JSON.",
                          line_no, file.c_str());
    }

    std::string   type = ev["type"].asString();
    unsigned long seq = (unsigned long)ev["seq"].asUInt64();
    unsigned long layer = (unsigned long)ev["layer"].asUInt64();
    m_EventCounts[type]++;
    m_LastMs = std::max(m_LastMs, (unsigned long long)ev["ms"].asUInt64());

    if (line_no == 1)
    {
      if (type != "session" || ev["format"].asString() != TraceLog::FormatName())
        throw IRISException("Error: Not a trace log. %s does not start with an ITK-SNAP "
                            "trace log header.", file.c_str());
      if (ev["version"].asInt() > TraceLog::FORMAT_VERSION)
        throw IRISException("Error: Trace log too new. %s uses version %d of the format; "
                            "this ITK-SNAP reads version %d.", file.c_str(),
                            ev["version"].asInt(), TraceLog::FORMAT_VERSION);
      m_Session = ev;
    }
    else if (type == "image")
    {
      if (ev["loaded"].asBool())
      {
        m_Image = ev;
        m_HasImage = true;
      }
    }
    else if (type == "layer")
    {
      LayerState ls;
      ls.seq = seq;
      ls.kind = ev["kind"].asString();
      ls.checksum = ev["checksum"].asString();
      ls.tps = ev["tps"].asUInt();
      for (unsigned int d = 0; d < 3; d++)
        ls.size[d] = ev["size"][d].asUInt();

      // Logs from before "blank" was recorded: a blank layer has the checksum
      // of a blank segmentation of its size
      ls.blank = ev.isMember("blank") ? ev["blank"].asBool()
                                      : ls.checksum == BlankChecksum(ls.size, ls.tps);
      if (have_content)
      {
        ls.content.swap(content);
        contents_by_seq[seq] = ls.content;
      }
      else if (ev.isMember("content_seq"))
      {
        auto it = contents_by_seq.find((unsigned long)ev["content_seq"].asUInt64());
        if (it == contents_by_seq.end())
          throw IRISException("Error: Invalid trace log. Event %lu refers to the content of "
                              "event %lu, which is not in the log.",
                              seq, (unsigned long)ev["content_seq"].asUInt64());
        ls.content = it->second;
      }
      else
      {
        const Json::Value &jc = ev["content"];
        for (Json::ArrayIndex i = 0; i < jc.size(); i++)
          ls.content.push_back(
            std::make_pair((unsigned long)jc[i][0].asUInt64(), (LabelType)jc[i][1].asUInt()));
      }

      unsigned long long total = 0, expected = (unsigned long long)ls.size[0] * ls.size[1] *
                                                 ls.size[2] * std::max(1u, ls.tps);
      for (const auto &run : ls.content)
        total += run.first;
      if (!ls.content.empty() && total != expected)
        throw IRISException("Error: Invalid trace log. The content of layer %lu in event %lu "
                            "covers %llu voxels, but the layer has %llu.",
                            layer, seq, total, expected);
      m_LayerStates[layer].push_back(ls);
    }
    else if (type == "save")
    {
      LayerState ls;
      ls.seq = seq;
      ls.kind = "save";
      ls.checksum = ev["checksum"].asString();
      ls.tps = ev["tps"].asUInt();
      m_Saves.push_back(std::make_pair(layer, ls));
    }
    else if (type == "edit" || type == "undo" || type == "redo")
    {
      Step s;
      s.seq = seq;
      s.kind = type;
      s.op = ev["op"].asString();
      s.tp = ev["tp"].asUInt();
      s.changed = (unsigned long)ev["changed"].asUInt64();
      const Json::Value &r = ev["region"];
      for (unsigned int d = 0; d < 3; d++)
      {
        s.region.SetIndex(d, (itk::IndexValueType)r["index"][d].asInt64());
        s.region.SetSize(d, (itk::SizeValueType)r["size"][d].asUInt64());
      }

      if (have_runs)
      {
        s.runs.swap(runs);
      }
      else
      {
        const Json::Value &jruns = ev["runs"];
        s.runs.reserve(jruns.size());
        for (Json::ArrayIndex i = 0; i < jruns.size(); i++)
        {
          const Json::Value &jr = jruns[i];
          Run run;
          run.length = (unsigned long)jr[0].asUInt64();
          if (jr.size() == 3)
          {
            run.changed = true;
            run.old_label = (LabelType)jr[1].asUInt();
            run.new_label = (LabelType)jr[2].asUInt();
          }
          s.runs.push_back(run);
        }
      }

      unsigned long total = 0;
      for (const Run &run : s.runs)
        total += run.length;
      if (total != s.region.GetNumberOfPixels())
        throw IRISException("Error: Invalid trace log. Event %lu covers %lu voxels, but its "
                            "region has %lu.", seq, total,
                            (unsigned long)s.region.GetNumberOfPixels());

      m_AllSteps.push_back(std::make_pair(layer, s));
    }
  }

  if (m_Session.isNull())
    throw IRISException("Error: Not a trace log. %s is empty.", file.c_str());

  // Replay the layer saved last; failing that, the layer edited last. Only
  // layers with a starting point can be replayed, which leaves out the snake
  // mode's own layers.
  if (!m_Saves.empty())
    m_TargetLayer = m_Saves.back().first;
  else
  {
    for (const auto &ls : m_AllSteps)
      if (m_LayerStates.count(ls.first))
        m_TargetLayer = ls.first;
    if (!m_TargetLayer && !m_LayerStates.empty())
      m_TargetLayer = m_LayerStates.begin()->first;
  }

  this->Select();
}

std::vector<unsigned long>
TraceLogReplay::GetLayers() const
{
  std::vector<unsigned long> layers;
  for (const auto &kv : m_LayerStates)
    layers.push_back(kv.first);
  return layers;
}

void
TraceLogReplay::SetTargetLayer(unsigned long layer)
{
  m_TargetLayer = layer;
  this->Select();
}

void
TraceLogReplay::Select()
{
  m_Base = LayerState();
  m_Steps.clear();
  m_FinalChecksum.clear();

  // The starting point is the last time the layer appeared or was loaded;
  // what happened to it before that does not affect its final content
  auto it = m_LayerStates.find(m_TargetLayer);
  if (it != m_LayerStates.end() && !it->second.empty())
    m_Base = it->second.back();

  for (const auto &ls : m_AllSteps)
    if (ls.first == m_TargetLayer && ls.second.seq > m_Base.seq)
      m_Steps.push_back(ls.second);

  unsigned long last_seq = m_Steps.empty() ? m_Base.seq : m_Steps.back().seq;
  for (const auto &sv : m_Saves)
    if (sv.first == m_TargetLayer && sv.second.seq > last_seq)
      m_FinalChecksum = sv.second.checksum;
}

unsigned long
TraceLogReplay::ApplyStep(LabelImage4D *image, const Step &step)
{
  itk::ImageRegion<4> region;
  for (unsigned int d = 0; d < 3; d++)
  {
    region.SetIndex(d, step.region.GetIndex(d));
    region.SetSize(d, step.region.GetSize(d));
  }
  region.SetIndex(3, step.tp);
  region.SetSize(3, 1);

  if (!image->GetBufferedRegion().IsInside(region))
    throw IRISException("Error: Trace log does not fit. Event %lu changes voxels outside of "
                        "this segmentation.", step.seq);

  // The region is one time point thick, so raster order over it is the same
  // as over the 3D region the step was recorded on
  unsigned long                           mismatches = 0;
  itk::ImageRegionIterator<LabelImage4D> it(image, region);
  for (const Run &run : step.runs)
  {
    for (unsigned long j = 0; j < run.length && !it.IsAtEnd(); j++, ++it)
    {
      if (run.changed)
      {
        if (it.Get() == run.old_label)
          it.Set(run.new_label);
        else
          ++mismatches;
      }
    }
  }
  return mismatches;
}

std::string
TraceLogReplay::ComputeChecksum(const LabelImage4D *image)
{
  // Raster order over (x, y, z, t) visits the rows of one time point after
  // another, as TraceLog::ComputeChecksum does
  TraceLog::ChecksumHasher hasher;
  TraceLog::AddToChecksum(hasher, image);
  return TraceLog::FormatChecksum(hasher.GetHash());
}

std::string
TraceLogReplay::BlankChecksum(const unsigned int size[3], unsigned int tps)
{
  TraceLog::ChecksumHasher hasher;
  unsigned long            rows = (unsigned long)size[1] * size[2] * std::max(1u, tps);
  for (unsigned long r = 0; r < rows; r++)
  {
    hasher.Add(0, size[0]);
    hasher.EndRow();
  }
  return TraceLog::FormatChecksum(hasher.GetHash());
}

TraceLogReplay::LabelImage4D::Pointer
TraceLogReplay::CreateStartingSegmentation() const
{
  // The content runs cover the voxels in raster order, as the buffer of a 4D
  // image holds them
  LabelImage4D::Pointer image = this->CreateBlankSegmentation();
  LabelType            *buffer = image->GetBufferPointer();
  size_t                i = 0;
  for (const auto &run : m_Base.content)
  {
    std::fill(buffer + i, buffer + i + run.first, run.second);
    i += run.first;
  }
  return image;
}

TraceLogReplay::LabelImage4D::Pointer
TraceLogReplay::CreateBlankSegmentation() const
{
  LabelImage4D::Pointer      image = LabelImage4D::New();
  LabelImage4D::RegionType   region;
  LabelImage4D::SpacingType  spacing;
  LabelImage4D::PointType    origin;
  LabelImage4D::DirectionType direction;
  spacing.Fill(1.0);
  origin.Fill(0.0);
  direction.SetIdentity();

  for (unsigned int d = 0; d < 3; d++)
  {
    region.SetSize(d, m_Base.size[d]);
    if (m_HasImage)
    {
      spacing[d] = m_Image["spacing"][d].asDouble();
      origin[d] = m_Image["origin"][d].asDouble();
      for (unsigned int e = 0; e < 3; e++)
        direction(d, e) = m_Image["direction"][d * 3 + e].asDouble();
    }
  }
  region.SetSize(3, std::max(1u, m_Base.tps));

  image->SetRegions(region);
  image->SetSpacing(spacing);
  image->SetOrigin(origin);
  image->SetDirection(direction);
  image->Allocate();
  image->FillBuffer(0);
  return image;
}
