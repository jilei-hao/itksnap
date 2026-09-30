#include "TimePointProperties.h"
#include "Registry.h"
#include "GenericImageData.h"
#include "IRISApplication.h"
#include "ImageWrapperBase.h"
#include "MetaDataAccess.h"
#include "Rebroadcaster.h"
#include <sstream>
#include <vector>
#include <string>

namespace {
// Mirrors the dictionary keys written by GuidedNativeImageIO. The generic
// frame-axis keys are modality-agnostic (CT: %R-R; echo: frame time); the
// cardiac keys carry CT-specific provenance (the "approx" flag).
const char *ITKSNAP_FRAME_AXIS_VALUES  = "ITKSNAP_FrameAxis_Values";
const char *ITKSNAP_FRAME_AXIS_UNIT    = "ITKSNAP_FrameAxis_Unit";
const char *ITKSNAP_CARDIAC_RR_PERCENT = "ITKSNAP_Cardiac_RRPercent";
const char *ITKSNAP_CARDIAC_RR_EXACT   = "ITKSNAP_Cardiac_RRPercentExact";

std::vector<double> ParseDoubleList(const std::string &s)
{
  std::vector<double> out;
  std::istringstream ss(s);
  double v;
  while (ss >> v) out.push_back(v);
  return out;
}
} // anonymous namespace

TimePointProperties::TimePointProperties()
{
}

TimePointProperties::~TimePointProperties()
{
}

void
TimePointProperties::SetParent(GenericImageData * parent)
{
  m_Parent = parent;
	Rebroadcaster::Rebroadcast(this, WrapperGlobalMetadataChangeEvent(),
														 parent, WrapperGlobalMetadataChangeEvent());
}

void
TimePointProperties::Reset()
{
  this->m_TPPropertiesMap.clear();
}


void
TimePointProperties::CreateNewData()
{
  // Main Image has to be loaded
  assert(m_Parent->GetParent()->IsMainImageLoaded());

  // Clear the map
  this->m_TPPropertiesMap.clear();

  unsigned int nt = m_Parent->GetParent()->GetNumberOfTimePoints();

  // Pull the per-frame axis (if any) from the main image metadata so each time
  // point is tagged with its value. The generic frame axis is filled by both
  // 4D CTA (%R-R) and 4D echo (frame time, ms); the cardiac %R-R keys add the
  // CT-specific "approx" flag. Absent for non-cardiac/non-cine series.
  std::vector<double> frame, rr;
  std::string frameUnit;
  bool rrExact = false;
  if (ImageWrapperBase *mainw = m_Parent->GetMain())
    {
    auto mda = mainw->GetMetaDataAccess();
    if (mda.HasKey(ITKSNAP_FRAME_AXIS_VALUES))
      frame = ParseDoubleList(mda.GetValueAsString(ITKSNAP_FRAME_AXIS_VALUES));
    if (mda.HasKey(ITKSNAP_FRAME_AXIS_UNIT))
      frameUnit = mda.GetValueAsString(ITKSNAP_FRAME_AXIS_UNIT);
    if (mda.HasKey(ITKSNAP_CARDIAC_RR_PERCENT))
      rr = ParseDoubleList(mda.GetValueAsString(ITKSNAP_CARDIAC_RR_PERCENT));
    if (mda.HasKey(ITKSNAP_CARDIAC_RR_EXACT))
      rrExact = (mda.GetValueAsString(ITKSNAP_CARDIAC_RR_EXACT) == "1");
    }
  const bool haveFrame = (frame.size() == nt);
  const bool haveRR = (rr.size() == nt);

  for (unsigned int i = 1; i <= nt; ++i)
    {
		auto tpp = TimePointProperty::New();
		if (haveFrame)
			tpp->SetFrameValue(frame[i - 1], frameUnit);
		if (haveRR)
			{
			tpp->SetRRPercent(rr[i - 1]);
			tpp->SetRRPercentExact(rrExact);
			}
		m_TPPropertiesMap[i] = tpp;
		Rebroadcaster::Rebroadcast(tpp, WrapperGlobalMetadataChangeEvent(),
															 this, WrapperGlobalMetadataChangeEvent());
    }
}

TimePointProperty*
TimePointProperties::GetProperty(unsigned int tp)
{
  // Map size should always consistent with current main image timepoint #
  assert(tp <= m_TPPropertiesMap.size());

	return m_TPPropertiesMap[tp];
}



void
TimePointProperties::Load(Registry &folder)
{
  // Validate version
  string version = folder["FormatVersion"][""];

  // Validate number of timepoints
  unsigned int nt = folder["TimePoints.ArraySize"][0u];

  assert(nt == m_Parent->GetParent()->GetNumberOfTimePoints());

  // Load data (1-based index timepoint array)
  for (unsigned int i = 1u; i <= nt; ++i)
    {
      // Check folder existence
      // To make this robust, create an empty property for a missing entry
      std::string key = Registry::Key("TimePoints.TimePoint[%d]", i);
			auto tpp = TimePointProperty::New();

      if (folder.HasFolder(key))
        {
          Registry &tpFolder = folder.Folder(key);

					tpp->SetNickname(tpFolder["Nickname"][""]);
					tpFolder["Tags"].GetList(tpp->GetModifiableTags());

					// Cardiac phase (FormatVersion >= 2; absent entries stay NaN)
					double rr = tpFolder["RRPercent"][std::numeric_limits<double>::quiet_NaN()];
					if (!std::isnan(rr))
						{
						tpp->SetRRPercent(rr);
						tpp->SetRRPercentExact(tpFolder["RRPercentExact"][false]);
						}

					// Generic frame axis value + unit (FormatVersion >= 3)
					double fv = tpFolder["FrameValue"][std::numeric_limits<double>::quiet_NaN()];
					if (!std::isnan(fv))
						tpp->SetFrameValue(fv, tpFolder["FrameUnit"][""]);
        }

      this->m_TPPropertiesMap[i] = tpp;
    }

}

void
TimePointProperties::Save(Registry &folder) const
{
  // Record format version
  //  Future change of format use this to be backward compatible
  //  v2 adds the cardiac %R-R fields; v3 adds the generic frame axis value+unit.
  folder["FormatVersion"] << "3";

  // Save array size for loading
  folder["TimePoints.ArraySize"] << m_TPPropertiesMap.size();

  for (auto cit = m_TPPropertiesMap.cbegin();
       cit != m_TPPropertiesMap.cend(); ++cit)
    {
      // Create a folder for each timepoint
      Registry &tp_folder = folder.Folder(folder.Key("TimePoints.TimePoint[%d]", cit->first));

      // Write timepoint properties to the folder
      tp_folder["TimePoint"] << cit->first;
			tp_folder["Nickname"] << cit->second->GetNickname();
			tp_folder["Tags"].PutList(cit->second->GetTags());

			// Cardiac phase (only when known)
			if (cit->second->HasRRPercent())
				{
				tp_folder["RRPercent"] << cit->second->GetRRPercent();
				tp_folder["RRPercentExact"] << cit->second->GetRRPercentExact();
				}

			// Generic frame axis value + unit (only when known)
			if (cit->second->HasFrameValue())
				{
				tp_folder["FrameValue"] << cit->second->GetFrameValue();
				tp_folder["FrameUnit"] << cit->second->GetFrameUnit();
				}
    }
}




