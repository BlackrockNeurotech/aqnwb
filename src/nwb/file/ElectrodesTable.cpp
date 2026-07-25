#include "nwb/file/ElectrodesTable.hpp"

#include "Channel.hpp"
#include "Utils.hpp"

using namespace AQNWB::NWB;

// ElectrodesTable
// Initialize the static registered_ member to trigger registration
REGISTER_SUBCLASS_IMPL(ElectrodesTable)

/** Constructor */
ElectrodesTable::ElectrodesTable(std::shared_ptr<IO::BaseIO> io)
    : DynamicTable(electrodesTablePath,  // use the electrodesTablePath
                   io)
{
}

ElectrodesTable::ElectrodesTable(const std::string& path,
                                 std::shared_ptr<IO::BaseIO> io)
    : DynamicTable(electrodesTablePath, io)
{
  if (path != this->electrodesTablePath) {
    std::cerr << "WARNING: ElectrodesTable object is required to appear at "
              << this->electrodesTablePath << ". Ignoring provided path."
              << std::endl;
  }
}

/** Destructor */
ElectrodesTable::~ElectrodesTable() {}

SizeArray ElectrodesTable::columnWriteOffset(
    const std::shared_ptr<IO::BaseRecordingData>& dataset) const
{
  if (!dataset) {
    return SizeArray {0};
  }
  const SizeArray& currentShape = dataset->getShape();
  return SizeArray {currentShape.empty() ? 0 : currentShape[0]};
}

std::vector<DynamicTable::DataSpecPtr> ElectrodesTable::createDefaultDataSpecs(
    const SizeType rowChunkSize)
{
  std::vector<DataSpecPtr> specs =
      DynamicTable::createDefaultDataSpecs(rowChunkSize);

  IO::ArrayDataSetConfig locationConfig(
      IO::BaseDataType::V_STR, SizeArray {0}, SizeArray {rowChunkSize});
  specs.push_back(std::make_shared<VectorData::DataSpec>(
      "location",
      locationConfig,
      "the location of channel within the subject e.g. brain region"));

  IO::ArrayDataSetConfig groupNameConfig(
      IO::BaseDataType::V_STR, SizeArray {0}, SizeArray {rowChunkSize});
  specs.push_back(std::make_shared<VectorData::DataSpec>(
      "group_name",
      groupNameConfig,
      "the name of the ElectrodeGroup this electrode is a part of"));

  // "group" is added in finalize() because DataSpec does not support
  // reference columns.

  return specs;
}

Status ElectrodesTable::validateDataSpecs(
    const std::vector<DataSpecPtr>& dataSpecs) const
{
  return checkRequiredColumnNames({"id", "location", "group_name"}, dataSpecs);
}

Status ElectrodesTable::initialize(const std::string& description,
                                   const std::vector<DataSpecPtr>& columnSpecs)
{
  // Configure and register each DataSpec-backed column exactly once.
  const auto specs =
      columnSpecs.empty() ? createDefaultDataSpecs() : columnSpecs;
  return DynamicTable::initialize(description, specs);
}

void ElectrodesTable::addElectrodes(const std::vector<Channel>& channelsInput)
{
  // Buffer electrode metadata until finalize().
  for (const auto& ch : channelsInput) {
    m_groupReferences.push_back(
        AQNWB::mergePaths(m_groupPathBase, ch.getGroupName()));
    m_groupNames.push_back(ch.getGroupName());
    m_electrodeNumbers.push_back(static_cast<int>(ch.getGlobalIndex()));
    m_locationNames.push_back("unknown");
  }
}

Status ElectrodesTable::finalize()
{
  Status status = Status::Success;
  // Append identifiers for newly added electrodes.
  if (!m_electrodeNumbers.empty()) {
    status = status && setRowIDs(m_electrodeNumbers);
    m_electrodeNumbers.clear();
  }

  // Append string metadata at each column's current extent.
  const auto appendStrings =
      [this, &status](const std::string& name, std::vector<std::string>& values)
  {
    if (!values.empty()) {
      auto column = getConfiguredColumn(name);
      if (!column || !column->recordData()) {
        std::cerr << "ElectrodesTable::finalize failed to get " << name
                  << " column." << std::endl;
        status = Status::Failure;
        return;
      }
      status = status && addColumn(column, values);
      values.clear();
    }
  };
  appendStrings("location", m_locationNames);
  appendStrings("group_name", m_groupNames);

  // Create or extend references to the owning ElectrodeGroup objects.
  if (!m_groupReferences.empty()) {
    status =
        status
        && addReferenceColumn(
            "group",
            "a reference to the ElectrodeGroup this electrode is a part of",
            m_groupReferences);
    m_groupReferences.clear();
  }

  // Flush column names after all dynamic columns have been registered.
  return status && DynamicTable::finalize();
}
