#ifndef TIMEPOINTPROPERTIES_H
#define TIMEPOINTPROPERTIES_H

#include "SNAPCommon.h"
#include "SNAPEvents.h"
#include "itkDataObject.h"
#include "itkObjectFactory.h"
#include "TagList.h"
#include <cmath>
#include <limits>


class Registry;
class GenericImageData;

class TimePointProperty : public itk::DataObject
{
public:
	irisITKObjectMacro(TimePointProperty, itk::DataObject)

	std::string GetNickname() const
	{ return Nickname; }

	void SetNickname(std::string _nickname)
	{
		Nickname = _nickname;
		InvokeEvent(WrapperGlobalMetadataChangeEvent());
	}

	TagList GetTags() const
	{ return Tags; }

	TagList &GetModifiableTags()
	{ return Tags; }

	void AddTag(std::string &tag)
	{
		Tags.AddTag(tag);
	}

	void SetTagList(TagList tlist)
	{
		Tags = tlist;
	}

	/** Cardiac phase of this time point, as a percentage of the R-R interval.
	 *  NaN when unknown (e.g. the image is not a gated cardiac series). */
	double GetRRPercent() const
	{ return RRPercent; }

	bool HasRRPercent() const
	{ return !std::isnan(RRPercent); }

	void SetRRPercent(double rr)
	{
		RRPercent = rr;
		InvokeEvent(WrapperGlobalMetadataChangeEvent());
	}

	/** Whether the %R-R value is an exact (clean integer-step) recon phase, as
	 *  opposed to an approximate value derived from an ambiguous label. */
	bool GetRRPercentExact() const
	{ return RRPercentExact; }

	void SetRRPercentExact(bool exact)
	{ RRPercentExact = exact; }

protected:
	TimePointProperty() {};
	virtual ~TimePointProperty() {}

private:
  std::string Nickname;
  TagList Tags;
  double RRPercent = std::numeric_limits<double>::quiet_NaN();
  bool RRPercentExact = false;
};

class TimePointProperties : public itk::DataObject
{
public:
  irisITKObjectMacro(TimePointProperties, itk::DataObject)

  /** Get a reference to IRISApplication */
  void SetParent(GenericImageData *parent);

  /** Load data from a registry */
  void Load(Registry &folder);

  /** Save data to a registry */
  void Save(Registry &folder) const;

  /** Unload the object, when unloading the main image */
  void Reset();

  /** Create an empty new map */
  void CreateNewData();

  /** Get property by timepoint */
  TimePointProperty* GetProperty(unsigned int tp);

protected:
  TimePointProperties();
  virtual ~TimePointProperties();

private:
	std::map<unsigned int, SmartPtr<TimePointProperty>> m_TPPropertiesMap;
  GenericImageData *m_Parent;
};

#endif // TIMEPOINTPROPERTIES_H
