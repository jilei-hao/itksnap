#include "VoxelChangeReportDialog.h"
#include "ui_VoxelChangeReportDialog.h"

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
