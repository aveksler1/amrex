#include <Particles.H>
#include <ParmParse.H>
#include <limits>

bool
ParticleBase::CrseToFine (const BoxArray& cfba,
                          const IntVect*  cells,
                          IntVect*        cfshifts,
                          const Geometry& gm,
                          bool*           which,
                          Array<IntVect>& pshifts)
{
    //
    // We're in AssignDensity(). We want to know whether or not updating
    // with a particle, will we cross a  crse->fine boundary of the level
    // with coarsened fine BoxArray "cfba".  "cells" are as calculated from
    // CIC_Cells_Fracs().
    //
    const int M = D_TERM(2,+2,+4);

    for (int i = 0; i < M; i++)
        which[i] =  false;

    bool result = false;

    for (int i = 0; i < M; i++)
    {
        if (cfba.contains(cells[i]))
        {
            result      = true;
            which[i]    = true;
            cfshifts[i] = IntVect::TheZeroVector();
        }
        else if (!gm.Domain().contains(cells[i]))
        {
            BL_ASSERT(gm.isAnyPeriodic());
            //
            // Can the cell be shifted into cfba?
            //
            const Box bx(cells[i],cells[i]);

            gm.periodicShift(bx, gm.Domain(), pshifts);

            if (!pshifts.empty())
            {
                BL_ASSERT(pshifts.size() == 1);

                const Box dbx = bx - pshifts[0];

                BL_ASSERT(dbx.ok());

                if (cfba.contains(dbx))
                {
                    //
                    // Note that pshifts[0] is from the coarse perspective.
                    // We'll later need to multiply it by ref ratio to use
                    // at the fine level.
                    //
                    result      = true;
                    which[i]    = true;
                    cfshifts[i] = pshifts[0];
                }
            }
        }
    }

    return result;
}

bool
ParticleBase::FineToCrse (const ParticleBase&                p,
                          int                                flev,
                          const Amr*                         amr,
                          const IntVect*                     fcells,
                          const BoxArray&                    fvalid,
                          const BoxArray&                    compfvalid_grown,
                          IntVect*                           ccells,
                          Real*                              cfracs,
                          bool*                              which,
                          int*                               cgrid,
                          Array<IntVect>&                    pshifts,
                          std::vector< std::pair<int,Box> >& isects)
{
    BL_ASSERT(amr != 0);
    BL_ASSERT(flev > 0);
    //
    // We're in AssignDensity(). We want to know whether or not updating
    // with a particle we'll cross a fine->crse boundary.  Note that crossing
    // a periodic boundary, where the periodic shift lies in our valid region,
    // is not considered a Fine->Crse crossing.
    //
    const int M = D_TERM(2,+2,+4);

    for (int i = 0; i < M; i++)
    {
        cgrid[i] = -1;
        which[i] = false;
    }

    const Box ibx = BoxLib::grow(amr->boxArray(flev)[p.m_grid],-1);

    BL_ASSERT(ibx.ok());

    if (ibx.contains(p.m_cell))
        //
        // We're strictly contained in our valid box.
        // We can't cross a fine->crse boundary.
        //
        return false;

    if (!compfvalid_grown.contains(p.m_cell))
        //
        // We're strictly contained in our "valid" region. Note that the valid
        // region contains any periodically shifted ghost cells that intersect
        // valid region.
        //
        return false;
    //
    // Otherwise ...
    //
    const Geometry& cgm = amr->Geom(flev-1);
    const IntVect   rr  = amr->refRatio(flev-1);
    const BoxArray& cba = amr->boxArray(flev-1);

    ParticleBase::CIC_Cells_Fracs(p, cgm.ProbLo(), cgm.CellSize(), cfracs, ccells);

    bool result = false;

    for (int i = 0; i < M; i++)
    {
        IntVect ccell_refined = ccells[i]*rr;
        //
        // We've got to protect against the case when we're at the low
        // end of the domain because coarsening & refining don't work right
        // when indices go negative.
        //
        for (int dm = 0; dm < BL_SPACEDIM; dm++)
            ccell_refined[dm] = std::max(ccell_refined[dm], -1);

        if (!fvalid.contains(ccell_refined))
        {
            result   = true;
            which[i] = true;

            Box cbx(ccells[i],ccells[i]);

            if (!cgm.Domain().contains(ccells[i]))
            {
                //
                // We must be at a periodic boundary.
                // Find valid box into which we can be periodically shifted.
                //
                BL_ASSERT(cgm.isAnyPeriodic());

                cgm.periodicShift(cbx, cgm.Domain(), pshifts);

                BL_ASSERT(pshifts.size() == 1);

                cbx -= pshifts[0];

                ccells[i] -= pshifts[0];

                BL_ASSERT(cbx.ok());
                BL_ASSERT(cgm.Domain().contains(cbx));
            }
            //
            // Which grid at the crse level do we need to update?
            //
            isects = cba.intersections(cbx);

            BL_ASSERT(!isects.empty());
            BL_ASSERT(isects.size() == 1);

            cgrid[i] = isects[0].first;  // The grid ID at crse level that we hit.
        }
    }

    return result;
}

void
ParticleBase::FineCellsToUpdateFromCrse (const ParticleBase&                p,
                                         int                                lev,
                                         const Amr*                         amr,
                                         const IntVect&                     ccell,
                                         const IntVect&                     cshift,
                                         Array<int>&                        fgrid,
                                         Array<Real>&                       ffrac,
                                         Array<IntVect>&                    fcells,
                                         std::vector< std::pair<int,Box> >& isects)
{
    BL_ASSERT(lev >= 0);
    BL_ASSERT(lev < amr->finestLevel());

    const Box       fbx = BoxLib::refine(Box(ccell,ccell),amr->refRatio(lev));
    const BoxArray& fba = amr->boxArray(lev+1);
    const Real*     plo = amr->Geom(lev).ProbLo();
    const Real*     dx  = amr->Geom(lev).CellSize();
    const Real*     fdx = amr->Geom(lev+1).CellSize();

    if (cshift == IntVect::TheZeroVector())
    {
        BL_ASSERT(fba.contains(fbx));
    }
    fgrid.clear();
    ffrac.clear();
    fcells.clear();
    //
    // Which fine cells does particle "p" (that wants to update "ccell") do we
    // touch at the finer level?
    //
    for (IntVect iv = fbx.smallEnd(); iv <= fbx.bigEnd(); fbx.next(iv))
    {
        bool touches = true;

        for (int k = 0; k < BL_SPACEDIM; k++)
        {
            const Real celllo = iv[k]  * fdx[k] + plo[k];
            const Real cellhi = celllo + fdx[k];

            if ((p.m_pos[k] < celllo) && (celllo > (p.m_pos[k] + dx[k]/2)))
                touches = false;

            if ((p.m_pos[k] > cellhi) && (cellhi < (p.m_pos[k] - dx[k]/2)))
                touches = false;
        }

        if (touches)
        {
            fcells.push_back(iv);
        }
    }

    Real sum_fine = 0;
    //
    // We need to figure out the fine fractions and the fine grid needed updating.
    //
    for (int j = 0; j < fcells.size(); j++)
    {
        IntVect& iv = fcells[j];

        Real the_frac = 1;

        for (int k = 0; k < BL_SPACEDIM; k++)
        {
            const Real celllo = (iv[k] * fdx[k] + plo[k]);

            if (p.m_pos[k] <= celllo)
            {
                const Real isecthi = p.m_pos[k] + dx[k]/2;

                the_frac *= std::min((isecthi - celllo),fdx[k]);
            }
            else
            {
                const Real cellhi  = (iv[k]+1) * fdx[k] + plo[k];
                const Real isectlo = p.m_pos[k] - dx[k]/2;

                the_frac *= std::min((cellhi - isectlo),fdx[k]);
            }
        }

        ffrac.push_back(the_frac);

        sum_fine += the_frac;

        if (cshift != IntVect::TheZeroVector())
        {
            //
            // Update to the correct fine cell needing updating.
            // Note that "cshift" is from the coarse perspective.
            //
            const IntVect fshift = cshift * amr->refRatio(lev);
            //
            // Update fcells[j] to indicate a shifted fine cell needing updating.
            //
            iv -= fshift;
        }

        isects = fba.intersections(Box(iv,iv));

        BL_ASSERT(!isects.empty());
        BL_ASSERT(isects.size() == 1);

        fgrid.push_back(isects[0].first);
    }

    BL_ASSERT(ffrac.size() == fcells.size());
    BL_ASSERT(fgrid.size() == fcells.size());
    //
    // Now adjust the fine fractions so they sum to one.
    //
    for (int j = 0; j < ffrac.size(); j++)
        ffrac[j] /= sum_fine;
}

int
ParticleBase::MaxReaders ()
{
    const int Max_Readers_def = 64;

    static int Max_Readers;

    static bool first = true;

    if (first)
    {
        first = false;

        ParmParse pp("particles");

        Max_Readers = Max_Readers_def;

        pp.query("nreaders", Max_Readers);

        Max_Readers = std::min(ParallelDescriptor::NProcs(),Max_Readers);

        if (Max_Readers <= 0)
            BoxLib::Abort("particles.nreaders must be positive");
    }

    return Max_Readers;
}

const std::string&
ParticleBase::DataPrefix ()
{
    //
    // The actual particle data is stored in files of the form: DATA_nnnn.
    //
    static const std::string data("DATA_");

    return data;
}

const std::string&
ParticleBase::Version ()
{
    //
    // If we change the Checkpoint/Restart format we should increment this.
    //
    static const std::string version("Version_One_Dot_Zero");

    return version;
}

static int the_next_id = 1;

int
ParticleBase::NextID ()
{
    int next;

#ifdef _OPENMP
#pragma omp critical(nextid_lock)
#endif
    {
        if (the_next_id == std::numeric_limits<int>::max())
            BoxLib::Abort("ParticleBase::NextID() -- too many particles");

        next = the_next_id++;
    }

    return next;
}

void
ParticleBase::NextID (int nextid)
{
    the_next_id = nextid;
}

IntVect
ParticleBase::Index (const ParticleBase& p,
                     int                 lev,
                     const Amr*          amr)
{
    BL_ASSERT(amr != 0);
    BL_ASSERT(lev >= 0 && lev <= amr->finestLevel());

    IntVect iv;

    const Geometry& geom = amr->Geom(lev);

    D_TERM(iv[0]=floor((p.m_pos[0]-geom.ProbLo(0))/geom.CellSize(0));,
           iv[1]=floor((p.m_pos[1]-geom.ProbLo(1))/geom.CellSize(1));,
           iv[2]=floor((p.m_pos[2]-geom.ProbLo(2))/geom.CellSize(2)););

    iv += geom.Domain().smallEnd();

    return iv;
}

bool
ParticleBase::Where (ParticleBase& p,
                     const Amr*    amr,
                     bool          update,
                     int           lev_min)
{
    BL_ASSERT(amr != 0);
    if (update)
    {
        //
        // We have a valid particle whose position has changed slightly.
        // Try to update m_cell and m_grid smartly.
        //
        BL_ASSERT(p.m_id > 0);
        BL_ASSERT(p.m_grid >= 0 && p.m_grid < amr->boxArray(p.m_lev).size());

        IntVect iv = ParticleBase::Index(p,p.m_lev,amr);

        if (p.m_cell == iv)
            //
            // The particle hasn't left its cell.
            //
            return true;

        if (p.m_lev == amr->finestLevel())
        {
            p.m_cell = iv;

            if (amr->boxArray(p.m_lev)[p.m_grid].contains(p.m_cell))
                //
                // It has left its cell but is still in the same grid.
                //
                return true;
        }
    }

    std::vector< std::pair<int,Box> > isects;

    for (int lev = amr->finestLevel(); lev >= 0; lev--)
    {
        IntVect iv = ParticleBase::Index(p,lev,amr);

        isects = amr->boxArray(lev).intersections(Box(iv,iv));

        if (!isects.empty())
        {
            BL_ASSERT(isects.size() == 1);

            p.m_lev  = lev;
            p.m_grid = isects[0].first;
            p.m_cell = iv;

            return true;
        }
    }
    return false;
}

bool
ParticleBase::PeriodicWhere (ParticleBase& p,
                     const Amr*    amr,
                     int           lev_min)
{
    BL_ASSERT(amr != 0);
    
    //create a copy "dummy" particle to check for periodic outs
    ParticleBase p_prime;
    p_prime.m_grid = p.m_grid;
    p_prime.m_lev = p.m_lev;
    p_prime.m_cell = p.m_cell;
    for (int d = 0; d < BL_SPACEDIM; d++)
       p_prime.m_pos[d] = p.m_pos[d];
    
    ParticleBase::PeriodicShift(p_prime, amr);
    
    bool shifted = false; //this could be cleaned up
    for (int d = 0; d < BL_SPACEDIM; d++)
    {
        if (p_prime.m_pos[d] != p.m_pos[d])
        {
            shifted = true;
            break;
        }
    }
    
    if (shifted)
    {
        std::vector< std::pair<int,Box> > isects;

        for (int lev = amr->finestLevel(); lev >= lev_min; lev--)
        {
            IntVect iv = ParticleBase::Index(p_prime,lev,amr);

            isects = amr->boxArray(lev).intersections(Box(iv,iv));

            if (!isects.empty())
            {
                BL_ASSERT(isects.size() == 1);
                for (int d = 0; d < BL_SPACEDIM; d++)
                    p.m_pos[d] = p_prime.m_pos[d];
                p.m_lev  = lev;
                p.m_grid = isects[0].first;
                p.m_cell = iv;

                return true;
            }
        }
    }

    return false;
}

bool
ParticleBase::RestrictedWhere(ParticleBase& p,
                     const Amr*    amr,
                     int           ngrow)
{
    IntVect iv = ParticleBase::Index(p,p.m_lev,amr);
    

    if (BoxLib::grow(amr->boxArray(p.m_lev)[p.m_grid], ngrow).contains(iv))
    {
        p.m_cell = iv;

        return true;
    }
    return false;
}

void
ParticleBase::PeriodicShift (ParticleBase& p,
                             const Amr*    amr)
{
    //
    // This routine should only be called when Where() returns false.
    //
    BL_ASSERT(amr != 0);
    //
    // We'll use level 0 stuff since ProbLo/ProbHi are the same for every level.
    //
    const Geometry& geom = amr->Geom(0);
    const Box&      dmn  = geom.Domain();
    IntVect         iv   = ParticleBase::Index(p,0,amr);
    const Real      eps  = 1.e-13;
    for (int i = 0; i < BL_SPACEDIM; i++)
    {
        if (!geom.isPeriodic(i)) continue;

        if (iv[i] > dmn.bigEnd(i))
        {
            if (p.m_pos[i] == geom.ProbHi(i))
                //
                // Don't let particles lie exactly on the domain face.
                // Force the particle to be outside the domain so the
                // periodic shift will bring it back inside.
                //
                p.m_pos[i] += eps;

            p.m_pos[i] -= geom.ProbLength(i);

            BL_ASSERT(p.m_pos[i] >= geom.ProbLo(i));
        }
        else if (iv[i] < dmn.smallEnd(i))
        {
            if (p.m_pos[i] == geom.ProbLo(i))
                //
                // Don't let particles lie exactly on the domain face.
                // Force the particle to be outside the domain so the
                // periodic shift will bring it back inside.
                //
                p.m_pos[i] -= eps;

            p.m_pos[i] += geom.ProbLength(i);

            BL_ASSERT(p.m_pos[i] <= geom.ProbHi(i));
        }
    }
    //
    // The particle may still be outside the domain in the case
    // where we aren't periodic on the face out which it travelled.
    //
}

void
ParticleBase::Reset (ParticleBase& p,
                     const Amr*    amr,
                     bool          update)
{
    BL_ASSERT(amr != 0);

    if (!ParticleBase::Where(p,amr,update))
    {
        //
        // Here's where we need to deal with boundary conditions.
        //
        // Attempt to shift the particle back into the domain if it
        // crossed a periodic boundary.  Otherwise (for now) we
        // invalidate the particle.
        //
        ParticleBase::PeriodicShift(p,amr);

        if (!ParticleBase::Where(p,amr))
        {
#ifdef _OPENMP
#pragma omp critical(reset_lock)
#endif
            {
                std::cout << "Invalidating out-of-domain particle: " << p << '\n';
            }

            BL_ASSERT(p.m_id > 0);

            p.m_id = -p.m_id;
        }
    }
}

Real
ParticleBase::InterpDoit (const FArrayBox& fab,
                          const IntVect&   cell,
                          const Real*      frac,
                          int              comp)
{
    const int M = D_TERM(2,+2,+4);

    Real    fracs[M];
    IntVect cells[M];

    ParticleBase::CIC_Fracs(frac, fracs);
    ParticleBase::CIC_Cells(cell, cells);

    Real val = ParticleBase::InterpDoit(fab,fracs,cells,comp);

    return val;
}

Real
ParticleBase::InterpDoit (const FArrayBox& fab,
                          const Real*      fracs,
                          const IntVect*   cells,
                          int              comp)
{
    const int M = D_TERM(2,+2,+4);

    Real val = 0;

    for (int i = 0; i < M; i++)
    {
        val += fab(cells[i],comp) * fracs[i];
    }

    return val;
}

void
ParticleBase::Interp (const ParticleBase& prt,
                      const Amr*          amr,
                      const FArrayBox&    fab,
                      const int*          idx,
                      Real*               val,
                      int                 cnt)
{
    BL_ASSERT(amr != 0);
    BL_ASSERT(idx != 0);
    BL_ASSERT(val != 0);

    const int       M   = D_TERM(2,+2,+4);
    const Geometry& gm  = amr->Geom(prt.m_lev);
    const Real*     plo = gm.ProbLo();
    const Real*     dx  = gm.CellSize();

    Real    fracs[M];
    IntVect cells[M];
    //
    // Get "fracs" and "cells".
    //
    ParticleBase::CIC_Cells_Fracs(prt, plo, dx, fracs, cells);

    for (int i = 0; i < cnt; i++)
    {
        BL_ASSERT(idx[i] >= 0 && idx[i] < fab.nComp());

        val[i] = ParticleBase::InterpDoit(fab,fracs,cells,idx[i]);
    }
}

void
ParticleBase::GetGravity (const FArrayBox&    gfab,
                          const Amr*          amr,
                          const ParticleBase& p,
                          Real*               grav)
{
    BL_ASSERT(amr  != 0);
    BL_ASSERT(grav != 0);

    int idx[BL_SPACEDIM] = { D_DECL(0,1,2) };

    ParticleBase::Interp(p,amr,gfab,idx,grav,BL_SPACEDIM);
}

std::ostream&
operator<< (std::ostream& os, const ParticleBase& p)
{
    os << p.m_id   << ' '
       << p.m_cpu  << ' '
       << p.m_lev  << ' '
       << p.m_grid << ' '
       << p.m_cell << ' ';

    for (int i = 0; i < BL_SPACEDIM; i++)
        os << p.m_pos[i] << ' ';

    if (!os.good())
        BoxLib::Error("operator<<(ostream&,ParticleBase&) failed");

    return os;
}
