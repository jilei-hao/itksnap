/*=========================================================================

  Program:   ITK-SNAP
  Module:    SegmentationAuditRecord.h
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
#ifndef __SegmentationAuditRecord_h_
#define __SegmentationAuditRecord_h_

#include "SNAPCommon.h"
#include "UndoDataManager.h"
#include "itkIndex.h"
#include "itkImageRegionConstIteratorWithIndex.h"

#include <string>
#include <map>
#include <list>

/**
 * \class SegmentationAuditRecord
 * \brief A structured, machine-consumable provenance record for a single
 * segmentation edit commit.
 *
 * ITK-SNAP's undo engine (\see UndoDataManager) already stores each edit as
 * an RLE label-delta commit, but that delta is provenance-*capable*, not an
 * audit trail: it carries no timestamp, no actor identity, no operation
 * semantics, and cannot be exported. This record layers exactly that metadata
 * on top of the existing delta so that an expert correction becomes a
 * first-class *return value* an external agent can consume, rather than a side
 * effect trapped inside the GUI.
 *
 * The record is deliberately toolkit-independent (no Qt): it is produced in
 * the Logic tier at commit time and serialized to JSON with ToJSON().
 *
 * The rich fields (bounding box, before/after label histograms) are
 * reconstructed from the committed delta(s) walked against the *current*
 * (post-edit) image: for each changed voxel, new = image value and
 * old = new - delta (modular LabelType arithmetic, exactly as Undo() recovers
 * the previous state).
 *
 * PRECONDITION for exactness: within a single commit no voxel is written to two
 * different non-zero values by different deltas. Every current edit path
 * satisfies this -- the active label is constant per commit, so a revisited
 * voxel encodes a zero delta (paintbrush strokes, polygon/lasso fill, 3D spray,
 * automatic-segmentation apply, slice drawing). If a future path violated it,
 * the changed-voxel count and before/after labels for the doubly-written voxels
 * would be reconstructed incorrectly.
 */
class SegmentationAuditRecord
{
public:
  /** Who performed the edit that produced this commit. */
  enum Actor
  {
    HUMAN = 0,
    AGENT,
    UNKNOWN
  };

  typedef itk::Index<3> IndexType;

  /** Human-readable operation / undo title, e.g. "Drawing with paintbrush". */
  std::string op;

  /** ISO-8601 UTC timestamp, e.g. "2026-07-18T21:33:05Z". */
  std::string timestamp;

  /** Actor that produced the commit (defaults to HUMAN). */
  Actor actor = HUMAN;

  /** Number of voxels whose label value actually changed in this commit. */
  unsigned long changed_voxels = 0;

  /** Number of RLE runs in the underlying delta (a proxy for edit complexity). */
  unsigned long rle_count = 0;

  /** 4D time point the commit was applied to. */
  int time_point = 0;

  /** Tight voxel-index bounding box of the changed voxels (inclusive). */
  bool      bbox_valid = false;
  IndexType bbox_min;
  IndexType bbox_max;

  /** Histogram of the label each changed voxel had *before* the commit. */
  std::map<LabelType, unsigned long> before_counts;

  /** Histogram of the label each changed voxel has *after* the commit. */
  std::map<LabelType, unsigned long> after_counts;

  /** Serialize to a compact, valid JSON object string. */
  std::string ToJSON() const;

  /** "human" / "agent" / "unknown". */
  static const char *ActorToString(Actor a);

  /** Parse an actor from a string (case-insensitive); UNKNOWN on no match. */
  static Actor ActorFromString(const std::string &s);

  /** Current wall-clock time formatted as ISO-8601 UTC. */
  static std::string NowIso8601Utc();

  /**
   * Build a record by walking the committed delta(s) against the current
   * (post-edit) image. Templated on the image type so it works both with the
   * production RLE label image and with a plain itk::Image in unit tests.
   */
  template <class TImage>
  static SegmentationAuditRecord
  BuildFromDeltas(const TImage                             *current_image,
                  const std::list<UndoDelta<LabelType> *>  &deltas,
                  const std::string                        &op,
                  Actor                                     actor,
                  int                                       time_point)
  {
    SegmentationAuditRecord rec;
    rec.op = op;
    rec.actor = actor;
    rec.time_point = time_point;
    rec.timestamp = NowIso8601Utc();

    typedef itk::ImageRegionConstIteratorWithIndex<TImage> IterType;
    for (UndoDelta<LabelType> *delta : deltas)
    {
      if (!delta)
        continue;

      rec.rle_count += delta->GetNumberOfRLEs();

      IterType it(current_image, delta->GetRegion());
      for (size_t i = 0; i < delta->GetNumberOfRLEs(); ++i)
      {
        size_t    n = delta->GetRLELength(i);
        LabelType d = delta->GetRLEValue(i);
        for (size_t j = 0; j < n; ++j)
        {
          if (d != 0)
          {
            // Recover the pre-commit label via modular LabelType arithmetic.
            LabelType new_label = it.Get();
            LabelType old_label = (LabelType)(new_label - d);

            rec.before_counts[old_label]++;
            rec.after_counts[new_label]++;
            rec.changed_voxels++;

            const typename TImage::IndexType &idx = it.GetIndex();
            if (!rec.bbox_valid)
            {
              for (int a = 0; a < 3; ++a)
              {
                rec.bbox_min[a] = idx[a];
                rec.bbox_max[a] = idx[a];
              }
              rec.bbox_valid = true;
            }
            else
            {
              for (int a = 0; a < 3; ++a)
              {
                if (idx[a] < rec.bbox_min[a])
                  rec.bbox_min[a] = idx[a];
                if (idx[a] > rec.bbox_max[a])
                  rec.bbox_max[a] = idx[a];
              }
            }
          }
          ++it;
        }
      }
    }

    return rec;
  }
};

#endif // __SegmentationAuditRecord_h_
