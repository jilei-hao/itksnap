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
 * A replay applies the edits, undos and redos of one segmentation layer, in
 * order, starting from that layer's last starting point in the log (the state
 * when recording started, or when the layer was added or loaded). By default
 * the layer is the one saved last, which is the segmentation that a copy of
 * the log saved next to a segmentation file belongs to.
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

  /** The "session" event: user, session id, start time, application */
  const Json::Value &GetSession() const { return m_Session; }

  /** Number of events of each type */
  const std::map<std::string, unsigned long> &GetEventCounts() const { return m_EventCounts; }

  /** Duration of the log, in milliseconds */
  unsigned long long GetDurationMs() const { return m_LastMs; }

  /** Geometry of the main image from the last "image" event, if any */
  bool HasImageGeometry() const { return m_HasImage; }
  const Json::Value &GetImageGeometry() const { return m_Image; }

  /** Layers that have a starting point in the log */
  std::vector<unsigned long> GetLayers() const;

  /** The layer to replay. Defaults to the layer saved last. */
  unsigned long GetTargetLayer() const { return m_TargetLayer; }
  void SetTargetLayer(unsigned long layer);

  /** Starting point of the target layer */
  const LayerState &GetBase() const { return m_Base; }

  /** Changes to the target layer after its starting point, in order */
  const std::vector<Step> &GetSteps() const { return m_Steps; }

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

  Json::Value m_Session, m_Image;
  bool        m_HasImage = false;
  std::map<std::string, unsigned long> m_EventCounts;
  unsigned long long m_LastMs = 0;

  // Everything read from the file, for all layers
  std::map<unsigned long, std::vector<LayerState>> m_LayerStates;
  std::vector<std::pair<unsigned long, Step>> m_AllSteps;
  std::vector<std::pair<unsigned long, LayerState>> m_Saves;

  // Selection for the target layer
  unsigned long m_TargetLayer = 0;
  LayerState m_Base;
  std::vector<Step> m_Steps;
  std::string m_FinalChecksum;
};

#endif // TRACELOGREPLAY_H
