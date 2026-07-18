#ifndef LABELIMAGEWRAPPER_H
#define LABELIMAGEWRAPPER_H

#include "ImageWrapperTraits.h"
#include "ScalarImageWrapper.h"
#include "SegmentationAuditRecord.h"

#include <vector>

template <typename TPixel> class UndoDataManager;
template <typename TPixel> class UndoDataManagerCommit;
template <typename TPixel> class UndoDelta;
class SegmentationUpdateIterator;

class LabelImageWrapper : public ScalarImageWrapper<LabelImageWrapperTraits>
{
public:

  // Standard ITK business
  typedef LabelImageWrapper                                               Self;
  typedef ScalarImageWrapper<LabelImageWrapperTraits>               Superclass;
  typedef SmartPtr<Self>                                               Pointer;
  typedef SmartPtr<const Self>                                    ConstPointer;
  itkTypeMacro(LabelImageWrapper, ImageWrapper)
  itkNewMacro(Self)

  // Image Types
  typedef Superclass::ImageBaseType                              ImageBaseType;
  typedef Superclass::ImageType                                      ImageType;
  typedef Superclass::ImagePointer                                ImagePointer;
  typedef Superclass::PixelType                                      PixelType;
  typedef Superclass::ITKTransformType                        ITKTransformType;

  // Undo manager typedefs
  typedef UndoDataManager<PixelType>       UndoManagerType;
  typedef UndoDelta<PixelType>             UndoManagerDelta;
  typedef UndoDataManagerCommit<PixelType> UndoDataManagerCommitType;

  // We are friends with the SegmentationUpdateIterator
  friend class SegmentationUpdateIterator;

  /**
   * We override the SetImage method to reset the undo manager when an image is
   * assigned to the segmentation.
   */
  virtual void UpdateWrappedImages(Image4DType *image_4d,
                                   ImageBaseType *refSpace = NULL,
                                   ITKTransformType *tran = NULL) override;

  /**
   * Store an intermediate delta without committing it as an undo point
   * Multiple deltas can be stored and then committed with StoreUndoPoint()
   */
  void StoreIntermediateUndoDelta(UndoManagerDelta *delta);

  /**
   * Store an undo point. The first parameter is the description of the
   * update, and the second parameter is the delta to be applied. The delta
   * can be NULL. All deltas previously submitted with StoreIntermediateUndoDelta
   * and the delta passed in to this method will be commited to this undo point.
   */
  void StoreUndoPoint(const char *text, UndoManagerDelta *delta = NULL);

  /** Clear all undo points */
  void ClearUndoPoints();

  /** Clear all undo points */
  void ClearUndoPointsForAllTimePoints();

  /** Check whether undo is possible */
  bool IsUndoPossible();

  /** Check whether undo is possible */
  bool IsRedoPossible();

  /** Undo (revert to last stored undo point) */
  void Undo();

  /** Redo (undo the undo) */
  void Redo();

  /** Get the undo manager */
  const UndoManagerType *GetUndoManager() const;

  /**
   * Provenance / audit trail. Every committed edit (see StoreUndoPoint) is
   * captured as a structured SegmentationAuditRecord: operation name, actor,
   * timestamp, changed-voxel count, bounding box, and before/after label
   * histograms. This turns an expert correction into a machine-consumable
   * return value rather than a side effect.
   */

  /** Declare who is responsible for the *next* commit. Auto-resets to HUMAN
   *  after each commit, so agent-driven code must set AGENT immediately before
   *  the operation that produces the commit. */
  void SetNextCommitActor(SegmentationAuditRecord::Actor actor)
    { m_NextCommitActor = actor; }

  /** Whether an audit record has been captured since the wrapper was created. */
  bool HasLastAuditRecord() const
    { return m_HasLastAuditRecord; }

  /** The audit record for the most recent committed edit. Only valid when
   *  HasLastAuditRecord() is true. */
  const SegmentationAuditRecord &GetLastAuditRecord() const
    { return m_LastAuditRecord; }

  /** The full ordered log of audit records for this wrapper's lifetime. */
  const std::vector<SegmentationAuditRecord> &GetAuditLog() const
    { return m_AuditLog; }

  /** This is not used by the undo system itself, but uses the undo code to
   * store the contents of the image as an undo delta object, which can then
   * be stored in memory compactly. The caller is responsible for deleting the
   * array created in this call. */
  UndoManagerDelta *CompressImage() const;

  /**
   * Return type for GenerateImageForRedo
   */
  struct GenerateImageForUndoRedoResult
  {
    unsigned int n_other, n_background, n_foreground;
  };

  /**
   * Given an empty segmentation image, apply deltas from an redo commit to this
   * image, basically generating a segmentation corresponding just to the commit
   */
  GenerateImageForUndoRedoResult GenerateImageForRedo(const UndoDataManagerCommitType &commit,
                                                      ImageType                   *image,
                                                      LabelType                    activeLabel);

protected:

  LabelImageWrapper();
  ~LabelImageWrapper();

  // Undo data manager, stores 'deltas', i.e., differences between states of the segmentation
  // image. These deltas are compressed, allowing us to store a bunch of
  // undo steps with little cost in performance or memory. We currently associate each time
  // point with its own undo manager
  std::vector<UndoManagerType *> m_TimePointUndoManagers;

  // Actor responsible for the next commit; reset to HUMAN after each commit.
  SegmentationAuditRecord::Actor m_NextCommitActor = SegmentationAuditRecord::HUMAN;

  // Provenance for the most recent committed edit, and the running log.
  SegmentationAuditRecord              m_LastAuditRecord;
  bool                                 m_HasLastAuditRecord = false;
  std::vector<SegmentationAuditRecord> m_AuditLog;
};

#endif // LABELIMAGEWRAPPER_H
