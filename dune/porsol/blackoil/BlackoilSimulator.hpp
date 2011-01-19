/*
  Copyright 2011 SINTEF ICT, Applied Mathematics.

  This file is part of the Open Porous Media project (OPM).

  OPM is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  OPM is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with OPM.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef OPM_BLACKOILSIMULATOR_HEADER_INCLUDED
#define OPM_BLACKOILSIMULATOR_HEADER_INCLUDED




#include <dune/grid/io/file/vtk/vtkwriter.hh>
#include <dune/common/Units.hpp>
#include <dune/common/EclipseGridParser.hpp>
#include <dune/common/param/ParameterGroup.hpp>
#include <dune/porsol/common/BoundaryConditions.hpp>
#include <boost/filesystem/convenience.hpp>
#include <boost/lexical_cast.hpp>
#include <iostream>
#include <string>
#include <vector>


namespace Opm
{

    template<class Grid, class Rock, class Fluid, class Wells, class FlowSolver, class TransportSolver>
    class BlackoilSimulator
    {
    public:
        void init(const Dune::parameter::ParameterGroup& param);
        void simulate();

    private:
        Grid grid_;
        Rock rock_;
        Fluid fluid_;
        Wells wells_;
        FlowSolver flow_solver_;
        TransportSolver transport_solver_;
        double total_time_;
        double initial_stepsize_;
        bool do_impes_;
        std::string output_dir_;
        bool gravity_test_;
        bool newcode_;

        static void output(const Grid& grid,
                           const std::vector<typename Fluid::PhaseVec>& cell_pressure,
                           const std::vector<typename Fluid::CompVec>& z,
                           const std::vector<double>& face_flux,
                           const int step,
                           const std::string& filebase);

    };




    // Method implementations below.


template<class Grid, class Rock, class Fluid, class Wells, class FlowSolver, class TransportSolver>
void
BlackoilSimulator<Grid, Rock, Fluid, Wells, FlowSolver, TransportSolver>::
init(const Dune::parameter::ParameterGroup& param)
{
    using namespace Dune;
    std::string fileformat = param.getDefault<std::string>("fileformat", "cartesian");
    if (fileformat == "eclipse") {
        Dune::EclipseGridParser parser(param.get<std::string>("filename"));
        double z_tolerance = param.getDefault<double>("z_tolerance", 0.0);
        bool periodic_extension = param.getDefault<bool>("periodic_extension", false);
        bool turn_normals = param.getDefault<bool>("turn_normals", false);
        grid_.processEclipseFormat(parser, z_tolerance, periodic_extension, turn_normals);
        double perm_threshold_md = param.getDefault("perm_threshold_md", 0.0);
        double perm_threshold = Dune::unit::convert::from(perm_threshold_md, Dune::prefix::milli*Dune::unit::darcy);
        rock_.init(parser, grid_.globalCell(), perm_threshold);
        fluid_.init(parser);
        wells_.init(parser);
    } else if (fileformat == "cartesian") {
        Dune::array<int, 3> dims = {{ param.getDefault<int>("nx", 1),
                                      param.getDefault<int>("ny", 1),
                                      param.getDefault<int>("nz", 1) }};
        Dune::array<double, 3> cellsz = {{ param.getDefault<double>("dx", 1.0),
                                           param.getDefault<double>("dy", 1.0),
                                           param.getDefault<double>("dz", 1.0) }};
        grid_.createCartesian(dims, cellsz);
        double default_poro = param.getDefault("default_poro", 1.0);
        double default_perm_md = param.getDefault("default_perm_md", 100.0);
        double default_perm = unit::convert::from(default_perm_md, prefix::milli*unit::darcy);
        MESSAGE("Warning: For generated cartesian grids, we use uniform rock properties.");
        rock_.init(grid_.size(0), default_poro, default_perm);
        EclipseGridParser parser(param.get<std::string>("filename")); // Need a parser for the fluids anyway.
        fluid_.init(parser);
        wells_.init(parser);
    } else {
        THROW("Unknown file format string: " << fileformat);
    }
    flow_solver_.init(param);
    transport_solver_.init(param);
    total_time_ = param.getDefault("total_time", 30*unit::day);
    initial_stepsize_ = param.getDefault("initial_stepsize", 1.0*unit::day);
    do_impes_ = param.getDefault("do_impes", false);
    output_dir_ = param.getDefault<std::string>("output_dir", "output");
    gravity_test_ = param.getDefault("gravity_test", false);
    newcode_ = param.getDefault("newcode", true);
}






template<class Grid, class Rock, class Fluid, class Wells, class FlowSolver, class TransportSolver>
void
BlackoilSimulator<Grid, Rock, Fluid, Wells, FlowSolver, TransportSolver>::
simulate()
{
    // Boundary conditions.
    typedef Dune::FlowBC BC;
    typedef Dune::BasicBoundaryConditions<true, false>  FBC;
    FBC flow_bc(7);
    if (!gravity_test_) {
        flow_bc.flowCond(1) = BC(BC::Dirichlet, 300.0*Dune::unit::barsa);
        flow_bc.flowCond(2) = BC(BC::Dirichlet, 100.0*Dune::unit::barsa); // WELLS
    }

    // Gravity.
    typename Grid::Vector gravity(0.0);
    if (gravity_test_) {
        // gravity[2] = Dune::unit::gravity;
        gravity[2] = 10;
    }

    // Flow solver setup.
    flow_solver_.setup(grid_, rock_, fluid_, wells_, gravity, flow_bc);

    // Transport solver setup.
    transport_solver_.setup(grid_, rock_, fluid_, wells_, flow_solver_.faceTransmissibilities(), gravity);

    // Source terms.
    std::vector<double> src(grid_.numCells(), 0.0);
//     if (g.numberOfCells() > 1) {
//         src[0]     = 1.0;
//         src.back() = -1.0;
//     }

    // Initial state.
    typedef typename Fluid::CompVec CompVec;
    typedef typename Fluid::PhaseVec PhaseVec;
    CompVec init_z(0.0);
    if (gravity_test_) {
        init_z[Fluid::Oil] = 0.5;
        init_z[Fluid::Water] = 0.5;
    } else {
        init_z[Fluid::Oil] = 1.0;
    }
    CompVec bdy_z = flow_solver_.inflowMixture();
    if (gravity_test_) {
        bdy_z = -1e100;
    }
    std::vector<CompVec> cell_z(grid_.numCells(), init_z);
    MESSAGE("******* Assuming zero capillary pressures *******");
    PhaseVec init_p(100.0*Dune::unit::barsa);
    std::vector<PhaseVec> cell_pressure(grid_.numCells(), init_p);
    if (gravity_test_) {
        double ref_gravpot = grid_.cellCentroid(0)*gravity;
        double rho = init_z*fluid_.surfaceDensities();  // Assuming incompressible, and constant initial z.
        for (int cell = 1; cell < grid_.numCells(); ++cell) {
            double press = rho*(grid_.cellCentroid(cell)*gravity - ref_gravpot) + cell_pressure[0][0];
            cell_pressure[cell] = PhaseVec(press);
        }
    }
    PhaseVec bdy_p(300.0*Dune::unit::barsa);
    // PhaseVec bdy_p(100.0*Dune::unit::barsa); // WELLS
    // Rescale z values so that pore volume is filled exactly
    // (to get zero initial volume discrepancy).
    for (int cell = 0; cell < grid_.numCells(); ++cell) {
        double pore_vol = grid_.cellVolume(cell)*rock_.porosity(cell);
        typename Fluid::FluidState state = fluid_.computeState(cell_pressure[cell], cell_z[cell]);
        double fluid_vol = state.total_phase_volume_;
        cell_z[cell] *= pore_vol/fluid_vol;
    }
    int num_faces = grid_.numFaces();
    std::vector<PhaseVec> face_pressure(num_faces);
    for (int face = 0; face < num_faces; ++face) {
        int bid = grid_.boundaryId(face);
        if (flow_bc.flowCond(bid).isDirichlet()) {
            face_pressure[face] = flow_bc.flowCond(bid).pressure();
        } else {
            int c[2] = { grid_.faceCell(face, 0), grid_.faceCell(face, 1) };
            face_pressure[face] = 0.0;
            int num = 0;
            for (int j = 0; j < 2; ++j) {
                if (c[j] >= 0) {
                    face_pressure[face] += cell_pressure[c[j]];
                    ++num;
                }
            }
            face_pressure[face] /= double(num);
        }
    }
    double voldisclimit = flow_solver_.volumeDiscrepancyLimit();
    double stepsize = initial_stepsize_;
    double current_time = 0.0;
    int step = -1;
    std::vector<double> face_flux;
    std::vector<double> well_pressure;
    std::vector<double> well_flux;
    std::vector<PhaseVec> cell_pressure_start;
    std::vector<PhaseVec> face_pressure_start;
    std::vector<CompVec> cell_z_start;
    while (current_time < total_time_) {
        cell_pressure_start = cell_pressure;
        face_pressure_start = face_pressure;
        cell_z_start = cell_z;

        // Do not run past total_time_.
        if (current_time + stepsize > total_time_) {
            stepsize = total_time_ - current_time;
        }
        ++step;
        std::cout << "\n\n================    Simulation step number " << step
                  << "    ==============="
                  << "\n      Current time (days)     " << Dune::unit::convert::to(current_time, Dune::unit::day)
                  << "\n      Current stepsize (days) " << Dune::unit::convert::to(stepsize, Dune::unit::day)
                  << "\n      Total time (days)       " << Dune::unit::convert::to(total_time_, Dune::unit::day)
                  << "\n" << std::endl;

        // Solve flow system.
        enum FlowSolver::ReturnCode result
            = flow_solver_.solve(cell_pressure, face_pressure, cell_z, face_flux, well_pressure, well_flux, src, stepsize, do_impes_);

        // Check if the flow solver succeeded.
        if (result != FlowSolver::SolveOk) {
            THROW("Flow solver refused to run due to too large volume discrepancy.");
        }

        // Update wells with new perforation pressures and fluxes.
        wells_.update(grid_.numCells(), well_pressure, well_flux);






        if (gravity_test_) {
            std::fill(face_flux.begin(), face_flux.end(), 0.0);
        }





        // Transport and check volume discrepancy.
        bool voldisc_ok = true;
        if (!do_impes_) {
            double actual_computed_time
                = transport_solver_.transport(bdy_p, bdy_z,
                                             face_flux, cell_pressure, face_pressure,
                                             stepsize, voldisclimit, cell_z, newcode_);
            voldisc_ok = (actual_computed_time == stepsize);
        } else {
            voldisc_ok = flow_solver_.volumeDiscrepancyAcceptable(cell_pressure, face_pressure, cell_z, stepsize);
        }

        // If discrepancy too large, redo entire pressure step.
        if (!voldisc_ok) {
            std::cout << "********* Shortening (pressure) stepsize, redoing step number " << step <<" **********" << std::endl;
            stepsize *= 0.5;
            --step;
            cell_pressure = cell_pressure_start;
            face_pressure = face_pressure_start;
            cell_z = cell_z_start;
            continue;
        }

        // Output.
        std::string output_name = output_dir_ + "/" + "blackoil-output";
        output(grid_, cell_pressure, cell_z, face_flux, step, output_name);

        // Adjust time.
        current_time += stepsize;
        if (voldisc_ok) {
            // stepsize *= 1.5;
        }
    }
}






template<class Grid, class Rock, class Fluid, class Wells, class FlowSolver, class TransportSolver>
void
BlackoilSimulator<Grid, Rock, Fluid, Wells, FlowSolver, TransportSolver>::
output(const Grid& grid,
       const std::vector<typename Fluid::PhaseVec>& cell_pressure,
       const std::vector<typename Fluid::CompVec>& z,
       const std::vector<double>& face_flux,
       const int step,
       const std::string& filebase)
{
    // Ensure directory exists.
    boost::filesystem::path fpath(filebase);
    if (fpath.has_branch_path()) {
        create_directories(fpath.branch_path());
    }

    // Output to VTK.
    std::vector<typename Grid::Vector> cell_velocity;
    estimateCellVelocitySimpleInterface(cell_velocity, grid, face_flux);
    // Dune's vtk writer wants multi-component data to be flattened.
    std::vector<double> cell_pressure_flat(&*cell_pressure.front().begin(),
                                           &*cell_pressure.back().end());
    std::vector<double> cell_velocity_flat(&*cell_velocity.front().begin(),
                                           &*cell_velocity.back().end());
    std::vector<double> z_flat(&*z.front().begin(),
                               &*z.back().end());
    Dune::VTKWriter<typename Grid::LeafGridView> vtkwriter(grid.leafView());
    vtkwriter.addCellData(cell_pressure_flat, "pressure", Fluid::numPhases);
    vtkwriter.addCellData(cell_velocity_flat, "velocity", Grid::dimension);
    vtkwriter.addCellData(z_flat, "z", Fluid::numComponents);
    vtkwriter.write(filebase + '-' + boost::lexical_cast<std::string>(step),
                    Dune::VTKOptions::ascii);

    // Dump data for Matlab.
    std::vector<double> zv[Fluid::numComponents];
    for (int comp = 0; comp < Fluid::numComponents; ++comp) {
        zv[comp].resize(grid.numCells());
        for (int cell = 0; cell < grid.numCells(); ++cell) {
            zv[comp][cell] = z[cell][comp];
        }
    }
    std::string matlabdumpname(filebase + "-");
    matlabdumpname += boost::lexical_cast<std::string>(step);
    matlabdumpname += ".dat";
    std::ofstream dump(matlabdumpname.c_str());
    dump.precision(15);
    int num_cells = cell_pressure.size();
    std::vector<double> liq_press(num_cells);
    for (int cell = 0; cell < num_cells; ++cell) {
        liq_press[cell] = cell_pressure[cell][Fluid::Liquid];
    }
    std::copy(liq_press.begin(), liq_press.end(),
              std::ostream_iterator<double>(dump, " "));
    dump << '\n';
    for (int comp = 0; comp < Fluid::numComponents; ++comp) {
        std::copy(zv[comp].begin(), zv[comp].end(),
                  std::ostream_iterator<double>(dump, " "));
        dump << '\n';
    }
}





} // namespace Opm





#endif // OPM_BLACKOILSIMULATOR_HEADER_INCLUDED
