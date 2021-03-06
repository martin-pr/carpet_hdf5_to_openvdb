#include <iostream>
#include <vector>
#include <functional>
#include <set>
#include <utility>

#include <boost/program_options.hpp>
#include <boost/filesystem.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <boost/regex.hpp>

#include <H5Cpp.h>
#include <H5Location.h>
#include <H5File.h>

#include <openvdb.h>
#include <math/Transform.h>

#include "printing.h"
#include "attr_getter.h"
#include "grid_collection.h"

using namespace H5;

namespace po = boost::program_options;

using std::cout;
using std::endl;

namespace {
	///////////////

	// template<typename T>
	// void printDatasetValues(const H5::DataSet& ds) {
	// 	const unsigned count = ds.getInMemDataSize() / sizeof(T);
	// 	T values[count];
	// 	DataType type = ds.getDataType();
	// 	ds.read((void*)values, type);

	// 	for(unsigned a=0;a<count;++a)
	// 		cout << values[a] << "  ";
	// }

	// void printDatasetContent(const H5File& file, const std::string datasetName) {
	// 	H5::DataSet ds = file.openDataSet(datasetName);
	// 	printDataset(ds);

	// 	cout << endl;

	// 	if(ds.getDataType().getClass() == H5T_INTEGER)
	// 		printDatasetValues<int>(ds);
	// 	else if(ds.getDataType().getClass() == H5T_FLOAT)
	// 		printDatasetValues<double>(ds);
	// 	else
	// 		cout << "(print not implemented)";
	// 	cout << endl;
	// }

	std::string houdiniName(const std::string& in) {
		// houdini hack for grid naming - Houdini has problems with many special symbols in grid name
		std::string gridName = in;
		boost::replace_all(gridName, " ", "_");
		boost::replace_all(gridName, ":", "_");
		boost::replace_all(gridName, "=", "_");

		return gridName;
	}

	openvdb::FloatGrid::Ptr makeGrid(const std::string& name, const std::array<double, 3>& origin, const std::array<double, 3>& delta) {
		// create the grid
		openvdb::FloatGrid::Ptr grid = openvdb::FloatGrid::create();
		// houdini hack for grid naming - Houdini has problems with many special symbols in grid name
		grid->setName(houdiniName(name));

		// transformed by the origin and delta values
		//   based on http://www.carpetcode.org/hg/carpet/index.cgi/rev/245224d7a5ec
		//   and http://www.openvdb.org/documentation/doxygen/classopenvdb_1_1v3__1__0_1_1math_1_1Transform.html#details
		const openvdb::math::Transform::Ptr tr = openvdb::math::Transform::createLinearTransform(openvdb::Mat4R(
			(float)delta[0], 0.0f, 0.0f, 0.0f,
			0.0f, (float)delta[1], 0.0f, 0.0f,
			0.0f, 0.0f, (float)delta[2], 0.0f,
			(float)origin[0], (float)origin[1], (float)origin[2], 1.0f
		));
		grid->setTransform(tr);

		return grid;
	}

	void writevdb(const H5File& file, const std::string datasetName, openvdb::FloatGrid::Ptr& grid, const std::array<double, 3>& gridOrigin, bool normalize, float offset) {
		H5::DataSet ds = file.openDataSet(datasetName);
		if(ds.getDataType().getClass() != H5T_FLOAT)
			throw std::runtime_error("OpenVDB output only supports float grids");

		H5::DataSpace space = ds.getSpace();
		if(space.getSimpleExtentNdims() != 3)
			throw std::runtime_error("OpenVDB output only supports 3D grids");

		hsize_t dims[3];
		space.getSimpleExtentDims(dims);

		const unsigned count = ds.getInMemDataSize() / sizeof(float);
		const unsigned computedSize = (dims[0]) * (dims[1]) * (dims[2]);

		if(count == 0)
			throw std::runtime_error("Empty grid cannot be used!");

		if(count != computedSize) {
			std::stringstream err;
			err << "Something wrong with the data count - getInMemDataSize gives " << count << "elements, while the computed size is " << computedSize << " elements.";
			throw std::runtime_error(err.str());
		}

		float values[count];
		DataType type = ds.getDataType();
		ds.read((void*)values, type);

		// normalize the values
		if(normalize) {
			float min = values[0];
			float max = values[0];
			for(unsigned a=0;a<count;++a) {
				min = std::min(values[a], min);
				max = std::max(values[a], max);
			}

			for(unsigned a=0;a<count;++a)
				values[a] = (values[a] - min) / (max - min) + offset;
		}
		else if(offset != 0.0f)
			for(unsigned a=0;a<count;++a)
				values[a] += offset;

		// transformed by the origin and delta values
		//   based on http://www.carpetcode.org/hg/carpet/index.cgi/rev/245224d7a5ec
		//   and http://www.openvdb.org/documentation/doxygen/classopenvdb_1_1v3__1__0_1_1math_1_1Transform.html#details
		const std::array<double, 3> origin = attr_getter<std::array<double, 3>>::get(ds, "origin");
		const std::array<double, 3> delta = attr_getter<std::array<double, 3>>::get(ds, "delta");

		int offsets[3];
		for(unsigned a=0;a<3;++a) {
			assert(gridOrigin[a] <= origin[a]);
			offsets[a] = round((origin[a] - gridOrigin[a]) / delta[a]);
		}

		// write the values to the grid
		openvdb::FloatGrid::Accessor accessor = grid->getAccessor();
		float* ptr = values;
		for(hsize_t x = 0; x < dims[0]; ++x)
			for(hsize_t y = 0; y < dims[1]; ++y)
				for(hsize_t z = 0; z < dims[2]; ++z) {
					openvdb::Coord xyz(z+offsets[0], y+offsets[1], x+offsets[2]);
					accessor.setValue(xyz, *(ptr++));
				}
	}

	void forAllDatasets(const std::vector<std::string>& filenames, const boost::regex& datasetRegex, const std::function<void(H5File&, const std::string&)>& fn) {
		for(auto& filename : filenames) {
			H5File file(filename, H5F_ACC_RDONLY );

			for(hsize_t i = 0; i < file.getNumObjs(); ++i) {
				std::string type;
				auto typeId = file.getObjTypeByIdx(i, type);

				if((typeId == H5G_DATASET) && (boost::regex_match(file.getObjnameByIdx(i), datasetRegex)))
					fn(file, file.getObjnameByIdx(i));
			}
		}
	}

	struct CollectionData {
		std::set<std::string> datasets;
		openvdb::FloatGrid::Ptr grid;
	};
}

int main(int argc, char* argv[]) {
	// Declare the supported options.
	po::options_description desc("Allowed options");
	desc.add_options()
		("help", "produce help message")
		("input", po::value<std::vector<std::string>>()->multitoken(), "input hdf5 file(s) - can process arbitrary number of files at the same time")
		("detail", "print out details about each grid, not just names")
		("writevdb", po::value<std::string>(), "write all datasets into an openvdb file")
		("dataset_regex", po::value<std::string>()->default_value(std::string(".*")), "read only datasets matching a regex (optional)")
		("normalize", "normalize the output to be between 0 and 1")
		("offset", po::value<float>()->default_value(0.0f), "offset the data by a given amount")
	;

	// process the options
	po::variables_map vm;
	po::store(po::parse_command_line(argc, argv, desc), vm);
	po::notify(vm);

	if(vm.count("help") || (!vm.count("input"))) {
		cout << desc << "\n";
		return 1;
	}

	// get the regex to match processed things
	const boost::regex datasetRegex(vm["dataset_regex"].as<std::string>());

	if(vm.count("writevdb")) {
		// determine grid collections, which should be merged into a single file, eventually
		std::vector<std::pair<grid_collection, CollectionData>> collections;
		forAllDatasets(vm["input"].as<std::vector<std::string>>(), datasetRegex,
			[&collections](H5File& file, const std::string& datasetName) {
				H5::DataSet ds = file.openDataSet(datasetName);
				if(ds.getDataType().getClass() != H5T_FLOAT)
					throw std::runtime_error("OpenVDB output only supports float grids");

				H5::DataSpace space = ds.getSpace();
				if(space.getSimpleExtentNdims() != 3)
					throw std::runtime_error("OpenVDB output only supports 3D grids");

				const std::array<double, 3> origin = attr_getter<std::array<double, 3>>::get(ds, "origin");
				const std::array<double, 3> delta = attr_getter<std::array<double, 3>>::get(ds, "delta");
				const std::array<int, 3> iorigin = attr_getter<std::array<int, 3>>::get(ds, "iorigin");

				grid_collection col(datasetName, origin, delta, iorigin);

				auto it = collections.end();
				for(auto i = collections.begin(); i != collections.end(); ++i)
					if(i->first.isConsistentWith(col)) {
						it = i;

						i->first = i->first + col;
						i->second.datasets.insert(datasetName);

						break;
					}

				if(it == collections.end())
					collections.push_back(std::make_pair(col, CollectionData{std::set<std::string>{datasetName}}));
			}
		);

		cout << "Data collections:" << endl << endl;
		for(auto& c : collections) {
			cout << c.first << endl;
			for(auto& x : c.second.datasets)
				cout << "  " << x << endl;
		}

		openvdb::initialize();

		// initialise the grids in the collections set
		for(auto& c : collections)
			c.second.grid = makeGrid(c.first.name(), c.first.origin(), c.first.scale());

		// open the output file
		openvdb::io::File out(vm["writevdb"].as<std::string>());

		forAllDatasets(vm["input"].as<std::vector<std::string>>(), datasetRegex,
			[&collections, &out, &vm](H5File& file, const std::string& datasetName) {
				// find the right collection for this grid
				auto it = collections.end();
				for(auto i = collections.begin(); i != collections.end(); ++i)
					if(i->second.datasets.find(datasetName) != i->second.datasets.end()) {
						it = i;
						break;
					}
				assert(it != collections.end());

				cout << "Writing grid " << datasetName << " to " << out.filename() << "..." << endl;
				writevdb(file, datasetName, it->second.grid, it->first.origin(), vm.count("normalize"), vm["offset"].as<float>());
				cout << "done." << endl;
			}
		);

		// make the grid vector
		openvdb::GridPtrVec grids;
		for(auto& c : collections)
			grids.push_back(c.second.grid);

		out.write(grids);
		out.close();
	}
	else {
		for(auto& filename : vm["input"].as<std::vector<std::string>>()) {
			H5File file(filename, H5F_ACC_RDONLY);

			printContent(file, "", vm.count("detail"), datasetRegex);
		}
	}

	return 0;
}

