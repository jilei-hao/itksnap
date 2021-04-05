#include "VoxelChangeReportDialog.h"
#include "ui_VoxelChangeReportDialog.h"
#include "VoxelChangeReportModel.h"

VoxelChangeReportDialog::VoxelChangeReportDialog(QWidget *parent) :
  QDialog(parent),
  ui(new Ui::VoxelChangeReportDialog)
{
  ui->setupUi(this);
}

VoxelChangeReportDialog::~VoxelChangeReportDialog()
{
  delete ui;
}

void VoxelChangeReportDialog::setStartPoint()
{
  m_Model->setStartingPoint();
}

void VoxelChangeReportDialog::showReport()
{
  typedef VoxelChangeReportModel::VoxelChangeReportType ReportType;
  ReportType report = m_Model->getReport();
  this->exec();
}

void VoxelChangeReportDialog::SetModel(VoxelChangeReportModel *model)
{
  this->m_Model = model;
}
