/*=========================================================================

  Program:   ITK-SNAP
  Module:    TraceLogModel.h
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
#ifndef TRACELOGMODEL_H
#define TRACELOGMODEL_H

#include "AbstractModel.h"
#include "SNAPCommon.h"
#include "json/json.h"

#include <map>
#include <memory>
#include <set>

class GlobalUIModel;
class GenericSliceModel;
class LabelImageWrapper;
class TraceLogReplay;

/**
 * \class TraceLogModel
 * \brief Records how a segmentation is traced, and replays trace logs.
 *
 * Starts and stops the application's TraceLog, and feeds it what the Logic
 * layer cannot see: the zoom and pan of each slice view, cursor moves (slice
 * changes), the active tool, the drawing and draw-over labels, the selected
 * segmentation, the view layout, the contrast of each image layer and the
 * free rotation of the image. It
 * listens to events the models fire anyway and writes only actual changes.
 * Edits are recorded by the Logic layer (see TraceLog).
 *
 * Interaction code wraps its calls in a TraceLog::ScopedContext built with
 * ViewContext, so that the events they cause say which view they came from.
 */
class TraceLogModel : public AbstractModel
{
public:
  irisITKObjectMacro(TraceLogModel, AbstractModel)

  void SetParentModel(GlobalUIModel *parent);

  /**
   * Start recording. The user is a pseudonymous identifier. Returns false if
   * the log file cannot be created.
   */
  bool StartRecording(const std::string &user);

  void StopRecording();

  bool IsRecording() const;

  /** The working file the log is being written to */
  std::string GetFileName() const;

  /**
   * Why recording stopped by itself (the log could not be written), or empty.
   * The model fires ModelUpdateEvent when that happens.
   */
  std::string GetWriteError() const;

  /**
   * Directory where working files are kept: the one set with
   * SetTraceLogDirectory, or TraceLogs in ITK-SNAP's application data folder
   */
  std::string GetTraceLogDirectory() const;
  void SetTraceLogDirectory(const std::string &dir) { m_Directory = dir; }

  /** Outcome of a replay */
  struct ReplayResult
  {
    unsigned long total_steps = 0, applied_steps = 0;

    // Sequence number of the step that did not apply (0 if all applied), and
    // how many of its voxels had a label other than the one in the log, or
    // whether it lies outside the segmentation. A step that does not apply is
    // left out entirely.
    unsigned long failed_seq = 0, mismatches = 0;
    bool outside = false;

    // Whether the full replay was compared with the saved segmentation
    bool compared = false, final_matches = false;

    // Whether the replay went into a new segmentation layer
    bool new_layer = false;
  };

  /**
   * The trace log saved next to the file of the selected segmentation, if
   * there is one, or an empty string
   */
  std::string FindLogOfSelectedSegmentation() const;

  /**
   * Read a trace log for replay, and check that it was recorded on an image of
   * the size of the one loaded. Returns the number of steps. Throws
   * IRISException if the log cannot be read or does not fit.
   */
  unsigned long PrepareReplay(const std::string &file);

  /**
   * Replay the first n steps of the prepared log. The replay goes into a new
   * segmentation layer, which starts as the segmentation the log started
   * from; the open segmentations are left alone. (A log recorded before
   * ITK-SNAP kept that starting segmentation is replayed onto the selected
   * segmentation, which PrepareReplay checks is the starting point.) Each step
   * is an edit that can be undone. Stops at a step that does not apply
   * exactly.
   */
  ReplayResult Replay(unsigned long n_steps);

  /**
   * Context naming a slice view and the action in it, for use with
   * TraceLog::ScopedContext. Null (and cheap) when nothing is recording.
   */
  static Json::Value ViewContext(GenericSliceModel *view, const char *action);

protected:
  TraceLogModel();
  virtual ~TraceLogModel();

  void OnCursorUpdate();
  void OnLayerChange();
  void OnTraceLogModified();
  void OnViewGeometryChange(itk::Object *source, const itk::EventObject &event);
  void OnToolbarModeChange();
  void OnLabelsChange();
  void OnSelectedSegmentationChange();
  void OnLayoutChange();
  void OnDisplayMappingChange();
  void OnRotationChange();

  // Record the state of everything watched, as one "snapshot" event
  void LogSnapshot();

  Json::Value GetCursorState() const;
  Json::Value GetViewState(unsigned int view) const;
  Json::Value GetToolState() const;
  Json::Value GetLabelState() const;
  Json::Value GetSegmentationState() const;
  Json::Value GetLayoutState() const;
  Json::Value GetRotationState() const;
  std::map<std::string, Json::Value> GetContrastStates() const;

  GlobalUIModel *m_Parent = nullptr;
  std::string m_Directory;

  // The slice views were still being set up when the cursor last moved, so
  // its state is taken again once they are (see OnViewGeometryChange)
  bool m_CursorNotReady = false;

  // The free rotation last recorded (see OnRotationChange)
  Json::Value m_LastRotation;
  std::unique_ptr<TraceLogReplay> m_Replay;
  bool m_ReplayIntoNewLayer = false;

  // Write the starting segmentation the log holds into a new, blank layer
  void FillStartingSegmentation(LabelImageWrapper *seg, std::set<LabelType> &labels);
};

#endif // TRACELOGMODEL_H
