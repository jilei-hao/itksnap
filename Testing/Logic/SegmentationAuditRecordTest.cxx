/*=========================================================================

  Program:   ITK-SNAP
  Module:    SegmentationAuditRecordTest.cxx
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

// L1 (logic-tier, Qt-free) test for the segmentation audit record. It proves
// two things: (1) the audit machinery links against itksnaplogic alone, and
// (2) BuildFromDeltas correctly reconstructs the changed-voxel count, tight
// bounding box, and before/after label histograms from an RLE delta walked
// against the post-edit image -- exactly the path exercised at commit time.

#include "SegmentationAuditRecord.h"
#include "SNAPCommon.h"
#include "UndoDataManager.h"
#include "LabelImageWrapper.h"   // for the production RLE image type

#include "itkImage.h"
#include "itkImageRegionIteratorWithIndex.h"

#include <iostream>
#include <list>
#include <string>

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

typedef itk::Image<LabelType, 3> TestImageType;

// Build a delta by painting `new_label` (respecting an old value) into a box
// [lo,hi] of `image`, encoding (new - old) in raster order over the whole
// image region -- mirroring what SegmentationUpdateIterator does at edit time.
// Templated so it runs over both a plain itk::Image and the production RLE
// label image, exercising the same iterator ordering the reconstruction relies
// on.
template <class TImage>
UndoDelta<LabelType> *
PaintBox(TImage *image, LabelType new_label,
         const itk::Index<3> &lo, const itk::Index<3> &hi)
{
  UndoDelta<LabelType> *delta = new UndoDelta<LabelType>();
  typename TImage::RegionType region = image->GetLargestPossibleRegion();
  delta->SetRegion(region);

  itk::ImageRegionIteratorWithIndex<TImage> it(image, region);
  for (; !it.IsAtEnd(); ++it)
  {
    itk::Index<3> idx = it.GetIndex();
    bool inside = idx[0] >= lo[0] && idx[0] <= hi[0] &&
                  idx[1] >= lo[1] && idx[1] <= hi[1] &&
                  idx[2] >= lo[2] && idx[2] <= hi[2];
    LabelType old_val = it.Get();
    LabelType new_val = inside ? new_label : old_val;
    delta->Encode((LabelType)(new_val - old_val));  // per-voxel delta
    it.Set(new_val);                                // apply to image
  }
  delta->FinishEncoding();
  return delta;
}

} // anonymous namespace

int main(int, char *[])
{
  // ---- Scenario 1: paint a 2x2x2 box (label 0 -> 3) --------------------
  {
    TestImageType::Pointer img = TestImageType::New();
    TestImageType::RegionType r;
    r.SetIndex({{0, 0, 0}});
    r.SetSize({{4, 4, 4}});
    img->SetRegions(r);
    img->Allocate();
    img->FillBuffer(0);

    itk::Index<3> lo = {{1, 1, 1}}, hi = {{2, 2, 2}};
    UndoDelta<LabelType> *delta = PaintBox(img.GetPointer(),3, lo, hi);

    std::list<UndoDelta<LabelType> *> deltas{delta};
    SegmentationAuditRecord rec = SegmentationAuditRecord::BuildFromDeltas(
      img.GetPointer(), deltas, "Test paint", SegmentationAuditRecord::AGENT, 0);

    CHECK(rec.changed_voxels == 8, "changed_voxels should be 8");
    CHECK(rec.actor == SegmentationAuditRecord::AGENT, "actor should be AGENT");
    CHECK(rec.op == "Test paint", "op should round-trip");
    CHECK(rec.bbox_valid, "bbox should be valid");
    CHECK(rec.bbox_min[0] == 1 && rec.bbox_min[1] == 1 && rec.bbox_min[2] == 1,
          "bbox min should be (1,1,1)");
    CHECK(rec.bbox_max[0] == 2 && rec.bbox_max[1] == 2 && rec.bbox_max[2] == 2,
          "bbox max should be (2,2,2)");
    CHECK(rec.before_counts.size() == 1 && rec.before_counts[0] == 8,
          "before_counts should be {0:8}");
    CHECK(rec.after_counts.size() == 1 && rec.after_counts[3] == 8,
          "after_counts should be {3:8}");

    std::string js = rec.ToJSON();
    std::cout << "scenario1 json: " << js << std::endl;
    CHECK(js.find("\"actor\":\"agent\"") != std::string::npos, "json has agent actor");
    CHECK(js.find("\"changed_voxels\":8") != std::string::npos, "json has changed_voxels");
    CHECK(js.find("\"before_counts\":{\"0\":8}") != std::string::npos, "json before_counts");
    CHECK(js.find("\"after_counts\":{\"3\":8}") != std::string::npos, "json after_counts");
    CHECK(js.find("\"min\":[1,1,1]") != std::string::npos, "json bbox min");
    CHECK(js.find("\"timestamp\":\"") != std::string::npos, "json has timestamp");

    delete delta;
  }

  // ---- Scenario 2: relabel with a non-zero "before" label (3 -> 5) ------
  // Pre-set a box to label 3 (as prior state), then a single delta relabels a
  // sub-box 3 -> 5. Confirms before/after histograms with non-zero old labels.
  {
    TestImageType::Pointer img = TestImageType::New();
    TestImageType::RegionType r;
    r.SetIndex({{0, 0, 0}});
    r.SetSize({{4, 4, 4}});
    img->SetRegions(r);
    img->Allocate();
    img->FillBuffer(0);

    // Prior state: a 4x4 box on the z=0 plane is label 3 (set directly).
    itk::ImageRegionIteratorWithIndex<TestImageType> pit(img, r);
    for (; !pit.IsAtEnd(); ++pit)
      if (pit.GetIndex()[2] == 0)
        pit.Set(3);

    // Single delta: relabel a 2x2 sub-box of the plane 3 -> 5.
    itk::Index<3> lo = {{0, 0, 0}}, hi = {{1, 1, 0}};
    UndoDelta<LabelType> *delta = PaintBox(img.GetPointer(),5, lo, hi);

    std::list<UndoDelta<LabelType> *> deltas{delta};
    SegmentationAuditRecord rec = SegmentationAuditRecord::BuildFromDeltas(
      img.GetPointer(), deltas, "Relabel", SegmentationAuditRecord::HUMAN, 0);

    CHECK(rec.actor == SegmentationAuditRecord::HUMAN, "scenario2 actor HUMAN");
    CHECK(rec.changed_voxels == 4, "scenario2 changed 4 voxels");
    CHECK(rec.before_counts.size() == 1 && rec.before_counts[3] == 4,
          "scenario2 before_counts {3:4}");
    CHECK(rec.after_counts.size() == 1 && rec.after_counts[5] == 4,
          "scenario2 after_counts {5:4}");
    std::cout << "scenario2 json: " << rec.ToJSON() << std::endl;

    delete delta;
  }

  // ---- Scenario 2b: multi-delta accumulation (the paintbrush pattern) ---
  // Two non-overlapping deltas painting distinct boxes to the same label are
  // committed as one record, mirroring a paintbrush stroke's intermediate
  // deltas. The record must accumulate counts and union the bounding box.
  {
    TestImageType::Pointer img = TestImageType::New();
    TestImageType::RegionType r;
    r.SetIndex({{0, 0, 0}});
    r.SetSize({{4, 4, 4}});
    img->SetRegions(r);
    img->Allocate();
    img->FillBuffer(0);

    UndoDelta<LabelType> *d1 = PaintBox(img.GetPointer(),7, {{0, 0, 0}}, {{0, 0, 0}}); // 1 voxel
    UndoDelta<LabelType> *d2 = PaintBox(img.GetPointer(),7, {{3, 3, 3}}, {{3, 3, 3}}); // 1 voxel

    std::list<UndoDelta<LabelType> *> deltas{d1, d2};
    SegmentationAuditRecord rec = SegmentationAuditRecord::BuildFromDeltas(
      img.GetPointer(), deltas, "Drawing with paintbrush",
      SegmentationAuditRecord::HUMAN, 0);

    CHECK(rec.changed_voxels == 2, "scenario2b changed 2 voxels across 2 deltas");
    CHECK(rec.after_counts.size() == 1 && rec.after_counts[7] == 2,
          "scenario2b after_counts {7:2}");
    CHECK(rec.bbox_min[0] == 0 && rec.bbox_min[1] == 0 && rec.bbox_min[2] == 0,
          "scenario2b bbox min (0,0,0)");
    CHECK(rec.bbox_max[0] == 3 && rec.bbox_max[1] == 3 && rec.bbox_max[2] == 3,
          "scenario2b bbox max unions to (3,3,3)");
    std::cout << "scenario2b json: " << rec.ToJSON() << std::endl;

    delete d1;
    delete d2;
  }

  // ---- Scenario 2c: OVERLAPPING same-voxel deltas in one commit --------
  // The correctness invariant behind the reconstruction: within a commit a
  // revisited voxel encodes a ZERO delta (same active label), so walking each
  // delta against the final image never double-counts. Delta 1 paints a box
  // 0 -> 4; delta 2 re-paints the SAME box to 4 (already 4 -> d == 0). The
  // record must count each voxel exactly once.
  {
    TestImageType::Pointer img = TestImageType::New();
    TestImageType::RegionType r;
    r.SetIndex({{0, 0, 0}});
    r.SetSize({{4, 4, 4}});
    img->SetRegions(r);
    img->Allocate();
    img->FillBuffer(0);

    itk::Index<3> lo = {{1, 1, 1}}, hi = {{2, 2, 2}};
    UndoDelta<LabelType> *d1 = PaintBox(img.GetPointer(), (LabelType)4, lo, hi);
    UndoDelta<LabelType> *d2 = PaintBox(img.GetPointer(), (LabelType)4, lo, hi); // all d==0

    std::list<UndoDelta<LabelType> *> deltas{d1, d2};
    SegmentationAuditRecord rec = SegmentationAuditRecord::BuildFromDeltas(
      img.GetPointer(), deltas, "Overlap", SegmentationAuditRecord::HUMAN, 0);

    CHECK(rec.changed_voxels == 8, "scenario2c counts each voxel once (not 16)");
    CHECK(rec.before_counts.size() == 1 && rec.before_counts[0] == 8,
          "scenario2c before_counts {0:8}");
    CHECK(rec.after_counts.size() == 1 && rec.after_counts[4] == 8,
          "scenario2c after_counts {4:8}");
    std::cout << "scenario2c json: " << rec.ToJSON() << std::endl;

    delete d1;
    delete d2;
  }

  // ---- Scenario 3: serializer edge cases -------------------------------
  {
    SegmentationAuditRecord rec;
    rec.op = "quote\"and\\slash\nnewline";
    rec.timestamp = "2026-07-18T00:00:00Z";
    rec.actor = SegmentationAuditRecord::UNKNOWN;
    std::string js = rec.ToJSON();
    std::cout << "scenario3 json: " << js << std::endl;
    CHECK(js.find("\\\"and\\\\slash\\nnewline") != std::string::npos,
          "json escapes quotes, backslash, newline");
    CHECK(js.find("\"bbox\":{\"valid\":false}") != std::string::npos,
          "empty record has invalid bbox");
    CHECK(js.find("\"actor\":\"unknown\"") != std::string::npos, "unknown actor string");

    CHECK(SegmentationAuditRecord::ActorFromString("agent") == SegmentationAuditRecord::AGENT,
          "ActorFromString agent");
    CHECK(SegmentationAuditRecord::ActorFromString("human") == SegmentationAuditRecord::HUMAN,
          "ActorFromString human");
    CHECK(SegmentationAuditRecord::ActorFromString("bogus") == SegmentationAuditRecord::UNKNOWN,
          "ActorFromString bogus -> unknown");
  }

  // ---- Scenario 4: the PRODUCTION RLE label image type -----------------
  // Repeat scenario 1 on LabelImageWrapper::ImageType (the run-length-encoded
  // label image used at runtime). This confirms that ImageRegionConstIterator-
  // WithIndex walks the RLE image in the same raster order the delta was
  // encoded in -- the one property the plain-itk::Image cases cannot prove.
  {
    typedef LabelImageWrapper::ImageType RLEImageType;
    RLEImageType::Pointer img = RLEImageType::New();
    RLEImageType::RegionType r;
    r.SetIndex({{0, 0, 0}});
    r.SetSize({{4, 4, 4}});
    img->SetRegions(r);
    img->Allocate();
    img->FillBuffer(0);

    itk::Index<3> lo = {{1, 1, 1}}, hi = {{2, 2, 2}};
    UndoDelta<LabelType> *delta = PaintBox(img.GetPointer(), (LabelType)3, lo, hi);

    std::list<UndoDelta<LabelType> *> deltas{delta};
    SegmentationAuditRecord rec = SegmentationAuditRecord::BuildFromDeltas(
      img.GetPointer(), deltas, "RLE paint", SegmentationAuditRecord::AGENT, 0);

    CHECK(rec.changed_voxels == 8, "scenario4 (RLE) changed_voxels should be 8");
    CHECK(rec.bbox_valid && rec.bbox_min[0] == 1 && rec.bbox_min[1] == 1 &&
            rec.bbox_min[2] == 1 && rec.bbox_max[0] == 2 && rec.bbox_max[1] == 2 &&
            rec.bbox_max[2] == 2,
          "scenario4 (RLE) bbox (1,1,1)-(2,2,2)");
    CHECK(rec.before_counts.size() == 1 && rec.before_counts[0] == 8,
          "scenario4 (RLE) before_counts {0:8}");
    CHECK(rec.after_counts.size() == 1 && rec.after_counts[3] == 8,
          "scenario4 (RLE) after_counts {3:8}");
    std::cout << "scenario4 (RLE) json: " << rec.ToJSON() << std::endl;

    delete delta;
  }

  // ---- Scenario 5: the log tracks undo/redo ----------------------------
  // The log must describe the edits *in effect*: an undone edit has to leave
  // it (or an agent reading the whole log would be told about a correction the
  // human rolled back) and come back on redo. Temporary commits never enter the
  // log, so undoing one must not pop the genuine record beneath it.
  {
    typedef LabelImageWrapper::Image4DType Image4DType;
    Image4DType::Pointer img4d = Image4DType::New();
    Image4DType::RegionType r4;
    r4.SetIndex({{0, 0, 0, 0}});
    r4.SetSize({{4, 4, 4, 1}});
    img4d->SetRegions(r4);
    img4d->Allocate();
    img4d->FillBuffer(0);

    SmartPtr<LabelImageWrapper> wrapper = LabelImageWrapper::New();
    wrapper->UpdateWrappedImages(img4d);

    LabelImageWrapper::ImageType *img = wrapper->GetModifiableImage();
    itk::Index<3> lo1 = {{0, 0, 0}}, hi1 = {{1, 1, 1}};
    itk::Index<3> lo2 = {{2, 2, 2}}, hi2 = {{3, 3, 3}};

    wrapper->StoreUndoPoint("Edit A", PaintBox(img, 1, lo1, hi1));
    wrapper->StoreUndoPoint("Edit B", PaintBox(img, 2, lo2, hi2));

    CHECK(wrapper->GetAuditLog().size() == 2, "log should hold both edits");
    CHECK(wrapper->GetAuditLog().back().op == "Edit B", "newest record is Edit B");
    CHECK(wrapper->HasLastAuditRecord(), "last record valid after commits");

    // Undo B: it is no longer in effect, so it must leave the log.
    wrapper->Undo();
    CHECK(wrapper->GetAuditLog().size() == 1, "undo should drop the undone edit");
    CHECK(wrapper->GetAuditLog().back().op == "Edit A", "Edit A remains after undo");
    CHECK(!wrapper->HasLastAuditRecord(), "last record invalidated by undo");

    // Redo B: in effect again, so it returns and becomes the newest record.
    wrapper->Redo();
    CHECK(wrapper->GetAuditLog().size() == 2, "redo should restore the record");
    CHECK(wrapper->GetAuditLog().back().op == "Edit B", "restored record is Edit B");
    CHECK(wrapper->HasLastAuditRecord(), "last record valid again after redo");

    // A temporary commit is not audit-worthy; undoing it must leave the log alone.
    wrapper->StoreUndoPoint("Temporary undo point", PaintBox(img, 3, lo1, hi1));
    CHECK(wrapper->GetAuditLog().size() == 2, "temporary commit stays out of the log");
    wrapper->Undo();
    CHECK(wrapper->GetAuditLog().size() == 2,
          "undoing a temporary commit must not pop a real record");
    CHECK(wrapper->GetAuditLog().back().op == "Edit B",
          "Edit B survives the temporary commit's undo");

    std::cout << "scenario5 log size after undo/redo: "
              << wrapper->GetAuditLog().size() << std::endl;
  }

  if (g_failures == 0)
    std::cout << "SegmentationAuditRecordTest: ALL PASS" << std::endl;
  else
    std::cerr << "SegmentationAuditRecordTest: " << g_failures << " failure(s)" << std::endl;

  return g_failures == 0 ? 0 : 1;
}
