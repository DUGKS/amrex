#include <AMReX.H>
#include <AMReX_ParmParse.H>
#include <AMReX_MultiFab.H>
#include <AMReX_Particles.H>

using namespace amrex;

static constexpr int NSR = 0;
static constexpr int NSI = 0;
static constexpr int NAR = 0;
static constexpr int NAI = 2;

void get_position_unit_cell(Real* r, const IntVect& nppc, int i_part)
{
    int nx = nppc[0];
    int ny = nppc[1];
    int nz = nppc[2];
    
    int ix_part = i_part/(ny * nz);
    int iy_part = (i_part % (ny * nz)) % ny;
    int iz_part = (i_part % (ny * nz)) / ny;
    
    r[0] = (0.5+ix_part)/nx;
    r[1] = (0.5+iy_part)/ny;
    r[2] = (0.5+iz_part)/nz;
}

class TestParticleContainer
    : public amrex::ParticleContainer<NSR, NSI, NAR, NAI>
{

public:

    using ParticleTileType = ParticleTile<NSR, NSI, NAR, NAI>;

    
    TestParticleContainer (const amrex::Geometry            & a_geom,
                           const amrex::DistributionMapping & a_dmap,
                           const amrex::BoxArray            & a_ba)
        : amrex::ParticleContainer<NSR, NSI, NAR, NAI>(a_geom, a_dmap, a_ba)
    {}

    void InitParticles (const amrex::IntVect& a_num_particles_per_cell)
    {
        BL_PROFILE("InitParticles");
        const int lev = 0;   
        const Real* dx = Geom(lev).CellSize();
        const Real* plo = Geom(lev).ProbLo();
    
        const int num_ppc = AMREX_D_TERM( a_num_particles_per_cell[0],
                                         *a_num_particles_per_cell[1],
                                         *a_num_particles_per_cell[2]);

        for(MFIter mfi = MakeMFIter(lev); mfi.isValid(); ++mfi)
        {
            const Box& tile_box  = mfi.tilebox();

            Gpu::HostVector<ParticleType> host_particles;
            std::array<Gpu::HostVector<Real>, NAR> host_real;
            std::array<Gpu::HostVector<int>, NAI> host_int;
            for (IntVect iv = tile_box.smallEnd(); iv <= tile_box.bigEnd(); tile_box.next(iv))
            {
                for (int i_part=0; i_part<num_ppc;i_part++) {
                    Real r[3];
                    get_position_unit_cell(r, a_num_particles_per_cell, i_part);
                
                    Real x = plo[0] + (iv[0] + r[0])*dx[0];
                    Real y = plo[1] + (iv[1] + r[1])*dx[1];
                    Real z = plo[2] + (iv[2] + r[2])*dx[2];
                
                    ParticleType p;
                    p.id()  = ParticleType::NextID();
                    p.cpu() = ParallelDescriptor::MyProc();                
                    p.pos(0) = x;
                    p.pos(1) = y;
                    p.pos(2) = z;
                    
                    for (int i = 0; i < NSR; ++i) p.rdata(i) = i;
                    for (int i = 0; i < NSI; ++i) p.idata(i) = i;
                    
                    host_particles.push_back(p);
                    for (int i = 0; i < NAR; ++i)
                        host_real[i].push_back(i);
                    for (int i = 0; i < NAI; ++i)
                        host_int[i].push_back(i);
                }
            }
        
            auto& particles = GetParticles(lev);
            auto& particle_tile = particles[std::make_pair(mfi.index(), mfi.LocalTileIndex())];
            auto old_size = particle_tile.GetArrayOfStructs().size();
            auto new_size = old_size + host_particles.size();
            particle_tile.resize(new_size);
            
            Gpu::copy(Gpu::hostToDevice, host_particles.begin(), host_particles.end(),
                      particle_tile.GetArrayOfStructs().begin() + old_size);        
            
            auto& soa = particle_tile.GetStructOfArrays();
            for (int i = 0; i < NAR; ++i)
            {
                Gpu::copy(Gpu::hostToDevice, host_real[i].begin(), host_real[i].end(),
                          soa.GetRealData(i).begin() + old_size);
            }
            
            for (int i = 0; i < NAI; ++i)
            {
                Gpu::copy(Gpu::hostToDevice, host_int[i].begin(), host_int[i].end(),
                          soa.GetIntData(i).begin() + old_size);
            }
        }
    }

    template <typename F>
    void transformParticles (F&& f)
    {
        BL_PROFILE("transformParticles");

        for (int lev = 0; lev <= finestLevel(); ++lev)
        {
            for(MFIter mfi = MakeMFIter(lev); mfi.isValid(); ++mfi)
            {
                int gid = mfi.index();
                int tid = mfi.LocalTileIndex();
                auto& ptile = ParticlesAt(lev, mfi);

                ParticleTileType ptile_tmp;
                ptile_tmp.resize(ptile.size());

                amrex::transformParticles(ptile_tmp, ptile, f);
                ptile.swap(ptile_tmp);
            }
        }
    }
};

struct Transformer
{
    template <typename DstData, typename SrcData>
    AMREX_GPU_HOST_DEVICE
    void operator() (const DstData& dst, const SrcData& src,
                     int src_i, int dst_i) const noexcept
    {
        for (int j = 0; j < DstData::NAI; ++j)
            dst.m_idata[j][dst_i] = 2*src.m_idata[j][src_i];
    }
};

struct TestParams
{
    IntVect size;
    int max_grid_size;
    int num_ppc;
};

void testReduce();

int main (int argc, char* argv[])
{
    amrex::Initialize(argc,argv);

    amrex::Print() << "Running particle reduction test \n";
    testReduce();

    amrex::Finalize();
}

void get_test_params(TestParams& params, const std::string& prefix)
{
    ParmParse pp(prefix);
    pp.get("size", params.size);
    pp.get("max_grid_size", params.max_grid_size);
    pp.get("num_ppc", params.num_ppc);
}

void testReduce ()
{
    BL_PROFILE("testReduce");
    TestParams params;
    get_test_params(params, "reduce");

    RealBox real_box;
    for (int n = 0; n < BL_SPACEDIM; n++)
    {
        real_box.setLo(n, 0.0);
        real_box.setHi(n, params.size[n]);
    }

    IntVect domain_lo(AMREX_D_DECL(0, 0, 0));
    IntVect domain_hi(AMREX_D_DECL(params.size[0]-1,params.size[1]-1,params.size[2]-1));
    const Box domain(domain_lo, domain_hi);

    int coord = 0;
    int is_per[BL_SPACEDIM];
    for (int i = 0; i < BL_SPACEDIM; i++)
        is_per[i] = 1;
    Geometry geom(domain, &real_box, coord, is_per);
    
    BoxArray ba(domain);
    ba.maxSize(params.max_grid_size);
    DistributionMapping dm(ba);

    TestParticleContainer pc(geom, dm, ba);

    int npc = params.num_ppc;
    IntVect nppc = IntVect(AMREX_D_DECL(npc, npc, npc));

    if (ParallelDescriptor::MyProc() == dm[0])
        amrex::Print() << "About to initialize particles \n";

    pc.InitParticles(nppc);
    
    using PType = typename TestParticleContainer::SuperParticleType;

    auto mx1 = amrex::ReduceMax(pc, [=] AMREX_GPU_HOST_DEVICE (const PType& p) -> int { return p.idata(NSI+1); });

    pc.transformParticles(Transformer());
    
    auto mx2 = amrex::ReduceMax(pc, [=] AMREX_GPU_HOST_DEVICE (const PType& p) -> int { return p.idata(NSI+1); });

    AMREX_ALWAYS_ASSERT(2*mx1 == mx2);    
    
    amrex::Print() << "pass \n";
}