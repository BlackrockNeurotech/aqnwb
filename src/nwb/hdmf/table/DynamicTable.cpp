#include <algorithm>
#include <numeric>
#include <type_traits>
#include <unordered_set>

#include "nwb/hdmf/table/DynamicTable.hpp"

#include "Utils.hpp"
#include "nwb/hdmf/table/MeaningsTable.hpp"

using namespace AQNWB::NWB;

namespace
{
using BufferVariant = AQNWB::IO::BaseDataType::BaseDataVectorVariant;
using CellValue = AQNWB::IO::BaseDataType::BaseDataVariant;

template<typename T>
bool appendTypedCell(BufferVariant& buffer, const CellValue& cellValue)
{
  auto* typedValue = std::get_if<T>(&cellValue);
  if (!typedValue) {
    return false;
  }
  auto* typedBuffer = std::get_if<std::vector<T>>(&buffer);
  if (!typedBuffer) {
    return false;
  }
  typedBuffer->push_back(*typedValue);
  return true;
}

bool appendCellToBuffer(BufferVariant& buffer,
                        const AQNWB::IO::BaseDataType& dataType,
                        const CellValue& cellValue)
{
  switch (dataType.type) {
    case AQNWB::IO::BaseDataType::T_U8:
      return appendTypedCell<uint8_t>(buffer, cellValue);
    case AQNWB::IO::BaseDataType::T_U16:
      return appendTypedCell<uint16_t>(buffer, cellValue);
    case AQNWB::IO::BaseDataType::T_U32:
      return appendTypedCell<uint32_t>(buffer, cellValue);
    case AQNWB::IO::BaseDataType::T_U64:
      return appendTypedCell<uint64_t>(buffer, cellValue);
    case AQNWB::IO::BaseDataType::T_I8:
      return appendTypedCell<int8_t>(buffer, cellValue);
    case AQNWB::IO::BaseDataType::T_I16:
      return appendTypedCell<int16_t>(buffer, cellValue);
    case AQNWB::IO::BaseDataType::T_I32:
      return appendTypedCell<int32_t>(buffer, cellValue);
    case AQNWB::IO::BaseDataType::T_I64:
      return appendTypedCell<int64_t>(buffer, cellValue);
    case AQNWB::IO::BaseDataType::T_F32:
      return appendTypedCell<float>(buffer, cellValue);
    case AQNWB::IO::BaseDataType::T_F64:
      return appendTypedCell<double>(buffer, cellValue);
    case AQNWB::IO::BaseDataType::T_STR:
    case AQNWB::IO::BaseDataType::V_STR:
      return appendTypedCell<std::string>(buffer, cellValue);
  }
  return false;
}
}  // namespace

// DynamicTable
// Initialize the static registered_ member to trigger registration
REGISTER_SUBCLASS_IMPL(DynamicTable)

/** Constructor */
DynamicTable::DynamicTable(const std::string& path,
                           std::shared_ptr<IO::BaseIO> io)
    : Container(path, io)
    , m_colNames({})
    , m_rowElementIdentifiers(nullptr)
{
  // Read the colNames attribute if it exists such that any columns
  // we may add append to the existing list of columns rather than
  // replacing it. This is important for the finalize function
  // to ensure that all columns are correctly listed.
  auto ioPtr = getIO();
  if (ioPtr) {
    if (ioPtr->isOpen()) {
      auto colNamesFromFile = readColNames();
      if (colNamesFromFile->exists()) {
        m_colNames = colNamesFromFile->values().data;
      }
    }
  }
}

/** Destructor */
DynamicTable::~DynamicTable() {}

/** Initialization function*/
Status DynamicTable::initialize(const std::string& description,
                                const std::vector<DataSpecPtr>& dataSpecs)
{
  auto ioPtr = getIO();
  if (!ioPtr) {
    std::cerr << "DynamicTable::initialize IO object has been deleted."
              << std::endl;
    return Status::Failure;
  }

  Status containerStatus = Container::initialize();
  if (description != "") {
    ioPtr->createAttribute(description, m_path, "description");
  }

  const auto effectiveSpecs =
      dataSpecs.empty() ? createDefaultDataSpecs() : dataSpecs;

  Status validationStatus = validateDataSpecs(effectiveSpecs);
  if (validationStatus != Status::Success) {
    throw std::invalid_argument(
        "DynamicTable::initialize: provided dataSpecs are invalid.");
  }

  Status configureStatus = configureDataObjects(effectiveSpecs);
  return containerStatus && configureStatus;
}

Status DynamicTable::validateDataSpecs(
    const std::vector<DataSpecPtr>& dataSpecs) const
{
  return checkRequiredColumnNames({"id"}, dataSpecs);
}

Status DynamicTable::checkRequiredColumnNames(
    const std::vector<std::string>& requiredNames,
    const std::vector<DataSpecPtr>& dataSpecs) const
{
  if (dataSpecs.empty()) {
    std::cerr
        << "DynamicTable::checkRequiredColumnNames: dataSpecs vector is empty."
        << std::endl;
    return Status::Failure;
  }

  for (const auto& reqName : requiredNames) {
    if (!std::any_of(dataSpecs.begin(),
                     dataSpecs.end(),
                     [&reqName](const DataSpecPtr& spec)
                     { return spec && spec->name == reqName; }))
    {
      std::cerr << "DynamicTable::checkRequiredColumnNames: required column '"
                << reqName << "' not found." << std::endl;
      return Status::Failure;
    }
  }

  return Status::Success;
}

std::vector<DynamicTable::DataSpecPtr> DynamicTable::createDefaultDataSpecs(
    const SizeType rowChunkSize)
{
  return {ElementIdentifiers::createDataSpec(
      "id",
      IO::ArrayDataSetConfig(
          IO::BaseDataType::I32, SizeArray {0}, SizeArray {rowChunkSize}))};
}

void DynamicTable::setColNames(const std::vector<std::string>& newColNames)
{
  if (newColNames == m_colNames) {
    return;
  }

  const std::unordered_set<std::string> uniqueNames(newColNames.begin(),
                                                    newColNames.end());
  const bool hasDuplicates = uniqueNames.size() != newColNames.size();
  const bool removesExistingColumn =
      std::any_of(m_colNames.begin(),
                  m_colNames.end(),
                  [&uniqueNames](const std::string& name)
                  { return uniqueNames.find(name) == uniqueNames.end(); });
  if (hasDuplicates || removesExistingColumn) {
    std::cerr << "New column names must be unique and retain every existing "
                 "column name."
              << std::endl;
    throw std::invalid_argument(
        "New column names must be unique and retain existing columns.");
  }

  m_colNames = newColNames;
  flushColNames();
}

SizeType DynamicTable::addColumnName(const std::string& colName)
{
  auto it = std::find(m_colNames.begin(), m_colNames.end(), colName);
  if (it != m_colNames.end()) {
    // Column name already exists, return its index
    return static_cast<SizeType>(std::distance(m_colNames.begin(), it));
  } else {
    // Column name does not exist, add it and return new index
    m_colNames.push_back(colName);
    flushColNames();
    return m_colNames.size() - 1;
  }
}

SizeArray DynamicTable::columnWriteOffset(
    const std::shared_ptr<IO::BaseRecordingData>& /*dataset*/) const
{
  return SizeArray {0};
}

Status DynamicTable::addColumn(const DataSpecPtr& dataSpec)
{
  if (!dataSpec) {
    std::cerr << "DynamicTable::addColumn received null DataSpec." << std::endl;
    return Status::Failure;
  }
  return configureDataObject(*dataSpec);
}

/** Add column to table */
Status DynamicTable::addColumn(const std::shared_ptr<VectorData>& vectorData,
                               const std::vector<std::string>& values)
{
  if (!vectorData) {
    std::cerr << "VectorData column is null" << std::endl;
    return Status::Failure;
  }
  if (!vectorData->isInitialized()) {
    std::cerr << "VectorData dataset is not initialized "
              << vectorData->getPath() << std::endl;
    return Status::Failure;
  }
  auto dataset = vectorData->recordData();
  if (!dataset) {
    return Status::Failure;
  }
  Status writeStatus = dataset->writeDataBlock(SizeArray {values.size()},
                                               columnWriteOffset(dataset),
                                               IO::BaseDataType::V_STR,
                                               values);
  addColumnName(vectorData->getName());
  addConfiguredColumn(vectorData);
  return writeStatus;
}

Status DynamicTable::addColumn(const std::shared_ptr<VectorData>& vectorData)
{
  if (!vectorData) {
    std::cerr << "VectorData column is null" << std::endl;
    return Status::Failure;
  }
  if (!vectorData->isInitialized()) {
    std::cerr << "VectorData dataset is not initialized "
              << vectorData->getPath() << std::endl;
    return Status::Failure;
  }
  addColumnName(vectorData->getName());
  addConfiguredColumn(vectorData);
  return Status::Success;
}

Status DynamicTable::setRowIDs(const std::vector<int>& values)
{
  if (values.empty()) {
    return Status::Success;
  }

  if (!m_rowElementIdentifiers) {
    m_rowElementIdentifiers = readIdColumn();
  }
  if (!m_rowElementIdentifiers) {
    std::cerr << "ElementIdentifiers dataset is not initialized" << std::endl;
    return Status::Failure;
  }

  auto idData = m_rowElementIdentifiers->recordData();
  if (!idData) {
    return Status::Failure;
  }
  const auto& currentShape = idData->getShape();
  const SizeArray positionOffset = {currentShape.empty() ? 0 : currentShape[0]};
  return idData->writeDataBlock(SizeArray {values.size()},
                                positionOffset,
                                IO::BaseDataType::I32,
                                values.data());
}

Status DynamicTable::addRow(const RowData& row, const std::optional<int>& rowId)
{
  if (rowId.has_value()) {
    return addRows(std::vector<RowData> {row}, std::vector<int> {*rowId});
  }
  return addRows(std::vector<RowData> {row});
}

Status DynamicTable::addRows(const std::vector<RowData>& rows,
                             const std::vector<int>& rowIds)
{
  if (rows.empty()) {
    return Status::Success;
  }
  Status loadStatus = ensureConfiguredColumnsLoaded();
  if (loadStatus != Status::Success) {
    return Status::Failure;
  }
  if (m_configuredColumns.empty()) {
    std::cerr << "DynamicTable::addRows no configured columns available."
              << std::endl;
    return Status::Failure;
  }
  if (!rowIds.empty() && rowIds.size() != rows.size()) {
    std::cerr << "DynamicTable::addRows rowIds size must match rows size."
              << std::endl;
    return Status::Failure;
  }

  std::vector<BufferVariant> columnBuffers;
  columnBuffers.reserve(m_configuredColumns.size());
  for (const auto& configuredColumn : m_configuredColumns) {
    columnBuffers.push_back(
        IO::BaseDataType::createEmptyVectorVariant(configuredColumn.dataType));
  }

  for (const auto& row : rows) {
    if (row.size() != m_configuredColumns.size()) {
      std::cerr << "DynamicTable::addRows row size does not match configured "
                   "column count."
                << std::endl;
      return Status::Failure;
    }
    for (SizeType i = 0; i < m_configuredColumns.size(); ++i) {
      const auto& configuredColumn = m_configuredColumns[i];
      auto rowValueIt = row.find(configuredColumn.name);
      if (rowValueIt == row.end()) {
        std::cerr << "DynamicTable::addRows missing value for column '"
                  << configuredColumn.name << "'." << std::endl;
        return Status::Failure;
      }
      if (!appendCellToBuffer(
              columnBuffers[i], configuredColumn.dataType, rowValueIt->second))
      {
        std::cerr << "DynamicTable::addRows value type mismatch for column '"
                  << configuredColumn.name << "'." << std::endl;
        return Status::Failure;
      }
    }
  }

  Status status = Status::Success;
  for (SizeType i = 0; i < m_configuredColumns.size(); ++i) {
    status = status
        && writeColumnBuffer(
                 m_configuredColumns[i], columnBuffers[i], rows.size());
  }
  if (status != Status::Success) {
    return status;
  }

  std::vector<int> idsToWrite =
      rowIds.empty() ? generateRowIDs(rows.size()) : rowIds;
  return setRowIDs(idsToWrite);
}

Status DynamicTable::addReferenceColumn(const std::string& name,
                                        const std::string& colDescription,
                                        const std::vector<std::string>& dataset)
{
  if (dataset.empty()) {
    std::cerr << "Data to add to column is empty" << std::endl;
    return Status::Failure;
  }

  auto ioPtr = getIO();
  if (!ioPtr) {
    std::cerr << "DynamicTable::addReferenceColumn IO object has been deleted."
              << std::endl;
    return Status::Failure;
  }

  const std::string columnPath = AQNWB::mergePaths(m_path, name);
  std::shared_ptr<VectorData> refColumn;
  if (ioPtr->objectExists(columnPath)) {
    if (ioPtr->appendReferenceDataSet(columnPath, dataset) != Status::Success) {
      std::cerr << "Failed to append to reference column " << columnPath
                << std::endl;
      return Status::Failure;
    }
    refColumn = VectorData::create(columnPath, ioPtr);
  } else {
    refColumn = VectorData::createReferenceVectorData(
        columnPath, ioPtr, colDescription, dataset);
    if (!refColumn) {
      std::cerr << "Failed to create reference column" << std::endl;
      return Status::Failure;
    }
  }

  addColumnName(name);
  addConfiguredColumn(refColumn);
  return Status::Success;
}

Status DynamicTable::flushColNames()
{
  auto ioPtr = getIO();
  if (!ioPtr) {
    std::cerr << "DynamicTable::flushColNames IO object has been deleted."
              << std::endl;
    return Status::Failure;
  }
  Status colNamesStatus = ioPtr->createAttribute(
      m_colNames,
      m_path,
      "colnames",
      true  // overwrite the attribute if it already exists
  );
  return colNamesStatus;
}

Status DynamicTable::finalize()
{
  Status parentStatus = Container::finalize();
  return parentStatus;
}

std::shared_ptr<MeaningsTable> DynamicTable::createMeaningsTable(
    const std::string& columnName, const SizeType rowChunkSize)
{
  // Get the I/O object an ensure it is valid
  auto ioPtr = getIO();
  if (!ioPtr) {
    std::cerr << "DynamicTable::createMeaningsTable IO object has been deleted."
              << std::endl;
    return nullptr;
  }

  // Check that the column VectorData exists in the DynamicTable
  auto columnVectorData = readColumn<VectorData>(columnName);
  if (!columnVectorData) {
    std::cerr << "Column VectorData '" << columnName
              << "' does not exist in DynamicTable '" << m_path
              << "'. Cannot create MeaningsTable." << std::endl;
    return nullptr;
  }
  auto valueDataType = columnVectorData->readData()->getDataType();

  // Check the meanings_tables group exists, if not create it
  std::string meaningsTablesGroupPath =
      AQNWB::mergePaths(m_path, "meanings_tables");
  if (!ioPtr->objectExists(meaningsTablesGroupPath)) {
    Status createGroupStatus = ioPtr->createGroup(meaningsTablesGroupPath);
    if (createGroupStatus != Status::Success) {
      std::cerr << "Failed to create meanings_tables group at '"
                << meaningsTablesGroupPath << "'." << std::endl;
      return nullptr;
    }
  }

  // Create the MeaningsTable for the specified column
  std::string meaningsTablePath =
      AQNWB::mergePaths(meaningsTablesGroupPath, columnName + "_meanings");
  auto meaningsTable = MeaningsTable::create(meaningsTablePath, ioPtr);
  if (!meaningsTable) {
    std::cerr << "Failed to create MeaningsTable at '" << meaningsTablePath
              << "'." << std::endl;
    return nullptr;
  }

  // Initialize the MeaningsTable with the target VectorData and value data type
  auto specs =
      MeaningsTable::createDefaultDataSpecs(valueDataType, rowChunkSize);
  Status initStatus =
      meaningsTable->initialize(*columnVectorData,
                                valueDataType,
                                "Meanings table for column: " + columnName,
                                specs);

  // Report error if initialization failed
  if (initStatus != Status::Success) {
    std::cerr << "Failed to initialize MeaningsTable at '" << meaningsTablePath
              << "'." << std::endl;
    return nullptr;
  }

  // Return the created MeaningsTable
  return meaningsTable;
}

std::shared_ptr<MeaningsTable> DynamicTable::readMeaningsTable(
    const std::string& objectName) const
{
  std::string prefixPath = AQNWB::mergePaths(m_path, "meanings_tables");
  std::string objectPath = AQNWB::mergePaths(prefixPath, objectName);
  auto ioPtr = getIO();
  if (!ioPtr) {
    std::cerr << "IO object has been deleted. Can't read field: " << objectPath
              << std::endl;
    return nullptr;
  }
  if (ioPtr->objectExists(objectPath)) {
    return MeaningsTable::create(objectPath, ioPtr);
  }
  return nullptr;
}

std::shared_ptr<MeaningsTable> DynamicTable::createMeaningsTableInstance(
    const std::string& objectName) const
{
  std::string prefixPath = AQNWB::mergePaths(m_path, "meanings_tables");
  std::string objectPath = AQNWB::mergePaths(prefixPath, objectName);
  auto ioPtr = getIO();
  if (!ioPtr) {
    std::cerr << "IO object has been deleted. Can't create field: "
              << objectPath << std::endl;
    return nullptr;
  }
  return MeaningsTable::create(objectPath, ioPtr);
}

std::shared_ptr<VectorData> DynamicTable::getConfiguredColumn(
    const std::string& name)
{
  auto it = m_configuredColumnIndices.find(name);
  if (it == m_configuredColumnIndices.end()) {
    if (loadConfiguredColumnsFromFile() != Status::Success) {
      return nullptr;
    }
    it = m_configuredColumnIndices.find(name);
  }
  if (it != m_configuredColumnIndices.end()) {
    return m_configuredColumns[it->second].column;
  }
  return nullptr;
}

Status DynamicTable::configureDataObjects(
    const std::vector<DataSpecPtr>& dataSpecs)
{
  m_configuredColumns.clear();
  m_configuredColumnIndices.clear();

  for (const auto& spec : dataSpecs) {
    if (!spec) {
      std::cerr << "DynamicTable::configureDataObjects received null spec."
                << std::endl;
      return Status::Failure;
    }
    Status status = configureDataObject(*spec);
    if (status != Status::Success) {
      return status;
    }
  }
  return Status::Success;
}

Status DynamicTable::configureDataObject(const DataSpec& dataSpec)
{
  auto ioPtr = getIO();
  if (!ioPtr) {
    return Status::Failure;
  }
  std::string columnPath = AQNWB::mergePaths(m_path, dataSpec.name);
  auto dataObj = dataSpec.create(columnPath, ioPtr);
  if (!dataObj) {
    return Status::Failure;
  }
  Status initStatus = dataSpec.initialize(*dataObj);
  if (initStatus != Status::Success) {
    return Status::Failure;
  }

  if (dataSpec.name == "id") {
    m_rowElementIdentifiers =
        std::dynamic_pointer_cast<ElementIdentifiers>(dataObj);
    return Status::Success;
  }

  auto vectorData = std::dynamic_pointer_cast<VectorData>(dataObj);
  if (!vectorData) {
    return Status::Failure;
  }

  m_configuredColumns.push_back(
      {dataSpec.name, dataSpec.getType(), vectorData});
  m_configuredColumnIndices[dataSpec.name] = m_configuredColumns.size() - 1;
  addColumnName(dataSpec.name);

  return Status::Success;
}

SizeType DynamicTable::addConfiguredColumn(
    const std::shared_ptr<VectorData>& column)
{
  if (!column) {
    std::cerr << "DynamicTable::addConfiguredColumn received null column."
              << std::endl;
    return static_cast<SizeType>(-1);
  }
  const auto existing = m_configuredColumnIndices.find(column->getName());
  if (existing != m_configuredColumnIndices.end()) {
    return existing->second;
  }

  const SizeType index = m_configuredColumns.size();
  m_configuredColumns.push_back(
      {column->getName(), column->readData()->getDataType(), column});
  m_configuredColumnIndices[column->getName()] = index;
  return index;
}

Status DynamicTable::ensureConfiguredColumnsLoaded()
{
  return loadConfiguredColumnsFromFile();
}

Status DynamicTable::loadConfiguredColumnsFromFile()
{
  auto ioPtr = getIO();
  if (!ioPtr) {
    return Status::Failure;
  }

  for (const auto& colName : m_colNames) {
    if (m_configuredColumnIndices.find(colName)
        != m_configuredColumnIndices.end())
    {
      continue;
    }
    if (auto col = readColumn<VectorData>(colName)) {
      addConfiguredColumn(col);
    }
  }
  return Status::Success;
}

Status DynamicTable::writeColumnBuffer(
    const ConfiguredColumn& configuredColumn,
    const IO::BaseDataType::BaseDataVectorVariant& buffer,
    SizeType rowCount)
{
  auto dataset = configuredColumn.column->recordData();
  SizeArray positionOffset = {0};
  auto currentShape = dataset->getShape();
  if (!currentShape.empty()) {
    positionOffset[0] = currentShape[0];
  }

  return std::visit(
      [&](auto&& arg) -> Status
      {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
          return Status::Failure;
        } else {
          if constexpr (std::is_same_v<T, std::vector<std::string>>) {
            return dataset->writeDataBlock(SizeArray {rowCount},
                                           positionOffset,
                                           configuredColumn.dataType,
                                           arg);
          } else {
            return dataset->writeDataBlock(SizeArray {rowCount},
                                           positionOffset,
                                           configuredColumn.dataType,
                                           arg.data());
          }
        }
      },
      buffer);
}

std::vector<int> DynamicTable::generateRowIDs(SizeType rowCount)
{
  std::vector<int> ids(rowCount);
  int startId = 0;
  if (!m_rowElementIdentifiers) {
    m_rowElementIdentifiers = readIdColumn();
  }
  const auto idData =
      m_rowElementIdentifiers ? m_rowElementIdentifiers->recordData() : nullptr;
  if (idData && !idData->getShape().empty()) {
    startId = static_cast<int>(idData->getShape()[0]);
  }
  std::iota(ids.begin(), ids.end(), startId);
  return ids;
}
