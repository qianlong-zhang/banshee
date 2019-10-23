#include "mc.h"
#include "line_placement.h"
#include "page_placement.h"
#include "os_placement.h"
#include "mem_ctrls.h"
#include "dramsim_mem_ctrl.h"
#include "ddr_mem.h"
#include "zsim.h"

MemoryController::MemoryController(g_string& name, uint32_t frequency, uint32_t domain, Config& config)
	: _name (name)
{
    trace(MC,  "In MC!!!!!!!!!!!!!!");
	// Trace Related
	_collect_trace = config.get<bool>("sys.mem.enableTrace", false);
	if (_collect_trace && _name == "mem-0") {
		_cur_trace_len = 0;
		_max_trace_len = 10000;
		_trace_dir = config.get<const char *>("sys.mem.traceDir", "./");
		FILE * f = fopen((_trace_dir + g_string("/") + _name + g_string("trace.bin")).c_str(), "wb");
		uint32_t num = 0;
		fwrite(&num, sizeof(uint32_t), 1, f);
		fclose(f);
	    futex_init(&_lock);
	}
	_sram_tag = config.get<bool>("sys.mem.sram_tag", false);
	trace(MC, "_sram_tag is %d", _sram_tag);
	_llc_latency = config.get<uint32_t>("sys.caches.l3.latency");
	double timing_scale = config.get<double>("sys.mem.dram_timing_scale", 1);
	g_string scheme = config.get<const char *>("sys.mem.cache_scheme", "LongCache");
	_ext_type = config.get<const char *>("sys.mem.ext_dram.type", "Simple");

    trace(MC, "scheme is:%s", scheme.c_str());
	assert(scheme == "LongCache" && "Only LongCache is supported in this version");
	_scheme = LongCache;

	_footprint_size = config.get<uint32_t>("sys.mem.mcdram.footprint_size");
	_in_way_replace = config.get<bool>("sys.mem.mcdram.inWayReplace", true);
	_granularity = config.get<uint32_t>("sys.mem.mcdram.cache_granularity");
	_num_ways = config.get<uint32_t>("sys.mem.mcdram.num_ways");
	_mcdram_type = config.get<const char *>("sys.mem.mcdram.type", "Simple");
	_cache_size = config.get<uint32_t>("sys.mem.mcdram.size", 128) * 1024 * 1024;
	_bw_balance = config.get<bool>("sys.mem.bwBalance", false);
	_ds_index = 0;

	// Configure the external Dram
	g_string ext_dram_name = _name + g_string("-ext");
	if (_ext_type == "Simple") {
    	uint32_t latency = config.get<uint32_t>("sys.mem.ext_dram.latency", 100);
        _ext_dram = (SimpleMemory *) gm_malloc(sizeof(SimpleMemory));
		new (_ext_dram)	SimpleMemory(latency, ext_dram_name, config);
	} else if (_ext_type == "DDR")
        _ext_dram = BuildDDRMemory(config, frequency, domain, ext_dram_name, "sys.mem.ext_dram.", 4, 1.0);
	else if (_ext_type == "MD1") {
    	uint32_t latency = config.get<uint32_t>("sys.mem.ext_dram.latency", 100);
        uint32_t bandwidth = config.get<uint32_t>("sys.mem.ext_dram.bandwidth", 6400);
        _ext_dram = (MD1Memory *) gm_malloc(sizeof(MD1Memory));
		new (_ext_dram) MD1Memory(64, frequency, bandwidth, latency, ext_dram_name);
    } else if (_ext_type == "DRAMSim") {
	    uint64_t cpuFreqHz = 1000000 * frequency;
        uint32_t capacity = config.get<uint32_t>("sys.mem.capacityMB", 16384);
        string dramTechIni = config.get<const char*>("sys.mem.techIni");
        string dramSystemIni = config.get<const char*>("sys.mem.systemIni");
        string outputDir = config.get<const char*>("sys.mem.outputDir");
        string traceName = config.get<const char*>("sys.mem.traceName", "dramsim");
		traceName += "_ext";
        _ext_dram = (DRAMSimMemory *) gm_malloc(sizeof(DRAMSimMemory));
    	uint32_t latency = config.get<uint32_t>("sys.mem.ext_dram.latency", 100);
		new (_ext_dram) DRAMSimMemory(dramTechIni, dramSystemIni, outputDir, traceName, capacity, cpuFreqHz, latency, domain, name);
	} else
        panic("Invalid memory controller type %s", _ext_type.c_str());

	if (_scheme != NoCache) {
		// Configure the MC-Dram (Timing Model)
		_mcdram_per_mc = config.get<uint32_t>("sys.mem.mcdram.mcdramPerMC", 4);
		//_mcdram = new MemObject * [_mcdram_per_mc];
		_mcdram = (MemObject **) gm_malloc(sizeof(MemObject *) * _mcdram_per_mc);
		for (uint32_t i = 0; i < _mcdram_per_mc; i++) {
			g_string mcdram_name = _name + g_string("-mc-") + g_string(to_string(i).c_str());
    	    //g_string mcdram_name(ss.str().c_str());
			if (_mcdram_type == "Simple") {
	    		uint32_t latency = config.get<uint32_t>("sys.mem.mcdram.latency", 50);
				_mcdram[i] = (SimpleMemory *) gm_malloc(sizeof(SimpleMemory));
				new (_mcdram[i]) SimpleMemory(latency, mcdram_name, config);
	        	//_mcdram[i] = new SimpleMemory(latency, mcdram_name, config);
			} else if (_mcdram_type == "DDR") {
				// XXX HACK tBL for mcdram is 1, so for data access, should multiply by 2, for tad access, should multiply by 3.
        		_mcdram[i] = BuildDDRMemory(config, frequency, domain, mcdram_name, "sys.mem.mcdram.", 4, timing_scale);
			} else if (_mcdram_type == "MD1") {
				uint32_t latency = config.get<uint32_t>("sys.mem.mcdram.latency", 50);
        		uint32_t bandwidth = config.get<uint32_t>("sys.mem.mcdram.bandwidth", 12800);
        		_mcdram[i] = (MD1Memory *) gm_malloc(sizeof(MD1Memory));
				new (_mcdram[i]) MD1Memory(64, frequency, bandwidth, latency, mcdram_name);
		    } else if (_mcdram_type == "DRAMSim") {
			    uint64_t cpuFreqHz = 1000000 * frequency;
		        uint32_t capacity = config.get<uint32_t>("sys.mem.capacityMB", 16384);
		        string dramTechIni = config.get<const char*>("sys.mem.techIni");
		        string dramSystemIni = config.get<const char*>("sys.mem.systemIni");
		        string outputDir = config.get<const char*>("sys.mem.outputDir");
		        string traceName = config.get<const char*>("sys.mem.traceName");
				traceName += "_mc";
				traceName += to_string(i);
		        _mcdram[i] = (DRAMSimMemory *) gm_malloc(sizeof(DRAMSimMemory));
    			uint32_t latency = config.get<uint32_t>("sys.mem.mcdram.latency", 50);
				new (_mcdram[i]) DRAMSimMemory(dramTechIni, dramSystemIni, outputDir, traceName, capacity, cpuFreqHz, latency, domain, name);
			} else
    	     	panic("Invalid memory controller type %s", _mcdram_type.c_str());
		}
		// Configure MC-Dram Functional Model
		_num_sets = _cache_size / _num_ways / _granularity;
		_cache = (Set *) gm_malloc(sizeof(Set) * _num_sets);
		for (uint64_t i = 0; i < _num_sets; i ++) {
			_cache[i].ways = (Way *) gm_malloc(sizeof(Way) * _num_ways);
			_cache[i].num_ways = _num_ways;
			for (uint32_t j = 0; j < _num_ways; j++)
            {
	   			_cache[i].ways[j].primeTag = 0;
	   			_cache[i].ways[j].primeTagValidBits = 0;
	   			_cache[i].ways[j].primeTagReferenceBits = 0;
	   			_cache[i].ways[j].primeTagDirtyBits = 0;
	   			_cache[i].ways[j].primeTagNumAccess = 0;
	   			_cache[i].ways[j].primeTagNumRealHits = 0;
	   			_cache[i].ways[j].primeTagNumFalseHits = 0;
	   			_cache[i].ways[j].primeTagNumMisses = 0;
	   			_cache[i].ways[j].tagRoleChanged = false;
	   			_cache[i].ways[j].inWayReplaceCounts = 0;
	   			_cache[i].ways[j].subTag = 0;
	   			_cache[i].ways[j].subTagValidBits = 0;
	   			_cache[i].ways[j].subTagReferenceBits = 0;
	   			_cache[i].ways[j].subTagDirtyBits = 0;
	   			_cache[i].ways[j].subTagNumAccess = 0;
	   			_cache[i].ways[j].subTagNumRealHits = 0;
	   			_cache[i].ways[j].subTagNumFalseHits = 0;
	   			_cache[i].ways[j].subTagNumMisses = 0;
            }
		}

		assert (_scheme == LongCache);
		_page_placement_policy = (PagePlacementPolicy *) gm_malloc(sizeof(PagePlacementPolicy));
		new (_page_placement_policy) PagePlacementPolicy(this);
		_page_placement_policy->initialize(config);
	}

	_tag_buffer = (TagBuffer *) gm_malloc(sizeof(TagBuffer));
	new (_tag_buffer) TagBuffer(config, this);

 	// Stats
   _num_hit_per_step = 0;
   _num_miss_per_step = 0;
   _mc_bw_per_step = 0;
   _ext_bw_per_step = 0;
   for (uint32_t i = 0; i < MAX_STEPS; i++)
      _miss_rate_trace[i] = 0;
   _num_requests = 0;
}

uint64_t
MemoryController::access(MemReq& req)
{
	switch (req.type) {
		case PUTS:
		case PUTX:
			*req.state = I;
			break;
		case GETS:
			*req.state = req.is(MemReq::NOEXCL)? S : E;
			break;
		case GETX:
			*req.state = M;
			break;
		default: panic("!?");
	}
	if (req.type == PUTS)
		return req.cycle;
	futex_lock(&_lock);
	// ignore clean LLC eviction
	if (_collect_trace && _name == "mem-0") {
		_address_trace[_cur_trace_len] = req.lineAddr;
		_type_trace[_cur_trace_len] = (req.type == PUTX)? 1 : 0;
		_cur_trace_len ++;
		assert(_cur_trace_len <= _max_trace_len);
		if (_cur_trace_len == _max_trace_len) {
			FILE * f = fopen((_trace_dir + g_string("/") + _name + g_string("trace.bin")).c_str(), "ab");
			fwrite(_address_trace, sizeof(Address), _max_trace_len, f);
			fwrite(_type_trace, sizeof(uint32_t), _max_trace_len, f);
			fclose(f);
			_cur_trace_len = 0;
		}
	}

	_num_requests ++;
	info("_num_requests = %ld", _num_requests);
	ReqType type = (req.type == GETS || req.type == GETX)? LOAD : STORE;

	Address address = req.lineAddr;	// lineAddr = real_physical_address / 64, so this lineAddr is not real physical address
	uint32_t mcdram_select = (address / 64) % _mcdram_per_mc;
	Address mc_address = (address / 64 / _mcdram_per_mc * 64) | (address % 64);
	Address tag = address / (_granularity / 64);
	assert(((address - tag * 64) / _footprint_size)<64);
	uint64_t access_bit = (uint64_t)1UL<<((address - tag * 64) / _footprint_size);

	if(type == STORE)
		info("Request tag 0x%lx is STORE!", tag);

	trace(MC, "\n\n!!!!!!!!!!!In DramCache, address=0x%lx, tag: 0x%lx, access_bit:0x%lx,  _mcdram_per_mc=%d, mc_address=0x%lx, type=%d", address, tag, access_bit, _mcdram_per_mc, mc_address, type);
	//info("\n\n!!!!!!!!!!!In DramCache, address=0x%lx, tag: 0x%lx, access_bit:0x%lx,  _mcdram_per_mc=%d, mc_address=0x%lx, type=%d", address, tag, access_bit, _mcdram_per_mc, mc_address, type);
	uint64_t set_num = tag % _num_sets;
	uint32_t hit_way = _num_ways;
	//uint64_t orig_cycle = req.cycle;
	uint64_t data_ready_cycle = req.cycle;
	MESIState state;

	uint64_t step_length = _cache_size / 64 / 10;
	// whether needs to probe tag for LongCache.
	// need to do so for LLC dirty eviction and if the page is not in TB
	bool hybrid_tag_probe = false;
	bool tagbuffer_hit = false;
	assert(_granularity >= 4096);

	if (_tlb.find(tag) == _tlb.end()){
		trace(MC, "can not find tlb entry for 0x%lx, creating one", tag);
		_tlb[tag] = TLBEntry {tag, _num_ways, 0};
#if 0
		trace(MC,  "Printing TLB: ");
		for(auto iter=_tlb.begin(); iter!=_tlb.end(); iter++)
		{
			//if(iter->first != 0 && iter->second.way!=_num_ways)
			{
				print( "\tAddress:0x%lx, tag:0x%lx, way:%ld, footprint_history:0x%lx", iter->first, iter->second.tag, iter->second.way, iter->second.footprint_history);
			}
			print("\n");
		}
#endif
		_numTouchedPages.inc();
	}
	if ( _sram_tag)
	{
		trace(MC, "With SRAM Tag");
		req.cycle += _llc_latency;
	}

#if 0
	trace(MC,  "Printing Tag Buffer: \n");
	_tag_buffer->printAll();

	trace(MC,  "Printing TLB: \n");
	for(auto iter=_tlb.begin(); iter!=_tlb.end(); iter++)
	{
		if(iter->first != 0 && iter->second.way!=_num_ways)
		{
			trace(MC,  "Address:0x%lx, tag:0x%lx, way:%ld, footprint_history:0x%lx\n", iter->first, iter->second.tag, iter->second.way, iter->second.footprint_history);
		}
	}
#endif

	// TagBuffer miss
	if(_tag_buffer->existInTB(tag) == _tag_buffer->getNumWays() && set_num >= _ds_index)
	{
		_numTagBufferMiss.inc();
		if ( type == STORE)
		{
			_numTBDirtyMiss.inc();
			if (!_sram_tag)
				hybrid_tag_probe = true;
		}
		trace(MC, "Miss in tagbuffer!");
	}
	else
	{
		trace(MC, "Hit in tagbuffer!");
		tagbuffer_hit=true;
		_numTagBufferHit.inc();
		if(type==STORE)
			_numTBDirtyHit.inc();
	}

	//DC hit
	if (_tlb[tag].way != _num_ways)
	{
		hit_way = _tlb[tag].way;
		trace(MC, "Dram Cache Hit!, tag=0x%lx, hit way:%d", tag, hit_way);
		assert((_cache[set_num].ways[hit_way].subTagValidBits & _cache[set_num].ways[hit_way].primeTagValidBits) == 0);
		//TLB hit does not means footprint data must in Dram Cache, only means Tag is in Dram Cache
		assert(_cache[set_num].ways[hit_way].isRealHit(tag, address) ||
				_cache[set_num].ways[hit_way].onlyTagHit(tag, address));

		/********** dealing with tag access *********/
		if (!hybrid_tag_probe) {
			trace(MC, "hybrid_tag_probe = %d, _sram_tag= %d", hybrid_tag_probe, _sram_tag);
			req.lineAddr = mc_address;
			req.cycle = _mcdram[mcdram_select]->access(req, 0, 4);
			_mc_bw_per_step += 4;
			req.lineAddr = address;
			data_ready_cycle = req.cycle;
		}
		else
		{
			trace(MC, "hybrid_tag_probe = %d", hybrid_tag_probe);
			assert(!_sram_tag);
			/********** dealing with TagBuffer *********/
			if(!tagbuffer_hit)
			{
				// search tag in Dram cache, and fetch tag info into TB
				MemReq tag_probe = {mc_address, GETS, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
				req.cycle = _mcdram[mcdram_select]->access(tag_probe, 0, 2);//2*16B already including valid/dirty/ref bits
				_mc_bw_per_step += 2;
				_numTagLoad.inc();

				if(_tag_buffer->canInsert(tag))
				{
					trace(MC, " tag buffer can insert tag with remap=false");
					_tag_buffer->insert(tag, false);
				}
			}
		}

		// TB miss, DC subTag Hit(maybe), valid=0 or valid=1;
		// where the request hit(primeTag or subTag) is determined by dram cache itself
		// subTag hit may invoke cache replacement and subTag promote
		bool counter_access = false;
		uint32_t primeTag_victim_way = _page_placement_policy->handleCacheHit(tag, address, type, set_num, &_cache[set_num], counter_access, hit_way);
		if(primeTag_victim_way == _num_ways)
		{
			trace(MC, "tag 0x%lx: primeTag_victim_way = %d NOT allocate!", tag, primeTag_victim_way);
		}
		else
		{
			trace(MC, "tag 0x%lx: primeTag_victim_way = %d will allocate into primeTag!", tag, primeTag_victim_way);
		}
#if 0
		//print tlb
		if(_tlb.find(tag) != _tlb.end())
		{
			trace(MC, "In DramCache, DramCache hit, access address is 0x%lx, tag is 0x%lx", req.lineAddr, tag);
		}
#endif

		/********** dealing with Dram Cache tag *********/
		// DC hit, valid = 1
		// if TB miss, fetch DC tag into TB
		// update the dram cache tag info, if hit in subTag, maybe we need swap primeTag and subTag in Dram cache and TLB
		if(_cache[set_num].ways[hit_way].isRealHit(tag, address))
		{
			trace(MC, "tag:0x%lx for set:%ld, way:%d is real hit", tag, set_num, hit_way);
			assert((_cache[set_num].ways[hit_way].subTagValidBits & _cache[set_num].ways[hit_way].primeTagValidBits) == 0);
			assert(((~_cache[set_num].ways[hit_way].primeTagValidBits) & _cache[set_num].ways[hit_way].primeTagDirtyBits) == 0);
			assert(((~_cache[set_num].ways[hit_way].subTagValidBits) & _cache[set_num].ways[hit_way].subTagDirtyBits) == 0);

			/********** dealing with data access *********/
			// realhit should access mcdram
			req.lineAddr = mc_address;
			req.cycle = _mcdram[mcdram_select]->access(req, 1, 4);
			_mc_bw_per_step += 4;
			req.lineAddr = address;
			data_ready_cycle = req.cycle;

			uint64_t dirty_bits=0;

			/********** dealing with Dram Cache meta-data *********/
			if(tag == _cache[set_num].ways[hit_way].primeTag)
			{
				trace(MC, "hit primeTag, dealing with dram cache tag");
				assert(_cache[set_num].ways[hit_way].primeTagValidBits & access_bit);
				//valid_bits = _cache[set_num].ways[hit_way].primeTagValidBits;
				if (req.type == PUTX)
				{
					_numStoreHit.inc();
					dirty_bits = _cache[set_num].ways[hit_way].primeTagDirtyBits | access_bit;
				}
				else
				{
					_numLoadHit.inc();
				}
				_cache[set_num].ways[hit_way].primeTagReferenceBits |= access_bit;
				_cache[set_num].ways[hit_way].primeTagDirtyBits |= dirty_bits;
				_cache[set_num].ways[hit_way].primeTagNumAccess++;
				_cache[set_num].ways[hit_way].primeTagNumRealHits++;

			}
			else if(tag == _cache[set_num].ways[hit_way].subTag)
			{
				trace(MC, "hit primeTag, dealing with dram cache tag");
				_numSubTagHit.inc();
				assert(_cache[set_num].ways[hit_way].subTagValidBits & access_bit);

				if (req.type == PUTX)
				{
					_numStoreHit.inc();
					dirty_bits = _cache[set_num].ways[hit_way].subTagDirtyBits | access_bit;
				}
				else
				{
					_numLoadHit.inc();
				}
				_cache[set_num].ways[hit_way].subTagReferenceBits |= access_bit;
				_cache[set_num].ways[hit_way].subTagDirtyBits |= dirty_bits;
				_cache[set_num].ways[hit_way].subTagNumAccess++;
				_cache[set_num].ways[hit_way].subTagNumRealHits++;
			}
		}
		// DC tag hit, valid=0
		// fetch footprint from ext_dram and fetch DC tag into TB
		else
		{
			trace(MC, "tag:0x%lx for set:%ld, way:%d is false hit", tag, set_num, hit_way);
			assert(_cache[set_num].ways[hit_way].onlyTagHit(tag, address));
			assert((_cache[set_num].ways[hit_way].subTagValidBits & _cache[set_num].ways[hit_way].primeTagValidBits) == 0);
			assert(((~_cache[set_num].ways[hit_way].primeTagValidBits) & _cache[set_num].ways[hit_way].primeTagDirtyBits) == 0);
			assert(((~_cache[set_num].ways[hit_way].subTagValidBits) & _cache[set_num].ways[hit_way].subTagDirtyBits) == 0);


			/********** dealing with data access *********/
			// onlyTagHit should access ext_dram
			req.cycle = _ext_dram->access(req, 1, 4);
			_ext_bw_per_step += 4;
			req.lineAddr = address;
			data_ready_cycle = req.cycle;

			if(tag == _cache[set_num].ways[hit_way].primeTag)
			{
				trace(MC, "hit primeTag, dealing with dram cache tag");
				assert((_cache[set_num].ways[hit_way].primeTagValidBits & access_bit) == 0);
				//valid_bits = _cache[set_num].ways[hit_way].primeTagValidBits | access_bit;
				_cache[set_num].ways[hit_way].primeTagValidBits |= access_bit;
				_cache[set_num].ways[hit_way].primeTagDirtyBits |= (type == STORE?access_bit:0);
				_cache[set_num].ways[hit_way].primeTagReferenceBits |= access_bit;
				_cache[set_num].ways[hit_way].primeTagNumAccess++;
				_cache[set_num].ways[hit_way].primeTagNumFalseHits++;

				// evict subTag footprints which are conflict with fetched primeTag footprints
				uint64_t conflict_bits = _cache[set_num].ways[hit_way].primeTagValidBits & _cache[set_num].ways[hit_way].subTagValidBits;
				if(conflict_bits)
				{
					_cache[set_num].ways[hit_way].subTagValidBits &= ~conflict_bits;
					_cache[set_num].ways[hit_way].subTagDirtyBits &= _cache[set_num].ways[hit_way].subTagValidBits;

					uint32_t conflict_size = __builtin_popcountll(conflict_bits & _cache[set_num].ways[hit_way].subTagDirtyBits );
					uint64_t conflict_addr= _cache[set_num].ways[hit_way].subTag;
					MemReq str_conflict_data = {conflict_addr*64, PUTX, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
					_ext_dram->access(str_conflict_data, 2, conflict_size*4);
					_ext_bw_per_step += conflict_size*4;
					assert((_cache[set_num].ways[hit_way].subTagValidBits & _cache[set_num].ways[hit_way].primeTagValidBits) == 0);
				}
			}
			else
			{
				assert(tag == _cache[set_num].ways[hit_way].subTag);
				trace(MC, "hit subTag, dealing with dram cache tag");
				assert((_cache[set_num].ways[hit_way].subTagValidBits & access_bit) == 0);
				// only if that bit in primeTag is not valid, the subTag can update that bit
				if(( _cache[set_num].ways[hit_way].primeTagValidBits & access_bit) == 0 )
				{
					trace(MC, "primeTagValidBits & access_bit == 0");
					//valid_bits = _cache[set_num].ways[hit_way].subTagValidBits | access_bit;
					_cache[set_num].ways[hit_way].subTagValidBits |= access_bit;
					_cache[set_num].ways[hit_way].subTagDirtyBits |= (type == STORE?(access_bit):0);
					_cache[set_num].ways[hit_way].subTagReferenceBits |= access_bit;
					_cache[set_num].ways[hit_way].subTagNumAccess++;
					_cache[set_num].ways[hit_way].subTagNumFalseHits++;
					assert((_cache[set_num].ways[hit_way].subTagValidBits & _cache[set_num].ways[hit_way].primeTagValidBits) == 0);
				}
				else
				{
					trace(MC, "primeTagValidBits & access_bit != 0");
					// subTag's priority is lower than primeTag, only reference bits can be updated
					_cache[set_num].ways[hit_way].subTagReferenceBits |= access_bit;
				}
			}
		}

		// both real-hit and false hit may invoke subTag/primeTag swap
		// if hit subTag and swap happens
		if(tag == _cache[set_num].ways[hit_way].subTag && primeTag_victim_way < _num_ways)
		{
			trace(MC, "hit subTag and primeTag_victim_way < _num_ways, swap may happen");
			trace(MC, "_in_way_replace = %d, hit_way=%d, primeTag_victim_way=%d", _in_way_replace, hit_way, primeTag_victim_way);
			if(_in_way_replace || (hit_way==primeTag_victim_way))
			{
				trace(MC, "swap happen, repalce in the same way");
				assert(hit_way == primeTag_victim_way);
				// we should set tagRoleChanged = 1,
				// but to make coding easy we still using swap to make code look the same

				/********** dealing with footprint *********/
				if(access_bit & _cache[set_num].ways[primeTag_victim_way].primeTagValidBits)
				{
					//requested tag in subTag is conflict with victim primeTag, write back the victim primeTag footprint
					if(_cache[set_num].ways[primeTag_victim_way].primeTagDirtyBits & access_bit)
					{
						trace(MC, "swaping primeTag and subTag, dealing with conflit");
						uint32_t conflict_size = _footprint_size;
						//uint64_t conflict_addr= _cache[set_num].ways[primeTag_victim_way].primeTag + conflict_size*64*;
						uint64_t conflict_addr= _cache[set_num].ways[primeTag_victim_way].primeTag;
						MemReq str_conflict_data = {conflict_addr*64, PUTX, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
						_mcdram[(conflict_addr / 64) % _mcdram_per_mc]->access(str_conflict_data, 2, conflict_size*4);
						_mc_bw_per_step += conflict_size*4;
					}
				}

				/********** dealing with tag swap *********/
				// subTag in hit_way swap with primeTag_victim_way
				uint64_t temp_tag=0, temp_valid_bits=0, temp_refer_bits=0, temp_dirty_bits=0;
				uint32_t temp_NumAccess=0, temp_RealHits=0, temp_FalseHits=0, temp_NumMisses=0;
				uint64_t subTag_valid_bits = _cache[set_num].ways[hit_way].subTagValidBits | access_bit;
				uint64_t subTag_dirty_bits = _cache[set_num].ways[hit_way].subTagDirtyBits | (type == STORE?(access_bit):0);
				uint64_t subTag_refer_bits = _cache[set_num].ways[hit_way].subTagReferenceBits | access_bit;


				temp_tag		= _cache[set_num].ways[primeTag_victim_way].primeTag;
				temp_valid_bits = _cache[set_num].ways[primeTag_victim_way].primeTagValidBits & (~subTag_valid_bits);
				temp_refer_bits = _cache[set_num].ways[primeTag_victim_way].primeTagReferenceBits;
				temp_dirty_bits = _cache[set_num].ways[primeTag_victim_way].primeTagDirtyBits & temp_valid_bits;
				temp_NumAccess	= _cache[set_num].ways[primeTag_victim_way].primeTagNumAccess;
				temp_RealHits	= _cache[set_num].ways[primeTag_victim_way].primeTagNumRealHits;
				temp_FalseHits	= _cache[set_num].ways[primeTag_victim_way].primeTagNumFalseHits;
				temp_NumMisses	= _cache[set_num].ways[primeTag_victim_way].primeTagNumMisses;

				_cache[set_num].ways[primeTag_victim_way].primeTag				= _cache[set_num].ways[hit_way].subTag;
				_cache[set_num].ways[primeTag_victim_way].primeTagValidBits 	= subTag_valid_bits;
				_cache[set_num].ways[primeTag_victim_way].primeTagReferenceBits = subTag_refer_bits;
				_cache[set_num].ways[primeTag_victim_way].primeTagDirtyBits 	= subTag_dirty_bits;
				_cache[set_num].ways[primeTag_victim_way].primeTagNumAccess 	= _cache[set_num].ways[hit_way].subTagNumAccess;
				_cache[set_num].ways[primeTag_victim_way].primeTagNumRealHits	= _cache[set_num].ways[hit_way].subTagNumRealHits;
				_cache[set_num].ways[primeTag_victim_way].primeTagNumFalseHits	= _cache[set_num].ways[hit_way].subTagNumFalseHits;
				_cache[set_num].ways[primeTag_victim_way].primeTagNumMisses 	= _cache[set_num].ways[hit_way].subTagNumMisses;

				_cache[set_num].ways[hit_way].subTag				= temp_tag;
				_cache[set_num].ways[hit_way].subTagValidBits		= temp_valid_bits;
				_cache[set_num].ways[hit_way].subTagReferenceBits	= temp_refer_bits;
				_cache[set_num].ways[hit_way].subTagDirtyBits		= temp_dirty_bits;
				_cache[set_num].ways[hit_way].subTagNumAccess		= temp_NumAccess;
				_cache[set_num].ways[hit_way].subTagNumRealHits 	= temp_RealHits;
				_cache[set_num].ways[hit_way].subTagNumFalseHits	= temp_FalseHits;
				_cache[set_num].ways[hit_way].subTagNumMisses		= temp_NumMisses;

				assert((_cache[set_num].ways[hit_way].primeTagValidBits & _cache[set_num].ways[hit_way].subTagValidBits) == 0);
				assert((_cache[set_num].ways[primeTag_victim_way].primeTagValidBits & _cache[set_num].ways[primeTag_victim_way].subTagValidBits) == 0);

				/********** dealing with TagBuffer *********/
				if(_tag_buffer->canInsert(tag, temp_tag))
				{
					// no remap for in_way_replace
					_tag_buffer->insert(tag, false);
					_tag_buffer->insert(temp_tag, false);
				}

				/********** dealing with TLB *********/
				assert( _tlb[temp_tag].way == _tlb[tag].way );
			}
			else // different way swap
			{
				trace(MC, "swap happen, repalce in different way");
				/********** dealing with footprint *********/
				// 1. cast out conlict footprints
				// 2. swap the hit_way's subTag with primeTag_victim_way's primeTag footprints

				// 1. cast out conlict footprints
				//primeTag:primeTag_victim_way     subTag:hit_way
				// handle primeTag_victim_way's subTag footprints which are conflict with hit_way's subTag valid bits, step1
				if(_cache[set_num].ways[primeTag_victim_way].subTagValidBits & _cache[set_num].ways[hit_way].subTagValidBits)
				{
					uint64_t conflict_bits = _cache[set_num].ways[primeTag_victim_way].subTagValidBits &(_cache[set_num].ways[hit_way].subTagValidBits | access_bit);
					// when only tag hit, the requested footprint maybe need to fill into primeTag_victim_way's primeTag
					if(conflict_bits)
					{
						_cache[set_num].ways[primeTag_victim_way].subTagValidBits &= (~conflict_bits);
						_cache[set_num].ways[primeTag_victim_way].subTagDirtyBits &= _cache[set_num].ways[primeTag_victim_way].subTagValidBits;
						//_cache[set_num].ways[primeTag_victim_way].subTagReferenceBits  don't change

						uint32_t conflict_size = __builtin_popcountll(conflict_bits &_cache[set_num].ways[primeTag_victim_way].subTagValidBits);
						uint64_t conflict_subtag_addr= _cache[set_num].ways[primeTag_victim_way].subTag;
						MemReq str_conflict_data = {conflict_subtag_addr*64, PUTX, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
						_mcdram[(conflict_subtag_addr / 64) % _mcdram_per_mc]->access(str_conflict_data, 0, conflict_size*4);
						_mc_bw_per_step += conflict_size*4;
					}
				}
				// else  keep  _cache[set_num].ways[primeTag_victim_way].subTagValidBits don't change

				// handle hit_way's subTag which is from primeTag_victim_way's primeTag
				if(_cache[set_num].ways[primeTag_victim_way].primeTagValidBits & _cache[set_num].ways[hit_way].primeTagValidBits)
				{
					uint64_t conflict_bits = _cache[set_num].ways[primeTag_victim_way].primeTagValidBits & _cache[set_num].ways[hit_way].primeTagValidBits;
					if(conflict_bits)
					{
						_cache[set_num].ways[primeTag_victim_way].primeTagValidBits &= (~conflict_bits);
						_cache[set_num].ways[primeTag_victim_way].primeTagDirtyBits &= (_cache[set_num].ways[primeTag_victim_way].primeTagValidBits);
						assert((_cache[set_num].ways[hit_way].primeTagValidBits & _cache[set_num].ways[hit_way].subTagValidBits) == 0);

						uint32_t conflict_size = __builtin_popcountll(conflict_bits & _cache[set_num].ways[primeTag_victim_way].primeTagDirtyBits);
						uint64_t conflict_addr= _cache[set_num].ways[primeTag_victim_way].primeTag;
						MemReq str_conflict_data = {conflict_addr*64, PUTX, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
						_mcdram[(conflict_addr / 64) % _mcdram_per_mc]->access(str_conflict_data, 0, conflict_size*4);
						_mc_bw_per_step += conflict_size*4;
					}
				}
				// else  keep  _cache[set_num].ways[primeTag_victim_way].primeTagValidBits don't change

				// 2. swap the hit_way's subTag with primeTag_victim_way's primeTag footprints
				/********** dealing with tag swap *********/
				// subTag in hit_way swap with primeTag_victim_way
				uint64_t temp_tag=0, temp_valid_bits=0, temp_refer_bits=0, temp_dirty_bits=0;
				uint32_t temp_NumAccess=0, temp_RealHits=0, temp_FalseHits=0, temp_NumMisses=0;
				uint64_t valid_bits = _cache[set_num].ways[hit_way].subTagValidBits | access_bit;
				uint64_t dirty_bits = _cache[set_num].ways[hit_way].subTagDirtyBits | (type == STORE?access_bit:0);
				uint64_t refer_bits = _cache[set_num].ways[hit_way].subTagReferenceBits | access_bit;

				temp_tag 		= _cache[set_num].ways[primeTag_victim_way].primeTag;
				temp_valid_bits = _cache[set_num].ways[primeTag_victim_way].primeTagValidBits;
				temp_refer_bits = _cache[set_num].ways[primeTag_victim_way].primeTagReferenceBits;
				temp_dirty_bits = _cache[set_num].ways[primeTag_victim_way].primeTagDirtyBits;
				temp_NumAccess	= _cache[set_num].ways[primeTag_victim_way].primeTagNumAccess;
				temp_RealHits	= _cache[set_num].ways[primeTag_victim_way].primeTagNumRealHits;
				temp_FalseHits	= _cache[set_num].ways[primeTag_victim_way].primeTagNumFalseHits;
				temp_NumMisses	= _cache[set_num].ways[primeTag_victim_way].primeTagNumMisses;

				_cache[set_num].ways[primeTag_victim_way].primeTag				= _cache[set_num].ways[hit_way].subTag;
				_cache[set_num].ways[primeTag_victim_way].primeTagValidBits 	= valid_bits;
				_cache[set_num].ways[primeTag_victim_way].primeTagReferenceBits = refer_bits;
				_cache[set_num].ways[primeTag_victim_way].primeTagDirtyBits 	= dirty_bits;
				_cache[set_num].ways[primeTag_victim_way].primeTagNumAccess 	= _cache[set_num].ways[hit_way].subTagNumAccess;
				_cache[set_num].ways[primeTag_victim_way].primeTagNumRealHits	= _cache[set_num].ways[hit_way].subTagNumRealHits;
				_cache[set_num].ways[primeTag_victim_way].primeTagNumFalseHits	= _cache[set_num].ways[hit_way].subTagNumFalseHits;
				_cache[set_num].ways[primeTag_victim_way].primeTagNumMisses 	= _cache[set_num].ways[hit_way].subTagNumMisses;

				_cache[set_num].ways[hit_way].subTag				= temp_tag;
				_cache[set_num].ways[hit_way].subTagValidBits		= temp_valid_bits;
				_cache[set_num].ways[hit_way].subTagReferenceBits	= temp_refer_bits;
				_cache[set_num].ways[hit_way].subTagDirtyBits		= temp_dirty_bits;
				_cache[set_num].ways[hit_way].subTagNumAccess		= temp_NumAccess;
				_cache[set_num].ways[hit_way].subTagNumRealHits 	= temp_RealHits;
				_cache[set_num].ways[hit_way].subTagNumFalseHits	= temp_FalseHits;
				_cache[set_num].ways[hit_way].subTagNumMisses		= temp_NumMisses;

				assert((_cache[set_num].ways[hit_way].primeTagValidBits & _cache[set_num].ways[hit_way].subTagValidBits) == 0);
				assert((_cache[set_num].ways[primeTag_victim_way].primeTagValidBits & _cache[set_num].ways[primeTag_victim_way].subTagValidBits) == 0);

				/********** dealing with TagBuffer *********/
				if(_tag_buffer->canInsert(tag, temp_tag))
				{
					_tag_buffer->insert(tag, true);
					_tag_buffer->insert(temp_tag, true);
				}

				/********** dealing with TLB *********/
				uint32_t temp_way = _tlb[temp_tag].way;
				_tlb[temp_tag].way = _tlb[tag].way;
				_tlb[tag].way = temp_way;
			}
		}
	}
	// DC miss
	else
	{
		trace(MC, "Dram Cache Miss: 0x%lx", tag);
		// for LongCache, this assertion takes too much time
		for (uint32_t i = 0; i < _num_ways; i ++)
			assert(_cache[set_num].ways[i].primeTag != tag && _cache[set_num].ways[i].subTag != tag);

		// if replace happens in primeTag, then the replaced data must insert into the subTag too.
		uint32_t primeTag_replace_way = _num_ways;
		uint32_t subTag_replace_way = _num_ways;
		//uint64_t cur_cycle = req.cycle;
		_num_miss_per_step ++;
		if (type == LOAD)
			_numLoadMiss.inc();
		else
			_numStoreMiss.inc();

		if (set_num >= _ds_index)
		{
			if (!hybrid_tag_probe)
			{
				req.cycle = _ext_dram->access(req, 0, 4);
				_ext_bw_per_step += 4;
				data_ready_cycle = req.cycle;
			}
			else
			{
				trace(MC, "accessing mcdram and ext_dram");
				MemReq tag_probe = {mc_address, GETS, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
				req.cycle = _mcdram[mcdram_select]->access(tag_probe, 0, 2);
				_mc_bw_per_step += 2;
				req.cycle = _ext_dram->access(req, 1, 4);
				_ext_bw_per_step += 4;
				_numTagLoad.inc();
				data_ready_cycle = req.cycle;
			}

			trace(MC, "set_num:%ld, _ds_index:%ld", set_num, _ds_index);
			bool counter_access = false;
			primeTag_replace_way = _page_placement_policy->handleCacheMiss(tag, address, type, set_num, &_cache[set_num], counter_access);
			trace(MC, "cache set: %ld, primeTag_replace_way:%d", set_num, primeTag_replace_way);


			if(primeTag_replace_way == _num_ways)
			{
				// primeTag_replace_way is invalid
				// Dram cache miss but no replacement, insert into TB to accelerate read access
				trace(MC, "request type = %d", type);
				if (type == LOAD && _tag_buffer->canInsert(tag, address))
				{
					//trace(MC,  "Before inserting Tag Buffer: \n");
					//_tag_buffer->printAll();
					_tag_buffer->insert(tag, false);
					//trace(MC,  "After inserting Tag Buffer: \n");
					//_tag_buffer->printAll();
				}

				subTag_replace_way = _num_ways;
			}
			else
			{
				if(_cache[set_num].ways[primeTag_replace_way].primeTagValidBits)
				{
					subTag_replace_way = _page_placement_policy->handlePrimeTagEvict(primeTag_replace_way, _cache[set_num].ways[primeTag_replace_way].primeTag, type, set_num, &_cache[set_num], counter_access);
					trace(MC, "handlePrimeTagEvict return subTag_replace_way:%d", subTag_replace_way);
					// if primeTag replaced and valid, subTag must be replaced
					assert(subTag_replace_way != _num_ways);
					{
						//_cache[set_num].ways[primeTag_replace_way].primeTagValidBits: valid   replaced_subTag_way: valid      candidate_way
						//_cache[set_num].ways[primeTag_replace_way].primeTagValidBits: valid   replaced_subTag_way: invalid    candidate_way
						//replaced_primeTag_way: invalid   replaced_subTag_way: valid    candidate_way
						//replaced_primeTag_way: invalid   replaced_subTag_way: invalid  candidate_way

						// primeTag_replace_way is valid
						// Dram cache miss with replacement
						trace(MC, "primeTag_replace_way:%d", primeTag_replace_way);
						uint64_t replaced_primeTag_valid_dirty_bits = _cache[set_num].ways[primeTag_replace_way].primeTagValidBits &
							_cache[set_num].ways[primeTag_replace_way].primeTagDirtyBits;
						uint64_t replaced_subTag_valid_dirty_bits = _cache[set_num].ways[subTag_replace_way].subTagValidBits &
							_cache[set_num].ways[subTag_replace_way].subTagDirtyBits;
						uint64_t prefetched_fp_bits = _tlb[tag].footprint_history | access_bit;
						uint32_t prefetch_size = 0;
						if (_tlb.find(tag) != _tlb.end())
							prefetch_size = __builtin_popcountll(prefetched_fp_bits) * _footprint_size;
						trace(MC, "prefetched_fp_bits:0x%lx, prefetch_size:%d", prefetched_fp_bits, prefetch_size);

						/********** dealing with data access *********/
						// 1.load prefetched footprints data from ext dram
						MemReq load_req = {tag*64, GETS, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
						_ext_dram->access(load_req, 2, prefetch_size*4);
						_ext_bw_per_step += prefetch_size * 4;
						_numTagLoad.inc();

						// 2.store the prefetched footprints into in-chip dram cache
						MemReq str_prefetched_data = {mc_address, PUTX, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
						_mcdram[mcdram_select]->access(str_prefetched_data, 2, prefetch_size*4);
						_mc_bw_per_step += prefetch_size * 4;

						//Dram cache miss, candidate->primeTag/primeTag->subTag/subTag->candidate
						if(_cache[set_num].ways[primeTag_replace_way].primeTagValidBits)
						{
							trace(MC, "primeTagValidBits :0x%lx",_cache[set_num].ways[primeTag_replace_way].primeTagValidBits);
							_numEvictedValidPrimeTagPages.inc();
							Address replacedPrimeTag = _cache[set_num].ways[primeTag_replace_way].primeTag;
							Address replacedSubTag = _cache[set_num].ways[subTag_replace_way].subTag;
							trace(MC, "replacedPrimeTag:0x%lx, replacedSubTag:0x%lx", replacedPrimeTag, replacedSubTag);

							/********** dealing with TagBuffer *********/
							assert(_tag_buffer->canInsert(tag, replacedSubTag));
							{
								trace(MC, "inserting tag_buffer");
								_tag_buffer->insert(tag, true);
								_tag_buffer->insert(replacedSubTag, true);
							}
							// replacedPrimeTag is still in Dram Cache, no need to remap
							assert(_tag_buffer->canInsert(replacedPrimeTag));

							// even _in_way_replace is NOT enabled, the primeTag_replace_way maybe same as subTag_replace_way too
							if(primeTag_replace_way == subTag_replace_way)
							{
								_numPlacement.inc();
								/********** dealing with footprints *********/
								if(replaced_primeTag_valid_dirty_bits || replaced_subTag_valid_dirty_bits)
								{
									_numDirtyEviction.inc();

									// 3.store the primeTag_replace_way's subTag dirty footprints to dram
									MemReq subTag_writeback_req = {_cache[set_num].ways[primeTag_replace_way].subTag*64, PUTX, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
									_ext_dram->access(subTag_writeback_req, 2, __builtin_popcountll(replaced_subTag_valid_dirty_bits)*4);
									_ext_bw_per_step += __builtin_popcountll(replaced_subTag_valid_dirty_bits) * 4;
									uint32_t dirty_lines = __builtin_popcountll(replaced_subTag_valid_dirty_bits);

									// 4.primeTag is changed to subTag, store the conflict(with prefetched footprints) primeTag's dirty footprints to dram
									MemReq primeTag_writeback_req = {_cache[set_num].ways[subTag_replace_way].primeTag*64, PUTX, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
									_ext_dram->access(primeTag_writeback_req, 2, __builtin_popcountll(prefetched_fp_bits & replaced_primeTag_valid_dirty_bits)*4);
									_ext_bw_per_step += __builtin_popcountll((prefetched_fp_bits & replaced_primeTag_valid_dirty_bits)) * 4;
									dirty_lines += __builtin_popcountll((prefetched_fp_bits & replaced_primeTag_valid_dirty_bits));
									_numEvictedDirtyLines.inc(dirty_lines);
								}
								else
								{
									_numCleanEviction.inc();
								}

								/********** dealing with TLB *********/
								assert(_tlb[_cache[set_num].ways[primeTag_replace_way].primeTag].way == subTag_replace_way);
								if (_tlb.find(_cache[set_num].ways[subTag_replace_way].subTag) != _tlb.end())
								{
									_tlb[_cache[set_num].ways[subTag_replace_way].subTag].way = _num_ways;
									// record Dram Cache evicted pages' referenced bits into TLB for prefetching
									_tlb[_cache[set_num].ways[subTag_replace_way].subTag].footprint_history = _cache[set_num].ways[subTag_replace_way].subTagReferenceBits;
								}
								else
								{
									_tlb[_cache[set_num].ways[subTag_replace_way].subTag] = TLBEntry {_cache[set_num].ways[subTag_replace_way].subTag, _num_ways, _cache[set_num].ways[subTag_replace_way].subTagReferenceBits};
								}
								//assert(_tlb.find(tag) != _tlb.end());
								_tlb[tag].way = primeTag_replace_way;
								trace(MC, "for 0x%lx, way is setting to %d", tag, primeTag_replace_way);
								_tlb[tag].footprint_history = prefetched_fp_bits;

								/********** dealing with Dram Cache tag *********/
								// swap primeTag and subTag
								_cache[set_num].ways[subTag_replace_way].subTag = _cache[set_num].ways[primeTag_replace_way].primeTag;
								_cache[set_num].ways[subTag_replace_way].subTagValidBits = _cache[set_num].ways[primeTag_replace_way].primeTagValidBits & (~prefetched_fp_bits);
								_cache[set_num].ways[subTag_replace_way].subTagReferenceBits = _cache[set_num].ways[primeTag_replace_way].primeTagReferenceBits;
								_cache[set_num].ways[subTag_replace_way].subTagDirtyBits = _cache[set_num].ways[primeTag_replace_way].primeTagDirtyBits & (~prefetched_fp_bits);
								_cache[set_num].ways[subTag_replace_way].subTagNumAccess = _cache[set_num].ways[primeTag_replace_way].primeTagNumAccess;
								_cache[set_num].ways[subTag_replace_way].subTagNumRealHits = _cache[set_num].ways[primeTag_replace_way].primeTagNumRealHits;
								_cache[set_num].ways[subTag_replace_way].subTagNumFalseHits = _cache[set_num].ways[primeTag_replace_way].primeTagNumFalseHits;
								_cache[set_num].ways[subTag_replace_way].subTagNumMisses = 	_cache[set_num].ways[primeTag_replace_way].primeTagNumMisses;

								_cache[set_num].ways[subTag_replace_way].tagRoleChanged = true;
								_cache[set_num].ways[subTag_replace_way].inWayReplaceCounts++;

								// store the prefetched footprints into in-chip dram cache
								_cache[set_num].ways[primeTag_replace_way].primeTag = tag;
								_cache[set_num].ways[primeTag_replace_way].primeTagValidBits = prefetched_fp_bits;
								_cache[set_num].ways[primeTag_replace_way].primeTagReferenceBits |= access_bit;
								if (type == STORE)
								{
									_cache[set_num].ways[primeTag_replace_way].primeTagDirtyBits |= access_bit;
								}
								else
								{
									_cache[set_num].ways[primeTag_replace_way].primeTagDirtyBits = 0;
								}
								_cache[set_num].ways[primeTag_replace_way].primeTagNumAccess = 1;
								_cache[set_num].ways[primeTag_replace_way].primeTagNumRealHits = 0;
								_cache[set_num].ways[primeTag_replace_way].primeTagNumFalseHits = 0;
								_cache[set_num].ways[primeTag_replace_way].primeTagNumMisses = 1;
								assert((_cache[set_num].ways[hit_way].primeTagValidBits & _cache[set_num].ways[hit_way].subTagValidBits) == 0);
							}
							else
							{
								if(replaced_primeTag_valid_dirty_bits || replaced_subTag_valid_dirty_bits)
								{
									_numPlacement.inc(2);
									_numDirtyEviction.inc();

									/************* dealing with footprints **************/
									// 3.store primeTag_replace_way's subTag footprints which are confilct with prefetched footprints into ext dram
									MemReq subTag_writeback_req = {_cache[set_num].ways[primeTag_replace_way].subTag*64, PUTX, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
									_ext_dram->access(subTag_writeback_req, 2, __builtin_popcountll(replaced_subTag_valid_dirty_bits & prefetched_fp_bits)*4);
									_ext_bw_per_step += __builtin_popcountll(replaced_subTag_valid_dirty_bits & prefetched_fp_bits) * 4;
									uint32_t dirty_lines = __builtin_popcountll(replaced_subTag_valid_dirty_bits & prefetched_fp_bits);

									// 4.put primeTag_replace_way primeTag's not confilct valid footprints into subTag_replace_way's subTag
									//Address mc_address = (address / 64 / _mcdram_per_mc * 64) | (address % 64);
									//	uint32_t mcdram_select = (address / 64) % _mcdram_per_mc;
									Address primeTag_mcdram_select = _cache[set_num].ways[primeTag_replace_way].primeTag % _mcdram_per_mc;
									assert((_cache[set_num].ways[primeTag_replace_way].primeTag % 64) == 0);
									Address primeTag_mc_address = _cache[set_num].ways[primeTag_replace_way].primeTag / _mcdram_per_mc * 64;
									MemReq primeTag_2subTag_req = {primeTag_mc_address, PUTX, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
									_mcdram[primeTag_mcdram_select]->access(primeTag_2subTag_req, 0, __builtin_popcountll(_cache[set_num].ways[primeTag_replace_way].primeTagValidBits & (_cache[set_num].ways[subTag_replace_way].primeTagValidBits))*4);
									_mc_bw_per_step += __builtin_popcountll(_cache[set_num].ways[primeTag_replace_way].primeTagValidBits & (~_cache[set_num].ways[subTag_replace_way].primeTagValidBits)) * 4;

									// 5.because step 4, evict subTag_replace_way's all subTag footprints into ext dram first
									MemReq subTag_writeback_req2 = {_cache[set_num].ways[subTag_replace_way].subTag*64, PUTX, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
									_ext_dram->access(subTag_writeback_req2, 0, __builtin_popcountll(_cache[set_num].ways[subTag_replace_way].subTagDirtyBits)*4);
									_ext_bw_per_step += __builtin_popcountll(_cache[set_num].ways[subTag_replace_way].subTagDirtyBits) * 4;
									dirty_lines +=__builtin_popcountll(_cache[set_num].ways[subTag_replace_way].subTagDirtyBits);

									// 6.put primeTag_replace_way's primeTag dirty footprints which are conflict with subTag_replace_way's primeTag into ext dram
									MemReq primeTag_writeback_req = {_cache[set_num].ways[primeTag_replace_way].primeTag*64, PUTX, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
									_ext_dram->access(primeTag_writeback_req, 0, __builtin_popcountll(_cache[set_num].ways[primeTag_replace_way].primeTagDirtyBits & _cache[set_num].ways[subTag_replace_way].primeTagValidBits)*4);
									_ext_bw_per_step += __builtin_popcountll(_cache[set_num].ways[primeTag_replace_way].primeTagDirtyBits & _cache[set_num].ways[subTag_replace_way].primeTagValidBits) * 4;
									dirty_lines +=__builtin_popcountll(_cache[set_num].ways[primeTag_replace_way].primeTagDirtyBits & _cache[set_num].ways[subTag_replace_way].primeTagValidBits);
									_numEvictedDirtyLines.inc(dirty_lines);
									assert((_cache[set_num].ways[hit_way].primeTagValidBits & _cache[set_num].ways[hit_way].subTagValidBits) == 0);
								}
								else
								{
									_numCleanEviction.inc();
								}
								assert((_cache[set_num].ways[hit_way].primeTagValidBits & _cache[set_num].ways[hit_way].subTagValidBits) == 0);

								/********** dealing with TLB *********/
								//assert(_tlb.find(tag) != _tlb.end());
								_tlb[tag].way = primeTag_replace_way;
								_tlb[tag].footprint_history = prefetched_fp_bits;

								_tlb[_cache[set_num].ways[primeTag_replace_way].primeTag].way = subTag_replace_way;
								if (_tlb.find(_cache[set_num].ways[subTag_replace_way].subTag) != _tlb.end())
								{
									_tlb[_cache[set_num].ways[subTag_replace_way].subTag].way = _num_ways;
									//_tlb[_cache[set_num].ways[subTag_replace_way].subTag].touch_bitvec = 0;
									//_tlb[_cache[set_num].ways[subTag_replace_way].subTag].dirty_bitvec = 0;
									// record Dram Cache evicted pages' referenced bits into TLB for prefetching
									_tlb[_cache[set_num].ways[subTag_replace_way].subTag].footprint_history = _cache[set_num].ways[subTag_replace_way].subTagReferenceBits;
								}
								else
								{
									_tlb[_cache[set_num].ways[subTag_replace_way].subTag] = TLBEntry {_cache[set_num].ways[subTag_replace_way].subTag, _num_ways, _cache[set_num].ways[subTag_replace_way].subTagReferenceBits};
								}

								/************* dealing with tag **************/
								// 4/5/6.put primeTag_replace_way's not confilct valid footprints into subTag_replace_way's subTag
								_cache[set_num].ways[subTag_replace_way].subTag = _cache[set_num].ways[primeTag_replace_way].primeTag;
								_cache[set_num].ways[subTag_replace_way].subTagValidBits = _cache[set_num].ways[primeTag_replace_way].primeTagValidBits & (~_cache[set_num].ways[subTag_replace_way].primeTagValidBits);
								_cache[set_num].ways[subTag_replace_way].subTagReferenceBits = _cache[set_num].ways[primeTag_replace_way].primeTagReferenceBits;
								_cache[set_num].ways[subTag_replace_way].subTagDirtyBits &= _cache[set_num].ways[subTag_replace_way].subTagValidBits;
								_cache[set_num].ways[subTag_replace_way].subTagNumAccess = _cache[set_num].ways[primeTag_replace_way].primeTagNumAccess;
								_cache[set_num].ways[subTag_replace_way].subTagNumRealHits = _cache[set_num].ways[primeTag_replace_way].primeTagNumRealHits;
								_cache[set_num].ways[subTag_replace_way].subTagNumFalseHits = _cache[set_num].ways[primeTag_replace_way].primeTagNumFalseHits;
								_cache[set_num].ways[subTag_replace_way].subTagNumMisses = _cache[set_num].ways[primeTag_replace_way].primeTagNumMisses;

								// 2.put prefetched footprints into primeTag_replace_way's primeTag
								_cache[set_num].ways[primeTag_replace_way].primeTag = tag;
								_cache[set_num].ways[primeTag_replace_way].primeTagNumAccess = 1;
								_cache[set_num].ways[primeTag_replace_way].primeTagValidBits = prefetched_fp_bits;
								_cache[set_num].ways[primeTag_replace_way].primeTagReferenceBits |= access_bit;
								if (type == STORE)
									_cache[set_num].ways[primeTag_replace_way].primeTagDirtyBits |= access_bit;
								else
									_cache[set_num].ways[primeTag_replace_way].primeTagDirtyBits = 0;

								_cache[set_num].ways[primeTag_replace_way].primeTagNumAccess = 1;
								_cache[set_num].ways[primeTag_replace_way].primeTagNumRealHits = 0;
								_cache[set_num].ways[primeTag_replace_way].primeTagNumFalseHits = 0;
								_cache[set_num].ways[primeTag_replace_way].primeTagNumMisses = 1;

								// 3.store primeTag_replace_way's subTag footprints into ext dram, which are confilct with prefetched footprints
								// other fields should not be changed
								_cache[set_num].ways[primeTag_replace_way].subTagValidBits = (_cache[set_num].ways[primeTag_replace_way].subTagValidBits & (~prefetched_fp_bits));
								_cache[set_num].ways[primeTag_replace_way].subTagDirtyBits &= _cache[set_num].ways[primeTag_replace_way].subTagValidBits;
								assert((_cache[set_num].ways[hit_way].primeTagValidBits & _cache[set_num].ways[hit_way].subTagValidBits) == 0);
							}
						}
						else
						{
							// primeTag_replace_way is invalid, fill into it directly
							trace(MC, "primeTagValidBits is 0");
							/********** dealing with TagBuffer *********/
							if(_tag_buffer->canInsert(tag))
							{
								trace(MC, "inserting tag_buffer");
								_tag_buffer->insert(tag, true);
							}
							else
							{
								trace(MC, "Tag buffer is full, can not insert!");
							}
							/********** dealing with TLB *********/
							assert ((_tlb.find(tag) != _tlb.end()) && (_tlb[tag].way == _num_ways));
							_tlb[tag].way = primeTag_replace_way;

							/************* dealing with tag **************/
							_cache[set_num].ways[primeTag_replace_way].primeTag = tag;
							_cache[set_num].ways[primeTag_replace_way].primeTagValidBits = prefetched_fp_bits;
							_cache[set_num].ways[primeTag_replace_way].primeTagReferenceBits |= access_bit;
							if (type == STORE)
								_cache[set_num].ways[primeTag_replace_way].primeTagDirtyBits |= access_bit;
							else
								_cache[set_num].ways[primeTag_replace_way].primeTagDirtyBits = 0;

							_cache[set_num].ways[primeTag_replace_way].primeTagNumAccess = 1;
							_cache[set_num].ways[primeTag_replace_way].primeTagNumRealHits = 0;
							_cache[set_num].ways[primeTag_replace_way].primeTagNumFalseHits = 0;
							_cache[set_num].ways[primeTag_replace_way].primeTagNumMisses = 1;
							if(_cache[set_num].ways[primeTag_replace_way].primeTagValidBits & _cache[set_num].ways[primeTag_replace_way].subTagValidBits)
							{
								printKeyInfo();
								info("set_num:%ld, way:%d, primeTag:0x%lx, primeTagValidBit:0x%lx, subTag:0x%lx, subTagValidBits:0x%lx",
										set_num, primeTag_replace_way,	_cache[set_num].ways[primeTag_replace_way].primeTag, _cache[set_num].ways[primeTag_replace_way].primeTagValidBits,
										_cache[set_num].ways[primeTag_replace_way].subTag, _cache[set_num].ways[primeTag_replace_way].subTagValidBits);
								assert((_cache[set_num].ways[primeTag_replace_way].primeTagValidBits & _cache[set_num].ways[primeTag_replace_way].subTagValidBits) == 0);
							}
#if 0
							// evict subTag footprints which are conflict with prefetched primeTag footprints
							uint64_t conflict_bits = _cache[set_num].ways[primeTag_replace_way].primeTagValidBits & _cache[set_num].ways[primeTag_replace_way].subTagValidBits;
							_cache[set_num].ways[primeTag_replace_way].subTagValidBits &= ~conflict_bits;
							_cache[set_num].ways[primeTag_replace_way].subTagDirtyBits &= _cache[set_num].ways[primeTag_replace_way].subTagValidBits;

							uint32_t conflict_size = __builtin_popcountll(conflict_bits);
							uint64_t conflict_addr= _cache[set_num].ways[primeTag_replace_way].subTag;
							MemReq str_conflict_data = {conflict_addr*64, PUTX, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
							_ext_dram->access(str_conflict_data, 0, conflict_size*4);
							_ext_bw_per_step += conflict_size*4;
#endif
						}
					}
				}
#if 0
				// TODO: finish this
				else
				{
				}
#endif
			}
		}

		// dram cache logic. Here, I'm assuming the 4 mcdram channels are
		// organized centrally
		bool counter_access = false;
		// use the following state for requests, so that req.state is not changed

		// TODO. make this part work again.
		if (counter_access && !_sram_tag) {
			// TODO may not need the counter load if we can store freq info inside TAD
			/////// model counter access in mcdram
			// One counter read and one coutner write
			assert(set_num >= _ds_index);
			_numCounterAccess.inc();
			MemReq counter_req = {mc_address, GETS, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
			_mcdram[mcdram_select]->access(counter_req, 2, 2);
			counter_req.type = PUTX;
			_mcdram[mcdram_select]->access(counter_req, 2, 2);
			_mc_bw_per_step += 4;
			//////////////////////////////////////
		}
		if (_tag_buffer->getOccupancy() > 0.7) {
			printf("[Tag Buffer FLUSH] occupancy = %f\n", _tag_buffer->getOccupancy());
			_tag_buffer->clearTagBuffer();
			_tag_buffer->setClearTime(req.cycle);
			_numTagBufferFlush.inc();
		}

		if (_num_requests % step_length == 0)
		{
			_num_hit_per_step /= 2;
			_num_miss_per_step /= 2;
			_mc_bw_per_step /= 2;
			_ext_bw_per_step /= 2;
			if (_bw_balance && _mc_bw_per_step + _ext_bw_per_step > 0) {
				// adjust _ds_index	based on mc vs. ext dram bandwidth.
				double ratio = 1.0 * _mc_bw_per_step / (_mc_bw_per_step + _ext_bw_per_step);
				double target_ratio = 0.8;  // because mc_bw = 4 * ext_bw

				// the larger the gap between ratios, the more _ds_index changes.
				// _ds_index changes in the granualrity of 1/1000 dram cache capacity.
				// 1% in the ratio difference leads to 1/1000 _ds_index change.
				// 300 is arbitrarily chosen.
				// XXX XXX XXX
				// 1000 is only used for graph500 and pagerank.
				//uint64_t index_step = _num_sets / 300; // in terms of the number of sets
				uint64_t index_step = _num_sets / 1000; // in terms of the number of sets
				int64_t delta_index = (ratio - target_ratio > -0.02 && ratio - target_ratio < 0.02)?
					0 : index_step * (ratio - target_ratio) / 0.01;
				printf("ratio = %f\n", ratio);
				if (delta_index > 0) {
					// _ds_index will increase. All dirty data between _ds_index and _ds_index + delta_index
					// should be written back to external dram.
					// For Alloy cache, this is relatively easy.
					// For Hybrid, we need to update tag buffer as well...
					for (uint32_t mc = 0; mc < _mcdram_per_mc; mc ++) {
						for (uint64_t set = _ds_index; set < (uint64_t)(_ds_index + delta_index); set ++) {
							if (set >= _num_sets) break;
							for (uint32_t way = 0; way < _num_ways; way ++)	 {
								Way &meta = _cache[set].ways[way];
								if (meta.primeTagValidBits & meta.primeTagDirtyBits)
								{
									uint64_t primeTag_dirty_bits = meta.primeTagValidBits & meta.primeTagDirtyBits;
									// should write back to external dram.
									MemReq load_req = { meta.primeTag * 64, GETS, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
									_mcdram[mc]->access(load_req, 2, __builtin_popcountll(primeTag_dirty_bits)*4);
									MemReq wb_req = {meta.primeTag * 64, GETS, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
									_ext_dram->access(wb_req, 2, __builtin_popcountll(primeTag_dirty_bits)*4);
									_ext_bw_per_step += __builtin_popcountll(primeTag_dirty_bits)*4;
									_mc_bw_per_step += __builtin_popcountll(primeTag_dirty_bits)*4;
								}
								if(meta.subTagValidBits & meta.subTagDirtyBits)
								{
									uint64_t subTag_dirty_bits = meta.subTagValidBits & meta.subTagDirtyBits;
									// should write back to external dram.
									MemReq subTag_load_req = { meta.subTag * 64, GETS, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
									_mcdram[mc]->access(subTag_load_req, 2, __builtin_popcountll(subTag_dirty_bits)*4);
									MemReq subTag_wb_req = {meta.subTag * 64, GETS, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
									_ext_dram->access(subTag_wb_req, 2, __builtin_popcountll(subTag_dirty_bits)*4);
									_ext_bw_per_step += __builtin_popcountll(subTag_dirty_bits)*4;
									_mc_bw_per_step += __builtin_popcountll(subTag_dirty_bits)*4;
								}
								if ( meta.primeTagValidBits )
								{
									_tlb[meta.primeTag].way = _num_ways;
									// for Hybrid cache, should insert to tag buffer as well.
									if (!_tag_buffer->canInsert(meta.primeTag, address))
									{
										printf("Rebalance. [Tag Buffer FLUSH] primeTag occupancy = %f\n", _tag_buffer->getOccupancy());
										_tag_buffer->clearTagBuffer();
										_tag_buffer->setClearTime(req.cycle);
										_numTagBufferFlush.inc();
									}
									assert(_tag_buffer->canInsert(meta.primeTag, address));
									_tag_buffer->insert(meta.primeTag, true);
								}
								meta.primeTagValidBits = 0;
								meta.primeTagDirtyBits = 0;

								if ( meta.subTagValidBits )
								{
									_tlb[meta.subTag].way = _num_ways;
									// for Hybrid cache, should insert to tag buffer as well.
									if (!_tag_buffer->canInsert(meta.subTag, address))
									{
										printf("Rebalance. [Tag Buffer FLUSH] subTag occupancy = %f\n", _tag_buffer->getOccupancy());
										_tag_buffer->clearTagBuffer();
										_tag_buffer->setClearTime(req.cycle);
										_numTagBufferFlush.inc();
									}
									assert(_tag_buffer->canInsert(meta.subTag, address));
									_tag_buffer->insert(meta.subTag, true);
								}
								meta.subTagValidBits = 0;
								meta.subTagDirtyBits = 0;
							}
							_page_placement_policy->flushChunk(set);
						}
					}
				}
				_ds_index = ((int64_t)_ds_index + delta_index <= 0)? 0 : _ds_index + delta_index;
				printf("_ds_index = %ld/%ld\n", _ds_index, _num_sets);
			}
		}

#if 0
		trace(MC, "~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~")
			trace(MC,  "Printing Tag Buffer: ");
		_tag_buffer->printAll();
		trace(MC, "~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~")
			trace(MC,  "Printing TLB: ");
		for(auto iter=_tlb.begin(); iter!=_tlb.end(); iter++)
		{
			if(iter->first != 0 && iter->second.way!=_num_ways && iter->second.way!=0)
			{
				print( "\tAddress:0x%lx, tag:0x%lx, way:%ld, footprint_history:0x%lx", iter->first, iter->second.tag, iter->second.way, iter->second.footprint_history);
				print("\n");
			}
		}
		trace(MC, "~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~")
			for(uint32_t j=0; j<_num_ways; j++)
			{
				uint64_t i = set_num;
				if(_cache[i].ways[j].primeTag || _cache[i].ways[j].subTag)
				{
					trace(MC, "Printing Dram Cache: set:%ld, way:%d", i, j);
					print("\tprimeTag:0x%lx\t", _cache[i].ways[j].primeTag);
					print("\tprimeTagValidBits:0x%lx\t", _cache[i].ways[j].primeTagValidBits);
					print("\tprimeTagReferenceBits:0x%lx\t", _cache[i].ways[j].primeTagReferenceBits);
					print("\tprimeTagDirtyBits:0x%lx\t", _cache[i].ways[j].primeTagDirtyBits);
					print("\n");
					print("\tsubTag:0x%lx\t", _cache[i].ways[j].subTag);
					print("\tsubTagValidBits:0x%lx\t", _cache[i].ways[j].subTagValidBits);
					print("\tsubTagReferenceBits:0x%lx\t", _cache[i].ways[j].subTagReferenceBits);
					print("\tsubTagDirtyBits:0x%lx\t", _cache[i].ways[j].subTagDirtyBits);
					print("\n");
				}
			}
#endif

#if 0
		// print ALL dram cache which valid, time consuming!!
		for(uint64_t i=0; i<_num_sets ; i++)
		{
			for(uint32_t j=0; j<_num_ways; j++)
			{
				if(_cache[i].ways[j].primeTag || _cache[i].ways[j].subTag)
				{
					trace(MC, "Printing Dram Cache: set:%ld, way:%d", i, j);
					print("\tprimeTag:0x%lx\t", _cache[i].ways[j].primeTag);
					print("\tprimeTagValidBits:0x%lx\t", _cache[i].ways[j].primeTagValidBits);
					print("\tprimeTagReferenceBits:0x%lx\t", _cache[i].ways[j].primeTagReferenceBits);
					print("\tprimeTagDirtyBits:0x%lx\t", _cache[i].ways[j].primeTagDirtyBits);
					print("\n");
					print("\tsubTag:0x%lx\t", _cache[i].ways[j].subTag);
					print("\tsubTagValidBits:0x%lx\t", _cache[i].ways[j].subTagValidBits);
					print("\tsubTagReferenceBits:0x%lx\t", _cache[i].ways[j].subTagReferenceBits);
					print("\tsubTagDirtyBits:0x%lx\t", _cache[i].ways[j].subTagDirtyBits);
					print("\n");
				}
			}
		}
#endif
	}
	futex_unlock(&_lock);
	//uint64_t latency = req.cycle - orig_cycle;
	//req.cycle = orig_cycle;
	return data_ready_cycle; //req.cycle + latency;
}

void MemoryController::printKeyInfo()
{
    info("~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~");
    info("Printing Tag Buffer: ");
    _tag_buffer->printAll();
    info("~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~");
    info("Printing TLB: ");
    for(auto iter=_tlb.begin(); iter!=_tlb.end(); iter++)
    {
        //if(iter->first != 0 && iter->second.way!=_num_ways && iter->second.way!=0)
        {
            print( "\tAddress:0x%lx, tag:0x%lx, way:%ld, footprint_history:0x%lx", iter->first, iter->second.tag, iter->second.way, iter->second.footprint_history);
			print("\n");
        }
    }
    info("~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~");
	// print ALL dram cache which valid, time consuming!!
	for(uint64_t i=0; i<_num_sets ; i++)
	{
		for(uint32_t j=0; j<_num_ways; j++)
		{
			if(_cache[i].ways[j].primeTag || _cache[i].ways[j].subTag)
			{
				info("Printing Dram Cache: set:%ld, way:%d", i, j);
				print("\tprimeTag:0x%lx\t", _cache[i].ways[j].primeTag);
				print("\tprimeTagValidBits:0x%lx\t", _cache[i].ways[j].primeTagValidBits);
				print("\tprimeTagReferenceBits:0x%lx\t", _cache[i].ways[j].primeTagReferenceBits);
				print("\tprimeTagDirtyBits:0x%lx\t", _cache[i].ways[j].primeTagDirtyBits);
				print("\n");
				print("\tsubTag:0x%lx\t", _cache[i].ways[j].subTag);
				print("\tsubTagValidBits:0x%lx\t", _cache[i].ways[j].subTagValidBits);
				print("\tsubTagReferenceBits:0x%lx\t", _cache[i].ways[j].subTagReferenceBits);
				print("\tsubTagDirtyBits:0x%lx\t", _cache[i].ways[j].subTagDirtyBits);
				print("\n");
			}
		}
	}
}
DDRMemory*
MemoryController::BuildDDRMemory(Config& config, uint32_t frequency,
								 uint32_t domain, g_string name, const string& prefix, uint32_t tBL, double timing_scale)
{
    uint32_t ranksPerChannel = config.get<uint32_t>(prefix + "ranksPerChannel", 4);
    uint32_t banksPerRank = config.get<uint32_t>(prefix + "banksPerRank", 8);  // DDR3 std is 8
    uint32_t pageSize = config.get<uint32_t>(prefix + "pageSize", 8*1024);  // 1Kb cols, x4 devices
    const char* tech = config.get<const char*>(prefix + "tech", "DDR3-1333-CL10");  // see cpp file for other techs
    const char* addrMapping = config.get<const char*>(prefix + "addrMapping", "rank:col:bank");  // address splitter interleaves channels; row always on top

    // If set, writes are deferred and bursted out to reduce WTR overheads
    bool deferWrites = config.get<bool>(prefix + "deferWrites", true);
    bool closedPage = config.get<bool>(prefix + "closedPage", true);

    // Max row hits before we stop prioritizing further row hits to this bank.
    // Balances throughput and fairness; 0 -> FCFS / high (e.g., -1) -> pure FR-FCFS
    uint32_t maxRowHits = config.get<uint32_t>(prefix + "maxRowHits", 4);

    // Request queues
    uint32_t queueDepth = config.get<uint32_t>(prefix + "queueDepth", 16);
    uint32_t controllerLatency = config.get<uint32_t>(prefix + "controllerLatency", 10);  // in system cycles

    auto mem = (DDRMemory *) gm_malloc(sizeof(DDRMemory));
	new (mem) DDRMemory(zinfo->lineSize, pageSize, ranksPerChannel, banksPerRank, frequency, tech, addrMapping, controllerLatency, queueDepth, maxRowHits, deferWrites, closedPage, domain, name, tBL, timing_scale);
    return mem;
}

void
MemoryController::initStats(AggregateStat* parentStat)
{
	AggregateStat* memStats = new AggregateStat();
	memStats->init(_name.c_str(), "Memory controller stats");

	_numPlacement.init("placement", "Number of Placement"); memStats->append(&_numPlacement);
	_numCleanEviction.init("cleanEvict", "Clean Eviction"); memStats->append(&_numCleanEviction);
	_numDirtyEviction.init("dirtyEvict", "Dirty Eviction"); memStats->append(&_numDirtyEviction);
	_numLoadHit.init("loadHit", "Load Hit"); memStats->append(&_numLoadHit);
	_numLoadMiss.init("loadMiss", "Load Miss"); memStats->append(&_numLoadMiss);
	_numStoreHit.init("storeHit", "Store Hit"); memStats->append(&_numStoreHit);
	_numStoreMiss.init("storeMiss", "Store Miss"); memStats->append(&_numStoreMiss);
	_numCounterAccess.init("counterAccess", "Counter Access"); memStats->append(&_numCounterAccess);

	_numTagLoad.init("tagLoad", "Number of tag loads"); memStats->append(&_numTagLoad);
	_numTagStore.init("tagStore", "Number of tag stores"); memStats->append(&_numTagStore);
	_numTagBufferFlush.init("tagBufferFlush", "Number of tag buffer flushes"); memStats->append(&_numTagBufferFlush);

	_numTagBufferMiss.init("TB miss", "Tag buffer misses, including false and real"); memStats->append(& _numTagBufferMiss);
	_numTagBufferHit.init("TB Hit", "Tag buffer hit, including false and real"); memStats->append(& _numTagBufferHit);
	_numTBDirtyHit.init("TBDirtyHit", "Tag buffer hits (LLC dirty evict)"); memStats->append(&_numTBDirtyHit);
	_numTBDirtyMiss.init("TBDirtyMiss", "Tag buffer misses (LLC dirty evict)"); memStats->append(&_numTBDirtyMiss);


	_numTouchedLines.init("totalTouchLines", "total # of touched lines in Unison/Tagless/Hybrid Cache"); memStats->append(&_numTouchedLines);
	_numTouchedLines8Blocks.init("totalTouchLines8Blocks", "total # of touched lines in Unison/Tagless/Hybrid Cache less than 8 blocks"); memStats->append(&_numTouchedLines8Blocks);
	_numTouchedLines16Blocks.init("totalTouchLines16Blocks", "page count of total num of touched lines in Unison/Tagless/Hybrid Cache between 8 and 16 blocks"); memStats->append(&_numTouchedLines16Blocks);
	_numTouchedLines24Blocks.init("totalTouchLines24Blocks", "page count of total num of touched lines in Unison/Tagless/Hybrid Cache between 16 and 24 blocks"); memStats->append(&_numTouchedLines24Blocks);
	_numTouchedLines32Blocks.init("totalTouchLines32Blocks", "page count of total num of touched lines in Unison/Tagless/Hybrid Cache between 24 and 32 blocks"); memStats->append(&_numTouchedLines32Blocks);
	_numTouchedLines48Blocks.init("totalTouchLines48Blocks", "page count of total num of touched lines in Unison/Tagless/Hybrid Cache between 32 and 48 blocks"); memStats->append(&_numTouchedLines48Blocks);
	_numTouchedLines64Blocks.init("totalTouchLines64Blocks", "page count of total num of touched lines in Unison/Tagless/Hybrid Cache between 48 and 64 blocks"); memStats->append(&_numTouchedLines64Blocks);
	_numTouchedLinesFullBlocks.init("totalTouchLinesFullBlocks", "page count of total number of touched lines in Unison/Tagless/Hybrid Cache are 64 blocks"); memStats->append(&_numTouchedLinesFullBlocks);
	_numEvictedDirtyLines.init("totalEvictLines", "total # of evicted lines in Unison/Tagless/Hybrid Cache"); memStats->append(&_numEvictedDirtyLines);
	_numSubTagHit.init("subTag hit number", "count of all accesses that real hit in subTag"); memStats->append(&_numSubTagHit);
	//_numEvictedValidPages.init("totalEvictValidPages", "total # of evicted Pages have valid data in UnisonCache"); memStats->append(&_numEvictedValidPages);

	_numTouchedPages.init("totalTouchedPages", "Number of pages touched"); memStats->append(&_numTouchedPages);

	_ext_dram->initStats(memStats);
	for (uint32_t i = 0; i < _mcdram_per_mc; i++)
		_mcdram[i]->initStats(memStats);

    parentStat->append(memStats);
}


Address
MemoryController::transMCAddress(Address mc_addr)
{
	// 28 lines per DRAM row (2048 KB row)
	uint64_t num_lines_per_mc = 128*1024*1024 / 2048 * 28;
	uint64_t set = mc_addr % num_lines_per_mc;
	return set / 28 * 32 + set % 28;
}

Address
MemoryController::transMCAddressPage(uint64_t set_num, uint32_t way_num)
{
	return (_num_ways * set_num + way_num) * _granularity;
}


TagBuffer::TagBuffer(Config & config, MemoryController *mc)
{
	uint32_t tb_size = config.get<uint32_t>("sys.mem.mcdram.tag_buffer_size", 1024);
	_num_ways = 8;
	_num_sets = tb_size / _num_ways;
	_entry_occupied = 0;
	_tag_buffer = (TagBufferEntry **) gm_malloc(sizeof(TagBufferEntry *) * _num_sets);
	_mc = mc;
	//_tag_buffer = new TagBufferEntry * [_num_sets];
	for (uint32_t i = 0; i < _num_sets; i++) {
		_tag_buffer[i] = (TagBufferEntry *) gm_malloc(sizeof(TagBufferEntry) * _num_ways);
		//_tag_buffer[i] = new TagBufferEntry [_num_ways];
		for (uint32_t j = 0; j < _num_ways; j ++) {
			_tag_buffer[i][j].remap = false;
			_tag_buffer[i][j].tag = 0;
			_tag_buffer[i][j].lru = j;
		}
	}
}

uint32_t
TagBuffer::existInTB(Address tag)
{
	uint32_t set_num = tag % _num_sets;
	for (uint32_t i = 0; i < _num_ways; i++)
		if (_tag_buffer[set_num][i].tag == tag) {
			//printf("existInTB\n");
			return i;
		}
	return _num_ways;
}

#if 0
bool TagBuffer::isRealHit(Address tag, Address addr)
{
	uint32_t set_num = tag % _num_sets;
	uint32_t hit_way=_num_ways;
	uint64_t access_bit = (uint64_t)1UL<<((addr - tag * 64)/(_mc->_granularity/_mc->_footprint_size));
	for (uint32_t i = 0; i < _num_ways; i++)
		if (_tag_buffer[set_num][i].tag == tag) {
			hit_way = i;
		}
	assert(hit_way != _num_ways);

	//Tag is aligned to Page, addr is aligned to cacheline(64B)
	assert((addr - tag * 64)<64);
	Set *cache = _mc->getSets();
	uint64_t valid_bits=0;
	bool FPisValid = 0;
	if(cache[set_num].ways[hit_way].primeTag == tag)
	{
		valid_bits = cache[set_num].ways[hit_way].primeTagValidBits;
	}
	else
	{
		assert(cache[set_num].ways[hit_way].subTag == tag);
		valid_bits = cache[set_num].ways[hit_way].subTagValidBits;
	}
	if(valid_bits & access_bit)
		return true;
	else
		return false;
}
#endif


bool
TagBuffer::canInsert(Address tag)
{
#if 1
	uint32_t num = 0;
	for (uint32_t i = 0; i < _num_sets; i++)
		for (uint32_t j = 0; j < _num_ways; j++)
			if (_tag_buffer[i][j].remap)
				num ++;
	assert(num == _entry_occupied);
#endif

	uint32_t set_num = tag % _num_sets;
	//printf("tag_buffer=%#lx, set_num=%d, tag_buffer[set_num]=%#lx, num_ways=%d\n",
	//	(uint64_t)_tag_buffer, set_num, (uint64_t)_tag_buffer[set_num], _num_ways);
	for (uint32_t i = 0; i < _num_ways; i++)
		if (!_tag_buffer[set_num][i].remap || _tag_buffer[set_num][i].tag == tag)
			return true;
	return false;
}

bool
TagBuffer::canInsert(Address tag1, Address tag2)
{
	uint32_t set_num1 = tag1 % _num_sets;
	uint32_t set_num2 = tag2 % _num_sets;
	if (set_num1 != set_num2)
		return canInsert(tag1) && canInsert(tag2);
	else {
		uint32_t num = 0;
		for (uint32_t i = 0; i < _num_ways; i++)
			if (!_tag_buffer[set_num1][i].remap
				|| _tag_buffer[set_num1][i].tag == tag1
				|| _tag_buffer[set_num1][i].tag == tag2)
				num ++;
		return num >= 2;
	}
}


void
TagBuffer::insert(Address tag, bool remap)
{
	uint32_t set_num = tag % _num_sets;
	uint32_t exist_way = existInTB(tag);
#if 1
	for (uint32_t i = 0; i < _num_ways; i++)
		for (uint32_t j = i+1; j < _num_ways; j++) {
			//if (_tag_buffer[set_num][i].tag != 0 && _tag_buffer[set_num][i].tag == _tag_buffer[set_num][j].tag) {
			//	for (uint32_t k = 0; k < _num_ways; k++)
			//		printf("_tag_buffer[%d][%d]: tag=%ld, remap=%d\n",
			//			set_num, k, _tag_buffer[set_num][k].tag, _tag_buffer[set_num][k].remap);
			//}
			assert(_tag_buffer[set_num][i].tag != _tag_buffer[set_num][j].tag
				  || _tag_buffer[set_num][i].tag == 0);
		}
#endif
	if (exist_way < _num_ways) {
		// the tag already exists in the Tag Buffer
		assert(tag == _tag_buffer[set_num][exist_way].tag);
		if (remap) {
			if (!_tag_buffer[set_num][exist_way].remap)
				_entry_occupied ++;
			_tag_buffer[set_num][exist_way].remap = true;
		} else if (!_tag_buffer[set_num][exist_way].remap)
			updateLRU(set_num, exist_way);
		return;
	}

	uint32_t max_lru = 0;
	uint32_t replace_way = _num_ways;
	for (uint32_t i = 0; i < _num_ways; i++) {
		if (!_tag_buffer[set_num][i].remap && _tag_buffer[set_num][i].lru >= max_lru) {
			max_lru = _tag_buffer[set_num][i].lru;
			replace_way = i;
		}
	}
	assert(replace_way != _num_ways);
	_tag_buffer[set_num][replace_way].tag = tag;
	_tag_buffer[set_num][replace_way].remap = remap;
	if (!remap) {
		//printf("\tset=%d way=%d, insert. no remap\n", set_num, replace_way);
		updateLRU(set_num, replace_way);
	} else {
		//printf("set=%d way=%d, insert\n", set_num, replace_way);
		_entry_occupied ++;
	}
}


void TagBuffer::evict(Address tag)
{
	uint32_t set_num = tag % _num_sets;
	uint32_t exist_way = existInTB(tag);
	assert(exist_way != _num_ways);
	_tag_buffer[set_num][exist_way].tag = 0;
	_tag_buffer[set_num][exist_way].remap = 0;
	_tag_buffer[set_num][exist_way].lru = 0;
}


/*
void
TagBuffer::insert(Address tag, bool remap, uint64_t v_bits, uint64_t dirty_bits, uint64_t ref_bits)
{
	uint32_t set_num = tag % _num_sets;
	uint32_t exist_way = existInTB(tag);
#if 1
	for (uint32_t i = 0; i < _num_ways; i++)
		for (uint32_t j = i+1; j < _num_ways; j++) {
			//if (_tag_buffer[set_num][i].tag != 0 && _tag_buffer[set_num][i].tag == _tag_buffer[set_num][j].tag) {
			//	for (uint32_t k = 0; k < _num_ways; k++)
			//		printf("_tag_buffer[%d][%d]: tag=%ld, remap=%d\n",
			//			set_num, k, _tag_buffer[set_num][k].tag, _tag_buffer[set_num][k].remap);
			//}
			assert(_tag_buffer[set_num][i].tag != _tag_buffer[set_num][j].tag
				  || _tag_buffer[set_num][i].tag == 0);
		}
#endif
	if (exist_way < _num_ways) {
		// the tag already exists in the Tag Buffer
		assert(tag == _tag_buffer[set_num][exist_way].tag);
		if (remap) {
			if (!_tag_buffer[set_num][exist_way].remap)
				_entry_occupied ++;
			_tag_buffer[set_num][exist_way].remap = true;
		} else if (!_tag_buffer[set_num][exist_way].remap)
			updateLRU(set_num, exist_way);
		return;
	}

	uint32_t max_lru = 0;
	uint32_t replace_way = _num_ways;
	for (uint32_t i = 0; i < _num_ways; i++) {
		if (!_tag_buffer[set_num][i].remap && _tag_buffer[set_num][i].lru >= max_lru) {
			max_lru = _tag_buffer[set_num][i].lru;
			replace_way = i;
		}
	}
	assert(replace_way != _num_ways);
	_tag_buffer[set_num][replace_way].tag = tag;
	_tag_buffer[set_num][replace_way].remap = remap;
	_tag_buffer[set_num][replace_way].valid_bits = v_bits;
	_tag_buffer[set_num][replace_way].reference_bits = ref_bits;
	_tag_buffer[set_num][replace_way].dirty_bits = dirty_bits;
	if (!remap) {
		//printf("\tset=%d way=%d, insert. no remap\n", set_num, replace_way);
		updateLRU(set_num, replace_way);
	} else {
		//printf("set=%d way=%d, insert\n", set_num, replace_way);
		_entry_occupied ++;
	}
}
*/

void
TagBuffer::updateLRU(uint32_t set_num, uint32_t way)
{
	assert(!_tag_buffer[set_num][way].remap);
	for (uint32_t i = 0; i < _num_ways; i++)
		if (!_tag_buffer[set_num][i].remap && _tag_buffer[set_num][i].lru < _tag_buffer[set_num][way].lru)
			_tag_buffer[set_num][i].lru ++;
	_tag_buffer[set_num][way].lru = 0;
}

void
TagBuffer::clearTagBuffer()
{
	_entry_occupied = 0;
	for (uint32_t i = 0; i < _num_sets; i++) {
		for (uint32_t j = 0; j < _num_ways; j ++) {
			_tag_buffer[i][j].remap = false;
			_tag_buffer[i][j].tag = 0;
			_tag_buffer[i][j].lru = j;
		}
	}
}
void
TagBuffer::printAll()
{
	for (uint32_t i = 0; i < _num_sets; i++)
    {
		for (uint32_t j = 0; j < _num_ways; j ++)
        {
            if(_tag_buffer[i][j].tag != 0)
            {
                print("\tTB set[%d] way[%d]:    ", i, j);
                print("remap:%d\t", _tag_buffer[i][j].remap);
                print("tag:0x%lx\t", _tag_buffer[i][j].tag);
                print("lru:%d\t", _tag_buffer[i][j].lru);
                print("\n");
            }
        }
    }

}
