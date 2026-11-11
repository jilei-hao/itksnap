/*=========================================================================

  Program:   ITK-SNAP
  Module:    TraceLogTest.cxx
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
// Logic-tier (Qt-free) test of the trace log. It records a session through
// the real edit paths -- box edits, a paintbrush-like commit of two
// overlapping deltas, undo and redo, a Label Editor relabel followed by
// clearing the undo history, and a save -- and then checks that:
//  - the copy of the log next to the saved file is valid and has no paths,
//  - an edit made of overlapping deltas is recorded as its net change,
//  - replaying the log onto a blank segmentation reproduces the saved file
//    exactly, and a partial replay stops at the right state,
//  - a segmentation that differs from the log is detected,
//  - after loading a segmentation, the log starts from it.
// It leaves its files in <temp>/TraceLogTest for the itksnap-wt replay test.
// With --check-gui, it checks the log recorded by the TraceLogGUI test.
#include "IRISApplication.h"
#include "ImageIODelegates.h"
#include "GuidedNativeImageIO.h"
#include "LabelImageWrapper.h"
#include "SegmentationUpdateIterator.h"
#include "TraceLog.h"
#include "TraceLogReplay.h"
#include "UIReporterDelegates.h"
#include "itkImageFileReader.h"
#include "itksys/Directory.hxx"
#include "itksys/SystemTools.hxx"

#include <cstring>
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace
{

int g_failures = 0;

#define CHECK(cond, msg)                                                        \
  do {                                                                          \
    if (!(cond)) {                                                              \
      std::cerr << "FAIL: " << (msg) << "  [" << #cond << "]" << std::endl;     \
      ++g_failures;                                                             \
    }                                                                           \
  } while (0)

class DummySystemInfoDelegate : public SystemInfoDelegate
{
public:
  DummySystemInfoDelegate(const char *argv0, const std::string &temp)
    : m_ExecutableName(argv0), m_Temp(temp) {}

  virtual std::string GetApplicationDirectory()
    { return itksys::SystemTools::GetFilenamePath(m_ExecutableName); }
  virtual std::string GetApplicationFile() { return m_ExecutableName; }
  virtual std::string GetApplicationPermanentDataLocation() { return m_Temp + "/appdata"; }
  virtual std::string GetUserDocumentsLocation() { return m_Temp + "/appdata"; }
  virtual std::string GetTempDirectory() { return m_Temp; }
  virtual std::string EncodeServerURL(const std::string &url) { return url; }

  virtual void LoadResourceAsImage2D(std::string, GrayscaleImage *) {}
  virtual void LoadResourceAsRegistry(std::string, Registry &) {}
  virtual void WriteRGBAImage2D(std::string, RGBAImageType *) {}

protected:
  std::string m_ExecutableName, m_Temp;
};

class DummyColorMapPresetNameSource : public AbstractColorMapPresetNameSource
{
public:
  virtual std::string GetPresetName(ColorMap::SystemPreset preset, bool) override
  {
    return "Color Map " + std::to_string(static_cast<int>(preset));
  }
};

itk::ImageRegion<3> Box(int lo, int hi)
{
  itk::ImageRegion<3> r;
  for (unsigned int d = 0; d < 3; d++)
  {
    r.SetIndex(d, lo);
    r.SetSize(d, hi - lo + 1);
  }
  return r;
}

// Paint a box into the staging area of the selected segmentation, as one of
// the deltas of a paintbrush drag
void StageBox(LabelImageWrapper *seg, const itk::ImageRegion<3> &region, LabelType label)
{
  SegmentationUpdateIterator it(seg, region, label, DrawOverFilter(PAINT_OVER_ALL, 0));
  for (; !it.IsAtEnd(); ++it)
    it.PaintAsForeground();
  it.Finalize();
  seg->StoreIntermediateUndoDelta(it.RelinquishDelta());
}

void Save(IRISApplication *app, LabelImageWrapper *seg, const std::string &fname)
{
  SmartPtr<AbstractSaveImageDelegate> del = app->CreateSaveDelegateForLayer(seg, LABEL_ROLE);
  SmartPtr<GuidedNativeImageIO>       io = GuidedNativeImageIO::New();
  Registry                            reg;
  IRISWarningList                     wl;
  del->SaveImage(fname, io, reg, wl);
  CHECK(wl.empty(), "saving the segmentation gave no warnings");
}

std::vector<Json::Value> ReadEvents(const std::string &file, std::string &text)
{
  std::ifstream     in(file.c_str());
  std::stringstream ss;
  ss << in.rdbuf();
  text = ss.str();

  std::vector<Json::Value> events;
  std::istringstream       lines(text);
  std::string              line;
  Json::Reader             reader;
  while (std::getline(lines, line))
  {
    Json::Value ev;
    bool        ok = reader.parse(line, ev, false);
    CHECK(ok, "every line of the log is JSON");
    events.push_back(ev);
  }
  return events;
}

TraceLogReplay::LabelImage4D::Pointer ReadSegmentation(const std::string &file)
{
  typedef itk::ImageFileReader<TraceLogReplay::LabelImage4D> ReaderType;
  ReaderType::Pointer reader = ReaderType::New();
  reader->SetFileName(file);
  reader->Update();
  return reader->GetOutput();
}

LabelType Voxel(TraceLogReplay::LabelImage4D *img, int x, int y, int z)
{
  TraceLogReplay::LabelImage4D::IndexType idx = { { x, y, z, 0 } };
  return img->GetPixel(idx);
}

// Check the log recorded by the TraceLogGUI test (test_TraceLogGUI.js): the
// actions it performed are in the log with the view they came from, and the
// log replays to the segmentation it saved
int CheckGuiLog(const std::string &dir)
{
  itksys::Directory d;
  d.Load(dir);
  std::string file;
  for (unsigned long i = 0; i < d.GetNumberOfFiles(); i++)
  {
    std::string f = d.GetFile(i);
    if (f.size() > 6 && f.substr(f.size() - 6) == ".jsonl")
      file = dir + "/" + f;
  }
  if (file.empty())
  {
    std::cerr << "FAIL: no trace log in " << dir << std::endl;
    return 1;
  }
  std::cout << "Checking " << file << std::endl;

  std::string              text;
  std::vector<Json::Value> events = ReadEvents(file, text);
  CHECK(events.size() && events[0]["user"].asString() == "gui-tester", "the pseudonym is recorded");
  std::string app = events.size() ? events[0]["app"].asString() : "";
  CHECK(app.find("ITK-SNAP") == 0 && app.find("ITK-SNAP", 1) == std::string::npos,
        "the application is named once");

  int  key_slices = 0, paint_edits = 0, undos = 0, saves = 0, zoom_events = 0;
  bool zoom_merged = false;
  bool to_paintbrush = false, zoomed = false, panned = false, slider = false;
  int  last_axial_slice = -1;
  for (const Json::Value &ev : events)
  {
    std::string type = ev["type"].asString();
    const Json::Value &ctx = ev["ctx"];
    if (type == "cursor")
    {
      for (const char *end : { "from", "to" })
        for (Json::ArrayIndex i = 0; i < ev[end]["slices"].size(); i++)
          CHECK(ev[end]["slices"][i].asInt() >= 0, "cursor events have real slice numbers");
    }

    if (type == "tool" && ev["to"]["mode"].asString() == "paintbrush")
      to_paintbrush = true;
    else if (type == "cursor" && ctx["action"].asString() == "slider")
    {
      CHECK(ctx["view"].asInt() == 0 && ctx["plane"].asString() == "axial",
            "the slider slice change comes from the axial view");
      slider |= ev["to"]["slices"][0] != ev["from"]["slices"][0];
    }
    else if (type == "cursor" && ctx["action"].asString() == "keys")
    {
      CHECK(ctx["view"].asInt() == 0 && ctx["plane"].asString() == "axial",
            "keyboard slice changes come from the axial view");
      int slice = ev["to"]["slices"][0].asInt();
      if (slice != last_axial_slice)
        key_slices++;
      last_axial_slice = slice;
    }
    else if (type == "view" && ctx["action"].asString() == "zoom" && ev["view"].asInt() == 0)
    {
      // A drag is merged into one event (or a few, if the machine is slow
      // enough that the changes come more than a second apart)
      zoom_events++;
      zoom_merged |= ev["n"].asUInt() >= 2;
      zoomed |= ev["to"]["zoom"] != ev["from"]["zoom"];
    }
    else if (type == "view" && ctx["action"].asString() == "pan")
      panned |= ev["view"].asInt() == 0 && ev["to"]["pos"] != ev["from"]["pos"];
    else if (type == "edit" && ctx["tool"].asString() == "paintbrush")
    {
      paint_edits++;
      CHECK(ctx["view"].asInt() == 0 && ctx["plane"].asString() == "axial",
            "the strokes were painted in the axial view");
      CHECK(ctx["brush"]["size"].asDouble() == 5.0, "the brush size is recorded");
      CHECK(ctx["stroke"].size() >= 2, "the stroke path has several points");
      for (Json::ArrayIndex i = 1; i < ctx["stroke"].size(); i++)
        CHECK(ctx["stroke"][i][3].asInt() >= ctx["stroke"][i - 1][3].asInt(),
              "stroke points are in time order");
      CHECK(ev["actor"].asString() == "human", "painting is done by a human");
    }
    else if (type == "undo")
      undos++;
    else if (type == "save")
      saves++;
  }

  CHECK(to_paintbrush, "switching to the paintbrush is recorded");
  CHECK(key_slices >= 3, "each keyboard slice change is recorded");
  CHECK(slider, "the slice slider change is recorded, with its view");
  CHECK(zoomed, "the zoom is recorded, with its view");
  CHECK(zoom_merged && zoom_events <= 2, "the changes during the zoom drag are merged");
  CHECK(text.find("MRIcrop") == std::string::npos && text.find("TraceLogGUI_seg") == std::string::npos,
        "no file names in the log");
  CHECK(panned, "the pan is recorded, with its view");
  CHECK(paint_edits == 2, "both strokes are recorded");
  CHECK(undos == 1, "the undo is recorded");
  CHECK(saves == 1, "the save is recorded");

  TraceLogReplay replay;
  replay.Read(file);
  CHECK(replay.GetSteps().size() == 3, "two strokes and an undo to replay");
  CHECK(replay.GetBase().kind == "loaded" && !replay.GetBase().blank,
        "the GUI session started from a loaded segmentation with labels");
  TraceLogReplay::LabelImage4D::Pointer seg = replay.CreateStartingSegmentation();
  CHECK(TraceLogReplay::ComputeChecksum(seg) == replay.GetBase().checksum,
        "the log holds that segmentation");
  for (const TraceLogReplay::Step &s : replay.GetSteps())
    CHECK(TraceLogReplay::ApplyStep(seg, s) == 0, "each step applies exactly");
  CHECK(!replay.GetFinalChecksum().empty() &&
          TraceLogReplay::ComputeChecksum(seg) == replay.GetFinalChecksum(),
        "the replay rebuilds the saved segmentation");

  if (g_failures)
    std::cerr << g_failures << " check(s) failed" << std::endl;
  else
    std::cout << "All GUI trace log checks passed" << std::endl;
  return g_failures ? 1 : 0;
}

} // namespace

int main(int argc, char *argv[])
{
  if (argc == 3 && std::string(argv[1]) == "--check-gui")
    return CheckGuiLog(argv[2]);

  if (argc < 3)
  {
    std::cerr << "Usage: " << argv[0] << " <TestData dir> <temp dir>" << std::endl
              << "       " << argv[0] << " --check-gui <trace log dir>" << std::endl;
    return 2;
  }
  std::string testdata = argv[1];
  std::string work = std::string(argv[2]) + "/TraceLogTest";
  itksys::SystemTools::RemoveADirectory(work);
  itksys::SystemTools::MakeDirectory(work);

  DummySystemInfoDelegate sidel(argv[0], work);
  SystemInterface::SetSystemInfoDelegate(&sidel);
  DummyColorMapPresetNameSource color_map_source;
  ColorMap::SetColorMapPresetNameSource(&color_map_source);

  IRISApplication::Pointer app = IRISApplication::New();
  IRISWarningList          wl;
  app->OpenImage((testdata + "/MRIcrop-orig.gipl.gz").c_str(), MAIN_ROLE, wl);

  // ---- Record a session --------------------------------------------------
  CHECK(app->StartTraceLog(work + "/logs", "tester", "TraceLogTest"), "trace log starts");
  CHECK(TraceLog::GetActive() == app->GetTraceLog(), "the started log is the active one");

  LabelImageWrapper *seg = app->GetSelectedSegmentationLayer();
  unsigned long      layer = seg->GetUniqueId();

  CHECK(app->PaintRegionWithLabel(Box(10, 14), 1, "box A") == 125, "box A paints 125 voxels");
  CHECK(app->PaintRegionWithLabel(Box(12, 16), 2, "box B") == 125, "box B paints 125 voxels");

  // A paintbrush-like commit: two overlapping deltas, label 3 then label 4
  StageBox(seg, Box(20, 22), 3);
  StageBox(seg, Box(21, 23), 4);
  CHECK(seg->StoreUndoPoint("stroke"), "the stroke commits");

  app->Undo();
  app->Redo();
  app->Undo();

  CHECK(app->PaintRegionWithLabel(Box(30, 31), 5, "box C") == 8, "box C paints 8 voxels");

  // The Label Editor's "change label ID": relabel 5 as 6, then clear undo
  CHECK(app->ReplaceLabel(6, 5) == 8, "relabel changes 8 voxels");
  app->ClearUndoPoints();

  std::string segfile = work + "/seg.nii.gz";
  Save(app, seg, segfile);
  std::string saved_checksum = TraceLog::ComputeChecksum(seg);
  app->StopTraceLog();
  CHECK(TraceLog::GetActive() == nullptr, "no log is active after stopping");

  // ---- The copy next to the segmentation --------------------------------
  std::string sidecar = TraceLog::SidecarFileName(segfile);
  CHECK(itksys::SystemTools::FileExists(sidecar), "the log is saved next to the segmentation");

  std::string              text;
  std::vector<Json::Value> events = ReadEvents(sidecar, text);
  CHECK(!events.empty() && events.front()["type"].asString() == "session", "starts with session");
  CHECK(!events.empty() && events.back()["type"].asString() == "save", "the copy ends with the save");
  CHECK(text.find(work) == std::string::npos, "no file paths in the log");
  CHECK(text.find("seg.nii.gz") == std::string::npos, "no file names in the log");
  for (size_t i = 0; i < events.size(); i++)
    CHECK(events[i]["seq"].asUInt64() == i + 1, "events are numbered 1, 2, 3, ...");

  // The stroke is recorded as its net change: the 8 voxels painted 3 then 4
  // went from 0 to 4, never from 3
  bool found_stroke = false;
  for (const Json::Value &ev : events)
  {
    if (ev["type"].asString() == "edit" && ev["op"].asString() == "stroke")
    {
      found_stroke = true;
      CHECK(ev["changed"].asUInt64() == 27 + 27 - 8, "the stroke changed 46 voxels");
      unsigned long to3 = 0, to4 = 0;
      for (const Json::Value &run : ev["runs"])
      {
        if (run.size() == 3)
        {
          CHECK(run[1].asUInt() == 0, "every voxel of the stroke started at label 0");
          (run[2].asUInt() == 3 ? to3 : to4) += run[0].asUInt64();
        }
      }
      CHECK(to3 == 19 && to4 == 27, "19 voxels ended at label 3 and 27 at label 4");
      CHECK(ev["before"].size() == 1 && ev["before"]["0"].asUInt64() == 46,
            "the label counts are of the net change: 46 voxels were label 0");
      CHECK(ev["after"]["3"].asUInt64() == 19 && ev["after"]["4"].asUInt64() == 27,
            "...and became 19 of label 3 and 27 of label 4");
    }
  }
  CHECK(found_stroke, "the stroke is in the log");

  // ---- Replay -----------------------------------------------------------
  TraceLogReplay replay;
  replay.Read(sidecar);
  CHECK(replay.GetTargetLayer() == layer, "the replay targets the saved layer");
  CHECK(replay.GetBase().kind == "present", "the layer starts as it was when recording began");
  CHECK(replay.GetSteps().size() == 8, "5 edits, 2 undos and 1 redo");
  CHECK(replay.GetFinalChecksum() == saved_checksum, "the log has the saved checksum");
  CHECK(replay.GetBase().blank && replay.HasStartingSegmentation(),
        "a blank start can be replayed from the log alone");

  TraceLogReplay::LabelImage4D::Pointer rebuilt = replay.CreateBlankSegmentation();
  CHECK(TraceLogReplay::ComputeChecksum(rebuilt) == replay.GetBase().checksum,
        "a blank segmentation is the starting point");
  for (const TraceLogReplay::Step &s : replay.GetSteps())
    CHECK(TraceLogReplay::ApplyStep(rebuilt, s) == 0, "each step applies exactly");
  CHECK(TraceLogReplay::ComputeChecksum(rebuilt) == saved_checksum, "the replay is exact");

  TraceLogReplay::LabelImage4D::Pointer on_disk = ReadSegmentation(segfile);
  CHECK(TraceLogReplay::ComputeChecksum(on_disk) == saved_checksum,
        "the checksum of the saved file matches the log");
  CHECK(Voxel(rebuilt, 30, 30, 30) == 6, "the relabel is replayed");
  CHECK(Voxel(rebuilt, 21, 21, 21) == 0, "the undone stroke is not in the result");

  // A partial replay: after the first two edits
  TraceLogReplay::LabelImage4D::Pointer partial = replay.CreateBlankSegmentation();
  TraceLogReplay::ApplyStep(partial, replay.GetSteps()[0]);
  TraceLogReplay::ApplyStep(partial, replay.GetSteps()[1]);
  CHECK(Voxel(partial, 10, 10, 10) == 1 && Voxel(partial, 12, 12, 12) == 2 &&
          Voxel(partial, 16, 16, 16) == 2,
        "a partial replay stops at the state after those edits");

  // A segmentation that differs from the log is detected
  TraceLogReplay::LabelImage4D::Pointer other = replay.CreateBlankSegmentation();
  TraceLogReplay::LabelImage4D::IndexType idx = { { 11, 11, 11, 0 } };
  other->SetPixel(idx, 7);
  CHECK(TraceLogReplay::ApplyStep(other, replay.GetSteps()[0]) == 1,
        "a voxel with an unexpected label is a mismatch");

  // ---- A session that starts by loading a segmentation ------------------
  CHECK(app->StartTraceLog(work + "/logs", "tester", "TraceLogTest"), "a second log starts");
  app->OpenImage(segfile.c_str(), LABEL_ROLE, wl);
  seg = app->GetSelectedSegmentationLayer();
  CHECK(app->PaintRegionWithLabel(Box(40, 41), 2, "box D") == 8, "box D paints 8 voxels");
  std::string segfile2 = work + "/seg2.nii.gz";
  Save(app, seg, segfile2);
  std::string saved_checksum2 = TraceLog::ComputeChecksum(seg);
  app->StopTraceLog();

  // The file loaded had a log next to it, so the copy saved with the result
  // holds both sessions, and the replay goes back to the first one's start
  std::string              text2;
  std::vector<Json::Value> events2 = ReadEvents(TraceLog::SidecarFileName(segfile2), text2);
  bool loaded_with_history = false;
  for (const Json::Value &ev : events2)
    if (ev["type"].asString() == "layer" && ev["kind"].asString() == "loaded")
      loaded_with_history = ev["history"].asUInt() == 1;
  CHECK(loaded_with_history, "the loaded layer inherits one session of history");

  TraceLogReplay replay2;
  replay2.Read(TraceLog::SidecarFileName(segfile2));
  CHECK(replay2.GetSessions().size() == 2, "the second log holds the first session too");
  CHECK(replay2.GetReplaySessions() == 2 && !replay2.IsHistoryIncomplete(),
        "the replay goes through both sessions");
  CHECK(replay2.GetBase().blank && replay2.GetBase().session == 0,
        "...from the blank start of the first");
  CHECK(replay2.GetSteps().size() == 9, "the first session's 8 edits, then box D");
  {
    TraceLogReplay::LabelImage4D::Pointer all = replay2.CreateBlankSegmentation();
    for (const TraceLogReplay::Step &s : replay2.GetSteps())
      CHECK(TraceLogReplay::ApplyStep(all, s) == 0, "each step of both sessions applies");
    CHECK(TraceLogReplay::ComputeChecksum(all) == saved_checksum2,
          "replaying both sessions reproduces the second save");
  }

  // Starting from the file the second session loaded
  CHECK(replay2.StartFrom(saved_checksum), "the replay can start from the loaded file");
  CHECK(replay2.GetBase().kind == "loaded" && replay2.GetBase().session == 1,
        "...which the second session loaded");
  CHECK(!replay2.GetBase().blank && !replay2.GetBase().content.empty(),
        "the log keeps a copy of a loaded segmentation with labels");
  TraceLogReplay::LabelImage4D::Pointer from_log = replay2.CreateStartingSegmentation();
  CHECK(TraceLogReplay::ComputeChecksum(from_log) == saved_checksum,
        "the copy in the log is the loaded segmentation");
  CHECK(replay2.GetSteps().size() == 1, "one edit after loading");
  CHECK(TraceLogReplay::ApplyStep(from_log, replay2.GetSteps()[0]) == 0 &&
          TraceLogReplay::ComputeChecksum(from_log) == saved_checksum2,
        "replaying from the copy in the log reproduces the save");
  CHECK(!replay2.StartFrom("fnv1a64:0000000000000000"), "a state never reached is no start");

  // A log from before the starting segmentation and "blank" were recorded
  // (and before logs kept their history): a blank start is still recognized;
  // one with labels needs its file. Such a log has one session: the second.
  {
    std::string        last_session;
    std::istringstream all_lines(text2);
    std::string        line;
    while (std::getline(all_lines, line))
    {
      if (line.find("\"type\":\"session\"") != std::string::npos)
        last_session.clear();
      last_session += line + "\n";
    }

    // Also the fixture of the command line test that starts from the wrong
    // segmentation (TraceLogReplayCLIWrongStart)
    std::ofstream((work + "/seg2_last_session.jsonl").c_str()) << last_session;

    std::string        old_style;
    std::istringstream lines(last_session);
    while (std::getline(lines, line))
    {
      size_t c = line.find(",\"content\":");
      if (c != std::string::npos)
        line = line.substr(0, c) + "}";
      line = std::regex_replace(line, std::regex("\"content_seq\":[0-9]+,"), "");
      for (const char *b : { "\"blank\":true,", "\"blank\":false," })
        for (size_t p; (p = line.find(b)) != std::string::npos;)
          line.erase(p, strlen(b));
      old_style += line + "\n";
    }
    std::string old_file = work + "/old_style.jsonl";
    std::ofstream(old_file.c_str()) << old_style;
    TraceLogReplay old_replay;
    old_replay.Read(old_file);
    CHECK(!old_replay.HasStartingSegmentation(), "an old log does not hold a start with labels");
    CHECK(old_replay.GetSteps().size() == 1, "...and replays the edit after loading");
    TraceLogReplay::LabelImage4D::Pointer from_file = ReadSegmentation(segfile);
    CHECK(TraceLogReplay::ApplyStep(from_file, old_replay.GetSteps()[0]) == 0, "it applies");
    CHECK(TraceLogReplay::ComputeChecksum(from_file) == saved_checksum2, "and reproduces the save");

    std::string text1, old_style1;
    ReadEvents(sidecar, text1);
    std::istringstream lines1(text1);
    while (std::getline(lines1, line))
    {
      for (const char *b : { "\"blank\":true,", "\"blank\":false," })
        for (size_t p; (p = line.find(b)) != std::string::npos;)
          line.erase(p, strlen(b));
      old_style1 += line + "\n";
    }
    std::string old_file1 = work + "/old_style_blank.jsonl";
    std::ofstream(old_file1.c_str()) << old_style1;
    TraceLogReplay old_replay1;
    old_replay1.Read(old_file1);
    CHECK(old_replay1.GetBase().blank, "an old log's blank start is recognized by its checksum");
  }

  // ---- Later sessions on the file: every save keeps its whole history ----
  // Save As, save again in place, reload from the file and save once more:
  // each session is in the log once
  CHECK(app->StartTraceLog(work + "/logs", "tester", "TraceLogTest"), "a later session starts");
  app->OpenImage(segfile2.c_str(), LABEL_ROLE, wl);
  seg = app->GetSelectedSegmentationLayer();

  // A save that leaves the segmentation as it was loaded: a replay must not
  // take it for where the loaded file came from
  std::string segfile7 = work + "/seg7.nii.gz";
  CHECK(app->PaintRegionWithLabel(Box(42, 43), 3, "box undone") == 8, "a box to undo");
  app->Undo();
  Save(app, seg, segfile7);
  CHECK(TraceLog::ComputeChecksum(seg) == saved_checksum2, "...saved as it was loaded");

  CHECK(app->PaintRegionWithLabel(Box(44, 45), 3, "box D2") == 8, "box D2 paints 8 voxels");
  Save(app, seg, segfile7);
  CHECK(app->PaintRegionWithLabel(Box(46, 47), 4, "box D3") == 8, "box D3 paints 8 voxels");
  Save(app, seg, segfile7);
  {
    SmartPtr<ReloadSegmentationWrapperDelegate> reload7 = ReloadSegmentationWrapperDelegate::New();
    reload7->Initialize(app, seg);
    reload7->ValidateHeader(wl);
    reload7->UpdateWrapper();
  }
  CHECK(app->PaintRegionWithLabel(Box(48, 49), 5, "box D4") == 8, "box D4 paints 8 voxels");
  Save(app, seg, segfile7);
  std::string saved_checksum7 = TraceLog::ComputeChecksum(seg);
  app->StopTraceLog();

  TraceLogReplay replay7;
  replay7.Read(TraceLog::SidecarFileName(segfile7));
  CHECK(replay7.GetSessions().size() == 3, "the file's log holds three sessions, each once (got " +
                                             std::to_string(replay7.GetSessions().size()) + ")");
  CHECK(replay7.GetReplaySessions() == 3 && !replay7.IsHistoryIncomplete() &&
          replay7.GetBase().blank && replay7.GetBase().session == 0,
        "the replay goes back through all three to the first blank start");
  CHECK(replay7.GetSteps().size() == 14,
        "8 edits, box D, a box and its undo, then D2, D3 and D4 (got " +
          std::to_string(replay7.GetSteps().size()) + ")");
  {
    TraceLogReplay::LabelImage4D::Pointer all = replay7.CreateBlankSegmentation();
    for (const TraceLogReplay::Step &s : replay7.GetSteps())
      CHECK(TraceLogReplay::ApplyStep(all, s) == 0, "each step of the three sessions applies");
    CHECK(TraceLogReplay::ComputeChecksum(all) == saved_checksum7,
          "replaying the whole history reproduces the last save");
    CHECK(replay7.GetFinalChecksum() == saved_checksum7, "the log says what was saved last");
  }

  // ---- A change made while not recording breaks the history --------------
  app->OpenImage(segfile7.c_str(), LABEL_ROLE, wl);
  seg = app->GetSelectedSegmentationLayer();
  CHECK(app->PaintRegionWithLabel(Box(50, 51), 6, "unrecorded") == 8, "an unrecorded change");
  Save(app, seg, segfile7);
  CHECK(app->StartTraceLog(work + "/logs", "tester", "TraceLogTest"), "recording resumes");
  app->OpenImage(segfile7.c_str(), LABEL_ROLE, wl);
  seg = app->GetSelectedSegmentationLayer();
  CHECK(app->PaintRegionWithLabel(Box(52, 53), 7, "box D5") == 8, "box D5 paints 8 voxels");
  std::string segfile8 = work + "/seg8.nii.gz";
  Save(app, seg, segfile8);
  std::string saved_checksum8 = TraceLog::ComputeChecksum(seg);
  app->StopTraceLog();

  TraceLogReplay replay8;
  replay8.Read(TraceLog::SidecarFileName(segfile8));
  CHECK(replay8.GetSessions().size() == 4, "the log keeps the earlier sessions");
  CHECK(replay8.IsHistoryIncomplete() && replay8.GetBase().session == 3 &&
          replay8.GetBase().kind == "loaded",
        "but the replay starts at the session after the unrecorded change, and says so");
  CHECK(replay8.HasStartingSegmentation() && replay8.GetSteps().size() == 1,
        "...from the copy in the log, with one edit");
  {
    TraceLogReplay::LabelImage4D::Pointer from8 = replay8.CreateStartingSegmentation();
    CHECK(TraceLogReplay::ApplyStep(from8, replay8.GetSteps()[0]) == 0 &&
            TraceLogReplay::ComputeChecksum(from8) == saved_checksum8,
          "which reproduces the save");
  }

  // ---- A second layer, undo and redo of one-delta edits, a temporary ------
  // ---- commit that is redone, and a reload from file ----------------------
  CHECK(app->StartTraceLog(work + "/logs", "tester", "TraceLogTest"), "a third log starts");
  app->AddBlankSegmentation();
  LabelImageWrapper *seg3 = app->GetSelectedSegmentationLayer();
  CHECK(seg3->GetUniqueId() != seg->GetUniqueId(), "a second segmentation layer");

  CHECK(app->PaintRegionWithLabel(Box(10, 11), 3, "box E") == 8, "box E paints 8 voxels");
  app->Undo();
  app->Redo();

  // The deep learning brush commits a temporary edit and undoes it; when the
  // model then changes nothing, Redo brings the stroke back
  StageBox(seg3, Box(50, 51), 4);
  CHECK(seg3->StoreUndoPoint("Temporary undo point"), "the temporary commit");
  app->Undo();
  CHECK(seg3->IsRedoPossible(), "the temporary commit can be redone");
  app->Redo();

  std::string segfile3 = work + "/seg3.nii.gz";
  Save(app, seg3, segfile3);
  std::string saved_checksum3 = TraceLog::ComputeChecksum(seg3);

  TraceLogReplay replay3;
  replay3.Read(TraceLog::SidecarFileName(segfile3));
  CHECK(replay3.GetTargetLayer() == seg3->GetUniqueId(), "the replay targets the second layer");
  CHECK(replay3.GetBase().kind == "added", "which started blank");
  CHECK(replay3.GetSteps().size() == 6, "two edits, each undone and redone");
  TraceLogReplay::LabelImage4D::Pointer rebuilt3 = replay3.CreateBlankSegmentation();
  for (const TraceLogReplay::Step &s : replay3.GetSteps())
    CHECK(TraceLogReplay::ApplyStep(rebuilt3, s) == 0, "each step applies exactly");
  CHECK(TraceLogReplay::ComputeChecksum(rebuilt3) == saved_checksum3,
        "the redone temporary commit is replayed");

  // Change the layer, reload it from its file (which drops the change), and
  // change it again: the log starts the layer over at the reload
  CHECK(app->PaintRegionWithLabel(Box(20, 21), 5, "box F") == 8, "box F paints 8 voxels");
  SmartPtr<ReloadSegmentationWrapperDelegate> reload = ReloadSegmentationWrapperDelegate::New();
  reload->Initialize(app, seg3);
  reload->ValidateHeader(wl);
  reload->UpdateWrapper();
  CHECK(app->PaintRegionWithLabel(Box(30, 31), 6, "box G") == 8, "box G paints 8 voxels");
  std::string segfile4 = work + "/seg4.nii.gz";
  Save(app, seg3, segfile4);
  std::string saved_checksum4 = TraceLog::ComputeChecksum(seg3);

  // The reload brought back what was saved, so the replay goes on through it
  // from the layer's blank start, leaving out box F
  TraceLogReplay replay4;
  replay4.Read(TraceLog::SidecarFileName(segfile4));
  CHECK(replay4.GetSessions().size() == 1, "a file saved in this session has no older history");
  CHECK(replay4.GetBase().kind == "added" && replay4.GetBase().blank &&
          replay4.GetSteps().size() == 7,
        "the replay goes through the reload, from the blank start: 6 steps, then box G");
  {
    TraceLogReplay::LabelImage4D::Pointer all = replay4.CreateBlankSegmentation();
    for (const TraceLogReplay::Step &s : replay4.GetSteps())
      CHECK(TraceLogReplay::ApplyStep(all, s) == 0, "each step applies, box F left out");
    CHECK(TraceLogReplay::ComputeChecksum(all) == saved_checksum4, "and reproduces the save");
  }
  CHECK(replay4.StartFrom(saved_checksum3) && replay4.GetBase().kind == "loaded" &&
          replay4.GetBase().checksum == saved_checksum3,
        "the replay can start at the reload");
  CHECK(replay4.GetSteps().size() == 1, "only box G comes after it");
  TraceLogReplay::LabelImage4D::Pointer from_seg3 = ReadSegmentation(segfile3);
  for (const TraceLogReplay::Step &s : replay4.GetSteps())
    CHECK(TraceLogReplay::ApplyStep(from_seg3, s) == 0, "box G applies");
  CHECK(TraceLogReplay::ComputeChecksum(from_seg3) == saved_checksum4,
        "replaying from the reloaded file reproduces the save");

  // A new main image: the copy saved with its segmentation leaves out what
  // was done to the image before
  app->OpenImage((testdata + "/MRIcrop-orig.gipl.gz").c_str(), MAIN_ROLE, wl);
  LabelImageWrapper *seg5 = app->GetSelectedSegmentationLayer();
  CHECK(app->PaintRegionWithLabel(Box(40, 42), 7, "box H") == 27, "box H paints 27 voxels");
  std::string segfile5 = work + "/seg5.nii.gz";
  Save(app, seg5, segfile5);
  std::string saved_checksum5 = TraceLog::ComputeChecksum(seg5);
  app->StopTraceLog();

  std::string              text5;
  std::vector<Json::Value> events5 = ReadEvents(TraceLog::SidecarFileName(segfile5), text5);
  CHECK(events5.size() > 2 && events5[0]["type"].asString() == "session" &&
          events5[1]["type"].asString() == "image",
        "the copy is the session, then everything from the image on");
  CHECK(text5.find("box G") == std::string::npos, "edits to the earlier image are not in it");
  TraceLogReplay replay5;
  replay5.Read(TraceLog::SidecarFileName(segfile5));
  TraceLogReplay::LabelImage4D::Pointer rebuilt5 = replay5.CreateBlankSegmentation();
  for (const TraceLogReplay::Step &s : replay5.GetSteps())
    CHECK(TraceLogReplay::ApplyStep(rebuilt5, s) == 0, "each step applies exactly");
  CHECK(TraceLogReplay::ComputeChecksum(rebuilt5) == saved_checksum5, "the copy replays");

  // ---- 4D: edits at two time points --------------------------------------
  CHECK(app->StartTraceLog(work + "/logs", "tester", "TraceLogTest"), "a 4D log starts");
  app->OpenImage((testdata + "/img4d_11f.nii.gz").c_str(), MAIN_ROLE, wl);
  app->OpenImage((testdata + "/seg4d_11f.nii.gz").c_str(), LABEL_ROLE, wl);
  LabelImageWrapper *seg4d = app->GetSelectedSegmentationLayer();
  CHECK(seg4d->GetNumberOfTimePoints() == 11, "the segmentation has 11 time points");
  app->SetCursorTimePoint(3);
  CHECK(app->PaintRegionWithLabel(Box(5, 7), 9, "box at 3") > 0, "paint at time point 3");
  app->SetCursorTimePoint(7);
  CHECK(app->PaintRegionWithLabel(Box(8, 10), 9, "box at 7") > 0, "paint at time point 7");
  CHECK(app->PaintRegionWithLabel(Box(12, 13), 2, "box at 7, undone") > 0, "another at 7");
  app->Undo();
  std::string segfile6 = work + "/seg4d.nii.gz";
  Save(app, seg4d, segfile6);
  std::string saved_checksum6 = TraceLog::ComputeChecksum(seg4d);
  app->StopTraceLog();

  TraceLogReplay replay6;
  replay6.Read(TraceLog::SidecarFileName(segfile6));
  CHECK(replay6.GetBase().kind == "loaded" && replay6.GetBase().tps == 11,
        "the 4D log starts from the loaded segmentation");
  CHECK(replay6.GetSteps().size() == 4 && replay6.GetSteps()[0].tp == 3 &&
          replay6.GetSteps()[1].tp == 7,
        "the edits are recorded at their time points");
  TraceLogReplay::LabelImage4D::Pointer seg4d_file = ReadSegmentation(testdata + "/seg4d_11f.nii.gz");
  CHECK(TraceLogReplay::ComputeChecksum(seg4d_file) == replay6.GetBase().checksum,
        "the file's checksum matches the loaded starting point");
  for (const TraceLogReplay::Step &s : replay6.GetSteps())
    CHECK(TraceLogReplay::ApplyStep(seg4d_file, s) == 0, "each 4D step applies exactly");
  CHECK(TraceLogReplay::ComputeChecksum(seg4d_file) == saved_checksum6, "the 4D replay is exact");

  if (g_failures)
    std::cerr << g_failures << " check(s) failed" << std::endl;
  else
    std::cout << "All trace log checks passed" << std::endl;
  return g_failures ? 1 : 0;
}
