#include "NeighborListParticleContainer.H"

#include "neighbor_list_F.H"

using namespace amrex;

const Real NeighborListParticleContainer::min_r;
const Real NeighborListParticleContainer::cutoff;

NeighborListParticleContainer::NeighborListParticleContainer(const Geometry            & geom,
                                                             const DistributionMapping & dmap,
                                                             const BoxArray            & ba,
                                                             int                         ncells)
    : ParticleContainer<2*BL_SPACEDIM> (geom, dmap, ba),
      num_neighbor_cells(ncells)
{
    BL_PROFILE("NeighborListParticleContainer::NeighborListParticleContainer");

    mask.define(ba, dmap, 2, num_neighbor_cells);
    mask.setVal(-1, num_neighbor_cells);
    for (MFIter mfi = MakeMFIter(lev); mfi.isValid(); ++mfi) {
        const Box& box = mfi.tilebox();
        const int grid_id = mfi.index();
        const int tile_id = mfi.LocalTileIndex();
        mask.setVal(grid_id, box, 0, 1);
        mask.setVal(tile_id, box, 1, 1);
    }

    mask.FillBoundary(geom.periodicity());
}

void NeighborListParticleContainer::InitParticles() {

    BL_PROFILE("NeighborListParticleContainer::InitParticles");

    const Geometry& geom = Geom(lev);
    const Real* dx  = geom.CellSize();
    
    std::mt19937 mt(0451);
    std::uniform_real_distribution<double> dist(-1.0, 1.0);

    for (MFIter mfi = MakeMFIter(lev); mfi.isValid(); ++mfi) {
        const Box& tile_box = mfi.tilebox();
        const RealBox tile_real_box { tile_box, dx, geom.ProbLo() };
        
        const int grid_id = mfi.index();
        const int tile_id = mfi.LocalTileIndex();
        auto& particle_tile = GetParticles(lev)[std::make_pair(grid_id, tile_id)];
        
        const auto& boxlo = tile_box.smallEnd();
        ParticleType p;
        for (IntVect iv = tile_box.smallEnd(); iv <= tile_box.bigEnd(); tile_box.next(iv)) {
            
            p.id() = ParticleType::NextID();
            p.cpu() = ParallelDescriptor::MyProc();
            
            p.pos(0) = tile_real_box.lo(0) + (iv[0]- boxlo[0] + 0.5)*dx[0];
            p.pos(1) = tile_real_box.lo(1) + (iv[1]- boxlo[1] + 0.5)*dx[1];
#if (BL_SPACEDIM == 3)
            p.pos(2) = tile_real_box.lo(2) + (iv[2]- boxlo[2] + 0.5)*dx[2];
#endif
            p.rdata(0) = dist(mt);
            p.rdata(1) = dist(mt);
#if (BL_SPACEDIM == 3)
            p.rdata(2) = dist(mt);
#endif
            
            p.rdata(BL_SPACEDIM)   = 0;
            p.rdata(BL_SPACEDIM+1) = 0;
#if (BL_SPACEDIM == 3)
            p.rdata(BL_SPACEDIM+2) = 0;
#endif
            
            particle_tile.push_back(p);
        }
    }
}

void NeighborListParticleContainer::fillNeighbors() {

    BL_PROFILE("NeighborListParticleContainer::fillNeighbors");

    NeighborCommMap neighbors_to_comm;
    for (MyParIter pti(*this, lev); pti.isValid(); ++pti) {
        const Box& tile_box = pti.tilebox();
        const IntVect& lo = tile_box.smallEnd();
        const IntVect& hi = tile_box.bigEnd();
        
        Box shrink_box = pti.tilebox();
        shrink_box.grow(-num_neighbor_cells);
        
        auto& particles = pti.GetArrayOfStructs();
        for (unsigned i = 0; i < pti.numParticles(); ++i) {
            const ParticleType& p = particles[i];
            const IntVect& iv = Index(p, lev);
            
            // if the particle is more than one cell away from 
            // the tile boundary, it's not anybody's neighbor
            if (shrink_box.contains(iv)) continue;
            
            // shift stores whether we are near the tile boundary in each direction.
            // -1 means lo, 1 means hi, 0 means not near the boundary
            IntVect shift = IntVect::TheZeroVector();
            for (int idim = 0; idim < BL_SPACEDIM; ++idim) {
                if (iv[idim] == lo[idim])
                    shift[idim] = -num_neighbor_cells;
                else if (iv[idim] == hi[idim])
                    shift[idim] = num_neighbor_cells;
            }
            
            // Based on the value of shift, we add the particle to a map to be sent
            // to the neighbors. A particle can be sent to up to 3 neighbors in 2D
            // and up to 7 in 3D, depending on whether is near the tile box corners,
            // edges, or just the faces. First, add the particle for the "face" neighbors
            for (int idim = 0; idim < BL_SPACEDIM; ++idim) {
                if (shift[idim] == 0) continue;
                IntVect neighbor_cell = iv;
                neighbor_cell.shift(idim, shift[idim]);
                BL_ASSERT(mask[pti].box().contains(neighbor_cell));
                packNeighborParticle(neighbor_cell, mask[pti], p, neighbors_to_comm);
            }
            
            // Now add the particle to the "edge" neighbors
            for (int idim = 0; idim < BL_SPACEDIM; ++idim) {
                for (int jdim = 0; jdim < idim; ++jdim) {
                    if (shift[idim] != 0 and shift[jdim] != 0) {
                        IntVect neighbor_cell = iv;
                        neighbor_cell.shift(idim, shift[idim]);
                        neighbor_cell.shift(jdim, shift[jdim]);
                        BL_ASSERT(mask[pti].box().contains(neighbor_cell));
                        packNeighborParticle(neighbor_cell, mask[pti], p, neighbors_to_comm);
                    }
                }
            }
            
#if (BL_SPACEDIM == 3)
            // Finally, add the particle for the "vertex" neighbors (only relevant in 3D)
            if (shift[0] != 0 and shift[1] != 0 and shift[2] != 0) {
                IntVect neighbor_cell = iv;
                neighbor_cell.shift(shift);
                BL_ASSERT(mask[pti].box().contains(neighbor_cell));
                packNeighborParticle(neighbor_cell, mask[pti], p, neighbors_to_comm);
            }
#endif
        }
    }
    
    fillNeighborsMPI(neighbors_to_comm);
}

void NeighborListParticleContainer::clearNeighbors() 
{

    BL_PROFILE("NeighborListParticleContainer::clearNeighbors");

    neighbors.clear();
}

void NeighborListParticleContainer::computeForces() {

    BL_PROFILE("NeighborListParticleContainer::computeForces");

#ifdef _OPENMP
#pragma omp parallel
#endif
    for (MyParIter pti(*this, lev); pti.isValid(); ++pti) {
        AoS& particles = pti.GetArrayOfStructs();
        int Np = particles.size();
        PairIndex index(pti.index(), pti.LocalTileIndex());
        int Nn = neighbors[index].size() / pdata_size;
        amrex_compute_forces(particles.data(), &Np, 
                             neighbors[index].dataPtr(), &Nn, 
                             &cutoff, &min_r);
    }
}

void NeighborListParticleContainer::computeForcesNL() {

    BL_PROFILE("NeighborListParticleContainer::computeForces");

    buildNeighborList();

#ifdef _OPENMP
#pragma omp parallel
#endif
    for (MyParIter pti(*this, lev); pti.isValid(); ++pti) {
        AoS& particles = pti.GetArrayOfStructs();
        int Np = particles.size();
        PairIndex index(pti.index(), pti.LocalTileIndex());
        int Nn = neighbors[index].size() / pdata_size;
        int size = neighbor_list[index].size();
        amrex_compute_forces_nl(particles.data(), &Np, 
                                neighbors[index].dataPtr(), &Nn,
                                neighbor_list[index].dataPtr(), &size, 
                                &cutoff, &min_r);
    }
}

void NeighborListParticleContainer::moveParticles(const Real dt) {

    BL_PROFILE("NeighborListParticleContainer::moveParticles");

    const RealBox& prob_domain = Geom(lev).ProbDomain();

#ifdef _OPENMP
#pragma omp parallel
#endif
    for (MyParIter pti(*this, lev); pti.isValid(); ++pti) {
        AoS& particles = pti.GetArrayOfStructs();
        int Np = particles.size();
        amrex_move_particles(particles.data(), &Np, &dt,
                             prob_domain.lo(), prob_domain.hi());
    }
}

void NeighborListParticleContainer::writeParticles(int n) {

    BL_PROFILE("NeighborListParticleContainer::writeParticles");

    const std::string& pltfile = amrex::Concatenate("particles", n, 5);
    WriteAsciiFile(pltfile);
}

void NeighborListParticleContainer::applyPeriodicShift(ParticleType& p,
                                                     const IntVect& neighbor_cell) {

    BL_PROFILE("NeighborListParticleContainer::applyPeriodicShift");

    const Periodicity& periodicity = Geom(lev).periodicity();
    if (not periodicity.isAnyPeriodic()) return;

    const Box& domain = Geom(lev).Domain();
    const IntVect& lo = domain.smallEnd();
    const IntVect& hi = domain.bigEnd();
    const RealBox& prob_domain = Geom(lev).ProbDomain();

    for (int dir = 0; dir < BL_SPACEDIM; ++dir) {
        if (not periodicity.isPeriodic(dir)) continue;
        if (neighbor_cell[dir] < lo[dir]) {
            p.pos(dir) += prob_domain.length(dir);
        } 
        else if (neighbor_cell[dir] > hi[dir]) {
            p.pos(dir) -= prob_domain.length(dir);
        }
    }
}

void NeighborListParticleContainer::packNeighborParticle(const IntVect& neighbor_cell,
                                                       const BaseFab<int>& mask,
                                                       const ParticleType& p,
                                                       NeighborCommMap& neighbors_to_comm) {

    BL_PROFILE("NeighborListParticleContainer::packNeighborParticle");

    const int neighbor_grid = mask(neighbor_cell, 0);
    if (neighbor_grid >= 0) {
        const int who = ParticleDistributionMap(lev)[neighbor_grid];
        const int MyProc = ParallelDescriptor::MyProc();
        const int neighbor_tile = mask(neighbor_cell, 1);
        PairIndex dst_index(neighbor_grid, neighbor_tile);
        ParticleType particle = p;
        applyPeriodicShift(particle, neighbor_cell);
        if (who == MyProc) {
            size_t old_size = neighbors[dst_index].size();
            size_t new_size = neighbors[dst_index].size() + pdata_size;
            neighbors[dst_index].resize(new_size);
            std::memcpy(&neighbors[dst_index][old_size], &particle, pdata_size);
        } else {
            NeighborCommTag tag(who, neighbor_grid, neighbor_tile);
            Array<char>& buffer = neighbors_to_comm[tag];
            size_t old_size = buffer.size();
            size_t new_size = buffer.size() + pdata_size;
            buffer.resize(new_size);
            std::memcpy(&buffer[old_size], &particle, pdata_size);
        }
    }
}

void NeighborListParticleContainer::fillNeighborsMPI(NeighborCommMap& neighbors_to_comm) {

    BL_PROFILE("NeighborListParticleContainer::fillNeighborsMPI");

#ifdef BL_USE_MPI
    const int MyProc = ParallelDescriptor::MyProc();
    const int NProcs = ParallelDescriptor::NProcs();
    
    // count the number of tiles to be sent to each proc
    std::map<int, int> tile_counts;
    for (const auto& kv: neighbors_to_comm) {
        tile_counts[kv.first.proc_id] += 1;
    }
    
    // flatten all the data for each proc into a single buffer
    // once this is done, each dst proc will have an Array<char>
    // the buffer will be packed like:
    // ntiles, gid1, tid1, size1, data1....  gid2, tid2, size2, data2... etc. 
    std::map<int, Array<char> > send_data;
    for (const auto& kv: neighbors_to_comm) {
        Array<char>& buffer = send_data[kv.first.proc_id];
        buffer.resize(sizeof(int));
        std::memcpy(&buffer[0], &tile_counts[kv.first.proc_id], sizeof(int));
    }
    
    for (auto& kv : neighbors_to_comm) {
        int data_size = kv.second.size();
        Array<char>& buffer = send_data[kv.first.proc_id];
        size_t old_size = buffer.size();
        size_t new_size = buffer.size() + 2*sizeof(int) + sizeof(int) + data_size;
        buffer.resize(new_size);
        char* dst = &buffer[old_size];
        std::memcpy(dst, &(kv.first.grid_id), sizeof(int)); dst += sizeof(int);
        std::memcpy(dst, &(kv.first.tile_id), sizeof(int)); dst += sizeof(int);
        std::memcpy(dst, &data_size,          sizeof(int)); dst += sizeof(int);
        if (data_size == 0) continue;
        std::memcpy(dst, &kv.second[0], data_size);
        Array<char>().swap(kv.second);
    }
    
    // each proc figures out how many bytes it will send, and how
    // many it will receive
    Array<long> snds(NProcs, 0), rcvs(NProcs, 0);
    long num_snds = 0;
    for (const auto& kv : send_data) {
        num_snds      += kv.second.size();
        snds[kv.first] = kv.second.size();
    }
    ParallelDescriptor::ReduceLongMax(num_snds);
    if (num_snds == 0) return;
    
    // communicate that information
    BL_COMM_PROFILE(BLProfiler::Alltoall, sizeof(long),
                    ParallelDescriptor::MyProc(), BLProfiler::BeforeCall());
    
    BL_MPI_REQUIRE( MPI_Alltoall(snds.dataPtr(),
                                 1,
                                 ParallelDescriptor::Mpi_typemap<long>::type(),
                                 rcvs.dataPtr(),
                                 1,
                                 ParallelDescriptor::Mpi_typemap<long>::type(),
                                 ParallelDescriptor::Communicator()) );
    BL_ASSERT(rcvs[MyProc] == 0);
    
    BL_COMM_PROFILE(BLProfiler::Alltoall, sizeof(long),
                    ParallelDescriptor::MyProc(), BLProfiler::AfterCall());
    
    Array<int> RcvProc;
    Array<std::size_t> rOffset; // Offset (in bytes) in the receive buffer
    
    std::size_t TotRcvBytes = 0;
    for (int i = 0; i < NProcs; ++i) {
        if (rcvs[i] > 0) {
            RcvProc.push_back(i);
            rOffset.push_back(TotRcvBytes);
            TotRcvBytes += rcvs[i];
        }
    }
    
    const int nrcvs = RcvProc.size();
    Array<MPI_Status>  stats(nrcvs);
    Array<MPI_Request> rreqs(nrcvs);
    
    const int SeqNum = ParallelDescriptor::SeqNum();
    
    // Allocate data for rcvs as one big chunk.
    Array<char> recvdata(TotRcvBytes);
    
    // Post receives.
    for (int i = 0; i < nrcvs; ++i) {
        const auto Who    = RcvProc[i];
        const auto offset = rOffset[i];
        const auto Cnt    = rcvs[Who];
        
        BL_ASSERT(Cnt > 0);
        BL_ASSERT(Cnt < std::numeric_limits<int>::max());
        BL_ASSERT(Who >= 0 && Who < NProcs);
        
        rreqs[i] = ParallelDescriptor::Arecv(&recvdata[offset], Cnt, Who, SeqNum).req();
    }
    
    // Send.
    for (const auto& kv : send_data) {
        const auto Who = kv.first;
        const auto Cnt = kv.second.size();
        
        BL_ASSERT(Cnt > 0);
        BL_ASSERT(Who >= 0 && Who < NProcs);
        BL_ASSERT(Cnt < std::numeric_limits<int>::max());
        
        ParallelDescriptor::Send(kv.second.data(), Cnt, Who, SeqNum);
    }
    
    // unpack the received data and put them into the proper neighbor buffers
    if (nrcvs > 0) {
        BL_MPI_REQUIRE( MPI_Waitall(nrcvs, rreqs.data(), stats.data()) );
        for (int i = 0; i < nrcvs; ++i) {
            const int offset = rOffset[i];
            char* buffer = &recvdata[offset];
            int num_tiles, gid, tid, size;
            std::memcpy(&num_tiles, buffer, sizeof(int)); buffer += sizeof(int);
            for (int j = 0; j < num_tiles; ++j) {
                std::memcpy(&gid,  buffer, sizeof(int)); buffer += sizeof(int);
                std::memcpy(&tid,  buffer, sizeof(int)); buffer += sizeof(int);
                std::memcpy(&size, buffer, sizeof(int)); buffer += sizeof(int);
                
                if (size == 0) continue;
                
                PairIndex dst_index(gid, tid);
                size_t old_size = neighbors[dst_index].size();
                size_t new_size = neighbors[dst_index].size() + size;
                neighbors[dst_index].resize(new_size);
                std::memcpy(&neighbors[dst_index][old_size], buffer, size); buffer += size;
            }
        }
    }
#endif
}

void NeighborListParticleContainer::buildNeighborList() {
    
    BL_PROFILE("NeighborListParticleContainer::buildNeighborList");
    
#ifdef _OPENMP
#pragma omp parallel
#endif    
    for (MyParIter pti(*this, lev); pti.isValid(); ++pti) {

        PairIndex index(pti.index(), pti.LocalTileIndex());
        neighbor_list[index].clear();

        // For each cell on this tile, we build linked lists storing the
        // indices of the particles belonging to it. We have one list for
        // real particles, another for those from neighboring tiles.
        Box box = pti.tilebox();
        box.grow(num_neighbor_cells);
        BaseFab<int> head(box);
        head.setVal(-1);
        BaseFab<int> nhead(box);
        nhead.setVal(-1);

        // first the real particles
        AoS& particles = pti.GetArrayOfStructs();
        int Np = particles.size();
        Array<int> list(Np, -1);
        for (unsigned i = 0; i < pti.numParticles(); ++i) {
            const ParticleType& p = particles[i];
            const IntVect& cell = Index(p, lev);
            list[i] = head(cell);
            head(cell) = i;
        }
        
        // then the neighbors
        int Nn = neighbors[index].size() / pdata_size;
        Array<int> nlist(Nn, -1);
        char* nbuf = neighbors[index].dataPtr();
        for (int i = 0; i < Nn; ++i) {
            ParticleType n;
            std::memcpy(&n, nbuf, pdata_size);
            nbuf += pdata_size;
            const IntVect& cell = Index(n, lev);
            nlist[i] = nhead(cell);
            nhead(cell) = i;
        }

        // using these lists, we build a neighbor list containing both 
        // kinds of particles.
        int p_start_index = 0;
        for (unsigned i = 0; i < pti.numParticles(); ++i) {
            const ParticleType& p = particles[i];

            int num_neighbors = 0;
            int num_ghost_neighbors = 0;
            neighbor_list[index].push_back(0);
            neighbor_list[index].push_back(0);

            const IntVect& cell = Index(p, lev);
            Box bx(cell, cell);
            bx.grow(num_neighbor_cells);

            // first the real particles...
            for (IntVect iv = bx.smallEnd(); iv <= bx.bigEnd(); bx.next(iv)) {
                int j = head(iv);
                while (j >= 0) {
                    if (i == j) {
                        j = list[j];
                        continue;
                    }
                    if ( check_pair(p, particles[j]) ) {
                        neighbor_list[index].push_back(j+1);
                        num_neighbors += 1;
                    }
                    j = list[j];
                }
            }

            // then the neighbors.
            for (IntVect iv = bx.smallEnd(); iv <= bx.bigEnd(); bx.next(iv)) {
                int j = nhead(iv);
                while (j >= 0) {
                    if (i == j) {
                        j = nlist[j];
                        continue;
                    }
                    ParticleType n;
                    char* nbuf = neighbors[index].dataPtr() + j*pdata_size;
                    std::memcpy(&n, nbuf, pdata_size);
                    if ( check_pair(p, n) ) {
                        neighbor_list[index].push_back(j+1);
                        num_ghost_neighbors += 1;
                    }
                    j = nlist[j];
                }
            }
            
            neighbor_list[index][p_start_index]   = num_neighbors;
            neighbor_list[index][p_start_index+1] = num_ghost_neighbors;
            p_start_index += num_neighbors + num_ghost_neighbors + 2;
        }
    }
}
