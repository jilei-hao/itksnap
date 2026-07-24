/*=========================================================================

  Program:   ITK-SNAP
  Module:    $RCSfile: LabelImageWrapper.cxx,v $
  Language:  C++
  Date:      $Date: 2018/01/05 $
  Version:   $Revision: 1 $
  Copyright (c) 2018 Paul A. Yushkevich

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

  -----

  Copyright (c) 2003 Insight Software Consortium. All rights reserved.
  See ITKCopyright.txt or http://www.itk.org/HTML/Copyright.htm for details.

  This software is distributed WITHOUT ANY WARRANTY; without even
  the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
  PURPOSE.  See the above copyright notices for more information.

  -----

  Copyright (c) 2003 Insight Software Consortium. All rights reserved.
  See ITKCopyright.txt or http://www.itk.org/HTML/Copyright.htm for details.

  This software is distributed WITHOUT ANY WARRANTY; without even
  the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
  PURPOSE.  See the above copyright notices for more information.

=========================================================================*/
#include "LabelImageWrapper.h"
#include "UndoDataManager.h"
#include "Rebroadcaster.h"

#include <string>

// Upper bound on the in-memory audit log so it cannot grow without limit over a
// long session (mirroring the undo engine's own memory bound). The last record
// is always retained separately in m_LastAuditRecord.
static const size_t MAX_AUDIT_LOG = 4096;

// Throwaway commit name used by the smart-brush / lasso DLS paths, which commit
// a temporary edit only to immediately Undo() it. Such commits are not
// audit-worthy: they must neither consume the armed actor nor enter the log.
static const char *const TEMPORARY_UNDO_POINT_NAME = "Temporary undo point";

LabelImageWrapper::LabelImageWrapper()
{
}

LabelImageWrapper::~LabelImageWrapper()
{
  for(auto p : m_TimePointUndoManagers)
    delete p;
}

void LabelImageWrapper::UpdateWrappedImages(
    Image4DType *image_4d,
    ImageBaseType *refSpace,
    ITKTransformType *tran)
{
  Superclass::UpdateWrappedImages(image_4d, refSpace, tran);

  // Clean up existing undo managers
  for(auto p : m_TimePointUndoManagers)
    delete p;

  // Set up new undo managers
  m_TimePointUndoManagers.resize(this->GetNumberOfTimePoints());
  for(auto &p : m_TimePointUndoManagers)
    p = new UndoManagerType(4, 200000);

  // Modified event on the image is rebroadcast as the WrapperImageChangeEvent
  Rebroadcaster::Rebroadcast(image_4d, itk::ModifiedEvent(), this, WrapperImageChangeEvent());

  // Modified event on each of the timepoints should also be rebroadcast
  for(auto &img : this->m_ImageTimePoints)
    Rebroadcaster::Rebroadcast(img, itk::ModifiedEvent(), this, WrapperImageChangeEvent());
}

void LabelImageWrapper::StoreIntermediateUndoDelta(UndoManagerDelta *delta)
{
  UndoManagerType *um = m_TimePointUndoManagers[m_TimePointIndex];
  um->AddDeltaToStaging(delta);
}

void LabelImageWrapper::StoreUndoPoint(const char *text, UndoManagerDelta *delta)
{
  UndoManagerType *um = m_TimePointUndoManagers[m_TimePointIndex];

  // If there is a delta, add it to staging
  if(delta)
    um->AddDeltaToStaging(delta);

  // Commit the deltas
  int n_rles = um->CommitStaging(text);

  // Capture a structured audit record for a genuine, non-throwaway commit. The
  // before/after label counts and bounding box are reconstructed by walking the
  // committed delta(s) against the current (post-edit) image: for each changed
  // voxel new = image value and old = new - delta.
  bool is_temporary = (text && std::string(text) == TEMPORARY_UNDO_POINT_NAME);
  if(n_rles > 0 && !is_temporary)
    {
    m_LastAuditRecord = SegmentationAuditRecord::BuildFromDeltas(
          m_Image,
          um->GetLastCommit().GetDeltas(),
          text ? text : "",
          m_NextCommitActor,
          m_TimePointIndex);
    m_HasLastAuditRecord = true;

    m_AuditLog.push_back(m_LastAuditRecord);
    if(m_AuditLog.size() > MAX_AUDIT_LOG)
      m_AuditLog.erase(m_AuditLog.begin());

    // Consume the actor tag exactly when a record is captured, so it applies to
    // this commit only. (Callers must arm SetNextCommitActor immediately before
    // an operation known to produce a commit; a no-op operation leaves the tag
    // armed for the next real commit.)
    m_NextCommitActor = SegmentationAuditRecord::HUMAN;
    }
}

bool LabelImageWrapper::MoveAuditRecord(std::vector<SegmentationAuditRecord> &from,
                                        std::vector<SegmentationAuditRecord> &to)
{
  // Search backwards for the newest record belonging to the current time point:
  // each time point has its own undo manager, so the record produced by the
  // commit being undone/redone is the last one logged against this time point.
  for(std::vector<SegmentationAuditRecord>::reverse_iterator it = from.rbegin();
      it != from.rend(); ++it)
    {
    if(it->time_point == (int) m_TimePointIndex)
      {
      to.push_back(*it);
      from.erase((it + 1).base());
      return true;
      }
    }
  return false;
}

void LabelImageWrapper::ClearUndoPoints()
{
  UndoManagerType *um = m_TimePointUndoManagers[m_TimePointIndex];
  um->Clear();

  // Keep the audit trail consistent with the (now-empty) undo history.
  m_AuditLog.clear();
  m_UndoneAuditRecords.clear();
  m_HasLastAuditRecord = false;
}

void LabelImageWrapper::ClearUndoPointsForAllTimePoints()
{
  for(auto um : m_TimePointUndoManagers)
    um->Clear();

  m_AuditLog.clear();
  m_UndoneAuditRecords.clear();
  m_HasLastAuditRecord = false;
}

bool LabelImageWrapper::IsUndoPossible()
{
  UndoManagerType *um = m_TimePointUndoManagers[m_TimePointIndex];
  return um->IsUndoPossible();
}

void LabelImageWrapper::Undo()
{
  UndoManagerType *um = m_TimePointUndoManagers[m_TimePointIndex];

  // Get the commit for the undo
  const UndoManagerType::Commit &commit = um->GetCommitForUndo();

  // The label image that will undergo undo
  typedef itk::ImageRegionIterator<ImageType> IteratorType;

  // Iterate over all the deltas in reverse order
  UndoManagerType::DList::const_reverse_iterator dit = commit.GetDeltas().rbegin();
  for(; dit != commit.GetDeltas().rend(); ++dit)
    {
    // Apply the changes in the current delta
    UndoManagerType::Delta *delta = *dit;

    // Iterator for the relevant region in the label image
    IteratorType lit(m_Image, delta->GetRegion());

    // Iterate over the rles in the delta
    for(size_t i = 0; i < delta->GetNumberOfRLEs(); i++)
      {
      size_t n = delta->GetRLELength(i);
      LabelType d = delta->GetRLEValue(i);
      for(size_t j = 0; j < n; j++)
        {
        if(d != 0)
          lit.Set(lit.Get() - d);
        ++lit;
        }
      }
    }

  // Set modified flags
  this->PixelsModified();

  // The most recent commit has been reverted, so the "last audit record" no
  // longer reflects the current segmentation state. Invalidate it: get_audit
  // reports the last committed edit *in effect*, not one that was undone.
  m_HasLastAuditRecord = false;

  // Drop the reverted edit from the log as well, so a caller reading the whole
  // log is never told about an edit that is no longer in effect. Temporary
  // commits never entered the log (see StoreUndoPoint), so undoing one must not
  // pop the genuine record beneath it. Retained for Redo().
  if(commit.GetName() != TEMPORARY_UNDO_POINT_NAME)
    this->MoveAuditRecord(m_AuditLog, m_UndoneAuditRecords);
}

bool LabelImageWrapper::IsRedoPossible()
{
  UndoManagerType *um = m_TimePointUndoManagers[m_TimePointIndex];
  return um->IsRedoPossible();
}

void LabelImageWrapper::Redo()
{
  UndoManagerType *um = m_TimePointUndoManagers[m_TimePointIndex];

  // Get the commit for the redo
  const UndoManagerType::Commit &commit = um->GetCommitForRedo();

  // The label image that will undergo redo
  typedef itk::ImageRegionIterator<ImageType> IteratorType;

  // Iterate over all the deltas in reverse order
  UndoManagerType::DList::const_iterator dit = commit.GetDeltas().begin();
  for(; dit != commit.GetDeltas().end(); ++dit)
    {
    // Apply the changes in the current delta
    UndoManagerType::Delta *delta = *dit;

    // Iterator for the relevant region in the label image
    IteratorType lit(m_Image, delta->GetRegion());

    // Iterate over the rles in the delta
    for(size_t i = 0; i < delta->GetNumberOfRLEs(); i++)
      {
      size_t n = delta->GetRLELength(i);
      LabelType d = delta->GetRLEValue(i);
      for(size_t j = 0; j < n; j++)
        {
        if(d != 0)
          lit.Set(lit.Get() + d);
        ++lit;
        }
      }
    }

  // Set modified flags
  this->PixelsModified();

  // The edit is in effect again, so restore its record to the log and make it
  // the "last" record once more -- get_audit/get_audit_log stay in step with
  // undo/redo rather than only with undo.
  if(commit.GetName() != TEMPORARY_UNDO_POINT_NAME)
    {
    if(this->MoveAuditRecord(m_UndoneAuditRecords, m_AuditLog))
      {
      m_LastAuditRecord = m_AuditLog.back();
      m_HasLastAuditRecord = true;
      }
    }
}

const
LabelImageWrapper::UndoManagerType *
LabelImageWrapper
::GetUndoManager() const
{
  return m_TimePointUndoManagers[m_TimePointIndex];
}

LabelImageWrapper::UndoManagerDelta *
LabelImageWrapper::CompressImage() const
{
  UndoManagerDelta *new_cumulative = new UndoManagerDelta();

  itk::ImageRegionConstIterator<ImageType> it(m_Image, m_Image->GetLargestPossibleRegion());
  for (; !it.IsAtEnd(); ++it)
    new_cumulative->Encode(it.Get());

  new_cumulative->FinishEncoding();
  return new_cumulative;
}

LabelImageWrapper::GenerateImageForUndoRedoResult
LabelImageWrapper::GenerateImageForRedo(const UndoDataManagerCommitType &commit,
                                        ImageType                   *img_delta,
                                        LabelType                    activeLabel)
{
  GenerateImageForUndoRedoResult result = {0u, 0u, 0u};
  for (auto *delta : commit.GetDeltas())
  {
    // Iterator for the relevant region in the label image
    using IteratorType = itk::ImageRegionIterator<LabelImageWrapper::ImageType>;
    using CIteratorType = itk::ImageRegionConstIterator<LabelImageWrapper::ImageType>;
    CIteratorType src_it(this->GetImage(), delta->GetRegion());
    IteratorType  dst_it(img_delta, delta->GetRegion());

    // Iterate over the rles in the delta
    for (size_t i = 0; i < delta->GetNumberOfRLEs(); i++)
    {
      size_t    n = delta->GetRLELength(i);
      LabelType d = delta->GetRLEValue(i);
      for (size_t j = 0; j < n; j++)
      {
        if (d != 0)
        {
          LabelType old_value = src_it.Get();
          LabelType new_value = old_value + d;
          if (new_value == activeLabel)
          {
            dst_it.Set(1);
            result.n_foreground++;
          }
          else if (new_value == 0)
          {
            dst_it.Set(1);
            result.n_background++;
          }
          else
          {
            result.n_other++;
          }
        }
        ++src_it;
        ++dst_it;
      }
    }
  }

  return result;
}
