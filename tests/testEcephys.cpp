#include <algorithm>
#include <array>

#include <H5Cpp.h>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_all.hpp>

#include "Channel.hpp"
#include "Types.hpp"
#include "Utils.hpp"
#include "io/BaseIO.hpp"
#include "io/hdf5/HDF5IO.hpp"
#include "nwb/RegisteredType.hpp"
#include "nwb/device/Device.hpp"
#include "nwb/ecephys/ElectricalSeries.hpp"
#include "nwb/ecephys/FeatureExtraction.hpp"
#include "nwb/ecephys/SpikeEventSeries.hpp"
#include "nwb/file/ElectrodeGroup.hpp"
#include "nwb/file/ElectrodesTable.hpp"
#include "testUtils.hpp"

using namespace AQNWB;

TEST_CASE("registered ecephys types", "[ecephys]")
{
  auto registry = AQNWB::NWB::RegisteredType::getRegistry();
  REQUIRE(registry.find("core::Device") != registry.end());
  REQUIRE(registry.find("core::ElectrodeGroup") != registry.end());
  REQUIRE(registry.find("core::ElectrodesTable") != registry.end());
  REQUIRE(registry.find("core::ElectricalSeries") != registry.end());
  REQUIRE(registry.find("core::FeatureExtraction") != registry.end());
  REQUIRE(registry.find("core::SpikeEventSeries") != registry.end());
}

TEST_CASE("ElectricalSeries", "[ecephys]")
{
  // setup recording info
  constexpr SizeType numSamples = 100;
  constexpr SizeType numChannels = 2;
  constexpr SizeType bufferSize = numSamples / 5;
  std::vector<float> dataBuffer(bufferSize);
  std::vector<double> timestampsBuffer(bufferSize);
  std::vector<Types::ChannelVector> mockArrays = getMockChannelArrays();
  std::string dataPath = "/esdata";
  BaseDataType dataType = BaseDataType::F32;
  std::vector<std::vector<float>> mockData =
      getMockData2D(numSamples, numChannels);
  std::vector<double> mockTimestamps = getMockTimestamps(numSamples, 1);
  std::string devicePath = "/device";
  std::string electrodePath =
      "/general/extracellular_ephys/" + mockArrays[0][0].getGroupName();

  // Helper: creates and initializes an IO object together with the standard
  // device / electrode-group / electrode-table / ElectricalSeries stack that
  // is common to all write-related test SECTIONs in this TEST_CASE.
  // Returns {io, es, elecTable}.
  auto createTestElectricalSeries = [&](const std::string& path)
      -> std::tuple<std::shared_ptr<BaseIO>,
                    std::shared_ptr<NWB::ElectricalSeries>,
                    std::shared_ptr<NWB::ElectrodesTable>>
  {
    std::shared_ptr<BaseIO> io = createIO("HDF5", path);
    io->open();
    io->createGroup("/general");
    io->createGroup("/general/extracellular_ephys");

    auto device = NWB::Device::create(devicePath, io);
    device->initialize("description", "unknown");
    auto elecGroup = NWB::ElectrodeGroup::create(electrodePath, io);
    elecGroup->initialize("description", "unknown", device);

    auto elecTable = NWB::ElectrodesTable::create(io);
    Status elecTableStatus = elecTable->initialize("description");
    REQUIRE(elecTableStatus == Status::Success);
    elecTable->addElectrodes(mockArrays[0]);
    elecTableStatus = elecTable->finalize();
    REQUIRE(elecTableStatus == Status::Success);

    auto es = NWB::ElectricalSeries::create(dataPath, io);
    IO::ArrayDataSetConfig config(
        dataType, SizeArray {0, mockArrays[0].size()}, SizeArray {1, 1});
    es->initialize(config, mockArrays[0], "no description");

    return {io, es, elecTable};
  };

  SECTION("test writing channels")
  {
    std::string path = getTestFilePath("ElectricalSeries.h5");
    auto [io, es, elecTable] = createTestElectricalSeries(path);

    // Confirm that the electrode table is created correctly
    auto readColNames = elecTable->readColNames()->values().data;
    std::vector<std::string> expectedColNames = {
        "location", "group_name", "group"};
    REQUIRE(readColNames == expectedColNames);

    // write channel data
    for (SizeType ch = 0; ch < numChannels; ++ch) {
      es->writeChannel(
          ch, numSamples, mockData[ch].data(), mockTimestamps.data());
    }
    io->flush();
    io->close();

    // Read data back from file
    std::unique_ptr<H5::H5File> file =
        std::make_unique<H5::H5File>(path, H5F_ACC_RDONLY);
    std::unique_ptr<H5::DataSet> dataset =
        std::make_unique<H5::DataSet>(file->openDataSet(dataPath + "/data"));
    std::vector<std::vector<float>> dataOut(numChannels,
                                            std::vector<float>(numSamples));
    float* buffer = new float[numSamples * numChannels];

    H5::DataSpace fSpace = dataset->getSpace();
    hsize_t dims[1] = {numSamples * numChannels};
    H5::DataSpace mSpace(1, dims);
    dataset->read(buffer, H5::PredType::NATIVE_FLOAT, mSpace, fSpace);

    for (SizeType i = 0; i < numChannels; ++i) {
      for (SizeType j = 0; j < numSamples; ++j) {
        dataOut[i][j] = buffer[j * numChannels + i];
      }
    }
    delete[] buffer;
    REQUIRE_THAT(dataOut[0], Catch::Matchers::Approx(mockData[0]).margin(1));
    REQUIRE_THAT(dataOut[1], Catch::Matchers::Approx(mockData[1]).margin(1));
  }

  SECTION("test samples recorded tracking")
  {
    std::string path = getTestFilePath("ElectricalSeriesSampleTracking.h5");
    auto [io, es, elecTable] = createTestElectricalSeries(path);

    // Confirm that the electrode table is created correctly
    auto readColNames = elecTable->readColNames()->values().data;
    std::vector<std::string> expectedColNames = {
        "location", "group_name", "group"};
    REQUIRE(readColNames == expectedColNames);

    // write channel data in segments
    for (SizeType ch = 0; ch < numChannels; ++ch) {
      SizeType samplesRecorded = 0;
      for (SizeType b = 0; b * bufferSize < numSamples; b += 1) {
        // copy chunk of data
        std::copy(
            mockData[ch].begin() + static_cast<std::ptrdiff_t>(samplesRecorded),
            mockData[ch].begin()
                + static_cast<std::ptrdiff_t>(samplesRecorded + bufferSize),
            dataBuffer.begin());
        std::copy(
            mockTimestamps.begin()
                + static_cast<std::ptrdiff_t>(samplesRecorded),
            mockTimestamps.begin()
                + static_cast<std::ptrdiff_t>(samplesRecorded + bufferSize),
            timestampsBuffer.begin());

        es->writeChannel(
            ch, dataBuffer.size(), dataBuffer.data(), timestampsBuffer.data());
        samplesRecorded += bufferSize;
      }
    }
    io->close();

    // Read data back from file
    std::unique_ptr<H5::H5File> file =
        std::make_unique<H5::H5File>(path, H5F_ACC_RDONLY);
    std::unique_ptr<H5::DataSet> dataset =
        std::make_unique<H5::DataSet>(file->openDataSet(dataPath + "/data"));
    std::vector<std::vector<float>> dataOut(numChannels,
                                            std::vector<float>(numSamples));
    float* buffer = new float[numSamples * numChannels];

    H5::DataSpace fSpace = dataset->getSpace();
    hsize_t dims[1] = {numSamples * numChannels};
    H5::DataSpace mSpace(1, dims);
    dataset->read(buffer, H5::PredType::NATIVE_FLOAT, mSpace, fSpace);

    for (SizeType i = 0; i < numChannels; ++i) {
      for (SizeType j = 0; j < numSamples; ++j) {
        dataOut[i][j] = buffer[j * numChannels + i];
      }
    }
    delete[] buffer;
    REQUIRE_THAT(dataOut[0], Catch::Matchers::Approx(mockData[0]).margin(1));
    REQUIRE_THAT(dataOut[1], Catch::Matchers::Approx(mockData[1]).margin(1));
  }

  SECTION("test writing interleaved multichannel data")
  {
    // setup io object and electrical series
    std::string path = getTestFilePath("ElectricalSeriesMultichannel.h5");
    auto [io, es, elecTable] = createTestElectricalSeries(path);

    // Build interleaved buffer as 2D array: interleavedData[t][ch]
    std::array<std::array<float, numChannels>, numSamples> interleavedData;
    for (SizeType t = 0; t < numSamples; ++t) {
      for (SizeType ch = 0; ch < numChannels; ++ch) {
        interleavedData[t][ch] = mockData[ch][t];
      }
    }

    // Write all channels at once using the new writeAllChannels method
    Status writeStatus = es->writeAllChannels(
        numSamples, interleavedData.data(), mockTimestamps.data());
    REQUIRE(writeStatus == Status::Success);

    io->flush();
    io->close();
    std::unique_ptr<H5::H5File> file =
        std::make_unique<H5::H5File>(path, H5F_ACC_RDONLY);
    std::unique_ptr<H5::DataSet> dataset =
        std::make_unique<H5::DataSet>(file->openDataSet(dataPath + "/data"));
    float* readBuffer = new float[numSamples * numChannels];

    H5::DataSpace fSpace = dataset->getSpace();
    hsize_t dims[1] = {numSamples * numChannels};
    H5::DataSpace mSpace(1, dims);
    dataset->read(readBuffer, H5::PredType::NATIVE_FLOAT, mSpace, fSpace);

    std::vector<std::vector<float>> dataOut(numChannels,
                                            std::vector<float>(numSamples));
    for (SizeType i = 0; i < numChannels; ++i) {
      for (SizeType j = 0; j < numSamples; ++j) {
        dataOut[i][j] = readBuffer[j * numChannels + i];
      }
    }
    delete[] readBuffer;
    REQUIRE_THAT(dataOut[0], Catch::Matchers::Approx(mockData[0]).margin(1));
    REQUIRE_THAT(dataOut[1], Catch::Matchers::Approx(mockData[1]).margin(1));
  }

  SECTION("test writing interleaved multichannel data in segments")
  {
    // setup io object and electrical series
    std::string path =
        getTestFilePath("ElectricalSeriesMultichannelSegmented.h5");
    auto [io, es, elecTable] = createTestElectricalSeries(path);

    // Build interleaved buffer as 2D array: interleavedData[t][ch]
    std::array<std::array<float, numChannels>, numSamples> interleavedData;
    for (SizeType t = 0; t < numSamples; ++t) {
      for (SizeType ch = 0; ch < numChannels; ++ch) {
        interleavedData[t][ch] = mockData[ch][t];
      }
    }

    // Write in chunks using writeData
    SizeType samplesRecorded = 0;
    while (samplesRecorded < numSamples) {
      SizeType chunkSamples =
          std::min(bufferSize, numSamples - samplesRecorded);
      Status writeStatus =
          es->writeAllChannels(chunkSamples,
                               interleavedData.data() + samplesRecorded,
                               mockTimestamps.data() + samplesRecorded);
      REQUIRE(writeStatus == Status::Success);
      samplesRecorded += chunkSamples;
    }

    io->close();

    // Read data back and verify
    std::unique_ptr<H5::H5File> file =
        std::make_unique<H5::H5File>(path, H5F_ACC_RDONLY);
    std::unique_ptr<H5::DataSet> dataset =
        std::make_unique<H5::DataSet>(file->openDataSet(dataPath + "/data"));
    float* readBuffer = new float[numSamples * numChannels];

    H5::DataSpace fSpace = dataset->getSpace();
    hsize_t dims[1] = {numSamples * numChannels};
    H5::DataSpace mSpace(1, dims);
    dataset->read(readBuffer, H5::PredType::NATIVE_FLOAT, mSpace, fSpace);

    std::vector<std::vector<float>> dataOut(numChannels,
                                            std::vector<float>(numSamples));
    for (SizeType i = 0; i < numChannels; ++i) {
      for (SizeType j = 0; j < numSamples; ++j) {
        dataOut[i][j] = readBuffer[j * numChannels + i];
      }
    }
    delete[] readBuffer;
    REQUIRE_THAT(dataOut[0], Catch::Matchers::Approx(mockData[0]).margin(1));
    REQUIRE_THAT(dataOut[1], Catch::Matchers::Approx(mockData[1]).margin(1));
  }

  SECTION("test channelsAtSameSampleOffset")
  {
    // setup io object and electrical series
    std::string path = getTestFilePath("ElectricalSeriesChannelOffset.h5");
    auto [io, es, elecTable] = createTestElectricalSeries(path);

    // Immediately after initialize all per-channel counters are 0 → same
    // offset, so the function should return true.
    REQUIRE(es->channelsAtSameSampleOffset() == true);

    // Write the same number of samples to every channel; offsets remain
    // equal, so the function should still return true.
    es->writeChannel(0, bufferSize, mockData[0].data(), mockTimestamps.data());
    es->writeChannel(1, bufferSize, mockData[1].data(), mockTimestamps.data());
    REQUIRE(es->channelsAtSameSampleOffset() == true);

    // Write only to channel 0 so that its offset now exceeds channel 1's.
    // The function should report false, and writeAllChannels should fail.
    es->writeChannel(0, bufferSize, mockData[0].data(), mockTimestamps.data());
    REQUIRE(es->channelsAtSameSampleOffset() == false);

    // writeAllChannels must return Failure when offsets differ.
    std::array<std::array<float, numChannels>, bufferSize> interleavedData {};
    Status writeStatus = es->writeAllChannels(
        bufferSize, interleavedData.data(), mockTimestamps.data());
    REQUIRE(writeStatus == Status::Failure);

    // Restore balance: write the same number of samples to channel 1.
    es->writeChannel(1, bufferSize, mockData[1].data(), mockTimestamps.data());
    REQUIRE(es->channelsAtSameSampleOffset() == true);

    // After rebalancing, writeAllChannels should succeed.
    writeStatus = es->writeAllChannels(
        bufferSize, interleavedData.data(), mockTimestamps.data());
    REQUIRE(writeStatus == Status::Success);

    io->close();
  }

  SECTION("test writing electrodes")
  {
    std::vector<Types::ChannelVector> mockArraysElectrodes =
        getMockChannelArrays(4);

    // setup io object
    std::string path = getTestFilePath("ElectricalSeriesElectrodes.h5");
    std::shared_ptr<BaseIO> io = createIO("HDF5", path);
    io->open();
    io->createGroup("/general");
    io->createGroup("/general/extracellular_ephys");

    // setup device and electrode group
    auto device = NWB::Device::create(devicePath, io);
    device->initialize("description", "unknown");
    auto elecGroup = NWB::ElectrodeGroup::create(electrodePath, io);
    elecGroup->initialize("description", "unknown", device);

    // setup electrode table
    auto elecTable = NWB::ElectrodesTable::create(io);
    elecTable->initialize("description");
    elecTable->addElectrodes(mockArraysElectrodes[0]);
    elecTable->finalize();

    // setup electrical series
    auto es = NWB::ElectricalSeries::create(dataPath, io);
    IO::ArrayDataSetConfig config(BaseDataType::F32,
                                  SizeArray {0, mockArrays[0].size()},
                                  SizeArray {1, 1});
    es->initialize(config, mockArraysElectrodes[0], "no description");
    io->close();

    // // read the data back in
    io = createIO("HDF5", path);
    io->open();

    // Verify electrodes dataset exists and contains correct data
    auto readElectricalSeries =
        NWB::RegisteredType::create<NWB::ElectricalSeries>(dataPath, io);
    auto readElectrodesWrapper = readElectricalSeries->readElectrodes();
    auto readElectrodesValues = readElectrodesWrapper->values();
    for (size_t i = 0; i < mockArraysElectrodes[0].size(); ++i) {
      REQUIRE(static_cast<SizeType>(readElectrodesValues.data[i])
              == mockArraysElectrodes[0][i].getGlobalIndex());
    }

    // Verify dataset attributes
    auto readElectrodesDescriptionWrapper =
        readElectricalSeries->readElectrodesDescription();
    auto readElectrodesDescriptionValues =
        readElectrodesDescriptionWrapper->values().data[0];
    REQUIRE(readElectrodesDescriptionValues
            == "the electrodes that generated this electrical series");

    // Read the references to the ElectrodesTable
    auto readElectrodesTable = readElectricalSeries->readElectrodesTable();
    REQUIRE(readElectrodesTable != nullptr);
    REQUIRE(readElectrodesTable->getPath()
            == AQNWB::NWB::ElectrodesTable::electrodesTablePath);
  }

  SECTION("test reading electrodes")
  {
    std::vector<Types::ChannelVector> mockArraysElectrodes =
        getMockChannelArrays(4);

    // setup io object
    std::string path = getTestFilePath("ElectrodesTableRead.h5");
    std::shared_ptr<BaseIO> io = createIO("HDF5", path);
    io->open();
    io->createGroup("/general");
    io->createGroup("/general/extracellular_ephys");

    // setup device and electrode group
    auto device = NWB::Device::create(devicePath, io);
    device->initialize("description", "unknown");
    auto elecGroup = NWB::ElectrodeGroup::create(electrodePath, io);
    elecGroup->initialize("description", "unknown", device);

    // setup electrode table
    auto elecTable = NWB::ElectrodesTable::create(io);
    elecTable->initialize("description");
    elecTable->addElectrodes(mockArraysElectrodes[0]);
    elecTable->finalize();

    // // read the data back in
    io = createIO("HDF5", path);
    io->open();

    // Confirm the typename in the file
    AQNWB::IO::DataBlockGeneric typeData = io->readAttribute(AQNWB::mergePaths(
        AQNWB::NWB::ElectrodesTable::electrodesTablePath, "neurodata_type"));
    auto typeBlock = AQNWB::IO::DataBlock<std::string>::fromGeneric(typeData);
    std::string typeName = typeBlock.data[0];
    REQUIRE(typeName == "ElectrodesTable");

    // Read using the RegisteredType::create where we infer the type from the
    // file This should result in a `ElectrodesTable` object
    std::string electrodesTableTypeName2 =
        io->getFullTypeName(AQNWB::NWB::ElectrodesTable::electrodesTablePath);
    REQUIRE(electrodesTableTypeName2 == "core::ElectrodesTable");
    auto readElectrodesTable2 = AQNWB::NWB::RegisteredType::create(
        AQNWB::NWB::ElectrodesTable::electrodesTablePath, io);
    REQUIRE(readElectrodesTable2->getFullTypeName() == "core::ElectrodesTable");
    auto readElectrodesTable2Cast =
        std::dynamic_pointer_cast<AQNWB::NWB::ElectrodesTable>(
            readElectrodesTable2);
    REQUIRE(readElectrodesTable2Cast != nullptr);

    // Testing backward compatibility of ElectrodesTable with NWB <=2.8
    // To test for older files, we modify the neurodata_type attribute for our
    // ElectrodesTable to be DynamicTable instead
    io->createAttribute("DynamicTable",
                        AQNWB::NWB::ElectrodesTable::electrodesTablePath,
                        "neurodata_type",
                        true);
    // read to confirm the overwrite worked
    typeData = io->readAttribute(AQNWB::mergePaths(
        AQNWB::NWB::ElectrodesTable::electrodesTablePath, "neurodata_type"));
    auto typeBlock2 = AQNWB::IO::DataBlock<std::string>::fromGeneric(typeData);
    typeName = typeBlock2.data[0];
    REQUIRE(typeName == "DynamicTable");

    // Ensure the mapping of the typename in the I/O works
    std::string electrodesTableTypeName3 =
        io->getFullTypeName(AQNWB::NWB::ElectrodesTable::electrodesTablePath);
    REQUIRE(electrodesTableTypeName2 == "core::ElectrodesTable");

    // Ensure that reading with ElectrodesTable type directly still works as
    // expected
    auto readElectrodesTable4 =
        AQNWB::NWB::RegisteredType::create<AQNWB::NWB::ElectrodesTable>(
            AQNWB::NWB::ElectrodesTable::electrodesTablePath, io);
    REQUIRE(readElectrodesTable4 != nullptr);
    REQUIRE(readElectrodesTable4->getFullTypeName() == "core::ElectrodesTable");

    // Confirm that reading with the generic approach where the type is being
    // read from the file, also still works. I.e., confirm that the remapping to
    // the ElectrodesTable type is working as expected
    auto readElectrodesTable5 = AQNWB::NWB::RegisteredType::create(
        AQNWB::NWB::ElectrodesTable::electrodesTablePath, io);
    REQUIRE(readElectrodesTable5 != nullptr);
    REQUIRE(readElectrodesTable5->getFullTypeName() == "core::ElectrodesTable");
    auto readElectrodesTable5_cast =
        std::dynamic_pointer_cast<AQNWB::NWB::ElectrodesTable>(
            readElectrodesTable5);
    REQUIRE(readElectrodesTable5_cast != nullptr);
  }
}

TEST_CASE("ElectrodesTable append", "[ecephys][table]")
{
  const std::string path = getTestFilePath("ElectrodesTableAppend.h5");
  auto io = createIO("HDF5", path);
  REQUIRE(io->open() == Status::Success);
  REQUIRE(io->createGroup("/general") == Status::Success);
  REQUIRE(io->createGroup("/general/extracellular_ephys") == Status::Success);

  const auto mockArrays = getMockChannelArrays(2, 2);
  auto device = NWB::Device::create("/device", io);
  REQUIRE(device->initialize("description", "unknown") == Status::Success);
  for (const auto& channels : mockArrays) {
    const std::string groupPath =
        "/general/extracellular_ephys/" + channels.front().getGroupName();
    auto electrodeGroup = NWB::ElectrodeGroup::create(groupPath, io);
    REQUIRE(electrodeGroup->initialize("description", "unknown", device)
            == Status::Success);
  }

  auto electrodesTable = NWB::ElectrodesTable::create(io);
  REQUIRE(electrodesTable->initialize("description") == Status::Success);
  auto colNames = electrodesTable->readColNames()->values().data;
  colNames.push_back("label");
  electrodesTable->setColNames(colNames);
  const std::string labelPath =
      NWB::ElectrodesTable::electrodesTablePath + "/label";
  auto labelColumn = NWB::VectorData::create(labelPath, io);
  IO::ArrayDataSetConfig labelConfig(
      BaseDataType::V_STR, SizeArray {0}, SizeArray {2});
  REQUIRE(labelColumn->initialize(labelConfig, "electrode label")
          == Status::Success);
  REQUIRE(electrodesTable->addColumn(
              labelColumn, std::vector<std::string> {"label0", "label1"})
          == Status::Success);
  electrodesTable->addElectrodes(mockArrays[0]);
  REQUIRE(electrodesTable->finalize() == Status::Success);

  // A fresh wrapper around the existing table matches Orion's deferred-device
  // path: initialize is intentionally not called again.
  auto reopenedTable = NWB::ElectrodesTable::create(io);
  auto reopenedLabelColumn = NWB::VectorData::create(labelPath, io);
  REQUIRE(
      reopenedTable->addColumn(reopenedLabelColumn,
                               std::vector<std::string> {"label2", "label3"})
      == Status::Success);
  reopenedTable->addElectrodes(mockArrays[1]);
  REQUIRE(reopenedTable->finalize() == Status::Success);

  const std::vector<int> expectedIds = {0, 1, 2, 3};
  REQUIRE(reopenedTable->readIdColumn()->readData()->values().data
          == expectedIds);
  REQUIRE(reopenedTable->readLocationColumn()->readData()->values().data
          == std::vector<std::string>(
              {"unknown", "unknown", "unknown", "unknown"}));
  REQUIRE(
      reopenedTable->readGroupNameColumn()->readData()->values().data
      == std::vector<std::string>({"array0", "array0", "array1", "array1"}));
  REQUIRE(
      reopenedTable->readColumn<std::string>("label")->readData()->values().data
      == std::vector<std::string>({"label0", "label1", "label2", "label3"}));
  REQUIRE(io->getStorageObjectShape(NWB::ElectrodesTable::electrodesTablePath
                                    + "/group")
          == SizeArray {4});
  REQUIRE_FALSE(io->getStorageObjectChunking(
                      NWB::ElectrodesTable::electrodesTablePath + "/group")
                    .empty());

  io->close();

  H5::H5File file(path, H5F_ACC_RDONLY);
  H5::DataSet groupDataset =
      file.openDataSet(NWB::ElectrodesTable::electrodesTablePath + "/group");
  std::vector<hobj_ref_t> groupReferences(expectedIds.size());
  groupDataset.read(groupReferences.data(), H5::PredType::STD_REF_OBJ);
  const std::vector<std::string> expectedGroupPaths = {
      "/general/extracellular_ephys/array0",
      "/general/extracellular_ephys/array0",
      "/general/extracellular_ephys/array1",
      "/general/extracellular_ephys/array1"};
  for (SizeType i = 0; i < groupReferences.size(); ++i) {
    const hid_t groupId = H5Rdereference2(
        file.getId(), H5P_DEFAULT, H5R_OBJECT, &groupReferences[i]);
    REQUIRE(groupId >= 0);
    H5::Group group(groupId);
    REQUIRE(group.getObjName() == expectedGroupPaths[i]);
    H5Gclose(groupId);
  }
}

TEST_CASE("FeatureExtraction", "[ecephys]")
{
  constexpr SizeType numChannels = 2;
  constexpr SizeType numFeatures = 2;
  const std::string path = getTestFilePath("FeatureExtraction.h5");
  auto io = createIO("HDF5", path);
  REQUIRE(io->open() == Status::Success);
  REQUIRE(io->createGroup("/general") == Status::Success);
  REQUIRE(io->createGroup("/general/extracellular_ephys") == Status::Success);
  REQUIRE(io->createGroup("/processing") == Status::Success);
  REQUIRE(io->createGroup("/processing/ecephys") == Status::Success);

  const auto mockArrays = getMockChannelArrays(numChannels, 1);
  auto device = NWB::Device::create("/device", io);
  REQUIRE(device->initialize("description", "unknown") == Status::Success);
  const std::string electrodeGroupPath =
      "/general/extracellular_ephys/" + mockArrays[0].front().getGroupName();
  auto electrodeGroup = NWB::ElectrodeGroup::create(electrodeGroupPath, io);
  REQUIRE(electrodeGroup->initialize("description", "unknown", device)
          == Status::Success);

  auto electrodesTable = NWB::ElectrodesTable::create(io);
  REQUIRE(electrodesTable->initialize("description") == Status::Success);
  electrodesTable->addElectrodes(mockArrays[0]);
  REQUIRE(electrodesTable->finalize() == Status::Success);

  const std::string featurePath = "/processing/ecephys/features";
  auto featureExtraction = NWB::FeatureExtraction::create(featurePath, io);
  IO::ArrayDataSetConfig featuresConfig(BaseDataType::F32,
                                        SizeArray {0, numChannels, numFeatures},
                                        SizeArray {2, 0, 0});
  IO::ArrayDataSetConfig timesConfig(
      BaseDataType::F64, SizeArray {0}, SizeArray {2});
  const std::vector<std::string> featureLabels = {"spk", "sbp"};
  const std::vector<int> electrodeIndices = {0, 1};
  REQUIRE(featureExtraction->initialize(featuresConfig,
                                        timesConfig,
                                        featureLabels,
                                        electrodeIndices,
                                        numChannels,
                                        numFeatures)
          == Status::Success);
  REQUIRE(featureExtraction->numChannels() == numChannels);
  REQUIRE(featureExtraction->numFeatures() == numFeatures);

  const std::vector<float> firstFeatures = {
      1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f};
  const std::vector<double> firstTimes = {0.1, 0.2};
  REQUIRE(
      featureExtraction->writeEvents(2, firstFeatures.data(), firstTimes.data())
      == Status::Success);
  const std::vector<float> secondFeatures = {9.0f, 10.0f, 11.0f, 12.0f};
  const std::vector<double> secondTimes = {0.3};
  REQUIRE(featureExtraction->writeEvents(
              1, secondFeatures.data(), secondTimes.data())
          == Status::Success);

  std::vector<float> expectedFeatures = firstFeatures;
  expectedFeatures.insert(
      expectedFeatures.end(), secondFeatures.begin(), secondFeatures.end());
  std::vector<double> expectedTimes = firstTimes;
  expectedTimes.insert(
      expectedTimes.end(), secondTimes.begin(), secondTimes.end());
  REQUIRE(featureExtraction->readFeatures()->values().data == expectedFeatures);
  REQUIRE(featureExtraction->readTimes()->values().data == expectedTimes);
  REQUIRE(featureExtraction->readDescriptionDataset()->values().data
          == featureLabels);
  REQUIRE(featureExtraction->readElectrodes()->values().data
          == electrodeIndices);
  REQUIRE(featureExtraction->readElectrodesTable()->getPath()
          == NWB::ElectrodesTable::electrodesTablePath);
  REQUIRE(io->getStorageObjectShape(featurePath + "/features")
          == (SizeArray {3, numChannels, numFeatures}));

  SECTION("initialize rejects inconsistent arguments")
  {
    auto badFeatures =
        NWB::FeatureExtraction::create("/processing/ecephys/bad_features", io);

    // One label per feature dimension, one electrode index per channel.
    REQUIRE(badFeatures->initialize(featuresConfig,
                                    timesConfig,
                                    {"only_one_label"},
                                    electrodeIndices,
                                    numChannels,
                                    numFeatures)
            == Status::Failure);
    REQUIRE(badFeatures->initialize(featuresConfig,
                                    timesConfig,
                                    featureLabels,
                                    {0},
                                    numChannels,
                                    numFeatures)
            == Status::Failure);

    // features must be [0, numChannels, numFeatures] float32 and times must be
    // [0] float64, otherwise writeEvents would compute offsets that do not
    // match the dataset it writes into.
    IO::ArrayDataSetConfig wrongShape(
        BaseDataType::F32,
        SizeArray {0, numChannels + 1, numFeatures},
        SizeArray {2, numChannels + 1, numFeatures});
    REQUIRE(badFeatures->initialize(wrongShape,
                                    timesConfig,
                                    featureLabels,
                                    electrodeIndices,
                                    numChannels,
                                    numFeatures)
            == Status::Failure);

    IO::ArrayDataSetConfig wrongType(BaseDataType::F64,
                                     SizeArray {0, numChannels, numFeatures},
                                     SizeArray {2, numChannels, numFeatures});
    REQUIRE(badFeatures->initialize(wrongType,
                                    timesConfig,
                                    featureLabels,
                                    electrodeIndices,
                                    numChannels,
                                    numFeatures)
            == Status::Failure);

    IO::ArrayDataSetConfig wrongTimes(
        BaseDataType::F32, SizeArray {0}, SizeArray {2});
    REQUIRE(badFeatures->initialize(featuresConfig,
                                    wrongTimes,
                                    featureLabels,
                                    electrodeIndices,
                                    numChannels,
                                    numFeatures)
            == Status::Failure);
  }

  io->close();
}

TEST_CASE("SpikeEventSeries", "[ecephys]")
{
  // setup recording info
  SizeType numSamples = 32;
  SizeType numEvents = 10;
  std::string dataPath = "/sesdata";
  BaseDataType dataType = BaseDataType::F32;
  std::vector<double> mockTimestamps = getMockTimestamps(numEvents, 1);
  std::string devicePath = "/device";

  SECTION("test writing events - events x channels x samples")
  {
    // setup mock data
    SizeType numChannels = 4;
    std::vector<Types::ChannelVector> mockArrays =
        getMockChannelArrays(numChannels);
    std::vector<std::vector<float>> mockData =
        getMockData2D(numSamples * numChannels, numEvents);
    std::string electrodePath =
        "/general/extracellular_ephys/" + mockArrays[0][0].getGroupName();

    // setup io object
    std::string path = getTestFilePath("SpikeEventSeries3D.h5");
    std::shared_ptr<BaseIO> io = createIO("HDF5", path);
    io->open();
    io->createGroup("/general");
    io->createGroup("/general/extracellular_ephys");

    // setup device and electrode group
    auto device = NWB::Device::create(devicePath, io);
    device->initialize("description", "unknown");
    auto elecGroup = NWB::ElectrodeGroup::create(electrodePath, io);
    elecGroup->initialize("description", "unknown", device);

    // setup electrode table, device, and electrode group
    auto elecTable = NWB::ElectrodesTable::create(io);
    Status elecTableStatus = elecTable->initialize("description");
    REQUIRE(elecTableStatus == Status::Success);
    elecTable->addElectrodes(mockArrays[0]);
    elecTableStatus = elecTable->finalize();
    REQUIRE(elecTableStatus == Status::Success);

    // setup electrical series
    auto ses = NWB::SpikeEventSeries::create(dataPath, io);
    IO::ArrayDataSetConfig config(
        dataType, SizeArray {0, numChannels, numSamples}, SizeArray {8, 1, 1});
    ses->initialize(config, mockArrays[0], "no description");

    // write channel data
    for (SizeType e = 0; e < numEvents; ++e) {
      double timestamp = mockTimestamps[e];
      ses->writeSpike(numSamples, numChannels, mockData[e].data(), &timestamp);
    }
    io->close();

    // Read data back from file
    std::unique_ptr<H5::H5File> file =
        std::make_unique<H5::H5File>(path, H5F_ACC_RDONLY);
    std::unique_ptr<H5::DataSet> dataset =
        std::make_unique<H5::DataSet>(file->openDataSet(dataPath + "/data"));
    std::vector<std::vector<float>> dataOut(
        numEvents, std::vector<float>(numSamples * numChannels));
    float* buffer = new float[numEvents * numSamples * numChannels];

    H5::DataSpace fSpace = dataset->getSpace();
    hsize_t dims[3];
    fSpace.getSimpleExtentDims(dims, NULL);
    dataset->read(buffer, H5::PredType::NATIVE_FLOAT, fSpace, fSpace);

    for (SizeType i = 0; i < numEvents; ++i) {
      for (SizeType j = 0; j < (numSamples * numChannels); ++j) {
        dataOut[i][j] = buffer[i * (numSamples * numChannels) + j];
      }
    }
    delete[] buffer;
    REQUIRE_THAT(dataOut[0], Catch::Matchers::Approx(mockData[0]).margin(1));
    REQUIRE_THAT(dataOut[1], Catch::Matchers::Approx(mockData[1]).margin(1));
  }

  SECTION("test writing events - events x samples")
  {
    // setup mock data
    std::vector<Types::ChannelVector> mockArrays = getMockChannelArrays(1);
    std::vector<std::vector<float>> mockData =
        getMockData2D(numSamples, numEvents);
    std::string electrodePath =
        "/general/extracellular_ephys/" + mockArrays[0][0].getGroupName();

    // setup io object
    std::string path = getTestFilePath("SpikeEventSeries2D.h5");
    std::shared_ptr<BaseIO> io = createIO("HDF5", path);
    io->open();
    io->createGroup("/general");
    io->createGroup("/general/extracellular_ephys");

    // setup device and electrode group
    auto device = NWB::Device::create(devicePath, io);
    device->initialize("description", "unknown");
    auto elecGroup = NWB::ElectrodeGroup::create(electrodePath, io);
    elecGroup->initialize("description", "unknown", device);

    // setup electrode table, device, and electrode group
    auto elecTable = NWB::ElectrodesTable::create(io);
    Status elecTableStatus = elecTable->initialize("description");
    REQUIRE(elecTableStatus == Status::Success);
    elecTable->addElectrodes(mockArrays[0]);
    elecTableStatus = elecTable->finalize();
    REQUIRE(elecTableStatus == Status::Success);

    // setup electrical series
    auto ses = NWB::SpikeEventSeries::create(dataPath, io);
    IO::ArrayDataSetConfig config(
        dataType, SizeArray {0, numSamples}, SizeArray {8, 1});
    ses->initialize(config, mockArrays[0], "no description");

    // write channel data
    for (SizeType e = 0; e < numEvents; ++e) {
      double timestamp = mockTimestamps[e];
      ses->writeSpike(numSamples, 1, mockData[e].data(), &timestamp);
    }
    io->close();

    // Read data back from file
    std::unique_ptr<H5::H5File> file =
        std::make_unique<H5::H5File>(path, H5F_ACC_RDONLY);
    std::unique_ptr<H5::DataSet> dataset =
        std::make_unique<H5::DataSet>(file->openDataSet(dataPath + "/data"));
    std::vector<std::vector<float>> dataOut(numEvents,
                                            std::vector<float>(numSamples));
    float* buffer = new float[numEvents * numSamples];

    H5::DataSpace fSpace = dataset->getSpace();
    hsize_t dims[3];
    fSpace.getSimpleExtentDims(dims, NULL);
    dataset->read(buffer, H5::PredType::NATIVE_FLOAT, fSpace, fSpace);

    for (SizeType i = 0; i < numEvents; ++i) {
      for (SizeType j = 0; j < (numSamples); ++j) {
        dataOut[i][j] = buffer[i * (numSamples) + j];
      }
    }
    delete[] buffer;
    REQUIRE_THAT(dataOut[0], Catch::Matchers::Approx(mockData[0]).margin(1));
    REQUIRE_THAT(dataOut[1], Catch::Matchers::Approx(mockData[1]).margin(1));
  }
}
