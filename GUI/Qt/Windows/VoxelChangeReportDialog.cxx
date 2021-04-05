#include "VoxelChangeReportDialog.h"
#include "ui_VoxelChangeReportDialog.h"
#include "VoxelChangeReportModel.h"
#include <QTreeWidget>
#include <QTreeWidgetItem>


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
  // Get report from the model
  typedef VoxelChangeReportModel::VoxelChangeReportType ReportType;
  ReportType &report = m_Model->getReport();

  // Build the table
  QTreeWidget *tree = ui->tree;

  // -- configure columns
  tree->setColumnCount(5);
  QStringList header;
  header << "Frame" << "Label" << "Before" << "After" << "Change";
  tree->setHeaderLabels(header);
  tree->setAlternatingRowColors(true);

  // -- add items to the tree
  for (auto fit = report.cbegin(); fit != report.cend(); ++fit)
    {
      if (fit->second.size() > 1)
        {
          QTreeWidgetItem *frame = new QTreeWidgetItem(tree);
          frame->setText(0, QString::number(fit->first + 1)); // frame
          for (auto lit = fit->second.cbegin(); lit != fit->second.cend(); ++lit)
            {
              if (lit->second->change != 0)
                {
                  QTreeWidgetItem *labelLine = new QTreeWidgetItem(frame);
                  labelLine->setText(0, QString::number(fit->first + 1)); // frame
                  labelLine->setText(1, QString::number(lit->first)); // label
                  labelLine->setText(2, QString::number(lit->second->before)); // before
                  labelLine->setText(3, QString::number(lit->second->after)); // after
                  labelLine->setText(4, QString::number(lit->second->change)); // change
                }
            }
        }

    }


  // Render the GUI
  this->exec();
}



void VoxelChangeReportDialog::SetModel(VoxelChangeReportModel *model)
{
  this->m_Model = model;
}
