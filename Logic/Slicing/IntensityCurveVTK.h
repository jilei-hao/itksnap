/*=========================================================================

  Program:   ITK-SNAP
  Module:    $RCSfile: IntensityCurveVTK.h,v $
  Language:  C++
  Date:      $Date: 2009/09/21 21:54:21 $
  Version:   $Revision: 1.4 $
  Copyright (c) 2007 Paul A. Yushkevich
  
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

=========================================================================*/
#ifndef __IntensityCurveVTK_h_
#define __IntensityCurveVTK_h_

#include <IntensityCurveInterface.h>
#include <vtkKochanekSpline.h>
#include <Registry.h>

/**
 * \class IntensityCurveVTK
 * \brief The spline intensity mapping based on the VTK spline class.
 */
class ITK_EXPORT IntensityCurveVTK : public IntensityCurveInterface 
{
public:

  /** Standard class typedefs. */
  typedef IntensityCurveVTK Self;
  typedef IntensityCurveInterface Superclass;
  typedef itk::SmartPointer<Self> Pointer;
  typedef itk::SmartPointer<const Self> ConstPointer;

  /** Run-time type information (and related methods). */
  itkTypeMacro(IntensityCurveVTK,IntensityCurveInterface)

  /** New method */
  itkNewMacro(Self)

  // Defined in the parent class
  void Initialize(unsigned int nControlPoints = 3) override;
  void GetControlPoint(unsigned int iControlPoint, double &t, double &x) const override;
  void UpdateControlPoint(unsigned int iControlPoint, double t, double x) override;
  bool IsMonotonic() const override;
  void ScaleControlPointsToWindow(double tMin, double tMax) override;
  void RecomputeInteriorControlPoints(unsigned int nControlPoints) override;

  std::pair<double, double> GetRange() const override;

  unsigned int GetControlPointCount() const override {
    return m_ControlPoints.size();
  }

  // Evaluate the curve
  double Evaluate(const double &t) const override {
    if(t < m_ControlPoints.front().t)
      return -.000001;
    else if(t > m_ControlPoints.back().t)
      return 1.000001;
    else
      return m_Spline->Evaluate(t);
  }

  // Load the curve from a registry
  void LoadFromRegistry(Registry &registry) override;

  // Save the curve to a registry
  void SaveToRegistry(Registry &registry) const override;

  // Check if the curve is in default state (linear from 0 to 1)
  bool IsInDefaultState() override;

protected:
  IntensityCurveVTK();
  virtual ~IntensityCurveVTK();
  void PrintSelf(std::ostream &s, itk::Indent indent) const override;

private:
  IntensityCurveVTK(const Self& ); //purposely not implemented
  void operator=(const Self& ); //purposely not implemented

  // The spline object
  vtkKochanekSpline *m_Spline;

  // Control point structure
  struct ControlPoint {
    double t;
    double x;
  };

  // A storage for the control points
  std::vector<struct ControlPoint> m_ControlPoints;
  typedef std::vector<struct ControlPoint>::iterator IteratorType;
};

#endif // __IntensityCurveVTK_h_
