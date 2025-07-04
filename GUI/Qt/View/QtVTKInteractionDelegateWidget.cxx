#include "QtVTKInteractionDelegateWidget.h"
#include "vtkRenderWindowInteractor.h"
#include "QtVTKRenderWindowBox.h"
#include <QApplication>

QtVTKInteractionDelegateWidget::QtVTKInteractionDelegateWidget(QWidget *parent, QWidget *canvasWidget) :
    QtInteractionDelegateWidget(parent, canvasWidget)
{
}

void QtVTKInteractionDelegateWidget::SetVTKEventState(QMouseEvent *ev)
{
  Qt::KeyboardModifiers km = QApplication::keyboardModifiers();

  // Account for Retina displays
  int x = ev->pos().x() * this->devicePixelRatio();
  int y = ev->pos().y() * this->devicePixelRatio();

  m_VTKInteractor->SetEventInformationFlipY(
        x, y, 
        km.testFlag(Qt::ControlModifier),
        km.testFlag(Qt::ShiftModifier));
}

void QtVTKInteractionDelegateWidget::mousePressEvent(QMouseEvent *ev)
{
  // Set the position information
  SetVTKEventState(ev);

  // Fire appropriate event
  if(ev->button() == Qt::LeftButton)
    m_VTKInteractor->LeftButtonPressEvent();
  else if(ev->button() == Qt::RightButton)
    m_VTKInteractor->RightButtonPressEvent();
  else if(ev->button() == Qt::MiddleButton)
    m_VTKInteractor->MiddleButtonPressEvent();

  // TODO: why do we need to force this?
  this->m_CanvasWidget->update();
}

void QtVTKInteractionDelegateWidget::mouseReleaseEvent(QMouseEvent *ev)
{
  // Set the position information
  SetVTKEventState(ev);

  // Fire appropriate event
  if(ev->button() == Qt::LeftButton)
    m_VTKInteractor->LeftButtonReleaseEvent();
  else if(ev->button() == Qt::RightButton)
    m_VTKInteractor->RightButtonReleaseEvent();
  else if(ev->button() == Qt::MiddleButton)
    m_VTKInteractor->MiddleButtonReleaseEvent();

  // TODO: why do we need to force this?
  this->m_CanvasWidget->update();
}

void QtVTKInteractionDelegateWidget::mouseMoveEvent(QMouseEvent *ev)
{
  // Set the position information
  SetVTKEventState(ev);
  m_VTKInteractor->MouseMoveEvent();

  // TODO: why do we need to force this?
  this->m_CanvasWidget->update();
}

void QtVTKInteractionDelegateWidget::SetVTKInteractor(vtkRenderWindowInteractor *iren)
{
  m_VTKInteractor = iren;
}

vtkRenderWindowInteractor * QtVTKInteractionDelegateWidget::GetVTKInteractor() const
{
  return m_VTKInteractor;
}


