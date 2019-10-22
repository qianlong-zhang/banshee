#ifndef _MC_H_
#define _MC_H_

#include "config.h"
#include "g_std/g_string.h"
#include "memory_hierarchy.h"
#include <string>
#include "stats.h"
#include "g_std/g_unordered_map.h"

#define MAX_STEPS 10000

enum ReqType
{
	LOAD = 0,
	STORE
};

enum Scheme
{
   AlloyCache,
   UnisonCache,
   HMA,
   HybridCache,
   NoCache,
   CacheOnly,
   Tagless,
   LongCache
};

#if 0
class ReplaceInfo
{
	uint32_t replace_way;
	bool isPrimeTag;
};
#endif

class Way
{
public:
	Address primeTag;
	uint64_t primeTagValidBits; 	// whether a Footprint is valid in a page
    uint64_t primeTagReferenceBits;// used to record which Footprint is really accessed after prefetched by valid_bits, shared in primeTag and subTag
    uint64_t primeTagDirtyBits; 	// whether a Footprint is dirty in page, shared in primeTag and subTag
   	uint32_t primeTagNumAccess;
	uint32_t primeTagNumRealHits;
	uint32_t primeTagNumFalseHits;
	uint32_t primeTagNumMisses;

	bool tagRoleChanged;	// if this is 1, the role of primeTag and subTag is exchanged, default is 0, we don't use this currently
	uint32_t inWayReplaceCounts; // record how many times the in way replace happens

	Address subTag;
	uint64_t subTagValidBits; 	// whether a Footprint is valid in a page
	uint64_t subTagReferenceBits;
    uint64_t subTagDirtyBits;
   	uint32_t subTagNumAccess;	// record subTag access counts
	uint32_t subTagNumRealHits;
	uint32_t subTagNumFalseHits;
	uint32_t subTagNumMisses;

	//Tag is aligned to Page, addr is aligned to cacheline(64B)
	bool isRealHit(Address Tag, Address addr)
	{
		assert( primeTag!=subTag );
		assert((addr%Tag)<64);
		assert( (primeTagValidBits & subTagValidBits) == 0);

		bool primeTagIsValid = primeTagValidBits & (((uint64_t)1UL)<<(addr%Tag));
		bool subTagIsValid = subTagValidBits & (((uint64_t)1UL)<<(addr%Tag));
		if(primeTag == Tag && primeTagIsValid)
		{
			return true;
		}
		else if( subTag == Tag && subTagIsValid)
		{
			return true;
		}
		else
			return false;
	}

	// isRealHit=Real hit, onlyTagHit means Tag hit but footprint data not.
	bool onlyTagHit(Address Tag, Address addr)
	{
		assert( primeTag!=subTag );
		assert((addr%Tag)<64);

		bool primeTagIsValid = primeTagValidBits & (((uint64_t)1UL)<<(addr%Tag));
		bool subTagIsValid = subTagValidBits & (((uint64_t)1UL)<<(addr%Tag));

		if(primeTag == Tag && !primeTagIsValid)
		{
			return true;
		}
		else if( subTag == Tag && !subTagIsValid)
		{
			return true;
		}
		else
			return false;
	}
};

class Set
{
public:
   Way * ways;
   uint32_t num_ways;

   uint32_t primeTagGetEmptyWay()
   {
      for (uint32_t i = 0; i < num_ways; i++)
         if (ways[i].primeTagValidBits == 0)
            return i;
      return num_ways;
   };
   uint32_t subTagGetEmptyWay()
   {
      for (uint32_t i = 0; i < num_ways; i++)
         if (!ways[i].subTagValidBits)
            return i;
      return num_ways;
   };
//   bool hasEmptyWay() { return getEmptyWay() < num_ways; };
   bool primeTagHasEmptyWay() { return primeTagGetEmptyWay() < num_ways; };
   bool subTagHasEmptyWay() { return subTagGetEmptyWay() < num_ways; };
};

// Not modeling all details of the tag buffer.
class TagBufferEntry
{
public:
	// replaced way info and cached info are updated in _tlb in real-time
	// _tlb[tag].way == _num_ways means not cached, or it is cached in the _tlb[tag].way
	// so it's not necessary to record replaced way info and cached info here, only remap is recorded
	Address tag;
	bool remap;
	uint32_t lru;

/*  //no need record those info in tagbuffer, we can keep those up-to-date in dram cache and only simulate the delay is ok

	// store in TB to reduce Dram Cache tag probe, maybe newer than those in DC
	uint64_t valid_bits; 	// whether a Footprint is valid in a page
    uint64_t dirty_bits; 	// whether a Footprint is dirty in page
    uint64_t reference_bits;// used to record which Footprint is really accessed after prefetched by valid_bits
 */
};

class MemoryController;

class TagBuffer : public GlobAlloc {
public:
	TagBuffer(Config &config, MemoryController *mc);
	// return: exists in tag buffer or not.
	uint32_t existInTB(Address tag);
	uint32_t getNumWays() { return _num_ways; };

	// return: if the address can be inserted to tag buffer or not.
	bool canInsert(Address tag);
	bool canInsert(Address tag1, Address tag2);
	void insert(Address tag, bool remap);
	void evict(Address tag);
	double getOccupancy() { return 1.0 * _entry_occupied / _num_ways / _num_sets; };
	void clearTagBuffer();
	void setClearTime(uint64_t time) { _last_clear_time = time; };
	uint64_t getClearTime() { return _last_clear_time; };
    void printAll(void);
	MemoryController *_mc;

private:
	void updateLRU(uint32_t set_num, uint32_t way);
	TagBufferEntry ** _tag_buffer;
	uint32_t _num_ways;
	uint32_t _num_sets;
	uint32_t _entry_occupied;
	uint64_t _last_clear_time;
};


class TLBEntry
{
public:
   uint64_t tag;
   uint64_t way;
   //uint64_t count; // for OS based placement policy

   // the following two are only for LongCache
   //uint64_t touch_bitvec; // whether a footprint is touched in a page
   //uint64_t dirty_bitvec; // whether a footprint is dirty in page
   uint64_t footprint_history; // record every page's history footprint info for prefetch, that is the real referenced bits.
};



class LinePlacementPolicy;
class PagePlacementPolicy;
class OSPlacementPolicy;

//class PlacementPolicy;
class DDRMemory;



class MemoryController : public MemObject {
private:
	DDRMemory * BuildDDRMemory(Config& config, uint32_t frequency, uint32_t domain, g_string name, const std::string& prefix, uint32_t tBL, double timing_scale);

	g_string _name;

	// Trace related code
	lock_t _lock;
	bool _collect_trace;
	g_string _trace_dir;
	Address _address_trace[10000];
	uint32_t _type_trace[10000];
	uint32_t _cur_trace_len;
	uint32_t _max_trace_len;

	// External Dram Configuration
	MemObject *	_ext_dram;
	g_string _ext_type;
public:
	// MC-Dram Configuration
	MemObject ** _mcdram;
	uint32_t _mcdram_per_mc;
	g_string _mcdram_type;

	uint64_t getNumRequests() { return _num_requests; };
   	uint64_t getNumSets()     { return _num_sets; };
   	uint32_t getNumWays()     { return _num_ways; };
   	double getRecentMissRate(){
        return ((double) _num_miss_per_step / (_num_miss_per_step + _num_hit_per_step));
    };
   	Scheme getScheme()      { return _scheme; };
   	Set * getSets()         { return _cache; };
   	g_unordered_map<Address, TLBEntry> * getTLB() { return &_tlb; };
	TagBuffer * getTagBuffer() { return _tag_buffer; };
    double getRecentBWRatio() {
        if(_mc_bw_per_step + _ext_bw_per_step > 0)
            return (1.0 * _mc_bw_per_step / (_mc_bw_per_step + _ext_bw_per_step));
        else
            return 0.0000001;
    }
	uint64_t getGranularity() { return _granularity; };

	//for LongCache dynamic footprint
	uint64_t getFootPrintSize() { return _footprint_size; };
	void SetFootPrintSize(uint32_t fpSize) { _footprint_size = fpSize; };

private:
	// For Alloy Cache.
	Address transMCAddress(Address mc_addr);
	// For Page Granularity Cache
	Address transMCAddressPage(uint64_t set_num, uint32_t way_num);

	// For Tagless.
	// For Tagless, we don't use "Set * _cache;" as other schemes. Instead, we use the following
	// structure to model a fully associative cache with FIFO replacement
	//vector<Address> _idx_to_address;
	uint64_t _next_evict_idx;
	//map<uint64_t, uint64_t> _address_to_idx;

	// Cache structure
	uint64_t _granularity;
	uint64_t _num_ways;
	uint64_t _cache_size;  // in Bytes
	uint64_t _num_sets;
	Set * _cache;
	PagePlacementPolicy * _page_placement_policy;
	uint64_t _num_requests;
	Scheme _scheme;
	TagBuffer * _tag_buffer;

	// 4 means one footprint is consist of 4 cachelines
	uint32_t _footprint_size;
	// if enabled, if wayN primeTag is selected to be replaced,
	// then the subTag in the same way will be replaced and tagRoleChanged will be set true.
	uint32_t _in_way_replace;

	// Balance in- and off-package DRAM bandwidth.
	// From "BATMAN: Maximizing Bandwidth Utilization of Hybrid Memory Systems"
	bool _bw_balance;
	uint64_t _ds_index;

	// TLB Hack
	g_unordered_map <Address, TLBEntry> _tlb;
	uint64_t _os_quantum;

    // Stats
	Counter _numPlacement;
  	Counter _numCleanEviction;
	Counter _numDirtyEviction;
	Counter _numLoadHit;
	Counter _numLoadMiss;
	Counter _numStoreHit;
	Counter _numStoreMiss;
	Counter _numCounterAccess; // for FBR placement policy

	Counter _numTagLoad;
	Counter _numTagStore;
	// For HybridCache
	Counter _numTagBufferFlush;
	Counter _numTagBufferHit;
	Counter _numTagBufferMiss;
	Counter _numTBDirtyMiss;
	Counter _numTBDirtyHit;
	Counter _numTouchedLines;
	Counter _numTouchedLines8Blocks;
	Counter _numTouchedLines16Blocks;
	Counter _numTouchedLines24Blocks;
	Counter _numTouchedLines32Blocks;
	Counter _numTouchedLines48Blocks;
	Counter _numTouchedLines64Blocks;
	Counter _numTouchedLinesFullBlocks;
	Counter _numEvictedDirtyLines;
    Counter _numEvictedValidPrimeTagPages;

    // For HybridCache
	Counter _numNotTouchedLines;
	Counter _numTouchedPages;

	// For LongCache
	Counter _numSubTagHit;
	//Counter _numEvictedLines;

	uint64_t _num_hit_per_step;
   	uint64_t _num_miss_per_step;
	uint64_t _mc_bw_per_step;
	uint64_t _ext_bw_per_step;
   	double _miss_rate_trace[MAX_STEPS];

   	uint32_t _num_steps;

	// to model the SRAM tag
	bool 	_sram_tag;
	uint32_t _llc_latency;
public:
	MemoryController(g_string& name, uint32_t frequency, uint32_t domain, Config& config);
	uint64_t access(MemReq& req);
    void printKeyInfo();
	const char * getName() { return _name.c_str(); };
	void initStats(AggregateStat* parentStat);
	// Use glob mem
	//using GlobAlloc::operator new;
	//using GlobAlloc::operator delete;
};

#endif
