#pragma once

#include <string>
#include <vector>

#include "Utils.hpp"
#include "io/BaseIO.hpp"
#include "io/ReadIO.hpp"
#include "nwb/base/NWBDataInterface.hpp"
#include "nwb/file/ElectrodesTable.hpp"
#include "spec/core.hpp"

namespace AQNWB::NWB
{
/**
 * @brief Features extracted from electrical recordings.
 *
 * Stores a three-dimensional `[num_events, num_channels, num_features]`
 * `features` dataset, a `description` text dataset naming each feature
 * dimension, a one-dimensional `times` dataset, and an `electrodes`
 * DynamicTableRegion linking the channel axis to the ElectrodesTable.
 */
class FeatureExtraction : public NWBDataInterface
{
public:
  REGISTER_SUBCLASS(FeatureExtraction,
                    NWBDataInterface,
                    AQNWB::SPEC::CORE::namespaceName)

protected:
  /**
   * @brief Constructor.
   * @param path Path to the FeatureExtraction group in the file.
   * @param io A shared pointer to the IO object.
   */
  FeatureExtraction(const std::string& path, std::shared_ptr<IO::BaseIO> io);

public:
  /**
   * @brief Destructor.
   */
  ~FeatureExtraction() override;

  /**
   * @brief Initialize the datasets used by this FeatureExtraction.
   *
   * @param featuresConfig Configuration for the float32 `features` dataset.
   *        Its shape must be `[0, numChannels, numFeatures]`.
   * @param timesConfig Configuration for the float64 `times` dataset. Its
   *        shape must be `[0]`.
   * @param featureLabels Description of each feature dimension.
   * @param electrodeIndices Row indices into ElectrodesTable, one per channel.
   * @param numChannels Size of the channel axis.
   * @param numFeatures Size of the feature axis.
   * @return Status::Success on success, otherwise Status::Failure.
   */
  Status initialize(const IO::BaseArrayDataSetConfig& featuresConfig,
                    const IO::BaseArrayDataSetConfig& timesConfig,
                    const std::vector<std::string>& featureLabels,
                    const std::vector<int>& electrodeIndices,
                    SizeType numChannels,
                    SizeType numFeatures);

  /**
   * @brief Append events to the `features` and `times` datasets.
   *
   * @param numEvents Number of new events.
   * @param featuresInput Contiguous float32 data with shape
   *        `[numEvents, numChannels, numFeatures]`.
   * @param timesInput Float64 timestamps with length `numEvents`.
   * @return Status::Success on success, otherwise Status::Failure.
   */
  Status writeEvents(const SizeType& numEvents,
                     const void* featuresInput,
                     const void* timesInput);

  /**
   * @brief Number of channels in the features dataset.
   */
  SizeType numChannels() const { return m_numChannels; }

  /**
   * @brief Number of feature dimensions in the features dataset.
   */
  SizeType numFeatures() const { return m_numFeatures; }

  DEFINE_DATASET_FIELD(readFeatures,
                       recordFeatures,
                       float,
                       "features",
                       Multi - dimensional array of extracted features)

  DEFINE_DATASET_FIELD(readTimes,
                       recordTimes,
                       double,
                       "times",
                       Times of events that features correspond to)

  DEFINE_DATASET_FIELD(readDescriptionDataset,
                       recordDescriptionDataset,
                       std::string,
                       "description",
                       Description of each extracted feature)

  DEFINE_DATASET_FIELD(readElectrodes,
                       recordElectrodes,
                       int,
                       "electrodes",
                       Indices of the electrodes that
                           generated this feature extraction)

  DEFINE_REFERENCED_REGISTERED_FIELD(
      readElectrodesTable,
      ElectrodesTable,
      "electrodes/table",
      The electrodes table referenced by the DynamicTableRegion.)

private:
  SizeType m_numChannels = 0;
  SizeType m_numFeatures = 0;
  SizeType m_eventsRecorded = 0;
};
}  // namespace AQNWB::NWB
