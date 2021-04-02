#ifndef VOXELCHANGEREPORTDIALOG_H
#define VOXELCHANGEREPORTDIALOG_H

#include <QDialog>

namespace Ui {
  class VoxelChangeReportDialog;
}

class VoxelChangeReportDialog : public QDialog
{
  Q_OBJECT

public:
  explicit VoxelChangeReportDialog(QWidget *parent = nullptr);
  ~VoxelChangeReportDialog();

private:
  Ui::VoxelChangeReportDialog *ui;
};

#endif // VOXELCHANGEREPORTDIALOG_H
