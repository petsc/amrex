//BL_COPYRIGHT_NOTICE

//
// $Id: CArena.cpp,v 1.8 1998-02-09 22:02:14 lijewski Exp $
//

#ifdef BL_USE_NEW_HFILES
#include <iostream>
#include <iomanip>
#include <fstream>
#include <utility>
using std::ofstream;
using std::ios;
using std::setw;
using std::clog;
using std::endl;
using std::pair;
#else
#include <iostream.h>
#include <iomanip.h>
#include <fstream.h>
#include <stdlib.h>
#include <utility.h>
#endif

#include <CArena.H>

//
// If you want to test some FAB-based code without using the coalescing
// memory manager [you don't trust this code :-)] simply touch this module
// and recompile with the -DBL_DONT_COALESCE_MEMORY flag.  Fabs will then
// use BArena which directly calls ::operator new().
//
#ifdef BL_DONT_COALESCE_MEMORY
#include <BArena.H>
static CArena The_Static_FAB_BArena;
extern Arena* The_FAB_Arena = &The_Static_FAB_BArena;
#else
static CArena The_Static_FAB_CArena;
extern Arena* The_FAB_Arena = &The_Static_FAB_CArena;
#endif

CArena::CArena (size_t hunk_size)
{
    //
    // Force alignment of hunksize.
    //
    m_hunk = Arena::align(hunk_size == 0 ? DefaultHunkSize : hunk_size);

    assert(m_hunk >= hunk_size);
    assert(m_hunk%sizeof(Arena::Word) == 0);
}

CArena::~CArena ()
{
    for (int i = 0; i < m_alloc.size(); i++)
    {
        ::operator delete(m_alloc[i]);
    }
}

void*
CArena::alloc (size_t nbytes)
{
    nbytes = Arena::align(nbytes == 0 ? 1 : nbytes);
    //
    // Find node in freelist at lowest memory address that'll satisfy request.
    //
    NL::iterator free_it = m_freelist.begin();

    for ( ; free_it != m_freelist.end(); ++free_it)
    {
        if ((*free_it).size() >= nbytes)
            break;
    }

    void* vp = 0;

    if (free_it == m_freelist.end())
    {
        vp = ::operator new(nbytes < m_hunk ? m_hunk : nbytes);

        m_alloc.push_back(vp);

        if (nbytes < m_hunk)
        {
            //
            // Add leftover chunk to free list.
            //
            // Insert with a hint -- should be largest block in the set.
            //
            void* block = static_cast<char*>(vp) + nbytes;

            m_freelist.insert(m_freelist.end(), Node(block, m_hunk-nbytes));
        }
    }
    else
    {
        assert((*free_it).size() >= nbytes);

        assert(m_busylist.find(*free_it) == m_busylist.end());

        vp = (*free_it).block();

        if ((*free_it).size() > nbytes)
        {
            //
            // Insert remainder of free block back into freelist.
            //
            // Insert with a hint -- right after the current block being split.
            //
            Node freeblock = *free_it;

            freeblock.size(freeblock.size() - nbytes);

            freeblock.block(static_cast<char*>(vp) + nbytes);

            m_freelist.insert(free_it, freeblock);
        }

        m_freelist.erase(free_it);
    }

    m_busylist.insert(Node(vp, nbytes));

    assert(!(vp == 0));

    return vp;
}

void
CArena::free (void* vp)
{
    if (vp == 0)
        //
        // Allow calls with NULL as allowed by C++ delete.
        //
        return;
    //
    // `vp' had better be in the busy list.
    //
    NL::iterator busy_it = m_busylist.find(Node(vp,0));

    assert(!(busy_it == m_busylist.end()));

    assert(m_freelist.find(*busy_it) == m_freelist.end());

    void* freeblock = static_cast<char*>((*busy_it).block());
    //
    // Put free'd block on free list and save iterator to insert()ed position.
    //
    pair<NL::iterator,bool> pair_it = m_freelist.insert(*busy_it);

    assert(pair_it.second == true);

    NL::iterator free_it = pair_it.first;

    assert(free_it != m_freelist.end() && (*free_it).block() == freeblock);
    //
    // And remove from busy list.
    //
    m_busylist.erase(busy_it);
    //
    // Coalesce freeblock(s) on lo and hi side of this block.
    //
    if (!(free_it == m_freelist.begin()))
    {
        NL::iterator lo_it = free_it;
        --lo_it;
        void* addr = static_cast<char*>((*lo_it).block()) + (*lo_it).size();

        if (addr == (*free_it).block())
        {
            //
            // This cast is needed as iterators to set return const values;
            // i.e. we can't legally change an element of a set.
            // In this case I want to change the size() of a block
            // in the freelist.  Since size() is not used in the ordering
            // relations in the set, this won't effect the order;
            // i.e. it won't muck up the ordering of elements in the set.
            // I don't want to have to remove the element from the set and
            // then reinsert it with a different size() as it'll just go
            // back into the same place in the set.
            //
            Node* node = const_cast<Node*>(&(*lo_it));
            assert(!(node == 0));
            node->size((*lo_it).size() + (*free_it).size());
            m_freelist.erase(free_it);
            free_it = lo_it;
        }
    }

    NL::iterator hi_it = free_it;

    void* addr = static_cast<char*>((*free_it).block()) + (*free_it).size();

    if (++hi_it != m_freelist.end() && addr == (*hi_it).block())
    {
        //
        // Ditto the above comment.
        //
        Node* node = const_cast<Node*>(&(*free_it));
        assert(!(node == 0));
        node->size((*free_it).size() + (*hi_it).size());
        m_freelist.erase(hi_it);
    }
}
