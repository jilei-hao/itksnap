/*=========================================================================

  Program:   ITK-SNAP
  Module:    TraceLog.h
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
#ifndef TRACELOG_H
#define TRACELOG_H

#include "SNAPCommon.h"
#include "RLEImageRegionIterator.h"
#include "itkImageRegionConstIterator.h"
#include "itkObject.h"
#include "itkObjectFactory.h"
#include "itkImageRegion.h"
#include "json/json.h"

#include <chrono>
#include <cstdint>
#include <fstream>
#include <list>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

template <typename TPixel> class UndoDelta;
class LabelImageWrapper;
class ImageWrapperBase;
class SegmentationAuditRecord;

/**
 * \class TraceLog
 * \brief Records a segmentation session as a timeline that can be replayed.
 *
 * The log is a JSON Lines file: one event per line, appended as it happens,
 * so that it survives a crash. Every event has a sequence number "seq", the
 * milliseconds since the session started "ms" and a "type".
 *
 * Events that change a segmentation ("edit", "undo", "redo") carry the voxels
 * they changed, as runs over a region in raster order: [n] for n unchanged
 * voxels and [n, old, new] for n voxels that went from label old to label new.
 * Storing the new label (not the difference) lets a replay check each voxel's
 * old label and stop at the first step where a segmentation diverges from the
 * log (see TraceLogReplay). Every change is recorded, including the temporary
 * ones the deep learning tools make and undo, so that a replay never misses
 * one. "layer" events give the starting point of each segmentation layer with
 * a checksum, and "save" events the checksum of what was saved, so that a
 * replay can show it reproduced the saved file.
 *
 * Other events (view, cursor, tool, labels, layout, contrast) are for studying
 * how a structure was traced; a replay ignores them.
 *
 * No file names or paths are written: images are identified by geometry and
 * segmentations by checksum.
 *
 * While recording, the log is written to a working file. Each time a
 * segmentation is saved, the part of the log since its image was loaded is
 * copied next to it (SidecarFileName).
 */
class TraceLog : public itk::Object
{
public:
  irisITKObjectMacro(TraceLog, itk::Object)

  typedef UndoDelta<LabelType>       Delta;
  typedef std::list<Delta *>         DeltaList;
  typedef itk::ImageRegion<3>        RegionType;
  typedef RLEImage<LabelType>        LabelImage;

  /** Version of the file format, written in the "session" event */
  static const int FORMAT_VERSION = 1;

  /** Name of the file format, written in the "session" event */
  static const char *FormatName() { return "itksnap-trace"; }

  /**
   * Start recording into a new file in directory dir. The user is a
   * pseudonymous identifier chosen by the person being recorded. Returns false
   * if the file cannot be created. Makes this log the active one.
   */
  bool Start(const std::string &dir, const std::string &user, const std::string &app_version);

  /** Stop recording and close the file */
  void Stop();

  bool IsRecording() const { return m_Stream.is_open(); }

  irisGetMacro(FileName, const std::string &)
  irisGetMacro(SessionId, const std::string &)
  irisGetMacro(User, const std::string &)
  irisGetMacro(NumberOfEvents, unsigned long)

  /**
   * Why recording stopped by itself, if it did: the file could not be written
   * (disk full, drive removed). Recording stops at the first failed write and
   * the log fires an itk::ModifiedEvent. Empty while all is well.
   */
  irisGetMacro(WriteError, const std::string &)

  /**
   * The log that is recording, or nullptr. Segmentation commits are reported
   * through this, because the commit point (LabelImageWrapper::StoreUndoPoint)
   * has no path back to the application that owns the log.
   */
  static TraceLog *GetActive() { return s_Active; }

  /** Write an event. Fields are merged into the event object. */
  void Log(const std::string &type, const Json::Value &fields = Json::Value(Json::objectValue));

  /**
   * Record a change of some state, such as one view's zoom and pan, merging
   * repeated changes of the same key into one event. A drag that pans a view
   * fires dozens of changes per second; they become a single event with the
   * state before ("from") and after ("to") the gesture, the number of changes
   * "n" and the time span. Changes are merged while they come less than
   * COALESCE_MS apart and no other kind of event is written in between.
   * Several keys can be merging at once, as when zooming one view zooms the
   * linked views too. A state equal to the last one recorded for the key is
   * ignored.
   */
  void LogStateChange(const std::string &type, const std::string &key,
                      const Json::Value &state, const Json::Value &fields = Json::Value());

  /** Set the last known state of a key without writing an event */
  void PrimeState(const std::string &key, const Json::Value &state);

  /** Write the merged events in progress, if any */
  void FlushPendingState();

  static const unsigned int COALESCE_MS = 1000;

  /**
   * Context for the events written while it exists, such as the view and tool
   * of a paintbrush stroke. Set it around the call that commits the edit, so
   * the context belongs to that call rather than to whatever happens next.
   * Contexts nest; inner fields override outer ones. Does nothing if no log
   * is recording, or if the context is null.
   */
  class ScopedContext
  {
  public:
    ScopedContext(const Json::Value &ctx);
    ~ScopedContext();
    ScopedContext(const ScopedContext &) = delete;
    ScopedContext &operator=(const ScopedContext &) = delete;
  private:
    TraceLog *m_Log;
  };

  /** The merged current context, or a null value */
  Json::Value GetContext() const;

  /**
   * Record a change to a segmentation: kind is "edit", "undo" or "redo". The
   * deltas have already been applied to image (the time point tp of the layer)
   * with the given sign (+1 for an edit or redo, -1 for an undo). The audit
   * record, if given, supplies the actor.
   */
  void LogSegmentationChange(const char *kind, unsigned long layer, unsigned int tp,
                             const LabelImage *image, const DeltaList &deltas, int sign,
                             const std::string &op, const SegmentationAuditRecord *audit);

  /**
   * Record the starting point of a segmentation layer: kind is "present",
   * "added" or "loaded". The event has the layer's checksum and whether it is
   * blank; a layer that is not blank also gets its labels, as "content": runs
   * [n, label] over all its voxels in raster order (x fastest, then y, z and
   * time point). So a replay can start from the log alone, without the file
   * the layer was loaded from. Loading a segmentation can report the same
   * layer several times; content written before (since the current main
   * image was loaded) is referred to by its event, as "content_seq".
   */
  void LogLayer(LabelImageWrapper *seg, const char *kind);

  /**
   * The checksum of a segmentation, its content as JSON text (see LogLayer)
   * and whether it is blank, in one pass
   */
  static void EncodeLayer(LabelImageWrapper *seg, std::string &checksum, std::string &content,
                          bool &blank);

  /**
   * Bring the set of known segmentation layers up to date: layers not seen
   * before get a "layer" event of kind "added", the layer whose content was
   * just read from a file (if any) one of kind "loaded", and layers that are
   * gone a "layer_removed" event.
   */
  void TraceLayers(const std::vector<LabelImageWrapper *> &layers,
                   LabelImageWrapper *loaded = nullptr);

  /** Record that the undo history of a layer was cleared */
  void LogUndoCleared(unsigned long layer);

  /** Record the geometry of the main image, or that it was unloaded (nullptr) */
  void LogMainImage(ImageWrapperBase *main);

  /**
   * Record that a segmentation was saved, and copy the log next to the saved
   * file: the session event, and everything since the current main image was
   * loaded, so that edits made earlier to another image do not go with it.
   * The path is not written to the log. Returns false if the copy could not
   * be written.
   */
  bool LogSegmentationSaved(LabelImageWrapper *seg, const std::string &fname);

  /**
   * Checksum of all time points of a segmentation, as "fnv1a64:<hex>": an
   * FNV-1a 64 bit hash of the label runs of each image row (see
   * ChecksumHasher). It detects accidental differences; it is not proof
   * against deliberate tampering.
   */
  static std::string ComputeChecksum(LabelImageWrapper *seg);

  /**
   * Builds the checksum. Rows are visited in raster order (y, then z, then
   * time point). Each row is split into maximal runs of one label, and each
   * run adds its length (4 bytes) and label (2 bytes), low bytes first. Hashing
   * runs keeps the checksum of an RLE segmentation quick to compute.
   */
  class ChecksumHasher
  {
  public:
    void Add(LabelType label, std::uint32_t count)
    {
      if (m_Count > 0 && label != m_Label)
        Flush();
      m_Label = label;
      m_Count += count;
    }
    void EndRow() { Flush(); }
    unsigned long long GetHash() const { return m_Hash; }

  private:
    void Flush()
    {
      if (m_Count == 0)
        return;
      for (int i = 0; i < 4; i++)
        Byte((unsigned char)(m_Count >> (8 * i)));
      Byte((unsigned char)(m_Label & 0xff));
      Byte((unsigned char)(m_Label >> 8));
      m_Count = 0;
    }
    void Byte(unsigned char b) { m_Hash = (m_Hash ^ b) * 1099511628211ULL; }

    unsigned long long m_Hash = 14695981039346656037ULL;
    LabelType          m_Label = 0;
    std::uint32_t      m_Count = 0;
  };

  /** Add an RLE image (one time point of a segmentation) to the checksum */
  static void AddToChecksum(ChecksumHasher &hasher, const LabelImage *image);

  /** Add a plain image of any dimension to the checksum */
  template <class TImage>
  static void AddToChecksum(ChecksumHasher &hasher, const TImage *image);

  static std::string FormatChecksum(unsigned long long hash);

  /** File name of the copy of the log kept next to a segmentation file */
  static std::string SidecarFileName(const std::string &seg_file);

  /** The net change made by a set of deltas (see EncodeChange) */
  struct EncodedChange
  {
    RegionType    region;
    unsigned long changed = 0;

    // The runs, as JSON text (see the class description)
    std::string runs;

    // Label counts of the changed voxels before and after the change
    std::map<LabelType, unsigned long> before, after;
  };

  /**
   * Encode the net change made by a set of deltas to image, as runs over the
   * bounding region of the deltas. Overlapping deltas are summed, so a voxel
   * painted twice in one stroke is reported once, with its label before the
   * first delta and after the last. The runs are written as text directly,
   * since a whole-image operation can have millions of them.
   */
  static void EncodeChange(const LabelImage *image, const DeltaList &deltas, int sign,
                           EncodedChange &change);

  static Json::Value RegionToJSON(const RegionType &region);

protected:
  TraceLog();
  virtual ~TraceLog();

  // Write an event; raw_value, if given, is JSON text added as member raw_key
  void WriteLocked(Json::Value &event, const char *raw_key = nullptr,
                   const std::string *raw_value = nullptr);
  void LogLocked(const std::string &type, const Json::Value &fields,
                 const char *raw_key = nullptr, const std::string *raw_value = nullptr);
  void FlushPendingLocked();
  void FailLocked(const std::string &what);
  unsigned long long ElapsedMs() const;

  static TraceLog *s_Active;

  std::ofstream m_Stream;
  std::string m_FileName, m_SessionId, m_User, m_WriteError;
  unsigned long m_NumberOfEvents = 0;
  std::chrono::steady_clock::time_point m_StartTime;

  // The "session" line, and where the event for the current main image
  // starts in the file (see LogSegmentationSaved)
  std::string m_SessionLine;
  std::streamoff m_ImageOffset = 0;

  // Merged state changes in progress, by key, in the order they started (see
  // LogStateChange), and the last state recorded for each key
  struct PendingChange
  {
    std::string        key;
    Json::Value        event;
    unsigned long long last_ms = 0;
  };
  std::vector<PendingChange> m_Pending;
  std::map<std::string, Json::Value> m_LastState;

  // Context stack (see ScopedContext)
  std::vector<Json::Value> m_Context;

  // Segmentation layers that have a "layer" event
  std::set<unsigned long> m_KnownLayers;

  // The event that holds the content with a given checksum, since the current
  // main image was loaded (see LogLayer)
  std::map<std::string, unsigned long> m_ContentSeq;

  mutable std::recursive_mutex m_Mutex;
};

template <class TImage>
void TraceLog::AddToChecksum(ChecksumHasher &hasher, const TImage *image)
{
  // Rows run along x; raster order visits them in the same order as the rows
  // of an RLE image, one time point after another
  const typename TImage::RegionType &region = image->GetBufferedRegion();
  const unsigned long width = region.GetSize(0);
  unsigned long x = 0;
  for (itk::ImageRegionConstIterator<TImage> it(image, region); !it.IsAtEnd(); ++it)
  {
    hasher.Add(it.Get(), 1);
    if (++x == width)
    {
      hasher.EndRow();
      x = 0;
    }
  }
}

#endif // TRACELOG_H
