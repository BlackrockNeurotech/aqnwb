#include <iostream>

#include "nwb/ecephys/FeatureExtraction.hpp"

#include "Utils.hpp"
#include "nwb/file/ElectrodesTable.hpp"

using namespace AQNWB::NWB;

REGISTER_SUBCLASS_IMPL(FeatureExtraction)

FeatureExtraction::FeatureExtraction(const std::string& path,
                                     std::shared_ptr<IO::BaseIO> io)
    : NWBDataInterface(path, io)
{
}

FeatureExtraction::~FeatureExtraction() = default;

Status FeatureExtraction::initialize(
    const IO::BaseArrayDataSetConfig& featuresConfig,
    const IO::BaseArrayDataSetConfig& timesConfig,
    const std::vector<std::string>& featureLabels,
    const std::vector<int>& electrodeIndices,
    SizeType numChannels,
    SizeType numFeatures)
{
  auto ioPtr = getIO();
  if (!ioPtr) {
    std::cerr << "FeatureExtraction::initialize: IO object is not valid."
              << std::endl;
    return Status::Failure;
  }

  const auto validConfig = [&](const IO::BaseArrayDataSetConfig& config,
                               const SizeArray& expectedShape,
                               const IO::BaseDataType& expectedType)
  {
    SizeArray shape;
    SizeArray chunking;
    IO::BaseDataType type;
    return config.getProperties(ioPtr.get(), shape, chunking, type)
        == Status::Success
        && shape == expectedShape && type == expectedType;
  };
  if (numChannels == 0 || numFeatures == 0
      || !validConfig(featuresConfig,
                      SizeArray {0, numChannels, numFeatures},
                      IO::BaseDataType::F32)
      || !validConfig(timesConfig, SizeArray {0}, IO::BaseDataType::F64))
  {
    std::cerr << "FeatureExtraction::initialize: invalid dataset "
                 "configuration."
              << std::endl;
    return Status::Failure;
  }
  if (featureLabels.size() != numFeatures) {
    std::cerr << "FeatureExtraction::initialize: featureLabels.size()="
              << featureLabels.size()
              << " must match numFeatures=" << numFeatures << "." << std::endl;
    return Status::Failure;
  }
  if (electrodeIndices.size() != numChannels) {
    std::cerr << "FeatureExtraction::initialize: electrodeIndices.size()="
              << electrodeIndices.size()
              << " must match numChannels=" << numChannels << "." << std::endl;
    return Status::Failure;
  }

  if (NWBDataInterface::initialize() != Status::Success) {
    return Status::Failure;
  }

  try {
    ioPtr->createArrayDataSet(featuresConfig,
                              AQNWB::mergePaths(getPath(), "features"));
    ioPtr->createArrayDataSet(timesConfig,
                              AQNWB::mergePaths(getPath(), "times"));
    // The schema defines `description` as a dataset holding one label per
    // feature, not as a group-level description attribute.
    if (ioPtr->createStringDataSet(AQNWB::mergePaths(getPath(), "description"),
                                   featureLabels)
        != Status::Success)
    {
      return Status::Failure;
    }
    IO::ArrayDataSetConfig electrodesConfig(IO::BaseDataType::I32,
                                            SizeArray {numChannels},
                                            SizeArray {numChannels});
    const std::string electrodesPath =
        AQNWB::mergePaths(getPath(), "electrodes");
    ioPtr->createArrayDataSet(electrodesConfig, electrodesPath);

    auto electrodesRecorder = recordElectrodes();
    if (!electrodesRecorder
        || electrodesRecorder->writeDataBlock(SizeArray {numChannels},
                                              IO::BaseDataType::I32,
                                              electrodeIndices.data())
            != Status::Success
        || ioPtr->createCommonNWBAttributes(
               electrodesPath, "hdmf-common", "DynamicTableRegion")
            != Status::Success
        || ioPtr->createAttribute(
               "the electrodes that generated this feature extraction",
               electrodesPath,
               "description")
            != Status::Success
        || ioPtr->createReferenceAttribute(
               ElectrodesTable::electrodesTablePath, electrodesPath, "table")
            != Status::Success)
    {
      return Status::Failure;
    }
  } catch (const std::runtime_error& error) {
    std::cerr << "FeatureExtraction::initialize: failed to create datasets: "
              << error.what() << std::endl;
    return Status::Failure;
  }

  m_numChannels = numChannels;
  m_numFeatures = numFeatures;
  m_eventsRecorded = 0;
  return Status::Success;
}

Status FeatureExtraction::writeEvents(const SizeType& numEvents,
                                      const void* featuresInput,
                                      const void* timesInput)
{
  if (numEvents == 0) {
    return Status::Success;
  }
  if (!featuresInput || !timesInput || m_numChannels == 0 || m_numFeatures == 0)
  {
    return Status::Failure;
  }

  auto featuresRecorder = recordFeatures();
  auto timesRecorder = recordTimes();
  if (!featuresRecorder || !timesRecorder) {
    return Status::Failure;
  }

  if (featuresRecorder->writeDataBlock(
          SizeArray {numEvents, m_numChannels, m_numFeatures},
          SizeArray {m_eventsRecorded, 0, 0},
          IO::BaseDataType::F32,
          featuresInput)
          != Status::Success
      || timesRecorder->writeDataBlock(SizeArray {numEvents},
                                       SizeArray {m_eventsRecorded},
                                       IO::BaseDataType::F64,
                                       timesInput)
          != Status::Success)
  {
    return Status::Failure;
  }

  m_eventsRecorded += numEvents;
  return Status::Success;
}
