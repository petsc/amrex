target_compile_definitions(amrex PUBLIC $<BUILD_INTERFACE:AMREX_PARTICLES>)

if (NOT ENABLE_DP_PARTICLES)
   target_compile_definitions(amrex PUBLIC $<BUILD_INTERFACE:AMREX_SINGLE_PRECISION_PARTICLES>)
endif ()

target_include_directories(amrex PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}>)

target_sources( amrex
   PRIVATE
   AMReX_TracerParticles.cpp
   AMReX_LoadBalanceKD.cpp
   AMReX_ParticleMPIUtil.cpp
   AMReX_Particles.H
   AMReX_ParGDB.H
   AMReX_Particle_mod_K.H
   AMReX_TracerParticles.H
   AMReX_NeighborParticles.H
   AMReX_NeighborParticlesI.H
   AMReX_NeighborList.H
   AMReX_Particle.H
   AMReX_ParticleInit.H
   AMReX_ParticleContainerI.H
   AMReX_LoadBalanceKD.H
   AMReX_KDTree_F.H
   AMReX_ParIterI.H
   AMReX_ParticleMPIUtil.H
   AMReX_ParticleUtil.H
   AMReX_ParticleUtil.cpp
   AMReX_StructOfArrays.H
   AMReX_ArrayOfStructs.H
   AMReX_ParticleTile.H
   AMReX_NeighborParticlesCPUImpl.H
   AMReX_NeighborParticlesGPUImpl.H
   AMReX_KDTree_${DIM}d.F90
   AMReX_ParticleBufferMap.H
   AMReX_ParticleBufferMap.cpp
   AMReX_ParticleCommunication.H
   AMReX_ParticleCommunication.cpp
   AMReX_ParticleReduce.H
   AMReX_ParticleMesh.H
   AMReX_ParticleLocator.H
   AMReX_ParticleIO.H
   AMReX_DenseBins.H
   AMReX_ParticleTransformation.H
   )
