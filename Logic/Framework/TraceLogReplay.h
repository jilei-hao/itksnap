/*=========================================================================

  Program:   ITK-SNAP
  Module:    TraceLogReplay.h
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
#ifndef TRACELOGREPLAY_H
#define TRACELOGREPLAY_H

#include "SNAPCommon.h"
#include "itkImage.h"
#include "itkImageRegion.h"
#include "json/json.h"

#include <map>
#include <string>
#include <vector>

/**
 * \class TraceLogReplay
 * \brief Reads a trace log (see TraceLog) and rebuilds a segmentation from it.
 *
 * A log can hold several recording sessions: the copy saved next to a
 * segmentation file starts with the sessions of that file's history, oldest
 * first (see TraceLog::LogLayer). Event numbers and layer ids belong to their
 * session.
 *
 * A replay applies the edits, undos and redos of one segmentation layer, in
 * order. By default the layer is the one saved last in the latest session,
 * which is the segmentation that a copy of the log saved next to a file
 * belongs to. The replay starts from the layer's last starting point (the
 * state when recording started, or when the layer was added or loaded), and
 * follows its history back from there: a layer loaded from a file starts
 * from what an earlier save wrote (in the same session or an earlier one),
 * found by its checksum, so the replay goes on from that save's own starting
 * point, and so on. It stops at a blank start, at content that was not loaded
 * (a layer added with labels), or at a loaded file that no save in the log
 * wrote (it was changed while nothing was recording).
 *
 * Each step records the old label of every voxel it changed. A voxel whose
 * label differs from that is a mismatch: the segmentation has diverged from
 * the log, and a replay should stop there.
 */
class TraceLogReplay
{
public:
  typedef itk::Image<LabelType, 4> LabelImage4D;

  /** A run of voxels in raster order over a step's region */
  struct Run
  {
    unsigned long length = 0;
    bool          changed = false;
    LabelType     old_label = 0, new_label = 0;
  };

  /** One change to the segmentation: an edit, undo or redo */
  struct Step
  {
    unsigned int        session = 0; // index in GetSessions()
    unsigned long       seq = 0;
    std::string         kind, op;
    unsigned int        tp = 0;
    itk::ImageRegion<3> region;
    unsigned long       changed = 0;
    std::vector<Run>    runs;
  };

  /** The starting point of a layer ("layer" event) */
  struct LayerState
  {
    unsigned int  session = 0; // index in GetSessions()
    unsigned long seq = 0;
    std::string   kind, checksum;
    unsigned int  size[3] = { 0, 0, 0 };
    unsigned int  tps = 0;

    // Whether the layer was blank, and if not, its labels as runs over all its
    // voxels in raster order (empty in logs recorded before ITK-SNAP stored them)
    bool                                             blank = false;
    std::vector<std::pair<unsigned long, LabelType>> content;
  };

  /** Read a trace log. Throws IRISException if it is not a valid trace log. */
  void Read(const std::string &file);

  /** One recording session in the log */
  struct Session
  {
    Json::Value                          header; // the "session" event
    unsigned long long                   duration_ms = 0;
    std::map<std::string, unsigned long> event_counts;
  };

  /** The sessions in the log, oldest first */
  const std::vector<Session> &GetSessions() const { return m_SessionList; }

  /** The "session" event of the latest session: user, id, start time, application */
  const Json::Value &GetSession() const { return m_SessionList.back().header; }

  /** Number of events of each type, in all sessions */
  const std::map<std::string, unsigned long> &GetEventCounts() const { return m_EventCounts; }

  /** Recording time of all sessions, in milliseconds */
  unsigned long long GetDurationMs() const { return m_DurationMs; }

  /** Geometry of the main image from the last "image" event, if any */
  bool HasImageGeometry() const { return m_HasImage; }
  const Json::Value &GetImageGeometry() const { return m_Image; }

  /** Layers that have a starting point in the latest session */
  std::vector<unsigned long> GetLayers() const;

  /** The layer to replay, in the latest session. Defaults to the layer saved last. */
  unsigned long GetTargetLayer() const { return m_TargetLayer; }
  void SetTargetLayer(unsigned long layer);

  /** Starting point of the replay */
  const LayerState &GetBase() const { return m_Base; }

  /** Changes to the target layer after the starting point, in order */
  const std::vector<Step> &GetSteps() const { return m_Steps; }

  /** Number of sessions the replay goes through */
  unsigned int GetReplaySessions() const;

  /**
   * Whether the replay starts after the first session of the log, from a
   * segmentation that was loaded but that no save in the log wrote: the file
   * was changed while nothing was recording, so the history before that point
   * cannot be replayed onto it.
   */
  bool IsHistoryIncomplete() const { return m_HistoryIncomplete; }

  /**
   * Start the replay at the first point of the history where the target
   * layer's segmentation had the given checksum: a starting point, or a save.
   * Returns false, and changes nothing, if it never had it. A start at a save
   * has no content in the log (HasStartingSegmentation is false): the
   * segmentation to start from has to be given.
   */
  bool StartFrom(const std::string &checksum);

  /**
   * Checksum of the target layer when it was last saved, if that save came
   * after the last step, so that it describes the result of a full replay.
   * Empty otherwise.
   */
  const std::string &GetFinalChecksum() const { return m_FinalChecksum; }

  /**
   * Apply a step to a 4D segmentation (x, y, z, time point). Voxels whose
   * label is not the step's old label are left alone and counted; the return
   * value is that count, so 0 means the step applied exactly.
   */
  static unsigned long ApplyStep(LabelImage4D *image, const Step &step);

  /** Checksum of a 4D segmentation, comparable with TraceLog checksums */
  static std::string ComputeChecksum(const LabelImage4D *image);

  /** A blank segmentation with the size and geometry recorded in the log */
  LabelImage4D::Pointer CreateBlankSegmentation() const;

  /**
   * Whether the log holds the target layer's starting point itself: it was
   * blank, or its labels are in the log. If not, a replay needs the
   * segmentation file the layer started from.
   */
  bool HasStartingSegmentation() const { return m_Base.blank || !m_Base.content.empty(); }

  /** The target layer's starting point, from the log (see HasStartingSegmentation) */
  LabelImage4D::Pointer CreateStartingSegmentation() const;

  /** Checksum of a blank segmentation of the given size */
  static std::string BlankChecksum(const unsigned int size[3], unsigned int tps);

protected:
  void Select();

  // Rebuild the base and steps from the links of the history
  void Concatenate();

  std::vector<Session> m_SessionList;
  Json::Value          m_Image;
  bool                 m_HasImage = false;
  std::map<std::string, unsigned long> m_EventCounts;
  unsigned long long   m_DurationMs = 0;

  // Everything read from the file, for all layers, by session
  struct SessionData
  {
    std::map<unsigned long, std::vector<LayerState>>  layer_states;
    std::vector<std::pair<unsigned long, Step>>       steps;
    std::vector<std::pair<unsigned long, LayerState>> saves;
  };
  std::vector<SessionData> m_Data;

  // The history of the target layer: one link per starting point, oldest
  // first, each with the steps from it to the save the next link loaded
  struct Link
  {
    LayerState        base;
    unsigned long     layer = 0;
    std::vector<Step> steps;
  };
  std::vector<Link> m_Chain;

  // Selection for the target layer
  unsigned long m_TargetLayer = 0;
  LayerState m_Base;
  std::vector<Step> m_Steps;
  std::string m_FinalChecksum;
  bool m_HistoryIncomplete = false;
};

#endif // TRACELOGREPLAY_H
