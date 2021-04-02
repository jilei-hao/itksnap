#ifndef VOXELCHANGEREPORTMODEL_H
#define VOXELCHANGEREPORTMODEL_H

#include <AbstractModel.h>

class GlobalUIModel;

class VoxelChangeReportModel : public AbstractModel
{
public:
  irisITKObjectMacro(VoxelChangeReportModel, AbstractModel);

protected:
  // Constructor
  VoxelChangeReportModel();
  virtual ~VoxelChangeReportModel() {}

  // The parent model
  GlobalUIModel *m_Parent;
};

#endif // VOXELCHANGEREPORTMODEL_H
